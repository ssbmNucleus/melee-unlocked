// Host services: the log (a writer thread, melee_port.log and its rotation), startup timing, and the
// machine lines a report needs (processor and power, memory). Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_internal.h"
#include "session_logs.h"
#include "sysgen.h"
#include "startup_clock.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host {

// ---------------- logging ----------------
// Milliseconds since this process was created (Windows: the kernel's creation time, so the loader
// and static initialisation count, as they do for launch timing). Elsewhere, since
// this file's static initialisation, which runs before main.
static const auto g_process_start = std::chrono::steady_clock::now();
uint64_t startup_ms() {
#ifdef _WIN32
  static const uint64_t created = [] {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return uint64_t(0);
    return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
  }();
  if (created) {
    FILETIME n{};
    GetSystemTimeAsFileTime(&n);
    const uint64_t now = (uint64_t(n.dwHighDateTime) << 32) | n.dwLowDateTime;
    if (now >= created) return (now - created) / 10000;
  }
#endif
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_process_start).count();
}
std::string startup_mark() { return startup::suffix(startup_ms()); }
// Once, at the first real game frame on screen: where the startup timing ends.
void startup_first_frame() {
  static std::atomic<bool> told{false};
  if (!told.exchange(true)) log("startup: first frame presented%s", startup_mark().c_str());
}
// Every line also goes to melee_port.log in the working directory (truncated at start), so a play
// session can be inspected afterwards without the console window.
FILE* g_log_file = nullptr;
static void open_log_file() {
  static bool tried = false;
  if (tried) return;
  tried = true;
  // Keep the previous session's log (a desync or crash report is often noticed only after relaunching).
  std::string path = options.log_file.empty() ? "melee_port.log" : options.log_file;
  bool rotated = rotate_session_logs(path);
  g_log_file = std::fopen(path.c_str(), rotated ? "w" : "a");
  // Fall back to the user directory wherever the installation is read-only.
  if (!g_log_file && !std::filesystem::path(path).is_absolute()) {
    const std::filesystem::path user = sysgen::user_data_dir();
    std::error_code ec;
    if (!user.empty()) std::filesystem::create_directories(user.parent_path(), ec);
    if (!user.empty() && !ec) {
      const std::string moved = (user.parent_path() / std::filesystem::path(path).filename()).string();
      rotated = rotate_session_logs(moved);
      g_log_file = std::fopen(moved.c_str(), rotated ? "w" : "a");
      std::fprintf(stderr, "log: %s is not writable here; logging to %s\n", path.c_str(), g_log_file ? moved.c_str() : "nowhere");
      if (g_log_file) path = moved;
    }
  }
  if (!rotated) {
    std::fprintf(stderr,"log: could not rotate %s; appending to preserve the previous session\n",path.c_str());
    if (g_log_file) std::fprintf(g_log_file,"log: could not rotate previous session; appending\n");
  }
  // Which build wrote the log comes first, so a report always says it.
  if (g_log_file) {
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = std::strrchr(exe, '\\');
    std::fprintf(g_log_file, "%s\n", startup::log_header(MELEE_PORT_VERSION, name ? name + 1 : exe).c_str());
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory))
      std::fprintf(g_log_file, "memory: %.1f GB installed, %.1f GB free, %.1f GB more the system can hand out%s\n",
                   memory.ullTotalPhys / 1073741824.0, memory.ullAvailPhys / 1073741824.0, memory.ullAvailPageFile / 1073741824.0,
                   startup_mark().c_str());
    std::fprintf(g_log_file, "%s\n", cpu_power_line("at start").c_str());
    // Started again by the last process after the graphics device was lost (restart_after_device_loss).
    if (const char* lost = std::getenv("MELEE_DEVICE_LOST")) {
      const char* why = std::getenv("MELEE_DEVICE_LOST_WHY");
      const int length = why ? (int)std::min<size_t>(std::strcspn(why, "\r\n"), 200) : 0;
      std::fprintf(g_log_file, "started again after the graphics device was lost (restart %d)%s%.*s\n",
                   std::atoi(lost), length ? ": " : "", length, why ? why : "");
    }
  }
}

