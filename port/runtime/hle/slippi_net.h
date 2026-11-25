// Slippi Online networking: SFML-compatible packets, the ENet netplay client, the matchmaking
// client and the user record. Ports of Dolphin's SlippiNetplay/SlippiMatchmaking/SlippiUser
// with the same wire formats, so the port can play against stock Slippi clients.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "slippi_game_prep.h"
#include "slippi_mm_service.h"
#include "mu_content_wire.h"   // custom fighters online: the list of fighters this side sends
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "early_input_rules.h"
#include "../abi/mu_setup_hash.h"
#include "../abi/mu_pick_preview.h"
#include "../abi/mu_build_capabilities.h"

struct _ENetHost;
struct _ENetPeer;


namespace slippi {

constexpr int ROLLBACK_MAX_FRAMES = 7;
// How long a peer connection may take (NetplayClient::ThreadFunc), and how long a match setup
// waits for every peer's setup hash before that peer counts as disconnected.
constexpr uint64_t kConnectTimeoutMs = 8000;
constexpr int ONLINE_LOCKSTEP_INTERVAL = 30;
constexpr int REMOTE_PLAYER_MAX = 3;
constexpr int PLAYER_COUNT_MAX = REMOTE_PLAYER_MAX + 1;
constexpr int PAD_FULL_SIZE = 0xC;
constexpr int PAD_DATA_SIZE = 0x8;   // buttons, sticks and triggers
// What goes over the network for each frame (Slippi 3.7.0): the inputs and one byte of RNG bits.
constexpr int PAD_WIRE_SIZE = PAD_DATA_SIZE + 1;
// Player drops (Slippi 3.7.0): how long the other players' reports about a dropped player are awaited,
// and how many frames after the dropped player's last input the fighter despawns (2 * 7 + 2).
constexpr uint64_t DROP_REPORT_WAIT_MS = 300;
constexpr int32_t DROP_DESPAWN_FRAMES = 16;
constexpr uint8_t CHAT_MSG_CHAT_DISABLED = 0x10;
extern const char* const SLIPPI_SEMVER;   // netplay version this port speaks
// Whether the tests' matchmaking stand-in (MELEE_TEST_MM_HOST) answers in place of Slippi's servers.
bool matchmaking_stand_in();

enum NetMsg : uint8_t {
  NP_MSG_SLIPPI_PAD = 0x80, NP_MSG_SLIPPI_PAD_ACK = 0x81, NP_MSG_SLIPPI_MATCH_SELECTIONS = 0x82, NP_MSG_SLIPPI_CONN_SELECTED = 0x83,
  NP_MSG_SLIPPI_CHAT_MESSAGE = 0x84, NP_MSG_SLIPPI_COMPLETE_STEP = 0x85, NP_MSG_SLIPPI_SYNCED_STATE = 0x86,
  // Slippi 3.7.0: a mid-frame input hint (unreliable), and what a client has of a dropped player's inputs.
  NP_MSG_SLIPPI_PAD_SUGGESTION = 0x87, NP_MSG_SLIPPI_PLAYER_DROP = 0x88,
  // Melee Unlocked only: the sender's build (retail or a mod's identity). Slippi Dolphin logs an
  // unknown id and ignores it, so it is safe to send to anyone.
  NP_MSG_MU_BUILD = 0xE0,
  // Melee Unlocked only: an early input (early_input.h), sent only to a peer whose build message
  // said it takes them. A peer without that word (Slippi Dolphin, an older build) never gets one.
  NP_MSG_MU_EARLY_PAD = 0xE1,
  // Melee Unlocked only: a ping before the match (request or reply), sent only to a peer whose build
  // message said it answers them, so auto delay has a ping when the match is set up.
  NP_MSG_MU_PING = 0xE2,
  NP_MSG_MU_LEGACY_ARENA = 0xE3,
  // Melee Unlocked only: the hash of the match setup this side built (mu_setup_hash.h). Sent to
  // every peer; only a peer whose build message carried MU_SETUP_HASH_CAP is waited for.
  NP_MSG_MU_SETUP_HASH = 0xE6,
  // Melee Unlocked only: the sender's live pick on the character screen (mu_pick_preview.h), sent
  // only to a peer whose build message carried MU_PICK_PREVIEW_CAP. Display only: the match is
  // still set up from 0x82 (and 0xE3).
  NP_MSG_MU_PICK_PREVIEW = MU_PICK_PREVIEW_MSG,
  // Reserved, never to be reused: Melee Unlocked's arena room traffic (MU_BUILD_CAPS2_ARENA;
  // upstream's 0xE3 and capability bit 0x10). This build has no arena rooms: it neither sends
  // nor advertises them, and drops one that arrives.
  NP_MSG_MU_ARENA = 0xE7,
  // Melee Unlocked only: Gecko codes offered, answered and agreed for the match (gecko_online.h),
  // sent only to a peer whose build message said it takes them (MU_BUILD_CAPS2_GECKO). Upstream
  // gives it this build's pick preview id and capability bit 0x40 (the setup hash here), so it
  // rides 0xE8 and the second capability byte here.
  NP_MSG_MU_GECKO = 0xE8,
  // Reserved, never to be reused: the warm-up room's message (a player warming up in Training while
  // staying connected; caps3 bit 0x10, and 0xE5 with first capability bit 0x80 from Melee Unlocked
  // 0.9.215 and 0.9.216, NP_MSG_MU_WARMUP_UPSTREAM). This build has no warm-up room: it neither
  // sends nor advertises it, and drops one that arrives.
  NP_MSG_MU_WARMUP = 0xE9,
  // Melee Unlocked only: the skin the sender's player wears for the pick that follows (mu_skin_wire.h,
  // host/opponent_skins.h), sent just before each 0x82 and only to a peer whose build message set
  // MU_BUILD_CAPS3_SKINS. Cosmetic: never in the setup hash, the layer fingerprint or the mod view.
  NP_MSG_MU_SKINS = 0xEA,
  // Nucleus only: the sender's Items choice (Direct and Teams, slippi_online.h items_option): 'M' 'U'
  // 'I', version 1, sender port, choice (0 off, 1 on). Sent only to a peer whose build message set
  // MU_BUILD_CAPS4_ITEMS: once its build arrives, on each change, and just before each 0x82, so a
  // peer always has the choice that goes with a lock-in. Items are on only when every player sent 1.
  NP_MSG_MU_ITEMS = 0xEB,
  // Nucleus only: custom fighters online (mu_content_wire.h, host/online_fighters.h, docs/online-custom-
  // content.md). The added fighters the sender has installed, by content hash with its own id for each.
  // Sent only to a peer whose build message set MU_BUILD_CAPS4_CUSTOM_FIGHTERS.
  NP_MSG_MU_CONTENT = 0xEC,
  // Nucleus only: the sender's Squad Strike choice and lineup (Direct, slippi_online.h squad_option):
  // 'M' 'U' 'Q', version 1, sender port, choice (0 off, 1 on), count (0-4), count * {kind, color} in
  // fighting order (mu_squad_wire.h mu_squad_lineup_valid; 0: none picked yet). Sent only to a peer whose
  // build message set MU_BUILD_CAPS4_SQUAD: once its build arrives, on each change, and just before each
  // 0x82, so a peer always has the lineup that goes with a lock-in. Squads are on only when every player
  // sent choice 1; the match then needs both lineups (the setup hash covers them).
  NP_MSG_MU_SQUAD = 0xED,
};
static_assert(NP_MSG_MU_SETUP_HASH == MU_SETUP_HASH_MSG, "one setup hash id");
// The warm-up message's id on the wire toward (and from) Melee Unlocked 0.9.215 and 0.9.216, which have
// no third capability byte. Only interpreted as warm-up after negotiating that layout.
constexpr uint8_t NP_MSG_MU_WARMUP_UPSTREAM = 0xE5;
// MU_BUILD third capability byte (this build on): 0x01 Squad Strike, 0x02 its friend rooms, 0x04 pick
// previews, 0x08 setup hash, 0x10 warm-up messages (NP_MSG_MU_WARMUP, 0xE9; reserved, no longer set).
constexpr uint8_t MU_BUILD_CAPS3_WARMUP = 0x10;
// 0x20: this side decides Direct pausing from every player's 0x40 (1.0.1 on); 0x40: this player turned
// pausing off. A pause rule only one side applies would split the game at the first Start press, so
// pausing goes off only when every peer sends 0x20 (and the setup hash covers the pause bit anyway).
constexpr uint8_t MU_BUILD_CAPS3_PAUSE_RULE = 0x20;
constexpr uint8_t MU_BUILD_CAPS3_NO_PAUSE = 0x40;
// 0x80: this side takes skins messages (NP_MSG_MU_SKINS); set only while the "opponentskins" setting is on.
constexpr uint8_t MU_BUILD_CAPS3_SKINS = 0x80;
// MU_BUILD fourth capability byte ("caps4"), after the product and its version (Nucleus 1.0.6 on; older
// builds stop reading at the version). One byte, every bit set in SendBuild and read in OnData:
// 0x01 this side decides Direct and Teams items from every player's NP_MSG_MU_ITEMS choice. Absent
// (Slippi Dolphin, Melee Unlocked, older Nucleus): that player's game has no such rule, so items stay off.
constexpr uint8_t MU_BUILD_CAPS4_ITEMS = 0x01;
// 0x02 this side takes custom fighters online (NP_MSG_MU_CONTENT); 0x04 reserved for custom stages.
constexpr uint8_t MU_BUILD_CAPS4_CUSTOM_FIGHTERS = 0x02;
constexpr uint8_t MU_BUILD_CAPS4_CUSTOM_STAGES = 0x04;
// 0x08: this side plays Squad Strike in Direct from every player's NP_MSG_MU_SQUAD (choice and lineup;
// builds with Squad Strike only). Absent: that player's game has no such option, so a Direct match
// there is a normal one.
constexpr uint8_t MU_BUILD_CAPS4_SQUAD = 0x08;
// First capability byte bit Melee Unlocked 0.9.215 and 0.9.216 read as warm-up (their 0xE5 messages).
constexpr uint8_t MU_BUILD_CAPS_UPSTREAM_WARMUP = 0x80;

// sf::Packet wire format: integers big-endian, bool as one byte, strings as u32 length + bytes.
class Packet {
 public:
  Packet() = default;
  Packet(const uint8_t* data, size_t size) : buf_(data, data + size) {}
  void append(const void* data, size_t size);
  const uint8_t* data() const { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  Packet& operator<<(uint8_t v);
  Packet& operator<<(bool v) { return *this << (uint8_t)(v ? 1 : 0); }
  Packet& operator<<(uint16_t v);
  Packet& operator<<(uint32_t v);
  Packet& operator<<(int32_t v) { return *this << (uint32_t)v; }
  Packet& operator<<(const std::string& s);
  Packet& operator>>(uint8_t& v);
  Packet& operator>>(bool& v);
  Packet& operator>>(uint16_t& v);
  Packet& operator>>(uint32_t& v);
  Packet& operator>>(int32_t& v);
  Packet& operator>>(std::string& s);
 private:
  bool check(size_t n);
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool ok_ = true;
};

struct Pad {
  int32_t frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t buf[PAD_FULL_SIZE] = {};
  explicit Pad(int32_t f) : frame(f) {}
  Pad(int32_t f, const uint8_t* data) : frame(f) { for (int i = 0; i < PAD_WIRE_SIZE; ++i) buf[i] = data[i]; }
  Pad(int32_t f, int32_t cf, uint32_t c, const uint8_t* data) : Pad(f, data) { checksum_frame = cf; checksum = c; }
};

struct SyncedFighterState { uint8_t stocks_remaining = 4; uint16_t current_health = 0; };
struct SyncedGameState { std::string match_id; uint32_t game_index = 0, tiebreak_index = 0, seconds_remaining = 480; SyncedFighterState fighters[4]; };
struct DesyncRecoveryResp { bool is_recovering = false, is_waiting = false, is_error = false; SyncedGameState state; };

struct RemotePadOutput {
  int32_t latest_frame = 0, checksum_frame = 0;
  uint32_t checksum = 0;
  uint8_t player_idx = 0;
  bool is_disconnected = false;
  int32_t despawn_frame = 0;   // the frame a dropped player's fighter despawns on, 0 until settled
  std::vector<uint8_t> data;
};


struct PlayerSelections {
  uint8_t player_idx = 0, character_id = 0, character_color = 0, team_id = 0;
  bool is_character_selected = false;
  uint16_t stage_id = 0;
  bool is_stage_selected = false;
  uint8_t alt_stage_mode = 0;
  uint32_t rng_offset = 0;
  int message_id = 0;
  bool error = false;
  void Merge(const PlayerSelections& s) {
    rng_offset = s.rng_offset;
    if (s.is_stage_selected) { stage_id = s.stage_id; is_stage_selected = true; alt_stage_mode = s.alt_stage_mode; }
    if (s.is_character_selected) { character_id = s.character_id; character_color = s.character_color; team_id = s.team_id; is_character_selected = true; }
  }
  void Reset() { character_id = character_color = team_id = 0; is_character_selected = false; stage_id = 0; is_stage_selected = false; rng_offset = 0; }
};

struct MatchInfo {
  PlayerSelections local;
  PlayerSelections remote[REMOTE_PLAYER_MAX];
  void Reset() { local.Reset(); for (auto& r : remote) r.Reset(); }
};

struct UserInfo {
  std::string uid, play_key, display_name, connect_code, latest_version;
  int port = 0;
  std::vector<std::string> chat_messages;
  bool is_bot = false;
  // What the matchmaking service says of the player's rank (Slippi 3.7.0 records it in the replay).
  float ranked_rating = 0, ranked_mmr = 0;
  int ranked_update_count = 0, ranked_global_placement = 0, ranked_regional_placement = 0;
};

// The user record written by the Slippi Launcher (user.json in the Slippi user folder).
class User {
 public:
  explicit User(std::string user_dir);
  bool AttemptLogin(bool rediscover = false);  // Log In rechecks shared launcher folders too.
  bool IsLoggedIn() const { return logged_in_; }
  UserInfo GetUserInfo() const { std::lock_guard<std::mutex> lock(mutex_); return info_; }
  void LogOut();
  void OverwriteLatestVersion(const std::string& v) { std::lock_guard<std::mutex> lock(mutex_); info_.latest_version = v; }
  std::vector<std::string> GetUserChatMessages() const;
  static std::vector<std::string> GetDefaultChatMessages();
  const std::string& dir() const { return dir_; }
  // Refreshes display name, connect code, latest version and chat messages from Slippi's user
  // API on a background thread. user.json is only rewritten by the Slippi Launcher at login, so a
  // name changed on the website since then is stale in it; the server's copy is what the opponent
  // and the in-game name tag already show.
  void RefreshFromServer();
  // Fetch the current server version, with the existing offline-account fallback.
  using VersionFetch = bool (*)(const std::string&, std::string*, int*);
  bool CheckOnlineCompatibility(std::string* error, VersionFetch fetch = nullptr);
  // One verification shared by a search, its peer connection and first game. A reset starts
  // fresh verification for the next search/rematch, including after a cancelled request.
  bool CheckSessionCompatibility(std::string* error, VersionFetch fetch = nullptr);
  // Game thread only: 0 checking, 1 approved, -1 refused. Also covers rematches which reuse
  // a peer connection. Checking never blocks the character-select/render thread.
  int PollOnlineCompatibility(std::string* error, VersionFetch fetch = nullptr);
  void ResetOnlineCompatibility() { ++compatibility_generation_; compatibility_done_ = false; }
 private:
  std::string dir_;
  std::string account_file_;
  bool account_discovered_ = false;
  UserInfo info_;
  bool logged_in_ = false;
  mutable std::mutex mutex_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  bool compatibility_done_ = false, compatibility_ok_ = false;
  std::atomic<uint64_t> compatibility_generation_{0};
  std::string compatibility_error_;
  std::mutex compatibility_request_mutex_;
  uint64_t verified_generation_ = ~uint64_t{0};
  bool verified_ok_ = false;
  std::string verified_error_;
  bool CheckSessionCompatibilityAt(uint64_t generation, std::string* error, VersionFetch fetch);
  // Destroyed before the fields used by their workers: no detached access to a destroyed User.
  std::future<void> refresh_;
  struct VersionResult { bool ok; std::string error; uint64_t generation; };
  std::future<VersionResult> compatibility_;
};

// Connect-code history (direct-codes.json / teams-codes.json in the user folder).
class DirectCodes {
 public:
  DirectCodes(std::string path);
  std::string get(int index) const;
  int length() const { return (int)codes_.size(); }
  void AddOrUpdateCode(const std::string& code);
 private:
  void Load();
  void Save();
  std::string path_;
  std::vector<std::string> codes_;
};

class NetplayClient {
 public:
  enum class ConnectStatus { UNSET, INITIATED, CONNECTED, FAILED, DISCONNECTED };
  enum class DisconnectReason : uint32_t { UNSPECIFIED = 0, POOR_PERFORMANCE = 1
  };

  NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                bool is_decider, uint8_t player_idx, std::vector<std::string> public_fallbacks = {});
  ~NetplayClient();

  bool IsDecider() const { return is_decider_; }
  uint8_t LocalPlayerPort() const { return player_idx_; }
  ConnectStatus GetSlippiConnectStatus() const { return status_.load(std::memory_order_acquire); }
  std::vector<int> GetFailedConnections() const { return failed_connections_; }
  void StartSlippiGame();
  void SendSlippiPad(std::unique_ptr<Pad> pad);
  int RemoteMachineCount() const { return remote_player_count_; }
  int RemotePortCount() const { return remote_player_count_; }
  void SetMatchSelections(PlayerSelections& s);
  void ResendSelections();   // the same selection again, with a fresh Gecko lock
  // Custom fighters online (host/online_fighters.h). EveryRemoteTakesCustomFighters: every remote's build
  // set MU_BUILD_CAPS4_CUSTOM_FIGHTERS. CustomPickMasked: this side's last 0x82 hid a custom pick (sent
  // as "no character locked") because it was not selectable for every player then. UnlockLocalCharacter:
  // the local pick is no longer locked (an added fighter not every player has); nothing is sent.
  // SendFighterListIfDue: this side's list of added fighters to every remote that takes them, when one
  // has not had the current list yet (host thread, every frame the online select polls).
  bool EveryRemoteTakesCustomFighters();
  bool CustomPickMasked() const { return custom_masked_.load(std::memory_order_acquire); }
  void UnlockLocalCharacter();
  void SendFighterListIfDue();
  std::unique_ptr<RemotePadOutput> GetSlippiRemotePad(int index, int max_frame_count);
  void DropOldRemoteInputs(int32_t finalized_frame);
  std::unordered_map<uint8_t, bool> GetActivePlayerIndices() const;
  void ForceDisconnectPlayer(uint8_t player_idx);
  void ForceDisconnect(DisconnectReason reason = DisconnectReason::UNSPECIFIED);
  DisconnectReason GetDisconnectReason() const { return (DisconnectReason)disconnect_reason_.load(std::memory_order_acquire); }
  // A copy: the network thread merges the remote players' selections while the game reads them.
  MatchInfo GetMatchInfo() { std::lock_guard<std::mutex> lk(selection_mutex_); return match_info_; }
  PlayerSelections GetSlippiRemoteChatMessage(bool chat_enabled);
  uint8_t GetSlippiRemoteSentChatMessage(bool chat_enabled);
  int32_t CalcTimeOffsetUs();
  double GetAndResetAvgPingMs();
  // Once a dropped player's last input is settled it repeats up to `frame` (Slippi 3.7.0).
  void FillDroppedPlayerInputs(int32_t frame);
  int LastPingMs() const { return (int)(last_ping_ms_.load(std::memory_order_relaxed)); }
  // Each remote machine's last ping (Teams has up to three), and how many there are. UI thread safe.
  int RemotePingMs(int index) const {
    return index >= 0 && index < REMOTE_PLAYER_MAX ? (int)remote_ping_ms_[index].load(std::memory_order_relaxed) : 0;
  }
  int RemoteCount() const { return (int)remote_count_seen_.load(std::memory_order_relaxed); }
  // The median of the last pre-match pings (NP_MSG_MU_PING) to the first opponent, 0 if none yet.
  int PreMatchPingMs() const { return (int)(prematch_ping_ms_.load(std::memory_order_relaxed)); }
  void SendChatMessage(int message_id);
  void SendSyncedGameState(const SyncedGameState&);
  DesyncRecoveryResp GetDesyncRecoveryState();
  void SendGamePrepStep(const GamePrepStep& step);
  std::array<uint8_t, 6> FetchGamePrepStep(uint8_t index) { return game_prep_.fetch(index); }
  void SendAsync(std::unique_ptr<Packet> packet);
  uint8_t remote_sent_chat_message_id = 0;
  // The opponents' builds (NP_MSG_MU_BUILD), and when the connection completed. early_pads: the
  // build takes early inputs (a trailing byte older builds never sent and never read).
  struct RemoteBuild { bool received = false, mod_view = false; std::string fingerprint, name; bool early_pads = false, pings = false;
                       uint32_t shield_rgb = 0;   // their own shield colour, 0x1000000 | RGB, 0 none sent
                       uint8_t port = 0xFF;
                       bool static_engine = false; std::string ranked_rules_digest;   // their in-game slot
                       bool setup_hash = false;   // they send NP_MSG_MU_SETUP_HASH and wait for ours
                       bool pick_previews = false;   // takes NP_MSG_MU_PICK_PREVIEW
                       uint8_t caps2 = 0;   // the second capability byte (mu_build_capabilities.h), 0 when absent
                       // Which MU_BUILD layout the peer speaks: 0 Melee Unlocked 0.9.200 to 0.9.216 (one byte:
                       // 0x10 arena, 0x20 automatic L-cancel, 0x40 Gecko, 0.9.215 on 0x80 warm-up; arena, Gecko and
                       // warm-up ride ids 0xE3, 0xE4 and 0xE5), 1 legacy 1xx builds (two bytes, no warm-up),
                       // 2 this build and later (three bytes; warm-up when caps3 has MU_BUILD_CAPS3_WARMUP).
                       uint8_t layout = 2;
                       bool auto_lcancel = false;     // presses for L-cancels this session (automatic L-cancel)
                       bool gecko = false;            // takes Gecko code messages (NP_MSG_MU_GECKO)
                       bool warmup = false;           // sends warm-up messages (NP_MSG_MU_WARMUP; read and dropped)
                       bool pause_rule = false, no_pause = false;   // MU_BUILD_CAPS3_PAUSE_RULE / _NO_PAUSE
                       int8_t port_colour[2] = {-1, -1};   // their port colour choices (1.0.2 on), -1 none
                       bool skins = false;   // takes skins messages (MU_BUILD_CAPS3_SKINS)
                       // The product it says it is (product_identity.h kWireName, "nucleus"; "netlee" before the rename) and its version,
                       // after the port colours; empty from Melee Unlocked, which sends none.
                       std::string product, product_version;
                       // Items (MU_BUILD_CAPS4_ITEMS): it reads the rule; its last choice (NP_MSG_MU_ITEMS), -1 none yet.
                       bool items_rule = false;
                       int8_t items_choice = -1;
                       // Custom fighters online (MU_BUILD_CAPS4_CUSTOM_FIGHTERS from a "nucleus" build).
                       bool custom_fighters = false;
                       // Squad Strike in Direct (MU_BUILD_CAPS4_SQUAD): it plays it; its last choice and lineup
                       // (NP_MSG_MU_SQUAD; choice -1 none yet, lineup count 0 none picked).
                       bool squad_rule = false;
                       int8_t squad_choice = -1;
                       uint8_t squad_count = 0;
                       uint8_t squad_lineup[8] = {}; };
  // The shield colour the opponent on in-game `slot` sent (0 none).
  uint32_t RemoteShieldColor(int slot);
  RemoteBuild GetRemoteBuild(int index);
  // Whether every remote machine has sent NP_MSG_MU_BUILD, which plain Slippi Dolphin never does.
  bool EveryRemoteReadsGoneOwnFrames();
  // The match setup hash (mu_setup_hash.h). SendSetupHash queues ours for every peer (any thread);
  // GetSetupHash gives remote `index`'s latest one.
  struct PeerSetupHash { bool received = false; MuSetupHash value{}; };
  void SendSetupHash(uint64_t inputs, uint64_t setup);
  PeerSetupHash GetSetupHash(int index);
  uint64_t ConnectedAtMs() const { return connected_at_ms_.load(std::memory_order_acquire); }
  // Early inputs. SendEarlyPad goes to the peers that take them (any thread: it only queues).
  // EarlyPads gives remote `index`'s early pads for frames after `latest` (its confirmed pads reach
  // `latest`; older entries are dropped), for the game's prediction.
  void SendEarlyPad(int32_t target, const uint8_t pad[host::early_input::kPadBytes]);
  // Slippi 3.7.0 input suggestion: this side's newest input between frames, for the players that do
  // not take this build's early pads (Slippi Dolphin, and the Static Recomp). Any thread: it only queues.
  void SendSlippiPadSuggestion(int32_t target_frame, uint32_t seq, const uint8_t pad[host::early_input::kPadBytes]);
  void EarlyPads(int index, int32_t latest, std::vector<host::early_input::Entry>& out);
  // Live pick previews (display only). SendPickPreview: the game thread, every CSS frame; sends
  // to the peers that take them, only while CONNECTED, only on change, at most one per
  // MU_PICK_PREVIEW_INTERVAL frames. RemotePickPreview: remote `index`'s latest preview and its
  // port, false when none arrived since the last game start or that player is gone.
  void SendPickPreview(const MuPickPreview& pick, uint32_t frame);
  bool RemotePickPreview(int index, MuPickPreview* out, uint8_t* port);
  // Direct pausing is off for this match: someone turned it off and every peer's build reads the rule.
  bool PauseOff();
  // Items (NP_MSG_MU_ITEMS). SendItemsChoice queues this player's choice (online::items_choice()) for
  // every peer that reads the rule (any thread). Items: what the match uses, from the choice this side
  // last sent and each remote's last one: on only when every one of them is 1. `blocker` is the port of
  // the first player keeping it off (-1: this player, or nobody), `unsupported` whether that player's
  // game has no such rule (or sent no build). LocalLocked: this player's last 0x82 was a lock-in.
  struct ItemsVerdict { bool on = false, unsupported = false; int blocker = -1; };
  void SendItemsChoice();
  ItemsVerdict Items();
  bool LocalLocked();
  // This side has Items on and a peer that reads the rule has not sent its choice yet: the match is not
  // set up until it is here (as SquadPending). Its 0xEB can trail its pick right after connecting: a
  // pick made before our build reached it goes out alone (Send passes 0xEB only to peers whose build it
  // has), and the choice follows when our build arrives there.
  bool ItemsPending();
  // Squad Strike in Direct (NP_MSG_MU_SQUAD), as Items: SendSquad queues this player's choice
  // (online::squad_choice(), Off where it is not offered) and lineup (online::squad_local_lineup) for every peer that plays it. Squad:
  // on only in a 1v1 when every peer plays it and this side and the peer sent choice 1. SquadLineups:
  // when on, the lineup this side last sent and the peer's, on their ports (count 0 where none yet).
  struct SquadVerdict { bool on = false, unsupported = false; int blocker = -1; };
  void SendSquad();
  SquadVerdict Squad();
  bool SquadLineups(uint8_t counts[2], uint8_t lineups[2][8]);
  // This side has Squad Strike on and a peer that plays it has not sent its choice yet (its message can
  // trail its pick right after connecting): the match is not set up until it is here.
  bool SquadPending();
  // The port colour choices this side sent and remote 0's (1v1), -1 none. False before both are known.
  bool PortColours(int8_t mine[2], int8_t theirs[2]);
  // Gecko codes for the match (one remote): SendGecko queues one gecko_online message for a peer that
  // takes them (false otherwise); PopGecko takes the oldest received.
  bool SendGecko(const std::vector<uint8_t>& message);
  bool PopGecko(std::vector<uint8_t>& out);

