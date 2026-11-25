// New stages (new_stages.h): which stages they play over, the checks, the store beside the settings, icons.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "new_stages.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

#include "host.h"
#include "http_client.h"
#include "menu_mods.h"
#include "mod_data_safety.h"
#include "stage_dat_safety.h"

#include "nlohmann/json.hpp"

// The icons are made from PNG pictures: stb_image's reader, kept private to this file. It refuses a
// picture wider or taller than 4096 pixels.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace host::new_stages {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// The stages a new stage may play over. Each retail versus stage but Pokémon Stadium (its four
// transformations are files of their own, swapped in by its code), and the Target Test stages that
// host a four-player versus match as they are: four starting points, the camera's limits and blast
// zones in place, tested with a custom file over each (2026-11-24). The others were not tested.
struct Base { const char* file; const char* name; bool target; };
constexpr Base kBases[] = {
    {"GrNBa.dat", "Battlefield", false},
    {"GrNLa.dat", "Final Destination", false},
    {"GrOp.dat", "Dream Land", false},
    {"GrIz.dat", "Fountain of Dreams", false},
    {"GrSt.dat", "Yoshi's Story", false},
    {"GrPu.dat", "Poke Floats", false},
    {"GrBb.dat", "Big Blue", false},
    {"GrCn.dat", "Corneria", false},
    {"GrCs.dat", "Princess Peach's Castle", false},
    {"GrFs.dat", "Fourside", false},
    {"GrFz.dat", "Flat Zone", false},
    {"GrGb.dat", "Great Bay", false},
    {"GrGd.dat", "Jungle Japes", false},
    {"GrGr.dat", "Green Greens", false},
    {"GrI1.dat", "Mushroom Kingdom", false},
    {"GrI2.dat", "Mushroom Kingdom II", false},
    {"GrIm.dat", "Icicle Mountain", false},
    {"GrKg.dat", "Kongo Jungle", false},
    {"GrKr.dat", "Brinstar Depths", false},
    {"GrMc.dat", "Mute City", false},
    {"GrOk.dat", "Kongo Jungle 64", false},
    {"GrOt.dat", "Onett", false},
    {"GrOy.dat", "Yoshi's Island 64", false},
    {"GrRc.dat", "Rainbow Cruise", false},
    {"GrSh.dat", "Temple", false},
    {"GrVe.dat", "Venom", false},
    {"GrYt.dat", "Yoshi's Island", false},
    {"GrZe.dat", "Brinstar", false},
    {"GrTCa.dat", "C. Falcon's Target Test", true},
    {"GrTSk.dat", "Sheik's Target Test", true},
    {"GrTFe.dat", "Roy's Target Test", true},
    {"GrTKp.dat", "Bowser's Target Test", true},
    {"GrTYs.dat", "Yoshi's Target Test", true},
    {"GrTZd.dat", "Zelda's Target Test", true},
    {"GrTDr.dat", "Dr. Mario's Target Test", true},
    {"GrTKb.dat", "Kirby's Target Test", true},
};

// The points a versus match needs (map_head's general points, their ids): the four starting points,
// the camera's limits and the blast zones (gr/ground.c Ground_GetPosition kinds).
constexpr int kNeededPoints[] = {0, 1, 2, 3, 148, 149, 150, 151};

std::mutex g_mutex;
fs::path g_root;
bool g_configured = false;
bool g_random = false;
std::vector<Installed> g_installed;
std::set<std::string> g_logged;   // "served" lines said once per stage and outcome

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

std::string file_part(const std::string& path) {
  const size_t at = path.find_last_of("/\\");
  return at == std::string::npos ? path : path.substr(at + 1);
}

const Base* find_base(const std::string& file) {
  const std::string want = lower(file);
  for (const auto& b : kBases)
    if (lower(b.file) == want) return &b;
  return nullptr;
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out, uint64_t limit) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->resize((size_t)size);
  in.read((char*)out->data(), (std::streamsize)size);
  return (uint64_t)in.gcount() == size;
}

bool write_atomic(const fs::path& path, const void* data, size_t size, std::string* error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path staged = path.parent_path() / (path.filename().wstring() + L".new");
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out.write((const char*)data, (std::streamsize)size);
    if (!out) {
      fs::remove(staged, ec);
      if (error) *error = "Could not write the stage file.";
      return false;
    }
  }
  fs::rename(staged, path, ec);
  if (ec) {
    fs::remove(staged, ec);
    if (error) *error = "Could not keep the stage file.";
    return false;
  }
  return true;
}

