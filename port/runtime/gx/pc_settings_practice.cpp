// The Practice menu (Training, in the game window): the practice CPU's pages and the Training tools in
// one panel, its corner hint, and the replay viewer's controller layer. settings_frame calls
// draw_native_practice and draw_replay_practice.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// The practice overlays' card: rounded, dark, with the settings menu's pink accent bar on the left
// and, while something is in progress, a pulsing dot. Begin/End pair like ImGui::Begin.
static bool practice_card_begin(const char* id, ImVec2 pos, ImVec2 pivot, bool inputs) {
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.065f, 0.115f, 0.94f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.96f, 0.16f, 0.55f, 0.55f));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing;
  if (!inputs) flags |= ImGuiWindowFlags_NoInputs;
  const bool open = ImGui::Begin(id, nullptr, flags);
  const ImVec2 at = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(at.x + 1.0f, at.y + 8.0f), ImVec2(at.x + 6.0f, at.y + size.y - 8.0f),
                      IM_COL32(245, 40, 140, 255), 3.0f);
  return open;
}
static void practice_card_end() {
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}
// A title line with a pulsing dot in front of it (something in progress).
static void practice_card_title(const char* text, bool pulse) {
  const float t = (float)ImGui::GetTime();
  const float glow = pulse ? 0.55f + 0.45f * std::sin(t * 4.0f) : 1.0f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 5.0f, at.y + h * 0.5f), 4.5f,
                                              IM_COL32(245, 40, 140, (int)(255 * glow)), 16);
  ImGui::Dummy(ImVec2(14.0f, h));
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::TextColored(ImVec4(1.0f, 0.97f, 0.99f, 1.0f), "%s", text);
}

// ---------------- Practice hub: the Tab panel in Training ----------------
// The practice CPU and the Training tools in one panel. Every change applies at once, mid
// match, and is saved with the settings.
namespace practice_ui {
const ImU32 kPink = IM_COL32(245, 40, 140, 255);
const ImVec4 kPinkV(0.96f, 0.16f, 0.55f, 1.0f);

// A section heading: pink bar, title, and a short line under it.
void heading(const char* title, const char* subtitle = nullptr) {
  ImGui::Dummy(ImVec2(0, 6));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x, at.y + 1), ImVec2(at.x + 4, at.y + h - 1), kPink, 2.0f);
  ImGui::SetCursorScreenPos(ImVec2(at.x + 12, at.y));
  ImGui::PushFont(settings_heading_font());
  ImGui::TextColored(ImVec4(1.0f, 0.97f, 0.99f, 1.0f), "%s", title);
  ImGui::PopFont();
  if (subtitle) ImGui::TextDisabled("%s", subtitle);
}

// One option of an odds group: name, a weight bar 0..100, and its share of the group.
bool odds_row(const char* label, uint8_t* weight, int total) {
  ImGui::PushID(weight);  // two options of a group can share a name
  const float avail = ImGui::GetContentRegionAvail().x;
  const float name_w = std::min(190.0f, avail * 0.38f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(name_w);
  int v = *weight;
  const int share = total > 0 ? (v * 100 + total / 2) / total : 0;
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kPinkV);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.35f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.11f, 0.18f, 1.0f));
  ImGui::SetNextItemWidth(std::max(80.0f, avail - name_w - 64.0f));
  // The bar's fill shows the share at a glance.
  const ImVec2 bar = ImGui::GetCursorScreenPos();
  const bool changed = settings_widgets::SliderInt("##w", &v, 0, 100, v == 0 ? "never" : "%d");
  settings_page::label_item(label);
  const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
  (void)bar;
  if (share > 0)
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(bmin.x, bmax.y - 3), ImVec2(bmin.x + (bmax.x - bmin.x) * share / 100.0f, bmax.y),
                                              IM_COL32(245, 40, 140, 170), 2.0f);
  ImGui::PopStyleColor(3);
  ImGui::SameLine();
  if (total > 0 && v > 0) ImGui::Text("%3d%%", share); else ImGui::TextDisabled("  -");
  if (changed) *weight = (uint8_t)std::clamp(v, 0, 100);
  ImGui::PopID();
  return changed;
}

