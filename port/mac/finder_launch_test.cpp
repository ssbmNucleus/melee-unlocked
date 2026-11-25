// Run from inside a temporary .app, with HOME set to a temporary directory.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "full_launch.h"
#include "../runtime/hle/slippi_account.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
unsigned picker_calls = 0;
fs::path picked;
fs::path picker(const std::string&) { ++picker_calls; return picked; }

int main(int argc, char** argv) {
  assert(argc == 1);
  const fs::path home = std::getenv("HOME");
  if (!fs::is_regular_file(home / "disposable-finder-test")) return 2;
  const fs::path user = home / "Library/Application Support/Melee Unlocked/User/Slippi";
  const fs::path resource_dir = mac_host::resources();
  assert(resource_dir.filename() == "Resources" && fs::is_directory(resource_dir / "Sys"));
  assert(fs::is_regular_file(resource_dir / "melee_game.so"));
  assert(mac_host::full_launch_arguments(argc, argv, picker).empty() && picker_calls == 1);
  picked = home / "My Disc.iso";
  std::string header("GALE01\x00\x02", 8);
  header.resize(28);
  header.append("\xC2\x33\x9F\x3D", 4);
  std::ofstream(picked, std::ios::binary).write(header.data(), header.size());
  auto args = mac_host::full_launch_arguments(argc, argv, picker);
  assert(std::find(args.begin(), args.end(), "--pc-settings") != args.end());
  auto arg = [&](const char* name) {
    auto it = std::find(args.begin(), args.end(), name);
    assert(it != args.end() && ++it != args.end());
    return fs::path(*it);
  };
  assert(arg("--sys-dir") == resource_dir / "Sys" && arg("--iso") == picked && picker_calls == 2);
  assert(arg("--user-dir") == user && arg("--log-file") == user / "melee_port.log");
  assert(arg("--card-dir") == user / "GC/CardA" && arg("--settings-path") == user / "port-settings.ini");
  assert(fs::equivalent(fs::current_path(), user));
  mac_host::remember_disc(user, picked);
  const fs::path original = picked;
  picked.clear();
  args = mac_host::full_launch_arguments(argc, argv, picker);
  assert(arg("--iso") == original && picker_calls == 2);
  fs::remove(original);
  assert(mac_host::full_launch_arguments(argc, argv, picker).empty() && picker_calls == 3);
  auto launcher = home / "Library/Application Support/com.project-slippi.dolphin/Slippi";
  fs::create_directories(launcher);
  std::ofstream(launcher / "user.json") << R"({"uid":"synthetic","playKey":"synthetic-key","displayName":"Test","connectCode":"TEST#1"})";
  assert(slippi::account::resolve(user, slippi::account::launcher_roots()).file == launcher / "user.json");
  assert(!fs::exists(user / "user.json"));
}
