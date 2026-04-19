/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// WASM drop-in replacement for CurlFile.cpp.
//
// On a standard build CCurlFile is backed by libcurl + BSD sockets. In the
// Emscripten build we don't have real sockets (Emscripten tunnels them over
// a WebSocket relay that we do not run), so we reimplement CCurlFile on top
// of the browser's XMLHttpRequest. Any cross-origin request is rewritten to
// `/proxy?u=...` by the JS shim in xbmc/platform/wasm/kodi_pre.js so CORS is
// also handled transparently.
//
// The public ABI (CCurlFile + CReadState) is preserved so DAVFile, Repository,
// HTTPDirectory, ShoutcastFile, ... keep compiling unchanged. Curl-specific
// internals (m_easyHandle, m_multiHandle, curl_slist, g_curlInterface) are
// left as nullptr/no-op; callers that reach into those will fail cleanly.

#include "filesystem/CurlFile.h"

#include "URL.h"
#include "utils/Base64.h"
#include "utils/CharsetConverter.h"
#include "utils/HttpHeader.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <emscripten.h>

using namespace XFILE;

namespace
{
// JS-side request/response store. Synchronous XHR on a pthread fully serializes
// per-thread; concurrent instances on the *same* thread don't overlap either
// (each request() finishes before the next one starts). Cross-thread contention
// is the same pre-existing concern as in WasmHttpFile.cpp.
//
// clang-format off (EM_JS JavaScript snippets)
EM_JS(int, kodi_curl_wasm_request,
      (const char* methodPtr, const char* urlPtr, const char* bodyPtr, int bodyLen,
       const char* headersPtr, const char* userAgentPtr, const char* refererPtr,
       int timeoutSeconds, int includeBody), {
  var method = UTF8ToString(methodPtr) || "GET";
  var url = UTF8ToString(urlPtr);
  var body = bodyLen > 0 ? HEAPU8.slice(bodyPtr, bodyPtr + bodyLen) : null;
  var headers = headersPtr ? UTF8ToString(headersPtr) : "";
  var userAgent = userAgentPtr ? UTF8ToString(userAgentPtr) : "";
  var referer = refererPtr ? UTF8ToString(refererPtr) : "";

  var store = Module.kodiCurlFileStore = Module.kodiCurlFileStore || {};
  store.status = 0;
  store.statusText = "";
  store.responseURL = "";
  store.rawHeaders = "";
  store.body = null;
  store.contentLength = -1;

  try {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, false);
    if (includeBody)
      xhr.responseType = "arraybuffer";
    if (timeoutSeconds > 0)
      xhr.timeout = timeoutSeconds * 1000;
    // headers is "name:value\nname:value\n..." (already validated C-side)
    if (headers) {
      var lines = headers.split("\n");
      for (var i = 0; i < lines.length; ++i) {
        var sep = lines[i].indexOf(":");
        if (sep > 0) {
          try { xhr.setRequestHeader(lines[i].substr(0, sep),
                                     lines[i].substr(sep + 1)); }
          catch (_) {} // setRequestHeader throws on forbidden header names
        }
      }
    }
    if (userAgent) { try { xhr.setRequestHeader("User-Agent", userAgent); } catch (_) {} }
    if (referer)   { try { xhr.setRequestHeader("Referer",    referer);   } catch (_) {} }
    xhr.send(body);

    store.status = xhr.status | 0;
    store.statusText = xhr.statusText || "";
    store.responseURL = xhr.responseURL || url;
    store.rawHeaders = xhr.getAllResponseHeaders() || "";
    var clen = xhr.getResponseHeader("Content-Length");
    store.contentLength = clen ? Number(clen) : -1;
    if (includeBody && xhr.response)
      store.body = new Uint8Array(xhr.response);
    return store.status;
  } catch (e) {
    console.warn("[kodi][curl-wasm] request failed:", method, url, e);
    return 0;
  }
});

EM_JS(int, kodi_curl_wasm_body_size, (), {
  var s = Module.kodiCurlFileStore || {};
  return s.body ? (s.body.length | 0) : 0;
});

EM_JS(int, kodi_curl_wasm_copy_body, (char* outPtr, int outSize), {
  var s = Module.kodiCurlFileStore || {};
  if (!s.body || !outPtr || outSize <= 0) return 0;
  var n = Math.min(s.body.length, outSize);
  HEAPU8.set(s.body.subarray(0, n), outPtr);
  return n | 0;
});

