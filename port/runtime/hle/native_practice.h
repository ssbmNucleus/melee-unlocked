// The Training tools' host side (frame advance, the card's unlocked characters) and the way back to
// Training after an online session ("After an online session ends, go back to Training mode", Game
// tab). Renderers read snapshots; tick() runs on the simulation thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_practice_model.h"

#include <cstdint>

namespace slippi::native_practice {

struct Snapshot {
  bool in_practice = false;
  uint8_t scene_major = 0, scene_minor = 0;   // the game's scene as of the last tick
  // The Practice menu can open: an offline match (Training, events), no online session, no replay.
  bool tab_available = false;
  bool returning = false;     // on the way back to Training after an online session
  // Characters the memory card has unlocked, a bit per character kind (all set when not known, and on
  // the Static Recomp).
  uint32_t unlocked = 0xFFFFFFFFu;
};

Snapshot snapshot();

// Called once per 60 Hz retrace, before the guest advances its frame.
void tick();
void shutdown();

// The Source Port game's practice operations (MuGameApi::practice, MU_PRACTICE_* in mu_host.h).
// Unset, the coordinator works on the static recomp's console memory.
using NativeBridge = int32_t (*)(int32_t op, int32_t* args, int32_t count);
void set_native_bridge(NativeBridge bridge);

// Source-only host content policy, simulation thread: the way back to Training puts the offline
// files back (mode -1, activate). Unset on Static.
using ContentBridge = void (*)(int mode, bool activate);
void set_content_bridge(ContentBridge bridge);

}  // namespace slippi::native_practice
