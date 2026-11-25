// Slippi Online command handling (see slippi_online.h): port of the online half of Dolphin's
// CEXISlippi plus SlippiSavestate. The game-side Slippi codes drive everything: they ask for
// the match state each frame in the lobby, send local inputs and fetch remote ones each frame
// in a match, and capture/load savestates around rollbacks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_agent_lock.h"
#include "test_env.h"
#include "slippi_online.h"
#include "slippi_stage_pool.h"
#include "gecko_session.h"
#include "slippi_ranked_policy.h"
#include "slippi_match_quality.h"
#include "../host/test_env.h"
#include "../host/public_build.h"   // log_id: other players' codes in a public build's log
#include "../host/gameplay_policy.h"
#include <atomic>
#include <future>
#include "netplay_summary.h"
#include "native_slippi_bridge.h"
#include "slippi_net.h"
#include "slippi_gone_port.h"
#include "slippi_time_sync.h"
#include "slippi_input_suggestions.h"
#include "slippi_report.h"
#include "exi_slippi.h"
#include "host.h"
#include <chrono>
#include <thread>
#include "window.h"
#include "discord_presence.h"
#include "gx_core.h"
#include "cosmetic_mods.h"
#include "net_trace.h"
#include "netplay_state.h"
#include "early_input.h"
#include "render_options.h"
#include "online_fighters.h"   // custom fighters online (docs/online-custom-content.md)
#if MU_SQUAD
#include "squad_test_spec.h"   // Squad Strike in Direct: MELEE_TEST_ONLINE_SQUAD, mu_squad_wire.h
#endif
#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace slippi::online {
namespace {

Config g_config;

NativeGameplayProfile g_native_gameplay_profile = NativeGameplayProfile::Vanilla;
LocalBuild g_local_build;   // retail Source by default; each engine sets its build before playing
// The profile the online rules see: the mod's only while its files are what the game shows.
NativeGameplayProfile effective_profile() {
  return g_local_build.mod_view ? g_native_gameplay_profile : NativeGameplayProfile::Vanilla;
}
const char* local_matchmaking_error(int mode) {
  if (g_local_build.static_engine && g_local_build.mod_view &&
      (mode == 1 || mode == 4) &&
      native_profile_mode_error(effective_profile(), mode) && host::restart_with_mod_looks())
    return "Starting the standard game with this mod's looks for Unranked and Party...";
  if (const char* reason = native_profile_mode_error(effective_profile(), mode)) return reason;
  if (g_local_build.mod_view && !native_mod_fingerprint_valid(g_local_build.fingerprint)) {
    return "Could not verify this mod's content. Restart the game before playing Direct.";
  }
  return nullptr;
}
// Vanilla character select kinds are 0-25 and the stage kinds end with the heal stage (0x55); an id
// past those comes from a build with more content (m-ex adds its fighters and stages after them).
constexpr uint64_t kBuildWaitMs = 3000;   // how long a mod build waits for the opponent's build message
static void (*g_plain_opponent_handler)(bool, const char*) = nullptr;   // see set_plain_opponent_handler

// Command ids live in native_slippi_bridge.h, shared with the native game's call path.

inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void append_u32(std::vector<uint8_t>& q, uint32_t v) { for (int i = 3; i >= 0; --i) q.push_back((uint8_t)(v >> (8 * i))); }

// ---------------------------------------------------------------- savestates
// Exactly Dolphin's SlippiSavestate: full RAM regions minus the sound/VI exclusions, with the
// main heap bounds read from the game (0x804d76b8/0x804d76bc).
struct PreserveBlock { uint32_t address, length; bool operator==(const PreserveBlock& o) const { return address == o.address && length == o.length; } };
struct PreserveHash { size_t operator()(const PreserveBlock& b) const { return b.address ^ b.length; } };

class Savestate {
 public:
  struct Loc { uint32_t start, end; std::vector<uint8_t> data; };
  static bool force_init;
  Savestate() { initBackupLocs(); for (auto& l : locs_) l.data.resize(l.end - l.start); }
  void Capture() { for (auto& l : locs_) std::memcpy(l.data.data(), host::ptr(l.start, l.end - l.start), l.end - l.start); }
  void Load(const std::vector<PreserveBlock>& blocks) {
    for (auto& b : blocks) {
      auto& keep = preservation_[b];
      keep.resize(b.length);
      std::memcpy(keep.data(), host::ptr(b.address, b.length), b.length);
    }
    for (auto& l : locs_) { std::memcpy(host::ptr(l.start, l.end - l.start), l.data.data(), l.end - l.start); host::mark_ram_write(l.start, l.end - l.start); }
    for (auto& b : blocks) { std::memcpy(host::ptr(b.address, b.length), preservation_[b].data(), b.length); host::mark_ram_write(b.address, b.length); }
  }
 private:
  static std::vector<Loc> processed_;
  std::vector<Loc> locs_;
  std::unordered_map<PreserveBlock, std::vector<uint8_t>, PreserveHash> preservation_;