EM_JS(double, kodi_curl_wasm_content_length, (), {
  var s = Module.kodiCurlFileStore || {};
  return typeof s.contentLength === "number" ? s.contentLength : -1;
});

// fieldId: 0=rawHeaders 1=responseURL 2=statusText
EM_JS(int, kodi_curl_wasm_copy_string,
      (int fieldId, char* outPtr, int outSize), {
  var s = Module.kodiCurlFileStore || {};
  if (!outPtr || outSize <= 0) return 0;
  var v = "";
  if      (fieldId === 0) v = s.rawHeaders || "";
  else if (fieldId === 1) v = s.responseURL || "";
  else if (fieldId === 2) v = s.statusText || "";
  var bytes = lengthBytesUTF8(v) | 0;
  stringToUTF8(v, outPtr, outSize);
  return bytes;
});

EM_JS(int, kodi_curl_wasm_string_size, (int fieldId), {
  var s = Module.kodiCurlFileStore || {};
  var v = fieldId === 0 ? (s.rawHeaders || "")
        : fieldId === 1 ? (s.responseURL || "")
        : fieldId === 2 ? (s.statusText || "") : "";
  return lengthBytesUTF8(v) | 0;
});
// clang-format on

std::string FetchString(int fieldId)
{
  int size = kodi_curl_wasm_string_size(fieldId);
  std::string out(static_cast<size_t>(size + 1), '\0');
  kodi_curl_wasm_copy_string(fieldId, out.data(), size + 1);
  out.resize(size);
  return out;
}

void ParseResponseHeaders(const std::string& raw, CHttpHeader& out)
{
  out.Clear();
  size_t pos = 0;
  while (pos < raw.size())
  {
    size_t nl = raw.find('\n', pos);
    std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? raw.size() : nl + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!line.empty())
      out.Parse(line + "\r\n");
  }
}
} // namespace

// ---------------------------------------------------------------------------
// CReadState
// ---------------------------------------------------------------------------

CCurlFile::CReadState::CReadState()
  : m_easyHandle(nullptr),
    m_multiHandle(nullptr),
    m_buffer(),
    m_bufferSize(0),
    m_overflowBuffer(nullptr),
    m_overflowSize(0),
    m_stillRunning(0),
    m_cancelled(false),
    m_fileSize(0),
    m_filePos(0),
    m_bFirstLoop(true),
    m_isPaused(false),
    m_sendRange(false),
    m_bLastError(false),
    m_bRetry(true),
    m_readBuffer(nullptr),
    m_httpheader(),
    m_curlHeaderList(nullptr),
    m_curlAliasList(nullptr)
{
}

CCurlFile::CReadState::~CReadState()
{
  delete[] m_overflowBuffer;
  m_overflowBuffer = nullptr;
}

size_t CCurlFile::CReadState::ReadCallback(char*, size_t, size_t)
{
  return 0;
}
size_t CCurlFile::CReadState::WriteCallback(char*, size_t, size_t)
{
  return 0;
}
size_t CCurlFile::CReadState::HeaderCallback(void*, size_t, size_t)
{
  return 0;
}

bool CCurlFile::CReadState::Seek(int64_t pos)
{
  if (pos < 0 || pos > m_fileSize)
    return false;
  m_filePos = pos;
  return true;
}

ssize_t CCurlFile::CReadState::Read(void* lpBuf, size_t uiBufSize)
{
  if (!lpBuf || uiBufSize == 0 || !m_overflowBuffer)
    return 0;
  int64_t remaining = m_fileSize - m_filePos;
  if (remaining <= 0)
    return 0;
  size_t n = std::min(uiBufSize, static_cast<size_t>(remaining));
  std::memcpy(lpBuf, m_overflowBuffer + m_filePos, n);
  m_filePos += static_cast<int64_t>(n);
  return static_cast<ssize_t>(n);
}

IFile::ReadLineResult CCurlFile::CReadState::ReadLine(char* buffer, std::size_t bufferSize)
{
  if (!buffer || bufferSize == 0 || !m_overflowBuffer)
    return {IFile::ReadLineResult::FAILURE, 0};
  int64_t remaining = m_fileSize - m_filePos;
  if (remaining <= 0)
    return {IFile::ReadLineResult::FAILURE, 0};

  size_t max = std::min(bufferSize - 1, static_cast<size_t>(remaining));
  const char* src = m_overflowBuffer + m_filePos;
  size_t n = 0;
  bool sawNewline = false;
  for (; n < max; ++n)
  {
    buffer[n] = src[n];
    if (src[n] == '\n')
    {
      ++n;
      sawNewline = true;
      break;
    }
  }
  buffer[n] = '\0';
  m_filePos += static_cast<int64_t>(n);
  if (!sawNewline && n == max && static_cast<size_t>(remaining) > max)
    return {IFile::ReadLineResult::TRUNCATED, n};
  return {IFile::ReadLineResult::OK, n};
}

