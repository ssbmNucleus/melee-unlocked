// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace host::test_env {
inline bool agent_copy() {
  // Keep the account lock for automation launched with an older configuration.
  for (const char* flag : {"MELEE_AGENT_COPY", "YOUGAME_AGENT_COPY"}) {
    const char* value = std::getenv(flag);
    if (value && std::strcmp(value, "1") == 0) return true;
  }
  const char* legacy_label = std::getenv("YOUGAME_TEST_LABEL");
  return legacy_label && *legacy_label;
}
// The test hooks' one way into the environment (MELEE_TEST_* and the other diagnostics). In a public
// build (NUCLEUS_PUBLIC_BUILD, public_build.h) it answers nothing, so no hook can be switched on.
#ifdef NUCLEUS_PUBLIC_BUILD
inline const char* getenv(const char*) { return nullptr; }
#else
inline const char* getenv(const char* name) { return std::getenv(name); }
#endif
// A run started by automation (--hidden, --headless or --script), set by main() before anything
// reads the environment. The overrides that send the player's data to another server (a stand-in
// for the mod site or for Slippi's matchmaking) are read only then: a variable left in a player's
// environment does nothing. A public build reads none of them.
inline bool automated = false;
#ifdef NUCLEUS_PUBLIC_BUILD
inline const char* automated_getenv(const char*) { return nullptr; }
#else
inline const char* automated_getenv(const char* name) { return automated ? std::getenv(name) : nullptr; }
#endif
// Whether a host named in such an override is this computer: 127.0.0.0/8, ::1 or localhost.
inline bool loopback_host(const char* host, size_t length) {
  if (length == 9 && std::strncmp(host, "localhost", 9) == 0) return true;
  if (length == 3 && std::strncmp(host, "::1", 3) == 0) return true;
  if (length < 4 || std::strncmp(host, "127.", 4) != 0) return false;
  int dots = 0;
  for (size_t i = 0; i < length; ++i) {
    if (host[i] == '.') ++dots;
    else if (host[i] < '0' || host[i] > '9') return false;
  }
  return dots == 3;
}
} // namespace host::test_env
