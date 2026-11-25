// The Controls page's binding helpers: the names a binding shows for keys and buttons, which actions a
// device presses now, the press-level sliders, and the profile row (load, new, delete, import from
// Dolphin). The binding tables themselves are the settings store's (settings_controls.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "pc_settings_internal.h"

namespace gx {

// The same, as the controls picture writes them.
const char* kActionTitles[(size_t)host::BindAction::Count] = {
  "A", "B", "X", "Y", "Z", "Start", "L", "R", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
  "C Up", "C Down", "C Left", "C Right",
  "Stick Up", "Stick Down", "Stick Left", "Stick Right",
  "L analog", "R analog",
  "Mod X", "Mod Y", "Light shield", "Mid shield",
  "Stick modifier"
};

// ---- binding -> label helpers, used by the Controls tabs ----
// A generic HID pad publishes no names for its buttons, only numbers, so that is what is shown.
// The number is the device's own: button 1 is what the device calls button 1.
static std::string hid_button_name(uint32_t mask) {
  if (!mask) return "Unbound";
  if (mask == host::HID_HAT_UP) return "Hat Up";
  if (mask == host::HID_HAT_DOWN) return "Hat Down";
  if (mask == host::HID_HAT_LEFT) return "Hat Left";
  if (mask == host::HID_HAT_RIGHT) return "Hat Right";
  int bit = 0;
  while (bit < 31 && !(mask & (1u << bit))) ++bit;
  return "Button " + std::to_string(bit + 1);
}

static void format_key_label(int vk, char* buf, size_t n) {
  if (!vk) { std::snprintf(buf, n, "Unbound"); return; }
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { std::snprintf(buf, n, "%c", (char)vk); return; }
  switch (vk) {
    case VK_RETURN: std::snprintf(buf, n, "Enter"); return;
    case VK_SPACE:  std::snprintf(buf, n, "Space"); return;
    case VK_TAB:    std::snprintf(buf, n, "Tab"); return;
    case VK_SHIFT:  std::snprintf(buf, n, "Shift"); return;
    case VK_CONTROL:std::snprintf(buf, n, "Ctrl"); return;
    case VK_LEFT:   std::snprintf(buf, n, "Left"); return;
    case VK_RIGHT:  std::snprintf(buf, n, "Right"); return;
    case VK_UP:     std::snprintf(buf, n, "Up"); return;
    case VK_DOWN:   std::snprintf(buf, n, "Down"); return;
    default:        std::snprintf(buf, n, "VK 0x%02X", vk); return;
  }
}

// XInput wButtons is a different bit layout than the GC-style kActionPadBit table,
// so it needs its own name lookup (unlike the GC adapter, whose raw mask already
// matches kActionPadBit -- see gc_button_name below).
static const char* xinput_button_name(unsigned short mask) {
  switch (mask) {
    case 0: return "Unbound";
    case XINPUT_GAMEPAD_DPAD_UP: return "D-Up";
    case XINPUT_GAMEPAD_DPAD_DOWN: return "D-Down";
    case XINPUT_GAMEPAD_DPAD_LEFT: return "D-Left";
    case XINPUT_GAMEPAD_DPAD_RIGHT: return "D-Right";
    case XINPUT_GAMEPAD_START: return "Start";
    case XINPUT_GAMEPAD_BACK: return "Back";
    case XINPUT_GAMEPAD_LEFT_THUMB: return "L3";
    case XINPUT_GAMEPAD_RIGHT_THUMB: return "R3";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB";
    case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
    case host::kXInputBindLT: return "LT";
    case host::kXInputBindRT: return "RT";
    case XINPUT_GAMEPAD_A: return "A";
    case XINPUT_GAMEPAD_B: return "B";
    case XINPUT_GAMEPAD_X: return "X";
    case XINPUT_GAMEPAD_Y: return "Y";
    default: return "?";
  }
}

static const char* ds4_button_name(unsigned short mask) {
  switch (mask) {
    case 0: return "Unbound";
    case host::DS4_DPAD_UP: return "D-Up"; case host::DS4_DPAD_DOWN: return "D-Down";
    case host::DS4_DPAD_LEFT: return "D-Left"; case host::DS4_DPAD_RIGHT: return "D-Right";
    case host::DS4_SQUARE: return "Square"; case host::DS4_CROSS: return "Cross";
    case host::DS4_CIRCLE: return "Circle"; case host::DS4_TRIANGLE: return "Triangle";
    case host::DS4_L1: return "L1"; case host::DS4_R1: return "R1";
    case host::DS4_L2: return "L2"; case host::DS4_R2: return "R2";
    case host::DS4_SHARE: return "Share"; case host::DS4_OPTIONS: return "Options";
    case host::DS4_L3: return "L3"; case host::DS4_R3: return "R3";
    default: return "?";
  }
}

static const char* swpro_button_name(unsigned short mask) {
  switch (mask) {
    case 0: return "Unbound";
    case host::SWPRO_DPAD_UP: return "D-Up"; case host::SWPRO_DPAD_DOWN: return "D-Down";
    case host::SWPRO_DPAD_LEFT: return "D-Left"; case host::SWPRO_DPAD_RIGHT: return "D-Right";
    case host::SWPRO_B: return "B"; case host::SWPRO_A: return "A";
    case host::SWPRO_Y: return "Y"; case host::SWPRO_X: return "X";
    case host::SWPRO_L: return "L"; case host::SWPRO_R: return "R";
    case host::SWPRO_ZL: return "ZL"; case host::SWPRO_ZR: return "ZR";
    case host::SWPRO_MINUS: return "Minus"; case host::SWPRO_PLUS: return "Plus";
    case host::SWPRO_L3: return "L-Stick"; case host::SWPRO_R3: return "R-Stick";
    default: return "?";
  }
}

// GC adapter raw button bits match kActionPadBit exactly (see input_bindings.h /
// default_gc_bindings comments), so this is a reverse lookup into kActionNames.
static const char* gc_button_name(unsigned short mask) {
  if (!mask) return "Unbound";
  for (int i = 0; i < (int)host::BindAction::Count; ++i)
    if (host::kActionPadBit[i] && host::kActionPadBit[i] == mask) return kActionTitles[i];
  return "?";
}

// Space-joined list of action names whose bit is set, for the live "what's this
// device pressing right now" overlay lines. Bit i corresponds to BindAction i.
static std::string active_actions_label(uint32_t bits) {
  std::string s;
  for (int i = 0; i < (int)host::BindAction::Count; ++i)
    if (bits & (uint32_t)(1u << i)) { if (!s.empty()) s += " "; s += kActionNames[i]; }
  return s.empty() ? std::string("-") : s;
}

// Same idea but decoding a final PadState.button field, which uses the GC-native
// bits (kActionPadBit), not the BindAction-bit-index encoding active_actions_label
// reads -- used for the per-port output overlay.
static std::string active_pad_buttons_label(uint16_t button) {
  std::string s;
  for (int i = 0; i < (int)host::BindAction::Count; ++i)
    if (host::kActionPadBit[i] && (button & host::kActionPadBit[i])) { if (!s.empty()) s += " "; s += kActionNames[i]; }
  return s.empty() ? std::string("-") : s;
}

static std::string binding_source_label(host::CaptureDevice kind, int action, uint32_t v) {
  char label[32];
  switch (kind) {
    case host::CaptureDevice::Keyboard:  format_key_label((int)v, label, sizeof label); return label;
    case host::CaptureDevice::XInputPad: return xinput_button_name((unsigned short)v);
    case host::CaptureDevice::DS4Pad:    return ds4_button_name((unsigned short)v);
    case host::CaptureDevice::SwitchPro: return swpro_button_name((unsigned short)v);
    case host::CaptureDevice::HidPad:    return hid_button_name(v);
    case host::CaptureDevice::GCAdapter:
      // On a travel binding the adapter's L and R bits are the trigger's travel, not its click.
      if (host::is_analog_action(action) && v == host::kActionPadBit[(size_t)host::BindAction::L]) return "L trigger";
      if (host::is_analog_action(action) && v == host::kActionPadBit[(size_t)host::BindAction::R]) return "R trigger";
      return gc_button_name((unsigned short)v);
    default: return "?";
  }
}

std::string binding_label(host::CaptureDevice kind, int index, int action) {
  const uint32_t v = binding_get(kind, index, action);
  // A HID pad's sticks are read from its own axes, not bound: these rows are only for a box that sends
  // directions as buttons. "Not bound" there read as a broken stick (B0XX reports 11-22).
  if (!v && kind == host::CaptureDevice::HidPad && (host::is_stick_action(action) || host::is_cstick_action(action)))
    return host::is_stick_action(action) ? "Controller's stick" : "Controller's C-stick";
  if (!v && (host::is_trigger_action(action) || host::is_analog_action(action))) {
    // A box's own trigger axis has no source to bind: it always feeds the travel.
    if (kind == host::CaptureDevice::HidPad && host::is_analog_action(action)) return "Device axis";
    return "Not bound";
  }
  // A binding carried over from before L and R were split can hold two sources (a bumper and the
  // trigger that also clicked): both are named. Key codes are numbers, not bits.
  if (kind != host::CaptureDevice::Keyboard && (v & (v - 1))) {
    std::string joined;
    for (uint32_t bit = 1; bit; bit <<= 1)
      if (v & bit) joined += (joined.empty() ? "" : " + ") + binding_source_label(kind, action, bit);
    return joined;
  }
  return binding_source_label(kind, action, v);
}


// Which actions this device is pressing right now (bit i = BindAction i).
uint32_t live_actions(const host::InputDebugSnapshot& snap, host::CaptureDevice kind, int index) {
  switch (kind) {
    case host::CaptureDevice::Keyboard:  return snap.keyboard_actions;
    case host::CaptureDevice::XInputPad: return snap.xinput_actions[index];
    case host::CaptureDevice::DS4Pad:    return snap.ds4_actions[index];
    case host::CaptureDevice::SwitchPro: return snap.swpro_actions[index];
    case host::CaptureDevice::HidPad:    return snap.hid_actions[index];
    case host::CaptureDevice::GCAdapter: return snap.gc_actions[index];
    default: return 0;
  }
}

// Saved profiles, listed once and then again only after a save or delete, or every few seconds:
// listing reads the folder, and this runs on every frame the Controls tab is drawn.
static const std::vector<std::string>& cached_profiles(host::ProfileDevice device, bool refresh) {
  static std::vector<std::string> lists[(int)host::ProfileDevice::Count];
  static double listed_at[(int)host::ProfileDevice::Count] = {};
  const int d = (int)device;
  const double now = ImGui::GetTime();
  if (refresh || listed_at[d] == 0.0 || now - listed_at[d] > 3.0) { lists[d] = host::profile_list(device); listed_at[d] = now > 0.0 ? now : 1e-9; }
  return lists[d];
}

// Load, save and delete named layouts for this kind of device. Returns true when the bindings of the
// device on this tab changed.
static const std::vector<std::string>& cached_profiles(host::ProfileDevice device, bool refresh);
// Keeps a changed layout in the device's profile. Called whenever its buttons change in the panel.
void autosave_profile(int tab) {
  if (!g_defaults_captured || bindings_are_default(tab)) return;
  const host::ProfileDevice device = profile_device_of_tab(tab);
  host::profile_save(device, active_profile_name(tab), bindings_of(tab));
  g_named_profile_active[tab] = true;
  cached_profiles(device, true);
}

// Width left on the current row after the last item, so a hint that would be cut off is left out.
static float room_after_last_item() {
  return ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
}

bool draw_profile_row(host::CaptureDevice kind, int index, int tab) {
  static char new_name[48];
  static std::string status[32];
  const host::ProfileDevice device = profile_device_for(kind);
  bool changed = false;
  const std::vector<std::string>& list = cached_profiles(device, false);
  // What the dropdown shows: whichever entry the player picked, not whether the live buttons happen
  // to match the factory defaults right now (a profile can legitimately hold default-equal buttons).
  const bool is_default = !g_named_profile_active[tab];
  const std::string current = is_default ? std::string("Default") : active_profile_name(tab);

  // One line: the layout in use (picking another loads it at once), a new one, delete.
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Profile");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.0f);
  if (settings_widgets::BeginCombo("##profile", current.c_str())) {
    // The defaults, always one click away. The player's own profile is left as it is, and changes
    // made after this go to a new profile, so neither layout is lost.
    if (settings_widgets::Selectable("Default", is_default)) {
      set_bindings_of(tab, g_default_bindings[tab]);
      g_active_profile[tab] = next_free_profile_name(device);
      g_named_profile_active[tab] = false;
      status[tab] = "Back to the default buttons.";
      changed = true;
    }
    for (const std::string& name : list) {
      if (settings_widgets::Selectable(name.c_str(), !is_default && name == current)) {
        host::ProfileBindings pb = bindings_of(tab);   // an older profile keeps the current stick keys
        // Pre-modifier profiles keep the factory defaults for the five appended actions.
        for (int i = (int)host::BindAction::ModX; i < host::kProfileActions; ++i) pb[i] = g_default_bindings[tab][i];
        const int la = (int)host::BindAction::LAnalog, ra = (int)host::BindAction::RAnalog;
        pb[la] = pb[ra] = host::kProfileActionAbsent;
        if (host::profile_load(device, name, pb)) {
          // A profile from before L and R were split: its single L / R bindings are carried over.
          if (pb[la] == host::kProfileActionAbsent || pb[ra] == host::kProfileActionAbsent) {
            pb[la] = pb[ra] = 0;
            split_old_trigger_bindings(kind, index, pb);
          }
          set_bindings_of(tab, pb);
          g_active_profile[tab] = name;
          g_named_profile_active[tab] = true;
          status[tab] = "Loaded " + name + ".";
          changed = true;
        } else {
          status[tab] = "Could not read " + name + ".";
        }
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Pick a layout to use it. Every change you make is saved to the profile shown here.\n"
                      "Profiles belong to a kind of controller: the same number is a different button\n"
                      "on a different controller.");
  ImGui::SameLine();
  if (settings_widgets::Button("New")) { std::snprintf(new_name, sizeof new_name, "%s", next_free_profile_name(device).c_str()); ImGui::OpenPopup("new_profile"); }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep these buttons as a new profile, under a name you choose.");
  ImGui::SameLine();
  ImGui::BeginDisabled(is_default || std::find(list.begin(), list.end(), current) == list.end());
  if (settings_widgets::Button("Delete")) {
    status[tab] = host::profile_delete(device, current) ? "Deleted " + current + "." : "Could not delete " + current + ".";
    set_bindings_of(tab, g_default_bindings[tab]);
    g_active_profile[tab].clear();
    cached_profiles(device, true);
    g_active_profile[tab] = next_free_profile_name(device);
    g_named_profile_active[tab] = false;
    changed = true;
  }
  ImGui::EndDisabled();
  // A layout made in Dolphin: read into a profile of the same name and put to use at once. Only the
  // keyboard and XInput pads, since those are the devices whose Dolphin names map one to one.
  if (kind == host::CaptureDevice::Keyboard || kind == host::CaptureDevice::XInputPad) {
    ImGui::SameLine();
    if (settings_widgets::Button("Import from Dolphin...")) {
      const std::string path = host::dolphin_profile_choose_file();
      if (!path.empty()) {
        std::string name;
        const host::DolphinProfile read = host::dolphin_profile_read_file(path, &name);
        name = host::profile_clean_name(name);
        if (!read.ok) status[tab] = read.message;
        else if (read.device != device)
          status[tab] = read.device == host::ProfileDevice::Keyboard ? "That is a keyboard profile: import it on the Keyboard tab."
                                                                     : "That is an XInput pad profile: import it on that controller's tab.";
        else if (name.empty()) status[tab] = "The profile's file name has no usable letters.";
        else {
          // A direction the Dolphin profile leaves to an analog stick keeps what it has here.
          host::ProfileBindings pb = read.bindings;
          const host::ProfileBindings now = bindings_of(tab);
          for (int i = 0; i < (int)host::BindAction::Count; ++i)
            if (pb[i] == 0 && (host::is_stick_action(i) || host::is_cstick_action(i))) pb[i] = now[i];
          if (host::profile_save(device, name, pb)) {
            set_bindings_of(tab, pb);
            g_active_profile[tab] = name;
            g_named_profile_active[tab] = true;
            cached_profiles(device, true);
            char text[160];
            if (read.skipped) std::snprintf(text, sizeof text, "Imported %s: %d buttons, %d not understood (bind those here).", name.c_str(), read.bound, read.skipped);
            else std::snprintf(text, sizeof text, "Imported %s: %d buttons.", name.c_str(), read.bound);
            status[tab] = text;
            changed = true;
          } else {
            status[tab] = "Could not save to " + host::profiles_folder() + ".";
          }
        }
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Pick a profile from Dolphin's User\\Config\\Profiles\\GCPad folder (Slippi Launcher keeps it under\n"
                        "AppData\\Roaming\\Slippi Launcher\\netplay\\User). Keyboard and XInput profiles can be read.\n"
                        "Combined bindings such as A | B are left for you to bind here.");
  }
  if (ImGui::BeginPopup("new_profile")) {
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = settings_widgets::InputText("##new_name", new_name, sizeof new_name, ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string clean = host::profile_clean_name(new_name);
    ImGui::BeginDisabled(clean.empty());
    if ((settings_widgets::Button("Save") || enter) && !clean.empty()) {
      status[tab] = host::profile_save(device, clean, bindings_of(tab)) ? "Saved " + clean + "." : "Could not save to " + host::profiles_folder() + ".";
      g_active_profile[tab] = clean;
      g_named_profile_active[tab] = true;
      cached_profiles(device, true);
      changed = true;   // the profile in use is kept in the settings file
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
  }
  {
    char hint[160];
    if (!status[tab].empty()) std::snprintf(hint, sizeof hint, "%s", status[tab].c_str());
    else if (is_default) std::snprintf(hint, sizeof hint, "Change any button and it is kept as %s.", active_profile_name(tab).c_str());
    else std::snprintf(hint, sizeof hint, "Changes save automatically.");
    if (room_after_last_item() > ImGui::CalcTextSize(hint).x + 16) { ImGui::SameLine(); ImGui::TextDisabled("%s", hint); }
    else if (!status[tab].empty()) ImGui::TextDisabled("%s", hint);   // a result is never dropped for lack of room
  }
  return changed;
}


// The level of one binding as a compact slider, for the rows where it means something. Returns
// true when it changed.
bool binding_level_slider(host::CaptureDevice kind, int index, int action) {
  const int what = binding_level_kind(kind, index, action);
  uint8_t* level = binding_level(kind, index, action);
  if (!what || !level) return false;
  bool changed = false;
  char label[96];
  const std::string bound_to = binding_label(kind, index, action);
  ImGui::PushID(action);
  ImGui::SetNextItemWidth(200.0f);
  if (what == 1) {
    std::snprintf(label, sizeof label, "%s (%s) pressed at", kActionTitles[action], bound_to.c_str());
    int shown = *level;
    if (settings_slider(label, &shown, 0, 254, shown ? "%d" : "Default")) {
      // The PlayStation reader reports nothing below its own press point.
      if (kind == host::CaptureDevice::DS4Pad && shown > 0 && shown <= host::kPlayStationTriggerPress)
        shown = host::kPlayStationTriggerPress + 1;
      *level = (uint8_t)std::clamp(shown, 0, 254);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How far the trigger travels (out of 255) before this binding counts as pressed.\n"
                        "Default uses the controller's own press point.");
  } else {
    std::snprintf(label, sizeof label, "%s (%s) press depth", kActionTitles[action], bound_to.c_str());
    // 0 is "not set", which presses the default depth; the slider shows that depth and stores a number.
    int shown = *level ? *level : host::kDefaultAnalogDepth;
    if (settings_slider(label, &shown, 1, 255, "%d")) {
      *level = (uint8_t)std::clamp(shown, 1, 255);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How far this button presses the trigger (out of 255).\n"
                        "It is the travel only and never clicks: Melee shields lightly from 43 to 140.\n"
                        "For the full press (full shield, tech, air dodge) bind L or R as well.");
  }
  ImGui::PopID();
  return changed;
}

// The same press point inside a row of the button list, as a percent of the travel. Only rows whose
// source is analog get one; 0 is the default, so an untouched row stays exactly as it was.
bool binding_press_at_slider(host::CaptureDevice kind, int index, int action, float width) {
  uint8_t* level = binding_level(kind, index, action);
  if (!level || binding_level_kind(kind, index, action) != 1) return false;
  int shown = *level ? (*level * 100 + 127) / 255 : 0;
  ImGui::SetNextItemWidth(width);
  const bool changed = settings_widgets::SliderInt("##press_at", &shown, 0, 99, shown ? "Press at %d%%" : "Press at default");
  settings_page::label_item((std::string(kActionTitles[action]) + " press depth").c_str());
  if (changed) {
    int value = shown ? std::clamp((shown * 255 + 50) / 100, 1, 254) : 0;
    // The PlayStation reader reports nothing below its own press point.
    if (kind == host::CaptureDevice::DS4Pad && value > 0 && value <= host::kPlayStationTriggerPress)
      value = host::kPlayStationTriggerPress + 1;
    *level = (uint8_t)value;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How far this has to be pushed before it counts as pressed.");
  return changed;
}

}  // namespace gx
