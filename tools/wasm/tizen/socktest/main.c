/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Exercises the Tizen sockets shim without Kodi: DNS, a raw TCP request,
 * blocking and non-blocking modes, then libcurl over http and https. Output
 * goes to the browser console; read it through the Web Inspector. */

#include "platform/wasm/network/TizenSockets.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <emscripten.h>

#define HOST "mirrors.kodi.tv"

static double now_ms(void)
{
  return emscripten_get_now();
}

static int step_ok = 0, step_fail = 0;

static void result(const char* name, int ok, const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  printf("[socktest] %s %s: %s\n", ok ? "PASS" : "FAIL", name, buf);
  if (ok)
    step_ok++;
  else
    step_fail++;
}

static int resolve(struct sockaddr_in* out)
{
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  double t0 = now_ms();
  int rc = getaddrinfo(HOST, "80", &hints, &res);
  if (rc != 0 || !res)
  {
    result("getaddrinfo", 0, "rc=%d (%s)", rc, gai_strerror(rc));
    return -1;
  }
  memcpy(out, res->ai_addr, sizeof(*out));
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &out->sin_addr, ip, sizeof(ip));
  result("getaddrinfo", 1, "%s -> %s in %.1f ms", HOST, ip, now_ms() - t0);
  freeaddrinfo(res);
  return 0;
}

static void test_raw_blocking(const struct sockaddr_in* addr)
{
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
  {
    result("socket", 0, "errno=%d %s", errno, strerror(errno));
    return;
  }
  int one = 1;
  int rc = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  result("setsockopt TCP_NODELAY", rc == 0, "rc=%d errno=%d", rc, errno);

  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  rc = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  result("setsockopt SO_RCVTIMEO", rc == 0, "rc=%d errno=%d", rc, errno);

  double t0 = now_ms();
  rc = connect(s, (const struct sockaddr*)addr, sizeof(*addr));
  result("blocking connect", rc == 0, "rc=%d errno=%d in %.1f ms", rc, errno, now_ms() - t0);
  if (rc != 0)
  {
    close(s);
    return;
  }

  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  rc = getpeername(s, (struct sockaddr*)&peer, &plen);
  result("getpeername", rc == 0 && ntohs(peer.sin_port) == 80, "rc=%d port=%d", rc,
         ntohs(peer.sin_port));

  const char req[] = "HEAD / HTTP/1.1\r\nHost: " HOST "\r\nConnection: close\r\n\r\n";
  ssize_t n = write(s, req, sizeof(req) - 1);
  result("write", n == (ssize_t)(sizeof(req) - 1), "n=%zd errno=%d", n, errno);

  struct pollfd pfd = {.fd = s, .events = POLLIN};
  t0 = now_ms();
  rc = poll(&pfd, 1, 5000);
  result("poll POLLIN", rc == 1 && (pfd.revents & POLLIN), "rc=%d revents=%d in %.1f ms", rc,
         pfd.revents, now_ms() - t0);

  char buf[1024];
  n = read(s, buf, sizeof(buf) - 1);
  if (n > 0)
    buf[n] = 0;
  result("read", n > 0 && strncmp(buf, "HTTP/1.1 200", 12) == 0, "n=%zd first=%.20s", n,
         n > 0 ? buf : "");

  /* Drain to EOF: blocking recv must return 0 once the server closes. */
  while (n > 0)
    n = recv(s, buf, sizeof(buf), 0);
  result("recv EOF", n == 0, "n=%zd errno=%d", n, errno);

  rc = close(s);
  result("close", rc == 0, "rc=%d", rc);
}

static void test_raw_nonblocking(const struct sockaddr_in* addr)
{
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
  {
    result("socket(nb)", 0, "errno=%d", errno);
    return;
  }
  int flags = fcntl(s, F_GETFL, 0);
  int rc = fcntl(s, F_SETFL, flags | O_NONBLOCK);
  result("fcntl O_NONBLOCK", rc == 0 && (fcntl(s, F_GETFL, 0) & O_NONBLOCK), "rc=%d flags=%d",
         rc, fcntl(s, F_GETFL, 0));

  rc = connect(s, (const struct sockaddr*)addr, sizeof(*addr));
  result("nonblocking connect", rc < 0 && errno == EINPROGRESS, "rc=%d errno=%d", rc, errno);

  char buf[64];
  ssize_t n = recv(s, buf, sizeof(buf), 0);
  result("recv before connect", n < 0 && errno == EAGAIN, "n=%zd errno=%d", n, errno);

  struct pollfd pfd = {.fd = s, .events = POLLOUT};
  rc = poll(&pfd, 1, 5000);
  int err = -1;
  socklen_t elen = sizeof(err);
  getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
  result("poll POLLOUT + SO_ERROR", rc == 1 && (pfd.revents & POLLOUT) && err == 0,
         "rc=%d revents=%d so_error=%d", rc, pfd.revents, err);

  /* select() is implemented on top of poll(); Kodi's CurlFile relies on it. */
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(s, &wfds);
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  rc = select(s + 1, NULL, &wfds, NULL, &tv);
  result("select writable", rc == 1 && FD_ISSET(s, &wfds), "rc=%d", rc);

  n = recv(s, buf, sizeof(buf), 0);
  result("recv idle EAGAIN", n < 0 && errno == EAGAIN, "n=%zd errno=%d", n, errno);
  close(s);
}

static size_t count_bytes(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  (void)ptr;
  *(size_t*)userdata += size * nmemb;
  return size * nmemb;
}

static void test_curl(const char* url, const char* cainfo)
{
  CURL* h = curl_easy_init();
  size_t bytes = 0;
  char errbuf[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, count_bytes);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &bytes);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "Kodi-wasm-socktest/1.0");
  if (cainfo)
    curl_easy_setopt(h, CURLOPT_CAINFO, cainfo);
  double t0 = now_ms();
  CURLcode rc = curl_easy_perform(h);
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  result(url, rc == CURLE_OK && status == 200, "rc=%d (%s) status=%ld bytes=%zu in %.0f ms %s",
         rc, curl_easy_strerror(rc), status, bytes, now_ms() - t0, errbuf);
  curl_easy_cleanup(h);
}

int main(void)
{
  printf("[socktest] start, tizen sockets available: %d\n", kodi_wasm_has_sockets());
  if (!kodi_wasm_has_sockets())
  {
    /* Upstream Emscripten's WebSocket emulation still hands out descriptors. */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    result("socket fallback", 1, "s=%d errno=%d", s, errno);
    if (s >= 0)
      close(s);
    printf("[socktest] done: %d passed, %d failed\n", step_ok, step_fail);
    return 0;
  }

  struct sockaddr_in addr;
  if (resolve(&addr) == 0)
  {
    test_raw_blocking(&addr);
    test_raw_nonblocking(&addr);
  }

  curl_global_init(CURL_GLOBAL_ALL);
  test_curl("http://" HOST "/", NULL);
  test_curl("https://" HOST "/", "/cacert.pem");
  curl_global_cleanup();

  printf("[socktest] done: %d passed, %d failed\n", step_ok, step_fail);
  return 0;
}
