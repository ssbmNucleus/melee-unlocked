// The one HTTP client of the host (http_client.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "http_client.h"
#include "host.h"
#include "product_identity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace host::http {
namespace {
using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

const char* agent_text(Agent agent) {
  switch (agent) {
  case Agent::Browser: return PRODUCT_BROWSER_USER_AGENT(MELEE_PORT_VERSION);
  case Agent::Slippi: return PRODUCT_SLIPPI_USER_AGENT(MELEE_PORT_VERSION);
  default: return PRODUCT_USER_AGENT(MELEE_PORT_VERSION);
  }
}

// SHA-256 over data that arrives in pieces.
class Sha256 {
 public:
  Sha256() {
    ok_ = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
          BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) >= 0;
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void add(const void* data, size_t size) {
    const char* p = (const char*)data;
    while (ok_ && size) {
      const ULONG part = (ULONG)std::min<size_t>(size, 1u << 30);
      ok_ = BCryptHashData(hash_, (PUCHAR)p, part, 0) >= 0;
      p += part;
      size -= part;
    }
  }
  std::string hex() {
    unsigned char digest[32]{};
    if (!ok_ || BCryptFinishHash(hash_, digest, sizeof digest, 0) < 0) return {};
    ok_ = false;
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : digest) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  bool ok_ = false;
};

bool is_sha256(const std::string& s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// ---- addresses ----

struct Url {
  bool https = true;
  std::string host;     // lower case
  int port = 443;
  std::string target;   // path and query, from '/'
};

std::string origin(const Url& u) {
  const bool usual = u.port == (u.https ? 443 : 80);
  return std::string(u.https ? "https://" : "http://") + u.host + (usual ? "" : ":" + std::to_string(u.port));
}

// scheme://host[:port][/path][?query][#fragment], http or https, a host of letters, digits, dots and
// hyphens (no user name, no IPv6 literal). Spaces and bytes past ASCII in the path are escaped as
// browsers do; control characters refuse the address.
bool parse_url(const std::string& s, Url* u) {
  if (s.size() > 8192) return false;
  for (const unsigned char c : s)
    if (c < 0x20 || c == 0x7F) return false;
  const size_t scheme_end = s.find("://");
  if (scheme_end == std::string::npos) return false;
  const std::string scheme = lower(s.substr(0, scheme_end));
  if (scheme == "https") { u->https = true; u->port = 443; }
  else if (scheme == "http") { u->https = false; u->port = 80; }
  else return false;
  const size_t host_start = scheme_end + 3;
  const size_t host_end = s.find_first_of("/?#", host_start);
  const std::string authority = s.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  std::string host = authority;
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    const std::string port = authority.substr(colon + 1);
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    const int p = std::atoi(port.c_str());
    if (p < 1 || p > 65535) return false;
    u->port = p;
    host = authority.substr(0, colon);
  }
  host = lower(host);
  if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' || host.front() == '-') return false;
  for (const char c : host)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  u->host = host;
  std::string rest = host_end == std::string::npos ? std::string() : s.substr(host_end);
  const size_t hash = rest.find('#');
  if (hash != std::string::npos) rest.resize(hash);
  if (rest.empty() || rest[0] == '?') rest = "/" + rest;
  static const char* hex = "0123456789ABCDEF";
  u->target.clear();
  for (const unsigned char c : rest) {
    if (c == ' ' || c >= 0x80) { u->target += '%'; u->target += hex[c >> 4]; u->target += hex[c & 15]; }
    else u->target += (char)c;
  }
  return true;
}

std::string path_of(const std::string& target) { return target.substr(0, target.find('?')); }

// A Location, against the address that answered with it.
std::string resolve(const Url& base, const std::string& location) {
  const size_t colon = location.find("://");
  if (colon != std::string::npos && colon < location.find_first_of("/?#")) return location;
  if (location.rfind("//", 0) == 0) return std::string(base.https ? "https:" : "http:") + location;
  if (!location.empty() && location[0] == '/') return origin(base) + location;
  if (!location.empty() && location[0] == '?') return origin(base) + path_of(base.target) + location;
  const std::string path = path_of(base.target);
  return origin(base) + path.substr(0, path.rfind('/') + 1) + location;
}

// Whether `u` may be asked: HTTPS (plain HTTP only to 127.0.0.1 where the caller allows it, and never
// after HTTPS), and the caller's host rule.
bool may_ask(const Request& req, const Url& u, const Url* from, std::string* why) {
  if (!u.https) {
    if (!req.allow_loopback_http || u.host != "127.0.0.1") { *why = "plain HTTP to " + u.host + " is not allowed"; return false; }
    if (from && from->https) { *why = "a redirect from HTTPS down to HTTP"; return false; }
  }
  if (req.host_allowed && !req.host_allowed(u.host)) { *why = u.host + " is not a host this request may ask"; return false; }
  return true;
}

// The address as a log shows it: host and path (the query kept), cut short.
std::string log_address(const std::string& url) {
  std::string s = url;
  const size_t scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  if (s.size() > 180) s = s.substr(0, 177) + "...";
  return s;
}

int retry_seconds(const std::string& value) {
  if (value.empty() || value.size() > 6) return 0;
  int seconds = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return 0;
    seconds = seconds * 10 + (c - '0');
  }
  return seconds;
}

