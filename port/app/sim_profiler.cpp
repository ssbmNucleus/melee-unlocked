// --profile: the sampling profiler of the simulation thread (Windows only; the other platforms keep
// the stub from crash_linux.inl's old place). Moved out of main.cpp unchanged.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "sim_profiler.h"
#include "test_env.h"
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "host.h"
#include "guest_registry.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// The return addresses above a stopped thread's context, as crash_stack walks them. Allocates nothing
// (the stopped thread may hold the heap lock) and gives up quietly on a frame it cannot read.
static void profile_unwind(CONTEXT c, uint64_t* out, int depth) {
  for (int frame = 0; frame < depth && c.Rip; ++frame) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    __try {
      if (!function) { c.Rip = *(const DWORD64*)c.Rsp; c.Rsp += 8; }   // a leaf: the return address is on top
      else {
        void* handler_data = nullptr; DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, function, &c, &handler_data, &establisher, nullptr);
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    out[frame] = c.Rip;
  }
}

// --profile: samples where the simulation thread is ~1000 times a second and logs the hottest
// functions at exit. Translated game functions are named through the dispatch table, everything
// else through the executable's debug symbols. The sample buffer is reserved up front: the sampler
// must not allocate while the simulation thread (which may hold the heap lock) is suspended.
struct SimProfiler {
  HANDLE target = nullptr;
  std::atomic<bool> running{false};
  std::thread sampler;
  bool render_thread = false;   // --profile-render: after warm-up, sample the busiest other thread instead
  DWORD main_thread_id = 0;
  // CPU time (100 ns units) of every other thread of this process, by thread id.
  std::unordered_map<DWORD, uint64_t> thread_times() {
    std::unordered_map<DWORD, uint64_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{}; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == main_thread_id || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
      if (!h) continue;
      FILETIME c, e, k, u;
      if (GetThreadTimes(h, &c, &e, &k, &u))
        out[te.th32ThreadID] = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
      CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
  }
  std::vector<uint64_t> samples;
  std::vector<uint64_t> returns;   // [rsp] at the sample: the caller when the sample is in a leaf system routine
  std::vector<uint32_t> sample_frames;
  std::vector<uint64_t> sample_cycles;   // target thread cycles since the previous sample (0: it was waiting)
  // MELEE_PROFILE_STACK=<n> (1..16): also the n return addresses above each sample, unwound while the
  // thread is stopped, so a sample inside a system routine (the heap, memcpy) names who called it.
  int stack_depth = 0;
  std::vector<uint64_t> stacks;   // stack_depth entries per sample, 0 past the end of the stack
  void start() {
    if (const char* depth = host::test_env::getenv("MELEE_PROFILE_STACK")) stack_depth = std::clamp(std::atoi(depth), 0, 16);
    if (stack_depth) stacks.reserve((size_t)stack_depth << 20);
    main_thread_id = GetCurrentThreadId();
    if (!render_thread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    samples.reserve(1u << 22);
    returns.reserve(1u << 22);
    sample_frames.reserve(1u << 22);
    sample_cycles.reserve(1u << 22);
    running = true;
    sampler = std::thread([this] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      if (render_thread) {
        Sleep(8000);   // past boot and menus into the match
        auto before = thread_times();
        Sleep(1000);
        auto after = thread_times();
        DWORD busiest = 0; uint64_t most = 0;
        for (auto& kv : after) { auto b = before.find(kv.first); uint64_t d = kv.second - (b == before.end() ? 0 : b->second); if (d > most) { most = d; busiest = kv.first; } }
        if (busiest) target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, busiest);
        host::log("profile: sampling thread %lu (%.0f%% of a core over 1 s)", busiest, most / 1e5);
        if (!target) return;
      }
      ULONG64 last_cycles = 0;
      while (running.load(std::memory_order_relaxed) && samples.size() < samples.capacity()) {
        if (SuspendThread(target) != (DWORD)-1) {
          CONTEXT ctx{}; ctx.ContextFlags = stack_depth ? (CONTEXT_CONTROL | CONTEXT_INTEGER) : CONTEXT_CONTROL;
          ULONG64 cycles = 0; QueryThreadCycleTime(target, &cycles);
          if (GetThreadContext(target, &ctx)) {
            samples.push_back(ctx.Rip);
            returns.push_back(*(uint64_t*)ctx.Rsp);
            sample_frames.push_back(host::profiler_frame_id());
            sample_cycles.push_back(cycles - last_cycles);
            if (stack_depth && stacks.size() + (size_t)stack_depth <= stacks.capacity()) {
              uint64_t frames[16] = {};
              profile_unwind(ctx, frames, stack_depth);
              stacks.insert(stacks.end(), frames, frames + stack_depth);
            }
          }
          last_cycles = cycles;
          ResumeThread(target);
        }
        Sleep(1);
      }
    });
  }
  void report() {
    if (!running) return;
    running = false;
    if (sampler.joinable()) sampler.join();
    // MELEE_PROFILE_SAMPLES=<csv>: the raw samples, for offline symbolization of code this report
    // cannot name (the Source Port's GCC game DLL has no PDB; tools/profile_symbolize.py reads them).
    if (const char* path = host::test_env::getenv("MELEE_PROFILE_SAMPLES"); path && *path) {
      if (FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "# exe %llx\n", (unsigned long long)(uintptr_t)GetModuleHandleA(nullptr));
        HMODULE mods[512]; DWORD needed = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed))
          for (DWORD m = 0; m < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++m) {
            MODULEINFO mi{}; char name[MAX_PATH] = "";
            if (K32GetModuleInformation(GetCurrentProcess(), mods[m], &mi, sizeof mi) && K32GetModuleBaseNameA(GetCurrentProcess(), mods[m], name, MAX_PATH))
              std::fprintf(f, "# module %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, (unsigned long)mi.SizeOfImage, name);
          }
        std::fprintf(f, stack_depth ? "frame,rip,ret,cycles,stack\n" : "frame,rip,ret,cycles\n");
        for (size_t i = 0; i < samples.size(); ++i) {
          std::fprintf(f, "%u,%llx,%llx,%llu", sample_frames[i], (unsigned long long)samples[i], (unsigned long long)returns[i], (unsigned long long)sample_cycles[i]);
          if (stack_depth) {
            std::fputc(',', f);
            for (int k = 0; k < stack_depth && (i + 1) * (size_t)stack_depth <= stacks.size(); ++k)
              std::fprintf(f, k ? ";%llx" : "%llx", (unsigned long long)stacks[i * (size_t)stack_depth + k]);
          }
          std::fputc('\n', f);
        }
        std::fclose(f);
      }
    }
    std::vector<std::pair<uintptr_t, uint32_t>> fns;
    fns.reserve(guest::fn_table_count);
    for (size_t i = 0; i < guest::fn_table_count; ++i) fns.push_back({(uintptr_t)guest::fn_table[i].fn, guest::fn_table[i].addr});
    std::sort(fns.begin(), fns.end());
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    std::unordered_map<std::string, uint64_t> hits, slow_hits, system_callers;
    const auto& slow_frames = host::slow_sim_frames();
    const uintptr_t exe_base = (uintptr_t)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(exe_base + ((IMAGE_DOS_HEADER*)exe_base)->e_lfanew);
    const uintptr_t exe_end = exe_base + nt->OptionalHeader.SizeOfImage;
    std::unordered_map<uint64_t, std::string> host_names;
    auto host_name = [&](uint64_t addr) -> std::string {
      auto cached = host_names.find(addr);
      if (cached != host_names.end()) return cached->second;
      char buf[sizeof(SYMBOL_INFO) + 256]{}; SYMBOL_INFO* sym = (SYMBOL_INFO*)buf; sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
      DWORD64 disp = 0;
      std::string name = SymFromAddr(process, addr, &disp, sym) ? std::string(sym->Name) : "?";
      if (name == "?") {
        // No symbols (release builds carry a .map, not a .pdb): the module and offset, which the map names.
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)addr, &module) && module && GetModuleFileNameA(module, path, MAX_PATH)) {
          const char* leaf = std::strrchr(path, '\\') ? std::strrchr(path, '\\') + 1 : path;
          char buf[MAX_PATH + 32];
          std::snprintf(buf, sizeof buf, "%s+0x%llX", leaf, (unsigned long long)(addr - (uintptr_t)module));
          name = buf;
        }
      }
      return host_names.emplace(addr, name).first->second;
    };
    uint64_t guest_hits = 0, slow_guest_hits = 0, slow_total = 0;
    for (size_t si = 0; si < samples.size(); ++si) {
      const uint64_t rip = samples[si];
      const bool slow = !render_thread &&
          std::binary_search(slow_frames.begin(), slow_frames.end(), sample_frames[si]);
      std::string label;
      auto it = std::upper_bound(fns.begin(), fns.end(), std::make_pair((uintptr_t)rip, UINT32_MAX));
      if (it != fns.begin() && rip < fns.back().first + 0x10000) {
        --it;
        ++guest_hits;
        if (slow) ++slow_guest_hits;
        label = std::string("game ") + host::symbol_name(it->second);
      } else {
        const bool in_exe = rip >= exe_base && rip < exe_end;
        label = std::string(in_exe ? "host " : "system ") + host_name(rip);
        // A system DLL has no symbols here, so its exported names are only nearest guesses; the
        // return address shows which of our functions called into it.
        if (!in_exe && returns[si] >= exe_base && returns[si] < exe_end) ++system_callers[host_name(returns[si])];
      }
      ++hits[label];
      if (slow) { ++slow_hits[label]; ++slow_total; }
    }
    std::vector<std::pair<uint64_t, std::string>> top;
    for (auto& kv : hits) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    const double total = (double)std::max<size_t>(1, samples.size());
    host::log("profile: %zu samples of the %s thread, game code %.1f%%, runtime %.1f%%", samples.size(), render_thread ? "busiest non-simulation" : "simulation",
              100.0 * guest_hits / total, 100.0 * (samples.size() - guest_hits) / total);
    // MELEE_TEST_PROFILE_ALL=1 (tests): every label, so offsets without symbols can be summed per function from the map.
    static const bool all = [] { const char* v = host::test_env::getenv("MELEE_TEST_PROFILE_ALL"); return v && *v == '1'; }();
    for (size_t i = 0; i < top.size() && (all || i < 40); ++i) host::log("profile: %5.1f%%  %s", 100.0 * top[i].first / total, top[i].second.c_str());
    if (!render_thread && !slow_frames.empty()) {
      std::vector<std::pair<uint64_t, std::string>> slow_top;
      for (auto& kv : slow_hits) slow_top.push_back({kv.second, kv.first});
      std::sort(slow_top.rbegin(), slow_top.rend());
      const double selected = (double)std::max<uint64_t>(1, slow_total);
      host::log("profile slow frames: %zu frames, %llu samples, game code %.1f%%, runtime %.1f%%",
                slow_frames.size(), (unsigned long long)slow_total,
                100.0 * slow_guest_hits / selected, 100.0 * (slow_total - slow_guest_hits) / selected);
      for (size_t i = 0; i < slow_top.size() && i < 25; ++i)
        host::log("profile slow: %5.1f%%  %s", 100.0 * slow_top[i].first / selected, slow_top[i].second.c_str());
    }
    std::vector<std::pair<uint64_t, std::string>> callers;
    for (auto& kv : system_callers) callers.push_back({kv.second, kv.first});
    std::sort(callers.rbegin(), callers.rend());
    for (size_t i = 0; i < callers.size() && i < 15; ++i) host::log("profile: %5.1f%%  system call from %s", 100.0 * callers[i].first / total, callers[i].second.c_str());
  }
};
static SimProfiler g_profiler;
static bool g_profile = false;
#else
// --profile is a Windows sampling profiler (it suspends the simulation thread and reads its
// registers). On Linux, use perf: `perf record -g ./melee_source ...` gives the same picture.
struct SimProfiler {
  bool render_thread = false;
  void start() { host::log("profile: --profile samples on Windows only; on Linux run the game under perf record -g"); }
  void report() {}
};
static SimProfiler g_profiler;
static bool g_profile = false;
#endif
namespace sim_profiler {
void request(bool render_thread) {   // --profile, --profile-render
  g_profile = true;
  if (render_thread) g_profiler.render_thread = true;
}
void start() { if (g_profile) g_profiler.start(); }   // the native game runs on this thread; report() runs at shutdown
void report() { g_profiler.report(); }   // no-op without --profile
}  // namespace sim_profiler
