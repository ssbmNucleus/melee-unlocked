// Host services: booting the recompiled guest. The main.dol into RAM (the retail one, or a mod disc's
// over the vanilla code, with the functions it changed run from RAM and watched for code it writes
// later), the Gecko boot tables, low memory, the FST's place and the registers. Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include "cosmetic_mods.h"
#include "mod_profile.h"
#include "maker_source.h"
#include "user_gecko.h"
#include "memory_range.h"
#include <windows.h>
#include <bcrypt.h>
#include "guest_registry.h"
#include "ax_ucode.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "exi_slippi.h"
#include "slippi_playback.h"
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include "gecko_data.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace host {

// Where boot put the disc's file table. The game's own lookups use its cached copy; the host's
// used to read the pointer back from low memory (80000038), which a stray guest store through a
// near-null pointer can overwrite (addresses are masked into RAM, so it lands there instead of
// faulting as on a console). The walk then ran off into garbage: "host access outside RAM".
static uint32_t g_fst_addr = 0;

static bool trace_ax_voice_window() {
  const char* range = host::test_env::getenv("MELEE_TRACE_AX_SFX");
  unsigned first = 0, last = 0;
  const uint32_t retrace = retrace_count();
  return range && std::sscanf(range, "%u:%u", &first, &last) == 2 &&
         first <= last && retrace >= first && retrace <= last;
}

static void trace_ax_voice(const ax::VoiceTrace& trace) {
  if (!trace_ax_voice_window()) return;
  log("[ax-voice-legacy] retrace=%u block=%llu ms=%u pb=%08X cur=%08X>%08X end=%08X ratio=%08X frac=%04X>%04X env=%04X/%04X input=%d/%d peak=%d output=%d/%d peak=%d",
      retrace_count(), (unsigned long long) trace.frame, trace.millisecond,
      trace.pb_addr, trace.cur_before, trace.cur_after, trace.end_addr,
      trace.ratio, trace.frac_before, trace.frac_after, trace.volume_before,
      trace.volume_delta, trace.input_first, trace.input_last,
      trace.input_peak, trace.output_first, trace.output_last,
      trace.output_peak);
}

// MELEE_TRACE_AX_VOICES (M4 diagnostic, off by default): every AX frame and every voice block the
// ucode renders, with hashes of the voice's own samples before the final mix (ax_ucode.h).
static void trace_ax_frame(const char* line) {
  log("[ax-vtrace] retrace=%u tb=%llu vi=%llu %s", retrace_count(), (unsigned long long)(cpu ? cpu->tb : 0),
      (unsigned long long)(next_retrace_tb() - TB_PER_FRAME), line);
}

// ---------------- boot ----------------
static uint32_t disc_dol_offset() {
  uint8_t hdr[4];
  if (!disc_read(0x420, hdr, 4)) die("cannot read disc DOL offset");
  return ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
}
// Both engines are built for the retail main.dol: the recompiled one runs its code, the native one
// reads its data. A modded disc (a patched DOL, m-ex and friends) fails the SHA-1 of NTSC 1.02.
// True for the retail NTSC 1.02 main.dol (kVanillaDolSize bytes).
bool vanilla_dol_image(const std::vector<uint8_t>& image) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  uint8_t digest[20];
  const uint8_t expected[20] = {0x08,0xe0,0xbf,0x20,0x13,0x4d,0xfc,0xb2,0x60,0x69,0x96,0x71,0x00,0x45,0x27,0xb2,0xd6,0xbb,0x1a,0x45};
  if (image.size() != kVanillaDolSize) return false;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
    die("cannot initialize game-image verification");
  NTSTATUS hash_status = BCryptHash(algorithm, nullptr, 0, (PUCHAR)image.data(), kVanillaDolSize, digest, sizeof digest);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hash_status >= 0 && std::memcmp(digest, expected, sizeof digest) == 0;
}
bool disc_has_vanilla_dol() {
  std::vector<uint8_t> image(kVanillaDolSize);
  if (!disc_read(disc_dol_offset(), image.data(), kVanillaDolSize))
    die("this file is not a full Melee disc image. Use a clean, uncompressed Melee NTSC 1.02 ISO (a trimmed or compressed image will not work)");
  return vanilla_dol_image(image);
}
// ---- mod discs (Static Recomp) ----
// The recompiled game is the vanilla 1.02 code. For a mod disc the vanilla game is booted first from
// the player's own vanilla disc (to build the reference image), then the mod's main.dol replaces it in
// RAM and every function whose bytes differ runs from RAM (ppc::redirect_changed_functions).
bool g_mod_disc = false;
static std::vector<std::pair<uint32_t, uint32_t>> g_text_ranges;   // vanilla DOL text sections
static std::vector<uint8_t> g_mod_reference;                       // vanilla + Slippi code, per text range, concatenated
static std::vector<uint8_t> g_mod_reference_boot;                  // the same before Slippi's served table joined it
static std::vector<uint8_t> g_vanilla_text;                        // vanilla code alone, same layout
static std::vector<uint32_t> g_mod_block_versions;                  // write generations of the watched code blocks

