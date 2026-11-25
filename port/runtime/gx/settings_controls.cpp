// Controller settings as the settings file keeps them: the binding tables' key names, which device
// feeds each port, per-family stick options, per-binding levels, and which profile each controller
// uses. Read and written for settings_file.cpp; the Controls page edits them. Interface:
// settings_controls.h.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings_controls.h"
#include "host.h"
#include "input_bindings.h"
#include "hid_pad.h"
#include "controller_profiles.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace gx {

// Names used both when drawing the Controls list and when saving/loading bindings
// to port-settings.ini (as "key_<name>" / "pad<idx>_<name>" / "gc<idx>_<name>" lines).
// Order must match host::BindAction exactly.
const char* kActionNames[(size_t)host::BindAction::Count] = {
  "A", "B", "X", "Y", "Z", "Start", "L", "R", "DUp", "DDown", "DLeft", "DRight",
  "CUp", "CDown", "CLeft", "CRight",
  // Added after the control stick became rebindable. A settings file written before that has no
  // these lines, so the arrow key defaults stand and nobody's setup changes on upgrade.
  "SUp", "SDown", "SLeft", "SRight",
  // Added when L and R were split into the click and the travel. A file without these lines is an
  // older one, whose single L / R bindings are carried over (see split_old_trigger_bindings).
  "LAnalog", "RAnalog",
  // Added with the box controller buttons. A file without them has none bound: no box mode.
  "ModX", "ModY", "LightShield", "MidShield",
  // Added with the stick modifier (Dolphin's Modifier). A file without it has none bound.
  "StickMod"
};

// ---- Port-source <-> combo-box index, shared by load/save and the Port assignment UI ----
// 0 = None, 1 = Keyboard, 2..5 = XInput, 6..9 = DS4, 10..13 = GC Adapter, 14..17 = Switch Pro,
// 18..21 = generic HID (B0XX, Frame1, vJoy and any other pad without a reader of its own).
// Saved settings store the index, so new devices are appended and the existing ones never move.
// Switch Pro says "experimental" because it has never been tried against the hardware.
const char* kPortSourceNames[kPortSourceCount] = {
  "None", "Keyboard",
  "XInput Pad 1", "XInput Pad 2", "XInput Pad 3", "XInput Pad 4",
  "PlayStation 1", "PlayStation 2", "PlayStation 3", "PlayStation 4",
  "GC Adapter 1", "GC Adapter 2", "GC Adapter 3", "GC Adapter 4",
  "Switch Pro 1 (experimental)", "Switch Pro 2 (experimental)",
  "Switch Pro 3 (experimental)", "Switch Pro 4 (experimental)",
  "HID Pad 1", "HID Pad 2", "HID Pad 3", "HID Pad 4"
};

int port_source_to_combo(const host::PortSource& s) {
  switch (s.kind) {
    case host::DeviceKind::Keyboard:  return 1;
    case host::DeviceKind::XInputPad: return 2 + std::clamp(s.index, 0, 3);
    case host::DeviceKind::DS4Pad:     return 6 + std::clamp(s.index, 0, 3);
    case host::DeviceKind::GCAdapter: return 10 + std::clamp(s.index, 0, 3);
    case host::DeviceKind::SwitchPro: return 14 + std::clamp(s.index, 0, 3);
    case host::DeviceKind::HidPad:    return 18 + std::clamp(s.index, 0, 3);
    case host::DeviceKind::None: default: return 0;
  }
}

host::PortSource combo_to_port_source(int idx) {
  if (idx == 1) return { host::DeviceKind::Keyboard, 0 };
  if (idx >= 2 && idx <= 5) return { host::DeviceKind::XInputPad, idx - 2 };
  if (idx >= 6 && idx <= 9) return { host::DeviceKind::DS4Pad, idx - 6 };
  if (idx >= 10 && idx <= 13) return { host::DeviceKind::GCAdapter, idx - 10 };
  if (idx >= 14 && idx <= 17) return { host::DeviceKind::SwitchPro, idx - 14 };
  if (idx >= 18 && idx <= 21) return { host::DeviceKind::HidPad, idx - 18 };
  return { host::DeviceKind::None, 0 };
}

