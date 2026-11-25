// Opponents' skins online: fetching one skin file by id and hash (skin_fetch.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "skin_fetch.h"
#include "archive_unpack.h"
#include "hackpack_ai.h"
#include "host.h"
#include "mod_browser.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "nlohmann/json.hpp"

// A stock icon is a PNG: stb_image's reader, kept private to this file (as roster.cpp's), told the
// size from the header before it decodes anything.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace host::skins {
namespace {
namespace fs = std::filesystem;

std::mutex g_mutex;
fs::path g_folder;
std::atomic<uint32_t> g_part_serial{0};

constexpr uint64_t kMaxPostBytes = 4u * 1024u * 1024u;   // one post of /mods/{id}, every file of it

std::string sha256_of(const std::vector<uint8_t>& bytes) {
  return host::hackpack_ai::rules::sha256_hex(bytes.data(), bytes.size());
}

// The whole file when it is at most `limit` bytes.
bool read_limited(const fs::path& path, uint64_t limit, std::vector<uint8_t>* out) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->resize((size_t)size);
  if (size && !in.read((char*)out->data(), (std::streamsize)size)) return false;
  return in.peek() == std::char_traits<char>::eof();
}

// A file of the download (a zip: the site's export, or a post's own zip) whose bytes have exactly
// `sha256`: the costume file the opponent's machine installed, which it named by that hash. Read in
// memory by the host's zip reader (archive_unpack.h); nothing is unpacked to disk.
bool member_with_hash(const std::vector<uint8_t>& download, const std::string& sha256, std::vector<uint8_t>* out) {
  host::archive::Zip zip;
  std::string why;
  if (!zip.open_memory(download, {64, 64ull << 20, 64ull << 20, 250}, &why)) {
    host::log("skins: the download is not a usable zip: %s", why.c_str());
    return false;
  }
  for (const host::archive::Entry& e : zip.entries()) {
    if (e.folder() || e.size > kMaxSkinBytes) continue;
    const std::string ext = e.extension();   // from the name's bytes: a legacy-encoded name must not throw
    if (ext != ".dat" && ext != ".usd") continue;
    std::vector<uint8_t> bytes;
    if (zip.read(e, &bytes, &why) && sha256_of(bytes) == sha256) {
      *out = std::move(bytes);
      return true;
    }
  }
  return false;
}
}  // namespace

void configure_cache(const std::string& settings_path_utf8) {
  fs::path parent = fs::u8path(settings_path_utf8).parent_path();
  if (parent.empty()) parent = fs::current_path();
  set_cache_folder((parent / L"OpponentSkins").u8string());
}

void set_cache_folder(const std::string& folder_utf8) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_folder = fs::u8path(folder_utf8);
}

std::string cache_folder() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_folder.u8string();
}

