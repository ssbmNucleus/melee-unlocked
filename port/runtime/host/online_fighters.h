// Custom fighters online (docs/online-custom-content.md): fighters installed beside the game, picked in
// Direct and Teams against other Nucleus players, each machine playing its own copy of the identical
// package. Nothing is downloaded: a fighter is selectable online only while every player in the
// session has the same package installed; otherwise its icon is greyed out on the character select,
// with a line saying why, and A on it plays the error sound.
//
// Each machine sends every other player the list of its offered fighters by content hash, each with
// its own id for it (hle/mu_content_wire.h, NP_MSG_MU_CONTENT). A peer's 0x82 that locks in an added
// fighter carries the peer's id; this machine maps it to its own fighter with the same content hash
// through the peer's list.
//
// The content hash covers what the match reads: SHA-256 over "nucleus-fighter-2", the roster record
// the game is handed, and each file the roster serves for the fighter (name and SHA-256 of its bytes)
// except the ones a match never opens: sound banks and music (.ssm, .sem, .hps), movies (.thp, .mth),
// and the intro, ending and results scenes (ftDemo*, GmRst*). Two players with the same fighter but
// other sounds are not kept apart; costume and effect files are in, since the match reads the
// skeleton from the costume file and runs the effects inside rollback.
//
// MELEE_TEST_NO_CUSTOM_FIGHTERS=1 turns it off (an older Nucleus): no capability bit, nothing offered.
// So does a release without Fighters (release_scope.h kFighters off).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mu_content_wire.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace host::online_fighters {

namespace cw = slippi::content_wire;

// The feature is on in this run (not switched off for a test).
bool enabled();

// ---- the roster (game thread, source_host_roster.cpp publish_roster) ----
// The roster was read again (host::roster::fighters()). `usable`: this run serves the roster as its
// fighter table (no m-ex disc, a fighters folder, not the retail view); added fighters are offered
// online only then.
void roster_published(bool usable);
// The last published roster is this run's fighter table (ids 26 and up are its fighters).
bool roster_usable();
// `id` is a roster fighter (26 + position) of the last published roster.
bool is_custom(uint8_t id);
// Bit k: roster fighter k may be offered online at all (no plugin of its own, no file name clash,
// within 16); and bit k for every roster fighter (the offline screens).
uint32_t offered_online();
uint32_t installed_mask();
// Hashes the offered fighters now if they are not yet (host thread: the online select screen's
// first look, and the first connected frame). Cached by file size and time.
void prepare();

// ---- the list this machine sends ----
// False until prepare() has hashed every offered fighter.
bool local_have(cw::Have* out);
// Changes whenever the list would (a roster read, hashing done).
uint32_t have_generation();

// ---- the session (slippi_online.cpp, host thread; the network thread for peer_have) ----
// A peer's list (bound to the sender already), and whether one came from that peer.
void peer_have(const cw::Have& have);
bool peer_list_known(uint8_t port);
struct Peer {
  uint8_t port = 0;        // the peer's player index
  bool build_known = false;
  bool takes = false;      // its build message set the custom fighters bit
};
// The players connected now, every frame the online select screen polls, and how long ago the
// connection was made (a build message missing after 3 s, or a list after 10 s, is a build that takes
// no added fighters). An empty list: no session (before a search connects).
void update_session(const std::vector<Peer>& peers, uint64_t connected_ms);
// A new netplay client, or the session ended: every peer forgotten.
void reset();

enum Verdict : uint8_t { kSelectable = 0, kPending = 1, kRefused = 2 };
// Whether this machine's fighter `id` may be locked in now; `why` is the player's words (for the
// select screen's line) when it is not. Before any peer is connected every offered fighter is.
Verdict verdict(uint8_t id, std::string* why);
// Bit k: roster fighter k is offered and selectable now (the online select screen).
uint32_t selectable_mask();
// The line for roster fighter k when it is not selectable ("P2 does not have Slippy installed.").
std::string reason(int k);
// Every player in `ports` sent a list holding the content of this machine's fighter `id`: its pick
// may go on the wire (NetplayClient::WireSelections). Unlike verdict(), it does not wait for
// update_session, so a pick sent the moment a connection is made is judged on what arrived.
bool every_peer_has(uint8_t id, const std::vector<uint8_t>& ports);
// A peer's lock-in of its fighter `sent_id`: this machine's fighter with the same content hash.
bool map_remote(uint8_t port, uint8_t sent_id, uint8_t* local_id, cw::Sha256* content);
// The content hash of a fighter of this machine (false when it has none or no such fighter).
bool content_of(uint8_t local_id, cw::Sha256* out);

// ---- the match (replays record it, metadata key "nucleusFighters") ----
// The match being set up: per port, the added fighter's content hash (all zero: none).
void set_match(const cw::Sha256* content, size_t ports);
// Per port (0-based) with an added fighter: "<content SHA-256 hex> <name>". Each machine records its
// own id for the fighter in Game Start; this is what names it anywhere. Empty: no added fighter.
std::map<uint8_t, std::string> match_fighters();

// ---- the select screen's line (MU_CMD_ONLINE_CONTENT, source_host_online.cpp) ----
void set_line(const std::string& text, bool spinner);
std::string line(bool* spinner);

// ---- pieces (tests, logs) ----
// A served file the match never opens, left out of the content hash.
bool presentation_only(const std::string& served_name);

}  // namespace host::online_fighters