  void initBackupLocs() {
    struct Region { uint32_t start, end; };
    std::vector<Region> full = {
        {0x80005520, 0x80005940}, {0x803b7240, 0x804DEC00}, {0x8065c000, 0x8071b000}, {0x80bd5c40, 0x811AD5A0},
    };
    std::vector<PreserveBlock> exclude = {
        {0x804031A0, 0x24}, {0x80407FB4, 0x34C}, {0x80433C64, 0x1EE80}, {0x804A8D78, 0x17A68}, {0x804C28E0, 0x399C}, {0x804D7474, 0x8},
        {0x804D74F0, 0x50}, {0x804D7548, 0x4}, {0x804D7558, 0x24}, {0x804D7580, 0xC}, {0x804D759C, 0x4}, {0x804D7720, 0x4},
        {0x804D7744, 0x4}, {0x804D774C, 0x8}, {0x804D7758, 0x8}, {0x804D7788, 0x10}, {0x804D77C8, 0x4}, {0x804D77D0, 0x4},
        {0x804D77E0, 0x4}, {0x804DE358, 0x80}, {0x804DE800, 0x70},
        {0x804d6030, 0x4}, {0x804d603c, 0x4}, {0x804d7218, 0x4}, {0x804d7228, 0x8}, {0x804d7740, 0x4}, {0x804d7754, 0x4},
        {0x804d77bc, 0x4}, {0x804de7f0, 0x10},
        {0x804c0980, 0x15F8},
    };
    if (!processed_.empty() && !force_init) { locs_ = processed_; for (auto& l : locs_) l.data.clear(); return; }
    force_init = false;
    full[3].start = host::rd32(0x804d76b8);
    full[3].end = host::rd32(0x804d76bc);
    host::log("slippi: savestate heap region %08X-%08X", full[3].start, full[3].end);
    std::sort(exclude.begin(), exclude.end(), [](const PreserveBlock& a, const PreserveBlock& b) { return a.address < b.address; });
    std::vector<Loc> locs;
    for (auto& r : full) locs.push_back({r.start, r.end, {}});
    size_t idx = 0;
    for (PreserveBlock ipb : exclude) {
      while (ipb.length > 0) {
        while (idx < locs.size() && ipb.address >= locs[idx].end) ++idx;
        if (idx >= locs.size()) break;
        if (ipb.address < locs[idx].start) {
          int new_size = (int32_t)ipb.length - ((int32_t)locs[idx].start - (int32_t)ipb.address);
          ipb.length = new_size > 0 ? new_size : 0;
          ipb.address = locs[idx].start;
          continue;
        }
        int new_size = (int32_t)ipb.length - ((int32_t)locs[idx].end - (int32_t)ipb.address);
        if (locs[idx].end > ipb.address + ipb.length) locs.insert(locs.begin() + idx + 1, {ipb.address + ipb.length, locs[idx].end, {}});
        locs[idx].end = ipb.address;
        if (locs[idx].end <= locs[idx].start) locs.erase(locs.begin() + idx);
        new_size = new_size > 0 ? new_size : 0;
        ipb.address = ipb.address + (ipb.length - new_size);
        ipb.length = (uint32_t)new_size;
      }
    }
    processed_ = locs;
    locs_ = locs;
  }
};
bool Savestate::force_init = true;
std::vector<Savestate::Loc> Savestate::processed_;

// ---------------------------------------------------------------- state
std::unique_ptr<User> g_user;
std::unique_ptr<Matchmaking> g_matchmaking;
std::unique_ptr<NetplayClient> g_netplay;
std::unique_ptr<DirectCodes> g_direct_codes, g_teams_codes;
std::string g_discord_join_code;
uint64_t g_discord_join_expires_ms = 0;
std::map<int32_t, std::unique_ptr<Savestate>> g_active_savestates;
std::deque<std::unique_ptr<Savestate>> g_available_savestates;
PlayerSelections g_local_selections;
bool g_local_random_stage = false;
Matchmaking::MatchSearchSettings g_last_search;
Matchmaking::MatchmakeResult g_recent_mm_result;
bool g_ranked_completed = false;
std::shared_ptr<report::ReplayData> g_report_replay;
bool official_matchmaking() {
  return !Matchmaking::local_peer.enabled && !matchmaking_stand_in();
}
MatchQuality g_quality;
uint8_t g_local_player_index = 0, g_remote_player_index = 1;
std::vector<uint16_t> g_allowed_stages = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
std::vector<uint16_t> g_stage_pool;
const std::vector<uint16_t>& allowed_stages(int) { return g_allowed_stages; }
std::vector<PlayerSelections> g_overwrite_selections;
std::string g_forced_error;
// The match setup hash (mu_setup_hash.h) of the setup the game is offered now. Game thread only.
struct SetupHashState {
  bool have = false, agreed = false;
  MuSetupHash local{};
  uint64_t since_ms = 0;   // when this setup was first built: the wait for every peer's hash
  int logged[REMOTE_PLAYER_MAX] = {-1, -1, -1};   // the last decision logged per peer
  uint64_t logged_setup[REMOTE_PLAYER_MAX] = {};
  MuSetupHashPending pending[REMOTE_PLAYER_MAX] = {};   // a mismatch that has not stood long enough yet
  MuSetupHashWait wait[REMOTE_PLAYER_MAX] = {};         // each peer's latest hash: the wait counts from it too
};
SetupHashState g_setup_hash;
bool g_play_session_active = false;
uint32_t g_stall_frame_counts[REMOTE_PLAYER_MAX] = {};
int g_frames_to_skip = 0, g_frames_to_advance = 0, g_fall_behind = 0, g_fall_far_behind = 0;
bool g_currently_skipping = false, g_currently_advancing = false;
static bool g_poor_performance_logged = false;
std::mt19937 g_rng((uint32_t)time_ms());
uint64_t g_rollbacks = 0;
static diagnostics::Summary g_summary;
static void finish_summary(const char* result);
// For the one-line summary at the end of an online game (delay, ping, rollbacks, stalls).
static uint64_t g_match_rollbacks_start = 0, g_match_stalls = 0, g_match_input_waits = 0, g_match_advances = 0;
static int g_input_wait_frames = 0;   // length of the wait for remote inputs that is running now
static int g_input_wait_latest = 0;   // the other player's newest frame when that wait began
// Early inputs: the sampler thread's way to the netplay client. It only queues a packet; the
// mutex keeps the client alive across the call, and the sampler is stopped before the client goes.
static std::mutex g_early_send_mutex;
static void send_early_pad(int32_t target, const uint8_t pad[host::early_input::kPadBytes]) {
  std::lock_guard<std::mutex> lk(g_early_send_mutex);
  if (g_netplay) g_netplay->SendEarlyPad(target, pad);
}
static void send_suggestion(int32_t target, uint32_t seq, const uint8_t pad[host::early_input::kPadBytes]) {
  std::lock_guard<std::mutex> lk(g_early_send_mutex);
  if (g_netplay) g_netplay->SendSlippiPadSuggestion(target, seq, pad);
}
static void stop_early_inputs() {
  host::early_input::stop();
  suggestions::end_game();
}
static int g_match_delay = 0;
bool g_in_online_match = false;
GameFacts g_game_facts;   // the negotiated stage and characters, and the last online frame
// Session trace (net_trace.h): one record per online tick for the "Network and timing" overlay.
// A record stays open until the next tick asks for inputs, because its work time, its rollbacks
// and the frames presented during it are only known by then. Observation only.
static net_trace::Record g_trace_record;
static bool g_trace_desync_pending = false;   // a checksum disagreed: marked on the next traced tick
static bool g_trace_open = false;
static int32_t g_trace_offset_us = 0;   // the time sync offset as last measured (every 30 frames)
void trace_close_tick() {
  if (!g_trace_open) return;
  g_trace_open = false;
  g_trace_record.sim_ms = (float)host::last_sim_frame_ms();
  g_trace_record.presents = (uint16_t)std::min<uint32_t>(net_trace::take_presents(), 0xFFFFu);
  net_trace::push(g_trace_record);
}
// The match is over (game report) or the connection is gone: the last record goes out and the
// session trace file is closed. The file work is the trace's own thread's.
void trace_end_match() {
  trace_close_tick();
  net_trace::file_end();
}
// A savestate load in the open tick. `to_frame` is the frame loaded, or kRollbackFrameUnknown. The
// game asks for frame N's inputs first, finds the misprediction in that answer, and loads at the
// start of frame N: the depth is N minus the loaded frame, the game's own count of it.
void trace_rollback(int32_t to_frame) {
  if (!g_trace_open) return;
  if (g_trace_record.rollbacks < 0xFF) ++g_trace_record.rollbacks;
  if (to_frame == kRollbackFrameUnknown) return;
  const int depth = std::clamp(g_trace_record.frame - to_frame, 0, 0xFF);
  g_summary.max_rollback = std::max(g_summary.max_rollback, (uint32_t)depth);
  if (depth > g_trace_record.rollback_depth) g_trace_record.rollback_depth = (uint8_t)depth;
}
// Determinism oracle: the game hands us a checksum of its finalized state each frame and the
// opponent's client sends theirs; a mismatch is a desync between the two simulations.
std::map<int32_t, uint32_t> g_local_checksums;
uint32_t g_checksums_compared = 0, g_checksums_mismatched = 0;
int32_t g_last_checksum_frame[REMOTE_PLAYER_MAX] = {};   // per remote: every peer's checksums are compared, not only the first to report a frame
unsigned g_gone_ports = 0;
int g_gone_own_frames = -1;   // -1 until the match's first leave, then whether every peer reads a player who left by their own frames
static_assert(REMOTE_PLAYER_MAX == 3, "one entry each below");
int32_t g_gone_last_frame[REMOTE_PLAYER_MAX] = {slippi::kNoFrame, slippi::kNoFrame, slippi::kNoFrame};   // per remote: its last frame, the first time it is read after leaving

// Real remote machines, bounded before indexing connection-scoped build and setup-hash arrays.
uint8_t bounded_remote_machine_count() {
  const int n = g_netplay ? g_netplay->RemoteMachineCount() : g_matchmaking ? (int)g_matchmaking->RemotePlayerCount() : 0;
  return (uint8_t)std::min(n, REMOTE_PLAYER_MAX);
}

// Rollback has one entry per human port other than the main local port, including our partner.
uint8_t bounded_remote_port_count() {
  return uint8_t(g_netplay ? std::min(g_netplay->RemotePortCount(), REMOTE_PLAYER_MAX) : bounded_remote_machine_count());
}

bool is_disconnected() { return !g_netplay || g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED; }
bool chat_enabled() { return g_last_search.mode == Matchmaking::DIRECT ? (g_config.chat == 0 || g_config.chat == 1) : g_config.chat == 0; }

uint16_t random_stage(int mode) {
  const auto& stages = g_allowed_stages;
  // Doubles: no Fountain of Dreams, also not one a singles game left in the bag.
  stage_pool::prepare(g_stage_pool, stages, mode == Matchmaking::TEAMS);
  int i = (int)(g_rng() % g_stage_pool.size());
  uint16_t s = g_stage_pool[i];
  g_stage_pool.erase(g_stage_pool.begin() + i);
  return s;
}

// ---------------------------------------------------------------- Discord presence
// Both of these return on a single atomic load when the player has not enabled Discord presence,
// and neither ever touches a socket, a pipe or the filesystem on this thread: the presence module
// owns one thread of its own and these only hand it a small struct.
const char* mode_name(Matchmaking::OnlinePlayMode mode) {
  switch (mode) {
    case Matchmaking::RANKED: return "Ranked";
    case Matchmaking::UNRANKED: return "Unranked";
    case Matchmaking::DIRECT: return "Direct match";
    case Matchmaking::TEAMS: return "Teams";
    case Matchmaking::PARTY: return "Party";
  }
  return "Online";
}

// What the player is doing when they are not online. Melee routes every mode through one state
// machine (gm_1A3F.c, `static struct stateMachine state_machine` at 0x80479D30) whose first byte is
// routingInfo::curr_mode, a GameModeKind. Reading that one byte is how the presence can say
// "Training" instead of calling everything outside a netplay match "In the menus", which is what a
// player in training mode saw. Read only, on the simulation thread, and nothing is written back.
constexpr uint32_t kStateMachine = 0x80479D30;
const char* offline_mode_name() {
  // The native game keeps its state machine in its own image, not at the console address.
  if (host::game_image) return nullptr;
  switch (host::rd8(kStateMachine)) {
    case 0x02: return "VS Mode";
    case 0x03: return "Classic";
    case 0x04: return "Adventure";
    case 0x05: return "All-Star";
    case 0x0F: return "Target Test";
    case 0x10: return "Super Sudden Death";
    case 0x11: return "Invisible Melee";
    case 0x12: return "Slo-Mo Melee";
    case 0x13: return "Lightning Melee";
    case 0x1B: return "Tournament";
    case 0x1C: return "Training";
    case 0x1D: return "Tiny Melee";
    case 0x1E: return "Giant Melee";
    case 0x1F: return "Stamina Mode";
    case 0x20: return "Home-Run Contest";
    case 0x21: return "10-Man Melee";
    case 0x22: return "100-Man Melee";
    case 0x23: return "3-Minute Melee";
    case 0x24: return "15-Minute Melee";
    case 0x25: return "Endless Melee";
    case 0x26: return "Cruel Melee";
    case 0x2B: return "Event Match";
    // Title, menus, trophy gallery, boot and the rest are all honestly "the menus".
    default: return nullptr;
  }
}

void update_discord_presence(bool force = false) {
  if (!host::discord::enabled()) return;
  // prepare_online_match_state runs this every frame the online menus are up. Rebuilding twice a
  // second is already far more often than Discord accepts an update. Every caller is on the
  // simulation thread (all of them are reached through handle()), so the static needs no guard.
  static uint64_t last_ms = 0;
  const uint64_t now = time_ms();
  if (!force && now - last_ms < 2000) return;
  last_ms = now;

  host::discord::Presence p;
  // The join secret is this player's own Slippi connect code and never anything else. A presence is
  // public, and a connect code carries no network location; an IP address must never end up here.
  const std::string my_code = g_user && g_user->IsLoggedIn() ? g_user->GetUserInfo().connect_code : std::string();
  const bool searching = g_matchmaking && g_matchmaking->IsSearching();
  const bool in_match = g_in_online_match && !is_disconnected();

  if (in_match) {
    p.details = mode_name(g_last_search.mode);
    std::string opponent;
    if (g_matchmaking)
      for (int i = 0; i < 4 && opponent.empty(); ++i)
        if (i != (int)g_local_player_index) opponent = g_matchmaking->GetPlayerName((uint8_t)i);
    p.state = opponent.empty() ? "In a match" : "vs " + opponent;
    p.party_size = 2; p.party_max = 2;
    // Keeps the Discord party id stable across the whole session. The party is full, so this only
    // names the party: no join secret is published while a match is running.
    p.join_code = my_code;
  } else if (searching) {
    const bool by_code = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
    p.details = mode_name(g_last_search.mode);
    p.state = by_code ? "Waiting for a friend" : "Searching";
    p.party_size = 1; p.party_max = 2;
    if (by_code) p.join_code = my_code;   // a friend can join a code search, not a matchmaking queue
  } else if (const char* offline = offline_mode_name()) {
    p.details = offline;
    p.state = my_code;
    // Still joinable: someone in training is exactly the person a friend wants to pull into a game.
    if (!my_code.empty()) { p.party_size = 1; p.party_max = 2; p.join_code = my_code; }
  } else {
    p.details = "In the menus";
    p.state = my_code;
    if (!my_code.empty()) { p.party_size = 1; p.party_max = 2; p.join_code = my_code; }
  }
  host::discord::publish(p);
}

void consume_discord_join() {
  if (!host::discord::enabled()) return;
  const std::string code = host::discord::take_join_code();
  if (code.empty() || !g_direct_codes) return;
  // Queue it for the Direct screen and remember it for the next Direct matchmaking command. The
  // latter makes the invite usable even if the player accepts the suggestion without typing it.
  g_direct_codes->AddOrUpdateCode(code);
  g_discord_join_code = code;
  g_discord_join_expires_ms = time_ms() + 5 * 60 * 1000;
  host::log("slippi: Discord invite from %s; it is now the first suggestion under Online > Direct",
            host::public_build::log_id(code).c_str());   // another player's code: a hash in a public build
}

bool discord_join_pending() {
  if (!g_discord_join_code.empty() && time_ms() >= g_discord_join_expires_ms) {
    g_discord_join_code.clear();
  }
  return !g_discord_join_code.empty();
}

// ---------------------------------------------------------------- custom fighters online
// docs/online-custom-content.md, host/online_fighters.h. The select screen's line after this player's
// own pick was unlocked (an added fighter not every player has), kept until a fighter is locked in again.
static std::string g_custom_refusal;
static void custom_fighters_session_ended() {
  g_custom_refusal.clear();
  host::online_fighters::reset();
}

// Teams and Direct may start a search with an added fighter this screen offered; whether every player
// has it is decided once they are connected.
static bool custom_fighter_offered(uint8_t id) {
  return id >= 26 && id < 26 + 32 && host::online_fighters::is_custom(id) &&
         (host::online_fighters::offered_online() >> (id - 26) & 1u) != 0;
}

// Every frame the online select polls, once connected. Tells online_fighters who is connected, sends
// this side's list of added fighters to each player that has not had it, sends a held back pick once
// every player has that fighter, holds "opponent ready" while a pick waits on a list, and unlocks this
// player's pick of an added fighter not every player has (the player picks again).
static void custom_fighters_gate(uint8_t remote_count, const MatchInfo& mi, uint8_t& local_ready, uint8_t& remote_ready) {
  namespace of = host::online_fighters;
  std::vector<of::Peer> peers;
  for (int i = 0; i < remote_count && i < REMOTE_PLAYER_MAX; ++i) {
    const auto rb = g_netplay->GetRemoteBuild(i);
    of::Peer peer;
    peer.port = mi.remote[i].player_idx;
    peer.build_known = rb.received;
    peer.takes = rb.received && rb.custom_fighters;
    peers.push_back(peer);
  }
  const uint64_t since = g_netplay->ConnectedAtMs();
  of::update_session(peers, since ? time_ms() - since : 0);
  g_netplay->SendFighterListIfDue();
  const uint8_t id = g_local_selections.character_id;
  if (g_netplay->CustomPickMasked() && g_local_selections.is_character_selected && of::is_custom(id) &&
      g_netplay->EveryRemoteTakesCustomFighters() && of::verdict(id, nullptr) == of::kSelectable) {
    host::log("custom fighters: every player has id %u: the held back pick goes out", id);
    g_netplay->ResendSelections();
  }
  std::string line;
  bool spinner = false;
  // Another player's lock-in of an added fighter names that machine's id: it maps to this machine's
  // fighter with the same content hash through that player's list. While the list is on its way the
  // match waits; a lock-in it does not map is refused below as a foreign character.
  for (int i = 0; i < remote_count && i < REMOTE_PLAYER_MAX; ++i) {
    const PlayerSelections& r = mi.remote[i];
    uint8_t local = 0;
    if (!r.is_character_selected || r.character_id < 26 || !of::roster_usable() ||
        of::map_remote(r.player_idx, r.character_id, &local, nullptr))
      continue;
    if (peers[i].takes && !of::peer_list_known(r.player_idx)) remote_ready = 0;
  }
  // This player's own pick of an added fighter: every player has it, or it is unlocked.
  if (g_local_selections.is_character_selected && of::is_custom(id)) {
    g_custom_refusal.clear();
    const bool by_code = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
    std::string why;
    const of::Verdict v = by_code ? of::verdict(id, &why) : of::kRefused;
    if (!by_code) why = "Added fighters play in Direct and Teams only.";
    if (v == of::kRefused) {
      host::log("custom fighters: this player's pick (id %u) is unlocked: %s", id, why.c_str());
      g_netplay->UnlockLocalCharacter();
      g_local_selections.is_character_selected = false;
      local_ready = 0;
      g_custom_refusal = why;
      // Tests only: the harness has no player to pick again, so MELEE_TEST_REPICK=<id> does.
      const char* repick = Matchmaking::local_peer.enabled ? host::test_env::getenv("MELEE_TEST_REPICK") : nullptr;
      if (repick && *repick) {
        PlayerSelections s = g_local_selections;
        s.character_id = (uint8_t)std::atoi(repick);
        s.is_character_selected = true;
        g_local_selections.Merge(s);
        host::log("custom fighters: test pick again: id %u", s.character_id);
        g_netplay->SetMatchSelections(g_local_selections);
        local_ready = 1;
      }
    } else if (v == of::kPending) {
      remote_ready = 0;
      line = why;
      spinner = true;
    }
  }
  if (!g_local_selections.is_character_selected && !g_custom_refusal.empty()) { line = g_custom_refusal; spinner = false; }
  of::set_line(line, spinner);
}

enum class Leave { Lost, Quit, Next };
void cleanup_connection(Leave why = Leave::Lost) {
  finish_summary(why == Leave::Quit ? "quit" : why == Leave::Next ? "completed" : "disconnected");
  host::log("slippi: connection cleanup");
  if (g_user) g_user->ResetOnlineCompatibility();
  host::cosmetics::thaw_after_online_session();
  stop_early_inputs();
  if (g_matchmaking || g_netplay) {
    std::lock_guard<std::mutex> lk(g_early_send_mutex);
    std::thread([mm = std::move(g_matchmaking), nc = std::move(g_netplay)]() mutable { mm.reset(); nc.reset(); }).detach();
  }
  g_matchmaking = std::make_unique<Matchmaking>(g_user.get());
  g_netplay = nullptr;
  g_local_selections.Reset();
  g_stage_pool.clear();
  g_forced_error.clear();
  g_overwrite_selections.clear();
  g_setup_hash = {};
  g_play_session_active = false;
  g_in_online_match = false;
  custom_fighters_session_ended();
  gecko_session::disconnected();
  trace_end_match();
  host::set_emulation_speed(1.0);
  update_discord_presence(true);   // back to "In the menus" straight away, not two seconds later
}

// ---------------------------------------------------------------- per-frame online flow
// The remote player the current wait is for, and how far along they were when it began.
static int g_input_wait_player = 0;
bool should_skip_online_frame(int32_t frame, int32_t finalized_frame) {
  auto st = g_netplay->GetSlippiConnectStatus();
  if (st == NetplayClient::ConnectStatus::FAILED || st == NetplayClient::ConnectStatus::DISCONNECTED) return false;
  bool any_needs_inputs = false;
  int behind = -1;
  uint8_t remote_count = bounded_remote_port_count();
  for (uint8_t i = 0; i < remote_count; ++i) {
    auto pad = g_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
    if (pad->is_disconnected && pad->despawn_frame != 0) { g_stall_frame_counts[i] = 0; continue; }
    int32_t latest = pad->latest_frame;
    bool enough = latest - finalized_frame >= (frame - finalized_frame - ROLLBACK_MAX_FRAMES);
    // Start in step. The Source Port loads a match with no emulated disc time, so it reached frame 1
    // well before a Slippi Dolphin opponent, ran into the countdown and froze at frame 8 (the rollback
    // limit) until they caught up, then time sync halted it again at frame 30: in one player's log 47
    // of 49 matches froze there, up to 1.25 s (GitHub #44). Holding frame 2 until the other side has
    // started puts that wait before anything moves, and the two games then start together. Frame 2,
    // not 1: both games have sent frame 1's inputs by then (frame 1 re-run would also redo the
    // match setup), so neither waits on the other forever.
    if (frame == 2 && latest < 1) enough = false;
    if (enough) { g_stall_frame_counts[i] = 0; continue; }
    g_stall_frame_counts[i]++;
    any_needs_inputs = true;
    if (behind < 0) behind = i;
    // 7 s without the other side's inputs means they are gone, except before their first frame (the frame-2
    // hold above): they may still be loading the stage, which took 13.3 s on a slow GPU in one match, and the
    // player waiting dropped them as "opponent left" before the match began. Until then they get 30 s.
    const int stall_limit = latest < 1 ? 60 * 30 : 60 * 7;
    if (g_stall_frame_counts[i] > stall_limit) {
      host::log("slippi: force-disconnecting player %u after %d s stall (frame %d, latest %d)", pad->player_idx, stall_limit / 60, frame, latest);
      g_netplay->ForceDisconnectPlayer(pad->player_idx);
      g_stall_frame_counts[i] = 0;
      continue;
    }
  }
  // A report of "it froze, then felt delayed" cannot be judged without knowing which side stopped.
  // The start and the end of every wait for the other player's inputs are logged for that.
  if (any_needs_inputs) {
    if (g_input_wait_frames++ == 0) {
      ++g_match_input_waits;
      // The player who is behind, not always the first one: with three others in a doubles match
      // this line used to name the first player's frame whoever the wait was for.
      g_input_wait_player = behind < 0 ? 0 : behind;
      auto pad = g_netplay->GetSlippiRemotePad(g_input_wait_player, ROLLBACK_MAX_FRAMES);
      g_input_wait_latest = pad->latest_frame;
      // That player's own ping: the first player's was printed here whoever the wait was for, so a doubles
      // match held up by one far player read as a short connection stalling (upstream 17eeaf83).
      host::log("slippi: waiting for the other player's inputs on frame %d (player %d of %u others, their latest frame %d, their ping %u ms)",
                frame, g_input_wait_player + 1, (unsigned)remote_count, pad->latest_frame,
                (unsigned)g_netplay->RemotePingMs(g_input_wait_player));
    }
    return true;
  }
  if (g_input_wait_frames > 0) {
    // Which side stopped: while this side waited N frames, a game that kept running sent N or more
    // new frames, and they arrive together once the connection lets them through. A game that had
    // itself stopped comes back with few.
    auto pad = g_netplay->GetSlippiRemotePad(g_input_wait_player < (int)remote_count ? g_input_wait_player : 0, ROLLBACK_MAX_FRAMES);
    const int gained = pad->latest_frame - g_input_wait_latest;
    if (gained >= g_input_wait_frames) ++g_summary.held_up;
    else ++g_summary.fell_behind;
    host::log("slippi: inputs arrived on frame %d after waiting %d frames (%.2f s); their latest frame went %d to %d: %s", frame,
              g_input_wait_frames, g_input_wait_frames / 60.0, g_input_wait_latest, pad->latest_frame,
              gained >= g_input_wait_frames ? "their game kept running, the inputs were held up on the way"
                                            : "their game fell behind too");
    // A wait the player can feel (6 frames, 0.1 s, or more) marks the trace by itself, so the match
    // is kept and Send recent game logs carries it even when nobody pressed a mark button.
    if (g_input_wait_frames >= 6) g_trace_record.flags |= net_trace::kMark;
    g_input_wait_frames = 0;
  }
  // Every 30 frames for the whole match: a strict threshold while the match starts, then only for a
  // gap the other side's own catch-up cannot cover (slippi_time_sync.h). Checking only the first 120
  // frames left an instance that drifted ahead there for the rest of the match, predicting and
  // rolling back.
  if (frame % ONLINE_LOCKSTEP_INTERVAL == 0 && !g_currently_skipping) {
    int32_t offset = g_netplay->CalcTimeOffsetUs();
    g_trace_offset_us = offset;
    if (const int halt = slippi::time_sync::HaltFrames(offset, frame)) {
      g_currently_skipping = true;
      g_frames_to_skip = halt;
      host::log("slippi: halting on frame %d for time sync (offset %d us, %d frames)", frame, offset, g_frames_to_skip);
    } else if (offset > slippi::time_sync::kSlackUs || offset < -slippi::time_sync::kSlackUs) {
      host::log("slippi: time offset %d us on frame %d (no frame shed)", offset, frame);
    }
  }
  if (g_frames_to_skip > 0) { --g_frames_to_skip; return true; }
  g_currently_skipping = false;
  return false;
}

bool opponent_runahead() {
  return g_matchmaking->AllRemotesAreBots();
}

bool should_advance_online_frame(int32_t frame) {
  if (opponent_runahead()) return false;
  if (frame % ONLINE_LOCKSTEP_INTERVAL == 0) {
    int32_t offset = g_netplay->CalcTimeOffsetUs();
    g_trace_offset_us = offset;
    float deviation = 0;
    // MELEE_SYNC_SMOOTH=1 (measured against the default before it becomes a setting): start the
    // speed correction at 2 ms instead of 8 ms ahead, up to the same 1% Slippi allows, so the two
    // games meet again before a frame has to be halted or skipped. Only this side's own speed
    // changes; the other player's game, Slippi Dolphin included, runs its own sync unchanged.
    static const bool smooth = [] { const char* v = host::test_env::getenv("MELEE_SYNC_SMOOTH"); return v && *v == '1'; }();
    if (smooth) {
      if (offset > -2000 && offset < 2000) deviation = 0;
      else if (offset < 0) deviation = std::min(-offset / (2 * 16683.0f), 1.0f) * 0.01f;
      else deviation = std::min(offset / (2 * 16683.0f), 1.0f) * -0.01f;
    } else if (offset > -250 && offset < 8000) deviation = 0;
    else if (offset < 0) deviation = std::min(-offset / (3 * 16683.0f), 1.0f) * 0.01f;
    else deviation = std::min(offset / (3 * 16683.0f), 1.0f) * -0.005f;
    // Slippi Dolphin runs the console's 59.94 Hz (16683 us a frame) and its time sync is built on
    // that; the host ticks 16667 us, which left this side drifting ahead until the opponent sped up.
    host::set_emulation_speed(16667.0 / 16683.0 * (1.0 + deviation));
    const int32_t frame_time = 16683, t1 = 10000, t2 = frame_time + t1;
    g_fall_behind += offset < -t1 ? 1 : 0;
    g_fall_far_behind += offset < -t2 ? 1 : 0;
    bool slow = (offset < -t1 && g_fall_behind > 50) || (offset < -t2 && g_fall_far_behind > 15);
    // The counts add up over the whole match, as in Slippi Dolphin (where this is a 10 s notice), so
    // once past the mark every later check repeated the line: once a match is enough.
    if (slow && !g_poor_performance_logged && g_matchmaking->RemotePlayerCount() == 1) {
      g_poor_performance_logged = true;
      host::log("slippi: possible poor match performance detected on frame %d (offset %d us)", frame, offset);
    }
    if (offset < -t2 && !g_currently_advancing) {
      g_currently_advancing = true;
      g_frames_to_advance = slippi::time_sync::AdvanceFrames(offset, frame);
      host::log("slippi: advancing on frame %d for time sync (offset %d us, %d frames)", frame, offset, g_frames_to_advance);
    }
  }
  if (g_frames_to_advance > 0) {
    if (frame % 5 != 0) return false;
    --g_frames_to_advance; ++g_match_advances;
    return true;
  }
  g_currently_advancing = false;
  return false;
}

// The input delay this match uses. Auto: enough frames to cover half the round trip plus 4 ms of
// jitter, never under Slippi's default 2 nor over 6; with no ping measured yet, the set value. The
// game asks for it twice while a match is set up (CMD_GET_DELAY, then the match start block), so the
// value is latched on the first ask and held until the match ends: both asks must agree.
static int g_latched_delay = 0;
static int effective_delay() {
  // MELEE_TEST_AUTO_DELAY=1 (hidden runs, which read no settings file) turns auto on.
  static const bool test_auto = [] { const char* v = host::test_env::getenv("MELEE_TEST_AUTO_DELAY"); return v && *v == '1'; }();
  if (!g_config.auto_delay && !test_auto) return g_config.delay;
  if (g_latched_delay > 0) return g_latched_delay;
  // The game polls the match state block (which carries this value) every frame of the search, long
  // before an opponent is connected, and takes the delay from it once, when the match starts
  // (mu_online.c). So nothing is latched before there is a ping, or before a connected opponent is
  // known never to send one (Slippi Dolphin and older builds send no pre-match ping): until then the
  // set delay is answered without being held. In-match pings come from pad acks; at match setup
  // only the pre-match ping (0xE2) exists.
  const int match_ping = g_netplay ? (int)g_netplay->LastPingMs() : 0;
  const int ping = match_ping > 0 ? match_ping : g_netplay ? g_netplay->PreMatchPingMs() : 0;
  if (ping <= 0) {
    if (!g_netplay || g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED) return g_config.delay;
    const auto build = g_netplay->GetRemoteBuild(0);
    const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t connected_at = g_netplay->ConnectedAtMs();
    const bool waited = connected_at > 0 && now_ms > connected_at + 2000;
    if (!(build.received && !build.pings) && !waited) return g_config.delay;   // the ping is on its way
    g_latched_delay = g_config.delay;
    host::log("slippi: auto delay: no ping from this opponent, using the set delay %d", g_latched_delay);
    return g_latched_delay;
  }
  g_latched_delay = std::clamp((int)std::ceil((ping / 2.0 + 4.0) / 16.683), 2, 6);
  host::log("slippi: auto delay: %s ping %d ms, %d frames", match_ping > 0 ? "match" : "pre-match", ping, g_latched_delay);
  return g_latched_delay;
}

void prepare_opponent_inputs(int32_t frame, bool should_skip, std::vector<uint8_t>& q) {
  q.clear();
  uint8_t frame_result = 1;
  auto st = g_netplay->GetSlippiConnectStatus();
  if (should_skip) frame_result = 2;
  else if (st != NetplayClient::ConnectStatus::CONNECTED) frame_result = 3;
  else if (should_advance_online_frame(frame)) frame_result = 4;
  q.push_back(frame_result);
  uint8_t remote_count = bounded_remote_port_count();
  q.push_back(remote_count);
  g_netplay->FillDroppedPlayerInputs(frame);
  std::unique_ptr<RemotePadOutput> results[REMOTE_PLAYER_MAX];
  int32_t latest_from_opps = -123 - 1;
  uint32_t last_checksum_frame = 0, last_checksum = 0;
  for (int i = 0; i < remote_count; ++i) {
    results[i] = g_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
    if (results[i]->is_disconnected) continue;
    int32_t cf = results[i]->checksum_frame;
    if (cf > g_last_checksum_frame[i] && results[i]->checksum) {
      auto it = g_local_checksums.find(cf);
      if (it != g_local_checksums.end()) {
        g_last_checksum_frame[i] = cf; ++g_checksums_compared;
        if (it->second != results[i]->checksum) { ++g_checksums_mismatched; g_trace_desync_pending = true; host::log("slippi: DESYNC: checksum mismatch at frame %d (ours %08X, player %u %08X)", cf, it->second, results[i]->player_idx, results[i]->checksum); }
        else if (g_checksums_compared % 20 == 0) host::log("slippi: checksums agree through frame %d (%u compared, %u mismatched)", cf, g_checksums_compared, g_checksums_mismatched);
      }
    }
    if (results[i]->latest_frame > latest_from_opps) {
      last_checksum_frame = (uint32_t)results[i]->checksum_frame;
      last_checksum = results[i]->checksum;
      latest_from_opps = results[i]->latest_frame;
    }
  }
  // Slippi 3.7.0's player drop protocol (despawn_frame, FillDroppedPlayerInputs) replaces our gone-port
  // remap; the host retains which ports left.
  for (int i = 0; i < remote_count; ++i)
    if (results[i]->is_disconnected && results[i]->player_idx < PLAYER_COUNT_MAX) g_gone_ports |= 1u << results[i]->player_idx;
  for (int i = 0; i < remote_count; ++i) {
    if (!results[i]->is_disconnected) { append_u32(q, (uint32_t)results[i]->checksum_frame); append_u32(q, results[i]->checksum); continue; }
    // A dropped player's checksums stop: the game gets someone else's current one to compare. Their
    // frames need no such trick, FillDroppedPlayerInputs keeps them current.
    append_u32(q, last_checksum_frame); append_u32(q, last_checksum);
  }
  for (int i = remote_count; i < REMOTE_PLAYER_MAX; ++i) { append_u32(q, 0); append_u32(q, 0); }
  int offset[REMOTE_PLAYER_MAX] = {};
  int32_t latest_read[REMOTE_PLAYER_MAX] = {};
  for (int i = 0; i < remote_count; ++i) {
    offset[i] = std::max(0, (results[i]->latest_frame - frame) * PAD_FULL_SIZE);
    int32_t latest = std::min(results[i]->latest_frame, frame);
    latest_read[i] = latest;
    append_u32(q, (uint32_t)latest);
  }
  for (int i = remote_count; i < REMOTE_PLAYER_MAX; ++i) { latest_read[i] = frame; append_u32(q, (uint32_t)frame); }
  append_u32(q, (uint32_t)*std::min_element(std::begin(latest_read), std::end(latest_read)));
  uint8_t latest_inputs[REMOTE_PLAYER_MAX][PAD_FULL_SIZE] = {};
  for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) {
    std::vector<uint8_t> tx;
    if (i < remote_count && offset[i] < (int)results[i]->data.size()) tx.assign(results[i]->data.begin() + offset[i], results[i]->data.end());
    tx.resize(PAD_FULL_SIZE * ROLLBACK_MAX_FRAMES, 0);
    std::copy(tx.begin(), tx.begin() + PAD_FULL_SIZE, latest_inputs[i]);   // what the game loads when it has no input for the frame
    q.insert(q.end(), tx.begin(), tx.end());
  }
  // Slippi 3.7.0. The frame each remote player's fighter despawns on, 0 if it does not: the game
  // compares it with the frame it is processing, so it lands on the same frame for everyone, also in
  // a rollback.
  for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) append_u32(q, i < remote_count ? (uint32_t)results[i]->despawn_frame : 0);
  // The input to predict with for each of the frames after each player's latest: the latest input,
  // or a newer input suggestion from that player. The game checks every prediction against the real
  // input and rolls back if they differ.
  for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) {
    uint8_t predictions[suggestions::kPredictedFrames][suggestions::kReportBytes];
    for (auto& p : predictions) std::memcpy(p, latest_inputs[i], PAD_FULL_SIZE);
    if (i < remote_count) suggestions::fill_predictions((size_t)i, latest_read[i], results[i]->is_disconnected, latest_inputs[i], predictions);
    q.insert(q.end(), &predictions[0][0], &predictions[0][0] + sizeof predictions);
  }
  // Random bits the game sends with its next pad and mixes into the RNG seed. Not from the match's
  // own generator: the opponent sees them and could work out that generator's state.
  { static std::random_device pad_rng_bits; q.push_back((uint8_t)(pad_rng_bits() & 0xFF)); }
  // Early inputs, Source Port only (its online code is native and reads past Slippi's reply; the
  // Static Recomp's game code reads the fixed reply and must get exactly that): per remote, a count
  // and then entries of target pad frame + 12-byte pad, for frames after that remote's latest
  // confirmed one. The game predicts a not-yet-confirmed frame from the newest entry up to it.
  if (host::game_image && early_inputs_accept()) {
    std::vector<host::early_input::Entry> entries;
    for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) {
      entries.clear();
      if (i < remote_count && !results[i]->is_disconnected) g_netplay->EarlyPads(i, latest_read[i], entries);
      if (entries.size() > (size_t)host::early_input::kMaxEntries) entries.resize(host::early_input::kMaxEntries);
      q.push_back((uint8_t)entries.size());
      for (const auto& e : entries) {
        append_u32(q, (uint32_t)e.target);
        q.insert(q.end(), e.pad, e.pad + host::early_input::kPadBytes);
        for (int k = host::early_input::kPadBytes; k < PAD_FULL_SIZE; ++k) q.push_back(0);
      }
    }
  }
}

