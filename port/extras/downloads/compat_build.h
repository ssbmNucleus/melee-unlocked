// The build for processors without AVX2, as the old launcher fetched it (taken out of the game's former
// host/updater.cpp, where only the launcher called it). Not built by this repository; see ../README.md.
//
// A release package carries compat.sha256 beside the game, written by tools/package_release.py:
//   "<sha256> MeleeUnlocked-<version>-compat-win64.zip"
// The zip holds melee_port_compat.exe, melee_source_compat.exe, melee_game_compat.dll and
// melee_game_compat.snapexcl. A launcher that finds no AVX2 (../launch/cpu_baseline.h) and
// compat_build_needed() true downloads compat_build_url(), refuses it unless its SHA-256 is the
// recorded one, unpacks it (port/runtime/host/archive_unpack.h), checks the four files are there,
// puts every file beside the game, then writes compat.installed with the SHA-256 and a newline.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace extras::downloads {

inline const char* const kCompatFiles[] = {"melee_port_compat.exe", "melee_source_compat.exe",
                                           "melee_game_compat.dll", "melee_game_compat.snapexcl"};

inline std::string read_small(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), {});
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// compat.sha256 read and checked: a lowercase SHA-256 and MeleeUnlocked-<x.y.z>-compat-win64.zip.
inline bool compat_record(const std::filesystem::path& game_dir, std::string* sha, std::string* file) {
  const std::string line = read_small(game_dir / "compat.sha256");
  const size_t space = line.find(' ');
  if (space != 64 || line.size() <= 65) return false;
  *sha = line.substr(0, 64);
  *file = line.substr(65);
  const std::string prefix = "MeleeUnlocked-", suffix = "-compat-win64.zip";
  if (sha->find_first_not_of("0123456789abcdef") != std::string::npos ||
      file->rfind(prefix, 0) != 0 || file->size() <= prefix.size() + suffix.size() ||
      file->compare(file->size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  const std::string version = file->substr(prefix.size(), file->size() - prefix.size() - suffix.size());
  return version.find_first_not_of("0123456789.") == std::string::npos &&
         std::count(version.begin(), version.end(), '.') == 2 && version.front() != '.' && version.back() != '.';
}

// True when the package names a compat build and the one beside the game is not that one (or is
// incomplete). False for a package with the build inside it, or a source build.
inline bool compat_build_needed(const std::filesystem::path& game_dir) {
  std::string sha, file;
  if (!compat_record(game_dir, &sha, &file)) return false;
  if (read_small(game_dir / "compat.installed") != sha) return true;
  for (const char* name : kCompatFiles) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(game_dir / name, ec)) return true;
  }
  return false;
}

// Where the old launcher fetched it: the version's parts release beside the release itself. A launcher
// hosting its own copies passes its own base ("https://.../download/parts-<version>/" style).
inline std::string compat_build_url(const std::string& file, const std::string& releases_download_base =
    "https://github.com/0xburn/melee-unlocked-slippi-ranked/releases/download/") {
  const std::string version = file.substr(14, file.find("-compat-") - 14);
  return releases_download_base + "parts-" + version + "/" + file;
}

}  // namespace extras::downloads
