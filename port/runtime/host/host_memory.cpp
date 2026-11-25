// Host services: the guest's memory as the host reads and writes it (big-endian, checked against RAM
// and the Source Port's game image), and guest symbol names. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "host.h"
#include "memory_range.h"
#include <windows.h>
#include "guest_registry.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace host {

const char* symbol_name(uint32_t addr) {
  // Binary search the sorted function name table for the containing function.
  size_t lo = 0, hi = guest::name_table_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (guest::name_table[mid].addr <= addr) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) return "?";
  return guest::name_table[lo - 1].name;
}

// ---------------- memory ----------------
uint8_t* game_image = nullptr;
uint32_t game_image_size = 0;
uint8_t* try_ptr(uint32_t addr, uint32_t bytes) {
  uint32_t off = addr & 0x3FFFFFFFu;
  if (valid_range(off, bytes, ram_size)) return ram + off;
  constexpr uint32_t IMAGE_PHYS = 0x10000000u;
  if (game_image && off >= IMAGE_PHYS && valid_range(off - IMAGE_PHYS, bytes, game_image_size)) return game_image + (off - IMAGE_PHYS);
  return nullptr;
}
uint8_t* ptr(uint32_t addr, uint32_t bytes) {
  uint8_t* p = try_ptr(addr, bytes);
  if (!p) die("host access outside RAM: %08X+%X", addr, bytes);
  return p;
}
uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, ptr(a, 4), 4); return _byteswap_ulong(v); }
uint16_t rd16(uint32_t a) { uint16_t v; std::memcpy(&v, ptr(a, 2), 2); return _byteswap_ushort(v); }
uint8_t rd8(uint32_t a) { return *ptr(a); }
void mark_ram_write(uint32_t a, uint32_t bytes) { ppc::mark_ram_write(a, bytes); }
void wr32(uint32_t a, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(ptr(a, 4), &v, 4); mark_ram_write(a, 4); }
void wr16(uint32_t a, uint16_t v) { v = _byteswap_ushort(v); std::memcpy(ptr(a, 2), &v, 2); mark_ram_write(a, 2); }
void wr8(uint32_t a, uint8_t v) { *ptr(a) = v; mark_ram_write(a, 1); }
// A guest string for a report. A pointer outside RAM (a panic raised with a bad argument) names
// itself instead of stopping the report that was about to say what went wrong.
std::string cstr(uint32_t addr, size_t max) {
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    const uint8_t* p = try_ptr(addr + (uint32_t)i, 1);
    if (!p) { char bad[40]; std::snprintf(bad, sizeof bad, "<bad pointer %08X>", addr); return s + bad; }
    if (!*p) break;
    s += (char)*p;
  }
  return s;
}

}  // namespace host
