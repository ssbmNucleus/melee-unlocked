// host::skins::fetch_skin (host/skin_fetch.h) against a stand-in for the SSBM Nucleus API that this
// test serves itself on the loopback address (127.0.0.1, ports 42050-42069), through the mod
// browser's real WinHTTP GET. Cases: a good fetch through the post's download_url and its redirect,
// the cache (hit, and a damaged cached file fetched again), a hash that differs, a file past the 8 MB
// cap and one exactly at it, a missing post and a missing file, the site's export (the DAT of the
// hash taken out of the zip; none of that hash; an export address of another card), a redirect off
// the allowed host, the server down, and bad ids or hashes (refused without a request). Both routes
// are asked for with the view marker (an opponent's skin shown, not installed): the stand-in's download
// and export routes answer only the marked address. download_view_address's forms, without a request.
// With --view (ctest port_skin_fetch_view, a process of its own, so its requests to the API stay
// within the mod browser's 30 a minute): the API's own download route asked for with &view=1; a site
// that answers 400 to the marker asked once more without it; no second retry, and none after a 404;
// an install asks for the bare route and is refused an address with the marker.
// The skin's own stock icon (fetch_stock_icon, the card's stock_url on the same post): a good icon
// (kept beside the skin, then from the cache, and made the HUD's 24x24 texture), a larger one, a card
// with none, a WebP address, an icon another file of the post shares (and Nana's, which is not a
// sibling), and refused pictures (past 4 MB, more than 1024 pixels a side, not a PNG, a cut PNG), a
// damaged cached icon, and the server down (nothing kept).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "skin_fetch.h"
#include "mod_browser.h"
#include "hackpack_ai.h"
#include "roster.h"
#include "test_env.h"
#include "public_build.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace host {
void log(const char*, ...) {}
bool disc_find_file(const std::string&, uint32_t*, uint32_t*) { return false; }
bool disc_read(uint32_t, void*, uint32_t) { return false; }
}

namespace {
namespace fs = std::filesystem;
int g_failures = 0;
#define CHECK(cond, what)                                                         \
  do {                                                                            \
    if (!(cond)) { ++g_failures; std::printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } \
    else std::printf("ok   %s\n", what);                                          \
  } while (0)

std::string sha(const std::vector<uint8_t>& b) { return host::hackpack_ai::rules::sha256_hex(b.data(), b.size()); }
std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> v(n);
  for (auto& c : v) c = (uint8_t)rng();
  return v;
}

struct Route { int code = 200; std::string location; std::vector<uint8_t> body; };

// A tiny HTTP/1.1 server: one request per connection, routes by the request target.
class StandIn {
 public:
  bool start(const char* address = "127.0.0.1", int first = 42050, int last = 42070) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    for (int p = first; p < last; ++p) {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in a{};
      a.sin_family = AF_INET; a.sin_port = htons((u_short)p);
      inet_pton(AF_INET, address, &a.sin_addr);
      if (bind(s, (sockaddr*)&a, sizeof a) == 0 && listen(s, 8) == 0) { listen_ = s; port_ = p; break; }
      closesocket(s);
    }
    if (listen_ == INVALID_SOCKET) return false;
    thread_ = std::thread([this] { serve(); });
    return true;
  }
  void stop() {
    stopping_ = true;
    closesocket(listen_);
    if (thread_.joinable()) thread_.join();
  }
  int port() const { return port_; }
  int hits() const { return hits_; }
  void route(const std::string& target, Route r) { std::lock_guard<std::mutex> l(m_); routes_[target] = std::move(r); }
  // Whether a request asked for exactly `target` so far; the last request's target.
  bool saw(const std::string& target) {
    std::lock_guard<std::mutex> l(m_);
    for (const auto& t : targets_)
      if (t == target) return true;
    return false;
  }
  std::string last() {
    std::lock_guard<std::mutex> l(m_);
    return targets_.empty() ? std::string() : targets_.back();
  }

 private:
  void serve() {
    while (!stopping_) {
      SOCKET c = accept(listen_, nullptr, nullptr);
      if (c == INVALID_SOCKET) break;
      std::string req;
      char buf[4096];
      while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, n);
      }
      const size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
      const std::string target = sp2 == std::string::npos ? std::string() : req.substr(sp1 + 1, sp2 - sp1 - 1);
      ++hits_;
      Route r;
      {
        std::lock_guard<std::mutex> l(m_);
        targets_.push_back(target);
        const auto it = routes_.find(target);
        if (it != routes_.end()) r = it->second;
        else r.code = 404;
      }
      std::string head = "HTTP/1.1 " + std::to_string(r.code) +
                         (r.code == 200 ? " OK" : r.code == 302 ? " Found" : r.code == 400 ? " Bad Request" : " Not Found") +
                         "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
      if (!r.location.empty()) head += "Location: " + r.location + "\r\n";
      head += "\r\n";
      send(c, head.data(), (int)head.size(), 0);
      for (size_t off = 0; off < r.body.size();) {
        const int n = send(c, (const char*)r.body.data() + off, (int)std::min<size_t>(65536, r.body.size() - off), 0);
        if (n <= 0) break;   // the client stopped reading (the size cap)
        off += (size_t)n;
      }
      shutdown(c, SD_SEND);
      closesocket(c);
    }
  }
  SOCKET listen_ = INVALID_SOCKET;
  int port_ = 0;
  std::atomic<int> hits_{0};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex m_;
  std::map<std::string, Route> routes_;
  std::vector<std::string> targets_;
};

