// The --profile sampler (sim_profiler.cpp), moved out of main.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace sim_profiler {
void request(bool render_thread);   // --profile (false) or --profile-render (true)
void start();                       // when requested
void report();                      // at exit; a no-op when not requested
}