// Diagnostic, off unless MELEE_TRACE_ONLINE_FRAMES=1, Legacy only (it reads console addresses):
// each online command's frame beside the engine's frame counters (gm_80479D58: +0 bodies run,
// +8 unpaused bodies) and the pad queue (HSD_PadLibData: qread, qwrite, qcount). This is how the
// native engine's online frame numbering is matched to the console game's.
bool trace_online_frames() {
  static const bool on = [] {
    const char* v = host::test_env::getenv("MELEE_TRACE_ONLINE_FRAMES");
    return v && v[0] == '1';
  }();
  return on && !host::game_image;
}
void trace_online_frame(const char* what, int32_t frame) {
  if (!trace_online_frames()) return;
  const uint32_t pad = 0x804C1F78u;
  host::log("online trace: %s frame %d engine unk0 %d unk8 %d pad qread %u qwrite %u qcount %u retrace %u",
            what, frame, (int32_t)host::rd32(0x80479D58u), (int32_t)host::rd32(0x80479D60u),
            host::rd8(pad + 1), host::rd8(pad + 2), host::rd8(pad + 3), host::retrace_count());
}

void handle_online_inputs(const uint8_t* payload, uint32_t size, std::vector<uint8_t>& q) {
  if (size != 27) { q.assign(1, 3); return; }
  q.clear();
  int32_t frame = (int32_t)be32(payload), finalized = (int32_t)be32(payload + 4);
  trace_online_frame("inputs", frame);
  uint32_t finalized_checksum = be32(payload + 8);
  uint8_t delay = payload[12];
  const uint8_t* inputs = payload + 13;
  const bool is_leaving = payload[26] != 0;   // Slippi 3.7.0; payload[25] is the port the local inputs come from
  if (frame == 1) {
    g_quality = {};
    if (!g_summary.active && !is_disconnected()) {
      g_summary.begin((int)g_last_search.mode);
    }
    g_available_savestates.clear();
    g_active_savestates.clear();
    // Console-RAM savestates are the static recomp's; the native game keeps its own snapshots, and
    // its memory holds no console heap bounds to size these from.
    if (!host::game_image)
      for (int i = 0; i < ROLLBACK_MAX_FRAMES; ++i) g_available_savestates.push_back(std::make_unique<Savestate>());
    for (auto& c : g_stall_frame_counts) c = 0;
    g_frames_to_skip = 0; g_currently_skipping = false;
    g_frames_to_advance = 0; g_currently_advancing = false; g_fall_behind = 0; g_fall_far_behind = 0;
    g_poor_performance_logged = false;
    g_local_selections.Reset();
    g_match_rollbacks_start = g_rollbacks; g_match_stalls = 0; g_match_delay = delay;
    g_match_input_waits = 0; g_match_advances = 0; g_input_wait_frames = 0;
    // The game resends frame 1 while it waits for the opponent: the trace starts on the first one.
    if (!g_trace_open || g_trace_record.frame != 1) {
      g_trace_open = false; g_trace_offset_us = 0;
      net_trace::take_presents();
      net_trace::begin_match();
      // The session trace file sits beside the match's replay, under its name. The Static Recomp's
      // replay is open and named by now; the Source Port names its replay when it writes it and
      // reports it then (net_trace::replay_saved). No replay, no trace.
      host::wait_file_writes();   // the last match's replay names its trace: it is on disk first
      if (host::game_image) net_trace::file_begin(host::options.replay_dir.c_str(), nullptr);
      else if (slippi::recording()) net_trace::file_begin(slippi::replay_directory().c_str(), slippi::last_replay_path().c_str());
    }
    if (g_netplay) { g_netplay->GetAndResetAvgPingMs(); g_netplay->StartSlippiGame(); }
    host::log("slippi: online game starts, delay %d frames, direct peer to peer", (int)delay);
    g_latched_delay = (int)delay;   // the delay the game took: every later answer this match agrees with it
    g_in_online_match = true;
    gecko_session::match_started();   // the agreed Gecko codes, decided from both locks
    if (g_user) g_user->ResetOnlineCompatibility();  // the next game needs its own fresh check
    host::input_mark_match_start();
    // Early inputs: watch the controller for the match. Sent only to peers whose build takes them.
    host::early_input::reset_stats();
    // Slippi input suggestions: on with the same setting, and off whenever a bot is in the game, on
    // either side (a bot is expected to make the connection feel good on its own).
    bool bot_in_game = false;
    if (g_matchmaking) for (const auto& player : g_matchmaking->GetPlayerInfo()) bot_in_game |= player.is_bot;
    const bool suggest = early_inputs_enabled() && !bot_in_game;
    suggestions::start_game(suggest, bounded_remote_port_count());
    if (early_inputs_enabled()) host::early_input::start(send_early_pad, suggest ? send_suggestion : nullptr);
    g_local_checksums.clear(); g_checksums_compared = 0; g_checksums_mismatched = 0; g_trace_desync_pending = false; std::fill(std::begin(g_last_checksum_frame), std::end(g_last_checksum_frame), 0);
    g_gone_ports = 0;
    g_gone_own_frames = -1;
    std::fill(std::begin(g_gone_last_frame), std::end(g_gone_last_frame), slippi::kNoFrame);
  }
  if (frame > 0 && (uint32_t)frame > g_summary.frames) {
    g_summary.frames = (uint32_t)frame;
    if (g_netplay) g_summary.ping.add((uint64_t)std::max(0, g_netplay->LastPingMs()));
  }
  // The local player chose to leave (party mode): that ends the game for this side as losing the
  // connection does, and the other players take it for a disconnect too.
  if (is_leaving && !is_disconnected() && g_netplay) g_netplay->ForceDisconnect();
  if (is_disconnected()) {
    if (g_netplay && g_netplay->GetDisconnectReason() == NetplayClient::DisconnectReason::POOR_PERFORMANCE)
      g_forced_error = "Ranked match ended due to poor network quality.";
    finish_summary("disconnected");
    q.push_back(3);
    return;
  }
  if (frame % 30 == 0) host::log("slippi: online frame %d wall %.3f s retrace %u rollbacks %llu", frame, host::now_seconds(), host::retrace_count(), (unsigned long long)g_rollbacks);
  g_game_facts.frames = frame;
  if (finalized > 0 && finalized_checksum) { g_local_checksums[finalized] = finalized_checksum; while (g_local_checksums.size() > 600) g_local_checksums.erase(g_local_checksums.begin()); }
  g_netplay->DropOldRemoteInputs(finalized);
  bool skip = should_skip_online_frame(frame, finalized);
  if (g_last_search.mode == Matchmaking::RANKED && official_matchmaking()) {
    const auto pad = g_netplay->GetSlippiRemotePad(0, ROLLBACK_MAX_FRAMES);
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_quality.should_terminate(frame, pad->latest_frame, delay, skip && g_input_wait_frames > 0, now_us)) {
      const auto me = g_user->GetUserInfo();
      report::match_status(me.uid, me.play_key, g_recent_mm_result.id, "poor_performance", true);
      g_netplay->ForceDisconnect(NetplayClient::DisconnectReason::POOR_PERFORMANCE);
    }
  }
  if (skip) { ++g_match_stalls; g_netplay->SendSlippiPad(nullptr); host::early_input::note_skipped(); }
  else {
    if (frame == 1) for (int i = 1; i <= delay; ++i) g_netplay->SendSlippiPad(std::make_unique<Pad>(i));
    g_netplay->SendSlippiPad(std::make_unique<Pad>(frame + delay, finalized, finalized_checksum, inputs));
    host::early_input::note_sent_pad(frame + delay, inputs);
    host::input_note_pad_frame(frame);
  }
  prepare_opponent_inputs(frame, skip, q);
  host::early_input::note_frame_done(skip, !q.empty() && q[0] == 4, inputs);
  // Session trace: the tick before is complete now; open this one.
  trace_close_tick();
  g_trace_record = {};
  g_trace_record.wall = host::now_seconds();
  g_trace_record.frame = frame;
  g_trace_record.offset_us = g_trace_offset_us;
  g_trace_record.wait_frames = (uint16_t)std::min(g_input_wait_frames, 0xFFFF);
  g_trace_record.ping_ms = (uint16_t)std::clamp(g_netplay->LastPingMs(), 0, 0xFFFF);
  if (skip) g_trace_record.flags |= g_input_wait_frames > 0 ? net_trace::kWait : net_trace::kShed;
  else if (!q.empty() && q[0] == 4) g_trace_record.flags |= net_trace::kAdvance;
  if (g_trace_desync_pending) { g_trace_record.flags |= net_trace::kDesync; g_trace_desync_pending = false; }
  // F8: the player marks "that just felt wrong", so a report can point at the moment.
  static const char* const kMarkNames[] = {"", "", "looked wrong", "input wrong", "sounded wrong"};
  static const uint8_t kMarkFlags[] = {0, net_trace::kMark, net_trace::kMarkVisual, net_trace::kMarkInput, net_trace::kMarkAudio};
  auto mark = [&](int kind) {
    g_trace_record.flags |= kMarkFlags[kind];
    if (kind == 1) host::log("slippi: marked frame %d in the session trace", frame);
    else host::log("slippi: marked frame %d in the session trace (%s)", frame, kMarkNames[kind]);
  };
  if (const int kind = host::window_take_trace_mark()) mark(kind);
  // The same from the controller, and saying which kind: D-pad Left "that looked wrong", D-pad
  // Right "my input came out wrong or late", D-pad Down "that sounded wrong". None of the three does anything in a match, and the
  // press is read from the pad the game is given, so it works on every controller and binding.
  {
    static uint8_t held = 0;
    const uint8_t dpad = inputs[1] & 0x07;   // PAD_BUTTON_LEFT 1, PAD_BUTTON_RIGHT 2, PAD_BUTTON_DOWN 4
    const uint8_t pressed = (uint8_t)(dpad & ~held);
    held = dpad;
    if (pressed & 1) mark(2);
    if (pressed & 2) mark(3);
    if (pressed & 4) mark(4);
  }
  std::memcpy(g_trace_record.pad, inputs, PAD_DATA_SIZE);
  g_trace_open = true;
}

void handle_capture_savestate(const uint8_t* payload) {
  if (is_disconnected()) return;
  int32_t frame = (int32_t)be32(payload);
  trace_online_frame("capture", frame);
  std::unique_ptr<Savestate> ss;
  if (!g_available_savestates.empty()) { ss = std::move(g_available_savestates.back()); g_available_savestates.pop_back(); }
  else if (!g_active_savestates.empty()) { auto it = g_active_savestates.begin(); ss = std::move(it->second); g_active_savestates.erase(it); }
  else {
    // Both pools are empty: no online game has started them, or this is the native game, which keeps
    // its own snapshots and creates none here. There is nothing to capture into.
    static bool logged = false;
    if (!logged) { logged = true; host::log("slippi: savestate capture for frame %d ignored: no savestate pool", frame); }
    return;
  }
  if (g_active_savestates.count(frame)) { g_available_savestates.push_back(std::move(g_active_savestates[frame])); g_active_savestates.erase(frame); }
  ss->Capture();
  g_active_savestates[frame] = std::move(ss);
}