// The collision's line and group counts (coll_data +0x0C, +0x28); false when they cannot be read.
bool collision_counts(const cosmetics::stage_safety::Archive& a, uint32_t* lines, uint32_t* groups) {
  const auto it = a.roots.find("coll_data");
  if (it == a.roots.end() || !a.bounds(it->second, 0x2C)) return false;
  *lines = a.get(it->second + 0x0C);
  *groups = a.get(it->second + 0x28);
  return true;
}

// The ids of map_head's general points: a list of 12-byte entries (joint, pairs, count) whose pairs
// are (joint index, id) halfwords. An entry whose pairs do not lie in the file is passed over.
std::set<int> point_ids(const cosmetics::data_safety::Archive& a) {
  std::set<int> ids;
  uint32_t head = 0, list = 0;
  if (!a.root("map_head", &head) || !a.bounds(head, 8) || !a.pointer(head, &list)) return ids;
  const uint32_t count = a.word(head + 4);
  if (count > 4096 || !a.bounds(list, uint64_t(count) * 12)) return ids;
  for (uint32_t e = 0; e < count; ++e) {
    uint32_t pairs = 0;
    const uint32_t n = a.word(list + e * 12 + 8);
    if (!a.pointer(list + e * 12 + 4, &pairs) || n > 4096 || !a.bounds(pairs, uint64_t(n) * 4)) continue;
    for (uint32_t k = 0; k < n; ++k) ids.insert((int)(int16_t)(uint16_t)(a.word(pairs + k * 4) & 0xFFFF));
  }
  return ids;
}

json to_json(const Installed& m) {
  return json{{"id", m.id},         {"base", m.base},     {"name", m.name},       {"creator", m.creator},
              {"author", m.author}, {"description", m.description}, {"file_name", m.file_name},
              {"mod_id", m.mod_id}, {"file_id", m.file_id}, {"downloads", m.downloads}, {"size", m.size},
              {"sha256", m.sha256}};
}

std::string text_of(const json& j, const char* key, size_t max) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  std::string s = it->get<std::string>();
  return s.size() > max ? s.substr(0, max) : s;
}

int int_of(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number_integer() ? (int)std::max<int64_t>(0, std::min<int64_t>(it->get<int64_t>(), 2000000000)) : 0;
}

