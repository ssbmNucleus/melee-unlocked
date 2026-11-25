// Crash reporting for melee_source: the unhandled-exception filter, the throw capture, the frame
// generation recovery, the report host::die writes and the MELEE_TEST_CRASH hook. Moved out of
// main.cpp unchanged; main.cpp reaches the file-statics through the setters at the end.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "crash_report.h"
#include "test_env.h"
#include <windows.h>
#include <dbghelp.h>
#include "host.h"
#include "guest_registry.h"
#include "pipeline_cache_store.h"
#include "product_identity.h"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

// Crash report: a native crash used to close the console with nothing on screen. Any unhandled
// exception now leaves melee_port_crash.txt (exception, module offset, last guest functions), a
// minidump, the same lines in melee_port.log, and a dialog pointing at them when a player launched it.
static bool g_crash_dialog = true;

#ifdef _WIN32
// What an unhandled C++ exception said. The thrown object and its type list are in the exception
// record (the Visual C++ layout for 64-bit programs: every pointer in the type list is an offset
// from the image base in parameter 3). Only an object that is a std::exception is read.
static void cxx_exception_text(const EXCEPTION_RECORD* er, char* out, size_t size) {
  out[0] = 0;
  if (er->ExceptionCode != 0xE06D7363u || er->NumberParameters < 4) return;
  __try {
    const uint8_t* base = (const uint8_t*)er->ExceptionInformation[3];
    const uint8_t* object = (const uint8_t*)er->ExceptionInformation[1];
    const int32_t* throw_info = (const int32_t*)er->ExceptionInformation[2];
    if (!base || !object || !throw_info) return;
    const int32_t* types = (const int32_t*)(base + throw_info[3]);
    for (int32_t i = 0; i < types[0] && i < 16; ++i) {
      const int32_t* type = (const int32_t*)(base + types[1 + i]);
      const char* name = (const char*)(base + type[1]) + 2 * sizeof(void*);
      if (i == 0) std::snprintf(out, size, "type %s", name);
      if (std::strcmp(name, ".?AVexception@std@@") != 0 || type[3] != -1) continue;
      const std::exception* e = (const std::exception*)(object + type[2]);
      const char* first = (const char*)(base + ((const int32_t*)(base + types[1]))[1]) + 2 * sizeof(void*);
      std::snprintf(out, size, "%s: %s", first, e->what());
      return;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// The call stack of the thread that stopped, as module+offset lines (no symbols are shipped).
static void crash_stack(const CONTEXT* at, FILE* f) {
  CONTEXT c = *at;
  for (int frame = 0; frame < 32 && c.Rip; ++frame) {
    HMODULE module = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &module))
      GetModuleFileNameA(module, name, MAX_PATH);
    const char* leaf = std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name;
    char line[400];
    std::snprintf(line, sizeof line, "stack %2d: %s+0x%llX", frame, leaf, (unsigned long long)(c.Rip - (DWORD64)module));
    host::log("%s", line);
    if (f) std::fprintf(f, "%s\n", line);
    DWORD64 image = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!function) {   // a leaf function: the return address is at the top of the stack
      __try { c.Rip = *(const DWORD64*)c.Rsp; c.Rsp += 8; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
      continue;
    }
    void* handler_data = nullptr; DWORD64 establisher = 0;
    __try { RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, function, &c, &handler_data, &establisher, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  }
}
// The graphics drivers' own files (NVIDIA, AMD, Intel) and the Vulkan loader, by file name.
static bool graphics_driver_module(const char* path) {
  const char* name = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
  for (const char* driver : {"nvoglv64.dll", "nvwgf2umx.dll", "nvd3dumx.dll", "nvldumdx.dll", "amdvlk64.dll", "amdxx64.dll",
                             "amdxc64.dll", "atio6axx.dll", "atiumd6a.dll", "igvk64.dll", "igd10iumd64.dll", "igd12umd64.dll",
                             "igxelpicd64.dll", "vulkan-1.dll"})
    if (_stricmp(name, driver) == 0) return true;
  return false;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
  ::gx::cache_store::freeze();   // first: no cache save starts or lands from a crashing process
  static volatile LONG entered = 0;
  if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* er = info->ExceptionRecord;
  HMODULE module = nullptr; char module_name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)er->ExceptionAddress, &module))
    GetModuleFileNameA(module, module_name, MAX_PATH);
  const uintptr_t offset = (uintptr_t)er->ExceptionAddress - (uintptr_t)module;
  // Quitting already, and the fault is in a graphics driver tearing down: everything is saved, so
  // end quietly instead of offering a crash report for it.
  if (host::process_ending() && graphics_driver_module(module_name)) {
    host::log("exit: %s faulted while the game was closing (exception %08lX); ended quietly",
              std::strrchr(module_name, '\\') ? std::strrchr(module_name, '\\') + 1 : module_name, er->ExceptionCode);
    host::log_flush();
    TerminateProcess(GetCurrentProcess(), 0);
  }
  char head[512];
  std::snprintf(head, sizeof head, "CRASH: exception %08lX at %p (%s+0x%llX), version %s", er->ExceptionCode, er->ExceptionAddress,
                std::strrchr(module_name, '\\') ? std::strrchr(module_name, '\\') + 1 : module_name, (unsigned long long)offset, MELEE_PORT_VERSION);
  host::log("%s", head);
  char said[400];
  cxx_exception_text(er, said, sizeof said);
  if (said[0]) host::log("exception text: %s", said);
  if (FILE* f = std::fopen("melee_port_crash.txt", "w")) {
    std::fprintf(f, "%s\n", head);
    if (said[0]) std::fprintf(f, "exception text: %s\n", said);
    crash_stack(info->ContextRecord, f);
    host::log_flush();
    if (host::cpu) {
      std::fprintf(f, "last guest function %08X %s, lr %08X\nrecent guest functions (oldest first):\n", host::cpu->last_pc, host::symbol_name(host::cpu->last_pc), host::cpu->lr);
      for (uint32_t i = 0; i < 64; ++i) { uint32_t pc = host::cpu->trace[(host::cpu->trace_pos + i) & 63]; if (pc) std::fprintf(f, "  %08X %s\n", pc, host::symbol_name(pc)); }
    }
    std::fclose(f);
  }
  HANDLE dump = CreateFileA("melee_port_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal, &mei, nullptr, nullptr);
    CloseHandle(dump);
  }
  if (g_crash_dialog) {
    // A crash on a helper thread leaves the match running behind this message, so it says when the
    // game closes: a player in an online match pressed OK mid-game expecting to play on.
    std::string text = std::string(head) + "\n\n" PRODUCT_NAME " crashed and closes when you press OK. melee_port_crash.txt, "
                       "melee_port_crash.dmp and melee_port.log in the game folder describe what happened.";
    MessageBoxA(nullptr, text.c_str(), product::kName, MB_ICONERROR | MB_OK);
  }

  return EXCEPTION_EXECUTE_HANDLER;
}

