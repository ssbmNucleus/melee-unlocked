// Host services: changes made to the running recompiled game (Static Recomp). Player options done in
// the game's own code (language, True 16:9 fighters, low poly, player tags, shield colors, the Arcade
// hitboxes, Freeze Final Destination, KO explosions, HUD emblems, clean mode's music) and guards that
// keep a mod's data from stopping it (costume texture animations, tints, cameras, the title demo's
// stage, Final Destination's background). Split from host.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "test_env.h"
#include "host_internal.h"
#include "cosmetic_mods.h"
#include <windows.h>
#include "gx_core.h"
#include "gx_shader.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "exi_slippi.h"
#include "slippi_playback.h"
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include "gecko_data.h"
#include "render_options.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace host {

// ---- the game's language as a PC setting ----
std::atomic<int> g_game_language{0};
std::atomic<bool> g_cpu_20xx{false};   // "20XX CPUs", see hackpack_ai.cpp
std::atomic<bool> g_freeze_fd{false};   // "Freeze Final Destination", see apply_freeze_final_destination
std::atomic<bool> g_hide_ko_explosions{false};   // "Hide KO explosions", see apply_ko_explosions
std::atomic<bool> g_hide_hud_emblems{false};   // "Hide emblems behind percents", see apply_hud_emblems
std::atomic<uint32_t> g_community_codes{0};       // "Community codes", MU_DISPLAY_OPTION_CODES_ALL bits
int game_language_override() {
  const int choice = g_game_language.load(std::memory_order_relaxed);
  return choice == 1 ? 0 : choice == 2 ? 1 : -1;
}
// Static Recomp. The game reads its saved language through three small functions (lblanguage.c:
// lbLang_GetSavedLanguage, lbLang_IsSavedLanguageJP, lbLang_IsSavedLanguageUS). Each is replaced by a
// host function that answers with the player's choice, or reads the save exactly as the original
// does: GamePrefs.saved_language, the byte at +0x16 of the preferences at +0x1CB0 of the save data
// (r13 - 0x77C0 holds its address).
static uint32_t saved_language(ppc::Context& c) {
  const int forced = game_language_override();
  if (forced >= 0) return (uint32_t)forced;
  const uint32_t save = rd32(c.r[13] - 0x77C0u);
  if (!try_ptr(save + 0x1CB0u + 0x16u, 1)) return 1;   // before the save data exists: English, as the disc sets it
  return rd8(save + 0x1CB0u + 0x16u);
}
static void lang_get_saved(ppc::Context& c, uint8_t*) { c.r[3] = saved_language(c); }
static void lang_is_saved_jp(ppc::Context& c, uint8_t*) { c.r[3] = saved_language(c) == 0 ? 1u : 0u; }
static void lang_is_saved_us(ppc::Context& c, uint8_t*) { c.r[3] = saved_language(c) == 1 ? 1u : 0u; }
// The title demo preloads four fighters and a stage into a heap of a fixed size (heap 4,
// 0x64B400 bytes), sized for the original files: the largest original set leaves about 113 KB.
// A stage skin a megabyte or more larger than the file it replaces no longer fits with most
// fighter draws, and the game stops with lbmemory.c:233 ("memp_kouho") on the title screen.
// gm_801BF684 (801BF684) is where gm_SetupTitleDemo stores the stage it drew (a stage kind, u16 at
// 8049E554), after the fighters. When that stage's file cannot fit beside the largest original
// fighter set plus the growth of the installed fighter skins, another stage of the game's own
// demo list (803B7808, 0x1D entries) that does fit is stored in its place. With original files
// every stage fits, so nothing changes for a game without large skins.
static uint32_t demo_stage_file_size(uint32_t stkind) {
  if (stkind >= 0x11Eu) return 0;                              // stage_id_map has 0x11E entries of 12 bytes
  const uint32_t grkind = rd32(0x803E9960u + stkind * 12);
  if (grkind >= 0x6Fu) return 0;                               // stage_datas: 0x1BC bytes of pointers
  const uint32_t data = rd32(0x803DFEDCu + grkind * 4);
  if (!try_ptr(data, 12)) return 0;
  const uint32_t name = rd32(data + 8);
  if (!try_ptr(name, 1)) return 0;
  const std::string path = cstr(name, 64);
  const int32_t entry = fst_find_path(path.c_str());
  if (entry <= 0) return 0;
  return rd32(disc_fst_addr() + (uint32_t)entry * 12 + 8);
}
static void title_demo_store_stage(ppc::Context& c, uint8_t*) {
  constexpr uint32_t kHeap = 0x64B400u, kOriginalFighters = 4170176u, kStageSlot = 0x8049E554u;
  uint32_t stage = c.r[3] & 0xFFFFu;
  // MELEE_TEST_DEMO_GROWTH=<bytes>: test runs only (hidden runs never open the GameCube adapter),
  // stands in for installed skins that much larger than their originals.
  static const uint32_t test_growth = [] {
    const char* v = host::test_env::getenv("MELEE_TEST_DEMO_GROWTH");
    return v && options.no_gc_adapter ? (uint32_t)std::strtoul(v, nullptr, 0) : 0u;
  }();
  const uint64_t fighters = (uint64_t)kOriginalFighters + cosmetics::largest_fighter_growth() + test_growth;
  const auto fits = [&](uint32_t kind) {
    const uint32_t size = demo_stage_file_size(kind);
    return size != 0 && ((uint64_t)((size + 31u) & ~31u) + 0x60u + fighters) <= kHeap;
  };
  // A mod disc has its own stage tables (and its own guard): the stage is stored as drawn.
  if (!g_mod_disc && demo_stage_file_size(stage) != 0 && !fits(stage)) {
    for (uint32_t i = 0; i < 0x1Du; ++i) {
      const uint32_t other = rd16(0x803B7808u + i * 2);
      if (other == stage || !fits(other)) continue;
      static bool told = false;
      if (!told) {
        told = true;
        log("mods: the title demo drew a stage whose skin does not fit its memory with the fighters; another stage plays in the demo");
      }
      stage = other;
      break;
    }
  }
  wr16(kStageSlot, (uint16_t)stage);
}
void install_language_override() {
  if (!ppc::redirect_to_host(0x801BF684u, title_demo_store_stage))
    log("mods: the title demo's stage store could not be replaced; large stage skins can stop the title screen");
  const bool get = ppc::redirect_to_host(0x8000ADF4u, lang_get_saved);
  const bool jp = ppc::redirect_to_host(0x8000AE58u, lang_is_saved_jp);
  const bool us = ppc::redirect_to_host(0x8000AE90u, lang_is_saved_us);
  if (!get || !jp || !us) log("language: the saved-language functions could not be replaced; the game's own choice applies");
}
// True 16:9 on the Static Recomp. The renderer shows more to each side, but the game's draw test
// (ftLib_80086A8C) still answers for its own 73:60 camera, so a fighter in the added sides was not
// drawn at all. Slippi's widescreen code changes one instruction of that test ("Draw High Poly
// Models", 80086B24: the branch that returns "do not draw" becomes a nop) and nothing else in it:
// the on-screen flag, the bubble and everything else the game reads are set before that branch.
// That instruction is compiled to follow the Slippi option, which also widens the cameras, so here
// the same change is made in RAM and the function runs from there (a few calls per frame).
// Only when the instruction is the game's own: a mod that changed this function is left alone.
void clean_mode_music_frame();
// The two switches over lines of Slippi's code set that the game was translated both ways for
// (recomp/gecko.py: TWO_WAY_TEXT and the "Results Screen Offline" port codes). A network session,
// from matchmaking to the end of the match, always plays as Slippi does: everything unlocked and no
// results screen. Offline the player's "Unlock everything" setting decides the first, and the
// results screen is shown unless the player turned "Show results screen" off.
static void apply_code_switches() {
  const bool online = slippi::online::session_mode() >= 0;
  gecko::option_unlock_all = online || gx::RenderOptions::live_unlock_all();
  gecko::option_offline_results = !online && gx::RenderOptions::live_results_screen();
}
void apply_wide_fighter_draw() {
  apply_code_switches();
  clean_mode_music_frame();
  constexpr uint32_t kSite = 0x80086B24u, kBranch = 0x4182000Cu, kNop = 0x60000000u;
  static bool live = false;
  const bool want = gx::true_widescreen_active() && !gecko::option_widescreen;
  if (want == live || !ram) return;
  if (want) {
    if (rd32(kSite) != kBranch) return;   // not the game's own code here
    wr32(kSite, kNop);
    if (!ppc::redirect_function_at(kSite)) { wr32(kSite, kBranch); return; }
    live = true;
    log("widescreen: fighters in the added sides are drawn (True 16:9)");
  } else {
    if (rd32(kSite) == kNop) wr32(kSite, kBranch);
    live = false;
  }
}
// "Low poly fighters" on the Static Recomp. The game's fighter draw (ftDrawCommon_800805C8) picks
// the model with four calls to ftParts_800750C8(fp, table, show) at 80080A54..80080ACC: table 1
// (the far, low-polygon model) hidden, table 4 hidden, then table 2 (metal) hidden and table 0 (the
// full model) shown, or 0 hidden and 2 shown for a metal fighter. The decision is in those immediate
// operands, not in data, so the Source Port swaps tables 0 and 1 in C (ftdrawcommon.c) and here the
// same swap is made in front of the parts function: a dispatch-table hook that, for a call returning
// into that range (the link register), with the option on and the fighter owning a far model
// (x5AC.xC[1], fp + 0x5BC), exchanges 0 and 1 and then runs the compiled original. Compiled code
// calls its functions directly and never sees the table, so while the option is on the draw function
// runs from its own unchanged RAM bytes (the widescreen pattern above), whose calls do. Every other
// caller (the reflection, shadow and magnifier passes, which draw the far model already) is passed
// through untouched, and so is everything while the option is off. No RAM word is written. The hook
// changes DObj hidden flags only, which nothing but the draw reads: no simulation state, replay
// digest or online state moves.
static ppc::Fn g_parts_show_original = nullptr;
static void parts_show_low_poly(ppc::Context& c, uint8_t* m) {
  constexpr uint32_t kFirstReturn = 0x80080A58u, kLastReturn = 0x80080AD0u;
  if (gx::low_poly_fighters_active() && c.lr >= kFirstReturn && c.lr <= kLastReturn && c.r[4] <= 1u) {
    const uint32_t fp = c.r[3];
    if (try_ptr(fp + 0x5BCu, 4) && rd32(fp + 0x5BCu) != 0) c.r[4] ^= 1u;
  }
  g_parts_show_original(c, m);
}
void install_low_poly_fighters() {
  constexpr uint32_t kPartsShow = 0x800750C8u;
  if (g_parts_show_original) return;
  g_parts_show_original = ppc::set_hook(kPartsShow, parts_show_low_poly);
  if (!g_parts_show_original) {
    ppc::set_hook(kPartsShow, nullptr);   // never leave a hook with nothing to call behind it
    log("low poly: the fighter parts function has no dispatch entry; the option has no effect");
  }
}
// A costume from a look or a skin (Diet Melee 64's in a 0.9.106 report) can lack a texture animation
// that the fighter's data lists for its costume textures (the eyes): the game stopped with "can't
// find fighter texture anim!" (OSPanic at ftanim.c:1236). That setup is ftAnim_80070200, which the
// game's own compiler also built into ftAnim_80070308 (the fighter's animation setup at every load),
// so both run here instead, as the Source Port's does: the list of animated costume textures ends at
// the first one without an animation, and those textures stay still. Display only; with retail files
// it does what the game does. Offsets are the console's: Fighter +0x4 kind, +0x10C ft_data, +0x5CC
// CostumeTObjList {count, texture ids, 5 TObjs}, +0x5EC DObjList, +0x619 costume; ftData +0x8 -> +0x8
// {count, per-costume id lists}; HSD_TObj +0x64 aobj; HSD_AObj +0x10 rate.
static void costume_texture_list(ppc::Context& c, uint8_t* m, uint32_t fp, uint32_t data, uint32_t list, uint32_t dobjs) {
  constexpr uint32_t kCostumeTObj = 0x80075240u;   // ftParts_80075240(DObjList*, int): the n-th TObj
  static bool told_missing = false, told_over = false;
  uint32_t count = rd32(data);
  if (count > 5) {   // the game's own "fighter tobj num over!" (ftanim.c:1228): the list holds five
    if (!told_over) { told_over = true; log("fighters: a costume lists %u animated textures; the first 5 animate", count); }
    count = 5;
  }
  const uint32_t table = rd32(data + 4);
  uint32_t ids = rd32(table + 4u * rd8(fp + 0x619));
  if (ids == 0) ids = rd32(table);
  wr32(list, count);
  wr32(list + 4, ids);
  for (uint32_t i = 0; i < count; ++i) {
    c.r[3] = dobjs;
    c.r[4] = rd16(ids + 2 * i);
    c.lr = 0;
    ppc::call(c, m, kCostumeTObj);
    const uint32_t tobj = c.r[3];
    wr32(list + 8 + 4 * i, tobj);
    const uint32_t aobj = tobj ? rd32(tobj + 0x64) : 0;
    if (aobj == 0) {
      wr32(list, i);
      if (!told_missing) { told_missing = true; log("fighters: a costume has no animation for one of its animated textures; it stays still"); }
      break;
    }
    wr32(aobj + 0x10, 0);   // HSD_AObjSetRate(aobj, 0.0F)
  }
}
// The host functions below make a frame of their own below the caller's, back-chained, for the
// calls they make (as call_guest does), and give back r1 and LR as the guest function would.
static void costume_texture_anims(ppc::Context& c, uint8_t* m) {   // ftAnim_80070200(fp, data, list, dobjs)
  const uint32_t fp = c.r[3], data = c.r[4], list = c.r[5], dobjs = c.r[6];
  const uint32_t return_lr = c.lr, saved_r1 = c.r[1];
  c.r[1] = (saved_r1 - 0x40u) & ~7u;
  wr32(c.r[1], saved_r1);
  costume_texture_list(c, m, fp, data, list, dobjs);
  c.r[1] = saved_r1;
  c.lr = return_lr;
}
static void fighter_anim_setup(ppc::Context& c, uint8_t* m) {   // ftAnim_80070308(gobj)
  constexpr uint32_t kCostumeLists = 0x803C0EC0u;   // CostumeListsForeachCharacter: {costume list, count} per kind
  constexpr uint32_t kJObjAddAnimAll = 0x8036FB5Cu, kJObjReqAnimAll = 0x8036F8BCu;
  const uint32_t gobj = c.r[3];
  const uint32_t return_lr = c.lr, saved_r1 = c.r[1];
  const uint32_t fp = rd32(gobj + 0x2C), jobj = rd32(gobj + 0x28);
  c.r[1] = (saved_r1 - 0x40u) & ~7u;
  wr32(c.r[1], saved_r1);
  // HSD_JObjAddAnimAll(jobj, NULL, the costume's material animation joint, NULL)
  const uint32_t costume = rd32(kCostumeLists + 8u * rd32(fp + 4)) + 24u * rd8(fp + 0x619);
  c.r[3] = jobj; c.r[4] = 0; c.r[5] = rd32(costume + 4); c.r[6] = 0;
  c.lr = 0;
  ppc::call(c, m, kJObjAddAnimAll);
  // HSD_JObjReqAnimAll(jobj, the game's 0.0F constant)
  c.r[3] = jobj;
  c.f[1].ps0 = c.f[1].ps1 = ppc::float_bits_to_double(rd32(c.r[2] - 30532u));
  c.lr = 0;
  ppc::call(c, m, kJObjReqAnimAll);
  costume_texture_list(c, m, fp, rd32(rd32(fp + 0x10C) + 8) + 8, fp + 0x5CC, fp + 0x5EC);
  c.r[1] = saved_r1;
  c.lr = return_lr;
}
// Then the game animates those textures by index (ftAnim_80070458, and the same built into
// ftAnim_800704F0): an index past the shortened list stopped it with "texture no exist!"
// (ftanim.c:1264). As in the Source Port, that texture stays still and the rest runs as before.
static void costume_tobj_anim(ppc::Context& c, uint8_t* m, uint32_t tobj, const ppc::FPR& frame) {
  c.r[3] = rd32(tobj + 0x64);   // HSD_AObjReqAnim(tobj->aobj, frame)
  c.f[1] = frame;
  c.lr = 0;
  ppc::call(c, m, 0x8036410Cu);
  c.r[3] = tobj;                // HSD_TObjAnim(tobj)
  c.lr = 0;
  ppc::call(c, m, 0x8035EB6Cu);
}
static void costume_texture_frame(ppc::Context& c, uint8_t* m) {   // ftAnim_80070458(fp, list, index, frame)
  const uint32_t list = c.r[4], index = c.r[5];
  const ppc::FPR frame = c.f[1];
  const uint32_t return_lr = c.lr, saved_r1 = c.r[1];
  if (index < rd32(list)) {
    c.r[1] = (saved_r1 - 0x40u) & ~7u;
    wr32(c.r[1], saved_r1);
    costume_tobj_anim(c, m, rd32(list + 8 + 4 * index), frame);
    c.r[1] = saved_r1;
  }
  c.lr = return_lr;
}
static void fighter_texture_frame(ppc::Context& c, uint8_t* m) {   // ftAnim_800704F0(gobj, index, frame)
  constexpr uint32_t kTextureCallbacks = 0x803C2258u;   // ftData_UnkCallbackPairs0: {x0, x4} per kind
  const uint32_t gobj = c.r[3], index = c.r[4];
  const ppc::FPR frame = c.f[1];
  const uint32_t return_lr = c.lr, saved_r1 = c.r[1];
  const uint32_t fp = rd32(gobj + 0x2C);
  c.r[1] = (saved_r1 - 0x40u) & ~7u;
  wr32(c.r[1], saved_r1);
  if (index < rd32(fp + 0x5CC)) costume_tobj_anim(c, m, rd32(fp + 0x5D4 + 4 * index), frame);
  const uint32_t callback = rd32(kTextureCallbacks + 8u * rd32(fp + 4) + 4);
  if (callback != 0) {
    c.r[3] = gobj; c.r[4] = index; c.f[1] = frame;
    c.lr = 0;
    ppc::call(c, m, callback);
  }
  wr8(fp + 0x221E, (uint8_t)(rd8(fp + 0x221E) | 1));   // fp->x221E_b7 = true
  c.r[1] = saved_r1;
  c.lr = return_lr;
}
void install_costume_texture_anims() {
  const bool list = ppc::redirect_to_host(0x80070200u, costume_texture_anims);
  const bool setup = ppc::redirect_to_host(0x80070308u, fighter_anim_setup);
  const bool frame = ppc::redirect_to_host(0x80070458u, costume_texture_frame);
  const bool fighter_frame = ppc::redirect_to_host(0x800704F0u, fighter_texture_frame);
  if (!list || !setup || !frame || !fighter_frame) log("fighters: the costume texture setup keeps the game's own code");
}
// MELEE_TEST_ADVENTURE_SCENE=<decimal scene id> (tests only, Static Recomp): Adventure starts at
// that scene (stage * 8 + part: 25 is the Zelda fight, 33 the Kirby team), as the Source Port's hook
// of the same name does, so a hidden run can reach a later stage. The game's own start function
// (gm_801B4350) is run from RAM so that its call to gm_SetNextGameModeStateId goes through the
// dispatch table, and the id is replaced there. Nothing is installed without the variable.
static ppc::Fn g_set_next_scene_original = nullptr;
static int g_test_adventure_scene = -1;
static void set_next_scene_test(ppc::Context& c, uint8_t* m) {
  constexpr uint32_t kAdventureStart = 0x801B4350u, kAdventureStartEnd = 0x801B4408u;
  if (g_test_adventure_scene >= 0 && c.lr >= kAdventureStart && c.lr < kAdventureStartEnd) c.r[3] = (uint32_t)g_test_adventure_scene;
  g_set_next_scene_original(c, m);
}
static void apply_test_adventure_scene() {
  constexpr uint32_t kSetNextScene = 0x801A42A0u, kAdventureStart = 0x801B4350u;
  static bool tried = false;
  if (tried || !ram) return;
  tried = true;
  const char* v = host::test_env::getenv("MELEE_TEST_ADVENTURE_SCENE");
  if (!v || !*v) return;
  g_set_next_scene_original = ppc::set_hook(kSetNextScene, set_next_scene_test);
  if (!g_set_next_scene_original) { ppc::set_hook(kSetNextScene, nullptr); log("test: the scene function has no dispatch entry"); return; }
  if (!ppc::redirect_to_interpreter(kAdventureStart)) { log("test: the Adventure start could not be run from RAM"); return; }
  g_test_adventure_scene = std::atoi(v);
  log("test: Adventure starts at scene %d", g_test_adventure_scene);
}
// "Always show player tags": the game's own "show every tag" bytes (if/ifnametag.c un_804D6D70, one
// per player, which un_802FD404 sets for the modes that always show them) are held at 1 while the
// option is on, and given back as 0 once when it goes off. Display only: nothing in the match reads
// them. Retail game only, a mod disc has its own memory layout.
void apply_player_tags_always() {
  constexpr uint32_t kShowAllTags = 0x804D6D70u;
  static bool held = false;
  if (!ram || mod_disc_active()) return;
  const bool on = gx::player_tags_always_active();
  if (!on && !held) return;
  for (uint32_t i = 0; i < 6; ++i) wr8(kShowAllTags + i, on ? 1 : 0);
  held = on;
}
// "Shield colours" on the Static Recomp: the port colour table the shield bubble and the KO blast read
// (Fighter_804D650C, 4 bytes a colour index) holds the player's own colour at that player's index while
// one is chosen, and its own colour comes back when it is not. Only where the player wears its own port
// colour (StaticPlayer +0x45, Player_GetUnk45): team matches keep the team colours. Retail game only.
void apply_shield_colors() {
  static uint32_t table = 0;
  static uint8_t original[4][3];
  static bool changed[4] = {};
  if (!ram || mod_disc_active()) return;
  const uint32_t now = rd32(0x804D650Cu);
  if (now != table) { table = now; for (bool& c : changed) c = false; }   // a new table (a new match): its own colours
  if (!table || table < 0x80000000u || table >= 0x81800000u) return;
  for (int port = 0; port < 4; ++port) {
    const uint32_t at = table + 4u * port;
    const uint32_t choice = gx::shield_color(port);
    const bool own = rd8(0x80453080u + 0xE90u * port + 0x45u) == port;
    if ((choice & 0x1000000u) && own) {
      if (!changed[port]) { for (int k = 0; k < 3; ++k) original[port][k] = rd8(at + k); changed[port] = true; }
      for (int k = 0; k < 3; ++k) {
        const uint8_t want = (uint8_t)(choice >> (16 - 8 * k));
        if (rd8(at + k) != want) wr8(at + k, want);
      }
    } else if (changed[port]) {
      for (int k = 0; k < 3; ++k) wr8(at + k, original[port][k]);
      changed[port] = false;
    }
  }
}
// The Arcade look's hitbox switches on the Static Recomp: the always-display hitbox code's own five
// words, written into the game's code while a switch is on and the retail words put back when it goes
// off. The three functions run from RAM once any of them has been written (as low poly fighters does),
// so what runs is whatever words are there. Offline matches only: not online and not in replays.
// Retail code only, like the player tags.
void apply_arcade_hitboxes() {
  struct Patch { uint32_t addr, word, retail, bit; };
  static const Patch kPatches[] = {
      {0x800805FCu, 0x60000000u, 0x41820210u, gx::kArcadeHitboxes},             // ftDrawCommon_800805C8+0x34: every fighter's bubbles
      {0x80080638u, 0x480001D4u, 0x881F221Du, gx::kArcadeHitboxes},             // +0x70: over the hurtbox, reflect, absorb and shield block
      {0x8026ED50u, 0x60000000u, 0x418200F8u, gx::kArcadeProjectileHitboxes},   // it_8026ECE0+0x70: item bubbles
      {0x8026ED58u, 0x60000000u, 0x4182003Cu, gx::kArcadeProjectileHitboxes},   // +0x78
      {0x80009430u, 0x38600001u, 0x38600002u, gx::kArcadeHitboxesBehind},       // lbColl_80008FC8+0x468: cull front faces
  };
  static bool redirected = false, logged_mismatch = false;
  if (!ram || mod_disc_active()) return;
  const bool offline = slippi::online::session_mode() < 0 && !slippi::playback::enabled();
  const uint32_t bits = offline ? gx::arcade_display() & gx::kArcadeGameBits : 0u;
  if (!bits && !redirected) return;
  for (const auto& p : kPatches) {
    const uint32_t now = rd32(p.addr), want = (bits & p.bit) ? p.word : p.retail;
    if (now == want) continue;
    if (now != p.word && now != p.retail) {   // another code owns these bytes: leave them
      if (!logged_mismatch) { log("arcade: %08X holds %08X, not the retail word; hitbox switches left alone there", p.addr, now); logged_mismatch = true; }
      continue;
    }
    wr32(p.addr, want);
    if (!redirected) {
      for (const auto& q : kPatches) ppc::redirect_function_at(q.addr);
      redirected = true;
      log("arcade: the hitbox draws run from RAM");
    }
  }
}
void apply_low_poly_fighters() {
  apply_test_adventure_scene();
  constexpr uint32_t kFighterDraw = 0x800805C8u;
  static bool live = false;
  if (live || !ram || !g_parts_show_original || !gx::low_poly_fighters_active()) return;
  // Once from RAM, it stays there (a few hundred instructions per fighter draw): switching the option
  // off makes the hook pass every call through, which is the game's own picture again.
  if (!ppc::redirect_to_interpreter(kFighterDraw)) { log("low poly: the fighter draw could not be run from RAM; the option has no effect"); gx::set_low_poly_fighters(false); return; }
  live = true;
  log("low poly: fighters draw with the game's far models");
}
// Final Destination's background runs through timed phases (gr/grlast.c). One of them waits on an
// animation of the fourth background model and stops the game when the model has none there
// (assertion "aobj", grlast.c line 1012). The disc's stage always has it; a stage skin may not: two
// reports, each two and a half minutes into a match with a Final Destination skin installed. While
// such a skin is in use the function runs from RAM with the stop replaced by "this phase is not
// over yet": the match goes on and the background stays in that phase. The disc's stage never
// reaches the replaced instruction. Retail code only: any other bytes there are left alone.
// "Freeze Final Destination" on the Static Recomp. The background's think is grLast_8021B2E8, called
// only from grLast_8021AAB0, which the Source Port skips (gr/grlast.c). Here the think runs from
// RAM with its first word a return, so every caller finds it does nothing: the call in
// grLast_8021AAB0 and the cave of Slippi's DesyncProofBGTransformations. An earlier version made
// that call (8021AAE4) a nop instead, which held in Direct, but in Unranked the background still
// moved. The seed is not touched either way, so both players stay in step whatever each one chose.
// A fighter whose player slot carries a tint index past the game's four (a mod's mode can leave one)
// stops the game at fighter.c:828 ("fighter sub color num over!"). The tint is only a colour: it is
// set to none before the fighter reads it, and the fighter loads.
void install_sub_color_guard() {
  static bool done = false;
  if (done || !ram) return;
  done = true;
  // The stage's music pick (Ground_801C24F8(stkind, flags, &song)) reports the stage's main and
  // alternate songs to the jukebox, so a music pack made for the stage covers its alternate song
  // too, as on the Source Port (gr/ground.c). Read only: the pick itself is left alone.
  ppc::add_entry_hook(0x801C24F8u, [](ppc::Context& c) {
    const uint32_t param = rd32(0x8049E6C8u + 0x6B0u);   // stage_info.param
    if (!try_ptr(param, 0xB8)) return;
    const uint32_t rows = rd32(param + 0xB0), count = rd32(param + 0xB4);
    if (count > 64 || !try_ptr(rows, count * 0x64u)) return;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t row = rows + i * 0x64u;
      if (rd32(row) != c.r[3]) continue;
      slippi::jukebox::note_stage_songs((int32_t)rd32(row + 4), (int32_t)rd32(row + 8));
      slippi::jukebox::note_stage_songs((int32_t)rd32(row + 0xC), (int32_t)rd32(row + 0x10));
      return;
    }
  });
  ppc::add_entry_hook(0x80068914u, [](ppc::Context& c) {   // Fighter_UnkInitLoad_80068914(gobj, plAllocInfo*)
    if (!try_ptr(c.r[4], 8)) return;
    const uint32_t slot = rd8(c.r[4] + 4);
    if (slot >= 6) return;
    const uint32_t at = 0x80453080u + slot * 0xE90u + 0x46u;   // player_slots[slot].sub_color
    const int8_t tint = (int8_t)rd8(at);
    if (tint <= 4) return;   // the game's own test (signed): only past 4 stops it
    wr8(at, 0);
    static bool logged = false;
    if (!logged) { logged = true; log("fighters: player %u had tint %d, past the game's 4; loaded without a tint", slot + 1, tint); }
  });
}
// A camera description whose projection type is none of the game's three (a mod's stage or menu
// file can carry one) stops the game at cobj.c:2000. It is read as a perspective camera, the
// common case, and the game carries on.
void install_cobj_guard() {
  static bool done = false;
  if (done || !ram) return;
  done = true;
  ppc::add_entry_hook(0x8036A2ECu, [](ppc::Context& c) {   // CObjLoad(HSD_CObj*, HSD_CObjDesc*)
    if (!try_ptr(c.r[4], 8)) return;
    const uint32_t at = c.r[4] + 6;                        // HSD_CameraDescCommon::projection_type
    const uint32_t type = rd16(at);
    if (type >= 1 && type <= 3) return;
    wr16(at, 1);
    static bool logged = false;
    if (!logged) { logged = true; log("cameras: a camera had projection type %u; loaded as perspective", type); }
  });
  // Fountain of Dreams' reflection camera copies the main camera's aspect (grIzumi_801CCEA0) and
  // builds the water's texture matrix from it. True widescreen widens the picture in the renderer,
  // not that camera, so the reflection covered only the 4:3 middle and the water's sides stretched
  // its outer pixels. The copied aspect is widened the same way, which keeps both in step.
  ppc::add_entry_hook(0x80369C30u, [](ppc::Context& c) {   // HSD_CObjSetAspect(cobj, f1 aspect)
    if (c.lr < 0x801CCEA0u || c.lr >= 0x801CCEA0u + 0x1F0u) return;
    if (!gx::true_widescreen_active() || gecko::option_widescreen) return;
    const float widen = gx::true_widescreen_factor();
    if (widen > 0.2f && widen < 1.0f) c.f[1].ps0 = (double)(float)(c.f[1].ps0 / widen);
  });
}
void apply_freeze_final_destination() {
  constexpr uint32_t kThink = 0x8021B2E8u, kReturn = 0x4E800020u;
  static bool frozen = false;
  static uint32_t saved = 0;
  if (!ram) return;
  // This is a local backdrop preference, like the HUD and video backgrounds. A vanilla
  // launcher session suppresses gameplay mods, not the player's display settings.
  const bool want = g_freeze_fd.load(std::memory_order_relaxed);
  if (want == frozen) return;
  if (want) {
    if (!try_ptr(kThink, 4)) return;
    saved = rd32(kThink);
    if (!ppc::redirect_to_interpreter(kThink)) {
      static bool logged = false;
      if (!logged) { logged = true; log("stages: Freeze Final Destination could not run the stage's code from RAM; left off"); }
      return;
    }
    wr32(kThink, kReturn);
    frozen = true;
    log("stages: Final Destination's background frozen");
  } else {
    if (rd32(kThink) == kReturn) wr32(kThink, saved);
    ppc::undo_redirect(kThink);
    frozen = false;
    log("stages: Final Destination's background moves again");
  }
}
void apply_final_destination_guard() {
  constexpr uint32_t kPhase = 0x8021B5C4u, kLookup = 0x8021B7C4u, kStop = 0x8021B7D0u;
  static constexpr uint32_t kStopCode[4] = {0x387D0000u, 0x388003F4u, 0x38AD92ECu, 0x4816CA45u};   // __assert("grlast.c", 1012, "aobj")
  static bool done = false;
  static uint32_t calls = 0;
  if (done || !ram || calls++ % 60) return;
  // MELEE_TEST_FD_NO_ANIMATION (tests only, hidden and headless runs): the lookup finds nothing, as
  // with such a skin. MELEE_TEST_FD_NO_GUARD with it: the stop is left in, to show the report.
  const bool test = options.no_gc_adapter && host::test_env::getenv("MELEE_TEST_FD_NO_ANIMATION");
  if (!test && !cosmetics::stage_skin_active("GrNLa")) return;
  done = true;
  for (uint32_t i = 0; i < 4; ++i)
    if (!try_ptr(kStop + i * 4, 4) || rd32(kStop + i * 4) != kStopCode[i]) {
      log("mods: Final Destination's background code is not the original game's; it is left as it is");
      return;
    }
  if (!ppc::redirect_to_interpreter(kPhase)) { log("mods: Final Destination's background code could not be run from RAM; a skin without its animations can stop the game"); return; }
  const bool guard = !(test && host::test_env::getenv("MELEE_TEST_FD_NO_GUARD"));
  if (guard) wr32(kStop, 0x4800012Cu);        // b 8021B8FC: return 0, the phase stays
  if (test) {
    if (std::strcmp(host::test_env::getenv("MELEE_TEST_FD_NO_ANIMATION"), "0")) wr32(kLookup, 0x38600000u);   // li r3, 0 ("0": the stage's own animation, guard in place)
    // The test's match is played on Final Destination whatever the stage select picked (Stage_802251E8).
    ppc::add_entry_hook(0x802251E8u, [](ppc::Context& c) { if (c.r[3] >= 2 && c.r[3] <= 32) c.r[3] = 32; });
    ppc::add_entry_hook(kPhase, [](ppc::Context& c) {
      static uint32_t last = ~0u;
      const uint32_t ground = try_ptr(c.r[3] + 0x2Cu, 4) ? rd32(c.r[3] + 0x2Cu) : 0;
      const uint32_t phase = ground && try_ptr(ground + 0xC4u, 4) ? (rd32(ground + 0xC4u) >> 14) & 0xFFFFu : ~0u;
      if (phase != last) log("test: Final Destination background phase %u", phase);
      last = phase;
    });
  }
  log("mods: Final Destination skin: a background model without its animation no longer stops the game%s%s",
      test ? " (test: no animation found)" : "", guard ? "" : " (test: guard off)");
}
// "Hide KO explosions" on the Static Recomp. While it is on, the four blast-zone deaths run from RAM,
// so their call to efSync_Spawn goes through the dispatch table, where spawn_ko_explosion moves the
// position it passes far below every camera for that call alone (as the native game does in
// ft_0D31.c). The explosion still runs in full there: its particles draw random numbers and some of
// them spawn only as its model is drawn, so leaving it out would change the rest of the match.
namespace {
constexpr uint32_t kEfSyncSpawn = 0x8005FDDCu;
constexpr uint32_t kKoDeaths[4] = {0x800D3680u, 0x800D3950u, 0x800D3BC8u, 0x800D3E40u};   // left, right, down, up
constexpr uint32_t kKoSpawnReturns[4] = {0x800D3884u, 0x800D3B54u, 0x800D3DCCu, 0x800D4044u};   // after each one's bl efSync_Spawn
constexpr float kKoHiddenY = -1.0e7f;   // MU_KO_EXPLOSION_HIDDEN_Y
ppc::Fn g_ef_sync_spawn = nullptr;
void spawn_ko_explosion(ppc::Context& c, uint8_t* m) {
  const uint32_t pos = c.r[5];   // efSync_Spawn(0x42B or 0x42C, gobj, &pos, &angle, &scale, rgb1, rgb2)
  const bool ko = (c.r[3] == 0x42Bu || c.r[3] == 0x42Cu) && g_hide_ko_explosions.load(std::memory_order_relaxed) &&
                  std::find(std::begin(kKoSpawnReturns), std::end(kKoSpawnReturns), c.lr) != std::end(kKoSpawnReturns) &&
                  try_ptr(pos, 12);
  if (!ko) { g_ef_sync_spawn(c, m); return; }
  const uint32_t y = rd32(pos + 4);
  uint32_t hidden = 0;
  std::memcpy(&hidden, &kKoHiddenY, 4);
  wr32(pos + 4, hidden);
  g_ef_sync_spawn(c, m);
  wr32(pos + 4, y);   // the death goes on with its own position
}
}  // namespace
// "Hide emblems behind percents" on the Static Recomp. The emblem's draw (ifStatus_802F5E50) is a
// GX link callback, reached only through the dispatch table: while the option is on it draws
// nothing. The emblem is still made and animated, as on the Source Port (if/ifstatus.c).
namespace {
ppc::Fn g_hud_emblem_draw = nullptr;
void draw_hud_emblem(ppc::Context& c, uint8_t* m) {
  if (g_hide_hud_emblems.load(std::memory_order_relaxed)) return;
  g_hud_emblem_draw(c, m);
}
}  // namespace
void apply_hud_emblems() {
  constexpr uint32_t kEmblemDraw = 0x802F5E50u;
  if (g_hud_emblem_draw || !ram || !g_hide_hud_emblems.load(std::memory_order_relaxed)) return;
  if (!try_ptr(kEmblemDraw, 4) || rd32(kEmblemDraw) != 0x7C0802A6u) {   // mflr r0: the game's own function
    static bool logged = false;
    if (!logged) { logged = true; log("hud: the emblem draw is not the original game's; emblems stay"); }
    return;
  }
  g_hud_emblem_draw = ppc::set_hook(kEmblemDraw, draw_hud_emblem);   // null: the table is not ready yet, next frame
  if (g_hud_emblem_draw) log("hud: emblems behind percents hidden");
}
void apply_ko_explosions() {
  static bool live = false, ours[4] = {};
  if (!ram) return;
  const bool want = g_hide_ko_explosions.load(std::memory_order_relaxed);
  if (want == live) return;
  if (want) {
    if (!g_ef_sync_spawn) {
      const ppc::Fn previous = ppc::set_hook(kEfSyncSpawn, spawn_ko_explosion);
      if (!previous) return;   // the dispatch table is not ready yet: next frame
      g_ef_sync_spawn = previous;
    }
    int from_ram = 0;
    for (int i = 0; i < 4; ++i) {
      ours[i] = !ppc::runs_from_ram(kKoDeaths[i]) && ppc::redirect_to_interpreter(kKoDeaths[i]);
      from_ram += ppc::runs_from_ram(kKoDeaths[i]) ? 1 : 0;
    }
    log("ko: blast-zone explosions out of sight (%d of 4 deaths run from RAM)", from_ram);
  } else {
    for (int i = 0; i < 4; ++i)
      if (ours[i]) { ppc::undo_redirect(kKoDeaths[i]); ours[i] = false; }
    log("ko: blast-zone explosions shown");
  }
  live = want;
}

