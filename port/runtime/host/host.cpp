// Host services, the core: the recompiled guest's frame loop. Events and interrupt delivery at the
// guest's wait points, console time, the VI retrace with its pacing and cost accounting, guest calls
// from the host, MMIO and the GX FIFO sink. The other host services are in host_*.cpp beside this
// file (logging, exit, memory, disc, boot, ...); what they share is in host_internal.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include "sim_work.h"
#include "audio.h"
#include <windows.h>
#include "gx_core.h"
#include "authored_pose.h"
#include "window.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "exi_slippi.h"
#include "slippi_playback.h"
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace host {

Options options;
uint8_t* ram = nullptr;
uint32_t ram_size = ppc::RAM_SIZE;
uint8_t* aram = nullptr;
ppc::Context* cpu = nullptr;

FILE* g_state_trace = nullptr;
static std::deque<Completion> g_completions;
static bool g_pe_finish_pending = false;
static bool g_pe_token_pending = false;
static uint16_t g_pe_token = 0;
uint32_t g_retraces = 0;
static std::atomic<uint32_t> g_profiler_frame{0};
static std::vector<uint32_t> g_slow_sim_frames;
static std::atomic<bool> g_exit{false};
std::atomic<int> g_exit_code{0};
std::chrono::steady_clock::time_point g_next_frame;
uint8_t g_mmio[0x10000];      // 0xCC000000 - 0xCC00FFFF register file (big-endian bytes)
static bool g_in_interrupt = false;

// ---------------- guest calls from host ----------------
static void (*g_after_guest_call)(uint32_t) = nullptr;
void call_guest(uint32_t addr, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  ppc::Context& c = *cpu;
  uint32_t saved_lr = c.lr;
  // A callback delivered at a poll (an interrupt on the console) may find the interrupted function
  // between saving its LR into its caller's frame (stw r0,4(r1)) and making its own frame (stwu).
  // The callback's prologue writes 4(r1) as well, so on the same r1 it overwrote that saved LR with 0
  // and the interrupted function returned to address 0 (the ACE sound crash). The console's interrupt
  // path gives handlers a frame of their own; so does this: r1 moves below a back-chained frame.
  const uint32_t saved_r1 = c.r[1];
  if (saved_r1 >= 0x80000100u && saved_r1 < 0x81800000u) {
    c.r[1] = (saved_r1 - 0x40u) & ~7u;
    wr32(c.r[1], saved_r1);
  }
  c.r[3] = r3; c.r[4] = r4; c.r[5] = r5; c.r[6] = r6;
  c.lr = 0;
  ppc::call(c, ram, addr);
  c.lr = saved_lr;
  c.r[1] = saved_r1;
  if (g_after_guest_call) g_after_guest_call(addr);
}
void set_after_guest_call(void (*hook)(uint32_t addr)) { g_after_guest_call = hook; }

// ---------------- events ----------------
void post_completion(Completion fn) { g_completions.push_back(std::move(fn)); }
void set_pe_finish_pending() { g_pe_finish_pending = true; }
void set_pe_token_pending(uint16_t token) { g_pe_token = token; g_pe_token_pending = true; }
bool exit_requested() { return g_exit; }
void request_exit(int code) { g_exit_code.store(code); g_exit.store(true); }
int exit_code() { return g_exit_code.load(); }
uint32_t retrace_count() { return g_retraces; }
uint32_t profiler_frame_id() { return g_profiler_frame.load(std::memory_order_relaxed); }
const std::vector<uint32_t>& slow_sim_frames() { return g_slow_sim_frames; }
// The VI retrace is periodic in virtual time, like the hardware interrupt: `g_next_retrace_tb`
// is the timebase value of the next retrace. A sleeping guest (wait_event) jumps time straight
// to that boundary; a guest that busy-waits with interrupts enabled advances time in small steps
// at HLE entry points and loop polls and takes the retrace when it crosses the boundary (Slippi's
// lag-reduction code waits for pad data that the retrace path produces).
static uint64_t g_next_retrace_tb = TB_PER_FRAME;
static bool g_in_retrace = false;
void (*native_retrace)() = nullptr;
void (*native_state_snapshot)(MuStatePod*) = nullptr;
bool retrace_due() { return cpu->tb >= g_next_retrace_tb && !g_in_retrace; }
uint64_t next_retrace_tb() { return g_next_retrace_tb; }
void advance_time(uint64_t ticks) { cpu->tb += ticks; }
void shift_time(uint64_t ticks) {
  cpu->tb += ticks; g_next_retrace_tb += ticks;   // the retrace keeps its phase in the frame
  hle::audio_shift(ticks);
}
static void advance_frame() {
  if (cpu->tb < g_next_retrace_tb) cpu->tb = g_next_retrace_tb;   // idle: jump to the boundary
  g_next_retrace_tb += TB_PER_FRAME;
}

void deliver_interrupt(uint32_t number) {
  // __OSInterruptHandlerTable lives at 0x80003040 (OS_INTERRUPTTABLE_ADDR).
  uint32_t handler = rd32(0x80003040u + number * 4);
  if (!handler) return;
  uint32_t context = rd32(0x800000D4u);  // OS current context (virtual address)
  ppc::Context& c = *cpu;
  ppc::Context saved = c;                // handlers clobber registers; restore like an rfi would
  bool was = g_in_interrupt;
  g_in_interrupt = true;
  try {
    call_guest(handler, number, context);
  } catch (const LoadContextUnwind&) {
  }
  g_in_interrupt = was;
  uint64_t tb = c.tb;
  c = saved;
  c.tb = tb;
  ppc::update_mxcsr(c);
}

static bool deliver_completions(bool force);

// Set when the game hands over the frame's picture (the display copy): from then until the retrace it
// is only waiting, so its polls may wait for real time without delaying any game work.
static bool g_frame_submitted = false;
std::function<void()> capture_native_boot_clock() {
  const auto tb = cpu->tb, next = g_next_retrace_tb;
  const auto retraces = g_retraces;
  return [=] {
    cpu->tb = tb; g_next_retrace_tb = next; g_retraces = retraces;
    g_profiler_frame.store(retraces);
    g_next_frame = std::chrono::steady_clock::now();
    g_in_retrace = false; g_in_interrupt = false; g_frame_submitted = false;
    g_completions.clear(); g_pe_finish_pending = false; g_pe_token_pending = false;
  };
}
void note_frame_submitted() { g_frame_submitted = true; }


void pump_completions() {
  // Called from HLE entry points the guest polls. Virtual time flows a little so periodic
  // alarms (pad sampling) fire even in loops that never sleep. Nothing is delivered while the
  // guest has interrupts disabled; ppc::mtmsr flushes when they come back on.
  note_guest_stack_overrun();
  advance_time(2048);
  hle::dvd_poll();
  if (!ppc::interrupts_on(*cpu)) return;
  // Slippi's lag reduction busy-waits for the retrace with interrupts on, and each poll moves
  // console time on: without this, time raced ahead of the clock and a frame's audio blocks went out
  // together. Once the frame is submitted, a block that is due waits for its real time (the AI
  // interrupt's 5 ms rhythm on the console). Game work before the display copy is never delayed.
  if (g_frame_submitted && !options.fast) {
    static const bool pace = [] { const char* v = std::getenv("MELEE_AUDIO_PACING"); return !(v && *v == '0'); }();
    const uint64_t due = hle::audio_next_due();
    if (pace && due && cpu->tb >= due && due < g_next_retrace_tb) wait_until_console_time(due);
  }
  hle::audio_tick(false);
  deliver_completions(false);
  if (cpu->tb >= g_next_retrace_tb && !g_in_retrace) retrace();   // periodic VI interrupt during busy waits
}

bool g_has_window = false;

// Field-wise CPU hash excludes C++ padding and diagnostic counters/trace history.
static void trace_state() {
  if (!g_state_trace) return;
  hle::dvd_settle();   // a disc read still being copied in would make the RAM hash depend on the machine's load
  uint64_t h = 0;
  auto add = [&](const auto& v) { h = (h ^ gx::hash_bytes(&v, sizeof v)) * 0x100000001b3ull; };
  const auto& c = *cpu;
  add(c.r); add(c.f); add(c.cr); add(c.lr); add(c.ctr);
  add(c.ca); add(c.so); add(c.ov); add(c.fpscr); add(c.gqr);
  add(c.msr); add(c.hid0); add(c.hid2); add(c.dec); add(c.tb); add(c.spr);
  uint64_t events = (uint64_t)g_completions.size() << 32 |
      (uint64_t)g_pe_token << 8 | (g_pe_finish_pending ? 1 : 0) | (g_pe_token_pending ? 2 : 0);
  std::fprintf(g_state_trace, "%u,%016llX,%016llX,%016llX,%016llX\n", g_retraces,
      h, gx::hash_bytes(ram, ppc::CONSOLE_RAM_SIZE), gx::hash_bytes(aram, 0x01000000), events);
  std::fflush(g_state_trace);
}

static double g_frame_time = 0.0;
static double g_emulation_speed = 1.0;
void set_emulation_speed(double speed) {
  g_emulation_speed = speed < 0.5 ? 0.5 : speed > 2.0 ? 2.0 : speed;
  // The game makes its sound at this speed too (Slippi's time sync nudges it by up to 1% online):
  // the output follows it directly, since its own drift correction is capped far below that.
  audio_set_speed(g_emulation_speed);
}
double emulation_speed() { return g_emulation_speed; }
double now_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double frame_time() { return g_frame_time; }
static TickTiming g_tick_timing;
TickTiming& tick_timing() { return g_tick_timing; }

// Simulation-thread cost accounting: HLE entry points add their time to a slot; at the next
// retrace the frame's wall time (including in-frame waits) is logged when it exceeds 20 ms, with the
// slots that explain it, so a hitch is attributed instead of guessed.
// Seconds per time stamp counter tick, measured against the performance counter over 20 ms at startup.
const double tsc_seconds = [] {
  LARGE_INTEGER freq, q0, q1; QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&q0); const uint64_t t0 = __rdtsc();
  Sleep(20);
  QueryPerformanceCounter(&q1); const uint64_t t1 = __rdtsc();
  return t1 > t0 ? (double)(q1.QuadPart - q0.QuadPart) / (double)freq.QuadPart / (double)(t1 - t0) : 0.0;
}();
static double g_sim_costs[SIM_COST_COUNT];
static double g_sim_costs_window[SIM_COST_COUNT];   // accumulated over the 60-frame log interval
static double g_sim_ms_window = 0, g_sim_ms_worst = 0;
static ULONG64 g_slow_frame_cycles = 0;   // this thread's cycle count at the start of the frame, for the slow-frame line
static const char* const g_sim_cost_names[SIM_COST_COUNT] = {"disc", "ax", "jukebox", "exi", "texsnap", "queue", "observe", "record", "gxdecode", "input", "wait", "late"};   // record includes texsnap and observe; gxdecode includes record and queue
static double g_sim_frame_start = 0.0, g_last_sim_ms = 0.0, g_last_sim_work_ms = 0.0;
static FILE* g_sim_times = nullptr;
static bool g_last_sim_late = false;   // the most recent frame missed its place on the 60 Hz schedule
static ULONG64 g_sim_frame_start_cycles = 0;   // this thread's cycle count at g_sim_frame_start (MELEE_SIM_TIMES)
void sim_cost_add(int slot, double seconds) { if (slot >= 0 && slot < SIM_COST_COUNT) { g_sim_costs[slot] += seconds; g_sim_costs_window[slot] += seconds; } }
// "sim: 3.1 ms/frame (worst 12.4) | observe 0.9 texsnap 0.4" for the periodic frame log.
static std::string sim_cost_line(uint32_t frames) {
  char buf[320];
  static uint64_t last_enters = 0;
  const uint64_t enters = ppc::g_enter_count - last_enters; last_enters = ppc::g_enter_count;
  size_t n = (size_t)std::snprintf(buf, sizeof buf, "sim: %.1f ms/frame (worst %.1f), %llu guest calls/frame", g_sim_ms_window / std::max(1u, frames), g_sim_ms_worst,
                                   (unsigned long long)(enters / std::max(1u, frames)));
  // Code run from RAM (mod discs, RAM-resident routines): the interpreter's share of the window.
  static uint64_t last_insns = 0;
  uint64_t calls = 0, insns = 0;
  ppc::interpreter_counts(&calls, &insns);
  if (insns != last_insns)
    n += (size_t)std::snprintf(buf + n, sizeof buf - n, ", %llu interpreted insns/frame", (unsigned long long)((insns - last_insns) / std::max(1u, frames)));
  last_insns = insns;
  bool first = true;
  for (int i = 0; i < SIM_COST_COUNT; ++i) {
    double ms = g_sim_costs_window[i] * 1000.0 / std::max(1u, frames);
    if (ms < 0.05) continue;
    n += (size_t)std::snprintf(buf + n, sizeof buf - n, "%s %s %.2f", first ? " |" : "", g_sim_cost_names[i], ms);
    first = false;
  }
  std::memset(g_sim_costs_window, 0, sizeof g_sim_costs_window);
  g_sim_ms_window = 0; g_sim_ms_worst = 0;
  return buf;
}
double last_sim_frame_ms() { return g_last_sim_ms; }
void report_sim_work() {
  static std::atomic<bool> reported{false};
  if (reported.exchange(true)) return;
  if (g_sim_times) std::fflush(g_sim_times);
  const auto& t = perf::sim_timer;
  log("perf: sim work mean %.2f ms p99 %.2f ms max %.2f ms over %llu frames (excluding pacing waits); game step mean %.2f p99 %.2f, gxdecode mean %.2f, submit mean %.2f, input mean %.2f; waits mean %.2f ms; resim frames %llu",
      t.work.mean(), t.work.p99(), t.work.maximum, (unsigned long long)t.work.count,
      t.step.mean(), t.step.p99(), t.mean(perf::Stage::Decode), t.mean(perf::Stage::Submit),
      t.mean(perf::Stage::Input), t.mean(perf::Stage::Wait), (unsigned long long)t.resim_frames);
  log("%s", cpu_power_line("at the end").c_str());
}
double last_sim_frame_work_ms() { return g_last_sim_work_ms; }
bool last_sim_frame_late() { return g_last_sim_late; }

