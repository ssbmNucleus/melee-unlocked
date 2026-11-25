// Menu mods (menu_mods.h): the store beside the settings, the checks, what is served.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_mods.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "host.h"
#include "http_client.h"

#include "nlohmann/json.hpp"

namespace host::menu_mods {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::mutex g_mutex;
fs::path g_root;
bool g_configured = false;
std::vector<Installed> g_installed;
std::set<std::string> g_logged;   // "served" lines said once per mod and outcome

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

std::string base_of(const std::string& path) {
  const size_t at = path.find_last_of("/\\");
  return at == std::string::npos ? path : path.substr(at + 1);
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

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
      if (error) *error = "Could not write the menu file.";
      return false;
    }
  }
  fs::rename(staged, path, ec);
  if (ec) {
    fs::remove(staged, ec);
    if (error) *error = "Could not keep the menu file.";
    return false;
  }
  return true;
}

// The root symbols' names (sorted), or false when the archive's tables do not lie inside it. Every
// relocated pointer must point inside the data block or just past it, as the game's own reader takes
// (sysdolphin/baselib/archive.c HSD_ArchiveCheck); `loose`: also to a picture after the tables
// (normalize() makes such a file one the game reads).
bool archive_names(const std::vector<uint8_t>& b, std::vector<std::string>* roots, std::vector<std::string>* refs,
                   std::string* why, bool loose = false) {
  const auto fail = [&](const char* text) { if (why) *why = text; return false; };
  if (b.size() < 0x20) return fail("It is not a menu file (too short).");
  const uint64_t file_size = be32(&b[0]), data_size = be32(&b[4]), relocs = be32(&b[8]), nroots = be32(&b[12]),
                 nrefs = be32(&b[16]);
  if (file_size < 0x20 || file_size > b.size()) return fail("Its header says another size than the file has.");
  if (relocs > (1u << 20) || nroots > 256 || nrefs > 256) return fail("Its tables are larger than a menu file's.");
  const uint64_t reloc_at = 0x20 + data_size, root_at = reloc_at + relocs * 4, ref_at = root_at + nroots * 8,
                 strings_at = ref_at + nrefs * 8;
  if (data_size == 0 || strings_at > file_size) return fail("Its tables run past the end of the file.");
  const uint64_t strings_size = file_size - strings_at;
  uint64_t names_end = strings_at;   // the end of the last symbol name: where the tables end
  for (uint64_t i = 0; i < nroots + nrefs; ++i) {
    const uint8_t* e = &b[(size_t)(root_at + i * 8)];
    const uint64_t at = be32(e), name = be32(e + 4);
    if (i < nroots && at >= data_size) return fail("A symbol of it lies outside its data.");
    if (name >= strings_size) return fail("A symbol's name lies outside the file.");
    std::string text;
    for (uint64_t k = strings_at + name; k < file_size && b[(size_t)k] != 0; ++k) {
      if (text.size() >= 96 || b[(size_t)k] < 0x20 || b[(size_t)k] > 0x7E) return fail("A symbol's name is not a name.");
      text += (char)b[(size_t)k];
    }
    if (strings_at + name + text.size() >= file_size) return fail("A symbol's name runs past the end of the file.");
    names_end = std::max(names_end, strings_at + name + text.size() + 1);
    (i < nroots ? roots : refs)->push_back(text);
  }
  // Every relocated pointer: its place inside the data, what it points to inside the data (or just past
  // it). Loose: also a picture after the tables, where DAT Texture Wizard puts a texture the file grew by:
  // past the last symbol name, 32-byte aligned as GX image data is, inside the file. Never into the
  // tables themselves.
  const uint64_t tail_from = names_end - 0x20, tail_to = file_size - 0x20;
  for (uint64_t i = 0; i < relocs; ++i) {
    const uint64_t at = be32(&b[(size_t)(reloc_at + i * 4)]);
    if (at + 4 > data_size || (at & 3)) return fail("A pointer of it lies outside its data.");
    const uint64_t to = be32(&b[(size_t)(0x20 + at)]);
    if (to <= data_size) continue;
    if (!loose || to < tail_from || to >= tail_to || (to & 31)) return fail("A pointer of it points outside its data.");
  }
  std::sort(roots->begin(), roots->end());
  std::sort(refs->begin(), refs->end());
  return true;
}

void put_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

