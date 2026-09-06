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
// also handled transparently. GET responses are read through ranged windows
// so seeking in a large file does not download all of it.
//
// The public ABI (CCurlFile + CReadState) is preserved so DAVFile, Repository,
// HTTPDirectory, ShoutcastFile, ... keep compiling unchanged. Curl-specific
// internals (m_easyHandle, m_multiHandle, curl_slist, g_curlInterface) are
// left as nullptr/no-op; callers that reach into those will fail cleanly.

#include "filesystem/CurlFile.h"

#include "ServiceBroker.h"
#include "dialogs/GUIDialogKaiToast.h"

#include "URL.h"
#include "utils/Base64.h"
#include "utils/CharsetConverter.h"
#include "utils/HttpHeader.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include <emscripten.h>

using namespace XFILE;

namespace
{
constexpr int DEFAULT_REQUEST_TIMEOUT_SECONDS = 30;
constexpr double MAX_EAGER_RESPONSE_BYTES = 1024.0 * 1024.0 * 1024.0;
constexpr int64_t RANGE_WINDOW_BYTES = 2 * 1024 * 1024;

constexpr int8_t FILLBUFFER_OK = 0;
constexpr int8_t FILLBUFFER_NO_DATA = 1;
constexpr int8_t FILLBUFFER_FAIL = -1;

int EffectiveTimeout(int timeoutSeconds)
{
  return timeoutSeconds > 0 ? timeoutSeconds : DEFAULT_REQUEST_TIMEOUT_SECONDS;
}

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
  store.error = "";

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
    if (store.status === 0)
      store.error = "no response (blocked by the browser: CORS, mixed content or network)";
    return store.status;
  } catch (e) {
    store.error = (e && e.name ? e.name + ": " : "") + (e && e.message ? e.message : String(e));
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

// fieldId: 0=rawHeaders 1=responseURL 2=statusText 3=error
EM_JS(int, kodi_curl_wasm_copy_string,
      (int fieldId, char* outPtr, int outSize), {
  var s = Module.kodiCurlFileStore || {};
  if (!outPtr || outSize <= 0) return 0;
  var v = "";
  if      (fieldId === 0) v = s.rawHeaders || "";
  else if (fieldId === 1) v = s.responseURL || "";
  else if (fieldId === 2) v = s.statusText || "";
  else if (fieldId === 3) v = s.error || "";
  var bytes = lengthBytesUTF8(v) | 0;
  stringToUTF8(v, outPtr, outSize);
  return bytes;
});

EM_JS(int, kodi_curl_wasm_string_size, (int fieldId), {
  var s = Module.kodiCurlFileStore || {};
  var v = fieldId === 0 ? (s.rawHeaders || "")
        : fieldId === 1 ? (s.responseURL || "")
        : fieldId === 2 ? (s.statusText || "")
        : fieldId === 3 ? (s.error || "") : "";
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

// Request parameters a CReadState needs to fetch further windows of the
// resource it was opened on; CReadState::m_easyHandle points at it.
struct RequestContext
{
  std::string url;
  std::string headers;
  std::string userAgent;
  std::string referer;
  int timeout{DEFAULT_REQUEST_TIMEOUT_SECONDS};
};

RequestContext* Context(const CCurlFile::CReadState& state)
{
  return static_cast<RequestContext*>(state.m_easyHandle);
}

struct WindowResponse
{
  int status{0};
  std::string error;
  std::string rawHeaders;
  std::string responseUrl;
  std::unique_ptr<char[]> body;
  unsigned int bodySize{0};
  // Set for a 206 whose Content-Range matches the body: first is the offset of
  // body[0] in the resource, total the resource length.
  bool partial{false};
  int64_t first{0};
  int64_t total{0};
};

// Content-Range: bytes <first>-<last>/<length>
bool ParseContentRange(const std::string& value, int64_t& first, int64_t& last, int64_t& length)
{
  long long f = 0;
  long long l = 0;
  long long len = 0;
  if (std::sscanf(value.c_str(), "bytes %lld-%lld/%lld", &f, &l, &len) != 3 || f < 0 || l < f ||
      len <= l)
    return false;
  first = f;
  last = l;
  length = len;
  return true;
}

// Fetches [rangeStart, rangeStart + rangeLength) of the resource, or all of it
// when rangeLength is 0. A server that ignores the range answers 200 with the
// whole body.
WindowResponse RequestWindow(const RequestContext& ctx,
                             const std::string& method,
                             const std::string& body,
                             int64_t rangeStart,
                             int64_t rangeLength)
{
  std::string headers = ctx.headers;
  if (rangeLength > 0)
    headers += StringUtils::Format("Range:bytes={}-{}\n", rangeStart, rangeStart + rangeLength - 1);

  WindowResponse out;
  out.status = kodi_curl_wasm_request(
      method.c_str(), ctx.url.c_str(), body.empty() ? nullptr : body.data(),
      static_cast<int>(body.size()), headers.empty() ? nullptr : headers.c_str(),
      ctx.userAgent.c_str(), ctx.referer.c_str(), ctx.timeout, 1);
  if (out.status <= 0)
  {
    out.error = FetchString(3);
    return out;
  }

  out.rawHeaders = FetchString(0);
  out.responseUrl = FetchString(1);
  const int bodySize = kodi_curl_wasm_body_size();
  if (bodySize > MAX_EAGER_RESPONSE_BYTES)
  {
    out.status = 0;
    out.error = StringUtils::Format("refusing to keep a {:.1f} MiB response in the WASM heap",
                                    static_cast<double>(bodySize) / (1024.0 * 1024.0));
    return out;
  }
  if (bodySize > 0)
  {
    out.body.reset(new char[bodySize]);
    out.bodySize = static_cast<unsigned int>(bodySize);
    kodi_curl_wasm_copy_body(out.body.get(), bodySize);
  }
  if (out.status == 206)
  {
    CHttpHeader header;
    ParseResponseHeaders(out.rawHeaders, header);
    int64_t last = 0;
    out.partial = ParseContentRange(header.GetValue("content-range"), out.first, last, out.total) &&
                  last - out.first + 1 == out.bodySize;
  }
  return out;
}
} // namespace

// ---------------------------------------------------------------------------
// CReadState
// ---------------------------------------------------------------------------
// One window of the resource lives in m_overflowBuffer/m_overflowSize.
// m_bufferSize is the read offset inside that window, m_filePos the absolute
// position, and m_sendRange records that the server honours ranges so the
// window can be moved.

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
  delete Context(*this);
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
  const int64_t offset = pos - (m_filePos - m_bufferSize);
  if (offset >= 0 && offset <= m_overflowSize)
  {
    m_bufferSize = static_cast<unsigned int>(offset);
  }
  else
  {
    delete[] m_overflowBuffer;
    m_overflowBuffer = nullptr;
    m_overflowSize = 0;
    m_bufferSize = 0;
  }
  m_filePos = pos;
  return true;
}

ssize_t CCurlFile::CReadState::Read(void* lpBuf, size_t uiBufSize)
{
  if (!lpBuf || uiBufSize == 0)
    return 0;
  if (m_bufferSize >= m_overflowSize)
  {
    const auto want = static_cast<unsigned int>(
        std::min<size_t>(uiBufSize, std::numeric_limits<unsigned int>::max()));
    const int8_t filled = FillBuffer(want);
    if (filled != FILLBUFFER_OK)
      return filled == FILLBUFFER_NO_DATA ? 0 : -1;
  }
  const size_t n = std::min(uiBufSize, static_cast<size_t>(m_overflowSize - m_bufferSize));
  std::memcpy(lpBuf, m_overflowBuffer + m_bufferSize, n);
  m_bufferSize += static_cast<unsigned int>(n);
  m_filePos += static_cast<int64_t>(n);
  return static_cast<ssize_t>(n);
}

IFile::ReadLineResult CCurlFile::CReadState::ReadLine(char* buffer, std::size_t bufferSize)
{
  if (!buffer || bufferSize == 0)
    return {IFile::ReadLineResult::FAILURE, 0};

  size_t n = 0;
  bool sawNewline = false;
  while (n < bufferSize - 1 && !sawNewline)
  {
    if (m_bufferSize >= m_overflowSize)
    {
      const int8_t filled = FillBuffer(1);
      if (filled == FILLBUFFER_FAIL && n == 0)
        return {IFile::ReadLineResult::FAILURE, 0};
      if (filled != FILLBUFFER_OK)
        break;
    }
    const char* src = m_overflowBuffer + m_bufferSize;
    const size_t scan =
        std::min(static_cast<size_t>(m_overflowSize - m_bufferSize), bufferSize - 1 - n);
    const char* nl = static_cast<const char*>(std::memchr(src, '\n', scan));
    const size_t take = nl ? static_cast<size_t>(nl - src) + 1 : scan;
    std::memcpy(buffer + n, src, take);
    n += take;
    m_bufferSize += static_cast<unsigned int>(take);
    m_filePos += static_cast<int64_t>(take);
    sawNewline = nl != nullptr;
  }
  buffer[n] = '\0';
  if (n == 0)
    return {IFile::ReadLineResult::FAILURE, 0};
  if (!sawNewline && n == bufferSize - 1 && m_filePos < m_fileSize)
    return {IFile::ReadLineResult::TRUNCATED, n};
  return {IFile::ReadLineResult::OK, n};
}

// Moves the window to m_filePos, covering at least `want` bytes.
int8_t CCurlFile::CReadState::FillBuffer(unsigned int want)
{
  if (m_filePos >= m_fileSize)
    return FILLBUFFER_NO_DATA;
  const RequestContext* ctx = Context(*this);
  if (!ctx || !m_sendRange)
    return FILLBUFFER_FAIL;

  const int64_t length = std::max<int64_t>(want, RANGE_WINDOW_BYTES);
  WindowResponse response = RequestWindow(*ctx, "GET", {}, m_filePos, length);
  if (response.status == 416)
    return FILLBUFFER_NO_DATA;
  const bool usable =
      response.status == 206 ? response.partial : response.status > 0 && response.status < 400;
  const int64_t first = response.partial ? response.first : 0;
  if (!usable || m_filePos < first || m_filePos >= first + response.bodySize)
  {
    CLog::Log(LOGERROR, "CCurlFile::{} - range {}+{} of <{}> failed: HTTP {} {}", __FUNCTION__,
              m_filePos, length, CURL::GetRedacted(ctx->url), response.status, response.error);
    return FILLBUFFER_FAIL;
  }

  if (response.partial)
  {
    m_fileSize = response.total;
  }
  else
  {
    m_fileSize = response.bodySize;
    m_sendRange = false;
  }
  delete[] m_overflowBuffer;
  m_overflowBuffer = response.body.release();
  m_overflowSize = response.bodySize;
  m_bufferSize = static_cast<unsigned int>(m_filePos - first);
  return FILLBUFFER_OK;
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

bool HasHeader(const std::map<std::string, std::string>& headers, const char* name)
{
  return std::any_of(headers.begin(), headers.end(),
                     [name](const auto& kv) { return StringUtils::EqualsNoCase(kv.first, name); });
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
  const std::string body = m_postdataset ? Base64::Decode(m_postdata) : std::string();

  auto* ctx = new RequestContext{
      m_url, FlattenHeaders(m_requestheaders, m_cookie, m_acceptencoding, m_acceptCharset),
      m_userAgent, m_referer, EffectiveTimeout(m_connecttimeout)};
  m_state->m_easyHandle = ctx;

  // Only a plain GET is read in windows; other requests are taken whole.
  const bool windowed = method == "GET" && !HasHeader(m_requestheaders, "Range");
  WindowResponse response =
      RequestWindow(*ctx, method, body, 0, windowed ? RANGE_WINDOW_BYTES : 0);
  if (response.status == 206 && (!response.partial || response.first != 0))
    response = RequestWindow(*ctx, method, body, 0, 0);
  m_httpresponse = response.status;

  if (m_httpresponse <= 0)
  {
    CLog::Log(LOGERROR, "CCurlFile::{} - {} <{}> failed: {}", __FUNCTION__, method,
              url.GetRedacted(), response.error);
    // The TV has no console to read this from; with debug logging on, show it.
    if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG))
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "HTTP " + method + " failed",
                                            url.GetHostName() + ": " + response.error, 8000);
    m_inError = true;
    return false;
  }

  ParseResponseHeaders(response.rawHeaders, m_state->m_httpheader);
  if (!response.responseUrl.empty())
  {
    m_url = response.responseUrl;
    ctx->url = m_url;
  }

  m_state->m_sendRange = response.partial;
  m_state->m_fileSize = response.partial ? response.total : response.bodySize;
  m_state->m_filePos = 0;
  m_state->m_bufferSize = 0;
  delete[] m_state->m_overflowBuffer;
  m_state->m_overflowBuffer = response.body.release();
  m_state->m_overflowSize = response.bodySize;

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
                                      EffectiveTimeout(m_connecttimeout), 0);
  return status > 0 && status < 400;
}

