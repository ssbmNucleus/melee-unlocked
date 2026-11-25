// What runs before the C runtime and before main: the AVX2 check, --print-info and the arguments
// read ahead of the C++ initializers. The initializer (mu_early_guard in .CRT$XCC) lives here and
// is kept by the linker's /include. Moved out of main.cpp unchanged (see early_start.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "early_start.h"
#include "crash_report.h"
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW: the options read before the C++ initialisers run
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "host.h"
#include "product_identity.h"
#include "public_build.h"   // --print-info: public_build
#ifdef _WIN32
#include "plugin_gate.h"   // fighter plugin approval: --print-info
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif


// The runtime and the game library are built for AVX2. Without it the first AVX2 instruction
// kills the process before anything is logged, so say so plainly instead (this file is not AVX2).
#ifdef __APPLE__
#include "../mac/cpu_check.h"
#endif
#ifndef MELEE_CPU_BASELINE_NAME
#define MELEE_CPU_BASELINE_NAME "AVX2"
#endif
bool cpu_has_avx2() {
#ifdef __APPLE__
  return mac_host::cpu_ok();
#endif
  int r[4];
  __cpuid(r, 0); if (r[0] < 7) return false;
  __cpuid(r, 1); const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
  if (!osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;
  __cpuidex(r, 7, 0); return (r[1] >> 5) & 1;
}

// ...and the check in melee_main is too late to catch it. The runtime and guest libraries are built
// for AVX2, and their C++ static initialisers run before main does, so a CPU without AVX2 died
// during CRT startup: exit code 0xC000001D, no log file written, and the message below never shown.
// A player reported exactly that, and the empty folder they were asked to find the log in is what
// gave it away.
//
// The initialiser slots run in this order: XCC (compiler), XCL (library), XCU (user). Every static
// constructor in the runtime and guest libraries is XCU, so XCC is ahead of all of them and is
// still after the C runtime has set itself up.
//
// The first attempt used .CRT$XIB, which is a slot the CRT uses for its own early initialisation,
// and it ran before the CRT was ready. That is a plausible way to get a wrong answer out of a
// check that is correct everywhere else, which is why the message below prints what the processor
// actually reported rather than only the conclusion.
// --print-info: one JSON object about this build on stdout, then exit 0, for a launcher or a script
// that wants to know what an executable is without starting it: no disc, no window, no message box,
// no log file. On Windows it runs from the initialiser slot below, ahead of the AVX2 refusal and of
// every static constructor, so it also answers on a processor that cannot run the game (that is
// what cpu_supported says). For the same reason it uses Win32 calls and plain character handling,
// nothing that needs the C++ runtime's initialisers. Fields:
//   version, product (product_identity.h kToken: "Nucleus", not the display name), upstream (the project it
//   is built from, "Melee Unlocked"), engine (always "source"), cpu_baseline, cpu_supported,
//   game_library (the Source Port's library beside the program, else null), plugin_format and
//   plugin_abi (what a .mufighter plugin must be built for, read from that library without running
//   any of it; null when it cannot be read), linked_fighters (m-ex fighters linked into the library
//   rather than shipped as plugins, a list; null when unknown), fighters_folder (the default folder
//   of installed fighters and plugins: "fighters" beside the program), and the options this build
//   takes, each true or false: fighters_dir (--fighters-dir), no_crash_dialog, no_dialogs; public_build
//   (built with NUCLEUS_PUBLIC_BUILD, public_build.h) and game_public_build (the library beside it was
//   too: it exports mu_public_build; null when it cannot be read). windows-build/client_release.py
//   packages only a build where both are true. If reading any of it fails, the answer is {"error": "..."} and exit code 1, still
//   without a window (answer()). The Source Port adds fighter plugin approval (plugin_gate.h):
//   plugin_signing_keys (signing keys built in), plugin_test_key (true only in a test build),
//   plugin_approvals (the approvals file), plugins_folder (null: none) and plugins_blocked (the plugins
//   a run would leave out: file, name, sha256, signature, approval "none" or "refused").
namespace print_info {
struct Text {
  char data[32768];   // room for the plugin fields (plugin_gate.h print_info_fields)
  size_t size;
  void add(const char* s) { while (*s && size + 1 < sizeof data) data[size++] = *s++; data[size] = 0; }
  void add_char(char c) { const char s[2] = {c, 0}; add(s); }
  void add_string(const char* s) {   // a JSON string, or null
    if (!s) { add("null"); return; }
    static const char hex[] = "0123456789abcdef";
    add_char('"');
    for (; *s; ++s) {
      const unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') { add_char('\\'); add_char((char)c); }
      else if (c < 0x20) { add("\\u00"); add_char(hex[c >> 4]); add_char(hex[c & 15]); }
      else add_char((char)c);
    }
    add_char('"');
  }
  void add_uint(unsigned v) { char b[12]; int n = 0; do { b[n++] = char('0' + v % 10); v /= 10; } while (v); while (n) add_char(b[--n]); }
  void field(const char* name) { add(size > 1 ? ",\"" : "\""); add(name); add("\":"); }
};
// mu_fighter_plugin_info in the game library (sourceport/game/mexft/mexft_plugins.c), the same layout.
struct FighterPluginInfo { unsigned int size, format; char abi[36]; char linked[512]; };

#ifdef _WIN32
static void utf8(const wchar_t* in, char* out, int size) {
  if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, size, nullptr, nullptr)) out[0] = 0;
}
// The library mapped without running any of it (no DllMain, no imports resolved), so only its data
// is read: the export is a plain struct with no pointers in it.
// *public_game: 1 when the library was built with NUCLEUS_PUBLIC_BUILD (it exports mu_public_build,
// sourceport/game/shim/mu_entry.c), 0 when it maps without that export, left alone when it does not map.
static bool plugin_info(const wchar_t* library, FighterPluginInfo* out, int* public_game) {
  const HMODULE lib = LoadLibraryExW(library, nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!lib) return false;
  *public_game = GetProcAddress(lib, "mu_public_build") ? 1 : 0;
  const auto* info = reinterpret_cast<const FighterPluginInfo*>(GetProcAddress(lib, "mu_fighter_plugin_info"));
  const bool ok = info && info->size >= sizeof(FighterPluginInfo);
  if (ok) {
    *out = *info;
    out->abi[sizeof out->abi - 1] = 0;
    out->linked[sizeof out->linked - 1] = 0;
  }
  FreeLibrary(lib);
  return ok;
}
// A program built for the Windows subsystem: stdout is whatever the caller handed over (a pipe, a
// file), else the console it was started from.
void write_out(const char* data, size_t size) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE || GetFileType(out) == FILE_TYPE_UNKNOWN) {
    AttachConsole(ATTACH_PARENT_PROCESS);   // fails when there is none, or when attached already
    out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  }
  if (out == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(out, data, (DWORD)size, &written, nullptr);   // no FlushFileBuffers: on a pipe it waits for the reader
}
#endif

