#include "test_env.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_practice.h"

#include "host.h"
#include "practice_cpu.h"
#include "slippi_online.h"
#include "slippi_playback.h"
#include "../gx/render_options.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace slippi::native_practice {
namespace {

constexpr uint32_t kSceneState = 0x80479D30;
constexpr uint32_t kMinorSceneControl = 0x80479D58;
constexpr uint8_t kOnlineMajor = 8;
constexpr uint8_t kTrainingMajor = 0x1C;
// OFST_R13_ONLINE_MODE is -0x5060 from Melee's r13 base (0x804DB6A0).
constexpr uint32_t kOnlineMode = 0x804D6640;

std::mutex g_mutex;
Snapshot g_snapshot;
// After an online session: on the way back to Training (request_major asked for it).
bool g_returning = false;
uint32_t g_return_ticks = 0;
constexpr uint32_t kReturnTimeoutTicks = 600;   // ten seconds of frames
uint32_t g_return_wait_ticks = 0;   // since the return was asked for, whatever scene the game is in

// The Source Port's game answers through its practice bridge (game API 6); the static recomp's
// state is read and written at its console addresses. The numbering is mu_host.h's MU_PRACTICE_*
// (ops the game no longer has keep their numbers).
NativeBridge g_bridge = nullptr;
ContentBridge g_content_bridge = nullptr;
enum { OP_SCENE, OP_GET_PLAYER, OP_SET_PLAYER, OP_GET_STAGE, OP_SET_STAGE, OP_SET_EVENT_BACKUP,
       OP_REQUEST_MAJOR, OP_SET_ONLINE_MODE, OP_GET_ONLINE_MODE, OP_DIRECT_FIRST_MATCH, OP_FRAME_PAUSE, OP_CPU_MOVE,
       OP_TRAINING_PICK, OP_TRAINING_STAGE, OP_UNLOCKED, OP_PAGE_PAUSE, OP_CSS_PICK, OP_FRIENDS_RETURN };
static_assert(OP_REQUEST_MAJOR == 6 && OP_GET_ONLINE_MODE == 8 && OP_FRAME_PAUSE == 10 && OP_CPU_MOVE == 11 &&
              OP_UNLOCKED == 14, "practice op numbering (mu_host.h)");
std::atomic<uint32_t> g_unlocked{0xFFFFFFFFu};   // tick(): the card's unlocked characters, read in Training

uint8_t current_major() {
  if (g_bridge) { int32_t a[2] = {}; g_bridge(OP_SCENE, a, 2); return (uint8_t)a[0]; }
  return host::rd8(kSceneState);
}
uint8_t current_minor() {
  if (g_bridge) { int32_t a[2] = {}; g_bridge(OP_SCENE, a, 2); return (uint8_t)a[1]; }
  return host::rd8(kSceneState + 3);
}
uint8_t online_mode() {
  if (g_bridge) { int32_t a[1] = {}; g_bridge(OP_GET_ONLINE_MODE, a, 1); return (uint8_t)a[0]; }
  return host::rd8(kOnlineMode);
}

bool is_training() { return current_major() == kTrainingMajor; }
bool is_online_scene() { return current_major() == kOnlineMajor; }
// An offline match where the player already picked a character: Training and events on the Source
// Port; VS, Training, single player and events on the Static Recomp.
bool is_offline_gameplay() {
  if (!is_offline_gameplay_scene(current_major(), current_minor())) return false;
  if (g_bridge && current_major() != kTrainingMajor && current_major() != 0x2B) return false;
  return true;
}

// The game's debug pause (gm_80479D58.unk_10.x0 bit 0) and its one-frame step (x2 bit 0).
// pause: 1 freeze, 0 run, -1 leave as is. Returns whether the game is frozen.
bool frame_pause(int pause, bool step) {
  if (g_bridge) { int32_t a[2] = {pause, step ? 1 : 0}; g_bridge(OP_FRAME_PAUSE, a, 2); return a[0] != 0; }
  const uint32_t flags = kMinorSceneControl + 0x10;
  uint8_t v = host::rd8(flags);
  if (pause > 0) v |= 1; else if (pause == 0) v &= (uint8_t)~1u;
  host::wr8(flags, v);
  if (step) host::wr8(flags + 2, (uint8_t)(host::rd8(flags + 2) | 1));
  return (v & 1) != 0;
}

// Moves the CPU fighter by (dx, dy) game units: the native game through its bridge, the Static
// Recomp in console memory (player slots at 0x80453080, 0xE90 each: kind +0x8 (1 CPU), fighter
// object +0xB0; the fighter's data at object +0x2C, its position at data +0xB0, previous +0xBC).
void cpu_move(float dx, float dy) {
  if (g_bridge) { int32_t a[2] = {(int32_t)(dx * 100.0f), (int32_t)(dy * 100.0f)}; g_bridge(OP_CPU_MOVE, a, 2); return; }
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t slot = 0x80453080u + i * 0xE90u;
    if (host::rd32(slot + 8) != 1) continue;
    const uint32_t gobj = host::rd32(slot + 0xB0);
    if (gobj < 0x80000000u || gobj >= 0x81800000u) continue;
    const uint32_t fp = host::rd32(gobj + 0x2C);
    if (fp < 0x80000000u || fp >= 0x81800000u) continue;
    for (int axis = 0; axis < 2; ++axis) {
      uint32_t bits = host::rd32(fp + 0xB0 + axis * 4);
      float v; std::memcpy(&v, &bits, 4);
      v += axis ? dy : dx;
      std::memcpy(&bits, &v, 4);
      host::wr32(fp + 0xB0 + axis * 4, bits);
      host::wr32(fp + 0xBC + axis * 4, bits);
      if (axis == 0 && host::test_env::getenv("MELEE_TEST_FRAME_ADVANCE")) host::log("practice: CPU moved to x %.1f", v);
    }
    return;
  }
}