// Clean mode (a mod disc that runs without Slippi's codes) has none of the code that hands the music
// to the host's player, and the game's own disc stream does not advance here: every song repeated
// its first 1.8 seconds. The pack replaces the function that starts a stream with its own code, so
// nothing is hooked. Once per game frame the host reads the game's own bookkeeping: the id of the
// music stream (HSD_Synth_804D7760) and the disc entry it opened (HSD_Synth_804D7764). A new stream
// starts that file in the player Slippi's codes use, the game's own stream voice is kept silent
// (its volumes in the voice table, hsd_SynthSFXNodes), and the player stops when the stream is gone.
// The level follows the game's music level, the value Slippi's volume code sends
// (lbl_804D38AC * lbl_804D3884 * 2).
static std::atomic<bool> g_clean_music = false;
void install_clean_mode_music() { g_clean_music.store(true); }
void clean_mode_music_frame() {
  if (!g_clean_music.load(std::memory_order_relaxed)) return;
  static uint32_t playing_id = 0;
  const uint32_t id = rd32(0x804D7760u);
  const uint32_t node = 0x804C2C64u + (id & 0x3Fu) * 0x50u;
  const bool live = (int32_t)id > 0 && rd32(node) == id;
  if (!live) {
    if (playing_id) { slippi::jukebox::stop(); playing_id = 0; }
    return;
  }
  // MELEE_TRACE_CLEAN_MUSIC (tests only): what became of the previous stream's voice slot two
  // seconds after the game replaced it. A slot still holding the old id was never freed.
  static const bool trace = host::test_env::getenv("MELEE_TRACE_CLEAN_MUSIC") != nullptr;
  static uint32_t watched_id = 0, watched_frames = 0;
  if (trace && watched_id && ++watched_frames == 120) {
    const uint32_t old_node = 0x804C2C64u + (watched_id & 0x3Fu) * 0x50u;
    log("mods: music stream %08X two seconds after it was replaced: slot holds %08X, flags %02X, pending %u (%s)", watched_id,
        rd32(old_node), rd8(old_node + 0x09), rd8(old_node + 0x26), rd32(old_node) == watched_id ? "NOT freed" : "freed");
    watched_id = 0;
  }
  if (id != playing_id) {
    if (trace && playing_id) { watched_id = playing_id; watched_frames = 0; }
    const uint32_t entry = rd32(0x804D7764u), fst = disc_fst_addr();
    const uint32_t count = fst ? rd32(fst + 8) : 0;
    if (entry > 0 && entry < count && rd8(fst + entry * 12) == 0) {
      slippi::jukebox::start_song(rd32(fst + entry * 12 + 4), rd32(fst + entry * 12 + 8));
      static bool told = false;
      if (!told) { told = true; log("mods: this disc's music plays through the host's player"); }
      const char* name = (const char*)try_ptr(fst + count * 12 + (rd32(fst + entry * 12) & 0x00FFFFFFu), 1);
      log("mods: music stream %08X starts %s (entry %u, %u bytes)", id, name ? name : "?", entry, rd32(fst + entry * 12 + 8));
    }
    playing_id = id;
  }
  // The stream voice itself stays silent: its three volume factors, then the node is queued for the
  // sound driver's volume pass exactly as the game queues one (HSD_SynthSFXUpdateVolume): linked
  // into the list at HSD_Synth_804D774C with its pending byte set. The byte means "already on the
  // list". Setting it alone, as this did, kept the node off the list for good, and the driver's
  // key-off of that voice goes through the same list: the slot was never faded or freed, and a
  // sound that later landed in it could not be stopped until the game closed (reported as one
  // sound repeating after a few matches). A stream still loading (flag 8) queues itself when its
  // first data arrives and reads the zeroed factors then.
  if (rd32(node + 0x28) | rd32(node + 0x2C) | rd32(node + 0x34)) {
    constexpr uint32_t kVolumeList = 0x804D774Cu;
    wr32(node + 0x28, 0); wr32(node + 0x2C, 0); wr32(node + 0x34, 0);
    // MELEE_TEST_CLEAN_MUSIC_OLD (tests only): the byte alone, as before, to show the slot leak.
    static const bool old_way = host::test_env::getenv("MELEE_TEST_CLEAN_MUSIC_OLD") != nullptr;
    if (old_way) wr8(node + 0x26, 1);
    else if (rd8(node + 0x26) == 0 && (rd8(node + 0x09) & 8) == 0) {
      wr32(node + 0x20, rd32(kVolumeList)); wr32(kVolumeList, node); wr8(node + 0x26, 1);
    }
  }
  const uint32_t bits = rd32(0x804D38ACu);
  float level; std::memcpy(&level, &bits, 4);
  if (!std::isfinite(level)) return;
  static int last = -1;
  const int volume = (int)std::clamp(level * (float)(rd32(0x804D3884u) * 2u), 0.0f, 254.0f);
  if (volume != last) { last = volume; slippi::jukebox::set_melee_volume((uint8_t)volume); }
}

}  // namespace host
