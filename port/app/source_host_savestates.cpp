// Source Port host: rollback savestates. The snapshot engine for online matches, the MEM1 watch
// diagnostic, Slippi's savestate commands, the resim phase, the determinism self-test.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

void log_savestate_stats(const char* when) {
  const auto& s = g_savestates.stats();
  host::log("online: snapshots %s: %llu captures (avg %.2f ms, max %.2f), %llu loads (avg %.2f ms, max %.2f), "
            "%llu missing; %llu re-simulations, %llu polls and %llu VI waits inside them",
            when, (unsigned long long)s.captures, s.captures ? s.capture_ms_total / s.captures : 0.0,
            s.capture_ms_max, (unsigned long long)s.loads, s.loads ? s.load_ms_total / s.loads : 0.0,
            s.load_ms_max, (unsigned long long)s.missing_loads, (unsigned long long)g_resim_phases,
            (unsigned long long)g_resim_polls, (unsigned long long)g_resim_vi_waits);
}

void mem1_watch_begin(const std::vector<MuStateRegion>& ranges);

// The sound driver's whole state (every variable of the AX files, synth and axdriver), from the
// ranges the build wrote beside the game library: a console savestate leaves all of it out, and
// restoring its voice tables while the voices play on cuts and restarts sounds on every rollback.
void append_sound_driver_exclusions(std::vector<MuStateRegion>& exclusions) {
  if (!g_loaded_game_path.empty() && host::game_image) {
    auto path = g_loaded_game_path; path.replace_extension(".snapexcl");
    std::ifstream in(path);
    std::error_code size_error;
    const auto dll_size = std::filesystem::file_size(g_loaded_game_path,size_error);
    std::vector<MuStateRegion> sound;
    std::string line;
    size_t bytes = 0;
    bool other_build = bool(size_error), size_seen = false;
    while (std::getline(in, line)) {
      unsigned long long value = 0;
      if (std::sscanf(line.c_str(), "# dll_size %llu", &value) == 1) { size_seen = true; other_build = other_build || value != dll_size; continue; }
      unsigned long long offset = 0, size = 0;
      if (std::sscanf(line.c_str(), "%llx %llx", &offset, &size) == 2 && size) {
        sound.push_back(MuStateRegion{(void*)((uintptr_t)host::game_image + offset), (uint32_t)size});
        bytes += size;
      }
    }
    if (!in.is_open() || sound.empty())
      host::log("online: no snapexcl applied: file missing or empty (%s)",path.string().c_str());
    else if (other_build || !size_seen)
      host::log("online: no snapexcl applied: size mismatch (%s)",path.string().c_str());
    else {
      exclusions.insert(exclusions.end(), sound.begin(), sound.end());
      host::log("online: sound driver state left out of snapshots: %zu ranges, %zu bytes", sound.size(), bytes);
    }
  } else host::log("online: no snapexcl applied: library not found");
}

// Immutable identity only; never capture or read span contents on the room worker.




// The snapshot of an online match: the game's capture ranges (all of main memory and the image's
// writable sections) minus its exclusions. They are fixed within a match, so the spans are computed
// once, at online frame 1.
void begin_native_savestates(int slots) {   // a replay viewer keeps more (replay_keep)
  std::vector<MuStateRegion> ranges(g_game.snapshot_ranges(nullptr, 0));
  g_game.snapshot_ranges(ranges.data(), (uint32_t)ranges.size());
  std::vector<MuStateRegion> exclusions(g_game.state_exclusions(nullptr, 0));
  g_game.state_exclusions(exclusions.data(), (uint32_t)exclusions.size());
  append_sound_driver_exclusions(exclusions);
  // Diagnostic (MELEE_SNAPSHOT_EXCLUDE=lo-hi[,lo-hi...], hex): leave extra ranges out, to find
  // which memory a rollback must not restore.
  if (const char* extra = host::test_env::getenv("MELEE_SNAPSHOT_EXCLUDE")) {
    for (const char* p = extra; *p;) {
      char* end = nullptr;
      const unsigned long long lo = std::strtoull(p, &end, 16);
      if (!end || *end != '-') break;
      const unsigned long long hi = std::strtoull(end + 1, &end, 16);
      if (hi > lo) {
        exclusions.push_back(MuStateRegion{(void*)(uintptr_t)lo, (uint32_t)(hi - lo)});
        host::log("online: snapshot test exclusion %08llX-%08llX", lo, hi);
      }
      p = end;
      while (*p == ',') ++p;
    }
  }
  for (const auto& r : ranges)
    host::log("online: snapshot range %08llX +%X", (unsigned long long)(uintptr_t)r.address, r.size);
  auto spans = native_savestate::subtract(ranges, exclusions);
  g_savestates.begin(std::move(spans), slots);
  mem1_watch_begin(ranges);
  host::log("online: snapshots cover %zu bytes in %zu spans (first range %u bytes, %zu exclusions)",
            g_savestates.bytes(), g_savestates.spans().size(), ranges.empty() ? 0u : ranges[0].size,
            exclusions.size());
}