// Frame advance (Practice > Tools): in an offline Training match, L+R+Z freezes the game and
// unfreezes it; while frozen Z steps one frame, and held Z keeps stepping (every fourth frame
// after a short wait). Anything else (leaving Training, an online session, the option off)
// lets the game run again.
void frame_advance_tick() {
  static bool frozen = false, combo_was = false, z_was = false;
  static uint32_t z_held = 0;
  // Tests (hidden VS matches): MELEE_TEST_FRAME_ADVANCE=1 turns the option on outside Training.
  static const bool test = host::test_env::getenv("MELEE_TEST_FRAME_ADVANCE") != nullptr;
  const bool allowed = ((practice_cpu::config().tools & MU_PC_TOOL_FRAME_ADVANCE) || test) && (is_training() || test) &&
                       (test ? is_offline_gameplay_scene(current_major(), current_minor()) : is_offline_gameplay()) && !g_returning &&
                       !slippi::playback::enabled() && slippi::online::session_mode() < 0;
  if (!allowed) {
    if (test) {   // tests: say which condition holds frame advance off when L+R+Z is pressed
      host::PadState pads[4];
      host::input_last_pads(pads);
      static uint32_t told = 0;
      if ((pads[0].button & 0x0070u) == 0x0070u && told++ < 3)
        host::log("practice: frame advance held off: scene %02X/%02X returning %d playback %d session %d",
                  current_major(), current_minor(), (int)g_returning, (int)slippi::playback::enabled(),
                  slippi::online::session_mode());
    }
    if (frozen) { frame_pause(0, false); frozen = false; host::log("practice: frame advance off, the game runs"); }
    combo_was = z_was = false;
    return;
  }
  host::PadState pads[4];
  host::input_last_pads(pads);
  bool combo = false, z = false;
  for (const host::PadState& p : pads) {
    if ((p.button & 0x0070u) == 0x0070u) combo = true;           // L + R + Z
    else if ((p.button & 0x0010u) && !(p.button & 0x0060u)) z = true;
  }
  // Tests: MELEE_TEST_FRAME_ADVANCE=<freeze retrace>:<step retrace>,... presses the buttons itself.
  if (test) {
    static uint32_t freeze_at = 0; static std::vector<uint32_t> steps; static bool parsed = false;
    if (!parsed) {
      parsed = true;
      const std::string v = host::test_env::getenv("MELEE_TEST_FRAME_ADVANCE");
      freeze_at = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
      for (size_t at = v.find(':'); at != std::string::npos; at = v.find(',', at + 1))
        steps.push_back((uint32_t)std::strtoul(v.c_str() + at + 1, nullptr, 10));
    }
    const uint32_t now = host::retrace_count();
    if (now == freeze_at) combo = true;
    for (uint32_t s : steps) if (now == s) z = true;
  }
  if (combo && !combo_was) {
    frozen = !frozen;
    frame_pause(frozen ? 1 : 0, false);
    host::log("practice: frame advance %s", frozen ? "froze the game" : "let the game run");
  }
  combo_was = combo;
  if (frozen) {
    if (frame_pause(-1, false) == false) frame_pause(1, false);   // keep it frozen
    z_held = z ? z_held + 1 : 0;
    if (z && (!z_was || (z_held > 20 && z_held % 4 == 0))) frame_pause(1, true);
    // While frozen the D-pad places the CPU (a step on each press, then quickly while held).
    static uint32_t dpad_held = 0;
    uint16_t dpad = 0;
    for (const host::PadState& p : pads) dpad |= p.button & 0x000Fu;
    if (test && host::retrace_count() >= 2265 && host::retrace_count() <= 2285) dpad |= 0x2;   // tests: hold right
    dpad_held = dpad ? dpad_held + 1 : 0;
    if (dpad && (dpad_held == 1 || (dpad_held > 15 && dpad_held % 2 == 0))) {
      const float step = 2.0f;
      cpu_move((dpad & 0x2) ? step : (dpad & 0x1) ? -step : 0.0f, (dpad & 0x8) ? step : (dpad & 0x4) ? -step : 0.0f);
      frame_pause(1, true);   // one frame so the move shows (and the game settles the fighter there)
    }
  }
  z_was = z;
}