#ifdef _WIN32
// The fighter plugins a run would leave out (plugin_gate.h print_info_fields): plugin_signing_keys,
// plugin_test_key, plugin_approvals, plugins_folder and plugins_blocked, from --settings-path and
// --fighters-dir when the command line has them. A function of its own: it holds C++ objects, which
// answer()'s __try cannot (C2712).
__declspec(noinline) static void add_plugin_fields(Text& t) {
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) { t.add(",\"plugins_blocked\":null"); return; }
  const std::string fields = plugin_gate::print_info_fields(std::filesystem::path(exe).parent_path());
  t.add(fields.c_str());
}
#endif

static Text& build(bool cpu_supported) {
  static Text t;
  t.size = 0; t.data[0] = 0;
  t.add("{");
  t.field("version"); t.add_string(MELEE_PORT_VERSION);
  t.field("product"); t.add_string(product::kToken);   // stays "Nucleus": programs read it
  t.field("upstream"); t.add_string(product::kUpstreamName);
  t.field("engine"); t.add_string("source");
  t.field("cpu_baseline"); t.add_string(MELEE_CPU_BASELINE_NAME);
  t.field("cpu_supported"); t.add(cpu_supported ? "true" : "false");
  const char* library = nullptr;
  char fighters[1024] = {};
  FighterPluginInfo info{};
  bool have_info = false;
  int public_game = -1;   // unknown: no library read
#ifdef _WIN32
  // The same name source_host.cpp loads (g_dll), beside the program.
  library = lstrcmpA(MELEE_CPU_BASELINE_NAME, "AVX2") == 0 ? "melee_game.dll" : "melee_game_compat.dll";
  wchar_t exe[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    wchar_t* slash = exe;
    for (wchar_t* p = exe; *p; ++p) if (*p == L'\\' || *p == L'/') slash = p + 1;
    *slash = 0;
    wchar_t path[MAX_PATH + 32] = {};
    lstrcpynW(path, exe, MAX_PATH);
    const size_t dir = (size_t)lstrlenW(path);
    for (size_t i = 0; library[i] && dir + i + 1 < sizeof path / sizeof *path; ++i) { path[dir + i] = (wchar_t)library[i]; path[dir + i + 1] = 0; }
    have_info = plugin_info(path, &info, &public_game);
    lstrcpynW(path + dir, L"fighters", (int)(sizeof path / sizeof *path - dir));
    utf8(path, fighters, (int)sizeof fighters);
  }
#endif
  t.field("game_library"); t.add_string(library);
  t.field("plugin_format"); if (have_info) t.add_uint(info.format); else t.add("null");
  t.field("plugin_abi"); t.add_string(have_info ? info.abi : nullptr);
  t.field("linked_fighters");
  if (!have_info) t.add("null");
  else {
    // A comma-separated list in the library; a JSON list here.
    t.add("[");
    char name[128]; size_t at = 0; bool first = true;
    for (const char* p = info.linked;; ++p) {
      if (*p == ',' || *p == 0) {
        name[at] = 0;
        if (at) { if (!first) t.add(","); t.add_string(name); first = false; }
        at = 0;
        if (!*p) break;
      } else if (at + 1 < sizeof name) name[at++] = *p;
    }
    t.add("]");
  }
  t.field("fighters_folder"); t.add_string(fighters[0] ? fighters : nullptr);
  t.field("fighters_dir"); t.add("true");
  t.field("no_crash_dialog"); t.add("true");
  t.field("no_dialogs"); t.add("true");
  t.field("public_build"); t.add(host::public_build::kOn ? "true" : "false");
  t.field("game_public_build"); t.add(public_game < 0 ? "null" : public_game ? "true" : "false");
#ifdef _WIN32
  add_plugin_fields(t);
#endif
  t.add("}\n");
  return t;
}