bool valid_sha256(const std::string& text) {
  if (text.size() != 64) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string post_url(uint32_t mod_id) { return host::mod_browser::api_address() + "/mods/" + std::to_string(mod_id); }

namespace {
// A file's text field, at most 1024 bytes, else "".
std::string text_of(const nlohmann::json& f, const char* key) {
  const auto it = f.find(key);
  return it != f.end() && it->is_string() && it->get_ref<const std::string&>().size() <= 1024 ? it->get<std::string>()
                                                                                                : std::string();
}
bool file_is(const nlohmann::json& f, uint32_t file_id) {
  return f.is_object() && f.contains("id") && f["id"].is_number_unsigned() && f["id"].get<uint64_t>() == file_id;
}
std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
// A picture address the mod browser takes for a card (mods_menu_cards.cpp picture_address): https (or
// the test stand-in), and a kind the reader takes (WebP and GIF are not).
bool picture_address(const std::string& url) {
  if (url.empty() || url.size() > 1024 || (url.rfind("https://", 0) != 0 && url.rfind("http://127.0.0.1:", 0) != 0)) return false;
  const std::string path = lower(url.substr(0, url.find('?')));
  const auto ends = [&](const char* e) {
    const size_t n = std::strlen(e);
    return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
  };
  return !ends(".webp") && !ends(".gif");
}

// One file's stock icon as its post gives it: the address, or none with the reason.
struct StockOfFile { std::string url, reason; };
StockOfFile stock_of(const nlohmann::json& files, const nlohmann::json& own) {
  const std::string url = text_of(own, "stock_url");
  if (url.empty()) return {{}, "its card has no stock icon"};
  if (!picture_address(url)) return {{}, "its card's stock icon is not a picture this reader takes (" + url + ")"};
  // Another file of the post with the same picture (the mod browser's judge_pictures): not this file's
  // for sure, so the sender's machine installed the skin without it and shows the retail icon. Files
  // of another kind, the same file, and Nana's file (Popo's partner, one costume) are not siblings.
  const std::string type = text_of(own, "file_type"), sha = text_of(own, "sha256"), name = lower(text_of(own, "filename"));
  for (const auto& f : files) {
    if (&f == &own || !f.is_object() || text_of(f, "file_type") != type || text_of(f, "stock_url") != url) continue;
    if (f.contains("id") && f["id"] == own["id"]) continue;
    const std::string other_sha = text_of(f, "sha256"), other_name = lower(text_of(f, "filename"));
    if (other_name.find("plnn") != std::string::npos) continue;
    if (!sha.empty() && !other_sha.empty() ? sha != other_sha : name != other_name)
      return {{}, "its card's stock icon is another file's of the post too"};
  }
  return {url, {}};
}

// The stock icons of the posts read this run, by (mod, file): a skin fetched by its download reads its
// post once, and its stock icon is then known without a second request (the API's requests are few).
std::mutex g_seen_mutex;
std::map<std::pair<uint32_t, uint32_t>, StockOfFile> g_stock_seen;

// The post /mods/<mod> (one GET): its files array, or false with the reason. Every file's stock icon
// is noted (g_stock_seen).
bool post_files(uint32_t mod_id, nlohmann::json* files, std::string* reason) {
  std::string body, error;
  int status = 0;
  host::mod_browser::NetResult net;
  if (!host::mod_browser::get_text(post_url(mod_id), &body, kMaxPostBytes, &error, &status, host::mod_browser::kWaitList, &net)) {
    *reason = status == 404 || status == 410 ? "SSBM Nucleus no longer offers mod " + std::to_string(mod_id) : error;
    return false;
  }
  nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
  nlohmann::json* post = j.is_object() && j.contains("data") && j["data"].is_object() ? &j["data"] : nullptr;
  if (!post || !post->contains("files") || !(*post)["files"].is_array()) {
    *reason = "SSBM Nucleus sent no post for mod " + std::to_string(mod_id);
    return false;
  }
  *files = std::move((*post)["files"]);
  std::lock_guard<std::mutex> lock(g_seen_mutex);
  for (const auto& f : *files)
    if (f.is_object() && f.contains("id") && f["id"].is_number_unsigned() && f["id"].get<uint64_t>() <= UINT32_MAX)
      g_stock_seen[{mod_id, (uint32_t)f["id"].get<uint64_t>()}] = stock_of(*files, f);
  return true;
}
}  // namespace

std::string export_view_address(uint32_t mod_id, uint32_t file_id, const std::string& given, std::string* reason) {
  const std::string built = host::mod_browser::export_url((int)mod_id, (int)file_id);
  const std::string tail = "/api/posts/" + std::to_string(mod_id) + "/export-nucleus/" + std::to_string(file_id);
  // The card's export_url is taken only when it is exactly this card's route on the site (or the test
  // stand-in), with nothing after it but that same view parameter.
  std::string bare = given;
  const size_t view = std::strlen(kViewQuery);
  if (bare.size() > view && bare.compare(bare.size() - view, view, kViewQuery) == 0) bare.resize(bare.size() - view);
  if (!given.empty() && bare != built && bare != "https://ssbmnucleus.net" + tail && bare != "https://www.ssbmnucleus.net" + tail) {
    *reason = "refused export address " + given;
    return {};
  }
  return (given.empty() ? built : bare) + kViewQuery;
}

std::string download_view_address(const std::string& download_url, std::string* reason) {
  if (download_url.empty()) {
    *reason = "no download address";
    return {};
  }
  const size_t hash = download_url.find('#');
  const std::string head = download_url.substr(0, hash);
  const std::string fragment = hash == std::string::npos ? std::string() : download_url.substr(hash);
  const size_t q = head.find('?');
  if (q != std::string::npos) {
    // A view the address names already: this one is taken as it is, any other refused.
    const std::string query = head.substr(q + 1);
    for (size_t at = 0; at <= query.size();) {
      size_t amp = query.find('&', at);
      if (amp == std::string::npos) amp = query.size();
      const std::string param = query.substr(at, amp - at);
      if (param.substr(0, param.find('=')) == "view") {
        if (param == kViewParam) return download_url;
        *reason = "refused download address " + download_url + " (another view parameter)";
        return {};
      }
      at = amp + 1;
    }
  }
  return head + (q == std::string::npos ? std::string(kViewQuery) : std::string("&") + kViewParam) + fragment;
}

std::string file_source(uint32_t mod_id, uint32_t file_id, bool* is_export, std::string* reason, std::string* unmarked) {
  *is_export = false;
  if (unmarked) unmarked->clear();
  nlohmann::json files;
  if (!post_files(mod_id, &files, reason)) return {};
  for (const auto& f : files) {
    if (!file_is(f, file_id)) continue;
    const auto text = [&](const char* key) { return text_of(f, key); };
    // The card's own download, as the mod browser installs it (mods_menu_install.cpp), asked for as a
    // view (the marker after its query): the site serves the same bytes without counting a download of
    // the post. The mod browser's GET takes SSBM Nucleus's hosts only, whatever the address says.
    const std::string own = text("download_url");
    if (!own.empty()) {
      if (unmarked) *unmarked = own;
      return download_view_address(own, reason);
    }
    // Else the site's per-file export of exactly this card (what the mod browser's export route takes),
    // asked for as a view (kViewQuery): the site serves the same bytes without counting a download of
    // the post, which only an install from the Mods menu counts (install_export never sends it).
    *is_export = true;
    return export_view_address(mod_id, file_id, text("export_url"), reason);
  }
  *reason = "mod " + std::to_string(mod_id) + " has no file " + std::to_string(file_id);
  return {};
}

Fetched fetch_skin(uint32_t mod_id, uint32_t file_id, const std::string& sha256) {
  Fetched r;
  if (!mod_id || !file_id) { r.reason = "no mod or file id"; return r; }
  if (!valid_sha256(sha256)) { r.reason = "not a SHA-256"; return r; }
  const fs::path folder = fs::u8path(cache_folder());
  if (folder.empty()) { r.reason = "no cache folder"; return r; }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path kept = folder / fs::u8path(sha256 + ".skin");
  if (fs::is_regular_file(kept, ec)) {
    std::vector<uint8_t> bytes;
    if (read_limited(kept, kMaxSkinBytes, &bytes) && sha256_of(bytes) == sha256) {
      r.ok = true; r.from_cache = true; r.path = kept.u8string(); r.bytes = std::move(bytes);
      r.reason = "cached";
      return r;
    }
    fs::remove(kept, ec);   // damaged or changed since it was kept: fetched again
  }
  bool is_export = false;
  std::string unmarked;
  const std::string url = file_source(mod_id, file_id, &is_export, &r.reason, &unmarked);
  if (url.empty()) return r;
  // A name of its own per call, so two fetches of one hash never write the same file.
  const std::string serial = std::to_string(++g_part_serial);
  const fs::path part = folder / fs::u8path(sha256 + "." + serial + ".part");
  std::string error;
  bool too_large = false, unviewed = false;
  int status = 0;
  bool got_it = host::mod_browser::get_file(url, part.u8string(), kMaxSkinBytes, &error, &too_large, &status);
  if (!got_it && status == 400 && !is_export && !unmarked.empty() && unmarked != url) {
    // The download route of a site that does not know the view marker yet answers 400 to it: asked
    // once more as the card gives it (counted there as a download of the post).
    host::log("skins: mod %u file %u: the site answered 400 to the view marker on its download route; asked once without it",
              mod_id, file_id);
    fs::remove(part, ec);
    error.clear();
    too_large = false;
    got_it = host::mod_browser::get_file(unmarked, part.u8string(), kMaxSkinBytes, &error, &too_large);
    unviewed = got_it;
  }
  if (!got_it) {
    fs::remove(part, ec);
    r.reason = too_large ? "larger than " + std::to_string(kMaxSkinBytes >> 20) + " MB" : error;
    return r;
  }
  std::vector<uint8_t> bytes;
  if (!read_limited(part, kMaxSkinBytes, &bytes)) {
    fs::remove(part, ec);
    r.reason = "larger than " + std::to_string(kMaxSkinBytes >> 20) + " MB";
    return r;
  }
  const std::string got = sha256_of(bytes);
  bool whole = got == sha256;
  // The hash names the costume file itself: the download when it is that file, else the file of
  // that hash inside it (the site's export is a zip of the file and its pictures).
  const bool zip = bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4;
  if (!whole && zip) {
    std::vector<uint8_t> member;
    if (member_with_hash(bytes, sha256, &member)) {
      bytes = std::move(member);
      whole = true;
    }
  }
  if (!whole) {
    fs::remove(part, ec);
    r.reason = "SHA-256 differs from the one sent (got " + got.substr(0, 12) + (zip ? ", no file of that hash in it" : "") + ")";
    return r;
  }
  if (zip && got != sha256) {
    // Only the checked file is kept, under its hash.
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!out) { out.close(); fs::remove(part, ec); }
  }
  fs::rename(part, kept, ec);
  if (ec) {
    // Another fetch of the same hash got there first, or the folder refuses: the checked bytes are
    // still good for this call.
    fs::remove(part, ec);
  }
  r.ok = true;
  r.path = kept.u8string();
  r.bytes = std::move(bytes);
  r.reason = is_export ? "fetched through the site's export" : unviewed ? "fetched (without the view marker, which the site refused)" : "fetched";
  return r;
}

