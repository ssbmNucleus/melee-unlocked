// Source Port host: the disc. The filesystem table, Slippi's system files (--slippi-menus), entry
// numbers and reads in every range the host serves, the music packs and the jukebox's songs.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool read_fst() {
  uint8_t header[0x440];
  if (!host::disc_read(0, header, sizeof header)) return false;
  const uint32_t fst_offset = be32(header + 0x424), fst_size = be32(header + 0x428);
  // A trimmed, compressed or damaged image can have no usable table here: refuse it plainly instead
  // of reading an empty buffer (a 0.8.5 player's disc crashed at startup this way).
  if (fst_size < 12 || fst_size > (16u << 20)) return false;
  std::vector<uint8_t> fst(fst_size);
  if (!host::disc_read(fst_offset, fst.data(), fst_size)) return false;
  const uint32_t count = be32(fst.data() + 8);
  if (count < 1 || (uint64_t)count * 12 > fst_size) return false;
  const char* names = (const char*)fst.data() + count * 12;
  g_fst_raw = fst;
  g_fst.assign(count, FstFile{});
  std::vector<std::pair<uint32_t, std::string>> dirs{{count, ""}};   // (end entry, path) of open directories
  for (uint32_t i = 1; i < count; ++i) {
    while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
    const uint8_t* e = fst.data() + i * 12;
    const uint32_t name_at = count * 12 + (be32(e) & 0xFFFFFFu);
    if (name_at >= fst_size) return false;
    std::string name(names + (be32(e) & 0xFFFFFFu), strnlen(names + (be32(e) & 0xFFFFFFu), fst_size - name_at));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    const std::string full = dirs.back().second + "/" + name;
    g_fst[i] = {be32(e + 4), be32(e + 8), e[0] != 0};
    g_paths[full] = (int32_t)i;
    if (e[0]) dirs.push_back({be32(e + 8), full});
  }
#if MU_SQUAD
  g_disc_paths = g_paths;
#endif
  // The boot loader left the disc header at the start of MEM1, then the OS globals. The native game
  // reads those words as ordinary integers, so they are in host byte order.
  std::memcpy((void*)MEM1_BASE, header, 0x20);
  auto low = [](uint32_t offset, uint32_t value) { std::memcpy((void*)(MEM1_BASE + offset), &value, 4); };
  low(0x28, 24u << 20);      // physical memory size
  low(0xCC, 0);              // TV mode: NTSC
  low(0xF0, 24u << 20);      // simulated memory size
  low(0xF8, 162000000u);     // bus clock
  low(0xFC, 486000000u);     // core clock
  return true;
}

// ---- Slippi system files (--slippi-menus) ----
// The online menus read Slippi's patched menu files (online rows, descriptions, frozen Stadium name,
// connect-code text) and its CSS file. The static recomp gets them over EXI (D1/D2, exi_slippi.cpp);
// the native game asks for them through the ordinary disc path, so they are registered in the
// filesystem table here, built once at boot with the same code. Own offset range, never counted as
// mod assets. Any failure turns the menus off for the run instead of mixing vanilla and patched.
const char* const kSystemFileNames[] = {
  "MnMaAll.usd", "MnMaAll.dat", "SdMenu.usd", "SdMenu.dat", "MnSlMap.usd", "MnSlMap.dat",
  "SdSlChr.usd", "SdSlChr.dat", "MnExtAll.usd", "MnExtAll.dat", "slpCSS.dat", "GameSetup_gui.dat",
  "SlippiCSS_gui.dat",
};
// Slippi 3.7.0's name for slpCSS.dat, built from links into the player's disc (exi_slippi.cpp). The
// native CSS asks for it first and falls back to slpCSS.dat, so when it cannot be built (a disc that
// lacks what it links to) it is left out rather than turning the menus off.
bool optional_system_file(const char* name) { return std::strcmp(name, "SlippiCSS_gui.dat") == 0; }

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// True when the served file (a mod disc's stage select) is an m-ex one: its symbol table names
// mexMapData, the stage pages the native m-ex layer reads (mnstagesel.c, mexsel_map).
bool mex_stage_select(const FstFile& file) {
  if (file.dir || file.length < 32 || file.length > 64u * 1024 * 1024) return false;
  std::vector<uint8_t> bytes(file.length);
  bool ok = false;
  if (!read_overlay(file.offset, bytes.data(), file.length, &ok)) ok = host::disc_read(file.offset, bytes.data(), file.length);
  if (!ok) return false;
  static const char kName[] = "mexMapData";
  return std::search(bytes.begin(), bytes.end(), kName, kName + sizeof kName) != bytes.end();   // with its 0
}