void handle_load_savestate(const uint8_t* payload) {
  int32_t frame = (int32_t)be32(payload);
  trace_online_frame("load", frame);
  if (!g_active_savestates.count(frame)) { host::log("slippi: savestate for frame %d does not exist", frame); return; }
  std::vector<PreserveBlock> blocks;
  for (int i = 4; i + 8 <= 32 && be32(payload + i) != 0; i += 8)
    blocks.push_back({be32(payload + i), be32(payload + i + 4)});
  g_active_savestates[frame]->Load(blocks);
  ++g_rollbacks;
  trace_rollback(frame);
  // The next frame the game finishes continues from this older state, not from the frame on screen.
  // Blending the two (sub-frame animation) would draw positions that never existed on either
  // timeline, which on continuously animated stages showed as a glitch that only ever happened
  // online. The renderer holds exact frames across it instead. Training Mode's savestates use this
  // same load outside an online match; those are hard cuts.
  gx::mark_discontinuity(is_online_match());
  for (auto& kv : g_active_savestates) g_available_savestates.push_back(std::move(kv.second));
  g_active_savestates.clear();
}

// ---------------------------------------------------------------- lobby
void start_find_match(const uint8_t* payload) {
  Matchmaking::MatchSearchSettings search;
  search.mode = (Matchmaking::OnlinePlayMode)payload[0];
  if (const char* reason = local_matchmaking_error((int)search.mode)) {
    g_forced_error = reason;
    return;
  }
  std::string sj((const char*)payload + 1, 18);
  sj.erase(std::find(sj.begin(), sj.end(), '\0'), sj.end());
  if (search.mode == Matchmaking::DIRECT && discord_join_pending()) {
    // Choosing Direct after accepting a Discord Join means join that sender. Use the queued code
    // even if the game's text field still contains its old value; the user need not retype/paste it.
    const std::string invite_sj = utf8_to_shiftjis(g_discord_join_code);
    sj.assign(invite_sj.begin(), invite_sj.begin() + std::min<size_t>(invite_sj.size(), 18));
    g_discord_join_code.clear();
  }
    if (search.mode == Matchmaking::DIRECT) g_direct_codes->AddOrUpdateCode(shiftjis_to_utf8(sj));
    else if (search.mode == Matchmaking::TEAMS) g_teams_codes->AddOrUpdateCode(shiftjis_to_utf8(sj));
  search.connect_code = sj;
  g_last_search = search;
  // A new search: whoever answers may be on this build with the same mod, so the mod shows again.
  if (g_plain_opponent_handler) g_plain_opponent_handler(false, nullptr);
  if (Matchmaking::IsFixedRulesMode(search.mode)) {
    if (g_local_selections.character_id >= 26) { g_forced_error = "The character you selected is not allowed in this mode"; return; }
    const auto& stages = allowed_stages(search.mode);
    if (g_local_selections.is_stage_selected && std::find(stages.begin(), stages.end(), g_local_selections.stage_id) == stages.end()) {
      g_forced_error = "The stage being requested is not allowed in this mode"; return;
    }
  } else if (search.mode == Matchmaking::TEAMS && g_local_selections.character_id >= 26 &&
             !(g_local_build.mod_view && g_local_build.extended_content) && !custom_fighter_offered(g_local_selections.character_id)) {
    g_forced_error = "The character you selected is not allowed in this mode"; return;
  }
  if (!enet_ready()) { g_forced_error = "Networking unavailable"; return; }
  host::cosmetics::freeze_for_online_session();
  // A client that already searched (it found a match, failed or ended) still holds its search
  // thread: starting another on it ends the process (std::thread assigned while joinable). The
  // character select cleans up before every search, but other paths can reach here without that
  // (a harness or launcher match), so replace it here too.
  if (g_matchmaking && g_matchmaking->GetMatchmakeState() != Matchmaking::IDLE) {
    host::log("slippi: a new search on a used client (state %d): replaced first", (int)g_matchmaking->GetMatchmakeState());
    std::lock_guard<std::mutex> lk(g_early_send_mutex);
    std::thread([mm = std::move(g_matchmaking)]() mutable { mm.reset(); }).detach();
    g_matchmaking = std::make_unique<Matchmaking>(g_user.get());
  }
  g_matchmaking->FindMatch(search);
  update_discord_presence(true);
}

bool tag_matches_input(const uint8_t* input, uint8_t len, const std::string& tag) {
  std::string jis = history_code_for_name_entry(tag);
  for (int i = 0; i < len; ++i) {
    uint8_t a = i * 2 < (int)jis.size() ? (uint8_t)jis[i * 2] : 0, b = i * 2 + 1 < (int)jis.size() ? (uint8_t)jis[i * 2 + 1] : 0;
    if (input[i * 3] != a || input[i * 3 + 1] != b) return false;
  }
  return true;
}

void handle_name_entry_load(const uint8_t* payload, std::vector<uint8_t>& q) {
  // The field holds 8 characters (24 bytes of payload); a longer length from the game is cut to that.
  uint8_t len = std::min<uint8_t>(payload[24], 8);
  uint32_t initial = be32(payload + 25);
  uint8_t scroll = payload[29], mode = payload[30];
  DirectCodes* history = mode == Matchmaking::TEAMS ? g_teams_codes.get() : g_direct_codes.get();
  uint32_t cur = initial;
  if (scroll == 1) ++cur;
  else if (scroll == 2) cur = cur > 0 ? cur - 1 : cur;
  else if (scroll == 3) cur = 0;
  std::string tag = "1";
  if (mode == Matchmaking::DIRECT && discord_join_pending()) {
    // Return the invite as the active completion even with an empty field, so the code appears
    // immediately when Online > Direct opens. start_find_match also consumes it if the field was
    // not explicitly accepted by the user.
    tag = history->get(0);
    cur = 0;
  } else {
    while (cur < (uint32_t)history->length()) {
      tag = history->get((int)cur);
      if (tag_matches_input(payload, len, tag)) break;
      cur = scroll == 2 ? cur - 1 : cur + 1;
    }
    tag = history->get((int)cur);
  }
  if (tag == "1") {
    std::string init_tag = history->get((int)initial);
    if (tag_matches_input(payload, len, init_tag)) { tag = init_tag; cur = initial; }
  }
  q.clear();
  if (tag == "1") {
    q.push_back(0);
    q.insert(q.end(), payload, payload + 3 * len);
    q.insert(q.end(), 3 * (8 - len), 0);
    q.push_back(len);
    append_u32(q, initial);
    return;
  }
  q.push_back(1);
  std::string jis = history_code_for_name_entry(tag);
  for (int i = 0; i < 8; ++i) {
    for (int j = i * 2; j < i * 2 + 2; ++j) q.push_back(j < (int)jis.size() ? (uint8_t)jis[j] : 0);
    q.push_back(0);
  }
  q.push_back((uint8_t)(jis.size() / 2));
  append_u32(q, cur);
}


void set_match_selections(const uint8_t* payload) {
  PlayerSelections s;
  s.team_id = payload[0];
  s.character_id = payload[1];
  s.character_color = payload[2];
  s.is_character_selected = payload[3] != 0;
  s.stage_id = be16(payload + 4);
  uint8_t stage_option = payload[6];
  s.alt_stage_mode = payload[8];
  s.is_stage_selected = stage_option == 1 || stage_option == 3;
  if (stage_option == 3) s.stage_id = random_stage(payload[7]);
  s.rng_offset = g_rng() % 0xFFFF;
  const auto& local_peer = Matchmaking::local_peer;
  if (local_peer.enabled && local_peer.test_stage >= 0) {
    s.stage_id = static_cast<uint16_t>(local_peer.test_stage);
    s.is_stage_selected = true;
    host::log("slippi: local regression stage forced to %u", s.stage_id);
  }
  g_local_selections.Merge(s);
  if (s.is_stage_selected) g_local_random_stage = stage_option == 3 && !(local_peer.enabled && local_peer.test_stage >= 0);
  if (g_netplay) g_netplay->SetMatchSelections(g_local_selections);
}

void prepare_online_match_state(std::vector<uint8_t>& q);

// The same-build rule. 1: play, 0: wait for the opponent's build, -1: refuse (message in *why).
// Direct with a mod: every opponent must be on the same mod build (or, at the player's word, send
// no build: Slippi Dolphin with the same mod). Retail game: an opponent who says it is on a mod is
// refused, since their fighters and stages would not exist here. Other modes play the retail game on
// both sides; an opponent claiming a mod there is refused too.
int build_verdict(uint8_t remote_count, std::string* why) {
  if (!g_netplay) return 1;
  // Teams is played by code like Direct, so a mod build is checked on every remote player the same way.
  const bool direct = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
  bool waiting = false;

  for (int i = 0; i < remote_count; ++i) {
    const auto rb = g_netplay->GetRemoteBuild(i);
    if (!rb.received) {
      if (g_local_build.mod_view && direct && !g_local_build.allow_unverified) waiting = true;
      continue;
    }
    if (!direct) {
      if (rb.mod_view) { *why = "Your opponent is on a mod build; this mode plays the retail game"; return -1; }
      continue;
    }
    if (g_local_build.mod_view) {
      if (!native_direct_builds_match(true, g_local_build.fingerprint, rb.mod_view, rb.fingerprint)) {
        const int port = i < g_netplay->LocalPlayerPort() ? i + 1 : i + 2;
        // An opponent on the standard game: say it is this player's own mod disc that differs, and
        // where it is switched off. A copy of Melee with one changed file left in Mods\Discs read as
        // "a different mod build" to a player whose ISOs were the same (0.9.203 report).
        // Short enough for the character select's four-line error box (the longer wording was cut off).
        if (!rb.mod_view)
          *why = "Your mods are on" + (g_local_build.name.empty() ? std::string("") : ": " + g_local_build.name) +
                 ". Player " + std::to_string(port) + " has none. Turn yours off in Mods, or use the same";
        else
          *why = "Player " + std::to_string(port) + " has different mods. Everyone needs the same ones";
        return -1;
      }
    } else if (rb.mod_view) {
      *why = "Opponent has mods on" + (rb.name.empty() ? std::string("") : ": " + rb.name) +
             ". Turn them off in Mods, or use the same";
      return -1;
    }
  }
  if (!waiting) return 1;
  const uint64_t since = g_netplay->ConnectedAtMs();
  const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (since == 0 || now - since < kBuildWaitMs) return 0;
  // No build arrived in the wait: the opponent is on Slippi Dolphin (or a version from before the
  // build message), which cannot say what it runs. This used to be refused unless the player had
  // found and ticked a switch for it, every session; players read the switch as the opponent's and
  // could not play at all. Then it started with the mod's files on this side only, and a report
  // desynced on the first thing the mod's files do differently (a Stadium transformation, six games
  // in). Slippi Dolphin plays the retail game unless the player says it has the same mod, so the
  // match starts on the retail game: the host switches this side's files before any are loaded for it.
  static uint64_t logged_for = 0;
  if (logged_for != since) {
    logged_for = since;
    host::log("slippi: the opponent sent no build (Slippi Dolphin); this Direct match plays the retail game without %s",
              g_local_build.name.c_str());
    if (g_plain_opponent_handler) g_plain_opponent_handler(true, "Opponent is on Slippi Dolphin: this match plays the standard game");
  }
  return 1;
}

// Squad Strike in Direct (mu_squad_wire.h version 4): the setup the last match both sides were ready
// for was built with, empty for a normal match. The game reads it at the match's setup
// (MU_SQUAD_OP_MATCH_SETUP, port/app/source_host.cpp); the setup hash covers it.
static std::mutex g_squad_match_mutex;
static std::vector<uint8_t> g_squad_match;

#if MU_SQUAD
// The exchanged lineups when both players agreed on Squad Strike (NetplayClient::SquadLineups), for the
// inputs hash: {1, count P1, pairs, count P2, pairs}; empty when squads are off.
static std::vector<uint8_t> squad_exchanged_lineups() {
  uint8_t counts[2] = {}, lineups[2][8] = {};
  if (g_last_search.mode != Matchmaking::DIRECT || !g_netplay || !g_netplay->SquadLineups(counts, lineups)) return {};
  std::vector<uint8_t> bytes{1};
  for (int p = 0; p < 2; ++p) {
    bytes.push_back(counts[p]);
    bytes.insert(bytes.end(), lineups[p], lineups[p] + 2 * (counts[p] <= 4 ? counts[p] : 0));
  }
  return bytes;
}

// This match's squad setup bytes from the finished block: Direct 1v1 only. 1 squads (`out` filled),
// 0 none, -1 squads agreed but a lineup missing (the match does not start). Tests (local peering):
// MELEE_TEST_ONLINE_SQUAD="<P1>;<P2>" (squad_test_spec.h) forces both lineups on this side alone, so a
// side given other lineups shows as a setup mismatch, never as a match that drifts apart.
static int squad_setup_bytes(const std::vector<uint8_t>& block, uint32_t rng, std::vector<uint8_t>& out) {
  out.clear();
  if (g_last_search.mode != Matchmaking::DIRECT || bounded_remote_port_count() != 1 || block.size() < 0x63 + 0x24) return 0;
  MuSquadSetup setup{};
  bool have = false;
  if (Matchmaking::local_peer.enabled && squad_offered()) {   // never in a Vanilla run
    if (const char* forced = host::test_env::automated_getenv("MELEE_TEST_ONLINE_SQUAD")) {
      have = squad_test::parse_direct(forced, &setup.sides[0], &setup.sides[1]);
      static bool told = false;
      if (!have && !told) { told = true; host::log("slippi: MELEE_TEST_ONLINE_SQUAD \"%s\" is not two lineups; no squads", forced); }
    }
  }
  if (!have) {
    // Both players chose Squad Strike (the online select's row): the lineups they sent.
    uint8_t counts[2] = {}, lineups[2][8] = {};
    if (!g_netplay || !g_netplay->SquadLineups(counts, lineups)) return 0;
    for (int p = 0; p < 2; ++p) {
      setup.sides[p].count = counts[p] <= 4 ? counts[p] : 0;
      for (unsigned i = 0; i < setup.sides[p].count; ++i) setup.sides[p].members[i] = {lineups[p][2 * i], lineups[p][2 * i + 1]};
    }
    if (!mu_squad_lineup_valid(&setup.sides[0]) || !mu_squad_lineup_valid(&setup.sides[1])) {
      host::log("slippi: squad strike agreed but a lineup is missing (P1 %u, P2 %u fighters); match not started",
                (unsigned)counts[0], (unsigned)counts[1]);
      return -1;
    }
    have = true;
  }
  setup.stocks = block[0x62] >= 1 && block[0x62] <= 99 ? block[0x62] : 4;
  std::vector<uint8_t>& bytes = out;
  bytes.resize(MU_SQUAD_MATCH_MAX);
  bytes.resize(mu_squad_match_encode(&setup, bytes.data(), bytes.size()));
  // Once per match setup (its rng offset) and lineups: this runs every frame both sides are ready.
  static std::vector<uint8_t> logged;
  static uint32_t logged_rng = 0;
  if (!bytes.empty() && (bytes != logged || rng != logged_rng)) {
    logged = bytes;
    logged_rng = rng;
    std::string text;
    for (int p = 0; p < 2; ++p) {
      text += p ? " P2" : "P1";
      for (unsigned i = 0; i < setup.sides[p].count; ++i)
        text += (i ? "," : " ") + std::to_string(setup.sides[p].members[i].kind) + ":" + std::to_string(setup.sides[p].members[i].color);
    }
    host::log("slippi: squad strike on: %s (%u stocks)", text.c_str(), (unsigned)setup.stocks);
  }
  return bytes.empty() ? 0 : 1;
}
#endif

// mu_setup_hash.h, inputs: what the setup is built from, in player order. Only what every machine
// holds the same once every message has arrived: the selections as sent (before the game's own
// overwrites; character fields only once picked, stage fields only once picked, as Merge keeps
// them), those overwrites, the room's teams and the Squad Strike lineups.
uint64_t setup_inputs_hash(const std::vector<PlayerSelections>& sent, const int* room_teams, bool room_teams_used,
                           const std::vector<uint8_t>& squad) {
  uint64_t h = mu_fnv64_bytes(MU_FNV64_OFFSET, "mu-setup-inputs-1", 17);
  h = mu_fnv64_u8(h, (unsigned)sent.size());
  for (const auto& s : sent) {
    h = mu_fnv64_u8(h, s.player_idx);
    h = mu_fnv64_u8(h, s.is_character_selected);
    if (s.is_character_selected) {
      h = mu_fnv64_u8(h, s.character_id);
      h = mu_fnv64_u8(h, s.character_color);
      h = mu_fnv64_u8(h, s.team_id);
    }
    h = mu_fnv64_u8(h, s.is_stage_selected);
    if (s.is_stage_selected) {
      h = mu_fnv64_u16(h, s.stage_id);
      h = mu_fnv64_u8(h, s.alt_stage_mode);
    }
    h = mu_fnv64_u32(h, s.rng_offset);
  }
  h = mu_fnv64_u8(h, (unsigned)g_overwrite_selections.size());
  for (const auto& o : g_overwrite_selections) {
    h = mu_fnv64_u8(h, o.player_idx);
    h = mu_fnv64_u8(h, o.character_id);
    h = mu_fnv64_u8(h, o.character_color);
    h = mu_fnv64_u16(h, o.stage_id);
  }
  h = mu_fnv64_u8(h, room_teams_used);
  if (room_teams_used)
    for (int i = 0; i < PLAYER_COUNT_MAX; ++i) h = mu_fnv64_u8(h, (unsigned)(room_teams[i] + 1));
  return mu_fnv64_bytes(h, squad.data(), squad.size());
}

