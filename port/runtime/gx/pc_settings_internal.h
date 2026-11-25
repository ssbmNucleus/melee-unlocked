// The settings window's shared declarations: what its files use from one another. Internal to the
// window: pc_settings.cpp (its frame and settings_frame), the pc_settings_<job>.cpp files beside it,
// and the pages, settings_page_<page>.cpp. Code outside the window uses pc_settings.h; the values the
// window edits are the settings store's (settings_store.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "walter_egg.h"
#include "settings_search.h"
#include "test_env.h"
#include "pc_settings.h"
#include "settings_store.h"      // the values the panel shows, and the settings file
#include "settings_controls.h"   // controller tables, port devices, profiles
#include "settings_imgui_input.h"
#include "settings_page_navigation.h"
#ifdef __APPLE__
#include "../../mac/platform_paths.h"
#endif
#include <mutex>
#include <thread>
#include "gx_streamline.h"
#ifdef _WIN32
#include "gx_fsr.h"
#endif
#include <unordered_map>
#include "texture_pack.h"
#include "gx_retro.h"
#include "video_background.h"
#include "user_shader.h"
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cfloat>
#include <sstream>
#include <sstream>
#include "pc_settings_shared.h"
#include "gx_backend.h"
#include "gx_vulkan.h"
#include "gx_shader.h"
#include "gx_d3d12.h"
#ifdef MELEE_NO_SLIPPI
#include "netplay_state.h"   // the same names, answered from the neutral netplay state
#else
#include "jukebox.h"
#include "slippi_online.h"
#include "slippi_spectate.h"
#include "native_practice.h"
#endif
#include "practice_cpu.h"
#include "window.h"
#include "audio.h"
#include "host.h"
#include "early_input.h"
#include "input_bindings.h"
#include "settings_nav.h"
#include "lcancel.h"
#include "../abi/mu_lcancel_flash.h"
#include "user_gecko.h"

#include "lag_test_codes.h"
#include "gecko_data.h"
#include <chrono>
#include "hid_pad.h"
#include "phob_menu.h"
#include "optional_downloads.h"
#include "opponent_skins.h"   // the "opponentskins" setting
#include "lobby_directory.h"  // the "doubleslobbies" setting
#include "discord_presence.h"

#include "controller_profiles.h"
#include "settings_profile_bindings.h"
#include "dolphin_profile.h"
#include "cosmetic_mods.h"
#include "hackpack_ai.h"
#include "mod_profile.h"
#include "mod_scan.h"
#include "training_overlay.h"
#include "net_overlay.h"
#include "replay_bar.h"
#include "replay_control.h"
#include "replay_coach.h"
#include "replay_hud.h"
#include "practice_menu.h"
#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <commdlg.h>
#include <xinput.h>
#include <mmsystem.h>
#include <mmsystem.h>
#ifdef _WIN32
#include <d3d12.h>
#include <wrl/client.h>
#else
#include "backends/imgui_impl_sdl2.h"
#include <SDL.h>
#endif
#include <array>
#include <functional>
#include <map>
#include <string>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
namespace gx {

// ---- pc_settings.cpp: the window's frame, its ImGui context, settings_frame ----
extern std::atomic<bool> g_textures_dirty;   // a texture pack was switched on or off (settings_textures_dirty)
extern std::atomic<float> g_frame_ui_scale;  // the scale the last frame was laid out at
extern ImVec2 g_frame_pixel_size;            // the window's client area in pixels, before the division
// Menu art beside the game (ui_sources) and PNG pictures, as ImGui textures cached for the session.
std::string ui_source_asset_path(const char* source, const char* filename);
ImTextureData* cosmetic_preview(const std::string& path);

// ---- pc_settings_widgets.cpp: the setting rows, the window's look, small helpers the pages share ----
bool settings_hit_button(const char* id, ImVec2 size,
                         ImGuiButtonFlags flags = ImGuiButtonFlags_EnableNav);
bool settings_close_hit(const char* id, ImVec2 size);
bool settings_shaped_hit_button(const char* id, ImVec2 size);
bool settings_nav_frame_on_item();
void settings_nav_frame_outline(ImDrawList* draw, const ImVec2* outline, int count);
void settings_nav_frame(ImDrawList* draw, const ImVec2 (&quad)[4]);
bool settings_nav_input_pressed();
extern ImFont* g_settings_heading_font;
extern ImFont* g_settings_body_font;
extern ImFont* g_settings_old_font;
extern ImFont* g_settings_old_vector_font;
ImFont* settings_old_font();
ImU32 settings_accent();

// The window's interaction colors, including popups and focus.
struct SettingsAppearanceScope {
  SettingsAppearanceScope() {
    const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(settings_accent());
    const ImVec4 muted(0.17f,0.06f,0.14f,1);
    ImGui::PushStyleColor(ImGuiCol_Button,muted);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,accent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,muted);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(accent.x*.4f,accent.y*.4f,accent.z*.4f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,muted);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(0.95f,0.96f,1,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,accent);
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(accent.x*.55f,accent.y*.55f,accent.z*.55f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,accent);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,accent);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,ImVec4(0.04f,0.055f,0.085f,0.99f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor,accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,4.0f);
  }
  ~SettingsAppearanceScope() { ImGui::PopStyleVar(); ImGui::PopStyleColor(14); }
};

