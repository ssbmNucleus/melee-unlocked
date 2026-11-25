// Host services: how deep the stacks run. The game's own stack past its end (Static Recomp), the game
// thread's deepest point in a session, and the per-scene depth test (MELEE_TEST_STACK_DEPTH). Split from
// host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include <windows.h>
#include "guest_registry.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace host {

// The game's own stack (64 KB on the console) running past its end. Calls that deep also run this
// program's stack out, so the reports of it are all the same exception C00000FD with thirty-two host
// frames and no word on what called itself: by then the game has written over its own data below
// the stack, and the last thing seen is a stray write. This notes the game's call chain the first
// time the stack pointer is found past the end (and again 128 KB further), which is what a later
// crash line needs above it. It changes nothing: the console overruns the same memory the same way.
void note_guest_stack_overrun() {
  constexpr uint32_t kCurrentThread = 0x800000E4u, kStackBase = 0x304u, kStackEnd = 0x308u;
  static uint32_t noted_below = 0;   // how far past the end the last note was, +1
  if (native_retrace || !ram || !cpu) return;   // the Source Port's game runs on this program's stack
  const uint32_t thread = rd32(kCurrentThread);
  if (thread - ppc::RAM_BASE >= ram_size - 0x310u) return;
  // MELEE_TEST_GUEST_STACK_KB=<n> (tests): the stack counts as n KB, so ordinary calls run past it.
  static const uint32_t test_kb = [] { const char* v = host::test_env::getenv("MELEE_TEST_GUEST_STACK_KB"); return v ? (uint32_t)std::atoi(v) : 0u; }();
  const uint32_t base = rd32(thread + kStackBase), sp = cpu->r[1];
  const uint32_t end = test_kb ? base - test_kb * 1024u : rd32(thread + kStackEnd);
  if (end - ppc::RAM_BASE >= ram_size || base <= end || sp >= end || sp - ppc::RAM_BASE >= ram_size) return;
  const uint32_t past = end - sp;
  if (noted_below && past < noted_below - 1 + 128u * 1024u) return;
  noted_below = past + 1;
  // Walk the back chain: each frame holds the caller's frame address, and the caller's return address beside it.
  struct Count { uint32_t function; uint32_t calls; };
  std::vector<Count> counts;
  std::string inner, outer;
  std::vector<uint32_t> chain;
  for (uint32_t at = sp; chain.size() < 100000;) {
    const uint32_t up = rd32(at);
    if (up <= at || up - ppc::RAM_BASE >= ram_size - 8u) break;
    chain.push_back(rd32(up + 4));
    at = up;
  }
  auto function_of = [](uint32_t lr) {
    size_t lo = 0, hi = guest::name_table_count;
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (guest::name_table[mid].addr <= lr) lo = mid + 1; else hi = mid; }
    return lo ? guest::name_table[lo - 1].addr : 0u;
  };
  for (uint32_t lr : chain) {
    const uint32_t f = function_of(lr);
    auto it = std::find_if(counts.begin(), counts.end(), [&](const Count& c) { return c.function == f; });
    if (it == counts.end()) counts.push_back({f, 1}); else ++it->calls;
  }
  std::sort(counts.begin(), counts.end(), [](const Count& a, const Count& b) { return a.calls > b.calls; });
  std::string most;
  for (size_t i = 0; i < counts.size() && i < 6; ++i) { char t[96]; std::snprintf(t, sizeof t, "%s%s x%u", i ? ", " : "", symbol_name(counts[i].function), counts[i].calls); most += t; }
  for (size_t i = 0; i < chain.size() && i < 10; ++i) { inner += i ? " < " : ""; inner += symbol_name(chain[i]); }
  for (size_t i = chain.size() > 6 ? chain.size() - 6 : 0; i < chain.size(); ++i) { outer += outer.empty() ? "" : " < "; outer += symbol_name(chain[i]); }
  uint32_t major = 0, minor = 0, match_frame = 0;
  current_scene(&major, &minor, &match_frame);
  log("stack: the game's own stack is %u KB past its end (%u KB of stack, %zu calls deep, scene %02X:%02X, retrace %u). Most repeated: %s",
      past / 1024, (base - end) / 1024, chain.size(), major, minor, g_retraces, most.c_str());
  log("stack: innermost calls: %s | outermost: %s", inner.c_str(), outer.c_str());
  log_flush();
}