// ---- per-family stick options (deadzones, C-stick mode), saved as deadzone_<family>_main etc ----
static const char* kFamilyKeys[(size_t)host::PadFamily::Count] = {"gamecube", "xbox", "playstation", "switch", "box"};

static bool load_family_option(const std::string& key, const std::string& value) {
  if (key == "stick_modifier_percent") { host::g_stick_modifier_percent = std::clamp(std::atoi(value.c_str()), 10, 100); return true; }
  for (int f = 0; f < (int)host::PadFamily::Count; ++f) {
    const std::string k = kFamilyKeys[f];
    if (key == "deadzone_" + k + "_main") { host::g_deadzones[f].main = std::clamp(std::atoi(value.c_str()), 0, 100); return true; }
    if (key == "deadzone_" + k + "_c") { host::g_deadzones[f].c = std::clamp(std::atoi(value.c_str()), 0, 100); return true; }
    if (key == "radius_" + k + "_main") { host::g_deadzones[f].radius_main = std::clamp(std::atoi(value.c_str()), 50, 100); return true; }
    if (key == "radius_" + k + "_c") { host::g_deadzones[f].radius_c = std::clamp(std::atoi(value.c_str()), 50, 100); return true; }
    // The 0.9.113 strength (1..10), then the size and window that replace it (written after it).
    if (key == "snapback_" + k + "_x") { host::snapback_from_strength(std::atoi(value.c_str()), host::g_deadzones[f].snapback_x, host::g_deadzones[f].snapback_ms_x); return true; }
    if (key == "snapback_" + k + "_y") { host::snapback_from_strength(std::atoi(value.c_str()), host::g_deadzones[f].snapback_y, host::g_deadzones[f].snapback_ms_y); return true; }
    if (key == "snapback_size_" + k + "_x") { host::g_deadzones[f].snapback_x = std::clamp(std::atoi(value.c_str()), 0, host::kSnapbackEdge); return true; }
    if (key == "snapback_size_" + k + "_y") { host::g_deadzones[f].snapback_y = std::clamp(std::atoi(value.c_str()), 0, host::kSnapbackEdge); return true; }
    if (key == "snapback_ms_" + k + "_x") { host::g_deadzones[f].snapback_ms_x = std::clamp(std::atoi(value.c_str()), host::kSnapbackMinMs, host::kSnapbackMaxMs); return true; }
    if (key == "snapback_ms_" + k + "_y") { host::g_deadzones[f].snapback_ms_y = std::clamp(std::atoi(value.c_str()), host::kSnapbackMinMs, host::kSnapbackMaxMs); return true; }
    if (key == "trigger_" + k + "_l") { host::g_deadzones[f].trig_l = std::clamp(std::atoi(value.c_str()), 0, 255); return true; }
    if (key == "trigger_" + k + "_r") { host::g_deadzones[f].trig_r = std::clamp(std::atoi(value.c_str()), 0, 255); return true; }
    if (key == "trigger_" + k + "_l_threshold" || key == "trigger_" + k + "_r_threshold") {
      char* end = nullptr;
      const double normalized = std::strtod(value.c_str(), &end);
      if (end == value.c_str() || *end != '\0' || !std::isfinite(normalized)) return true;
      const int threshold = (int)std::lround(std::clamp(normalized, 0.0, 1.0) * 255.0);
      if (key == "trigger_" + k + "_l_threshold") host::g_deadzones[f].click_l = threshold;
      else host::g_deadzones[f].click_r = threshold;
      return true;
    }
  }
  return false;
}

