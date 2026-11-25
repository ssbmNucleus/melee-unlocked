// The host's one HTTP client (http_client.h) against a small HTTP server this test runs itself on
// 127.0.0.1 (the first free port of 43800-43899): the size cap (announced and streamed), the redirect
// limit, the SHA-256 check (memory and file), the time limit, the status of an error answer, and the
// addresses it refuses without asking anyone (plain HTTP elsewhere than 127.0.0.1, no size cap).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "http_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace host {
void log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::printf("     log: ");
  std::vprintf(fmt, args);
  std::printf("\n");
  va_end(args);
}
void request_exit(int) {}
}  // namespace host

namespace {
namespace http = host::http;

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// ---- the server: one thread per connection, one request per connection ----
SOCKET g_listen = INVALID_SOCKET;
int g_port = 0;
std::atomic<bool> g_stop{false};
std::mutex g_mutex;
std::vector<std::thread> g_workers;
std::atomic<int> g_requests{0};

void send_all(SOCKET s, const std::string& data) {
  size_t at = 0;
  while (at < data.size()) {
    const int n = send(s, data.data() + at, (int)std::min<size_t>(data.size() - at, 1 << 16), 0);
    if (n <= 0) return;
    at += (size_t)n;
  }
}
std::string answer(int status, const std::string& body, const std::string& more = {}) {
  const char* reason = status == 200 ? "OK" : status == 302 ? "Found" : status == 404 ? "Not Found" : "Status";
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n" + more + "\r\n" + body;
}

void serve(SOCKET s) {
  std::string request;
  char buf[4096];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
    const int n = recv(s, buf, sizeof buf, 0);
    if (n <= 0) break;
    request.append(buf, (size_t)n);
  }
  ++g_requests;
  const size_t sp = request.find(' '), sp2 = sp == std::string::npos ? sp : request.find(' ', sp + 1);
  const std::string path = sp2 == std::string::npos ? "" : request.substr(sp + 1, sp2 - sp - 1);
  if (path == "/ok") {
    send_all(s, answer(200, "hello"));
  } else if (path == "/announced-big") {
    // A Content-Length past the cap: refused before the body is read.
    send_all(s, "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n" + std::string(100000, 'a'));
  } else if (path == "/streamed-big") {
    // No Content-Length: the body runs until the connection closes, past the cap.
    send_all(s, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
    for (int i = 0; i < 100 && !g_stop; ++i) send_all(s, std::string(1000, 'b'));
  } else if (path.rfind("/hop/", 0) == 0) {
    const int left = std::atoi(path.c_str() + 5);
    if (left <= 0) send_all(s, answer(200, "arrived"));
    else send_all(s, answer(302, "", "Location: /hop/" + std::to_string(left - 1) + "\r\n"));
  } else if (path == "/away") {
    // A redirect to plain HTTP somewhere other than 127.0.0.1: never asked.
    send_all(s, answer(302, "", "Location: http://127.0.0.2:" + std::to_string(g_port) + "/ok\r\n"));
  } else if (path == "/slow") {
    // Nothing for ten seconds (or until the test ends).
    for (int i = 0; i < 100 && !g_stop; ++i) Sleep(100);
    send_all(s, answer(200, "late"));
  } else if (path == "/stall") {
    // Half the body, then nothing for ten seconds (or until the test ends).
    send_all(s, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\nhalf.");
    for (int i = 0; i < 100 && !g_stop; ++i) Sleep(100);
  } else if (path == "/short") {
    // Fewer bytes than announced, then the connection closes.
    send_all(s, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\nonly this");
  } else {
    send_all(s, answer(404, "{\"error\":\"no such thing\"}", "Content-Type: application/json\r\n"));
  }
  shutdown(s, SD_SEND);
  closesocket(s);
}

bool start_server() {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  for (int port = 43800; port <= 43899; ++port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof exclusive);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof a) == 0 && listen(s, 16) == 0) {
      g_listen = s;
      g_port = port;
      break;
    }
    closesocket(s);
  }
  if (g_listen == INVALID_SOCKET) return false;
  std::thread([] {
    for (;;) {
      SOCKET c = accept(g_listen, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;   // closed at the end
      std::lock_guard<std::mutex> lock(g_mutex);
      g_workers.emplace_back(serve, c);
    }
  }).detach();
  return true;
}

void stop_server() {
  g_stop = true;
  closesocket(g_listen);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (std::thread& t : g_workers) t.join();
  WSACleanup();
}

std::string at(const std::string& path) { return "http://127.0.0.1:" + std::to_string(g_port) + path; }

http::Request request(const std::string& path) {
  http::Request r;
  r.url = at(path);
  r.allow_loopback_http = true;
  r.max_bytes = 1000;
  r.resolve_ms = r.connect_ms = r.send_ms = r.receive_ms = 2000;
  r.log_label = "test";
  return r;
}
}  // namespace

