// Per-player effect files, the host's half of the prototype (game: sourceport/game/shim/mu_player_fx.c).
// Test switch only: MELEE_TEST_PLAYER_FX="2:C:\path\EfCaData.dat" (port 1-4, then the file; several
// entries separated by ';'), read only in automated runs. Without it every ask is answered "none" and
// the game changes nothing. Never online, in a p2p session or in a replay (`allowed`).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace host::player_fx {
// MU_CMD_MOD_BROWSER 19: {19, op, port (0-3), effect bank, the bank's disc file name ("EfCaData.dat")}.
// op 0: reply 1 and the variant's size (big-endian u32), or 0 for none; op 1: the variant's bytes.
// A variant is served only when it has the disc file's table, the same model effect count, the same
// generator ids and the same texture group count (refused with one log line otherwise).
void command(const uint8_t* p, size_t n, bool allowed, std::vector<uint8_t>& reply);
}  // namespace host::player_fx
