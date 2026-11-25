// Source Port host: settings the game reads. Display, game and audio option words, 20XX TE's
// features and its in-game settings, Zelda as Sheik.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {
// The HUD sliders (stock icon and damage number size), display only, as the backend last set them.
uint32_t h_hud_scales() { return gx::hud_scales_packed(); }
static_assert(gx::kArcadeHitboxes == 0x20u && gx::kArcadeProjectileHitboxes == 0x40u && gx::kArcadeHitboxesBehind == 0x80u,
              "user_gecko::arcade_bits_of_codes");
static_assert(gx::kArcadeHitboxes == MU_DISPLAY_OPTION_HITBOXES &&
              gx::kArcadeProjectileHitboxes == MU_DISPLAY_OPTION_PROJECTILE_HITBOXES &&
              gx::kArcadeHitboxesBehind == MU_DISPLAY_OPTION_HITBOXES_BEHIND && gx::kArcadeGrid == MU_DISPLAY_OPTION_GRID,
              "Arcade look bits");
// "Show Training mode's HUD" off. MELEE_TEST_TRAINING_HUD=0|1 (tests) overrides the setting.
bool training_hud_hidden() {
  static const int env = [] { const char* v = host::test_env::getenv("MELEE_TEST_TRAINING_HUD"); return v && *v ? (*v == '0' ? 0 : 1) : -1; }();
  return env >= 0 ? env == 0 : !gx::RenderOptions::live_training_hud();
}
// Display-only options (MU_DISPLAY_OPTION_*), read live by the native game's draw code. Never part
// of a replay or of the match the opponent plays: the viewer's own, as the HUD sizes are.
uint32_t h_display_options() {

  return (gx::low_poly_fighters_active() ? MU_DISPLAY_OPTION_LOW_POLY : 0u) |
         (training_hud_hidden() ? MU_DISPLAY_OPTION_NO_TRAINING_HUD : 0u) |
         (slippi::online::port_colour_bits() & MU_DISPLAY_OPTION_PORT_COLOURS) |   // online 1v1 port colours
         (gx::player_tags_always_active() ? MU_DISPLAY_OPTION_PLAYER_TAGS : 0u) |
         (gx::fod_reflections_active() ? MU_DISPLAY_OPTION_FOD_REFLECTIONS : 0u) |
         (gx::css_background_free_active() ? MU_DISPLAY_OPTION_CSS_BACKGROUND_FREE : 0u) |
         (gx::RenderOptions::live_freeze_fd() ? MU_DISPLAY_OPTION_FREEZE_FD : 0u) |
         (host::g_hide_ko_explosions.load(std::memory_order_relaxed) ? MU_DISPLAY_OPTION_NO_KO_EXPLOSIONS : 0u) |
         (host::g_hide_hud_emblems.load(std::memory_order_relaxed) ? MU_DISPLAY_OPTION_NO_HUD_EMBLEMS : 0u) |
         (host::g_community_codes.load(std::memory_order_relaxed) & MU_DISPLAY_OPTION_CODES_ALL) |
         // The offline community codes (menu timing, the pause): never in a network match.
         (slippi::online::session_mode() >= 0 || source_p2p::requested()
              ? 0u : host::g_community_codes.load(std::memory_order_relaxed) & MU_DISPLAY_OPTION_OFFLINE_CODES) |
         // The Arcade look's hitboxes: never in a network match (the game also checks). Its grid is a
         // picture like the rest of the background, so it stays.
         (slippi::online::session_mode() >= 0 || source_p2p::requested() ? 0u : gx::arcade_display() & gx::kArcadeGameBits) |
         // Its 2D camera, offline: the game runs the 2D-style build's camera code; online the renderer's
         // flat projection stands in (gx_shader.cpp build_projection).
         (slippi::online::session_mode() >= 0 || source_p2p::requested() || !(gx::arcade_display() & gx::kArcadeCamera2d)
              ? 0u : MU_DISPLAY_OPTION_ARCADE_CAMERA) |
         (gx::arcade_display() & gx::kArcadeGrid);
}
uint32_t h_shield_color(int32_t slot) { return gx::shield_color(slot); }
float h_screen_shake_scale() { return gecko::option_screen_shake_percent / 100.0f; }
uint32_t h_grid_color() { return gx::look_live().arcade_grid_rgb.load(std::memory_order_relaxed) & 0xFFFFFFu; }
void h_hud_combo(int32_t slot, int32_t link_slot, float percent, int32_t stunned) {
  gx::set_native_hud_combo(slot, link_slot, percent, stunned != 0);
}
void h_hud_extras(int32_t slot, int32_t nana_damage, int32_t team) { gx::set_native_hud_extras(slot, nana_damage, team); }
void h_hud_player(int32_t slot, int32_t present, int32_t damage, int32_t stocks, float tag_x, float tag_y,
                  int32_t tag_visible) {
  gx::set_native_hud_player(slot, present != 0, damage, stocks, tag_x, tag_y, tag_visible != 0);
}

