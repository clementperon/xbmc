/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "XhrFile.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#include <emscripten.h>

using namespace XFILE;

namespace
{
constexpr int DEFAULT_REQUEST_TIMEOUT_SECONDS = 30;
constexpr int MAX_EAGER_RESPONSE_BYTES = 1024 * 1024 * 1024;
constexpr int64_t RANGE_WINDOW_BYTES = 2 * 1024 * 1024;

// Synchronous XHR on a pthread serializes requests per thread, so one store
// per module suffices between a request and the copies that follow it.
// clang-format off
EM_JS(int, kodi_xhr_request,
      (const char* methodPtr, const char* urlPtr, const char* bodyPtr, int bodyLen,
       const char* headersPtr, const char* userAgentPtr, const char* refererPtr,
       int timeoutSeconds, int includeBody), {
  var method = UTF8ToString(methodPtr) || "GET";
  var url = UTF8ToString(urlPtr);
  var body = bodyLen > 0 ? HEAPU8.slice(bodyPtr, bodyPtr + bodyLen) : null;
  var headers = headersPtr ? UTF8ToString(headersPtr) : "";
  var userAgent = userAgentPtr ? UTF8ToString(userAgentPtr) : "";
  var referer = refererPtr ? UTF8ToString(refererPtr) : "";

  var store = Module.kodiXhrStore = Module.kodiXhrStore || {};
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
    // headers is "name:value\nname:value\n..."
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
    console.warn("[kodi][xhr] request failed:", method, url, e);
    return 0;
  }
});

EM_JS(int, kodi_xhr_body_size, (), {
  var s = Module.kodiXhrStore || {};
  return s.body ? (s.body.length | 0) : 0;
});

EM_JS(int, kodi_xhr_copy_body, (char* outPtr, int outSize), {
  var s = Module.kodiXhrStore || {};
  if (!s.body || !outPtr || outSize <= 0) return 0;
  var n = Math.min(s.body.length, outSize);
  HEAPU8.set(s.body.subarray(0, n), outPtr);
  return n | 0;
});

EM_JS(double, kodi_xhr_content_length, (), {
  var s = Module.kodiXhrStore || {};
  return typeof s.contentLength === "number" ? s.contentLength : -1;
});