int8_t CCurlFile::CReadState::FillBuffer(unsigned int)
{
  return 0;
}
void CCurlFile::CReadState::SetReadBuffer(const void*, int64_t) {}
void CCurlFile::CReadState::SetResume() {}
long CCurlFile::CReadState::Connect(unsigned int)
{
  return -1;
}
void CCurlFile::CReadState::Disconnect() {}

// ---------------------------------------------------------------------------
// CCurlFile
// ---------------------------------------------------------------------------

CCurlFile::CCurlFile()
  : m_state(new CReadState()),
    m_oldState(nullptr),
    m_bufferSize(0),
    m_writeOffset(0),
    m_ftppasvip(false),
    m_connecttimeout(0),
    m_redirectlimit(5),
    m_lowspeedtime(0),
    m_opened(false),
    m_forWrite(false),
    m_inError(false),
    m_seekable(true),
    m_multisession(true),
    m_skipshout(false),
    m_postdataset(false),
    m_allowRetry(true),
    m_overflowBuffer(nullptr),
    m_stillRunning(0),
    m_httpresponse(-1)
{
}

CCurlFile::~CCurlFile()
{
  Close();
  delete m_state;
  m_state = nullptr;
  delete m_oldState;
  m_oldState = nullptr;
}

namespace
{
std::string FlattenHeaders(const std::map<std::string, std::string>& headers,
                           const std::string& cookie,
                           const std::string& acceptEncoding,
                           const std::string& acceptCharset)
{
  std::string out;
  for (const auto& [k, v] : headers)
  {
    if (k.empty())
      continue;
    out += k;
    out += ':';
    out += v;
    out += '\n';
  }
  if (!cookie.empty())
    out += "Cookie:" + cookie + "\n";
  if (!acceptEncoding.empty())
    out += "Accept-Encoding:" + acceptEncoding + "\n";
  if (!acceptCharset.empty())
    out += "Accept-Charset:" + acceptCharset + "\n";
  return out;
}
} // namespace

bool CCurlFile::Service(const std::string& strURL, std::string& strHTML)
{
  if (!Open(CURL(strURL)))
    return false;
  return ReadData(strHTML);
}

std::string CCurlFile::GetInfoString(int)
{
  return {};
}

void CCurlFile::ParseAndCorrectUrl(CURL& /*url*/) {}
void CCurlFile::SetCommonOptions(CReadState*, bool /*failOnError*/) {}
void CCurlFile::SetRequestHeaders(CReadState*) {}
void CCurlFile::SetCorrectHeaders(CReadState*) {}

bool CCurlFile::Open(const CURL& url)
{
  Close();

  if (!url.IsProtocol("http") && !url.IsProtocol("https"))
  {
    CLog::Log(LOGERROR, "CCurlFile::{} - unsupported protocol in <{}>", __FUNCTION__,
              url.GetRedacted());
    return false;
  }

  m_url = url.Get();
  const std::string method = m_customrequest.empty()
                                 ? (m_postdataset ? std::string("POST") : std::string("GET"))
                                 : m_customrequest;
  const std::string headers = FlattenHeaders(m_requestheaders, m_cookie, m_acceptencoding,
                                             m_acceptCharset);
  const std::string body = m_postdataset ? Base64::Decode(m_postdata) : std::string();

  m_httpresponse = kodi_curl_wasm_request(
      method.c_str(), m_url.c_str(),
      body.empty() ? nullptr : body.data(), static_cast<int>(body.size()),
      headers.empty() ? nullptr : headers.c_str(),
      m_userAgent.c_str(), m_referer.c_str(), m_connecttimeout, 1);

  if (m_httpresponse <= 0)
  {
    m_inError = true;
    return false;
  }

  const int bodySize = kodi_curl_wasm_body_size();
  delete[] m_state->m_overflowBuffer;
  m_state->m_overflowBuffer = nullptr;
  m_state->m_overflowSize = 0;
  if (bodySize > 0)
  {
    m_state->m_overflowBuffer = new char[bodySize];
    m_state->m_overflowSize = static_cast<unsigned int>(bodySize);
    kodi_curl_wasm_copy_body(m_state->m_overflowBuffer, bodySize);
  }
  m_state->m_fileSize = bodySize;
  m_state->m_filePos = 0;

  ParseResponseHeaders(FetchString(0), m_state->m_httpheader);
  const std::string responseUrl = FetchString(1);
  if (!responseUrl.empty())
    m_url = responseUrl;

  if (m_httpresponse >= 400 && m_failOnError)
  {
    CLog::Log(LOGERROR, "CCurlFile::{} - <{}> failed with code {}", __FUNCTION__,
              url.GetRedacted(), m_httpresponse);
    m_inError = true;
    return false;
  }

  m_opened = true;
  m_forWrite = false;
  m_seekable = true;
  return true;
}

