// The release switches: which of the Mods menu's six categories this run has. One table, in the menu's
// order (the game's shim/mu_mods_categories.c; the host's mods_menu kCharacters .. kMenus), decided per
// release without a rebuild (docs/guide/areas/mods-menu-core.md, "Release switches").
//
//   Characters  on, always           Fighters    off ("modsfighters 1" turns it on)
//   Stages      on, always           New Stages  off ("modsnewstages 1")
//   Particles   off ("modsparticles 1")   Menus  off ("modsmenus 1")
//
// Turning one on:
//   - the owner: its line in the settings file (port-settings.ini, "modsparticles 1"; "modsparticles=1"
//     is read too). Written back only while on, so a file a release wrote carries nothing and the next
//     release's defaults apply.
//   - tests: an automated run (--hidden, --headless, --script; test_env::automated) has every category,
//     so every existing harness plays as before. MELEE_TEST_MODS_SCOPE asks for less: "release" is the
//     table's defaults plus the settings file's lines; a list of keys adds those over them
//     ("particles,fighters"); "all" every one. Read only in an automated run. A public build
//     (NUCLEUS_PUBLIC_BUILD, public_build.h) has neither: every run, automated or not, is the table plus
//     the settings file's lines.
//
// A category that is off: Browse shows its row dimmed, its words "Coming soon.", A gives the menus'
// error sound and Manage leaves it out (the game, from MU_CMD_MOD_BROWSER 8's second byte, mask()). The
// host refuses its list and installs (source_host_mods_menu.cpp), and what is installed of it stays on
// disk unloaded, one log line a category (gate()):
//   Fighters    no installed fighters on the roster, no fighter plugins (source_host_roster.cpp
//               installed_fighters_folder, plugin_source_folder), and online no custom-fighters
//               capability bit and no list (online_fighters.cpp enabled: SendBuild's caps4, SendContentHave)
//   New Stages  no added stages on the stage select, no pick served (source_host_new_stages.cpp
//               new_stages_offline)
//   Particles   installed effects not applied in matches (cosmetic_publish.cpp publish_locked)
//   Menus       installed menu mods not published (source_host_menu_mods.cpp publish_menu_mods)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace host::release_scope {

enum Category { kCharacters = 0, kStages = 1, kParticles = 2, kFighters = 3, kNewStages = 4, kMenus = 5, kCategories = 6 };

struct Entry {
  const char* name;      // as the menu's row reads ("Particles": the effects)
  const char* key;       // MELEE_TEST_MODS_SCOPE's word for it; nullptr: always on
  const char* setting;   // its settings file line; nullptr: always on
  bool on_by_default;    // in a player's run with no line
};

// The one table: what the first release ships.
inline constexpr Entry kTable[kCategories] = {
    {"Characters", nullptr, nullptr, true},
    {"Stages", nullptr, nullptr, true},
    {"Particles", "particles", "modsparticles", false},
    {"Fighters", "fighters", "modsfighters", false},
    {"New Stages", "newstages", "modsnewstages", false},
    {"Menus", "menus", "modsmenus", false},
};

// Whether `category` is on in this run (false for a number outside the table).
bool on(int category);
// Bit c: category c is on (MU_CMD_MOD_BROWSER 8's second byte; the game greys the others).
uint8_t mask();
// on(category); when it is off, the first time a path asks says so in the log, once a category:
// "<what>" is what is not loaded ("installed fighters and their plugins are not loaded").
bool gate(int category, const char* what);
// One line for the log: "Characters, Stages on; Particles, Fighters, New Stages, Menus coming soon"
// and where that comes from.
std::string describe();

// ---- the settings file (gx/settings_file.cpp) ----
// Before a file is read: every switch as the table says.
void reset_settings();
// A settings line ("modsparticles", "1"; also "modsparticles=1" as the key with no value). True when
// it was one of these.
bool load_setting(const std::string& key, const std::string& value);
// The lines to write: "\nmodsparticles 1" for each switch the file turned on, nothing for the rest.
std::string settings_lines();
// The file's own choice for `category` (the table's default without a line).
bool setting(int category);
void set_setting(int category, bool on);   // tests

}  // namespace host::release_scope
