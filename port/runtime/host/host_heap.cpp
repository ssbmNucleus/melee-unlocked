// Host services: the game's memory as the game manages it (Static Recomp). Its heaps (the allocator
// failure trace, the preloader's full heap, space added above the console's 24 MB, the reports when a
// heap runs out), fixed buffers and tables a mod overruns (the title demo, item counts, animations), and
// install_mod_disc_guards, which installs these with the file guards of host_mod_files.cpp. Split from
// host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include "user_gecko.h"
#include "guest_heap_trace.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace host {

namespace {
struct GuestHeapCall { uint32_t handle = 0, requested = 0, caller = 0, stack = 0; bool available = false; };
thread_local GuestHeapCall g_guest_heap_call;
void record_guest_heap_call(ppc::Context& c) {
  g_guest_heap_call = {c.r[3], c.r[4], c.lr, c.r[1], true};
}
bool heap_trace_read32(uint32_t address, uint32_t& value) {
  if (!try_ptr(address, 4)) return false;
  value = rd32(address);
  return true;
}
bool heap_trace_literal(uint32_t address, const char* literal) {
  const size_t bytes = std::strlen(literal) + 1;
  const uint8_t* data = try_ptr(address, static_cast<uint32_t>(bytes));
  return data && std::memcmp(data, literal, bytes) == 0;
}
void trace_guest_heap_assert(ppc::Context& c) {
  // guest_002.cpp: before its __assert call, r24=Handle*, r30=32-byte-rounded request,
  // and the allocator's caller LR is at r1+60. The __assert entry hook runs BEFORE its
  // prologue overwrites r30 (guest_130.cpp:6532-6538), including direct C++ callers.
  if (c.lr != 0x80015098u || c.r[4] != 233u || !heap_trace_literal(c.r[3], "lbmemory.c")) return;
  const uint32_t handle = c.r[24], rounded = c.r[30];
  uint32_t saved_caller = 0;
  const bool have_saved_caller = c.r[1] <= 0xFFFFFFC3u && heap_trace_read32(c.r[1]+60u, saved_caller);
  const bool have_original = g_guest_heap_call.available && g_guest_heap_call.handle == handle &&
    ((g_guest_heap_call.requested + 31u) & ~31u) == rounded && g_guest_heap_call.stack >= 56u &&
    g_guest_heap_call.stack - 56u == c.r[1];
  log("[guest-heap] failure retrace=%u assert=lbmemory.c:233 assert_lr=%08X heap=%08X rounded=%08X "
      "caller=%08X caller_available=%u original_requested=%08X original_available=%u", retrace_count(),
      c.lr, handle, rounded, have_saved_caller ? saved_caller : have_original ? g_guest_heap_call.caller : 0u,
      (unsigned)(have_saved_caller || have_original), have_original ? g_guest_heap_call.requested : 0u,
      (unsigned)have_original);
  log("[guest-heap] registers sp=%08X r24=%08X r25=%08X r26=%08X r27=%08X r29=%08X r30=%08X r31=%08X cr=%08X",
      c.r[1], c.r[24], c.r[25], c.r[26], c.r[27], c.r[29], c.r[30], c.r[31], ppc::mfcr(c));
  const auto snapshot = guest_heap_trace::inspect(handle, heap_trace_read32);
  log("[guest-heap] bounds=%08X..%08X first=%08X blocks=%u free=%08X largest_gap=%08X "
      "complete=%u error=%s", snapshot.lo, snapshot.hi, snapshot.first, (unsigned)snapshot.count,
      snapshot.free, snapshot.largest_gap, (unsigned)snapshot.complete, snapshot.error ? snapshot.error : "none");
  for (size_t i = 0; i < snapshot.count; ++i) {
    const auto& block = snapshot.blocks[i];
    log("[guest-heap] block=%u descriptor=%08X next=%08X address=%08X size=%08X",
        (unsigned)i, block.descriptor, block.next, block.address, block.size);
  }
  // Exact retail Allocator offsets from guest_002.cpp, not a host-native struct cast.
  for (uint32_t offset : {0u, 4u, 0x62Cu, 0x630u, 0x634u, 0x698u, 0x69Cu}) {
    uint32_t value = 0;
    const uint32_t address = 0x804318B0u + offset;
    if (heap_trace_read32(address, value)) log("[guest-heap] allocator+%03X=%08X", offset, value);
    else log("[guest-heap] allocator+%03X unreadable", offset);
  }
  log_flush();
}
} // namespace
void install_guest_heap_trace() {
  static bool installed = false;
  const char* requested = host::test_env::getenv("MELEE_TRACE_GUEST_HEAP");
  if (installed || !requested || std::strcmp(requested, "1") || !options.no_gc_adapter) return;
  installed = true;
  // enter() is present in compiled functions and interpret() entries, so this catches direct
  // compiled __assert calls. Entirely RAM-local branches can bypass entry hooks; the assert-site
  // register capture remains available if __assert itself reaches its known compiled entry.
  ppc::add_entry_hook(0x80014FC8u, record_guest_heap_call);
  ppc::add_entry_hook(0x80388220u, trace_guest_heap_assert);
  log("[guest-heap] opt-in retail allocator failure hooks installed; no allocation behavior changes");
}

