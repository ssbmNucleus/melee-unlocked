// Host services: the disc image. Reads (and the file-cache warm-up), file reads with the served
// override files in front of the disc, FST lookups and the main.dol's data. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_internal.h"
#include "cosmetic_mods.h"
#include "disc_fst_paths.h"
#include <windows.h>
#include "window.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host {

static FILE* g_disc = nullptr;
uint32_t g_fst_offset, g_fst_size, g_fst_max;

// ---------------- disc ----------------
// Reads the whole disc image once, in the background at the lowest I/O priority, so the file cache
// holds it. Every game read is synchronous on the simulation thread (as the console's drive
// interrupt only ever arrives between frames), so a read that has to seek a hard drive or wait for
// a busy disk stalled a frame: files loaded during a match (items, Pokemon, stage changes) showed up
// as stutter. Load timing never reaches the game state: replays and online are unaffected.
static std::atomic<bool> g_disc_prefetch_done{true};
static std::atomic<uint64_t> g_disc_prefetch_bytes{0};
static void prefetch_disc_image(const std::string& path) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  g_disc_prefetch_done = false;
  std::thread([path] { optional_work("disc", [&] {
    struct Completion { ~Completion() { g_disc_prefetch_done.store(true, std::memory_order_release); } } completion;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low CPU and I/O priority
    const auto begin = std::chrono::steady_clock::now();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::vector<uint8_t> chunk(4u << 20);
    uint64_t total = 0;
    for (size_t got; (got = std::fread(chunk.data(), 1, chunk.size(), f)) > 0;) {
      total += got;
      g_disc_prefetch_bytes.store(total, std::memory_order_relaxed);
    }
    std::fclose(f);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    log("disc: image prefetched into the file cache (%llu MB in %.1f s)%s", (unsigned long long)(total >> 20),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count(), startup_mark().c_str());
  }); }).detach();
}

