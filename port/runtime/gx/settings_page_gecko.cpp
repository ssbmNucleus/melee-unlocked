// The settings window's Gecko Codes: the built-in codes and the player's own (GeckoCodes.ini).
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Gecko Codes page (tab 6): the built-in codes and the player's.
void settings_page_gecko(SettingsPageContext& page) {
  RenderOptions& options = page.options;
  bool& changed = page.changed;
    // The Source Port runs the game as C: a Gecko code is PowerPC written over console addresses,
    // so there is nothing for it to patch. Slippi's optional codes are rebuilt in C instead, and
    // these switches are the same settings as their copies on the other tabs.
#ifdef MELEE_NO_SLIPPI   // the same switches under neutral names
    ImGui::TextUnformatted("Built-in codes");
#else
    ImGui::TextUnformatted("Slippi's codes, built in");
#endif
    if (settings_toggle("Disable Screen Shake", &gecko::option_no_screen_shake)) {
      te_follow_ours(options, kTeNoScreenRumble, gecko::option_no_screen_shake);
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera only. Online safe.%s", te_pair_hover(kTeNoScreenRumble));
    if (settings_lcancel_flash(options)) changed = true;
    ImGui::BeginDisabled(host::gameplay::vanilla_only());
#ifdef MELEE_NO_SLIPPI
    // This build plays the retail game (no General Codes), and it is the only engine: the lines
    // below say what runs here without naming another one.
    ImGui::Separator();
    ImGui::TextUnformatted("Your codes");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("From:\n%s\nCodes that only write game variables run. Other codes are PowerPC and cannot run in this build.", user_gecko::path().c_str());
    if (user_gecko::codes().empty()) {
      settings_hint("None yet. Use \"+ Add Gecko code\" below and paste a code, or put a GeckoCodes.ini next to port-settings.ini.");
    } else {
      for (user_gecko::Code& c : user_gecko::codes()) {
        ImGui::PushID(&c);
        const char* built_in = user_gecko::native_equivalent(c);
        // The always-display hitbox code keeps its switch: on, it turns on the Arcade look's hitboxes.
        const bool arcade_code = built_in == user_gecko::kArcadeHitboxCodeLabel;
        const bool runs_here = (c.supported && !built_in) || arcade_code;
        ImGui::BeginDisabled(!runs_here);
        if (settings_toggle(c.name.c_str(), &c.enabled) && runs_here) { c.wanted = c.enabled; changed = true; settings_store::panel_values().gecko_chosen = true; }
        ImGui::EndDisabled();
        if (arcade_code) {
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs natively: while it is on, attack hitboxes show as with VIDEO > Arcade look.\n"
                              "Offline matches and replays only.");
        } else if (built_in)
          engine_only_reason("Built in: \"%s\" is the switch that applies here.", built_in);
        else if (!runs_here)
          engine_only_reason("Cannot run in this build: this code %s.", c.reason.c_str());
        else if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\nNever applied online.",
                            c.fn_bytes.empty() ? "Writes game variables only, so it runs here."
                                               : "Only makes a game function give a fixed answer, which this build does too.");
        ImGui::PopID();
      }
    }
#else
    settings_hint("Always on, as in Slippi: the General Codes (UCF 0.84 and the rest) and Lagless FoD.");
    ImGui::Separator();
    ImGui::TextUnformatted("Your codes");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("From:\n%s\nOther codes are PowerPC and run on the Static Recomp engine.", user_gecko::path().c_str());
    if (user_gecko::codes().empty()) {
      settings_hint("None yet. Use \"+ Add Gecko code\" below and paste a code. Codes you add on the Static Recomp engine are listed here too.");
    } else {
      // The rule for settings one engine runs: each code keeps its switch (its Static Recomp
      // setting), greyed here, and hovering it says why.
      for (user_gecko::Code& c : user_gecko::codes()) {
        ImGui::PushID(&c);
        const char* built_in = user_gecko::native_equivalent(c);
        // A code that only writes variables this build can place runs here too (user_gecko.h);
        // the rest keep the rule above.
        // The always-display hitbox code keeps its switch: on, it turns on the Arcade look's hitboxes.
        const bool arcade_code = built_in == user_gecko::kArcadeHitboxCodeLabel;
        const bool runs_here = (c.supported && !built_in) || arcade_code;
        ImGui::BeginDisabled(!runs_here);
        // A code that cannot run here shows the switch it keeps for the Static Recomp.
        if (settings_toggle(c.name.c_str(), runs_here ? &c.enabled : &c.wanted) && runs_here) { c.wanted = c.enabled; changed = true; settings_store::panel_values().gecko_chosen = true; }
        ImGui::EndDisabled();
        if (arcade_code) {
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs natively: while it is on, attack hitboxes show as with VIDEO > Arcade look.\n"
                              "Offline matches and replays only.");
        } else if (built_in)
          engine_only_reason("Built into the Source Port: \"%s\" is the switch that applies here.\n"
                             "This one is your setting for the Static Recomp.", built_in);
        else if (!runs_here) {
          engine_only_reason("Cannot run on the Source Port: this code %s.\n"
                             "It keeps its setting for the Static Recomp.", c.reason.c_str());
        }
        else if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\nNever applied online or in replays.",
                            c.fn_bytes.empty() ? "Writes game variables only, so it runs on the Source Port too."
                                               : "Only makes a game function give a fixed answer, which the Source Port does too.");
        ImGui::PopID();
      }
    }
#endif
    ImGui::Separator();
    if (gecko_add_code_button()) changed = true;
    ImGui::EndDisabled();
}

}  // namespace gx