// A port in the range with nothing listening on it.
int free_port(int avoid) {
  for (int p = 42070; p < 42090; ++p) {
    if (p == avoid) continue;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)p);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    const bool ok = bind(s, (sockaddr*)&a, sizeof a) == 0;
    closesocket(s);
    if (ok) return p;
  }
  return 0;
}

void set_api(int port) {
  const std::string api = "http://127.0.0.1:" + std::to_string(port) + "/api/public/v1";
  _putenv_s("MELEE_TEST_MOD_BROWSER_API", api.c_str());
}

// The post /mods/<mod> as the API answers it, with one file.
std::vector<uint8_t> post_json(int mod, int file, const std::string& download_url, const std::string& export_url = "") {
  std::string s = "{\"data\":{\"id\":" + std::to_string(mod) + ",\"title\":\"t\",\"files\":[{\"id\":" + std::to_string(file + 100) +
                  ",\"download_url\":\"https://elsewhere.example/x.dat\"},{\"id\":" + std::to_string(file) + ",\"download_url\":" +
                  (download_url.empty() ? std::string("null") : "\"" + download_url + "\"") +
                  (export_url.empty() ? std::string() : ",\"export_url\":\"" + export_url + "\"") + "}]}}";
  return std::vector<uint8_t>(s.begin(), s.end());
}

uint32_t crc32_of(const std::vector<uint8_t>& b) {
  uint32_t c = 0xFFFFFFFFu;
  for (const uint8_t byte : b) {
    c ^= byte;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// A zip of stored (not compressed) files, as the site's export is a small zip.
std::vector<uint8_t> stored_zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> out, central;
  const auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
  const auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };
  for (const auto& [name, data] : files) {
    const uint32_t offset = (uint32_t)out.size(), crc = crc32_of(data), size = (uint32_t)data.size();
    u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);
    u32(out, crc); u32(out, size); u32(out, size); u16(out, (uint32_t)name.size()); u16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0); u16(central, 0);
    u16(central, 0x21); u32(central, crc); u32(central, size); u32(central, size); u16(central, (uint32_t)name.size());
    u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
    central.insert(central.end(), name.begin(), name.end());
  }
  const uint32_t at = (uint32_t)out.size();
  out.insert(out.end(), central.begin(), central.end());
  u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, (uint32_t)files.size()); u16(out, (uint32_t)files.size());
  u32(out, (uint32_t)central.size()); u32(out, at); u16(out, 0);
  return out;
}

size_t files_in(const fs::path& folder, const std::string& ext) {
  size_t n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ext) ++n;
  return n;
}

// A width x height RGBA PNG (a fixture made here: a colored square on clear), by stb_image_write.
std::vector<uint8_t> png(int width, int height, uint8_t red = 200) {
  std::vector<uint8_t> px((size_t)width * height * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      uint8_t* p = &px[((size_t)y * width + x) * 4];
      const bool inside = x > width / 6 && x < width - width / 6 && y > height / 6 && y < height - height / 6;
      p[0] = red; p[1] = (uint8_t)(x * 255 / (width > 1 ? width - 1 : 1)); p[2] = 40; p[3] = inside ? 255 : 0;
    }
  std::vector<uint8_t> out;
  stbi_write_png_to_func([](void* ctx, void* data, int size) {
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (const uint8_t*)data, (const uint8_t*)data + size);
  }, &out, width, height, 4, px.data(), width * 4);
  return out;
}

// The post /mods/<mod> with files of their own stock icons: {id, filename, stock_url ("" none)}.
struct PostFile { int id; std::string filename, stock_url; std::string sha256 = {}; std::string download_url = {}; };
std::vector<uint8_t> post_with_stock(int mod, const std::vector<PostFile>& files) {
  std::string s = "{\"data\":{\"id\":" + std::to_string(mod) + ",\"title\":\"t\",\"files\":[";
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    s += std::string(i ? "," : "") + "{\"id\":" + std::to_string(f.id) + ",\"filename\":\"" + f.filename +
         "\",\"file_type\":\"character_dat\",\"download_url\":" +
         (f.download_url.empty() ? std::string("null") : "\"" + f.download_url + "\"") + ",\"stock_url\":" +
         (f.stock_url.empty() ? std::string("null") : "\"" + f.stock_url + "\"") +
         ",\"sha256\":" + (f.sha256.empty() ? std::string("null") : "\"" + f.sha256 + "\"") + "}";
  }
  s += "]}}";
  return std::vector<uint8_t>(s.begin(), s.end());
}

