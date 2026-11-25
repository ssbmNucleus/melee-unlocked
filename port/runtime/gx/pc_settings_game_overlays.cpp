// What settings_frame draws over the game (game window only): the Arcade look's combo counter, the
// community codes' displays, player nicknames, the controller display, and the notices that go to the
// log instead of over the game. settings_frame calls them; the HUD snapshot comes from the renderer.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"
#include "public_build.h"   // log_id: other players' codes in a public build's log

namespace gx {

namespace {
float g_overlay_game_aspect = 73.0f / 60.0f;
std::array<HudPlayerSnapshot, 4> g_hud_players{};
std::array<std::string, 4> g_hud_player_names{};
bool g_hud_match = false;
// The Arcade look's combo counter, counted per player hit, once per game frame (a frame presented again
// on a fast display counts nothing twice). A hit is a rise of that player's percent; it goes to the
// player whose combo link (Fighter x2094) holds them, or to the only opponent. The combo lives while
// the player hit keeps getting hit before they have been able to act for 45 frames, and ends at once
// when their percent drops (a KO).
struct ComboTrack { int attacker = -1; float percent = 0; int hits = 0, free_frames = 0; bool live = false;
                    double changed = 0, ended = -1; };
std::array<ComboTrack, 4> g_combo{};   // by the player hit
uint64_t g_combo_sequence = ~0ull;
void combo_track(uint64_t sequence) {
  if (sequence == g_combo_sequence) return;
  g_combo_sequence = sequence;
  const double now = ImGui::GetTime();
  static const bool log = [] { const char* v = host::test_env::getenv("MELEE_TEST_COMBO_LOG"); return v && *v == '1'; }();
  if (log && sequence % 120 == 0) host::log("combo: frame %llu match %d P1 %.0f%% stun %d link %d, P2 %.0f%% stun %d link %d", (unsigned long long)sequence, g_hud_match ? 1 : 0, g_hud_players[0].percent, g_hud_players[0].stunned ? 1 : 0, g_hud_players[0].combo_link, g_hud_players[1].percent, g_hud_players[1].stunned ? 1 : 0, g_hud_players[1].combo_link);
  for (int slot = 0; slot < 4; ++slot) {   // `slot` is the player hit
    const HudPlayerSnapshot& p = g_hud_players[slot];
    ComboTrack& c = g_combo[slot];
    if (!g_hud_match || !p.present) { if (c.live) { c.live = false; c.ended = now; } c.percent = 0; continue; }
    if (p.percent > c.percent + 0.01f) {
      int attacker = -1, others = 0, other = -1;
      for (int a = 0; a < 4; ++a) {
        if (a == slot || !g_hud_players[a].present) continue;
        ++others; other = a;
        if (g_hud_players[a].combo_link == slot) attacker = a;
      }
      if (attacker < 0) attacker = others == 1 ? other : c.attacker;
      if (c.live && attacker == c.attacker) ++c.hits;
      else { c.hits = 1; c.attacker = attacker; }
      c.live = attacker >= 0; c.free_frames = 0; c.changed = now;
      if (log) host::log("combo: P%d hits %d on P%d at %.0f%%", attacker + 1, c.hits, slot + 1, p.percent);
    } else if (c.live) {
      c.free_frames = p.stunned ? 0 : c.free_frames + 1;
      if (c.free_frames >= 45 || p.percent + 0.01f < c.percent) {
        c.live = false; c.ended = now;
        if (log) host::log("combo: P%d's combo on P%d ended at %d hits", c.attacker + 1, slot + 1, c.hits);
      }
    }
    c.percent = p.percent;
  }
}
}

OverlayBounds overlay_bounds() {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float aspect = std::max(0.1f, g_overlay_game_aspect);
  float width = display.x, height = width / aspect;
  if (height > display.y) { height = display.y; width = height * aspect; }
  const float x = (display.x - width) * 0.5f;
  const float y = (display.y - height) * 0.5f;
  return {x, y, x + width, y + height};
}

void clamp_overlay_window() {
  const OverlayBounds bounds = overlay_bounds();
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  const float max_x = std::max(bounds.left, bounds.right - size.x);
  const float max_y = std::max(bounds.top, bounds.bottom - size.y);
  const ImVec2 clamped(std::clamp(pos.x, bounds.left, max_x), std::clamp(pos.y, bounds.top, max_y));
  if (clamped.x != pos.x || clamped.y != pos.y) ImGui::SetWindowPos(clamped);
}

void settings_set_game_aspect(float aspect) {
  if (std::isfinite(aspect) && aspect > 0.1f) g_overlay_game_aspect = aspect;
}

void settings_set_hud_snapshot(const Frame& frame) {
  g_hud_match = frame_in_match(frame);
  g_hud_players = frame.hud_players;
  g_hud_player_names = frame.player_names;
  combo_track(frame.sequence);
}

// The Arcade look's combo counter (counted in combo_track): "N HITS" over the attacker, in the game's
// font and the port's colour, from the second hit. Offline matches and replays only.
void draw_combo_counter() {
  if (!g_hud_match || slippi::online::is_online_match() || !(arcade_display() & kArcadeComboCounter)) return;
  const OverlayBounds bounds = overlay_bounds();
  const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  const double now = ImGui::GetTime();
  static constexpr uint32_t colors[4] = {IM_COL32(255, 92, 92, 255), IM_COL32(96, 150, 255, 255),
                                         IM_COL32(255, 214, 72, 255), IM_COL32(88, 214, 120, 255)};
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->PushClipRect(ImVec2(bounds.left, bounds.top), ImVec2(bounds.right, bounds.bottom), true);
  for (int victim = 0; victim < 4; ++victim) {
    const ComboTrack& c = g_combo[victim];
    if (c.hits < 2 || c.attacker < 0) continue;
    const int slot = c.attacker;
    const HudPlayerSnapshot& p = g_hud_players[slot];
    if (!p.present) continue;
    // Holds while the combo lives, then fades over a third of a second.
    const float fade = c.live ? 1.0f : 1.0f - (float)std::clamp((now - c.ended) / 0.35, 0.0, 1.0);
    if (fade <= 0.0f) continue;
    // Each hit pops the number: a little larger, settling back over a tenth of a second.
    const float pop = 1.0f + 0.35f * (1.0f - (float)std::clamp((now - c.changed) / 0.12, 0.0, 1.0));
    const float px = std::clamp(height / 18.0f, 18.0f, 44.0f) * pop;
    char label[24];
    std::snprintf(label, sizeof label, "%d HITS", c.hits);
    const float w = practice_menu::text_width(px, label);
    // Over the attacker's damage display: the game lays the four out by port, P1 a fifth of the way
    // across and each next one 151 of 640 further (measured on its HUD), widened like the HUD under True 16:9.
    const float hud_x = 128.0f + 151.0f * slot;
    const float at_x = true_widescreen_active() ? 320.0f + (hud_x - 320.0f) * true_widescreen_factor() : hud_x;
    const float x = std::clamp(bounds.left + at_x / 640.0f * width - w * 0.5f, bounds.left + 4.0f,
                               std::max(bounds.left + 4.0f, bounds.right - w - 4.0f));
    const float y = bounds.top + height * 0.775f - px;
    const uint32_t color = (colors[slot] & 0x00FFFFFFu) | ((uint32_t)(255.0f * fade) << 24);
    practice_menu::text(draw, x, y, px, color, label, 0.12f);
  }
  draw->PopClipRect();
}

// Community codes drawn over the match (HUD tab), from what the native game reports each drawn frame
// (host API 24), in the game's font. Nana's percent: under each Ice Climbers player's damage, while
// Nana is in play. Ally markers (online, as the code has them): YOU over your own fighter, a heart
// over your teammate in a teams match.
void draw_community_code_overlays() {
  const uint32_t codes = host::g_community_codes.load(std::memory_order_relaxed);
  if (!g_hud_match || !(codes & (MU_DISPLAY_OPTION_NANA_PERCENT | MU_DISPLAY_OPTION_ALLY_INDICATORS))) return;
  const OverlayBounds bounds = overlay_bounds();
  const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  static constexpr ImU32 colors[4] = {
    IM_COL32(255, 99, 107, 255), IM_COL32(83, 164, 255, 255),
    IM_COL32(255, 211, 84, 255), IM_COL32(93, 220, 143, 255)};
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->PushClipRect(ImVec2(bounds.left, bounds.top), ImVec2(bounds.right, bounds.bottom), true);
  const auto at_x = [&](float x640) {
    const float x = true_widescreen_active() ? 320.0f + (x640 - 320.0f) * true_widescreen_factor() : x640;
    return bounds.left + x / 640.0f * width;
  };
  if (codes & MU_DISPLAY_OPTION_NANA_PERCENT) {
    const float px = std::clamp(height / 30.0f, 12.0f, 30.0f);
    for (int slot = 0; slot < 4; ++slot) {
      const HudPlayerSnapshot& p = g_hud_players[slot];
      if (!p.present || p.nana_damage < 0) continue;
      char label[24];
      std::snprintf(label, sizeof label, "NANA %d%%", p.nana_damage);
      const float w = practice_menu::text_width(px, label);
      // Under the damage display (the game lays them out by port, as for the combo counter).
      const float x = at_x(128.0f + 151.0f * slot) - w * 0.5f;
      const float y = bounds.top + height * 0.955f - px;
      practice_menu::text(draw, x, y, px, IM_COL32(255, 255, 255, 235), label, 0.0f, true);
    }
  }
  const int me = slippi::online::is_online_match() ? slippi::online::local_player_index() : -1;
  if ((codes & MU_DISPLAY_OPTION_ALLY_INDICATORS) && me >= 0 && me < 4) {
    const float px = std::clamp(height / 26.0f, 14.0f, 34.0f);
    for (int slot = 0; slot < 4; ++slot) {
      const HudPlayerSnapshot& p = g_hud_players[slot];
      const bool mine = slot == me, ally = !mine && p.team >= 0 && p.team == g_hud_players[me].team;
      if (!p.present || (!mine && !ally)) continue;
      if (!std::isfinite(p.tag_x) || !std::isfinite(p.tag_y) || p.tag_y <= 0.0f) continue;
      const char* label = mine ? "YOU" : "\xE2\x99\xA5";
      const float w = practice_menu::text_width(px, label);
      const float x = at_x(p.tag_x) - w * 0.5f;
      const float y = bounds.top + p.tag_y / 480.0f * height - px * 2.2f;
      practice_menu::text(draw, x, y, px, mine ? colors[slot] : IM_COL32(255, 90, 140, 255), label, 0.0f, true);
    }
  }
  draw->PopClipRect();
}

void draw_player_nicknames() {
  if (!g_hud_match) return;
  const OverlayBounds bounds = overlay_bounds();
  const float width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  const float font_size = std::clamp(height / 27.0f, 15.0f, 24.0f);
  static constexpr ImU32 colors[4] = {
    IM_COL32(255, 99, 107, 255), IM_COL32(83, 164, 255, 255),
    IM_COL32(255, 211, 84, 255), IM_COL32(93, 220, 143, 255)};
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->PushClipRect(ImVec2(bounds.left, bounds.top), ImVec2(bounds.right, bounds.bottom), true);
  for (int slot = 0; slot < 4; ++slot) {
    const auto& player = g_hud_players[slot];
    if (!player.tag_visible || g_hud_player_names[slot].empty()) continue;
    const std::string label = "P" + std::to_string(slot + 1) + "  " + g_hud_player_names[slot];
    const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label.c_str());
    // Under True 16:9 the tag is drawn 219/320 as far from the centre (gx_shader.cpp, is_nametag_layer).
    const float tag_x = true_widescreen_active() ? 320.0f + (player.tag_x - 320.0f) * true_widescreen_factor() : player.tag_x;
    const float x = std::clamp(bounds.left + tag_x / 640.0f * width - text_size.x * 0.5f,
                               bounds.left + 4.0f, std::max(bounds.left + 4.0f, bounds.right - text_size.x - 4.0f));
    const float y = std::clamp(bounds.top + player.tag_y / 480.0f * height - font_size * 2.4f,
                               bounds.top + 4.0f, bounds.bottom - font_size - 8.0f);
    draw->AddRectFilled(ImVec2(x - 5, y - 3), ImVec2(x + text_size.x + 5, y + font_size + 4),
                        IM_COL32(9, 11, 22, 215), 5.0f);
    draw->AddRect(ImVec2(x - 5, y - 3), ImVec2(x + text_size.x + 5, y + font_size + 4),
                  colors[slot], 5.0f, 0, 1.5f);
    draw->AddText(ImGui::GetFont(), font_size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), label.c_str());
  }
  draw->PopClipRect();
}

