// Source Port host: fighters installed beside the game (host/roster.h), their plugin folder and their
// own sounds (host/fighter_sound.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- fighters installed beside the game, on a disc with no m-ex table (host/roster.h) ----

// An m-ex disc carries its own fighters in MxDt.dat; the installed ones are for a disc without it.
bool roster_served() { return g_paths.find("/mxdt.dat") == g_paths.end(); }

// The one folder this run's installed fighters come from: their folders (host/roster.h), their
// .mufighter plugins (MU_CMD_MOD_BROWSER 6, mexft/mexft_plugins.c) and the mod browser's installs.
// Empty: none this run. A Vanilla launch is the retail game, so it has none: the owner launched with
// --vanilla-only and found Shulk and Wolf on the character select. --fighters-dir names the folder
// (or "none"), so a launcher can keep one set of fighters per listing; otherwise the folder beside
// the game, as before. None while this release has no Fighters (release_scope.h): what is installed stays
// on disk, unloaded.
std::filesystem::path installed_fighters_folder() {
  if (host::gameplay::vanilla_only() || host::options.vanilla_game) return {};
  if (!host::release_scope::gate(host::release_scope::kFighters, "installed fighters and their plugins are not loaded"))
    return {};
  if (host::options.fighters_dir_set)
    return host::options.fighters_dir == "none" ? std::filesystem::path() : std::filesystem::u8path(host::options.fighters_dir);
  return g_loaded_game_path.parent_path() / "fighters";
}

#ifdef _WIN32
// Where the fighter plugins come from before the gate: --fighters-dir, else MELEE_FIGHTER_PLUGINS, else
// the fighters folder beside the game library (the library's own rule from before the host gated it).
std::filesystem::path plugin_source_folder() {
  if (host::gameplay::vanilla_only() || host::options.vanilla_game) return {};
  if (!host::release_scope::gate(host::release_scope::kFighters, "installed fighters and their plugins are not loaded"))
    return {};
  if (host::options.fighters_dir_set) return installed_fighters_folder();
  return plugin_gate::default_plugin_folder(g_loaded_game_path.parent_path());
}

// The folder the game library loads this run's fighter plugins from: the host's copy of only those
// Nucleus signed or the player approved (plugin_gate.h). Empty: none this run. `ask`: a plugin nobody
// has decided on is put to the player first (an interactive start only, never mid-game).
std::filesystem::path gated_plugin_folder(bool ask) {
  const std::filesystem::path source = plugin_source_folder();
  if (source.empty()) return {};
  const plugin_gate::Prompt prompt = plugin_gate::ask_player;
  return plugin_gate::run_folder(source, ask ? &prompt : nullptr);
}
#endif

// ---- installed fighters' own sounds (host/fighter_sound.h) ----
// Their banks, the smash2.sem with those banks' scripts and nr_name.ssm with their calls, made in
// memory and served from the installed fighters' range after their files. A path the set no longer has
// gets the entry it had back. No retail view (g_view_alias): that table being empty or not is what
// says this run plays a mod online, and these files keep every retail bank and script as it is (bank
// 51's samples only move), so the retail fighters sound the same in either view.

// The disc's own copy of `path` (no pack or skin in its place), empty when there is none.
std::vector<uint8_t> disc_own_file(const std::string& path) {
  const auto replaced = g_sound_replaced.find(path);
  const auto found = g_paths.find(path);
  const int32_t i = replaced != g_sound_replaced.end() ? replaced->second : found != g_paths.end() ? found->second : -1;
  if (i <= 0 || (size_t)i >= g_fst.size() || (size_t)i * 12 + 12 > g_fst_raw.size() || g_fst[(size_t)i].dir) return {};
  const uint32_t offset = be32(g_fst_raw.data() + (size_t)i * 12 + 4), length = be32(g_fst_raw.data() + (size_t)i * 12 + 8);
  if (g_fst[(size_t)i].offset != offset || g_fst[(size_t)i].length != length || g_view_alias.count(i) || length == 0 ||
      length > (8u << 20))
    return {};
  std::vector<uint8_t> bytes(length);
  if (!host::disc_read(offset, bytes.data(), length)) return {};
  return bytes;
}

