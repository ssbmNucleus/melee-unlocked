// Render-only settings presentation. The values and saving are the settings store's (settings_store.h).
// Mockup provenance: ui_sources/mockups/ORIGIN.md. No game state is written here.
// The settings window's widgets (was pc_settings_visuals.inl): the hit buttons and focus frames, the
// fonts and accent, the setting rows (toggle, combo, slider), the page footer, and the small helpers
// several pages share. The search and the sections the rows answer to are pc_settings_search.cpp.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

bool settings_hit_button(const char* id, ImVec2 size,
                         ImGuiButtonFlags flags) {
  const bool pressed = ImGui::InvisibleButton(id, size, flags);
  settings_page::label_item(id);
  return pressed;
}

// Close (X) buttons are mouse-only. Keyboard/controller focus landing on one meant pressing A
// closed the whole panel, and the same A then reached the game as a fresh press.
bool settings_close_hit(const char* id, ImVec2 size) {
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  const bool pressed = ImGui::InvisibleButton(id, size, 0);
  ImGui::PopItemFlag();
  return pressed;
}

// Shaped buttons suppress the rectangular nav cursor; callers outline the painted face.
bool settings_shaped_hit_button(const char* id, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
  const bool pressed = settings_hit_button(id, size);
  ImGui::PopStyleColor();
  return pressed;
}

// Whether ImGui would have drawn its focus frame on the last item (same test as RenderNavCursor).
bool settings_nav_frame_on_item() {
  const ImGuiContext& g = *GImGui;
  return g.NavCursorVisible && g.NavId != 0 && g.LastItemData.ID == g.NavId &&
         !ImGui::GetCurrentWindow()->DC.NavHideHighlightOneFrame;
}

// Two-pixel focus outline on the exact face, with a light tint of this panel's accent.
void settings_nav_frame_outline(ImDrawList* draw, const ImVec2* outline, int count) {
  const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(ImGuiCol_NavCursor));
  const ImU32 light = ImGui::ColorConvertFloat4ToU32(
      ImVec4(.75f + accent.x * .25f, .75f + accent.y * .25f, .75f + accent.z * .25f, 1.0f));
  draw->AddPolyline(outline, count, light, ImDrawFlags_Closed, 2.0f);
}

void settings_nav_frame(ImDrawList* draw, const ImVec2 (&quad)[4]) {
  settings_nav_frame_outline(draw, quad, 4);
}

// Hovering a category must not change the active page. Only an explicit
// navigation move may transfer selection to ImGui's focused category item.
bool settings_nav_input_pressed() {
  return ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ||
         ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ||
         ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
         ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Tab, false);
}

ImFont* g_settings_heading_font = nullptr;
ImFont* g_settings_body_font = nullptr;
ImFont* g_settings_old_font = nullptr;
ImFont* g_settings_old_vector_font = nullptr;
// The 0.6.61 font. It is a pixel font: exact at its own size, uneven when a scaled frame draws it
// at 1.33 or 2.5 times that, so a scaled frame takes the outline version of the same design.
ImFont* settings_old_font() {
  return g_frame_ui_scale.load(std::memory_order_relaxed) > 1.0f && g_settings_old_vector_font ?
      g_settings_old_vector_font : g_settings_old_font;
}
// The window's one accent: Clean side's pink.
ImU32 settings_accent() { return IM_COL32(247,38,133,255); }


void settings_hint(const char* format, ...) {
  va_list args;
  va_start(args, format);
  ImGui::TextDisabledV(format,args);
  va_end(args);
}

ImFont* settings_heading_font() {
  return g_settings_heading_font ? g_settings_heading_font : ImGui::GetFont();
}

// Back: from a page to the category list, and from the list out of the window.
void settings_back_page(SettingsState& state) {
  if (state.clean_detail_open) state.clean_detail_open = false;
  else state.open = false;
}

void settings_slanted_text(ImDrawList* draw, ImFont* font, float size,
                           ImVec2 pos, ImU32 color, const char* text, float slope);