// fieldId: 0=rawHeaders 1=responseURL 2=statusText 3=error
EM_JS(int, kodi_xhr_copy_string, (int fieldId, char* outPtr, int outSize), {
  var s = Module.kodiXhrStore || {};
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

EM_JS(int, kodi_xhr_string_size, (int fieldId), {
  var s = Module.kodiXhrStore || {};
  var v = fieldId === 0 ? (s.rawHeaders || "")
        : fieldId === 1 ? (s.responseURL || "")
        : fieldId === 2 ? (s.statusText || "")
        : fieldId === 3 ? (s.error || "") : "";
  return lengthBytesUTF8(v) | 0;
});
// clang-format on

std::string FetchString(int fieldId)
{
  int size = kodi_xhr_string_size(fieldId);
  std::string out(static_cast<size_t>(size + 1), '\0');
  kodi_xhr_copy_string(fieldId, out.data(), size + 1);
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

bool IsHttp(const CURL& url)
{
  return url.IsProtocol("http") || url.IsProtocol("https");
}

bool HasHeader(const std::map<std::string, std::string>& headers, const char* name)
{
  return std::any_of(headers.begin(), headers.end(),
                     [name](const auto& kv) { return StringUtils::EqualsNoCase(kv.first, name); });
}
} // namespace

struct CXhrFile::Request
{
  std::string url;
  std::string headers;
  std::string userAgent;
  std::string referer;
  int timeout{DEFAULT_REQUEST_TIMEOUT_SECONDS};
};

struct CXhrFile::Response
{
  int status{0};
  std::string error;
  std::string rawHeaders;
  std::string responseUrl;
  std::unique_ptr<char[]> body;
  size_t bodySize{0};
  // Set for a 206 whose Content-Range matches the body: first is the offset of
  // body[0] in the resource, total the resource length.
  bool partial{false};
  int64_t first{0};
  int64_t total{0};
};

CXhrFile::CXhrFile() = default;

CXhrFile::~CXhrFile()
{
  Close();
}

CXhrFile::Request CXhrFile::BuildRequest() const
{
  Request request;
  request.url = m_url;
  for (const auto& [name, value] : m_requestHeaders)
  {
    if (!name.empty())
      request.headers += name + ':' + value + '\n';
  }
  if (!m_cookie.empty())
    request.headers += "Cookie:" + m_cookie + "\n";
  if (!m_acceptEncoding.empty())
    request.headers += "Accept-Encoding:" + m_acceptEncoding + "\n";
  if (!m_acceptCharset.empty())
    request.headers += "Accept-Charset:" + m_acceptCharset + "\n";
  request.userAgent = m_userAgent;
  request.referer = m_referer;
  request.timeout = m_timeout > 0 ? m_timeout : DEFAULT_REQUEST_TIMEOUT_SECONDS;
  return request;
}

// Fetches [rangeStart, rangeStart + rangeLength) of the resource, or all of it
// when rangeLength is 0. A server that ignores the range answers 200 with the
// whole body.
CXhrFile::Response CXhrFile::Fetch(const Request& request,
                                   const std::string& method,
                                   const std::string& body,
                                   int64_t rangeStart,
                                   int64_t rangeLength) const
{
  std::string headers = request.headers;
  if (rangeLength > 0)
    headers += StringUtils::Format("Range:bytes={}-{}\n", rangeStart, rangeStart + rangeLength - 1);

  Response out;
  out.status = kodi_xhr_request(method.c_str(), request.url.c_str(),
                                body.empty() ? nullptr : body.data(), static_cast<int>(body.size()),
                                headers.empty() ? nullptr : headers.c_str(),
                                request.userAgent.c_str(), request.referer.c_str(),
                                request.timeout, 1);
  if (out.status <= 0)
  {
    out.error = FetchString(3);
    return out;
  }

  out.rawHeaders = FetchString(0);
  out.responseUrl = FetchString(1);
  const int bodySize = kodi_xhr_body_size();
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
    out.bodySize = static_cast<size_t>(bodySize);
    kodi_xhr_copy_body(out.body.get(), bodySize);
  }
  if (out.status == 206)
  {
    CHttpHeader header;
    ParseResponseHeaders(out.rawHeaders, header);
    int64_t last = 0;
    out.partial = ParseContentRange(header.GetValue("content-range"), out.first, last, out.total) &&
                  last - out.first + 1 == static_cast<int64_t>(out.bodySize);
  }
  return out;
}

bool CXhrFile::Open(const CURL& url)
{
  Close();

  if (!IsHttp(url))
  {
    CLog::LogF(LOGERROR, "unsupported protocol in <{}>", url.GetRedacted());
    return false;
  }

  m_url = url.Get();
  const std::string method =
      !m_customRequest.empty() ? m_customRequest : (m_postDataSet ? "POST" : "GET");
  const std::string& body = m_postDataSet ? m_postData : std::string();
  Request request = BuildRequest();

  // Only a plain GET is read in windows; other requests are taken whole.
  const bool windowed = method == "GET" && !HasHeader(m_requestHeaders, "Range");
  Response response = Fetch(request, method, body, 0, windowed ? RANGE_WINDOW_BYTES : 0);
  if (response.status == 206 && (!response.partial || response.first != 0))
    response = Fetch(request, method, body, 0, 0);
  m_httpResponse = response.status;

  if (m_httpResponse <= 0)
  {
    CLog::LogF(LOGERROR, "{} <{}> failed: {}", method, url.GetRedacted(), response.error);
    // The TV has no console to read this from; with debug logging on, show it.
    if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG))
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, "HTTP " + method + " failed",
                                            url.GetHostName() + ": " + response.error, 8000);
    return false;
  }

  ParseResponseHeaders(response.rawHeaders, m_httpHeader);
  if (!response.responseUrl.empty())
    m_url = response.responseUrl;

  m_rangeSupported = response.partial;
  m_fileSize = response.partial ? response.total : static_cast<int64_t>(response.bodySize);
  m_filePos = 0;
  m_windowStart = 0;
  m_windowSize = response.bodySize;
  m_window = std::move(response.body);

  if (m_httpResponse >= 400)
  {
    CLog::LogF(LOGERROR, "<{}> failed with code {}", url.GetRedacted(), m_httpResponse);
    return false;
  }

  m_opened = true;
  return true;
}

bool CXhrFile::Head(const CURL& url)
{
  if (!IsHttp(url))
    return false;
  const int timeout = m_timeout > 0 ? m_timeout : DEFAULT_REQUEST_TIMEOUT_SECONDS;
  const int status = kodi_xhr_request("HEAD", url.Get().c_str(), nullptr, 0, nullptr,
                                      m_userAgent.c_str(), m_referer.c_str(), timeout, 0);
  if (status <= 0 || status >= 400)
    return false;
  ParseResponseHeaders(FetchString(0), m_httpHeader);
  return true;
}

bool CXhrFile::Exists(const CURL& url)
{
  return Head(url);
}