// "cpu: Intel(R) Core(TM) i5-9300H, rated 2400 MHz, now 1190 MHz, limit 2400 MHz, on battery (64%), power plan
// Balanced": a laptop on battery or a power saving plan runs the game step at a fraction of its speed, which
// a report could not tell from a slow processor (a GTX 1650 laptop, 11-10: three times this PC's step time).
#ifndef _WIN32
// Linux: /proc/cpuinfo, cpufreq and the power supply class.
static std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}
std::string cpu_power_line(const char* when) {
  std::string name = "?";
  {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
      if (line.rfind("model name", 0) == 0) { const size_t c = line.find(':'); if (c != std::string::npos) name = line.substr(c + 2); break; }
  }
  std::string out = std::string("cpu (") + when + "): " + name;
  const std::string cpufreq = "/sys/devices/system/cpu/cpu0/cpufreq/";
  const std::string max = read_first_line(cpufreq + "cpuinfo_max_freq"), now = read_first_line(cpufreq + "scaling_cur_freq"),
                    limit = read_first_line(cpufreq + "scaling_max_freq"), governor = read_first_line(cpufreq + "scaling_governor");
  const auto mhz = [](const std::string& khz) { return std::to_string(std::atol(khz.c_str()) / 1000); };
  if (!max.empty()) out += ", rated " + mhz(max) + " MHz";
  if (!now.empty()) out += ", now " + mhz(now) + " MHz";
  if (!limit.empty()) out += ", limit " + mhz(limit) + " MHz";
  if (!governor.empty()) out += ", governor " + governor;
  std::error_code ec;
  bool battery = false, on_ac = false, any_ac = false;
  std::string capacity;
  for (const auto& e : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
    const std::string type = read_first_line((e.path() / "type").string());
    if (type == "Mains") { any_ac = true; on_ac = on_ac || read_first_line((e.path() / "online").string()) == "1"; }
    if (type == "Battery") { battery = true; capacity = read_first_line((e.path() / "capacity").string()); }
  }
  if (battery && any_ac && !on_ac) out += ", on battery" + (capacity.empty() ? std::string() : " (" + capacity + "%)");
  else if (battery) out += ", plugged in";
  else out += ", mains power (no battery)";
  return out;
}
#else
std::string cpu_power_line(const char* when) {
  char name[128] = "?";
  DWORD rated = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
    DWORD size = sizeof name - 1, type = 0;
    if (RegQueryValueExA(key, "ProcessorNameString", nullptr, &type, (LPBYTE)name, &size) != ERROR_SUCCESS || type != REG_SZ) std::strcpy(name, "?");
    name[sizeof name - 1] = 0;
    size = sizeof rated;
    RegQueryValueExA(key, "~MHz", nullptr, &type, (LPBYTE)&rated, &size);
    RegCloseKey(key);
  }
  const char* trimmed = name;
  while (*trimmed == ' ') ++trimmed;
  std::string out = std::string("cpu (") + when + "): " + trimmed;
  if (rated) out += ", rated " + std::to_string(rated) + " MHz";
  // powrprof.dll at run time: the game links nothing more for one log line.
  struct ProcessorPower { ULONG number, max_mhz, current_mhz, mhz_limit, max_idle, current_idle; };
  using CallNtPower = LONG(WINAPI*)(int, void*, ULONG, void*, ULONG);
  using ActiveScheme = DWORD(WINAPI*)(HKEY, GUID**);
  using FriendlyName = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
  if (HMODULE power = LoadLibraryW(L"powrprof.dll")) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::vector<ProcessorPower> cores(si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1);
    auto call = (CallNtPower)GetProcAddress(power, "CallNtPowerInformation");
    if (call && call(11 /* ProcessorInformation */, nullptr, 0, cores.data(), (ULONG)(cores.size() * sizeof(ProcessorPower))) == 0) {
      ULONG now = 0, limit = 0;
      for (const ProcessorPower& c : cores) { now = std::max(now, c.current_mhz); limit = std::max(limit, c.mhz_limit); }
      out += ", now " + std::to_string(now) + " MHz, limit " + std::to_string(limit) + " MHz";
    }
    auto active = (ActiveScheme)GetProcAddress(power, "PowerGetActiveScheme");
    auto friendly = (FriendlyName)GetProcAddress(power, "PowerReadFriendlyName");
    GUID* scheme = nullptr;
    if (active && friendly && active(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
      wchar_t plan[128] = {};
      DWORD bytes = sizeof plan - sizeof(wchar_t);
      if (friendly(nullptr, scheme, nullptr, nullptr, (UCHAR*)plan, &bytes) == ERROR_SUCCESS) {
        char narrow[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, plan, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
        out += std::string(", power plan ") + narrow;
      }
      LocalFree(scheme);
    }
    FreeLibrary(power);
  }
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status)) {
    if (status.ACLineStatus == 0) {
      out += ", on battery";
      if (status.BatteryLifePercent <= 100) out += " (" + std::to_string(status.BatteryLifePercent) + "%)";
    } else if (status.ACLineStatus == 1) {
      out += status.BatteryFlag == 128 ? ", mains power (no battery)" : ", plugged in";
    }
    if (status.SystemStatusFlag & 1) out += ", Windows battery saver on";
  }
  return out;
}
#endif