// The one switch of the installed fighters' own sounds and calls: MELEE_FIGHTER_SOUNDS=0 serves nothing
// for them (no tail on the records, serial 0), so the fighters keep their base fighters' banks and no
// call, as before the feature.
bool fighter_sounds_on() {
  static const bool on = [] {
    const char* v = std::getenv("MELEE_FIGHTER_SOUNDS");
    return !(v && v[0] == '0');
  }();
  return on;
}

void publish_roster_sound(bool log) {
  if (!fighter_sounds_on()) {
    if (log) host::log("roster: sound: off (MELEE_FIGHTER_SOUNDS=0)");
    g_sound_tails.clear();
    return;
  }
  if (!g_disc_sounds_read) {
    g_disc_sounds_read = true;
    g_disc_sounds.sem_us = disc_own_file("/audio/us/smash2.sem");
    g_disc_sounds.sem_jp = disc_own_file("/audio/smash2.sem");
    g_disc_sounds.names_us = disc_own_file("/audio/us/nr_name.ssm");
  }
  host::fighter_sound::Set set = host::fighter_sound::build(host::roster::fighters(), g_disc_sounds, &g_sound_session);
  if (log)
    for (const auto& line : set.log) host::log("roster: %s", line.c_str());
  // All of the set or none of it: smash2.sem, nr_name.ssm and the banks only work together.
  uint64_t needed = 0;
  for (const auto& f : set.files) {
    uint32_t h = 2166136261u;
    for (uint8_t b : f.data) { h ^= b; h *= 16777619u; }
    if (!g_sound_entries.count(std::make_pair(f.path, h))) needed += ((uint64_t)f.data.size() + 0xFFFFull) & ~0xFFFFull;
  }
  if (g_roster_next + needed > kPackOnlineBase) {
    host::log("roster: the installed fighters' sounds do not fit their disc range; the disc's own sounds are served");
    for (auto& tail : set.tails)
      if (tail.size() >= 3) {   // no bank, no file: the base fighter's (the calls stay what the game checks)
        std::vector<uint8_t> t{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        t.insert(t.end(), tail.end() - 8, tail.end());
        std::copy(tail.end() - 4, tail.end(), t.end() - 8);   // the call is the fallback
        tail = std::move(t);
      }
    set.files.clear();
    set.serial = 0;
  }
  std::set<std::string> served;
  for (const auto& f : set.files) served.insert(f.path);
  for (auto it = g_sound_replaced.begin(); it != g_sound_replaced.end();) {
    if (served.count(it->first)) { ++it; continue; }
    if (it->second >= 0) g_paths[it->first] = it->second;
    else g_paths.erase(it->first);
    it = g_sound_replaced.erase(it);
  }
  for (const auto& f : set.files) {
    uint32_t h = 2166136261u;
    for (uint8_t b : f.data) { h ^= b; h *= 16777619u; }
    const auto key = std::make_pair(f.path, h);
    auto known = g_sound_entries.find(key);
    if (known == g_sound_entries.end()) {
      const uint64_t span = ((uint64_t)f.data.size() + 0xFFFFull) & ~0xFFFFull;
      if (g_roster_next + span > kPackOnlineBase) {
        host::log("roster: %s does not fit the installed fighters' range; the disc's copy stays", f.path.c_str());
        continue;
      }
      const int32_t entry = (int32_t)g_fst.size();
      g_fst.push_back({g_roster_next, (uint32_t)f.data.size(), false});
      g_roster_memory.push_back({g_roster_next, f.data});
      g_roster_next += (uint32_t)span;
      known = g_sound_entries.emplace(key, entry).first;
    }
    if (!g_sound_replaced.count(f.path)) {
      const auto old = g_paths.find(f.path);
      g_sound_replaced[f.path] = old != g_paths.end() ? old->second : -1;
    }
    g_paths[f.path] = known->second;
  }
  if (log && set.serial != g_sound_serial)
    host::log("roster: sounds %08X: %zu files served for the installed fighters%s", set.serial, set.files.size(),
              set.serial ? "" : " (the disc's own)");
  g_sound_serial = set.serial;
  g_sound_tails = std::move(set.tails);
}

// Reads the fighters folder again and gives every file of an installed fighter a place on the
// disc. A name the disc already has keeps the disc's file: a fighter made from a retail one names
// that fighter's own effect and hat files.
void publish_roster() {
  const std::filesystem::path folder = roster_served() ? installed_fighters_folder() : std::filesystem::path();
  if (folder.empty()) {
    host::online_fighters::roster_published(false);
    return;
  }
  static bool sound_published = false;
  const bool changed = host::roster::scan(folder);
  if (changed || !sound_published) {
    publish_roster_sound(true);
    sound_published = true;
  }
  for (const auto& fighter : host::roster::fighters()) {
    uint32_t added = 0;
    for (const auto& file : fighter.files) {
      std::string path = "/" + file.name;
      std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
      if (g_paths.find(path) != g_paths.end()) continue;
      const uint64_t span = (file.size + 0xFFFFull) & ~0xFFFFull;
      if (g_roster_next + span > kPackOnlineBase) {
        host::log("roster: %s of %s does not fit the installed fighters' range; left out", file.name.c_str(), fighter.name.c_str());
        continue;
      }
      const int32_t entry = (int32_t)g_fst.size();
      g_fst.push_back({g_roster_next, (uint32_t)file.size, false});
      g_paths[path] = entry;
      g_roster_files.push_back({g_roster_next, (uint32_t)file.size, file.path});
      g_roster_next += (uint32_t)span;
      ++added;
    }
    if (changed || added)
      host::log("roster: %s (fighters\\%s): %zu files, %u added to the disc", fighter.name.c_str(), fighter.folder.c_str(),
                fighter.files.size(), added);
  }
  // Custom fighters online need the roster to be this run's fighter table: not in the retail view.
  host::online_fighters::roster_published(!g_retail_view);
}

// Serves a read inside the installed fighters' range; false when the offset is outside it.
bool read_roster_file(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kRosterBase || offset >= kPackOnlineBase || (g_roster_files.empty() && g_roster_memory.empty())) return false;
  *ok = false;
  for (const auto& f : g_roster_memory) {
    if (offset < f.start || offset >= f.start + (uint32_t)f.data.size()) continue;
    // As the system files: the last 32-byte chunk runs past the end, into zeros up to the 64 KB slot.
    const uint32_t at = offset - f.start;
    const uint32_t slot = ((uint32_t)f.data.size() + 0xFFFFu) & ~0xFFFFu;
    if (size > slot - at) return true;
    const uint32_t have = std::min<uint32_t>(size, (uint32_t)f.data.size() - at);
    std::memcpy(dst, f.data.data() + at, have);
    std::memset((uint8_t*)dst + have, 0, size - have);
    *ok = true;
    return true;
  }
  for (const auto& f : g_roster_files) {
    if (offset < f.start || offset >= f.start + f.length) continue;
    // The last read of a file runs a few bytes past its end (32-byte chunks): zeros, as on a disc.
    const uint32_t at = offset - f.start;
    const uint32_t slot = (f.length + 0xFFFFu) & ~0xFFFFu;
    if (size > slot - at) return true;
    const uint32_t have = std::min<uint32_t>(size, f.length - at);
    std::ifstream in(f.path, std::ios::binary);
    in.seekg(at);
    in.read((char*)dst, have);
    *ok = (uint32_t)in.gcount() == have;
    std::memset((uint8_t*)dst + have, 0, size - have);
    return true;
  }
  return true;
}

}  // namespace host_internal
}  // namespace source_port
