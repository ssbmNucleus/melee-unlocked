// Host services: the files a mod disc's game asks for (Static Recomp). A file the disc lacks opens a
// stand-in of the disc's own (a tune, a movie, another fighter's results animation), results-table rows
// without a file are cleared, and the last names asked for go in a stop's report. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace host {

// A mod disc can ask for a file it does not ship (ACE's results screen asks for /audio/ff_step1.hps
// for one of its fighters). The game then opens entry -1, DVDFastOpen refuses and leaves the file
// information of whatever was opened last, and the music stream reads its header out of that file:
// junk, which stops the game. Only the music stream opens a file without checking its name first, so
// in a mod session an entry outside the table opens a short victory tune the disc does have
// (ff_good.hps, else the first ff_*.hps): the wrong tune instead of a stop.
void dvd_fast_open_checked(ppc::Context& c, uint8_t*) {
  uint32_t entry = c.r[3];
  const uint32_t info = c.r[4];
  const uint32_t fst = disc_fst_addr();
  const uint32_t count = fst ? rd32(fst + 8) : 0;
  auto is_file = [&](uint32_t e) { return e < count && rd8(fst + e * 12) == 0; };
  if (!is_file(entry) && (int32_t)entry < 0) {
    static uint32_t fallback = 0;
    if (!fallback) {
      const uint32_t strings = fst + count * 12;
      for (uint32_t e = 1; e < count; ++e) {
        if (rd8(fst + e * 12)) continue;
        const char* name = (const char*)try_ptr(strings + (rd32(fst + e * 12) & 0x00FFFFFFu), 16);
        if (!name || std::strncmp(name, "ff_", 3) != 0 || !std::strstr(name, ".hps")) continue;
        if (!fallback) fallback = e;
        if (!std::strcmp(name, "ff_good.hps")) { fallback = e; break; }
      }
      log("mods: the game asked for a music file this disc does not have; %s", fallback ? "another tune plays in its place" : "no tune to play instead");
    }
    if (fallback) entry = fallback;
  }
  if (!is_file(entry)) { c.r[3] = 0; return; }   // as the game's own code: refused, information untouched
  wr32(info + 0x30, rd32(fst + entry * 12 + 4));   // startAddr
  wr32(info + 0x34, rd32(fst + entry * 12 + 8));   // length
  wr32(info + 0x38, 0);                            // callback
  wr32(info + 0x0C, 0);                            // cb.state
  c.r[3] = 1;
}
// The game's own name search (DVDConvertPathToEntrynum), rule for rule, without the 8.3 name check:
// from the root or the given directory, one path part at a time, letters compared without case.
int32_t fst_find_path(const char* path) {
  const uint32_t fst = disc_fst_addr();
  const uint32_t count = fst ? rd32(fst + 8) : 0;
  if (!count) return -1;
  const uint32_t strings = fst + count * 12;
  auto is_dir = [&](uint32_t e) { return rd8(fst + e * 12) != 0; };
  auto next_of = [&](uint32_t e) { return rd32(fst + e * 12 + 8); };
  uint32_t dir = 0;
  for (;;) {
    if (*path == '\0') return (int32_t)dir;
    if (*path == '/') { dir = 0; ++path; continue; }
    if (*path == '.') {
      if (path[1] == '.') {
        if (path[2] == '/') { dir = rd32(fst + dir * 12 + 4); path += 3; continue; }
        if (path[2] == '\0') return (int32_t)rd32(fst + dir * 12 + 4);
      } else if (path[1] == '/') { path += 2; continue; }
      else if (path[1] == '\0') return (int32_t)dir;
    }
    const char* end = path;
    while (*end != '\0' && *end != '/') ++end;
    const bool want_dir = *end != '\0';
    uint32_t found = 0;
    const uint32_t stop = std::min(next_of(dir), count);
    for (uint32_t e = dir + 1; e < stop; e = is_dir(e) ? std::max(next_of(e), e + 1) : e + 1) {
      if (!is_dir(e) && want_dir) continue;
      const char* name = (const char*)try_ptr(strings + (rd32(fst + e * 12) & 0x00FFFFFFu), 1);
      if (!name) continue;
      const char* p = path;
      while (*name != '\0' && std::tolower((unsigned char)*p) == std::tolower((unsigned char)*name)) { ++p; ++name; }
      if (*name == '\0' && (*p == '/' || *p == '\0')) { found = e; break; }
    }
    if (!found) return -1;
    if (!want_dir) return (int32_t)found;
    dir = found;
    path = end + 1;
  }
}
// Where a code list's branch at `at` leads (the code it put in place of that instruction), or 0 when
// the instruction there is not a plain branch.
static uint32_t mod_code_behind(uint32_t at) {
  const uint32_t first = rd32(at);
  if ((first & 0xFC000003u) != 0x48000000u) return 0;
  const uint32_t code = at + (uint32_t)((int32_t)(first << 6) >> 6);
  return code >= 0x80000000u && code < 0x81800000u ? code : 0;
}
// m-ex replaces gm_80160438 (the results animation file of a fighter) with a read of its own table:
//   lwz r4,N(r2) ; mulli r3,r3,4 ; lwzx r3,r3,r4 ; blr
// A row can name a file the disc does not have (ACE 2.0.0: Crazy Hand's row names GmRstMGk.dat). The game
// itself has no file for such a fighter and its results screen then shows no model for it (a NULL row).
// Rows without a file are cleared when the results screen opens its own file, before any fighter is
// asked for. Nothing is read or written unless that exact code is behind the function's entry, so the
// retail game and a mod without this table are left alone. A cleared row is skipped from then on.
//
// A row can also name a file the disc has for a fighter that has no results animation in any file
// (ACE 2.0.0: Master Hand's row names Mario's file). m-ex then looks up the fighter's symbol name, an
// empty string, and stops ("fighter N has no symbol"). Such a row is cleared too, but only when both of
// m-ex's reads are confirmed the same way: the fighter id table (Player_80036E20: lwz r3,8(r2)) and the
// symbol name table with its stop (ftDemo_SetArchiveData: lwz r4,0x78(r2)).
static void clear_result_rows_without_file(ppc::Context& c) {
  auto in_ram = [](uint32_t a, uint32_t n) { return a >= 0x80000000u && a < 0x81800000u && try_ptr(a, n) != nullptr; };
  const uint32_t code = mod_code_behind(0x80160438u);
  if (!code || !in_ram(code, 16) || (rd32(code) & 0xFFFF0000u) != 0x80820000u || rd32(code + 4) != 0x1C630004u ||
      rd32(code + 8) != 0x7C63202Eu || rd32(code + 12) != 0x4E800020u) return;
  const uint32_t holder = c.r[2] + (uint32_t)(int32_t)(int16_t)(rd32(code) & 0xFFFFu);
  if (!in_ram(holder, 4)) return;
  const uint32_t table = rd32(holder);
  // The fighter id of a row (3 bytes a row, the id first) and the symbol names of a fighter (4 a fighter,
  // the results one first), both 0 unless m-ex's code for them is in place.
  uint32_t ids = 0, symbols = 0;
  const uint32_t id_code = mod_code_behind(0x80036E34u), symbol_code = mod_code_behind(0x800BEBC8u);
  if (id_code && in_ram(id_code, 8) && rd32(id_code) == 0x80620008u && rd32(id_code + 4) == 0x3803FFE0u &&
      rd32(0x80036E24u) == 0x1CC30003u && rd32(0x80036E3Cu) == 0x7FE03214u && rd32(0x80036E50u) == 0x887F0020u &&
      symbol_code && in_ram(symbol_code, 0x48) && rd32(symbol_code + 0x10) == 0x7C7D1B78u &&
      rd32(symbol_code + 0x18) == 0x80820078u && rd32(symbol_code + 0x1C) == 0x1C1D0004u &&
      rd32(symbol_code + 0x20) == 0x7C84002Eu && rd32(symbol_code + 0x24) == 0x1C050004u &&
      rd32(symbol_code + 0x28) == 0x7F84002Eu && rd32(symbol_code + 0x40) == 0x2C030000u &&
      rd32(symbol_code + 0x44) == 0x4182000Cu && in_ram(c.r[2] + 8u, 4) && in_ram(c.r[2] + 0x78u, 4)) {
    ids = rd32(c.r[2] + 8u);
    symbols = rd32(c.r[2] + 0x78u);
  }
  for (uint32_t row = 0; row < 256; ++row) {
    if (!in_ram(table + row * 4, 4)) break;
    const uint32_t at = rd32(table + row * 4);
    if (!at) continue;
    if (!in_ram(at, 16)) break;
    const char* name = (const char*)try_ptr(at, 16);
    if (!*name) continue;                                                    // a fighter without an entry
    if (_strnicmp(name, "GmRstM", 6) != 0 || !std::memchr(name, '\0', 16)) break;   // past the table
    if (fst_find_path(name) < 0) {
      log("mods: results table row for character %u named a file this disc does not have (%s); cleared", row, name);
      wr32(table + row * 4, 0);
      continue;
    }
    if (!ids || !symbols || !in_ram(ids + row * 3, 1)) continue;
    const uint32_t fighter = rd8(ids + row * 3);
    if (fighter >= 0x80u || !in_ram(symbols + fighter * 4, 4)) continue;
    const uint32_t names = rd32(symbols + fighter * 4);
    if (!in_ram(names, 4) || !in_ram(rd32(names), 1) || rd8(rd32(names)) != 0) continue;
    log("mods: results table row for character %u named a file (%s) for fighter %u, which has no results animation in any file; cleared", row, name, fighter);
    wr32(table + row * 4, 0);
  }
}
// A mod can send the results screen for a fighter that has no results animation on any disc (Master
// Hand, picked from a mod's debug menu, wins a match: GmRstMMh.dat). The game stops on the missing
// file. In a mod session a results animation the disc does not have opens as Mario's instead.
void report_object_memory();
// The last files a mod disc's game asked for by name: the heap stop's report prints them, so it
// names the fighters and the stage that were being loaded.
static char g_recent_files[12][24];
// The last file the game asked for that this disc does not have (empty when none): a stop in lbfile
// right after it is the game finding no such file, so the crash names it.
static char g_missing_file[64];
const char* last_missing_file() { return g_missing_file; }
static uint32_t g_recent_file_count = 0;
std::string recent_files(uint32_t count) {
  std::string names;
  const uint32_t shown = std::min<uint32_t>(std::min<uint32_t>(g_recent_file_count, 12), count);
  for (uint32_t i = g_recent_file_count - shown; i < g_recent_file_count; ++i) { if (!names.empty()) names += ", "; names += g_recent_files[i % 12]; }
  return names;
}
static void note_file_asked(const char* path) {
  const char* slash = std::strrchr(path, '/');
  const char* base = slash ? slash + 1 : path;
  if (g_recent_file_count && !_stricmp(g_recent_files[(g_recent_file_count - 1) % 12], base)) return;
  char* slot = g_recent_files[g_recent_file_count++ % 12];
  std::strncpy(slot, base, sizeof g_recent_files[0] - 1);
  slot[sizeof g_recent_files[0] - 1] = 0;
}
// One line with those names, once: for the heap stop and for any other stop of a mod disc's game (a
// file of the disc that the game cannot read stops it in whatever code parses the file).
void report_recent_files() {
  static bool told = false;
  if (told || !g_recent_file_count) return;
  told = true;
  std::string names;
  const uint32_t shown = std::min<uint32_t>(g_recent_file_count, 12);
  for (uint32_t i = g_recent_file_count - shown; i < g_recent_file_count; ++i) { if (!names.empty()) names += ", "; names += g_recent_files[i % 12]; }
  log("mods: last files the game asked for, oldest first: %s", names.c_str());
  log_flush();
}
void dvd_convert_path_checked(ppc::Context& c, uint8_t*) {
  const char* path = (const char*)try_ptr(c.r[3], 1);
  if (!path) { c.r[3] = 0xFFFFFFFFu; return; }
  note_file_asked(path);
  // MELEE_TEST_HEAP_REPORT (tests only, hidden and headless runs): the heap stop's lines once, after
  // that many files, without the stop.
  if (static const char* test = options.no_gc_adapter ? host::test_env::getenv("MELEE_TEST_HEAP_REPORT") : nullptr; test && g_recent_file_count == (uint32_t)std::atoi(test))
    { report_heap_stop_lines(0x80431F28u, 0, 0); report_object_memory(); }
  {
    // The results screen asks for its own file before any fighter's (see clear_result_rows_without_file).
    const char* slash0 = std::strrchr(path, '/');
    const char* base0 = slash0 ? slash0 + 1 : path;
    if (!_stricmp(base0, "GmRst.usd") || !_stricmp(base0, "GmRst.dat")) clear_result_rows_without_file(c);
  }
  int32_t entry = fst_find_path(path);
  if (entry < 0) {
    const char* slash = std::strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (std::strlen(base) == 12 && !_strnicmp(base, "GmRstM", 6) && !_stricmp(base + 8, ".dat")) {
      entry = fst_find_path("GmRstMMr.dat");
      static bool told = false;
      if (!told && entry >= 0) { told = true; log("mods: the game asked for a results animation this disc does not have (%s); another fighter's plays in its place", base); }
    }
    // A movie the disc left out (ACE has no MvHowto.mth, which the title screen plays after its demo
    // fights): the game opens entry -1 without checking, and DVDFastOpen's stand-in for that is a tune,
    // which the movie player cannot read (assertion "src % 32 == 0", devcom.c line 495, some 45
    // seconds into an idle title screen). Another movie of the disc plays in its place.
    const size_t length = std::strlen(base);
    if (length > 4 && !_stricmp(base + length - 4, ".mth")) {
      entry = fst_find_path("MvEndMario.mth");
      static bool told = false;
      if (!told && entry >= 0) { told = true; log("mods: the game asked for a movie this disc does not have (%s); another movie plays in its place", base); }
    }
    if (entry < 0) {
      std::strncpy(g_missing_file, base, sizeof g_missing_file - 1);
      g_missing_file[sizeof g_missing_file - 1] = 0;
    }
  } else {
    g_missing_file[0] = 0;   // only the newest lookup counts
  }
  c.r[3] = (uint32_t)entry;
}

}  // namespace host
