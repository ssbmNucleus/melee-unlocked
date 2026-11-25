// Host services: the per-retrace state digest (--state-digest, and Squad Strike's confirmed rows), the
// game's scene as scripts and reports read it, and the other test-run hooks read at each retrace
// (RAM peeks and dumps, widescreen toggles, an early RNG seed). Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#include "squad_digest.h"
#endif
#include <windows.h>
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "exi_slippi.h"
#include "slippi_playback.h"
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace host {

static FILE* g_state_digest = nullptr;

// Test runs (MELEE_DIGEST_RAM=N): every N retraces the state digest also carries a hash of all of main
// RAM, so two runs that must behave the same (a function compiled or run from RAM) are compared on every
// byte the game keeps, not only on the fighters. MELEE_DIGEST_RAM_DUMP=<retrace>:<file> writes RAM at
// that retrace, to find what differs when two hashes do not match.
static uint32_t digest_ram_interval() {
  static const uint32_t n = [] { const char* v = host::test_env::getenv("MELEE_DIGEST_RAM"); return v ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u; }();
  return n;
}
// MELEE_DIGEST_RAM_SKIP=<addr>+<bytes>[,...] leaves words out of the hash: the few that hold host time
// (two identical runs differ only there, e.g. 800030D8+8 and the OS time words in .sbss).
static uint64_t ram_hash() {
  static const std::vector<std::pair<uint32_t, uint32_t>> skip = [] {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (const char* v = host::test_env::getenv("MELEE_DIGEST_RAM_SKIP"))
      for (const char* p = v; *p;) {
        char* end = nullptr;
        const uint32_t a = (uint32_t)std::strtoul(p, &end, 16);
        if (end == p || *end != '+') break;
        const uint32_t n = (uint32_t)std::strtoul(end + 1, &end, 0);
        out.push_back({(a & 0x3FFFFFFFu) & ~7u, ((a & 7u) + n + 7) / 8});   // whole 8-byte words
        p = (*end == ',') ? end + 1 : end;
      }
    return out;
  }();
  const uint64_t* p = reinterpret_cast<const uint64_t*>(ram);
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < ppc::CONSOLE_RAM_SIZE / 8; ++i) {   // the console's memory, as before the extra heap space
    uint64_t w = p[i];
    for (const auto& s : skip) if (i >= s.first / 8 && i < s.first / 8 + s.second) { w = 0; break; }
    h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29;
  }
  return h;
}
// MELEE_TEST_PEEK=<addr>[,<addr>...]@<retrace>: logs those RAM words at that retrace (test runs).
void test_peek_and_dump() {
  static const auto peek = [] {
    std::pair<std::vector<uint32_t>, uint32_t> out{{}, 0};
    const char* v = host::test_env::getenv("MELEE_TEST_PEEK");
    if (!v) return out;
    const char* at = std::strchr(v, '@');
    if (!at) return out;
    out.second = (uint32_t)std::strtoul(at + 1, nullptr, 0);
    for (const char* p = v; p < at;) {
      char* end = nullptr;
      const uint32_t a = (uint32_t)std::strtoul(p, &end, 16);
      if (end == p) break;
      out.first.push_back(a);
      p = (*end == ',') ? end + 1 : end;
    }
    return out;
  }();
  if (!peek.first.empty() && g_retraces == peek.second)
    for (uint32_t a : peek.first) log("peek %08X = %08X", a, try_ptr(a, 4) ? rd32(a) : 0u);
  static const auto dump = [] {
    std::pair<uint32_t, std::string> out{0, {}};
    const char* v = host::test_env::getenv("MELEE_DIGEST_RAM_DUMP");
    if (!v) return out;
    const char* colon = std::strchr(v, ':');
    if (!colon) return out;
    out.first = (uint32_t)std::strtoul(v, nullptr, 0);
    out.second = colon + 1;
    return out;
  }();
  // MELEE_TEST_WIDESCREEN_TOGGLES=<retrace>[,<retrace>...]: flips Slippi widescreen at those retraces,
  // as the settings panel would (a switch during a session, in hidden test runs).
  static const std::vector<uint32_t> toggles = [] {
    std::vector<uint32_t> out;
    if (const char* v = host::test_env::getenv("MELEE_TEST_WIDESCREEN_TOGGLES"))
      for (const char* p = v; *p;) { char* end = nullptr; out.push_back((uint32_t)std::strtoul(p, &end, 0)); if (end == p) break; p = (*end == ',') ? end + 1 : end; }
    return out;
  }();
  if (std::find(toggles.begin(), toggles.end(), g_retraces) != toggles.end()) {
    slippi::request_widescreen(!slippi::widescreen());
    log("test: widescreen switched %s at retrace %u", slippi::widescreen() ? "off" : "on", g_retraces);
  }
  if (!dump.second.empty() && g_retraces == dump.first) {
    if (FILE* f = std::fopen(dump.second.c_str(), "wb")) {
      std::fwrite(ram, 1, ppc::RAM_SIZE, f);
      std::fclose(f);
      log("ram: written to %s at retrace %u", dump.second.c_str(), g_retraces);
    }
  }
}

