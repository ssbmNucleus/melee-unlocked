// The Mods menu's pictures: decoding, fitting, the copies on disk, the picture queue and the viewer's
// pictures (picture(), pack_picture(), prefetch()).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"
#include "mods_menu_stb.h"

namespace host::mods_menu {
namespace menu_internal {

// width x height RGBA, fitted inside kMaxPictureSide with its shape kept, both sides multiples of 4. A
// wide picture (a pack's screenshot) may be up to twice as wide while it has no more pixels than the
// square: 1920 x 1080 becomes 340 x 192, sharper on the wider stand than 256 x 144.
// w x h RGBA made width x height, each new pixel the average of the ones it covers.
std::vector<uint8_t> box_resize(const stbi_uc* px, int w, int h, int width, int height) {
  std::vector<uint8_t> out((size_t)width * height * 4);
  for (int y = 0; y < height; ++y) {
    const int y0 = y * h / height, y1 = std::max(y0 + 1, (y + 1) * h / height);
    for (int x = 0; x < width; ++x) {
      const int x0 = x * w / width, x1 = std::max(x0 + 1, (x + 1) * w / width);
      uint32_t sum[4] = {0, 0, 0, 0}, count = 0;
      for (int sy = y0; sy < y1 && sy < h; ++sy)
        for (int sx = x0; sx < x1 && sx < w; ++sx, ++count)
          for (int c = 0; c < 4; ++c) sum[c] += px[((size_t)sy * w + sx) * 4 + c];
      for (int c = 0; c < 4; ++c) out[((size_t)y * width + x) * 4 + c] = (uint8_t)(count ? sum[c] / count : 0);
    }
  }
  return out;
}

std::vector<uint8_t> fit(const stbi_uc* px, int w, int h, int* out_w, int* out_h) {
  double scale = std::min(1.0, std::min((double)kMaxPictureSide / w, (double)kMaxPictureSide / h));
  if (w > h)
    scale = std::min({1.0, 2.0 * kMaxPictureSide / w, (double)kMaxPictureSide / h,
                      std::sqrt((double)kMaxPictureSide * kMaxPictureSide / ((double)w * h))});
  int width = std::max(4, ((int)(w * scale) + 3) / 4 * 4), height = std::max(4, ((int)(h * scale) + 3) / 4 * 4);
  width = std::min(width, w > h ? 2 * kMaxPictureSide : kMaxPictureSide);
  height = std::min(height, kMaxPictureSide);
  *out_w = width;
  *out_h = height;
  return box_resize(px, w, h, width, height);
}

std::vector<uint8_t> gx_rgba8(const std::vector<uint8_t>& px, int width, int height) {
  std::vector<uint8_t> out;
  out.reserve(px.size());
  for (int by = 0; by < height; by += 4)
    for (int bx = 0; bx < width; bx += 4) {
      for (int i = 0; i < 16; ++i) {
        const uint8_t* p = &px[((size_t)(by + i / 4) * width + bx + i % 4) * 4];
        out.push_back(p[3]); out.push_back(p[0]);
      }
      for (int i = 0; i < 16; ++i) {
        const uint8_t* p = &px[((size_t)(by + i / 4) * width + bx + i % 4) * 4];
        out.push_back(p[1]); out.push_back(p[2]);
      }
    }
  return out;
}

struct Pixels { std::vector<uint8_t> rgba; int w = 0, h = 0; };

// A PNG or JPEG file's bytes as RGBA pixels; false when it is neither or breaks a limit: at most
// `max_bytes` (kMaxPictureBytes; a screenshot kMaxShotBytes), 4096 pixels a side and kMaxPicturePixels
// in all, told by the header before anything is decoded.
bool decode_of(const std::string& bytes, uint64_t max_bytes, Pixels* out) {
  if (bytes.size() < 8 || bytes.size() > max_bytes) return false;
  const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
  const bool png = std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
  const bool jpeg = data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
  if (!png && !jpeg) return false;
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(data, (int)bytes.size(), &w, &h, &n) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  if ((int64_t)w * h > kMaxPicturePixels) return false;
  stbi_uc* px = stbi_load_from_memory(data, (int)bytes.size(), &w, &h, &n, 4);
  if (!px) return false;
  out->rgba.assign(px, px + (size_t)w * h * 4);
  out->w = w;
  out->h = h;
  stbi_image_free(px);
  return true;
}

// Pixels as the picture archive the stand shows, fitted (fit).
std::vector<uint8_t> archive_of_pixels(const Pixels& p) {
  int width = 0, height = 0;
  const std::vector<uint8_t> fitted = fit(p.rgba.data(), p.w, p.h, &width, &height);
  return picture_archive(gx_rgba8(fitted, width, height), width, height);
}

// A PNG or JPEG file's bytes as a picture archive; empty when decode_of refuses it.
std::vector<uint8_t> archive_of(const std::string& bytes, uint64_t max_bytes = kMaxPictureBytes) {
  Pixels p;
  return decode_of(bytes, max_bytes, &p) ? archive_of_pixels(p) : std::vector<uint8_t>{};
}

// A character skin's portrait with its stock icon beside its lower right corner, one picture on the
// stand: the icon about a quarter of the portrait's height, a whole multiple of its own size when it is
// smaller (its pixels stay sharp, as the game's own stock icons are drawn), a small gap between. A
// 136 x 188 portrait with a 24 x 24 icon is 196 x 188, which fit() keeps as it is.
Pixels with_stock(const Pixels& portrait, const Pixels& stock) {
  const int target = std::max(8, portrait.h / 4);
  int sw = 0, sh = 0;
  std::vector<uint8_t> icon;
  if (stock.h <= target) {
    const int k = std::max(1, (target + stock.h / 2) / stock.h);
    sw = stock.w * k;
    sh = stock.h * k;
    icon.resize((size_t)sw * sh * 4);
    for (int y = 0; y < sh; ++y)
      for (int x = 0; x < sw; ++x)
        std::memcpy(&icon[((size_t)y * sw + x) * 4], &stock.rgba[((size_t)(y / k) * stock.w + x / k) * 4], 4);
  } else {
    sh = target;
    sw = std::max(1, (int)((int64_t)stock.w * target / stock.h));
    icon = box_resize(stock.rgba.data(), stock.w, stock.h, sw, sh);
  }
  if (sw > portrait.w) return portrait;   // (an icon wider than the portrait is not a stock icon)
  const int gap = std::max(2, sh / 4);
  Pixels out;
  out.w = portrait.w + gap + sw;
  out.h = std::max(portrait.h, sh);
  out.rgba.assign((size_t)out.w * out.h * 4, 0);
  for (int y = 0; y < portrait.h; ++y)
    std::memcpy(&out.rgba[((size_t)(out.h - portrait.h + y) * out.w) * 4], &portrait.rgba[(size_t)y * portrait.w * 4],
                (size_t)portrait.w * 4);
  for (int y = 0; y < sh; ++y)
    std::memcpy(&out.rgba[((size_t)(out.h - sh + y) * out.w + portrait.w + gap) * 4], &icon[(size_t)y * sw * 4],
                (size_t)sw * 4);
  return out;
}

// Nothing on the stand: a small square, wholly transparent. What the stand shows while a picture is on
// its way (the viewer's spinner says so) and for a card with no picture this reader takes.
std::vector<uint8_t> empty_archive() {
  const int side = 4;
  const std::vector<uint8_t> px((size_t)side * side * 4, 0);
  return picture_archive(gx_rgba8(px, side, side), side, side);
}

// ---- the pictures' copies on disk (kDiskCacheBytes) ----
// One file per address, named by its hash: "MUPIC1", the address, its ETag, when it was read (Unix
// seconds) and the body's size, one a line, then the body. A file whose address or size differs is
// not used. Written whole under another name first, then put in place.

fs::path disk_folder() {
  const fs::path cache = fs::u8path(host::skins::cache_folder());
  return cache.empty() ? fs::path() : cache.parent_path() / L"ModsMenuPictures";
}
std::string disk_name(const std::string& url) {
  uint64_t h = 1469598103934665603ull;   // FNV-1a
  for (const unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
  char name[40];
  std::snprintf(name, sizeof name, "%016llx.pic", (unsigned long long)h);
  return name;
}
struct DiskCopy { std::string etag, bytes; int64_t saved = 0; };
bool disk_read(const std::string& url, uint64_t limit, DiskCopy* out) {
  const fs::path folder = disk_folder();
  if (folder.empty()) return false;
  const fs::path path = folder / disk_name(url);
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > limit + 4096 + url.size()) return false;
  std::ifstream in(path, std::ios::binary);
  std::string magic, address, etag, saved, length;
  if (!in || !std::getline(in, magic) || magic != "MUPIC1" || !std::getline(in, address) || address != url ||
      !std::getline(in, etag) || !std::getline(in, saved) || !std::getline(in, length))
    return false;
  char* end = nullptr;
  const unsigned long long n = std::strtoull(length.c_str(), &end, 10);
  if (length.empty() || *end || n == 0 || n > limit) return false;
  std::string bytes(n, '\0');
  if (!in.read(bytes.data(), (std::streamsize)n) || in.peek() != EOF) return false;
  out->etag = etag;
  out->saved = std::strtoll(saved.c_str(), nullptr, 10);
  out->bytes = std::move(bytes);
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);   // (used: the last to go)
  return true;
}
// The folder kept under kDiskCacheBytes: the least recently used files go. Locked (g_disk_mutex).
void disk_trim_locked(const fs::path& folder) {
  std::error_code ec;
  if (!g_disk_counted) {
    g_disk_counted = true;
    g_disk_bytes = 0;
    for (const auto& e : fs::directory_iterator(folder, ec))
      if (e.is_regular_file(ec)) g_disk_bytes += e.file_size(ec);
  }
  if (g_disk_bytes <= kDiskCacheBytes) return;
  std::vector<std::pair<fs::file_time_type, fs::path>> files;
  uint64_t total = 0;
  for (const auto& e : fs::directory_iterator(folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    total += e.file_size(ec);
    files.emplace_back(e.last_write_time(ec), e.path());
  }
  std::sort(files.begin(), files.end());
  size_t gone = 0;
  for (const auto& [when, path] : files) {
    if (total <= kDiskCacheBytes * 3 / 4) break;
    const uint64_t size = fs::file_size(path, ec);
    if (fs::remove(path, ec)) { total -= std::min(total, size); ++gone; }
  }
  g_disk_bytes = total;
  host::log("mods menu: picture copies on disk over %llu MB: %zu least recently used removed, %llu KB kept",
            (unsigned long long)(kDiskCacheBytes >> 20), gone, (unsigned long long)(total >> 10));
}
void disk_write(const std::string& url, const std::string& etag, const std::string& bytes) {
  const fs::path folder = disk_folder();
  if (folder.empty() || bytes.empty() || url.find('\n') != std::string::npos || etag.find('\n') != std::string::npos) return;
  std::lock_guard<std::mutex> lock(g_disk_mutex);
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path path = folder / disk_name(url), part = path.string() + ".part";
  const uint64_t before = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    const long long now = (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    out << "MUPIC1\n" << url << "\n" << etag << "\n" << now << "\n" << bytes.size() << "\n";
    out.write(bytes.data(), (std::streamsize)bytes.size());
    if (!out) { out.close(); fs::remove(part, ec); return; }
  }
  fs::rename(part, path, ec);
  if (ec) { fs::remove(part, ec); return; }
  const uint64_t after = fs::file_size(path, ec);
  if (g_disk_counted) g_disk_bytes = g_disk_bytes - std::min(g_disk_bytes, before) + after;
  disk_trim_locked(folder);
}

// A picture file by its address: the copy on disk while it is fresh (kDiskFresh), else from the site
// with the copy's ETag (a 304 keeps the copy); a copy is also what the player gets when the site does
// not answer. `*where` says which, for the log.
bool cached_get(const std::string& url, uint64_t limit, std::string* body, host::mod_browser::NetResult* net,
                const char** where) {
  DiskCopy copy;
  const bool have = disk_read(url, limit, &copy);
  const long long now = (long long)std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (have && now - copy.saved >= 0 && now - copy.saved < (long long)std::chrono::seconds(kDiskFresh).count()) {
    *body = std::move(copy.bytes);
    *where = "disk";
    return true;
  }
  std::string error, etag;
  bool not_modified = false;
  if (host::mod_browser::get_picture(url, have ? copy.etag : std::string(), body, limit, &error, &not_modified, &etag, net)) {
    if (not_modified && have) {
      *body = std::move(copy.bytes);
      disk_write(url, copy.etag, *body);
      *where = "the site (not changed: the copy on disk)";
      return true;
    }
    if (!body->empty()) {
      disk_write(url, etag, *body);
      *where = "the site";
      return true;
    }
    return false;
  }
  if (have) {
    *body = std::move(copy.bytes);
    *where = "disk (the site did not answer)";
    return true;
  }
  return false;
}

// A plain tile (dark, a lighter edge), the shape of a select screen portrait, small: it is stretched.
// (No longer shown: the stand stays empty instead, empty_archive.)
[[maybe_unused]] std::vector<uint8_t> placeholder_archive() {
  const int width = 64, height = 88;
  std::vector<uint8_t> px((size_t)width * height * 4);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const bool edge = x < 2 || y < 2 || x >= width - 2 || y >= height - 2;
      uint8_t* p = &px[((size_t)y * width + x) * 4];
      p[0] = edge ? 150 : 40; p[1] = edge ? 150 : 44; p[2] = edge ? 165 : 60; p[3] = 255;
    }
  return picture_archive(gx_rgba8(px, width, height), width, height);
}

// `known`: the card's post when its row knew the post's screenshots (/mods), so a stage's or an
// effect's needs no request to the API for them. `prefetch`: asked for ahead of the viewer, which never
// spends a request to the API (its 30 a minute are kept for the lists): a card that would need one is
// left for the viewer to ask for, unless the viewer asked for it meanwhile.
void fetch_picture(const Card& card, const Post* known, bool prefetch) {
  using clock = std::chrono::steady_clock;
  const auto started = clock::now();
  double net_ms = 0, decode_ms = 0, waited_ms = -1;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_pictures.find(card_key(card));
    if (it != g_pictures.end() && it->second.asked.time_since_epoch().count() != 0)
      waited_ms = std::chrono::duration<double, std::milli>(started - it->second.asked).count();
  }
  std::vector<uint8_t> archive;
  Pixels shown;   // the picture, decoded (made the archive at the end)
  bool network_failed = false;
  const auto read_local = [](const std::string& path, Pixels* out) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_of(bytes, kMaxPictureBytes, out);
  };
  if (!card.local_picture.empty() && !read_local(card.local_picture, &shown)) shown = Pixels{};
  std::string from;
  const char* where = "";
  // A screenshot (a stage's or an effect's) may be larger than a portrait (kMaxShotBytes).
  const auto try_urls = [&](const std::vector<std::string>& urls, uint64_t limit) {
    for (const auto& url : urls) {
      if (!shown.rgba.empty() || g_blocked.load()) break;
      std::string body;
      host::mod_browser::NetResult net;
      const auto asked = clock::now();
      const bool got = cached_get(url, limit, &body, &net, &where);
      net_ms += std::chrono::duration<double, std::milli>(clock::now() - asked).count();
      if (!got) {
        if (net.status == 0 || net.status == 429 || net.status >= 500) network_failed = true;
        continue;
      }
      const auto decoding = clock::now();
      if (!decode_of(body, limit, &shown)) shown = Pixels{};
      decode_ms += std::chrono::duration<double, std::milli>(clock::now() - decoding).count();
      if (shown.rgba.empty()) host::log("mods menu: card %d: a picture that could not be read (%zu bytes)", card.file_id, body.size());
      else from = url;
    }
  };
  // Manage (an installed skin's row): only what was kept with the skin, never the site.
  const bool installed_row = !card.asset_id.empty();
  const bool screenshots = card.kind == "stage_visual" || card.kind == "effect_visual" || card.kind == "fighter";
  if (!installed_row) try_urls(card.pictures, screenshots ? kMaxShotBytes : kMaxPictureBytes);
  // Nothing this reader takes for a stage card (it has no portrait, and its thumbnail is WebP): the
  // mod's screenshots, which its row's post has when its list came from /mods, else /mods/{id} lists
  // full size. A character card never shows a screenshot.
  const bool known_shots = known && known->shots_known && known->mod_id == card.mod_id;
  if (shown.rgba.empty() && !installed_row && screenshots && known_shots) try_urls(known->shots, kMaxShotBytes);
  if (shown.rgba.empty() && !installed_row && screenshots && !known_shots && prefetch && card.mod_id > 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_pictures.find(card_key(card));
    if (it != g_pictures.end() && it->second.asked.time_since_epoch().count() == 0) {
      g_pictures.erase(it);   // (the viewer's ask fetches it, with the API's list of screenshots)
      host::log("mods menu: card %d of mod %d: picture left for the viewer (its screenshots need /mods/{id})",
                card.file_id, card.mod_id);
      return;
    }
  }
  if (shown.rgba.empty() && !installed_row && screenshots && !known_shots &&
      card.mod_id > 0 && !g_blocked.load() && !network_failed) {
    json j;
    std::string error;
    int status = 0;
    if (get_json(host::mod_browser::api_address() + "/mods/" + std::to_string(card.mod_id), &j, &status, &error) &&
        j.is_object() && j.contains("data") && j["data"].is_object()) {
      Card shots;
      if (const auto list = j["data"].find("screenshots"); list != j["data"].end() && list->is_array())
        for (const auto& s : *list)
          if (s.is_string() && shots.pictures.size() < 3) add_picture(shots, s.get<std::string>());
      try_urls(shots.pictures, kMaxShotBytes);
    }
  }
  // A character skin's stock icon beside its portrait (with_stock): Browse, the card's stock_url (one
  // more picture file, through the same reader and copies on disk) unless the stock icon is the
  // picture itself; Manage, the icon kept with the skin. One the reader does not take (WebP), or that
  // does not come, leaves the portrait alone.
  bool stocked = false;
  if (!shown.rgba.empty() && card.kind == "character_costume" && !g_blocked.load()) {
    Pixels stock;
    bool got_stock = false;
    if (installed_row) {
      got_stock = !card.local_stock.empty() && read_local(card.local_stock, &stock);
    } else if (!card.stock_url.empty() && card.stock_url != from) {
      std::string body;
      host::mod_browser::NetResult net;
      const char* stock_where = "";
      const auto asked = clock::now();
      got_stock = cached_get(card.stock_url, kMaxPictureBytes, &body, &net, &stock_where) &&
                  decode_of(body, kMaxPictureBytes, &stock);
      net_ms += std::chrono::duration<double, std::milli>(clock::now() - asked).count();
    }
    if (got_stock && stock.w > 0 && stock.h > 0) {
      shown = with_stock(shown, stock);
      stocked = true;
    }
  }
  if (!shown.rgba.empty()) archive = archive_of_pixels(shown);
  const bool found = !archive.empty();
  char timing[128];
  std::snprintf(timing, sizeof timing, " (%s%d ms in line, network %d ms, decode %d ms)", waited_ms < 0 ? "prefetch, " : "",
                (int)std::max(0.0, waited_ms), (int)net_ms, (int)decode_ms);
  host::log("mods menu: card %d of mod %d: picture %s%s%s%s%s", card.file_id, card.mod_id,
            found ? (card.local_picture.empty() ? ("from " + log_host(from)).c_str() : "from the installed skin")
                  : network_failed ? "not fetched (the site did not answer; asked again later)"
                                   : "none the reader takes (the stand stays empty)",
            found && card.local_picture.empty() ? ", read from " : "", found && card.local_picture.empty() ? where : "",
            stocked ? ", its stock icon beside it" : "", timing);
  if (!found) archive = empty_archive();
  std::lock_guard<std::mutex> lock(g_mutex);
  Picture& p = g_pictures[card_key(card)];
  p.status = found ? kReady : kError;
  p.archive = std::move(archive);
  p.generation = ++g_picture_generation;
  p.used = std::chrono::steady_clock::now();
  p.retry = network_failed;
  p.fetching = false;
  while (g_pictures.size() > kPicturesKept) {
    auto oldest = std::min_element(g_pictures.begin(), g_pictures.end(),
                                   [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    if (oldest->second.status == kLoading) break;
    g_pictures.erase(oldest);
  }
}

// A pack's own page: the post's own picture, its first screenshot this reader takes (a pack of a
// fighter's list, read from /cards, learns its screenshots from /mods/{id} first: one API request,
// once), else its first card's portrait (or what else the card has). The screenshot may be larger
// (kMaxShotBytes) and wider (picture_archive) than a portrait; both are made small here.
void fetch_post_picture(const Job& job) {
  using clock = std::chrono::steady_clock;
  const auto started = clock::now();
  const Post& post = job.post;
  std::vector<std::string> shots = post.shots;
  bool network_failed = false, asked_api = false;
  double waited_ms = -1, net_ms = 0, decode_ms = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_pictures.find(job.key);
    if (it != g_pictures.end() && it->second.asked.time_since_epoch().count() != 0)
      waited_ms = std::chrono::duration<double, std::milli>(started - it->second.asked).count();
  }
  if (!post.shots_known && post.mod_id > 0 && !g_blocked.load()) {
    json j;
    std::string error;
    int status = 0;
    asked_api = true;
    const auto asked = clock::now();
    if (get_json(host::mod_browser::api_address() + "/mods/" + std::to_string(post.mod_id), &j, &status, &error) &&
        j.is_object() && j.contains("data") && j["data"].is_object()) {
      Post words;
      post_words_of(j["data"], words);
      shots = words.shots;
      std::lock_guard<std::mutex> lock(g_mutex);
      learn_post_words_locked(post.mod_id, words);
    } else if (status == 0 || status == 429 || status >= 500) {
      network_failed = true;
    }
    net_ms += std::chrono::duration<double, std::milli>(clock::now() - asked).count();
  }
  std::vector<uint8_t> archive;
  std::string from;
  const char* where = "";
  const char* which = "";
  const auto try_urls = [&](const std::vector<std::string>& urls, uint64_t limit, const char* what) {
    for (const auto& url : urls) {
      if (!archive.empty() || g_blocked.load()) break;
      std::string body;
      host::mod_browser::NetResult net;
      const auto asked = clock::now();
      const bool got = cached_get(url, limit, &body, &net, &where);
      net_ms += std::chrono::duration<double, std::milli>(clock::now() - asked).count();
      if (!got) {
        if (net.status == 0 || net.status == 429 || net.status >= 500) network_failed = true;
        continue;
      }
      const auto decoding = clock::now();
      archive = archive_of(body, limit);
      decode_ms += std::chrono::duration<double, std::milli>(clock::now() - decoding).count();
      if (archive.empty()) host::log("mods menu: pack %d: a picture that could not be read (%zu bytes)", post.mod_id, body.size());
      else { from = url; which = what; }
    }
  };
  try_urls(shots, kMaxShotBytes, "its first screenshot");
  if (!post.cards.empty()) try_urls(post.cards.front().pictures, kMaxPictureBytes, "its first card's picture");
  const bool found = !archive.empty();
  host::log("mods menu: pack %d: picture %s%s%s%s (%s%d ms in line, network %d ms, decode %d ms%s)", post.mod_id,
            found ? which : network_failed ? "not fetched (the site did not answer; asked again later)"
                                           : "none the reader takes (the stand stays empty)",
            found ? " from " : "", found ? log_host(from).c_str() : "", found ? (std::string(", read from ") + where).c_str() : "",
            waited_ms < 0 ? "prefetch, " : "", (int)std::max(0.0, waited_ms), (int)net_ms, (int)decode_ms,
            asked_api ? ", its screenshots from /mods/{id}" : "");
  if (!found) archive = empty_archive();
  std::lock_guard<std::mutex> lock(g_mutex);
  Picture& p = g_pictures[job.key];
  p.status = found ? kReady : kError;
  p.archive = std::move(archive);
  p.generation = ++g_picture_generation;
  p.used = clock::now();
  p.retry = network_failed;
  p.fetching = false;
  while (g_pictures.size() > kPicturesKept) {
    auto oldest = std::min_element(g_pictures.begin(), g_pictures.end(),
                                   [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    if (oldest->second.status == kLoading) break;
    g_pictures.erase(oldest);
  }
}

// A picture job in line. The viewer's go first (the newest first: the one on screen), those asked for
// ahead of the viewer (prefetch) after them, in the order asked. A picture is in line once: the
// viewer's ask moves it to the front. Past the few kept waiting of each kind (kPictureQueue,
// kPrefetchQueue) the oldest is dropped, and asked for again if its row is seen again.
void push_picture_locked(Job job) {
  start_locked(g_picture_queue);
  auto& jobs = g_picture_queue.jobs;
  for (auto it = jobs.begin(); it != jobs.end(); ++it)
    if (it->kind == Job::kPicture && it->key == job.key) {
      if (job.prefetch) return;   // in line already
      jobs.erase(it);
      break;
    }
  if (job.prefetch) jobs.push_back(std::move(job));
  else jobs.push_front(std::move(job));
  const auto drop = [&](bool prefetched) {
    for (auto it = jobs.end(); it != jobs.begin();) {
      --it;
      if (it->prefetch != prefetched) continue;
      const auto pic = g_pictures.find(it->key);
      if (pic != g_pictures.end() && pic->second.status == kLoading && !pic->second.fetching) g_pictures.erase(pic);
      jobs.erase(it);
      return;
    }
  };
  for (;;) {
    size_t views = 0, ahead = 0;
    for (const auto& j : jobs) ++(j.prefetch ? ahead : views);
    if (views > kPictureQueue) drop(false);
    else if (ahead > kPrefetchQueue) drop(true);
    else break;
  }
  g_picture_queue.wake.notify_all();
}

// A picture asked for: first in line (the newest is the one on screen). Past the few kept waiting,
// the oldest is dropped, and asked for again if its row is seen again.
[[maybe_unused]] void queue_picture_locked(const Card& card) {
  Job job;
  job.kind = Job::kPicture;
  job.card = card;
  job.key = card_key(card);
  push_picture_locked(std::move(job));
}

// A pack's own page (its row in the posts' list): its picture's key, by list and post (its first card
// is the list's fighter's).
std::string post_picture_key(const Post& post) { return "p" + pack_key(g_category, g_filter, post.mod_id); }

// The picture of a row (a card, or a pack's own page): asked for when not held, `prefetch` ahead of the
// viewer (behind what the viewer asks for). Null for a row with no picture, and for a pack of a
// fighter's list fetched ahead (its picture needs the API: only for the page the player opens). Locked.
Picture* want_row_picture_locked(int kind, const Card* card, const Post* post, bool prefetch) {
  Job job;
  job.kind = Job::kPicture;
  job.prefetch = prefetch;
  if (kind == 0 && card) {
    job.card = *card;
    job.key = card_key(*card);
    // A stage's or an effect's card: its post's screenshots, when its row knew them (fetch_picture).
    if (post && post->shots_known && post->mod_id == card->mod_id) {
      job.post.mod_id = post->mod_id;
      job.post.shots = post->shots;
      job.post.shots_known = true;
    }
  } else if (kind == 1 && post && post->mod_id > 0) {
    if (prefetch && !post->shots_known) return nullptr;
    job.of_post = true;
    job.key = post_picture_key(*post);
    // What its picture needs: its screenshots (when known) and its first card's pictures.
    job.post.mod_id = post->mod_id;
    job.post.title = post->title;
    job.post.shots = post->shots;
    job.post.shots_known = post->shots_known;
    if (!post->cards.empty()) job.post.cards.push_back(post->cards.front());
  } else {
    return nullptr;
  }
  const auto now = std::chrono::steady_clock::now();
  const std::string key = job.key;
  auto pic = g_pictures.find(key);
  if (pic != g_pictures.end() && pic->second.status == kError && pic->second.retry &&
      now - pic->second.used > std::chrono::seconds(30)) {
    g_pictures.erase(pic);   // the site did not answer then: asked again
    pic = g_pictures.end();
  }
  if (pic == g_pictures.end()) {
    pic = g_pictures.emplace(key, Picture{}).first;
    if (!prefetch) pic->second.asked = now;
    pic->second.used = now;
    push_picture_locked(std::move(job));
  } else if (pic->second.status == kLoading && !pic->second.fetching) {
    if (!prefetch && pic->second.asked.time_since_epoch().count() == 0) pic->second.asked = now;
    const auto& jobs = g_picture_queue.jobs;
    const bool front = !jobs.empty() && jobs.front().key == key && !jobs.front().prefetch;
    // In line behind others, or dropped from it. (A prefetch already in line stays where it is.)
    if (!front) push_picture_locked(std::move(job));
  }
  // A full line may have dropped this very one (a prefetch past kPrefetchQueue): found again.
  pic = g_pictures.find(key);
  if (pic == g_pictures.end()) return nullptr;
  pic->second.used = now;
  return &pic->second;
}

}  // namespace menu_internal

std::vector<uint8_t> picture_archive(const std::vector<uint8_t>& gx_pixels, int width, int height) {
  // Data section layout (offsets from its start), every number big-endian as on the disc.
  enum : uint32_t {
    kJoint = 0x000, kDObj = 0x040, kMObj = 0x050, kMaterial = 0x068, kTObj = 0x080, kImage = 0x0E0,
    kPObj = 0x100, kVtx = 0x118, kPositions = 0x160, kTexCoords = 0x190, kDisplay = 0x1C0, kPixels = 0x200,
  };
  if (width <= 0 || height <= 0 || width % 4 || height % 4 || gx_pixels.size() != (size_t)width * height * 4) return {};
  std::vector<uint8_t> d(kPixels + gx_pixels.size(), 0);
  std::vector<uint32_t> relocs;
  const auto u32 = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + i] = (uint8_t)(v >> (24 - 8 * i)); };
  const auto u16 = [&](uint32_t at, uint32_t v) { d[at] = (uint8_t)(v >> 8); d[at + 1] = (uint8_t)v; };
  const auto f32 = [&](uint32_t at, float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(at, b); };
  const auto ptr = [&](uint32_t at, uint32_t target) { u32(at, target); relocs.push_back(at); };
  // Joint: drawn translucent (JOBJ_XLU and its root flag), one DObj, scale 1.
  u32(kJoint + 0x04, (1u << 19) | (1u << 29));
  ptr(kJoint + 0x10, kDObj);
  for (int i = 0; i < 3; ++i) f32(kJoint + 0x20 + 4 * i, 1.0f);
  ptr(kDObj + 0x08, kMObj);
  ptr(kDObj + 0x0C, kPObj);
  // Material: constant colour, one texture, translucent without writing depth (as the menus' labels).
  u32(kMObj + 0x04, (1u << 0) | (1u << 4) | (1u << 29) | (1u << 30));
  ptr(kMObj + 0x08, kTObj);
  ptr(kMObj + 0x0C, kMaterial);
  u32(kMaterial + 0x00, 0x808080FFu);
  u32(kMaterial + 0x04, 0xFFFFFFFFu);
  u32(kMaterial + 0x08, 0xFFFFFFFFu);
  f32(kMaterial + 0x0C, 1.0f);
  f32(kMaterial + 0x10, 50.0f);
  // Texture: map 0 from texture coordinates 0, clamped, colour and alpha modulated, linear filtering.
  u32(kTObj + 0x08, 0);
  u32(kTObj + 0x0C, 4);
  for (int i = 0; i < 3; ++i) f32(kTObj + 0x1C + 4 * i, 1.0f);
  d[kTObj + 0x3C] = 1;
  d[kTObj + 0x3D] = 1;
  u32(kTObj + 0x40, 0x340010u);
  f32(kTObj + 0x44, 1.0f);
  u32(kTObj + 0x48, 1);
  ptr(kTObj + 0x4C, kImage);
  ptr(kImage + 0x00, kPixels);
  u16(kImage + 0x04, (uint32_t)width);
  u16(kImage + 0x06, (uint32_t)height);
  u32(kImage + 0x08, 6);   // GX_TF_RGBA8
  // One square: positions and texture coordinates by index, one display list block.
  ptr(kPObj + 0x08, kVtx);
  u16(kPObj + 0x0E, 1);
  ptr(kPObj + 0x10, kDisplay);
  u32(kVtx + 0x00, 9);  u32(kVtx + 0x04, 2); u32(kVtx + 0x08, 1); u32(kVtx + 0x0C, 4); u16(kVtx + 0x12, 12);
  ptr(kVtx + 0x14, kPositions);
  u32(kVtx + 0x18, 13); u32(kVtx + 0x1C, 2); u32(kVtx + 0x20, 1); u32(kVtx + 0x24, 4); u16(kVtx + 0x2A, 8);
  ptr(kVtx + 0x2C, kTexCoords);
  u32(kVtx + 0x30, 0xFF);
  // One unit tall and as wide as the picture's shape; a picture wider than kMaxPictureAspect is made
  // that wide and less tall instead, so all of it stays on the stand, clear of the words' panel. (1.6:
  // a pack's screenshot, landscape, as wide as the stand allows.)
  constexpr float kMaxPictureAspect = 1.6f;
  float half = (float)width / (float)height / 2.0f, top = 1.0f;
  if (half * 2.0f > kMaxPictureAspect) {
    top = kMaxPictureAspect * (float)height / (float)width;
    half = kMaxPictureAspect / 2.0f;
  }
  const float pos[4][3] = {{-half, 0, 0}, {half, 0, 0}, {half, top, 0}, {-half, top, 0}};
  const float tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  for (int v = 0; v < 4; ++v) {
    for (int k = 0; k < 3; ++k) f32(kPositions + 12 * v + 4 * k, pos[v][k]);
    for (int k = 0; k < 2; ++k) f32(kTexCoords + 8 * v + 4 * k, tex[v][k]);
  }
  d[kDisplay] = 0x80;   // GX_QUADS, vertex format 0
  u16(kDisplay + 1, 4);
  for (int v = 0; v < 4; ++v) { d[kDisplay + 3 + 2 * v] = (uint8_t)v; d[kDisplay + 4 + 2 * v] = (uint8_t)v; }
  std::memcpy(d.data() + kPixels, gx_pixels.data(), gx_pixels.size());
  // The archive: header, data, relocations, one public symbol, its name.
  static const char kName[] = "mu_mods_picture_joint";
  std::vector<uint8_t> out(0x20, 0);
  out.insert(out.end(), d.begin(), d.end());
  for (const uint32_t at : relocs) for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(at >> (24 - 8 * i)));
  for (int i = 0; i < 8; ++i) out.push_back(0);   // symbol: data offset 0, name offset 0
  out.insert(out.end(), kName, kName + sizeof kName);
  while (out.size() % 32) out.push_back(0);
  const auto head = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = (uint8_t)(v >> (24 - 8 * i)); };
  head(0x00, (uint32_t)out.size());
  head(0x04, (uint32_t)d.size());
  head(0x08, (uint32_t)relocs.size());
  head(0x0C, 1);
  head(0x10, 0);
  return out;
}

