// The AVX2 check, --print-info and the pre-CRT arguments (early_start.cpp), moved out of main.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#ifndef MELEE_CPU_BASELINE_NAME
#define MELEE_CPU_BASELINE_NAME "AVX2"
#endif
bool cpu_has_avx2();
bool means_no_dialogs(const char* a);   // an option that means nobody is there to answer a message box
void turn_dialogs_off();                // host::g_no_dialogs, no crash dialog, no Windows error boxes
#ifdef _WIN32
void attach_parent_console();           // WinMain: print to the console the program was started from
#endif
namespace print_info {
[[noreturn]] void answer();   // --print-info: one JSON object on stdout, then the process ends
#ifdef _WIN32
void write_out(const char* data, size_t size);   // stdout of a Windows-subsystem program
#endif
}