bool id_ok(const std::string& id) {
  if (id.size() < 2 || id.size() > 12 || id[0] != 's') return false;
  return std::all_of(id.begin() + 1, id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

void load_locked() {
  g_installed.clear();
  g_random = false;
  std::ifstream in(g_root / L"installed.json", std::ios::binary);
  if (!in) return;
  json doc = json::parse(in, nullptr, false);
  if (!doc.is_object() || !doc.contains("stages") || !doc["stages"].is_array()) {
    host::log("new stages: installed.json is not readable; no new stage this run");
    return;
  }
  if (const auto r = doc.find("random"); r != doc.end() && r->is_boolean()) g_random = r->get<bool>();
  for (const auto& j : doc["stages"]) {
    if (!j.is_object() || g_installed.size() >= kMaxInstalled) continue;
    Installed m;
    m.id = text_of(j, "id", 16);
    const Base* base = find_base(text_of(j, "base", 32));
    if (!id_ok(m.id) || !base) continue;
    m.base = base->file;
    m.name = text_of(j, "name", 120);
    m.creator = text_of(j, "creator", 100);
    m.author = text_of(j, "author", 100);
    m.description = text_of(j, "description", 500);
    m.file_name = text_of(j, "file_name", 260);
    m.mod_id = int_of(j, "mod_id");
    m.file_id = int_of(j, "file_id");
    m.downloads = int_of(j, "downloads");
    m.size = (uint64_t)int_of(j, "size");
    m.sha256 = text_of(j, "sha256", 64);
    std::error_code ec;
    const fs::path picture = g_root / fs::u8path(m.id + ".png");
    if (fs::is_regular_file(picture, ec)) m.picture = picture.u8string();
    if (std::any_of(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.id == m.id; })) continue;
    g_installed.push_back(std::move(m));
  }
}

bool save_locked(std::string* error) {
  json stages = json::array();
  for (const auto& m : g_installed) stages.push_back(to_json(m));
  const std::string text = json{{"random", g_random}, {"stages", stages}}.dump(2) + "\n";
  return write_atomic(g_root / L"installed.json", text.data(), text.size(), error);
}

// RGBA pixels as the console's RGBA8 texture: 4x4 blocks, each its 16 alpha and red bytes, then its
// 16 green and blue bytes (as host/roster.cpp). The width and height are multiples of 4.
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

// A PNG cut to the icon's shape at its middle and made kIconWidth x kIconHeight (each pixel the average
// of those it covers). Empty when it cannot be read.
std::vector<uint8_t> icon_pixels(const fs::path& path) {
  std::vector<uint8_t> out, file;
  if (!read_file(path, &file, 16u * 1024 * 1024) || file.empty()) return out;
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(file.data(), (int)file.size(), &w, &h, &n) || w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return out;
  stbi_uc* px = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
  if (!px) return out;
  int cx = 0, cy = 0, cw = w, ch = h;
  if ((int64_t)w * kIconHeight > (int64_t)h * kIconWidth) {   // wider than the icon: its sides cut
    cw = std::max(1, (int)((int64_t)h * kIconWidth / kIconHeight));
    cx = (w - cw) / 2;
  } else {
    ch = std::max(1, (int)((int64_t)w * kIconHeight / kIconWidth));
    cy = (h - ch) / 2;
  }
  out.resize((size_t)kIconWidth * kIconHeight * 4);
  for (int y = 0; y < kIconHeight; ++y) {
    const int y0 = cy + y * ch / kIconHeight, y1 = std::max(y0 + 1, cy + (y + 1) * ch / kIconHeight);
    for (int x = 0; x < kIconWidth; ++x) {
      const int x0 = cx + x * cw / kIconWidth, x1 = std::max(x0 + 1, cx + (x + 1) * cw / kIconWidth);
      uint32_t sum[4] = {0, 0, 0, 0}, count = 0;
      for (int sy = y0; sy < y1 && sy < h; ++sy)
        for (int sx = x0; sx < x1 && sx < w; ++sx, ++count)
          for (int c = 0; c < 4; ++c) sum[c] += px[((size_t)sy * w + sx) * 4 + c];
      uint8_t* p = &out[((size_t)y * kIconWidth + x) * 4];
      for (int c = 0; c < 3; ++c) p[c] = (uint8_t)(count ? sum[c] / count : 0);
      p[3] = 255;   // an icon is opaque: a picture's see-through parts would show the screen behind
    }
  }
  stbi_image_free(px);
  return out;
}

// A plain tile for a stage with no picture: dark, with a lighter edge.
std::vector<uint8_t> plain_tile() {
  std::vector<uint8_t> out((size_t)kIconWidth * kIconHeight * 4);
  for (int y = 0; y < kIconHeight; ++y)
    for (int x = 0; x < kIconWidth; ++x) {
      const bool edge = x < 2 || y < 2 || x >= kIconWidth - 2 || y >= kIconHeight - 2;
      uint8_t* p = &out[((size_t)y * kIconWidth + x) * 4];
      p[0] = edge ? 150 : 52; p[1] = edge ? 150 : 56; p[2] = edge ? 160 : 68; p[3] = 255;
    }
  return out;
}

}  // namespace

std::string base_of(const std::string& file_name, const std::string& post_stage) {
  const std::string name = file_part(file_name);
  if (name.size() < 5 || lower(name.substr(name.size() - 4)) != ".dat") return {};
  const std::string stem = name.substr(0, name.size() - 4);
  std::vector<std::string> words;
  std::string word;
  for (size_t i = 0; i <= stem.size(); ++i) {
    if (i < stem.size() && std::isalnum((unsigned char)stem[i])) {
      word += stem[i];
    } else if (!word.empty()) {
      words.push_back(lower(word));
      word.clear();
    }
  }
  std::set<std::string> found;
  for (const auto& w : words)
    for (const auto& b : kBases) {
      const std::string key = lower(std::string(b.file).substr(0, std::strlen(b.file) - 4));
      if (w == key) found.insert(b.file);
      // "GrNLaPs5": a word that begins with a retail versus stage's name, taken only when the post says
      // it is that stage (a Final Destination post).
      else if (!b.target && w.size() > key.size() && w.compare(0, key.size(), key) == 0 && !post_stage.empty() &&
               lower(post_stage) == lower(b.name))
        found.insert(b.file);
    }
  return found.size() == 1 ? *found.begin() : std::string();
}

std::string base_name(const std::string& base) {
  const Base* b = find_base(base);
  return b ? b->name : std::string();
}

bool target_test_base(const std::string& base) {
  const Base* b = find_base(base);
  return b && b->target;
}