namespace menu_internal {
// A held picture into picture()'s reply, its first hand-over since its row was opened logged: how long
// after the opening (and whether it was fetched ahead of it).
void hand_over_locked(Picture& p, const std::string& what, std::vector<uint8_t>& reply) {
  if (!p.shown_logged) {
    p.shown_logged = true;
    const auto now = std::chrono::steady_clock::now();
    const auto ms = [&](std::chrono::steady_clock::time_point t) {
      return t.time_since_epoch().count() == 0 ? -1 : (int)std::chrono::duration<double, std::milli>(now - t).count();
    };
    const int opened = ms(p.viewed), asked = ms(p.asked);
    host::log("mods menu: %s: picture handed to the viewer %s%s (%s, %zu bytes)", what.c_str(),
              opened < 0 ? "" : std::to_string(opened).c_str(), opened < 0 ? "(not opened)" : " ms after its row was opened",
              asked < 0 ? "fetched ahead of the viewer" : ("first asked for " + std::to_string(asked) + " ms before").c_str(),
              p.archive.size());
  }
  reply.push_back(1);
  put_u32(reply, p.generation);
  put_u32(reply, (uint32_t)p.archive.size());
  reply.insert(reply.end(), p.archive.begin(), p.archive.end());
}
}  // namespace menu_internal

