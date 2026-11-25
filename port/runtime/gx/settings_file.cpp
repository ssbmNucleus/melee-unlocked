// The settings file (port-settings.ini): read, written, and what reading it applies. No UI: the F1
// panel (pc_settings*.cpp and the settings_page_*.cpp files) is a view over these values and saves
// through settings_store::write. Controller lines are read and written by settings_controls.cpp.
// Interface: settings_store.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings_store.h"
#include "settings_controls.h"
#include "render_options.h"
#include "gx_core.h"
#include "texture_pack.h"
#include "gx_retro.h"         // kSurfaceCount (the look_* settings)
#include "test_env.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "jukebox.h"
#include "slippi_online.h"
#endif
#include "practice_cpu.h"
#include "host.h"
#include "early_input.h"
#include "input_bindings.h"
#include "lcancel.h"
#include "../abi/mu_lcancel_flash.h"
#include "user_gecko.h"
#include "lag_test_codes.h"
#include "gecko_data.h"
#include "phob_menu.h"
#include "opponent_skins.h"   // the "opponentskins" setting
#include "lobby_directory.h"  // the "doubleslobbies" setting
#include "release_scope.h"    // the Mods categories' release switches ("modsparticles" ...)
#include "mod_scan.h"
#include "net_overlay.h"
#include "practice_menu.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>


namespace gecko { int option_screen_shake_percent = 100; }