 private:
  struct FrameTiming { int32_t frame = 0; uint64_t time_us = 0; };
  struct FrameOffsetData { int idx = 0; std::vector<int32_t> buf; };
  struct ActiveConnectionInfo { uint8_t player_idx = 0; bool is_disconnected = false; };

  // Connection index, used only by machine-scoped messages and clocks.
  uint8_t PlayerIdxFromPort(uint8_t port) const {
    for (int i = 0; i < remote_player_count_; ++i) if (match_info_.remote[i].player_idx == port) return (uint8_t)i;
    return REMOTE_PLAYER_MAX;
  }
  int SenderPort(_ENetPeer* peer) const;
  void OnData(Packet& packet, _ENetPeer* peer);
  void Send(Packet& packet);
  void Disconnect();
  void ThreadFunc();
  bool AreAllPeersDisconnectedForKey(const std::string& key);
  // Player drops (Slippi 3.7.0), network thread: the others are told which inputs this side has of a
  // dropped player, and after a short wait everyone settles on the same last input and despawn frame.
  bool ProcessPlayerDrops();
  void SendPlayerDropReport(uint8_t port);
  void AddPlayerDropReportInputs(uint8_t port, int32_t last_frame, uint8_t count, const uint8_t* inputs);
  bool AreAllConnectionsDisconnected();
  void WriteSelections(Packet& p, const PlayerSelections& s);
  std::unique_ptr<PlayerSelections> ReadSelections(Packet& p);
  std::unique_ptr<PlayerSelections> ReadChatMessage(Packet& p);

