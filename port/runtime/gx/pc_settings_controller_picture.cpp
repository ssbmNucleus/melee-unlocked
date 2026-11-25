// The controllers to rebind by pointing at them, on the Controls page: the GameCube controller and the
// Switch Pro Controller, laid out as Smash Ultimate's button settings, and the deadzone view.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// The GameCube controller to rebind by pointing at it, laid out the way Smash Ultimate's button
// settings screen is: the controller in the middle of a light panel, callout boxes grouped in grey
// panels around it, each joined to its part by a cyan line. Click a part or its box to bind it,
// right-click to clear. A part lights while pressed; the box being bound turns purple.
// Authored on a 960x392 canvas and scaled to the panel's width.
struct GcCallout { int action; float x, y, w; };
// The keyboard has two rows of direction boxes under the pad: the C-stick, and the control stick
// (added once the stick stopped being hard wired to the arrow keys).
constexpr float kGcCanvasHKeys = 476, kGcCanvasHAnalog = 350;   // keys: a third row for the box buttons

// The deadzone as the settings picture shows it: an orange ring sized to the deadzone, and a dot at
// the stick's real position, orange while it is inside the ring (where the game reads the stick as
// centred), white outside. `raw` and `dz` are in stick units over 80, the full throw of a stick.
// Shown only while a deadzone slider is being moved, and for a moment after (these hold until when).
double g_show_dz_main_until = 0, g_show_dz_c_until = 0;
static bool inside_deadzone(ImVec2 raw, float dz) { return dz > 0 && raw.x * raw.x + raw.y * raw.y < dz * dz; }
static void draw_deadzone_view(ImDrawList* dl, ImVec2 c, float reach, ImVec2 raw, float dz, float k, bool c_stick) {
  if (ImGui::GetTime() > (c_stick ? g_show_dz_c_until : g_show_dz_main_until)) return;
  const ImU32 zone = IM_COL32(255, 140, 40, 255);
  if (dz > 0) {
    dl->AddCircleFilled(c, dz * reach, IM_COL32(255, 140, 40, 60), 40);
    dl->AddCircle(c, dz * reach, zone, 40, 2.0f * k);
  }
  const ImVec2 p(c.x + raw.x * reach, c.y - raw.y * reach);
  dl->AddCircleFilled(p, 4.5f * k, inside_deadzone(raw, dz) ? zone : IM_COL32(255, 255, 255, 255), 16);
  dl->AddCircle(p, 4.5f * k, IM_COL32(20, 20, 24, 255), 16, 1.5f * k);
}