int CXhrFile::Stat(const CURL& url, struct __stat64* buffer)
{
  if (m_opened)
  {
    if (buffer)
    {
      *buffer = {};
      buffer->st_size = m_fileSize;
      buffer->st_mode = _S_IFREG;
    }
    return 0;
  }

  if (!Head(url))
  {
    errno = ENOENT;
    return -1;
  }

  if (buffer)
  {
    *buffer = {};
    const double contentLength = kodi_xhr_content_length();
    buffer->st_size = contentLength >= 0.0 ? static_cast<int64_t>(contentLength) : 0;
    buffer->st_mode = _S_IFREG;
  }
  return 0;
}

void CXhrFile::DropWindow()
{
  m_window.reset();
  m_windowSize = 0;
  m_windowStart = 0;
}

// Moves the window so that it starts at `position` and covers at least `want`
// bytes.
bool CXhrFile::FillWindow(int64_t position, size_t want)
{
  if (!m_rangeSupported)
    return false;

  const int64_t length = std::max<int64_t>(static_cast<int64_t>(want), RANGE_WINDOW_BYTES);
  Response response = Fetch(BuildRequest(), "GET", {}, position, length);
  const bool usable =
      response.status == 206 ? response.partial : response.status > 0 && response.status < 400;
  const int64_t first = response.partial ? response.first : 0;
  if (!usable || position < first || position >= first + static_cast<int64_t>(response.bodySize))
  {
    CLog::LogF(LOGERROR, "range {}+{} of <{}> failed: HTTP {} {}", position, length,
               CURL::GetRedacted(m_url), response.status, response.error);
    return false;
  }

  if (response.partial)
  {
    m_fileSize = response.total;
  }
  else
  {
    m_fileSize = static_cast<int64_t>(response.bodySize);
    m_rangeSupported = false;
  }
  m_window = std::move(response.body);
  m_windowSize = response.bodySize;
  m_windowStart = first;
  return true;
}

ssize_t CXhrFile::Read(void* lpBuf, size_t uiBufSize)
{
  if (!m_opened || !lpBuf || uiBufSize == 0)
    return 0;
  if (m_filePos >= m_fileSize)
    return 0;
  if (m_filePos < m_windowStart || m_filePos >= m_windowStart + static_cast<int64_t>(m_windowSize))
  {
    if (!FillWindow(m_filePos, uiBufSize))
      return -1;
  }
  const size_t offset = static_cast<size_t>(m_filePos - m_windowStart);
  const size_t n = std::min(uiBufSize, m_windowSize - offset);
  std::memcpy(lpBuf, m_window.get() + offset, n);
  m_filePos += static_cast<int64_t>(n);
  return static_cast<ssize_t>(n);
}

IFile::ReadLineResult CXhrFile::ReadLine(char* buffer, std::size_t bufferSize)
{
  if (!m_opened || !buffer || bufferSize == 0)
    return {ReadLineResult::FAILURE, 0};

  size_t n = 0;
  bool sawNewline = false;
  while (n < bufferSize - 1 && !sawNewline && m_filePos < m_fileSize)
  {
    if (m_filePos < m_windowStart ||
        m_filePos >= m_windowStart + static_cast<int64_t>(m_windowSize))
    {
      if (!FillWindow(m_filePos, 1))
      {
        if (n == 0)
          return {ReadLineResult::FAILURE, 0};
        break;
      }
    }
    const size_t offset = static_cast<size_t>(m_filePos - m_windowStart);
    const char* src = m_window.get() + offset;
    const size_t scan = std::min(m_windowSize - offset, bufferSize - 1 - n);
    const char* nl = static_cast<const char*>(std::memchr(src, '\n', scan));
    const size_t take = nl ? static_cast<size_t>(nl - src) + 1 : scan;
    std::memcpy(buffer + n, src, take);
    n += take;
    m_filePos += static_cast<int64_t>(take);
    sawNewline = nl != nullptr;
  }
  buffer[n] = '\0';
  if (n == 0)
    return {ReadLineResult::FAILURE, 0};
  if (!sawNewline && n == bufferSize - 1 && m_filePos < m_fileSize)
    return {ReadLineResult::TRUNCATED, n};
  return {ReadLineResult::OK, n};
}

int64_t CXhrFile::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_opened)
    return -1;
  int64_t next = m_filePos;
  switch (iWhence)
  {
    case SEEK_SET:
      next = iFilePosition;
      break;
    case SEEK_CUR:
      next += iFilePosition;
      break;
    case SEEK_END:
      next = m_fileSize + iFilePosition;
      break;
    default:
      return -1;
  }
  if (next < 0 || next > m_fileSize)
    return -1;
  m_filePos = next;
  return m_filePos;
}