// Where the last C++ exception was thrown. One that reaches the top of the program is reported with
// its text, but its call stack is gone by then: "No mapping for the Unicode character exists in the
// target multi-byte code page" did not say which file operation had failed. A vectored handler sees
// every throw first and keeps the return addresses; the report prints the last one. Ordinary caught
// exceptions pass through here too, so it only records (a few microseconds per throw).
static void* g_throw_stack[24];
static volatile LONG g_throw_depth = 0;
static volatile LONG g_throw_frozen = 0;
LONG WINAPI note_throw(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == 0xE06D7363u && !g_throw_frozen)
    InterlockedExchange(&g_throw_depth, (LONG)RtlCaptureStackBackTrace(0, 24, g_throw_stack, nullptr));
  return EXCEPTION_CONTINUE_SEARCH;
}
static void throw_stack_lines(FILE* f) {
  const LONG depth = g_throw_depth;
  for (LONG i = 0; i < depth && i < 24; ++i) {
    HMODULE module = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_throw_stack[i], &module))
      GetModuleFileNameA(module, name, MAX_PATH);
    const char* leaf = std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name;
    char line[400];
    std::snprintf(line, sizeof line, "throw %2ld: %s+0x%llX", (long)i, leaf, (unsigned long long)((uintptr_t)g_throw_stack[i] - (uintptr_t)module));
    host::log("%s", line);
    if (f) std::fprintf(f, "%s\n", line);
  }
}