// ---- one request on the wire ----

struct Head {
  int status = 0;
  std::string location, retry_after, etag, content_type;
  int64_t length = -1;   // Content-Length, -1 none
};
struct Exchange {
  Error error = Error::None;
  unsigned long code = 0;   // WinHTTP's or curl's
};
using HeadFn = std::function<bool(const Head&)>;              // true: read the body
using DataFn = std::function<bool(const char*, size_t)>;      // false: stop (the caller noted why)

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n > 0 ? n : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

// One WinHTTP session per kind of client for the whole run: its connections to a host stay open and
// are used again (the next picture, the next page) instead of a new connection and TLS handshake each
// time. Each request sets its own time limits.
HINTERNET session_for(Agent agent, bool system_proxy) {
  static std::mutex mutex;
  static HINTERNET sessions[3][2] = {};
  std::lock_guard<std::mutex> lock(mutex);
  HINTERNET& s = sessions[(int)agent][system_proxy ? 1 : 0];
  if (s) return s;
  const std::wstring name = widen(agent_text(agent));
#ifdef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
  // Windows' own proxy and auto-configuration, as browsers and the Slippi Launcher use them: WinHTTP's
  // default ignores both, and behind such a network every Slippi request timed out (error 12002).
  // Windows before 8.1 lacks the mode: the default there.
  if (system_proxy) s = WinHttpOpen(name.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
  if (!s) s = WinHttpOpen(name.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (s) {
    // TLS 1.2 and 1.3 asked for by name: on some Windows installs WinHTTP's default left them out,
    // and Slippi's servers take nothing older, so every request failed there.
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#else
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#endif
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // an older WinHTTP without TLS 1.3
      WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
  }
  return s;
}

std::string header(HINTERNET req, DWORD which) {
  DWORD size = 0;
  WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 16384) return {};
  std::wstring w(size / sizeof(wchar_t) + 1, L'\0');
  if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, w.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
  w.resize(size / sizeof(wchar_t));
  return narrow(w);
}

Error classify(unsigned long code, bool answered) {
  switch (code) {
  case ERROR_WINHTTP_TIMEOUT: return Error::Timeout;
  case ERROR_WINHTTP_OPERATION_CANCELLED: return Error::Cancelled;
  case ERROR_WINHTTP_SECURE_FAILURE: case ERROR_WINHTTP_SECURE_CHANNEL_ERROR: case ERROR_WINHTTP_SECURE_INVALID_CA:
  case ERROR_WINHTTP_SECURE_CERT_CN_INVALID: case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: case ERROR_WINHTTP_SECURE_INVALID_CERT:
  case ERROR_WINHTTP_SECURE_CERT_REVOKED: case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
    return answered ? Error::Dropped : Error::Tls;
  default: return answered ? Error::Dropped : Error::Connect;
  }
}

std::string system_error_text(unsigned long code) {
  const char* name = "error";
  switch (code) {
  case ERROR_WINHTTP_TIMEOUT: name = "timed out"; break;
  case ERROR_WINHTTP_NAME_NOT_RESOLVED: name = "name not resolved"; break;
  case ERROR_WINHTTP_CANNOT_CONNECT: name = "cannot connect"; break;
  case ERROR_WINHTTP_CONNECTION_ERROR: name = "connection reset"; break;
  case ERROR_WINHTTP_SECURE_FAILURE: name = "secure channel failed"; break;
  case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: name = "invalid server response"; break;
  case ERROR_WINHTTP_OPERATION_CANCELLED: name = "cancelled"; break;
  default: break;
  }
  return "WinHTTP error " + std::to_string(code) + " (" + name + ")";
}

Exchange exchange(const Request& req, const std::string& method, const std::string& body, const Url& u, const HeadFn& on_head,
                  const DataFn& on_data) {
  Exchange x;
  const HINTERNET session = session_for(req.agent, req.system_proxy);
  if (!session) { x.code = GetLastError(); x.error = Error::Connect; return x; }
  const HINTERNET conn = WinHttpConnect(session, widen(u.host).c_str(), (INTERNET_PORT)u.port, 0);
  const HINTERNET h = conn ? WinHttpOpenRequest(conn, widen(method).c_str(), widen(u.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                           : nullptr;
  if (!h) {
    x.code = GetLastError();
    x.error = Error::Connect;
    if (conn) WinHttpCloseHandle(conn);
    return x;
  }
  // (WinHTTP keeps these loosely: a 1.5 s limit has been seen to fire after about 4 s.)
  WinHttpSetTimeouts(h, req.resolve_ms, req.connect_ms, req.send_ms, req.receive_ms);
  // The wait for the answer's headers has a limit of its own (90 s unless set).
  DWORD response_ms = (DWORD)req.receive_ms;
  WinHttpSetOption(h, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &response_ms, sizeof response_ms);
  // Redirects are followed by the caller (each hop checked), and no cookies are kept.
  DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
  WinHttpSetOption(h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
  std::wstring headers;
  for (const std::string& line : req.headers) headers += widen(line) + L"\r\n";
  if (!req.if_none_match.empty()) headers += L"If-None-Match: " + widen(req.if_none_match) + L"\r\n";
  const bool answered =
      WinHttpSendRequest(h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L,
                         body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
      WinHttpReceiveResponse(h, nullptr);
  if (!answered) {
    x.code = GetLastError();
    x.error = classify(x.code, false);
  } else {
    Head head;
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    head.status = (int)code;
    head.location = header(h, WINHTTP_QUERY_LOCATION);
    head.retry_after = header(h, WINHTTP_QUERY_RETRY_AFTER);
    head.etag = header(h, WINHTTP_QUERY_ETAG);
    head.content_type = header(h, WINHTTP_QUERY_CONTENT_TYPE);
    const std::string length = header(h, WINHTTP_QUERY_CONTENT_LENGTH);
    if (!length.empty() && length.size() <= 18 && std::all_of(length.begin(), length.end(), [](char c) { return c >= '0' && c <= '9'; }))
      head.length = std::stoll(length);
    if (on_head(head)) {
      std::vector<char> buffer(256 * 1024);
      uint64_t total = 0;
      for (;;) {
        // Never more than one byte past the cap is read off the wire.
        const DWORD want = (DWORD)std::min<uint64_t>(buffer.size(), req.max_bytes - std::min(total, req.max_bytes) + 1);
        DWORD got = 0;
        if (!WinHttpReadData(h, buffer.data(), want, &got)) {
          x.code = GetLastError();
          x.error = classify(x.code, true);
          break;
        }
        total += got;
        if (!got || !on_data(buffer.data(), got)) break;
      }
    }
  }
  WinHttpCloseHandle(h);
  WinHttpCloseHandle(conn);
  return x;
}
#else
// libcurl, one easy handle a request; redirects left to the caller as on Windows.
struct CurlCall {
  const HeadFn* on_head;
  const DataFn* on_data;
  const std::atomic<bool>* cancel;
  Head head;
  bool head_done = false, reading = false, stopped = false;
};

bool curl_start_body(CurlCall* c) {
  if (!c->head_done) {
    c->head_done = true;
    c->reading = (*c->on_head)(c->head);
  }
  return c->reading;
}

size_t curl_header(char* p, size_t size, size_t n, void* user) {
  auto* c = static_cast<CurlCall*>(user);
  std::string line(p, size * n);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.rfind("HTTP/", 0) == 0) {
    c->head = Head{};
    const size_t space = line.find(' ');
    c->head.status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
  } else {
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      const std::string name = lower(line.substr(0, colon));
      size_t start = colon + 1;
      while (start < line.size() && line[start] == ' ') ++start;
      const std::string value = line.substr(start);
      if (name == "location") c->head.location = value;
      else if (name == "retry-after") c->head.retry_after = value;
      else if (name == "etag") c->head.etag = value;
      else if (name == "content-type") c->head.content_type = value;
      else if (name == "content-length" && !value.empty() && value.size() <= 18 &&
               std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        c->head.length = std::stoll(value);
    }
  }
  return size * n;
}

size_t curl_write(char* p, size_t size, size_t n, void* user) {
  auto* c = static_cast<CurlCall*>(user);
  if (!curl_start_body(c) || !(*c->on_data)(p, size * n)) { c->stopped = true; return 0; }
  return size * n;
}

int curl_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* c = static_cast<CurlCall*>(user);
  return c->cancel && c->cancel->load() ? 1 : 0;
}

std::string system_error_text(unsigned long code) {
  return "curl error " + std::to_string(code) + " (" + curl_easy_strerror((CURLcode)code) + ")";
}

Exchange exchange(const Request& req, const std::string& method, const std::string& body, const Url& u, const HeadFn& on_head,
                  const DataFn& on_data) {
  static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  Exchange x;
  CURL* curl = ready ? curl_easy_init() : nullptr;
  if (!curl) { x.error = Error::Connect; return x; }
  CurlCall call{&on_head, &on_data, req.cancel};
  const std::string url = origin(u) + u.target;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, agent_text(req.agent));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(req.resolve_ms + req.connect_ms));
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, std::max(1L, (long)req.receive_ms / 1000));
  if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
  }
  struct curl_slist* list = curl_slist_append(nullptr, "Expect:");
  for (const std::string& line : req.headers) list = curl_slist_append(list, line.c_str());
  if (!req.if_none_match.empty()) list = curl_slist_append(list, ("If-None-Match: " + req.if_none_match).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &call);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &call);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &call);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  const CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && call.stopped)) {
    if (rc == CURLE_OK) curl_start_body(&call);   // an answer with no body
  } else {
    x.code = (unsigned long)rc;
    const bool answered = call.head.status != 0;
    if (answered && !call.head_done) curl_start_body(&call);   // the status, for the caller
    x.error = rc == CURLE_OPERATION_TIMEDOUT ? Error::Timeout
            : rc == CURLE_ABORTED_BY_CALLBACK ? Error::Cancelled
            : (rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CERTPROBLEM ||
               rc == CURLE_SSL_CIPHER || rc == CURLE_SSL_CACERT_BADFILE) && !answered ? Error::Tls
            : answered ? Error::Dropped : Error::Connect;
  }
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return x;
}
#endif