// --view: the view marker on the card's own download_url, the API's /mods/<mod>/download?file=<file>.
void view_cases(StandIn& server, const std::string& base, const fs::path& cache) {
  // a. Asked for with &view=1 after the route's own query (the route answers only that).
  const auto dat17 = random_bytes(50 * 1024, 71);
  const std::string route17 = "/api/public/v1/mods/17/download?file=1";
  server.route("/api/public/v1/mods/17", {200, "", post_json(17, 1, base + route17)});
  server.route(route17 + "&view=1", {302, base + "/media/17-1.dat", {}});
  server.route("/media/17-1.dat", {200, "", dat17});
  int before = server.hits();
  auto r = host::skins::fetch_skin(17, 1, sha(dat17));
  std::printf("     reason: %s (%d requests)\n", r.reason.c_str(), server.hits() - before);
  CHECK(r.ok && r.bytes == dat17 && server.hits() - before == 3 && server.saw(route17 + "&view=1") && !server.saw(route17),
        "download route: an opponent's skin asked for with &view=1 (the post, the marked route, its redirect)");

  // b. A site that answers 400 to the marker on that route (the live site until it takes it): asked
  // once more as the card gives it, then taken and kept by its hash.
  const auto dat18 = random_bytes(40 * 1024, 72);
  const std::string route18 = "/api/public/v1/mods/18/download?file=1";
  server.route("/api/public/v1/mods/18", {200, "", post_json(18, 1, base + route18)});
  server.route(route18 + "&view=1", {400, "", {}});
  server.route(route18, {302, base + "/media/18-1.dat", {}});
  server.route("/media/18-1.dat", {200, "", dat18});
  before = server.hits();
  r = host::skins::fetch_skin(18, 1, sha(dat18));
  std::printf("     reason: %s (%d requests)\n", r.reason.c_str(), server.hits() - before);
  CHECK(r.ok && r.bytes == dat18 && server.hits() - before == 4 && server.saw(route18 + "&view=1") && server.saw(route18) &&
            r.reason.find("without the view marker") != std::string::npos,
        "download route answers 400 to the marker: asked once without it, taken");
  CHECK(fs::exists(cache / fs::u8path(sha(dat18) + ".skin")) && files_in(cache, ".part") == 0, "...: kept under its hash");
  before = server.hits();
  r = host::skins::fetch_skin(18, 1, sha(dat18));
  CHECK(r.ok && r.from_cache && server.hits() == before, "...: then from the cache, no request");

  // c. 400 without the marker too: one retry only, refused, nothing kept.
  const auto want20 = random_bytes(100, 73);
  const std::string route20 = "/api/public/v1/mods/20/download?file=1";
  server.route("/api/public/v1/mods/20", {200, "", post_json(20, 1, base + route20)});
  server.route(route20 + "&view=1", {400, "", {}});
  server.route(route20, {400, "", {}});
  before = server.hits();
  r = host::skins::fetch_skin(20, 1, sha(want20));
  std::printf("     reason: %s (%d requests)\n", r.reason.c_str(), server.hits() - before);
  CHECK(!r.ok && server.hits() - before == 3 && files_in(cache, ".part") == 0 && !fs::exists(cache / fs::u8path(sha(want20) + ".skin")),
        "400 without the marker too: one retry only, refused, nothing kept");

  // d. Any other refusal of the marked route (404): not asked again without the marker.
  const auto want21 = random_bytes(100, 74);
  const std::string route21 = "/api/public/v1/mods/21/download?file=1";
  server.route("/api/public/v1/mods/21", {200, "", post_json(21, 1, base + route21)});
  server.route(route21, {302, base + "/media/18-1.dat", {}});   // there, but never asked for
  before = server.hits();
  r = host::skins::fetch_skin(21, 1, sha(want21));
  CHECK(!r.ok && server.hits() - before == 2 && !server.saw(route21), "the marked route answers 404: no retry without the marker");

  // e. An install from the Mods menu (mod_browser::install_file, with the card's download_url as it
  // is): the bare route, never the marker; an address with the marker is refused without a request.
  const std::string route22 = "/api/public/v1/mods/22/download?file=1";
  before = server.hits();
  auto installed = host::mod_browser::install_file(22, 1, base + route22, "PlMrNr.dat", "t");
  CHECK(!installed.ok && server.hits() - before == 1 && server.last() == route22,
        "an install: the download route as the card gives it, no view marker");
  before = server.hits();
  installed = host::mod_browser::install_file(22, 1, base + route22 + "&view=1", "PlMrNr.dat", "t");
  CHECK(!installed.ok && installed.message == "Not an SSBM Nucleus link." && server.hits() == before,
        "an install is refused a download address with the view marker, nothing asked");
}
}  // namespace