bool mod_disc_active() { return g_mod_disc; }

// Clean mode: a mod disc that was built for the plain game and cannot share RAM with Slippi's code
// (the 20XX Hack Pack calls code inside its character select file at fixed heap addresses, and
// Slippi's heap table moves that file). Such a disc runs without Slippi's codes: only the disc's own
// code is in RAM, and every function Slippi's codes touch runs from RAM too, because the compiled
// game has those codes built in. No online play and no replays in this mode.
bool g_mod_clean = false;
bool mod_clean_mode() { return g_mod_clean; }

bool mod_reference_from_table() {
  static const bool boot_only = [] { const char* v = host::test_env::getenv("MELEE_MOD_REFERENCE"); return v && std::strcmp(v, "boot") == 0; }();
  return !boot_only;
}

// The reference byte for a guest address inside the text ranges (the vectors concatenate them).
static uint8_t* reference_byte(std::vector<uint8_t>& image, uint32_t addr) {
  size_t at = 0;
  for (const auto& r : g_text_ranges) {
    if (addr >= r.first && addr - r.first < r.second) return image.data() + at + (addr - r.first);
    at += r.second;
  }
  return nullptr;
}

// Makes the reference differ from RAM at these bytes, so the function that owns them runs from RAM.
// A function the port implements on the host keeps that implementation: Slippi's change to it never
// ran in the compiled game either.
static void mod_reference_force(uint32_t addr, uint32_t n) {
  uint32_t lo = 0, hi = 0;
  if (ppc::function_bounds(addr, &lo, &hi) && ppc::compiled_as_host_function(lo)) return;
  for (uint32_t i = 0; i < n; ++i)
    if (uint8_t* p = reference_byte(g_mod_reference, addr + i)) *p = (uint8_t)~rd8(addr + i);
}

bool mod_reference_set(uint32_t addr, const uint8_t* bytes, uint32_t n) {
  if (!g_mod_disc || g_mod_reference.empty()) return false;
  bool any = false;
  for (uint32_t i = 0; i < n; ++i)
    if (uint8_t* p = reference_byte(g_mod_reference, addr + i)) { *p = bytes[i]; any = true; }
  return any;
}

