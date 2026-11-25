// What the host.cpp pieces (host.cpp and host_*.cpp) share among themselves: state one piece owns and
// another reads, and the functions one calls in another. Only those files include this; everything
// public is in host.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace hle { void audio_tick(bool force); uint64_t audio_next_due(); void audio_shift(uint64_t ticks); }

namespace hle { void dvd_poll(); void dvd_settle(); }
namespace host {

// host.cpp, the frame loop
extern FILE* g_state_trace;   // --state-trace, opened by boot_setup
extern uint32_t g_retraces;
extern std::atomic<int> g_exit_code;
extern std::chrono::steady_clock::time_point g_next_frame;
extern uint8_t g_mmio[0x10000];

// host_log.cpp
extern FILE* g_log_file;   // die() writes its FATAL line there too

// host_disc.cpp
extern uint32_t g_fst_offset, g_fst_size, g_fst_max;

// host_boot.cpp
extern bool g_mod_disc;
extern bool g_mod_clean;
void check_mod_code_writes();

// host_timing.cpp
void wait_for_tick(std::chrono::steady_clock::time_point deadline);

// host_digest.cpp
void test_peek_and_dump();
void digest_state();
void apply_early_rng_seed();

// host_exit.cpp
void end_after_lobby_match();

// host_stack.cpp
void note_guest_stack_overrun();
void log_stack_peak(bool at_exit);
void test_stack_depth(bool at_exit);

// host_heap.cpp
void install_text_heap_for_user_codes();
void report_heap_stop_lines(uint32_t handle, uint32_t rounded, uint32_t caller);

// host_mod_files.cpp
int32_t fst_find_path(const char* path);
void dvd_fast_open_checked(ppc::Context& c, uint8_t*);
void dvd_convert_path_checked(ppc::Context& c, uint8_t*);

}  // namespace host