std::string family_options_text() {
  std::string out;
  // written only when changed, so a file that never used it stays as it was
  if (host::g_stick_modifier_percent != 50) out += "\nstick_modifier_percent " + std::to_string(host::g_stick_modifier_percent);
  for (int f = 0; f < (int)host::PadFamily::Count; ++f) {
    const std::string k = kFamilyKeys[f];
    out += "\ndeadzone_" + k + "_main " + std::to_string(host::g_deadzones[f].main);
    out += "\ndeadzone_" + k + "_c " + std::to_string(host::g_deadzones[f].c);
    // written only when changed, so a file that never used them stays as it was
    if (host::g_deadzones[f].radius_main < 100) out += "\nradius_" + k + "_main " + std::to_string(host::g_deadzones[f].radius_main);
    if (host::g_deadzones[f].radius_c < 100) out += "\nradius_" + k + "_c " + std::to_string(host::g_deadzones[f].radius_c);
    // The nearest 0.9.113 strength first (an older version reads that), then the exact size and window.
    for (int axis = 0; axis < 2; ++axis) {
      const int size = axis ? host::g_deadzones[f].snapback_y : host::g_deadzones[f].snapback_x;
      const int ms = axis ? host::g_deadzones[f].snapback_ms_y : host::g_deadzones[f].snapback_ms_x;
      if (!size) continue;
      const std::string a = axis ? "_y " : "_x ";
      out += "\nsnapback_" + k + a + std::to_string(std::clamp((size - 10 + 2) / 4, 1, 10));
      out += "\nsnapback_size_" + k + a + std::to_string(size) + "\nsnapback_ms_" + k + a + std::to_string(ms);
    }
    if (host::g_deadzones[f].trig_l < 255) out += "\ntrigger_" + k + "_l " + std::to_string(host::g_deadzones[f].trig_l);
    if (host::g_deadzones[f].trig_r < 255) out += "\ntrigger_" + k + "_r " + std::to_string(host::g_deadzones[f].trig_r);
    if (host::g_deadzones[f].click_l != host::kDefaultTriggerClick)
      out += "\ntrigger_" + k + "_l_threshold " + std::to_string(host::g_deadzones[f].click_l / 255.0);
    if (host::g_deadzones[f].click_r != host::kDefaultTriggerClick)
      out += "\ntrigger_" + k + "_r_threshold " + std::to_string(host::g_deadzones[f].click_r / 255.0);
  }
  return out;
}

// ---- per-binding analog level (input_bindings.h), saved as "<the binding's key>_level" ----
uint8_t* binding_level(host::CaptureDevice kind, int index, int action) {
  switch (kind) {
    case host::CaptureDevice::Keyboard:  return &host::g_key_bindings.level[action];
    case host::CaptureDevice::XInputPad: return &host::g_pad_bindings[index].level[action];
    case host::CaptureDevice::DS4Pad:    return &host::g_ds4_bindings[index].level[action];
    case host::CaptureDevice::SwitchPro: return &host::g_swpro_bindings[index].level[action];
    case host::CaptureDevice::HidPad:    return &host::g_hid_bindings[index].level[action];
    case host::CaptureDevice::GCAdapter: return &host::g_gc_bindings[index].level[action];
    default: return nullptr;
  }
}

// The bits of this kind of device's bindings that name its analog triggers (0: it has none that can
// be bound), and whether it is an Xbox pad, whose triggers used to click by themselves.
static void trigger_sources(host::CaptureDevice kind, uint32_t* left, uint32_t* right, bool* xinput) {
  *left = *right = 0; *xinput = false;
  switch (kind) {
    case host::CaptureDevice::XInputPad: *left = host::kXInputBindLT; *right = host::kXInputBindRT; *xinput = true; break;
    case host::CaptureDevice::DS4Pad:    *left = host::DS4_L2; *right = host::DS4_R2; break;
    case host::CaptureDevice::GCAdapter:
      *left = host::kActionPadBit[(size_t)host::BindAction::L]; *right = host::kActionPadBit[(size_t)host::BindAction::R]; break;
    default: break;
  }
}

// What the level means on this row. 0: nothing, no control is shown. 1: the press point of an
// analog source (an Xbox LT/RT, a PlayStation L2/R2, a GameCube trigger). 2: how far a button bound
// to L analog or R analog presses the trigger. The same split input_bindings.h makes when it
// applies the bindings.
int binding_level_kind(host::CaptureDevice kind, int index, int action) {
  const uint32_t v = binding_get(kind, index, action);
  if (!v) return 0;
  uint32_t left, right; bool xinput;
  trigger_sources(kind, &left, &right, &xinput);
  // The travel bindings: a trigger passes its travel through and has nothing to set; a button has
  // its depth.
  if (host::is_analog_action(action)) return (v & ~(left | right)) ? 2 : 0;
  // The D-pad and Start are plain presses whatever they are bound to.
  if (action == (int)host::BindAction::Start || (action >= (int)host::BindAction::DUp && action <= (int)host::BindAction::DRight)) return 0;
  return (v & (left | right)) ? 1 : 0;
}

