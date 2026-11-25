// Full host bundle defaults and output isolation. Caller-supplied paths take precedence.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "platform_paths.h"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <string>
#include <vector>
namespace mac_host {
std::filesystem::path choose_disc(const std::string& message);
void show_launch_error(const std::string& message);

inline bool melee_disc_header(const std::filesystem::path& path) {
  unsigned char bytes[32];
  std::ifstream in(path, std::ios::binary);
  return in.read(reinterpret_cast<char*>(bytes), sizeof bytes) &&
         std::string(reinterpret_cast<const char*>(bytes), 6) == "GALE01" &&
         bytes[7] == 2 && bytes[28] == 0xC2 && bytes[29] == 0x33 && bytes[30] == 0x9F && bytes[31] == 0x3D;
}

// Write only after the runtime has successfully opened and verified the disc.
inline void remember_disc(const std::filesystem::path& user, const std::filesystem::path& disc) {
  const auto saved = user / "disc-path";
  const auto pending = user / ("disc-path." + std::to_string(getpid()) + ".tmp");
  std::ofstream out(pending, std::ios::binary);
  out << disc.string();
  out.close();
  std::error_code ec;
  if (out) std::filesystem::rename(pending, saved, ec);
  std::filesystem::remove(pending, ec);
}

inline std::vector<std::string> full_launch_arguments(int argc, char** argv,
    std::filesystem::path (*picker)(const std::string&) = choose_disc) {
  namespace fs = std::filesystem;
  std::vector<std::string> args(argv, argv + argc);
  if (args.size() == 2 && args[1].compare(0, 5, "-psn_") == 0) args.pop_back();
  const bool finder_launch = args.size() == 1;
  if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h" || args[1] == "--version"))
    return args;
  const std::vector<std::string> paths = {"--iso", "-e", "--sysgen-data-dir", "--settings-path", "--sys-dir", "--user-dir", "--replay-dir",
      "--card-dir", "--log-file", "--shader-cache", "--script", "--state-digest", "--state-trace", "--replay",
      "--p2p-identity", "--p2p-result", "--capture", "--capture-frames"};
  auto value = [&](const std::string& name) -> std::string {
    for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == name) return args[i+1];
    return {};
  };
  for (size_t i = 1; i + 1 < args.size(); ++i)
    for (const auto& flag : paths) if (args[i] == flag) { args[i+1] = fs::absolute(fs::u8path(args[i+1])).string(); ++i; break; }
  const auto seed = resources() / "shader-seed";
  if (!std::getenv("MELEE_SHADER_SEED") && fs::is_directory(seed))
    setenv("MELEE_SHADER_SEED", seed.c_str(), 0);
  fs::path user = value("--user-dir");
  if (user.empty()) {
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (data && *data) user = fs::path(data) / "melee-unlocked/User/Slippi";
    else if (home && *home) user = fs::path(home) / "Library/Application Support/Melee Unlocked/User/Slippi";
    else throw std::runtime_error("Cannot locate your Application Support folder.");
    user = fs::absolute(user);
  }
  if (finder_launch) {
    std::ifstream saved(user / "disc-path", std::ios::binary);
    fs::path disc(std::string((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>()));
    std::string message = "Select your Super Smash Bros. Melee NTSC 1.02 ISO or GCM.";
    while (!melee_disc_header(disc)) {
      disc = picker(message);
      if (disc.empty()) return {};
      message = "That file is not a Melee NTSC 1.02 disc. Choose another ISO or GCM.";
    }
    args.emplace_back("--iso");
    args.push_back(disc.string());
    args.emplace_back("--pc-settings");
  }
  auto default_path = [&](const std::string& flag, const fs::path& path) {
    if (value(flag).empty()) { args.push_back(flag); args.push_back(path.string()); }
  };
  default_path("--user-dir", user);
  default_path("--settings-path", user / "port-settings.ini");
  default_path("--card-dir", user / "GC/CardA");
  default_path("--replay-dir", user / "Replays");
  default_path("--log-file", user / "melee_port.log");
  default_path("--shader-cache", user / "shaders");
  default_path("--sys-dir", resources() / "Sys");
  fs::create_directories(user);
  for (const auto& flag : {"--card-dir", "--replay-dir", "--shader-cache"}) fs::create_directories(value(flag));
  for (const auto& flag : {"--settings-path", "--log-file"}) fs::create_directories(fs::path(value(flag)).parent_path());
  // Relative diagnostic and auxiliary writes (including crash reports) belong to this profile.
  fs::create_directories(user / "tmp");
  setenv("TMPDIR", (user / "tmp").c_str(), 1);
  fs::current_path(user);
  return args;
}
}