int main(int argc, char** argv) {
  const bool view_only = argc > 1 && std::string(argv[1]) == "--view";
  // A public build never reads the stand-in's address (public_build.h): every fetch here would ask the
  // real site, so the test is skipped (77) there.
  if (host::public_build::kOn) { std::printf("SKIP public build: no stand-in for the site\n"); return 77; }
  host::test_env::automated = true;   // the stand-in API is honoured only in automated runs
  StandIn server;
  if (!server.start()) { std::printf("FAIL no loopback port in 42050-42069\n"); return 1; }
  const int port = server.port();
  set_api(port);
  const std::string base = "http://127.0.0.1:" + std::to_string(port);
  std::printf("stand-in on %s\n", base.c_str());

  const fs::path cache = fs::temp_directory_path() / ("mu_skin_fetch_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(cache, ec);
  host::skins::set_cache_folder(cache.u8string());

  if (view_only) {
    view_cases(server, base, cache);
    server.stop();
    fs::remove_all(cache, ec);
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
  }

  const auto skin = random_bytes(300 * 1024, 1);
  // The post names the file's own download, which redirects once to the media host, as the site does.
  // An opponent's skin asks for it with the view marker (download_view_address): the route answers
  // only "<download_url>?view=1" (this one has no query), so every fetch below that passes sent it.
  server.route("/api/public/v1/mods/7", {200, "", post_json(7, 3, base + "/dl/7-3")});
  server.route("/dl/7-3?view=1", {302, base + "/media/7-3.dat", {}});
  server.route("/media/7-3.dat", {200, "", skin});

  CHECK(host::skins::post_url(7) == base + "/api/public/v1/mods/7", "the post's address is the API's own");

  // 1. A good fetch.
  int before = server.hits();
  auto r = host::skins::fetch_skin(7, 3, sha(skin));
  CHECK(r.ok && !r.from_cache && r.bytes == skin, "good fetch: ok, the bytes as served");
  CHECK(server.hits() - before == 3 && server.saw("/dl/7-3?view=1") && !server.saw("/dl/7-3"),
        "good fetch: the post, then its download_url with ?view=1 and the one redirect");
  CHECK(r.ok && fs::path(fs::u8path(r.path)).filename() == fs::u8path(sha(skin) + ".skin"), "kept under its hash");
  CHECK(files_in(cache, ".part") == 0, "no partial file left");

  // 2. The cache.
  before = server.hits();
  r = host::skins::fetch_skin(7, 3, sha(skin));
  CHECK(r.ok && r.from_cache && r.bytes == skin && server.hits() == before, "second fetch: from the cache, no request");
  {
    std::ofstream damage(cache / fs::u8path(sha(skin) + ".skin"), std::ios::binary | std::ios::trunc);
    damage << "not the skin";
  }
  r = host::skins::fetch_skin(7, 3, sha(skin));
  CHECK(r.ok && !r.from_cache && r.bytes == skin && server.hits() > before, "damaged cached file: fetched again");

  // 3. A hash that differs.
  const auto other = random_bytes(1000, 2);
  server.route("/api/public/v1/mods/8", {200, "", post_json(8, 1, base + "/media/8-1.dat")});
  server.route("/media/8-1.dat?view=1", {200, "", random_bytes(5000, 3)});
  r = host::skins::fetch_skin(8, 1, sha(other));
  CHECK(!r.ok && r.reason.find("SHA-256 differs") != std::string::npos, "hash mismatch refused");
  CHECK(!fs::exists(cache / fs::u8path(sha(other) + ".skin")) && files_in(cache, ".part") == 0, "hash mismatch: nothing kept");
  std::printf("     reason: %s\n", r.reason.c_str());

  // 4. The size cap: one byte past 8 MB refused, exactly 8 MB taken.
  const auto big = random_bytes((size_t)host::skins::kMaxSkinBytes + 1, 4);
  server.route("/api/public/v1/mods/9", {200, "", post_json(9, 1, base + "/media/9-1.dat")});
  server.route("/media/9-1.dat?view=1", {200, "", big});
  r = host::skins::fetch_skin(9, 1, sha(big));
  CHECK(!r.ok && r.reason.find("larger than 8 MB") != std::string::npos, "8 MB + 1 byte refused");
  CHECK(!fs::exists(cache / fs::u8path(sha(big) + ".skin")) && files_in(cache, ".part") == 0, "oversize: nothing kept");
  std::printf("     reason: %s\n", r.reason.c_str());
  const std::vector<uint8_t> exact(big.begin(), big.end() - 1);
  server.route("/api/public/v1/mods/19", {200, "", post_json(19, 2, base + "/media/9-2.dat")});
  server.route("/media/9-2.dat?view=1", {200, "", exact});
  r = host::skins::fetch_skin(19, 2, sha(exact));
  CHECK(r.ok && r.bytes.size() == host::skins::kMaxSkinBytes, "exactly 8 MB taken");

  // 5. Not on the server: the post is gone, or it has no such file.
  r = host::skins::fetch_skin(10, 1, sha(random_bytes(10, 5)));
  CHECK(!r.ok && r.reason.find("no longer offers") != std::string::npos, "missing post refused");
  std::printf("     reason: %s\n", r.reason.c_str());
  r = host::skins::fetch_skin(7, 4, sha(random_bytes(10, 5)));
  CHECK(!r.ok && r.reason.find("has no file 4") != std::string::npos, "a file the post does not have refused");
  std::printf("     reason: %s\n", r.reason.c_str());

  // 5b. A file with no download of its own: the site's export of exactly that file, a zip holding the
  // DAT (named by its hash) and its pictures; the DAT is what is taken and kept.
  {
    const auto dat = random_bytes(40 * 1024, 21), csp = random_bytes(900, 22);
    const auto zip = stored_zip({{"csp.png", csp}, {"PlMrNrSkin.dat", dat}, {"stc.png", random_bytes(100, 23)}});
    server.route("/api/public/v1/mods/13", {200, "", post_json(13, 1, "")});
    server.route("/api/posts/13/export-nucleus/1?view=1", {200, "", zip});   // a view, not a download
    before = server.hits();
    r = host::skins::fetch_skin(13, 1, sha(dat));
    std::printf("     reason: %s (%d requests)\n", r.reason.c_str(), server.hits() - before);
    CHECK(r.ok && r.bytes == dat && server.hits() - before == 2, "export route: the DAT of that hash taken out of the export");
    CHECK(fs::exists(cache / fs::u8path(sha(dat) + ".skin")) && files_in(cache, ".part") == 0 && files_in(cache, ".zip") == 0,
          "export route: the DAT kept, the zip gone");
    r = host::skins::fetch_skin(13, 1, sha(dat));
    CHECK(r.ok && r.from_cache && r.bytes == dat, "export route: then from the cache");
    // The same export with no DAT of the hash sent: refused, nothing kept.
    const auto wanted = random_bytes(500, 24);
    server.route("/api/public/v1/mods/14", {200, "", post_json(14, 1, "")});
    server.route("/api/posts/14/export-nucleus/1?view=1", {200, "", zip});   // a view, not a download
    r = host::skins::fetch_skin(14, 1, sha(wanted));
    CHECK(!r.ok && r.reason.find("no file of that hash") != std::string::npos && files_in(cache, ".part") == 0 &&
              !fs::exists(cache / fs::u8path(sha(wanted) + ".skin")),
          "export route: no DAT of that hash refused");
    std::printf("     reason: %s\n", r.reason.c_str());
    // An export_url that is not this card's export on the site: refused without a download.
    server.route("/api/public/v1/mods/15", {200, "", post_json(15, 1, "", base + "/api/posts/99/export-nucleus/1")});
    before = server.hits();
    r = host::skins::fetch_skin(15, 1, sha(random_bytes(77, 25)));
    CHECK(!r.ok && r.reason.find("refused export address") != std::string::npos && server.hits() - before == 1,
          "an export address of another card refused");
    // The export is asked for as a view (kViewQuery: the site serves it without counting a download of
    // the post); the routes above answer only "<route>?view=1", so the fetches above sent it. An install
    // from the Mods menu asks for the bare route, and is refused an address with the view parameter.
    {
      std::string why;
      const std::string install = host::mod_browser::export_url(13, 1);
      const std::string opponent = host::skins::export_view_address(13, 1, "", &why);
      std::printf("     opponent's skin from %s; an install from %s\n", opponent.c_str(), install.c_str());
      CHECK(opponent == install + "?view=1", "an opponent's skin: the export address with ?view=1");
      CHECK(install.find("view=") == std::string::npos, "an install: the export address without the view parameter");
      const std::string site = "https://ssbmnucleus.net/api/posts/13/export-nucleus/1";
      CHECK(host::skins::export_view_address(13, 1, site, &why) == site + "?view=1" &&
                host::skins::export_view_address(13, 1, site + "?view=1", &why) == site + "?view=1" &&
                host::skins::export_view_address(13, 1, install + "?view=1", &why) == opponent,
            "the card's own export_url, bare or with ?view=1: taken once, with ?view=1");
      CHECK(host::skins::export_view_address(13, 1, site + "?view=1&x=2", &why).empty() &&
                host::skins::export_view_address(13, 1, site + "?download=1", &why).empty() &&
                host::skins::export_view_address(13, 1, site + "?view=1?view=1", &why).empty() &&
                host::skins::export_view_address(13, 1, "https://ssbmnucleus.net/api/posts/13/export-nucleus/2?view=1", &why).empty() &&
                host::skins::export_view_address(13, 1, "https://elsewhere.example/api/posts/13/export-nucleus/1?view=1", &why).empty(),
            "any other address or parameter refused");
      before = server.hits();
      const auto refused = host::mod_browser::install_export(13, 1, opponent, "PlMrNrSkin.dat", "t");
      CHECK(!refused.ok && refused.message == "Not an SSBM Nucleus link." && server.hits() == before, "an install is refused the view address, nothing asked");
    }
  }

  // 5c. An export whose files are named in a legacy code page (CP437 0x82, not UTF-8), as old zip tools
  // write them: the DAT of the hash is still found, and nothing throws (a UTF-8 path conversion of such
  // a name threw on the fetch worker, which ended the game; the review of 2026-11-24).
  {
    const auto dat = random_bytes(30 * 1024, 31);
    const auto zip = stored_zip({{"Caf\x82 skin/csp \x82t\x82.png", random_bytes(700, 32)}, {"Caf\x82 skin/PlMrNr \x82.dat", dat}});
    server.route("/api/public/v1/mods/16", {200, "", post_json(16, 1, "")});
    server.route("/api/posts/16/export-nucleus/1?view=1", {200, "", zip});   // a view, not a download
    bool threw = false;
    try {
      r = host::skins::fetch_skin(16, 1, sha(dat));
    } catch (...) {
      threw = true;
    }
    CHECK(!threw && r.ok && r.bytes == dat, "legacy-encoded names in the export: the DAT of that hash taken, nothing thrown");
    threw = false;
    try {
      r = host::skins::fetch_skin(16, 1, sha(random_bytes(10, 33)));
    } catch (...) {
      threw = true;
    }
    CHECK(!threw && !r.ok, "legacy-encoded names, no DAT of that hash: refused, nothing thrown");
  }

  // 5d. The view marker on a card's own download_url (download_view_address; no request): after the
  // route's query, or as the query; once; before a fragment; any other view refused.
  {
    std::string why;
    const std::string api = "https://ssbmnucleus.net/api/public/v1/mods/13/download?file=1";
    const std::string media = "https://cdn.ssbmnucleus.net/files/13-1.dat";
    CHECK(host::skins::download_view_address(api, &why) == api + "&view=1", "the API's download route: &view=1 after its query");
    CHECK(host::skins::download_view_address(media, &why) == media + "?view=1", "an address with no query: ?view=1");
    CHECK(host::skins::download_view_address(api + "&view=1", &why) == api + "&view=1" &&
              host::skins::download_view_address(media + "?view=1", &why) == media + "?view=1",
          "an address with the marker already: taken as it is, not twice");
    CHECK(host::skins::download_view_address(media + "#top", &why) == media + "?view=1#top", "the marker goes before a fragment");
    CHECK(host::skins::download_view_address(api + "&view=0", &why).empty() && host::skins::download_view_address(api + "&view", &why).empty() &&
              host::skins::download_view_address(media + "?view=2", &why).empty() && host::skins::download_view_address("", &why).empty(),
          "another view parameter (or no address) refused");
  }

  // 6. A redirect off the allowed host: to a second stand-in on another loopback address (127.0.0.2),
  // which answers, so the refusal is the host rule's and nothing leaves this computer.
  const auto elsewhere = random_bytes(2000, 6);
  StandIn other_host;
  if (other_host.start("127.0.0.2", 42060, 42070)) {
    other_host.route("/media/x", {200, "", elsewhere});
    server.route("/api/public/v1/mods/11", {200, "", post_json(11, 1, base + "/dl/11-1")});
    server.route("/dl/11-1?view=1", {302, "http://127.0.0.2:" + std::to_string(other_host.port()) + "/media/x", {}});
    r = host::skins::fetch_skin(11, 1, sha(elsewhere));
    CHECK(!r.ok && r.reason.find("left SSBM Nucleus") != std::string::npos && files_in(cache, ".part") == 0 &&
              !fs::exists(cache / fs::u8path(sha(elsewhere) + ".skin")),
          "redirect off the allowed host refused (the other host answered)");
    std::printf("     reason: %s (other host hits %d)\n", r.reason.c_str(), other_host.hits());
    other_host.stop();
  } else {
    std::printf("FAIL could not bind 127.0.0.2 for the redirect case\n");
    ++g_failures;
  }

  // 7. Bad ids or hash: refused before any request.
  before = server.hits();
  CHECK(!host::skins::fetch_skin(0, 1, sha(skin)).ok, "mod id 0 refused");
  CHECK(!host::skins::fetch_skin(7, 0, sha(skin)).ok, "file id 0 refused");
  std::string upper = sha(random_bytes(10, 7));
  for (auto& c : upper) c = (char)toupper((unsigned char)c);
  CHECK(!host::skins::fetch_skin(7, 3, upper).ok, "upper-case hash refused");
  CHECK(!host::skins::fetch_skin(7, 3, "abc").ok, "short hash refused");
  CHECK(server.hits() == before, "no request for a bad id or hash");

  // 7b. The skin's own stock icon: the card's stock_url on the same post, kept beside the skin under
  // the skin's hash.
  const auto skin_sha = [](uint32_t seed) { return sha(random_bytes(64, 1000 + seed)); };
  {
    const auto icon = png(24, 24);
    // A skin fetched now: its post, read for the download, already gave its stock icon's address, so
    // the icon costs one request (the picture), not a second read of the post.
    const auto dat = random_bytes(20 * 1024, 39);
    server.route("/api/public/v1/mods/39", {200, "", post_with_stock(39, {{3901, "PlMrNr.dat", base + "/media/39.png", "",
                                                                           base + "/media/39.dat"}})});
    server.route("/media/39.dat?view=1", {200, "", dat});
    server.route("/media/39.png", {200, "", png(32, 32, 120)});
    r = host::skins::fetch_skin(39, 3901, sha(dat));
    before = server.hits();
    auto s = host::skins::fetch_stock_icon(39, 3901, sha(dat));
    CHECK(r.ok && s.ok && server.hits() - before == 1, "skin then its stock icon: the post read once, then only the picture");
    server.route("/api/public/v1/mods/40", {200, "", post_with_stock(40, {{4001, "PlCaNrMod.dat", base + "/media/40-stock.png"}})});
    server.route("/media/40-stock.png", {200, "", icon});
    before = server.hits();
    s = host::skins::fetch_stock_icon(40, 4001, skin_sha(1));
    std::printf("     reason: %s\n", s.reason.c_str());
    CHECK(s.ok && !s.none && !s.from_cache && server.hits() - before == 2, "good stock icon: the post, then the picture");
    CHECK(s.ok && fs::path(fs::u8path(s.path)).filename() == fs::u8path(skin_sha(1) + ".stock.png") &&
              files_in(cache, ".part") == 0,
          "good stock icon: kept beside the skin under the skin's hash");
    const auto texture = host::roster::console_picture(s.path, 24, 24);
    CHECK(texture.size() == 24 * 24 * 4, "good stock icon: the HUD's 24x24 console texture");
    before = server.hits();
    s = host::skins::fetch_stock_icon(40, 4001, skin_sha(1));
    CHECK(s.ok && s.from_cache && server.hits() == before, "good stock icon: then from the cache, no request");
    // A damaged cached icon is asked for again.
    {
      std::ofstream damage(cache / fs::u8path(skin_sha(1) + ".stock.png"), std::ios::binary | std::ios::trunc);
      damage << "not a picture";
    }
    before = server.hits();
    s = host::skins::fetch_stock_icon(40, 4001, skin_sha(1));
    CHECK(s.ok && !s.from_cache && server.hits() - before == 1, "damaged cached stock icon: fetched again (the post known)");

    // A larger icon (the site's are often 32 or 64 a side) is taken and resampled for the HUD.
    server.route("/api/public/v1/mods/41", {200, "", post_with_stock(41, {{4101, "PlMrNr.dat", base + "/media/41.png?v=2"}})});
    server.route("/media/41.png?v=2", {200, "", png(64, 64, 30)});
    s = host::skins::fetch_stock_icon(41, 4101, skin_sha(2));
    CHECK(s.ok && host::roster::console_picture(s.path, 24, 24).size() == 24 * 24 * 4, "64x64 stock icon taken, made 24x24");

    // No icon of its own: none, kept, and the post is not read again.
    server.route("/api/public/v1/mods/42", {200, "", post_with_stock(42, {{4201, "PlFxNr.dat", ""}})});
    before = server.hits();
    s = host::skins::fetch_stock_icon(42, 4201, skin_sha(3));
    std::printf("     reason: %s\n", s.reason.c_str());
    CHECK(!s.ok && s.none && server.hits() - before == 1 && fs::exists(cache / fs::u8path(skin_sha(3) + ".nostock")),
          "missing stock icon: none (the retail icon), kept as such");
    before = server.hits();
    s = host::skins::fetch_stock_icon(42, 4201, skin_sha(3));
    CHECK(!s.ok && s.none && s.from_cache && server.hits() == before, "missing stock icon: then from the cache, no request");

    // A WebP address: not a picture the reader takes, no request for it.
    server.route("/api/public/v1/mods/43", {200, "", post_with_stock(43, {{4301, "PlFxNr.dat", base + "/media/43.webp"}})});
    before = server.hits();
    s = host::skins::fetch_stock_icon(43, 4301, skin_sha(4));
    CHECK(!s.ok && s.none && server.hits() - before == 1, "a WebP stock icon address: none, not fetched");

    // The same icon address as another file of the post (a pack sharing one picture): not this file's
    // for sure, so none, as the mod browser installs it. Nana's file (Popo's partner) is no sibling.
    // (One post for both: read once.)
    server.route("/api/public/v1/mods/44", {200, "", post_with_stock(44, {{4401, "Pack/PlFxNr.dat", base + "/media/44.png"},
                                                                          {4402, "Pack/PlFxGr.dat", base + "/media/44.png"},
                                                                          {4501, "PlPpNr.dat", base + "/media/45.png"},
                                                                          {4502, "PlNnNr.dat", base + "/media/45.png"}})});
    server.route("/media/44.png", {200, "", png(24, 24, 90)});
    server.route("/media/45.png", {200, "", png(24, 24, 60)});
    s = host::skins::fetch_stock_icon(44, 4401, skin_sha(5));
    std::printf("     reason: %s\n", s.reason.c_str());
    CHECK(!s.ok && s.none && s.reason.find("another file") != std::string::npos, "a stock icon another file shares: none");
    s = host::skins::fetch_stock_icon(44, 4501, skin_sha(6));
    CHECK(s.ok, "Popo's stock icon shared with Nana's file: taken");

    // Refused pictures: kept as none (the same bytes would be refused again), nothing else kept. One
    // post, a file each.
    const struct { int file; std::vector<uint8_t> body; const char* what; const char* reason; } refused[] = {
        {4601, random_bytes((size_t)host::skins::kMaxStockBytes + 1, 41), "past 4 MB", "larger than 4 MB"},
        {4602, png(2048, 1), "2048 pixels wide", "more than 1024 a side"},
        {4603, random_bytes(3000, 42), "not a PNG", "not a PNG"},
        {4604, std::vector<uint8_t>(icon.begin(), icon.begin() + (std::ptrdiff_t)(icon.size() * 2 / 3)), "a cut PNG", "does not decode"},
        {4605, {0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F', 0}, "a JPEG", "not a PNG"},
    };
    std::vector<PostFile> refused_files;
    for (const auto& c : refused) {
      const std::string media = "/media/" + std::to_string(c.file) + ".png";
      refused_files.push_back({c.file, "PlFx" + std::to_string(c.file) + ".dat", base + media});
      server.route(media, {200, "", c.body});
    }
    server.route("/api/public/v1/mods/46", {200, "", post_with_stock(46, refused_files)});
    for (const auto& c : refused) {
      s = host::skins::fetch_stock_icon(46, (uint32_t)c.file, skin_sha((uint32_t)c.file));
      std::printf("     %s: %s\n", c.what, s.reason.c_str());
      CHECK(!s.ok && s.none && s.reason.find(c.reason) != std::string::npos &&
                !fs::exists(cache / fs::u8path(skin_sha((uint32_t)c.file) + ".stock.png")) && files_in(cache, ".part") == 0,
            (std::string("stock icon refused: ") + c.what).c_str());
    }

    // A post without that file: nothing kept (the request is tried again next time).
    s = host::skins::fetch_stock_icon(40, 4999, skin_sha(7));
    CHECK(!s.ok && !s.none && !fs::exists(cache / fs::u8path(skin_sha(7) + ".nostock")), "a file the post does not have: nothing kept");
    // Bad ids or hash: refused before any request.
    before = server.hits();
    CHECK(!host::skins::fetch_stock_icon(0, 4001, skin_sha(1)).ok && !host::skins::fetch_stock_icon(40, 4001, "abc").ok &&
              server.hits() == before,
          "stock icon: bad id or hash refused without a request");
  }

  // 8. The server down: a port in the range with nothing listening.
  const int down = free_port(port);
  set_api(down);
  const auto start = GetTickCount64();
  r = host::skins::fetch_skin(12, 1, sha(random_bytes(10, 8)));
  CHECK(!r.ok && r.reason.find("could not be reached") != std::string::npos, "server down refused");
  std::printf("     reason: %s (%llu ms)\n", r.reason.c_str(), (unsigned long long)(GetTickCount64() - start));
  // The cache still answers with the server down.
  r = host::skins::fetch_skin(7, 3, sha(skin));
  CHECK(r.ok && r.from_cache, "cached skin still served with the server down");
  {
    // A stock icon with the server down: nothing kept (asked for again next time); a cached one still served.
    const auto s = host::skins::fetch_stock_icon(60, 6001, skin_sha(60));
    CHECK(!s.ok && !s.none && !fs::exists(cache / fs::u8path(skin_sha(60) + ".nostock")) &&
              !fs::exists(cache / fs::u8path(skin_sha(60) + ".stock.png")),
          "stock icon with the server down: refused, nothing kept");
    std::printf("     reason: %s\n", s.reason.c_str());
    CHECK(host::skins::fetch_stock_icon(40, 4001, skin_sha(1)).from_cache, "cached stock icon still served with the server down");
  }

  // 9. Outside an automated run the stand-in is not honoured (the real site would be asked, so this
  // only checks the address, it does not fetch).
  host::test_env::automated = false;
  CHECK(host::skins::post_url(7) == "https://ssbmnucleus.net/api/public/v1/mods/7", "not automated: the real API address");
  host::test_env::automated = true;

  server.stop();
  fs::remove_all(cache, ec);
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