int64_t CXhrFile::GetPosition()
{
  return m_opened ? m_filePos : 0;
}

int64_t CXhrFile::GetLength()
{
  return m_opened ? m_fileSize : 0;
}

void CXhrFile::Close()
{
  DropWindow();
  m_httpHeader.Clear();
  m_fileSize = 0;
  m_filePos = 0;
  m_rangeSupported = false;
  m_opened = false;
  m_httpResponse = -1;
  m_url.clear();
}

const std::string CXhrFile::GetProperty(XFILE::FileProperty type, const std::string& name) const
{
  switch (type)
  {
    case FileProperty::RESPONSE_PROTOCOL:
      return m_httpHeader.GetProtoLine();
    case FileProperty::RESPONSE_HEADER:
      return m_httpHeader.GetValue(name);
    case FileProperty::CONTENT_TYPE:
      return m_httpHeader.GetValue("content-type");
    case FileProperty::CONTENT_CHARSET:
      return m_httpHeader.GetCharset();
    case FileProperty::MIME_TYPE:
      return m_httpHeader.GetMimeType();
    case FileProperty::EFFECTIVE_URL:
      return m_url;
    default:
      return {};
  }
}

const std::vector<std::string> CXhrFile::GetPropertyValues(XFILE::FileProperty type,
                                                           const std::string& name) const
{
  if (type == FileProperty::RESPONSE_HEADER)
    return m_httpHeader.GetValues(name);
  std::string value = GetProperty(type, name);
  if (value.empty())
    return {};
  return {std::move(value)};
}

int CXhrFile::IoControl(IOControl request, void* param)
{
  if (request == IOControl::SEEK_POSSIBLE)
    return 1;
  return IFile::IoControl(request, param);
}

bool CXhrFile::Get(const std::string& url, std::string& body)
{
  m_postDataSet = false;
  if (!Open(CURL(url)))
    return false;
  return ReadData(body);
}

bool CXhrFile::Post(const std::string& url, const std::string& postData, std::string& body)
{
  m_postData = postData;
  m_postDataSet = true;
  const bool opened = Open(CURL(url));
  m_postDataSet = false;
  m_postData.clear();
  return opened && ReadData(body);
}

bool CXhrFile::ReadData(std::string& body)
{
  body.clear();
  if (!m_opened)
    return false;
  while (m_filePos < m_fileSize)
  {
    if (m_filePos < m_windowStart ||
        m_filePos >= m_windowStart + static_cast<int64_t>(m_windowSize))
    {
      if (!FillWindow(m_filePos, static_cast<size_t>(std::min<int64_t>(
                                     m_fileSize - m_filePos, std::numeric_limits<int>::max()))))
        return false;
    }
    const size_t offset = static_cast<size_t>(m_filePos - m_windowStart);
    body.append(m_window.get() + offset, m_windowSize - offset);
    m_filePos = m_windowStart + static_cast<int64_t>(m_windowSize);
  }
  return true;
}

bool CXhrFile::IsInternet()
{
  std::string body;
  if (Get("https://www.msftconnecttest.com/connecttest.txt", body))
    return true;
  return Get("https://www.w3.org/", body);
}

void CXhrFile::SetRequestHeader(const std::string& header, const std::string& value)
{
  m_requestHeaders[header] = value;
}

void CXhrFile::SetRequestHeader(const std::string& header, long value)
{
  m_requestHeaders[header] = std::to_string(value);
}

std::string CXhrFile::GetRedirectURL() const
{
  const std::string location = m_httpHeader.GetValue("location");
  return location.empty() ? m_url : location;
}

// The url is driven directly rather than through CFile, so stored credentials
// (passwords.xml) have to be applied here or an authenticated source is probed
// anonymously and answers 401.
bool CXhrFile::GetHttpHeader(const CURL& url, CHttpHeader& headers)
{
  CXhrFile probe;
  if (!probe.Head(URIUtils::AddCredentials(url)))
    return false;
  headers = probe.GetHttpHeader();
  return true;
}

bool CXhrFile::GetMimeType(const CURL& url, std::string& content, const std::string& useragent)
{
  CXhrFile probe;
  probe.SetUserAgent(useragent);
  if (!probe.Head(URIUtils::AddCredentials(url)))
    return false;
  content = probe.GetHttpHeader().GetMimeType();
  return !content.empty();
}

bool CXhrFile::GetContentType(const CURL& url, std::string& content, const std::string& useragent)
{
  CXhrFile probe;
  probe.SetUserAgent(useragent);
  if (!probe.Head(URIUtils::AddCredentials(url)))
    return false;
  content = probe.GetHttpHeader().GetValue("content-type");
  return !content.empty();
}
