// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice_model.h"

namespace slippi::native_practice {

bool is_offline_gameplay_scene(uint8_t major, uint8_t minor) {
  // TM-CE uses Event Match's alternate major: 0 is its CSS, 2 its stage select,
  // and 1 the running exercise. Only that exercise has a live human fighter.
  if (major == 0x2B) return minor == 1;
  const bool match_mode = is_match_mode_major(major) || major == 0x1C;
  return match_mode && minor >= 2;
}

bool is_match_mode_major(uint8_t major) {
  return major == 0x02 || major == 0x03 || major == 0x04 || major == 0x05 || major == 0x0F ||
         (major >= 0x10 && major <= 0x13) || major == 0x1B;
}

}  // namespace slippi::native_practice