// ---- the skin's own stock icon ----

std::string stock_source(uint32_t mod_id, uint32_t file_id, bool* none, std::string* reason) {
  *none = false;
  const auto seen = [&](StockOfFile* out) {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    const auto it = g_stock_seen.find({mod_id, file_id});
    if (it == g_stock_seen.end()) return false;
    *out = it->second;
    return true;
  };
  StockOfFile s;
  if (!seen(&s)) {   // the post not read yet this run (the skin came from the cache): read it now
    nlohmann::json files;
    if (!post_files(mod_id, &files, reason)) return {};
    if (!seen(&s)) {
      *reason = "mod " + std::to_string(mod_id) + " has no file " + std::to_string(file_id);
      return {};
    }
  }
  *none = s.url.empty();
  *reason = s.reason;
  return s.url;
}

bool valid_stock_png(const std::vector<uint8_t>& bytes, int* width, int* height, std::string* why) {
  *width = *height = 0;
  if (bytes.size() > kMaxStockBytes) { *why = "larger than " + std::to_string(kMaxStockBytes >> 20) + " MB"; return false; }
  if (bytes.size() < 8 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) != 0) { *why = "not a PNG"; return false; }
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n)) { *why = "a PNG this reader cannot read"; return false; }
  if (w <= 0 || h <= 0 || w > kMaxStockSide || h > kMaxStockSide) {
    *why = std::to_string(w) + "x" + std::to_string(h) + " pixels, more than " + std::to_string(kMaxStockSide) + " a side";
    return false;
  }
  stbi_uc* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
  if (!px) { *why = "a PNG that does not decode"; return false; }
  stbi_image_free(px);
  *width = w;
  *height = h;
  return true;
}