// Static Recomp: the text heap Melee sizes at boot (gmmain.c, 0xC000) runs out under a player's own
// Gecko code that opens the developer menu ("Memory Empty", OSPanic at sislib.c:86, 0.9.992 crash
// report). The Source Port already doubles every text heap (sislib.c). Doubled here only while the
// player's own codes are on: a session without them keeps the retail heap layout, as online play needs.
void install_text_heap_for_user_codes() {
  ppc::add_entry_hook(0x803A6048u, [](ppc::Context& c) {
    if (c.r[3] != 0xC000u) return;
    bool any = false;
    for (const auto& code : user_gecko::codes()) any = any || code.enabled;
    if (!any) return;
    c.r[3] = 0x18000u;
    static bool logged = false;
    if (!logged) { logged = true; log("gecko: your own codes are on: the game's text heap is doubled (the developer menu fills the retail one)"); }
  });
}

// The title demo preloads four fighters and a stage into two heaps of a fixed size, sized for the 26
// fighters of the retail disc. A disc that draws the demo from a larger roster and keeps those sizes
// (ACE 2.0.0) stops with `assertion "memp_kouho"` in lbmemory.c when the draw is too large. At the entry
// of gm_PreloadTitleDemo (801BF3F8) the four picks are at 8049E548 (costumes at +4). A pick that is not
// one of the 26 becomes one of them, worked out from the pick itself (no random number is drawn); those
// always fit. 0x21 is the game's "no fighter" and loads nothing, so it stays. The retail game never has
// another value here, and this hook is only installed for a mod disc.
static void title_demo_fit(ppc::Context&) {
  constexpr uint32_t kPicks = 0x8049E548u;
  constexpr uint8_t kRetailFighters = 0x1Au, kNoFighter = 0x21u, kZelda = 0x12u, kSheik = 0x13u;
  uint8_t pick[4];
  for (int i = 0; i < 4; ++i) pick[i] = rd8(kPicks + i);
  for (int i = 0; i < 4; ++i) {
    if (pick[i] < kRetailFighters || pick[i] == kNoFighter) continue;
    // As the game's own draw: no fighter twice, and never Zelda and Sheik together.
    auto taken = [&](uint8_t v) {
      for (int j = 0; j < 4; ++j)
        if (j != i && (pick[j] == v || (v == kZelda && pick[j] == kSheik) || (v == kSheik && pick[j] == kZelda))) return true;
      return false;
    };
    uint8_t v = (uint8_t)(pick[i] % kRetailFighters);
    while (taken(v)) v = (uint8_t)((v + 1u) % kRetailFighters);
    log("mods: the title demo drew fighter %u, which its memory was not sized for; fighter %u of the original game plays in its place", (unsigned)pick[i], (unsigned)v);
    pick[i] = v;
    wr8(kPicks + i, v);
    wr8(kPicks + 4 + i, 0);
  }
}
// Melee's file preloader reads files it guesses the next scene needs into fixed heaps ahead of time
// (lbDvd_CachePreloadedFile). After hours of online Teams such a heap can be full of files read ahead
// and never handed out, and the next fighter's file no longer fits: lbMemory_80014FC8 then stops the
// game (`memp_kouho`, lbmemory.c:233). At the entry of that allocator, r3 is the heap's handle and r4
// the size. When the heap has no gap that large, preload entries whose data lies in this heap and that
// are loaded (state 3) but were never handed to the game (load_state 1) are released through the
// game's own lbDvd_800174E8, lowest load score first, until it fits. Nothing the game holds is freed;
// a released file is read again when it is asked for. preloadCache is at 80432078, its 80 entries at
// +0xAC, 0x1C bytes each (raw data descriptor at +0x10, the data address at the descriptor's +4).
static void preload_make_room(ppc::Context& c) {
  constexpr uint32_t kEntries = 0x80432078u + 0xACu, kEntrySize = 0x1Cu, kCount = 80u;
  const uint32_t handle = c.r[3], need = (c.r[4] + 31u) & ~31u;
  if (!try_ptr(handle, 16) || need == 0) return;
  auto snap = guest_heap_trace::inspect(handle, heap_trace_read32);
  if (!snap.complete) return;
  // MELEE_TEST_PRELOAD_SQUEEZE (tests only, hidden runs): act as though the heap were full, to show
  // the release working on a real session.
  static int squeeze = (host::test_env::getenv("MELEE_TEST_PRELOAD_SQUEEZE") && options.no_gc_adapter) ? 8 : 0;
  const bool test_full = squeeze > 0 && snap.largest_gap >= need;
  if (snap.largest_gap >= need && !test_full) return;
  auto in_heap = [&](uint32_t e) {
    const uint32_t raw = rd32(e + 0x10);
    if (!try_ptr(raw, 12)) return false;
    const uint32_t data = rd32(raw + 4);
    return data >= snap.lo && data < snap.hi;
  };
  const ppc::Context saved = c;
  unsigned released = 0;
  bool forced = test_full;
  while (snap.largest_gap < need || forced) {
    int best = -1, best_score = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
      const uint32_t e = kEntries + i * kEntrySize;
      if (rd8(e) != 3 || rd8(e + 3) != 1 || !in_heap(e)) continue;
      const int score = (int16_t)rd16(e + 8);
      if (best < 0 || score < best_score) { best = (int)i; best_score = score; }
    }
    if (best < 0) break;
    if (forced) { forced = false; --squeeze; }
    call_guest(0x800174E8u, (uint32_t)best);
    ++released;
    snap = guest_heap_trace::inspect(handle, heap_trace_read32);
    if (!snap.complete) break;
  }
  c = saved;
  if (released == 0 && test_full) return;
  log("heap: %08X had no room for %u bytes; released %u file(s) read ahead and never used, largest gap now %u", handle, need, released,
      snap.largest_gap);
}
// The same stop, should it still happen (a disc with larger files of the 26, a stage too large): one
// line with the heap that had no room and the title demo's draw, so a report names the cause. At the
// entry of __assert (80388220) called from lbMemory_80014FC8 for line 233, r24 is the heap's handle and
// r30 the size asked, rounded up to 32 bytes (see trace_guest_heap_assert, the test switch's full trace).
void report_heap_stop_lines(uint32_t handle, uint32_t rounded, uint32_t caller) {
  static bool told = false;   // the entry hook and the panic itself can both come here for one stop
  if (told) return;
  told = true;
  const auto heap = guest_heap_trace::inspect(handle, heap_trace_read32);
  log("heap: no room in a fixed heap: handle %08X, %08X..%08X (%u bytes), asked %u, free %u, largest gap %u, %u blocks%s, asked by %08X",
      handle, heap.lo, heap.hi, heap.hi - heap.lo, rounded, heap.free, heap.largest_gap, (unsigned)heap.count,
      heap.complete ? "" : " (list not read to its end)", caller);
  constexpr uint32_t kPicks = 0x8049E548u;
  if (try_ptr(kPicks, 0x10))
    log("heap: title demo draw: fighters %02X %02X %02X %02X, costumes %02X %02X %02X %02X, stage %04X", rd8(kPicks), rd8(kPicks + 1),
        rd8(kPicks + 2), rd8(kPicks + 3), rd8(kPicks + 4), rd8(kPicks + 5), rd8(kPicks + 6), rd8(kPicks + 7), rd16(kPicks + 0xC));
  report_recent_files();
  log_flush();
}
static void report_heap_stop(ppc::Context& c) {
  if (c.lr != 0x80015098u || c.r[4] != 233u || !heap_trace_literal(c.r[3], "lbmemory.c")) return;
  uint32_t caller = 0;
  if (c.r[1] > 0xFFFFFFC3u || !heap_trace_read32(c.r[1] + 60u, caller)) caller = 0;
  report_heap_stop_lines(c.r[24], c.r[30], caller);
}
// The same two lines for any disc, from the panic itself (no entry hook is installed for the retail
// disc). At the entry of OSPanic the stack holds the frame of the panic helper (80388278), then
// __assert's, where the size asked was saved (+0x18), then the allocator's, which holds __assert's
// return address (+4) and, above its 0x38 bytes, its own caller's. r24 is still the heap's handle:
// neither function in between uses it. Anything that does not match this exactly prints nothing.
void report_heap_panic(ppc::Context& c) {
  uint32_t helper = c.r[1], assert_frame = 0, allocator = 0, back = 0, rounded = 0, caller = 0;
  if (c.lr != 0x80388300u || !heap_trace_read32(helper, assert_frame) || !heap_trace_read32(assert_frame, allocator) ||
      allocator > 0xFFFFFFC3u || !heap_trace_read32(allocator + 4u, back) || back != 0x80015098u ||
      assert_frame > 0xFFFFFFE7u || !heap_trace_read32(assert_frame + 0x18u, rounded)) return;
  if (!heap_trace_read32(allocator + 60u, caller)) caller = 0;
  report_heap_stop_lines(c.r[24], rounded, caller);
}
// A heap that cannot serve HSD_MemAlloc gets a piece of the memory above the console's 24 MB, added
// to it the way the SDK's OSAddToHeap adds one (the cell inserted into the free list in address
// order, merged with a free neighbour it touches). Mod discs ran out here (a long Akaneia session
// loading Kirby's copy files, an older Akaneia's online character select): a console stops with
// memory.c:52. OSAllocFromHeap takes the first free cell that fits, walking up from the lowest
// address, and these pieces sit above every console address, so an allocation that fits in the
// console's memory lands exactly where it would on a console and only one that would have stopped
// the game is placed up here. Online rollback does not save this space (Slippi's regions are the
// console's): a match that needs it is one a console could not have played at all.
namespace {
struct ExtraPiece { uint32_t start, end, heap; };
std::vector<ExtraPiece> g_extra_pieces;
constexpr uint32_t kExtraBase = ppc::RAM_BASE + ppc::EXTRA_RAM_START;
constexpr uint32_t kExtraEnd = ppc::RAM_BASE + ppc::RAM_SIZE;

// Whether any cell of the heap (free or in use) still lies in the piece. A heap the game destroyed
// and created again over its arena no longer lists it, and the piece can be handed out again.
bool extra_piece_in_use(const ExtraPiece& p, uint32_t heaps, uint32_t count) {
  if (p.heap >= count) return false;
  uint32_t size = 0, lists[2] = {};
  if (!heap_trace_read32(heaps + p.heap * 12u, size) || !heap_trace_read32(heaps + p.heap * 12u + 4u, lists[0]) ||
      !heap_trace_read32(heaps + p.heap * 12u + 8u, lists[1]) || (int32_t)size <= 0) return false;
  for (uint32_t cell : lists)
    for (uint32_t guard = 0; cell && guard < 100000u; ++guard) {
      uint32_t next = 0, bytes = 0;
      if (cell < 0x80003000u || cell >= kExtraEnd || !heap_trace_read32(cell + 4u, next) || !heap_trace_read32(cell + 8u, bytes)) return true;
      if (cell < p.end && cell + bytes > p.start) return true;
      cell = next;
    }
  return false;
}
}  // namespace