struct BindingKeyPrefix { host::CaptureDevice kind; const char* prefix; };
static const BindingKeyPrefix kBindingKeyPrefixes[] = {
  {host::CaptureDevice::XInputPad, "pad"}, {host::CaptureDevice::GCAdapter, "gc"}, {host::CaptureDevice::DS4Pad, "ds4"},
  {host::CaptureDevice::SwitchPro, "swpro"}, {host::CaptureDevice::HidPad, "hid"}};

static void reset_binding_levels() {
  for (int i = 0; i < (int)host::BindAction::Count; ++i) {
    host::g_key_bindings.level[i] = 0;
    for (const BindingKeyPrefix& f : kBindingKeyPrefixes)
      for (int idx = 0; idx < 4; ++idx) *binding_level(f.kind, idx, i) = 0;
  }
}

static bool load_binding_level(const std::string& key, const std::string& value) {
  static const std::string suffix = "_level";
  if (key.size() <= suffix.size() || key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
  const std::string base = key.substr(0, key.size() - suffix.size());
  const uint8_t level = (uint8_t)std::clamp(std::atoi(value.c_str()), 0, 255);
  for (int i = 0; i < (int)host::BindAction::Count; ++i) {
    if (base == std::string("key_") + kActionNames[i]) { host::g_key_bindings.level[i] = level; return true; }
    for (const BindingKeyPrefix& f : kBindingKeyPrefixes)
      for (int idx = 0; idx < 4; ++idx)
        if (base == f.prefix + std::to_string(idx) + "_" + kActionNames[i]) { *binding_level(f.kind, idx, i) = level; return true; }
  }
  return false;
}

// Written only when set, so a file that never used them stays as it was.
static std::string binding_levels_text() {
  std::string out;
  for (int i = 0; i < (int)host::BindAction::Count; ++i)
    if (host::g_key_bindings.level[i])
      out += std::string("\nkey_") + kActionNames[i] + "_level " + std::to_string(host::g_key_bindings.level[i]);
  for (const BindingKeyPrefix& f : kBindingKeyPrefixes)
    for (int idx = 0; idx < 4; ++idx)
      for (int i = 0; i < (int)host::BindAction::Count; ++i)
        if (const int level = *binding_level(f.kind, idx, i))
          out += std::string("\n") + f.prefix + std::to_string(idx) + "_" + kActionNames[i] + "_level " + std::to_string(level);
  return out;
}

host::ProfileDevice profile_device_for(host::CaptureDevice kind) {
  switch (kind) {
    case host::CaptureDevice::Keyboard:  return host::ProfileDevice::Keyboard;
    case host::CaptureDevice::XInputPad: return host::ProfileDevice::XInput;
    case host::CaptureDevice::DS4Pad:    return host::ProfileDevice::PlayStation;
    case host::CaptureDevice::SwitchPro: return host::ProfileDevice::SwitchPro;
    case host::CaptureDevice::HidPad:    return host::ProfileDevice::Hid;
    default:                             return host::ProfileDevice::GCAdapter;
  }
}

// ---- automatic profiles ----
// A player's layout is never lost: whenever a controller's buttons differ from its defaults they are
// kept in a profile ("Profile 1" unless the player loaded or saved another one), and "Default" in the
// profile list puts the defaults back without touching that profile. Device numbers are the panel's
// (0 keyboard, 1-4 Xbox, 5-8 PlayStation, 9-12 adapter, 13-16 Switch, 17-20 box/HID).
host::ProfileBindings g_default_bindings[kDeviceTabs];   // captured before the settings file is read
bool g_defaults_captured = false;
std::string g_active_profile[kDeviceTabs];
// Whether a named profile (rather than "Default") is the active selection. Kept separate from
// comparing live bindings to the defaults: a profile whose saved buttons happen to equal the
// defaults (a fresh "New" profile nobody has rebound yet) must still show and highlight as itself,
// not silently read back as "Default" the next time the panel draws. Zero-initialised false, which
// is correct at first boot: nothing has been picked yet, so "Default" is what is showing.
bool g_named_profile_active[kDeviceTabs];

// ---- bindings saved before L and R were split into the click and the travel ----
// One table (the settings file's, or a profile's) carried over so it plays exactly as it did; the
// rule is in input_bindings.h (migrate_trigger_bindings). Levels live in the device's own table.
void split_old_trigger_bindings(host::CaptureDevice kind, int index, host::ProfileBindings& b) {
  uint32_t left, right; bool xinput;
  trigger_sources(kind, &left, &right, &xinput);
  if (uint8_t* level = binding_level(kind, index, 0)) host::migrate_trigger_bindings(b.data(), level, left, right, xinput);
}
// Which tables the settings file being read mentions at all, and which of those have an L analog or
// R analog line (bit = device number). A table with lines but neither of those is an older one.
static uint32_t g_binding_tables_seen = 0, g_binding_tables_split = 0;
// Looks at a settings key without claiming it: always false, so the line is still read below.
static bool note_binding_table(const std::string& key) {
  static const std::string suffix = "_level";
  std::string base = key;
  if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) base.resize(base.size() - suffix.size());
  int tab = -1;
  std::string action;
  if (base.rfind("key_", 0) == 0) { tab = 0; action = base.substr(4); }
  else if (base.rfind("pad_", 0) == 0) { tab = 1; action = base.substr(4); }   // the oldest format: Xbox pad 1
  else {
    for (const BindingKeyPrefix& f : kBindingKeyPrefixes) {
      const size_t n = std::char_traits<char>::length(f.prefix);
      if (base.size() <= n + 2 || base.compare(0, n, f.prefix) != 0 || base[n] < '0' || base[n] > '3' || base[n + 1] != '_') continue;
      const int first = f.kind == host::CaptureDevice::XInputPad ? 1 : f.kind == host::CaptureDevice::DS4Pad ? 5
                      : f.kind == host::CaptureDevice::GCAdapter ? 9 : f.kind == host::CaptureDevice::SwitchPro ? 13 : 17;
      tab = first + (base[n] - '0'); action = base.substr(n + 2);
      break;
    }
  }
  if (tab < 0) return false;
  for (int i = 0; i < (int)host::BindAction::Count; ++i) {
    if (action != kActionNames[i]) continue;
    g_binding_tables_seen |= 1u << tab;
    if (host::is_analog_action(i)) g_binding_tables_split |= 1u << tab;
  }
  return false;
}
static void split_old_trigger_tables() {
  for (int tab = 0; tab < kDeviceTabs; ++tab) {
    if (!(g_binding_tables_seen & (1u << tab)) || (g_binding_tables_split & (1u << tab))) continue;
    int index; const host::CaptureDevice kind = tab_kind_of(tab, &index);
    host::ProfileBindings b = bindings_of(tab);
    split_old_trigger_bindings(kind, index, b);
    set_bindings_of(tab, b);
  }
}