void settings_hint(const char* format, ...);
ImFont* settings_heading_font();
void settings_back_page(SettingsState& state);
void settings_slanted_text(ImDrawList* draw, ImFont* font, float size,
                           ImVec2 pos, ImU32 color, const char* text, float slope);   // slope: 0.12f where it is defined
void settings_page_footer(SettingsState& state);
bool settings_toggle(const char* label, bool* value);
bool settings_combo(const char* label, int* value, const char* const items[], int count);
bool settings_slider(const char* label, int* value, int low, int high,
                     const char* format = "%d", ImGuiSliderFlags flags = 0);
void wrapped_tooltip(const char* text);
// Every control marked "(experimental)" explains the word the same way on hover.
inline constexpr const char* kExperimentalNote =
    "Experimental: it may change or be removed, and it can show visual problems.";
bool gecko_add_code_button();
const char* te_pair_hover(uint32_t bit);
void engine_only_reason(const char* format, ...);
bool settings_lcancel_flash(RenderOptions& options);
bool settings_advanced_group(int which, bool* changed);

// ---- pc_settings_search.cpp: the search box, the collapsible sections, the pages by number ----
extern bool g_search;              // a query is in the box this frame
extern std::string g_goto_label;   // "go to" a search result: this row on g_goto_page
extern int g_goto_page;
int settings_page_id(int page);
const char* settings_page_name(int page);
const char* settings_page_title(int page);
// One setting row as settings_row_begin starts it (pc_settings_search.cpp).
struct SettingsRow {
  bool revealed = false, shown = true;
  std::string label;
};
SettingsRow settings_row_begin(const char* label);
void settings_row_end(const SettingsRow& r);
bool settings_begin_combo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
void settings_end_combo();
bool settings_slider_int(const char* label, int* v, int lo, int hi, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool settings_color_edit3(const char* label, float col[3], ImGuiColorEditFlags flags = 0);
// Not a plain test: it starts the page (its sections and the search). Call it as settings_frame does.
bool settings_page_shown(int active_tab, int page);
void settings_section(const char* title);
void settings_pages_end();
void settings_search_box();

// ---- pc_settings_bindings.cpp: binding labels, press levels and profiles (the Controls page) ----
extern const char* kActionTitles[(size_t)host::BindAction::Count];
std::string binding_label(host::CaptureDevice kind, int index, int action);
uint32_t live_actions(const host::InputDebugSnapshot& snap, host::CaptureDevice kind, int index);
void autosave_profile(int tab);
bool draw_profile_row(host::CaptureDevice kind, int index, int tab);
bool binding_level_slider(host::CaptureDevice kind, int index, int action);
bool binding_press_at_slider(host::CaptureDevice kind, int index, int action, float width);

// ---- pc_settings_controller_picture.cpp: the controllers to rebind by pointing at them ----
enum { kCStickModeBox = 100 };   // the single C-stick box of an analog controller: shows and toggles the mode
extern double g_show_dz_main_until, g_show_dz_c_until;   // the deadzone view shows until then
int draw_gc_bind_picture(uint32_t live, int capturing, int* right_clicked, int* hovered_out,
                         const std::function<std::string(int)>& label, bool analog_c,
                         const char* c_mode_text, ImVec2 stick_pos, ImVec2 c_pos, float dz_main, float dz_c);
bool draw_swpro_bind_picture(int index, uint16_t raw, ImVec2 lstick, ImVec2 rstick, float dz_main, float dz_c);

// ---- pc_settings_practice.cpp: the Practice menu (Training) and the replay viewer's controller layer ----
namespace practice_ui {
// The practice CPU's tabs 1 to 6 (draw_practice_cpu_tabs), shown as pages 0 to 5.
inline const char* const kTabs[] = {"CPU", "Defense", "Hit reactions", "Ledge", "Tools", "Presets"};
constexpr int kTabCount = (int)(sizeof kTabs / sizeof kTabs[0]);
extern int g_tab_request;
}  // namespace practice_ui
void draw_native_practice(SettingsState& state, RenderOptions& options,
                          const slippi::native_practice::Snapshot& practice,
                          const host::PadState& pad, bool have_pad);
void draw_replay_practice(RenderOptions& options, const host::PadState& pad, bool have_pad);

// ---- pc_settings_game_overlays.cpp: what settings_frame draws over the game ----
struct OverlayBounds { float left, top, right, bottom; };
OverlayBounds overlay_bounds();
void clamp_overlay_window();
void draw_combo_counter();
void draw_community_code_overlays();
void draw_player_nicknames();
void draw_input_overlay(int port, int row, bool lone, bool editable, bool borderless, bool values, int knob,
                        float (*rects)[6], bool* moved);
void log_game_notices();

// ---- the pages, settings_page_<page>.cpp, drawn by settings_frame ----
// What a page of the panel draws with: the panel's state, the options it edits, and whether a
// setting changed this frame (saved once the control is released).
struct SettingsPageContext {
  SettingsState& state;
  RenderOptions& options;
  bool& changed;
};
void settings_page_video(SettingsPageContext& page);      // 0
void settings_page_audio(SettingsPageContext& page);      // 1
void settings_page_game(SettingsPageContext& page);       // 2
void settings_page_controls(SettingsPageContext& page);   // 3
void settings_page_overlays(SettingsPageContext& page);   // 4
void settings_page_gecko(SettingsPageContext& page);      // 6

}  // namespace gx