std::string unsupported_reason(const std::string& file_name, const std::string& post_stage) {
  const std::string name = lower(file_part(file_name));
  if (name.size() < 5 || name.compare(name.size() - 4, 4, ".dat") != 0) return "Not a stage file.";
  if (!base_of(file_name, post_stage).empty()) return {};
  if (name.find("grps") != std::string::npos) return "Pokemon Stadium is not supported.";
  for (size_t i = 0; i + 5 <= name.size(); ++i)
    if (name.compare(i, 3, "grt") == 0 && std::isalpha((unsigned char)name[i + 3]) && std::isalpha((unsigned char)name[i + 4]) &&
        (i + 5 == name.size() || !std::isalnum((unsigned char)name[i + 5])))
      return "Only the Target Test stages tested as versus stages are taken.";
  return "Its name says no retail stage it replaces.";
}

bool prepare(std::vector<uint8_t>* bytes) { return host::menu_mods::normalize(bytes); }

bool check(const std::string& base, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
           std::string* why) {
  std::string local;
  if (!why) why = &local;
  const Base* b = find_base(base);
  if (!b) { *why = "Not a stage new stages are played over."; return false; }
  if (bytes.size() > kMaxBytes) {
    *why = "The file is larger than this game takes (" + std::to_string(bytes.size() / 1024) + " KB).";
    return false;
  }
  cosmetics::stage_safety::Archive stage{bytes};
  if (!stage.parse()) { *why = "It is not a stage file the game reads."; return false; }
  cosmetics::data_safety::Archive data{bytes};
  std::string inner;
  if (!data.parse()) { *why = "It is not a stage file the game reads."; return false; }
  if (!cosmetics::data_safety::stage_structure_valid(data, &inner)) { *why = "Its stage data is broken: " + inner + "."; return false; }
  if (retail.empty()) { *why = "The disc's own " + std::string(b->file) + " could not be read to compare."; return false; }
  if (bytes == retail) { *why = "It is the disc's own " + std::string(b->name) + "."; return false; }
  cosmetics::stage_safety::Archive original{retail};
  uint32_t lines = 0, groups = 0, base_lines = 0, base_groups = 0;
  if (!original.parse() || !collision_counts(original, &base_lines, &base_groups)) {
    *why = "The disc's own " + std::string(b->file) + " could not be read to compare.";
    return false;
  }
  if (!collision_counts(stage, &lines, &groups)) { *why = "Its collision cannot be read."; return false; }
  // The stage's code moves its collision groups by number (platforms, the parts that fall): a file with
  // fewer than its base would have them moved past its end. A retail stage's code also finds lines by
  // number; a Target Test stage's does not (custom files over them have far fewer lines and play).
  if (groups < base_groups) {
    *why = "It has " + std::to_string(groups) + " collision groups; " + b->name + " needs " + std::to_string(base_groups) + ".";
    return false;
  }
  if (!b->target && lines < base_lines) {
    *why = "It has " + std::to_string(lines) + " collision lines; " + b->name + " needs " + std::to_string(base_lines) + ".";
    return false;
  }
  const std::set<int> ids = point_ids(data);
  int starts = 0;
  for (int i = 0; i < 4; ++i) starts += ids.count(i) ? 1 : 0;
  if (starts < 4) {
    *why = "It has " + std::to_string(starts) + " starting point" + (starts == 1 ? "" : "s") + ": a versus match needs 4.";
    return false;
  }
  for (const int id : kNeededPoints)
    if (!ids.count(id)) {
      *why = id < 150 ? "It has no camera limits." : "It has no blast zones.";
      return false;
    }
  return true;
}

void configure(const std::string& settings_path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  fs::path parent = fs::u8path(settings_path).parent_path();
  if (parent.empty()) parent = fs::current_path();
  g_root = parent / L"NewStages";
  g_configured = true;
  load_locked();
  if (!g_installed.empty()) host::log("new stages: %zu installed%s", g_installed.size(), g_random ? ", in the random pick" : "");
}

std::vector<Installed> list() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto out = g_installed;
  std::stable_sort(out.begin(), out.end(), [](const Installed& a, const Installed& b) { return lower(a.name) < lower(b.name); });
  return out;
}

bool find(const std::string& id, Installed* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& m : g_installed)
    if (m.id == id) {
      if (out) *out = m;
      return true;
    }
  return false;
}

