// Host services: how the process ends. The ending itself (end_process, files still being written),
// die() for a fatal error, exit on a closed window, restarts (a setting, a lost graphics device, a mod
// disc's looks) and the end after a --lobby-direct match. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include <windows.h>
#include "pipeline_cache_store.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "exi_slippi.h"
#include "slippi_playback.h"
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host {

// Ends the process without running any DLL's unload code. The game has saved everything by the
// time this is called (replays, settings, shader cache, the log). The NVIDIA upscaler libraries
// release their D3D12 objects when their DLLs unload, which is after the device is gone, and on
// some machines that crashed: inside nvngx_dlss.dll, or in the graphics driver from
// nvngx_dlssd.dll. The player had only closed the game and was shown a crash report. Streamline's
// own shutdown is not an option either (it blocked inside the same unload, see
// streamline::shutdown_for_process_exit), so the unload is skipped: Windows reclaims the rest.
static std::atomic<bool> g_process_ending{false};
void set_process_ending() { g_process_ending.store(true); }
bool process_ending() { return g_process_ending.load(); }

// die() is ending the process: a crash, an abort or a lost device. The caches are not flushed then
// (the device may be gone, and an abort may hold the very locks a flush takes), and the savers are
// frozen (no save starts or renames a file from then on); what they already wrote stays.
static std::atomic<bool> g_dying{false};

static std::mutex g_file_writes_mutex;
static std::condition_variable g_file_writes_done;
static int g_file_writes = 0;
void file_write_begin() { std::lock_guard<std::mutex> lock(g_file_writes_mutex); ++g_file_writes; }
void file_write_end() {
  { std::lock_guard<std::mutex> lock(g_file_writes_mutex); --g_file_writes; }
  g_file_writes_done.notify_all();
}
void mark_dying() { g_dying.store(true); }

void wait_file_writes(unsigned max_ms) {
  if (g_dying.load() && max_ms > 2000u) max_ms = 2000u;   // a crashing process never waits 20 s for a replay
  std::unique_lock<std::mutex> lock(g_file_writes_mutex);
  g_file_writes_done.wait_for(lock, std::chrono::milliseconds(max_ms), [] { return g_file_writes <= 0; });
}

[[noreturn]] void end_process(int code) {
  // A replay still being written is kept (upstream 0.9.110); a dying process waits less for it.
  wait_file_writes();   // 2 s once dying (wait_file_writes caps it)
  set_process_ending();
  // The shader and pipeline caches are saved as the game runs; this writes whatever is still new
  // (bounded) so a process ended here never makes the next start compile again. Nothing to do when
  // the renderer is already gone (its destructor saved).
  if (!g_dying.load()) ::gx::cache_store::flush_all("exit", std::chrono::milliseconds(1500));
  log_flush();
  std::fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), (UINT)code);
  std::abort();   // not reached
}

static void (*g_die_hook)(const char*) = nullptr;
void set_die_hook(void (*hook)(const char*)) { g_die_hook = hook; }
bool g_no_dialogs = false;   // constant-initialised: main.cpp sets it from an initialiser that runs first

[[noreturn]] void die(const char* fmt, ...) {
  ::gx::cache_store::freeze();
  g_dying.store(true);
  log_flush();
  va_list ap; va_start(ap, fmt);
  std::fprintf(stderr, "\nFATAL: ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
  if (g_log_file) {
    va_list ap2; va_start(ap2, fmt);
    std::fprintf(g_log_file, "\nFATAL: ");
    std::vfprintf(g_log_file, fmt, ap2);
    std::fprintf(g_log_file, "\n");
    va_end(ap2);
    std::fflush(g_log_file);
  }
  std::fflush(stderr);
  std::fflush(stdout);
  // MELEE_TEST_RAM_DUMP=<file> (tests): the guest memory as it was when the game stopped, so a stop
  // inside a mod's own code can be read afterwards (the code a mod writes at run time is in no file).
  if (const char* path = host::test_env::getenv("MELEE_TEST_RAM_DUMP"); path && *path && ram && ram_size) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(ram, 1, ram_size, f); std::fclose(f); }
  }
  if (void (*hook)(const char*) = g_die_hook) {
    g_die_hook = nullptr;   // once, even if the hook itself fails
    char message[512];
    va_list ap3; va_start(ap3, fmt);
    std::vsnprintf(message, sizeof message, fmt, ap3);
    va_end(ap3);
    hook(message);
  }
  // Not std::exit: a crash in a library's unload code would replace the report just written.
  end_process(kDieExit);
}

static std::atomic<bool> g_replay_viewing{false};
void set_replay_viewing(bool on) { g_replay_viewing.store(on); }
// The player closed the window. If the game is stuck and never gets to its own exit, the process stayed
// in the background holding the controllers and the adapter (a frozen replay: the adapter read "in use
// by another program" until Task Manager ended it). A closed window ends the process within
// kStuckExitSeconds whatever the game thread is doing, through end_process (files still being written
// and the shader caches are saved first). A normal exit marks process_ending first and is left alone.
constexpr int kStuckExitSeconds = 10;
void request_user_exit() {
  request_exit(g_replay_viewing.load() ? kViewerClosedExit : 0);
  static std::atomic<bool> armed{false};
  if (armed.exchange(true)) return;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(kStuckExitSeconds));
    if (process_ending()) return;
    log("exit: still running %d s after the window was closed (the game is stuck); ending the process so it "
        "lets go of the controllers", kStuckExitSeconds);
    end_process(g_exit_code.load());
  }).detach();
}
static void (*g_before_restart)() = nullptr;
void set_before_restart(void (*hook)()) { g_before_restart = hook; }
void request_restart() {
  // Start the replacement first and only then ask for a clean shutdown: if the launch fails there
  // is nothing to recover to, so the running game is left alone rather than closed into nothing.
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) { log("restart: cannot find this executable"); return; }
  std::wstring cmd = GetCommandLineW();
  // The replacement reads the shader and pipeline caches as it starts: write them first.
  ::gx::cache_store::flush_all("relaunch", std::chrono::milliseconds(3000));
  if (g_before_restart) g_before_restart();
  STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
  // The new process must not inherit the window or the adapter, so it waits for this one to go.
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    log("restart: CreateProcess failed, error %lu", (unsigned long)GetLastError());
    return;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  log("restart: relaunched, shutting down this instance");
  request_exit(0);
}

