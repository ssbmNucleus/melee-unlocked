// Doubles lobbies: SSBM Nucleus's directory of open Teams lobbies, the host's half.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A lobby is only a Slippi Teams code that four players type. The directory (docs/public-api.md of
// the site, "Doubles lobbies") lists open lobbies and hands the code to whoever joins; Slippi's own
// matchmaking then matches the four and carries the game. Nothing here ever reaches Slippi or an
// opponent: the feature only decides which code the game types into its normal Teams search.
//
// What this module sends: the host name the player shows (the Slippi display name filtered to the
// letters the directory takes, else "Player"), the ruleset the directory requires (always
// "competitive": the game offers no choice and never shows it), the game build ("retail" or the mod
// content's id) and the client ("netlee/<version>"). Never the play key, the uid or the connect code.
// The owner and join tokens stay in memory and are never logged.
//
// Every request runs on one worker thread of its own: the game's calls (CMD_LOBBY, and the Gallery's
// CMD_MOD_BROWSER 9 to 14 while the lobby list is open) only read and change state and never wait on
// the network. Etiquette as the directory asks: the list at most every 15 s and only while the list
// is open (plus one look when the lobby menu opens, to know whether the site offers lobbies at all),
// heartbeats every heartbeat_seconds, back-off of 5 s doubling to 60 s on errors, 429's Retry-After.
//
// Address: https://ssbmnucleus.net only. Test runs (host/test_env.h automated) may name a stand-in
// on this computer instead, MELEE_TEST_LOBBY_API=http://127.0.0.1:<port>[/<path>].
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host::lobby_directory {

// The player's side of a lobby, read on the game thread when it is needed.
struct Identity {
  std::string name;     // host_name_from()
  std::string build;    // build_string(); empty: this game cannot use lobbies
  std::string client;   // client_string()
};
using IdentitySource = Identity (*)();

// The Slippi display name as the directory takes it: letters, digits, spaces and . _ ! ? -, 1 to 15
// characters, no space at either end and no two in a row. "Player" when nothing is left, or when the
// name is the connect code (a player with no display name: the code is never sent).
std::string host_name_from(const std::string& display_name, const std::string& connect_code);
// "retail" for the retail game, else the first 16 hex digits of the mod content's id in lower case;
// empty when that id is not hex.
std::string build_string(bool mod_view, const std::string& fingerprint);
// "<wire name>/<version>", the version as x.y.z (0.0.0 when it is not one).
std::string client_string(const char* wire_name, const char* version);

// The player's setting ("doubleslobbies" in the settings file): on by default.
void set_enabled(bool on);
bool enabled();

// CMD_LOBBY (0xC9) from the game (shim/mu_lobby_list.c documents the requests). `allowed`: this run
// may offer lobbies at all (not Vanilla, not a replay).
void command(const uint8_t* p, size_t n, bool allowed, IdentitySource identity, std::vector<uint8_t>& reply);
// CMD_MOD_BROWSER 9 to 14 while the lobby list is open (the Gallery's list screen shows the lobbies):
// true when answered here.
bool gallery(const uint8_t* p, size_t n, std::vector<uint8_t>& reply);
// Leaving the game: a hosted lobby is closed and a join given up (a short, best-effort DELETE).
void shutdown();

}  // namespace host::lobby_directory
