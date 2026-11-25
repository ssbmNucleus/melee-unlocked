// The public keys a fighter plugin's signature is checked against (plugin_gate.h). A plugin whose
// detached signature verifies against one of these loads without asking the player.
//
// The production key slot is EMPTY on purpose. The owner supplies SSBM Nucleus's real Ed25519 public
// key (32 bytes) here; until then no signature verifies and every plugin needs the player's approval.
// An all-zero slot is skipped, never used as a key: a zero point is a small-order key that some
// signatures would "verify" against.
//
// The test key (port/tests/plugin_test_key, whose private half is in the repository and so proves
// nothing) is trusted only by a host configured with -DMELEE_PLUGIN_TEST_KEY=ON, for the scripted
// runs; --print-info says so (plugin_test_key), and windows-build/client_release.py refuses to
// package such a build.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_gate.h"

namespace plugin_gate {

namespace {
// Nucleus's plugin signing key: left empty until the owner provides it. Do not invent one.
constexpr uint8_t kProductionKey[32] = {};

#ifdef MELEE_PLUGIN_TEST_KEY
// TEST ONLY: port/tests/plugin_test_key/TEST_ONLY_ed25519_public.txt.
constexpr uint8_t kTestKey[32] = {0x76, 0xc5, 0x69, 0x68, 0x36, 0x0a, 0xd8, 0x27, 0x4b, 0x61, 0x04, 0xc5, 0x8a, 0x73, 0x09, 0xf2,
                                  0x73, 0xa6, 0xf8, 0xfc, 0x70, 0x43, 0x2b, 0xc7, 0x36, 0xa6, 0xfc, 0xde, 0xf0, 0x8a, 0x8e, 0xf4};
#endif

bool empty_slot(const uint8_t (&key)[32]) {
  for (uint8_t b : key)
    if (b) return false;
  return true;
}
}  // namespace

const std::vector<Key>& builtin_keys() {
  static const std::vector<Key> keys = [] {
    std::vector<Key> list;
    Key key{};
    if (!empty_slot(kProductionKey)) {
      for (size_t i = 0; i < 32; ++i) key[i] = kProductionKey[i];
      list.push_back(key);
    }
#ifdef MELEE_PLUGIN_TEST_KEY
    for (size_t i = 0; i < 32; ++i) key[i] = kTestKey[i];
    list.push_back(key);
#endif
    return list;
  }();
  return keys;
}

bool builtin_test_key() {
#ifdef MELEE_PLUGIN_TEST_KEY
  return true;
#else
  return false;
#endif
}

}  // namespace plugin_gate