static void load_dol_from_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) die("cannot open the vanilla disc %s", path.c_str());
  auto read_at = [&](uint64_t off, void* dst, size_t n) {
    return _fseeki64(f, (long long)off, SEEK_SET) == 0 && std::fread(dst, 1, n, f) == n;
  };
  uint8_t hdr[4];
  if (!read_at(0x420, hdr, 4)) die("cannot read the vanilla disc header");
  const uint32_t dol_offset = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
  // The base has to be the retail game: the recompiled code is that game, and the mod's own code is
  // found by comparing against it. A modified disc here (the mod disc itself set as the Melee ISO,
  // for one) made the comparison find nothing, so the retail code ran on the mod's files and the
  // game stopped at boot on the first thing that no longer fit (the mod's larger sound bank).
  {
    std::vector<uint8_t> image(kVanillaDolSize);
    if (!read_at(dol_offset, image.data(), kVanillaDolSize) || !vanilla_dol_image(image)) {
      std::fclose(f);
      die("the Melee ISO (%s) is a modified disc. A mod runs on top of an unmodified Melee NTSC 1.02 ISO: "
          "choose your clean disc as the Melee ISO on the Play page and keep the mod's ISO in the Mods folder",
          path.c_str());
    }
  }
  uint8_t dh[0x100];
  if (!read_at(dol_offset, dh, sizeof dh)) die("cannot read the vanilla DOL header");
  auto be = [&](int o) { return ((uint32_t)dh[o] << 24) | ((uint32_t)dh[o + 1] << 16) | ((uint32_t)dh[o + 2] << 8) | dh[o + 3]; };
  g_text_ranges.clear();
  std::vector<std::pair<uint32_t, uint32_t>> data_sections;
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
    if (!size) continue;
    if (!read_at(dol_offset + off, ptr(addr, size), size)) die("cannot read vanilla DOL section %d", i);
    if (i < 7) g_text_ranges.push_back({addr, size});
    else data_sections.push_back({addr, size});
  }
  std::fclose(f);
  // Slippi's helper branch slots (800055EC..800056BC, inside a DOL data section): the recompiled
  // game compiled each one as a function that jumps to Slippi's helper. Mods put their own helpers
  // in the same slots (Akaneia replaces 8 of them, one is its sound bank request), so the slots are
  // watched as code too: a slot the mod rewrites runs the mod's branch, not the compiled one.
  for (const auto& d : data_sections) {
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (size_t i = 0; i < guest::name_table_count; ++i) {
      const uint32_t a = guest::name_table[i].addr;
      if (a < d.first || a - d.first >= d.second) continue;
      if (std::strncmp(guest::name_table[i].name, "gecko_hook_", 11) != 0) continue;
      lo = std::min(lo, a);
      hi = std::max(hi, a + 4);
    }
    if (lo < hi && hi - lo <= 0x1000) g_text_ranges.push_back({lo, hi - lo});
  }
  g_vanilla_text.clear();
  for (const auto& r : g_text_ranges) { const uint8_t* p = ptr(r.first, r.second); g_vanilla_text.insert(g_vanilla_text.end(), p, p + r.second); }
}

// Recognised by what the disc carries, never by its file name: the pack's numbered character select
// file (which holds the code the pack calls at fixed heap addresses) in place of the retail one. Every
// 20XX Hack Pack build has it; AI_Engine.bin, StageSwapTable.bin and IfAl0.usd came with 5.0, and
// requiring them left 4.07++ running with Slippi's codes, where its character select jumped into
// the heap (80FD0240) on entry.
static bool disc_is_fixed_address_pack() {
  return disc_find_file("MnSlChr.0sd", nullptr, nullptr) && !disc_find_file("MnSlChr.usd", nullptr, nullptr);
}

