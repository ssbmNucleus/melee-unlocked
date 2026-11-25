// Opponents' skins online, the receiving machine's half: for each opponent port, the skin that
// opponent named for its latest pick, fetched, checked and turned into the file this machine would
// serve in place of the retail costume of that pick. The game asks for it per port when it creates
// that port's fighter (served_costume below; sourceport/game/shim/mu_port_costume.c).
//
// Flow. The opponent's machine sends a skins message (hle/mu_skin_wire.h, NP_MSG_MU_SKINS) just
// before each 0x82 pick. slippi_net.cpp keeps it, and when that port's 0x82 arrives calls
// opponent_selected() with the pick and the message's records (none when no message came first:
// that port wears the retail costume). A worker thread then, per record:
//   1. checks the record names the pick's character and costume;
//   2. reads the slot's costume file from the open disc and takes it as the reference only if it is
//      the retail NTSC 1.02 file (size and SHA-256, retail_costume_hashes.h), mod disc or session
//      pack or not;
//   3. fetches the skin by id and checks its hash before reading it (skin_fetch.h);
//   4. takes the costume file out of the download (a zip of costume files, or one DAT): the one
//      made for the slot, else the only costume of that fighter in it;
//   5. builds the served form against the retail file (costume_served_form.h, peer_skin_policy: the
//      retail skeleton bit for bit, any rest pose replaced, the other skeleton checks strict).
// The partner file of a two-file pick (Nana, or Zelda's other form) goes the same way for its own
// slot. Each step logs one line on failure, and the outcome is logged per port.
//
// The API for the game side, "bytes for port N?": port_skin(N). kReady carries the bytes to serve in
// place of `slot` (and `partner_bytes` for `partner_slot` when the partner passed); any other state
// means serve the retail costume. A newer pick of the same port replaces the entry at once (kWorking
// again, or kNone), and work still running for an older pick is thrown away when it finishes.
// wait_port() is the bounded wait for a loading screen (step 6's host half).
//
// The skin's own stock icon (the HUD's stocks, the results), derived from the same ids: once the skin
// is ready, a second job (queued behind every port's skin, so no skin waits for a picture) fetches the
// card's stock icon from the site (skin_fetch.h fetch_stock_icon: checked, kept beside the skin) and
// makes it the 24x24 console texture the player's own skin's icon is (costume_slots.h). worn_stock()
// answers for a port whose fighter wears the opponent's skin (served_costume handed it over): the
// picture, "on its way" (the game asks again a little later, never waiting), or none (the retail
// icon: no skin worn, a skin with no icon of its own, or one refused). Presentation only, as the skin:
// nothing on the wire, the setup hash or the simulation changes.
//
// The whole feature sits behind one setting, "opponentskins" in the settings file, on by default
// (from 0.1.0; a file with no such line means on, only "opponentskins 0" turns it off).
// Off: the build message does not offer the capability, nothing is sent, a message that arrives
// anyway is dropped, nothing is fetched and every port is kNone.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mu_skin_wire.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace host::skins {

void set_enabled(bool on);
bool enabled();

enum class PortState { kNone = 0, kWorking = 1, kReady = 2, kRefused = 3 };
const char* state_name(PortState state);

struct PortSkin {
  PortState state = PortState::kNone;
  int character = -1, costume = 0;          // the pick it is for
  std::string slot, partner_slot;           // "PlFxGr.dat"; partner_slot empty for a one-file pick
  std::shared_ptr<const std::vector<uint8_t>> bytes;           // kReady: serve in place of `slot`
  std::shared_ptr<const std::vector<uint8_t>> partner_bytes;   // kReady and the partner passed, else null
  // kReady: the costume file taken out of the download for `slot` (and for `partner_slot`), before its
  // served form. When the game loads another costume of the same fighter for this port (Teams plays
  // each player in their team's color), served_costume builds that slot's served form from it.
  std::shared_ptr<const std::vector<uint8_t>> source, partner_source;
  uint64_t generation = 0;                  // the pick it is for (opponent_selected), for that cache
  std::string reason;                       // kRefused: why; kReady: a summary
  // kReady only: its own stock icon. kWorking while it is fetched, kReady with `stock` (24x24 console
  // RGBA8), kNone when the card has none of its own, kRefused when it could not be had or read.
  PortState stock_state = PortState::kNone;
  std::shared_ptr<const std::vector<uint8_t>> stock;
};

// ---- the game side's API (any thread) ----
PortSkin port_skin(int port);
// Waits up to `timeout_ms` for `port` to leave kWorking; the state it ended in.
PortState wait_port(int port, int timeout_ms);
// The stock icon of the opponent's skin `port`'s fighter wears (served_costume handed it over, for
// the port's latest pick): kReady and *pixels (24x24 console RGBA8); kWorking while it is fetched (ask
// again later); kNone otherwise (the retail icon). Never waits.
PortState worn_stock(int port, std::shared_ptr<const std::vector<uint8_t>>* pixels);