bool CCurlFile::OpenForWrite(const CURL& /*url*/, bool /*bOverWrite*/)
{
  return false;
}

bool CCurlFile::ReOpen(const CURL& url)
{
  return Open(url);
}

bool CCurlFile::Exists(const CURL& url)
{
  if (!url.IsProtocol("http") && !url.IsProtocol("https"))
    return false;
  int status = kodi_curl_wasm_request("HEAD", url.Get().c_str(), nullptr, 0, nullptr,
                                      m_userAgent.c_str(), m_referer.c_str(),
                                      m_connecttimeout, 0);
  return status > 0 && status < 400;
}

int CCurlFile::Stat(const CURL& url, struct __stat64* buffer)
{
  if (!url.IsProtocol("http") && !url.IsProtocol("https"))
  {
    errno = ENOENT;
    return -1;
  }

  int status = kodi_curl_wasm_request("HEAD", url.Get().c_str(), nullptr, 0, nullptr,
                                      m_userAgent.c_str(), m_referer.c_str(),
                                      m_connecttimeout, 0);
  if (status <= 0 || status >= 400)
  {
    errno = ENOENT;
    return -1;
  }

  if (buffer)
  {
    *buffer = {};
    const double contentLength = kodi_curl_wasm_content_length();
    buffer->st_size = contentLength >= 0.0 ? static_cast<int64_t>(contentLength) : 0;
    buffer->st_mode = _S_IFREG;
  }
  return 0;
}

int64_t CCurlFile::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_opened)
    return -1;
  int64_t next = m_state->m_filePos;
  switch (iWhence)
  {
    case SEEK_SET:
      next = iFilePosition;
      break;
    case SEEK_CUR:
      next += iFilePosition;
      break;
    case SEEK_END:
      next = m_state->m_fileSize + iFilePosition;
      break;
    default:
      return -1;
  }
  if (!m_state->Seek(next))
    return -1;
  return m_state->m_filePos;
}

int64_t CCurlFile::GetPosition()
{
  return m_opened ? m_state->m_filePos : 0;
}

int64_t CCurlFile::GetLength()
{
  return m_opened ? m_state->m_fileSize : 0;
}

void CCurlFile::Close()
{
  if (m_state)
  {
    delete[] m_state->m_overflowBuffer;
    m_state->m_overflowBuffer = nullptr;
    m_state->m_overflowSize = 0;
    m_state->m_fileSize = 0;
    m_state->m_filePos = 0;
    m_state->m_httpheader.Clear();
  }
  m_opened = false;
  m_forWrite = false;
  m_inError = false;
  m_httpresponse = -1;
  m_url.clear();
}

ssize_t CCurlFile::Write(const void*, size_t)
{
  return -1;
}

const std::string CCurlFile::GetProperty(XFILE::FileProperty type, const std::string& name) const
{
  if (!m_state)
    return {};
  switch (type)
  {
    case FileProperty::RESPONSE_PROTOCOL:
      return m_state->m_httpheader.GetProtoLine();
    case FileProperty::RESPONSE_HEADER:
      return m_state->m_httpheader.GetValue(name);
    case FileProperty::CONTENT_TYPE:
      return m_state->m_httpheader.GetValue("content-type");
    case FileProperty::CONTENT_CHARSET:
      return m_state->m_httpheader.GetCharset();
    case FileProperty::MIME_TYPE:
      return m_state->m_httpheader.GetMimeType();
    case FileProperty::EFFECTIVE_URL:
      return m_url;
    default:
      return {};
  }
}

const std::vector<std::string> CCurlFile::GetPropertyValues(XFILE::FileProperty type,
                                                            const std::string& name) const
{
  if (!m_state)
    return {};
  if (type == FileProperty::RESPONSE_HEADER)
    return m_state->m_httpheader.GetValues(name);
  std::string value = GetProperty(type, name);
  if (value.empty())
    return {};
  return {std::move(value)};
}