// ---- a whole request: its redirects, its body, its checks ----

bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

struct Target {
  const std::filesystem::path* file = nullptr;   // fetch_to_file
  std::ofstream out;
};

Response attempt(const Request& req, Target& target) {
  Response r;
  const auto fail = [&](Error e, const std::string& message) { r.error = e; r.message = message; return r; };
  Url u;
  std::string why;
  if (!parse_url(req.url, &u)) return fail(Error::BadAddress, "not an address this program reads");
  r.final_host = u.host;
  if (!may_ask(req, u, nullptr, &why)) return fail(Error::BadAddress, why);
  if (req.cancel && req.cancel->load()) return fail(Error::Cancelled, "cancelled");
  if (req.gate) {
    int wait = 0;
    if (!req.gate(u.host, path_of(u.target), &why, &wait)) { r.retry_after = wait; return fail(Error::Held, why); }
  }
  std::string method = req.method, body = req.body;
  for (int hop = 0;; ++hop) {
    r.final_host = u.host;
    r.status = 0;
    r.bytes = 0;
    r.body.clear();
    Head head;
    bool redirect = false, to_file = false;
    Error stop = Error::None;
    std::string stop_message;
    std::unique_ptr<Sha256> sha;
    if (!req.expected_sha256.empty()) sha = std::make_unique<Sha256>();
    Clock::time_point shown{};
    const auto on_head = [&](const Head& h) {
      head = h;
      r.status = h.status;
      r.content_type = h.content_type;
      r.etag = h.etag;
      r.retry_after = retry_seconds(h.retry_after);
      if (is_redirect(h.status) && !h.location.empty()) { redirect = true; return false; }
      if (h.status == 304 && !req.if_none_match.empty()) { r.not_modified = true; return false; }
      const bool success = h.status >= 200 && h.status < 300;
      if (!success && !req.keep_error_body) return false;
      if (h.length >= 0 && (uint64_t)h.length > req.max_bytes) {
        stop = Error::TooLarge;
        stop_message = "announces " + std::to_string(h.length) + " bytes, over " + std::to_string(req.max_bytes);
        return false;
      }
      if (success && target.file) {
        target.out.close();
        target.out.clear();
        target.out.open(*target.file, std::ios::binary | std::ios::trunc);
        if (!target.out) { stop = Error::Write; stop_message = "cannot write " + target.file->u8string(); return false; }
        to_file = true;
      }
      return true;
    };
    const auto on_data = [&](const char* p, size_t n) {
      if (req.cancel && req.cancel->load()) { stop = Error::Cancelled; stop_message = "cancelled"; return false; }
      r.bytes += n;
      if (r.bytes > req.max_bytes) { stop = Error::TooLarge; stop_message = "over " + std::to_string(req.max_bytes) + " bytes"; return false; }
      if (sha) sha->add(p, n);
      if (to_file) {
        target.out.write(p, (std::streamsize)n);
        if (!target.out) { stop = Error::Write; stop_message = "cannot write " + target.file->u8string(); return false; }
      } else {
        r.body.append(p, n);
      }
      if (req.progress && Clock::now() - shown >= std::chrono::milliseconds(250)) {
        shown = Clock::now();
        if (!req.progress(r.bytes, head.length > 0 ? (uint64_t)head.length : 0)) { stop = Error::Cancelled; stop_message = "stopped"; return false; }
      }
      return true;
    };
    const Exchange x = exchange(req, method, body, u, on_head, on_data);
    if (to_file) target.out.close();
    if (stop != Error::None) return fail(stop, stop_message);
    if (x.error != Error::None) {
      r.system_error = x.code;
      return fail(x.error, system_error_text(x.code));
    }
    if (redirect) {
      if (hop >= req.max_redirects) return fail(Error::Redirect, "more than " + std::to_string(req.max_redirects) + " redirects");
      Url next;
      if (!parse_url(resolve(u, head.location), &next)) return fail(Error::Redirect, "a redirect to an address this program does not read");
      if (!may_ask(req, next, &u, &why)) { r.final_host = next.host; return fail(Error::Redirect, "redirect refused: " + why); }
      if (head.status == 303 || ((head.status == 301 || head.status == 302) && method == "POST")) { method = "GET"; body.clear(); }
      u = next;
      ++r.redirects;
      continue;
    }
    if (r.not_modified) return r;
    if (head.status < 200 || head.status >= 300) return fail(Error::Status, "answered " + std::to_string(head.status));
    if (head.length >= 0 && r.bytes != (uint64_t)head.length)
      return fail(Error::Dropped, "stopped after " + std::to_string(r.bytes) + " of " + std::to_string(head.length) + " bytes");
    if (sha) {
      const std::string got = sha->hex();
      if (got != req.expected_sha256) { r.body.clear(); return fail(Error::HashMismatch, "SHA-256 " + got.substr(0, 12) + "..., not the expected file"); }
    }
    if (req.progress) req.progress(r.bytes, r.bytes);
    return r;
  }
}