void request_major(uint8_t major) {
  // Native equivalents of Scene_SetNextMajor, Scene_ExitMajor and Scene_ExitMinor. The last write
  // makes the current minor run its decide path on the next guest frame; writing only the major
  // pending flag would leave an endless Training minor alive.
  if (g_bridge) { int32_t a[1] = {major}; g_bridge(OP_REQUEST_MAJOR, a, 1); return; }
  host::wr8(kSceneState + 1, major);
  host::wr8(kSceneState + 12, 1);
  host::wr32(kMinorSceneControl + 12, 1);
}

void publish_snapshot() {
  Snapshot out;
  out.in_practice = is_training();
  out.scene_major = current_major();
  out.scene_minor = current_minor();
  out.unlocked = g_unlocked.load(std::memory_order_relaxed);
  out.returning = g_returning;
  const bool playback = slippi::playback::enabled();
  const bool online_match = slippi::online::is_online_match() || is_online_scene();
  const bool session_active = slippi::online::session_mode() >= 0;
  out.tab_available = !g_returning && !playback && !online_match && !session_active && is_offline_gameplay();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_snapshot = out;
}

}  // namespace

Snapshot snapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_snapshot;
}

void tick() {
  frame_advance_tick();

  // Which characters the memory card has, once a second in Training (a game library without the
  // question leaves it unknown).
  {
    static int unlocked_ticks = 0;
    if (g_bridge && is_training() && ++unlocked_ticks >= 60) {
      unlocked_ticks = 0;
      int32_t a[1] = {0};
      if (g_bridge(OP_UNLOCKED, a, 1) == 0) g_unlocked.store((uint32_t)a[0], std::memory_order_relaxed);
    }
  }

  // "After an online session ends, go back to Training mode" (Game tab, Online): when the session
  // ends with at least one match played (the other player left, Z held to leave, the connection
  // dropped) and the player is still on the online screens, Training's character select. Only once
  // the session is over, so nothing changes during a match or between a pair's games.
  {
    static bool played = false, session_was = false;
    static int session_mode_seen = -1;
    const bool session = slippi::online::session_mode() >= 0;
    const bool match_running = slippi::online::is_online_match();
    const bool session_ended = session_was && !session;
    session_was = session;
    if (session) session_mode_seen = slippi::online::session_mode();
    if (session && match_running) played = true;
    if (session_ended && played) {
      played = false;
      // The guest mode also covers local Ranked practice, which has no real
      // matchmaking session and must not inherit an earlier session's return.
      const bool allow_training = session_mode_seen != 0 && online_mode() != 0;
      if (allow_training && gx::RenderOptions::live_online_return_training() && !g_returning && is_online_scene()) {
        host::log("native practice: online session over; going to Training (setting on)");
        if (g_content_bridge) g_content_bridge(-1, true);
        request_major(kTrainingMajor);
        g_returning = true;
        g_return_ticks = 0;
        g_return_wait_ticks = 0;
        host::log("native practice: returning to offline scene %02X", kTrainingMajor);
      }
    }
  }

  if (g_returning) {
    if (current_major() == kTrainingMajor) ++g_return_ticks;
    else g_return_ticks = 0;
    if (g_return_ticks >= 3) {
      g_returning = false;
      host::log("native practice: offline scene %02X restored", kTrainingMajor);
    }
    // The return is a request to the game, and the player can leave for somewhere else before it
    // lands (or it never lands). Time spent in Training itself is not counted.
    else if (slippi::online::is_online_match() ||
             (current_major() != kTrainingMajor && ++g_return_wait_ticks > kReturnTimeoutTicks)) {
      host::log("native practice: return to offline scene %02X given up (%s)", kTrainingMajor,
                slippi::online::is_online_match() ? "an online match started" : "timed out");
      g_returning = false;
    }
  }

  publish_snapshot();
}

void shutdown() {
  if (g_content_bridge) {
    g_content_bridge(-1, true);
    g_content_bridge = nullptr;
  }
  g_returning = false;
  // The launcher's settings window shuts Slippi down without ever loading the game, so there is no
  // guest RAM to read the scene from; publishing then crashed every close of that window.
  if (host::ram) publish_snapshot();
}

void set_native_bridge(NativeBridge bridge) { g_bridge = bridge; }
void set_content_bridge(ContentBridge bridge) { g_content_bridge = bridge; }

}  // namespace slippi::native_practice