  GamePrepQueue game_prep_;
  std::atomic<bool> sent_no_pause_{false};
  // The Items choice last queued for the peers (-1 none). items_mutex_ makes reading the choice and
  // queueing it one step, so the last value queued is the one recorded; it is held only around
  // SendAsync (async_mutex_, a leaf).
  std::mutex items_mutex_;
  std::atomic<int> sent_items_choice_{-1};
  // Squad Strike in Direct: the choice and lineup last queued for the peers (sent_squad_choice_ -1 none),
  // under squad_mutex_ (held around SendAsync only, as items_mutex_).
  std::mutex squad_mutex_;
  uint8_t sent_squad_count_ = 0, sent_squad_lineup_[8] = {};
  std::atomic<int> sent_squad_choice_{-1};
  std::atomic<int> sent_port_colour_{-1};
  std::mutex sync_mutex_;
  SyncedGameState remote_sync_states_[REMOTE_PLAYER_MAX], local_sync_state_;
  bool recovering_ = false;
  std::mutex pad_mutex_, ack_mutex_, async_mutex_;
  // timing_mutex_: last_frame_timing_, has_game_started_, frame_offset_data_ (network thread and game
  // thread). chat_mutex_: remote_chat_message_selection_. selection_mutex_: match_info_ (the network
  // thread merges remote selections, the game thread reads, resets and sets its own; the player_idx
  // fields are written once in the constructor and read without it).
  // Selection receipt takes selection_mutex_, timing_mutex_ and pad_mutex_ together so its
  // pre-start decision and queue reset are atomic. Pad sends take pad_mutex_ before
  // ack_mutex_ and async_mutex_; chat replies take chat_mutex_ before async_mutex_.
  // Neither ACK receipt nor the async drain takes pad_mutex_.
  // async_mutex_ is never held while taking another, so no two are ever taken in opposite orders.
  // SetMatchSelections also queues a lock-in's pick preview under selection_mutex_; that takes the
  // leaves build_mutex_ and pick_mutex_ one at a time, never selection_mutex_ under them.
  std::mutex timing_mutex_, chat_mutex_, selection_mutex_;
  std::deque<std::unique_ptr<Packet>> async_queue_;
  _ENetHost* client_ = nullptr;
  std::vector<_ENetPeer*> server_;
  std::vector<std::string> public_fallbacks_; // parallel to remote seats, never additional players
  std::thread thread_;
  uint8_t remote_player_count_ = 0; // real remote machines: server_, ACKs, clocks, builds and setup hashes
  std::atomic<bool> do_loop_{true};
  bool is_decider_ = false, has_game_started_ = false;
  std::array<bool, REMOTE_PLAYER_MAX> remote_game_started_{};   // under timing_mutex_
  uint8_t player_idx_ = 0;
  std::unordered_map<std::string, std::map<_ENetPeer*, ActiveConnectionInfo>> active_connections_;
  std::atomic<bool> player_active_[PLAYER_COUNT_MAX] = {};
  std::deque<std::unique_ptr<Pad>> local_pad_queue_;
  std::deque<std::unique_ptr<Pad>> remote_pads_[PLAYER_COUNT_MAX];
  // Each remote port's latest inputs, newest first: what another client may be missing if the
  // player drops (Slippi 3.7.0). By port, like remote_pads_; under pad_mutex_.
  std::deque<Pad> remote_pad_history_[PLAYER_COUNT_MAX];
  std::atomic<int32_t> despawn_frames_[PLAYER_COUNT_MAX] = {};   // by port, 0 until settled
  uint64_t player_drop_times_ms_[PLAYER_COUNT_MAX] = {};         // network thread only
  struct ChecksumEntry { int32_t frame = 0; uint32_t value = 0; } remote_checksums_[REMOTE_PLAYER_MAX];
  uint64_t ping_us_[REMOTE_PLAYER_MAX] = {};
  std::atomic<uint64_t> ping_sample_sum_us_{0}, ping_sample_count_{0};
  std::atomic<uint32_t> last_ping_ms_{0};
  std::atomic<uint32_t> remote_ping_ms_[REMOTE_PLAYER_MAX] = {};
  std::atomic<uint8_t> remote_count_seen_{0};
  std::atomic<uint32_t> prematch_ping_ms_{0};
  std::deque<uint32_t> prematch_samples_;   // network thread only
  void SendPrematchPing();
  int32_t last_frame_acked_[REMOTE_PLAYER_MAX] = {};
  FrameOffsetData frame_offset_data_[REMOTE_PLAYER_MAX];
  FrameTiming last_frame_timing_[REMOTE_PLAYER_MAX];
  std::deque<FrameTiming> ack_timers_[REMOTE_PLAYER_MAX];
  std::atomic<ConnectStatus> status_{ConnectStatus::UNSET};
  std::atomic<uint32_t> pending_disconnect_reason_{0}, disconnect_reason_{0};
  std::vector<int> failed_connections_;
  MatchInfo match_info_;
  std::unique_ptr<PlayerSelections> remote_chat_message_selection_;
  std::mutex build_mutex_;
  RemoteBuild remote_build_[REMOTE_PLAYER_MAX];
  PeerSetupHash setup_hash_[REMOTE_PLAYER_MAX];   // under build_mutex_
  std::atomic<uint64_t> connected_at_ms_{0};
  host::early_input::Buffer early_pads_[REMOTE_PLAYER_MAX];   // under pad_mutex_
  std::mutex gecko_mutex_;                       // gecko_inbox_ (network thread in, game thread out)
  std::deque<std::vector<uint8_t>> gecko_inbox_;
  // Opponents' skins (NP_MSG_MU_SKINS): each remote's last skins message, checked and bound to its
  // sender, waiting for that remote's next 0x82 (under build_mutex_; cleared at each game start).
  std::vector<uint8_t> skins_pending_[REMOTE_PLAYER_MAX];
  bool skins_pending_set_[REMOTE_PLAYER_MAX] = {};
  void SendLocalSkins(bool selected, uint8_t character, uint8_t costume);
  // Custom fighters online (NP_MSG_MU_CONTENT): the list generation each remote was sent (0 none; under
  // build_mutex_).
  uint32_t fighter_list_sent_[REMOTE_PLAYER_MAX] = {};
  std::atomic<bool> custom_masked_{false};
  // The selection as it goes on the wire: a pick of an added fighter is hidden ("no character locked")
  // until every player has that fighter (host::online_fighters::verdict).
  PlayerSelections WireSelections(const PlayerSelections& s);
  void SendBuild();
  // Pick previews. pick_mutex_ is a leaf: never held while taking another lock (SendAsync and
  // build_mutex_ included). It guards everything below it here.
  void QueuePickPreview(MuPickPreview pick, uint32_t frame, bool locked_now);
  void SendLockedPickPreview();
  std::mutex pick_mutex_;
  MuPickPreviewSender pick_sender_{};
  MuPickPreview pick_local_{};
  bool pick_local_set_ = false;
  uint8_t pick_sent_peers_ = 0;   // remote indices that took previews at the last send
  MuPickPreview pick_remote_[REMOTE_PLAYER_MAX] = {};
  uint8_t pick_remote_port_[REMOTE_PLAYER_MAX] = {};
  bool pick_remote_set_[REMOTE_PLAYER_MAX] = {};
};

class Matchmaking {
 public:
  enum OnlinePlayMode { RANKED = 0, UNRANKED = 1, DIRECT = 2, TEAMS = 3, PARTY = 4 };
  enum ProcessState { IDLE, INITIALIZING, MATCHMAKING, OPPONENT_CONNECTING, CONNECTION_SUCCESS, ERROR_ENCOUNTERED };
  struct MatchSearchSettings { OnlinePlayMode mode = UNRANKED; std::string connect_code; std::vector<std::string> blocked_codes;
  };
  // True when an opponent in the match is on the Unranked block list (codes compared ignoring case
  // and spaces). Public for the unit test.
  static bool IsBlockedMatch(OnlinePlayMode mode, const std::vector<std::string>& blocked,
                             const std::vector<UserInfo>& players, int local_index);
  struct MatchmakeResult { std::string id; std::vector<UserInfo> players; std::vector<uint16_t> stages; uint32_t items = 0; };
  // The match a get-ticket-resp describes, read without trusting any of it. ok is false (and error
  // says why) unless there are 2 to 4 players, every port is 1..4 and used once, and one of them is
  // the local player; then no count or index taken from it can pass the fixed per-player arrays.
  struct ParsedTicket {
    bool ok = false;
    std::string error;
    std::string match_id;
    std::vector<UserInfo> players;          // in the ticket's order
    int local_player_index = 0;             // the local player's port - 1
    std::vector<std::string> remote_ips;    // "ip:port" per remote, in the ticket's order
    std::vector<std::string> public_fallbacks; // same shape; empty when no same-router fallback
    bool is_host = false;
    std::vector<uint16_t> stages;
    uint32_t items = 0;
  };
  // Pure parsing of a get-ticket-resp body (JSON text). Never throws; no sockets, no state.
  static ParsedTicket ParseTicket(const std::string& json_text);
  // Local test peering (no matchmaking server): fixed player index, local port and the address of
  // every other player ("ip:port", in player-index order with the local player left out). One
  // remote is a two-player match; three is a four-player (Teams) match.
  struct LocalPeer {
    bool enabled = false;
    int local_index = 0;
    uint16_t local_port = 0;
    std::vector<std::string> remotes;
    int test_stage = -1;  // deterministic stage selection for local regression runs only
  };