// A page's footer: the B BACK chip, which goes back to the category list.
void settings_page_footer(SettingsState& state) {
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float top = p.y+s.y-34;
  const float w = (s.x-78.0f)*.5f;
  const float x = p.x+11.0f;
  ImGui::SetCursorScreenPos(ImVec2(x,top-5));
  const bool clicked=settings_shaped_hit_button("##page_back",ImVec2(w,38.0f));
  const bool nav_frame=settings_nav_frame_on_item();
  const bool hovered=ImGui::IsItemHovered()||ImGui::IsItemFocused();
  state.footer_hover[0]+=((hovered?1.0f:0.0f)-state.footer_hover[0])*
                         std::min(1.0f,ImGui::GetIO().DeltaTime*16.0f);
  const float pop=state.footer_hover[0]*5.0f;
  const ImVec2 card[]={ImVec2(x+12,top+5-pop),ImVec2(x+w,top-7-pop),
                       ImVec2(x+w-10,top+23-pop),ImVec2(x,top+34-pop)};
  const ImVec2 shadow[]={ImVec2(card[0].x+2,card[0].y+4),ImVec2(card[1].x+2,card[1].y+4),
                         ImVec2(card[2].x+2,card[2].y+4),ImVec2(card[3].x+2,card[3].y+4)};
  draw->AddConvexPolyFilled(shadow,4,IM_COL32(0,0,0,105));
  draw->AddConvexPolyFilled(card,4,IM_COL32(154,17,95,248));
  draw->AddLine(card[0],card[1],hovered?IM_COL32(255,224,246,255):IM_COL32(255,135,204,220),1.8f);
  if (nav_frame) settings_nav_frame(draw,card);
  settings_slanted_text(draw,settings_heading_font(),13.0f,ImVec2(x+18.0f,top+6-pop),
                        IM_COL32(255,245,251,255),"B  BACK",.08f);
  if (clicked) settings_back_page(state);
}

// Label above value: narrow drawers and large desktop pages use the same readable rhythm.
// A hidden label remains hidden for controls that already have a heading in their parent.
static void settings_field_label(const char* label) {
  if (ImGui::GetCurrentWindow()->DC.IsSameLine)
    ImGui::NewLine();
  if (label[0] != '#') {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float x=ImGui::GetCursorPosX(), width=ImGui::GetContentRegionAvail().x;
    ImDrawList* draw=ImGui::GetWindowDrawList();
    constexpr float label_h=26.0f;
    draw->AddRectFilled(start,ImVec2(start.x+width,start.y+label_h),IM_COL32(18,20,35,224),7.0f);
    draw->AddRectFilled(start,ImVec2(start.x+3,start.y+label_h),settings_accent(),2.0f);
    draw->AddText(settings_heading_font(),15.0f,ImVec2(start.x+12,start.y+4),
                  IM_COL32(229,235,245,255),label);
    ImGui::SetCursorPos(ImVec2(x,ImGui::GetCursorPosY()+label_h+5.0f));
    ImGui::SetNextItemWidth(width);
    return;
  }
  if (label[1] != '#') ImGui::TextUnformatted(label);
  ImGui::SetNextItemWidth(-1.0f);
}

static bool settings_toggle_draw(const char* label, bool* value);
// Every setting row goes through these three: a search shows only the matching ones, a closed section
// none (settings_row_begin above).
bool settings_toggle(const char* label, bool* value) {
  const SettingsRow row = settings_row_begin(label);
  if (!row.shown) return false;
  const bool changed = settings_toggle_draw(label, value);
  settings_row_end(row);
  return changed;
}
static bool settings_toggle_draw(const char* label, bool* value) {
  if (ImGui::GetCurrentWindow()->DC.IsSameLine)
    ImGui::NewLine();
  ImGui::PushID(label);
  const float width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
  const float text_width = std::max(65.0f, width - 68.0f);
  const ImVec2 text_size = ImGui::CalcTextSize(label, nullptr, true, text_width);
  const float height = std::max(36.0f, text_size.y + 16.0f);
  // Clean side paints this row as a slanted strip; its focus frame follows the strip.
  const bool clicked = settings_shaped_hit_button("##switch", ImVec2(width, height));
  settings_page::label_item(label);
  const bool nav_frame = settings_nav_frame_on_item();
  if (clicked) *value = !*value;
  const bool focused = ImGui::IsItemHovered() || ImGui::IsItemFocused();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 strip[]={ImVec2(a.x+7,a.y),ImVec2(b.x,a.y-4),
                        ImVec2(b.x-4,b.y-4),ImVec2(a.x,b.y)};
  draw->AddConvexPolyFilled(strip,4,IM_COL32(10,12,27,205));
  draw->AddLine(strip[0],strip[1],IM_COL32(215,49,144,160),1.0f);
  if (nav_frame) settings_nav_frame(draw,strip);
  if (focused) draw->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(1,1,1,0.045f)), 5.0f);
  draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x+12.0f, a.y + 8.0f), ink,
                label, nullptr, text_width);
  const ImVec2 start(b.x - 48.0f, a.y + (height - 24.0f) * 0.5f);
  const ImVec2 end(b.x - 2.0f, start.y + 24.0f);
  const ImU32 accent = settings_accent();
  draw->AddRectFilled(start, end, *value ? accent : IM_COL32(63, 74, 91, 255), 12.0f);
  if (focused) draw->AddRect(start, end, IM_COL32(229, 237, 251, 210), 12.0f, 0, 1.5f);
  const ImGuiID id = ImGui::GetID("position");
  float position = ImGui::GetStateStorage()->GetFloat(id, *value ? 1.0f : 0.0f);
  position += ((*value ? 1.0f : 0.0f) - position) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
  ImGui::GetStateStorage()->SetFloat(id, position);
  draw->AddCircleFilled(ImVec2(start.x + 12.0f + position * 22.0f, start.y + 12.0f),
                        8.5f, IM_COL32(248, 250, 255, 255), 24);
  ImGui::PopID();
  return clicked;
}