int CCurlFile::IoControl(IOControl request, void* param)
{
  if (request == IOControl::SEEK_POSSIBLE)
    return 1;
  return IFile::IoControl(request, param);
}

double CCurlFile::GetDownloadSpeed()
{
  return 0.0;
}

bool CCurlFile::Post(const std::string& strURL, const std::string& strPostData, std::string& strHTML)
{
  m_postdata = Base64::Encode(strPostData);
  m_postdataset = true;
  const bool ok = Service(strURL, strHTML);
  m_postdataset = false;
  m_postdata.clear();
  return ok;
}

bool CCurlFile::Get(const std::string& strURL, std::string& strHTML)
{
  m_postdataset = false;
  return Service(strURL, strHTML);
}

bool CCurlFile::ReadData(std::string& strHTML)
{
  strHTML.clear();
  if (!m_state || !m_state->m_overflowBuffer)
    return false;
  const size_t remaining = static_cast<size_t>(m_state->m_fileSize - m_state->m_filePos);
  if (remaining == 0)
    return m_httpresponse > 0;
  strHTML.assign(m_state->m_overflowBuffer + m_state->m_filePos, remaining);
  m_state->m_filePos = m_state->m_fileSize;
  return true;
}

bool CCurlFile::Download(const std::string& strURL,
                         const std::string& strFileName,
                         unsigned int* pdwSize)
{
  std::string data;
  if (!Get(strURL, data))
    return false;

  FILE* fp = std::fopen(strFileName.c_str(), "wb");
  if (!fp)
    return false;
  const size_t written = std::fwrite(data.data(), 1, data.size(), fp);
  std::fclose(fp);
  if (pdwSize)
    *pdwSize = static_cast<unsigned int>(written);
  return written == data.size();
}

bool CCurlFile::IsInternet()
{
  std::string body;
  if (Get("https://www.msftconnecttest.com/connecttest.txt", body))
    return true;
  return Get("https://www.w3.org/", body);
}

void CCurlFile::Cancel()
{
  if (m_state)
    m_state->m_cancelled = true;
}
void CCurlFile::Reset()
{
  if (m_state)
    m_state->m_cancelled = false;
}

void CCurlFile::SetProxy(const std::string& /*type*/, const std::string& /*host*/,
                         uint16_t /*port*/, const std::string& /*user*/,
                         const std::string& /*password*/)
{
}

void CCurlFile::SetRequestHeader(const std::string& header, const std::string& value)
{
  m_requestheaders[header] = value;
}

void CCurlFile::SetRequestHeader(const std::string& header, long value)
{
  m_requestheaders[header] = std::to_string(value);
}

void CCurlFile::ClearRequestHeaders()
{
  m_requestheaders.clear();
}

void CCurlFile::SetBufferSize(unsigned int size)
{
  m_bufferSize = size;
}

std::string CCurlFile::GetRedirectURL()
{
  if (!m_state)
    return {};
  const std::string loc = m_state->m_httpheader.GetValue("location");
  if (!loc.empty())
    return loc;
  return m_url;
}

// ---------- static helpers ----------

bool CCurlFile::GetHttpHeader(const CURL& url, CHttpHeader& headers)
{
  CCurlFile probe;
  int status = kodi_curl_wasm_request("HEAD", url.Get().c_str(), nullptr, 0, nullptr,
                                      "", "", 0, 0);
  if (status <= 0)
    return false;
  ParseResponseHeaders(FetchString(0), headers);
  return true;
}

bool CCurlFile::GetMimeType(const CURL& url, std::string& content, const std::string& useragent)
{
  CCurlFile probe;
  probe.SetUserAgent(useragent);
  CHttpHeader h;
  if (!GetHttpHeader(url, h))
    return false;
  content = h.GetMimeType();
  return !content.empty();
}

bool CCurlFile::GetContentType(const CURL& url, std::string& content, const std::string& useragent)
{
  CCurlFile probe;
  probe.SetUserAgent(useragent);
  CHttpHeader h;
  if (!GetHttpHeader(url, h))
    return false;
  content = h.GetValue("content-type");
  return !content.empty();
}

bool CCurlFile::GetCookies(const CURL& /*url*/, std::string& cookies)
{
  cookies.clear();
  return false;
}

void CCurlFile::PreloadCaCertsBlob()
{
  // No-op: the browser manages CA certs.
}