bool odds_group(const char* const names[], uint8_t* weights, int count) {
  int total = 0;
  for (int i = 0; i < count; ++i) total += weights[i];
  bool changed = false;
  for (int i = 0; i < count; ++i) changed |= odds_row(names[i], &weights[i], total);
  return changed;
}

bool percent_row(const char* label, uint8_t* value, const char* tip) {
  ImGui::PushID(label);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float name_w = std::min(190.0f, avail * 0.38f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  ImGui::SameLine(name_w);
  int v = *value;
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kPinkV);
  ImGui::SetNextItemWidth(std::max(80.0f, avail - name_w - 64.0f));
  const bool changed = settings_widgets::SliderInt("##v", &v, 0, 100, "%d%%");
  settings_page::label_item(label);
  ImGui::PopStyleColor();
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  if (changed) *value = (uint8_t)v;
  ImGui::PopID();
  return changed;
}

// One group in two arrays (version 3 adds options to version 1 groups): shares are of both together.
bool odds_group2(const char* const* labels, uint8_t* a, int na, const char* const* labels2, uint8_t* b, int nb) {
  int total = 0;
  for (int i = 0; i < na; ++i) total += a[i];
  for (int i = 0; i < nb; ++i) total += b[i];
  bool changed = false;
  for (int i = 0; i < na; ++i) changed |= odds_row(labels[i], &a[i], total);
  for (int i = 0; i < nb; ++i) changed |= odds_row(labels2[i], &b[i], total);
  return changed;
}
static const char* const kSides[MU_PC_SIDE_COUNT] = {"Roll to the ledge", "Roll to the middle"};

bool number_row(const char* label, uint8_t* value, int low, int high, const char* format, const char* tip) {
  ImGui::PushID(label);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float name_w = std::min(190.0f, avail * 0.38f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  ImGui::SameLine(name_w);
  int v = *value;
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kPinkV);
  ImGui::SetNextItemWidth(std::max(80.0f, avail - name_w - 64.0f));
  const bool changed = settings_widgets::SliderInt("##v", &v, low, high, format);
  settings_page::label_item(label);
  ImGui::PopStyleColor();
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  if (changed) *value = (uint8_t)v;
  ImGui::PopID();
  return changed;
}

bool flag_row(const char* label, uint8_t* bits, uint8_t mask, const char* tip) {
  bool on = (*bits & mask) != 0;
  const bool changed = settings_toggle(label, &on);
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  if (changed) *bits = (uint8_t)(on ? (*bits | mask) : (*bits & ~mask));
  return changed;
}

const char* const kMoves[MU_PC_MOVE_COUNT] = {"Stand", "Walk", "Dash dance", "Approach", "Run away", "Short hop",
                                              "Full hop", "Wavedash in", "Wavedash out", "Crouch"};
const char* const kAttacks[MU_PC_ATK_COUNT] = {"Jab", "Forward tilt", "Up tilt", "Down tilt", "Forward smash",
                                               "Up smash", "Down smash", "Dash attack", "Grab", "Boost grab",
                                               "Neutral air", "Forward air", "Back air", "Up air", "Down air",
                                               "Neutral B", "Side B", "Up B", "Down B", "Wavedash, forward tilt"};
const char* const kDefend[MU_PC_DEF_COUNT] = {"Nothing", "Shield", "Spot dodge", "Roll in", "Roll away", "Jump away"};
const char* const kOos[MU_PC_OOS_COUNT] = {"Hold shield", "Drop shield", "Jump", "Neutral air", "Up B",
                                           "Shine (down B)", "Grab", "Wavedash", "Roll", "Spot dodge"};
