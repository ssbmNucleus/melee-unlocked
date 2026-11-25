// Controller settings as the settings file keeps them (settings_controls.cpp): the binding tables'
// key names, which device feeds each port, per-family stick options, per-binding levels, and which
// profile each controller uses. settings_file.cpp reads and writes the lines through the four
// functions at the bottom; the Controls page edits the same tables.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"            // host::PadState, which input_bindings.h uses without declaring
#include "input_bindings.h"
#include "settings_profile_bindings.h"
#include <cstdint>
#include <ostream>
#include <string>

namespace gx {

// Names used both when drawing the Controls list and when saving/loading bindings
// to port-settings.ini (as "key_<name>" / "pad<idx>_<name>" / "gc<idx>_<name>" lines).
// Order must match host::BindAction exactly.
extern const char* kActionNames[(size_t)host::BindAction::Count];

// Port-source <-> combo-box index, shared by load/save and the Port assignment UI. Saved settings
// store the index, so new devices are appended and the existing ones never move.
constexpr int kPortSourceCount = 22;
extern const char* kPortSourceNames[kPortSourceCount];
int port_source_to_combo(const host::PortSource& s);
host::PortSource combo_to_port_source(int idx);

// The per-family stick options as their settings lines (deadzone_<family>_main etc).
std::string family_options_text();

// The per-binding analog level (input_bindings.h), saved as "<the binding's key>_level".
uint8_t* binding_level(host::CaptureDevice kind, int index, int action);
// What the level means on this row: 0 nothing, 1 the press point of an analog source, 2 how far a
// button bound to L analog or R analog presses the trigger.
int binding_level_kind(host::CaptureDevice kind, int index, int action);

// ---- profiles ----
// Device numbers are the panel's (0 keyboard, 1-4 Xbox, 5-8 PlayStation, 9-12 adapter, 13-16
// Switch, 17-20 box/HID).
host::ProfileDevice profile_device_for(host::CaptureDevice kind);
extern host::ProfileBindings g_default_bindings[kDeviceTabs];   // captured before the settings file is read
extern bool g_defaults_captured;
extern std::string g_active_profile[kDeviceTabs];
// Whether a named profile (rather than "Default") is the active selection.
extern bool g_named_profile_active[kDeviceTabs];
// One table saved before L and R were split into the click and the travel, carried over so it plays
// exactly as it did.
void split_old_trigger_bindings(host::CaptureDevice kind, int index, host::ProfileBindings& b);
bool bindings_are_default(int tab);
std::string active_profile_name(int tab);
host::ProfileDevice profile_device_of_tab(int tab);
// The first "Profile N" not yet used for this kind of controller.
std::string next_free_profile_name(host::ProfileDevice device);

// ---- the settings file's controller lines (settings_file.cpp) ----
// Before the file is read: the built-in defaults captured, levels and trigger clicks reset.
void controls_begin_load();
// One "key value" line: true when it is a controller line (it has then been read).
bool load_controls_key(const std::string& key, const std::string& value);
// After the whole file: tables saved before the L/R split are carried over.
void controls_end_load();
// The controller lines, in the order the file has always had them.
void write_controls_lines(std::ostream& file);

}  // namespace gx