bool wait_until_console_time(uint64_t tb) {
  if (options.fast || tb >= g_next_retrace_tb || g_next_retrace_tb - tb > TB_PER_FRAME) return false;
  perf::Scope wait_cost(perf::Stage::Wait);
  // Position of `tb` inside the frame, as a fraction, mapped onto the frame's real-time period.
  const double into_frame = 1.0 - (double)(g_next_retrace_tb - tb) / (double)TB_PER_FRAME;
  const auto deadline = g_next_frame + std::chrono::microseconds((long long)(into_frame * 16667.0 / g_emulation_speed));
  const auto before = std::chrono::steady_clock::now();
  if (deadline > before) {
    wait_for_tick(deadline);
    const auto after = std::chrono::steady_clock::now();
    sim_cost_add(SIM_WAIT, std::chrono::duration<double>(after - before).count());
    sim_cost_add(SIM_LATE, std::chrono::duration<double>(after - deadline).count());
  }
  if (cpu->tb < tb) cpu->tb = tb;
  return true;
}

void retrace() {
  if (!perf::active) perf::active = &perf::sim_timer;
  struct Guard { Guard() { g_in_retrace = true; } ~Guard() { g_in_retrace = false; } } guard;
  // Tests: MELEE_TEST_GAME_LOAD_MS=<ms> adds that much work to every game frame, offline too: a PC whose
  // CPU cannot step the game at 60 Hz (the pacing hold in threaded_backend.cpp).
  static const double test_load_ms = [] { const char* v = host::test_env::getenv("MELEE_TEST_GAME_LOAD_MS"); return v ? std::atof(v) : 0.0; }();
  if (test_load_ms > 0.0)
    for (const double until = now_seconds() + test_load_ms / 1000.0; now_seconds() < until;) {}
  g_frame_submitted = false;
  ++g_retraces;
  if (!native_retrace) apply_early_rng_seed();
  // Work sampled since the prior retrace belongs to the prior frame id. Publish
  // the new id only after that interval is complete so a slow-frame report can
  // select the samples that actually occurred inside it.
  const uint32_t completed_frame = g_retraces - 1;
  g_profiler_frame.store(g_retraces, std::memory_order_relaxed);
  {
    double now = now_seconds();
    const auto work_frame = perf::active->finish();
    if (g_sim_frame_start > 0.0) {
      g_last_sim_ms = (now - g_sim_frame_start) * 1000.0;
      g_last_sim_work_ms = g_last_sim_ms - g_sim_costs[SIM_WAIT] * 1000.0;
      g_sim_ms_window += g_last_sim_ms;
      if (g_last_sim_ms > g_sim_ms_worst) g_sim_ms_worst = g_last_sim_ms;
      if (g_last_sim_ms > 20.0) {
        g_slow_sim_frames.push_back(completed_frame);
        char detail[256] = ""; size_t n = 0;
        for (int i = 0; i < SIM_COST_COUNT; ++i) if (g_sim_costs[i] * 1000.0 >= 0.5) n += (size_t)std::snprintf(detail + n, sizeof detail - n, " %s %.1f", g_sim_cost_names[i], g_sim_costs[i] * 1000.0);
        // How long this thread actually ran during the frame: far below the frame's length means it
        // was waiting or pushed aside, close to it means the work itself was slow.
        ULONG64 cycles = 0; QueryThreadCycleTime(GetCurrentThread(), &cycles);
        const double ran_ms = g_slow_frame_cycles ? (double)(cycles - g_slow_frame_cycles) * tsc_seconds * 1000.0 : 0.0;
        log("sim frame %u took %.1f ms, thread ran %.1f ms (ms:%s%s); exclusive decode %.3f submit %.3f step %.3f wait %.3f",
            g_retraces, g_last_sim_ms, ran_ms, detail, n ? "" : " guest code",
            work_frame.get(perf::Stage::Decode), work_frame.get(perf::Stage::Submit),
            work_frame.get(perf::Stage::Step), work_frame.get(perf::Stage::Wait));
        // This is end-of-frame context rather than attribution. With --profile, the sampling
        // report below also names routines sampled during this exact slow frame.
        if (cpu) {
          constexpr uint32_t kRecentFunctions = 16;
          const uint32_t count = std::min<uint32_t>(cpu->trace_pos, kRecentFunctions);
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t pc = cpu->trace[(cpu->trace_pos - count + i) & 63u];
            if (pc) log("  end-of-frame guest %08X %s", pc, symbol_name(pc));
          }
        }
      }
      // MELEE_SIM_TIMES=<csv>: every simulation interval's wall time and its cost slots, for
      // percentile comparisons between the engines (the log above only names frames over 20 ms).
      static FILE* sim_times = [] {
        const char* path = host::test_env::getenv("MELEE_SIM_TIMES");
          FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
        if (f) {
          std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
          std::fputs("retrace,scene_major,scene_minor,match_frame,sim_ms,cpu_ms", f);
          for (int i = 0; i < SIM_COST_COUNT; ++i) std::fprintf(f, ",%s", g_sim_cost_names[i]);
          std::fputs(",work_ms,step_ms,decode_excl_ms,submit_ms,wait_excl_ms\n", f);
        }
        g_sim_times = f;
        return f;
      }();
      if (sim_times) {
        uint32_t major = 0, minor = 0, match_frame = 0;
        current_scene(&major, &minor, &match_frame);
        // Cycles this thread actually ran during the frame (waits and preemption by other processes
        // excluded): separates the frame's own cost from contention.
        ULONG64 cycles = 0; QueryThreadCycleTime(GetCurrentThread(), &cycles);
        const double cpu_ms = g_sim_frame_start_cycles ? (double)(cycles - g_sim_frame_start_cycles) * tsc_seconds * 1000.0 : 0.0;
        std::fprintf(sim_times, "%u,%u,%u,%u,%.3f,%.3f", completed_frame, major, minor, match_frame, g_last_sim_ms, cpu_ms);
        for (int i = 0; i < SIM_COST_COUNT; ++i) std::fprintf(sim_times, ",%.3f", g_sim_costs[i] * 1000.0);
        std::fprintf(sim_times, ",%.3f,%.3f,%.3f,%.3f,%.3f\n", work_frame.work_ms(), work_frame.get(perf::Stage::Step),
                     work_frame.get(perf::Stage::Decode), work_frame.get(perf::Stage::Submit), work_frame.get(perf::Stage::Wait));
        if (g_retraces % 600 == 0) std::fflush(sim_times);
      }
    }
    std::memset(g_sim_costs, 0, sizeof g_sim_costs);
  }
  slippi::poll_options();
  check_mod_code_writes();
  {
    // Gameplay for the audio output's Auto mode: a match whose frame counter moves, past its first
    // second (the match start itself can hitch).
    static uint32_t last_match_frame = 0;
    uint32_t major = 0, minor = 0, match_frame = 0;
    current_scene(&major, &minor, &match_frame);
    audio_set_gameplay(match_frame != last_match_frame && match_frame > 60);
    last_match_frame = match_frame;
  }
  // Static Recomp: the guest sleeps until the retrace, and time used to jump straight to it, so the
  // blocks that fell due during the sleep went out together once the frame's real-time wait ended.
  // Deliver each at its own real time first (as the console's AI interrupt does every 5 ms); the
  // frame boundary does not move. The Source Port paces in its scene loop instead (mu_audio_idle).
  if (!native_retrace && !options.fast) {
    static const bool pace = [] { const char* v = std::getenv("MELEE_AUDIO_PACING"); return !(v && *v == '0'); }();
    for (int guard = 0; pace && guard < 8; ++guard) {
      const uint64_t due = hle::audio_next_due();
      if (!due || due >= g_next_retrace_tb) break;
      if (cpu->tb < due && !wait_until_console_time(due)) break;
      if (cpu->tb < due) cpu->tb = due;
      hle::audio_tick(true);
    }
  }
  advance_frame();
  if (g_has_window) window_pump();
  if (!options.fast) {
    g_next_frame += std::chrono::microseconds((long long)(16667.0 / g_emulation_speed));
    auto now = std::chrono::steady_clock::now();
    g_last_sim_late = !(g_next_frame > now);   // no time left to wait: the frame finished after its deadline
    if (g_next_frame > now) wait_for_tick(g_next_frame);
    else if (now - g_next_frame > std::chrono::milliseconds(34)) g_next_frame = now;   // after a stall, resume at 60 Hz instead of sprinting to catch up (audio would crackle)
    g_frame_time = std::chrono::duration<double>(g_next_frame.time_since_epoch()).count();
  } else {
    g_frame_time = now_seconds();
  }
  perf::active->begin();
  g_sim_frame_start = now_seconds();
  QueryThreadCycleTime(GetCurrentThread(), &g_slow_frame_cycles);
  static const bool sim_times_wanted = [] { const char* v = host::test_env::getenv("MELEE_SIM_TIMES"); return v && *v; }();
  if (sim_times_wanted) QueryThreadCycleTime(GetCurrentThread(), &g_sim_frame_start_cycles);
  // The latency probe's newest press is carried on (tick_timing.h); without the trace it stays 0.
  g_tick_timing.target = g_frame_time; g_tick_timing.wake = g_sim_frame_start;
  g_tick_timing.pad = 0; g_tick_timing.pad_age = -1;
  if (native_retrace) {
    native_retrace();
  } else {
    hle::audio_tick(true);
    // VI: mark display-interrupt 0 as pending (bit 15 of DI0 status, VI reg index 0x18).
    uint16_t di0 = ((uint16_t)g_mmio[0x2030] << 8) | g_mmio[0x2031];
    di0 |= 0x8000;
    g_mmio[0x2030] = (uint8_t)(di0 >> 8); g_mmio[0x2031] = (uint8_t)di0;
    deliver_interrupt(24);  // __OS_INTERRUPT_PI_VI
    trace_state();
  }
  digest_state();
  test_peek_and_dump();
  end_after_lobby_match();
  if (g_retraces % 60 == 0 || (options.frames && g_retraces >= options.frames)) {
    uint64_t commands, draws, vertices; uint32_t copies;
    gx_stats(&commands, &draws, &vertices, &copies);
    log("[frame %u] gx: %llu cmds %llu draws %llu verts %u efb-copies | disc: %llu reads %.1f MB | %s",
        g_retraces, commands, draws, vertices, copies, g_disc_reads, g_disc_bytes / 1048576.0, sim_cost_line(60).c_str());
    if (draws) {
      uint64_t dl_draws = 0, dl_vertices = 0;
      gx::display_list_stats(&dl_draws, &dl_vertices);
      log("[frame %u] gx sources: display lists %llu draws %llu verts, immediate %llu draws %llu verts",
          g_retraces, dl_draws, dl_vertices, draws - dl_draws, vertices - dl_vertices);
    }
    // Same-thread, fixed-frame reading of authored coverage: comparable between the two engines.
    if (const auto& a = gx::authored_stats(); a.posed_draws)
      log("[frame %u] authored coverage: posed draws %u (envelope %u), skinned draws %u",
          g_retraces, (unsigned)a.posed_draws, (unsigned)a.posed_draws_envelope, (unsigned)a.skinned_draws);
    if (gx::native_draw_audit_enabled()) {
      const auto audit = gx::native_draw_audit_stats();
      log("native PObj scope audit: %llu submitted, %llu streamed, %llu matched, %llu mismatched, %llu sequence errors, %llu scope errors, %llu/%llu scopes, %llu scoped / %llu unscoped draws, depth %llu, %llu pending, %llu open",
          (unsigned long long)audit.submitted_events, (unsigned long long)audit.streamed_events,
          (unsigned long long)audit.matched_events, (unsigned long long)audit.mismatched_events,
          (unsigned long long)audit.sequence_errors, (unsigned long long)audit.invalid_scope_events,
          (unsigned long long)audit.scopes_ended, (unsigned long long)audit.scopes_started,
          (unsigned long long)audit.scoped_draws, (unsigned long long)audit.unscoped_draws,
          (unsigned long long)audit.max_scope_depth, (unsigned long long)audit.pending_events,
          (unsigned long long)audit.open_scopes);
    }
  }
  // MELEE_TEST_RESIZE_AT=<retrace>:<w>x<h> (tests): the window changes size at that retrace, the
  // way a maximized or fullscreen window does shortly after start.
  static const struct ResizeAt {
    uint32_t at = 0; int w = 0, h = 0;
    ResizeAt() { if (const char* v = host::test_env::getenv("MELEE_TEST_RESIZE_AT")) std::sscanf(v, "%u:%dx%d", &at, &w, &h); }
  } resize_at;
  if (resize_at.at && g_retraces == resize_at.at) window_set_client_size(resize_at.w, resize_at.h);
  // MELEE_TEST_RESIZE_EVERY=<retraces>:<w1>x<h1>,<w2>x<h2> (tests): the window switches between the
  // two sizes over and over, a stress run for what a resize does to the renderer and its upscalers.
  static const struct ResizeEvery {
    uint32_t every = 0; int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    ResizeEvery() { if (const char* v = host::test_env::getenv("MELEE_TEST_RESIZE_EVERY")) std::sscanf(v, "%u:%dx%d,%dx%d", &every, &w1, &h1, &w2, &h2); }
  } resize_every;
  if (resize_every.every && g_retraces > 0 && g_retraces % resize_every.every == 0) {
    const bool second = (g_retraces / resize_every.every) % 2 == 1;
    window_set_client_size(second ? resize_every.w2 : resize_every.w1, second ? resize_every.h2 : resize_every.h1);
  }
  if (options.frames && g_retraces >= options.frames) request_exit(0);
  {
    // MELEE_TEST_CLOSE_STUCK=<retrace> (tests only): the window is closed at that retrace and the game
    // thread then hangs, as a stuck game does; the process must still end (request_user_exit).
    static const uint32_t close_stuck = [] {
      const char* v = test_env::getenv("MELEE_TEST_CLOSE_STUCK");
      return v ? (uint32_t)std::strtoul(v, nullptr, 10) : 0u;
    }();
    if (close_stuck && g_retraces >= close_stuck) {
      log("test: closing the window at retrace %u, then the game thread hangs", g_retraces);
      request_user_exit();
      for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  log_stack_peak(false);
  test_stack_depth(false);
  if (g_exit) {
    test_stack_depth(true);
    log_stack_peak(true);
    log("exit requested after %u retraces", g_retraces);
    std::fflush(stdout);
    throw ExitRequested{g_exit_code.load()};
  }
}

// Nesting: a callback that sleeps (OSSleepThread inside a DVD/ARQ chain) is a wait point where
// hardware would run further completions, so forced delivery may nest; polled entry points
// (force = false) never nest so callback order stays as posted.
static int g_pump_depth = 0;
static bool deliver_completions(bool force) {
  if (g_completions.empty()) return false;
  if (!force && (g_pump_depth > 0 || !ppc::interrupts_on(*cpu))) return false;
  if (g_pump_depth >= 16) return false;
  ++g_pump_depth;
  size_t n = g_completions.size();
  ppc::Context saved = *cpu;
  for (size_t i = 0; i < n && !g_completions.empty(); ++i) {
    Completion fn = std::move(g_completions.front());
    g_completions.pop_front();
    fn();
  }
  uint64_t tb = cpu->tb;
  *cpu = saved;
  cpu->tb = tb;
  ppc::update_mxcsr(*cpu);
  --g_pump_depth;
  return true;
}

void wait_event() {
  if (g_pe_finish_pending) {
    g_pe_finish_pending = false;
    // PE_ISR (0xCC00100A): finish interrupt status bit 3.
    g_mmio[0x100B] |= 0x08;
    // The game's draw-done callback (HSD_VIDrawDoneXFB, 803762C4) stops with video.c:722 when the
    // frame buffer it is told about is not waiting for it. A console delivers this interrupt right
    // after the frame; here it waits for the next sleep, and with Slippi's lag reduction lines (in
    // the replay viewer they ride with the widescreen code) the buffer index is -1 and two frames'
    // signals can meet the same slot: the viewer stopped at the end of every replay. Then the
    // callback is skipped for this one delivery: the wait flag still clears and GX still sees its
    // frame done, only the status change and the assert are left out, as Slippi's own
    // ForceNoVideoAssert does online. Display state only (outside the rollback snapshot).
    constexpr uint32_t kCallbackSlot = 0x804C1F68u, kArgSlot = 0x804C1F64u, kStatus0 = 0x804C1DDCu;
    constexpr uint32_t kDrawDoneXfb = 0x803762C4u, kWaitDone = 4u;
    uint32_t callback = 0;
    bool skip = false;
    if (!game_image && ram && rd32(kCallbackSlot) == kDrawDoneXfb) {
      const int32_t arg = (int32_t)rd32(kArgSlot);
      if (arg >= -1 && arg <= 2 && rd32(kStatus0 + (uint32_t)(arg * 0x60)) != kWaitDone) {
        skip = true;
        callback = rd32(kCallbackSlot);
        static bool told = false;
        if (!told) { told = true; log("video: a draw-done signal arrived for frame buffer %d, which was not waiting for it; skipped", arg); }
        wr32(kCallbackSlot, 0);
      }
    }
    deliver_interrupt(19);  // __OS_INTERRUPT_PI_PE_FINISH
    if (skip) wr32(kCallbackSlot, callback);
    return;
  }
  if (g_pe_token_pending) {
    g_pe_token_pending = false;
    g_mmio[0x100B] |= 0x04;
    g_mmio[0x100E] = (uint8_t)(g_pe_token >> 8); g_mmio[0x100F] = (uint8_t)g_pe_token;
    deliver_interrupt(18);  // __OS_INTERRUPT_PI_PE_TOKEN
    return;
  }
  // The sleeping thread yields: interrupts are effectively enabled during the switch, so pending
  // completions run now (nested if this sleep happens inside another callback). Otherwise time moves on.
  hle::dvd_poll();
  if (deliver_completions(true)) return;
  retrace();
}

}  // namespace host