static void capture_default_bindings() {
  if (g_defaults_captured) return;
  for (int t = 0; t < kDeviceTabs; ++t) g_default_bindings[t] = bindings_of(t);
  g_defaults_captured = true;
}
bool bindings_are_default(int tab) { return bindings_of(tab) == g_default_bindings[tab]; }
std::string active_profile_name(int tab) { return g_active_profile[tab].empty() ? std::string("Profile 1") : g_active_profile[tab]; }
host::ProfileDevice profile_device_of_tab(int tab) { int index; return profile_device_for(tab_kind_of(tab, &index)); }
// The first "Profile N" not yet used for this kind of controller.
std::string next_free_profile_name(host::ProfileDevice device) {
  const std::vector<std::string> list = host::profile_list(device);
  for (int n = 1;; ++n) {
    const std::string name = "Profile " + std::to_string(n);
    if (std::find(list.begin(), list.end(), name) == list.end()) return name;
  }
}

// ---- the settings file's controller lines (settings_file.cpp) ----
void controls_begin_load() {
  host::ds4_bindings_defaults();    // before the defaults are captured and the saved buttons read
  capture_default_bindings();   // the built-in buttons, before the saved ones replace them
  for (host::Deadzone& dz : host::g_deadzones) dz.click_l = dz.click_r = host::kDefaultTriggerClick;
  reset_binding_levels();   // a file without the keys: every binding as it always was
  for (int slot = 0; slot < host::kHidPadSlots; ++slot) {
    host::hidpad_set_axis_roles(slot, host::kHidAutomaticAxes);
    host::g_hid_stick_invert[slot].store(0, std::memory_order_relaxed);
  }
  g_binding_tables_seen = g_binding_tables_split = 0;
}

