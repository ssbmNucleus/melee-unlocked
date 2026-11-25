// Slippi Online command handling for the EXI device: matchmaking, netplay input exchange,
// rollback savestates, chat, match state. Port of the online half of Dolphin's CEXISlippi.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <memory>
#include "native_online_policy.h"


namespace slippi { class Matchmaking; }

namespace slippi::online {

struct Config {
  std::string lobby_code;        // --lobby-direct: one Direct match at boot, then exit; never a Discord invite.
  int lobby_character = 2;       // external character id (a mod disc has more than 26)
  std::string user_dir = "runtime/slippi/User/Slippi";   // user.json, direct-codes.json (Slippi Launcher layout)
  int delay = 2;                 // Slippi Online input delay (frames)
  bool auto_delay = false;       // pick the delay from the measured ping each match (delay is the fallback)
  int chat = 0;                  // 0 enabled, 1 direct only, 2 disabled
  // The network thread advertises this preference at connection time.
  std::atomic<bool> direct_pause{true};
  // Items in Direct and Teams ("onlineitems", off by default): this player's choice. Items are on for a
  // match only when every player's game reads the rule and every player has it on (items_option).
  std::atomic<bool> items{false};
  // Squad Strike in Direct ("onlinesquad", off by default): this player's choice. A Direct match is a
  // squad match only when every player's game plays it and every player has it on (squad_option).
  std::atomic<bool> squad{false};
  // Online port colour, first and second choice (0 red, 1 blue, 2 yellow, 3 green; -1 none). In a 1v1 each
  // player shows in their first choice; when both pick the same, the match's random seed gives it to one
  // and the other gets their second choice. Only between players who both have it (MU_BUILD port colours).
  int port_colour[2] = {-1, -1};
  // Up to three connect codes never to play in Unranked: a match with one is turned down and the
  // search goes on. Not in Ranked, Direct, Teams or Party.
  std::string blocked_codes[3];
};
Config& config();
// This online 1v1's port colours as bits 28-31 of the display options (MU_DISPLAY_OPTION_PORT_COLOURS), 0 none.
uint32_t port_colour_bits();

void init();
void request_boot_online();
void shutdown();
// Each engine sets this before its game starts; Static mod discs use OtherMod.
void set_native_gameplay_profile(NativeGameplayProfile profile);

// What this client plays online, sent to the opponent after connecting (a netplay message stock
// Slippi Dolphin ignores). mod_view false = the retail game; Source can also select its retail
// content view. In Direct a mod build plays only against the same
// build; allow_unverified lets it play an opponent who sends no build (Slippi Dolphin with the same
// mod), at the player's word.
struct LocalBuild {
  bool mod_view = false;
  std::string fingerprint;   // the mod content's identity (hex), empty for the retail game
  std::string name;          // shown in messages, e.g. "Akaneia 1.0.1"
  bool allow_unverified = false;
  bool extended_content = false;   // only Static mod boot runs the disc's added fighter/stage code
  bool static_engine = false;      // engine identity is independent of the selected content view
};
void set_local_build(const LocalBuild& build);
// Called with true when a Direct opponent sent no build while this side shows a mod (the opponent is
// on Slippi Dolphin, whose game is the retail one unless the player said otherwise), and with false
// when the next search starts. The host switches that match to the retail game and back.
void set_plain_opponent_handler(void (*handler)(bool plain, const char* why));
const LocalBuild& local_build();
// Handles one online command (cmd byte, payload after it); responses go to `read_queue`.
// Returns false for commands this module does not own.
bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& read_queue);
// Validates the fixed EXI payload size before native or legacy callers enter a command handler.
bool valid_command_payload(uint8_t cmd, const uint8_t* payload, uint32_t payload_len);
// Early inputs (host/early_input.h). enabled: the player's setting. accept: this engine also uses
// the other player's early inputs for its prediction, which only the Source Port can (the Static
// Recomp's prediction is Slippi's own game code); it is what the build message announces.
bool early_inputs_enabled();
bool early_inputs_accept();
// Counts rollback loads so the renderer can treat them as discontinuities.
uint64_t rollback_count();
// A native savestate load (the source port serves Slippi's savestate commands itself). `to_frame`
// is the frame loaded, for the session trace's rollback depth.
constexpr int32_t kRollbackFrameUnknown = INT32_MIN;
void note_rollback(int32_t to_frame = kRollbackFrameUnknown);
bool is_online_match();
// Gecko codes agreed for the match (gecko_session.h): call every character select frame.
void gecko_pump();
// A raw event emitted by either engine, including its command byte.
void record_report_event(const uint8_t* data, uint32_t size);
// The in-game slot the local player occupies in the running online match (0-3).
int local_player_slot();
// The shield colour the opponent on in-game `slot` sent after connecting (0x1000000 | RGB), 0 none.
uint32_t remote_shield_color(int slot);
// Simulation-thread snapshot for local display; names never enter the synchronized game state.
std::array<std::string, 4> player_names_for_overlay();
// Each player's Slippi display name and connect code ("ABC#123", UTF-8) in the Slippi online match
// about to be recorded, by port (0-3): the matchmaking result the match state block's names and
// codes come from, which the game copies into Game Start. The replay's metadata names (Dolphin's
// players.<port>.names). Empty strings for a port with no player and outside a Slippi online
// session (a peer-to-peer session included). Recording only: nothing here reaches the match.
struct RecordingName { std::string display_name, connect_code; };
std::array<RecordingName, 4> recording_names();
// Most recent measured round trip to the opponent, in milliseconds; 0 when not connected.
int ping_ms();
// Every other player's ping in a Slippi match (Teams: up to three), empty when there is none.
int remote_ping_count();
std::vector<int> remote_pings_ms();
// The online mode of the session that is running or being set up, as a Matchmaking::OnlinePlayMode
// value (RANKED 0, UNRANKED 1, DIRECT 2, TEAMS 3, PARTY 4). -1 when there is no online session at
// all, which is what "offline" means to the rest of the port. Covers matchmaking, the online
// character select screen and the match itself, so a feature can be gated on the mode before the
// match starts rather than only once it is running.
int session_mode();
// In-game player slot of the local player in an online match (0..3).
int local_player_index();
// True while the game is polling the online menus (mode select, the online character select screen,
// waiting for an opponent) and not running a match.
bool in_online_menus();
// Online submenu and CSS, including when the Friends row is hidden.
bool online_status_visible();
// Direct only: a connected opponent announced automatic L-cancel in its build message. Slippi
// Dolphin and builds before 0.9.200 never send it, so it is false against them.
bool opponent_auto_lcancel();