static bool settings_combo_draw(const char* label, int* value, const char* const items[], int count);
bool settings_combo(const char* label, int* value, const char* const items[], int count) {
  const SettingsRow row = settings_row_begin(label);
  if (!row.shown) return false;
  const bool changed = settings_combo_draw(label, value, items, count);
  settings_row_end(row);
  return changed;
}
static bool settings_combo_draw(const char* label, int* value, const char* const items[], int count) {
  ImGui::PushID(label);
  settings_field_label(label);
  const bool result = ImGui::Combo("##value", value, items, count);
    settings_page::label_item(label);
  ImGui::PopID();
  return result;
}

static bool settings_slider_draw(const char* label, int* value, int low, int high, const char* format, ImGuiSliderFlags flags);
bool settings_slider(const char* label, int* value, int low, int high,
                     const char* format, ImGuiSliderFlags flags) {
  const SettingsRow row = settings_row_begin(label);
  if (!row.shown) return false;
  const bool changed = settings_slider_draw(label, value, low, high, format, flags);
  settings_row_end(row);
  return changed;
}
static bool settings_slider_draw(const char* label, int* value, int low, int high, const char* format, ImGuiSliderFlags flags) {
  ImGui::PushID(label);
  settings_field_label(label);
  const bool result = ImGui::SliderInt("##value", value, low, high, format, flags);
    settings_page::label_item(label);
  ImGui::PopID();
  return result;
}

void settings_slanted_text(ImDrawList* draw, ImFont* font, float size,
                           ImVec2 pos, ImU32 color, const char* text, float slope = 0.12f) {
  const int begin=draw->VtxBuffer.Size;
  draw->AddText(font,size,pos,color,text);
  for (int v=begin;v<draw->VtxBuffer.Size;++v) {
    ImVec2& p=draw->VtxBuffer[v].pos;
    p.y-=(p.x-pos.x)*slope;
    p.x+=(size-(p.y-pos.y))*0.14f;
  }
}

