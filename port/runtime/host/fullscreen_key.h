// Fullscreen shortcuts in the Win32 game window.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>

namespace host {
constexpr bool fullscreen_key(unsigned vk, bool alt, bool ctrl_or_shift,
                              bool keyboard_on_a_port, bool typing, bool repeat) {
  return !repeat && (vk == VK_F11 || (vk == VK_RETURN && alt) ||
                     (vk == 'F' && !alt && !ctrl_or_shift && !keyboard_on_a_port && !typing));
}
} // namespace host