bool load_controls_key(const std::string& key, const std::string& value) {
  // "activeprofile<device> <name>": the profile each controller uses, so it is still the one
  // shown (and the one changes are saved to) after a restart.
  if (key.rfind("activeprofile", 0) == 0 && key.size() > 13 && std::isdigit((unsigned char)key[13])) {
    const int t = std::atoi(key.c_str() + 13);
    if (t >= 0 && t < kDeviceTabs) { g_active_profile[t] = value; g_named_profile_active[t] = true; }
  }
  else if (note_binding_table(key)) {}   // never true: it only notes which controller the line is for
  else if (load_family_option(key, value)) {}
  else if (load_binding_level(key, value)) {}
  else if (key.rfind("key_", 0) == 0) {
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      if (key == std::string("key_") + kActionNames[i]) host::g_key_bindings.vk[i] = std::stoi(value);
  }
  // Legacy pre-multi-device format: a single unindexed "pad_<Action>" line.
  // Migrate it onto XInput pad 0 so upgrading doesn't silently reset bindings.
  else if (key.rfind("pad_", 0) == 0) {
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      if (key == std::string("pad_") + kActionNames[i]) host::g_pad_bindings[0].mask[i] = (unsigned short)std::stoi(value);
  }
  // Current format: "pad<idx>_<Action>" / "gc<idx>_<Action>", idx 0-3.
  else if (key.size() > 4 && key.rfind("pad", 0) == 0 && std::isdigit((unsigned char)key[3])) {
    size_t us = key.find('_');
    if (us != std::string::npos) {
      int idx = std::stoi(key.substr(3, us - 3));
      std::string action = key.substr(us + 1);
      if (idx >= 0 && idx < 4)
        for (int i = 0; i < (int)host::BindAction::Count; ++i)
          if (action == kActionNames[i]) host::g_pad_bindings[idx].mask[i] = (unsigned short)std::stoi(value);
    }
  }
  else if (key.size() > 3 && key.rfind("gc", 0) == 0 && std::isdigit((unsigned char)key[2])) {
    size_t us = key.find('_');
    if (us != std::string::npos) {
      int idx = std::stoi(key.substr(2, us - 2));
      std::string action = key.substr(us + 1);
      if (idx >= 0 && idx < 4)
        for (int i = 0; i < (int)host::BindAction::Count; ++i)
          if (action == kActionNames[i]) host::g_gc_bindings[idx].mask[i] = (unsigned short)std::stoi(value);
    }
  }
  else if (key.size() > 4 && key.rfind("ds4", 0) == 0 && std::isdigit((unsigned char)key[3])) {
    size_t us = key.find('_');
    if (us != std::string::npos) {
      int idx = std::stoi(key.substr(3, us - 3));
      std::string action = key.substr(us + 1);
      if (idx >= 0 && idx < 4)
        for (int i = 0; i < (int)host::BindAction::Count; ++i)
          if (action == kActionNames[i]) host::g_ds4_bindings[idx].mask[i] = (unsigned short)std::stoi(value);
    }
  }
  else if (key.size() > 6 && key.rfind("swpro", 0) == 0 && std::isdigit((unsigned char)key[5])) {
    size_t us = key.find('_');
    if (us != std::string::npos) {
      int idx = std::stoi(key.substr(5, us - 5));
      std::string action = key.substr(us + 1);
      if (idx >= 0 && idx < 4)
        for (int i = 0; i < (int)host::BindAction::Count; ++i)
          if (action == kActionNames[i]) host::g_swpro_bindings[idx].mask[i] = (unsigned short)std::stoi(value);
    }
  }
  // Generic HID pads. The mask is 32 bits, so this parses as unsigned rather than int.
  // "hid<idx>_invert" is the stick and C-stick inversion (bits, Controls > Box layouts). It was
  // read back as "pad<idx>_invert", a key nothing writes, so the choice was lost at every launch.
  else if (key.size() > 4 && key.rfind("hid", 0) == 0 && std::isdigit((unsigned char)key[3])) {
    size_t us = key.find('_');
    if (us != std::string::npos) {
      int idx = std::stoi(key.substr(3, us - 3));
      std::string action = key.substr(us + 1);
      if (idx >= 0 && idx < host::kHidPadSlots && action == "axes") {
        std::istringstream values(value);
        auto roles = host::kHidAutomaticAxes;
        unsigned assigned = 0;
        for (auto& role_axis : roles) {
          int axis;
          if (!(values >> axis)) break;
          if (axis < 0 || axis >= 7 || (assigned & (1u << axis))) continue;
          assigned |= 1u << axis;
          role_axis = (uint8_t)axis;
        }
        host::hidpad_set_axis_roles(idx, roles);
      } else if (idx >= 0 && idx < 4 && action == "invert")
        host::g_hid_stick_invert[idx].store((uint8_t)(std::stoi(value) & 15), std::memory_order_relaxed);
      else if (idx >= 0 && idx < 4)
        for (int i = 0; i < (int)host::BindAction::Count; ++i)
          if (action == kActionNames[i]) host::g_hid_bindings[idx].mask[i] = (uint32_t)std::stoul(value);
    }
  }
  // "port<n> <comboIndex>" - comboIndex uses the same 0-9 encoding as the UI combo box.
  else if (key.size() == 9 && key.rfind("portname", 0) == 0 && key[8] >= '0' && key[8] <= '3') host::g_port_device_names[key[8] - '0'] = value;
  else if (key.size() > 4 && key.rfind("port", 0) == 0 && std::isdigit((unsigned char)key[4])) {
    int n = std::stoi(key.substr(4));
    if (n >= 0 && n < 4) host::g_port_sources[n] = combo_to_port_source(std::stoi(value));
  }
  else return false;
  return true;
}