int draw_gc_bind_picture(uint32_t live, int capturing, int* right_clicked, int* hovered_out,
                         const std::function<std::string(int)>& label, bool analog_c,
                         const char* c_mode_text, ImVec2 stick_pos, ImVec2 c_pos, float dz_main, float dz_c) {
  using A = host::BindAction;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float k = std::clamp(avail / 960.0f, 0.15f, 1.4f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float kGcCanvasH = analog_c ? kGcCanvasHAnalog : kGcCanvasHKeys;
  ImGui::InvisibleButton("gc_picture", ImVec2(960.0f * k, kGcCanvasH * k));
  const bool canvas_hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
  const ImVec2 mouse = ImGui::GetMousePos();
  const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
  const ImU32 cyan = IM_COL32(40, 200, 225, 255);

  // Light panel behind everything, as Ultimate's screen.
  dl->AddRectFilled(P(0, 0), P(960, kGcCanvasH), IM_COL32(222, 223, 229, 255), 12 * k);

  // ---- the controller ----
  // Traced, not drawn by eye: the outline is the silhouette of the controller in a 1200x675 capture
  // of Ultimate's button settings screen (dark body pixels, overlays closed over, boundary walked
  // and simplified), and every part sits where it measures in that same capture. Coordinates are
  // that capture's pixels, mapped onto the canvas by C().
  constexpr float kS = 0.85f;
  auto C = [&](float x, float y) { return P(480 + (x - 599) * kS, 50 + (y - 130) * kS); };
  const float kc = kS * k;   // a controller-space length in screen pixels
  // The left half, from the top centre round the stick's side and handle, up the handle's inner
  // edge, round the D-pad's lobe and up the keyhole arch to its top. The right half is its mirror.
  static const float left_half[][2] = {
    {599, 130}, {544, 135}, {496, 146}, {470, 157}, {445, 168}, {422, 181}, {405, 194}, {397, 203},
    {390, 217}, {386, 230}, {379, 274}, {378, 297}, {376, 333}, {375, 403}, {377, 423}, {381, 442},
    {386, 452}, {393, 459}, {399, 462}, {413, 462}, {420, 459}, {427, 452}, {436, 433}, {447, 385},
    {449, 379}, {457, 351}, {463, 345}, {471, 350}, {475, 356}, {485, 374}, {493, 382}, {504, 389},
    {516, 393}, {537, 393}, {544, 391}, {557, 384}, {566, 376}, {575, 361}, {578, 351}, {578, 331},
    {571, 313}, {563, 303}, {557, 292}, {557, 276}, {570, 273}, {599, 271}};
  constexpr int kHalf = (int)(sizeof left_half / sizeof left_half[0]);
  ImVec2 pts[kHalf * 2];
  int n = 0;
  for (int i = 0; i < kHalf; ++i) pts[n++] = C(left_half[i][0], left_half[i][1]);
  for (int i = kHalf - 2; i >= 1; --i) pts[n++] = C(1198 - left_half[i][0], left_half[i][1]);
  // Shoulders sit behind the body, only their tops showing: L grey on the left, R grey on the right
  // with the blue Z on top of it (measured: L 416..485 x 138..183, Z 713..779 x 147..177).
  // A shoulder lights for its click and for its travel alike.
  const bool l_on = ((live >> (int)A::L) | (live >> (int)A::LAnalog)) & 1, r_on = ((live >> (int)A::R) | (live >> (int)A::RAnalog)) & 1;
  const bool z_on = (live >> (int)A::Z) & 1;
  const ImU32 shoulder = IM_COL32(150, 150, 158, 255), shoulder_on = IM_COL32(240, 150, 40, 255);
  dl->AddEllipseFilled(C(451, 166), ImVec2(37 * kc, 21 * kc), l_on ? shoulder_on : shoulder, -0.45f, 40);
  dl->AddEllipseFilled(C(747, 166), ImVec2(37 * kc, 21 * kc), r_on ? shoulder_on : shoulder, 0.45f, 40);
  dl->AddEllipseFilled(C(746, 164), ImVec2(34 * kc, 13 * kc), z_on ? IM_COL32(110, 150, 255, 255) : IM_COL32(45, 75, 190, 255), 0.38f, 32);
  // Filled a pixel row at a time between the edge crossings (ImGui's concave triangulator gives up on
  // this outline), then outlined anti-aliased on top.
  {
    float y0 = pts[0].y, y1 = pts[0].y;
    for (int i = 1; i < n; ++i) { y0 = std::min(y0, pts[i].y); y1 = std::max(y1, pts[i].y); }
    std::vector<float> xs;
    for (float y = std::floor(y0) + 0.5f; y < y1; y += 1.0f) {
      xs.clear();
      for (int i = 0; i < n; ++i) {
        const ImVec2 a = pts[i], b = pts[(i + 1) % n];
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t j = 0; j + 1 < xs.size(); j += 2)
        dl->AddRectFilled(ImVec2(xs[j], y - 0.5f), ImVec2(xs[j + 1], y + 0.5f), IM_COL32(52, 52, 57, 255));
    }
  }
  dl->AddPolyline(pts, n, IM_COL32(28, 28, 31, 255), ImDrawFlags_Closed, 2.0f * k);
  // The lighter well round the control stick (the only one on Ultimate's picture).
  dl->AddCircleFilled(C(467, 246), 72 * kc, IM_COL32(68, 68, 74, 255), 48);

  // ---- parts, at their measured centres and sizes ----
  const ImVec2 stick = C(466, 245), a_c = C(732, 245), b_c = C(683, 270), start_c = C(599, 245),
               dpad_c = C(528, 344), cst_c = C(670, 342);
  const ImVec2 y_c = C(719, 199), x_c = C(779, 234);   // the Y and X beans
  constexpr float kYrx = 22, kYry = 13, kYrot = -0.22f, kXrx = 13, kXry = 23, kXrot = 0.16f;
  // The caps move with what the game gets: the real stick, or centre while it is inside the deadzone.
  const ImVec2 stick_game = inside_deadzone(stick_pos, dz_main) ? ImVec2(0, 0) : stick_pos;
  const ImVec2 c_game = inside_deadzone(c_pos, dz_c) ? ImVec2(0, 0) : c_pos;
  const ImVec2 stick_cap(stick.x + stick_game.x * 11 * kc, stick.y - stick_game.y * 11 * kc);
  const ImVec2 c_cap(cst_c.x + c_game.x * 10 * kc, cst_c.y - c_game.y * 10 * kc);
  auto lit = [&](int action) { return ((live >> action) & 1) != 0; };
  // Control stick: grey octagonal gate, cap with rings.
  dl->AddNgonFilled(stick, 33 * kc, IM_COL32(150, 150, 158, 255), 8);
  dl->AddCircleFilled(stick_cap, 24 * kc, IM_COL32(185, 185, 192, 255), 40);
  dl->AddCircle(stick_cap, 17 * kc, IM_COL32(150, 150, 158, 255), 40, 2.0f * k);
  dl->AddCircle(stick_cap, 9 * kc, IM_COL32(150, 150, 158, 255), 40, 2.0f * k);
  // Start.
  dl->AddCircleFilled(start_c, 9 * kc, lit((int)A::Start) ? IM_COL32(255, 255, 255, 255) : IM_COL32(165, 165, 172, 255), 24);
  // A, B, and the Y and X beans.
  dl->AddCircleFilled(a_c, 26 * kc, lit((int)A::A) ? IM_COL32(90, 230, 120, 255) : IM_COL32(40, 170, 75, 255), 48);
  dl->AddCircleFilled(b_c, 15 * kc, lit((int)A::B) ? IM_COL32(255, 110, 110, 255) : IM_COL32(215, 40, 40, 255), 32);
  auto bean = [&](ImVec2 c, float rx, float ry, float rot, bool on) {
    dl->AddEllipseFilled(c, ImVec2(rx * kc, ry * kc), on ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 206, 255), rot, 32);
  };
  bean(y_c, kYrx, kYry, kYrot, lit((int)A::Y));
  bean(x_c, kXrx, kXry, kXrot, lit((int)A::X));
  auto centred_text = [&](ImVec2 c, const char* t, ImU32 col) {
    const ImVec2 sz = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(c.x - sz.x / 2, c.y - sz.y / 2), col, t);
  };
  centred_text(a_c, "A", IM_COL32(255, 255, 255, 255));
  centred_text(b_c, "B", IM_COL32(255, 255, 255, 255));
  centred_text(x_c, "X", IM_COL32(60, 60, 66, 255));
  centred_text(y_c, "Y", IM_COL32(60, 60, 66, 255));
  // D-pad: a light plus with rounded arms and arrows, each arm its own part.
  const float arm = 24, half = 9;
  struct Arm { int action; float x0, y0, x1, y1; };
  const Arm arms[] = {{(int)A::DUp, -half, -arm, half, -half}, {(int)A::DDown, -half, half, half, arm},
                      {(int)A::DLeft, -arm, -half, -half, half}, {(int)A::DRight, half, -half, arm, half}};
  dl->AddRectFilled(ImVec2(dpad_c.x - half * kc, dpad_c.y - half * kc), ImVec2(dpad_c.x + half * kc, dpad_c.y + half * kc), IM_COL32(200, 200, 206, 255));
  for (const Arm& a : arms)
    dl->AddRectFilled(ImVec2(dpad_c.x + a.x0 * kc, dpad_c.y + a.y0 * kc), ImVec2(dpad_c.x + a.x1 * kc, dpad_c.y + a.y1 * kc),
                      lit(a.action) ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 206, 255), 3 * kc);
  auto dpad_arrow = [&](float dx, float dy) {
    const ImVec2 tip(dpad_c.x + dx * 20 * kc, dpad_c.y + dy * 20 * kc), base(dpad_c.x + dx * 13 * kc, dpad_c.y + dy * 13 * kc);
    const ImVec2 side(-dy * 5.0f * kc, dx * 5.0f * kc);
    dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y), IM_COL32(110, 110, 118, 255));
  };
  dpad_arrow(0, -1); dpad_arrow(0, 1); dpad_arrow(-1, 0); dpad_arrow(1, 0);
  // C-stick: yellow octagon and cap.
  const bool c_on = lit((int)A::CUp) || lit((int)A::CDown) || lit((int)A::CLeft) || lit((int)A::CRight);
  dl->AddNgonFilled(cst_c, 33 * kc, IM_COL32(215, 160, 20, 255), 8);
  dl->AddCircleFilled(c_cap, 22 * kc, c_on ? IM_COL32(255, 230, 120, 255) : IM_COL32(245, 195, 40, 255), 32);
  centred_text(c_cap, "C", IM_COL32(120, 80, 0, 255));
  // Main stick (keyboard digital bindings only): ring lights up while any stick-direction key is held.
  const bool s_on = lit((int)A::SUp) || lit((int)A::SDown) || lit((int)A::SLeft) || lit((int)A::SRight);
  if (s_on) dl->AddCircle(stick, 33 * kc, IM_COL32(255, 255, 255, 255), 8, 3.0f * k);
  draw_deadzone_view(dl, stick, 33 * kc, stick_pos, dz_main, k, false);
  draw_deadzone_view(dl, cst_c, 33 * kc, c_pos, dz_c, k, true);

  // ---- hit testing on the controller ----
  auto in_circle = [&](ImVec2 c, float r) { const float dx = mouse.x - c.x, dy = mouse.y - c.y; return dx * dx + dy * dy <= r * r * kc * kc; };
  auto in_bean = [&](ImVec2 c, float rx, float ry, float rot) {
    const float dx = mouse.x - c.x, dy = mouse.y - c.y, cs = std::cos(-rot), sn = std::sin(-rot);
    const float u = (dx * cs - dy * sn) / ((rx + 3) * kc), v = (dx * sn + dy * cs) / ((ry + 3) * kc);
    return u * u + v * v <= 1.0f;
  };
  auto in_rect = [&](ImVec2 a, ImVec2 z) { return mouse.x >= a.x && mouse.x <= z.x && mouse.y >= a.y && mouse.y <= z.y; };
  auto part_at_mouse = [&]() -> int {
    if (in_circle(a_c, 28)) return (int)A::A;
    if (in_circle(b_c, 17)) return (int)A::B;
    if (in_bean(x_c, kXrx, kXry, kXrot)) return (int)A::X;
    if (in_bean(y_c, kYrx, kYry, kYrot)) return (int)A::Y;
    if (in_circle(start_c, 13)) return (int)A::Start;
    for (const Arm& a : arms)
      if (in_rect(ImVec2(dpad_c.x + (a.x0 - 3) * kc, dpad_c.y + (a.y0 - 3) * kc), ImVec2(dpad_c.x + (a.x1 + 3) * kc, dpad_c.y + (a.y1 + 3) * kc)))
        return a.action;
    if (in_circle(cst_c, 35)) {
      if (analog_c) return -1;   // an analog C-stick is not rebindable
      const float dx = mouse.x - cst_c.x, dy = mouse.y - cst_c.y;
      return std::fabs(dx) > std::fabs(dy) ? (dx > 0 ? (int)A::CRight : (int)A::CLeft) : (dy > 0 ? (int)A::CDown : (int)A::CUp);
    }
    if (in_rect(C(713, 145), C(781, 170))) return (int)A::Z;
    if (in_circle(C(451, 158), 34) && mouse.y < C(0, 176).y) return (int)A::L;
    if (in_circle(C(760, 160), 30) && mouse.y < C(0, 176).y) return (int)A::R;
    return -1;
  };

  // ---- callouts: grouped the way Ultimate groups them ----
  // Each shoulder has its click and its travel as separate boxes (L, L analog; R, R analog, then Z).
  // The right group holds three, so its rows sit closer than the others to end above the face group.
  std::vector<GcCallout> boxes = {
    {(int)A::L, 24, 30, 200}, {(int)A::LAnalog, 24, 70, 200},
    {(int)A::DUp, 24, 120, 200}, {(int)A::DDown, 24, 160, 200}, {(int)A::DLeft, 24, 200, 200}, {(int)A::DRight, 24, 240, 200},
    {(int)A::R, 736, 22, 200}, {(int)A::RAnalog, 736, 56, 200}, {(int)A::Z, 736, 90, 200},
    {(int)A::Y, 736, 142, 200}, {(int)A::X, 736, 182, 200}, {(int)A::A, 736, 222, 200}, {(int)A::B, 736, 262, 200},
    {(int)A::Start, 380, 6, 200},
  };
  const float c_x = cst_c.x / k - o.x / k;   // the C-stick's canvas x, so its box sits right under it
  (void)c_x; (void)c_mode_text;
  if (!analog_c) {
    boxes.push_back({(int)A::CUp, 216, 350, 128}); boxes.push_back({(int)A::CDown, 352, 350, 128});
    boxes.push_back({(int)A::CLeft, 488, 350, 128}); boxes.push_back({(int)A::CRight, 624, 350, 128});
    // Second row: the control stick, on the same condition. A device with a real analog stick
    // feeds it directly and has nothing to bind here.
    boxes.push_back({(int)A::SUp, 216, 392, 128}); boxes.push_back({(int)A::SDown, 352, 392, 128});
    boxes.push_back({(int)A::SLeft, 488, 392, 128}); boxes.push_back({(int)A::SRight, 624, 392, 128});
    // Third row: a box controller's own buttons. Binding Mod X or Mod Y turns the two rows above
    // into the box's Melee coordinates (input_bindings.h, apply_box_actions).
    boxes.push_back({(int)A::ModX, 216, 434, 128}); boxes.push_back({(int)A::ModY, 352, 434, 128});
    boxes.push_back({(int)A::LightShield, 488, 434, 128}); boxes.push_back({(int)A::MidShield, 624, 434, 128});
    // The stick modifier (Dolphin's Modifier): held, the button-made stick pushes part of the way.
    boxes.push_back({(int)A::StickMod, 760, 434, 128});
  }
  constexpr float kBoxH = 32;
  // Grey group panels behind the boxes.
  const ImU32 group = IM_COL32(172, 173, 182, 255);
  dl->AddRectFilled(P(14, 20), P(234, 106), group, 8 * k);     // L and L analog
  dl->AddRectFilled(P(14, 110), P(234, 282), group, 8 * k);
  dl->AddRectFilled(P(726, 14), P(946, 128), group, 8 * k);    // R, R analog and Z
  dl->AddRectFilled(P(726, 132), P(946, 304), group, 8 * k);
  if (!analog_c) dl->AddRectFilled(P(206, 342), P(762, 432), group, 8 * k);   // both direction rows

  int hovered = -1;
  if (const char* force = host::test_env::getenv("MELEE_TEST_HOVER")) hovered = std::atoi(force);   // screenshot hook
  if (canvas_hovered) {
    for (const GcCallout& b : boxes)
      if (mouse.x >= P(b.x, 0).x && mouse.x <= P(b.x + b.w, 0).x && mouse.y >= P(0, b.y).y && mouse.y <= P(0, b.y + kBoxH).y) { hovered = b.action; break; }
    if (hovered < 0) hovered = part_at_mouse();
  }

  // Where each callout points, and the cyan outline drawn round that part when it is active.
  auto target_of = [&](int action) -> ImVec2 {
    switch (action) {
      case (int)A::L: case (int)A::LAnalog: return C(440, 145);   // one bracket round L and L analog
      case (int)A::DUp: return C(528, 344 - 24);
      case (int)A::DDown: return C(528, 344 + 24);
      case (int)A::DLeft: return C(528 - 24, 344);
      case (int)A::DRight: return C(528 + 24, 344);
      case (int)A::R: case (int)A::RAnalog: case (int)A::Z: return C(790, 160);   // one bracket round R, R analog and Z
      case (int)A::A: case (int)A::B: case (int)A::X: case (int)A::Y: return C(800, 240);
      case (int)A::Start: return start_c;
      default: break;
    }
    if (action == kCStickModeBox || (action >= (int)A::CUp && action <= (int)A::CRight)) return ImVec2(cst_c.x, cst_c.y + 37 * kc);
    // The control stick's own boxes point at the stick, not the C-stick.
    if (action >= (int)A::SUp && action <= (int)A::SRight) return ImVec2(stick.x, stick.y + 37 * kc);
    // Mod X and Mod Y shape the control stick; the shield buttons press R.
    if (action == (int)A::ModX || action == (int)A::ModY || action == (int)A::StickMod) return ImVec2(stick.x, stick.y + 37 * kc);
    if (action == (int)A::LightShield || action == (int)A::MidShield) return C(790, 160);
    return C(476, 344);   // the D-pad arms: the diamond's left corner
  };
  auto outline_part = [&](int action, ImU32 col, float th) {
    if (action == (int)A::A) dl->AddCircle(a_c, 30 * kc, col, 40, th);
    else if (action == (int)A::B) dl->AddCircle(b_c, 19 * kc, col, 28, th);
    else if (action == (int)A::X) dl->AddEllipse(x_c, ImVec2((kXrx + 4) * kc, (kXry + 4) * kc), col, kXrot, 32, th);
    else if (action == (int)A::Y) dl->AddEllipse(y_c, ImVec2((kYrx + 4) * kc, (kYry + 4) * kc), col, kYrot, 32, th);
    else if (action >= (int)A::DUp && action <= (int)A::DRight) {
      // Just that direction's arm, lit and outlined.
      for (const Arm& a : arms) {
        if (a.action != action) continue;
        const ImVec2 lo(dpad_c.x + (a.x0 - 2) * kc, dpad_c.y + (a.y0 - 2) * kc), hi(dpad_c.x + (a.x1 + 2) * kc, dpad_c.y + (a.y1 + 2) * kc);
        dl->AddRectFilled(lo, hi, IM_COL32(255, 255, 255, 255), 2 * kc);
        dl->AddRect(lo, hi, col, 3 * kc, 0, th);
      }
    } else if (action == kCStickModeBox || (action >= (int)A::CUp && action <= (int)A::CRight))
      dl->AddNgon(cst_c, 37 * kc, col, 8, th);
    else if (action == (int)A::Start) dl->AddCircle(start_c, 14 * kc, col, 24, th);
    else if (action == (int)A::L || action == (int)A::LAnalog) dl->AddEllipse(C(451, 166), ImVec2(41 * kc, 25 * kc), col, -0.45f, 40, th);
    else if (action == (int)A::R || action == (int)A::RAnalog) dl->AddEllipse(C(747, 166), ImVec2(41 * kc, 25 * kc), col, 0.45f, 40, th);
    else if (action == (int)A::Z) dl->AddEllipse(C(746, 164), ImVec2(38 * kc, 17 * kc), col, 0.38f, 32, th);
  };

  // Leader lines: one per group, from the group's edge to its part, as on Ultimate's screen.
  auto active = [&](int action) { return action == hovered || action == capturing; };
  auto any_active = [&](std::initializer_list<int> list) { for (int a : list) if (active(a)) return true; return false; };
  const ImU32 white = IM_COL32(255, 255, 255, 255);
  {  // L and L analog share one bracket, the mirror of the right shoulder's
    const ImVec2 to = target_of((int)A::L);
    const ImU32 col = any_active({(int)A::L, (int)A::LAnalog}) ? white : cyan;
    const float x = P(244, 0).x, y0 = P(0, 46).y, y1 = P(0, 86).y;
    dl->AddLine(P(234, 46), ImVec2(x, y0), col, 2.0f * k);
    dl->AddLine(P(234, 86), ImVec2(x, y1), col, 2.0f * k);
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col, 2.0f * k);
    dl->AddLine(ImVec2(x, to.y), to, col, 2.0f * k);
    dl->AddCircleFilled(P(234, 46), 3.5f * k, col, 12);
    dl->AddCircleFilled(P(234, 86), 3.5f * k, col, 12);
  }
  {  // D-pad: across to the pad, then to the arm being pointed at
    int dir = -1;
    for (int a : {(int)A::DUp, (int)A::DDown, (int)A::DLeft, (int)A::DRight}) if (active(a)) dir = a;
    const ImVec2 from = P(234, 196), corner = C(490, 344);
    const ImU32 col = dir >= 0 ? white : cyan;
    dl->AddLine(from, ImVec2(corner.x - 24 * k, from.y), col, 2.0f * k);
    dl->AddLine(ImVec2(corner.x - 24 * k, from.y), corner, col, 2.0f * k);
    dl->AddLine(corner, dir >= 0 ? target_of(dir) : C(528 - 24, 344), col, 2.0f * k);
    dl->AddCircleFilled(from, 3.5f * k, col, 12);
  }
  {  // R, R analog and Z share one bracket
    const ImVec2 to = target_of((int)A::R);
    const ImU32 col = any_active({(int)A::R, (int)A::RAnalog, (int)A::Z}) ? white : cyan;
    const float x = P(716, 0).x, y0 = P(0, 38).y, y1 = P(0, 106).y;
    for (float y : {38.0f, 72.0f, 106.0f}) {   // the three rows' middles
      dl->AddLine(P(726, y), ImVec2(x, P(0, y).y), col, 2.0f * k);
      dl->AddCircleFilled(P(726, y), 3.5f * k, col, 12);
    }
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col, 2.0f * k);
    dl->AddLine(ImVec2(x, to.y), to, col, 2.0f * k);
  }
  {  // face buttons: across, then up to the right side of their frame
    const ImVec2 to = target_of((int)A::A);
    const ImU32 col = any_active({(int)A::A, (int)A::B, (int)A::X, (int)A::Y}) ? white : cyan;
    const float x = P(716, 0).x;
    dl->AddLine(P(726, 218), ImVec2(x, P(0, 218).y), col, 2.0f * k);
    dl->AddLine(ImVec2(x, P(0, 218).y), ImVec2(x, to.y), col, 2.0f * k);
    dl->AddLine(ImVec2(x, to.y), to, col, 2.0f * k);
    dl->AddCircleFilled(P(726, 218), 3.5f * k, col, 12);
  }
  dl->AddLine(P(480, 38), start_c, active((int)A::Start) ? white : cyan, 2.0f * k);
  if (analog_c) {}
  else dl->AddLine(ImVec2(cst_c.x, P(0, 342).y), target_of((int)A::CUp), any_active({(int)A::CUp, (int)A::CDown, (int)A::CLeft, (int)A::CRight}) ? white : cyan, 2.0f * k);
  // Cyan outlines round the hovered or waiting part (drawn over the controller).
  for (int a : {hovered, capturing})
    if (a >= 0) outline_part(a, a == capturing ? IM_COL32(255, 215, 60, 255) : cyan, 3.0f * k);

  // The boxes: an icon circle naming the GameCube input, then what it is bound to.
  auto icon = [&](ImVec2 c, int action) {
    ImU32 bg = IM_COL32(52, 52, 58, 255);
    const char* t = nullptr;
    switch (action) {
      case (int)A::A: bg = IM_COL32(40, 170, 75, 255); t = "A"; break;
      case (int)A::B: bg = IM_COL32(215, 40, 40, 255); t = "B"; break;
      case (int)A::X: t = "X"; break;
      case (int)A::Y: t = "Y"; break;
      case (int)A::Z: bg = IM_COL32(45, 75, 190, 255); t = "Z"; break;
      case (int)A::L: t = "L"; break;
      case (int)A::R: t = "R"; break;
      case (int)A::LAnalog: t = "La"; break;
      case (int)A::RAnalog: t = "Ra"; break;
      case (int)A::Start: t = "S"; break;
      case (int)A::ModX: bg = IM_COL32(110, 110, 120, 255); t = "MX"; break;
      case (int)A::ModY: bg = IM_COL32(110, 110, 120, 255); t = "MY"; break;
      case (int)A::LightShield: t = "LS"; break;
      case (int)A::MidShield: t = "MS"; break;
      default: break;
    }
    const bool is_c = action == kCStickModeBox || (action >= (int)A::CUp && action <= (int)A::CRight);
    if (is_c) bg = IM_COL32(215, 160, 20, 255);
    dl->AddCircleFilled(c, 13 * k, bg, 24);
    if (t) { centred_text(c, t, IM_COL32(255, 255, 255, 255)); return; }
    if (action == kCStickModeBox) { centred_text(c, "C", IM_COL32(90, 60, 0, 255)); return; }
    // Directions: a small plus (D-pad) or nothing (C), and a triangle pointing the way.
    int dir = 0;   // 0 up, 1 down, 2 left, 3 right
    if (action == (int)A::DDown || action == (int)A::CDown) dir = 1;
    else if (action == (int)A::DLeft || action == (int)A::CLeft) dir = 2;
    else if (action == (int)A::DRight || action == (int)A::CRight) dir = 3;
    const float s = 6.5f * k;
    ImVec2 t0, t1, t2;
    switch (dir) {
      case 0: t0 = ImVec2(c.x, c.y - s); t1 = ImVec2(c.x + s, c.y + s * 0.6f); t2 = ImVec2(c.x - s, c.y + s * 0.6f); break;
      case 1: t0 = ImVec2(c.x, c.y + s); t1 = ImVec2(c.x - s, c.y - s * 0.6f); t2 = ImVec2(c.x + s, c.y - s * 0.6f); break;
      case 2: t0 = ImVec2(c.x - s, c.y); t1 = ImVec2(c.x + s * 0.6f, c.y - s); t2 = ImVec2(c.x + s * 0.6f, c.y + s); break;
      default: t0 = ImVec2(c.x + s, c.y); t1 = ImVec2(c.x - s * 0.6f, c.y + s); t2 = ImVec2(c.x - s * 0.6f, c.y - s); break;
    }
    dl->AddTriangleFilled(t0, t1, t2, is_c ? IM_COL32(90, 60, 0, 255) : IM_COL32(230, 230, 235, 255));
  };
  for (const GcCallout& b : boxes) {
    const bool waiting = b.action == capturing, over = b.action == hovered;
    const bool down = b.action < 32 && ((live >> b.action) & 1);
    const ImVec2 a = P(b.x, b.y), z = P(b.x + b.w, b.y + kBoxH);
    const ImU32 fill = waiting ? ImGui::GetColorU32(ImVec4(0.48f, 0.18f, 0.88f, 0.85f + 0.15f * pulse)) : IM_COL32(248, 248, 250, 255);
    dl->AddRectFilled(a, z, fill, 9 * k);
    dl->AddRect(a, z, waiting ? IM_COL32(255, 215, 60, 255) : over ? cyan : IM_COL32(150, 150, 160, 255), 9 * k, 0, waiting || over ? 3.0f * k : 1.0f);
    if (down) dl->AddRectFilled(ImVec2(z.x - 8 * k, a.y + 6 * k), ImVec2(z.x - 4 * k, z.y - 6 * k), IM_COL32(40, 190, 80, 255), 2 * k);
    icon(ImVec2(a.x + 19 * k, (a.y + z.y) / 2), b.action);
    std::string text = waiting ? std::string("Press a button...")
                     : b.action == kCStickModeBox ? std::string(c_mode_text) : label(b.action);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float left = a.x + 38 * k, right = z.x - 10 * k;
    const float tx = std::max(left, (left + right - ts.x) / 2);
    dl->PushClipRect(ImVec2(left, a.y), ImVec2(right, z.y), true);
    dl->AddText(ImVec2(tx, (a.y + z.y - ts.y) / 2), waiting ? IM_COL32(255, 255, 255, 255) : IM_COL32(30, 30, 36, 255), text.c_str());
    dl->PopClipRect();
  }

  if (hovered_out) *hovered_out = hovered;
  if (right_clicked) *right_clicked = hovered >= 0 && hovered != kCStickModeBox && ImGui::IsItemClicked(ImGuiMouseButton_Right) ? hovered : -1;
  return hovered >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left) ? hovered : -1;
}