void pack_picture(int row, std::vector<uint8_t>& reply) {
  reply.clear();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_picking) { reply.push_back(kLoading); return; }
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  const Post* post = row >= 0 && row < (int)rows.size() && rows[(size_t)row].kind == kRowPack ? rows[(size_t)row].post : nullptr;
  const auto pic = post ? g_pictures.find(post_picture_key(*post)) : g_pictures.end();
  if (pic == g_pictures.end() || pic->second.status == kLoading || pic->second.archive.empty()) {
    reply.push_back(kLoading);
    return;
  }
  hand_over_locked(pic->second, "pack " + std::to_string(post->mod_id), reply);
}

void prefetch(int row, int line) {
  std::lock_guard<std::mutex> lock(g_mutex);
  prefetch_screen_locked(row, line);
}

void picture(int row, std::vector<uint8_t>& reply) {
  reply.clear();
  if (row == 0xFFFF) {
    // The stand while a picture is on its way and for rows that are not cards: nothing (the viewer's
    // spinner says a picture is coming).
    static const std::vector<uint8_t> tile = empty_archive();
    reply.push_back(1);
    put_u32(reply, 0);
    put_u32(reply, (uint32_t)tile.size());
    reply.insert(reply.end(), tile.begin(), tile.end());
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_picking) { reply.push_back(kLoading); return; }
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  const Card* c = row >= 0 && row < (int)rows.size() && rows[(size_t)row].kind == kRowCard ? rows[(size_t)row].card : nullptr;
  const auto pic = c ? g_pictures.find(card_key(*c)) : g_pictures.end();
  if (pic == g_pictures.end() || pic->second.status == kLoading || pic->second.archive.empty()) {
    reply.push_back(kLoading);
    return;
  }
  hand_over_locked(pic->second, "card " + std::to_string(c->file_id), reply);
}

}  // namespace host::mods_menu