void controls_end_load() { split_old_trigger_tables(); }

void write_controls_lines(std::ostream& file) {
  for (int i = 0; i < (int)host::BindAction::Count; ++i)
    file << "\nkey_" << kActionNames[i] << " " << host::g_key_bindings.vk[i];
  for (int idx = 0; idx < 4; ++idx)
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      file << "\npad" << idx << "_" << kActionNames[i] << " " << host::g_pad_bindings[idx].mask[i];
  for (int idx = 0; idx < 4; ++idx)
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      file << "\ngc" << idx << "_" << kActionNames[i] << " " << host::g_gc_bindings[idx].mask[i];
  for (int idx = 0; idx < 4; ++idx)
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      file << "\nds4" << idx << "_" << kActionNames[i] << " " << host::g_ds4_bindings[idx].mask[i];
  for (int idx = 0; idx < 4; ++idx)
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      file << "\nswpro" << idx << "_" << kActionNames[i] << " " << host::g_swpro_bindings[idx].mask[i];
  for (int idx = 0; idx < 4; ++idx)
    for (int i = 0; i < (int)host::BindAction::Count; ++i)
      file << "\nhid" << idx << "_" << kActionNames[i] << " " << host::g_hid_bindings[idx].mask[i];
  for (int idx = 0; idx < host::kHidPadSlots; ++idx) {
    const int invert = host::g_hid_stick_invert[idx].load(std::memory_order_relaxed);
    if (invert) file << "\nhid" << idx << "_invert " << invert;
    const auto roles = host::hidpad_axis_roles(idx);
    if (roles == host::kHidAutomaticAxes) continue;
    file << "\nhid" << idx << "_axes";
    for (const uint8_t axis : roles) file << " " << (axis == host::kHidAxisAuto ? -1 : (int)axis);
  }
  file << binding_levels_text();
  for (int n = 0; n < 4; ++n)
    file << "\nport" << n << " " << port_source_to_combo(host::g_port_sources[n]);
  for (int n = 0; n < 4; ++n)
    if (!host::g_port_device_names[n].empty()) file << "\nportname" << n << " " << host::g_port_device_names[n];
  // Only while a named profile (not "Default") is actually active: g_active_profile also holds a
  // pre-allocated name while Default is selected (so an unprompted rebind has somewhere to save
  // to), and writing that out would read back next launch as if that profile were chosen.
  for (int t = 0; t < kDeviceTabs; ++t)
    if (g_named_profile_active[t] && !g_active_profile[t].empty()) file << "\nactiveprofile" << t << " " << g_active_profile[t];
}

}  // namespace gx