json to_json(const Installed& m) {
  return json{{"id", m.id},         {"target", m.target},     {"name", m.name},       {"creator", m.creator},
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
  if (id.size() < 2 || id.size() > 12 || id[0] != 'm') return false;
  return std::all_of(id.begin() + 1, id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

void load_locked() {
  g_installed.clear();
  std::ifstream in(g_root / L"installed.json", std::ios::binary);
  if (!in) return;
  json doc = json::parse(in, nullptr, false);
  if (!doc.is_object() || !doc.contains("mods") || !doc["mods"].is_array()) {
    host::log("menu mods: installed.json is not readable; no menu mod this run");
    return;
  }
  for (const auto& j : doc["mods"]) {
    if (!j.is_object()) continue;
    Installed m;
    m.id = text_of(j, "id", 16);
    m.target = text_of(j, "target", 32);
    if (!id_ok(m.id) || std::find(targets().begin(), targets().end(), m.target) == targets().end()) continue;
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
    // One per file: a second for a file already taken is left out (the list was edited by hand).
    if (std::any_of(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.target == m.target || o.id == m.id; }))
      continue;
    g_installed.push_back(std::move(m));
  }
}

bool save_locked(std::string* error) {
  json mods = json::array();
  for (const auto& m : g_installed) mods.push_back(to_json(m));
  const std::string text = json{{"mods", mods}}.dump(2) + "\n";
  return write_atomic(g_root / L"installed.json", text.data(), text.size(), error);
}

}  // namespace

const std::vector<std::string>& targets() {
  static const std::vector<std::string> list{"MnSlChr.usd"};
  return list;
}

std::string kind_of(const std::string& file_name) {
  const std::string b = lower(base_of(file_name));
  const bool dat = b.size() > 4 && (b.compare(b.size() - 4, 4, ".dat") == 0 || b.compare(b.size() - 4, 4, ".usd") == 0);
  if (!dat) return {};
  if (b.find("mnslchr") != std::string::npos) return "Character select screen";
  if (b.find("mnslmap") != std::string::npos) return "Stage select screen";
  if (b.find("gmpause") != std::string::npos) return "Pause screen";
  if (b.rfind("ifall", 0) == 0) return "HUD font";
  return {};
}

std::string target_of(const std::string& file_name) {
  return kind_of(file_name) == "Character select screen" ? "MnSlChr.usd" : std::string();
}

std::string unsupported_reason(const std::string& file_name) {
  const std::string kind = kind_of(file_name);
  if (kind.empty()) return "Not a menu screen file.";
  if (kind == "Stage select screen") return "Stage select screens are not supported yet.";
  if (kind == "Pause screen" || kind == "HUD font") return "The match reads this file: not supported.";
  return {};
}

bool normalize(std::vector<uint8_t>* bytes) {
  std::vector<std::string> roots, refs;
  if (archive_names(*bytes, &roots, &refs, nullptr)) return true;   // read as it is
  roots.clear(); refs.clear();
  if (!archive_names(*bytes, &roots, &refs, nullptr, true)) return false;
  // Pointers past the data block (a texture DAT Texture Wizard put after the tables): the data block
  // becomes the whole file after the header, so every pointer is inside it at the same offset, and the
  // tables are written again after it (the old copies stay inside it, unread).
  const std::vector<uint8_t>& b = *bytes;
  const uint32_t file_size = be32(&b[0]), data_size = be32(&b[4]);
  const uint64_t tables_at = 0x20ull + data_size, tables_end = (uint64_t)file_size;
  const uint64_t grown = ((uint64_t)file_size - 0x20 + 31) & ~31ull;
  const uint64_t new_size = 0x20 + grown + (tables_end - tables_at);
  if (new_size > kMaxBytes || grown > 0xFFFFFFFFull) return false;
  std::vector<uint8_t> out((size_t)new_size, 0);
  std::memcpy(out.data(), b.data(), 0x20);
  std::memcpy(out.data() + 0x20, b.data() + 0x20, (size_t)file_size - 0x20);
  std::memcpy(out.data() + 0x20 + grown, b.data() + tables_at, (size_t)(tables_end - tables_at));
  put_be32(&out[0], (uint32_t)new_size);
  put_be32(&out[4], (uint32_t)grown);
  roots.clear(); refs.clear();
  if (!archive_names(out, &roots, &refs, nullptr)) return false;
  *bytes = std::move(out);
  return true;
}

bool check(const std::string& target, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
           std::string* why) {
  std::string local;
  if (!why) why = &local;
  if (std::find(targets().begin(), targets().end(), target) == targets().end()) {
    *why = "Not a menu file this game takes.";
    return false;
  }
  if (bytes.size() > kMaxBytes) {
    *why = "The file is larger than a menu screen's (" + std::to_string(bytes.size() / 1024) + " KB).";
    return false;
  }
  std::vector<std::string> roots, refs, retail_roots, retail_refs;
  if (!archive_names(bytes, &roots, &refs, why)) return false;
  if (retail.empty() || !archive_names(retail, &retail_roots, &retail_refs, nullptr)) {
    *why = "The disc's own " + target + " could not be read to compare.";
    return false;
  }
  if (roots != retail_roots || refs != retail_refs) {
    *why = "It is not a " + target + " (its symbols are another file's).";
    return false;
  }
  return true;
}

void configure(const std::string& settings_path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  fs::path parent = fs::u8path(settings_path).parent_path();
  if (parent.empty()) parent = fs::current_path();
  g_root = parent / L"MenuMods";
  g_configured = true;
  load_locked();
  if (!g_installed.empty()) host::log("menu mods: %zu installed", g_installed.size());
}

std::vector<Installed> list() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto out = g_installed;
  std::sort(out.begin(), out.end(), [](const Installed& a, const Installed& b) { return a.name < b.name; });
  return out;
}

