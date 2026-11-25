// State and body of the PC settings panel, shared by the D3D12 and D3D11 renderer wrappers.
// The panel itself is pure ImGui; only the platform/renderer bindings differ per backend. It draws
// only in the standalone settings window (--settings-window); in the game window the same frame
// function draws the optional HUD overlays and the Practice menu, and saves what the game changed.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gx_core.h"
#include "render_options.h"
#include "host.h"            // host::PadState, which input_bindings.h uses without declaring
#include "input_bindings.h"
#include <array>
#include <cstdint>
#include <vector>
#include "settings_page_navigation.h"

namespace gx {

struct SettingsState {
  bool open = false, saved = false;   // open: the settings window shows its panel
  bool dirty = false;   // a setting changed since the last write; saved once the control is released
  uint32_t mod_choices_seen = 0;
  int volume = 0;
  std::array<float, 180> intervals{};
  unsigned cursor = 0;
  // Reflex's measured render latency, same rolling-buffer shape as intervals, for the performance
  // graph. Filled whether or not Reflex's low-latency mode is on (see gx_streamline.h).
  std::array<float, 180> latencies{};
  unsigned latency_cursor = 0;
  std::vector<int> readout_remote_pings;
  int readout_ping = 0;
  double readout_ping_at = -1.0;
  int rebind_action = -1;                                       // BindAction index while a "press a button" capture runs, -1 = none
  host::CaptureDevice rebind_kind = host::CaptureDevice::None;  // which device tab that capture belongs to
  int rebind_index = 0;                                         // pad / adapter port index for that tab
  int hid_axis_role = -1, hid_axis_slot = -1, hid_axis_frame = -1;
  double hid_axis_until = 0.0;
  bool running_vulkan = false;   // the Vulkan renderer owns the panel (same purpose as running_d3d11)
  bool running_d3d11 = false;                                   // which backend owns this panel, so it can say when a backend change needs a restart
  // A texture pack was switched on or off: the backend drops what it has uploaded so the next
  // draw rebuilds it with (or without) its replacement.
  bool textures_dirty = false;
  // The standalone settings window, the only place the panel draws (settings_fill_window).
  bool fill_window = false;
  settings_page::Navigation page_navigation;
  int active_tab = 0;             // the page: 0 Video, 1 Audio, 2 Game, 3 Controls, 4 Overlays, 6 Gecko Codes
  int settings_controller_port = 0;  // most recently used controller while the settings panel is open
  std::array<bool, 4> settings_pad_was_at_rest{};   // last frame: connected and at rest, per port (settings_nav.h)
  float practice_release_seconds = 0.0f;           // how long the game has waited for its controllers back
  int8_t settings_stick_x = 0, settings_stick_y = 0;
  // Clean side, the window's style: the category list, and a page that slides in over it.
  bool clean_detail_open = false;
  float clean_home_frames = 0.0f;
  float clean_detail_frames = 0.0f;
  std::array<float, 7> clean_hover{};
  std::array<float, 2> footer_hover{};
  // The Practice menu (Training): render-only input and focus state.
  bool practice_open = false;
  bool practice_nav_active = false;
  bool practice_nav_was = false;   // the Practice menu was open last frame (its pad starts on the trainee's)
  bool practice_pad_armed = false;
  bool practice_release_capture = false;
};

// ImGui context plus the Win32 platform backend; the renderer backend is set up by the caller.
void settings_context_create(void* window);
void settings_context_destroy();
// Everything between ImGui::NewFrame() and ImGui::Render(); true when render configuration changed.
bool settings_frame(SettingsState& state, RenderOptions& options);

}  // namespace gx