// "20XX CPUs" (Game tab) rides in word 2 as MU_GAME_OPTION2_TE_20XX_CPUS but is a plain option, not a
// 20XX TE feature: it goes to the game with or without a TE save, and the game gates it itself
// (mu_option2_offline: offline only, never in a TM-CE event).
uint32_t cpu_20xx_bit() { return gx::RenderOptions::live_cpu_20xx() ? MU_GAME_OPTION2_TE_20XX_CPUS : 0u; }
// The 20XX TE features the 20XX Hack Pack also has (its debug menu, removed, switched them without a
// TE save). With the pack and no TE save only these bits go to the game, never TE itself
// (MU_GAME_OPTION_TE): the game accepts them with the pack loaded (mu_te.c), and TE's menus and
// rules stay off.
constexpr uint32_t kHpTeOptions = MU_GAME_OPTION_TE_NO_STAR_KO | MU_GAME_OPTION_TE_TAUNT_CANCEL |
                                  MU_GAME_OPTION_TE_FIXED_CAMERA;
constexpr uint32_t kHpTeOptions2 = MU_GAME_OPTION2_TE_NO_SCREEN_RUMBLE | MU_GAME_OPTION2_TE_LCANCEL_FLASH |
                                   0x00780000u |   // the flash choices (mu_lcancel_flash.h)
                                   MU_GAME_OPTION2_TE_BUBBLES | MU_GAME_OPTION2_TE_INPUT_DISPLAY |
                                   MU_GAME_OPTION2_TE_COLOR_OVERLAYS;
bool hp_te_words() { return !mods::status().te_owned && hackpack::loaded(); }
uint32_t te_words_mask1() { return mods::status().te_owned ? 0x00007FF0u : hp_te_words() ? kHpTeOptions : 0u; }
// What settings_file.cpp apply_practice_tools sets in the options word for the Training tools.
constexpr uint32_t kPracticeLabBits = MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_BUBBLES | MU_GAME_OPTION_LAB_BUBBLES_ONLY |
                                      MU_GAME_OPTION_LAB_OVERLAY | MU_GAME_OPTION_LAB_LOOP;
uint32_t te_words_mask2() { return mods::status().te_owned ? 0xFFFFFFFFu : hp_te_words() ? kHpTeOptions2 : 0u; }
uint32_t h_game_options2() {

   // Ranked-style play, no offline CPU/TE modifiers
  if (host::gameplay::vanilla_only()) return 0;
  if (g_replaying) return g_replay_feature_options2;
  return (gx::RenderOptions::live_te_options2() & te_words_mask2() & ~(MU_GAME_OPTION2_STADIUM_FROZEN | MU_GAME_OPTION2_ARCADE_PACK)) |
         cpu_20xx_bit() |
         (gx::RenderOptions::live_stadium_frozen() ? MU_GAME_OPTION2_STADIUM_FROZEN : 0u) |   // a plain option, like 20XX CPUs
         (mods::status().arcade ? MU_GAME_OPTION2_ARCADE_PACK : 0u);   // the recognised 2D-style build's codes
}
// The 20XX Hack Pack training options for CPUs (Game tab), word 3: plain options, the game gates
// them itself (mu_options3_offline: offline only, never in a TM-CE event).
// The upper bits (MU_GAME_OPTION3_HP_STAGE_*) are set only by a replay that recorded the 20XX Hack
// Pack's stage state (older builds): playback passes the replay's word as it is.
// Faster sound effects (MU_AUDIO_OPTION_EARLY_KEYON): MELEE_AUDIO_EARLY_KEYON=0|1 over the setting;
// never during an online match or a replay (see mu_host.h).
float h_picture_widen() {

  return gx::true_widescreen_active() ? 1.0f / gx::true_widescreen_factor() : 1.0f;
}
uint32_t h_audio_options() {
  // Mute character voices: everywhere (online and replays too); it changes no timing.
  static const int mute_env = [] { const char* v = host::test_env::getenv("MELEE_TEST_MUTE_VOICES"); return v && *v ? (*v == '1' ? 1 : 0) : -1; }();
  const bool mute = mute_env >= 0 ? mute_env == 1 : gx::RenderOptions::live_mute_voices();
  static int mute_logged = -1;
  if ((int)mute != mute_logged) { host::log("audio: character voices %s", mute ? "muted" : "on"); mute_logged = mute; }
  const uint32_t voices = mute ? MU_AUDIO_OPTION_MUTE_VOICES : 0u;

  if (g_replaying || slippi::online::session_mode() >= 0 || source_p2p::requested()) return voices;
  static const int env = [] { const char* v = std::getenv("MELEE_AUDIO_EARLY_KEYON"); return v && *v ? (*v == '1' ? 1 : 0) : -1; }();
  const bool on = env >= 0 ? env == 1 : gx::RenderOptions::live_audio_early_keyon();
  static int logged = -1;
  if ((int)on != logged) { host::log("audio: faster sound effects %s", on ? "on" : "off"); logged = on; }
  return (on ? MU_AUDIO_OPTION_EARLY_KEYON : 0u) | voices;
}