  explicit Matchmaking(User* user);
  ~Matchmaking();
  void FindMatch(MatchSearchSettings settings);
  ProcessState GetMatchmakeState() const { return state_; }
  std::string GetErrorMessage() const { std::lock_guard<std::mutex> lk(result_mutex_); return error_msg_; }
  bool IsSearching() const { return state_ == INITIALIZING || state_ == MATCHMAKING || state_ == OPPONENT_CONNECTING; }
  std::unique_ptr<NetplayClient> GetNetplayClient() { std::lock_guard<std::mutex> lk(result_mutex_); return std::move(netplay_client_); }
  int LocalPlayerIndex() const { std::lock_guard<std::mutex> lk(result_mutex_); return local_player_index_; }
  std::vector<UserInfo> GetPlayerInfo() const { std::lock_guard<std::mutex> lk(result_mutex_); return player_info_; }
  std::string GetPlayerName(uint8_t port) const;
  std::vector<uint16_t> GetStages() const { std::lock_guard<std::mutex> lk(result_mutex_); return allowed_stages_; }
  // Never more than REMOTE_PLAYER_MAX: callers index fixed per-remote arrays with it.
  uint8_t RemotePlayerCount() const {
    std::lock_guard<std::mutex> lk(result_mutex_);
    const size_t n = player_info_.empty() ? 0 : player_info_.size() - 1;
    return (uint8_t)(n < (size_t)REMOTE_PLAYER_MAX ? n : (size_t)REMOTE_PLAYER_MAX);
  }
  // True when every player but the local one is a bot (asked every frame of a match: no copy).
  bool AllRemotesAreBots() const {
    std::lock_guard<std::mutex> lk(result_mutex_);
    for (size_t i = 0; i < player_info_.size(); ++i) { if ((int)i == local_player_index_) continue; if (!player_info_[i].is_bot) return false; }
    return true;
  }
  MatchmakeResult GetMatchmakeResult() const { std::lock_guard<std::mutex> lk(result_mutex_); return mm_result_; }
  static bool IsFixedRulesMode(OnlinePlayMode m) { return m == UNRANKED || m == RANKED || m == PARTY; }
  static LocalPeer local_peer;          // set from the command line for local 2-4 instance tests
  static uint16_t forced_port;          // 0 = random 41000..50999
  // False in hidden or scripted runs unless --allow-matchmaking: an automated run must never
  // queue on Slippi's servers by accident (local peering is unaffected).
  static bool server_allowed;