namespace ppc {
void loop_poll(Context& c) {
  (void)c;
  host::pump_completions();   // advances time; fires alarms / audio frames / completions when EE is set
}
void interrupts_enabled(Context& c) {
  // Called from mtmsr when EE goes 0 -> 1: flush events that arrived while masked.
  if (host::g_pump_depth == 0) host::deliver_completions(true);   // never nest from inside a callback here
}
}  // namespace ppc

namespace host {

// ---------------- MMIO ----------------
static uint32_t mmio_get(uint32_t off, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v = (v << 8) | g_mmio[(off + i) & 0xFFFF];
  return v;
}
static void mmio_put(uint32_t off, uint32_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) { g_mmio[(off + i) & 0xFFFF] = (uint8_t)value; value >>= 8; }
}

uint32_t mmio_read(uint32_t addr, int bytes) {
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    switch (off & 0xFFFE) {
      case 0x2002: return 0;                 // VI: vertical position (VIGetCurrentLine)
      case 0x2000: return 0;
      case 0x0000: return 0;                 // CP status: fifo idle, not overflowed
      case 0x0004: return 0;                 // CP control
      case 0x0034: case 0x0036: return mmio_get(off, bytes);   // CP fifo rw distance (we keep 0)
      case 0x3000: return 0;                 // PI INTSR
      case 0x5004: return 0;                 // DSP mailbox from DSP: nothing pending
      case 0x5000: return 0;                 // DSP mailbox to DSP: not busy
      case 0x500A: return mmio_get(off, bytes) & ~0x0001u;  // DSP CSR: DSP not "reset in progress"
      default: return mmio_get(off, bytes);
    }
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return 0;  // EFB peek
  static int reported = 0;
  if (reported++ < 20) {
    log("mmio read %08X (%d) from %s lr=%08X", addr, bytes, symbol_name(cpu->last_pc), cpu->lr);
    if (reported <= 2) {
      log("  recent entries:");
      for (uint32_t i = 48; i < 64; ++i) { uint32_t pc = cpu->trace[(cpu->trace_pos + i) & 63]; if (pc) log("    %08X %s", pc, symbol_name(pc)); }
    }
  }
  return 0;
}

void mmio_write(uint32_t addr, uint32_t value, int bytes) {
  if ((addr & 0xFFFFC000u) == 0xCC008000u) { gx_write(value, bytes); return; }
  if ((addr & 0xFFFF0000u) == 0xCC000000u) {
    uint32_t off = addr & 0xFFFF;
    mmio_put(off, value, bytes);
    if (off == 0x3000 || off == 0x3004) return;
    return;
  }
  if ((addr & 0xF8000000u) == 0xC8000000u) return;  // EFB poke
  static int reported = 0;
  if (reported++ < 20) log("mmio write %08X = %08X (%d) from %s", addr, value, bytes, symbol_name(cpu->last_pc));
}

// ---------------- GX glue ----------------
void gx_write(uint32_t value, int bytes) { gx::write_fifo(value, bytes); }
void gx_frame_present(uint32_t) {}
void gx_stats(uint64_t* commands, uint64_t* draws, uint64_t* vertices, uint32_t* efb_copies) {
  gx::stats(commands, draws, vertices, efb_copies);
}

}  // namespace host