// Print the object and end the process at once: no static destructor, no other thread, nothing that
// could stop on a message box. Anything that goes wrong while it is put together is answered with
// {"error": ...} and exit code 1, still without a window.
[[noreturn]] void answer() {
  const bool supported = std::strcmp(MELEE_CPU_BASELINE_NAME, "AVX2") != 0 || cpu_has_avx2();
#ifdef _WIN32
  // No Windows error box either (a library that cannot be mapped, a fault): this mode never shows one.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  const Text* text = nullptr;
  DWORD fault = 0;
  __try {
    // MELEE_TEST_PRINT_INFO_FAULT (port/tests/print_info_test.py): a fault here, to check the answer to one.
    // A test hook, so not in a public build (public_build.h).
    if (!host::public_build::kOn && GetEnvironmentVariableW(L"MELEE_TEST_PRINT_INFO_FAULT", nullptr, 0))
      RaiseException(0xE0000001u, 0, 0, nullptr);
    text = &build(supported);
  }
  __except (fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { text = nullptr; }
  if (text) {
    write_out(text->data, text->size);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  char error[160];
  wsprintfA(error, "{\"error\":\"exception %08lX while reading this build's information\"}\n", fault);
  write_out(error, (size_t)lstrlenA(error));
  TerminateProcess(GetCurrentProcess(), 1);
  ExitProcess(1);
#else
  const Text& text = build(supported);
  std::fwrite(text.data, 1, text.size, stdout);
  std::fflush(stdout);
  std::_Exit(0);
#endif
}
}  // namespace print_info

// The command line options that mean nobody is there to answer a message box: hidden and headless
// test runs, --no-dialogs (and its older name --no-crash-dialog), and the one-shot modes a launcher or
// a script runs unattended (--print-info, --version, --help, --prewarm-only, the skin catalog
// commands, --card-self-test). With any of them no start-up refusal, fatal error or crash opens a
// window: each logs, writes its report and exits with its own code (usage()). Read before anything
// else runs (the initialiser slot below on Windows, melee_main elsewhere).
bool means_no_dialogs(const char* a) {
  static const char* const options[] = {
    "--hidden", "--headless", "--no-dialogs", "--no-crash-dialog", "--print-info", "--version", "--help", "-h",
    "--prewarm-only", "--import-cosmetic", "--scan-disc-skins", "--select-cosmetic", "--deselect-cosmetic",
    "--enable-project-effects", "--restore-vanilla-cosmetics", "--cosmetic-status", "--card-self-test",
    "--approve-plugin", "--revoke-plugin", "--list-plugins", "--check-effect", "--check-effects"};
  for (const char* option : options) if (std::strcmp(a, option) == 0) return true;
  return false;
}
void turn_dialogs_off() {
  host::g_no_dialogs = true;
  crash_set_dialog(false);
#ifdef _WIN32
  // Nor Windows' own boxes: a missing-disk prompt, the fault report a crash past every handler gets.
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

#ifdef _WIN32
// The arguments as the C runtime will give them to main, read here before it has: plain ASCII
// options only, so a wide argument is narrowed by dropping anything that is not ASCII.
static void read_early_arguments(bool* print_info, bool* no_dialogs) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    char a[64];
    int n = 0;
    for (const wchar_t* w = argv[i]; *w && n + 1 < (int)sizeof a; ++w) a[n++] = *w < 128 ? (char)*w : '?';
    a[n] = 0;
    if (std::strcmp(a, "--print-info") == 0) *print_info = true;
    if (means_no_dialogs(a)) *no_dialogs = true;
  }
  if (argv) LocalFree(argv);
}

