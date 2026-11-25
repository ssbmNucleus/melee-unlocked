// The settings window's Controls: controller tiles and ports, bindings and profiles per device (keyboard, Xbox,
// PlayStation, GameCube adapter, Switch, box/HID), stick options and levels.
// A page of the settings window: settings_frame (pc_settings.cpp) calls it, and it draws with the
// window's shared helpers (pc_settings_internal.h). The values it edits are the settings store's
// (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// Controls page (tab 3).
void settings_page_controls(SettingsPageContext& page) {
  SettingsState& state = page.state;
  RenderOptions& options = page.options;
  bool& changed = page.changed;

    // First on the page, above the controller tiles, whatever controller is plugged in (owner: "its
    // not even in settings": it was one "Controller rumble" line, without a word of what it covers).
    // One switch for every controller that has a motor, off unless the player turns it on.
    bool rumble_enabled = host::g_rumble_enabled.load(std::memory_order_relaxed);
    if (settings_toggle("Rumble (controller vibration)", &rumble_enabled)) {
      host::g_rumble_enabled.store(rumble_enabled, std::memory_order_relaxed);
      if (!rumble_enabled) host::input_stop_all_rumble();
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Controller vibration (rumble), offline and online. Off by default.\n"
                        "GameCube controllers on an adapter and Xbox controllers have a motor; keyboards, boxes\n"
                        "and the other pads do not. Melee's own Options > Rumble (one per port) applies too.");
    if (!g_search)
      settings_hint(rumble_enabled ? "Rumble is on: GameCube controllers (adapter) and Xbox controllers vibrate, offline and online."
                                   : "Rumble is off for every controller, offline and online.");
    ImGui::Spacing();

    host::InputDebugSnapshot snap;
    host::input_debug_snapshot(snap);

    // Device numbers used by the profile row and the saved settings: 0 keyboard, 1-4 Xbox, 5-8
    // PlayStation, 9-12 adapter, 13-16 Switch, 17-20 box/HID.
    struct Family { const char* name; int fam; int first_tab, count; const char* device; };
    static const Family kFamilies[] = {
      {"GameCube", (int)host::PadFamily::GameCube, 9, 4, "Adapter port"},
      {"Xbox", (int)host::PadFamily::Xbox, 1, 4, "Xbox"},
      {"PlayStation", (int)host::PadFamily::PlayStation, 5, 4, "PlayStation"},
      {"Switch", (int)host::PadFamily::Switch, 13, 4, "Switch"},
      {"B0XX / Frame1 / Box", (int)host::PadFamily::Box, 17, 4, "Box"},
      {"Keyboard", -1, 0, 1, "Keyboard"},
    };
    constexpr int kFamilyCount = (int)(sizeof kFamilies / sizeof kFamilies[0]);
    auto tab_connected = [&](int t) {
      if (t == 0) return true;
      if (t <= 4) return snap.xinput_connected[t - 1];
      if (t <= 8) return snap.ds4_connected[t - 5];
      if (t <= 12) return (snap.gc_mask & (1u << (t - 9))) != 0;
      if (t <= 16) return snap.swpro_connected[t - 13];
      return snap.hid_connected[t - 17];
    };
    auto family_connected = [&](const Family& f) {
      int n = 0;
      if (f.fam < 0) return 0;
      for (int i = 0; i < f.count; ++i) n += tab_connected(f.first_tab + i) ? 1 : 0;
      return n;
    };
    auto tab_of_source = [](const host::PortSource& s) {
      switch (s.kind) {
        case host::DeviceKind::Keyboard: return 0;
        case host::DeviceKind::XInputPad: return 1 + std::clamp(s.index, 0, 3);
        case host::DeviceKind::DS4Pad: return 5 + std::clamp(s.index, 0, 3);
        case host::DeviceKind::GCAdapter: return 9 + std::clamp(s.index, 0, 3);
        case host::DeviceKind::SwitchPro: return 13 + std::clamp(s.index, 0, 3);
        case host::DeviceKind::HidPad: return 17 + std::clamp(s.index, 0, 3);
        default: return -1;
      }
    };
    static int family_sel = -1;
    static int device_sel[kFamilyCount] = {};
    auto select_tab = [&](int t) {
      for (int f = 0; f < kFamilyCount; ++f)
        if (t >= kFamilies[f].first_tab && t < kFamilies[f].first_tab + kFamilies[f].count) {
          family_sel = f; device_sel[f] = t - kFamilies[f].first_tab;
        }
    };
    // Each time the page is shown it opens on player 1's controller. After that only the player picks
    // what is edited (a player's card, a press on a controller, the not-playing row): nothing follows
    // port 1 on its own, so the page never jumps between pads.
    static int controls_shown_frame = -2;
    if (ImGui::GetFrameCount() > controls_shown_frame + 1 && state.rebind_action < 0) family_sel = -1;
    controls_shown_frame = ImGui::GetFrameCount();
    // Test runs (MELEE_TEST_PHOB): open on adapter socket 1, where the PhobGCC section is.
    static const bool phob_test = host::test_env::getenv("MELEE_TEST_PHOB") != nullptr;
    if (family_sel < 0) {   // open on whatever plays as port 1, else the first family with something connected
      family_sel = 0;
      for (int f = 0; f < kFamilyCount; ++f) if (family_connected(kFamilies[f]) > 0) { family_sel = f; break; }
      for (int f = 0; f < kFamilyCount; ++f)
        for (int i = 0; i < kFamilies[f].count; ++i) if (tab_connected(kFamilies[f].first_tab + i)) { device_sel[f] = i; break; }
      const int t = tab_of_source(snap.feeding[0]);
      if (t >= 0 && tab_connected(t)) select_tab(t);
      if (phob_test) select_tab(9);
      // Test hook for panel screenshots: MELEE_SETTINGS_EDIT=<device number> opens on that device.
      if (const char* e = host::test_env::getenv("MELEE_SETTINGS_EDIT")) select_tab(std::atoi(e));
    }
    auto light = [&](ImVec2 c, int count, bool show_count) {
      ImDrawList* d = ImGui::GetWindowDrawList();
      if (count > 0) {
        d->AddCircleFilled(c, 8, IM_COL32(60, 205, 95, 255), 20);
        if (show_count) {
          char n[8]; std::snprintf(n, sizeof n, "%d", count);
          const ImVec2 ts = ImGui::CalcTextSize(n);
          d->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(10, 40, 15, 255), n);
        }
      } else {
        d->AddCircle(c, 7, IM_COL32(110, 110, 120, 255), 20, 1.5f);
      }
    };
    auto chip = [&](const char* text, bool selected, bool show_light, int connected, bool show_count, float width) {
      ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.45f, 0.22f, 0.75f, 1.0f) : ImVec4(0.16f, 0.16f, 0.2f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? ImVec4(0.52f, 0.28f, 0.82f, 1.0f) : ImVec4(0.24f, 0.24f, 0.3f, 1.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
      const std::string padded = std::string("  ") + text;
      const bool pressed = settings_widgets::Button(padded.c_str(), ImVec2(width, 28));
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
      if (show_light) {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        light(ImVec2(mx.x - 15, (mn.y + mx.y) / 2), connected, show_count);
      }
      return pressed;
    };

    auto port_of_tab = [&](int t) {
      for (int port = 0; port < 4; ++port) if (tab_of_source(snap.feeding[port]) == t) return port + 1;
      return 0;
    };
    // A press on any controller (not the keyboard) selects it, so a player never has to work out
    // which adapter socket or pad number theirs is.
    // One controller can be listed twice (Steam's Xbox copy of a PlayStation pad, a box as HID and as
    // XInput). A pad that plays as no port, pressed within a few frames of a pad that does, is marked
    // as a likely copy: the one that plays stays selected, and the copy is not offered a port.
    static bool likely_copy[kDeviceTabs] = {};
    {
      // A stick pushed well off centre counts as well as a button, so moving a stick to watch it
      // in the picture shows that controller, whichever socket or pad number it is on.
      static uint32_t last_live[kDeviceTabs] = {};
      static bool last_pushed[kDeviceTabs] = {};
      static int pressed_frame[kDeviceTabs] = {};
      constexpr int kCopyFrames = 8;   // presses this close together are one press on one pad
      const int frame = ImGui::GetFrameCount();
      int pick = -1;
      for (int t = 1; t < kDeviceTabs; ++t) {
        int idx; const host::CaptureDevice kind = tab_kind_of(t, &idx);
        const uint32_t now_live = tab_connected(t) ? live_actions(snap, kind, idx) : 0;
        const host::PadState& ps = t <= 4 ? snap.xinput_pad[t - 1] : t <= 8 ? snap.ds4_pad[t - 5] : t <= 12 ? snap.gc_pad[t - 9]
                                 : t <= 16 ? snap.swpro_pad[t - 13] : snap.hid_pad[t - 17];
        auto far_out = [](int8_t v) { return v > 50 || v < -50; };
        const bool pushed = tab_connected(t) && (far_out(ps.stick_x) || far_out(ps.stick_y) || far_out(ps.sub_x) || far_out(ps.sub_y));
        const bool fresh = (now_live & ~last_live[t]) || (pushed && !last_pushed[t]);
        last_live[t] = now_live;
        last_pushed[t] = pushed;
        if (!fresh) continue;
        pressed_frame[t] = frame;
        if (pick < 0 || (port_of_tab(t) && !port_of_tab(pick))) pick = t;
      }
      if (pick >= 0) {
        // Judge again every pad that plays as no port and was pressed lately, whichever came first.
        for (int t = 1; t < kDeviceTabs; ++t) {
          if (port_of_tab(t) || pressed_frame[t] == 0 || frame - pressed_frame[t] > kCopyFrames) continue;
          bool with_player = false;
          for (int u = 1; u < kDeviceTabs; ++u)
            if (u != t && port_of_tab(u) && pressed_frame[u] != 0 && std::abs(pressed_frame[u] - pressed_frame[t]) <= kCopyFrames)
              with_player = true;
          likely_copy[t] = with_player;
        }
        if (state.rebind_action < 0 && !(likely_copy[pick] && !port_of_tab(pick))) select_tab(pick);
      }
    }
    // What a player calls each device: the kind of controller, then where it is plugged in.
    auto device_title = [&](int t) -> std::string {
      if (t < 0) return "Nobody";
      if (t == 0) return "Keyboard";
      if (t <= 4) return "Xbox controller";
      if (t <= 8) return "PlayStation controller";
      if (t <= 12) return "GameCube controller";
      if (t <= 16) return "Switch controller";
      return "B0XX / Frame1 / box";
    };
    auto device_where = [&](int t) -> std::string {
      char w[96];
      if (t <= 0) return t == 0 ? "" : "No controller";
      if (t <= 4) std::snprintf(w, sizeof w, "Pad %d", t);
      else if (t <= 8) std::snprintf(w, sizeof w, "Pad %d", t - 4);
      else if (t <= 12) std::snprintf(w, sizeof w, "Adapter socket %d", t - 8);
      else if (t <= 16) std::snprintf(w, sizeof w, "Pad %d", t - 12);
      else { const std::string n = host::hidpad_name(t - 17); if (n.empty()) std::snprintf(w, sizeof w, "Box %d", t - 16); else std::snprintf(w, sizeof w, "%s", n.c_str()); }
      return w;
    };
    auto device_short = [&](int t) -> std::string {
      if (t < 0) return "Nobody";
      if (t == 0) return "Keyboard";
      if (t <= 4) return "Xbox";
      if (t <= 8) return "PlayStation";
      if (t <= 12) return "GameCube";
      if (t <= 16) return "Switch";
      return "Box";
    };
    const int edit_tab = kFamilies[family_sel].first_tab + device_sel[family_sel];
    auto source_of_tab = [](int t) -> host::PortSource {
      if (t == 0) return {host::DeviceKind::Keyboard, 0};
      if (t <= 4) return {host::DeviceKind::XInputPad, t - 1};
      if (t <= 8) return {host::DeviceKind::DS4Pad, t - 5};
      if (t <= 12) return {host::DeviceKind::GCAdapter, t - 9};
      if (t <= 16) return {host::DeviceKind::SwitchPro, t - 13};
      return {host::DeviceKind::HidPad, t - 17};
    };
    auto set_port_source = [&](int port, const host::PortSource& src) {
      host::g_port_sources[port] = src;
      host::g_port_device_names[port] = src.kind == host::DeviceKind::HidPad ? host::port_device_key(host::hidpad_name(src.index)) : std::string();
      changed = true;
    };

    // ---- Players: the game's four ports. Each card says what plays as that port; clicking a card
    // edits that controller, and its Change button picks what plays there. ----
    {
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      const int card_columns = ImGui::GetContentRegionAvail().x < 600.0f ? 2 : 4;
      const float card_w = (ImGui::GetContentRegionAvail().x - (card_columns - 1) * gap) / card_columns;
      constexpr float card_h = 70.0f;
      for (int port = 0; port < 4; ++port) {
        if (port % card_columns) ImGui::SameLine();
        ImGui::PushID(100 + port);
        const int t = tab_of_source(snap.feeding[port]);
        const bool connected = t >= 0 && tab_connected(t);
        const bool editing = t >= 0 && t == edit_tab;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap();   // the arrow drawn on top of the card takes its own clicks
        if (settings_hit_button("card", ImVec2(card_w, card_h)) && t >= 0) select_tab(t);
        const bool hovered_card = ImGui::IsItemHovered();
        ImDrawList* d = ImGui::GetWindowDrawList();
        d->AddRectFilled(o, ImVec2(o.x + card_w, o.y + card_h), hovered_card ? IM_COL32(40, 40, 52, 255) : IM_COL32(30, 30, 40, 255), 8.0f);
        d->AddRect(o, ImVec2(o.x + card_w, o.y + card_h), editing ? IM_COL32(150, 90, 240, 255) : IM_COL32(60, 60, 74, 255), 8.0f, 0, editing ? 2.5f : 1.0f);
        // Port badge: green when its controller is plugged in, grey when not.
        const ImU32 col = connected ? IM_COL32(52, 176, 82, 255) : IM_COL32(80, 80, 90, 255);
        // Stacked so a narrow panel (the default size in game) still shows every word: the badge and
        // the arrow on the top row, then the kind of controller and where it is plugged in.
        d->AddRectFilled(ImVec2(o.x + 8, o.y + 8), ImVec2(o.x + 46, o.y + 30), col, 5.0f);
        char badge[4]; std::snprintf(badge, sizeof badge, "P%d", port + 1);
        const ImVec2 bs = ImGui::CalcTextSize(badge);
        d->AddText(ImVec2(o.x + 27 - bs.x / 2, o.y + 19 - bs.y / 2), IM_COL32(255, 255, 255, 255), badge);
        d->PushClipRect(ImVec2(o.x + 6, o.y), ImVec2(o.x + card_w - 6, o.y + card_h), true);
        d->AddText(ImVec2(o.x + 10, o.y + 34), connected ? IM_COL32(235, 235, 240, 255) : IM_COL32(140, 140, 150, 255), device_short(t).c_str());
        std::string where = t < 0 ? std::string("Empty") : device_where(t);
        if (where.rfind("Adapter socket", 0) == 0) where = "Socket" + where.substr(14);
        d->AddText(ImVec2(o.x + 10, o.y + 51), IM_COL32(140, 140, 155, 255), where.c_str());
        d->PopClipRect();
        // What plays as this port: a labelled button with an arrow, so it reads as the place to change it.
        {
          const float pick_w = ImGui::CalcTextSize("Change").x + 30.0f;
          ImGui::SetCursorScreenPos(ImVec2(o.x + card_w - pick_w - 8, o.y + 7));
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.17f, 0.50f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.22f, 0.75f, 1.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
          ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
          if (settings_widgets::Button("Change##port_pick", ImVec2(pick_w, 24.0f))) ImGui::OpenPopup("port_source");
          ImGui::PopStyleVar(3);
          ImGui::PopStyleColor(2);
          const ImVec2 pn = ImGui::GetItemRectMin(), px = ImGui::GetItemRectMax();
          const float ax = px.x - 12.0f, ay = (pn.y + px.y) / 2;
          d->AddTriangleFilled(ImVec2(ax - 4, ay - 2), ImVec2(ax + 4, ay - 2), ImVec2(ax, ay + 3), IM_COL32(235, 235, 240, 255));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose what plays as port %d.", port + 1);
        else if (hovered_card)
          ImGui::SetTooltip("%s%s%s\nClick to set up this controller's buttons.", device_title(t).c_str(), t > 0 ? ", " : "",
                            t > 0 ? (device_where(t) + (connected ? "" : " (unplugged)")).c_str() : "");
        if (ImGui::BeginPopup("port_source")) {
          settings_hint("Plays as port %d", port + 1);
          ImGui::Separator();
          const int cur = port_source_to_combo(host::g_port_sources[port]);
          // Plugged in first; the rest under "Not plugged in", so it can be set up ahead of time.
          auto source_item = [&](int c) {
            const host::PortSource src = combo_to_port_source(c);
            const int st = tab_of_source(src);
            std::string item = c == 0 ? std::string("Nobody") : st == 0 ? std::string("Keyboard") : device_title(st) + "  |  " + device_where(st);
            if (st > 0 && !tab_connected(st)) item += "  (not plugged in)";
            item += "##src" + std::to_string(c);   // unnamed boxes share a label
            if (settings_widgets::Selectable(item.c_str(), c == cur)) {
              set_port_source(port, src);
              if (st >= 0) select_tab(st);
            }
          };
          for (int c = 0; c < kPortSourceCount; ++c) {
            const int st = tab_of_source(combo_to_port_source(c));
            if (c == 0 || st == 0 || tab_connected(st)) source_item(c);
          }
          if (ImGui::BeginMenu("Not plugged in")) {
            for (int c = 0; c < kPortSourceCount; ++c) {
              const int st = tab_of_source(combo_to_port_source(c));
              if (st > 0 && !tab_connected(st)) source_item(c);
            }
            ImGui::EndMenu();
          }
          ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(o);   // the card as one item, so the next sits beside it
        ImGui::Dummy(ImVec2(card_w, card_h));
        ImGui::PopID();
      }
    }
    ImGui::Spacing();

    settings_hint("Click a player, or press a button on a controller, to set up its buttons.");

    // ---- Plugged in but not playing: every plugged-in controller that plays as no port (and the
    // keyboard when it does not play), so each can still be set up. A pad gets one click to play, on
    // the first port with nothing plugged in. Ports 2 to 4 never take a spare pad on their own
    // (input_bindings.h open_port_for_pad): a pad listed twice would become a copied player. ----
    {
      static const int kOrder[] = {9, 10, 11, 12, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 20, 0};
      const int open_port = host::open_port_for_pad(snap.feeding, host::input_device_facts(snap));
      int idle[kDeviceTabs], idle_count = 0;
      bool idle_pad = false;
      for (int t : kOrder)
        if (tab_connected(t) && !port_of_tab(t)) { idle[idle_count++] = t; idle_pad |= t > 0; }
      if (idle_count > 0) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Plugged in but not playing");
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        char play[24];
        std::snprintf(play, sizeof play, "Play as P%d", open_port + 1);
        const float play_w = ImGui::CalcTextSize("Play as P4").x + 28.0f;
        for (int i = 0; i < idle_count; ++i) {
          const int t = idle[i];
          ImGui::PushID(300 + t);
          std::string label = device_title(t);
          const std::string where = device_where(t);
          if (!where.empty()) label += "  |  " + where;
          const bool offer = t > 0 && open_port >= 0;
          const float row_w = ImGui::GetContentRegionAvail().x;
          if (chip(label.c_str(), t == edit_tab, false, 0, false, std::max(120.0f, offer ? row_w - play_w - gap : row_w)))
            select_tab(t);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to set up this controller's buttons.");
          if (offer) {
            ImGui::SameLine();
            ImGui::BeginDisabled(likely_copy[t]);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.66f, 0.36f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            if (settings_widgets::Button(play, ImVec2(play_w, 28))) {
              set_port_source(open_port, source_of_tab(t));
              select_tab(t);
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("This controller plays as player %d, where nothing is plugged in.\n"
                                "Change it any time with Change on the player's card.", open_port + 1);
          }
          if (t > 0 && likely_copy[t])
            settings_hint("Its presses came with a player's controller: probably the same controller listed twice "
                          "(by Steam, or a box in two modes), so it is not offered a port.");
          ImGui::PopID();
        }
        if (idle_pad && open_port < 0)
          settings_hint("All four players have a controller. Use Change on a player's card to swap one in.");
      }
    }
    if (!tab_connected(edit_tab))
      settings_hint("%s%s%s is not plugged in. Its buttons can still be set up.", device_title(edit_tab).c_str(),
                    edit_tab > 0 ? ", " : "", device_where(edit_tab).c_str());
    // Every other controller, to set one up before plugging it in.
    if (settings_widgets::SmallButton("Set up a controller that is not plugged in")) ImGui::OpenPopup("not_plugged_in");
    if (ImGui::BeginPopup("not_plugged_in")) {
      static const int kOrder[] = {9, 10, 11, 12, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 20};
      for (int t : kOrder)
        if (!tab_connected(t)) {
          const std::string item = device_title(t) + "  |  " + device_where(t) + "##dev" + std::to_string(t);
          if (settings_widgets::Selectable(item.c_str(), t == edit_tab)) select_tab(t);
        }
      ImGui::EndPopup();
    }
    const Family& cur_family = kFamilies[family_sel];
    ImGui::Spacing();
    // On every tab: with the adapter taken by Steam, port 1 falls back to Steam's Xbox copy of the
    // controller, so the player is usually looking at the Xbox tab when this matters.
    for (const std::string& problem : {host::gcadapter_problem(), host::switch_pad_problem()}) {
      if (problem.empty()) continue;
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
      ImGui::TextWrapped("%s", problem.c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }
    if (cur_family.fam == (int)host::PadFamily::Box)
      settings_hint("Mod X and Mod Y work inside the box itself and are never bound here. A Frame1 plugged in "
                    "normally shows up as a GameCube adapter: set it up under GameCube.");
    if (cur_family.fam == (int)host::PadFamily::GameCube) {
      const double rate = host::gcadapter_poll_rate_hz();
      if (rate > 0.0) ImGui::Text("Adapter polling rate: %.0f Hz", rate);
      else settings_hint("Adapter polling rate: --");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Incoming USB reports per second, shared by all four adapter sockets.\nThe game reads controller state once per frame.");
      {
        const bool resetting = host::gcadapter_reset_pending();
        ImGui::BeginDisabled(resetting);
        if (settings_widgets::Button(resetting ? "Resetting adapter..." : "Reset GameCube adapter"))
          host::gcadapter_request_reset();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Reopens the USB connection and recalibrates all four sockets.\nRelease buttons, sticks and triggers first. Use if inputs stick or the polling rate drops.");
      }
      ImGui::Spacing();
    }

    {
      const int tab = cur_family.first_tab + device_sel[family_sel];
      {
        host::CaptureDevice tab_kind;
        int tab_index = 0;
        if (tab == 0) { tab_kind = host::CaptureDevice::Keyboard; }
        else if (tab <= 4) { tab_kind = host::CaptureDevice::XInputPad; tab_index = tab - 1; }
        else if (tab <= 8) { tab_kind = host::CaptureDevice::DS4Pad; tab_index = tab - 5; }
        else if (tab <= 12) { tab_kind = host::CaptureDevice::GCAdapter; tab_index = tab - 9; }
        else if (tab <= 16) { tab_kind = host::CaptureDevice::SwitchPro; tab_index = tab - 13; }
        else { tab_kind = host::CaptureDevice::HidPad; tab_index = tab - 17; }

        // A rebind in progress for this device: wait for its next press.
        const bool capturing_here = state.rebind_action >= 0 && state.rebind_kind == tab_kind && state.rebind_index == tab_index;
        if (capturing_here) {
          host::CaptureDevice dev; int value; int device_index;
          if (host::input_poll_capture(dev, value, device_index)) {
            if (dev == host::CaptureDevice::None) {
              state.rebind_action = -1;   // Escape cancelled; binding unchanged.
            } else if (dev == tab_kind) {
              // Whichever controller of this kind answered is the one being set up: the player
              // need not know its adapter socket or pad number.
              const int idx = tab_kind == host::CaptureDevice::Keyboard ? 0 : device_index;
              // An Xbox trigger given to another action leaves its default shoulder bindings (xinput_bind).
              if (tab_kind == host::CaptureDevice::XInputPad) host::xinput_bind(host::g_pad_bindings[idx & 3], state.rebind_action, (uint16_t)value);
              else binding_set(tab_kind, idx, state.rebind_action, (uint32_t)value);
              state.rebind_action = -1;
              changed = true;
              if (idx != tab_index) select_tab(tab - tab_index + idx);
            } else {
              host::input_begin_capture(tab_kind, tab_index);
            }
          }
        }
        auto begin_rebind = [&](int action) {
          host::input_begin_capture(tab_kind, tab_index);
          state.rebind_action = action; state.rebind_kind = tab_kind; state.rebind_index = tab_index;
        };
        auto unbind = [&](int action) { binding_set(tab_kind, tab_index, action, 0); changed = true; };

        host::profiles_set_folder(options.settings_path);
        changed |= draw_profile_row(tab_kind, tab_index, tab);
        ImGui::Spacing();

        // The picture: one C-stick box, as Smash Ultimate has it.
        // Boxes and pads send their C-stick as a stick (a box computes it, modifier angles included,
        // in its own firmware), so only the keyboard binds the four directions; two keys held
        // together give the diagonals. A pad that really sends C directions as buttons can still
        // bind them in the list below.
        // The Box tab shows them too: a box in DInput or keyboard-style mode can send its C-stick
        // and stick as plain buttons, and its player looked for them in the picture, not the list.
        const bool analog_c = tab_kind != host::CaptureDevice::Keyboard && tab_kind != host::CaptureDevice::HidPad;
        const int waiting = state.rebind_kind == tab_kind && state.rebind_index == tab_index ? state.rebind_action : -1;
        const uint32_t live = live_actions(snap, tab_kind, tab_index);
        int right_clicked = -1, hovered = -1;
        ImVec2 stick_pos(0, 0), c_pos(0, 0);
        {
          const host::PadState& ps = tab == 0 ? snap.keyboard_pad : tab <= 4 ? snap.xinput_pad[tab - 1] : tab <= 8 ? snap.ds4_pad[tab - 5]
                                   : tab <= 12 ? snap.gc_pad[tab - 9] : tab <= 16 ? snap.swpro_pad[tab - 13] : snap.hid_pad[tab - 17];
          stick_pos = ImVec2(std::clamp(ps.stick_x / 80.0f, -1.0f, 1.0f), std::clamp(ps.stick_y / 80.0f, -1.0f, 1.0f));
          c_pos = ImVec2(std::clamp(ps.sub_x / 80.0f, -1.0f, 1.0f), std::clamp(ps.sub_y / 80.0f, -1.0f, 1.0f));
        }
        const bool switch_picture = tab_kind == host::CaptureDevice::SwitchPro && !settings_store::panel_values().swpro_gc_picture;
        // This family's deadzones, over 80 like the stick positions (the keyboard has none).
        const float dz_main = cur_family.fam >= 0 ? host::g_deadzones[(size_t)cur_family.fam].main / 80.0f : 0.0f;
        const float dz_c = cur_family.fam >= 0 ? host::g_deadzones[(size_t)cur_family.fam].c / 80.0f : 0.0f;
        if (tab_kind == host::CaptureDevice::SwitchPro) {
          if (settings_toggle("Use GameCube controller picture", &settings_store::panel_values().swpro_gc_picture)) changed = true;
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set up this Switch controller on the GameCube layout instead:\nclick a GameCube button, then press the Switch button for it.");
        }
        if (switch_picture) {
          changed |= draw_swpro_bind_picture(tab_index, snap.swpro_buttons[tab_index], stick_pos, c_pos, dz_main, dz_c);
          if (tab_connected(tab)) settings_hint("Click a button or its box to choose what it does. Right-click to clear.");
        }
        const int clicked = switch_picture ? -1 : draw_gc_bind_picture(live, waiting, &right_clicked, &hovered,
                                                 [&](int action) { return binding_label(tab_kind, tab_index, action); },
                                                 analog_c, "Smash Attack", stick_pos, c_pos, dz_main, dz_c);
        if (clicked >= 0 && clicked != kCStickModeBox) begin_rebind(clicked);
        if (right_clicked >= 0) unbind(right_clicked);
        if (hovered == kCStickModeBox)
          ImGui::SetTooltip("C-Stick: smash attacks.");
        else if (hovered >= 0)
          ImGui::SetTooltip("%s: %s\nClick to rebind, right-click to clear.", kActionTitles[hovered], binding_label(tab_kind, tab_index, hovered).c_str());
        if (waiting >= 0)
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key or button for %s (Esc to cancel)", kActionTitles[waiting]);
        else if (!tab_connected(tab))
          settings_hint("Not connected. Plug it in to rebind it; its saved buttons are shown.");
        else if (!switch_picture)
          settings_hint("Click a button or its box to rebind it. Right-click to clear.");

        // Any change to this controller's buttons (a rebind, a clear, a box layout, a loaded
        // profile) is kept in its profile straight away.
        {
          static host::ProfileBindings last_seen[kDeviceTabs];
          static bool seen[kDeviceTabs] = {};
          host::profiles_set_folder(options.settings_path);
          const host::ProfileBindings now_b = bindings_of(tab);
          if (seen[tab] && now_b != last_seen[tab]) autosave_profile(tab);
          last_seen[tab] = now_b;
          seen[tab] = true;
        }

        // The stick modifier's push, for the devices whose stick can be made of buttons.
        if (cur_family.fam < 0 || cur_family.fam == (int)host::PadFamily::Box) {
          ImGui::Spacing();
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("Stick modifier push", &host::g_stick_modifier_percent, 10, 100, "%d%%");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While the Stick modifier is held, the stick directions bound to buttons push only\n"
                              "this far, like Dolphin's Modifier: 50%% walks. With Mod X or Mod Y bound the box\n"
                              "coordinates apply instead.");
        }

        // ---- the rest, folded away ----
        if (cur_family.fam >= 0) {
          ImGui::Spacing();
          ImGui::SeparatorText("Sticks");
          host::Deadzone& dz = host::g_deadzones[(size_t)cur_family.fam];
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("Control stick deadzone", &dz.main, 0, 60, dz.main ? "%d" : "Off");
          if (ImGui::IsItemActive()) g_show_dz_main_until = ImGui::GetTime() + 1.5;
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Inside this distance from center the stick reads as centered; outside it the stick\n"
                              "is passed through exactly as the controller sends it (out of 127). Melee already\n"
                              "ignores about 23, so only a worn stick drifting past that needs it.");
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("C-stick deadzone", &dz.c, 0, 60, dz.c ? "%d" : "Off");
          if (ImGui::IsItemActive()) g_show_dz_c_until = ImGui::GetTime() + 1.5;
          static const char* kRadiusTip =
              "How far the stick reaches at a full push, like Dolphin's Radius. 100 leaves the\n"
              "stick exactly as the controller sends it. Lower it if the stick hits full speed\n"
              "before you have pushed it all the way: at 80 a full push gives 80% of the range.";
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("Control stick radius", &dz.radius_main, 50, 100, dz.radius_main >= 100 ? "Full" : "%d%%");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kRadiusTip);
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("C-stick radius", &dz.radius_c, 50, 100, dz.radius_c >= 100 ? "Full" : "%d%%");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kRadiusTip);
          if (cur_family.fam != (int)host::PadFamily::GameCube && cur_family.fam != (int)host::PadFamily::Box) {
            // The snapback filter for these sticks (a GameCube controller has PhobGCC's in the Phob section).
            static const char* kSnapTip =
                "Stops the stick reading on the other side for a frame or two after you let go of it\n"
                "(snapback). Right after a hard release, a reading on the other side up to this size\n"
                "reads as center, for this long; a full reversal (dash dance) passes at once.\n"
                "Size is out of the stick's 127 (60 is the edge); 0 is off. Try 40 and 20 ms.";
            for (int axis = 0; axis < 2; ++axis) {
              int& size = axis ? dz.snapback_y : dz.snapback_x;
              int& ms = axis ? dz.snapback_ms_y : dz.snapback_ms_x;
              ImGui::PushID(axis);
              ImGui::SetNextItemWidth(200.0f);
              changed |= settings_slider(axis ? "Snapback filter Y" : "Snapback filter X", &size, 0, host::kSnapbackEdge, size ? "%d" : "Off");
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSnapTip);
              ImGui::SameLine();
              ImGui::BeginDisabled(size == 0);
              ImGui::SetNextItemWidth(160.0f);
              changed |= settings_slider(axis ? "for##y" : "for##x", &ms, host::kSnapbackMinMs, host::kSnapbackMaxMs, "%d ms");
              ImGui::EndDisabled();
              if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", kSnapTip);
              ImGui::PopID();
            }
          }
          ImGui::SeparatorText("Triggers");
          static const char* kTrigTip =
              "Full: the trigger works as on a GameCube controller (a full press clicks and gives\n"
              "a full shield).\n"
              "A number: the trigger is analog only, like Dolphin's L-Analog / R-Analog setting. Its\n"
              "value stops there and it never clicks, so a digital or hair trigger gives a light\n"
              "shield. Melee shields lightly from 43 (lightest) to 140 (hardest).\n"
              "For a full press as well (full shield, L+R+A+Start), bind L or R to a button such as\n"
              "a bumper: the trigger keeps its light shield and the button gives the full press.";
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("L trigger", &dz.trig_l, 43, 255, dz.trig_l >= 255 ? "Full" : "%d");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTrigTip);
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("R trigger", &dz.trig_r, 43, 255, dz.trig_r >= 255 ? "Full" : "%d");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTrigTip);
          static const char* kClickTip =
              "How far the trigger travels (out of 255) before the game sees the full press.\n"
              "Lower it for a trigger with short travel. 200 is the default. It has no effect\n"
              "while the trigger above is set to a number, since that trigger never clicks.";
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("L full press at", &dz.click_l, 1, 254, dz.click_l == host::kDefaultTriggerClick ? "Default" : "%d");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kClickTip);
          ImGui::SetNextItemWidth(200.0f);
          changed |= settings_slider("R full press at", &dz.click_r, 1, 254, dz.click_r == host::kDefaultTriggerClick ? "Default" : "%d");
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kClickTip);
          settings_hint("Applies to every %s controller.", cur_family.name);
        }
        // Each binding's own analog level, only for the bindings that have one: a trigger bound
        // as a button (its press point) and a button bound to L analog or R analog (how far it
        // presses). Never the D-pad or Start (binding_level_kind).
        {
          bool any = false;
          for (int i = 0; i < (int)host::BindAction::Count; ++i) {
            if (!binding_level_kind(tab_kind, tab_index, i)) continue;
            if (!any) { ImGui::Spacing(); ImGui::SeparatorText("Per button"); any = true; }
            changed |= binding_level_slider(tab_kind, tab_index, i);
          }
        }
        // Every controller on the adapter, PhobGCC or not: the PhobScope screens on their own.
        if (tab_kind == host::CaptureDevice::GCAdapter) {
          ImGui::Spacing();
          ImGui::PushID("controller_test");
          draw_controller_test(tab_index);
          ImGui::PopID();
        }
        if (tab_kind == host::CaptureDevice::GCAdapter && settings_widgets::CollapsingHeader("PhobGCC")) {
          ImGui::PushID("phob");
          draw_phob_menu(tab_index);
          ImGui::PopID();
        }
        if (tab_kind == host::CaptureDevice::HidPad && settings_widgets::CollapsingHeader("Box layouts")) {
          if (settings_widgets::Button("B0XX (vJoy / b0xx-ahk)")) { host::g_hid_bindings[tab_index] = host::vjoy_b0xx_bindings(); changed = true; }
          ImGui::SameLine();
          if (settings_widgets::Button("B0XX-layout box (HayBox)")) { host::g_hid_bindings[tab_index] = host::haybox_dinput_bindings(); changed = true; }
          ImGui::SameLine();
          if (settings_widgets::Button("Generic")) { host::g_hid_bindings[tab_index] = host::default_hid_bindings()[0]; changed = true; }
          settings_hint("vJoy and HayBox boxes in DInput mode get their layout automatically.");
          // Invert a reported direction independently of the axis assigned to it.
          {
            static const char* const names[4] = {"Invert stick left/right", "Invert stick up/down",
                                                 "Invert C-stick left/right", "Invert C-stick up/down"};
            for (int k = 0; k < 4; ++k) {
              auto& invert = host::g_hid_stick_invert[tab_index];
              const uint8_t mask = invert.load(std::memory_order_relaxed);
              bool on = (mask >> k) & 1;
              if (k % 2) ImGui::SameLine();
              if (settings_toggle(names[k], &on)) {
                invert.store((uint8_t)((mask & ~(1u << k)) | (on ? 1u << k : 0u)), std::memory_order_relaxed);
                changed = true;
              }
            }
          }
          if (!host::hidpad_frame1_dinput(tab_index)) {
            static const char* const names[host::HID_ROLE_COUNT] = {
                "Stick left/right", "Stick up/down", "C-stick left/right", "C-stick up/down", "L trigger", "R trigger"};
            static const char* const prompts[host::HID_ROLE_COUNT] = {
                "Push the stick right...", "Push the stick up...", "Push the C-stick right...", "Push the C-stick up...",
                "Press L or its light shield...", "Press R or its light shield..."};
            int& capturing = state.hid_axis_role;
            const int frame = ImGui::GetFrameCount();
            if (state.hid_axis_slot != tab_index || state.hid_axis_frame != frame - 1 ||
                ImGui::GetTime() > state.hid_axis_until)
              capturing = -1;
            state.hid_axis_frame = frame;
            if (capturing >= 0 && host::hidpad_axis_capture_bind(tab_index, capturing)) {
              capturing = -1;
              changed = true;
            }
            auto roles = host::hidpad_axis_roles(tab_index);
            ImGui::Spacing();
            for (int role = 0; role < host::HID_ROLE_COUNT; ++role) {
              const uint8_t axis = roles[role];
              char label[96];
              if (role == capturing)
                std::snprintf(label, sizeof label, "%s##axis%d", prompts[role], role);
              else
                std::snprintf(label, sizeof label, "%s: %s##axis%d", names[role],
                              axis == host::kHidAxisAuto ? "Automatic" : host::hidpad_axis_name(axis), role);
              if (role % 2) ImGui::SameLine();
              if (settings_widgets::Button(label)) {
                if (capturing == role) capturing = -1;
                else if (host::hidpad_axis_capture_begin(tab_index)) {
                  capturing = role;
                  state.hid_axis_slot = tab_index;
                  state.hid_axis_until = ImGui::GetTime() + 8.0;
                }
              }
              if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && axis != host::kHidAxisAuto) {
                roles[role] = host::kHidAxisAuto;
                host::hidpad_set_axis_roles(tab_index, roles);
                capturing = -1;
                changed = true;
              }
            }
            settings_hint("Release the stick or trigger, click its binding, then move it. Right-click for Automatic.");
          }
          const host::HidPadAxes axes = host::hidpad_axes(tab_index);
          if (axes.count) {
            std::string line = "Axes:";
            for (int a = 0; a < axes.count; ++a) line += "  " + std::string(axes.name[a]) + " " + std::to_string(axes.value[a]);
            settings_hint("%s", line.c_str());
          }
        }
        if (settings_widgets::CollapsingHeader("All buttons as a list")) {
          if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < (int)host::BindAction::Count; ++i) {
              if (analog_c && host::is_cstick_action(i) && tab_kind != host::CaptureDevice::HidPad) continue;
              // The box buttons act only on the keyboard and a box (apply_box_actions).
              if (host::is_box_action(i) && tab_kind != host::CaptureDevice::Keyboard && tab_kind != host::CaptureDevice::HidPad) continue;
              ImGui::TableNextColumn();
              ImGui::PushID(i);
              const bool pressed_now = (live >> i) & 1;
              char row[64];
              std::snprintf(row, sizeof row, "%-12s %s", kActionTitles[i], i == waiting ? "(press...)" : binding_label(tab_kind, tab_index, i).c_str());
              if (pressed_now) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.55f, 1.0f));
              // An analog source gets its press point beside the binding; a digital one nothing.
              const float press_w = binding_level_kind(tab_kind, tab_index, i) == 1 ? 132.0f : 0.0f;
              const float press_gap = press_w > 0.0f ? press_w + ImGui::GetStyle().ItemSpacing.x : 0.0f;
              if (settings_widgets::Selectable(row, i == waiting || i == hovered, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x - 58.0f - press_gap, 0))) begin_rebind(i);
              if (pressed_now) ImGui::PopStyleColor();
              if (tab_kind == host::CaptureDevice::HidPad && (host::is_stick_action(i) || host::is_cstick_action(i)) && ImGui::IsItemHovered())
                ImGui::SetTooltip("This controller's stick is read directly, with no binding needed. Bind this only for a box that\n"
                                  "sends its directions as buttons. A direction the wrong way round: Box layouts > Invert.");
              if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) unbind(i);
              if (press_w > 0.0f) {
                ImGui::SameLine();
                changed |= binding_press_at_slider(tab_kind, tab_index, i, press_w);
              }
              ImGui::SameLine();
              if (settings_widgets::SmallButton("Clear")) unbind(i);
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
        }
      }
    }
}

}  // namespace gx