bool install(const Installed& info, const std::vector<uint8_t>& as_sent, const std::vector<uint8_t>& retail,
             const std::string& picture_png, std::string* error) {
  std::string why;
  std::vector<uint8_t> bytes = as_sent;
  if (bytes.size() > kMaxBytes) {
    if (error) *error = "The file is larger than this game takes (" + std::to_string(bytes.size() / 1024) + " KB).";
    return false;
  }
  if (prepare(&bytes) && bytes.size() != as_sent.size())
    host::log("new stages: card %d: its textures past its data were moved inside it (%zu -> %zu bytes)", info.file_id,
              as_sent.size(), bytes.size());
  if (!check(info.base, bytes, retail, &why)) {
    if (error) *error = why;
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_configured) {
    if (error) *error = "New stages are not set up this run.";
    return false;
  }
  Installed m = info;
  m.id = "s" + std::to_string(info.file_id);
  m.base = find_base(info.base)->file;
  m.size = bytes.size();
  m.sha256 = host::http::sha256_hex(bytes.data(), bytes.size());
  if (!id_ok(m.id)) {
    if (error) *error = "The card has no id.";
    return false;
  }
  const bool again = std::any_of(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.id == m.id; });
  if (!again && g_installed.size() >= kMaxInstalled) {
    if (error) *error = "There is room for " + std::to_string(kMaxInstalled) + " new stages: remove one first.";
    return false;
  }
  if (!write_atomic(g_root / fs::u8path(m.id + ".dat"), bytes.data(), bytes.size(), error)) return false;
  m.picture.clear();
  if (!picture_png.empty() &&
      write_atomic(g_root / fs::u8path(m.id + ".png"), picture_png.data(), picture_png.size(), nullptr))
    m.picture = (g_root / fs::u8path(m.id + ".png")).u8string();
  std::vector<Installed> before = g_installed;
  g_installed.erase(std::remove_if(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.id == m.id; }),
                    g_installed.end());
  g_installed.push_back(m);
  if (!save_locked(error)) {
    g_installed = std::move(before);
    return false;
  }
  host::log("new stages: %s \"%s\" installed over %s (%zu bytes, sha256 %s)", m.id.c_str(), m.name.c_str(), m.base.c_str(),
            bytes.size(), m.sha256.substr(0, 16).c_str());
  return true;
}

bool remove(const std::string& id, std::string* error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = std::find_if(g_installed.begin(), g_installed.end(), [&](const Installed& m) { return m.id == id; });
  if (it == g_installed.end()) {
    if (error) *error = "It is not installed.";
    return false;
  }
  const Installed gone = *it;
  std::vector<Installed> before = g_installed;
  g_installed.erase(it);
  if (!save_locked(error)) {
    g_installed = std::move(before);
    return false;
  }
  std::error_code ec;
  fs::remove(g_root / fs::u8path(gone.id + ".dat"), ec);
  fs::remove(g_root / fs::u8path(gone.id + ".png"), ec);
  host::log("new stages: %s (%s) removed", gone.id.c_str(), gone.name.c_str());
  return true;
}

bool served(const std::string& id, const std::vector<uint8_t>& retail, std::vector<uint8_t>* bytes, Installed* info) {
  Installed m;
  fs::path file;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = std::find_if(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.id == id; });
    if (!g_configured || it == g_installed.end()) return false;
    m = *it;
    file = g_root / fs::u8path(m.id + ".dat");
  }
  std::string why;
  std::vector<uint8_t> data;
  bool ok = read_file(file, &data, kMaxBytes);
  if (!ok) why = "its file is missing or too large";
  else if (!m.sha256.empty() && host::http::sha256_hex(data.data(), data.size()) != m.sha256) {
    ok = false;
    why = "its file changed since it was installed";
  } else if (!check(m.base, data, retail, &why)) {
    ok = false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string key = m.id + (ok ? "+" : "-") + m.sha256;
    if (g_logged.insert(key).second && !ok)
      host::log("new stages: %s (%s) not served, %s plays as it is: %s", m.id.c_str(), m.name.c_str(), m.base.c_str(), why.c_str());
  }
  if (!ok) return false;
  if (info) *info = m;
  *bytes = std::move(data);
  return true;
}

std::vector<uint8_t> icon(const std::string& id) {
  Installed m;
  std::vector<uint8_t> px;
  if (find(id, &m) && !m.picture.empty()) px = icon_pixels(fs::u8path(m.picture));
  if (px.empty()) px = plain_tile();
  return gx_rgba8(px, kIconWidth, kIconHeight);
}

bool random_included() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_random;
}

bool set_random_included(bool on, std::string* error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_configured) {
    if (error) *error = "New stages are not set up this run.";
    return false;
  }
  if (g_random == on) return true;
  g_random = on;
  if (!save_locked(error)) {
    g_random = !on;
    return false;
  }
  host::log("new stages: the random stage pick %s new stages", on ? "takes" : "leaves out");
  return true;
}

}  // namespace host::new_stages