static int __cdecl check_avx2_before_anything_else() {
  bool print_info_wanted = false, no_dialogs = false;
  read_early_arguments(&print_info_wanted, &no_dialogs);
  if (print_info_wanted) print_info::answer();
  if (no_dialogs) turn_dialogs_off();
#ifndef MELEE_NEEDS_AVX2
  return 0;   // built for an older baseline: nothing here to require
#else
  if (cpu_has_avx2()) return 0;
  // Say which processor and which of the four conditions failed. A player who is told "your CPU is
  // too old" and believes otherwise has no way to settle it, and neither do we: this makes the
  // screenshot itself the answer, instead of a round of guessing about what machine it is.
  char brand[64] = "unknown";
  int r[4];
  __cpuid(r, 0x80000000);
  if ((unsigned)r[0] >= 0x80000004u) {
    for (int i = 0; i < 3; ++i) { __cpuid(r, 0x80000002 + i); std::memcpy(brand + i * 16, r, 16); }
    brand[48] = '\0';
  }
  __cpuid(r, 0);
  const int max_leaf = r[0];
  int leaf1[4] = {};
  if (max_leaf >= 1) __cpuid(leaf1, 1);
  const bool osxsave = (leaf1[2] >> 27) & 1, avx = (leaf1[2] >> 28) & 1;
  const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
  int leaf7[4] = {};
  if (max_leaf >= 7) __cpuidex(leaf7, 7, 0);
  const bool avx2 = max_leaf >= 7 && ((leaf7[1] >> 5) & 1);

  // wsprintfA rather than snprintf: this runs from a CRT initialiser slot, before the C runtime
  // has finished setting itself up, and wsprintfA lives in user32 with no such dependency. The
  // last thing this should do is fault inside the code explaining a fault.
  char msg[768];
  wsprintfA(msg,
                PRODUCT_NAME " needs a processor with AVX2, and this one reports that it does not "
                "have it.\n\nAVX2 means Intel Core 4th generation (Haswell, 2013) or newer, or AMD "
                "Ryzen or newer. Every version of " PRODUCT_NAME " has been built for it; older "
                "versions crashed here without a message instead of showing this one.\n\n"
                "Processor: %s\n"
                "AVX: %s   AVX2: %s   OS support: %s\n\n"
                "If you believe this is wrong, send this window to the developer: these four values "
                "say exactly what the processor reported.",
                brand, avx ? "yes" : "no", avx2 ? "yes" : "no",
                (osxsave && (xcr0 & 6) == 6) ? "yes" : "no");
  if (no_dialogs) {   // unattended: the same text on stderr, and the exit code says it
    DWORD written = 0;
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) { WriteFile(err, msg, (DWORD)lstrlenA(msg), &written, nullptr); WriteFile(err, "\n", 1, &written, nullptr); }
  } else {
    MessageBoxA(nullptr, msg, product::kName, MB_ICONERROR | MB_OK);
  }
  ExitProcess(3);
  return 0;
#endif
}
// External and named to the linker: a static pointer nothing refers to was dropped from the build
// (melee_source.map had no .CRT$XCC section, so this never ran), and with it --print-info.
#pragma section(".CRT$XCC", long, read)
extern "C" __declspec(allocate(".CRT$XCC")) int (__cdecl* const mu_early_guard)() = check_avx2_before_anything_else;
#pragma comment(linker, "/include:mu_early_guard")
#endif

// Built for the Windows subsystem so double-clicking the game does not open a terminal alongside it.
// Anything started from a command line still prints there: this reattaches to the parent console when
// one exists, so `melee_source.exe --help` and scripted runs behave exactly as before.
#ifdef _WIN32
void attach_parent_console() {
  // Never take over output that is already going somewhere. A script running `--version` hands us a
  // pipe, and reopening CONOUT$ over it sends the answer to the terminal instead of back to the
  // caller, which is how this first broke the release packaging.
  const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
  if (existing && existing != INVALID_HANDLE_VALUE && GetFileType(existing) != FILE_TYPE_UNKNOWN) return;
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  freopen_s(&f, "CONIN$", "r", stdin);
}
#endif