StockIcon fetch_stock_icon(uint32_t mod_id, uint32_t file_id, const std::string& sha256) {
  StockIcon r;
  if (!mod_id || !file_id) { r.reason = "no mod or file id"; return r; }
  if (!valid_sha256(sha256)) { r.reason = "not a SHA-256"; return r; }
  const fs::path folder = fs::u8path(cache_folder());
  if (folder.empty()) { r.reason = "no cache folder"; return r; }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path kept = folder / fs::u8path(sha256 + ".stock.png"), none = folder / fs::u8path(sha256 + ".nostock");
  int w = 0, h = 0;
  std::string why;
  if (fs::is_regular_file(kept, ec)) {
    std::vector<uint8_t> bytes;
    if (read_limited(kept, kMaxStockBytes, &bytes) && valid_stock_png(bytes, &w, &h, &why)) {
      r.ok = r.from_cache = true;
      r.path = kept.u8string();
      r.reason = "cached, " + std::to_string(w) + "x" + std::to_string(h);
      return r;
    }
    fs::remove(kept, ec);   // damaged since it was kept: asked for again
  }
  if (fs::is_regular_file(none, ec)) {
    std::vector<uint8_t> text;
    read_limited(none, 4096, &text);
    r.none = r.from_cache = true;
    r.reason = std::string(text.begin(), text.end());
    if (r.reason.empty()) r.reason = "none (cached)";
    return r;
  }
  // The card has none, or the picture is refused: kept as such, beside the skin.
  const auto keep_none = [&](const std::string& reason) {
    std::ofstream out(none, std::ios::binary | std::ios::trunc);
    out << reason;
    r.none = true;
    r.reason = reason;
    return r;
  };
  bool no_icon = false;
  const std::string url = stock_source(mod_id, file_id, &no_icon, &r.reason);
  if (url.empty()) return no_icon ? keep_none(r.reason) : r;
  const fs::path part = folder / fs::u8path(sha256 + ".stock." + std::to_string(++g_part_serial) + ".part");
  std::string error;
  bool too_large = false;
  if (!host::mod_browser::get_file(url, part.u8string(), kMaxStockBytes, &error, &too_large)) {
    fs::remove(part, ec);
    if (too_large) return keep_none("its stock icon is larger than " + std::to_string(kMaxStockBytes >> 20) + " MB");
    r.reason = "its stock icon: " + error;   // a failed request: nothing kept
    return r;
  }
  std::vector<uint8_t> bytes;
  const bool read = read_limited(part, kMaxStockBytes, &bytes);
  if (!read || !valid_stock_png(bytes, &w, &h, &why)) {
    fs::remove(part, ec);
    return keep_none("its stock icon is refused: " + (read ? why : "larger than " + std::to_string(kMaxStockBytes >> 20) + " MB"));
  }
  fs::rename(part, kept, ec);
  if (ec) {
    fs::remove(part, ec);
    if (!fs::is_regular_file(kept, ec)) { r.reason = "its stock icon could not be kept"; return r; }
  }
  r.ok = true;
  r.path = kept.u8string();
  r.reason = "fetched, " + std::to_string(w) + "x" + std::to_string(h);
  return r;
}

}  // namespace host::skins