namespace gx {

namespace {
settings_store::PanelValues g_panel;
// Why the last settings write failed, shown in the panel: a folder Windows will not let the game write
// to (Program Files, Controlled folder access, a read-only or synced folder) used to lose every change
// silently, and the player only saw the old values come back at the next start.
std::string g_settings_save_error;
}  // namespace

settings_store::PanelValues& settings_store::panel_values() { return g_panel; }
const std::string& settings_store::save_error() { return g_settings_save_error; }

// 20XX TE has its own switch for two things the port already does: its "Disable screen rumble" is
// our Disable screen shake, and its "Input display" is our Controller overlay. While TE is on (its
// save loaded and "Use 20XX TE features" on) each pair is one setting: switching either one switches
// both, so neither reads off while the other has the effect on. Tournament Mode greys TE's two
// switches but keeps them in step, so leaving it finds the pairs still agreeing.
// The bits (kTeNoScreenRumble, kTeInputDisplay, kTeLockSettings) are in settings_store.h.
bool te_pairs_linked(const RenderOptions& options) {
  return options.native_source && source_port::mods::status().te_owned && (options.te_options & 0x10u) != 0;
}
// One of ours changed: TE's matching switch follows, unless the TE settings are locked.
void te_follow_ours(RenderOptions& options, uint32_t bit, bool on) {
  if (!te_pairs_linked(options) || (options.te_options2 & kTeLockSettings)) return;
  options.te_options2 = on ? (options.te_options2 | bit) : (options.te_options2 & ~bit);
  RenderOptions::live_te_options2() = options.te_options2;
}
// TE's switch changed, on the Mods page or in the game's own TE menu: ours follows.
void ours_follow_te(RenderOptions& options, uint32_t bit, bool on) {
  if (!te_pairs_linked(options)) return;
  if (bit == kTeNoScreenRumble) gecko::option_no_screen_shake = on;
  if (bit == kTeInputDisplay) options.input_overlay = on;
}

// The Training tools in the practice settings drive the lab's option bits (Source Port, offline
// Training only; the Static Recomp has no lab).
void apply_practice_tools(RenderOptions& options) {
  const MuPcConfig cfg = practice_cpu::config();
  uint32_t te = options.te_options & 0x00007FFFu;
  if (cfg.tools & MU_PC_TOOL_DPAD) te |= MU_GAME_OPTION_LAB;
  if (cfg.tools & (MU_PC_TOOL_HITBOXES | MU_PC_TOOL_HITBOXES_ONLY)) te |= MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_BUBBLES;
  if (cfg.tools & MU_PC_TOOL_HITBOXES_ONLY) te |= MU_GAME_OPTION_LAB_BUBBLES_ONLY;
  if (cfg.tools & MU_PC_TOOL_OVERLAY) te |= MU_GAME_OPTION_LAB | MU_GAME_OPTION_LAB_OVERLAY;
  if (cfg.tools & MU_PC_TOOL_LOOP) te |= MU_GAME_OPTION_LAB_LOOP;
  // Kept in the options word too, so every later write of it by the panel carries them (the file's
  // copy is masked off at load and rebuilt from the practice settings).
  options.te_options = te;
  if (RenderOptions::kModFeaturesAvailable) RenderOptions::live_te_options() = te;
  // The Arcade look's projectile hitboxes and combo counter, for as long as these rows are on.
  set_arcade_display(ArcadeSource::Practice, ((cfg.tools & MU_PC_TOOL_PROJECTILE_HITBOXES) ? kArcadeProjectileHitboxes : 0u) |
                                            ((cfg.tools & MU_PC_TOOL_COMBO_COUNTER) ? kArcadeComboCounter : 0u));
}

static std::string texpack_disabled_lines() {
  std::string out;
  for (const auto& name : texpack::disabled_packs()) out += std::string("\n") + "texpackoff " + name;
  return out;
}



void load_pc_settings(RenderOptions& options, int& volume) {
  controls_begin_load();   // the built-in buttons captured before the saved ones replace them
  std::ifstream file(options.settings_path);
  options.live_ping = false;  // legacy showping uses the once-a-second readout
  options.cpu_20xx = false;   // a file without the key: off
  options.cpu_ai = 0;         // likewise: every port Default
  options.freeze_fd = false;
  options.hide_ko_explosions = false;
  options.hide_hud_emblems = false;
  options.hide_percents = false;
  options.community_codes = 0;
  options.unlock_all = true;   // a file without the key: on
  options.results_screen = true;
  options.training_hud = true;
  options.offline_delay = false;
  options.offline_delay_frames = 2;
  bool offline_delay_frames_seen = false;
  // The Lossless adapter lag test arms one launch only ("lagtestnext", cleared by that launch:
  // consume_lag_test_launch). 0.9.47 to 0.9.62 kept it on ("losslesslagtest") until it was switched
  // off, so every later launch booted to an FFA character select that rumbled and turned the picture
  // black and white on X and A. That old key is read as off.
  options.lossless_lag_test = false;
  options.early_inputs = true;   // a file without the key: on
  host::g_rumble_enabled.store(false, std::memory_order_relaxed);   // a file without rumble2: off
#ifndef MELEE_NO_SLIPPI   // the spectator server is part of the Slippi layer
  options.slippi_spectator = true;   // a file without the key: on, as in Slippi Dolphin
  options.slippi_spectator_port = 51441;
#endif
  options.cpu_tech = options.cpu_getup = options.cpu_di = options.cpu_sdi = 0;   // files without the keys: off
  options.cpu_no_taunt = options.cpu_lcancel = options.cpu_no_rapid_jab = options.cpu_no_transform = false;
  options.mod_choices.clear();
  host::release_scope::reset_settings();   // a file without the lines: the release's own table
  g_panel.advanced_open[0] = g_panel.advanced_open[1] = false;   // a file without the keys (0.8.1 and older): closed
  // One setting per line: the key, then everything after it on that line. Reading the value as a
  // single token lost the multi-number "custompreset" line (0.5.5 and later) and shifted every
  // setting after it by one token, so the controller bindings, port choices and profiles saved
  // below it were never read back for anyone who had used the Custom video preset.
  std::string key, value;
  std::vector<std::string> gecko_on;   // the user's Gecko codes saved as on
  std::vector<std::string> texture_packs_off;
  bool gecko_chosen = false;           // the panel has saved a choice (even "none")
  int saved_lcancel_flash = -1, saved_lcancel_success = -1;
  bool vsync_seen = false, vsync_choice_seen = false;
  bool lowspec_look = false;   // "lowspec_look 1": written by builds that migrate Low spec to the image only
  bool saved_true_widescreen = false, truewsfit_seen = false;
  int saved_truewsaspect = -1;
  // Video > HDR keys, apart from the main chain below (one else-if chain that long exceeds MSVC's
  // nesting limit). The removed path tracing, Ray Reconstruction and RTX Pack renderers' keys
  // (pathtracing, rayreconstruction, rtx*) and the removed Lab view's (labview, labskipscene,
  // look_lab_*, lab_colour_*) are taken here and skipped, so an older file still loads.
  auto load_hdr_key = [&options](const std::string& key, const std::string& value) -> bool {
    try {
      if (key == "pathtracing" || key == "rayreconstruction" || key.rfind("rtx", 0) == 0 ||
          key == "labview" || key == "labskipscene" || key.rfind("look_lab_", 0) == 0 || key.rfind("lab_colour_", 0) == 0) {}
      else if (key == "hdroutput") options.hdr_output = value == "1";
      else if (key == "hdrpaperwhite") { int n = std::stoi(value); if (n >= 80 && n <= 500) options.hdr_paper_white = n; }
      else if (key == "hdrpeak") { int n = std::stoi(value); if (n >= 0 && n <= 10000) options.hdr_peak = n; }
      else return false;
    } catch (...) {}
    return true;
  };
  while (file >> key) {
    std::getline(file, value);
    const size_t first = value.find_first_not_of(" \t");
    value = first == std::string::npos ? std::string() : value.substr(first);
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (key == "truewsfit") { truewsfit_seen = true; continue; }
    // The Mods categories' release switches, "modsparticles 1" (also "modsparticles=1", a key with no value).
    if (host::release_scope::load_setting(key, value)) continue;
    if (value.empty()) continue;
    if (source_port::mods::parse_choice(key, value, &options.mod_choices)) continue;
    if (load_hdr_key(key, value)) continue;
    try {
      bool matched = true;
      if (key == "fps") { double rate = std::stod(value); if (rate == -1 || rate == 0 || rate >= 30) options.fps_cap = std::min(rate, 2000.0); }
      else if (key == "scale") { int scale = std::stoi(value); if (scale >= 0 && scale <= 8) options.efb_scale = scale; }
      else if (key == "fullscreen") options.fullscreen = value == "1";
      else if (key == "exclusivefullscreen") options.exclusive_fullscreen = value == "1";
      else if (key == "vsync") { options.vsync = value == "1"; vsync_seen = true; }
      else if (key == "vsyncchoice") vsync_choice_seen = value == "1";
      else if (key == "vrrfound") options.vrr_found = value == "1";
      else if (key == "widescreen") options.widescreen = value == "1";
      else if (key == "fodreflections") options.fod_reflections = value != "0";
      else if (key == "cssbackgroundfree") options.css_background_free = value == "1";
      else if (key == "truewidescreen") options.true_widescreen = saved_true_widescreen = value == "1";
      else if (key == "truewsaspect") { int n = std::stoi(value); if (n >= 0 && n <= 3) { options.true_aspect = (TrueAspect)n; saved_truewsaspect = n; } }
      else if (key == "truewscustomw") { int n = std::stoi(value); if (n >= 1 && n <= 100) options.true_custom_w = n; }
      else if (key == "truewscustomh") { int n = std::stoi(value); if (n >= 1 && n <= 100) options.true_custom_h = n; }
      else if (key == "customtextures") options.custom_textures = value == "1";
      else if (key == "dumptextures") options.dump_textures = value == "1";
      else if (key == "prefetchtextures") options.prefetch_textures = value != "0";
      else if (key == "videobackgrounds") options.video_backgrounds = value != "0";
      // One line per pack the player switched off; anything not listed is on, so a pack installed
      // later starts enabled rather than silently doing nothing.
      else if (key == "texpackoff") {
        // A pack is a folder, and a folder name can contain spaces ("HD Textures"); the value is the
        // whole rest of the line.
        texture_packs_off.push_back(value);
      }
      else if (key == "aspect") { int a = std::stoi(value); if (a >= 0 && a <= 4) options.aspect = (AspectMode)a; }
      // "window <w>x<h>", or "window follow" for the old behaviour of using whatever size the
      // window has been dragged to.
      else if (key == "window") {
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
          options.window_w = w; options.window_h = h; options.window_pinned = true;
        } else options.window_pinned = false;
      }
      else if (key == "sharpness") options.sharpness = std::clamp(std::stof(value), 0.0f, 1.0f);
      else if (key == "crt_filter") options.crt_filter = std::clamp(std::stoi(value), 0, 2);
      else if (key == "crt_curve") options.crt_curve = std::clamp(std::stof(value), 0.0f, 3.0f);
      else if (key == "borderart") options.border_art = value;
      else if (key == "borderedge") options.border_edge = value == "0" ? 0 : 1;
      else if (key == "shaderpreset") options.shader_preset = value;
      else if (key == "ssao") options.screen_space_ao = std::clamp(std::stof(value), 0.0f, 1.0f);
      else if (key == "brightness") options.brightness = std::clamp(std::stof(value), 0.5f, 1.5f);
      else if (key == "contrast") options.contrast = std::clamp(std::stof(value), 0.5f, 1.5f);
      else if (key == "vibrance") options.vibrance = std::clamp(std::stof(value), 0.0f, 2.0f);
      else if (key == "anisotropy") { int a = std::stoi(value); if (a == 1 || a == 2 || a == 4 || a == 8 || a == 16) options.anisotropy = a; }
      else if (key == "ssaa") { int a = std::stoi(value); if (a >= 1 && a <= 4) options.ssaa = a; }
      else if (key == "msaa") { int a = std::stoi(value); if (a == 1 || a == 2 || a == 4 || a == 8) options.msaa = a; }
      else if (key == "subframe") options.subframe = value == "0" ? SubFrameMode::Off : value == "2" ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored;
      else if (key == "onlineframeprotection") {}   // removed setting (online drop to 60): older files still carry it
      else if (key == "music") slippi::jukebox::set_user_volume(std::stoi(value));
      else if (key == "musicpacks") slippi::jukebox::set_music_packs_enabled(value == "1");
      else if (key == "audio_mode") options.audio_mode = std::clamp(std::stoi(value), 0, 3);
      else if (key == "audio_asio_driver") options.audio_asio_driver = value;
      else if (key == "audio_asio_buffer") { const int b = std::stoi(value); options.audio_asio_buffer = b == 0 ? 0 : std::clamp(b, 32, 2048); }
      else if (key == "audio_device") options.audio_device = value;
      else if (key == "audio_buffer_ms") options.audio_buffer_ms = std::clamp(std::stoi(value), 5, 120);
      else if (key == "audio_early_keyon") gx::RenderOptions::live_audio_early_keyon() = kShowFasterSoundEffects && value == "1";
      else if (key == "mute_voices") gx::RenderOptions::live_mute_voices() = value == "1";
      else if (key == "quickchat") slippi::online::config().chat = std::clamp(std::atoi(value.c_str()), 0, 2);
      else if (key == "directpause" || key == "onlineitems" || key == "onlinesquad" || key == "portcolour1" || key == "portcolour2") {
        if (key == "directpause") slippi::online::config().direct_pause = value != "0";
        else if (key == "onlineitems") slippi::online::config().items = value == "1";   // Items in Direct and Teams, off by default
        else if (key == "onlinesquad") slippi::online::config().squad = value == "1";   // Squad Strike in Direct, off by default
        else slippi::online::config().port_colour[key.back() - '1'] = std::clamp(std::atoi(value.c_str()), -1, 3);
      }
      else if (key == "blockcode1" || key == "blockcode2" || key == "blockcode3")
        slippi::online::config().blocked_codes[key.back() - '1'] = value.substr(0, 16);
      else if (key == "practicecpu") {
        MuPcConfig pc;
        if (practice_cpu::from_text(value, pc)) practice_cpu::set_config(pc);
      }
      else if (key.size() == 16 && key.rfind("practicecpuslot", 0) == 0 && key[15] >= '1' && key[15] <= '3') {
        MuPcConfig pc;
        if (practice_cpu::from_text(value, pc)) practice_cpu::set_slot(key[15] - '1', pc);
      }
      else if (key == "onlinedelay") { int d = std::atoi(value.c_str()); if (d >= 1 && d <= 9) slippi::online::config().delay = d; }
      else if (key == "onlinedelayauto") slippi::online::config().auto_delay = value == "1";
      else if (key == "offline_delay") options.offline_delay = value == "1";
      else if (key == "offline_delay_frames") {
        options.offline_delay_frames = std::clamp(std::stoi(value), 1, 9);
        offline_delay_frames_seen = true;
      }
      // early_inputs and early_inputs2, older keys (the second saved off for everyone), are not read.
      else if (key == "early_inputs3" || key == "early_inputs2" || key == "early_inputs") { if (key == "early_inputs3") options.early_inputs = value != "0"; }
      else if (key == "performance") options.performance_overlay = value == "1";
      else if (key == "showfps") options.show_fps = value == "1";
      else if (key == "showvram") options.show_vram = value == "1";
      else if (key == "showping" || key == "liveping")
        (key == "showping" ? options.show_ping : options.live_ping) = value == "1";
      else if (key == "netoverlay") net_overlay::set_enabled(value == "1");
      // Diagnostic, off unless someone is hunting a one-frame glitch: see RenderOptions::flicker_scan.
      // Settings-file only rather than a control in the panel, because it costs a readback on every
      // presented frame and nobody should switch it on by browsing.
      else if (key == "flickerscan") options.flicker_scan = value == "1";
      else if (key == "advancedvideo") g_panel.advanced_open[0] = value == "1";
      else if (key == "advancedoverlays") g_panel.advanced_open[1] = value == "1";
      // The in-game overlay and its looks were removed (2026-11-24): autoopenoverlay, settingsreminder,
      // skin_online_notice, legacymenu, legacymenustyle, menusounds, menucustom, menucolor,
      // overlaystyle, settingstransparency and overlaypalette0-6 are unknown now and skipped like any
      // other unknown key, so an older file still loads.
      else if (key == "stockhudscale") options.stock_hud_scale = std::clamp(std::atoi(value.c_str()), 75, 175);
      else if (key == "damagehudscale") options.damage_hud_scale = std::clamp(std::atoi(value.c_str()), 75, 175);
      else if (key == "playernicknames") options.show_player_nicknames = value == "1";
      else if (key == "matchmakinghint") options.matchmaking_hint = value != "0";
      else if (key == "practiceopens") options.practice_opens = std::clamp(std::atoi(value.c_str()), 0, 99);
      else if (key == "practicestyle") { const int n = std::atoi(value.c_str()); if (n >= 0 && n < practice_menu::kStyleCount) options.practice_style = n; }
      else if (key == "effects") { int n = std::atoi(value.c_str()); if (n >= 0 && n <= 3) options.effects_level = n; }
      else if (key == "playertags") options.player_tags_always = value == "1";
      else if (key == "arcade_hitboxes") options.arcade_hitboxes = value == "1";
      else if (key == "arcade_projectile_hitboxes") options.arcade_projectile_hitboxes = value == "1";
      else if (key == "arcade_hitboxes_behind") options.arcade_hitboxes_behind = value == "1";
      else if (key == "arcade_camera_2d") options.arcade_camera_2d = value == "1";
      else if (key == "arcade_combo_counter") options.arcade_combo_counter = value == "1";
      else if (key.size() == 14 && key.compare(0, 13, "shield_colorp") == 0 && key[13] >= '1' && key[13] <= '4')
        options.shield_color[key[13] - '1'] = (value == "-" || value.empty() ? 0u : 0x1000000u | ((uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFFFu));
      else if (key == "shield_color_online") options.shield_color_online = (value == "-" || value.empty() ? 0u : 0x1000000u | ((uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFFFu));
      else if (key == "arcade_backdrop") options.arcade_backdrop = std::clamp(std::atoi(value.c_str()), 0, 2);
      else if (key == "arcade_grid_rgb") options.arcade_grid_rgb = (uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFFFu;
      else if (key == "arcade_background_rgb") options.arcade_background_rgb = (uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFFFu;
      else if (key == "lowpoly") { int n = std::atoi(value.c_str()); if (n >= 0 && n <= 1) options.low_poly_fighters = n; }
      else if (key == "retro") options.retro = value == "1";
      else if (key == "look_fighter") { int n = std::atoi(value.c_str()); if (n >= 0 && n < kSurfaceCount) options.look_fighter = n; }
      else if (key == "look_stage") { int n = std::atoi(value.c_str()); if (n >= 0 && n < kSurfaceCount) options.look_stage = n; }
      else if (key == "look_menus") { int n = std::atoi(value.c_str()); if (n >= 0 && n < kSurfaceCount) options.look_menus = n; }
      else if (key == "look_unfiltered") options.look_unfiltered = value == "1";
      else if (key == "look_own") options.look_own = value == "1";
      else if (key == "look_full_models") options.look_full_models = value == "1";
      else if (key == "look_cap_lifted") options.look_cap_lifted = value == "1";
      else if (key == "look_saved_fps") options.look_saved_fps_cap = std::atof(value.c_str());
      else if (key == "look_saved_subframe") options.look_saved_subframe = std::clamp(std::atoi(value.c_str()), 0, 4);
      else if (key == "look_colours") { int n = std::atoi(value.c_str()); if (n >= 0 && n <= 1) options.look_colours = n; }
      else if (key == "look_background") { int n = std::atoi(value.c_str()); if (n >= 0 && n <= 2) options.look_background = n; }
      else if (key == "look_background_rgb") options.look_background_rgb = (uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFFFu;
      else if (key == "stadium_frozen") options.stadium_frozen = value == "1";
      else if (key == "lowspec_look") lowspec_look = value == "1";
      else if (key == "lightweight") { int n = std::atoi(value.c_str()); if (n >= 0 && n < kLightweightLevels) options.lightweight = n; }
      else if (key == "inputoverlay") options.input_overlay = value == "1";
      else if (key == "clean_replays") options.clean_replays = value == "1";
      // Settings saved before the overlay could show several ports name a single port number.
      else if (key == "inputoverlayport") { int n = std::atoi(value.c_str()); if (n >= 0 && n < 4) options.input_overlay_ports = 1 << n; }
      else if (key == "inputoverlayports") { int n = std::atoi(value.c_str()); if (n >= 0 && n < 16) options.input_overlay_ports = n; }
      else if (key == "inputoverlayhideborder") options.input_overlay_hide_border = value == "1";
      else if (key == "inputoverlayvalues") options.input_overlay_values = value == "1";
      else matched = false;
      // The chain goes on in a second one: MSVC stops at about 128 nested else-ifs (C1061).
      if (!matched) {
      if (key == "inputoverlaystick") options.input_overlay_stick = std::clamp(std::atoi(value.c_str()), 1, 10);
      else if (key.rfind("inputoverlayrect", 0) == 0 && key.size() == 17 && key[16] >= '0' && key[16] <= '3') {
        // x y width height, then the screen it was placed on (0.9.994); older lines have no screen.
        float r[6] = {};
        const int n = std::sscanf(value.c_str(), "%f %f %f %f %f %f", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]);
        if (n == 6 && !(r[4] >= 100 && r[5] >= 100 && r[4] <= 10000 && r[5] <= 10000)) r[4] = r[5] = 0;
        if (n >= 4 && r[2] >= 40 && r[3] >= 20 &&
            r[2] <= 4000 && r[3] <= 4000 && std::isfinite(r[0]) && std::isfinite(r[1]))
          std::copy(r, r + 6, options.input_overlay_rect[key[16] - '0']);
      }
      else if (key == "lcancelindicator") lcancel::set_indicator(value == "1");
      else if (key == "lcancel_flash_mode") saved_lcancel_flash = std::clamp(std::stoi(value), 0, 6);
      else if (key == "lcancel_success_flash") saved_lcancel_success = std::clamp(std::stoi(value), 0, 2);
      else if (key == "autolcancel") lcancel::set_automatic(value == "1");
      else if (key == "palstockicons") gecko::option_pal_stock_icons = value == "1";
      else if (key == "noscreenshake") gecko::option_no_screen_shake = value == "1";
      else if (key == "screenshake") gecko::option_screen_shake_percent = std::clamp(std::stoi(value), 0, 200);
      else if (key == "geckocode") gecko_on.push_back(value);
      else if (key == "geckochosen") gecko_chosen = value == "1";
      else if (key == "swpro_gc_picture") g_panel.swpro_gc_picture = value == "1";
      // rumble2: on only when the file says 1 (off by default from 11-23). The old key "rumble" was
      // saved as 1 for everyone who never touched it, so it cannot tell a choice from the default and
      // is not read: a player who wants rumble turns it on once more (Controls > Rumble).
      else if (key == "rumble2" || key == "rumble") { if (key == "rumble2") host::g_rumble_enabled.store(value == "1", std::memory_order_relaxed); }
      else if (key == "backgroundinput") host::g_background_input = value != "0";
      else if (key == "gamelanguage") host::g_game_language.store(value == "1" ? 1 : value == "2" ? 2 : 0);
      else if (key == "cpu_20xx") options.cpu_20xx = value == "1";
      else if (key == "cpu_ai") options.cpu_ai = (uint32_t)std::strtoul(value.c_str(), nullptr, 16) & 0xFFFFu;
      else if (key == "freeze_fd") options.freeze_fd = value == "1";
      else if (key == "hide_ko_explosions") options.hide_ko_explosions = value == "1";
      else if (key == "hide_hud_emblems" || key == "hide_percents")
        (key == "hide_percents" ? options.hide_percents : options.hide_hud_emblems) = value == "1";
      else if (key == "community_codes") options.community_codes = (uint32_t)std::strtoul(value.c_str(), nullptr, 16) & (MU_DISPLAY_OPTION_CODES_ALL | MU_DISPLAY_OPTION_OFFLINE_CODES);
      else if (key == "unlock_all") options.unlock_all = value != "0";
      else if (key == "zelda_as_sheik") options.zelda_as_sheik = value == "1";
      else if (key == "results_screen") options.results_screen = value != "0";
      else if (key == "training_hud") options.training_hud = value != "0";
      else if (key == "opponentskins") host::skins::set_enabled(value != "0");   // opponents' skins online, on by default (0.1.0 on)
      else if (key == "doubleslobbies") host::lobby_directory::set_enabled(value != "0");   // Teams lobbies, on by default
#ifndef MELEE_NO_SLIPPI
      else if (key == "slippi_spectator") options.slippi_spectator = value != "0";
      else if (key == "online_return_training") options.online_return_training = value == "1";
      // Keys of removed features (online_training_after, online_back_to_training, warmup_*, keep_search,
      // search_banner, room_watch, spectate_view: the warm-up room, the kept search, arena rooms) are
      // unknown now and skipped like any other unknown key, so an older file still loads.
      else if (key == "slippi_spectator_port") options.slippi_spectator_port = std::clamp(std::atoi(value.c_str()), 1, 65535);
#endif
      else if (key == "cpu_tech") options.cpu_tech = std::clamp(std::atoi(value.c_str()), 0, 5);
      else if (key == "cpu_getup") options.cpu_getup = std::clamp(std::atoi(value.c_str()), 0, 5);
      else if (key == "cpu_di") options.cpu_di = std::clamp(std::atoi(value.c_str()), 0, 3);
      else if (key == "cpu_sdi") options.cpu_sdi = std::clamp(std::atoi(value.c_str()), 0, 6);
      else if (key == "cpu_notaunt") options.cpu_no_taunt = value == "1";
      else if (key == "cpu_lcancel") options.cpu_lcancel = value == "1";
      else if (key == "cpu_nojab") options.cpu_no_rapid_jab = value == "1";
      else if (key == "cpu_notransform") options.cpu_no_transform = value == "1";
      // Older files: the controller last edited and the "Follow player 1 controller" toggle. The
      // Controls page now always opens on player 1's controller, so both are read and left unused.
      else if (key == "editdevice" || key == "editdevice_follow") {}
      else if (key == "custompreset") {
        settings_store::CustomPreset c; c.set = true;
        if (std::sscanf(value.c_str(), "%d %d %d %d %lf %d", &c.efb, &c.ssaa, &c.aniso, &c.dlss, &c.fps, &c.sub) == 6) {
          if (c.dlss >= 6 && c.dlss <= 10) c.dlss = 0;   // as "dlss" below
          g_panel.custom_preset = c;
        }
      }
      // Bindings, port devices, stick options, levels, profiles (settings_controls.cpp). None of the
      // keys read below this line starts like one of these.
      else if (load_controls_key(key, value)) {}
      // 6 to 10 were Intel XeSS's modes (removed): an older file that chose one loads as Native.
      else if (key == "dlss") { int m = std::stoi(value); if (m >= 0 && m <= 15) options.dlss_mode = m >= 6 && m <= 10 ? 0 : m; }
      // Pre-multiplier saves wrote 0 or 1; both still mean what they always meant (off / 2x).
      else if (key == "framegen") options.frame_generation_mode = std::clamp(std::stoi(value), 0, 6);
      else if (key == "fgmax") options.fg_cached_max = std::clamp(std::atoi(value.c_str()), 0, 5);
      else if (key == "fgdynamic") options.fg_cached_dynamic = value == "1";
      else if (key == "reflex") options.reflex_mode = std::clamp(std::atoi(value.c_str()), 0, 2);
      else if (key == "reflexstats") options.reflex_stats = value == "1";
      else if (key == "reflexflash") options.reflex_flash = value == "1";
      else if (key == "latencytest") options.latency_test = value == "1";
      else if (key == "lagtestnext") options.lossless_lag_test = value == "1";
      // DLSS 5 (dlss5, dlss5intensity and the other dlss5* keys) was removed: its keys are unknown now
      // and skipped like any other unknown key, so an older file still loads.
      // Low spec: the switch, then what the player had before it was turned on, so turning it off
      // after a restart still restores their own settings rather than the defaults.
      else if (key == "lowspec") options.low_spec = value == "1";
      else if (key == "lowspec_prev_backend") options.low_spec_previous.api = value == "d3d11" ? RenderApi::D3D11 : value == "vulkan" ? RenderApi::Vulkan : RenderApi::D3D12;
      else if (key == "lowspec_prev_fps") { double rate = std::stod(value); if (rate == -1 || rate == 0 || rate >= 30) options.low_spec_previous.fps_cap = std::min(rate, 2000.0); }
      else if (key == "lowspec_prev_scale") { int n = std::stoi(value); if (n >= 0 && n <= 8) options.low_spec_previous.efb_scale = n; }
      else if (key == "lowspec_prev_ssaa") { int n = std::stoi(value); if (n >= 1 && n <= 4) options.low_spec_previous.ssaa = n; }
      else if (key == "lowspec_prev_anisotropy") { int n = std::stoi(value); if (n == 1 || n == 2 || n == 4 || n == 8 || n == 16) options.low_spec_previous.anisotropy = n; }
      else if (key == "lowspec_prev_effects") { int n = std::stoi(value); if (n >= 0 && n <= 3) options.low_spec_previous.effects_level = n; }
      else if (key == "lowspec_prev_dlss") { int n = std::stoi(value); if (n >= 0 && n <= 15) options.low_spec_previous.dlss_mode = n >= 6 && n <= 10 ? 0 : n; }   // as "dlss" (FSR is 11 to 15)
      else if (key == "lowspec_prev_subframe") options.low_spec_previous.subframe = value == "0" ? SubFrameMode::Off : value == "2" ? SubFrameMode::AuthoredInterpolate : SubFrameMode::Authored;
      // "discord" kept the old off-by-default choice: everyone starts on with 0.9.210, then their own
      // choice is kept as "discordon".
      else if (key == "discordon") options.discord_presence = value == "1";
      else if (key == "te_options") { options.te_options = (uint32_t) std::stoul(value, nullptr, 16); }
      else if (key == "mod_profile") { if (value.size() <= 1024) options.mod_profile = value; }
      else if (key == "mods_in_direct") options.mods_in_direct = value != "0";
      else if (key == "te_options2") options.te_options2 = (uint32_t)std::strtoul(value.c_str(), nullptr, 16);
      else if (key == "te_menu_music") options.te_menu_music = std::atoi(value.c_str());
      // An older build's own Discord application id (Melee Unlocked's): ignored, never written again
      // (the id is the build's, product_identity.h kDiscordAppId).
      else if (key == "discord_app_id") {}
      else if (key == "backend") options.api = value == "d3d11" ? RenderApi::D3D11 : value == "vulkan" ? RenderApi::Vulkan : RenderApi::D3D12;
      else if (key == "volume") { volume = std::clamp(std::stoi(value), 0, 100); g_panel.volume = volume; }
      }
    } catch (...) { /* Ignore a malformed preference, retaining the safe default. */ }
  }
  // A controller saved before L and R were split into the click and the travel keeps playing as it
  // did. After the whole file, since a binding and its level can come in either order.
  controls_end_load();
  // Every save before this wrote "vsync 1" for players who never touched it, so only "vsync 0" (the
  // default was on) or the new "vsyncchoice 1" is a choice.
  options.vsync_auto = !vsync_choice_seen && (!vsync_seen || options.vsync);
  if (options.vsync_auto) {
    options.vsync = false;
    host::log("present: vsync off from the start (automatic)");
  }
  // The old Ultra preset (4x supersampling under DLAA) becomes the new one (DLAA alone); see the
  // presets. Only that exact combination is changed.
  if (options.efb_scale == 0 && options.ssaa == 2 && options.anisotropy == 16 && options.dlss_mode == 1) options.ssaa = 1;
  // Display and Widescreen are one choice each on the Video tab. A file with both of a pair on (only
  // possible by hand) keeps the one the renderers already let win: exclusive over borderless, and
  // the Slippi code over True 16:9 (gx_d3d12 / gx_d3d11 apply True 16:9 only without the code).
  if (options.exclusive_fullscreen) options.fullscreen = false;
  if (options.widescreen) options.true_widescreen = false;
  // A saved True widescreen shape is the player's: never moved (0.9.211 behaviour).
  (void)saved_true_widescreen; (void)saved_truewsaspect; (void)truewsfit_seen;
  // Widescreen is True widescreen only (0.9.63). The Slippi code was a choice beside it that players
  // took for the widescreen they knew from Slippi Dolphin, and in fullscreen it left the picture
  // boxed in on wide screens; a saved Slippi code choice now fills the screen. --widescreen still
  // runs the code for test runs, after this.
  if (options.widescreen) {
    options.widescreen = false;
    options.true_widescreen = true;
    options.true_aspect = TrueAspect::Wide16_9;
  }
  migrate_lightweight(options, lowspec_look);   // files from before the "Lightweight" ladder
  set_lightweight_level(options.lightweight);
  std::sort(texture_packs_off.begin(), texture_packs_off.end());
  texture_packs_off.erase(std::unique(texture_packs_off.begin(), texture_packs_off.end()), texture_packs_off.end());
  texpack::set_disabled_packs(std::move(texture_packs_off));
  if (options.prewarm_only) return;   // no gameplay files or services in a graphics-only run
  // The player's Gecko codes live beside the settings file.
  std::filesystem::path codes = std::filesystem::path(options.settings_path).parent_path() / "GeckoCodes.ini";
  user_gecko::set_code_patches_allowed(!options.native_source);   // the Static Recomp can run a patched function from RAM
  if (!host::gameplay::vanilla_only() && (options.lossless_lag_test || std::getenv("MELEE_LAG_TEST"))) {
    // The Lossless adapter's lag test: the codes its app gives Dolphin, in place of the player's own
    // (the player's video settings stay, since they are what is measured). The Source Port does the
    // same natively and loads none.
    codes = std::filesystem::path(options.settings_path).parent_path() / "LagTestGeckoCodes.ini";
    if (!options.native_source) {
      if (FILE* f = _wfopen(codes.wstring().c_str(), L"wb")) { std::fputs(lag_test::kGeckoCodesIni, f); std::fclose(f); }
      user_gecko::load(codes.string(), {}, false);
      host::log("lag test: the Lossless adapter's lag test codes are on (%s)", codes.u8string().c_str());
    }
  } else {
    user_gecko::load(codes.string(), gecko_on, gecko_chosen);
  }
  g_panel.gecko_chosen = gecko_chosen;
  g_panel.gecko_saved_on = gecko_on;
  source_port::mods::set_choices(options.mod_choices);
  // The Training Lab's bits (0x8000 and up) are no longer offered: Training Mode CE replaces it.
  options.te_options &= 0x00007FFFu;
  // 20XX TE's "L-cancel training wheels" is not offered: the port's own Auto L-cancel does that.
  options.te_options2 &= ~0x800u;
  // "20XX CPUs" is a plain option now (Game tab, cpu_20xx, both engines, no TE save needed). A file
  // from when it was 20XX TE's word 2 bit 0x800000 turns the option on once; the bit itself is
  // cleared here and never saved again, so the migration happens one time.
  if (options.te_options2 & 0x800000u) { options.cpu_20xx = true; options.te_options2 &= ~0x800000u; }
  RenderOptions::live_cpu_20xx() = options.cpu_20xx;
  RenderOptions::live_cpu_ai().store(options.cpu_ai);
  RenderOptions::live_clean_replays().store(options.clean_replays, std::memory_order_relaxed);
  RenderOptions::live_stadium_frozen() = options.stadium_frozen;
  RenderOptions::live_freeze_fd() = options.freeze_fd;
  host::g_freeze_fd.store(options.freeze_fd, std::memory_order_relaxed);
  host::g_hide_ko_explosions.store(options.hide_ko_explosions, std::memory_order_relaxed);
  if (options.hide_percents) options.hide_hud_emblems = false;
  host::g_hide_hud_emblems.store(options.hide_hud_emblems, std::memory_order_relaxed);
  gx::set_hide_percents(options.hide_percents);
  host::g_community_codes.store(options.community_codes, std::memory_order_relaxed);
  RenderOptions::live_unlock_all() = options.unlock_all;
  RenderOptions::live_zelda_as_sheik() = options.zelda_as_sheik;
  RenderOptions::live_results_screen() = options.results_screen;
  RenderOptions::live_training_hud() = options.training_hud;
  RenderOptions::live_offline_delay() = options.offline_delay;
  // Old files used the online slider for offline play. Preserve that value once, independent
  // of key order, then save separate values so future online changes leave offline play alone.
  if (!offline_delay_frames_seen) options.offline_delay_frames = slippi::online::config().delay;
  RenderOptions::live_offline_delay_frames() = options.offline_delay_frames;
  host::early_input::set_enabled(options.early_inputs);
#ifndef MELEE_NO_SLIPPI
  RenderOptions::live_slippi_spectator() = options.slippi_spectator;
  RenderOptions::live_slippi_spectator_port() = options.slippi_spectator_port;
  RenderOptions::live_online_return_training() = options.online_return_training;
#endif
  host::g_cpu_20xx.store(options.cpu_20xx, std::memory_order_relaxed);
  RenderOptions::live_cpu_training() = options.cpu_training_word();
  // Explicit new choices win over legacy keys regardless of their order in the file. A legacy
  // TE_ENABLE alone remains both flashes, white success and red miss, including old recordings.
  if (saved_lcancel_flash >= 0) {
    options.te_options2 = mu_lcancel_with_flash_mode(options.te_options2, saved_lcancel_flash);
    lcancel::set_flash_mode(saved_lcancel_flash);
  } else if (options.te_options2 & MU_LCFLASH_TE_ENABLE) lcancel::set_indicator(false);
  if (saved_lcancel_success >= 0)
    options.te_options2 = mu_lcancel_with_success_color(options.te_options2, saved_lcancel_success);
  if (RenderOptions::kModFeaturesAvailable) RenderOptions::live_te_options() = options.te_options;
  apply_practice_tools(options);   // the Practice page's Training tools (lab bits) on top
  RenderOptions::live_mods_in_direct() = options.mods_in_direct;
  RenderOptions::live_te_options2() = options.te_options2;
  RenderOptions::live_te_menu_music().store(options.te_menu_music);
  host::phob_store_load(options.settings_path);
}

// The settings file. Written by the panel after a change, and at once when the game changes a
// saved value itself (the 20XX TE menu song).
// A colour as saved: "-" for the look's own, otherwise RRGGBB (first used by the removed Lab view).
static std::string lab_colour_text(uint32_t c) {
  if (!c) return "-";
  char buf[8];
  std::snprintf(buf, sizeof buf, "%06X", (unsigned)(c & 0xFFFFFFu));
  return buf;
}

// Puts a freshly written settings file in place of the old one. Windows Defender, the search indexer or
// a sync client can hold the old file open for a moment after each save, and the swap then failed with
// access denied or a sharing violation: the change was lost and the panel said settings could not be
// saved. A few short retries ride that out; a folder the game cannot write to never gets this far.
static bool replace_settings_file(const std::filesystem::path& temporary, const std::filesystem::path& path) {
  for (int attempt = 0;; ++attempt) {
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    const unsigned long code = GetLastError();
    if (attempt >= 12 || (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION)) {
      SetLastError(code);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5 + attempt * 3));
  }
}

static bool write_settings_file_impl(const RenderOptions& options);
static bool write_settings_file(const RenderOptions& options) {
  if (write_settings_file_impl(options)) { g_settings_save_error.clear(); return true; }
  const unsigned long code = GetLastError();
  const std::string reason = code == ERROR_ACCESS_DENIED ? "Windows denied access" : "error " + std::to_string(code);
  if (g_settings_save_error.empty())
    host::log("settings: could not save %s (%s)", options.settings_path.c_str(), reason.c_str());
  g_settings_save_error = "Settings could not be saved to " + options.settings_path + " (" + reason +
                          "). Move the game folder somewhere you can write to (not Program Files), or allow "
                          "the game in Windows Security > Controlled folder access.";
  return false;
}
static bool write_settings_file_impl(const RenderOptions& options) {
  std::filesystem::path path(options.settings_path), temporary = path; temporary += ".tmp";
  std::ofstream file(temporary);
  file << "fps " << options.fps_cap << "\nscale " << options.efb_scale << "\nfullscreen " << options.fullscreen
       << "\nexclusivefullscreen " << options.exclusive_fullscreen
       << "\nvsync " << (options.vsync || options.vsync_auto)
       << (options.vsync_auto ? "" : "\nvsyncchoice 1")
       << (options.vrr_found ? "\nvrrfound 1" : "")
       << "\nwidescreen " << options.widescreen
       << "\nfodreflections " << options.fod_reflections << "\ncssbackgroundfree " << options.css_background_free
       << "\ntruewidescreen " << options.true_widescreen << "\ntruewsaspect " << (int)options.true_aspect << "\ntruewsfit 1"
       << "\ntruewscustomw " << options.true_custom_w << "\ntruewscustomh " << options.true_custom_h << "\naspect " << (int)options.aspect
       << "\nwindow " << (options.window_pinned ? std::to_string(options.window_w) + "x" + std::to_string(options.window_h) : std::string("follow"))
       << "\nvolume " << g_panel.volume << "\nperformance " << options.performance_overlay
       << "\naudio_early_keyon " << (gx::RenderOptions::live_audio_early_keyon() ? 1 : 0)
       << "\nmute_voices " << (gx::RenderOptions::live_mute_voices() ? 1 : 0)
       << "\naudio_mode " << options.audio_mode << "\naudio_buffer_ms " << options.audio_buffer_ms << "\naudio_device " << options.audio_device
       << "\naudio_asio_driver " << options.audio_asio_driver << "\naudio_asio_buffer " << options.audio_asio_buffer
       << "\nshowfps " << options.show_fps
       << "\nshowvram " << (options.show_vram ? 1 : 0) << "\nshowping " << options.show_ping
       << "\nliveping " << (options.live_ping ? 1 : 0)
       << "\nnetoverlay " << (net_overlay::enabled() ? 1 : 0)
       << "\ndlss " << options.dlss_mode << "\nframegen " << options.frame_generation_mode
       << "\nfgmax " << options.fg_cached_max << "\nfgdynamic " << (options.fg_cached_dynamic ? 1 : 0)
       << "\nreflex " << options.reflex_mode << "\nreflexstats " << (options.reflex_stats ? 1 : 0)
       << "\nreflexflash " << (options.reflex_flash ? 1 : 0)
       << "\nlatencytest " << (options.latency_test ? 1 : 0)
       << "\nlagtestnext " << (options.lossless_lag_test ? 1 : 0)
       << "\nhdroutput " << (options.hdr_output ? 1 : 0) << "\nhdrpaperwhite " << options.hdr_paper_white
       << "\nhdrpeak " << options.hdr_peak
       << "\nbackend " << (options.api == RenderApi::D3D11 ? "d3d11" : options.api == RenderApi::Vulkan ? "vulkan" : "d3d12")
       << "\ncrt_filter " << options.crt_filter << "\ncrt_curve " << options.crt_curve << "\nborderart " << options.border_art << "\nborderedge " << options.border_edge << "\nshaderpreset " << options.shader_preset <<"\nsharpness " << options.sharpness << "\nssao " << options.screen_space_ao << "\nbrightness " << options.brightness
       << "\ncontrast " << options.contrast << "\nvibrance " << options.vibrance
       << "\nanisotropy " << options.anisotropy << "\nssaa " << options.ssaa << "\nmsaa " << options.msaa
       << "\nsubframe " << (options.subframe == SubFrameMode::Off ? 0 : options.subframe == SubFrameMode::AuthoredInterpolate ? 2 : 1) << "\nmusic " << slippi::jukebox::user_volume()
       << "\nmusicpacks " << (slippi::jukebox::music_packs_enabled() ? 1 : 0)
       << "\nonlinedelay " << slippi::online::config().delay
       << "\nonlinedelayauto " << (slippi::online::config().auto_delay ? 1 : 0)
       << "\noffline_delay " << (options.offline_delay ? 1 : 0)
       << "\noffline_delay_frames " << options.offline_delay_frames
       << "\nearly_inputs3 " << (options.early_inputs ? 1 : 0)
       << "\nquickchat " << slippi::online::config().chat
       << "\ndirectpause " << (slippi::online::config().direct_pause ? 1 : 0)
       << "\nonlineitems " << (slippi::online::config().items ? 1 : 0)
       << "\nonlinesquad " << (slippi::online::config().squad ? 1 : 0)
       << "\nportcolour1 " << slippi::online::config().port_colour[0]
       << "\nportcolour2 " << slippi::online::config().port_colour[1]
       << "\nblockcode1 " << slippi::online::config().blocked_codes[0]
       << "\nblockcode2 " << slippi::online::config().blocked_codes[1]
       << "\nblockcode3 " << slippi::online::config().blocked_codes[2]
       << "\nopponentskins " << (host::skins::enabled() ? 1 : 0)
       << "\ndoubleslobbies " << (host::lobby_directory::enabled() ? 1 : 0)
       << host::release_scope::settings_lines()   // only the switches turned on (release_scope.h)
       << "\npracticecpu " << practice_cpu::to_text(practice_cpu::config());
  for (int i = 0; i < practice_cpu::kSlots; ++i) {
    MuPcConfig saved;
    if (practice_cpu::slot(i, saved)) file << "\npracticecpuslot" << (i + 1) << " " << practice_cpu::to_text(saved);
  }
  file
       << "\nadvancedvideo " << (g_panel.advanced_open[0] ? 1 : 0)
       << "\nadvancedoverlays " << (g_panel.advanced_open[1] ? 1 : 0)
       << "\nstockhudscale " << options.stock_hud_scale
       << "\ndamagehudscale " << options.damage_hud_scale
       << "\nplayernicknames " << (options.show_player_nicknames ? 1 : 0)
       << "\nmatchmakinghint " << (options.matchmaking_hint ? 1 : 0) << "\npracticestyle " << options.practice_style << "\npracticeopens " << options.practice_opens
       << "\ninputoverlay " << options.input_overlay << "\ninputoverlayports " << options.input_overlay_ports
       << "\nclean_replays " << (options.clean_replays ? 1 : 0)
       << "\ninputoverlayhideborder " << options.input_overlay_hide_border
       << "\ninputoverlayvalues " << options.input_overlay_values
       << "\ninputoverlaystick " << options.input_overlay_stick;
  for (int i = 0; i < 4; ++i)
    if (options.input_overlay_rect[i][2] > 0.0f)
      file << "\ninputoverlayrect" << i << " " << options.input_overlay_rect[i][0] << " " << options.input_overlay_rect[i][1]
           << " " << options.input_overlay_rect[i][2] << " " << options.input_overlay_rect[i][3]
           << " " << options.input_overlay_rect[i][4] << " " << options.input_overlay_rect[i][5];
  file << "\neffects " << options.effects_level
       << "\nlightweight " << options.lightweight << "\nlowspec_look 1"
       << "\nretro " << (options.retro ? 1 : 0)
       << "\nlook_fighter " << options.look_fighter << "\nlook_stage " << options.look_stage
       << "\nlook_menus " << options.look_menus << "\nlook_unfiltered " << (options.look_unfiltered ? 1 : 0) << "\nlook_own " << (options.look_own ? 1 : 0)
       << "\nlook_full_models " << (options.look_full_models ? 1 : 0) << "\nlook_cap_lifted " << (options.look_cap_lifted ? 1 : 0)
       << "\nlook_saved_fps " << options.look_saved_fps_cap << "\nlook_saved_subframe " << options.look_saved_subframe
       << "\nlook_colours " << options.look_colours << "\nlook_background " << options.look_background
       << "\nlook_background_rgb " << std::hex << options.look_background_rgb << std::dec
       << "\nstadium_frozen " << (options.stadium_frozen ? 1 : 0)
       << "\nplayertags " << options.player_tags_always
       << "\narcade_hitboxes " << (options.arcade_hitboxes ? 1 : 0)
       << "\narcade_projectile_hitboxes " << (options.arcade_projectile_hitboxes ? 1 : 0)
       << "\narcade_hitboxes_behind " << (options.arcade_hitboxes_behind ? 1 : 0)
       << "\narcade_camera_2d " << (options.arcade_camera_2d ? 1 : 0)
       << "\narcade_combo_counter " << (options.arcade_combo_counter ? 1 : 0)
       << "\nshield_colorp1 " << lab_colour_text(options.shield_color[0]) << "\nshield_colorp2 " << lab_colour_text(options.shield_color[1])
       << "\nshield_colorp3 " << lab_colour_text(options.shield_color[2]) << "\nshield_colorp4 " << lab_colour_text(options.shield_color[3])
       << "\nshield_color_online " << lab_colour_text(options.shield_color_online)
       << "\narcade_backdrop " << options.arcade_backdrop
       << "\narcade_background_rgb " << std::hex << options.arcade_background_rgb << std::dec
       << "\narcade_grid_rgb " << std::hex << options.arcade_grid_rgb << std::dec
       // Low spec: the switch, and the settings it is holding for the player while it is on.
       << "\nlowspec " << (options.low_spec ? 1 : 0)
       << "\nlowspec_prev_backend " << (options.low_spec_previous.api == RenderApi::D3D11 ? "d3d11" : options.low_spec_previous.api == RenderApi::Vulkan ? "vulkan" : "d3d12")
       << "\nlowspec_prev_fps " << options.low_spec_previous.fps_cap
       << "\nlowspec_prev_scale " << options.low_spec_previous.efb_scale
       << "\nlowspec_prev_ssaa " << options.low_spec_previous.ssaa
       << "\nlowspec_prev_anisotropy " << options.low_spec_previous.anisotropy
       << "\nlowspec_prev_effects " << options.low_spec_previous.effects_level
       << "\nlowspec_prev_dlss " << options.low_spec_previous.dlss_mode
       << "\nlowspec_prev_subframe " << (options.low_spec_previous.subframe == SubFrameMode::Off ? 0 : options.low_spec_previous.subframe == SubFrameMode::AuthoredInterpolate ? 2 : 1)
       << "\nlcancel_flash_mode " << mu_lcancel_flash_mode(options.te_options2, lcancel::flash_mode())
       << "\nlcancel_success_flash " << mu_lcancel_success_color(options.te_options2)
       << "\nlcancelindicator " << (lcancel::indicator_enabled() ? 1 : 0)
       << "\nautolcancel " << (lcancel::automatic_enabled() ? 1 : 0)
       << "\npalstockicons " << (gecko::option_pal_stock_icons ? 1 : 0)
       << "\nnoscreenshake " << (gecko::option_no_screen_shake ? 1 : 0)
       << "\nscreenshake " << gecko::option_screen_shake_percent
       << "\nswpro_gc_picture " << (g_panel.swpro_gc_picture ? 1 : 0)
       << family_options_text()
       << "\nrumble2 " << (host::g_rumble_enabled.load(std::memory_order_relaxed) ? 1 : 0)
       << "\nbackgroundinput " << (host::g_background_input ? 1 : 0)
       << "\ngamelanguage " << host::g_game_language.load()
       << "\ncpu_20xx " << (options.cpu_20xx ? 1 : 0)
       << "\ncpu_ai " << std::hex << options.cpu_ai << std::dec
       << "\nfreeze_fd " << (options.freeze_fd ? 1 : 0)
       << "\nhide_ko_explosions " << (options.hide_ko_explosions ? 1 : 0)
       << "\nhide_hud_emblems " << (options.hide_hud_emblems ? 1 : 0)
       << "\nhide_percents " << (options.hide_percents ? 1 : 0)
       << "\ncommunity_codes " << std::hex << options.community_codes << std::dec
       << "\nunlock_all " << (options.unlock_all ? 1 : 0)
       << "\nzelda_as_sheik " << (options.zelda_as_sheik ? 1 : 0)
       << "\nresults_screen " << (options.results_screen ? 1 : 0)
       << "\ntraining_hud " << (options.training_hud ? 1 : 0)
#ifndef MELEE_NO_SLIPPI
       << "\nslippi_spectator " << (options.slippi_spectator ? 1 : 0)
       << "\nslippi_spectator_port " << options.slippi_spectator_port
       << "\nonline_return_training " << (options.online_return_training ? 1 : 0)
#endif
       << "\ncpu_tech " << options.cpu_tech
       << "\ncpu_getup " << options.cpu_getup
       << "\ncpu_di " << options.cpu_di
       << "\ncpu_sdi " << options.cpu_sdi
       << "\ncpu_notaunt " << (options.cpu_no_taunt ? 1 : 0)
       << "\ncpu_lcancel " << (options.cpu_lcancel ? 1 : 0)
       << "\ncpu_nojab " << (options.cpu_no_rapid_jab ? 1 : 0)
       << "\ncpu_notransform " << (options.cpu_no_transform ? 1 : 0)
       << (g_panel.custom_preset.set ? "\ncustompreset " + std::to_string(g_panel.custom_preset.efb) + " " + std::to_string(g_panel.custom_preset.ssaa) + " " +
                                     std::to_string(g_panel.custom_preset.aniso) + " " + std::to_string(g_panel.custom_preset.dlss) + " " +
                                     std::to_string(g_panel.custom_preset.fps) + " " + std::to_string(g_panel.custom_preset.sub)
                               : std::string())
       << "\ndiscordon " << (options.discord_presence ? 1 : 0);   // the key the load reads ("discord" was never read back)
  // Only when set: "key value" parsing would swallow the next line on an empty value.
  if (!options.mod_profile.empty()) file << "\nmod_profile " << options.mod_profile;
  file << source_port::mods::choice_lines(options.mod_choices);
  if (!options.mods_in_direct) file << "\nmods_in_direct 0";
  if (options.te_options2) { char te2[16]; std::snprintf(te2, sizeof te2, "%X", options.te_options2); file << "\nte_options2 " << te2; }
  if (options.te_menu_music) file << "\nte_menu_music " << options.te_menu_music;
  { char te[16]; std::snprintf(te, sizeof te, "%X", options.te_options); file << "\nte_options " << te; }
  // One line per pack that is switched off. Without this the loader parsed "texpackoff" but
  // nothing ever wrote it, so switching a pack off lasted only until the next launch. The
  // loader reads the name to end of line, so a name with spaces in it round-trips.
  // The texture settings themselves. The loader has always parsed these three, but nothing ever
  // wrote them, so "Use texture packs" came back unchecked at every launch and a player who had
  // set it up correctly was told their packs were off. Same omission that hid texpackoff.
  file << "\ncustomtextures " << (options.custom_textures ? 1 : 0)
       << "\ndumptextures " << (options.dump_textures ? 1 : 0)
       << "\nprefetchtextures " << (options.prefetch_textures ? 1 : 0)
       << "\nvideobackgrounds " << (options.video_backgrounds ? 1 : 0);
  file << texpack_disabled_lines();
  // The player's Gecko codes switched on, by name (names can hold spaces; read to end of line).
  if (g_panel.gecko_chosen) {
    file << "\ngeckochosen 1";
    if (std::getenv("MELEE_LAG_TEST")) {   // the lag test's codes are not the player's: keep theirs
      for (const std::string& name : g_panel.gecko_saved_on) file << "\ngeckocode " << name;
    } else {
      // A code this engine cannot run keeps the switch it was saved with (on for the Static Recomp
      // stays on when the Source Port writes this file); one that runs here is saved as it runs.
      for (const user_gecko::Code& c : user_gecko::codes())
        if (c.enabled || (!c.supported && c.wanted)) file << "\ngeckocode " << c.name;
    }
  }
  write_controls_lines(file);   // bindings, devices, levels, profiles (settings_controls.cpp)
  file << '\n';
  file.close();
  const bool saved = file.good() && replace_settings_file(temporary, path);
  host::log(saved ? "settings: file saved" : "settings: file save failed");
  return saved;
}

bool save_pc_settings(const RenderOptions& options, int volume) {
  g_panel.volume = volume;
  return write_settings_file(options);
}

bool consume_lag_test_launch(const std::string& settings_path) {
  // Only that line changes: the launch's own options can hold command-line values that are not the
  // player's saved ones, so the whole file is not written from them.
  std::ifstream in(std::filesystem::u8path(settings_path));
  if (!in) return false;
  std::string text, line;
  bool found = false;
  while (std::getline(in, line)) {
    if (line.rfind("lagtestnext ", 0) == 0) { line = "lagtestnext 0"; found = true; }
    text += line; text += '\n';
  }
  in.close();
  if (!found) return true;
  std::filesystem::path path = std::filesystem::u8path(settings_path), temporary = path; temporary += ".tmp";
  std::ofstream out(temporary);   // text mode, as write_settings_file writes it
  out << text;
  out.close();
  return out.good() && replace_settings_file(temporary, path);
}

namespace settings_store {

bool write(const RenderOptions& options) { return write_settings_file(options); }

void save_game_changes(RenderOptions& options, uint32_t& mod_choices_seen, bool& saved) {
  // Scans and mod switches also save from the game window. A first-found choice
  // must survive a restart, and a pack switched off must never turn itself back on.
  if (const uint32_t version = source_port::mods::choices_version(); version != mod_choices_seen) {
    options.mod_choices = source_port::mods::choices();
    saved = write_settings_file(options);
    if (saved) mod_choices_seen = version;
}
// 20XX TE: the game chose a menu song in Sound Test. Saved now, like TE saves it.
if (const int song = RenderOptions::live_te_menu_music().load(); song != options.te_menu_music) {
  options.te_menu_music = song;
  saved = write_settings_file(options);
}
// The CPU AI picker: a port's choice changed on the character select (host command 0xE7). Saved now.
{
  static uint32_t ai_seen = 0;
  if (const uint32_t seen = RenderOptions::live_cpu_ai_changes().load(); seen != ai_seen) {
    ai_seen = seen;
    options.cpu_ai = RenderOptions::live_cpu_ai().load();
    saved = write_settings_file(options);
  }
}
// 20XX TE: the settings changed in the game's own TE menu (Tournament Melee). Saved now too.
{
  static uint32_t te_game_seen = 0;
  if (const uint32_t seen = RenderOptions::live_te_game_changes().load(); seen != te_game_seen) {
    te_game_seen = seen;
    const uint32_t te2_before = options.te_options2;
    options.te_options = (options.te_options & ~0x7FF0u) | (RenderOptions::live_te_options() & 0x7FF0u);
    options.te_options2 = RenderOptions::live_te_options2();
    // A TE switch that is also one of ours, changed in that menu: ours follows (te_pairs_linked).
    for (const uint32_t bit : {kTeNoScreenRumble, kTeInputDisplay})
      if ((te2_before ^ options.te_options2) & bit) ours_follow_te(options, bit, (options.te_options2 & bit) != 0);
    saved = write_settings_file(options);
  }
}
// The online character select's Options (shim/mu_online_options.c, host command 0xDA with two
// bytes) changed a setting the host keeps itself (opponents' skins). Saved now, like the panel's.
{
  static uint32_t online_option_seen = 0;
  if (const uint32_t seen = RenderOptions::live_online_option_changes().load(); seen != online_option_seen) {
    online_option_seen = seen;
    saved = write_settings_file(options);
  }
}
// 20XX TE's two switches that are also ours (te_pairs_linked). The first frame the pairs are linked
// (at boot with TE loaded, or when TE is switched on), a pair that disagrees (a settings file from
// before the link) settles on what the player was already seeing: on when either one had its effect
// on. TE's own switch has none in Tournament Mode, so there ours decides. Locked TE settings stay
// as they are. Only that first frame: after it, every change moves both.
{
  static bool te_linked_before = false;
  const bool linked = te_pairs_linked(options);
  if (linked && !te_linked_before) {
    const bool te_effective = (options.te_options & 0x20u) == 0;
    const bool locked = (options.te_options2 & kTeLockSettings) != 0;
    bool moved = false;
    const auto settle = [&](bool& ours, uint32_t bit) {
      const bool te = (options.te_options2 & bit) != 0;
      const bool on = ours || (te && te_effective);
      if (ours != on) { ours = on; moved = true; }
      if (!locked && te != on) {
        options.te_options2 = on ? (options.te_options2 | bit) : (options.te_options2 & ~bit);
        moved = true;
      }
    };
    settle(gecko::option_no_screen_shake, kTeNoScreenRumble);
    settle(options.input_overlay, kTeInputDisplay);
    if (moved) {
      RenderOptions::live_te_options2() = options.te_options2;
      saved = write_settings_file(options);
      host::log("pc settings: 20XX TE screen rumble and input display linked with ours (%08X)", options.te_options2);
    }
  }
  te_linked_before = linked;
}
}

void follow_display_refresh(RenderOptions& options, bool& saved) {
  if (options.vsync_auto) {   // not chosen: off with G-Sync or FreeSync, on otherwise (on while unknown)
    static const bool assume_vrr = [] { const char* v = std::getenv("MELEE_ASSUME_VRR"); return v && *v == '1'; }();   // tests only
    const int vrr = assume_vrr ? 1 : RenderOptions::live_variable_refresh().load(std::memory_order_relaxed);
    // Not known yet (the first seconds of a launch): what the last session found stands.
    // Off on every display since 1.0 (owner, 11-21): V-Sync's wait was the largest single part of the
    // delay a fresh install had (a menu press: 23.8 ms with it, 14.4 without, on a 200 Hz screen). A
    // fixed-refresh screen can tear; the V-Sync toggle is the player's choice and ends this rule.
    const bool want = false;
    if (options.vsync != want) {
      options.vsync = want;
      host::log("present: vsync off (automatic; %s)", vrr == 1 ? "G-Sync or FreeSync found" : vrr == 0 ? "fixed refresh, frames can tear" : "display not judged yet");
    }
    if (vrr >= 0 && options.vrr_found != (vrr == 1)) {   // remembered for the next launch
      options.vrr_found = vrr == 1;
      saved = write_settings_file(options);
    }
}
}

}  // namespace settings_store

}  // namespace gx
