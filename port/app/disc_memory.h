// Locate the previously selected disc in local launcher settings, and the player's data folder.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "../runtime/host/product_identity.h"
#include "../third_party/nlohmann/json.hpp"   // the Slippi Launcher's Settings file (replay_folder)
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace disc_memory {
namespace fs = std::filesystem;

// The last "iso=" line of an ini's text, as written (bytes, line ending removed); "" when none.
inline std::string iso_line(const std::string& text) {
  std::string found;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("iso=", 0) == 0) found = line.substr(4);
    at = end + 1;
  }
  return found;
}

inline std::string read_iso(const fs::path& ini) {
  std::ifstream f(ini, std::ios::binary);
  if (!f) return {};
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return iso_line(text);
}

// standalone.ini is written as UTF-8; launcher.ini in the system code page (GetFullPathNameA).
enum class Encoding { Utf8, Ansi };
// full_check: the disc may be from a mod run, so the header is not enough (see above).
struct Place { fs::path ini; Encoding encoding; bool full_check = true; };

inline fs::path prior_standalone_dir(const fs::path& local_app_data) {
  return local_app_data / product::kUpstreamDataFolders[1];
}

// A folder a player has used: a remembered disc, settings, a Slippi login or a saved game in it.
// Merely opening the launcher creates launcher.ini, so that file does not identify a game profile.
inline bool has_profile(const fs::path& dir) {
  std::error_code ec;
  for (const auto* marker : {"standalone.ini", "port-settings.ini", "User/Slippi/user.json"})
    if (fs::is_regular_file(dir / marker, ec)) return true;
  const auto card = dir / "User" / "GC" / "CardA";
  for (const auto& folder : {card, card / "VanillaOnly"}) {
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code kind;
      if (it->path().extension() == ".gci" && it->is_regular_file(kind)) return true;
    }
  }
  return false;
}

// Melee Unlocked's own profile folder (read only here): its Windows executable used the older
// YouGame folder even when started on its own, and a player's complete profile stays together.
inline fs::path upstream_dir(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const auto current = local_app_data / product::kUpstreamDataFolders[0];
  const auto prior = prior_standalone_dir(local_app_data);
  return !has_profile(current) && has_profile(prior) ? prior : current;
}

// This program's own data folder (product_identity.h): settings, the remembered disc, the Slippi user
// folder, saves and replays of a no-argument start. Never shared with an installed Melee Unlocked.
inline fs::path standalone_dir(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  return local_app_data / product::kDataFolder;
}

// What the Slippi Launcher's Settings file (JSON, %APPDATA%\Slippi Launcher\Settings) says about
// replays: its replay folder (settings.rootSlpPath, an absolute path; empty when not set) and whether
// its "monthly subfolders" setting is on (settings.useMonthlySubfolders). The file is only read; a
// missing, unreadable or malformed file says nothing.
struct LauncherReplays { fs::path root; bool monthly = false; };
inline LauncherReplays launcher_replays(const fs::path& settings_file) {
  LauncherReplays out;
  std::error_code ec;
  if (settings_file.empty() || !fs::is_regular_file(settings_file, ec)) return out;
  const auto size = fs::file_size(settings_file, ec);
  if (ec || size > (4u << 20)) return out;   // a few kilobytes as the Launcher writes it
  std::ifstream f(settings_file, std::ios::binary);
  if (!f) return out;
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
  if (json.is_discarded() || !json.is_object()) return out;
  const auto settings = json.find("settings");
  if (settings == json.end() || !settings->is_object()) return out;
  const auto monthly = settings->find("useMonthlySubfolders");
  out.monthly = monthly != settings->end() && monthly->is_boolean() && monthly->get<bool>();
  const auto root = settings->find("rootSlpPath");
  if (root != settings->end() && root->is_string() && !root->get_ref<const std::string&>().empty()) {
    try {
      const fs::path path = fs::u8path(root->get_ref<const std::string&>());
      if (path.is_absolute()) out.root = path;
    } catch (...) {}   // not a path this system can name: no setting
  }
  return out;
}