void load_system_files(const char* forced_off_by) {
#ifdef MELEE_NO_SLIPPI
  // This build has no patched menu files to serve: the game reads the disc's own, always.
  (void)forced_off_by;
#else
  if (!g_slippi_menus_requested) { host::log("slippi menus: off (--slippi-menus off)"); return; }
  if (forced_off_by) { host::log("slippi menus: off (%s)", forced_off_by); return; }
  std::vector<SystemFile> files;
  uint32_t next = kSystemFileBase;
  for (const char* name : kSystemFileNames) {
    std::string path = std::string("/") + name;
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    const bool modded = std::any_of(g_mod_overlay.files().begin(), g_mod_overlay.files().end(),
        [&](const ModOverlay::File& f) { return f.path == path; });
    // Slippi's online menus need Slippi's copy. A pack's own copy (Akaneia's stage select) would
    // offer stages whose code the native game does not have, so Slippi's is used in both views.
    if (modded) host::log("slippi menus: Slippi's %s replaces the pack's copy", name);
    std::vector<uint8_t> data = slippi::system_game_file(name);
    if (data.empty() && optional_system_file(name)) { host::log("slippi menus: %s left out (cannot build it)", name); continue; }
    if (data.empty()) { host::log("slippi menus: off (cannot build %s)", name); return; }
    const uint32_t size = (uint32_t)data.size();
    if (uint64_t(next) + size > kMenuModBase) { host::log("slippi menus: off (system files exceed their range)"); return; }
    files.push_back({path, next, std::move(data)});
    next += (size + 0xFFFFu) & ~0xFFFFu;
  }
  uint32_t replaced = 0, added = 0;
  for (const auto& f : files) {
    auto found = g_paths.find(f.path);
    int32_t entry;
    if (found != g_paths.end()) {
      if (g_fst[found->second].dir) { host::log("slippi menus: %s is a disc directory", f.path.c_str()); continue; }
      entry = found->second;
      // An m-ex disc's own stage select (ACE: its stage pages, mexMapData) stays in the mod view:
      // Slippi's copy, built from the retail file, left only the retail stages on the screen
      // (0.9.97 report "ACE maps don't show up"). The native m-ex layer locks every added stage
      // it has no code for. The retail view (online against the standard game) gets Slippi's.
      const auto alias = g_view_alias.find(entry);
      if ((f.path == "/mnslmap.usd" || f.path == "/mnslmap.dat") && alias != g_view_alias.end() &&
          mex_stage_select(g_fst[entry])) {
        const int32_t slippi_entry = (int32_t)g_fst.size();
        g_fst.push_back({f.start, (uint32_t)f.data.size(), false});
        alias->second = slippi_entry;
        host::log("slippi menus: %s keeps the disc's m-ex stage select; Slippi's copy is its retail view", f.path.c_str());
        continue;
      }
      ++replaced;
    } else {
      entry = (int32_t)g_fst.size(); g_fst.push_back({0, 0, false}); g_paths[f.path] = entry; ++added;
    }
    g_fst[entry] = {f.start, (uint32_t)f.data.size(), false};
    g_view_alias.erase(entry);   // the same file in both views
    host::log("slippi menus: %s %u bytes fnv1a %08X", f.path.c_str(), (uint32_t)f.data.size(),
              fnv1a(f.data.data(), f.data.size()));
  }
  g_system_files = std::move(files);
  g_slippi_menus = true;
  host::log("slippi menus: on, %u system files replaced, %u added", replaced, added);
#endif
}

// Serves a read inside the system-file range; false when the offset is outside it.
bool read_system_file(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kSystemFileBase || offset >= ModOverlay::base || g_system_files.empty()) return false;
  for (auto& f : g_system_files) {
    if (offset < f.start || offset >= f.start + (uint32_t)f.data.size()) continue;
    // The game reads in 32-byte-rounded chunks, so the last read runs a few bytes past the end,
    // into padding on a real disc. Serve that padding as zeros, up to the file's 64 KB slot.
    const uint32_t at = offset - f.start;
    const uint32_t slot = ((uint32_t)f.data.size() + 0xFFFFu) & ~0xFFFFu;
    *ok = size <= slot - at;
    if (*ok) {
      const auto& bytes = f.data;
      const uint32_t have = std::min<uint32_t>(size, (uint32_t)bytes.size() - at);
      std::memcpy(dst, bytes.data() + at, have);
      std::memset((uint8_t*)dst + have, 0, size - have);
    }
    return true;
  }
  *ok = false;
  return true;
}

