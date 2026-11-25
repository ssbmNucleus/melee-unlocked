// The settings window's Overlays: replays and nicknames, the Training and Practice HUDs, the performance and
// controller overlays, and the developer readouts.
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Overlays page (tab 4).
void settings_page_overlays(SettingsPageContext& page) {
  RenderOptions& options = page.options;
  bool& changed = page.changed;
    if (settings_toggle("Clean replays", &options.clean_replays)) {
      RenderOptions::live_clean_replays().store(options.clean_replays, std::memory_order_relaxed);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("While a replay plays, nothing is drawn over the game: no controller or input display,\n"
                        "frame rate, graphs, notices, hitboxes or combo counter, and the replay bar starts hidden\n"
                        "(H shows it; every key still works). For tournament VODs and clips.");
    changed |= settings_toggle("Show player nicknames above fighters", &options.show_player_nicknames);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shows each online player's name at Melee's player-tag anchor. Local display only.");
    if (settings_toggle("Show Training mode's HUD", &options.training_hud)) {
      RenderOptions::live_training_hud() = options.training_hud;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Off: Training mode's Speed, Enemy and damage panels are not drawn, for sparring with nothing\n"
                        "on screen. They still show while Training's menu is open. Source Port.");
#ifndef MELEE_NO_SLIPPI   // no matchmaking panel in the build without that layer
    changed |= settings_toggle("Show the Practice menu hint in Training", &options.matchmaking_hint);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Start + D-pad Up + Z opens the Practice menu. The hint stops by itself once you have used it.");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tab still opens matchmaking with it off.");
    {
      static const char* const looks[practice_menu::kStyleCount] = {
          practice_menu::style_name(0), practice_menu::style_name(1), practice_menu::style_name(2),
          practice_menu::style_name(3), practice_menu::style_name(4)};
      int look = std::clamp(options.practice_style, 0, practice_menu::kStyleCount - 1);
      ImGui::SetNextItemWidth(260.0f);
      if (settings_combo("Practice menu look", &look, looks, practice_menu::kStyleCount)) { options.practice_style = look; changed = true; }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How the Practice menu (Start + D-pad Up + Z in Training) looks. All of them work with the controller;\n"
                          "the window also works with the mouse.");
    }
#endif
    changed |= settings_toggle("Performance overlay", &options.performance_overlay);
    changed |= settings_toggle("FPS counter (top left)", &options.show_fps);
    // VRAM has its own line in the Video tab, right by the settings that move it; no overlay needed.
    {
      static const char* const kPing[] = {"Off", "Once a second", "Live"};
      int ping = !options.show_ping ? 0 : options.live_ping ? 2 : 1;
      if (settings_combo("Ping while online (under the FPS)", &ping, kPing, 3)) {
        options.show_ping = ping != 0;
        if (ping) options.live_ping = ping == 2;
        changed = true;
      }
    }
    {
      bool net = net_overlay::enabled();
      if (settings_toggle("Network and timing", &net)) { net_overlay::set_enabled(net); changed = true; }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Online only: the last 10 seconds as you got them. Frame time, waits for the\nother player's inputs, rollbacks, time sync and ping. Display only.");
    }
    // Render latency, a developer readout, is under Advanced at the bottom of this tab.
    // Screenshot tooling (MELEE_SETTINGS_SCROLL_TO=controller): bring the controller overlay's rows into view.
    if (static int scroll_frames = 0; scroll_frames < 30) {
      const char* target = host::test_env::getenv("MELEE_SETTINGS_SCROLL_TO");
      if (target && std::strcmp(target, "controller") == 0) ImGui::SetScrollHereY(0.0f);
      ++scroll_frames;
    }
    // With 20XX TE on this is also TE's "Input display" (te_pairs_linked).
    if (settings_toggle("Controller overlay", &options.input_overlay)) {
      te_follow_ours(options, kTeInputDisplay, options.input_overlay);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("An on-screen controller for streams. Drag it to move it while the settings window is open.%s",
                        te_pair_hover(kTeInputDisplay));
    // Its own choices always show under it (greyed while it is off), so Hide background and the rest
    // can be found before switching it on; they were hidden until then.
    {
      ImGui::BeginDisabled(!options.input_overlay);
      ImGui::SameLine();
      changed |= settings_toggle("Show values", &options.input_overlay_values);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each stick's position as the game reads it. A full press is 1.0000.");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.0f);
      changed |= settings_slider("Stick size", &options.input_overlay_stick, 1, 10);
      // Several ports can be shown at once (doubles and crew streams want every player visible);
      // they stack upward from the bottom left corner. On a row of their own with Hide background:
      // on the one row above, that switch ran off the panel's right edge and could not be reached.
      ImGui::Indent(22.0f);
      for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof label, "P%d", i + 1);
        bool on = (options.input_overlay_ports & (1 << i)) != 0;
        if (settings_toggle(label, &on)) {
          options.input_overlay_ports = on ? (options.input_overlay_ports | (1 << i)) : (options.input_overlay_ports & ~(1 << i));
          changed = true;
        }
      }
      ImGui::SameLine();
      changed |= settings_toggle("Hide background", &options.input_overlay_hide_border);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes the panel behind the overlay, leaving only the buttons and sticks.");
      ImGui::Unindent(22.0f);
      ImGui::EndDisabled();
      if (options.input_overlay) settings_hint("  Drag an overlay to move it, and its edges to resize, while this settings window is open.");
    }
    // ---- Advanced: developer readouts, apart from the player HUD above ----
    if (settings_advanced_group(1, &changed)) {
      changed |= settings_toggle("Render latency (under the FPS)", &options.reflex_stats);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows the measured render latency under the FPS counter. Works whether or\n"
                          "not NVIDIA Reflex Low Latency (Video tab) is On, so Off has a number too.\n"
                          "Direct3D 12, and Vulkan on an NVIDIA card (its driver's frame report).\n"
                          "The full breakdown by stage is on the performance graph.");
    }
}

}  // namespace gx
