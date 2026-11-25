// Host services: real time beside console time. Sleeps that are measured before they are trusted and
// the wait for the next tick, the console's clock (OSGetTime with the date), audio blocks at their real
// times during the frame wait, and the latency and lag test switches. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <thread>

namespace host {

// ---- audio blocks at their 5 ms times during the frame wait (Static Recomp) ----
// The scene loop waits for the next frame by asking lb_80019894 how many pad samples are queued and
// polling until there is one. On the console the audio interface interrupt fires every 5 ms during
// that wait and the DSP renders one block each time; here console time used to run straight to the
// retrace in small poll steps, so a frame's three or four blocks went out together (a ~17 ms burst
// the output buffer had to absorb, the main cost in sound latency). While the answer is "none yet",
// wait for the next block's real time and play it; the retrace, the pad sample and frame timing are
// unchanged. The Source Port does the same in its scene loop (mu_audio_idle).
namespace {
ppc::Fn g_pad_queue_count = nullptr;
void pad_queue_count_paced(ppc::Context& c, uint8_t* m) {
  g_pad_queue_count(c, m);
  static const bool enabled = [] { const char* v = std::getenv("MELEE_AUDIO_PACING"); return !(v && *v == '0'); }();
  static const bool trace = [] { const char* v = std::getenv("MELEE_AUDIO_PACING_TRACE"); return v && *v == '1'; }();
  static uint64_t calls, has_pad, irq_off, no_due, past, steps;
  if (trace && ++calls % 600 == 0)
    log("audio pacing (recomp): %llu calls, %llu steps; pad queued %llu, interrupts off %llu, not due/none %llu, past retrace %llu",
        (unsigned long long)calls, (unsigned long long)steps, (unsigned long long)has_pad, (unsigned long long)irq_off,
        (unsigned long long)no_due, (unsigned long long)past);
  if (c.r[3] != 0) { ++has_pad; return; }
  if (!enabled || options.fast) return;
  if (!ppc::interrupts_on(c)) { ++irq_off; return; }
  const uint64_t due = hle::audio_next_due();
  if (!due || c.tb >= due) { ++no_due; return; }
  if (!wait_until_console_time(due)) { ++past; return; }   // due after the retrace: the retrace wait covers it
  hle::audio_tick(false);
  ++steps;
}
}  // namespace
ppc::Fn g_idle_poll = nullptr;
// lb_800195D0: the scene loop's idle poll. With Slippi's codes the wait loop around it is theirs, so
// pace here: when no pad sample is queued yet (the game is waiting), play the next block at its time.
void idle_poll_paced(ppc::Context& c, uint8_t* m) {
  static const bool trace = [] { const char* v = std::getenv("MELEE_AUDIO_PACING_TRACE"); return v && *v == '1'; }();
  static uint64_t calls;
  if (trace && ++calls % 600 == 0) log("audio pacing (recomp): idle poll %llu calls", (unsigned long long)calls);
  if (g_pad_queue_count) {
    const ppc::Context saved = c;
    g_pad_queue_count(c, m);          // how many pad samples are queued (reads only)
    const uint32_t queued = c.r[3];
    const uint64_t tb = c.tb;
    c = saved; c.tb = tb;
    if (queued == 0) {
      static const bool enabled = [] { const char* v = std::getenv("MELEE_AUDIO_PACING"); return !(v && *v == '0'); }();
      const uint64_t due = hle::audio_next_due();
      if (enabled && !options.fast && ppc::interrupts_on(c) && due && c.tb < due && wait_until_console_time(due))
        hle::audio_tick(false);
    }
  }
  g_idle_poll(c, m);
}
// OSGetTime is the console's time since 2000-01-01: the boot ROM loads the timebase register from
// the clock before a game starts. Here the timebase starts at zero and the date sits in the OS
// adjust at 0x800030D8, which only __OSGetSystemTime adds. So everything that turned OSGetTime
// into a date read January 1, 2000 at every start: the date in the save file's comment, and a mod
// stage that shows a clock or changes with the season.
// Every caller gets the dated time now, except __OSGetSystemTime (8034C410..8034C474), which adds
// the adjust itself. The same three timebase reads as the game's function, so emulated time
// advances exactly as it did; differences between two readings are unchanged.
static void os_get_time_dated(ppc::Context& c, uint8_t*) {
  ppc::enter(c, 0x8034C3F0u);
  uint32_t hi, lo, again;
  do {
    hi = (uint32_t)(ppc::read_tb(c) >> 32);
    lo = (uint32_t)ppc::read_tb(c);
    again = (uint32_t)(ppc::read_tb(c) >> 32);
  } while (hi != again);
  ppc::cr_set_s(c, 0, (int32_t)hi, (int32_t)again);
  uint64_t time = ((uint64_t)hi << 32) | lo;
  if (c.lr < 0x8034C410u || c.lr >= 0x8034C474u)
    time += ((uint64_t)rd32(0x800030D8u) << 32) | rd32(0x800030DCu);
  c.r[3] = (uint32_t)(time >> 32); c.r[4] = (uint32_t)time; c.r[5] = again;
}
// The console clock when the game starts, in timebase ticks since 2000-01-01 (--time-base presets
// it, which is what keeps a test run repeatable). Dolphin presets the timebase from the RTC the
// same way. The console's clock holds local time, so this is the local wall-clock reading taken as
// if it were UTC: a stage that shows the time of day, or a save's date, reads it from here.
uint64_t console_epoch_ticks() {
  static const uint64_t value = [] {
    if (options.time_base) return options.time_base;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    const long long local_secs = _mkgmtime64(&local);
    const long long GC_EPOCH = 946684800ll;
    return (uint64_t)(local_secs > GC_EPOCH ? local_secs - GC_EPOCH : 0) * TB_HZ;
  }();
  return value;
}
void install_console_clock() {
  if (!ppc::redirect_to_host(0x8034C3F0u, os_get_time_dated)) log("clock: OSGetTime keeps the time since start (not replaced)");
}

void install_audio_pacing() {
  if (ppc::Fn previous = ppc::set_hook(0x80019894u, pad_queue_count_paced)) g_pad_queue_count = previous;
  else log("audio: frame-wait pacing not installed (lb_80019894 not found)");
  if (ppc::Fn previous = ppc::set_hook(0x800195D0u, idle_poll_paced)) g_idle_poll = previous;
  else log("audio: frame-wait pacing not installed (lb_800195D0 not found)");
}

bool latency_trace_enabled() {
  static const bool enabled = [] { const char* v = host::test_env::getenv("MELEE_TRACE_LATENCY"); return v && *v && *v != '0'; }();
  return enabled;
}
int lagtest_flash_mode() {
  static const int mode = [] {
    const char* v = std::getenv("MELEE_LAGTEST_FLASH");
    const int m = !v ? 0 : !std::strcmp(v, "report") ? 1 : !std::strcmp(v, "tick") ? 2 : 0;
    if (m) log("lag test ledger: the picture turns white on X and black on A %s", m == 1 ? "when the adapter's report arrives (past the game)" : "on the frame of the tick that read it");
    return m;
  }();
  return mode;
}
static std::atomic<int> g_lagtest_fill{-1};
static std::atomic<double> g_lagtest_since{0.0};
void lagtest_flash_report(int fill) {
  if (g_lagtest_fill.load(std::memory_order_relaxed) == fill) return;
  g_lagtest_since.store(now_seconds(), std::memory_order_relaxed);
  g_lagtest_fill.store(fill, std::memory_order_release);
}
int lagtest_flash_report_fill(double* since) {
  const int fill = g_lagtest_fill.load(std::memory_order_acquire);
  if (since) *since = g_lagtest_since.load(std::memory_order_relaxed);
  return fill;
}

// ---- sleeps that can be trusted ----
// A report: the game ran at a steady 48 Hz, every frame 20 to 23 ms long, with almost none of that
// time spent on game work. 48 Hz is three frames per four ticks of Windows' default 15.6 ms timer:
// on that system the frame wait's short sleeps were ending on the coarse tick instead of on time
// (a power saving state can do this to a process whatever resolution it asked for). The frame wait
// has a short sleep before each 5 ms audio step, and one that overshoots past the frame's end makes
// the whole frame late.
// So sleeping is trusted only as far as it is measured: a helper thread times a 1 ms sleep four
// times a second, with the same call the frame wait uses, and publishes how late it wakes. The
// frame wait sleeps only when there is room for that lateness and spins the rest. On a healthy
// system the lateness is a few hundred microseconds and nothing changes.
// MELEE_TEST_COARSE_SLEEP=1 makes every such sleep end on the next 15.625 ms boundary, to stand in
// for such a system; =2 does the same with the measurement ignored, which is the old behaviour.
static std::atomic<double> g_sleep_slack{0.0};   // seconds a timed sleep wakes late by, as last measured
static int coarse_sleep_test() {
  static const int mode = [] { const char* v = host::test_env::getenv("MELEE_TEST_COARSE_SLEEP"); return v ? std::atoi(v) : 0; }();
  return mode;
}
// One timed sleep of about `seconds`: the high resolution timer, or a millisecond sleep before
// Windows 1803. False when there is no timer and the time is too short to hand to Sleep.
static bool timed_sleep(HANDLE timer, double seconds) {
  if (coarse_sleep_test()) {
    const double tick = 0.015625;
    const double until = std::ceil((now_seconds() + seconds) / tick) * tick;
    for (double left = until - now_seconds(); left > 0.0; left = until - now_seconds()) {
      LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(std::max(left - 0.0004, 0.0001) * 1e7);
      if (left > 0.0008 && timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
      else YieldProcessor();
    }
    return true;
  }
  LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(seconds * 1e7);
  if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); return true; }
  if (seconds > 0.0016) { Sleep(1); return true; }
  return false;
}
static void sleep_probe_start() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // measured as the simulation thread sleeps
      const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
      double late[8] = {};
      bool coarse = false;
      for (unsigned n = 0;; ++n) {
        const double before = now_seconds();
        timed_sleep(timer, 0.001);
        late[n % 8] = std::max(0.0, now_seconds() - before - 0.001);
        // The median of the last eight: one sleep cut into by another program is not the system's timer.
        double sorted[8]; std::copy(std::begin(late), std::end(late), sorted); std::sort(std::begin(sorted), std::end(sorted));
        const double typical = n < 7 ? sorted[7] : sorted[4];
        g_sleep_slack.store(typical > 0.0007 ? typical : 0.0, std::memory_order_relaxed);
        const bool now_coarse = typical > 0.002;
        if (now_coarse != coarse && n >= 7) {
          coarse = now_coarse;
          if (coarse) log("timing: sleeps on this system wake %.1f ms late (a power saving state does this); the frame wait spins instead of sleeping so the game keeps 60 Hz", typical * 1000.0);
          else log("timing: sleeps wake on time again; the frame wait sleeps as usual");
        }
        Sleep(250);
      }
    }).detach();
  });
}
static std::atomic<double> g_game_rate{0.0};
void set_game_rate(double hz) { g_game_rate.store(hz, std::memory_order_relaxed); }
double game_rate() { return g_game_rate.load(std::memory_order_relaxed); }
double sleep_slack() {
  sleep_probe_start();
  return coarse_sleep_test() == 2 ? 0.0 : g_sleep_slack.load(std::memory_order_relaxed);
}

// Every input waits for the next tick, so the tick wakes on time rather than on the millisecond
// sleep granularity (0.7 ms late on average, 1.2 ms at p95): a high resolution timer to just
// before the deadline, then a short spin. Without the high resolution timer (Windows before 1803)
// a millisecond sleep stands in for it. The sleep is as long as the measured lateness leaves room
// for (sleep_slack): on a system whose sleeps overshoot, the wait spins instead.
void wait_for_tick(std::chrono::steady_clock::time_point deadline) {
  static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  for (;;) {
    const double remaining = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0.0) return;
    const double slack = sleep_slack();
    if (remaining > 0.0006 + slack) {
      if (!timed_sleep(timer, remaining - 0.0004 - slack)) YieldProcessor();
    } else {
      YieldProcessor();
    }
  }
}

}  // namespace host