// The last throw came from NVIDIA's Streamline or DLSS libraries (sl.*.dll, *nvngx*.dll): their frame
// generation thread threw std::system_error "device or resource busy" from inside a present, on its
// own thread, after the output size changed, and the game ended there twice for one player.
// MELEE_TEST_STREAMLINE_THROW (hidden runs) counts the test throw as one of theirs.
static bool g_test_streamline_throw = false;
static bool last_throw_from_streamline() {
  if (g_test_streamline_throw) return true;
  // The thrower is the first frame past this handler (frame 0) and the system's throw machinery
  // (ntdll, KERNELBASE, the C++ runtime): every present also passes through Streamline's interposer
  // further up, so only the module that threw decides.
  static const char* const runtime[] = {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "vcruntime140.dll",
                                        "vcruntime140_1.dll", "msvcp140.dll", "ucrtbase.dll"};
  const LONG depth = g_throw_depth;
  for (LONG i = 1; i < depth && i < 24; ++i) {
    HMODULE module = nullptr; char name[MAX_PATH] = "";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_throw_stack[i], &module))
      return false;
    GetModuleFileNameA(module, name, MAX_PATH);
    const char* leaf = std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name;
    bool system = false;
    for (const char* r : runtime) system = system || _stricmp(leaf, r) == 0;
    if (system) continue;
    return _strnicmp(leaf, "sl.", 3) == 0 || _strnicmp(leaf, "nvngx", 5) == 0 || _strnicmp(leaf, "_nvngx", 6) == 0;
  }
  return false;
}
// That crash turns frame generation off for the next start, as a lost Direct3D 12 device switches the
// backend: a later key wins when the settings file is read. The player turns it back on in PC settings.
static std::string g_crash_settings_path;
static void turn_frame_generation_off_for_next_start() {
  if (g_crash_settings_path.empty()) return;
  std::ofstream out(std::filesystem::u8path(g_crash_settings_path), std::ios::binary | std::ios::app);
  if (!out) return;
  out << "\nframegen 0\n";
  out.flush();
  host::log("dlss: NVIDIA frame generation stopped the game; it is off for the next start (PC settings > Video turns it back on)");
}

// host::die (a guest fault, including one in a mod's own code, or any other fatal error) exits
// without an exception, so it writes the same report here: melee_port_crash.txt, a minidump of the
// process, and the dialog that names those files.
void die_report(const char* message) {
  char head[640];
  std::snprintf(head, sizeof head, "FATAL: %s, version %s", message, MELEE_PORT_VERSION);
  if (FILE* f = std::fopen("melee_port_crash.txt", "w")) {
    std::fprintf(f, "%s\n", head);
    // An internal error is an exception that reached the top: say where it was thrown.
    if (std::strstr(message, "internal error")) throw_stack_lines(f);
    // The host's own call stack too: a guest fault is raised by the host function that read the bad
    // address, and the guest trace alone does not say which one.
    CONTEXT here{}; here.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&here);
    crash_stack(&here, f);
    if (host::cpu) {
      std::fprintf(f, "last guest function %08X %s, lr %08X\nrecent guest functions (oldest first):\n", host::cpu->last_pc, host::symbol_name(host::cpu->last_pc), host::cpu->lr);
      for (uint32_t i = 0; i < 64; ++i) { uint32_t pc = host::cpu->trace[(host::cpu->trace_pos + i) & 63]; if (pc) std::fprintf(f, "  %08X %s\n", pc, host::symbol_name(pc)); }
    }
    std::fclose(f);
  }
  HANDLE dump = CreateFileA("melee_port_crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dump != INVALID_HANDLE_VALUE) {
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal, nullptr, nullptr, nullptr);
    CloseHandle(dump);
  }
  if (g_crash_dialog) {
    std::string text = std::string(head) + "\n\n" PRODUCT_NAME " stopped with an error and closes when you press OK. "
                       "melee_port_crash.txt, melee_port_crash.dmp and melee_port.log in the game folder describe what happened.";
    MessageBoxA(nullptr, text.c_str(), product::kName, MB_ICONERROR | MB_OK);
  }

}

