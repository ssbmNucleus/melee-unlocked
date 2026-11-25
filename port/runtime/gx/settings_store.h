// The settings store: port-settings.ini read into RenderOptions (and the host's own settings:
// bindings, deadzones, audio, online), written back from them, and what reading it applies. No UI.
// The settings window (pc_settings*.cpp, settings_page_*.cpp) is a view over these values: it edits them
// and saves through settings_store::write. Code: settings_file.cpp; the controller lines are
// settings_controls.cpp. The file is "key value" per line, the value to the end of the line; an
// unknown key is skipped, so an older file still loads and a newer one loads in an older version.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "render_options.h"
#include <cstdint>
#include <string>
#include <vector>

namespace gx {

// Read the file into options (and the host's settings); a missing key keeps its default.
void load_pc_settings(RenderOptions& options, int& volume);
// Same atomic writer used by the panel, for startup choices and settings validation.
bool save_pc_settings(const RenderOptions& options, int volume);
// The Lossless adapter lag test arms the next launch only: that launch sets the saved switch back
// off ("lagtestnext 0"), touching no other line of the file.
bool consume_lag_test_launch(const std::string& settings_path);

// "Faster sound effects" (Audio): shown since its timing was measured.
inline constexpr bool kShowFasterSoundEffects = true;

// 20XX TE has its own switch for two things the port already does: its "Disable screen rumble" is
// our Disable screen shake, and its "Input display" is our Controller overlay. While TE is on (its
// save loaded and "Use 20XX TE features" on) each pair is one setting: switching either one switches
// both, so neither reads off while the other has the effect on. Tournament Mode greys TE's two
// switches but keeps them in step, so leaving it finds the pairs still agreeing.
constexpr uint32_t kTeNoScreenRumble = 0x100u, kTeInputDisplay = 0x2000u, kTeLockSettings = 0x40000u;
bool te_pairs_linked(const RenderOptions& options);
// One of ours changed: TE's matching switch follows, unless the TE settings are locked.
void te_follow_ours(RenderOptions& options, uint32_t bit, bool on);
// TE's switch changed, on the Mods page or in the game's own TE menu: ours follows.
void ours_follow_te(RenderOptions& options, uint32_t bit, bool on);
// The Training tools in the practice settings drive the lab's option bits.
void apply_practice_tools(RenderOptions& options);

namespace settings_store {

// The player's own video settings, kept as the "Custom" quality preset: updated whenever the settings
// match none of the presets, so picking a preset and then Custom puts them back (and the two can be
// compared by switching). Saved as "custompreset efb ssaa aniso dlss fps subframe".
struct CustomPreset { bool set = false; int efb = 0, ssaa = 1, aniso = 16, dlss = 0; double fps = -1; int sub = 1; };

// Saved values that are no RenderOptions field: the panel edits them in place.
struct PanelValues {
  // The volume loaded from settings. The Audio tab runs only when it is being drawn, and ImGui runs a
  // tab's body only when it is the selected one, so saving from any other tab used to write whatever
  // that field happened to start as, which is zero. Players saw the volume set itself to 0 on a later
  // launch. 70 is what a game without a saved volume plays at (main.cpp). At 100, saving from the
  // launcher's Settings window before any volume was stored wrote 100, and the next game start was loud.
  int volume = 70;
  // The Advanced group at the bottom of Video ([0]) and Overlays ([1]): diagnostics and pack
  // making, out of the way of the everyday settings. Closed until the player opens it,
  // then remembered ("advancedvideo" and "advancedoverlays"; absent = closed).
  bool advanced_open[2] = {false, false};
  CustomPreset custom_preset;
  // The panel has saved which user Gecko codes are on (until then GeckoCodes.ini's own list is used).
  bool gecko_chosen = false;
  std::vector<std::string> gecko_saved_on;   // the player's own choice, kept through a lag test
  bool swpro_gc_picture = false;   // "Use GameCube controller picture" for Switch controllers
};
PanelValues& panel_values();

// Writes the whole file from options and the values above (atomically: a .tmp file moved into
// place). False when it could not be written; save_error() then says why, for the settings window
// to show.
bool write(const RenderOptions& options);
const std::string& save_error();

// Called every frame (in the game window and the settings window): settings the game or a scan changed by itself (mod
// choices, the 20XX TE and Hack Pack menus, the CPU pickers, the online Options) are taken into
// options and saved at once. saved is set by each write; mod_choices_seen is the last saved version.
void save_game_changes(RenderOptions& options, uint32_t& mod_choices_seen, bool& saved);
// Automatic V-Sync (no choice saved): applies it and remembers whether the display had G-Sync or
// FreeSync for the next launch.
void follow_display_refresh(RenderOptions& options, bool& saved);

}  // namespace settings_store
}  // namespace gx