// Reports that end in "bad allocation" or 8007000E said nothing about memory: the size of this
// process and what Windows still had, logged now and then, tell a leak from a full machine.
void log_memory() {
  PROCESS_MEMORY_COUNTERS_EX process{};
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof memory;
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&process, sizeof process) ||
      !GlobalMemoryStatusEx(&memory)) return;
  log("memory: game %.2f GB, system %.1f GB free of %.1f, %.1f GB more it can hand out",
      process.PrivateUsage / 1073741824.0, memory.ullAvailPhys / 1073741824.0, memory.ullTotalPhys / 1073741824.0,
      memory.ullAvailPageFile / 1073741824.0);
}
// Lines are formatted by the caller and written (and flushed) by a background thread. Flushing on
// the caller made the simulation thread wait for the disk: with a busy disk (a build, a download)
// the once-a-second frame line alone stalled 60 Hz ticks by 20-200 ms. Order is kept: the writer
// takes g_log_io before it takes the pending text, and so does log_flush().
static std::mutex g_log_mutex;             // guards g_log_pending
// Never destroyed. The writer thread below is detached and waits on it for the life of the process,
// and a plain exit() runs static destructors: glibc's pthread_cond_destroy then waits for that waiter
// forever, so the process hung instead of ending. On Linux Xlib's default error handler calls exit()
// when the GL driver refuses a context (an NVIDIA driver upgraded without a reboot does that).
static std::condition_variable& log_cv() {
  static std::condition_variable* cv = new std::condition_variable;
  return *cv;
}
static std::string g_log_pending;
static std::timed_mutex g_log_io;          // held while text goes to stdout and the file
static void log_drain(bool wait_for_io) {
  std::unique_lock<std::timed_mutex> io(g_log_io, std::defer_lock);
  // A crash on the writer thread itself must not wait on the lock it holds: give up after a while
  // and write anyway (a garbled line beats a hung crash report).
  if (wait_for_io) io.lock(); else if (!io.try_lock_for(std::chrono::milliseconds(500))) {}
  std::string batch;
  { std::lock_guard<std::mutex> lock(g_log_mutex); batch.swap(g_log_pending); }
  if (batch.empty()) return;
  FILE* out = stdout;
  std::fwrite(batch.data(), 1, batch.size(), out);
  std::fflush(out);
  if (g_log_file) { std::fwrite(batch.data(), 1, batch.size(), g_log_file); std::fflush(g_log_file); }
}
static void log_enqueue(const char* data, size_t len) {
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      for (;;) {
        { std::unique_lock<std::mutex> lock(g_log_mutex); log_cv().wait(lock, [] { return !g_log_pending.empty(); }); }
        log_drain(true);
      }
    }).detach();
    std::atexit([] { log_flush(); });
  });
  { std::lock_guard<std::mutex> lock(g_log_mutex); g_log_pending.append(data, len); }
  log_cv().notify_one();
}
void log_flush() { log_drain(false); }

void log(const char* fmt, ...) {
  if (options.quiet) return;
  open_log_file();
  char stack[1024];
  va_list ap; va_start(ap, fmt);
  const int n = std::vsnprintf(stack, sizeof stack - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof stack - 1) {
    stack[n] = '\n';
    log_enqueue(stack, (size_t)n + 1);
  } else {
    std::string line((size_t)n + 1, '\0');
    va_list ap2; va_start(ap2, fmt);
    std::vsnprintf(line.data(), line.size(), fmt, ap2);
    va_end(ap2);
    line.back() = '\n';
    log_enqueue(line.data(), line.size());
  }
}

void log_guest_text(const char* data, size_t len) { log_enqueue(data, len); }

}  // namespace host