// How deep the game thread's stack has been: the lowest page Windows has had to commit for it. The
// game's model loaders call themselves once per joint and per drawn part, each such call is a host
// call here, and a retrace interrupt that arrives mid-load runs on top of them. With the default
// 1 MB that depth ran out after an online match (exception C00000FD while the next screen loaded),
// though ordinary play uses about a tenth of it. This line says how much a session really used.
void log_stack_peak(bool at_exit) {
  static size_t logged = 0;
  const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const size_t used = (size_t)((uintptr_t)tib->StackBase - (uintptr_t)tib->StackLimit);
  if (!at_exit && used < logged + 128 * 1024) return;   // a new high by 128 KB, so a session logs a handful
  ULONG_PTR low = 0, high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  uint32_t major = 0, minor = 0, match_frame = 0;
  current_scene(&major, &minor, &match_frame);
  logged = std::max(logged, used);
  log("stack: the game thread has used %zu KB of %zu KB at most (scene %02X:%02X)", logged / 1024, (size_t)(high - low) / 1024, major, minor);
  // Interpreters inside each other are what a climbing number has been made of so far: name them.
  uint32_t nest[8][2];
  const int nested = ppc::interpreter_nesting(nest, 8);
  if (nested > 4) {
    std::string line;
    for (int i = 0; i < std::min(nested, 8); ++i) {
      char one[96];
      std::snprintf(one, sizeof one, "%s%08X (%s)", i ? ", " : "", nest[i][0], symbol_name(nest[i][0]));
      line += one;
    }
    log("stack: %d interpreters are running inside each other, outermost first: %s", nested, line.c_str());
  }
}

// MELEE_TEST_STACK_DEPTH=1 (tests): the shallowest point of the game thread's stack seen at a retrace
// during each scene, logged when the scene changes. That is the depth the scene loop itself runs at,
// so a number that climbs from scene to scene is host frames that never returned. Each new high by
// 4 KB is followed by the host return addresses at that point (offsets into this program, for the map).
void test_stack_depth(bool at_exit) {
  static const bool on = [] { const char* v = host::test_env::getenv("MELEE_TEST_STACK_DEPTH"); return v && *v == '1'; }();
  if (!on) return;
  static uint32_t scene = ~0u;
  static size_t least = SIZE_MAX, high = 0;
  static void* frames[4096];
  static USHORT count = 0;
  const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  volatile char here = 0;
  const size_t depth = (size_t)((uintptr_t)tib->StackBase - (uintptr_t)&here);
  if (depth < least) { least = depth; count = RtlCaptureStackBackTrace(0, 4096, frames, nullptr); }
  uint32_t major = 0, minor = 0, match_frame = 0;
  current_scene(&major, &minor, &match_frame);
  const uint32_t now = major << 8 | minor;
  if (now == scene && !at_exit) return;
  if (scene != ~0u && least != SIZE_MAX) {
    log("stack: scene %02X:%02X ran no shallower than %zu bytes, %u host frames", scene >> 8, scene & 0xFF, least, (unsigned)count);
    if (least >= high + 4096) {
      high = least;
      uint32_t nest[64][2];
      const int nested = ppc::interpreter_nesting(nest, 64);
      for (int i = 0; i < std::min(nested, 64); ++i)
        log("stack: interpreter %d of %d started at %08X (%s), returns to %08X (%s)", i + 1, nested, nest[i][0], symbol_name(nest[i][0]), nest[i][1], symbol_name(nest[i][1]));
      const uintptr_t module = (uintptr_t)GetModuleHandleW(nullptr);
      std::string line;
      for (USHORT i = 0; i < count; ++i) {
        char one[24];
        std::snprintf(one, sizeof one, " %zX", (size_t)((uintptr_t)frames[i] - module));
        line += one;
        if (i % 16 == 15 || i + 1 == count) { log("stack frames:%s", line.c_str()); line.clear(); }
      }
    }
  }
  scene = now; least = SIZE_MAX;
}

}  // namespace host