void init_state_digest() {
  if (!options.state_digest.empty()) {
    g_state_digest = std::fopen(options.state_digest.c_str(), "w");
    if (!g_state_digest) die("cannot open state digest");
    std::fprintf(g_state_digest, "frame,rng,scene");
    for (unsigned slot = 0; slot < 6; ++slot)
      for (const char* field : {"present", "stocks", "action", "anim_frame", "pos_x", "pos_y", "pos_z",
                                "vel_x", "vel_y", "vel_z", "percent", "facing"})
        std::fprintf(g_state_digest, ",p%u_%s", slot, field);
    std::fprintf(g_state_digest, ",scene_major,match_frame");
    if (digest_ram_interval()) std::fprintf(g_state_digest, ",ram");
    std::fputc('\n', g_state_digest);
  }
}

// Just the scene fields, cheap enough to call every retrace when an @scene script needs to know
// when the game reaches a particular mode/state (window.cpp). Shares the same addresses
// digest_state() uses for the full snapshot so the two never disagree about what "scene" means.
void current_scene(uint32_t* major, uint32_t* minor, uint32_t* match_frame) {
  if (native_state_snapshot) {
    MuStatePod state{};
    native_state_snapshot(&state);
    *major = state.scene_major;
    *minor = state.scene;
    *match_frame = state.match_frame;
  } else if (!ram) {
    // No game at all (the launcher's settings window polls the pads every frame): no scene yet.
    *major = *minor = *match_frame = 0;
  } else {
    *major = rd8(0x80479D30u);   // GameRouting::curr_mode (state_machine + 0)
    *minor = rd8(0x80479D33u);   // GameRouting::curr_state_id (state_machine + 3)
    *match_frame = rd32(0x8046B6C4u); // VsSceneController state frame count
  }
}

static void read_digest_state(MuStatePod& state) {
  if (native_state_snapshot) {
    native_state_snapshot(&state);
  } else {
    const uint32_t seed = rd32(0x804D5F94u);
    if (seed && try_ptr(seed, 4)) state.rng = rd32(seed);
    state.scene = rd8(0x80479D33u);         // GameRouting::curr_state_id (state_machine + 3)
    state.scene_major = rd8(0x80479D30u);   // GameRouting::curr_mode (state_machine + 0)
    state.match_frame = rd32(0x8046B6C4u);  // VsSceneController(0x8046B6A0)->state.frame_count (+0x24)
    for (uint32_t slot = 0; slot < 6; ++slot) {
      const uint32_t player = 0x80453080u + slot * 0xE90u;
      if (rd32(player) != 2) continue;
      const uint32_t active = rd8(player + 0xCu);
      const uint32_t gobj = rd32(player + 0xB0u + (active & 1u) * 4u);
      if (!gobj || !try_ptr(gobj, 0x30)) continue;
      const uint32_t fp = rd32(gobj + 0x2Cu);
      if (!fp || !try_ptr(fp, 0x1834)) continue;
      if (rd32(fp) != gobj) continue;
      MuFighterState& f = state.player[slot];
      f.present = 1;
      f.stocks = static_cast<int8_t>(rd8(player + 0x8Eu));
      f.action = rd32(fp + 0x10u);
      f.anim_frame = rd32(fp + 0x894u);
      f.pos_x = rd32(fp + 0xB0u); f.pos_y = rd32(fp + 0xB4u); f.pos_z = rd32(fp + 0xB8u);
      f.vel_x = rd32(fp + 0x80u); f.vel_y = rd32(fp + 0x84u); f.vel_z = rd32(fp + 0x88u);
      f.percent = rd32(fp + 0x1830u);
      f.facing = rd32(fp + 0x2Cu);
    }
  }
}