// ---- the network side (slippi_net.cpp, network thread; never blocks) ----
void opponent_selected(int port, uint8_t character, uint8_t costume, const std::vector<slippi::skin_wire::Record>& records);
// A new netplay client, or the session ended: every port forgotten, running work thrown away.
void reset();
// What this machine sends for its own pick (`port` is the local player's): the SSBM Nucleus ids and
// hash the mod browser recorded when it installed the skin picked as a costume slot for this pick
// (or else the skin this machine serves for the pick's costume file; record_origin below), and for
// a two-file pick the partner file's too when it has
// them; nothing (false) when that skin has none (imported by hand, or installed before ids were
// kept) or no skin is served. A run started by automation can name one instead in
// MELEE_TEST_SEND_SKIN ("<mod>:<file>:<sha256>[:<mod>:<file>:<sha256> partner]"), or ask for a broken
// message to test the receiver ("malformed", "otherport"). `payload` is the message after its id
// byte. Every call also notes the local player's port and pick for served_costume.
bool local_payload(uint8_t port, uint8_t character, uint8_t costume, std::vector<uint8_t>* payload);

// ---- the game's half: one port's own costume (CMD_MOD_BROWSER 7, source_host_mods_menu.cpp) ----
// The file the game loads for `port`'s fighter in place of `slot` (the costume file it would load,
// "PlFxGr.dat"), or nothing (false): the game then loads `slot` as before. First, with the setting on
// or off, the port's own costume slot (host/costume_slots.h: an installed skin picked on the select
// screen; offline any human port's, online the local player's). Then, with the setting off, nothing.
// Otherwise, in this order:
//   - a run started by automation: MELEE_TEST_PORT_SKIN="<port>:<file>[;<port>:<file>...]" (port 0-3,
//     a skin DAT or zip on this machine) gives that port the served form of the file's costume for
//     `slot`, offline or online (the offline per-port test);
//   - `online` only (an online session):
//     an opponent port whose skin is ready for `slot` (or for the partner slot): its served form;
//     an opponent port whose skin is ready for another costume of the same fighter than `slot` (Teams
//     puts the player in their team's color after the pick): the served form of the same costume
//     file for `slot`, built against `slot`'s own retail file (its skeleton and rest pose that slot's,
//     the peer rule an opponent's skin always gets), once per pick and slot, as the sender's own
//     machine wears it (costume_slots.h served_for);
//     the local player's port: the served form of this machine's own skin for `slot`, when one is
//     served and passes;
//     any other port whose `slot` this machine serves its own skin for: the retail file, so the
//     local player's skin is not drawn on an opponent who picked the same costume.
// Never waits: what is not ready is the retail costume. The answer is kept per port until the next
// call for that port, so the game's two calls (size, then bytes) see the same file.
bool served_costume(int port, const std::string& slot, bool online, std::shared_ptr<const std::vector<uint8_t>>* bytes);

// ---- installed skins' SSBM Nucleus ids (the mod browser records them) ----
// <settings dir>/CosmeticMods/nucleus-origins.json: per installed asset, the post's mod id, the file's
// id and the SHA-256 of the file as it was downloaded (`downloaded` is read and hashed here); for a
// file taken out of the mod's zip, also that zip's SHA-256 ("archive_sha256", kept for the record).
void record_origin(const std::string& asset_id, uint32_t mod_id, uint32_t file_id, const std::string& downloaded_path,
                   const std::string& archive_sha256 = {});
bool origin_of(const std::string& asset_id, slippi::skin_wire::FileRef* out);

// ---- pieces (tests) ----
// The other costume file of a two-file pick: Nana's slot for the Ice Climbers (14), Sheik's for Zelda
// (18), Zelda's for Sheik (19); empty for everyone else.
std::string partner_slot(uint8_t character, uint8_t costume);
// The costume number `slot` is for `character`'s pick: one of the fighter's own costume files
// (*partner false), or, when `with_partner`, the partner file of one (*partner true); -1 when `slot`
// is neither (served_costume's Teams color).
int costume_of_slot(uint8_t character, const std::string& slot, bool with_partner, bool* partner);
// A ready opponent skin's served form for `slot`, another costume of its fighter (or of the partner)
// than the one picked: the costume file it was made from (`source`, `partner_source`) against
// `retail_slot_bytes`, the retail file of `slot`, by the peer rule. Null with *why otherwise.
std::shared_ptr<const std::vector<uint8_t>> recolor_form(const PortSkin& remote, const std::string& slot,
                                                         const std::vector<uint8_t>& retail_slot_bytes, std::string* why);
// The open disc's file of `slot` when it is the retail NTSC 1.02 one.
bool retail_reference(const std::string& slot, std::vector<uint8_t>* bytes, std::string* why);
// The costume file for `slot` out of a download (a zip, unpacked under `scratch_dir`, or one DAT).
bool costume_from_download(const std::vector<uint8_t>& download, const std::string& slot, const std::string& scratch_dir,
                           std::vector<uint8_t>* dat, std::string* why);

}  // namespace host::skins