bool installed_for(const std::string& target, Installed* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& m : g_installed)
    if (m.target == target) {
      if (out) *out = m;
      return true;
    }
  return false;
}

bool install(const Installed& info, const std::vector<uint8_t>& as_sent, const std::vector<uint8_t>& retail,
             const std::string& picture_png, std::string* error, std::string* replaced) {
  std::string why;
  std::vector<uint8_t> bytes = as_sent;
  if (bytes.size() > kMaxBytes) {
    if (error) *error = "The file is larger than a menu screen's (" + std::to_string(bytes.size() / 1024) + " KB).";
    return false;
  }
  if (normalize(&bytes) && bytes.size() != as_sent.size())
    host::log("menu mods: card %d: its textures past its data were moved inside it (%zu -> %zu bytes)", info.file_id,
              as_sent.size(), bytes.size());
  if (!check(info.target, bytes, retail, &why)) {
    if (error) *error = why;
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_configured) {
    if (error) *error = "Menu mods are not set up this run.";
    return false;
  }
  Installed m = info;
  m.id = "m" + std::to_string(info.file_id);
  m.size = bytes.size();
  m.sha256 = host::http::sha256_hex(bytes.data(), bytes.size());
  if (!id_ok(m.id)) {
    if (error) *error = "The card has no id.";
    return false;
  }
  if (!write_atomic(g_root / fs::u8path(m.id + ".dat"), bytes.data(), bytes.size(), error)) return false;
  std::error_code ec;
  m.picture.clear();
  if (!picture_png.empty() &&
      write_atomic(g_root / fs::u8path(m.id + ".png"), picture_png.data(), picture_png.size(), nullptr))
    m.picture = (g_root / fs::u8path(m.id + ".png")).u8string();
  std::vector<Installed> before = g_installed;
  std::string old_name, old_id;
  for (auto it = g_installed.begin(); it != g_installed.end();) {
    if (it->target == m.target || it->id == m.id) {
      if (it->id != m.id) { old_name = it->name; old_id = it->id; }
      it = g_installed.erase(it);
    } else {
      ++it;
    }
  }
  g_installed.push_back(m);
  if (!save_locked(error)) {
    g_installed = std::move(before);
    return false;
  }
  if (!old_id.empty()) {
    fs::remove(g_root / fs::u8path(old_id + ".dat"), ec);
    fs::remove(g_root / fs::u8path(old_id + ".png"), ec);
  }
  if (replaced) *replaced = old_name;
  host::log("menu mods: %s installed for %s (%zu bytes, sha256 %s)%s%s", m.id.c_str(), m.target.c_str(), bytes.size(),
            m.sha256.substr(0, 16).c_str(), old_name.empty() ? "" : ", in place of ", old_name.c_str());
  return true;
}

bool remove(const std::string& id, std::string* error, std::string* target) {
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
  if (target) *target = gone.target;
  host::log("menu mods: %s (%s) removed; %s is the disc's again", gone.id.c_str(), gone.name.c_str(), gone.target.c_str());
  return true;
}

bool served(const std::string& target, const std::vector<uint8_t>& retail, std::vector<uint8_t>* bytes,
            std::string* name) {
  Installed m;
  fs::path file;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = std::find_if(g_installed.begin(), g_installed.end(), [&](const Installed& o) { return o.target == target; });
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
  } else if (!check(target, data, retail, &why)) {
    ok = false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string key = m.id + (ok ? "+" : "-") + m.sha256;
    if (g_logged.insert(key).second) {
      if (ok) host::log("menu mods: %s serves %s (%s, %zu bytes)", target.c_str(), m.id.c_str(), m.name.c_str(), data.size());
      else host::log("menu mods: %s (%s) not served, the disc's %s instead: %s", m.id.c_str(), m.name.c_str(),
                     target.c_str(), why.c_str());
    }
  }
  if (!ok) return false;
  if (name) *name = m.name;
  *bytes = std::move(data);
  return true;
}

}  // namespace host::menu_mods