bool retryable(const Response& r) {
  switch (r.error) {
  case Error::Connect: case Error::Tls: case Error::Timeout: case Error::Dropped: return true;
  case Error::Status: return r.status == 502 || r.status == 503 || r.status == 504 || (r.status >= 520 && r.status <= 524);
  default: return false;
  }
}

void log_attempt(const Request& req, const Response& r, double seconds, bool again) {
  if (!req.log) return;
  Url u;
  const std::string address = req.log_address ? log_address(req.url) : parse_url(req.url, &u) ? u.host : std::string("(address)");
  std::string outcome;
  if (r.ok()) {
    outcome = r.not_modified ? "304 (not modified)" : std::to_string(r.status) + ", " + std::to_string(r.bytes) + " bytes";
    if (parse_url(req.url, &u) && u.host != r.final_host) outcome += " via " + r.final_host;
  } else if (r.error == Error::Held) {
    outcome = "not sent: " + r.message;
  } else if (r.status != 0) {
    outcome = std::to_string(r.status);
    if (r.retry_after) outcome += " (Retry-After " + std::to_string(r.retry_after) + " s)";
    if (r.error != Error::Status) outcome += ", " + std::string(error_name(r.error)) + ": " + r.message;
  } else if (r.system_error) {
    outcome = r.message;
  } else {
    outcome = "refused: " + r.message;
  }
  host::log("%s: %s %s -> %s in %.2f s%s", req.log_label.c_str(), req.method.c_str(), address.c_str(), outcome.c_str(), seconds,
            again ? "; trying once more" : "");
}