bool grow_object_heap(uint32_t heap, uint32_t bytes) {
  uint32_t heaps = 0, count = 0, size = 0, free_list = 0;
  if (!heap_trace_read32(0x804D7360u, heaps) || !heap_trace_read32(0x804D7364u, count) || heaps < 0x80003000u ||
      heaps >= kExtraBase || !count || count > 64u || heap >= count) return false;
  if (!heap_trace_read32(heaps + heap * 12u, size) || !heap_trace_read32(heaps + heap * 12u + 4u, free_list) || (int32_t)size <= 0)
    return false;
  g_extra_pieces.erase(std::remove_if(g_extra_pieces.begin(), g_extra_pieces.end(),
                                      [&](const ExtraPiece& p) { return !extra_piece_in_use(p, heaps, count); }),
                       g_extra_pieces.end());
  // The cell header is 32 bytes and cells are 32-byte aligned (OSAlloc.c). A piece of at least 1 MB,
  // so a scene that needs a little more does not come back for every allocation.
  const uint64_t need = (((uint64_t)bytes + 32u + 31u) & ~31ull);
  const uint32_t piece = (uint32_t)std::min<uint64_t>(std::max<uint64_t>((need + 0xFFFFu) & ~0xFFFFull, 0x100000u), kExtraEnd - kExtraBase);
  if (need > piece) return false;
  std::sort(g_extra_pieces.begin(), g_extra_pieces.end(), [](const ExtraPiece& a, const ExtraPiece& b) { return a.start < b.start; });
  uint32_t start = kExtraBase;
  for (const ExtraPiece& p : g_extra_pieces) {
    if (p.start >= start && p.start - start >= piece) break;
    start = std::max(start, p.end);
  }
  if (start >= kExtraEnd || kExtraEnd - start < piece) {
    log("heap: object memory %u needs %u bytes and the space above the console's memory is used up", heap, bytes);
    return false;
  }
  const uint32_t end = start + piece;
  // DLInsert: the free list in address order, merged with a free neighbour the new cell touches.
  uint32_t prev = 0, next = free_list;
  for (uint32_t guard = 0; next && guard < 100000u && next < start; ++guard) { prev = next; next = rd32(next + 4u); }
  uint32_t cell = start, cell_size = piece;
  wr32(cell + 0u, prev); wr32(cell + 4u, next); wr32(cell + 8u, cell_size);
  if (next) {
    wr32(next + 0u, cell);
    if (cell + cell_size == next) {
      cell_size += rd32(next + 8u);
      next = rd32(next + 4u);
      wr32(cell + 8u, cell_size); wr32(cell + 4u, next);
      if (next) wr32(next + 0u, cell);
    }
  }
  if (prev) {
    wr32(prev + 4u, cell);
    if (prev + rd32(prev + 8u) == cell) {
      wr32(prev + 8u, rd32(prev + 8u) + cell_size);
      wr32(prev + 4u, next);
      if (next) wr32(next + 0u, prev);
    }
  } else {
    wr32(heaps + heap * 12u + 4u, cell);
  }
  wr32(heaps + heap * 12u, size + piece);
  g_extra_pieces.push_back({start, end, heap});
  log("heap: object memory %u was full (%u bytes asked); added %u bytes above the console's memory at %08X",
      heap, bytes, piece, start);
  return true;
}