// mu_setup_hash.h, setup: every byte that defines the match as this side will start it. The mode,
// the player count, each port's slot type, character, colour, costume index and team, the stage,
// the RNG offset, the stage variant, the whole game-info block (stocks, timer, rules and items
// included) and the Squad Strike setup bytes when Squad Strike is live (empty otherwise). With them,
// while Squad Strike is live, this side's budget identity (the 64-hex disc budget hash) and the
// online heap floors: each side admits and trims lineups against its own, so two sides that
// disagree would load different squads. They are setup, never inputs: the peer's identity never
// arrives in a message to wait for, so a difference is a mismatch that refuses the match. The block
// is kept between matches, so an empty port's record still holds whoever last played there: only
// its slot type defines this match, and the rest of that record is left out.
// A CPU's record (slot type 1: the Friends lobby's CPUs) is kept whole like a human's: its character,
// colour, team and level are this match's (mu_setup_record_hashed).
// A Friends lobby match folds in the frozen setup's own digest (`lobby`, 0 otherwise): two machines that
// hold different setups for one match refuse it.
uint64_t setup_block_hash(std::vector<uint8_t> block, unsigned players, uint32_t rng, uint16_t alt_stage,
                          const std::vector<uint8_t>& squad, const std::string& squad_budget, uint64_t lobby = 0,
                          const std::array<slippi::content_wire::Sha256, PLAYER_COUNT_MAX>* custom = nullptr) {
  // Custom fighters online: a port's id is this machine's (each side has its own), so it is hashed as
  // 0xFE and the fighter's content hash is folded in after. No custom fighter: the bytes are today's.
  bool any_custom = false;
  for (int i = 0; custom && i < PLAYER_COUNT_MAX; ++i)
    if ((*custom)[i] != slippi::content_wire::Sha256{} && 0x60 + (size_t)i * 0x24 < block.size()) {
      block[0x60 + i * 0x24] = 0xFE;
      any_custom = true;
    }
  for (int i = 0; i < PLAYER_COUNT_MAX; ++i) {
    const size_t record = 0x60 + (size_t)i * 0x24;
    if (record + 0x24 > block.size() || mu_setup_record_hashed(block[record + 1])) continue;   // 0x61: 0 human, 1 CPU
    const uint8_t type = block[record + 1];
    std::fill(block.begin() + record, block.begin() + record + 0x24, 0);
    block[record + 1] = type;
  }
  uint64_t h = mu_fnv64_bytes(MU_FNV64_OFFSET, "mu-setup-1", 10);
  h = mu_fnv64_u8(h, (unsigned)g_last_search.mode);
  h = mu_fnv64_u8(h, players);
  for (int i = 0; i < PLAYER_COUNT_MAX; ++i) {
    h = mu_fnv64_u8(h, (unsigned)i);
    for (int field : {0x61, 0x60, 0x63, 0x67, 0x69}) h = mu_fnv64_u8(h, block[field + i * 0x24]);
  }
  h = mu_fnv64_u16(h, (unsigned)((block[0xE] << 8) | block[0xF]));
  h = mu_fnv64_u32(h, rng);
  h = mu_fnv64_u16(h, alt_stage);
  h = mu_fnv64_bytes(h, block.data(), block.size());
  h = mu_fnv64_bytes(h, squad.data(), squad.size());
  (void)squad_budget;
  if (lobby) { h = mu_fnv64_u32(h, (uint32_t)(lobby >> 32)); h = mu_fnv64_u32(h, (uint32_t)lobby); }
  if (any_custom) {
    h = mu_fnv64_bytes(h, "nucleus-fighters-1", 18);
    for (int i = 0; i < PLAYER_COUNT_MAX; ++i) {
      if ((*custom)[i] == slippi::content_wire::Sha256{}) continue;
      h = mu_fnv64_u8(h, (unsigned)i);
      h = mu_fnv64_bytes(h, (*custom)[i].data(), (*custom)[i].size());
    }
  }
  return h;
}

// Waits for every peer's setup hash before the game is told the opponent is ready. 1: start,
// 0: wait, -1: refused (*why holds the player's text, empty for a plain disconnect). The only clock
// is the connect timeout, after which a peer whose hash never came counts as disconnected.
int setup_hash_gate(uint8_t remote_count, uint64_t inputs, uint64_t setup, std::string* why) {
  if (!g_netplay) return 1;
  // Tests only: this side builds a different setup, so a pair run shows the refusal end to end.
  if (const char* skew = host::test_env::getenv("MELEE_TEST_SETUP_HASH_SKEW"); skew && skew[0] == '1') setup ^= 1;
  if (!g_setup_hash.have || g_setup_hash.local.inputs != inputs || g_setup_hash.local.setup != setup) {
    g_setup_hash = {};
    g_setup_hash.have = true;
    g_setup_hash.local = {g_netplay->LocalPlayerPort(), inputs, setup};
    g_setup_hash.since_ms = time_ms();
    g_netplay->SendSetupHash(inputs, setup);
    host::log("setup hash %016llx local (inputs %016llx), sent", (unsigned long long)setup,
              (unsigned long long)inputs);
  }
  bool waiting = false, wait_over = false;
  const uint64_t now = time_ms();
  for (int i = 0; i < remote_count && i < REMOTE_PLAYER_MAX; ++i) {
    const auto build = g_netplay->GetRemoteBuild(i);
    const auto peer = g_netplay->GetSetupHash(i);
    const auto decision = mu_setup_hash_decide(build.setup_hash, peer.received, &g_setup_hash.local, &peer.value);
    // Noted on every call, agreed or not: a peer's newer hash restarts its wait from when it arrived
    // (mu_setup_hash_wait_over). A ranked seat one strike step ahead is not a lost peer.
    const bool over = mu_setup_hash_wait_over(&g_setup_hash.wait[i], g_setup_hash.since_ms, peer.received,
                                              &peer.value, now, kConnectTimeoutMs);
    if (g_setup_hash.logged[i] != (int)decision || g_setup_hash.logged_setup[i] != peer.value.setup) {
      g_setup_hash.logged[i] = (int)decision;
      g_setup_hash.logged_setup[i] = peer.value.setup;
      if (decision == MU_SETUP_HASH_SKIP)
        host::log("setup hash: remote[%d] has no setup hash; not checked (another pool)", i);
      else if (peer.received)
        host::log("setup hash %016llx local, %016llx remote[%d] (inputs %016llx local, %016llx remote): %s",
                  (unsigned long long)g_setup_hash.local.setup, (unsigned long long)peer.value.setup, i,
                  (unsigned long long)g_setup_hash.local.inputs, (unsigned long long)peer.value.inputs,
                  decision == MU_SETUP_HASH_AGREE ? "agree"
                  : decision == MU_SETUP_HASH_MISMATCH ? "MISMATCH, waiting for a newer hash before refusing"
                                                       : "other inputs, waiting");
      else host::log("setup hash %016llx local, waiting for remote[%d]", (unsigned long long)g_setup_hash.local.setup, i);
    }
    if (decision != MU_SETUP_HASH_MISMATCH) g_setup_hash.pending[i].open = 0;
    else if (mu_setup_hash_mismatch_final(&g_setup_hash.pending[i], &g_setup_hash.local, &peer.value, time_ms())) {
      host::log("setup hash: setup differs from remote[%d] for %d ms; match not started", i, MU_SETUP_HASH_SETTLE_MS);
      *why = MU_SETUP_HASH_TEXT;
      return -1;
    }
    if (decision == MU_SETUP_HASH_WAIT || decision == MU_SETUP_HASH_MISMATCH) {
      waiting = true;
      wait_over = wait_over || over;
    }
  }
  if (!waiting) {
    if (!g_setup_hash.agreed) host::log("setup hash %016llx: every peer that checks agrees", (unsigned long long)g_setup_hash.local.setup);
    g_setup_hash.agreed = true;
    return 1;
  }
  if (wait_over) {
    bool differs = false;
    for (int i = 0; i < remote_count && i < REMOTE_PLAYER_MAX; ++i) differs = differs || g_setup_hash.pending[i].open;
    if (differs) {
      host::log("setup hash: setup still differs after %llu ms; match not started", (unsigned long long)kConnectTimeoutMs);
      *why = MU_SETUP_HASH_TEXT;
      return -1;
    }
    host::log("setup hash: no matching hash after %llu ms; treated as a disconnect", (unsigned long long)kConnectTimeoutMs);
    why->clear();
    return -1;
  }
  return 0;
}

// Retrace of the last CMD_GET_MATCH_STATE. The game polls it every frame the online menus are up
// (mode select, the online character select, waiting for the opponent) and never during a match,
// so it is the signal for "the player is in the online menus right now".
uint32_t g_last_match_state_retrace = 0;
std::atomic<bool> g_online_menu_visible{false};