int main() {
  if (!start_server()) {
    std::printf("FAIL no free port in 43800-43899 for the test server\n");
    return 1;
  }
  std::printf("test server on 127.0.0.1:%d\n", g_port);
  const std::string hello_sha = http::sha256_hex("hello", 5);

  {
    const http::Response r = http::fetch(request("/ok"));
    check(r.ok() && r.status == 200 && r.body == "hello", "a plain answer comes back whole");
  }
  // The size cap: announced, and while the body streams.
  {
    const http::Response r = http::fetch(request("/announced-big"));
    check(r.error == http::Error::TooLarge && r.body.empty() && r.bytes == 0, "a Content-Length past the cap is refused before the body (" + r.message + ")");
  }
  {
    const http::Response r = http::fetch(request("/streamed-big"));
    check(r.error == http::Error::TooLarge && r.bytes == 1001 && r.body.size() <= 1000,
          "a body running past the cap is cut off one byte past it (" + r.message + ", " + std::to_string(r.bytes) + " read)");
  }
  // The redirect limit.
  {
    http::Request q = request("/hop/2");
    q.max_redirects = 3;
    const http::Response r = http::fetch(q);
    check(r.ok() && r.body == "arrived" && r.redirects == 2, "redirects within the limit are followed");
    q.url = at("/hop/5");
    const http::Response over = http::fetch(q);
    check(over.error == http::Error::Redirect && over.redirects == 3, "a fourth redirect past a limit of 3 is refused (" + over.message + ")");
    q.url = at("/hop/1");
    q.max_redirects = 0;
    check(http::fetch(q).error == http::Error::Redirect, "no redirect is followed with a limit of 0");
  }
  {
    const int before = g_requests.load();
    const http::Response r = http::fetch(request("/away"));
    check(r.error == http::Error::Redirect && g_requests.load() == before + 1,
          "a redirect to plain HTTP elsewhere than 127.0.0.1 is refused, not asked (" + r.message + ")");
  }
  // The SHA-256: in memory, and to a file that is deleted again.
  {
    http::Request q = request("/ok");
    q.expected_sha256 = hello_sha;
    check(http::fetch(q).ok(), "a body with the expected SHA-256 comes back");
    q.expected_sha256 = std::string(64, '0');
    const http::Response r = http::fetch(q);
    check(r.error == http::Error::HashMismatch && r.body.empty(), "a body with another SHA-256 is not handed back (" + r.message + ")");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / ("mu-http-test-" + std::to_string(GetCurrentProcessId()) + ".bin");
    const http::Response f = http::fetch_to_file(q, file);
    check(f.error == http::Error::HashMismatch && !std::filesystem::exists(file), "a file with another SHA-256 is deleted");
    q.expected_sha256 = hello_sha;
    const http::Response g = http::fetch_to_file(q, file);
    std::ifstream in(file, std::ios::binary);
    const std::string kept((std::istreambuf_iterator<char>(in)), {});
    in.close();
    check(g.ok() && kept == "hello", "a file with the expected SHA-256 is kept");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    q.expected_sha256 = "not a hash";
    check(http::fetch(q).error == http::Error::BadAddress, "an expected SHA-256 that is not one refuses the request");
  }
  // Time limits: no answer, and an answer that stalls. (WinHTTP keeps a limit loosely: 1 s has been
  // seen to fire after 3 to 4 s; the server waits 10 s.)
  {
    http::Request q = request("/slow");
    q.receive_ms = 1000;
    const auto timed = [&](const char* path, long long* ms) {
      q.url = at(path);
      const auto start = std::chrono::steady_clock::now();
      const http::Response r = http::fetch(q);
      *ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      return r;
    };
    long long ms = 0;
    const http::Response r = timed("/slow", &ms);
    check(r.error == http::Error::Timeout && r.timed_out() && ms < 8000, "no answer in time is a timeout (" + std::to_string(ms) + " ms)");
    const http::Response stalled = timed("/stall", &ms);
    check(stalled.error == http::Error::Timeout && ms < 8000, "a body that stalls is a timeout (" + stalled.message + ", " + std::to_string(ms) + " ms)");
  }
  {
    const http::Response r = http::fetch(request("/short"));
    check(r.error == http::Error::Dropped, "a body shorter than announced is not taken (" + r.message + ")");
  }
  // An error answer: its status, and its body only when asked for.
  {
    http::Request q = request("/missing");
    const http::Response r = http::fetch(q);
    check(r.error == http::Error::Status && r.status == 404 && r.body.empty(), "a 404 is a status error");
    q.keep_error_body = true;
    const http::Response kept = http::fetch(q);
    check(kept.status == 404 && kept.body.find("no such thing") != std::string::npos && kept.content_type.find("application/json") != std::string::npos,
          "an error answer's body is kept when asked for");
  }
  // Progress, and stopping from it.
  {
    http::Request q = request("/ok");
    uint64_t last = 0;
    q.progress = [&](uint64_t got, uint64_t) { last = got; return true; };
    check(http::fetch(q).ok() && last == 5, "progress hears the whole body");
    std::atomic<bool> cancel{true};
    q.cancel = &cancel;
    check(http::fetch(q).error == http::Error::Cancelled, "a cancelled request is not sent");
  }
  // Refused without asking anyone.
  {
    const int before = g_requests.load();
    http::Request q = request("/ok");
    q.allow_loopback_http = false;
    check(http::fetch(q).error == http::Error::BadAddress, "plain HTTP to 127.0.0.1 is refused unless the caller allows it");
    q.allow_loopback_http = true;
    q.url = "http://127.0.0.2:" + std::to_string(g_port) + "/ok";
    check(http::fetch(q).error == http::Error::BadAddress, "plain HTTP to another address is refused");
    q.url = "http://localhost:" + std::to_string(g_port) + "/ok";
    check(http::fetch(q).error == http::Error::BadAddress, "plain HTTP to a name is refused");
    q.url = "ftp://127.0.0.1/ok";
    check(http::fetch(q).error == http::Error::BadAddress, "another scheme is refused");
    q.url = "https://user@example.invalid/";
    check(http::fetch(q).error == http::Error::BadAddress, "an address with credentials is refused");
    q.url = at("/ok");
    q.max_bytes = 0;
    check(http::fetch(q).error == http::Error::BadAddress, "a request without a size cap is refused");
    q.max_bytes = 1000;
    q.host_allowed = [](const std::string& host) { return host == "ssbmnucleus.net"; };
    check(http::fetch(q).error == http::Error::BadAddress, "a host the caller does not allow is refused");
    check(g_requests.load() == before, "none of those reached the server");
  }
  stop_server();
  std::printf("%s\n", failures ? "http client checks FAILED" : "http client checks passed");
  return failures ? 1 : 0;
}