// OSPanic at memory.c:52: the game's object memory (the heaps OSAlloc serves, rebuilt for each scene)
// had no room. One line per live heap says how large it is, how much of it is free and the largest
// free piece, so a report tells a heap that is simply full from one that is in pieces. The layout is
// the SDK's: HeapArray (804D7360) points at NumHeaps (804D7364) entries of {size, free list, used
// list}, and each cell of a list is {previous, next, size}. Anything unreadable prints nothing.
void report_object_memory() {
  static bool told = false;
  if (told) return;
  told = true;
  uint32_t heaps = 0, count = 0, current = 0;
  if (!heap_trace_read32(0x804D7360u, heaps) || !heap_trace_read32(0x804D7364u, count) || !heap_trace_read32(0x804D5C08u, current)) return;
  if (heaps < 0x80003000u || heaps >= ppc::RAM_BASE + ppc::RAM_SIZE || !count || count > 64u) return;
  const auto walk = [](uint32_t cell, uint32_t& total, uint32_t& largest, uint32_t& cells) {
    for (uint32_t guard = 0; cell && guard < 100000u; ++guard) {
      uint32_t next = 0, size = 0;
      if (cell < 0x80003000u || cell >= ppc::RAM_BASE + ppc::RAM_SIZE || !heap_trace_read32(cell + 4u, next) || !heap_trace_read32(cell + 8u, size)) return false;
      total += size; largest = std::max(largest, size); ++cells;
      cell = next;
    }
    return cell == 0;
  };
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t size = 0, free_list = 0, used_list = 0;
    if (!heap_trace_read32(heaps + i * 12u, size) || !heap_trace_read32(heaps + i * 12u + 4u, free_list) ||
        !heap_trace_read32(heaps + i * 12u + 8u, used_list)) return;
    if ((int32_t)size <= 0) continue;   // a slot no heap uses
    uint32_t free_bytes = 0, largest = 0, free_cells = 0, used_bytes = 0, largest_used = 0, used_cells = 0;
    const bool whole = walk(free_list, free_bytes, largest, free_cells) & walk(used_list, used_bytes, largest_used, used_cells);
    log("heap: object memory %u%s: %u bytes, free %u in %u pieces (largest %u), used %u in %u blocks%s", i,
        i == current ? " (current)" : "", size, free_bytes, free_cells, largest, used_bytes, used_cells,
        whole ? "" : " (lists not read to their end)");
    // The largest blocks of the heap that ran out: a scene file loads as one block of its own size,
    // so these figures name what filled it when set beside the files asked for (next line).
    if (i == current) {
      std::vector<uint32_t> sizes;
      uint32_t cell = used_list;
      for (uint32_t guard = 0; cell && guard < 100000u; ++guard) {
        uint32_t next = 0, bytes = 0;
        if (cell < 0x80003000u || cell >= ppc::RAM_BASE + ppc::RAM_SIZE || !heap_trace_read32(cell + 4u, next) || !heap_trace_read32(cell + 8u, bytes)) break;
        sizes.push_back(bytes);
        cell = next;
      }
      std::sort(sizes.begin(), sizes.end(), [](uint32_t x, uint32_t y) { return x > y; });
      std::string text;
      for (size_t k = 0; k < sizes.size() && k < 10; ++k) text += (k ? ", " : "") + std::to_string(sizes[k]);
      if (!text.empty()) log("heap: its largest blocks: %s", text.c_str());
    }
  }
  log_flush();
}
// A mod disc's log gets one line per scene with the size of the game's current object heap and what
// was in use when the scene changed. A long session that ends in the stop above then shows whether
// the heap shrank from game to game or one scene simply asked for more than there is.
void note_scene_memory() {
  if (!mod_disc_active()) return;
  uint32_t heaps = 0, count = 0, current = 0, size = 0, cell = 0, used = 0, blocks = 0;
  if (!heap_trace_read32(0x804D7360u, heaps) || !heap_trace_read32(0x804D7364u, count) || !heap_trace_read32(0x804D5C08u, current)) return;
  if (heaps < 0x80003000u || heaps >= ppc::RAM_BASE + ppc::RAM_SIZE || current >= count || count > 64u) return;
  if (!heap_trace_read32(heaps + current * 12u, size) || !heap_trace_read32(heaps + current * 12u + 8u, cell) || (int32_t)size <= 0) return;
  for (uint32_t guard = 0; cell && guard < 100000u; ++guard) {
    uint32_t next = 0, bytes = 0;
    if (cell < 0x80003000u || cell >= ppc::RAM_BASE + ppc::RAM_SIZE || !heap_trace_read32(cell + 4u, next) || !heap_trace_read32(cell + 8u, bytes)) return;
    used += bytes; ++blocks;
    cell = next;
  }
  log("heap: object memory %u is %u bytes, %u in use in %u blocks", current, size, used, blocks);
}
// The game counts each fighter kind's live character items (Mario's fireball, Pikachu's jolt: the item
// kinds from It_Kind_Mario_Fire up to It_Kind_Unk4) in ft_8045996C, 33 words, one per retail kind:
// ftData_80085560(kind, +1) when such an item is made, (kind, -1) when it is removed, and a stop at
// ftdata.c:1944 when a count is below zero. Nothing else reads the counts. m-ex numbers the fighters
// it adds from 33 up and has no table of its own for this one, so an added fighter that throws a
// retail projectile counts in whatever word lies that far past the table: a retail fighter's costume
// list (33..62, 75..134), or the stage hazard entries of ftdevice.c (63..74), which keep a function
// address from the last stage that set one. An address plus one reads as a count below zero, and the
// game stopped on the fighter's first projectile (three reports on Chaos Eternal, 0.9.120). For a
// kind past the table the call is turned into "add nothing to the first count": no word changes, and
// the word that belongs to something else is left alone. Retail kinds reach the game's code as they
// came, and a mod that brings its own version of this function (it then runs from RAM) is left alone.
static void fighter_item_count_guard(ppc::Context& c) {   // ftData_80085560(kind, change)
  constexpr uint32_t kCounts = 0x8045996Cu, kKinds = 33u;
  if (ppc::runs_from_ram(0x80085560u)) return;
  const uint32_t kind = c.r[3];
  uint32_t held = 0;
  const bool readable = heap_trace_read32(kCounts + 4u * kind, held);
  if (kind < kKinds) {
    // The game's own stop follows; the log gets what the count held, which the game's line does not say.
    if (readable && (int32_t)(held + c.r[4]) < 0)
      log("fighters: the item count of fighter kind %u goes below zero (it held %08X, change %d)", kind, held, (int32_t)c.r[4]);
    return;
  }
  static bool told[256];
  bool& once = told[kind < 256u ? kind : 255u];
  if (!once) {
    once = true;
    log("fighters: kind %u is a fighter the mod adds; the game's item count has no place for it (the word that far past the table, at %08X, holds %08X), so its items are not counted",
        kind, kCounts + 4u * kind, readable ? held : 0u);
  }
  c.r[3] = 0;
  c.r[4] = 0;
}
// ftdata.c:2479: an animation of the fighter being loaded is larger than the 0x8000 bytes the game
// keeps for a fighter's current animation (ftData_80085A14 tests every row of the fighter's animation
// table when its animation file is first read; "fighter figatree over!" with the size is the game's
// own line). At that call the loop's registers are still set: r26 the row's index, r27 the fighter's
// data, r28 the kind's {file data, row count}, r30 the index times 0x18. The row goes in the log, so
// a report tells an animation a little over the limit from a row count or a file that does not match
// the fighter's data. Read only; anything that does not fit this exactly prints nothing.
static void report_figatree_stop(ppc::Context& c) {
  if (c.lr != 0x80085AD0u || c.r[4] != 2479u || !heap_trace_literal(c.r[3], "ftdata.c")) return;
  uint32_t table = 0, count = 0, name = 0, offset = 0, size = 0;
  if (c.r[27] > 0xFFFFFFF0u || !heap_trace_read32(c.r[27] + 12u, table)) return;
  const uint32_t row = table + c.r[30];
  if (!heap_trace_read32(row, name) || !heap_trace_read32(row + 4u, offset) || !heap_trace_read32(row + 8u, size)) return;
  if (!heap_trace_read32(c.r[28] + 4u, count)) count = 0;
  char text[64] = "no name";
  if (const uint8_t* s = try_ptr(name, 63)) {
    size_t n = 0;
    while (n < 62 && s[n] >= 0x20 && s[n] < 0x7F) ++n;
    if (n >= 3 && s[n] == 0) { std::memcpy(text, s, n); text[n] = 0; }
  }
  log("fighters: animation %u of %u (%s) is %u bytes at offset %u of the fighter's animation file; the game keeps 32768 bytes for one",
      c.r[26], count, text, size, offset);
  log_flush();
}
void install_mod_disc_guards() {
  if (!mod_disc_active()) return;
  if (!ppc::redirect_to_host(0x80337C60u, dvd_fast_open_checked)) log("mods: DVDFastOpen keeps the game's own code");
  if (!ppc::redirect_to_host(0x8033796Cu, dvd_convert_path_checked)) log("mods: the file name search keeps the game's own code");
  // MELEE_TEST_NO_DEMO_FIT (tests only, hidden and headless runs): the draw is left as the disc made
  // it, to show the stop and its report.
  if (!(host::test_env::getenv("MELEE_TEST_NO_DEMO_FIT") && options.no_gc_adapter)) ppc::add_entry_hook(0x801BF3F8u, title_demo_fit);
  ppc::add_entry_hook(0x80388220u, report_heap_stop);
  ppc::add_entry_hook(0x80388220u, report_figatree_stop);
  ppc::add_entry_hook(0x80085560u, fighter_item_count_guard);
}
// Every disc: the preloader's full heap (preload_make_room).
void install_preload_room() { ppc::add_entry_hook(0x80014FC8u, preload_make_room); }

}  // namespace host