// On-screen controller display for streaming: the octagonal gate, C-stick, analog triggers and the
// face buttons, drawn from the state the game read on its last PADRead rather than a fresh poll, so
// it shows what the game acted on and device polling stays on one thread at one rate.
// `row` stacks several overlays upward so two to four players can be shown at once. `editable` is on
// in the settings window, which is where the overlay may be dragged and resized.
void draw_input_overlay(int port, int row, bool lone, bool editable, bool borderless, bool values, int knob,
                        float (*rects)[6], bool* moved) {
  host::PadState pads[4]{};
  host::input_last_pads(pads);
  int slot = port < 0 || port > 3 ? 0 : port;
  // An adapter socket the player is not using reports nothing, and the overlay then drew an empty
  // controller with no hint why. With a single port selected, show the first one that has a device
  // instead, so picking the wrong one (or plugging into socket 3) is not mistaken for a broken
  // overlay. With several selected the player asked for specific ports, so leave them as they are.
  if (lone && pads[slot].err != 0)
    for (int i = 0; i < 4; ++i)
      if (pads[i].err == 0) { slot = i; break; }
  const host::PadState& pad = pads[slot];

  // Taller with the stick values on, so the numbers get their own row under the sticks.
  const float pad_w = 300.f, pad_h = values ? 150.f : 132.f;
  // Named and placed by the port the player picked, not the one shown: with a lone port whose socket
  // went quiet for a moment, the overlay used to swap to another port's window and its own spot.
  const int home = port < 0 || port > 3 ? 0 : port;
  char title[32];
  std::snprintf(title, sizeof title, "Controller%d", home);
  // Laid out for this size and scaled to whatever the window is dragged to, so it can be sized to
  // taste for a stream layout. In the settings window it can be moved and resized; over the game it
  // ignores the mouse entirely so it can never swallow a click meant for the game. ImGui remembers each overlay's position and size between launches by window name.
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav;
  if (!editable) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
  if (borderless) flags |= ImGuiWindowFlags_NoBackground;
  const OverlayBounds bounds = overlay_bounds();
  // Saved as x, y, width, height and the screen it was placed on. Positions used to be kept as pixels
  // from the top left, so an overlay put at the bottom of a small window (the launcher's settings
  // window, or the game window before a resize) showed halfway up a bigger one (player report 11-20).
  // Now it keeps its distance to the nearest edges of whatever screen it is on, re-placed whenever
  // that screen changes size. A line saved without the screen (0.9.993 and older) keeps its size only.
  float* saved = rects[home];
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  static ImVec2 placed_for[4];
  const bool reflow = placed_for[home].x != display.x || placed_for[home].y != display.y;
  placed_for[home] = display;
  const ImGuiCond when = reflow ? ImGuiCond_Always : ImGuiCond_Once;
  if (saved[2] > 0.0f && saved[4] > 0.0f && saved[5] > 0.0f) {
    const float x = saved[0] + saved[2] * 0.5f > saved[4] * 0.5f ? display.x - (saved[4] - saved[0]) : saved[0];
    const float y = saved[1] + saved[3] * 0.5f > saved[5] * 0.5f ? display.y - (saved[5] - saved[1]) : saved[1];
    ImGui::SetNextWindowPos(ImVec2(x, y), when);
    ImGui::SetNextWindowSize(ImVec2(saved[2], saved[3]), ImGuiCond_Once);
  } else {
    ImGui::SetNextWindowPos(ImVec2(bounds.left + 16, bounds.bottom - 16 - row * (pad_h + 6)), when, ImVec2(0, 1));
    ImGui::SetNextWindowSize(saved[2] > 0.0f ? ImVec2(saved[2], saved[3]) : ImVec2(pad_w, pad_h), ImGuiCond_Once);
  }
  ImGui::SetNextWindowBgAlpha(borderless ? 0.0f : 0.30f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, borderless ? 0.0f : 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  // The resize grip draws even with the background off, leaving a triangle in the bottom right of an
  // otherwise invisible overlay. Hidden in borderless mode; the window edges still resize it.
  if (borderless) {
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(255, 255, 255, 40));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(255, 255, 255, 70));
  }
  ImGui::Begin(title, nullptr, flags);
  {   // Anywhere in the window, the side borders beside the picture included (player request 11-20).
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const ImVec2 clamped(std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x)),
                         std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - size.y)));
    if (clamped.x != pos.x || clamped.y != pos.y) ImGui::SetWindowPos(clamped);
  }
  // Remembered in the settings file only when the player drags or resizes it in the settings window.
  // Keeping it on screen above is not a move: that clamp, in the small settings window, used to save
  // the squeezed spot as if the player had put it there.
  if (editable && ImGui::IsWindowFocused() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const float now[6] = {pos.x, pos.y, size.x, size.y, display.x, display.y};
    for (int i = 0; i < 6; ++i)
      if (std::fabs(now[i] - saved[i]) > 0.5f) { std::copy(now, now + 6, saved); *moved = true; break; }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  // Everything below is authored against a 300x132 controller and scaled to the current size.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float k = std::max(0.25f, std::min(avail.x / pad_w, avail.y / pad_h));
  auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
  auto S = [&](float v) { return v * k; };
  const float gate = S(46.f), cgate = S(30.f);
  const ImU32 line = IM_COL32(255, 255, 255, 190), dim = IM_COL32(255, 255, 255, 70);
  const ImU32 yellow = IM_COL32(245, 215, 65, 235), green = IM_COL32(120, 225, 150, 240), red = IM_COL32(235, 95, 95, 240);

  // Analog triggers: the bar fills with how far it is pressed, so light presses are visible.
  auto trigger = [&](float x, uint8_t value) {
    const ImVec2 a = P(x, 4), b = P(x + 58, 12);
    dl->AddRectFilled(a, ImVec2(a.x + S(58) * (value / 255.f), b.y), line, S(4.f));
    dl->AddRect(a, b, dim, S(4.f));
  };
  trigger(6, pad.trig_l);
  trigger(74, pad.trig_r);

  // Melee's gate is an octagon with vertices on the cardinals and diagonals, which is what an eight
  // sided ImGui n-gon gives. Stick values are signed and screen Y grows downward.
  // Scaled like the settings panel's sticks: 80 is the game's full tilt, so a stick at the rim reads
  // at the rim. It used to be 128, the raw range, which drew a full press at under two thirds out.
  auto stick = [&](ImVec2 c, float r, int8_t sx, int8_t sy, ImU32 colour) {
    dl->AddNgon(c, r, colour, 8, S(2.f));
    dl->AddCircle(c, S(2.f), dim, 8, 1.f);
    float fx = sx / 80.f, fy = sy / 80.f;
    const float mag = std::sqrt(fx * fx + fy * fy);
    if (mag > 1.f) { fx /= mag; fy /= mag; }
    // Sized off the gate; "Stick size" 5 is the look the overlay has always had (a fifth of the gate).
    const float knob_r = std::max(S(3.f), r * 0.04f * std::clamp(knob, 1, 10));
    // The knob is drawn as a circle around its centre, so at a full press the centre alone reaching
    // the gate's radius let the far side of a big knob bulge past the outline. Pull the centre's own
    // travel in by the knob's radius so the whole knob stays inside the gate and just touches the
    // edge at full tilt, rather than overlapping it -- the larger "Stick size" is, the more this matters.
    const float reach = std::max(0.f, r - knob_r);
    const ImVec2 tip(c.x + fx * reach, c.y - fy * reach);
    dl->AddCircleFilled(tip, knob_r, colour, 20);
  };
  stick(P(52, 74), gate, pad.stick_x, pad.stick_y, line);
  stick(P(146, 82), cgate, pad.sub_x, pad.sub_y, yellow);
  // The values the game works with: 1/80 steps, the vector clamped to length 1 as the game does,
  // so a full press reads 1.0000 and any B0XX or GRAM coordinate can be checked against its chart.
  if (values) {
    auto text = [&](float x, int8_t sx, int8_t sy, ImU32 colour) {
      float fx = sx / 80.f, fy = sy / 80.f;
      const float mag = std::sqrt(fx * fx + fy * fy);
      // Past the rim the game pulls the stick back to length 80 and drops the fraction (HSD_PadClamp), so
      // a full diagonal is 56, 56: 0.7000 0.7000. Without that last step it read 0.7071 (upstream d26a2eaf).
      if (mag > 1.f) { fx = std::trunc(fx / mag * 80.f) / 80.f; fy = std::trunc(fy / mag * 80.f) / 80.f; }
      char s[32];
      std::snprintf(s, sizeof s, "%.4f %.4f", fx, fy);
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * k, P(x, 134), colour, s);
    };
    text(6, pad.stick_x, pad.stick_y, line);
    text(150, pad.sub_x, pad.sub_y, yellow);
  }

  auto button = [&](ImVec2 c, float r, ImU32 colour, bool down, const char* label) {
    if (down) dl->AddCircleFilled(c, r, colour, 16);
    else dl->AddCircle(c, r, colour, 16, 1.5f);
    if (label) {
      const ImVec2 size = ImGui::CalcTextSize(label);
      dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), down ? IM_COL32(20, 20, 20, 230) : colour, label);
    }
  };
  const uint16_t b = pad.button;
  // GameCube face layout: A large in the middle, B low and left of it, X out to the right and Y up
  // over the top (the two kidney buttons wrap around A rather than sitting on the diagonals).
  button(P(232, 76), S(19.f), green, (b & 0x0100) != 0, "A");
  button(P(202, 101), S(10.f), red, (b & 0x0200) != 0, "B");
  button(P(267, 69), S(10.f), line, (b & 0x0400) != 0, "X");
  button(P(221, 42), S(10.f), line, (b & 0x0800) != 0, "Y");
  button(P(254, 28), S(9.f), IM_COL32(170, 130, 235, 240), (b & 0x0010) != 0, "Z");
  button(P(150, 24), S(7.f), line, (b & 0x1000) != 0, nullptr);

  // D-pad, small, only drawn when held: it is rarely used and should not clutter a stream.
  if (b & 0x0008) dl->AddTriangleFilled(P(104, 100), P(99, 108), P(109, 108), line);
  if (b & 0x0004) dl->AddTriangleFilled(P(104, 124), P(99, 116), P(109, 116), line);
  if (b & 0x0001) dl->AddTriangleFilled(P(92, 112), P(100, 107), P(100, 117), line);
  if (b & 0x0002) dl->AddTriangleFilled(P(116, 112), P(108, 107), P(108, 117), line);
  ImGui::End();
  if (borderless) ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
}