// Tooltips are clipped to the OS window, and the standalone settings window is deliberately narrow,
// so a long one ran off the right edge. Wrapping them at a fixed width keeps every line on screen
// whatever the window size.
void wrapped_tooltip(const char* text) {
  if (!ImGui::IsItemHovered()) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(380.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// "+ Add Gecko code": paste in a code without finding and editing GeckoCodes.ini by hand. A pasted
// block may carry its own "$Name" line (Dolphin's format, what most sites hand out); if it does, that
// name is used and the typed one is just what is offered until then. On both engines' code pages.
// True when a code was added (the settings are then saved).
bool gecko_add_code_button() {
  bool changed = false;
      static char add_name[64] = "";
      static char add_body[32768] = "";   // room for the longest codes going around (a few hundred lines)
      static std::string add_error;
      if (settings_widgets::Button("+ Add Gecko code")) { add_name[0] = 0; add_body[0] = 0; add_error.clear(); ImGui::OpenPopup("add_gecko_code"); }
      if (ImGui::BeginPopup("add_gecko_code")) {
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        settings_widgets::InputText("##gecko_name", add_name, sizeof add_name);
        ImGui::TextUnformatted("Code lines: paste them here (XXXXXXXX YYYYYYYY, one pair per line).");
        ImGui::TextUnformatted("If the paste starts with a $Name line, that name is used.");
        settings_widgets::InputTextMultiline("##gecko_body", add_body, sizeof add_body, ImVec2(440.0f, 180.0f));
        if (!add_error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", add_error.c_str());
        if (settings_widgets::Button("Add")) {
          add_error = user_gecko::add(add_name, add_body);
          if (add_error.empty()) { user_gecko::save(); changed = true; ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine();
        if (settings_widgets::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
  return changed;
}



// The hover line that says so, only for a player who has 20XX TE.
const char* te_pair_hover(uint32_t bit) {
  if (!source_port::mods::status().te_owned) return "";
  return bit == kTeNoScreenRumble
      ? "\nWith 20XX TE on, this is also TE's \"Disable screen rumble\" (Mods):\nswitching one switches both."
      : "\nWith 20XX TE on, this is also TE's \"Input display\" (Mods):\nswitching one switches both.";
}

// Settings that only one engine runs follow one rule on every page: the control stays where it is,
// greyed (the caller wraps it in BeginDisabled), and hovering it says which engine runs it and why.
// No separate greyed line in its place, and never hidden.
void engine_only_reason(const char* format, ...) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  va_list args;
  va_start(args, format);
  ImGui::SetTooltipV(format, args);
  va_end(args);
}

// One display choice on Game, Mods and the built-in codes page. TE choices use its native
// effect; the Nucleus Unlocked choice stays in the renderer and is safe online. Locked TE choices stay locked.
bool settings_lcancel_flash(RenderOptions& options) {
  static const char* modes[] = {"Off", PRODUCT_NAME ": missed (red)", "TE: missed (red)", "TE: success", "TE: both",
                               PRODUCT_NAME ": success (green)", PRODUCT_NAME ": both (red / green)"};
  const bool have_te = options.native_source && source_port::mods::status().te_owned;
  const bool locked = have_te && (options.te_options2 & kTeLockSettings) != 0;
  int mode = mu_lcancel_flash_mode(options.te_options2, lcancel::flash_mode());
  bool changed = false;
  if (settings_widgets::BeginCombo("L-cancel flash", modes[mode])) {
    for (int i : {0, 1, 5, 6, 2, 3, 4}) {
      const bool te_mode = i >= MU_LCFLASH_TE_MISSED && i <= MU_LCFLASH_TE_BOTH;
      const bool unavailable = te_mode && !have_te;
      ImGui::BeginDisabled(unavailable || (te_mode && locked));
      if (settings_widgets::Selectable(modes[i], i == mode)) {
        mode = i;
        options.te_options2 = mu_lcancel_with_flash_mode(options.te_options2, mode);
        RenderOptions::live_te_options2() = options.te_options2;
        if (te_mode) {
          options.te_options |= 0x10;  // Selecting a TE effect also enables its feature master.
          RenderOptions::live_te_options() = options.te_options;
        }
        lcancel::set_flash_mode(mode);
        changed = true;
      }
#ifdef MELEE_NO_SLIPPI   // one engine: only the save can be missing
      if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Requires a loaded 20XX TE save.");
#else
      if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Requires the Source Port and a loaded 20XX TE save.");
#endif
      if (te_mode && locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Unlock 20XX TE settings before selecting a TE effect.");
      ImGui::EndDisabled();
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(PRODUCT_NAME ": red on a miss, green on success, or both. Available without 20XX TE.\n"
                      "Rendered outside the game; safe online.\n"
                      "TE: the game's own effect, offline and outside Tournament Mode or TM-CE exercises.\n"
                      "TE success and missed are separate choices; Both combines them.\n"
                      "The same choice appears on Game, Mods and the built-in codes page.%s",
                      locked ? "\n20XX TE effects are locked; " PRODUCT_NAME " effects remain available." : "");
  if (mode == MU_LCFLASH_TE_SUCCESS || mode == MU_LCFLASH_TE_BOTH) {
    ImGui::BeginDisabled(locked);
    int color = mu_lcancel_success_color(options.te_options2);
    static const char* colors[] = {"Off", "White", "Green"};
    if (settings_combo("Success flash", &color, colors, 3)) {
      options.te_options2 = mu_lcancel_with_success_color(options.te_options2, color);
      RenderOptions::live_te_options2() = options.te_options2;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Color on a successful L-cancel. Off leaves missed flashes unchanged.\n"
                        "White is TE's original success color; Green changes only the color.");
    ImGui::EndDisabled();
  }
  return changed;
}

// The Advanced group at the bottom of Video (0) and Overlays (1). Returns whether it is open; opening
// or closing it counts as a change, so the choice is saved with the other settings.
bool settings_advanced_group(int which, bool* changed) {
  ImGui::Spacing();
  {
    // Screenshot tooling (MELEE_SETTINGS_SCROLL_TO=advanced): bring the group into view once.
    static int scroll_frames[2] = {0, 0};
    if (scroll_frames[which] < 30) {
      const char* target = host::test_env::getenv("MELEE_SETTINGS_SCROLL_TO");
      if (target && std::strcmp(target, "advanced") == 0) ImGui::SetScrollHereY(0.0f);
      ++scroll_frames[which];
    }
  }
  ImGui::SetNextItemOpen(settings_store::panel_values().advanced_open[which], ImGuiCond_Always);
  const bool open = settings_widgets::CollapsingHeader(which == 0 ? "Advanced##video_advanced" : "Advanced##overlays_advanced");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s\nClosed until you open it; it stays open after that.",
                      which == 0 ? "Diagnostics and texture dumping for pack makers."
                                 : "Developer readouts.");
  if (open != settings_store::panel_values().advanced_open[which]) { settings_store::panel_values().advanced_open[which] = open; *changed = true; }
  return open;
}

}  // namespace gx