const char* const kDi[MU_PC_DI_COUNT] = {"No DI", "In", "Out", "Survival (up and in)", "Down and in", "Down and out"};
const char* const kTech[MU_PC_TECH_COUNT] = {"In place", "Roll in", "Roll away", "Miss"};
const char* const kGetup[MU_PC_GETUP_COUNT] = {"Stand", "Roll in", "Roll away", "Getup attack", "Stay down"};
const char* const kLedge[MU_PC_LEDGE_COUNT] = {"Getup", "Roll", "Jump", "Attack", "Drop and jump", "Ledgedash"};
const char* const kAfter[MU_PC_AFTER_COUNT] = {"Nothing", "Jump", "Air dodge", "Neutral air", "Up B"};
const char* const kRecovery[MU_PC_RECOVER_COUNT] = {"Game's own CPU", "High", "Low", "Mix high and low"};
const char* const kMoves2[MU_PC_MOVE2_COUNT] = {"Waveland", "Shield drop (platforms)", "Dash jump", "Run past you", "Turn around"};
const char* const kThrows[MU_PC_THROW_COUNT] = {"Forward throw", "Back throw", "Up throw", "Down throw"};
const char* const kDrift[MU_PC_DRIFT_COUNT] = {"Mix (hops either way, aerials toward you)", "Toward you", "Away from you", "Straight up"};
const char* const kTiming[MU_PC_TIMING_COUNT] = {"Mix", "Rising (as it leaves the ground)", "Falling (after the top)"};
const char* const kRng[MU_PC_RNG_COUNT] = {"Game's own", "Same each Training start", "Frozen (the same every frame)"};
int g_tab = 0;
int g_tab_request = -1;
bool g_lr_was_down = false;
}  // namespace practice_ui