 private:
  void MatchmakeThread();
  void startMatchmaking();
  std::string openSlippiTicket(const UserInfo& me);
  void handleMatchmaking();
  void handleConnecting();
  void disconnectFromServer();
  void terminateMmConnection();
  struct Ticket;                          // a get-ticket-resp body (defined in slippi_net.cpp)
  bool ingest_ticket(const Ticket& ticket);   // false: the ticket was refused and the search has failed
  void fail(const std::string& message);      // the search ends with this error text

  User* user_;
  // Matchmaking thread publishes the selected route; only display code consumes it.
  _ENetHost* client_ = nullptr;
  _ENetPeer* server_ = nullptr;
  bool is_mm_connected_ = false;
  // Slippi 3.7.0: the search runs on the WebSocket matchmaking service (slippi_mm_service.h). client_
  // is then only the socket bound to the netplay port: STUN servers are asked through it for its
  // public address, and one of them is sent a packet now and then so the NAT mapping the opponent
  // will use does not expire while queued.
  std::unique_ptr<mm_service::Session> mm_session_;
  uint32_t stun_host_ = 0;        // the STUN server the keepalives go to, network byte order
  uint16_t stun_port_ = 0;
  bool has_stun_addr_ = false;
  uint64_t last_keepalive_ms_ = 0;
  bool stunBindingRequest(uint32_t host, uint16_t port, mm_service::StunObservation* result);
  void sendStunKeepalive();
  std::atomic<bool> is_mm_terminated_{false};
  std::thread thread_;
  MatchSearchSettings search_settings_;
  std::atomic<ProcessState> state_{IDLE};
  // result_mutex_: what the game thread reads of a search while the matchmaking thread works:
  // error_msg_, local_player_index_, mm_result_, player_info_, allowed_stages_, netplay_client_.
  // The matchmaking thread builds a result aside and swaps it in complete, and stores the error text
  // before the state that announces it. It is a leaf lock: nothing else is taken while it is held
  // (not the user record's mutex, none of the netplay client's), and no netplay client is created or
  // destroyed under it. The matchmaking thread is the only writer after FindMatch, so it reads its
  // own copies without the lock.
  mutable std::mutex result_mutex_;
  std::string error_msg_;
  int local_player_index_ = 0;
  MatchmakeResult mm_result_;
  std::vector<UserInfo> player_info_;
  std::vector<uint16_t> allowed_stages_;
  std::unique_ptr<NetplayClient> netplay_client_;
  // Matchmaking thread only.
  int host_port_ = 0;
  std::vector<std::string> remote_ips_;
  std::vector<std::string> public_fallbacks_;
  bool is_host_ = false;
};


// Helpers shared with the EXI device.
uint64_t time_us();
uint64_t time_ms();
std::string normalize_history_code(const std::string& code);
std::string history_code_for_name_entry(const std::string& code);
std::string utf8_to_shiftjis(const std::string& s);
std::string shiftjis_to_utf8(const std::string& s);
std::string convert_string_for_game(const std::string& input, int length);
std::string truncate_length_char(const std::string& input, int length);
std::string convert_connect_code_for_game(const std::string& input);
bool enet_ready();   // initializes ENet once

}  // namespace slippi