int32_t resolve_disc_entrynum(const std::string& p);
int32_t h_disc_entrynum(const char* path) {
  std::string p = path ? path : "";
  std::transform(p.begin(), p.end(), p.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
  if (p.empty() || p[0] != '/') p = "/" + p;
  const int32_t opened = resolve_disc_entrynum(p);
  return opened;
}
int32_t resolve_disc_entrynum(const std::string& p) {
  // A menu mod (publish_menu_mods): its entry in every view.
  if (!g_menu_paths.empty())
    if (const auto menu = g_menu_paths.find(p); menu != g_menu_paths.end()) return menu->second;
  // A new stage picked for this match (plan_new_stage): its base's path opens it.
  if (g_new_stage_entry > 0 && p == g_new_stage_path) return g_new_stage_entry;
  auto it = g_paths.find(p);
  if (it != g_paths.end() && g_retail_view) {
    // The retail copy, or -1 for a file the retail game does not have. A pack costume that only
    // changes looks keeps its own entry, so it is the same file (and the same cached copy) in both views.
    const auto grafted = g_pack_online_entry.find(it->second);
    if (grafted != g_pack_online_entry.end()) return grafted->second;
    const int32_t opened = skins::resolve_open(true, it->second, g_view_alias, g_pack_skin_served);
    if (opened != it->second) return opened;
  }
  if (it != g_paths.end() && !g_cosmetic_alias.empty()) {
    const auto alias = g_cosmetic_alias.find(it->second);
    if (alias != g_cosmetic_alias.end() && host::cosmetics::online_active()) {
      const bool form = alias->second >= 0 && (size_t)alias->second < g_fst.size() &&
                        g_fst[(size_t)alias->second].offset >= kCosmeticBase;
      host::log("cosmetics: %s opened as %s (online rule)", p.c_str(), form ? "its online form" : "the disc's copy");
      return alias->second;
    }
  }
  static const bool trace = host::test_env::getenv("MELEE_TRACE_DISC_OPEN") != nullptr;
  if (trace) host::log("disc: open %s -> %d at %08X", p.c_str(), it == g_paths.end() ? -1 : it->second,
                       it == g_paths.end() ? 0u : g_fst[it->second].offset);
  return it == g_paths.end() ? -1 : it->second;
}
int32_t h_disc_file(int32_t entrynum, uint32_t* start, uint32_t* length) {
  if (entrynum <= 0 || entrynum >= (int32_t)g_fst.size() || g_fst[entrynum].dir) return 0;
  *start = g_fst[entrynum].offset;
  *length = g_fst[entrynum].length;
  return 1;
}
// Music packs mirror the disc's /audio tree. A file with the original name replaces that track;
// a sibling folder with the original stem is a random HPS playlist for that track.
void index_music_paths() {
  g_music_paths.clear();
  for (const auto& path : g_paths) {
    const int32_t index = path.second;
    if (index <= 0 || index >= (int32_t)g_fst.size()) continue;
    const FstFile& file = g_fst[(size_t)index];
    if (file.dir || path.first.rfind("/audio/", 0) != 0 ||
        _stricmp(std::filesystem::u8path(path.first).extension().string().c_str(), ".hps") != 0)
      continue;
    const std::string relative = path.first.substr(1);
    g_music_paths.try_emplace(file.offset, relative);
    const auto alias = g_view_alias.find(index);
    if (alias != g_view_alias.end() && alias->second > 0 && alias->second < (int32_t)g_fst.size())
      g_music_paths.try_emplace(g_fst[(size_t)alias->second].offset, relative);
  }
#ifdef __APPLE__
  // macOS: the program sits inside a signed, read-only .app bundle (writing there would break the code seal), so music packs live in the player's user folder (--user-dir) instead.
  const auto& user = slippi::online::config().user_dir;
  if (user.empty()) return;
  const std::filesystem::path root = std::filesystem::u8path(user) / "MusicPacks";
#else
  wchar_t module[32768];
  const DWORD n = GetModuleFileNameW(nullptr, module, (DWORD)_countof(module));
  if (!n || n >= _countof(module)) return;
  const std::filesystem::path root = std::filesystem::path(std::wstring(module, n)).parent_path() / L"MusicPacks";
#endif
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) return;
  std::vector<std::string> paths;
  paths.reserve(g_music_paths.size());
  for (const auto& item : g_music_paths) paths.push_back(item.second);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::ofstream list(root / L"Tracks.txt", std::ios::trunc);
  if (!list) return;
  list << "Menu and stage music files on this game disc:\n";
  for (const auto& path : paths) list << "/" << path << "\n";
}