uint32_t h_game_options3() {
  // A console session unlocks every fighter and stage for both players, as a network
  // session does; a locked word would hand the roster to whichever card is loaded.


  if (g_replaying) return g_replay_feature_options3;
  return (host::gameplay::vanilla_only() ? 0u :
          (gx::RenderOptions::live_cpu_training() & MU_GAME_OPTION3_CPU_ALL)) |
         (gx::RenderOptions::live_unlock_all() ? 0u : MU_GAME_OPTION3_LOCKED_CONTENT) |
         (gx::RenderOptions::live_results_screen() ? 0u : MU_GAME_OPTION3_SKIP_RESULTS);
}
uint32_t h_game_options() {
   // canonical normal menus and language
  return (gecko::option_no_screen_shake ? MU_GAME_OPTION_NO_SCREEN_SHAKE : 0u) |
         (gecko::option_pal_stock_icons ? MU_GAME_OPTION_PAL_STOCK_ICONS : 0u) |
         (gecko::option_widescreen ? MU_GAME_OPTION_WIDESCREEN : 0u) |   // the viewer's own, also in replays
         (gx::true_widescreen_active() ? MU_GAME_OPTION_TRUE_WIDESCREEN : 0u) |   // likewise: display only
         (((uint32_t)host::g_game_language.load(std::memory_order_relaxed) << MU_GAME_OPTION_LANGUAGE_SHIFT) &
          MU_GAME_OPTION_LANGUAGE_MASK) |   // the player's language choice: text, names and voices only
         (host::options.vanilla_game ? MU_GAME_OPTION_VANILLA : 0u) |
         (g_slippi_menus ? MU_GAME_OPT_SLIPPI_MENUS : 0u) |
         (!host::gameplay::vanilla_only() && mods::status().tmce && !g_replaying ? MU_GAME_OPTION_TMCE : 0u) |
         ((host::gameplay::vanilla_only()) ? 0u : g_replaying ? g_replay_feature_options
                   : gx::RenderOptions::live_te_options() & te_words_mask1()) |
         // The Practice panel's Training tools (D-pad save states and recording, hitboxes, the loop)
         // are the native lab's bits (shim/mu_lab.c), set from the practice settings by
         // apply_practice_tools. The mask above passes only 20XX TE's own bits, so since the panel
         // came in these switches never reached the game. The lab itself acts only in an offline
         // Training match outside a replay; never sent in a network session or during playback.
         (host::gameplay::vanilla_only() || g_replaying || slippi::online::session_mode() >= 0 || source_p2p::requested()
              ? 0u : gx::RenderOptions::live_te_options() & kPracticeLabBits);
  // (Only 20XX TE's own bits, and only with its save, or the Hack Pack's share of them with the pack.
  // The old Training Lab's own rows (dummy DI, reactions, tech) are not offered any more: Training
  // Mode CE and the practice CPU replace them.)
}
// Match conveniences implemented by the host follow the same gates as mu_te2 in the game.
bool te_host_feature(uint32_t feature) {
  const uint32_t options = h_game_options();
  // Without TE itself, only the Hack Pack's share (its menu), with the pack or in a replay of it.
  const bool hp = (feature & ~kHpTeOptions2) == 0 && (hp_te_words() || g_replaying);
  if ((!(options & MU_GAME_OPTION_TE) && !hp) || (options & MU_GAME_OPTION_VANILLA) ||
      !(h_game_options2() & feature) || slippi::online::session_mode() >= 0) return false;
  if ((options & MU_GAME_OPTION_TE_TOURNAMENT) &&
      !(feature & MU_GAME_OPTION2_TE_TOURNAMENT_SAFE)) return false;
  uint32_t major = 0, minor = 0, match_frame = 0;
  host::current_scene(&major, &minor, &match_frame);
  return !(mods::status().tmce && major == 0x2B);
}

