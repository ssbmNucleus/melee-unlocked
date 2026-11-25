// The settings window's Game: input, language, input delay, online, Discord, stages and stage effects,
// L-cancel, HUD, community codes.
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Game page (tab 2).
void settings_page_game(SettingsPageContext& page) {
  SettingsState& state = page.state;
  RenderOptions& options = page.options;
  bool& changed = page.changed;

    // ---- Input ----
    settings_section("Input");
    if (settings_toggle("Background input", &host::g_background_input)) changed = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: controllers keep playing while another window is in front\n"
                        "(a stream, Discord, a second monitor).\n"
                        "Off: the game ignores all input until you click back into its window.");

    // The game's own language option (Options > Language) lives in its save. This offers the same
    // choice here: the game is answered with it wherever it asks for its saved language, on both
    // engines, and the save is left as it is.
    settings_section("Language");
    {
      int language = host::g_game_language.load();
      const char* languages[] = {"The game's own choice", "Japanese", "English"};
      ImGui::SetNextItemWidth(220.0f);
      if (settings_combo("Game language", &language, languages, 3)) {
        host::g_game_language.store(language);
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Japanese gives the Japanese menus, fighter names and announcer calls,\n"
                          "as the game's own Options > Language does.\n"
                          "The game's own choice follows that option. Applies as each screen loads.");
    }

    // "20XX CPUs": one plain option on both engines, not a 20XX TE feature (no TE save needed). The
    // Source Port plays its native version of the 20XX Hack Pack's AI (shim/mu_20xx_ai.c); the Static
    // Recomp runs the pack's own AI block, read from the player's copy of the pack's disc under Mods
    // (host/hackpack_ai.cpp). Offline matches only; online and replay playback never see it.
    // Slippi's "Unlock All Characters and Stages", switchable. Off, the save file decides, so a new
    // save starts with the original fighters and stages and the game hands out the rest as it does
    // on a console. A network session always has everything (host.cpp apply_code_switches, and
    // shim/mu_gecko.c on the Source Port).
    if (settings_toggle("Unlock everything", &options.unlock_all)) {
      RenderOptions::live_unlock_all() = options.unlock_all;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: every fighter, stage and mode is available from the start.\n"
                        "Off: your save file decides, and the game unlocks things as you play.\n"
                        "Offline only: online play always has everything. Takes effect on the next screen.");
    // GitHub #75: rogueMelee has no "hold A at the start", and a run that swaps Down B away would
    // keep Zelda for good. Offline only (the opponent's game would disagree), never in replays.
    if (settings_toggle("Zelda starts as Sheik", &options.zelda_as_sheik)) {
      RenderOptions::live_zelda_as_sheik() = options.zelda_as_sheik;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("When you pick Zelda, the match starts as Sheik, as holding A at the start does in Versus.\n"
                        "Works in every mode, rogueMelee included. Down B still changes between them.\n"
                        "Offline only: online, use Slippi's own character choice. Source Port only.");
    if (settings_toggle("Show results screen", &options.results_screen)) {
      RenderOptions::live_results_screen() = options.results_screen;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: the results screen after each offline match.\n"
                        "Off: straight back to character select, for playing sets back to back.\n"
                        "Online play always goes back to character select.");
    // ---- Shield colours (here since 0.9.201; they were under Video) ----
    // Each port's shield (and KO blast) colour: a colour wheel, the game's preset colours one click away,
    // and the port's own. "Online" is the player's own colour in online matches; Melee Unlocked opponents
    // are sent it and show it, Slippi Dolphin opponents keep the port colour. Display only.
    if (settings_widgets::CollapsingHeader("Shield colors##shields")) {
      ImGui::Indent(8.0f);
      auto shield_row = [&](const char* label, uint32_t* value, int port_colour) {
        ImGui::PushID(label);
        const uint32_t shown = *value ? (*value & 0xFFFFFFu)
                                      : ((uint32_t)kShieldColorRgb[port_colour][0] << 16 | (uint32_t)kShieldColorRgb[port_colour][1] << 8 |
                                         kShieldColorRgb[port_colour][2]);
        float c[3] = {((shown >> 16) & 0xFF) / 255.0f, ((shown >> 8) & 0xFF) / 255.0f, (shown & 0xFF) / 255.0f};
        if (settings_color_edit3(label, c, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_DisplayRGB)) {
          *value = 0x1000000u | ((uint32_t)(c[0] * 255 + 0.5f) << 16) | ((uint32_t)(c[1] * 255 + 0.5f) << 8) | (uint32_t)(c[2] * 255 + 0.5f);
          changed = true;
        }
        ImGui::SameLine();
        if (!*value) ImGui::TextDisabled("port color");
        else if (settings_widgets::SmallButton("Port color")) { *value = 0; changed = true; }
        // The game's own presets, one click each.
        for (int i = 0; i < kShieldColorCount; ++i) {
          const uint8_t* rgb = kShieldColorRgb[i];
          if (i) ImGui::SameLine(0.0f, 3.0f);
          ImGui::PushID(i);
          if (settings_widgets::ColorButton(kShieldColorNames[i], ImVec4(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1.0f), 0, ImVec2(16, 16))) {
            *value = 0x1000000u | ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
            changed = true;
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      };
      static const char* const ports[4] = {"P1 shield", "P2 shield", "P3 shield", "P4 shield"};
      for (int port = 0; port < 4; ++port) shield_row(ports[port], &options.shield_color[port], port);
      ImGui::Separator();
      shield_row("Online (you)", &options.shield_color_online, 0);
      settings_hint("Your shield and KO blast colors. Online, \"Online (you)\" is yours (left empty, your P1 color): players"
                    " on compatible clients see it. Players on Slippi Dolphin see your port color. Team matches keep team colors.");
      ImGui::Unindent(8.0f);
    }
    // Input delay, Online and Discord sit here, right below the game's own switches (owner, 0.9.201);
    // they were at the very end of this tab.
    settings_section("Input delay");
    {
      static const char* offline_choices[] = {"Off (0 frames)", "1 frame", "2 frames", "3 frames",
                                              "4 frames", "5 frames", "6 frames", "7 frames", "8 frames", "9 frames"};
      int offline = options.offline_delay ? options.offline_delay_frames : 0;
      ImGui::SetNextItemWidth(180.0f);
      if (settings_combo("Offline input delay", &offline, offline_choices, 10)) {
        options.offline_delay = offline > 0;
        if (offline > 0) options.offline_delay_frames = offline;
        RenderOptions::live_offline_delay() = options.offline_delay;
        RenderOptions::live_offline_delay_frames() = options.offline_delay_frames;
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Added input delay in offline VS, Training and 1P modes, on both engines.\n"
                          "Off adds no delay. Menus and replay playback are unaffected.");
#ifndef MELEE_NO_SLIPPI
      static const char* online_choices[] = {"Auto", "1 frame", "2 frames", "3 frames", "4 frames",
                                             "5 frames", "6 frames", "7 frames", "8 frames", "9 frames"};
      int online = slippi::online::config().auto_delay ? 0 : slippi::online::config().delay;
      ImGui::SetNextItemWidth(180.0f);
      if (settings_combo("Online input delay", &online, online_choices, 10)) {
        slippi::online::config().auto_delay = online == 0;
        if (online > 0) slippi::online::config().delay = online;
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Applies from the next online match. Higher delay can reduce rollbacks.\n"
                          "Auto picks 2 to 6 frames from ping at match start and holds it for that match.\n"
                          "Without a ping measurement, Auto uses the last manual online value (%d frames).",
                          slippi::online::config().delay);
      if (online == 1)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                           "1 frame online can cause more rollbacks: fighters may jump or stutter.");
#endif
      ImGui::TextDisabled("Added input delay, not total controller-to-screen latency.");
    }

#ifndef MELEE_NO_SLIPPI
    settings_section("Online");
    {
      // Early inputs (host/early_input.h): the other player's game learns of a press up to a frame
      // sooner, so it guesses right more often and rolls back less. Only between two Melee Unlocked
      // players who both have it on; a Slippi Dolphin opponent is never sent anything new.
      if (settings_toggle("Early inputs", &options.early_inputs)) {
        host::early_input::set_enabled(options.early_inputs);
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends your presses and stick moves the moment they happen, between frames, so the\n"
                          "other player's game guesses them right more often and rolls back less.\n"
                          "Both players need a compatible client and must enable Early inputs. Slippi Dolphin\n"
                          "opponents see nothing new. Takes effect from the next online match.");
      // Slippi's quick chat on the online character select screen, the same three choices as
      // Slippi Dolphin. Off also stops showing the other player's messages.
      static const char* chat_choices[] = {"On", "Direct matches only", "Off"};
      int chat = std::clamp(slippi::online::config().chat, 0, 2);
      ImGui::SetNextItemWidth(220.0f);
      if (settings_combo("Quick chat", &chat, chat_choices, 3)) { slippi::online::config().chat = chat; changed = true; }
      if (slippi::online::is_online_match()) {
        ImGui::SameLine();
        settings_hint("(applies from the next match)");
      }
      bool direct_pause = slippi::online::config().direct_pause.load();
      if (settings_toggle("Pause in Direct Connect", &direct_pause)) {
        slippi::online::config().direct_pause.store(direct_pause);
        changed = true;
      }
      if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
        ImGui::SetTooltip("Applies the next time you connect. Either player can turn off pausing for both.\n"
                          "Matches with Slippi Dolphin or older clients keep pausing enabled.");
      {
        // Online port colour: first and second choice; "Any" leaves the port's own colour.
        static const char* const colours[] = {"Any", "Red", "Blue", "Yellow", "Green"};
        for (int k = 0; k < 2; ++k) {
          int pick = std::clamp(slippi::online::config().port_colour[k], -1, 3) + 1;
          ImGui::SetNextItemWidth(220.0f);
          if (settings_combo(k == 0 ? "Port color, first choice" : "Port color, second choice", &pick, colours, 5)) {
            slippi::online::config().port_colour[k] = pick - 1;
            changed = true;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Online 1v1: you show in your first choice (your tag and damage display). If the other player\n"
                              "picked the same color, a coin flip gives it to one of you and the other gets their second\n"
                              "choice. Both players need a client that supports port color choices. Applies from your next connection.");
        }
      }
      // Up to three connect codes to skip in Unranked (a match with one is turned down and the
      // search goes on). Ranked, Direct, Teams and Party are not affected.
      {
        {   // Screenshot tooling (MELEE_SETTINGS_SCROLL_TO=online): bring this row into view once.
          static int scroll_frames = 0;
          if (scroll_frames < 30) {
            const char* target = host::test_env::getenv("MELEE_SETTINGS_SCROLL_TO");
            if (target && std::strcmp(target, "online") == 0) ImGui::SetScrollHereY(0.3f);
            ++scroll_frames;
          }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Skip in Unranked");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Up to three connect codes you do not want to play in Unranked. When the search finds one,\n"
                            PRODUCT_NAME " turns the match down and keeps searching. Other modes are not affected.");
        for (int i = 0; i < 3; ++i) {
          char buf[17];
          std::snprintf(buf, sizeof buf, "%s", slippi::online::config().blocked_codes[i].c_str());
          ImGui::SameLine();
          ImGui::SetNextItemWidth(110.0f);
          char id[24]; std::snprintf(id, sizeof id, "##blockcode%d", i);
          if (settings_widgets::InputTextWithHint(id, "CODE#123", buf, sizeof buf, ImGuiInputTextFlags_CharsUppercase)) {
            slippi::online::config().blocked_codes[i] = buf;
            changed = true;
          }
        }
      }
      if (settings_toggle("After an online session ends, go back to Training mode", &options.online_return_training)) {
        RenderOptions::live_online_return_training() = options.online_return_training;
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When the other player leaves, you hold Z to leave, or the connection drops after a match,\n"
                          "the game goes to Training mode's character select instead of staying online.");
      // Slippi Dolphin's spectator server (its Slippi settings: Enable spectator, Spectator port),
      // read once at start as Dolphin does.
      ImGui::BeginDisabled(false);
      if (settings_toggle("Spectator server (Slippi Launcher broadcast, overlays, experimental)", &options.slippi_spectator)) {
        RenderOptions::live_slippi_spectator() = options.slippi_spectator;
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends this game to the Slippi Launcher on this PC the way Slippi Dolphin does:\n"
                          "its Broadcast tab (a tournament stream's code) and stream overlays read it.\n"
                          "Uses the same port as Slippi Dolphin, so close Slippi Dolphin while playing here.\n"
                          "Takes effect the next time the game starts.");
      ImGui::SameLine();
      int spectator_port = options.slippi_spectator_port;
      ImGui::SetNextItemWidth(110.0f);
      if (settings_widgets::InputInt("Port##spectator", &spectator_port, 0, 0)) {
        options.slippi_spectator_port = std::clamp(spectator_port, 1, 65535);
        RenderOptions::live_slippi_spectator_port() = options.slippi_spectator_port;
        changed = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("51441 is Slippi Dolphin's port, the one the Slippi Launcher looks at.");
      ImGui::EndDisabled();
      if (slippi::spectate::running())
        settings_hint(("Serving on port " + std::to_string(slippi::spectate::port()) + ".").c_str());
      else if (options.slippi_spectator)
        settings_hint("Not serving: starts with the game (see the log if the port is taken).");
    }
#endif

    // ---- Discord presence ----
    // Only in a build with its own Discord application (product_identity.h kDiscordAppId; none yet,
    // so none of this is shown). Nothing reaches Discord until the box below is ticked. See
    // scratchpad/discord_invite_design.md for the whole design.
    const bool discord_shown = host::discord::available();
    if (discord_shown) settings_section("Discord");
    const bool discord_was = options.discord_presence;
#ifdef MELEE_NO_SLIPPI   // presence only: this build publishes no code for Join to hand over
    if (discord_shown) settings_toggle("Discord presence (show what you are playing)", &options.discord_presence);
#else
    if (discord_shown) settings_toggle("Discord presence (show what you are playing; friends can press Join)", &options.discord_presence);
#endif
    if (options.discord_presence != discord_was) {
      changed = true;
      host::discord::enable(options.discord_presence);   // starts or stops one background thread
    }
    if (discord_shown && options.discord_presence) {
      ImGui::TextWrapped("%s", host::discord::status().c_str());
#ifdef MELEE_NO_SLIPPI
      ImGui::TextWrapped("Your IP address is never published. Who can see what you are playing depends on your Discord activity privacy settings.");
#else
      ImGui::TextWrapped("Discord Join sends the other player's code to Online > Direct; selecting Direct automatically uses it for that attempt. Your Slippi code is copied to the clipboard for the other player. Your IP address is never published. Who can see or join depends on your Discord activity privacy settings.");
#endif
    } else if (discord_shown) {
      settings_hint("Off. Nothing is sent to Discord.");
    }
    settings_section("Stages");
    if (settings_toggle("Freeze Final Destination", &options.freeze_fd)) {
      RenderOptions::live_freeze_fd() = options.freeze_fd;
      host::g_freeze_fd.store(options.freeze_fd, std::memory_order_relaxed);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Final Destination's background stays on its first form: no transformations and no white flash.\n"
                        "Also keeps a Final Destination skin's own background in view. Yours alone, online too.\n"
                        "Takes effect immediately.");
    // Slippi's Frozen Pokemon Stadium for offline play. It changes the match, so never online.
    ImGui::BeginDisabled(!options.native_source || host::gameplay::vanilla_only());
    if (settings_toggle("Freeze Pokemon Stadium (offline only)", &options.stadium_frozen)) {
      RenderOptions::live_stadium_frozen() = options.stadium_frozen;
      changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Pokemon Stadium stays on its base form all match, never transforming.\n"
                        "Offline only: it does not apply to online matches, which follow Slippi's own rules.\n"
                        "Replays record it and play back frozen. Source Port only.");
    ImGui::TextUnformatted("CPU players");
    ImGui::BeginDisabled(host::gameplay::vanilla_only());
    if (settings_toggle("20XX CPUs", &options.cpu_20xx)) {
      RenderOptions::live_cpu_20xx() = options.cpu_20xx;
      host::g_cpu_20xx.store(options.cpu_20xx, std::memory_order_relaxed);
      if (options.cpu_20xx && !options.native_source) host::hackpack_ai::reload();   // the disc may have arrived since boot
      changed = true;
    }
#ifdef MELEE_NO_SLIPPI   // one engine, no replays: the rest of the line is about the other build
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("CPUs play with the 20XX Hack Pack's AI. Offline matches only, never online.");
#else
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("CPUs play with the 20XX Hack Pack's AI. Offline matches only, never online or in replays.\n"
                        "Static Recomp: needs the 20XX Hack Pack disc under Mods (the Source Port has its own version).");
#endif
    if (!options.native_source && !host::hackpack_ai::status().blob_ok)
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "Install the 20XX Hack Pack under Mods");
    // 20XX Hack Pack training options for CPUs (Source Port, shim/mu_20xx_cpu.c): plain offline
    // options carried in option word 3. They work with the game's own CPUs and with "20XX CPUs".
    if (options.native_source) {
      bool cpu_changed = false;
      static const char* cpu_tech_names[] = {"Off", "In place", "Roll forward", "Roll back", "Miss", "Random"};
      cpu_changed |= settings_combo("CPU tech", &options.cpu_tech, cpu_tech_names, 6);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("How a CPU techs when it lands in tumble. Offline only.");
      static const char* cpu_getup_names[] = {"Off", "Stand", "Roll forward", "Roll back", "Getup attack", "Random"};
      cpu_changed |= settings_combo("CPU getup", &options.cpu_getup, cpu_getup_names, 6);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("What a CPU does from the floor after a missed tech. Offline only.");
      static const char* cpu_di_names[] = {"Off", "None", "Random", "Survival"};
      cpu_changed |= settings_combo("CPU DI", &options.cpu_di, cpu_di_names, 4);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which way a CPU holds the stick when it is launched. Offline only.");
      static const char* cpu_sdi_names[] = {"Off", "None", "Random", "With the hit", "Against the hit", "Up", "Down"};
      cpu_changed |= settings_combo("CPU smash DI", &options.cpu_sdi, cpu_sdi_names, 7);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which way a CPU shifts during hitlag. Offline only.");
      cpu_changed |= settings_toggle("CPUs never taunt", &options.cpu_no_taunt);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("CPUs never start a taunt. Offline only.");
      cpu_changed |= settings_toggle("CPUs always L-cancel", &options.cpu_lcancel);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every CPU aerial landing is L-canceled. Offline only.");
      cpu_changed |= settings_toggle("CPU Falcon: no rapid jab", &options.cpu_no_rapid_jab);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("A CPU Captain Falcon never starts his rapid jab. Offline only.");
      cpu_changed |= settings_toggle("CPU Zelda and Sheik: no transform", &options.cpu_no_transform);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("A CPU Zelda or Sheik never uses the down special. Offline only.");
      if (options.cpu_training_word() != 0) {
        ImGui::TextWrapped("CPU training overrides are on. They also affect Training CPUs set to Stand "
                           "and stay on when mod packs are disabled.");
        if (ImGui::Button("Reset CPU training options")) {
          options.cpu_tech = options.cpu_getup = options.cpu_di = options.cpu_sdi = 0;
          options.cpu_no_taunt = options.cpu_lcancel = options.cpu_no_rapid_jab = options.cpu_no_transform = false;
          cpu_changed = true;
        }
      }
      if (cpu_changed) {
        RenderOptions::live_cpu_training() = options.cpu_training_word();
        changed = true;
      }
    }
    ImGui::EndDisabled();

    // Slippi's Lagless FoD code is a real game patch, so expose it as an offline/direct setting
    // instead of silently forcing the performance-oriented variant on every player.
    settings_section("Stage effects");
    // Both engines. The Source Port reads it (MU_DISPLAY_OPTION_FOD_REFLECTIONS) as each Fountain of
    // Dreams match is set up. It was greyed there with "always Lagless FoD", so a player who had
    // turned it on (Static Recomp) kept the reflection on the Source Port with no way to turn it off.
    if (settings_toggle("Fountain of Dreams reflections", &options.fod_reflections)) changed = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(options.native_source
                          ? "On: keep Fountain of Dreams' water reflection and particles.\n"
                            "Off: use the Lagless FoD code for lower GPU cost. Offline; online always uses Lagless FoD,\n"
                            "as Slippi does. Applies from the next match on Fountain of Dreams."
                          : "On: keep Fountain of Dreams' water reflection and particles.\n"
                            "Off: use the Lagless FoD code for lower GPU cost. Takes effect at the next retrace.");
    // Both engines, every mode: it changes only how the menu background moves, never the game.
    if (settings_toggle("Character select background plays through", &options.css_background_free)) changed = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: the character select background plays its whole animation instead of starting\n"
                        "over every few seconds, so a custom background with a longer animation is not cut off.\n"
                        "Menu visuals only, so it also works online.");

    // ---- L-cancel helpers ----
    // The indicator reads the fighter's action state and never writes anything, so it is display
    // only and safe in every mode. The automatic press is a real analog trigger press injected into
    // the local pad before the game reads it, so it is transmitted like any other input and both
    // clients compute the same landing lag: it cannot desync. It is still gated to offline and
    // Direct because it is a fairness question, not a safety one.
    settings_section("L-cancel");   // its own section, not under Stage effects (owner, 0.9.210)
    {
      if (settings_lcancel_flash(options)) changed = true;
      const bool ranked = lcancel::automatic_setting_locked();
      bool automatic = ranked ? false : lcancel::automatic_enabled();
      ImGui::BeginDisabled(host::gameplay::vanilla_only() || ranked);
      if (settings_toggle("Auto L-cancel", &automatic)) {
        lcancel::set_automatic(automatic);
        host::log("settings: Auto L-cancel %s", automatic ? "on" : "off");
        changed = true;
      }
      // Disabled controls cannot take navigation focus. Log the lock once on entering it,
      // and on a mouse attempt, while preserving the saved preference and vanilla-only gate.
      static bool auto_ranked_seen = false;
      static int auto_ranked_frame = -2;
      if (ranked && (!auto_ranked_seen || auto_ranked_frame + 1 != ImGui::GetFrameCount() ||
          (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseClicked(0))))
        host::log("settings: Auto L-cancel off (locked in Ranked)");
      auto_ranked_seen = ranked;
      auto_ranked_frame = ImGui::GetFrameCount();
      ImGui::EndDisabled();
      if (ranked) settings_hint(lcancel::policy::ranked_hint);
      if (host::gameplay::vanilla_only()) settings_hint("Disabled for this vanilla-only session.");
      ImGui::SameLine();
#ifdef MELEE_NO_SLIPPI   // this build's matches are all the one kind, where it works
      settings_hint("(NOTE: Works offline and in online matches)");
      if (automatic) {
        ImGui::TextWrapped("Presses the analog trigger for you during an aerial. It is a real input, sent over the "
                           "network like any other, so it cannot desync. In an online match both players should agree "
                           "to use it: it is a fairness question, not a safety one.");
#else
      settings_hint("(NOTE: Works offline and in Direct; disabled in Ranked, Unranked, Teams and Party)");
      if (automatic) {
        ImGui::TextWrapped("Presses the analog trigger for you during an aerial. It is a real input, sent over the "
                           "network like any other, so it cannot desync. In a Direct match both players should agree "
                           "to use it: it is a fairness question, not a safety one.");
#endif
        if (const char* mode = lcancel::auto_suppressed_mode())
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Disabled right now: this is %s.", mode);
      }
    }

    // ---- HUD ----
    // A port code compiled into the game (recomp/gecko.py PORT_CODES): the stock row drawn at PAL's
    // size and height. Display only, read when the HUD is built, so it applies from the next match.
    // The native game has its own versions of both (MuHostApi.game_options), so both engines show them.
    settings_section("HUD");
    // The two size sliders rescale the HUD roots: in guest RAM on the Static Recomp, from the HUD's
    // draw callbacks in the native game (sourceport/game/shim/mu_hud_scale.c).
    {
      ImGui::SetNextItemWidth(170.0f);
      if (settings_slider("Stock icon size", &options.stock_hud_scale, 75, 175, "%d%%")) {
        gecko::option_pal_stock_icons = false;
        changed = true;
      }
      ImGui::SetNextItemWidth(170.0f);
      changed |= settings_slider("Damage number size", &options.damage_hud_scale, 75, 175, "%d%%");
      settings_hint("Display only; updates during play. Larger values may overlap in four-player matches.");
    }
    if (settings_toggle("PAL style stock icons", &gecko::option_pal_stock_icons)) {
      if (gecko::option_pal_stock_icons) options.stock_hud_scale = 85;
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Smaller stock icons, set a little higher, as in the PAL version.\n"
                        "Display only. Applies from the next match.");
    // A player asked for the P1 / P2 markers to stay up: the game shows them at the start of a match
    // and then only for a name tag or an off-screen fighter.
    {
      bool tags = options.player_tags_always != 0;
      if (settings_toggle("Always show player tags", &tags)) { options.player_tags_always = tags ? 1 : 0; changed = true; }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The P1, P2, P3 and P4 markers over the fighters stay up for the whole match.\n"
                          "Display only, so it is yours alone online. Takes effect immediately.");
    }
    {
      // Screenshot tooling (MELEE_SETTINGS_SCROLL_TO=screenshake): bring the shake settings into view once.
      static int scroll_frames = 0;
      if (scroll_frames < 30) {
        const char* target = host::test_env::getenv("MELEE_SETTINGS_SCROLL_TO");
        if (target && std::strcmp(target, "screenshake") == 0) ImGui::SetScrollHereY(0.3f);
        ++scroll_frames;
      }
    }
    // Also a port code: zeroes the camera's shake offset before the game applies it. With 20XX TE on
    // it is the same setting as TE's "Disable screen rumble" (te_pairs_linked).
    if (settings_toggle("Disable screen shake", &gecko::option_no_screen_shake)) {
      te_follow_ours(options, kTeNoScreenRumble, gecko::option_no_screen_shake);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The camera no longer shakes on hard hits, explosions and stage effects.\n"
                        "Camera only: fighters and hits are unchanged. Takes effect immediately.%s",
                        te_pair_hover(kTeNoScreenRumble));
    // Strength in between: the native camera scales its quake offset (camera.c, host API 27). Greyed
    // only over a running Static Recomp: the launcher's settings window has no game behind it, and a
    // Source Port player sets it there too (it was greyed for everyone in that window).
    if (!gecko::option_no_screen_shake) {
      if (settings_slider_int("Screen shake strength", &gecko::option_screen_shake_percent, 0, 200, "%d%%",
                           ImGuiSliderFlags_AlwaysClamp))
        changed = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("How strongly the camera shakes on hard hits, explosions and stage effects.\n"
                          "100%% is the game's own. Online it can lower the shake, not raise it past 100%%. Source Port only.");
    }
    // The explosion still runs, out of sight (ft_0D31.c, host.cpp apply_ko_explosions): its particles
    // draw random numbers, so leaving it out would change the match.
    if (settings_toggle("Hide KO explosions", &options.hide_ko_explosions)) {
      host::g_hide_ko_explosions.store(options.hide_ko_explosions, std::memory_order_relaxed);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("No burst of color when a fighter is KO'd off the side, top or bottom.\n"
                        "The match is exactly the same, so it also works online and in replays.\n"
                        "Picture only, yours alone. Both engines. Takes effect from the next KO.");
    {
      static const char* const kPercents[] = {"Shown", "Without emblems", "Hidden"};
      int percents = options.hide_percents ? 2 : options.hide_hud_emblems ? 1 : 0;
      if (settings_combo("Damage percents", &percents, kPercents, 3)) {
        options.hide_hud_emblems = percents == 1;
        options.hide_percents = percents == 2;
        host::g_hide_hud_emblems.store(options.hide_hud_emblems, std::memory_order_relaxed);
        gx::set_hide_percents(options.hide_percents);
        changed = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Without emblems keeps the damage numbers. Hidden removes the numbers and emblems.\n"
                          "This affects your display in matches and replays.");
    }

    // Community codes: players' favourite Gecko codes, built in (shim/mu_code_features.c). Picture and
    // sound only, so each is yours alone, online and in replays too.
    settings_section("Community codes");
    {
      struct Code { uint32_t bit; const char* name; const char* tip; };
      static const Code kCodes[] = {
        {MU_DISPLAY_OPTION_HUD_FADE, "HUD transparency",
         "A player's damage and stock icons turn see-through while a fighter is behind them."},
        {MU_DISPLAY_OPTION_NANA_PERCENT, "Show Nana's percent",
         "Nana's damage under each Ice Climbers player's percent."},
        {MU_DISPLAY_OPTION_ALLY_INDICATORS, "Ally indicators online",
         "Online: YOU over your fighter, and a heart over your teammate in teams."},
        {MU_DISPLAY_OPTION_ANGEL_STOCK_COLORS, "Respawn platform by stocks",
         "The respawn platform turns green with three stocks left, orange with two, red with one."},
        {MU_DISPLAY_OPTION_NO_GO_TEXT, "Hide GO! at match start",
         "No GO! graphic when a match starts. Its sound and timing are unchanged."},
        {MU_DISPLAY_OPTION_VCANCEL_SOUND, "V-cancel sound",
         "The menu's confirm sound plays on a successful V-cancel."},
        {MU_DISPLAY_OPTION_NO_FIGHTER_SHADOWS, "Hide fighter shadows",
         "Fighters cast no shadow on the stage (the No Character Shadows code)."},
        {MU_DISPLAY_OPTION_CENTER_2P_HUD, "Center the HUD for two players",
         "With only players 1 and 2 in a match, their percents sit in the middle of the screen\n"
         "(the Center Align 2P HUD code)."},
        {MU_DISPLAY_OPTION_HITBOX_ID_COLORS, "Hitbox colors by ID",
         "Shown hitboxes: ID 0 red, 1 green, 2 cyan, 3 purple."},
        {MU_DISPLAY_OPTION_HITBOX_ID_ORDER, "Highest-priority hitbox on top",
         "Shown hitboxes are drawn 3 to 0, so ID 0 is in front."},
        {MU_DISPLAY_OPTION_HITBOX_ANGLES, "Hitbox knockback angles",
         "Shown hitboxes carry a line along their knockback angle."},
        {MU_DISPLAY_OPTION_HQ_FOD_REFLECTIONS, "Full Fountain of Dreams reflections",
         "With Fountain of Dreams reflections on: the reflection at full resolution with full\n"
         "fighter models and live background scenery. Applies from the next match on the stage.\n"
         "Stage skins with a different reflection layout keep their own background artwork."},
      };
      for (const Code& c : kCodes) {
        bool on = (options.community_codes & c.bit) != 0;
        if (settings_toggle(c.name, &on)) {
          options.community_codes = on ? (options.community_codes | c.bit) : (options.community_codes & ~c.bit);
          host::g_community_codes.store(options.community_codes, std::memory_order_relaxed);
          changed = true;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\nPicture or sound only, yours alone. Source Port.", c.tip);
      }
      // Offline only: these change menu timing and the pause, so online they stay off.
      static const Code kOffline[] = {
        {MU_DISPLAY_OPTION_INSTANT_MELEE, "Instant Melee",
         "Faster stage, stage select and character select loads, and pausing during the\n"
         "match countdown."},
        {MU_DISPLAY_OPTION_FREE_PAUSE_CAMERA, "Unrestricted pause camera",
         "The pause camera turns all the way down, left and right and zooms much further."},
        {MU_DISPLAY_OPTION_BOOT_TO_CSS, "Boot to character select",
         "The game starts at the character select, past the title screen and menus\n"
         "(the Boot to CSS code)."},
      };
      for (const Code& c : kOffline) {
        bool on = (options.community_codes & c.bit) != 0;
        if (settings_toggle(c.name, &on)) {
          options.community_codes = on ? (options.community_codes | c.bit) : (options.community_codes & ~c.bit);
          host::g_community_codes.store(options.community_codes, std::memory_order_relaxed);
          changed = true;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s\nOffline only: off in online matches. Source Port (on the Static Recomp, add the\n"
                            "Gecko code itself).", c.tip);
      }
    }

}

}  // namespace gx
