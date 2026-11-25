// SPDX-License-Identifier: GPL-2.0-or-later
// NUCLEUS_PUBLIC_BUILD (a CMake option of the host and of the game library, off by default; see
// docs/client-release.md): the build that goes to players. Off, everything behaves as it always has,
// for development and every test. On:
// - the launch options marked test-only in port/app/main.cpp's table are left out of --help and
//   refused (--online-test, --local-peer, --allow-matchmaking, --test-stage, --squad-test-invalid-lineup);
// - every host::test_env read (test_env.h) and the game's mu_test_getenv answer nothing, whatever the
//   environment holds, and MELEE_GAME_DLL is ignored (port/tests/raw_env_reads_test.py keeps every
//   test switch behind them);
// - the log names other players and lobbies by a short hash (log_id below; the game's mu_log_id);
// - --print-info says "public_build": true (and "game_public_build" for the game library beside it),
//   which windows-build/client_release.py requires.
// MELEE_AGENT_COPY (test_env::agent_copy) keeps working in both: it only ever takes things away.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace host::public_build {
#ifdef NUCLEUS_PUBLIC_BUILD
inline constexpr bool kOn = true;
#else
inline constexpr bool kOn = false;
#endif

// Another player's connect code, name or invite code, or a lobby's code, as melee_port.log writes it
// (sourceport/game/include/mu_log_id.h is the game library's copy of the same hash). A development build
// writes the text itself. A public build writes "id-" and six hex digits of a fixed hash of it: the
// same player gives the same mark in every log, which is enough to follow one opponent through a
// connection or a desync, and a log a player posts for help does not name the people they played.
inline std::string log_id(std::string_view text) {
  if (!kOn || text.empty()) return std::string(text);
  uint32_t h = 2166136261u;   // FNV-1a
  for (const unsigned char c : text) { h ^= c; h *= 16777619u; }
  char out[16];
  std::snprintf(out, sizeof out, "id-%06x", (unsigned)(h & 0xFFFFFFu));
  return out;
}
}  // namespace host::public_build