void prepare_online_match_state(std::vector<uint8_t>& q) {
  g_last_match_state_retrace = host::retrace_count();
  host::set_emulation_speed(1.0);
  update_discord_presence();   // rate limited internally; the game polls this every frame
  static std::vector<uint8_t> block = {
      0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00, 0x1F, 0x00, 0x00,
      0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
      0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
      0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
  };
  static const std::vector<uint8_t> shipped = block;   // as shipped, before any match wrote it
  q.clear();
  Matchmaking::ProcessState mm_state = !g_forced_error.empty() ? Matchmaking::ERROR_ENCOUNTERED : g_matchmaking->GetMatchmakeState();
  {
    // What the game reads as the match state, once per change: a set abandoned at the results screen
    // because this was not CONNECTION_SUCCESS (11-01, game 1 of a Ranked set) must say so.
    static int reported = -1;
    if ((int)mm_state != reported) {
      static const char* const names[] = {"idle", "initializing", "matchmaking", "opponent connecting", "connection success", "error"};
      const int n = (int)mm_state;
      std::string why;
      if (!g_forced_error.empty()) why = " (forced error: " + g_forced_error + ")";
      else if (mm_state == Matchmaking::ERROR_ENCOUNTERED) why = " (" + g_matchmaking->GetErrorMessage() + ")";
      host::log("slippi: match state %s -> %s%s", reported < 0 || reported > 5 ? "start" : names[reported],
                n >= 0 && n <= 5 ? names[n] : "unknown", why.c_str());
      reported = n;
    }
  }
  q.push_back((uint8_t)mm_state);
  uint8_t local_ready = g_local_selections.is_character_selected ? 1 : 0;
  uint8_t remote_ready = 0;
  UserInfo me = g_user->GetUserInfo();
  uint16_t alt_stage_mode = 0;
  if (mm_state == Matchmaking::CONNECTION_SUCCESS) {
    g_local_player_index = (uint8_t)std::clamp(g_matchmaking->LocalPlayerIndex(), 0, PLAYER_COUNT_MAX - 1);
    if (!g_netplay) {
      g_netplay = g_matchmaking->GetNetplayClient();
      g_recent_mm_result = g_matchmaking->GetMatchmakeResult();
      g_ranked_completed = false;
      g_allowed_stages = g_recent_mm_result.stages;
      if (g_allowed_stages.empty()) g_allowed_stages = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
      g_stage_pool.clear();
      g_local_selections.stage_id = random_stage(g_last_search.mode);
      g_netplay->SetMatchSelections(g_local_selections);
    }
    bool connected = g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED;
    if (g_netplay->GetActivePlayerIndices().size() != bounded_remote_port_count()) connected = false;
    if (!connected) {
      // The opponent may have refused first and left: a build that was already received still
      // explains why, instead of a plain disconnect.
      std::string refusal;
      if (build_verdict(bounded_remote_machine_count(), &refusal) < 0) {
        host::log("slippi: build check: %s", refusal.c_str());
        cleanup_connection();
        g_forced_error = refusal;
        prepare_online_match_state(q);
        return;
      }
    }
    if (connected) {
      const MatchInfo mi = g_netplay->GetMatchInfo();
      g_remote_player_index = mi.remote[0].player_idx;
      remote_ready = 1;
      uint8_t remote_count = bounded_remote_machine_count();
      for (int i = 0; i < remote_count; ++i) if (!mi.remote[i].is_character_selected) remote_ready = 0;
      std::string refusal;
      const int verdict = build_verdict(remote_count, &refusal);
      if (verdict < 0) {
        host::log("slippi: build check: %s", refusal.c_str());
        cleanup_connection();
        g_forced_error = refusal;
        prepare_online_match_state(q);
        return;
      }
      if (verdict == 0) remote_ready = 0;   // the opponent's build is not known yet
      // Items, as Squad Strike below: with Items on here (only where items_offered, so Direct and Teams),
      // every peer that reads the rule must have sent its choice before the setup is (else the two games
      // could set up different matches: its 0xEB can trail its pick right after connecting).
      if (remote_ready && g_netplay->ItemsPending()) {
        remote_ready = 0;
        static uint64_t told_at = 0;
        if (g_netplay->ConnectedAtMs() != told_at) {
          told_at = g_netplay->ConnectedAtMs();
          host::log("slippi: waiting for every player's Items choice before the match is set up");
        }
      }
#if MU_SQUAD
      // Squad Strike in Direct: with it on here, the peer's choice and lineup must be here before the
      // setup is (else both games could agree on a normal match before either heard the other).
      if (remote_ready && g_last_search.mode == Matchmaking::DIRECT && g_netplay->SquadPending()) remote_ready = 0;
#endif
      if (local_ready && remote_ready && !Matchmaking::local_peer.enabled) {
        std::string version_error;
        const int version = g_user->PollOnlineCompatibility(&version_error);
        if (version < 0) {
          cleanup_connection();
          g_forced_error = version_error;
          prepare_online_match_state(q);
          return;
        }
        if (version == 0) remote_ready = 0;  // fresh verification, including an existing connection's rematch
      }
      if (remote_count == 1) {
        bool decider = g_netplay->IsDecider();
        g_local_player_index = decider ? 0 : 1;
        g_remote_player_index = decider ? 1 : 0;
      }
      custom_fighters_gate(remote_count, mi, local_ready, remote_ready);
    } else {
      cleanup_connection();
      prepare_online_match_state(q);
      return;
    }
    if (!g_play_session_active) g_play_session_active = true;
  } else {
    stop_early_inputs();
    std::lock_guard<std::mutex> lk(g_early_send_mutex);
    g_netplay = nullptr;
  }
  uint32_t rng_offset = 0;
  std::string local_name, opp_name;
  uint8_t chat_message_id = 0, chat_message_player_idx = 0, sent_chat_message_id = 0;
  DesyncRecoveryResp recovery;
  if (g_netplay && g_last_search.mode == Matchmaking::RANKED) recovery = g_netplay->GetDesyncRecoveryState();
  if (recovery.is_error) { cleanup_connection(); prepare_online_match_state(q); return; }
  if (recovery.is_recovering && recovery.is_waiting) remote_ready = 0;
  q.push_back(local_ready);
  q.push_back(remote_ready);
  q.push_back(g_local_player_index);
  q.push_back(g_remote_player_index);
  if (g_netplay) {
    bool single = g_matchmaking && g_matchmaking->RemotePlayerCount() == 1;
    bool chat = chat_enabled();
    sent_chat_message_id = g_netplay->GetSlippiRemoteSentChatMessage(chat);
    if (sent_chat_message_id <= 0) {
      PlayerSelections rm = g_netplay->GetSlippiRemoteChatMessage(chat);
      chat_message_id = (uint8_t)rm.message_id;
      chat_message_player_idx = rm.player_idx;
      if (chat_message_id == CHAT_MSG_CHAT_DISABLED && !single) chat_message_id = chat_message_player_idx = 0;
    } else {
      chat_message_player_idx = g_local_player_index;
    }
    if (single || !g_matchmaking) chat_message_player_idx = sent_chat_message_id > 0 ? g_local_player_index : g_remote_player_index;
    local_name = me.display_name;
  }
  const bool setup_built = local_ready && remote_ready;
  uint64_t setup_lobby = 0;
  uint64_t setup_inputs = 0;
  std::vector<uint8_t> setup_squad;   // the Squad Strike setup bytes, empty when it is not live
  std::vector<uint8_t> squad_inputs;  // Squad Strike in Direct: the lineups as exchanged (inputs hash)
  std::string setup_budget;           // with them, this side's squad budget identity (setup hash only)
  std::array<slippi::content_wire::Sha256, PLAYER_COUNT_MAX> setup_custom{};   // custom fighters' content hashes by port
  if (local_ready && remote_ready) {
    bool decider = g_netplay->IsDecider();
    uint8_t remote_count = bounded_remote_machine_count();
    const MatchInfo mi = g_netplay->GetMatchInfo();
    PlayerSelections lps = mi.local;
    PlayerSelections rps[REMOTE_PLAYER_MAX];
    for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) rps[i] = mi.remote[i];
    bool local_char_ok = lps.character_id < 26, remote_char_ok = true;
    std::vector<PlayerSelections*> ordered(remote_count + 1, nullptr);
    if (lps.player_idx < ordered.size()) ordered[lps.player_idx] = &lps;
    for (int i = 0; i < remote_count; ++i) if (rps[i].player_idx < ordered.size()) ordered[rps[i].player_idx] = &rps[i];
    // A port with no selections is no match: refused, never filled with this player's own pick (that
    // played a fighter nobody picked, and in the Friends lobby a seat that is not this player's).
    if (std::find(ordered.begin(), ordered.end(), nullptr) != ordered.end()) {
      host::log("slippi: a port's selections are missing (%u remote); match not started", (unsigned)remote_count);
      cleanup_connection();
      g_forced_error = "A player's picks didn't arrive. Match not started.";
      prepare_online_match_state(q);
      return;
    }
    std::vector<PlayerSelections> sent_selections;   // as sent, before the game's overwrites (setup hash)
    for (auto* o : ordered) sent_selections.push_back(*o);
    // Custom fighters online: each remote sent its own machine's id; the block holds this machine's
    // fighter with the same content hash, through that remote's list (custom_fighters_gate).
    bool custom_port[PLAYER_COUNT_MAX] = {};
    const bool by_code = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
    for (auto* o : ordered) {
      if (!by_code || !o->is_character_selected || o->character_id < 26 || o->player_idx >= PLAYER_COUNT_MAX ||
          !host::online_fighters::roster_usable())
        continue;
      slippi::content_wire::Sha256 content{};
      if (o == &lps) {
        if (host::online_fighters::is_custom(o->character_id) && host::online_fighters::content_of(o->character_id, &content)) {
          custom_port[o->player_idx] = true;
          setup_custom[o->player_idx] = content;
        }
        continue;
      }
      uint8_t local = 0;
      if (!host::online_fighters::map_remote(o->player_idx, o->character_id, &local, &content)) continue;
      o->character_id = local;
      custom_port[o->player_idx] = true;
      setup_custom[o->player_idx] = content;
    }
    host::online_fighters::set_match(setup_custom.data(), setup_custom.size());   // the replay's record
    // Direct and Teams: a custom fighter every machine has is allowed; Ranked and Unranked never.
    local_char_ok = lps.character_id < 26 || (lps.player_idx < PLAYER_COUNT_MAX && custom_port[lps.player_idx]);
    for (int i = 0; i < remote_count; ++i)
      if (rps[i].character_id >= 26 && !(rps[i].player_idx < PLAYER_COUNT_MAX && custom_port[rps[i].player_idx])) remote_char_ok = false;
    bool fixed_char_ok = lps.character_id < 26, fixed_remote_ok = true;
    for (int i = 0; i < remote_count; ++i) if (rps[i].character_id >= 26) fixed_remote_ok = false;
    for (size_t i = 0; i < g_overwrite_selections.size() && i < ordered.size(); ++i) {
      ordered[i]->character_id = g_overwrite_selections[i].character_id;
      ordered[i]->character_color = g_overwrite_selections[i].character_color;
      ordered[i]->stage_id = g_overwrite_selections[i].stage_id;
    }
    uint16_t stage_id = 0x1F;
    for (auto* s : ordered) { if (!s->is_stage_selected) continue; stage_id = s->stage_id; alt_stage_mode = s->alt_stage_mode; break; }
    if (Matchmaking::local_peer.enabled && Matchmaking::local_peer.test_stage >= 0) {
      // Force the final host-built setup packet too: each test peer may have sent its menu choice
      // before the other peer finished connecting, so the earlier local selection override alone
      // is not sufficient to make a deterministic local regression match.
      stage_id = static_cast<uint16_t>(Matchmaking::local_peer.test_stage);
      host::log("slippi: local regression match stage %u", stage_id);
    }
    // A selection can arrive marked "stage picked" with stage 0, which is no stage (B-528: game 3 of a set; the stage
    // load found no music for it and stopped at ground.c). Both games then take the same legal stage, chosen by the
    // rng offset both sides already agree on, instead of starting a match on nothing.
    if (stage_id == 0) {
      const uint32_t agreed = decider ? lps.rng_offset : rps[0].rng_offset;
      stage_id = stage_pool::agreed_stage(agreed, remote_count > 1);
      host::log("slippi: the selections named no stage (0); both games take stage %u", (unsigned)stage_id);
    }
    if (Matchmaking::IsFixedRulesMode(g_last_search.mode)) {
      if (!fixed_char_ok) { cleanup_connection(Leave::Quit); g_forced_error = "The character you selected is not allowed in this mode"; prepare_online_match_state(q); return; }
      if (!fixed_remote_ok) { cleanup_connection(); prepare_online_match_state(q); return; }
      const auto& stages = allowed_stages(g_last_search.mode);
      if (
          std::find(stages.begin(), stages.end(), stage_id) == stages.end()) { cleanup_connection(); prepare_online_match_state(q); return; }
    } else if (g_last_search.mode == Matchmaking::TEAMS && !(g_local_build.mod_view && g_local_build.extended_content)) {
      if (!local_char_ok) { cleanup_connection(); g_forced_error = "The character you selected is not allowed in this mode"; prepare_online_match_state(q); return; }
      if (!remote_char_ok) { cleanup_connection(); prepare_online_match_state(q); return; }
    } else if (g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS) {
      // build_verdict already confirmed the peer's content. Only Static mod boot advertises that
      // it runs the disc's extended fighter/stage code; Source and retail keep vanilla limits.
      bool foreign = false;
        // A custom fighter both sides have (custom_port) is no foreign character.
        const auto shown = [&](const PlayerSelections& p) {
          return p.player_idx < PLAYER_COUNT_MAX && custom_port[p.player_idx] ? (uint8_t)0 : p.character_id;
        };
        foreign = !native_direct_selection_supported(g_local_build.mod_view, g_local_build.extended_content,
                                                     shown(lps), stage_id);
        for (int i = 0; i < remote_count; ++i)
          if (!native_direct_selection_supported(g_local_build.mod_view, g_local_build.extended_content,
                                                 shown(rps[i]), stage_id)) foreign = true;
      if (foreign) {
        cleanup_connection();
        g_forced_error = g_local_build.mod_view
            ? "Your opponent picked a character or stage whose code this build does not have"
            : "Your opponent is on a different build (their character or stage is not in this game)";
        host::log("slippi: build check: %s", g_forced_error.c_str());
        prepare_online_match_state(q);
        return;
      }
    }
    rng_offset = decider ? lps.rng_offset : rps[0].rng_offset;
    uint8_t first_team = ordered[0]->team_id;
    bool all_same_team = true;
    for (auto* s : ordered) if (s->team_id != first_team) all_same_team = false;
    static const uint8_t perms[6][4] = {{0, 0, 1, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 0, 1}, {1, 0, 1, 0}};
    const uint8_t* team_assign = perms[rng_offset % 6];
    bool teams = g_last_search.mode == Matchmaking::TEAMS;
    std::array<int, PLAYER_COUNT_MAX> room_teams;
    room_teams.fill(-1);
#if MU_SQUAD
    squad_inputs = squad_exchanged_lineups();   // Squad Strike in Direct: the lineups as exchanged
#endif
    setup_inputs = setup_inputs_hash(sent_selections, room_teams.data(), false, squad_inputs);
    // The block is kept between matches, and a Friends lobby setup writes bytes no other match does
    // (P1/P2 slot types, CPU kind and level, the rules bit): back to the shipped block's first. They
    // are hashed, so two machines with different pasts would otherwise refuse the match.
    for (int i = 0; i < 4; ++i) {
      if (i < 2) block[0x61 + i * 0x24] = shipped[0x61 + i * 0x24];
      block[0x6E + i * 0x24] = shipped[0x6E + i * 0x24];
      block[0x6F + i * 0x24] = shipped[0x6F + i * 0x24];
    }
    block[1] = (uint8_t)((block[1] & ~0x01) | (shipped[1] & 0x01));
    for (auto* s : ordered) {
      if (!s->is_character_selected) continue;
      uint8_t team = teams ? s->team_id : 0;
      if (teams && all_same_team) team = team_assign[s->player_idx];
      block[0x60 + s->player_idx * 0x24] = s->character_id;
      block[0x63 + s->player_idx * 0x24] = s->character_color;
      block[0x67 + s->player_idx * 0x24] = 0;
      block[0x69 + s->player_idx * 0x24] = team;
    }
    // P3/P4's slot types, then the costume indices over this match's human ports (mu_setup_hash.h).
    mu_setup_ports(block.data(), block.size(), uint8_t(ordered.size() - 1), teams ? 1 : 0);
    block[0x8] = teams ? 1 : 0;
    block[0xE] = (uint8_t)(stage_id >> 8); block[0xF] = (uint8_t)stage_id;
    // Direct pauses (Slippi's rule) unless a player turned it off and every build reads that (PauseOff).
    const bool pause_off = g_last_search.mode == Matchmaking::DIRECT && g_netplay && g_netplay->PauseOff();
    {
      static int logged = -1;
      if (g_last_search.mode == Matchmaking::DIRECT && logged != (pause_off ? 1 : 0)) {
        logged = pause_off ? 1 : 0;
        host::log("slippi: Direct pause %s", pause_off ? "off (a player turned it off)" : "on");
      }
    }
    bool pause_allowed = g_last_search.mode == Matchmaking::DIRECT && !pause_off;
    block[2] = pause_allowed ? (block[2] & 0xF7) : (block[2] | 0x8);
    bool stage_selection_mode = g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS;
    if (!stage_selection_mode) alt_stage_mode = 0;
    // Every game starts at 4 stocks and 0%; the timer is written below.
    for (int i = 0; i < 4; ++i) {
      block[0x62 + i * 0x24] = recovery.state.fighters[i].stocks_remaining;
      const auto health = recovery.state.fighters[i].current_health;
      block[0x70 + i * 0x24] = static_cast<uint8_t>(health >> 8);
      block[0x71 + i * 0x24] = static_cast<uint8_t>(health);
    }
    // Tests only (an automated run on local peering): MELEE_ONLINE_TEST_STOCKS=<1-99> so a long test
    // match ends by its timer rather than by random inputs' self-destructs.
    if (Matchmaking::local_peer.enabled && !recovery.is_recovering) {
      if (const char* stocks = host::test_env::automated_getenv("MELEE_ONLINE_TEST_STOCKS")) {
        const int n = std::atoi(stocks);
        if (n >= 1 && n <= 99)
          for (int i = 0; i < 4; ++i) block[0x62 + i * 0x24] = (uint8_t)n;
      }
    }
    // One line per distinct match setup (this runs every frame both sides are ready), so multi-
    // instance test logs show the teams, characters and stage every peer agreed on.
    {
      char setup[160];
      int len = std::snprintf(setup, sizeof setup, "teams %d stage %u rng %08X:", teams ? 1 : 0, (unsigned)stage_id, rng_offset);
      for (int i = 0; i < PLAYER_COUNT_MAX && len > 0 && len < (int)sizeof setup; ++i)
        if (block[0x61 + i * 0x24] == 0) {
          // A custom fighter by its content hash: the ids differ between machines, the fighter does not.
          char who[24];
          if (setup_custom[i] != slippi::content_wire::Sha256{})
            std::snprintf(who, sizeof who, "C:%s", slippi::content_wire::hex(setup_custom[i]).substr(0, 8).c_str());
          else std::snprintf(who, sizeof who, "%u", block[0x60 + i * 0x24]);
          len += std::snprintf(setup + len, sizeof setup - len, " P%d char %s color %u team %u", i + 1, who,
                               block[0x63 + i * 0x24], block[0x69 + i * 0x24]);
        }
      static std::string last_setup;
      if (last_setup != setup) { last_setup = setup; host::log("slippi: match setup %s", setup); }
      for (int i = 0; i < 4; ++i) g_game_facts.teams[i] = teams ? block[0x69 + i * 0x24] : -1;
      g_game_facts.stage = stage_id;
      g_game_facts.rng = rng_offset;
      g_game_facts.characters[0] = block[0x60];
      g_game_facts.characters[1] = block[0x60 + 0x24];
    }
  }
  {
    block[0x0] = 0x32; block[0x3] = 0x4C;
    uint32_t t = recovery.is_recovering && setup_built ? recovery.state.seconds_remaining : 8 * 60;
    // Tests only: MELEE_ONLINE_TEST_TIMER=<seconds> ends a match quickly so the
    // result reporting can be exercised.
    if (Matchmaking::local_peer.enabled) {
      if (const char* test_timer = host::test_env::getenv("MELEE_ONLINE_TEST_TIMER")) t = (uint32_t)std::atoi(test_timer);
    }
    block[0x10] = (uint8_t)(t >> 24); block[0x11] = (uint8_t)(t >> 16); block[0x12] = (uint8_t)(t >> 8); block[0x13] = (uint8_t)t;
  }
  block[0xB] = 0xFF;
  uint64_t items = 0xF80000000F000000ull;
  if (g_recent_mm_result.items != 0) { items |= (uint64_t)g_recent_mm_result.items << 28; block[0xB] = 3; }
  for (int i = 0; i < 8; ++i) block[0x23 + i] = (uint8_t)(items >> (56 - 8 * i));
  // Items in Direct and Teams (items_option): only when every player's game reads the rule and every
  // player has it on, from the choices sent just before each lock-in. Then the game's own VS default
  // (gm_SetupRulesDefaults): frequency Medium and every item switch on (rules.x20 all ones; 0x28-0x2A
  // stay as Slippi's write above leaves them, the default's x28 = 0). The block is hashed whole, so a
  // side that decided otherwise refuses the match (setup_hash_gate) instead of playing another game.
  if (setup_built && g_recent_mm_result.items == 0 && g_netplay &&
      (g_last_search.mode == Matchmaking::DIRECT || g_last_search.mode == Matchmaking::TEAMS)) {
    const NetplayClient::ItemsVerdict verdict = g_netplay->Items();
    if (verdict.on) {
      block[0xB] = 2;
      for (int i = 0; i < 8; ++i) block[0x20 + i] = 0xFF;
    }
    // Once per match setup (its rng offset) and verdict: this runs every frame both sides are ready.
    static uint64_t logged = ~0ull;
    const uint64_t key = ((uint64_t)rng_offset << 8) | (uint64_t)((verdict.on ? 1 : verdict.unsupported ? 2 : 0) << 4) |
                         (uint64_t)(verdict.blocker + 1);
    if (key != logged) {
      logged = key;
      if (verdict.on) host::log("slippi: items on (every player has Items on): every item, frequency Medium");
      else if (verdict.unsupported) host::log("slippi: items off (P%d's game has no items rule)", verdict.blocker + 1);
      else if (verdict.blocker >= 0) host::log("slippi: items off (P%d has Items off)", verdict.blocker + 1);
      else host::log("slippi: items off (this player has Items off)");
    }
  }
  // Tests only (an automated run on local peering): MELEE_TEST_ONLINE_ITEMS=<frequency>[:<mask hex>]
  // writes an item rule into this side's block before the setup hash is made from it: frequency 0-4
  // (Very Low to Very High) and the game's 64-bit item switch at 0x20 (default: every item).
  if (Matchmaking::local_peer.enabled) {
    if (const char* forced = host::test_env::automated_getenv("MELEE_TEST_ONLINE_ITEMS")) {
      char* end = nullptr;
      const long frequency = std::strtol(forced, &end, 10);
      uint64_t mask = ~0ull;
      if (end && *end == ':') mask = std::strtoull(end + 1, nullptr, 16);
      if (frequency >= 0 && frequency <= 4) {
        block[0xB] = (uint8_t)frequency;
        for (int i = 0; i < 8; ++i) block[0x20 + i] = (uint8_t)(mask >> (56 - 8 * i));
        static bool logged = false;
        if (!logged) {
          logged = true;
          host::log("slippi: test item rule: frequency %ld, mask %016llX (MELEE_TEST_ONLINE_ITEMS)", frequency,
                    (unsigned long long)mask);
        }
      }
    }
  }
#if MU_SQUAD
  // Squad Strike in Direct: the squad setup from the finished block (its stocks), kept for the game's
  // MU_SQUAD_OP_MATCH_SETUP and hashed with the block below. Empty (a normal match) otherwise.
  if (setup_built) {
    const int squads = squad_setup_bytes(block, rng_offset, setup_squad);
    {
      std::lock_guard<std::mutex> lk(g_squad_match_mutex);
      g_squad_match = setup_squad;
    }
    if (squads < 0) {
      if (g_netplay) g_netplay->ForceDisconnect();
      cleanup_connection();
      g_forced_error = "Squad Strike: a player has no lineup of retail fighters. Match not started.";
      prepare_online_match_state(q);
      return;
    }
  }
#endif
  // The block is final here. Both sides ready: hold "opponent ready" until every peer that sends a
  // setup hash has sent the same one; a different one refuses the match, before any frame runs.
  if (setup_built) {
    std::string why;
    const int gate = setup_hash_gate(bounded_remote_machine_count(), setup_inputs,
                                     setup_block_hash(block, bounded_remote_port_count() + 1u, rng_offset, alt_stage_mode,
                                                      setup_squad, setup_budget, setup_lobby, &setup_custom), &why);
    if (gate < 0) {
      g_netplay->ForceDisconnect();
      cleanup_connection();
      g_forced_error = why;
      prepare_online_match_state(q);
      return;
    }
    if (gate == 0) q[2] = 0;   // MSRB_REMOTE_READY: the game keeps waiting for the opponent
  } else if (g_setup_hash.have) {
    g_setup_hash = {};   // a new setup (a rematch, a changed pick) starts a new wait
  }
  append_u32(q, rng_offset);
  q.push_back((uint8_t)effective_delay());
  q.push_back(sent_chat_message_id);
  q.push_back(chat_message_id);
  q.push_back(chat_message_player_idx);
  std::array<uint8_t, 2> ranks{};
  if (g_last_search.mode == Matchmaking::RANKED) {
    for (const auto& player : g_matchmaking->GetPlayerInfo())
      if (player.port == 1 || player.port == 2)
        ranks[player.port - 1] = report::decide_rank(player.ranked_rating, player.ranked_global_placement, player.ranked_regional_placement, player.ranked_update_count);
  }
  q.push_back(ranks[0]);
  q.push_back(ranks[1]);
  std::string ln = convert_string_for_game(local_name, 15);
  q.insert(q.end(), ln.begin(), ln.end());
  for (int i = 0; i < 4; ++i) { std::string n = convert_string_for_game(g_matchmaking->GetPlayerName((uint8_t)i), 15); q.insert(q.end(), n.begin(), n.end()); }
  std::vector<std::string> opponent_names;
  auto players = g_matchmaking->GetPlayerInfo();
  int team_idx = block[0x69 + g_local_player_index * 0x24];
  for (int i = 0; i < 4; ++i) {
    bool teams = g_last_search.mode == Matchmaking::TEAMS;
    bool same_team = block[0x69 + i * 0x24] == team_idx;
    bool human = block[0x61 + i * 0x24] == 0;
    if (g_local_player_index == i || !human || (same_team && teams)) continue;
    std::string n = g_matchmaking->GetPlayerName((uint8_t)i);
    if (!n.empty()) opponent_names.push_back(n);
  }
  if (mm_state == Matchmaking::OPPONENT_CONNECTING) {
    // Before selections arrive, the ticket owns the local port; the game block and
    // g_local_player_index may still describe the previous match.
    opponent_names.clear();
    const int local = g_matchmaking->LocalPlayerIndex();
    for (const auto& p : players)
      if (p.port - 1 != local && !p.display_name.empty()) opponent_names.push_back(p.display_name);
  }
  size_t num_opp = opponent_names.empty() ? 1 : opponent_names.size();
  int chars_per_name = (int)((15 - (num_opp - 1)) / num_opp);
  std::string opp_text;
  for (auto& n : opponent_names) { if (!opp_text.empty()) opp_text += "/"; opp_text += truncate_length_char(n, chars_per_name); }
  // MSRB_OPP_NAME is only the CSS summary. Keep the four player names/codes above/below
  // raw: the native HUD and replay metadata consume those identity fields.
  opp_name = convert_string_for_game(opp_text, 15);
  q.insert(q.end(), opp_name.begin(), opp_name.end());
  std::array<UserInfo, PLAYER_COUNT_MAX> identities;
  for (const auto& player : players) if (player.port >= 1 && player.port <= PLAYER_COUNT_MAX) identities[player.port - 1] = player;
  for (const auto& player : identities) { std::string c = convert_connect_code_for_game(player.connect_code); q.insert(q.end(), c.begin(), c.end()); }
  for (const auto& player : identities) { std::string uid = player.uid; uid.resize(29); q.insert(q.end(), uid.begin(), uid.end()); }
  std::string err = convert_string_for_game(!g_forced_error.empty() ? g_forced_error : g_matchmaking->GetErrorMessage(), 120);
  q.insert(q.end(), err.begin(), err.end());
  q.insert(q.end(), block.begin(), block.end());
  std::string match_id = g_recent_mm_result.id;
  match_id.resize(51);
  q.insert(q.end(), match_id.begin(), match_id.end());
  q.push_back((uint8_t)alt_stage_mode);
  // Slippi 3.7.0: what decides each player's rank, and their ranked MMR, by port, then whether each
  // is a bot account. The game records them in the replay.
  for (int i = 0; i < 4; ++i) {
    const auto& info = identities[i];
    uint32_t rating, mmr;
    std::memcpy(&rating, &info.ranked_rating, 4); std::memcpy(&mmr, &info.ranked_mmr, 4);
    append_u32(q, rating);
    append_u32(q, (uint32_t)info.ranked_update_count);
    q.push_back((uint8_t)((uint16_t)info.ranked_global_placement >> 8)); q.push_back((uint8_t)info.ranked_global_placement);
    q.push_back((uint8_t)((uint16_t)info.ranked_regional_placement >> 8)); q.push_back((uint8_t)info.ranked_regional_placement);
    append_u32(q, mmr);
  }
  for (int i = 0; i < 4; ++i) q.push_back((uint8_t)(identities[i].is_bot));
}