Response run(const Request& req, Target& target) {
  Response r;
  if (!req.max_bytes || (!req.expected_sha256.empty() && !is_sha256(req.expected_sha256))) {
    r.error = Error::BadAddress;
    r.message = !req.max_bytes ? "no size cap given" : "the expected SHA-256 is not 64 lower-case hex digits";
    log_attempt(req, r, 0, false);
    return r;
  }
  const int attempts = std::max(1, req.attempts);
  for (int n = 1;; ++n) {
    const auto start = Clock::now();
    r = attempt(req, target);
    r.attempts = n;
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.ms = (int)(seconds * 1000);
    const bool again = n < attempts && retryable(r) && !(req.cancel && req.cancel->load());
    log_attempt(req, r, seconds, again);
    if (!again) break;
    for (int waited = 0; waited < 1500 && !(req.cancel && req.cancel->load()); waited += 100)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return r;
}
}  // namespace

const char* error_name(Error e) {
  switch (e) {
  case Error::None: return "ok";
  case Error::BadAddress: return "address";
  case Error::Redirect: return "redirect";
  case Error::Connect: return "connect";
  case Error::Tls: return "tls";
  case Error::Timeout: return "timeout";
  case Error::Dropped: return "dropped";
  case Error::Status: return "http-status";
  case Error::TooLarge: return "too-large";
  case Error::HashMismatch: return "hash";
  case Error::Cancelled: return "cancelled";
  case Error::Held: return "held";
  case Error::Write: return "write";
  }
  return "error";
}

Response fetch(const Request& request) {
  Target target;
  return run(request, target);
}

Response fetch_to_file(const Request& request, const std::filesystem::path& file) {
  Target target;
  target.file = &file;
  Response r = run(request, target);
  target.out.close();
  std::error_code ec;
  if (!r.ok()) std::filesystem::remove(file, ec);
  r.body.clear();
  return r;
}

std::string sha256_hex(const void* data, size_t size) {
  Sha256 sha;
  sha.add(data, size);
  return sha.hex();
}

}  // namespace host::http
