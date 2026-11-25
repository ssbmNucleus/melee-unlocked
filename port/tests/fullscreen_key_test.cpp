// SPDX-License-Identifier: GPL-2.0-or-later
#include "fullscreen_key.h"
#include <cstdlib>

static void expect(bool value) { if (!value) std::abort(); }

int main() {
  using host::fullscreen_key;
  expect(fullscreen_key(VK_F11, false, false, false, false, false));
  expect(fullscreen_key(VK_F11, true, true, true, true, false));
  expect(!fullscreen_key(VK_F11, false, false, false, false, true));

  expect(fullscreen_key(VK_RETURN, true, false, true, true, false));
  expect(!fullscreen_key(VK_RETURN, true, false, false, false, true));
  expect(!fullscreen_key(VK_RETURN, false, false, false, false, false));
  expect(!fullscreen_key('A', true, false, false, false, false));

  expect(fullscreen_key('F', false, false, false, false, false));
  expect(!fullscreen_key('F', true, false, false, false, false));
  expect(!fullscreen_key('F', false, true, false, false, false));
  expect(!fullscreen_key('F', false, false, true, false, false));
  expect(!fullscreen_key('F', false, false, false, true, false));
  expect(!fullscreen_key('F', false, false, false, false, true));
}