#else
#include "full/crash_linux.inl"
#endif

// MELEE_TEST_CRASH=<frame>: a deliberate crash during that frame, for the crash report gates. Only a
// hidden or headless run arms it (players never start one). It is an ordinary access violation, so
// melee_port_crash.txt, the minidump and the log line come from crash_filter exactly as for a real
// crash. It fires on a thread of its own, which leaves the frame loop untouched; in a --fast run it
// can land a frame or two late, and the log line names the frame it hit.
void arm_test_crash(bool automated) {
  const char* text = host::test_env::getenv("MELEE_TEST_CRASH");
  if (!text || !*text) return;
  char* end = nullptr;
  const unsigned long frame = std::strtoul(text, &end, 10);
  if (!automated || !end || *end || frame == 0) {
    host::log("test: MELEE_TEST_CRASH ignored (%s)", automated ? "not a frame number" : "hidden runs only");
    return;
  }
  host::log("test: crash armed for frame %lu", frame);
  std::thread([frame] {
    // Profiler frames or retraces, whichever moves. The armed line is written again once the game runs.
    const auto now = [] { return std::max(host::profiler_frame_id(), host::retrace_count()); };
    while (now() < 1) Sleep(1);
    host::log("test: crash armed for frame %lu", frame);
    while (now() < frame) Sleep(1);
    host::log("test: forced crash at frame %u", now());
    // MELEE_TEST_CRASH_KIND=throw: an unhandled C++ exception instead, for the report's text line.
    if (const char* kind = host::test_env::getenv("MELEE_TEST_CRASH_KIND"); kind && std::strcmp(kind, "throw") == 0)
      throw std::runtime_error("test exception text");
    volatile int* volatile target = nullptr;
    *target = 1;
  }).detach();
}


// A C++ exception that nothing handles (a file name the system cannot convert, a damaged file a
// parser gives up on) used to end the game with a bare exception code. It is reported with its own
// text instead, so the crash report says what failed.
[[noreturn]] void report_unhandled() {
  ::gx::cache_store::freeze();   // first: no cache save starts or lands from a failing process
  // Quitting already: NVIDIA's frame generation threw from its own thread while the process was being
  // ended (std::system_error "device or resource busy"), and that came out as a crash report.
  if (host::process_ending()) TerminateProcess(GetCurrentProcess(), 0);
  InterlockedExchange(&g_throw_frozen, 1);   // the rethrow below is not where it came from
#ifdef _WIN32   // DLSS frame generation exists on Windows only
  if (last_throw_from_streamline()) turn_frame_generation_off_for_next_start();
#endif
  try {
    if (std::exception_ptr error = std::current_exception()) std::rethrow_exception(error);
  } catch (const std::exception& error) {
    host::die("the game stopped on an internal error: %s", error.what());
  } catch (...) {
    host::die("the game stopped on an internal error of an unknown kind");
  }
  host::die("the game stopped itself (abort)");
}
// Setters for the file-statics above, written from melee_main.
void crash_set_dialog(bool on) { g_crash_dialog = on; }
#ifdef _WIN32
void crash_set_settings_path(const std::string& path) { g_crash_settings_path = path; }
void crash_set_test_streamline_throw(bool on) { g_test_streamline_throw = on; }
#endif