int CCurlFile::Stat(const CURL& url, struct __stat64* buffer)
{
  if (m_opened)
  {
    CLog::Log(LOGWARNING, "CCurlFile::{} - <{}> Stat called on open file", __FUNCTION__,
              url.GetRedacted());
    if (buffer)
    {
      *buffer = {};
      buffer->st_size = GetLength();
      buffer->st_mode = _S_IFREG;
    }
    return 0;
  }

  if (!url.IsProtocol("http") && !url.IsProtocol("https"))
  {
    errno = ENOENT;
    return -1;
  }

  int status = kodi_curl_wasm_request("HEAD", url.Get().c_str(), nullptr, 0, nullptr,
                                      m_userAgent.c_str(), m_referer.c_str(),
                                      EffectiveTimeout(m_connecttimeout), 0);
  if (status <= 0 || status >= 400)
  {
    errno = ENOENT;
    return -1;
  }

  ParseResponseHeaders(FetchString(0), m_state->m_httpheader);

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
    delete Context(*m_state);
    m_state->m_easyHandle = nullptr;
    delete[] m_state->m_overflowBuffer;
    m_state->m_overflowBuffer = nullptr;
    m_state->m_overflowSize = 0;
    m_state->m_bufferSize = 0;
    m_state->m_fileSize = 0;
    m_state->m_filePos = 0;
    m_state->m_sendRange = false;
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
  if (!m_state)
    return false;
  while (m_state->m_filePos < m_state->m_fileSize)
  {
    if (m_state->m_bufferSize >= m_state->m_overflowSize)
    {
      const int64_t remaining = m_state->m_fileSize - m_state->m_filePos;
      const int8_t filled = m_state->FillBuffer(static_cast<unsigned int>(
          std::min<int64_t>(remaining, std::numeric_limits<unsigned int>::max())));
      if (filled == FILLBUFFER_FAIL)
        return false;
      if (filled == FILLBUFFER_NO_DATA)
        break;
    }
    const size_t available = m_state->m_overflowSize - m_state->m_bufferSize;
    strHTML.append(m_state->m_overflowBuffer + m_state->m_bufferSize, available);
    m_state->m_bufferSize += static_cast<unsigned int>(available);
    m_state->m_filePos += static_cast<int64_t>(available);
  }
  return m_httpresponse > 0;
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

// The url is driven directly rather than through CFile, so stored credentials
// (passwords.xml) have to be applied here or an authenticated source is probed
// anonymously and answers 401.
bool CCurlFile::GetHttpHeader(const CURL& url, CHttpHeader& headers)
{
  CCurlFile probe;
  if (probe.Stat(URIUtils::AddCredentials(url), nullptr) != 0)
    return false;
  headers = probe.GetHttpHeader();
  return true;
}

bool CCurlFile::GetMimeType(const CURL& url, std::string& content, const std::string& useragent)
{
  CCurlFile probe;
  if (!useragent.empty())
    probe.SetUserAgent(useragent);
  if (probe.Stat(URIUtils::AddCredentials(url), nullptr) != 0)
    return false;
  content = probe.GetHttpHeader().GetMimeType();
  return !content.empty();
}

bool CCurlFile::GetContentType(const CURL& url, std::string& content, const std::string& useragent)
{
  CCurlFile probe;
  if (!useragent.empty())
    probe.SetUserAgent(useragent);
  if (probe.Stat(URIUtils::AddCredentials(url), nullptr) != 0)
    return false;
  content = probe.GetHttpHeader().GetValue("content-type");
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
