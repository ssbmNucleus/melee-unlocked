// The one HTTP client of the host: every request this program makes (the SSBM Nucleus client and the
// Mods menu, lobbies, opponents' skins, optional downloads, one-click looks and the patch tool,
// Slippi's services) goes through here, with one set of rules:
//
// - HTTPS only. Plain HTTP only where the caller allows it, and only to http://127.0.0.1:<port>
//   (tests and the stand-in site); never from HTTPS down to HTTP.
// - Redirects followed here, at most `max_redirects`, each hop's scheme and host checked
//   (`host_allowed`) before it is asked.
// - Time limits on resolving, connecting, sending and each wait for data.
// - A hard cap on the body (`max_bytes`, required): a Content-Length past it is refused before a byte
//   is read, and the body is cut off as soon as it runs past it.
// - An optional SHA-256 of the body, checked before the body is handed back (a file whose hash
//   differs is deleted).
// - The User-Agent from product_identity.h.
// - The outcome as a small enum plus plain text, and one log line per attempt.
//
// Blocking: worker threads only. Windows uses WinHTTP (one session per kind of client, shared, so
// connections are used again); elsewhere libcurl.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace host::http {

enum class Error {
  None = 0,
  BadAddress,     // not an address this request may ask (scheme, host, form)
  Redirect,       // past the redirect limit, or to a scheme or host this request may not ask
  Connect,        // not resolved, refused, or reset before an answer
  Tls,            // no secure channel
  Timeout,        // no answer or no data in the time allowed
  Dropped,        // the answer stopped before its end
  Status,         // the answer's status is not 2xx (nor a 304 to If-None-Match)
  TooLarge,       // the body is past max_bytes
  HashMismatch,   // the body is not the file expected_sha256 names
  Cancelled,      // `cancel` was set, or `progress` said stop
  Held,           // `gate` held it back; nothing was sent
  Write,          // the body could not be written to its file
};
// A short name for logs ("timeout", "too-large", ...).
const char* error_name(Error e);

// The User-Agent (product_identity.h): this program; the same after a browser-like prefix (pages that
// serve browsers only, the one-click looks' Mediafire pages); this program speaking Slippi's protocol.
enum class Agent { Product, Browser, Slippi };

struct Request {
  std::string method = "GET";
  std::string url;
  std::vector<std::string> headers;   // "Name: value", each
  std::string body;                   // sent as is (POST, PUT)
  uint64_t max_bytes = 0;             // required: the largest body taken (0 refuses the request)
  int resolve_ms = 6000, connect_ms = 6000, send_ms = 10000, receive_ms = 20000;
  int max_redirects = 5;
  bool allow_loopback_http = false;   // http://127.0.0.1:<port> may be asked
  // Each hop's host (lower case) before it is asked; unset: any host.
  std::function<bool(const std::string& host)> host_allowed;
  std::string expected_sha256;        // 64 lower-case hex digits, or empty
  Agent agent = Agent::Product;
  bool system_proxy = false;          // Windows' proxy settings and auto-configuration (WinHTTP's default ignores them)
  bool keep_error_body = false;       // read the body of a non-2xx answer too (up to max_bytes)
  std::string if_none_match;          // sent as If-None-Match; a 304 is then not_modified, not an error
  // Tried again after 1.5 s when the attempt could not connect, timed out, dropped, or was answered
  // 502, 503, 504 or 520-524.
  int attempts = 1;
  const std::atomic<bool>* cancel = nullptr;
  // (bytes so far, Content-Length or 0) about four times a second while the body comes; false stops it.
  std::function<bool(uint64_t got, uint64_t total)> progress;
  // Asked before each attempt with the host and path; false holds it back (`why`, `retry_after` s).
  std::function<bool(const std::string& host, const std::string& path, std::string* why, int* retry_after)> gate;
  // The log line: "<log_label>: <method> <address> -> <outcome> in <s>". `log_address` false: the
  // address is left out (it names an account, or carries a signature). `log` false: no line at all
  // (the caller logs the outcome itself, as the lobbies do).
  std::string log_label = "net";
  bool log_address = true;
  bool log = true;
};

struct Response {
  Error error = Error::None;
  std::string message;                // plain text, for a log
  int status = 0;                     // the last answer's status, 0 when none came
  unsigned long system_error = 0;     // WinHTTP's (or curl's) error code, when that is why
  std::string body;                   // fetch: the body (2xx, or any status with keep_error_body)
  std::string content_type, etag;
  std::string final_host;             // where the request ended, after its redirects
  int retry_after = 0;                // seconds, from a Retry-After of seconds (or `gate`)
  uint64_t bytes = 0;                 // body bytes received on the last attempt
  int redirects = 0, attempts = 0, ms = 0;
  bool not_modified = false;          // a 304 to if_none_match
  bool ok() const { return error == Error::None; }
  bool timed_out() const { return error == Error::Timeout; }
};

// The body into memory (Response::body).
Response fetch(const Request& request);
// The body into `file` (made or emptied), deleted again when anything fails (the hash too).
Response fetch_to_file(const Request& request, const std::filesystem::path& file);

// The SHA-256 of bytes, as 64 lower-case hex digits.
std::string sha256_hex(const void* data, size_t size);

}  // namespace host::http