// MU_CMD_ZELDA_AS_SHEIK, from h_slippi_command (source_host.cpp).
bool zelda_as_sheik_command(std::vector<uint8_t>& reply) {
  static const int test = [] { const char* v = host::test_env::getenv("MELEE_TEST_ZELDA_SHEIK"); return v && *v ? (*v == '1' ? 1 : 0) : -1; }();
  const bool want = test >= 0 ? test == 1 : gx::RenderOptions::live_zelda_as_sheik();
  const bool on = want && !g_replaying && slippi::online::session_mode() < 0 && !source_p2p::requested() &&
                  !host::gameplay::vanilla_only();
  if (on) host::log("game: Zelda starts as Sheik (setting)");
  reply.push_back(on ? 1 : 0);
  return true;
}

// MU_CMD_TE_MENU_MUSIC, from h_slippi_command (source_host.cpp).
bool te_menu_music_command(const uint8_t* p, std::vector<uint8_t>& reply) {
  // 20XX TE menu song from Sound Test (0x80 asks): TE's value, 0 none, -1 song 0.
  if (p[0] != 0x80) {
    gx::RenderOptions::live_te_menu_music().store((int8_t)p[0]);
    host::log("20xx: menu music %d", (int8_t)p[0]);
  }
  else if (gx::RenderOptions::live_te_menu_music().load() != 0) {
    host::log("20xx: menu music %d (chosen in Sound Test)", gx::RenderOptions::live_te_menu_music().load());
  }
  reply.push_back((uint8_t)(int8_t)gx::RenderOptions::live_te_menu_music().load());
  return true;
}

// MU_CMD_TE_SETTINGS, from h_slippi_command (source_host.cpp).
bool te_settings_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  // 20XX TE's in-game settings menu (shim/mu_te_debugmenu.c): both feature words, big-endian,
  // the persisted settings bits. Empty asks for the current ones. With "Lock settings" on,
  // only switching the lock off goes through.
  auto& w1 = gx::RenderOptions::live_te_options();
  auto& w2 = gx::RenderOptions::live_te_options2();
  if (n == 8 && (h_game_options() & MU_GAME_OPTION_TE) && slippi::online::session_mode() < 0 && !g_replaying) {
    const uint32_t next1 = read_be32(p), next2 = read_be32(p + 4);
    if (w2 & MU_GAME_OPTION2_TE_LOCK_SETTINGS) {
      if (!(next2 & MU_GAME_OPTION2_TE_LOCK_SETTINGS)) w2 &= ~MU_GAME_OPTION2_TE_LOCK_SETTINGS;
    } else {
      w1 = (w1 & ~0x7FE0u) | (next1 & 0x7FE0u);   // TE itself (0x10) stays on
      w2 = next2 & ~MU_GAME_OPTION2_TE_LCANCEL_WHEELS;   // the port's Auto L-cancel does that
      if (w2 & MU_GAME_OPTION2_TE_LCANCEL_FLASH) lcancel::set_indicator(false);   // one flash at a time
    }
    gx::RenderOptions::live_te_game_changes().fetch_add(1);
    host::log("20xx: settings from the game menu %08X %08X", w1, w2);
  }
  append_be32(reply, w1);
  append_be32(reply, w2);
  return true;
}

}  // namespace host_internal
}  // namespace source_port
