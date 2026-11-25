// Pure scene classification for the Training tools (native_practice.cpp).
// Kept independent of the Windows renderer and Slippi sockets so it can be unit tested anywhere.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace slippi::native_practice {

// Matches the runtime's established scene classification for offline gameplay. CSS, stage select,
// results/menu shells, Slippi online, and replay playback are gated separately.
bool is_offline_gameplay_scene(uint8_t major, uint8_t minor);
// A major scene of a match mode other than Training and events (VS and its kinds, Classic, Adventure,
// All-Star, the multi-man and Home-Run modes...): any of its minors (its selects, its match, results).
bool is_match_mode_major(uint8_t major);

}  // namespace slippi::native_practice
