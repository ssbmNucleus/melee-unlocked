// Installed skins as costume slots of their own on the character select (the Source Port).
//
// Each fighter's X / Y costume cycle is its retail colours followed by the costumes the player
// installed for it (imports: the Mods menu, the Mods tab; not a mod disc's or a look's, which keep
// L / R), in install order (the catalog's), at most kMaxPerFighter. Such a slot is presentation
// only: the game keeps the retail costume the skin was made for as the port's costume (what the
// simulation, the 0x82 pick online and every table indexed by costume see), and only that port's
// fighter is handed the skin's file in place of that costume's (served_for below; the game's half is
// sourceport/game/shim/mu_css_skins.c and mu_port_costume.c).
//
// What a port is handed online is the skin's served form (costume_served_form.h): its meshes,
// materials and textures on the retail skeleton bit for bit, so a match plays the same with or without
// skins. Only the peer form (costume_served_form.h wearer_form, by own_skin_policy, which is
// peer_skin_policy: any rest pose replaced, the other skeleton checks strict; what an opponent's machine
// builds from the same file, so a skin that passes it is announced). A skin it refuses is offline only:
// online its slot plays the retail costume it was made for, on both machines (the log says why, and the
// Mods menu's list says "Offline only", plays_online below). So online a skin is shown on both machines
// on a skeleton bit-equal to retail, or on neither. Offline (owner, 2026-11-24) the port is handed
// costume_offline_form: the strict form when the skin passes it, else the skin as installed, its own
// bones and all, whenever the game can load it; a skin that fails that cannot be picked. The forms are
// built once per skin, slot and mode.
//
// Pictures: the skin's own portrait (csp.png kept at install) for the select screen, 136x188, and
// its stock icon (stc.png, or the card's stock_url) for the match HUD and the results, 24x24, both
// console RGBA8 through the roster's reader (roster.h console_picture). None: the game keeps the
// retail picture of the slot.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace host::costume_slots {

constexpr int kMaxPerFighter = 16;

struct Slot {
  std::string asset_id, name;
  std::string target;     // the retail costume file it is made for, "PlMrNr.dat"
  int costume = -1;       // that costume's number in the game's order
  bool has_csp = false, has_stock = false;
};

// The installed costumes of a character select fighter (0 Captain Falcon to 25 Ganondorf; the Ice
// Climbers by Popo's files), in install order, at most kMaxPerFighter. `generation` changes whenever
// the catalog does (an install, a removal).
std::vector<Slot> list(int character, uint32_t* generation = nullptr);
uint32_t generation();

// A port's pick from the character select: `index` 1.. is the fighter's slot of that number in
// list(), 0 the retail colour. `local_online`: the online select screen's own door (the player's pick
// whatever port the session gives them). Refused (false, with the reason) when there is no such slot
// or its served form cannot be built; the pick is then the retail colour. *costume: the retail
// costume the slot keeps.
bool pick(int port, int character, int index, bool local_online, int* costume, std::string* why);
// Every pick forgotten (a new select screen session is not needed for this; the game re-sends its
// picks each frame they change).
void clear();

// The file `port`'s fighter loads in place of `slot` ("PlMrNr.dat", or the partner file of a
// two-file pick), from that port's pick; null when the pick does not cover that slot. `online`:
// only the local player's own pick counts (`local_port`, the session's port for this machine).
// With MELEE_TEST_SLOTS_RETAIL in a run started by automation, null always (the same picks, retail
// files: the trace comparison). *asset_id names the skin served.
std::shared_ptr<const std::vector<uint8_t>> served_for(int port, const std::string& slot, bool online, int local_port,
                                                       std::string* asset_id);
// The online pick's skin for (character, costume), when the local player picked one and its peer
// form passed (what an opponent's machine would build), else "". *partner_asset: the partner file's
// skin for a two-file pick, when it has one that passes.
std::string online_announce(int character, int costume, std::string* partner_asset);

// Whether the installed skin `asset_id` has an online form for `slot` (its own slot, "PlMrNr.dat"),
// the form its slot plays online; built and kept as a pick's is. False: offline only (*why: the
// reason), or no such skin.
bool plays_online(const std::string& asset_id, const std::string& slot, std::string* why);

// A slot's picture as a console RGBA8 texture: what 1 the portrait (136x188), 2 the stock icon
// (24x24). Empty when it has none. Converted once and kept.
const std::vector<uint8_t>* picture(int character, int index, int what);
// The stock icon of the skin last served to `port` (24x24), or null.
const std::vector<uint8_t>* worn_stock(int port);

}  // namespace host::costume_slots
