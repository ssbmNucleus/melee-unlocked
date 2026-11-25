// The game's optional part (AMD FSR), its own download behind a button in PC settings: fetched from the
// addresses in the one table at the top of optional_downloads.cpp (kPinned), each file checked against
// its SHA-256 before it goes beside the game, and loaded at the game's next start. Everything
// network-side runs on a background thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace host::downloads {
// Where an optional part stands, for its button.
enum class ModelState { Missing, Present, Downloading, Failed };

// AMD FSR (amd_fidelityfx_loader_dx12/vk.dll and amd_fidelityfx_upscaler_dx12/vk.dll), its own download:
// AMD's signed files from their FSR SDK repository at a pinned tag (nothing re-hosted), each checked
// against its SHA-256 before it goes beside the game; the game loads them at its next start.
bool fsr_present(const std::string& game_dir_utf8);
void fsr_download(const std::string& game_dir_utf8);
ModelState fsr_state(const std::string& game_dir_utf8);
std::string fsr_message();
constexpr const char* kFsrSize = "about 160 MB";

// Before the process exits: joins every finished download thread; one still running is left to end
// with the process, so quitting never waits minutes for a large download.
void shutdown();
}  // namespace host::downloads
