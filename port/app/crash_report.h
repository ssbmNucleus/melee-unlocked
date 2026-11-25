// Crash reporting (crash_report.cpp), moved out of main.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#ifdef _WIN32
#include <windows.h>
LONG WINAPI crash_filter(EXCEPTION_POINTERS* info);   // SetUnhandledExceptionFilter
LONG WINAPI note_throw(EXCEPTION_POINTERS* info);     // AddVectoredExceptionHandler: remembers where each C++ exception was thrown
void crash_set_settings_path(const std::string& path);   // the settings file a frame generation crash writes "framegen 0" to
void crash_set_test_streamline_throw(bool on);           // MELEE_TEST_STREAMLINE_THROW: count the test throw as Streamline's
#else
void linux_install_crash_handlers();
void linux_abort_note();
#endif
void crash_set_dialog(bool on);                // false: no message box after a crash or a fatal error
void die_report(const char* message);          // host::set_die_hook
void arm_test_crash(bool automated);           // MELEE_TEST_CRASH, hidden runs only
[[noreturn]] void report_unhandled();          // terminate handler and SIGABRT handler
