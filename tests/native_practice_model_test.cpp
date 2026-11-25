// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice_model.h"

#include <cstdio>

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
    return 1; \
  } \
} while (false)

int main() {
  CHECK(slippi::native_practice::is_match_mode_major(0x02));    // VS
  CHECK(slippi::native_practice::is_match_mode_major(0x03));    // Classic
  CHECK(slippi::native_practice::is_match_mode_major(0x1B));
  CHECK(!slippi::native_practice::is_match_mode_major(0x1C));   // Training
  CHECK(!slippi::native_practice::is_match_mode_major(0x2B));   // events
  CHECK(!slippi::native_practice::is_match_mode_major(0x01));   // the main menu
  CHECK(!slippi::native_practice::is_match_mode_major(0x08));   // online
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x02, 2));
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x1C, 2));
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x02, 0));
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x01, 0x02));  // menu shell
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x02, 0x00));  // CSS
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x02, 0x01));  // stage select
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x02, 0x02));   // versus
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x12, 0x02));   // event match
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x1C, 0x02));   // training
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x08, 0x02));  // online
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x2B, 0x00));  // TM-CE CSS
  CHECK(slippi::native_practice::is_offline_gameplay_scene(0x2B, 0x01));   // TM-CE exercise
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x2B, 0x02));  // TM-CE stage select
  CHECK(!slippi::native_practice::is_offline_gameplay_scene(0x2B, 0x03));  // unknown TM-CE scene
  return 0;
}