bool restart_after_device_loss(const char* why, bool (*switch_backend)()) {
  // MELEE_DEVICE_LOST="<restarts>:<unix seconds of the last one>", passed to the new process.
  int restarts = 0;
  long long last = 0;
  if (const char* v = std::getenv("MELEE_DEVICE_LOST")) std::sscanf(v, "%d:%lld", &restarts, &last);
  const long long now = (long long)std::time(nullptr);
  if (now - last > 600) restarts = 0;   // a loss long after the last restart starts the count again
  if (restarts >= 2) { log("device lost again (%s) after %d restarts: reporting it", why, restarts); return false; }
  const bool switched = restarts >= 1 && switch_backend && switch_backend();
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  char env[64];
  std::snprintf(env, sizeof env, "%d:%lld", restarts + 1, now);
  SetEnvironmentVariableA("MELEE_DEVICE_LOST", env);
  // Pass one bounded line to the new process, clearing any inherited reason.
  std::string reason = why ? why : "";
  if (reason.size() > 200) reason.resize(200);
  for (char& ch : reason) {
    if (ch == '\r' || ch == '\n') ch = ' ';
  }
  SetEnvironmentVariableA("MELEE_DEVICE_LOST_WHY",
                          reason.find_first_not_of(" \t") == std::string::npos ? nullptr : reason.c_str());
  std::wstring cmd = GetCommandLineW();
  if (g_before_restart) g_before_restart();
  STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    log("device lost (%s): the game could not start again (error %lu)", why, (unsigned long)GetLastError());
    return false;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  log("device lost (%s): the game starts again (restart %d%s, new process %lu)", why, restarts + 1,
      switched ? ", on the other renderer" : "", (unsigned long)pi.dwProcessId);
  end_process(0);
}

// Splits a Windows command line the way the C runtime does: spaces outside quotes separate, 2n
// backslashes before a quote are n backslashes and a quote that toggles, 2n+1 are n and a literal quote.
static std::vector<std::wstring> split_command_line(const std::wstring& line) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
    if (i >= line.size()) break;
    std::wstring arg;
    bool quoted = false;
    while (i < line.size() && (quoted || (line[i] != L' ' && line[i] != L'\t'))) {
      size_t slashes = 0;
      while (i < line.size() && line[i] == L'\\') { ++slashes; ++i; }
      if (i < line.size() && line[i] == L'"') {
        arg.append(slashes / 2, L'\\');
        if (slashes % 2) arg += L'"'; else quoted = !quoted;
        ++i;
      } else {
        arg.append(slashes, L'\\');
        if (i < line.size() && (quoted || (line[i] != L' ' && line[i] != L'\t'))) arg += line[i++];
      }
    }
    out.push_back(arg);
  }
  return out;
}
static std::wstring quote_argument(const std::wstring& arg) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
    else { out.append(slashes, L'\\'); out += ch; }
    slashes = 0;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}
bool restart_with_mod_looks() {
  if (!g_mod_disc || g_mod_clean || options.mod_base_iso.empty()) return false;
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) { log("restart: cannot find this executable"); return false; }
  const std::vector<std::wstring> args = split_command_line(GetCommandLineW());
  if (args.empty()) return false;
  std::wstring cmd = quote_argument(args[0]);
  for (size_t i = 1; i < args.size(); ++i) {
    if ((args[i] == L"--iso" || args[i] == L"--mod-base-iso" || args[i] == L"--look-iso") && i + 1 < args.size()) { ++i; continue; }
    cmd += L" " + quote_argument(args[i]);
  }
  cmd += L" --iso " + quote_argument(std::filesystem::u8path(options.mod_base_iso).wstring()) +
         L" --look-iso " + quote_argument(std::filesystem::u8path(options.iso).wstring());
  // The new copy reads the shader and pipeline caches as it starts: write them first (as request_restart).
  ::gx::cache_store::flush_all("relaunch", std::chrono::milliseconds(3000));
  if (g_before_restart) g_before_restart();
  STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    log("restart: CreateProcess failed, error %lu", (unsigned long)GetLastError());
    return false;
  }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  log("restart: starting the standard game with this mod's looks for Unranked and Party; shutting down this instance");
  request_exit(0);
  return true;
}

// --lobby-direct boots into a single negotiated match: the game exits after it ends (0), or with 2
// when it never started, so neither player is left in the loading scene.
void end_after_lobby_match() {
  const auto& cfg = slippi::online::config();
  if (cfg.lobby_code.empty()) return;
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(1)) return;
  last = now;
  const bool in_match = slippi::online::is_online_match() && !slippi::online::in_online_menus();
  static const auto started = now;
  static bool played = false;
  if (in_match) played = true;
  if ((game_image && played && !in_match) || (!played && now - started > std::chrono::seconds(90))) request_exit(played ? 0 : 2);
}

}  // namespace host
