// Linux: the PC settings panel on the OpenGL renderer, through Dear ImGui's OpenGL 3 backend. The
// panel itself (pc_settings.cpp) is the one Windows draws; this file only renders it.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings.h"
#include "pc_settings_shared.h"
#include "host.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include <memory>

namespace gx {

struct PcSettingsUIGL::Impl {
  SettingsState state;
  bool ready = false;
};

PcSettingsUIGL::PcSettingsUIGL(void* window, const RenderOptions& options) : impl_(std::make_unique<Impl>()) {
  (void)options;
  impl_->state.open = settings_fills_window();   // the panel shows only in the settings window
  settings_context_create(window);
#ifdef __APPLE__
  impl_->ready = ImGui_ImplOpenGL3_Init("#version 410");
#else
  impl_->ready = ImGui_ImplOpenGL3_Init("#version 450 core");
#endif
  if (!impl_->ready) host::log("opengl: settings panel unavailable (its renderer could not start)");
}

PcSettingsUIGL::~PcSettingsUIGL() {
  if (impl_->ready) ImGui_ImplOpenGL3_Shutdown();
  settings_context_destroy();
}

bool PcSettingsUIGL::begin(RenderOptions& options) {
  if (!impl_->ready) return false;
  ImGui_ImplOpenGL3_NewFrame();
  return settings_frame(impl_->state, options);
}

void PcSettingsUIGL::draw() {
  if (!impl_->ready) return;
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace gx