// A launcher lobby match or the --online-test harness starts its search with the same Slippi
// matchmaking as the game's online menus (simulation thread).
bool native_start_match(int mode, const std::string& connect_code, uint8_t character,
                        uint8_t color, std::string* error);
// The --online-test ranked harness: locks in the same character and colour again (a random legal
// stage) for the set's next game, when a ranked pair is connected and not locked in. True if it did.
bool native_ranked_next_game(uint8_t character, uint8_t color);

// The running (or last) online game as negotiated: stage id, the two players' characters, its rng
// offset and the last online frame. -1 when unknown.
struct GameFacts {
  int stage = -1, frames = -1;
  int characters[2] = {-1, -1};
  int teams[4] = {-1, -1, -1, -1}; // actual match block teams, -1 outside Teams
  int64_t rng = -1;   // the match's rng offset: the same on both players' games
};
GameFacts last_game_facts();
// Ports (bit per 0-based port) the netcode marked disconnected in the current or last match.
unsigned disconnected_ports();
// The Slippi account's connect code and display name; false when not logged in.
bool local_identity(std::string* code, std::string* name);

// The Items option of the online select (Direct and Teams; shim/mu_online_options.c through host
// command 0xDA). Items on means the game's own VS default: every item, frequency Medium. A match has
// them only when every player's game reads the rule (MU_BUILD_CAPS4_ITEMS) and every player chose On
// (NP_MSG_MU_ITEMS); the rule is written into the match block, so the setup hash covers it.
enum ItemsStatus : uint8_t {
  ITEMS_NO_PEERS = 0,      // no opponent yet: the choice is all there is
  ITEMS_ON = 1,            // every player has it on: this match has items
  ITEMS_OFF_MINE = 2,      // this player has it off
  ITEMS_OFF_THEIRS = 3,    // `blocker`'s player has it off
  ITEMS_UNSUPPORTED = 4,   // `blocker`'s game has no such rule (Slippi Dolphin, older builds): off, greyed
  ITEMS_OTHER_MODE = 5,    // not Direct or Teams: never items
};
struct ItemsOption {
  bool choice = false;     // this player's choice (the "onlineitems" setting)
  ItemsStatus status = ITEMS_NO_PEERS;
  int blocker = -1;        // the port (0-3) of the player keeping items off, -1 none or this player
  bool locked = false;     // locked in: the choice waits until this player unlocks
};
ItemsOption items_option();
// Whether this session offers the Items rule at all (the select's row): not in a Vanilla run
// (--vanilla-only, --vanilla-game), not peer-to-peer, and only in Direct and Teams. The one source of
// both "offered" and the choice in effect: where it is not offered, items_choice() is Off whatever the
// saved "onlineitems" says (kept as saved), and Off is what goes to the peers (NetplayClient
// SendItemsChoice) and into Items().
bool items_offered();
bool items_choice();
// Sets this player's choice (and tells the peers that read the rule). False, unchanged, while locked
// in, against a game without the rule, or where the rule is not offered.
bool set_items_choice(bool on);

// Squad Strike in Direct (mu_squad_wire.h version 4): the setup of the last match both sides were
// ready for, empty for a normal match (the game asks with MU_SQUAD_OP_MATCH_SETUP).
std::vector<uint8_t> squad_match_setup();
// The Squad Strike row of the online select (Direct only; shim/mu_online_options.c through host command
// 0xDA option 2), with the Items row's statuses (ItemsStatus values): a Direct match is a squad match
// only when every player's game plays it (MU_BUILD_CAPS4_SQUAD) and every player chose On
// (NP_MSG_MU_SQUAD). squad_offered/squad_choice as items_offered/items_choice: not in a Vanilla run, not
// peer-to-peer, Direct only, and never in a build without Squad Strike; where it is not offered the
// choice in effect (what SendSquad sends and the verdict reads) is Off whatever the saved "onlinesquad"
// says (kept as saved). set_squad_choice as set_items_choice. set_squad_lineup: this player's
// lineup from the online select ({kind, color} pairs in fighting order, 0-4 of them), sent with the
// choice on each change and before each lock-in.
bool squad_offered();
bool squad_choice();
ItemsOption squad_option();
bool set_squad_choice(bool on);
void set_squad_lineup(const uint8_t* pairs, uint8_t count);
// The lineup this player sends (pairs filled, the count returned): the select's, or a test's.
uint8_t squad_local_lineup(uint8_t pairs[8]);

}  // namespace slippi::online