// Diagnostic, off unless MELEE_ONLINE_WATCH_MEM1=1: hashes MEM1 outside the snapshot in 64 KB
// chunks at online frame 1 and every 600 captures after, and logs the ranges that changed. Game
// state that changes during a match must be inside the snapshot, or a rollback cannot restore it.

bool mem1_watch_enabled() {
  static const bool enabled = [] { const char* v = host::test_env::getenv("MELEE_ONLINE_WATCH_MEM1"); return v && v[0] == '1'; }();
  return enabled;
}


void note_disc_load(void* dst, uint32_t size, uint32_t offset) {
  if (!mem1_watch_enabled()) return;
  const uintptr_t a = (uintptr_t)dst;
  if (a < MEM1_BASE || a >= MEM1_BASE + MEM1_SIZE) return;
  g_disc_loads.push_back({a, size, offset});
}

std::string disc_file_at(uintptr_t address) {
  for (auto it = g_disc_loads.rbegin(); it != g_disc_loads.rend(); ++it) {
    if (address < it->dst || address >= it->dst + it->size) continue;
    const uint32_t disc = it->offset + (uint32_t)(address - it->dst);
    for (const auto& kv : g_paths) {
      const auto& f = g_fst[kv.second];
      if (!f.dir && disc >= f.offset && disc < f.offset + f.length) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " +%X", disc - f.offset);
        return kv.first + buf;
      }
    }
    return "unnamed disc data";
  }
  return "no disc read";
}

uint64_t hash_chunk(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
  return h;
}

void mem1_watch_begin(const std::vector<MuStateRegion>& ranges) {
  g_mem1_watch = Mem1Watch{};
  if (!mem1_watch_enabled()) return;
  g_mem1_watch.on = true;
  std::vector<MuStateRegion> inside;
  for (const auto& r : ranges)
    if ((uintptr_t)r.address >= MEM1_BASE && (uintptr_t)r.address < MEM1_BASE + MEM1_SIZE) inside.push_back(r);
  auto outside = native_savestate::subtract({MuStateRegion{(void*)MEM1_BASE, MEM1_SIZE}}, inside);
  for (const auto& s : outside) {
    for (uintptr_t a = (uintptr_t)s.address; a < (uintptr_t)s.address + s.size; a += 0x10000) {
      const uintptr_t e = std::min<uintptr_t>(a + 0x10000, (uintptr_t)s.address + s.size);
      g_mem1_watch.ranges.emplace_back(a, e);
      g_mem1_watch.hashes.push_back(hash_chunk((const uint8_t*)a, e - a));
      g_mem1_watch.bytes.emplace_back((const uint8_t*)a, (const uint8_t*)e);
    }
  }
  host::log("online: watching %zu MEM1 chunks outside the snapshot", g_mem1_watch.ranges.size());
}

void mem1_watch_check() {
  if (!g_mem1_watch.on) return;
  uintptr_t run_start = 0, run_end = 0;
  size_t changed = 0;
  auto flush = [&] {
    if (run_end > run_start) host::log("online: MEM1 outside the snapshot changed: %08llX-%08llX",
                                       (unsigned long long)run_start, (unsigned long long)run_end);
    run_start = run_end = 0;
  };
  for (size_t i = 0; i < g_mem1_watch.ranges.size(); ++i) {
    const auto& r = g_mem1_watch.ranges[i];
    const uint64_t h = hash_chunk((const uint8_t*)r.first, r.second - r.first);
    if (h == g_mem1_watch.hashes[i]) continue;
    g_mem1_watch.hashes[i] = h;
    ++changed;
    {
      // Byte ranges that changed in this chunk (merged within 16 bytes), with the file loaded there.
      auto& old = g_mem1_watch.bytes[i];
      const uint8_t* now = (const uint8_t*)r.first;
      int shown = 0;
      for (size_t k = 0; k < old.size() && shown < 12; ++k) {
        if (old[k] == now[k]) continue;
        size_t end = k + 1, last = k;
        while (end < old.size() && end <= last + 16) { if (old[end] != now[end]) last = end; ++end; }
        char was[40] = {}, is[40] = {};
        for (size_t b = 0; b < 16 && k + b <= last; ++b) {
          std::snprintf(was + 2 * b, 3, "%02X", old[k + b]);
          std::snprintf(is + 2 * b, 3, "%02X", now[k + b]);
        }
        host::log("online: MEM1 outside the snapshot: %08llX +%zu was %s now %s (%s)",
                  (unsigned long long)(r.first + k), last - k + 1, was, is, disc_file_at(r.first + k).c_str());
        ++shown;
        k = last;
      }
      std::memcpy(old.data(), now, old.size());
    }
    if (run_end == r.first) run_end = r.second;
    else { flush(); run_start = r.first; run_end = r.second; }
  }
  flush();
  host::log("online: MEM1 watch: %zu of %zu chunks outside the snapshot changed", changed, g_mem1_watch.ranges.size());
}