// Where a plain start saves its replays: where the player's replay tracker looks, as the Nucleus app's
// card does (its tracker's get_replay_root). The Slippi Launcher's replay folder when it exists (read
// from `launcher_settings`, launcher_replays), else `documents`\Slippi when that folder exists (the
// Launcher's default), else this program's own Replays folder in `data`. No Slippi folder is created
// here. `month_folders`: stock Slippi saves each game into a YYYY-MM folder inside the Slippi folder
// while the Launcher's "monthly subfolders" is on (--replay-month-folders); this program's folder is flat.
struct ReplayFolder { fs::path dir; bool month_folders = false; };
inline ReplayFolder replay_folder(const fs::path& launcher_settings, const fs::path& documents, const fs::path& data) {
  const LauncherReplays launcher = launcher_replays(launcher_settings);
  std::error_code ec;
  if (!launcher.root.empty() && fs::is_directory(launcher.root, ec)) return {launcher.root, launcher.monthly};
  if (!documents.empty() && documents.is_absolute() && fs::is_directory(documents / "Slippi", ec))
    return {documents / "Slippi", launcher.monthly};
  return {data / "Replays", false};
}

// The command line a no-argument start runs this same exe (`self`, in the folder `exe`) with: the
// packaged MeleeUnlocked.bat's, with the data folder `data` (standalone_dir) for the settings, the
// Slippi user folder, the memory card, the log and the shader cache, Sys beside the exe, and the
// replays in `replays` (replay_folder: where the player's tracker looks).
// No --vanilla-only (since 2026-11-25): a plain start is the Nucleus game (Mods menu, lobbies, online
// options); the Nucleus app's Vanilla card and the --vanilla-only option are the ways to a vanilla
// session.
inline std::vector<std::wstring> plain_start_args(const fs::path& self, const fs::path& exe, const fs::path& data,
                                                  const fs::path& iso, const ReplayFolder& replays) {
  std::vector<std::wstring> args = {self.wstring(),
          L"--iso", iso.wstring(),
          L"--threaded-renderer",
          L"--settings-path", (data / L"port-settings.ini").wstring(),
          L"--sys-dir", (exe / L"Sys").wstring(),
          L"--user-dir", (data / L"User" / L"Slippi").wstring(),
          L"--replay-dir", (replays.dir.empty() ? data / L"Replays" : replays.dir).wstring(),
          L"--card-dir", (data / L"User" / L"GC" / L"CardA").wstring(),
          L"--log-file", (data / L"melee.log").wstring(),
          L"--shader-cache", (data / L"shadercache").wstring()};
  if (replays.month_folders && !replays.dir.empty()) args.push_back(L"--replay-month-folders");
  return args;
}

// A plain start before 2026-11-25 was a vanilla-only session, which keeps its saves in the card's
// VanillaOnly folder (main.cpp); it plays on the card itself now. While `card` holds no save (.gci),
// the VanillaOnly folder's saves are copied into it, so a returning player keeps them. Nothing is
// moved, changed, deleted or overwritten. Returns how many were copied.
inline int carry_vanilla_saves(const fs::path& card) {
  const auto saves_in = [](const fs::path& folder) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code kind;
      if (it->path().extension() == ".gci" && it->is_regular_file(kind)) found.push_back(it->path());
    }
    return found;
  };
  if (card.empty() || !saves_in(card).empty()) return 0;
  int copied = 0;
  for (const fs::path& save : saves_in(card / "VanillaOnly")) {
    std::error_code ec;
    fs::create_directories(card, ec);
    copied += fs::copy_file(save, card / save.filename(), fs::copy_options::skip_existing, ec) && !ec;
  }
  return copied;
}

// This program's folder from before its rename (Netlee; read only here).
inline fs::path previous_dir(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  return local_app_data / product::kPreviousDataFolders[0];
}

// Where a first start copies a profile from: this program's earlier folder when the player used it,
// else Melee Unlocked's.
inline fs::path forward_source(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const fs::path previous = previous_dir(local_app_data);
  return has_profile(previous) ? previous : upstream_dir(local_app_data);
}