std::string g_rank_account;

void update_rank_account() {
  const auto uid = g_user->IsLoggedIn() ? g_user->GetUserInfo().uid : std::string();
  if (uid == g_rank_account) return;
  g_rank_account = uid;
  report::clear_rank();
  if (!uid.empty() && official_matchmaking())
    report::fetch_user_rank(uid);
}

void prepare_online_status(std::vector<uint8_t>& q) {
  q.clear();
  g_user->AttemptLogin();
  update_rank_account();
  UserInfo me = g_user->GetUserInfo();
  uint8_t app_state = 0;
  if (g_user->IsLoggedIn()) app_state = 1;   // version check: this port speaks SLIPPI_SEMVER; the server enforces the rest
  q.push_back(app_state);
  std::string name = convert_string_for_game(me.display_name, 15);
  q.insert(q.end(), name.begin(), name.end());
  std::string code = convert_connect_code_for_game(me.connect_code);
  q.insert(q.end(), code.begin(), code.end());
}

static void finish_summary(const char* result) {
  if (!g_summary.active) return;
  g_summary.active = false;
  host::log("%s", g_summary.line(g_rollbacks - g_match_rollbacks_start, g_match_stalls,
                               g_checksums_mismatched, result).c_str());
}

void handle_report_game(const uint8_t* p) {
  // ReportGameQuery (packed, big-endian fields): mode, frameLength, gameIndex, tiebreakIndex, winnerIdx, gameEndMethod, lrasInitiator, syncedTimer, players[4], gameInfoBlock[312]
  uint8_t mode = p[0];
  uint32_t frames = be32(p + 1), game_index = be32(p + 5), tiebreak = be32(p + 9);
  int8_t winner = (int8_t)p[13]; uint8_t end_method = p[14]; int8_t lras = (int8_t)p[15];
  g_summary.frames = frames;
  finish_summary(end_method == 7 ? "no-contest" : end_method == 1 ? "timeout" : lras >= 0 ? "lras" : "completed");
  const uint8_t* players = p + 20;
  if (mode == Matchmaking::RANKED && end_method == 7 && g_netplay) {
    SyncedGameState state;
    state.match_id = g_recent_mm_result.id;
    state.game_index = game_index;
    state.tiebreak_index = tiebreak;
    state.seconds_remaining = be32(p + 16);
    for (int i = 0; i < 4; ++i) {
      state.fighters[i].stocks_remaining = players[i * 9 + 6];
      state.fighters[i].current_health = be16(players + i * 9 + 7);
    }
    g_netplay->SendSyncedGameState(state);
  }
  const uint8_t* info_block = players + 4 * 9;
  int stage = be16(info_block + 0xE);
  host::log("slippi: game report: mode %u, %u frames, game %u, tiebreak %u, winner %d, end %u, lras %d, stage %d",
            mode, frames, game_index, tiebreak, winner, end_method, lras, stage);
  host::log("slippi: online game summary: delay %d frames, average ping %.1f ms, %llu rollbacks, %llu stalled frames, %llu waits for the other player, %llu frames advanced, %u frames",
            g_match_delay, g_netplay ? g_netplay->GetAndResetAvgPingMs() : 0.0,
            (unsigned long long)(g_rollbacks - g_match_rollbacks_start), (unsigned long long)g_match_stalls,
            (unsigned long long)g_match_input_waits, (unsigned long long)g_match_advances, frames);
  {
    const host::early_input::Stats e = host::early_input::stats();
    host::log("slippi: early inputs %s: %llu changes, %llu packets, %llu guarded, %llu resent after a skip, %llu follow-ups, %llu received, %llu controller reads, local controller port %d",
              early_inputs_enabled() ? (early_inputs_accept() ? "on" : "sending only") : "off",
              (unsigned long long)e.changes, (unsigned long long)e.packets, (unsigned long long)e.guarded,
              (unsigned long long)e.resent_after_skip, (unsigned long long)e.follow_ups, (unsigned long long)e.received, (unsigned long long)e.samples, e.local_port + 1);
  }
  {
    const suggestions::Stats s = suggestions::stats();
    host::log("slippi: input suggestions %s: %llu sent, %llu received, %llu replies predicted from one, %d players ignored",
              s.enabled ? "on" : "off", (unsigned long long)host::early_input::stats().suggestions,
              (unsigned long long)s.received, (unsigned long long)s.predicted_frames, s.ignored_players);
  }
  stop_early_inputs();
  g_latched_delay = 0;   // the next match picks its delay from its own ping
  trace_end_match();
  {
    // Exactly CEXISlippi::handleReportGame: one report per game with every slot's result.
    UserInfo me = g_user->GetUserInfo();
    report::GameReport r;
    r.uid = me.uid; r.play_key = me.play_key; r.match_id = g_recent_mm_result.id; r.replay = g_report_replay;
    r.online_mode = mode; r.duration_frames = frames; r.game_index = game_index; r.tiebreak_index = tiebreak;
    r.winner_index = winner; r.game_end_method = end_method; r.lras_initiator = lras; r.stage_id = stage;
    for (int i = 0; i < 4; ++i) {
      report::PlayerReport pr;
      for (const auto& player : g_recent_mm_result.players) if (player.port == i + 1) pr.uid = player.uid;
      pr.slot_type = players[i * 9]; pr.stocks_remaining = players[i * 9 + 1];
      uint32_t dmg = be32(players + i * 9 + 2); float dmg_f; std::memcpy(&dmg_f, &dmg, 4); pr.damage_done = dmg_f;
      pr.character_id = info_block[0x60 + 0x24 * i]; pr.color_id = info_block[0x63 + 0x24 * i];
      pr.starting_stocks = info_block[0x62 + 0x24 * i]; pr.starting_percent = be16(info_block + 0x70 + 0x24 * i);
      r.players.push_back(pr);
    }
    // A local two-instance test peering never went through Slippi's matchmaking, so there is no
    // server-side match: reporting it would file a fake game under the signed-in account.
    if (!official_matchmaking()) host::log("slippi: local test session, game report not sent");
    else report::log_game(r);
  }
}

void handle_get_player_settings(std::vector<uint8_t>& q) {
  q.clear();
  std::vector<std::vector<std::string>> by_player(4);
  auto mine = g_user->GetUserChatMessages();
  if (mine.size() == 16) by_player[0] = mine;
  for (auto& p : g_matchmaking->GetPlayerInfo()) if (p.port >= 1 && p.port <= 4) by_player[p.port - 1] = p.chat_messages;
  for (int i = 0; i < 4; ++i) {
    if (by_player[i].size() != 16) by_player[i] = User::GetDefaultChatMessages();
    for (int j = 0; j < 16; ++j) {
      std::string s = convert_string_for_game(by_player[i][j], 25);
      s.resize(51);
      q.insert(q.end(), s.begin(), s.end());
    }
  }
}

}  // namespace

Config& config() { return g_config; }
void set_plain_opponent_handler(void (*handler)(bool plain, const char* why)) { g_plain_opponent_handler = handler; }

void set_local_build(const LocalBuild& build) {
  if (build.mod_view != g_local_build.mod_view || build.fingerprint != g_local_build.fingerprint ||
      build.allow_unverified != g_local_build.allow_unverified || build.extended_content != g_local_build.extended_content ||
      build.static_engine != g_local_build.static_engine)
    host::log("slippi: local build: %s%s%s", build.mod_view ? "mod " : "retail game",
              build.mod_view ? build.name.c_str() : "", build.allow_unverified ? " (Slippi Dolphin opponents allowed)" : "");
  g_local_build = build;
}
const LocalBuild& local_build() { return g_local_build; }
static std::atomic<bool> g_boot_online{false};
void request_boot_online() { g_boot_online = true; }
void set_native_gameplay_profile(NativeGameplayProfile profile) {
  if (session_mode() >= 0) {
    host::log("slippi: cannot change gameplay profile during an online session");
    return;
  }
  g_native_gameplay_profile = profile;
}
uint64_t rollback_count() { return g_rollbacks; }
void note_rollback(int32_t to_frame) { ++g_rollbacks; trace_rollback(to_frame); }
bool early_inputs_enabled() { return host::early_input::enabled(); }
// MELEE_TEST_NO_EARLY_PADS=1 (tests only): this side does not take this build's early pads, as a Slippi
// Dolphin player does not, so the other side sends it Slippi input suggestions instead.
bool early_inputs_accept() {
  static const bool no_early_pads = [] { const char* v = host::test_env::getenv("MELEE_TEST_NO_EARLY_PADS"); return v && *v == '1'; }();
  return host::early_input::enabled() && host::game_image != nullptr && !no_early_pads;
}
// The project's own peer-to-peer session (app/source_p2p.cpp) publishes its state in
// host::netplay::session(). Everything in the host that asks "is a network match running" asks the
// functions below, so when Slippi has no session of its own they answer from that state: offline-only
// features (the player's Gecko codes, 20XX options, skin changes, automatic L-cancel) then stay off
// in a peer-to-peer match exactly as in a Slippi one. With a Slippi session, Slippi's answer stands.
static bool slippi_session_up() {
  return g_in_online_match || g_play_session_active || (g_matchmaking && g_matchmaking->IsSearching());
}
static bool p2p_session_up() { return !slippi_session_up() && host::netplay::session_mode() >= 0; }
// Gecko codes for the match (gecko_session.h), every character select frame: the exchange starts when
// a connection to one opponent is up and its build message came (or did not, within the wait), and
// its messages move both ways; when the codes are agreed, this side's selection goes again with the
// new lock.
void gecko_pump() {
  static const NetplayClient* started = nullptr;
  const bool up = g_netplay && g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED;
  if (!up) {
    if (started) { gecko_session::disconnected(); started = nullptr; }
    return;
  }
  if (started != g_netplay.get()) {
    const auto rb = g_netplay->GetRemoteBuild(0);
    const uint64_t since = g_netplay->ConnectedAtMs();
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!rb.received && (since == 0 || now - since < kBuildWaitMs)) return;   // its build message may still come
    const bool direct = g_last_search.mode == Matchmaking::DIRECT;
    gecko_session::connected(direct && rb.received && rb.gecko && g_netplay->RemoteCount() == 1);
    started = g_netplay.get();
  }
  std::vector<uint8_t> message;
  while (g_netplay->PopGecko(message)) gecko_session::receive(message.data(), message.size());
  for (const auto& out : gecko_session::take_outgoing()) g_netplay->SendGecko(out);
  if (gecko_session::take_resend()) g_netplay->ResendSelections();
}
bool is_online_match() { return g_in_online_match || (p2p_session_up() && host::netplay::is_online_match()); }
int local_player_slot() { return p2p_session_up() ? host::netplay::local_player_slot() : (int)g_local_player_index; }
std::array<std::string, 4> player_names_for_overlay() {
  if (p2p_session_up()) return host::netplay::player_names();
  std::array<std::string, 4> names{};
  if (!g_in_online_match || !g_matchmaking) return names;
  for (int i = 0; i < 4; ++i) names[i] = g_matchmaking->GetPlayerName((uint8_t)i);
  // HUD nicknames stay plain; provider labels belong to the CSS summary only.
  if (g_user && g_local_player_index < 4 && names[g_local_player_index].empty())
    names[g_local_player_index] = g_user->GetUserInfo().display_name;
  return names;
}
std::array<RecordingName, 4> recording_names() {
  std::array<RecordingName, 4> names{};
  if (p2p_session_up() || !g_matchmaking) return names;
  // The same per-port lookup as prepare_online_match_state's names (GetPlayerName) and codes.
  for (const auto& player : g_matchmaking->GetPlayerInfo())
    if (player.port >= 1 && player.port <= 4) names[player.port - 1] = {player.display_name, player.connect_code};
  return names;
}
int remote_ping_count() { return p2p_session_up() || !g_netplay ? 0 : g_netplay->RemoteCount(); }
std::vector<int> remote_pings_ms() {
  std::vector<int> out;
  if (p2p_session_up() || !g_netplay) return out;
  for (int i = 0; i < g_netplay->RemoteCount(); ++i) out.push_back(g_netplay->RemotePingMs(i));
  return out;
}
int ping_ms() { return p2p_session_up() ? host::netplay::ping_ms() : (g_netplay ? g_netplay->LastPingMs() : 0); }

// g_last_search keeps the mode of the last search for the whole session, so it only means anything
// while an online session is actually up: searching, set up (the online character select screen),
// or in the match. Everywhere else this is offline and the answer is -1.
// Port colours (Game > Online, owner 11-22 from a player's request): in an online 1v1 each player shows in
// their first choice; a colour both picked goes to one of them by the match's agreed seed and the other gets
// their second choice; a player with no choice keeps their own port's colour. Both games know both choices
// (the build message), so both draw the same. Display only (mu_port_colour in the game).
uint32_t port_colour_bits() {
  // Whole online session, not only once the match runs: the tags and damage displays take their colour as
  // the match scene builds them, before is_online_match() turns true.
  if (!g_netplay || session_mode() < 0 || bounded_remote_machine_count() != 1) return 0;
  int8_t mine[2], theirs[2];
  if (!g_netplay->PortColours(mine, theirs) || (mine[0] < 0 && theirs[0] < 0)) return 0;
  const int me = local_player_index();
  if (me < 0 || me > 1) return 0;
  const int8_t* pref[2] = {me == 0 ? mine : theirs, me == 0 ? theirs : mine};
  int colour[2] = {-1, -1};
  bool taken[4] = {};
  for (int p = 0; p < 2; ++p)
    if (pref[p][0] < 0) { colour[p] = p; taken[p] = true; }
  const int first = (int)((g_game_facts.rng >> 3) & 1u);
  for (int k = 0; k < 2; ++k) {
    const int p = k == 0 ? first : 1 - first;
    if (colour[p] >= 0) continue;
    for (const int c : {(int)pref[p][0], (int)pref[p][1], p})
      if (c >= 0 && c < 4 && !taken[c]) { colour[p] = c; break; }
    for (int c = 0; colour[p] < 0 && c < 4; ++c) if (!taken[c]) colour[p] = c;
    taken[colour[p]] = true;
  }
  static uint32_t logged = 0xFFFFFFFFu;
  const uint32_t bits = ((uint32_t)colour[0] | ((uint32_t)colour[1] << 2)) << 28;
  if (bits != logged) { logged = bits; host::log("slippi: port colours P1 %d, P2 %d (0 red, 1 blue, 2 yellow, 3 green)", colour[0], colour[1]); }
  return bits;
}

bool opponent_auto_lcancel() {
  if (!g_netplay || g_last_search.mode != Matchmaking::DIRECT) return false;
  if (g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED) return false;
  const int n = std::min<int>(bounded_remote_machine_count(), REMOTE_PLAYER_MAX);
  for (int i = 0; i < n; ++i) if (g_netplay->GetRemoteBuild(i).auto_lcancel) return true;
  return false;
}

void record_report_event(const uint8_t* data, uint32_t size) {
  if (!data || !size) return;
  if (data[0] == 0x35) {
    g_report_replay.reset();
    if (g_netplay && official_matchmaking() && !g_recent_mm_result.id.empty())
      g_report_replay = report::start_replay();
  }
  if (g_report_replay) report::append_replay(*g_report_replay, data, size);
}

int session_mode() {
  const bool session = g_in_online_match || g_play_session_active || (g_matchmaking && g_matchmaking->IsSearching());
  if (!session) return host::netplay::session_mode();   // a peer-to-peer session, or -1
  return (int)g_last_search.mode;
}
bool items_offered() {
  // A Vanilla run plays the retail rules only; a peer-to-peer session (--p2p-*, its own protocol) has
  // no items rule; Ranked and Unranked never have items.
  if (host::gameplay::vanilla_only() || host::options.vanilla_game || p2p_session_up()) return false;
  const int mode = session_mode();
  return mode < 0 || mode == Matchmaking::DIRECT || mode == Matchmaking::TEAMS;
}
bool items_choice() {
  const bool saved = config().items.load();
  if (items_offered()) return saved;
  static std::atomic<bool> told{false};
  if (saved && !told.exchange(true))
    host::log("online options: Items is saved on but not offered in this session (a Vanilla run, peer-to-peer, or "
              "not Direct or Teams): Off");
  return false;
}
ItemsOption items_option() {
  ItemsOption o;
  if (!items_offered()) {   // the row is not shown and the choice in effect is Off (items_choice)
    o.status = ITEMS_OTHER_MODE;
    return o;
  }
  o.choice = items_choice();
  if (!g_netplay || g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED) return o;
  o.locked = g_netplay->LocalLocked();
  const NetplayClient::ItemsVerdict v = g_netplay->Items();
  o.blocker = v.blocker;
  o.status = v.unsupported ? ITEMS_UNSUPPORTED : v.on ? ITEMS_ON : v.blocker >= 0 ? ITEMS_OFF_THEIRS : ITEMS_OFF_MINE;
  return o;
}

bool set_items_choice(bool on) {
  const ItemsOption now = items_option();
  if (now.choice == on) return true;
  if (now.locked || now.status == ITEMS_UNSUPPORTED || now.status == ITEMS_OTHER_MODE) {
    host::log("online options: Items %s refused (%s)", on ? "on" : "off",
              now.locked ? "locked in" : now.status == ITEMS_UNSUPPORTED ? "a player's game has no items rule" : "not offered here");
    return false;
  }
  config().items.store(on);
  host::log("online options: Items %s (from the character select)", on ? "on" : "off");
  if (g_netplay && g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED) g_netplay->SendItemsChoice();
  return true;
}

std::vector<uint8_t> squad_match_setup() {
  std::lock_guard<std::mutex> lk(g_squad_match_mutex);
  return g_squad_match;
}