// Slippi's savestate commands, served natively (Slippi preserves its bookkeeping with a preserve
// list; the native game excludes it instead, so the list is always empty).
void native_capture(int32_t frame) {
  if (!g_savestates.active()) begin_native_savestates();
  g_savestates.capture(frame);
  if (g_savestates.stats().captures % 60 == 0) mem1_watch_check();
  if (g_savestates.stats().captures % 1000 == 0) log_savestate_stats("so far");
}
void native_load(int32_t frame, const uint8_t* payload) {
  if (read_be32(payload + 4) != 0) host::log("online: ignoring a savestate preserve list (native state is excluded)");
  if (!g_savestates.load(frame)) {
    host::log("online: no snapshot kept for frame %d", frame);
    return;
  }
  // The frame the game finishes next continues from this older state; the renderer must not blend
  // across the jump.
  gx::mark_discontinuity(true);
  slippi::online::note_rollback(frame);
  if (g_savestates.stats().loads % 200 == 0) log_savestate_stats("so far");
}





// Determinism self-test (MELEE_ONLINE_SELFTEST, game side in mu_online.c): the game keeps its state
// at the top of a frame, rolls back a few frames, re-simulates them with the same inputs and asks
// for a comparison at the same point. Any difference is non-determinism in the rollback path.

uint64_t g_selftest_checks = 0, g_selftest_failures = 0;

void report_selftest(int32_t frame, std::vector<uint8_t>& reply) {
  reply.assign(4, 0);
  ++g_selftest_checks;
  if (!g_savestates.has_reference(frame)) {
    host::log("online selftest: no reference kept for frame %d", frame);
    return;
  }
  const auto diffs = g_savestates.compare_reference();
  if (diffs.empty()) {
    if (g_selftest_checks % 20 == 0)
      host::log("online selftest: %llu checks, %llu with differences", (unsigned long long)g_selftest_checks,
                (unsigned long long)g_selftest_failures);
    return;
  }
  ++g_selftest_failures;
  size_t bytes = 0;
  for (const auto& d : diffs) bytes += d.second;
  host::log("online selftest: frame %d differs after re-simulation: %zu ranges, %zu bytes", frame, diffs.size(), bytes);
  // Reply: count, then (address, length) pairs, big-endian; the game names what lives there.
  const uint32_t listed = (uint32_t)std::min<size_t>(diffs.size(), 480);
  reply.clear();
  append_be32(reply, listed);
  for (uint32_t i = 0; i < listed; ++i) {
    append_be32(reply, (uint32_t)diffs[i].first);
    append_be32(reply, diffs[i].second);
  }
  for (size_t i = 0; i < diffs.size() && i < 24; ++i) {
    const uint8_t* ref = g_savestates.reference_bytes_at(diffs[i].first);
    const uint8_t* now = (const uint8_t*)diffs[i].first;
    char was[40] = {}, is[40] = {};
    const uint32_t n = std::min<uint32_t>(diffs[i].second, 16);
    for (uint32_t k = 0; k < n && ref; ++k) {
      std::snprintf(was + 2 * k, 3, "%02X", ref[k]);
      std::snprintf(is + 2 * k, 3, "%02X", now[k]);
    }
    host::log("online selftest:   %08llX +%u: was %s now %s", (unsigned long long)diffs[i].first, diffs[i].second, was, is);
  }
}

void h_resim_phase(int32_t entering) {
  if (gl_costs_enabled()) {
    if (entering) g_resim_gx_bytes = 0;
    else host::log("rollback GX: resim_fifo_bytes=%llu", (unsigned long long)g_resim_gx_bytes);
  }
  // The existing read-only pause query exposes the engine body counter. Resimulation
  // loops bypass VI waits entirely, so counting those waits would miss real frames.
  if (auto* timer = host::perf::active) {
    int32_t state[3] = {-1, 0, 0};
    const bool have_frame = g_game.practice &&
        g_game.practice(MU_PRACTICE_PAGE_PAUSE, state, 3) == 0 && state[2] >= 0;
    timer->set_resim(entering != 0, static_cast<uint32_t>(state[2]), have_frame);
  }
  using namespace host::rollback;
  const auto ns = now_ns();
#if MU_SQUAD
  // Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
#endif
  if (entering) accumulator.begin(ns, rollback_engine_frame(), presentation);
  else accumulator.end(ns, rollback_engine_frame(), presentation);
  rollback_log();
  g_resim = entering != 0;
  if (g_resim) ++g_resim_phases;
}

// slippi::online::CMD_ONLINE_INPUTS, before Slippi's own handling of it, from h_slippi_command (source_host.cpp).
void online_inputs_command(const uint8_t* p) {
  // A new match starts at pad frame 1. The game resends frame 1 while it waits for the
  // opponent, so the snapshot pool (7 x all of main memory) is set up on the first one only.
  static int32_t last_pad_frame = 0;
  const int32_t pad_frame = (int32_t)read_be32(p);
  if (pad_frame == 1 && last_pad_frame != 1) {
    if (g_savestates.active()) log_savestate_stats("previous match");
    begin_native_savestates();
  }
  last_pad_frame = pad_frame;
}

}  // namespace host_internal
}  // namespace source_port