// The Switch Pro Controller, laid out as Smash Ultimate's button settings screen shows it: each
// physical button has a box naming what it does in Melee. Click a button or its box and pick from
// the list; right-click to clear. The right stick carries the C-stick mode, as the C-stick does on the
// GameCube picture. Authored on a 960x432 canvas.
constexpr float kSwCanvasH = 392;

bool draw_swpro_bind_picture(int index, uint16_t raw, ImVec2 lstick, ImVec2 rstick, float dz_main, float dz_c) {
  using A = host::BindAction;
  bool changed = false;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float k = std::clamp(avail / 960.0f, 0.5f, 1.4f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("sw_picture", ImVec2(960.0f * k, kSwCanvasH * k));
  const bool canvas_hovered = ImGui::IsItemHovered();
  const bool left_click = ImGui::IsItemClicked(ImGuiMouseButton_Left), right_click = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
  // Traced, not drawn by eye: the body is the silhouette of the Pro Controller in a 1280x720 capture
  // of Ultimate's button settings screen (left half, mirrored: boxes cover the right grip there), and
  // every part sits where it measures in that capture. Coordinates are that capture's pixels.
  constexpr float kP = 0.85f, kMid = 582.5f;
  auto S = [&](float x, float y) { return P(480 + (x - kMid) * kP, 70 + (y - 222) * kP); };
  const float kp = kP * k;   // a controller-space length in screen pixels
  const ImVec2 mouse = ImGui::GetMousePos();
  const ImU32 cyan = IM_COL32(40, 200, 225, 255), white = IM_COL32(255, 255, 255, 255);
  host::PadBindings& bind = host::g_swpro_bindings[(size_t)index];

  dl->AddRectFilled(P(0, 0), P(960, kSwCanvasH), IM_COL32(222, 223, 229, 255), 12 * k);

  // ---- the controller ----
  static const float left_half[][2] = {
    {582, 229}, {541, 229}, {502, 231}, {494, 228}, {476, 222}, {448, 224}, {424, 228}, {412, 232},
    {391, 245}, {385, 251}, {376, 265}, {365, 278}, {360, 289}, {351, 319}, {337, 393}, {336, 410},
    {333, 416}, {324, 470}, {320, 501}, {319, 523}, {322, 546}, {324, 553}, {330, 563}, {332, 566},
    {336, 571}, {341, 575}, {346, 579}, {357, 584}, {371, 585}, {382, 583}, {389, 579}, {399, 570},
    {413, 548}, {434, 505}, {443, 490}, {449, 484}, {456, 480}, {582, 479}};
  constexpr int kHalf = (int)(sizeof left_half / sizeof left_half[0]);
  ImVec2 pts[kHalf * 2];
  int n = 0;
  for (int i = 0; i < kHalf; ++i) pts[n++] = S(left_half[i][0], left_half[i][1]);
  for (int i = kHalf - 2; i >= 1; --i) pts[n++] = S(2 * kMid - left_half[i][0], left_half[i][1]);
  auto down = [&](uint16_t bit) { return (raw & bit) != 0; };
  const ImU32 shoulder = IM_COL32(70, 70, 76, 255), shoulder_on = IM_COL32(240, 150, 40, 255);
  // Triggers above the bumpers, bumpers peeking over the top edge (their places on the controller's
  // top edge: the bumps at x 424..500).
  dl->AddRectFilled(S(404, 182), S(500, 212), down(host::SWPRO_ZL) ? shoulder_on : IM_COL32(58, 58, 64, 255), 10 * kp);
  dl->AddRectFilled(S(665, 182), S(761, 212), down(host::SWPRO_ZR) ? shoulder_on : IM_COL32(58, 58, 64, 255), 10 * kp);
  dl->AddEllipseFilled(S(452, 228), ImVec2(58 * kp, 14 * kp), down(host::SWPRO_L) ? shoulder_on : shoulder, -0.05f, 32);
  dl->AddEllipseFilled(S(713, 228), ImVec2(58 * kp, 14 * kp), down(host::SWPRO_R) ? shoulder_on : shoulder, 0.05f, 32);
  // Filled a pixel row at a time between the edge crossings, then outlined on top.
  {
    float y0 = pts[0].y, y1 = pts[0].y;
    for (int i = 1; i < n; ++i) { y0 = std::min(y0, pts[i].y); y1 = std::max(y1, pts[i].y); }
    std::vector<float> xs;
    for (float y = std::floor(y0) + 0.5f; y < y1; y += 1.0f) {
      xs.clear();
      for (int i = 0; i < n; ++i) {
        const ImVec2 a = pts[i], b = pts[(i + 1) % n];
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
      }
      std::sort(xs.begin(), xs.end());
      for (size_t j = 0; j + 1 < xs.size(); j += 2)
        dl->AddRectFilled(ImVec2(xs[j], y - 0.5f), ImVec2(xs[j + 1], y + 0.5f), IM_COL32(60, 60, 64, 255));
    }
  }
  dl->AddPolyline(pts, n, IM_COL32(28, 28, 31, 255), ImDrawFlags_Closed, 2.0f * k);
  auto centred_text = [&](ImVec2 c, const char* t, ImU32 col) {
    const ImVec2 sz = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(c.x - sz.x / 2, c.y - sz.y / 2), col, t);
  };
  centred_text(S(452, 197), "ZL", IM_COL32(200, 200, 206, 255));
  centred_text(S(713, 197), "ZR", IM_COL32(200, 200, 206, 255));

  // ---- parts, at their measured centres, drawn as Ultimate draws them: black, white letters ----
  const ImVec2 ls = S(437, 330), dpad = S(497, 405), face = S(719, 332), rs = S(650, 406);
  const ImVec2 minus_c = S(518, 293), plus_c = S(646, 293);
  const ImU32 black = IM_COL32(22, 22, 24, 255);
  auto stick = [&](ImVec2 c, ImVec2 pos, float dz, bool c_stick) {
    const ImVec2 game = inside_deadzone(pos, dz) ? ImVec2(0, 0) : pos;
    const ImVec2 cap(c.x + game.x * 8 * kp, c.y - game.y * 8 * kp);
    dl->AddCircleFilled(cap, 29 * kp, black, 40);
    dl->AddCircle(cap, 20 * kp, IM_COL32(52, 52, 56, 255), 40, 2.0f * k);
    draw_deadzone_view(dl, c, 29 * kp, pos, dz, k, c_stick);
  };
  stick(ls, lstick, dz_main, false);
  stick(rs, rstick, dz_c, true);
  // D-pad: a black plus with white arrows.
  const float arm = 30, half = 10;
  struct Arm { uint16_t bit; float x0, y0, x1, y1; };
  const Arm arms[] = {{host::SWPRO_DPAD_UP, -half, -arm, half, -half}, {host::SWPRO_DPAD_DOWN, -half, half, half, arm},
                      {host::SWPRO_DPAD_LEFT, -arm, -half, -half, half}, {host::SWPRO_DPAD_RIGHT, half, -half, arm, half}};
  dl->AddRectFilled(ImVec2(dpad.x - half * kp, dpad.y - half * kp), ImVec2(dpad.x + half * kp, dpad.y + half * kp), black);
  for (const Arm& a : arms)
    dl->AddRectFilled(ImVec2(dpad.x + a.x0 * kp, dpad.y + a.y0 * kp), ImVec2(dpad.x + a.x1 * kp, dpad.y + a.y1 * kp),
                      down(a.bit) ? IM_COL32(120, 120, 128, 255) : black, 3 * kp);
  auto dpad_arrow = [&](float dx, float dy) {
    const ImVec2 tip(dpad.x + dx * 25 * kp, dpad.y + dy * 25 * kp), base(dpad.x + dx * 17 * kp, dpad.y + dy * 17 * kp);
    const ImVec2 side(-dy * 5.5f * kp, dx * 5.5f * kp);
    dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y), white);
  };
  dpad_arrow(0, -1); dpad_arrow(0, 1); dpad_arrow(-1, 0); dpad_arrow(1, 0);
  // Face buttons: X top, A right, B bottom, Y left (measured 37 to 39 px from the centre).
  struct Face { uint16_t bit; float dx, dy; const char* t; };
  const Face faces[] = {{host::SWPRO_X, 0, -37, "X"}, {host::SWPRO_A, 40, 0, "A"}, {host::SWPRO_B, 0, 38, "B"}, {host::SWPRO_Y, -39, 0, "Y"}};
  for (const Face& f : faces) {
    const ImVec2 c(face.x + f.dx * kp, face.y + f.dy * kp);
    dl->AddCircleFilled(c, 17 * kp, down(f.bit) ? white : black, 24);
    centred_text(c, f.t, down(f.bit) ? IM_COL32(20, 20, 20, 255) : white);
  }
  // Minus and Plus (grey dots on Ultimate's picture), Capture and Home (not bindable).
  const ImU32 dim_btn = IM_COL32(95, 95, 100, 255);
  dl->AddCircleFilled(minus_c, 11 * kp, down(host::SWPRO_MINUS) ? white : dim_btn, 20);
  dl->AddCircleFilled(plus_c, 11 * kp, down(host::SWPRO_PLUS) ? white : dim_btn, 20);
  dl->AddRectFilled(S(532, 325), S(550, 343), dim_btn, 2 * kp);
  dl->AddCircleFilled(S(616, 334), 11 * kp, dim_btn, 20);

  // ---- which physical button is under the mouse ----
  auto in_circle = [&](ImVec2 c, float r) { const float dx = mouse.x - c.x, dy = mouse.y - c.y; return dx * dx + dy * dy <= r * r * kp * kp; };
  auto in_rect = [&](ImVec2 a, ImVec2 z) { return mouse.x >= a.x && mouse.x <= z.x && mouse.y >= a.y && mouse.y <= z.y; };
  enum { kRStick = 0x10000 };
  auto part_at_mouse = [&]() -> int {
    for (const Face& f : faces) if (in_circle(ImVec2(face.x + f.dx * kp, face.y + f.dy * kp), 20)) return f.bit;
    for (const Arm& a : arms)
      if (in_rect(ImVec2(dpad.x + (a.x0 - 3) * kp, dpad.y + (a.y0 - 3) * kp), ImVec2(dpad.x + (a.x1 + 3) * kp, dpad.y + (a.y1 + 3) * kp))) return a.bit;
    if (in_circle(minus_c, 15)) return host::SWPRO_MINUS;
    if (in_circle(plus_c, 15)) return host::SWPRO_PLUS;
    if (in_rect(S(404, 182), S(500, 212))) return host::SWPRO_ZL;
    if (in_rect(S(665, 182), S(761, 212))) return host::SWPRO_ZR;
    if (in_rect(S(394, 213), S(510, 230))) return host::SWPRO_L;
    if (in_rect(S(655, 213), S(771, 230))) return host::SWPRO_R;
    return -1;
  };

  // ---- the boxes ----
  struct Box { int part; float x, y, w; const char* icon; };
  const Box boxes[] = {
    {host::SWPRO_ZL, 24, 40, 200, "ZL"}, {host::SWPRO_L, 24, 80, 200, "L"},
    {host::SWPRO_DPAD_UP, 24, 160, 200, nullptr}, {host::SWPRO_DPAD_DOWN, 24, 200, 200, nullptr},
    {host::SWPRO_DPAD_LEFT, 24, 240, 200, nullptr}, {host::SWPRO_DPAD_RIGHT, 24, 280, 200, nullptr},
    {host::SWPRO_ZR, 736, 40, 200, "ZR"}, {host::SWPRO_R, 736, 80, 200, "R"},
    {host::SWPRO_X, 736, 160, 200, "X"}, {host::SWPRO_Y, 736, 200, 200, "Y"},
    {host::SWPRO_A, 736, 240, 200, "A"}, {host::SWPRO_B, 736, 280, 200, "B"},
    {host::SWPRO_MINUS, 300, 4, 150, "-"}, {host::SWPRO_PLUS, 510, 4, 150, "+"},
  };
  constexpr float kBoxH = 32;
  const ImU32 group = IM_COL32(172, 173, 182, 255);
  dl->AddRectFilled(P(14, 30), P(234, 122), group, 8 * k);
  dl->AddRectFilled(P(14, 150), P(234, 322), group, 8 * k);
  dl->AddRectFilled(P(726, 30), P(946, 122), group, 8 * k);
  dl->AddRectFilled(P(726, 150), P(946, 322), group, 8 * k);

  int hovered = -1;
  if (canvas_hovered) {
    for (const Box& b : boxes)
      if (in_rect(P(b.x, b.y), P(b.x + b.w, b.y + kBoxH))) { hovered = b.part; break; }
    if (hovered < 0) hovered = part_at_mouse();
  }
  static int menu_part = -1;
  const bool menu_open = ImGui::IsPopupOpen("sw_assign");
  auto active = [&](int part) { return part == hovered || (menu_open && part == menu_part); };

  // What a button does: the Melee action it is bound to.
  auto action_of = [&](uint16_t bit) {
    for (int i = 0; i < (int)A::Count; ++i) if (!host::is_cstick_action(i) && bind.mask[i] == bit) return i;
    return -1;
  };
  auto target_of = [&](int part) -> ImVec2 {
    for (const Face& f : faces) if (f.bit == part) return ImVec2(face.x + f.dx * kp, face.y + f.dy * kp);
    for (const Arm& a : arms)
      if (a.bit == part) return ImVec2(dpad.x + (a.x0 + a.x1) / 2 * kp, dpad.y + (a.y0 + a.y1) / 2 * kp);
    switch (part) {
      case host::SWPRO_ZL: return S(404, 197);
      case host::SWPRO_L: return S(396, 226);
      case host::SWPRO_ZR: return S(761, 197);
      case host::SWPRO_R: return S(769, 226);
      case host::SWPRO_MINUS: return minus_c;
      case host::SWPRO_PLUS: return plus_c;
      case kRStick: return ImVec2(rs.x, rs.y + 32 * k);
      default: return ImVec2(0, 0);
    }
  };
  auto line_col = [&](std::initializer_list<int> parts) { for (int p : parts) if (active(p)) return white; return cyan; };
  auto leader = [&](ImVec2 from, ImVec2 to, ImU32 col) {
    const ImVec2 elbow(to.x, from.y);
    dl->AddLine(from, elbow, col, 2.0f * k);
    dl->AddLine(elbow, to, col, 2.0f * k);
    dl->AddCircleFilled(from, 3.5f * k, col, 12);
  };
  leader(P(224, 56), target_of(host::SWPRO_ZL), line_col({host::SWPRO_ZL}));
  leader(P(224, 96), target_of(host::SWPRO_L), line_col({host::SWPRO_L}));
  leader(P(736, 56), target_of(host::SWPRO_ZR), line_col({host::SWPRO_ZR}));
  leader(P(736, 96), target_of(host::SWPRO_R), line_col({host::SWPRO_R}));
  {  // D-pad group: to the pad, then to the arm being pointed at
    int dir = -1;
    for (const Arm& a : arms) if (active(a.bit)) dir = a.bit;
    const ImU32 col = dir >= 0 ? white : cyan;
    const ImVec2 from = P(234, 236), corner(dpad.x - 50 * kp, from.y);
    dl->AddLine(from, corner, col, 2.0f * k);
    dl->AddLine(corner, dir >= 0 ? target_of(dir) : ImVec2(dpad.x - 30 * kp, dpad.y), col, 2.0f * k);
    dl->AddCircleFilled(from, 3.5f * k, col, 12);
  }
  {  // face buttons: to the cluster, then to the button being pointed at
    int btn = -1;
    for (const Face& f : faces) if (active(f.bit)) btn = f.bit;
    const ImU32 col = btn >= 0 ? white : cyan;
    const ImVec2 from = P(726, 236), corner(face.x + 80 * kp, from.y), edge(face.x + 60 * kp, face.y);
    dl->AddLine(from, corner, col, 2.0f * k);
    dl->AddLine(corner, edge, col, 2.0f * k);
    if (btn >= 0) dl->AddLine(edge, target_of(btn), col, 2.0f * k);
    dl->AddCircleFilled(from, 3.5f * k, col, 12);
  }
  dl->AddLine(P(375, 36), P(375, 60), line_col({host::SWPRO_MINUS}), 2.0f * k);
  dl->AddLine(P(375, 60), minus_c, line_col({host::SWPRO_MINUS}), 2.0f * k);
  dl->AddLine(P(585, 36), P(585, 60), line_col({host::SWPRO_PLUS}), 2.0f * k);
  dl->AddLine(P(585, 60), plus_c, line_col({host::SWPRO_PLUS}), 2.0f * k);

  // Cyan outline round the part being pointed at.
  for (int part : {hovered, menu_open ? menu_part : -1}) {
    if (part < 0) continue;
    const ImU32 col = cyan; const float th = 3.0f * k;
    bool drawn = false;
    for (const Face& f : faces) if (f.bit == part) { dl->AddCircle(ImVec2(face.x + f.dx * kp, face.y + f.dy * kp), 21 * kp, col, 24, th); drawn = true; }
    for (const Arm& a : arms)
      if (a.bit == part) { dl->AddRect(ImVec2(dpad.x + (a.x0 - 2) * kp, dpad.y + (a.y0 - 2) * kp), ImVec2(dpad.x + (a.x1 + 2) * kp, dpad.y + (a.y1 + 2) * kp), col, 3 * kp, 0, th); drawn = true; }
    if (drawn) continue;
    if (part == host::SWPRO_ZL) dl->AddRect(S(400, 178), S(504, 216), col, 10 * kp, 0, th);
    else if (part == host::SWPRO_ZR) dl->AddRect(S(661, 178), S(765, 216), col, 10 * kp, 0, th);
    else if (part == host::SWPRO_L) dl->AddEllipse(S(452, 228), ImVec2(62 * kp, 18 * kp), col, -0.05f, 32, th);
    else if (part == host::SWPRO_R) dl->AddEllipse(S(713, 228), ImVec2(62 * kp, 18 * kp), col, 0.05f, 32, th);
    else if (part == host::SWPRO_MINUS) dl->AddCircle(minus_c, 15 * kp, col, 20, th);
    else if (part == host::SWPRO_PLUS) dl->AddCircle(plus_c, 15 * kp, col, 20, th);
    else if (part == kRStick) dl->AddCircle(rs, 34 * k, col, 32, th);
  }

  for (const Box& b : boxes) {
    const bool over = active(b.part);
    const ImVec2 a = P(b.x, b.y), z = P(b.x + b.w, b.y + kBoxH);
    dl->AddRectFilled(a, z, IM_COL32(248, 248, 250, 255), 9 * k);
    dl->AddRect(a, z, over ? cyan : IM_COL32(150, 150, 160, 255), 9 * k, 0, over ? 3.0f * k : 1.0f);
    const ImVec2 ic(a.x + 19 * k, (a.y + z.y) / 2);
    dl->AddCircleFilled(ic, 13 * k, IM_COL32(52, 52, 58, 255), 24);
    if (b.icon) centred_text(ic, b.icon, white);
    else {   // a D-pad direction: a triangle pointing the way
      const float t = 6.5f * k;
      ImVec2 p0, p1, p2;
      if (b.part == host::SWPRO_DPAD_UP) { p0 = ImVec2(ic.x, ic.y - t); p1 = ImVec2(ic.x + t, ic.y + t * 0.6f); p2 = ImVec2(ic.x - t, ic.y + t * 0.6f); }
      else if (b.part == host::SWPRO_DPAD_DOWN) { p0 = ImVec2(ic.x, ic.y + t); p1 = ImVec2(ic.x - t, ic.y - t * 0.6f); p2 = ImVec2(ic.x + t, ic.y - t * 0.6f); }
      else if (b.part == host::SWPRO_DPAD_LEFT) { p0 = ImVec2(ic.x - t, ic.y); p1 = ImVec2(ic.x + t * 0.6f, ic.y - t); p2 = ImVec2(ic.x + t * 0.6f, ic.y + t); }
      else { p0 = ImVec2(ic.x + t, ic.y); p1 = ImVec2(ic.x - t * 0.6f, ic.y + t); p2 = ImVec2(ic.x - t * 0.6f, ic.y - t); }
      dl->AddTriangleFilled(p0, p1, p2, white);
    }
    std::string text;
    if (b.part == kRStick) text = "Smash Attack";
    else { const int act = action_of((uint16_t)b.part); text = act >= 0 ? kActionTitles[act] : "Nothing"; }
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float left = a.x + 38 * k, right = z.x - 10 * k;
    dl->PushClipRect(ImVec2(left, a.y), ImVec2(right, z.y), true);
    dl->AddText(ImVec2(std::max(left, (left + right - ts.x) / 2), (a.y + z.y - ts.y) / 2), IM_COL32(30, 30, 36, 255), text.c_str());
    dl->PopClipRect();
  }

  // ---- clicks: the stick switches the C-stick mode, a button opens the list of Melee buttons ----
  if (hovered == kRStick) {
    ImGui::SetTooltip("Right stick: the C-stick (smash attacks).");
  } else if (hovered >= 0) {
    ImGui::SetTooltip("Click to choose what this button does. Right-click to clear it.");
    if (left_click) { menu_part = hovered; ImGui::OpenPopup("sw_assign"); }
    if (right_click) {
      for (int i = 0; i < (int)A::Count; ++i) if (!host::is_cstick_action(i) && bind.mask[i] == (uint16_t)hovered) { bind.mask[i] = 0; changed = true; }
    }
  }
  if (ImGui::BeginPopup("sw_assign")) {
    const uint16_t bit = (uint16_t)menu_part;
    const int current = action_of(bit);
    ImGui::TextDisabled("This button does");
    ImGui::Separator();
    if (settings_widgets::Selectable("Nothing", current < 0)) {
      for (int i = 0; i < (int)A::Count; ++i) if (!host::is_cstick_action(i) && bind.mask[i] == bit) bind.mask[i] = 0;
      changed = true;
    }
    for (int i = 0; i < (int)A::Count; ++i) {
      if (host::is_cstick_action(i)) continue;
      if (settings_widgets::Selectable(kActionTitles[i], i == current)) {
        // One button per Melee button: this button takes the action, and stops doing any other.
        for (int j = 0; j < (int)A::Count; ++j) if (!host::is_cstick_action(j) && bind.mask[j] == bit) bind.mask[j] = 0;
        bind.mask[i] = bit;
        changed = true;
      }
    }
    ImGui::EndPopup();
  }
  return changed;
}

}  // namespace gx