// The practice CPU tabs. Returns true when a setting changed (saved by the caller).
static bool draw_practice_cpu_tabs(RenderOptions& options, int tab) {
  using namespace practice_ui;
  MuPcConfig c = practice_cpu::config();
  bool changed = false;
  switch (tab) {
  case 1: {
    bool on = c.enabled != 0;
    if (settings_toggle("Practice CPU", &on)) { c.enabled = on; changed = true; }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The Training CPU moves, attacks, defends and reacts as set on these pages.\n"
                        "Training only, never online or in replays.");
    const practice_cpu::Status st = practice_cpu::status();
    if (c.enabled) {
      if (st.driving) ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.7f, 1.0f), "Now: %s", mu_pc_plan_name(st.plan));
      else ImGui::TextDisabled("Starts with the next Training match (a CPU opponent is needed).");
    }
    ImGui::BeginDisabled(!c.enabled);
    bool freeze = c.freeze != 0;
    if (settings_toggle("Only react to hits", &freeze)) { c.freeze = freeze; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stands still; only DI, techs, getups, ledge options and recovery stay on.");
    bool moving_target = c.no_attacks != 0;
    if (settings_toggle("Moving target (no attacks)", &moving_target)) { c.no_attacks = moving_target; changed = true; }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("It moves around, shields, dodges, DIs, techs and recovers, but never attacks or grabs:\n"
                        "no attacks in range, out of shield, from the ledge, getting up or after a hit.");
    heading("Style");
    changed |= percent_row("Aggression", &c.aggression, "How often it attacks (rather than moves) when you are in range.");
    changed |= number_row("Attack range", &c.attack_range, 10, 80, "%d units", "How close you must be before it attacks.");
    changed |= number_row("Reaction time", &c.reaction_frames, 0, 40, "%d frames", "Frames before it answers an attack. People react in 15 to 25.");
    changed |= number_row("CPU delay", &c.act_delay, 0, 30, "%d frames", "Frames it waits before it acts out of shield, getting up and leaving hitstun.");
    {
      static const char* const respawn[MU_PC_RESPAWN_COUNT] = {"Wait", "Get off at once", "Mix"};
      int r = c.respawn < MU_PC_RESPAWN_COUNT ? c.respawn : 0;
      if (settings_combo("Respawn platform", &r, respawn, MU_PC_RESPAWN_COUNT)) { c.respawn = (uint8_t)r; changed = true; }
    }
    heading("Movement", "What it does between attacks. Weights within the group.");
    {
      // The two movement groups are picked from as one.
      int total = 0;
      for (int i = 0; i < MU_PC_MOVE_COUNT; ++i) total += c.move_odds[i];
      for (int i = 0; i < MU_PC_MOVE2_COUNT; ++i) total += c.move2_odds[i];
      for (int i = 0; i < MU_PC_MOVE_COUNT; ++i) changed |= odds_row(kMoves[i], &c.move_odds[i], total);
      for (int i = 0; i < MU_PC_MOVE2_COUNT; ++i) changed |= odds_row(kMoves2[i], &c.move2_odds[i], total);
    }
    heading("Attacks", "Picked when you are in range.");
    {
      // The special moves show the CPU's own names once its character is known (Fox: Reflector).
      static const char* const prefix[4] = {"Neutral B", "Side B", "Up B", "Down B"};
      static char special[4][64];
      const char* labels[MU_PC_ATK_COUNT];
      for (int i = 0; i < MU_PC_ATK_COUNT; ++i) labels[i] = kAttacks[i];
      for (int k = 0; k < 4; ++k)
        if (const char* name = practice_cpu::special_name(st.kind, k)) {
          std::snprintf(special[k], sizeof special[k], "%s: %s", prefix[k], name);
          labels[MU_PC_ATK_NEUTRAL_B + k] = special[k];
        }
      static const char* const run_shine[1] = {"Run shine (dash in, then down special)"};
      changed |= odds_group2(labels, c.attack_odds, MU_PC_ATK_COUNT, run_shine, &c.run_shine, 1);
    }
    heading("Throws", "Once it grabs you.");
    changed |= odds_group(kThrows, c.throw_odds, MU_PC_THROW_COUNT);
    changed |= number_row("Throw wait", &c.throw_wait, 0, 60, "%d frames", "Frames it holds you before it throws. 0: 4 to 12.");
    heading("Jumps and aerials");
    {
      int d = c.jump_drift < MU_PC_DRIFT_COUNT ? c.jump_drift : 0;
      if (settings_combo("Drift", &d, kDrift, MU_PC_DRIFT_COUNT)) { c.jump_drift = (uint8_t)d; changed = true; }
      int tm = c.aerial_timing < MU_PC_TIMING_COUNT ? c.aerial_timing : 0;
      if (settings_combo("Aerial timing", &tm, kTiming, MU_PC_TIMING_COUNT)) { c.aerial_timing = (uint8_t)tm; changed = true; }
    }
    changed |= percent_row("Full hop", &c.full_hop_aerials, "Chance an aerial is full hopped instead of short hopped.");
    changed |= percent_row("Double jump", &c.double_jump, "Chance a hop or an aerial takes its double jump at the top.");
    changed |= percent_row("Fast fall", &c.fast_fall, "Chance it fast falls its hops and aerials.");
    changed |= number_row("Fast fall delay", &c.fast_fall_delay, 0, 30, "%d frames", "Frames after the top of the jump before it fast falls.");
    changed |= percent_row("L-cancel", &c.l_cancel, nullptr);
    ImGui::EndDisabled();
    break;
  }
  case 2:
    ImGui::BeginDisabled(!c.enabled || c.freeze);
    heading("When you attack near it", "Answered once per attack, after its reaction time.");
    changed |= odds_group2(kDefend, c.defend_odds, MU_PC_DEF_COUNT, kSides, c.def2_odds, MU_PC_SIDE_COUNT);
    heading("Out of shield", "Once shieldstun ends.");
    changed |= odds_group(kOos, c.oos_odds, MU_PC_OOS_COUNT);
    ImGui::EndDisabled();
    break;
  case 3: {
    ImGui::BeginDisabled(!c.enabled);
    heading("DI", "Picked for each hit. In and out are toward and away from you.");
    changed |= odds_group(kDi, c.di_odds, MU_PC_DI_COUNT);
    static const char* const sdi[] = {"None", "Light", "Medium", "Heavy"};
    int s = c.sdi_amount;
    if (settings_combo("Smash DI", &s, sdi, 4)) { c.sdi_amount = (uint8_t)s; changed = true; }
    changed |= percent_row("Floorhug", &c.floorhug, "Chance it holds down when hit on the ground (ASDI down), so weak hits leave it standing.");
    heading("Tech", "When it lands in tumble.");
    changed |= odds_group2(kTech, c.tech_odds, MU_PC_TECH_COUNT, kSides, c.tech2_odds, MU_PC_SIDE_COUNT);
    heading("After a missed tech");
    changed |= odds_group2(kGetup, c.getup_odds, MU_PC_GETUP_COUNT, kSides, c.getup2_odds, MU_PC_SIDE_COUNT);
    heading("As hitstun ends in the air");
    changed |= odds_group(kAfter, c.after_odds, MU_PC_AFTER_COUNT);
    ImGui::EndDisabled();
    break;
  }
  case 4: {
    ImGui::BeginDisabled(!c.enabled);
    heading("From the ledge");
    changed |= odds_group(kLedge, c.ledge_odds, MU_PC_LEDGE_COUNT);
    changed |= number_row("Most time on the ledge", &c.ledge_delay, 0, 120, "%d frames", "It waits a random time up to this before its option.");
    heading("Recovery", "Back to the stage when it is off it.");
    int r = c.recovery;
    if (settings_combo("Recovery", &r, kRecovery, MU_PC_RECOVER_COUNT)) { c.recovery = (uint8_t)r; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("\"Game's own CPU\" uses the CPU level set in Training.");
    ImGui::EndDisabled();
    break;
  }
  case 5: {
    heading("Practice tools", options.native_source ? "D-pad: right save state, left load, down record the CPU, up play it back."
                                                    : "Frame advance works here; the other tools run on the Source Port.");
    ImGui::BeginDisabled(!options.native_source);
    changed |= flag_row("Save states and recording on the D-pad", &c.tools, MU_PC_TOOL_DPAD,
                        "Right keeps this moment, left returns to it as often as you like.\n"
                        "Down records your controller as the CPU; up plays that back while you practice.");
    changed |= flag_row("Loop the recording", &c.tools, MU_PC_TOOL_LOOP, "A played back recording starts again from the kept moment.");
    ImGui::EndDisabled();
    {
      int r = c.rng_mode < MU_PC_RNG_COUNT ? c.rng_mode : 0;
      if (settings_combo("Random numbers", &r, kRng, MU_PC_RNG_COUNT)) { c.rng_mode = (uint8_t)r; changed = true; }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Training's random picks (items, Peach's turnips, Game & Watch's hammer).\n"
                          "Same each start: every Training match (and state load) starts from the same seed.\n"
                          "Frozen: the seed is put back every frame, so every random pick is the same.");
      if (c.rng_mode != MU_PC_RNG_GAME)
        changed |= number_row("Seed", &c.rng_seed, 0, 255, "%d", "Another seed gives other random picks.");
    }
    changed |= flag_row("Frame advance", &c.tools, MU_PC_TOOL_FRAME_ADVANCE,
                        "In Training, L + R + Z freezes the game; then Z steps one frame, held Z keeps stepping,\n"
                        "and the D-pad moves the CPU.");
    ImGui::BeginDisabled(!options.native_source);
    changed |= flag_row("Hitboxes and hurtboxes", &c.tools, MU_PC_TOOL_HITBOXES, nullptr);
    changed |= flag_row("Hitboxes only (no models)", &c.tools, MU_PC_TOOL_HITBOXES_ONLY, nullptr);
    changed |= flag_row("Projectile hitboxes", &c.tools, MU_PC_TOOL_PROJECTILE_HITBOXES, nullptr);
    changed |= flag_row("Combo counter", &c.tools, MU_PC_TOOL_COMBO_COUNTER,
                        "Counts the hits of each combo over the attacker's damage.");
    changed |= flag_row("Inputs and frame advantage", &c.tools, MU_PC_TOOL_OVERLAY,
                        "Both players' inputs and actions, and your frame advantage after each hit or shield.");
    ImGui::EndDisabled();
    break;
  }
  case 6: {
    heading("Presets", "Start from one, then change anything on the other pages.");
    static const char* const about[] = {
        "Moves, attacks and shields. All-round.",
        "Stands still; DIs and techs. Combos.",
        "Short hop nairs and bairs, L-canceled.",
        "Runs and hops away. Practice chasing.",
        "Wavedash forward tilt, boost grabs.",
        "Shields, then punishes out of shield.",
        "DIs down, mixes every tech and getup.",
        "Mixes every option from the ledge.",
    };
    const float w = (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
    for (int i = 0; i < mu_pc_preset_count(); ++i) {
      if (i % 2) ImGui::SameLine();
      // A card per preset: name, one line about it, and a pink frame on the one in use.
      ImGui::PushID(i);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const bool clicked = ImGui::InvisibleButton("##preset", ImVec2(w, 62));
      const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
      const bool active = c.preset == i + 1;
      const ImVec2 p1(p0.x + w, p0.y + 62);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(p0, p1, hot ? IM_COL32(88, 22, 60, 255) : IM_COL32(32, 25, 46, 255), 8.0f);
      if (active || hot) draw->AddRect(p0, p1, kPink, 8.0f, 0, active ? 2.0f : 1.0f);
      draw->AddText(settings_heading_font(), 17.0f, ImVec2(p0.x + 12, p0.y + 8), IM_COL32(255, 247, 252, 255), mu_pc_preset_name(i));
      draw->PushClipRect(p0, p1, true);
      draw->AddText(ImVec2(p0.x + 12, p0.y + 34), IM_COL32(190, 182, 205, 255), about[i % (int)(sizeof about / sizeof about[0])]);
      draw->PopClipRect();
      if (active) draw->AddText(ImVec2(p1.x - 52, p0.y + 9), kPink, "in use");
      if (clicked) {
        const uint8_t tools = c.tools;
        mu_pc_preset(i, &c);
        c.tools = tools;
        c.enabled = 1;
        c.preset = (uint8_t)(i + 1);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (settings_widgets::Button("Reset everything to the defaults", ImVec2(-1, 34))) {
      const uint8_t tools = c.tools, enabled = c.enabled;
      mu_pc_defaults(&c);
      c.tools = tools;
      c.enabled = enabled;
      changed = true;
    }
    heading("Your setups", "Save everything set now, and load it again any time.");
    for (int i = 0; i < practice_cpu::kSlots; ++i) {
      ImGui::PushID(100 + i);
      MuPcConfig saved;
      const bool have = practice_cpu::slot(i, saved);
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Setup %d", i + 1);
      ImGui::SameLine(110);
      if (have) ImGui::TextDisabled("saved"); else ImGui::TextDisabled("empty");
      const float bw = 110.0f;
      ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 2 * bw - 22.0f);
      if (settings_widgets::Button("Save here", ImVec2(bw, 30))) { practice_cpu::set_slot(i, c); changed = true; }
      ImGui::SameLine();
      ImGui::BeginDisabled(!have);
      if (settings_widgets::Button("Load", ImVec2(bw, 30))) {
        const uint8_t tools = c.tools;
        c = saved;
        c.tools = tools;
        c.preset = 0;
        changed = true;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    break;
  }
  default: break;
  }
  if (changed) {
    if (tab >= 1 && tab <= 4) c.preset = 0;   // changed by hand: no preset is "in use" any more
    practice_cpu::set_config(c);
    apply_practice_tools(options);
  }
  return changed;
}

void draw_native_practice(SettingsState& state, RenderOptions& options,
                          const slippi::native_practice::Snapshot& practice,
                          const host::PadState& pad, bool have_pad) {
  const ImVec2 screen = ImGui::GetIO().DisplaySize;

  // On the way back to Training after an online session ("After an online session ends, go back to
  // Training mode"), until Training's character select is up.
  if (practice.returning && !practice.in_practice &&
      !slippi::online::is_online_match()) {   // never over a match
    practice_card_begin("##native_practice_return", ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImVec2(0.5f, 0.5f), false);
    practice_card_title("Returning to practice...", true);
    practice_card_end();
    return;
  }

  // The "Start + D-pad Up + Z" hint teaches the chord: after the third open it stays away (the setting still hides it).
  static bool counted_open = false;
  if (state.practice_open && !counted_open && options.practice_opens < 99) { ++options.practice_opens; state.dirty = true; }
  counted_open = state.practice_open;
  // The corner hint stays in every Training match (owner 11-17: players must see how to open it); the
  // setting still hides it. (practice_opens no longer changes anything: it was the search's intro banner.)
  const bool show_hint = options.matchmaking_hint;

  // The Practice menu's looks (practice_menu.cpp). The window further down is the "Window" look, and
  // what "All settings" opens until the menu is closed again.
  static bool window_once = false;
  if (!state.practice_open) window_once = false;
  const int look = std::clamp(options.practice_style, 0, practice_menu::kStyleCount - 1);
  if (look != practice_menu::kWindow && !window_once) {
    static bool was_open = false;
    if (state.practice_open && !was_open) practice_menu::opened();
    was_open = state.practice_open;
    practice_menu::Frame f;
    f.display_w = screen.x;
    f.display_h = screen.y;
    const float aspect = presented_aspect(options, (int)screen.x, (int)screen.y, true);
    float gw = screen.x, gh = gw / std::max(aspect, 0.1f);
    if (gh > screen.y) { gh = screen.y; gw = gh * aspect; }
    f.game_x0 = (screen.x - gw) * 0.5f;
    f.game_y0 = (screen.y - gh) * 0.5f;
    f.game_x1 = f.game_x0 + gw;
    f.game_y1 = f.game_y0 + gh;
    f.native_source = options.native_source;
    f.open = state.practice_open;
    f.armed = state.practice_pad_armed;
    if (have_pad) { f.buttons = pad.button; f.stick_x = pad.stick_x; f.stick_y = pad.stick_y; }
    // Only in Training: a VS match with friends or a training mod's exercise stays clean.
    f.hint = show_hint && practice.in_practice && practice.tab_available;
    // MELEE_TEST_PRACTICE_HINT=1 (tests only): the hint in any Training match, for captures of its look.
    static const bool test_hint = host::test_env::getenv("MELEE_TEST_PRACTICE_HINT") != nullptr;
    if (test_hint && practice.in_practice) f.hint = show_hint;
    f.available = practice.tab_available;
    f.unlocked = practice.unlocked;
    f.time = ImGui::GetTime();
    const practice_menu::Result r = practice_menu::frame(look, f);
    if (r.changed) { state.dirty = true; apply_practice_tools(options); }
    const auto close_menu = [&state]() { state.practice_open = false; state.practice_release_capture = true; };
    if (r.open_window) window_once = true;
    if (r.close) close_menu();
    if (!window_once) return;
  }

  if (!state.practice_open) {
    if (show_hint && practice.in_practice && practice.tab_available) {
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetTime() < 20.0 ? 1.0f : 0.5f);
      practice_card_begin("##native_practice_hint", ImVec2(screen.x - 12, 52), ImVec2(1, 0), false);
      practice_card_title("Practice menu", false);
      ImGui::TextDisabled("Start + D-pad Up + Z (Tab on a keyboard)");
      practice_card_end();
      ImGui::PopStyleVar();
    }
    return;
  }

  // The hub: tabs across the top (L and R on the controller switch them), the page below.
  using namespace practice_ui;
  {
    const bool lr = have_pad && (pad.button & 0x60) != 0;
    if (lr && !g_lr_was_down) g_tab_request = (g_tab + ((pad.button & 0x20) ? 1 : kTabCount - 1)) % kTabCount;
    g_lr_was_down = lr;
    // Start on the controller closes the panel (after the press that opened it has been let go).
    static bool start_was_down = true;
    const bool start = have_pad && (pad.button & 0x1000) != 0;
    if (start && !start_was_down && state.practice_pad_armed) {
      state.practice_open = false;
      state.practice_release_capture = true;
      start_was_down = true;
      return;
    }
    start_was_down = start;
  }
  const ImVec2 size(std::min(660.0f, screen.x - 40.0f), std::min(620.0f, screen.y - 40.0f));
  ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 16.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.05f, 0.10f, 0.96f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.96f, 0.16f, 0.55f, 0.6f));
  ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.14f, 0.11f, 0.20f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.70f, 0.14f, 0.43f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.86f, 0.15f, 0.50f, 1.0f));
  const bool shown = ImGui::Begin("##practice_hub", &state.practice_open,
                                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
  struct HubStyle { ~HubStyle() { ImGui::PopStyleColor(5); ImGui::PopStyleVar(3); } } hub_style;
  if (!shown) { ImGui::End(); return; }
  {
    const ImVec2 at = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + 1, at.y + 14), ImVec2(at.x + 6, at.y + sz.y - 14), kPink, 3.0f);
  }
  {
    // Title, the controls under it, and on the right a pill with what the CPU is doing.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - 22.0f;
    draw->AddText(settings_heading_font(), 30.0f, at, IM_COL32(255, 247, 252, 255), "PRACTICE");
    const MuPcConfig cfg = practice_cpu::config();
    const practice_cpu::Status st = practice_cpu::status();
    char pill[64];
    if (!cfg.enabled) std::snprintf(pill, sizeof pill, "Practice CPU off");
    else if (st.driving) std::snprintf(pill, sizeof pill, "CPU: %s", mu_pc_plan_name(st.plan));
    else std::snprintf(pill, sizeof pill, "Practice CPU on");
    const ImVec2 ts = ImGui::CalcTextSize(pill);
    const ImVec2 p0(right - ts.x - 26.0f, at.y + 4.0f), p1(right, at.y + 4.0f + ts.y + 10.0f);
    draw->AddRectFilled(p0, p1, cfg.enabled ? IM_COL32(245, 40, 140, 60) : IM_COL32(80, 80, 100, 60), 12.0f);
    draw->AddRect(p0, p1, cfg.enabled ? kPink : IM_COL32(120, 120, 140, 255), 12.0f, 0, 1.2f);
    if (cfg.enabled && st.driving) {
      const float glow = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.0f);
      draw->AddCircleFilled(ImVec2(p0.x + 11.0f, (p0.y + p1.y) * 0.5f), 3.5f, IM_COL32(245, 40, 140, (int)(255 * glow)), 12);
    }
    draw->AddText(ImVec2(p0.x + 18.0f, p0.y + 5.0f), IM_COL32(245, 240, 248, 255), pill);
    ImGui::Dummy(ImVec2(0, 34));
    ImGui::TextDisabled("Tab, Esc or START: back to the game      L / R: change page      A: change      B: back");
  }
  if (ImGui::BeginTabBar("##practice_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int i = 0; i < kTabCount; ++i) {
      const ImGuiTabItemFlags f = g_tab_request == i ? ImGuiTabItemFlags_SetSelected : 0;
      if (settings_widgets::BeginTabItem(kTabs[i], nullptr, f)) { g_tab = i; ImGui::EndTabItem(); }
    }
    g_tab_request = -1;
    ImGui::EndTabBar();
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
  ImGui::BeginChild("##practice_page", ImVec2(0, 0), false);
  if (draw_practice_cpu_tabs(options, g_tab + 1)) state.dirty = true;
  ImGui::EndChild();
  ImGui::PopStyleVar(4);
  ImGui::End();
}

// The replay viewer's controller layer (gx/replay_hud.h): take over a player, jump between tips, retry.
void draw_replay_practice(RenderOptions& options, const host::PadState& pad, bool have_pad) {
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  practice_menu::Frame f;
  f.display_w = screen.x;
  f.display_h = screen.y;
  const float aspect = presented_aspect(options, (int)screen.x, (int)screen.y, true);
  float gw = screen.x, gh = gw / std::max(aspect, 0.1f);
  if (gh > screen.y) { gh = screen.y; gw = gh * aspect; }
  f.game_x0 = (screen.x - gw) * 0.5f;
  f.game_y0 = (screen.y - gh) * 0.5f;
  f.game_x1 = f.game_x0 + gw;
  f.game_y1 = f.game_y0 + gh;
  f.armed = true;
  f.buttons = have_pad ? pad.button : 0;
  if (have_pad) { f.stick_x = pad.stick_x; f.stick_y = pad.stick_y; }
  f.time = ImGui::GetTime();
  replay_hud::frame(std::clamp(options.practice_style, 0, practice_menu::kStyleCount - 1), f);
}

}  // namespace gx