void disc_prefetch_wait() {
  // Overlap file-cache warming with graphics initialization, but finish it before simulation.
  // In particular, a slow disk must not be competing with the first stage's synchronous reads.
  while (!g_disc_prefetch_done.load(std::memory_order_acquire)) {
    const size_t mb = (size_t)(g_disc_prefetch_bytes.load(std::memory_order_relaxed) >> 20);
    loading_show(L"Reading game assets into the file cache (MB)", mb, mb + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  loading_close();
}

bool disc_open(const std::string& path) {
  g_disc = std::fopen(path.c_str(), "rb");
  if (!g_disc) return false;
  prefetch_disc_image(path);
  uint8_t hdr[0x440];
  if (!disc_read(0, hdr, sizeof hdr)) return false;
  auto be = [&](int o) { return ((uint32_t)hdr[o] << 24) | ((uint32_t)hdr[o + 1] << 16) | ((uint32_t)hdr[o + 2] << 8) | hdr[o + 3]; };
  g_fst_offset = be(0x424);
  g_fst_size = be(0x428);
  g_fst_max = be(0x42C);
  if (std::memcmp(hdr, "GALE01", 6) != 0) log("warning: disc id is not GALE01");
  return true;
}
uint64_t g_disc_reads = 0, g_disc_bytes = 0;
static std::mutex g_disc_mutex;   // the DVD worker and the simulation thread share the file
bool disc_read(uint32_t offset, void* dst, uint32_t size) {
  std::lock_guard<std::mutex> lk(g_disc_mutex);
  if (!g_disc) return false;
  if (_fseeki64(g_disc, offset, SEEK_SET) != 0) return false;
  ++g_disc_reads;
  g_disc_bytes += size;
  bool ok = std::fread(dst, 1, size, g_disc) == size;
  auto* output = (uint8_t*)dst;
  if (ok && ram && output >= ram && output <= ram + ppc::RAM_SIZE &&
      size <= (uint32_t)(ram + ppc::RAM_SIZE - output))
    ppc::mark_ram_write(ppc::RAM_BASE + (uint32_t)(output - ram), size);
  return ok;
}
bool disc_read_file(uint32_t vanilla_file_start, uint32_t file_offset, void* dst, uint32_t size) {
  switch (cosmetics::read(vanilla_file_start, file_offset, dst, size)) {
    case cosmetics::OverrideRead::Success:
      if (ram) {
        auto* output = (uint8_t*)dst;
        if (output >= ram && output <= ram + ppc::RAM_SIZE &&
            size <= (uint32_t)(ram + ppc::RAM_SIZE - output))
          ppc::mark_ram_write(ppc::RAM_BASE + (uint32_t)(output - ram), size);
      }
      return true;
    case cosmetics::OverrideRead::Failed: return false;
    case cosmetics::OverrideRead::NotOverridden: break;
  }
  uint64_t absolute = (uint64_t)vanilla_file_start + file_offset;
  if (absolute > UINT32_MAX) return false;
  return disc_read((uint32_t)absolute, dst, size);
}
uint32_t disc_fst_offset() { return g_fst_offset; }
uint32_t disc_fst_size() { return g_fst_size; }

// Looks a file up by name in the disc's FST (root and nested directories; exact match first,
// then case-insensitive). Used to serve ISO files to host-side loaders (Slippi game files).
bool disc_find_file(const std::string& name, uint32_t* offset, uint32_t* size) {
  static std::vector<uint8_t> fst;
  if (fst.empty()) {
    if (!g_fst_size) return false;
    fst.resize(g_fst_size);
    if (!disc_read(g_fst_offset, fst.data(), g_fst_size)) { fst.clear(); return false; }
  }
  auto be32 = [&](size_t o) { return o + 4 <= fst.size() ? ((uint32_t)fst[o] << 24) | ((uint32_t)fst[o + 1] << 16) | ((uint32_t)fst[o + 2] << 8) | fst[o + 3] : 0u; };
  uint32_t entries = be32(8);
  size_t strings = (size_t)entries * 12;
  if (strings > fst.size()) return false;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 1; i < entries; ++i) {
      uint32_t a = be32(i * 12);
      if (a >> 24) continue;   // directory
      size_t so = strings + (a & 0xFFFFFF);
      if (so >= fst.size()) continue;
      const char* n = (const char*)&fst[so];
      size_t maxlen = fst.size() - so;
      const char* end = static_cast<const char*>(std::memchr(n, 0, maxlen));
      if (!end || size_t(end - n) != name.size()) continue;
      bool match = pass == 0 ? (std::strncmp(n, name.c_str(), name.size()) == 0) : (_strnicmp(n, name.c_str(), name.size()) == 0);
      if (match) {
        if (offset) *offset = be32(i * 12 + 4);
        if (size) *size = be32(i * 12 + 8);
        return true;
      }
    }
  }
  return false;
}

namespace {
const std::vector<uint8_t>* disc_path_fst() {
  static std::vector<uint8_t> fst;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  if (fst.empty() && g_fst_size) {
    fst.resize(g_fst_size);
    if (!disc_read(g_fst_offset, fst.data(), g_fst_size)) fst.clear();
  }
  return fst.empty() ? nullptr : &fst;
}
}  // namespace

bool disc_find_path_by_offset(uint32_t offset, std::string* path) {
  const auto* fst = disc_path_fst();
  return fst && disc_fst::path_by_offset(*fst, offset, path);
}

bool disc_music_paths(std::vector<std::string>* paths) {
  const auto* fst = disc_path_fst();
  return fst && disc_fst::music_paths(*fst, paths);
}
uint32_t disc_fst_max_size() { return g_fst_max; }
bool dol_data_read(uint32_t address, void* dst, uint32_t size) {
  const auto be = [](const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; };
  uint8_t at[4], header[0xD8];
  if (!disc_read(0x420, at, 4)) return false;
  const uint32_t dol = be(at);
  if (!disc_read(dol, header, sizeof header)) return false;
  for (int i = 0; i < 18; ++i) {
    const uint32_t offset = be(header + i * 4), start = be(header + 0x48 + i * 4), length = be(header + 0x90 + i * 4);
    if (length && address >= start && (uint64_t)address + size <= (uint64_t)start + length)
      return disc_read(dol + offset + (address - start), dst, size);
  }
  return false;
}

}  // namespace host
