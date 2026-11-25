// Menu mods: a menu screen's file from SSBM Nucleus (the character select's MnSlChr: its background,
// doors, frames and cursors), installed from the Mods menu's Menus (host/mods_menu.cpp) and served in
// place of the disc's copy (port/app/source_host_menu_mods.cpp, publish_menu_mods).
//
// Which files, and how:
//   MnSlChr.usd  the character select. The disc's copy is served as it is (Slippi's online select is
//                built from links into the disc's own file, SlippiCSS_gui.dat), so a mod's file replaces
//                it whole once it passes check(): no diff to apply after it.
//   others       not yet: MnSlMap.usd and MnMaAll.usd are Slippi's patched copies (online rows, the
//                frozen stages; the lettering the Mods menu, the lobby list and the options bar cut
//                from MnMaAll), GmPause.usd and IfAll.usd are read during a match. A card of those says
//                why (unsupported_reason) and is not offered.
// A menu file is only looked at on its screen, never by a match, so it is the same in every view,
// online included, and the opponent never sees it.
//
// One installed mod per file: installing a second for the same file replaces the first (the Mods
// menu asks first). Files live in <settings folder>/MenuMods: installed.json and one <id>.dat (and
// <id>.png, its picture) per mod.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host::menu_mods {

// The largest menu file taken: about twice the disc's character select (3.8 MB). A larger one is
// refused at install and, should one be on disk, not served.
constexpr uint64_t kMaxBytes = 8u * 1024u * 1024u;

struct Installed {
  std::string id;            // "m<file id>"
  std::string target;        // the disc file it replaces ("MnSlChr.usd")
  std::string name, creator, author, description, file_name;
  int mod_id = 0, file_id = 0, downloads = 0;
  std::string picture;       // its PNG (the post's first screenshot), absolute; empty when none
  uint64_t size = 0;
  std::string sha256;
};

// The disc file a site file replaces, from its name as the site's export reads it (frontend
// _lib/nucleusExport.js menuKindOf: "mnslchr" in a .dat or .usd name is the character select). Empty
// when it is not a menu screen this client takes.
std::string target_of(const std::string& file_name);
// What a menu screen file is ("Character select screen", "Stage select screen", "Pause screen", "HUD
// font"), empty when it is none.
std::string kind_of(const std::string& file_name);
// Why a menu screen file is not offered (empty when it is): one plain line.
std::string unsupported_reason(const std::string& file_name);

// The checks every menu file passes before it is installed and again before it is served: an HSD
// archive whose header, relocation table, root and reference tables and names lie inside the file,
// whose every relocated pointer points inside the file, with the same root symbols as the disc's own
// copy of `target` (`retail`), at most kMaxBytes. False with one plain line in `why`.
bool check(const std::string& target, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
           std::string* why);
// A file whose pointers reach past its data block (DAT Texture Wizard puts a texture that grew after
// the archive's tables; the game's reader refuses a pointer past the data) made into one the game
// reads: the data block grows to hold the whole file and the tables are written again after it. True
// when `bytes` is (now) such an archive; false, unchanged, when it is no archive at all.
bool normalize(std::vector<uint8_t>* bytes);

void configure(const std::string& settings_path);
// Installed mods, by name.
std::vector<Installed> list();
// The mod installed for `target`, if any.
bool installed_for(const std::string& target, Installed* out);
// Installs `bytes` for `info.target` (checked against `retail`), replacing the mod installed for that
// file before (its name in `replaced`). `picture_png`: the post's picture for the lists, may be empty.
bool install(const Installed& info, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
             const std::string& picture_png, std::string* error, std::string* replaced = nullptr);
bool remove(const std::string& id, std::string* error, std::string* target = nullptr);
// The bytes to serve for `target`: the installed mod's file, checked again against `retail`. False (the
// disc's copy is served) when none is installed or the file fails a check, said once in the log.
bool served(const std::string& target, const std::vector<uint8_t>& retail, std::vector<uint8_t>* bytes,
            std::string* name = nullptr);
// The files this client can serve a menu mod for.
const std::vector<std::string>& targets();

}  // namespace host::menu_mods