#if MU_SQUAD
// Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
// Only the squad/local-peer command interceptor calls this. No test hook needed.
static FILE* g_squad_confirmed = nullptr;
static FILE* g_squad_presented_frontier = nullptr;
static FILE* g_squad_late = nullptr;
static squad_digest::Pending g_squad_pending;
static int32_t g_squad_body = -1, g_squad_frontier = -1;
static uint32_t g_squad_match_frame = 0, g_squad_late_same = 0, g_squad_late_unknown = 0;
void squad_digest_observe(int32_t body, int32_t frontier, bool ending) {
  if (!g_state_digest || !native_state_snapshot) return;
  if (!g_squad_confirmed) {
    g_squad_confirmed = std::fopen((options.state_digest + ".confirmed.csv").c_str(), "w");
    g_squad_presented_frontier = std::fopen((options.state_digest + ".presented-frontier.csv").c_str(), "w");
    g_squad_late = std::fopen((options.state_digest + ".late.csv").c_str(), "w");
    if (!g_squad_confirmed || !g_squad_presented_frontier || !g_squad_late) die("cannot open squad digest");
    squad_digest::header(g_squad_confirmed);
    std::fprintf(g_squad_presented_frontier, "frame,match_frame,body_frame,confirmed_frontier\n");
    squad_digest::late_header(g_squad_late);
    std::fflush(g_squad_late);
  }
  if (!ending) {
    MuStatePod state{};
    read_digest_state(state); // exactly the presented digest's source, no new state fields
    squad_digest::LateRow late{};
    switch (g_squad_pending.sample(body, g_retraces, state, &late)) {
    case squad_digest::Late::none: break;
    case squad_digest::Late::same: ++g_squad_late_same; break; // e.g. a selftest rollback
    case squad_digest::Late::unknown: ++g_squad_late_unknown; break;
    case squad_digest::Late::different:
      squad_digest::write_late(g_squad_late, late);
      std::fflush(g_squad_late);
      log("squad digest: confirmed body %d (match frame %u) re-simulated with a different state", late.body,
          late.confirmed_match_frame);
      break;
    }
    g_squad_match_frame = state.match_frame;
    g_squad_body = body;
  }
  // Received inputs are not enough until their rollback has actually run. During
  // catch-up publish only through this completed body, after replacing its row.
  g_squad_frontier = std::min(body, frontier);
  g_squad_pending.confirm(g_squad_frontier, [](const squad_digest::Row& row) {
    squad_digest::write(g_squad_confirmed, row);
  });
  std::fflush(g_squad_confirmed);
  if (ending) {
    g_squad_pending.discard_tail(); // never promote an unconfirmed tail
    g_squad_body = -1; // menu retraces can retain the VS counter; they are not match presentations
    g_squad_match_frame = 0;
    log("squad digest: %u late resims with the same state, %u beyond the kept window",
        g_squad_late_same, g_squad_late_unknown);
    log("squad digest: %u new bodies on a held match frame", g_squad_pending.held());
  }
}
#endif

void digest_state() {
  if (!g_state_digest) return;
  MuStatePod state{};
  read_digest_state(state);
  std::fprintf(g_state_digest, "%u,%08X,%08X", g_retraces, state.rng, state.scene);
  for (const auto& f : state.player) {
    const uint32_t* words = &f.present;
    for (unsigned index = 0; index < 12; ++index) std::fprintf(g_state_digest, ",%08X", words[index]);
  }
  std::fprintf(g_state_digest, ",%08X,%08X", state.scene_major, state.match_frame);
  if (const uint32_t every = digest_ram_interval()) {
    if (g_retraces % every == 0 && !native_state_snapshot) {
      hle::dvd_settle();   // as the state trace does: a read still being copied in is not the game's state yet
      std::fprintf(g_state_digest, ",%016llX", (unsigned long long)ram_hash());
    }
    else std::fprintf(g_state_digest, ",-");
  }
  std::fputc('\n', g_state_digest);
  std::fflush(g_state_digest);
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
  // A separate sidecar preserves the presented CSV schema for all existing tools.
  if (g_squad_presented_frontier && state.match_frame == g_squad_match_frame && state.match_frame) {
    std::fprintf(g_squad_presented_frontier, "%u,%u,%d,%d\n", g_retraces, state.match_frame,
                 g_squad_body, g_squad_frontier);
    std::fflush(g_squad_presented_frontier);
  }
#endif
}

// MELEE_TEST_EARLY_RNG_SEED="<seed>@<retrace>" (M4 diagnostic, off by default): the recompiled
// game's seed is written through HSD_RandSeedPtr at the start of that retrace, before the VI
// interrupt, so both engines reach the pre-match scenes with the same RNG. The native game writes
// the same seed at the same point (mu_entry.c mu_early_rng_seed).
void apply_early_rng_seed() {
  static bool parsed = false, enabled = false;
  static uint32_t seed = 0, at = 0;
  if (!parsed) {
    parsed = true;
    if (const char* text = host::test_env::getenv("MELEE_TEST_EARLY_RNG_SEED")) {
      char* end = nullptr;
      seed = (uint32_t)std::strtoul(text, &end, 0);
      if (end && *end == '@') {
        at = (uint32_t)std::strtoul(end + 1, &end, 0);
        enabled = end && *end == '\0' && at != 0;
      }
    }
  }
  if (!enabled || g_retraces != at) return;
  const uint32_t seed_addr = rd32(0x804D5F94u);   // HSD_RandSeedPtr, as install_rng_seed_hook writes it
  if (seed_addr && try_ptr(seed_addr, 4)) wr32(seed_addr, seed);
  log("rng-seed: early %08X at retrace %u", seed, at);
}

}  // namespace host