// What the game window used to show over the game as notices now goes to the log, once each time it
// starts to apply: nothing is drawn over Melee but the HUD overlays a player switches on.
//   - automatic L-cancel switched off for a matchmaking mode, or an opponent's automatic L-cancel
//     (the missed L-cancel itself is still the red tint on the fighter, lcancel.cpp);
//   - a friend's Discord Join (their code is first in Online > Direct's suggestions);
//   - skins and stage mods shown as the standard ones in an online mode.
void log_game_notices() {
  {
    static std::string said;
    std::string now;
    if (lcancel::automatic_enabled() && lcancel::online_session_pending())
      if (const char* mode = lcancel::auto_suppressed_mode()) now = mode;
    if (now != said) {
      said = now;
      if (!now.empty()) {
        if (lcancel::automatic_setting_locked()) host::log("lcancel: %s", lcancel::policy::ranked_hint);
        else host::log("lcancel: automatic L-cancel is on, but switched off for %s mode", now.c_str());
      }
    }
  }
#ifndef MELEE_NO_SLIPPI
  {
    static bool said = false;
    const bool opponent_auto = slippi::online::opponent_auto_lcancel();
    if (opponent_auto && !said) host::log("lcancel: the opponent has automatic L-cancel on");
    said = opponent_auto;
  }
#endif
  if (host::discord::enabled()) {
    static std::string said;
    const std::string code = host::discord::invite_notice();
    if (code != said) {
      said = code;
      if (!code.empty())
        host::log("discord: invite from %s; their code is the first suggestion in Online > Direct",
                  host::public_build::log_id(code).c_str());   // another player's code: a hash in a public build
    }
  }
  {
    static bool said_this_visit = false;
    const bool online = slippi::online::in_online_menus() || slippi::online::is_online_match();
    if (!online) {
      said_this_visit = false;
    } else if (!said_this_visit) {
      uint32_t stages = 0;
      uint32_t skins = host::cosmetics::swapped_online_count(&stages);
      const auto& mod_status = source_port::mods::status();
      if (mod_status.retail_view)
        for (const auto& skin : mod_status.pack_skins) skins += skin.served ? 0u : 1u;
      if (skins + stages > 0) {
        said_this_visit = true;
        host::log("online: %u skins and %u stage mods are off for this online mode and show as the standard ones",
                  skins, stages);
      }
    }
  }
}

}  // namespace gx