bool jukebox_music_pack_read(uint32_t offset, std::filesystem::path* out) {
  if (!out) return false;
  const auto source = g_music_paths.find(offset);
  if (source == g_music_paths.end()) return false;
  return slippi::jukebox::resolve_music_pack_path(source->second, out);
}

// The jukebox reads songs from where the game would: Slippi system files, the mod overlay, the disc.
bool jukebox_disc_read(uint32_t offset, void* dst, uint32_t size) {
  bool ok = false;
  if (!read_cosmetic(offset, dst, size, &ok) && !read_pack_online(offset, dst, size, &ok) &&
      !read_roster_file(offset, dst, size, &ok) && !read_training_file(offset, dst, size, &ok) &&
      !read_new_stage(offset, dst, size, &ok) && !read_menu_mod(offset, dst, size, &ok) &&
      !read_system_file(offset, dst, size, &ok) && !read_overlay(offset, dst, size, &ok))
    ok = host::disc_read(offset, dst, size);
  return ok;
}
void h_disc_read(uint32_t offset, void* dst, uint32_t size, MuDiscDone done, void* user) {
  bool ok = false;
  const auto began = std::chrono::steady_clock::now();
  if (!read_cosmetic(offset, dst, size, &ok) && !read_pack_online(offset, dst, size, &ok) &&
      !read_roster_file(offset, dst, size, &ok) && !read_training_file(offset, dst, size, &ok) &&
      !read_new_stage(offset, dst, size, &ok) && !read_menu_mod(offset, dst, size, &ok) &&
      !read_system_file(offset, dst, size, &ok) && !read_overlay(offset, dst, size, &ok))
    ok = host::disc_read(offset, dst, size);
  if (!ok) host::log("disc: read %08X+%X failed", offset, size);
  // Reads are synchronous on the simulation thread: a slow one is a hitch the player feels. Logged
  // (at most 50 times) so a report of stutter can be matched against the disc.
  {
    static int slow_logged = 0;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    if (ms > 4.0 && slow_logged < 50) {
      ++slow_logged;
      uint32_t major = 0, minor = 0, match_frame = 0;
      host::current_scene(&major, &minor, &match_frame);
      host::log("disc: slow read %.1f ms (%08X+%X, scene %02X/%02X)", ms, offset, size, major, minor);
    }
  }
  note_disc_load(dst, size, offset);
  ++host::g_disc_reads;
  host::g_disc_bytes += size;
  done(ok ? (int32_t)size : -1, user);   // the game queues it and runs it as the drive interrupt
}
int32_t h_disc_status() { return 0; }
uint32_t h_disc_id(void* out, uint32_t size) { const uint32_t n = std::min<uint32_t>(size, 0x20); std::memcpy(out, (void*)MEM1_BASE, n); return n; }

// MU_CMD_MUSIC_PLAY, from h_slippi_command (source_host.cpp).
bool music_play_command(const uint8_t* p) {
  // 20XX TE "Reduce Dream Land volume" (offline): its song 90/127 as loud, as TE plays it.
  const uint32_t offset = read_be32(p);
  if (te_host_feature(MU_GAME_OPTION2_TE_DL64_QUIET)) {
    const auto dl64 = g_paths.find("/audio/old_kb.hps");
    if (dl64 != g_paths.end() && g_fst[dl64->second].offset == offset)
      slippi::jukebox::set_next_song_gain(90.0f / 127.0f);
  }
  slippi::jukebox::start_song(offset, read_be32(p + 4));
  return true;
}

}  // namespace host_internal
}  // namespace source_port