static void load_dol_from_disc() {
  const uint32_t dol_offset = disc_dol_offset();
  if (!disc_has_vanilla_dol()) {
    if (options.mod_base_iso.empty())
      die("ISO DOL does not match vanilla Melee NTSC 1.02; recompiled code cannot run this image");
    // A mod disc: boot the vanilla code first; the mod's code is laid over it after the Slippi
    // tables are installed (apply_mod_code, boot_setup).
    load_dol_from_file(options.mod_base_iso);
    g_mod_disc = true;
    install_mod_disc_guards();
    log("boot: mod disc; vanilla code from %s, the mod's code follows", options.mod_base_iso.c_str());
    if (disc_is_fixed_address_pack() && !host::test_env::getenv("MELEE_MOD_NO_CLEAN")) {
      g_mod_clean = true;
      install_clean_mode_music();
#ifdef MELEE_NO_SLIPPI   // the same lines of this file's boot path, worded for the build without that layer
      log("mods: this disc runs in clean mode (its own code only); online play and replays are off for it");
#else
      log("mods: this disc runs in clean mode (its own code only, without Slippi's codes); online play and replays are off for it");
#endif
    }
    return;
  }
  uint8_t dh[0x100];
  if (!disc_read(dol_offset, dh, sizeof dh)) die("cannot read DOL header");
  auto be = [&](int o) { return ((uint32_t)dh[o] << 24) | ((uint32_t)dh[o + 1] << 16) | ((uint32_t)dh[o + 2] << 8) | dh[o + 3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
    if (!size) continue;
    if (!disc_read(dol_offset + off, ptr(addr, size), size)) die("cannot read DOL section %d", i);
  }
  // The DOL header's bss range overlaps the loaded .sdata section; the guest's own
  // __init_data zeroes .bss/.sbss precisely and RAM starts zeroed, so do not memset here.
  log("boot: DOL loaded from disc offset %08X, bss %08X+%X, entry %08X", dol_offset, be(0xD8), be(0xDC), be(0xE0));
}

// Reproduces Slippi Dolphin's boot-time Gecko installation in guest RAM: codehandler.bin at
// 0x80001800, bootloader.gct at 0x800028B8, then the effect of running the handler once (its
// 32-bit writes and the C2 hook branches into the caves inside the table). The recompiled code
// already contains these patches; this keeps RAM identical to what the game expects to read.
static void apply_gecko_boot_ram() {
  std::memcpy(ptr(0x80001800u, (uint32_t)gecko::codehandler_bin_size), gecko::codehandler_bin, gecko::codehandler_bin_size);
  mark_ram_write(0x80001800u, (uint32_t)gecko::codehandler_bin_size);
  wr32(0x80001D6Cu, 0x4E800020u);   // USB Gecko I/O replaced by blr, as Slippi does
  wr32(0x80001800u, 0xD01F1BADu);   // handler magic
  std::memcpy(ptr(0x800028B8u, (uint32_t)gecko::bootloader_gct_size), gecko::bootloader_gct, gecko::bootloader_gct_size);
  mark_ram_write(0x800028B8u, (uint32_t)gecko::bootloader_gct_size);
  wr8(0x80001807u, 1);              // codes on
  for (size_t i = 0; i < gecko::boot_writes_count; ++i) {
    const gecko::Write& w = gecko::boot_writes[i];
    std::memcpy(ptr(w.addr, w.size), w.data, w.size);
    mark_ram_write(w.addr, w.size);
  }
  // The Lossless adapter's lag test (lag_test_codes.h): Dolphin runs those codes with Slippi's from the
  // first frame, so the game copies their default rules; the player's codes here first run at the
  // first controller read, after that copy. Their rule words go in with the boot writes.
  if (std::getenv("MELEE_LAG_TEST")) {
    static const uint32_t kRules[][2] = {{0x803D4A48u, 0x00340000u}, {0x803D4A60u, 0xFF000000u},
                                         {0x803D4A4Cu, 0x63000A01u}, {0x803D4A50u, 0x00010100u},
                                         {0x803D4A78u, 0xE4000000u}};
    for (const auto& w : kRules) wr32(w[0], w[1]);
  }
  for (size_t i = 0; i < gecko::boot_hooks_count; ++i) {
    const gecko::HookInstall& h = gecko::boot_hooks[i];
    wr32(h.hook, 0x48000000u | ((h.cave_addr - h.hook) & 0x03FFFFFCu));
    uint32_t last = h.cave_addr + (h.words - 1) * 4;
    wr32(last, 0x48000000u | (((h.hook + 4) - last) & 0x03FFFFFCu));
  }
}
static void install_gecko_boot() {
#ifdef MELEE_NO_SLIPPI
#define BOOT_TABLES_NAME "code tables"
#else
#define BOOT_TABLES_NAME "Slippi code tables"
#endif
  if (!gecko::codehandler_bin_size) { log("boot: translated without " BOOT_TABLES_NAME); return; }
  apply_gecko_boot_ram();
  slippi::init();
  log("boot: " BOOT_TABLES_NAME " installed (%zu boot writes, %zu boot hooks, main GCT %zu bytes served over EXI)",
      gecko::boot_writes_count, gecko::boot_hooks_count, gecko::slippi_gct_size);
#undef BOOT_TABLES_NAME
}

// The mod's main.dol over the vanilla code in RAM, then the Slippi tables again (as Dolphin applies
// them to whatever disc it boots), then every function whose code differs from vanilla + Slippi runs
// from RAM. The code ranges stay watched so code the mod writes later (its own code lists, a memory
// card payload) is picked up at the next retrace.
static void apply_mod_code() {
  g_mod_reference.clear();
  for (const auto& r : g_text_ranges) {
    const uint8_t* p = ptr(r.first, r.second);
    g_mod_reference.insert(g_mod_reference.end(), p, p + r.second);
  }
  g_mod_reference_boot = g_mod_reference;
  // Clean mode: Slippi's handler and boot table leave RAM before the mod's code goes in (the mod may
  // use that low memory itself), and nothing may reach their compiled twins by address any more. The
  // same holds for the heap addresses the compiled copies of Slippi's main list were made for: the
  // mod's own files load there now.
  if (g_mod_clean && gecko::codehandler_bin_size) {
    std::memset(ptr(0x80001800u, (uint32_t)gecko::codehandler_bin_size), 0, gecko::codehandler_bin_size);
    std::memset(ptr(0x800028B8u, (uint32_t)gecko::bootloader_gct_size), 0, gecko::bootloader_gct_size);
    mark_ram_write(0x80001800u, (uint32_t)gecko::codehandler_bin_size);
    mark_ram_write(0x800028B8u, (uint32_t)gecko::bootloader_gct_size);
    const uint32_t text_start = g_text_ranges.empty() ? 0x80003100u : g_text_ranges.front().first;
    ppc::disable_dispatch_range(0x80001800u, std::min(text_start, 0x80001800u + (uint32_t)gecko::codehandler_bin_size));
    ppc::disable_dispatch_range(0x800028B8u, std::min(text_start, 0x800028B8u + (uint32_t)gecko::bootloader_gct_size));
    if (gecko::gct_base_used)
      ppc::disable_dispatch_range(gecko::gct_base_used, gecko::gct_base_used + (uint32_t)gecko::slippi_gct_size + 0x1000u);
  }
  const uint32_t dol_offset = disc_dol_offset();
  uint8_t dh[0x100];
  if (!disc_read(dol_offset, dh, sizeof dh)) die("cannot read the mod's DOL header");
  auto be = [&](int o) { return ((uint32_t)dh[o] << 24) | ((uint32_t)dh[o + 1] << 16) | ((uint32_t)dh[o + 2] << 8) | dh[o + 3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
    if (!size) continue;
    if (!try_ptr(addr, size) || !disc_read(dol_offset + off, ptr(addr, size), size)) die("cannot read the mod's DOL section %d", i);
  }
  // Slippi's boot patches again over the mod's code, as Slippi Dolphin applies them to any disc.
  if (g_mod_clean) {
    // The reference so far is retail plus Slippi's boot codes. Every word Slippi's main list would
    // install is compiled in as well, so each of those is forced to differ: the functions that own
    // them run the disc's code from RAM, whatever the optional codes are set to.
    uint32_t forced = 0;
    slippi::for_each_served_code_write([&](uint32_t addr, uint32_t n) { mod_reference_force(addr, n); ++forced; });
#ifdef MELEE_NO_SLIPPI
    log("mods: clean mode; %u places the main code list changes run the disc's code", forced);
#else
    log("mods: clean mode; %u places Slippi's main list changes run the disc's code", forced);
#endif
  } else if (gecko::codehandler_bin_size) {
    apply_gecko_boot_ram();
    ppc::add_ram_code_range(0x80001800u, 0x80001800u + (uint32_t)gecko::codehandler_bin_size);
    ppc::add_ram_code_range(0x800028B8u, 0x800028B8u + (uint32_t)gecko::bootloader_gct_size);
  }
  size_t total = 0, at = 0;
  for (const auto& r : g_text_ranges) {
    total += ppc::redirect_changed_functions(g_mod_reference.data() + at, ram, r.first, r.second);
    at += r.second;
  }
  // A function the mod changed may be small enough to have been built into its callers, and such a
  // caller still carries the original. Every mod disc is checked, not only clean mode (where the
  // functions Slippi's codes touch go to RAM too): a mod that rewrote the four-instruction lookup of
  // Kirby's Yoshi egg model kept reading the original, empty table from the one caller that had it
  // built in, and the game stopped on a missing model when that Kirby caught someone.
  total += ppc::redirect_inlined_callers(ram);
  g_mod_block_versions.assign(ppc::RAM_WATCH_COUNT, 0);
  for (const auto& r : g_text_ranges) ppc::watch_ram_range(r.first & 0x3FFFFFFFu, r.second);
  for (uint32_t b = 0; b < ppc::RAM_WATCH_COUNT; ++b) g_mod_block_versions[b] = ppc::g_ram_versions[b].load();
  log("mods: the mod's main.dol is in; %zu game functions run its code", total);

  // Clean mode has no Slippi code in the game, so there is no online menu and nothing to pair: the
  // build is marked as a mod without an identity, which every matchmaking mode refuses.
  if (g_mod_clean) {
    char title[0x41] = {};
    disc_read(0x20, title, 0x40);
    slippi::online::LocalBuild build;
    build.static_engine = true;
    build.mod_view = true;
    build.name = title[0] ? std::string(title) : std::string("a modded disc");
    slippi::online::set_local_build(build);
    slippi::online::set_native_gameplay_profile(slippi::online::NativeGameplayProfile::OtherMod);
    return;
  }
  // Online: a mod disc plays Direct and Teams, against the same mod on every side (Unranked and Party refuse).
  // Hash the entire disc, including every file payload and DOL data section. The patched text
  // also binds the identity to Slippi's boot code. No path/mtime cache can reuse a stale digest.
  {
    std::string hex;
    const std::string disc_hash = source_port::mods::sha256_file(std::filesystem::u8path(options.iso));
    if (slippi::online::native_mod_fingerprint_valid(disc_hash)) {
      source_port::mods::Sha256 identity;
      identity.update("mu-static-mod-disc-v2\n");
      identity.update(disc_hash);
      for (const auto& r : g_text_ranges) identity.update(ptr(r.first, r.second), r.second);
      hex = identity.hex();
    }
    // Melee Maker: the fingerprint its native engine on the Source Port sends too (maker_source.h), so
    // players on either engine can play each other in Direct.
    if (source_port::maker::attach(std::filesystem::u8path(options.iso))) {
      hex = source_port::maker::fingerprint(disc_hash);
      log("mods: Melee Maker %s: Direct plays players on the same release on either engine", source_port::maker::kBuild);
    }
    if (!slippi::online::native_mod_fingerprint_valid(hex))
      log("mods: cannot verify the disc's complete content; Direct matchmaking will be refused");
    char title[0x41] = {};
    disc_read(0x20, title, 0x40);
    slippi::online::LocalBuild build;
    build.static_engine = true;
    build.mod_view = true;
    build.extended_content = true;   // the Static engine executes the mod's own code
    build.fingerprint = hex;
    build.name = title[0] ? std::string(title) : std::string("a modded disc");
    slippi::online::set_local_build(build);
    slippi::online::set_native_gameplay_profile(slippi::online::NativeGameplayProfile::OtherMod);
    log("mods: online plays Direct and Teams against the same mod (%s, %s)", build.name.c_str(), hex.substr(0, 16).c_str());
  }
}
// At each retrace: code blocks written since the last look are compared again. A 64 KB block can
// hold the end of one text range and the start of the next (block 0 holds both vanilla ranges), so
// the changed blocks are collected first, every range part in them is compared, and only then are
// their versions stored (storing inside the range loop skipped the second range's part).
void check_mod_code_writes() {
  if (!g_mod_disc || g_mod_block_versions.empty()) return;
  std::vector<std::pair<uint32_t, uint32_t>> changed;   // (block, the version read now)
  for (const auto& r : g_text_ranges) {
    const uint32_t first = (r.first & 0x3FFFFFFFu) >> ppc::RAM_WATCH_SHIFT, last = ((r.first & 0x3FFFFFFFu) + r.second - 1) >> ppc::RAM_WATCH_SHIFT;
    for (uint32_t b = first; b <= last; ++b) {
      const uint32_t v = ppc::g_ram_versions[b].load(std::memory_order_relaxed);
      if (v == g_mod_block_versions[b]) continue;
      if (std::find_if(changed.begin(), changed.end(), [&](const auto& c) { return c.first == b; }) == changed.end()) changed.push_back({b, v});
    }
  }
  if (changed.empty()) return;
  size_t at = 0;
  for (const auto& r : g_text_ranges) {
    for (const auto& c : changed) {
      const uint32_t b = c.first;
      const uint32_t lo = std::max(r.first, ppc::RAM_BASE + (b << ppc::RAM_WATCH_SHIFT));
      const uint32_t hi = std::min(r.first + r.second, ppc::RAM_BASE + ((b + 1) << ppc::RAM_WATCH_SHIFT));
      if (hi <= lo) continue;
      const size_t off = at + (lo - r.first);
      const size_t n = ppc::redirect_changed_functions(g_mod_reference.data() + off, ram, lo, hi - lo);
      if (n) log("mods: %zu more functions run code written at run time (block %08X)", n, lo);
      if (n) ppc::redirect_inlined_callers(ram);
      ppc::report_kept_compiled(g_mod_reference_boot.data() + off, g_mod_reference.data() + off, ram, lo, hi - lo);
    }
    at += r.second;
  }
  for (const auto& c : changed) g_mod_block_versions[c.first] = c.second;
}

void boot_setup() {
  install_guest_heap_trace();
  install_text_heap_for_user_codes();
  init_state_digest();
  if (!options.state_trace.empty()) {
    g_state_trace = std::fopen(options.state_trace.c_str(), "w");
    if (!g_state_trace) die("cannot open state trace");
    std::fprintf(g_state_trace, "retrace,cpu,ram,aram,events\n");
  }
  ram = (uint8_t*)std::calloc(ppc::RAM_SIZE + 64, 1);
  aram = (uint8_t*)std::calloc(0x01000000, 1);
  ax::set_memory({rd16, rd32, wr16, wr32, aram, 0x01000000});
  ax::set_voice_trace(host::test_env::getenv("MELEE_TRACE_AX_SFX") ? trace_ax_voice
                                                        : nullptr);
  ax::set_frame_trace(host::test_env::getenv("MELEE_TRACE_AX_VOICES") ? trace_ax_frame : nullptr);
  ax::reset();
  cpu = new ppc::Context();
  std::memset(cpu, 0, sizeof *cpu);
  if (!ram || !aram) die("out of memory");
  std::memset(g_mmio, 0, sizeof g_mmio);

  load_dol_from_disc();

  // Low memory, mirroring Dolphin's Boot_BS2Emu.cpp (GC path) plus what the apploader leaves.
  disc_read(0, ptr(0x80000000, 0x20), 0x20);              // disc id
  wr32(0x80000020, 0x0D15EA5E);                      // booted from bootrom
  // Physical memory size: the console's, or Slippi Dolphin's for a mod disc (ppc.h DOLPHIN_RAM_SIZE).
  const uint32_t ram_size = g_mod_disc ? ppc::DOLPHIN_RAM_SIZE : ppc::CONSOLE_RAM_SIZE;
  wr32(0x80000028, ram_size);
  wr32(0x8000002C, 0x10000006);                      // console type (Dolphin reports devkit)
  wr32(0x80000030, 0);                               // arena lo (0 = use linker default)
  wr32(0x800000CC, 0);                               // NTSC
  wr32(0x800000D0, 0x01000000);                      // ARAM size
  wr32(0x800000F0, ram_size);                        // simulated memory size
  wr32(0x800000F8, 0x09A7EC80);                      // bus clock
  wr32(0x800000FC, 0x1CF7C580);                      // cpu clock
  wr32(0x80000300, 0x4C000064);                      // rfi stubs
  wr32(0x80000800, 0x4C000064);
  wr32(0x80000C00, 0x4C000064);
  const uint64_t tb = console_epoch_ticks();
  // Like Dolphin: the timebase register starts near zero; 0x800030D8 holds the epoch adjust
  // that __OSGetSystemTime adds to mftb.
  wr32(0x800030D8, (uint32_t)(tb >> 32));
  wr32(0x800030DC, (uint32_t)tb);
  cpu->tb = 0;

  // Apploader: FST at the top of RAM, arena hi below it.
  if (!valid_range(0, g_fst_max, ppc::RAM_SIZE) || g_fst_size > g_fst_max) die("invalid FST size");
  uint32_t fst_addr = (ppc::RAM_BASE + ram_size - g_fst_max) & ~31u;
  if (!disc_read(g_fst_offset, ptr(fst_addr, g_fst_size), g_fst_size)) die("cannot read FST");
  // Resolve logical asset paths against this exact ISO before the guest initializes DVD. Selected
  // entries keep their vanilla starts but receive their replacement lengths; DVDFileInfo reads are
  // then served by file identity, so a larger mod never aliases the next physical ISO file.
  cosmetics::apply_to_fst(ptr(fst_addr, g_fst_size), g_fst_size);
  cosmetics::set_online_probe([] { return ram != nullptr && rd8(0x80479D30) == 8; });
  // The buttons held on the four controllers (the game's pad copy, 0x44 bytes a port): a disc with
  // alternate stages picks by them as a match starts, as its own stage code does.
  cosmetics::set_stage_button_probe([]() -> uint32_t {
    uint32_t held = 0;
    if (ram) for (uint32_t port = 0; port < 4; ++port) held |= rd32(0x804C20BCu + port * 0x44u);
    return held;
  });
  wr32(0x80000038, fst_addr);
  g_fst_addr = fst_addr;
  wr32(0x8000003C, g_fst_max);
  const uint32_t gecko_bytes = user_gecko::static_memory_required();
  const uint32_t gecko_address = gecko_bytes ? (fst_addr - gecko_bytes) & ~0xFFFFu : 0;
  wr32(0x80000034, gecko_bytes ? gecko_address : fst_addr); // arena hi
  if (gecko_bytes) user_gecko::static_memory_start(gecko_address, fst_addr - gecko_address);
  log("boot: FST %u bytes at %08X (max %X), arena hi %08X", g_fst_size, fst_addr, g_fst_max,
      gecko_bytes ? gecko_address : fst_addr);
  install_gecko_boot();
  if (g_mod_disc) apply_mod_code();
#ifndef MELEE_NO_SLIPPI
#endif
  // MELEE_TEST_INTERPRET=<addr>[,<addr>...]: run those functions in the interpreter on any disc, so
  // interpreted and compiled behaviour can be compared on the vanilla game. Test runs only (hidden
  // and headless runs never open the GameCube adapter, which is what marks them here).
  if (const char* v = host::test_env::getenv("MELEE_TEST_INTERPRET"); v && options.no_gc_adapter) {
    for (const char* p = v; *p;) {
      char* end = nullptr;
      const uint32_t a = (uint32_t)std::strtoul(p, &end, 16);
      if (end == p) break;
      log("test: %08X %s %s", a, symbol_name(a), ppc::redirect_to_interpreter(a) ? "runs in the interpreter" : "could not be redirected");
      p = (*end == ',') ? end + 1 : end;
    }
  }

  cpu->msr = 0x00002030u | 0x8000u;                  // FP | DR | IR | EE
  cpu->fpscr = 0;
  ppc::update_mxcsr(*cpu);
  g_next_frame = std::chrono::steady_clock::now();
}

// The file table's address for the host's own lookups. Says so once, and puts the pointer back,
// when low memory no longer holds it.
uint32_t disc_fst_addr() {
  if (!g_fst_addr) return rd32(0x80000038u);
  const uint32_t now = rd32(0x80000038u);
  if (now != g_fst_addr) {
    static bool told = false;
    if (!told) {
      told = true;
      log("low memory: the file table pointer at 80000038 was overwritten (now %08X, was %08X); put back", now, g_fst_addr);
    }
    wr32(0x80000038u, g_fst_addr);
  }
  return g_fst_addr;
}

}  // namespace host
