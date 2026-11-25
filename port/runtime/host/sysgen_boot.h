// Shared boot/lookup bridge, used by Source Port and Static Recomp on every host.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <vector>
namespace host {
// Empty when the Sys files are ready. A disc whose own files cannot build them (a modded character select,
// 1.0.0 report) returns the player's text instead of stopping with a crash report; other failures stop.
std::string prepare_sys_assets(const std::string& user_dir, const std::string& data_dir);
std::string sys_game_file_path(const std::string& name);
std::vector<std::string> generated_sys_names();
bool required_sys_asset(const std::string& name);
}  // namespace host