// The remembered disc: this program's own standalone.ini first (the only one it writes), then, read
// only, the one it remembered under its earlier name, then the discs Melee Unlocked remembered (its
// standalone.ini and its launcher's launcher.ini).
inline std::vector<Place> places(const fs::path& local_app_data) {
  if (local_app_data.empty()) return {};
  const auto current = local_app_data / product::kUpstreamDataFolders[0];
  const auto prior = prior_standalone_dir(local_app_data);
  const auto upstream = upstream_dir(local_app_data);
  return {{standalone_dir(local_app_data) / "standalone.ini", Encoding::Utf8, false},
          {previous_dir(local_app_data) / "standalone.ini", Encoding::Utf8, false},
          {upstream / "standalone.ini", Encoding::Utf8, false},
          {current / "launcher.ini", Encoding::Ansi, true},
          {(upstream == prior ? current : prior) / "standalone.ini", Encoding::Utf8, false}};
}

// The files of a profile copied forward (copy_forward): the remembered disc, the settings with the
// plugin approvals, Gecko codes and installed skins beside them, and the Slippi user folder. Saved
// games (User/GC), replays, logs and caches stay where they are.
inline const std::vector<const char*>& forward_items() {
  static const std::vector<const char*> items = {"standalone.ini", "port-settings.ini", "plugin-approvals.json",
                                                 "GeckoCodes.ini", "CosmeticMods", "User/Slippi"};
  return items;
}
inline constexpr const char kForwardMarker[] = "copied-forward.txt";

// Once: when this program's folder holds no profile yet and an earlier one does (forward_source:
// Netlee's, else Melee Unlocked's), copies the profile's files (forward_items) into this program's
// folder and writes copied-forward.txt naming where they came from, so it never happens again (not
// even if the player empties the folder). Nothing in the earlier folder is moved, changed or deleted,
// and a file already in this program's folder is never overwritten. Returns the folder copied from,
// or empty when nothing was.
inline fs::path copy_forward(const fs::path& local_app_data) {
  const fs::path own = standalone_dir(local_app_data), from = forward_source(local_app_data);
  std::error_code ec;
  if (own.empty() || from.empty() || fs::exists(own / kForwardMarker, ec) || has_profile(own) || !has_profile(from)) return {};
  fs::create_directories(own, ec);
  int copied = 0;
  for (const char* item : forward_items()) {
    const fs::path source = from / fs::u8path(item), target = own / fs::u8path(item);
    std::error_code e;
    if (fs::is_directory(source, e)) {
      fs::create_directories(target, e);
      fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::skip_existing, e);
      copied += !e;
    } else if (fs::is_regular_file(source, e)) {
      fs::create_directories(target.parent_path(), e);
      copied += fs::copy_file(source, target, fs::copy_options::skip_existing, e) && !e;
    }
  }
  std::ofstream marker(own / kForwardMarker, std::ios::binary | std::ios::trunc);
  marker << "profile copied from " << from.u8string() << " (" << copied << " items); that folder was left as it was"
         << '\n';
  return from;
}

// Records the disc a double-click used in standalone.ini (UTF-8), through a temporary file.
inline void remember(const fs::path& ini, const fs::path& disc) {
  const fs::path tmp = fs::path(ini).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "iso=" << disc.u8string() << "\n";
    if (!f) return;
  }
  std::error_code ec;
  fs::rename(tmp, ini, ec);
}

// The first remembered disc that passes `header` (cheap: a Melee NTSC 1.02 header) and, from a
// place with full_check, `unmodified` (reads the whole disc), or an empty path. `decode` turns an
// ini's bytes into a path in that ini's encoding.
template <class Decode, class Header, class Unmodified>
fs::path first_verified(const std::vector<Place>& from, Decode decode, Header header, Unmodified unmodified) {
  for (const Place& place : from) {
    const std::string raw = read_iso(place.ini);
    if (raw.empty()) continue;
    const fs::path disc = decode(raw, place.encoding);
    if (disc.empty() || !header(disc)) continue;
    if (place.full_check && !unmodified(disc)) continue;
    return disc;
  }
  return {};
}
}  // namespace disc_memory