bool squad_offered() {
#if MU_SQUAD
  // As items_offered: a Vanilla run plays the retail rules only and a peer-to-peer session has no squad
  // rule; Squad Strike is Direct only (never Teams, Ranked or Unranked).
  if (host::gameplay::vanilla_only() || host::options.vanilla_game || p2p_session_up()) return false;
  const int mode = session_mode();
  return mode < 0 || mode == Matchmaking::DIRECT;
#else
  return false;
#endif
}
bool squad_choice() {
  const bool saved = config().squad.load();
  if (squad_offered()) return saved;
  static std::atomic<bool> told{false};
  if (saved && !told.exchange(true))
    host::log("online options: Squad Strike is saved on but not offered in this session (a Vanilla run, peer-to-peer, "
              "not Direct, or a build without it): Off");
  return false;
}

ItemsOption squad_option() {
  ItemsOption o;
#if MU_SQUAD
  if (!squad_offered()) {   // the row is not shown and the choice in effect is Off (squad_choice)
    o.status = ITEMS_OTHER_MODE;
    return o;
  }
  o.choice = squad_choice();
  if (!g_netplay || g_netplay->GetSlippiConnectStatus() != NetplayClient::ConnectStatus::CONNECTED) return o;
  o.locked = g_netplay->LocalLocked();
  const NetplayClient::SquadVerdict v = g_netplay->Squad();
  o.blocker = v.blocker;
  o.status = v.unsupported ? ITEMS_UNSUPPORTED : v.on ? ITEMS_ON : v.blocker >= 0 ? ITEMS_OFF_THEIRS : ITEMS_OFF_MINE;
#else
  o.status = ITEMS_OTHER_MODE;
#endif
  return o;
}

bool set_squad_choice(bool on) {
  const ItemsOption now = squad_option();
  if (now.choice == on) return true;
  if (now.locked || now.status == ITEMS_UNSUPPORTED || now.status == ITEMS_OTHER_MODE) {
    host::log("online options: Squad Strike %s refused (%s)", on ? "on" : "off",
              now.locked ? "locked in" : now.status == ITEMS_UNSUPPORTED ? "a player's game has no Squad Strike option" : "not offered here");
    return false;
  }
  config().squad.store(on);
  host::log("online options: Squad Strike %s (from the character select)", on ? "on" : "off");
  if (g_netplay && g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED) g_netplay->SendSquad();
  return true;
}

// This player's lineup from the online select (set_squad_lineup), {kind, color} pairs.
static std::mutex g_squad_lineup_mutex;
static uint8_t g_squad_lineup[8] = {}, g_squad_lineup_count = 0;

void set_squad_lineup(const uint8_t* pairs, uint8_t count) {
  bool changed;
  {
    std::lock_guard<std::mutex> lk(g_squad_lineup_mutex);
    uint8_t next[8] = {};
    if (count > 4) count = 0;
    if (pairs && count) std::memcpy(next, pairs, 2u * count);
    changed = count != g_squad_lineup_count || std::memcmp(next, g_squad_lineup, sizeof next) != 0;
    g_squad_lineup_count = count;
    std::memcpy(g_squad_lineup, next, sizeof next);
  }
  if (changed && g_netplay && g_netplay->GetSlippiConnectStatus() == NetplayClient::ConnectStatus::CONNECTED)
    g_netplay->SendSquad();
}

uint8_t squad_local_lineup(uint8_t pairs[8]) {
#if MU_SQUAD
  // Tests only (local peering): MELEE_TEST_SQUAD_LINEUP="kind/color,..." is this player's lineup instead
  // of the select's (the harness has no select), sent to the peer like a picked one.
  if (Matchmaking::local_peer.enabled) {
    if (const char* forced = host::test_env::automated_getenv("MELEE_TEST_SQUAD_LINEUP")) {
      MuSquadSide a{}, b{};
      const std::string both = std::string(forced) + ";0/0";
      if (squad_test::parse_direct(both.c_str(), &a, &b)) {
        for (unsigned i = 0; i < a.count; ++i) pairs[2 * i] = a.members[i].kind, pairs[2 * i + 1] = a.members[i].color;
        return a.count;
      }
    }
  }
#endif
  std::lock_guard<std::mutex> lk(g_squad_lineup_mutex);
  std::memcpy(pairs, g_squad_lineup, sizeof g_squad_lineup);
  return g_squad_lineup_count;
}

uint32_t remote_shield_color(int slot) { return g_netplay ? g_netplay->RemoteShieldColor(slot) : 0u; }
int local_player_index() { return p2p_session_up() ? host::netplay::local_player_slot() : (int)g_local_player_index; }
bool online_status_visible() {
  return !is_online_match() && (g_online_menu_visible.load() || in_online_menus());
}
bool in_online_menus() {
  if (p2p_session_up()) return host::netplay::in_online_menus();
  const uint32_t now = host::retrace_count();
  return g_last_match_state_retrace && now - g_last_match_state_retrace < 10;
}

bool native_start_match(int mode, const std::string& connect_code, uint8_t character,
                        uint8_t color, std::string* error) {
  if (!g_user) init();
  if (session_mode() >= 0) {
    if (error) *error = "An online session is already active";
    return false;
  }
  if (const char* reason = local_matchmaking_error(mode)) {
    if (error) *error = reason;
    return false;
  }

  // Same payloads the guest sends from the stock Slippi online menus: a random legal stage, and the
  // launcher's or harness's character and costume into the normal online flow.
  uint8_t selections[9] = {0, character, color, 1, 0, 0, 3, (uint8_t)mode, 0};
  set_match_selections(selections);
  uint8_t find[19] = {};
  find[0] = (uint8_t)mode;
  const std::string sjis = utf8_to_shiftjis(connect_code);
  std::memcpy(find + 1, sjis.data(), std::min<size_t>(18, sjis.size()));
  start_find_match(find);
  if (!g_forced_error.empty()) {
    if (error) *error = g_forced_error;
    return false;
  }
  if (error) error->clear();
  return g_matchmaking && g_matchmaking->IsSearching();
}

GameFacts last_game_facts() { return g_game_facts; }
unsigned disconnected_ports() { return g_gone_ports; }

bool native_ranked_next_game(uint8_t character, uint8_t color) {
  if (!g_user || !g_matchmaking || !g_netplay || g_last_search.mode != Matchmaking::RANKED) return false;
  if (g_matchmaking->GetMatchmakeState() != Matchmaking::CONNECTION_SUCCESS || g_local_selections.is_character_selected) return false;
  uint8_t selections[9] = {0, character, color, 1, 0, 0, 3, (uint8_t)Matchmaking::RANKED, 0};   // a random legal stage
  set_match_selections(selections);
  return true;
}

bool local_identity(std::string* code, std::string* name) {
  if (!g_user) init();
  if (!g_user || !g_user->IsLoggedIn()) return false;
  const UserInfo info = g_user->GetUserInfo();
  if (code) *code = info.connect_code;
  if (name) *name = info.display_name.empty() ? info.connect_code : info.display_name;
  return !info.connect_code.empty();
}

void init() {
  report::init(host::options.iso, g_config.user_dir);
  g_user = std::make_unique<User>(g_config.user_dir);
  g_rank_account.clear();
  update_rank_account();
  g_matchmaking = std::make_unique<Matchmaking>(g_user.get());
  std::error_code ec;
  std::filesystem::create_directories(g_config.user_dir, ec);
  g_direct_codes = std::make_unique<DirectCodes>(g_config.user_dir + "/direct-codes.json");
  g_teams_codes = std::make_unique<DirectCodes>(g_config.user_dir + "/teams-codes.json");
  g_local_selections.Reset();
  Savestate::force_init = true;
  if (!g_user->IsLoggedIn()) host::log("slippi: not logged in; sign in with Slippi Launcher, then choose Log In in the game");
  update_discord_presence(true);   // the connect code is only known once user.json has been read
}

void flush_cmd_trace_at_exit();
void shutdown() {
  flush_cmd_trace_at_exit();
  host::cosmetics::thaw_after_online_session();
  if (g_user && !Matchmaking::local_peer.enabled && g_matchmaking && g_matchmaking->GetMatchmakeState() == Matchmaking::CONNECTION_SUCCESS) {
    const auto me = g_user->GetUserInfo();
    if (official_matchmaking() && !g_ranked_completed) report::match_status(me.uid, me.play_key, g_recent_mm_result.id, "abandoned", true);
  }
  report::shutdown();
  g_netplay.reset();
  g_matchmaking.reset();
  g_active_savestates.clear();
  g_available_savestates.clear();
  host::discord::clear();   // no stale "In a match" left on the profile
}

// Live pick previews (mu_pick_preview.h), private native ABI. Display only: nothing here reaches
// match setup. Without a connected netplay client a send is dropped and a read answers no slots.
static bool pick_preview_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& q) {
  if (!command_payload_valid(MU_PICK_PREVIEW_COMMAND, p, n)) return false;
  if (p[0] == MU_PICK_PREVIEW_OP_SEND) {
    MuPickPreview pick{};
    if (!mu_pick_preview_decode(p + 1, n - 1, &pick)) return false;
    if (g_netplay) g_netplay->SendPickPreview(pick, host::retrace_count());
    return true;
  }
  MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES] = {};
  for (int i = 0; g_netplay && i < MU_PICK_PREVIEW_REMOTES; ++i)
    slots[i].valid = g_netplay->RemotePickPreview(i, &slots[i].pick, &slots[i].port);
  q.resize(MU_PICK_PREVIEW_REPLY);
  return mu_pick_preview_reply_encode(slots, q.data(), q.size()) == MU_PICK_PREVIEW_REPLY;
}

bool valid_command_payload(uint8_t cmd, const uint8_t* payload, uint32_t payload_len) {
  return command_payload_valid(cmd, payload, payload_len);
}

namespace {
// MELEE_TRACE_SLIPPI_CMDS=1: one line per menu/online command, shared by both engines, so a
// static-recomp run and a native run can be compared with tools/slippi_trace_diff.py. Runs of
// identical (command, payload, reply) lines, like B3 every frame, collapse into one with a count.
bool trace_slippi_cmds() {
  static const bool on = [] { const char* v = host::test_env::getenv("MELEE_TRACE_SLIPPI_CMDS"); return v && v[0] == '1'; }();
  return on;
}
struct CmdTrace { std::string key; uint32_t count = 0, first_retrace = 0; uint64_t seq = 0; };
CmdTrace g_cmd_trace;
uint64_t g_cmd_seq = 0;
void flush_cmd_trace() {
  if (g_cmd_trace.count > 1)
    host::log("slippi-cmd: seq %llu retrace %u %s (x%u)", (unsigned long long)g_cmd_trace.seq,
              g_cmd_trace.first_retrace, g_cmd_trace.key.c_str(), g_cmd_trace.count);
}
void trace_cmd(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, bool ok, const std::vector<uint8_t>& q) {
  char buf[16];
  std::string key;
  std::snprintf(buf, sizeof buf, "cmd %02X len %u", cmd, payload_len); key += buf;
  key += " payload ";
  const uint32_t shown = payload ? std::min<uint32_t>(payload_len, 64) : 0;
  for (uint32_t i = 0; i < shown; ++i) { std::snprintf(buf, sizeof buf, "%02X", payload[i]); key += buf; }
  if (shown < payload_len) key += "..";
  uint32_t h = 2166136261u;
  for (uint8_t b : q) { h ^= b; h *= 16777619u; }
  char tail[64];
  std::snprintf(tail, sizeof tail, " reply %u fnv %08X%s", ok ? (uint32_t)q.size() : 0u, ok ? h : 0u, ok ? "" : " unhandled");
  key += tail;
  ++g_cmd_seq;
  if (g_cmd_trace.count && key == g_cmd_trace.key) { ++g_cmd_trace.count; return; }
  flush_cmd_trace();
  g_cmd_trace = {key, 1, host::retrace_count(), g_cmd_seq};
  host::log("slippi-cmd: seq %llu retrace %u %s", (unsigned long long)g_cmd_seq, g_cmd_trace.first_retrace, key.c_str());
}
}  // namespace

void flush_cmd_trace_at_exit() { flush_cmd_trace(); g_cmd_trace = {}; }
bool handle_command(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q);

bool handle(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q) {
  if (!trace_slippi_cmds()) return handle_command(cmd, payload, payload_len, q);
  // The replay and match streams (B0-B2) run every frame and are not menu traffic; nor are the
  // per-frame pick previews, which the static recomp never sends.
  if (cmd == CMD_ONLINE_INPUTS || cmd == CMD_CAPTURE_SAVESTATE || cmd == CMD_LOAD_SAVESTATE ||
      cmd == MU_PICK_PREVIEW_COMMAND)
    return handle_command(cmd, payload, payload_len, q);
  // The static recomp passes its EXI read queue, which still holds the previous reply for commands
  // that answer nothing; the native caller passes an empty vector. Trace what this command wrote.
  std::vector<uint8_t> reply;
  const bool ok = handle_command(cmd, payload, payload_len, reply);
  trace_cmd(cmd, payload, payload_len, ok, reply);
  if (!reply.empty()) q.swap(reply);
  return ok;
}

bool handle_command(uint8_t cmd, const uint8_t* payload, uint32_t payload_len, std::vector<uint8_t>& q) {
  if (cmd == MU_PICK_PREVIEW_COMMAND) return pick_preview_command(payload, payload_len, q);
  if (!valid_command_payload(cmd, payload, payload_len)) {
    host::log("slippi: invalid payload for command %02X (%u bytes)", cmd, payload_len);
    return false;
  }
  if (!g_user) init();
  consume_discord_join();   // one atomic load unless a friend's Discord invite is actually waiting
  switch (cmd) {
    case CMD_ONLINE_INPUTS: handle_online_inputs(payload, payload_len, q); return true;
    case CMD_CAPTURE_SAVESTATE: handle_capture_savestate(payload); return true;
    case CMD_LOAD_SAVESTATE: handle_load_savestate(payload); return true;
    case CMD_GET_MATCH_STATE: prepare_online_match_state(q); return true;
    case CMD_FIND_OPPONENT:
      start_find_match(payload); return true;
    case CMD_SET_MATCH_SELECTIONS: set_match_selections(payload); return true;
    case CMD_OPEN_LOGIN:
      if (!g_user->AttemptLogin(true)) host::log("slippi: login requested: sign in with Slippi Launcher, then choose Log In again");
      return true;
    case CMD_LOGOUT: g_user->LogOut(); update_rank_account(); return true;
    case CMD_UPDATE: host::log("slippi: update requested by the game (ignored)"); return true;
    case CMD_GET_ONLINE_STATUS: prepare_online_status(q); return true;
    case CMD_CLEANUP_CONNECTION:
      cleanup_connection(Leave::Quit); return true;
    case CMD_SEND_CHAT_MESSAGE: if (chat_enabled() && g_netplay) g_netplay->SendChatMessage(payload[0]); return true;
    case CMD_GET_NEW_SEED: q.clear(); append_u32(q, g_rng() % 0xFFFFFFFFu); return true;
    case CMD_REPORT_GAME: handle_report_game(payload); return true;
    case CMD_FETCH_CODE_SUGGESTION: handle_name_entry_load(payload, q); return true;
    case CMD_OVERWRITE_SELECTIONS: {
      g_overwrite_selections.clear();
      uint16_t stage = be16(payload);
      for (int i = 0; i < 4; ++i) {
        const uint8_t* c = payload + 2 + i * 3;
        if (!c[0]) continue;
        PlayerSelections s;
        s.is_character_selected = true; s.character_id = c[1]; s.character_color = c[2];
        s.is_stage_selected = true; s.stage_id = stage; s.player_idx = (uint8_t)i;
        g_overwrite_selections.push_back(s);
      }
      return true;
    }
    case CMD_GP_COMPLETE_STEP:
      if (g_netplay) {
        GamePrepStep step{};
        std::copy_n(payload, step.size(), step.begin());
        g_netplay->SendGamePrepStep(step);
      }
      return true;
    case CMD_GP_FETCH_STEP: {
      q.assign(6, 0);
      if (g_netplay) {
        const auto reply = g_netplay->FetchGamePrepStep(payload[0]);
        q.assign(reply.begin(), reply.end());
      }
      return true;
    }
    case CMD_REPORT_SET_COMPLETE:
    case CMD_REPORT_MATCH_STATUS_UPDATE: {
      if (!official_matchmaking() || g_recent_mm_result.id.find("mode.ranked") == std::string::npos || g_ranked_completed) return true;
      const char* status = cmd == CMD_REPORT_SET_COMPLETE ? (payload[0] == 0 ? "normal_completion" : "abnormal_completion") : ranked_status(payload[0]);
      if (status) {
        const auto me = g_user->GetUserInfo();
        report::match_status(me.uid, me.play_key, g_recent_mm_result.id, status, true);
        g_ranked_completed = std::strcmp(status, "normal_completion") == 0 || std::strcmp(status, "abnormal_completion") == 0 || std::strcmp(status, "abandoned") == 0;
      }
      return true;
    }
    case CMD_GET_PLAYER_SETTINGS: handle_get_player_settings(q); return true;
    // Offline input now passes through the same host queue on both engines.
    // Disable the console's second offline queue so inputs are delayed once.
    case CMD_GET_DELAY: q.clear(); q.push_back(session_mode() >= 0 ? 1 : 0);
      q.push_back(session_mode() >= 0 ? (uint8_t)effective_delay() : 0); return true;
    case CMD_NATIVE_MENU_FLAGS:
      if (payload_len) return false; // Replay control shares this byte with a five-byte payload.
      q.assign(1, (host::test_env::agent_copy() ? 8 : 0) | (Matchmaking::local_peer.enabled ? 16 : 0));
      return true;
    case CMD_GET_RANK: {
      q.clear();
      report::RankInfo rank;
      q.push_back(3);
      q.push_back(static_cast<uint8_t>(report::rank_info(&rank)));
      q.push_back(static_cast<uint8_t>(rank.rank));
      uint32_t ordinal, change;
      std::memcpy(&ordinal, &rank.rating_ordinal, sizeof ordinal);
      std::memcpy(&change, &rank.rating_change, sizeof change);
      append_u32(q, ordinal); append_u32(q, rank.rating_update_count); append_u32(q, change);
      q.push_back(static_cast<uint8_t>(rank.rank_change));
      return true;
    }
    case CMD_FETCH_RANK: {
      if (official_matchmaking()) {
        const auto me = g_user->GetUserInfo();
        report::fetch_match_result(g_recent_mm_result.id, me.uid, me.play_key);
      }
      return true;
    }
    case CMD_GET_RANK_VISIBILITY: q.assign(1, 3); return true;
    default: return false;
  }
}

}  // namespace slippi::online
