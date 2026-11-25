// Slippi Online networking (see slippi_net.h). Wire-compatible port of Dolphin's
// SlippiNetplayClient, SlippiMatchmaking and the user record.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_agent_lock.h"
#include "test_env.h"
#include "public_build.h"   // log_id: other players' codes in a public build's log
#include "slippi_net.h"
#include "slippi_address.h"
#include "slippi_netplay_receive.h"
#include "slippi_gone_port.h"
#include "slippi_online.h"
#include "slippi_report.h"
#include "slippi_account.h"
#include "host.h"
#include "gx_core.h"   // the player's own shield colour for the build message
#include "early_input.h"
#include "lcancel.h"   // automatic L-cancel, announced in the build message
#include "gecko_session.h"   // Gecko codes agreed for the match, locked with each selection
#include "mu_skin_wire.h"     // opponents' skins: the message
#include "opponent_skins.h"   // and what the receiving side does with it
#include "online_fighters.h"   // custom fighters online (NP_MSG_MU_CONTENT)
#include "product_identity.h"   // the product named in the build message
#include "../abi/mu_squad_wire.h"   // Squad Strike in Direct: lineup checks (MU_SQUAD builds)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "slippi_version.h"
#include "slippi_input_suggestions.h"

using json = nlohmann::json;

namespace slippi {

const char* const SLIPPI_SEMVER = "3.7.0";

// Type-checked reads of JSON that came from a server or a file: a missing key, a null or a value of
// another type gives the default. None of these throws (value() and get<T>() do on a wrong type).
static const json* json_member(const json& j, const char* key) {
  if (!j.is_object()) return nullptr;
  auto it = j.find(key);
  return it == j.end() ? nullptr : &*it;
}
static std::string str_or(const json& j, const char* key, const char* def) {
  const json* v = json_member(j, key);
  return v && v->is_string() ? v->get<std::string>() : std::string(def);
}
static int int_or(const json& j, const char* key, int def) {
  const json* v = json_member(j, key);
  return v && v->is_number() ? v->get<int>() : def;
}
static uint32_t uint_or(const json& j, const char* key, uint32_t def) {
  const json* v = json_member(j, key);
  return v && v->is_number() ? v->get<uint32_t>() : def;
}
static bool bool_or(const json& j, const char* key, bool def) {
  const json* v = json_member(j, key);
  return v && v->is_boolean() ? v->get<bool>() : def;
}

uint64_t time_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t time_ms() { return time_us() / 1000; }

// Test only. MELEE_NET_LOSS_PCT=P (1 to 99) drops P percent of the datagrams a game's netplay link
// receives, before ENet sees them, as a lossy connection does (ENet resends what was sent reliably;
// inputs carry their recent history). With MELEE_NET_LAG_MS / MELEE_NET_JITTER_MS it gives the test
// links of one PC a real connection's lag, jitter and loss. Unset, as it always is for a player
// (host::test_env), nothing changes.
static uint32_t test_loss_pct() {
  static const uint32_t pct = [] {
    const char* s = host::test_env::getenv("MELEE_NET_LOSS_PCT");
    const long v = s ? std::atol(s) : 0;
    return v > 0 && v < 100 ? (uint32_t)v : 0u;
  }();
  return pct;
}
static int ENET_CALLBACK test_loss_intercept(ENetHost*, ENetEvent*) {
  static thread_local uint64_t state = 0x2545F4914F6CDD1Dull ^ (uint64_t)time_us();
  state ^= state << 13; state ^= state >> 7; state ^= state << 17;
  return (state % 100) < test_loss_pct() ? 1 : 0;   // 1: taken (dropped); 0: ENet reads it
}

// ---------------------------------------------------------------- strings
static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
static std::string wide_to_cp(const std::wstring& w, UINT cp) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
std::string utf8_to_shiftjis(const std::string& s) { return wide_to_cp(utf8_to_wide(s), 932); }
std::string shiftjis_to_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(932, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(932, 0, s.data(), (int)s.size(), &w[0], n);
  return wide_to_cp(w, CP_UTF8);
}
// The game's name-entry field stores two Shift-JIS bytes per character.
std::string normalize_history_code(const std::string& code) {
  auto wide = utf8_to_wide(code);
  for (auto& ch : wide)
    if (ch >= 0xFF01 && ch <= 0xFF5E) ch -= 0xFEE0;
  return wide_to_cp(wide, CP_UTF8);
}
std::string history_code_for_name_entry(const std::string& code) {
  auto wide = utf8_to_wide(code);
  for (auto& ch : wide)
    if (ch >= 0x21 && ch <= 0x7E) ch += 0xFEE0;
  return wide_to_cp(wide, 932);
}
std::string truncate_length_char(const std::string& input, int length) {
  // Count code points, not bytes (UTF8ToUTF32 / resize / UTF32toUTF8 in Dolphin).
  std::string out;
  int count = 0;
  for (size_t i = 0; i < input.size() && count < length; ) {
    unsigned char c = (unsigned char)input[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    out.append(input, i, len);
    i += len; ++count;
  }
  return out;
}
static void convert_narrow_special_shiftjis(std::string& input) {
  static const std::unordered_map<char, uint16_t> table = {
      {'!', 0x8149}, {'"', 0x8168}, {'#', 0x8194}, {'$', 0x8190}, {'%', 0x8193}, {'&', 0x8195}, {'\'', 0x8166}, {'(', 0x8169},
      {')', 0x816a}, {'*', 0x8196}, {'+', 0x817b}, {',', 0x8143}, {'-', 0x817c}, {'.', 0x8144}, {'/', 0x815e}, {':', 0x8146},
      {';', 0x8147}, {'<', 0x8183}, {'=', 0x8181}, {'>', 0x8184}, {'?', 0x8148}, {'@', 0x8197}, {'[', 0x816d}, {'\\', 0x815f},
      {']', 0x816e}, {'^', 0x814f}, {'_', 0x8151}, {'`', 0x814d}, {'{', 0x816f}, {'|', 0x8162}, {'}', 0x8170}, {'~', 0x8160},
  };
  size_t pos = 0;
  while (pos < input.size()) {
    char c = input[pos];
    if ((unsigned char)c & 0x80) { pos += 2; continue; }
    auto it = table.find(c);
    if (it == table.end()) { ++pos; continue; }
    input.erase(pos, 1);
    // Dolphin inserts the little-endian bytes of the char16 in reverse, which yields big-endian order.
    input.insert(input.begin() + pos, (char)(it->second & 0xFF));
    input.insert(input.begin() + pos, (char)(it->second >> 8));
    pos += 2;
  }
}
std::string convert_string_for_game(const std::string& input, int length) {
  std::string sj = utf8_to_shiftjis(truncate_length_char(input, length));
  convert_narrow_special_shiftjis(sj);
  sj.resize(length * 2 + 1);
  return sj;
}
std::string convert_connect_code_for_game(const std::string& input) {
  std::string code;
  for (char c : input) { if (c == '#') { code += (char)0x81; code += (char)0x94; } else code += c; }
  code.resize(8 + 2);
  return code;
}

// ---------------------------------------------------------------- ENet
static bool g_enet_ready = false;
bool enet_ready() {
  if (!g_enet_ready) { if (enet_initialize() < 0) { host::log("slippi: enet_initialize failed"); return false; } g_enet_ready = true; }
  return true;
}
static void wakeup_thread(ENetHost* host) {
  if (!host) return;
  ENetAddress address;
  if (host->address.port != 0) address.port = host->address.port;
  else enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;
  uint8_t byte = 0;
  ENetBuffer buf; buf.data = &byte; buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}
static int ENET_CALLBACK intercept_callback(ENetHost* host, ENetEvent* event) {
  if (test_loss_pct() > 0 && test_loss_intercept(host, event)) return 1;   // tests: stays lossy once connected
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0) { event->type = (ENetEventType)42; return 1; }
  return 0;
}
static std::string peer_key(ENetPeer* p) { std::stringstream s; s << p->address.host << "-" << p->address.port; return s.str(); }

// ---------------------------------------------------------------- Packet
void Packet::append(const void* data, size_t size) { const uint8_t* p = (const uint8_t*)data; buf_.insert(buf_.end(), p, p + size); }
Packet& Packet::operator<<(uint8_t v) { buf_.push_back(v); return *this; }
Packet& Packet::operator<<(uint16_t v) { buf_.push_back((uint8_t)(v >> 8)); buf_.push_back((uint8_t)v); return *this; }
Packet& Packet::operator<<(uint32_t v) { for (int i = 3; i >= 0; --i) buf_.push_back((uint8_t)(v >> (8 * i))); return *this; }
Packet& Packet::operator<<(const std::string& s) { *this << (uint32_t)s.size(); append(s.data(), s.size()); return *this; }
bool Packet::check(size_t n) { if (!ok_ || n > buf_.size() - pos_) { ok_ = false; return false; } return true; }
Packet& Packet::operator>>(uint8_t& v) { if (check(1)) v = buf_[pos_++]; return *this; }
Packet& Packet::operator>>(bool& v) { uint8_t b = 0; *this >> b; v = b != 0; return *this; }
Packet& Packet::operator>>(uint16_t& v) { if (check(2)) { v = (uint16_t)((buf_[pos_] << 8) | buf_[pos_ + 1]); pos_ += 2; } return *this; }
Packet& Packet::operator>>(uint32_t& v) {
  if (check(4)) { v = ((uint32_t)buf_[pos_] << 24) | ((uint32_t)buf_[pos_ + 1] << 16) | ((uint32_t)buf_[pos_ + 2] << 8) | buf_[pos_ + 3]; pos_ += 4; }
  return *this;
}
Packet& Packet::operator>>(int32_t& v) { uint32_t u = 0; *this >> u; v = (int32_t)u; return *this; }
Packet& Packet::operator>>(std::string& s) {
  uint32_t n = 0; *this >> n;
  if (check(n)) { if (n) s.assign((const char*)buf_.data() + pos_, n); else s.clear(); pos_ += n; }
  return *this;
}

// Tests only (MELEE_TEST_MM_HOST=<ip>:<port>): the matchmaking stand-in (port/tests/mm_standin.cpp)
// answers in place of mm.slippi.gg, and while it is set no other Slippi server is asked either (the
// profile refresh and the version check), so test identities never reach Slippi. Read only in a run
// started by automation (host/test_env.h) and only for a stand-in on this computer: the stand-in is
// reached over plain ENet, so the play key is never sent to it.
static bool mm_test_server(std::string* host, int* port) {
  static const std::string spec = [] {
    const char* v = host::test_env::automated_getenv("MELEE_TEST_MM_HOST");
    std::string s = v ? v : "";
    const size_t colon = s.rfind(':');
    if (!s.empty() && (colon == std::string::npos || !host::test_env::loopback_host(s.c_str(), colon))) {
      host::log("slippi: MELEE_TEST_MM_HOST is not a stand-in on this computer; ignored");
      s.clear();
    }
    return s;
  }();
  const size_t colon = spec.rfind(':');
  if (spec.empty() || colon == std::string::npos) return false;
  if (host) *host = spec.substr(0, colon);
  if (port) *port = std::atoi(spec.c_str() + colon + 1);
  return true;
}
bool matchmaking_stand_in() { return mm_test_server(nullptr, nullptr); }

// ---------------------------------------------------------------- user
User::User(std::string user_dir) : dir_(std::move(user_dir)) {
  if (host::test_env::agent_copy()) host::log("agent: skipping Slippi Launcher search and server refresh");
  if (AttemptLogin() && !host::test_env::agent_copy()) RefreshFromServer();
}
void User::RefreshFromServer() {
  if (agent_lock::blocked(agent_lock::Operation::Refresh)) return;
  if (refresh_.valid() && refresh_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  UserInfo me = GetUserInfo();
  if (me.uid.empty()) return;
  if (Matchmaking::local_peer.enabled) return;   // local test peering stays off Slippi's servers
  if (mm_test_server(nullptr, nullptr)) return;  // so does the matchmaking stand-in
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  refresh_ = std::async(std::launch::async, [this, me, alive] {
    // Nothing may leave this thread as an exception: that would end the process.
    try {
      std::string response; int status = 0;
      if (!report::http_get("https://users-rest-dot-slippi.uc.r.appspot.com/user/" + me.uid, &response, &status) || status != 200) {
        host::log("slippi: profile refresh unavailable (HTTP %d); keeping user.json", status);
        return;
      }
      json j = json::parse(response, nullptr, false);
      if (j.is_discarded() || !alive->load()) return;
      if (!j.is_object()) { host::log("slippi: profile refresh: unexpected response; keeping user.json"); return; }
      const std::string name = str_or(j, "displayName", ""), code = str_or(j, "connectCode", "");
      const std::string latest = str_or(j, "latestVersion", "");
      // All sixteen must be text, or the stored messages stay.
      std::vector<std::string> chat;
      if (const json* cm = json_member(j, "chatMessages"))
        if (cm->is_array() && cm->size() == 16)
          for (const auto& m : *cm) if (m.is_string()) chat.push_back(m.get<std::string>());
      std::lock_guard<std::mutex> lock(mutex_);
      if (info_.uid != me.uid) return;
      if (!name.empty()) info_.display_name = name;
      if (!code.empty()) info_.connect_code = code;
      if (!latest.empty()) info_.latest_version = latest;
      if (chat.size() == 16) info_.chat_messages = std::move(chat);
      host::log("slippi: profile refreshed from server: %s (%s)", info_.display_name.c_str(), info_.connect_code.c_str());
      if (!latest.empty()) host::log("slippi: the servers are on version %s, this build speaks %s%s", latest.c_str(), SLIPPI_SEMVER,
                                     version_newer(latest, SLIPPI_SEMVER) ? ": online play is off until " PRODUCT_NAME " is updated" : "");
    } catch (const std::exception& e) {
      host::log("slippi: profile refresh failed (%s); keeping user.json", e.what());
    } catch (...) {
      host::log("slippi: profile refresh failed; keeping user.json");
    }
  });
}
bool User::CheckSessionCompatibilityAt(uint64_t generation, std::string* error, VersionFetch fetch) {
  // Matchmaking and the render thread's asynchronous poll can request the same check. Keep the
  // network request off the render thread and share its result instead of repeating HTTP at
  // queue entry, "connecting to player", and the first game's ready screen.
  std::lock_guard<std::mutex> lock(compatibility_request_mutex_);
  if (verified_generation_ != generation) {
    const uint64_t start = time_ms();
    verified_ok_ = CheckOnlineCompatibility(&verified_error_, fetch);
    verified_generation_ = generation;
    host::log("slippi: version verification %s in %llu ms", verified_ok_ ? "passed" : "refused",
              (unsigned long long)(time_ms() - start));
  }
  *error = verified_error_;
  return verified_ok_;
}
bool User::CheckSessionCompatibility(std::string* error, VersionFetch fetch) {
  return CheckSessionCompatibilityAt(compatibility_generation_.load(), error, fetch);
}
int User::PollOnlineCompatibility(std::string* error, VersionFetch fetch) {
  if (agent_lock::blocked(agent_lock::Operation::Version)) { *error = agent_lock::refusal; return -1; }
  if (!compatibility_done_) {
    if (!compatibility_.valid()) {
      const uint64_t generation = compatibility_generation_.load();
      compatibility_ = std::async(std::launch::async, [this, fetch, generation] {
        std::string reason;
        const bool ok = CheckSessionCompatibilityAt(generation, &reason, fetch);
        return VersionResult{ok, std::move(reason), generation};
      });
    }
    if (compatibility_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return 0;
    auto result = compatibility_.get();
    if (result.generation != compatibility_generation_) return 0;  // cancelled/preceding game's request
    compatibility_ok_ = result.ok;
    compatibility_error_ = std::move(result.error);
    compatibility_done_ = true;
  }
  *error = compatibility_error_;
  return compatibility_ok_ ? 1 : -1;
}
bool User::CheckOnlineCompatibility(std::string* error, VersionFetch fetch) {
  if (agent_lock::blocked(agent_lock::Operation::Version)) { *error = agent_lock::refusal; return false; }
  *error = "Could not check Slippi version. Try again.";
  const auto failed = [](const char* step, unsigned long code) {
    host::log("slippi: version check failed: %s %lu", step, code);
    return false;
  };
  const UserInfo me = GetUserInfo();
  if (me.uid.empty()) { *error = "Sign in to Slippi before playing online."; return failed("signed-out", 0); }
  if (mm_test_server(nullptr, nullptr)) { error->clear(); return true; }   // tests: the stand-in, no version server
  try {
    // Three tries, then the version the account last reported (user.json, which the Slippi
    // Launcher keeps current, or the profile refresh). One failed request used to block online
    // play outright for a player whose network or Windows refused this request. No known
    // version at all still blocks. Each failed try logs its step.
    // A server that failed all three tries once this session gets one try after that: every search
    // waited out three timeouts (up to a minute) before it could start.
    static std::atomic<bool> unreachable{false};
    std::string latest;
    int status = 0;
    const int tries = unreachable.load() ? 1 : 3;
    for (int attempt = 0; attempt < tries && latest.empty(); ++attempt) {
      if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(700));
      std::string response;
      status = 0;
      if (!(fetch ? fetch : report::http_get)("https://users-rest-dot-slippi.uc.r.appspot.com/user/" + me.uid, &response, &status)) {
        const auto why = fetch ? report::HttpFailure{"http-get", (unsigned long)status} : report::last_http_failure();
        failed(why.step, why.code);
        continue;
      }
      if (status != 200) { failed("http-status", status); continue; }
      const json reply = json::parse(response, nullptr, false);
      if (!reply.is_object()) { failed("json", 0); continue; }
      const std::string v = str_or(reply, "latestVersion", "");
      if (release_version_valid(v)) latest = v;
      else failed("latest-version", 0);
    }
    unreachable.store(latest.empty() && !fetch);   // tests pass their own transport: always three tries
    const std::filesystem::path cache = std::filesystem::u8path(dir_) / "version-check.json";
    if (latest.empty()) {
      // The last answer the server gave this game (kept below, three days at most) and the
      // account's own, whichever is newer.
      std::string known = me.latest_version;
      {
        std::ifstream in(cache, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const json saved = json::parse(text, nullptr, false);
        const std::string v = saved.is_object() ? str_or(saved, "latestVersion", "") : std::string();
        const json* checked = json_member(saved, "checkedAt");
        const int64_t at = checked && checked->is_number_integer() ? checked->get<int64_t>() : 0;
        const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
        if (release_version_valid(v) && now - at >= 0 && now - at < 3 * 24 * 3600 &&
            (!release_version_valid(known) || version_newer(v, known)))
          known = v;
      }
      if (!release_version_valid(known)) {
        host::log("slippi: the version check did not answer (HTTP %d) and the account has no known version", status);
        return false;
      }
      host::log("slippi: the version check did not answer (HTTP %d); going by the account's last known version %s", status,
                known.c_str());
      if (version_newer(known, SLIPPI_SEMVER)) {
        *error = "Slippi " + known + " is out. Update " PRODUCT_NAME;
        return false;
      }
      error->clear();
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (info_.uid != me.uid) return failed("account-changed", 0);
      info_.latest_version = latest;
    }
    {
      std::error_code ec;
      std::filesystem::create_directories(cache.parent_path(), ec);
      std::ofstream out(cache, std::ios::trunc);
      out << json{{"latestVersion", latest},
                  {"checkedAt", (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count()}}.dump();
    }
    if (version_newer(latest, SLIPPI_SEMVER)) {
      *error = "Slippi " + latest + " is out. Update " PRODUCT_NAME;
      return failed("update-required", 0);
    }
    error->clear();
    return true;
  } catch (...) { return failed("exception", 0); }
}

bool User::AttemptLogin(bool rediscover) {
  if (agent_lock::blocked(agent_lock::Operation::Login)) return false;
  account::Profile profile;
  if (rediscover || !account_discovered_) {
    profile = account::resolve(dir_);
    account_file_ = profile ? profile.file.string() : std::string();
    account_discovered_ = true;
  } else if (!account_file_.empty()) {
    profile = account::read(account_file_);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string previous_uid = info_.uid, previous_key = info_.play_key;
  if (profile) {
    const json& j = profile.data;
    info_.uid = str_or(j, "uid", "");
    info_.play_key = str_or(j, "playKey", "");
    info_.display_name = str_or(j, "displayName", "");
    info_.connect_code = str_or(j, "connectCode", "");
    info_.latest_version = str_or(j, "latestVersion", "");
    info_.chat_messages.clear();
    if (const json* cm = json_member(j, "chatMessages"))
      if (cm->is_array()) for (const auto& m : *cm) if (m.is_string()) info_.chat_messages.push_back(m.get<std::string>());
    if (info_.chat_messages.size() != 16) info_.chat_messages = GetDefaultChatMessages();
    bool was = logged_in_;
    logged_in_ = true;
    if (logged_in_ && !was) host::log("slippi: logged in as %s (%s)", info_.display_name.c_str(), info_.connect_code.c_str());
  } else {
    info_ = UserInfo();
    logged_in_ = false;
  }
  if (info_.uid != previous_uid || info_.play_key != previous_key) ResetOnlineCompatibility();
  return logged_in_;
}
void User::LogOut() { std::lock_guard<std::mutex> lock(mutex_); ResetOnlineCompatibility(); logged_in_ = false; info_ = UserInfo(); host::log("slippi: logged out (user.json left in place)"); }
std::vector<std::string> User::GetUserChatMessages() const { UserInfo me = GetUserInfo(); return me.chat_messages.size() == 16 ? me.chat_messages : GetDefaultChatMessages(); }
std::vector<std::string> User::GetDefaultChatMessages() {
  return {"ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
          "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection"};
}

DirectCodes::DirectCodes(std::string path) : path_(std::move(path)) { Load(); }
void DirectCodes::Load() {
  codes_.clear();
  std::ifstream f(path_);
  if (!f) return;
  try {
    json j = json::parse(f);
    // A code holding U+FFFD (EF BF BD) is one the Linux build before 0.9.53 saved as raw Shift-JIS:
    // it can only autofill as garbled letters, so it is dropped here and gone at the next save.
    int dropped = 0;
    if (j.is_array()) for (auto& e : j) {
      std::string c = e.is_object() ? str_or(e, "connectCode", "") : e.is_string() ? e.get<std::string>() : "";
      if (c.find("\xEF\xBF\xBD") != std::string::npos) { ++dropped; continue; }
      c = normalize_history_code(c);
      if (!c.empty() && std::find(codes_.begin(), codes_.end(), c) == codes_.end()) codes_.push_back(c);
    }
    if (dropped) host::log("slippi: %d saved connect codes in %s were unreadable (saved by an older Linux build) and were dropped",
                           dropped, std::filesystem::u8path(path_).filename().u8string().c_str());
  } catch (...) {}
}
void DirectCodes::Save() {
  json j = json::array();
  for (auto& c : codes_) j.push_back({{"connectCode", c}, {"lastPlayed", ""}});
  std::ofstream f(path_);
  if (f) f << j.dump(2);
}
std::string DirectCodes::get(int index) const { return index >= 0 && index < (int)codes_.size() ? codes_[index] : "1"; }
void DirectCodes::AddOrUpdateCode(const std::string& input) {
  const std::string code = normalize_history_code(input);
  if (code.empty()) return;
  codes_.erase(std::remove(codes_.begin(), codes_.end(), code), codes_.end());
  codes_.insert(codes_.begin(), code);
  Save();
}

// ---------------------------------------------------------------- netplay client
NetplayClient::NetplayClient(std::vector<std::string> addrs, std::vector<uint16_t> ports, uint8_t remote_player_count, uint16_t local_port,
                             bool is_decider, uint8_t player_idx, std::vector<std::string> public_fallbacks) {
  public_fallbacks_ = std::move(public_fallbacks);
  host::log("slippi: netplay client: decider %d, player index %u, %u remote players", is_decider, player_idx, remote_player_count);
  host::skins::reset();   // opponents' skins belong to one client's opponents
  host::online_fighters::reset();   // and so do their lists of added fighters
  is_decider_ = is_decider;
  // Every per-remote array here has REMOTE_PLAYER_MAX entries and every per-player one
  // PLAYER_COUNT_MAX; a count or index past them (or past the address lists) is never used.
  const size_t usable = std::min<size_t>({(size_t)remote_player_count, (size_t)REMOTE_PLAYER_MAX, addrs.size(), ports.size()});
  if (usable != remote_player_count) host::log("slippi: netplay client: %u remote players asked, %zu usable", remote_player_count, usable);
  remote_player_count = (uint8_t)usable;
  remote_player_count_ = remote_player_count;
  player_idx_ = player_idx;
  if (player_idx >= PLAYER_COUNT_MAX) {
    host::log("slippi: netplay client: invalid player index %u", player_idx);
    status_.store(ConnectStatus::FAILED);
    return;
  }
  for (int i = 0, j = 0; i < REMOTE_PLAYER_MAX; ++i, ++j) {
    if (j == player_idx) ++j;
    match_info_.remote[i] = PlayerSelections();
    match_info_.remote[i].player_idx = (uint8_t)j;
    last_frame_acked_[i] = 0;
  }
  ENetAddress local_addr;
  ENetAddress* local = nullptr;
  if (local_port > 0) { local_addr.host = ENET_HOST_ANY; local_addr.port = local_port; local = &local_addr; }
  client_ = enet_host_create(local, 10, 3, 0, 0);
  if (!client_) {
    host::log("slippi: cannot create ENet client"); status_.store(ConnectStatus::FAILED); return;
  }
  if (test_loss_pct() > 0) {
    client_->intercept = test_loss_intercept;
    host::log("slippi: MELEE_NET_LOSS_PCT: dropping %u%% of received datagrams (testing)", test_loss_pct());
  }
  for (int i = 0; i < remote_player_count; ++i) {
    ENetAddress addr;
    enet_address_set_host(&addr, addrs[i].c_str());
    addr.port = ports[i];
    ENetPeer* peer = enet_host_connect(client_, &addr, 3, 0);
    server_.push_back(peer);
    if (!peer) { host::log("slippi: cannot create peer"); continue; }
    ActiveConnectionInfo info;
    info.player_idx = match_info_.remote[i].player_idx;
    active_connections_[peer_key(peer)][peer] = info;
    player_active_[info.player_idx].store(true, std::memory_order_release);
  }
  status_.store(ConnectStatus::INITIATED, std::memory_order_release);
  thread_ = std::thread(&NetplayClient::ThreadFunc, this);
}

int NetplayClient::SenderPort(ENetPeer* peer) const {
  for (const auto& connection : active_connections_)
    for (const auto& entry : connection.second)
      if (entry.first == peer && !entry.second.is_disconnected) return entry.second.player_idx;
  return -1;
}

NetplayClient::~NetplayClient() {
  do_loop_.store(false);
  if (client_) wakeup_thread(client_);
  if (thread_.joinable()) thread_.join();
  if (!server_.empty()) Disconnect();
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
  host::log("slippi: netplay client cleanup complete");
}

void NetplayClient::OnData(Packet& packet, ENetPeer* peer) {
  uint8_t mid = 0;
  if (!(packet >> mid)) { host::log("slippi: empty netplay packet"); return; }
  // Melee Unlocked 0.9.200 to 0.9.211 number arena room messages 0xE3 and Gecko messages 0xE4 (this build's
  // Squad Strike and pick previews): from such a peer they mean what it meant.
  if (mid == NP_MSG_MU_LEGACY_ARENA || mid == NP_MSG_MU_PICK_PREVIEW) {
    std::lock_guard<std::mutex> lk(build_mutex_);
    if (remote_player_count_ == 1 && remote_build_[0].received && remote_build_[0].layout == 0)
      mid = mid == NP_MSG_MU_LEGACY_ARENA ? (uint8_t)NP_MSG_MU_ARENA : (uint8_t)NP_MSG_MU_GECKO;
  }
  // Accept the older warm-up ID only from a peer that negotiated that layout.
  if (mid == NP_MSG_MU_WARMUP_UPSTREAM && packet.size() > 3 && packet.data()[3] == 'W') {
    const int sender = SenderPort(peer);
    const uint8_t pidx = sender >= 0 ? PlayerIdxFromPort((uint8_t)sender) : (uint8_t)0xFF;
    std::lock_guard<std::mutex> lk(build_mutex_);
    if (pidx < remote_player_count_ && remote_build_[pidx].received && remote_build_[pidx].layout == 0 && remote_build_[pidx].warmup)
      mid = (uint8_t)NP_MSG_MU_WARMUP;
  }
  switch (mid) {
    case NP_MSG_SLIPPI_PAD: {
      uint64_t cur_time = time_us();
      int32_t frame = 0, checksum_frame = 0; uint32_t checksum = 0; uint8_t packet_player_port = 255;
      if (!(packet >> frame >> packet_player_port >> checksum_frame >> checksum)) break;
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) { host::log("slippi: pad packet with invalid player idx %u", pidx); break; }
      // The other players were told which inputs this side has of a dropped player: that cannot change any more.
      if (packet_player_port >= PLAYER_COUNT_MAX || !player_active_[packet_player_port].load(std::memory_order_acquire)) break;
      int conn_idx = 0;
      for (int i = 0; i < (int)server_.size(); ++i)
        if (peer->address.host == server_[i]->address.host && peer->address.port == server_[i]->address.port) { conn_idx = i; break; }
      std::string key = peer_key(peer);
      int live = 0; bool current_active = false;
      for (auto& c : active_connections_[key]) { if (c.second.is_disconnected) continue; if (c.first == peer) current_active = true; ++live; }
      if (current_active && live > 1 && player_idx_ < packet_player_port) {
        server_[conn_idx] = peer;
        for (auto& c : active_connections_[key]) {
          if (c.first == peer || c.second.is_disconnected) continue;
          enet_peer_disconnect(c.first, 0);
          c.second.is_disconnected = true;
        }
      }
      {
        // The game thread writes the frame timing when it sends its pad and reads the offsets for
        // time sync: both sides under timing_mutex_.
        std::lock_guard<std::mutex> lk(timing_mutex_);
        FrameTiming timing = last_frame_timing_[pidx];
        if (!has_game_started_) { timing.frame = 0; timing.time_us = cur_time; }
        int64_t opponent_send_time_us = (int64_t)cur_time - (int64_t)(ping_us_[pidx] / 2);
        int64_t frame_diff_offset_us = 16683 * ((int64_t)timing.frame - frame);
        int64_t time_offset_us = opponent_send_time_us - (int64_t)timing.time_us + frame_diff_offset_us;
        auto& fod = frame_offset_data_[pidx];
        if ((int)fod.buf.size() < ONLINE_LOCKSTEP_INTERVAL) fod.buf.push_back((int32_t)time_offset_us);
        else fod.buf[fod.idx] = (int32_t)time_offset_us;
        fod.idx = (fod.idx + 1) % ONLINE_LOCKSTEP_INTERVAL;
      }
      int64_t inputs_to_copy;
      {
        std::lock_guard<std::mutex> lk(pad_mutex_);
        const auto received = receive::PadInputs(frame, packet.data(), packet.size(), remote_pads_[packet_player_port], &remote_pad_history_[packet_player_port]);
        inputs_to_copy = received.inputs;
        if (received.error == receive::PadError::TooSmall) { host::log("slippi: pad packet too small for %lld inputs", (long long)inputs_to_copy); break; }
        if (received.error == receive::PadError::TooMany) { host::log("slippi: pad packet with too many frames (%lld)", (long long)inputs_to_copy); break; }
        remote_checksums_[pidx] = {checksum_frame, checksum};
      }
      // Input suggestions check their predictions against the real inputs, oldest first.
      static const bool trace_suggestions = [] { const char* v = host::test_env::getenv("MELEE_EARLY_TRACE"); return v && *v == '1'; }();
      const uint64_t mistakes_before = trace_suggestions ? suggestions::mistakes(pidx, nullptr) : 0;
      for (int64_t i = inputs_to_copy - 1; i >= 0; --i)
        if (suggestions::on_remote_input(pidx, (int32_t)((int64_t)frame - i), packet.data() + 14 + i * PAD_WIRE_SIZE))
          host::log("slippi: ignoring player %u's input suggestions for the rest of the game from frame %d: they kept disagreeing with the real inputs",
                    packet_player_port, (int32_t)((int64_t)frame - i));
      if (trace_suggestions) {
        suggestions::Remote::Mistake m;
        if (suggestions::mistakes(pidx, &m) != mistakes_before && m.frame < 1200)
          host::log("suggestion wrong: frame %d seq %u cost %u suggested %02x%02x %d,%d %d,%d %u %u previous %02x%02x %d,%d real %02x%02x %d,%d %d,%d %u %u",
                    m.frame, m.seq, m.cost, m.suggested[0], m.suggested[1], (int8_t)m.suggested[2], (int8_t)m.suggested[3], (int8_t)m.suggested[4], (int8_t)m.suggested[5], m.suggested[6], m.suggested[7],
                    m.previous[0], m.previous[1], (int8_t)m.previous[2], (int8_t)m.previous[3],
                    m.real[0], m.real[1], (int8_t)m.real[2], (int8_t)m.real[3], (int8_t)m.real[4], (int8_t)m.real[5], m.real[6], m.real[7]);
      }
      if (inputs_to_copy > 0) {
        Packet ack;
        ack << (uint8_t)NP_MSG_SLIPPI_PAD_ACK << frame << player_idx_;
        ENetPacket* epac = enet_packet_create(ack.data(), ack.size(), ENET_PACKET_FLAG_UNSEQUENCED);
        enet_peer_send(peer, 2, epac);
      }
      break;
    }
    case NP_MSG_SLIPPI_PAD_ACK: {
      std::lock_guard<std::mutex> lk(ack_mutex_);
      int32_t frame; uint8_t packet_player_port;
      if (!(packet >> frame) || !(packet >> packet_player_port)) { host::log("slippi: ack packet too small"); break; }
      uint8_t pidx = PlayerIdxFromPort(packet_player_port);
      if (pidx >= remote_player_count_) break;
      last_frame_acked_[pidx] = std::max(last_frame_acked_[pidx], frame);
      auto& timers = ack_timers_[pidx];
      while (!timers.empty() && timers.front().frame < frame) timers.pop_front();
      if (timers.empty() || timers.front().frame != frame) break;
      uint64_t send_time = timers.front().time_us;
      timers.pop_front();
      ping_us_[pidx] = time_us() - send_time;
      ping_sample_sum_us_.fetch_add(ping_us_[pidx], std::memory_order_relaxed);
      ping_sample_count_.fetch_add(1, std::memory_order_relaxed);
      if (pidx == 0) last_ping_ms_.store((uint32_t)(ping_us_[pidx] / 1000), std::memory_order_relaxed);
      remote_ping_ms_[pidx].store((uint32_t)(ping_us_[pidx] / 1000), std::memory_order_relaxed);
      remote_count_seen_.store(remote_player_count_, std::memory_order_relaxed);
      if (frame % 600 == 0 && pidx == 0) host::log("slippi: ping %llu ms", (unsigned long long)(ping_us_[0] / 1000));
      break;
    }
    case NP_MSG_SLIPPI_PAD_SUGGESTION: {
      // Slippi 3.7.0 input suggestions: the sender's newest input between frames, for predicting its
      // not yet received frames (slippi_input_suggestions.h). The real input still decides every frame.
      uint8_t port; int32_t target; uint32_t seq;
      constexpr size_t pad_offset = 10;   // message id, port, target frame and seq
      if (!(packet >> port) || !(packet >> target) || !(packet >> seq) || packet.size() < pad_offset + (size_t)PAD_DATA_SIZE) { host::log("slippi: input suggestion packet too small"); break; }
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (port == player_idx_ || pidx >= remote_player_count_) { host::log("slippi: input suggestion with invalid player idx %u", pidx); break; }
      suggestions::on_remote_suggestion(pidx, target, seq, packet.data() + pad_offset);
      break;
    }
    case NP_MSG_SLIPPI_PLAYER_DROP: {
      uint8_t sender_port, dropped_port, input_count; int32_t last_frame;
      if (!(packet >> sender_port) || !(packet >> dropped_port) || !(packet >> last_frame) || !(packet >> input_count)) { host::log("slippi: player drop packet too small"); break; }
      constexpr size_t inputs_offset = 8;   // message id, ports, last frame and input count
      if (packet.size() < inputs_offset + (size_t)input_count * PAD_WIRE_SIZE) { host::log("slippi: player drop packet too small for its inputs"); break; }
      if (sender_port == player_idx_ || sender_port == dropped_port || last_frame < 0 || sender_port >= PLAYER_COUNT_MAX || dropped_port >= PLAYER_COUNT_MAX ||
          PlayerIdxFromPort(sender_port) >= remote_player_count_ || (dropped_port != player_idx_ && PlayerIdxFromPort(dropped_port) >= remote_player_count_)) {
        host::log("slippi: invalid player drop packet (sender %u, dropped %u, last frame %d)", sender_port, dropped_port, last_frame);
        break;
      }
      // The others remove this side if it is about us. A player already lost has no say, or two players
      // that lost each other would take each other out.
      if (dropped_port == player_idx_ || !player_active_[sender_port].load(std::memory_order_acquire)) break;
      // Losing a player to anyone is losing them to everyone. This also reports what we have of them.
      ForceDisconnectPlayer(dropped_port);
      ProcessPlayerDrops();
      const int32_t despawn = despawn_frames_[dropped_port].load(std::memory_order_acquire);
      if (despawn != 0) {
        if (last_frame + DROP_DESPAWN_FRAMES > despawn) host::log("slippi: report about player %u came too late (last frame %d, despawn frame %d)", dropped_port, last_frame, despawn);
        break;
      }
      AddPlayerDropReportInputs(dropped_port, last_frame, input_count, packet.data() + inputs_offset);
      break;
    }
    case NP_MSG_SLIPPI_MATCH_SELECTIONS: {
      auto s = ReadSelections(packet);
      if (!s->error) {
        uint8_t idx = PlayerIdxFromPort(s->player_idx);
        if (idx >= remote_player_count_) break;
        // A seat's picks and pad queues change only by its own connection (the review of 2026-11-24): a
        // selection naming another seat than the one whose address it came from is dropped whole. A
        // connection at none of the seats' addresses (translated on the way) is not judged.
        int from_seat = -1;
        for (int i = 0; i < remote_player_count_ && i < (int)server_.size(); ++i)
          if (server_[i] && peer->address.host == server_[i]->address.host && peer->address.port == server_[i]->address.port) {
            from_seat = i;
            break;
          }
        if (from_seat >= 0 && from_seat != idx) {
          host::log("slippi: selections for player %u from the connection of player %u dropped", s->player_idx,
                    match_info_.remote[from_seat].player_idx);
          break;
        }
        host::log("slippi: received selections from player %u (char %u color %u stage %u)", s->player_idx, s->character_id, s->character_color, s->stage_id);
        {
          std::scoped_lock lock(selection_mutex_, timing_mutex_, pad_mutex_);
          receive::Selections(*s, match_info_.remote[idx], remote_game_started_, idx,
            remote_pads_[s->player_idx], early_pads_[idx]);
          if (s->player_idx < PLAYER_COUNT_MAX && remote_pads_[s->player_idx].empty()) {
            remote_pad_history_[s->player_idx].clear();
            suggestions::reset_remote(idx);
          }
          has_game_started_ = false;
        }
        // Opponents' skins: this pick takes the skins message that came just before it, or none. Only
        // a pick from the connection that negotiated its port.
        if (s->is_character_selected && host::skins::enabled() && SenderPort(peer) == s->player_idx) {
          std::vector<skin_wire::Record> records;
          {
            std::lock_guard<std::mutex> lk(build_mutex_);
            skin_wire::Message m;
            if (skins_pending_set_[idx] && skin_wire::decode(skins_pending_[idx].data(), skins_pending_[idx].size(), &m, nullptr))
              records = std::move(m.records);
            skins_pending_set_[idx] = false;
            skins_pending_[idx].clear();
          }
          host::skins::opponent_selected(s->player_idx, s->character_id, s->character_color, records);
        }
      }
      break;
    }
    case NP_MSG_SLIPPI_CHAT_MESSAGE: {
      auto s = ReadChatMessage(packet);
      if (!s->error) { std::lock_guard<std::mutex> lk(chat_mutex_); remote_chat_message_selection_ = std::move(s); }
      break;
    }
    case NP_MSG_MU_BUILD: {
      // 'M' 'U' 'B' version, player port, mod view, fingerprint length + bytes, name length + bytes.
      uint8_t m, u, b, version, port, mod_view, fp_len, name_len;
      if (!(packet >> m >> u >> b >> version >> port >> mod_view >> fp_len) || m != 'M' || u != 'U' || b != 'B' || version != 1) {
        host::log("slippi: invalid build message");
        break;
      }
      std::string fp, name;
      for (uint8_t i = 0; i < fp_len; ++i) { uint8_t ch; if (!(packet >> ch)) break; fp.push_back((char)ch); }
      if (packet >> name_len)
        for (uint8_t i = 0; i < name_len; ++i) { uint8_t ch; if (!(packet >> ch)) break; name.push_back((char)ch); }
      // Capabilities, a byte builds before early inputs did not send: absent means none.
      uint8_t caps = 0;
      if (!(packet >> caps)) caps = 0;
      const uint8_t caps_sent = caps;   // as sent: the layout below decides what each bit means
      std::string rules_digest;
      if (caps & 8) {
        for (int i = 0; i < 64; ++i) { uint8_t ch; if (!(packet >> ch)) break; rules_digest.push_back((char)ch); }
        if (!online::native_mod_fingerprint_valid(rules_digest)) rules_digest = "invalid";
      }
      // Their own shield colour, which builds before it do not send.
      uint8_t has_shield = 0, sr = 0, sg = 0, sb = 0;
      uint32_t shield = 0;
      if ((packet >> has_shield) && has_shield == 1 && (packet >> sr >> sg >> sb))
        shield = 0x1000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
      // Then the second capability byte (the first is full), which builds before it do not send.
      uint8_t caps2 = 0;
      if (!(packet >> caps2)) caps2 = 0;
      // The third capability byte (this build on): Squad Strike, its friend rooms, pick previews and the
      // setup hash, so the first byte keeps the meaning Melee Unlocked 0.9.200 to 0.9.211 give it.
      uint8_t caps3 = 0;
      const bool has_caps3 = caps2 && (packet >> caps3);
      uint8_t colour1 = 0, colour2 = 0;
      const bool has_colours = has_caps3 && (packet >> colour1) && (packet >> colour2);
      // Then the product and its version (SendBuild), which Melee Unlocked does not send: read only when
      // both are there and look like one (lowercase word, version of digits and dots).
      std::string product, product_version;
      {
        const auto read_word = [&packet](std::string* out, bool version) {
          uint8_t len = 0;
          if (!(packet >> len) || len == 0 || len > 32) return false;
          for (uint8_t i = 0; i < len; ++i) {
            uint8_t ch;
            if (!(packet >> ch)) return false;
            const bool ok = version ? ((ch >= '0' && ch <= '9') || ch == '.') : ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-');
            if (!ok) return false;
            out->push_back((char)ch);
          }
          return true;
        };
        if (!(has_colours && read_word(&product, false) && read_word(&product_version, true))) product.clear(), product_version.clear();
      }
      // Then the fourth capability byte (MU_BUILD_CAPS4_*, Nucleus 1.0.6 on), read only after a whole
      // product and version; builds before it do not send it.
      uint8_t caps4 = 0;
      const bool has_caps4 = !product_version.empty() && (packet >> caps4);
      const uint8_t layout = !caps2 ? 0 : has_caps3 ? 2 : 1;
      // Melee Unlocked 0.9.200 to 0.9.211: 0x10 arena rooms, 0x20 automatic L-cancel, 0x40 Gecko codes.
      const bool upstream_lcancel = !caps2 && (caps & 0x20),
                 upstream_gecko = !caps2 && (caps & 0x40);
      // Warm-up messages, decided by the layout this peer speaks: Melee Unlocked 0.9.215 and 0.9.216 flag
      // them with first byte bit 0x80 (their id 0xE5), builds with the third byte with caps3 bit 0x10 (id
      // 0xE9). A legacy 1xx build reads 0x80 as Squad Strike friend rooms and has no warm-up: never.
      const bool warmup = layout == 0 ? (caps_sent & MU_BUILD_CAPS_UPSTREAM_WARMUP) != 0
                        : layout == 2 ? (caps3 & MU_BUILD_CAPS3_WARMUP) != 0 : (caps2 & MU_BUILD_CAPS2_WARMUP) != 0;
      // This build's first byte carries the old meanings too: its own flags are taken from the third byte,
      // rewritten into the bits the code below reads (a legacy 1xx build's layout).
      if (has_caps3)
        caps = (uint8_t)((caps & 0x0F) |
                         ((caps3 & 0x04) ? MU_PICK_PREVIEW_CAP : 0) | ((caps3 & 0x08) ? MU_SETUP_HASH_CAP : 0));
      // Upstream Melee Unlocked (0.9.200 on) sets caps 0x10 (arena rooms) and 0x20 (automatic L-cancel),
      // 0.9.205 also 0x40 (Gecko codes), and sends no second byte; every extended build sends a non-zero
      // one. Without it those bits are upstream's, never Squad Strike, its friend rooms, pick previews
      // or the setup hash (a setup hash wait on an upstream peer would never end).
      if (!caps2) caps &= (uint8_t)~(0x10u | MU_PICK_PREVIEW_CAP | MU_SETUP_HASH_CAP | 0x80u);   // + MU_SQUAD_CAP, MU_SQUAD_FRIEND_CAP
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (pidx >= remote_player_count_) { host::log("slippi: build message with invalid player idx %u", pidx); break; }
      // The connection that negotiated the port owns its build capabilities.
      if (SenderPort(peer) != port) break;
      {
        std::lock_guard<std::mutex> lk(build_mutex_);
        remote_build_[pidx] = {true, mod_view != 0, fp, name, (caps & 1) != 0, (caps & 2) != 0, shield, port, (caps & 4) != 0, rules_digest};
        remote_build_[pidx].setup_hash = (caps & MU_SETUP_HASH_CAP) != 0;
        remote_build_[pidx].pick_previews = (caps & MU_PICK_PREVIEW_CAP) != 0;
        remote_build_[pidx].caps2 = caps2;
        // Upstream flags these with caps 0x10/0x20, which this build reads as Squad Strike and pick
        // previews: here they ride the second capability byte.
        remote_build_[pidx].auto_lcancel = (caps2 & MU_BUILD_CAPS2_AUTO_LCANCEL) != 0 || upstream_lcancel;
        remote_build_[pidx].gecko = (caps2 & MU_BUILD_CAPS2_GECKO) != 0 || upstream_gecko;
        remote_build_[pidx].layout = layout;
        remote_build_[pidx].warmup = warmup;
        remote_build_[pidx].pause_rule = has_caps3 && (caps3 & MU_BUILD_CAPS3_PAUSE_RULE) != 0;
        remote_build_[pidx].no_pause = remote_build_[pidx].pause_rule && (caps3 & MU_BUILD_CAPS3_NO_PAUSE) != 0;
        remote_build_[pidx].port_colour[0] = has_colours && colour1 >= 1 && colour1 <= 4 ? (int8_t)(colour1 - 1) : (int8_t)-1;
        remote_build_[pidx].port_colour[1] = has_colours && colour2 >= 1 && colour2 <= 4 ? (int8_t)(colour2 - 1) : (int8_t)-1;
        remote_build_[pidx].skins = has_caps3 && (caps3 & MU_BUILD_CAPS3_SKINS) != 0;
        remote_build_[pidx].product = product;
        remote_build_[pidx].product_version = product_version;
        remote_build_[pidx].items_rule = has_caps4 && (caps4 & MU_BUILD_CAPS4_ITEMS) != 0;
        remote_build_[pidx].items_choice = -1;
        remote_build_[pidx].custom_fighters = has_caps4 && (caps4 & MU_BUILD_CAPS4_CUSTOM_FIGHTERS) != 0;
        remote_build_[pidx].squad_rule = has_caps4 && (caps4 & MU_BUILD_CAPS4_SQUAD) != 0;   // Squad Strike in Direct
        remote_build_[pidx].squad_choice = -1;   // its NP_MSG_MU_SQUAD follows (SquadPending until then)
      }
      host::log("setup hash: opponent %u %s", port, (caps & MU_SETUP_HASH_CAP) ? "sends a setup hash" : "has no setup hash (not checked)");
      host::log("slippi: opponent %u build: %s%s%s%s, shield colour %s%06X", port, mod_view ? "mod " : "retail game",
                mod_view ? name.c_str() : "", mod_view ? (" " + fp.substr(0, 16)).c_str() : "",
                (caps & 1) ? ", takes early inputs" : "", shield ? "" : "none ", shield & 0xFFFFFFu);
      if ((caps2 & MU_BUILD_CAPS2_AUTO_LCANCEL) || upstream_lcancel) host::log("slippi: opponent %u has automatic L-cancel on", port);
      if (layout != 2) host::log("slippi: opponent %u speaks the %s build message", port, layout ? "legacy 1xx" : "Melee Unlocked 0.9.216 and older");
      if (has_caps3 && (caps3 & MU_BUILD_CAPS3_SKINS)) host::log("slippi: opponent %u takes skins messages", port);
      if (!product.empty()) host::log("slippi: opponent %u product: %s %s", port, product.c_str(), product_version.c_str());
      else host::log("slippi: opponent %u product: none named (%s)", port, product::kUpstreamName);
      host::log("slippi: opponent %u %s", port, has_caps4 && (caps4 & MU_BUILD_CAPS4_ITEMS) ? "reads the items rule" : "has no items rule (items stay off)");
      host::log("slippi: opponent %u %s", port, has_caps4 && (caps4 & MU_BUILD_CAPS4_CUSTOM_FIGHTERS) ? "takes custom fighters"
                                                                                                    : "takes no custom fighters");
      if (caps & 2) SendPrematchPing();   // at once: a match set up right after connecting needs a ping now
      // Our Items choice at once (after our build on the same ordered queue), so its select shows it.
      if (has_caps4 && (caps4 & MU_BUILD_CAPS4_ITEMS)) SendItemsChoice();
      // Squad Strike in Direct: our choice and lineup at once too.
      if (has_caps4 && (caps4 & MU_BUILD_CAPS4_SQUAD)) {
        host::log("slippi: opponent %u plays Squad Strike in Direct", port);
        SendSquad();
      }
      break;
    }
    case NP_MSG_MU_EARLY_PAD: {
      // 'M' 'U' 'E' version, player port, target pad frame, the eight pad bytes. Taken only from a
      // peer that announced early inputs, and only while this side uses them.
      uint8_t m, u, e, version, port;
      int32_t target = 0;
      if (!(packet >> m >> u >> e >> version >> port >> target) || m != 'M' || u != 'U' || e != 'E' || version != 1) {
        host::log("slippi: invalid early input message");
        break;
      }
      uint8_t pad[host::early_input::kPadBytes];
      bool ok = true;
      for (uint8_t& b : pad) if (!(packet >> b)) { ok = false; break; }
      if (!ok) { host::log("slippi: early input message too small"); break; }
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (pidx >= remote_player_count_) break;
      {
        std::lock_guard<std::mutex> lk(build_mutex_);
        if (!remote_build_[pidx].early_pads) break;
      }
      if (!online::early_inputs_accept()) break;
      host::early_input::clamp_rest(pad);
      {
        std::lock_guard<std::mutex> lk(pad_mutex_);
        early_pads_[pidx].add(target, pad);
      }
      host::early_input::note_received();
      break;
    }
    case NP_MSG_MU_PING: {
      // 'M' 'U' 'P' version, kind (0 request, 1 reply), sender port, the requester's time stamp (us).
      uint8_t m, u, pc, version, kind, port;
      uint32_t stamp = 0;
      if (!(packet >> m >> u >> pc >> version >> kind >> port >> stamp) || m != 'M' || u != 'U' || pc != 'P' || version != 1) break;
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (pidx >= remote_player_count_) break;
      if (kind == 0) {
        auto p = std::make_unique<Packet>();
        *p << (uint8_t)NP_MSG_MU_PING << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'P' << (uint8_t)1 << (uint8_t)1 << player_idx_ << stamp;
        Send(*p);   // network thread: straight out
      } else if (pidx == 0 && (uint32_t)time_us() - stamp < 2000000u) {
        prematch_samples_.push_back(((uint32_t)time_us() - stamp) / 1000);
        while (prematch_samples_.size() > 9) prematch_samples_.pop_front();
        std::vector<uint32_t> s(prematch_samples_.begin(), prematch_samples_.end());
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const uint32_t was = prematch_ping_ms_.exchange(std::max<uint32_t>(1, s[s.size() / 2]), std::memory_order_relaxed);
        if (!was) host::log("slippi: pre-match ping %u ms", s[s.size() / 2]);
      }
      break;
    }
    case NP_MSG_MU_SETUP_HASH: {
      // 'M' 'U' 'H' version, sender port, inputs hash, setup hash (mu_setup_hash.h), nothing more.
      MuSetupHash h{};
      if (packet.size() < 1 || !mu_setup_hash_decode(packet.data() + 1, packet.size() - 1, &h)) {
        host::log("setup hash: malformed message rejected (%u bytes)", (unsigned)packet.size());
        break;
      }
      const uint8_t pidx = PlayerIdxFromPort(h.port);
      // Bind the hash to ENet's sender, never only to the port the payload claims.
      bool sender = h.port != player_idx_ && pidx < remote_player_count_;
      if (sender) {
        sender = false;
        for (const auto& connection : active_connections_)
          for (const auto& entry : connection.second)
            if (entry.first == peer && !entry.second.is_disconnected && entry.second.player_idx == h.port) sender = true;
      }
      if (!sender) { host::log("setup hash: message for port %u not from that peer, rejected", h.port); break; }
      std::lock_guard<std::mutex> lk(build_mutex_);
      setup_hash_[pidx] = {true, h};
      break;
    }
    case NP_MSG_MU_PICK_PREVIEW: {
      // Display only. The sender is ENet's peer, never a port claimed in the payload.
      uint8_t sender = 0xFF;
      for (const auto& connection : active_connections_)
        for (const auto& entry : connection.second)
          if (entry.first == peer && !entry.second.is_disconnected) sender = entry.second.player_idx;
      if (sender >= PLAYER_COUNT_MAX || sender == player_idx_) break;
      const uint8_t pidx = PlayerIdxFromPort(sender);
      if (pidx >= remote_player_count_) break;
      MuPickPreview pick{};
      if (packet.size() <= 1 || !mu_pick_preview_decode(packet.data() + 1, packet.size() - 1, &pick)) {
        host::log("slippi: invalid pick preview from port %u", sender);
        break;
      }
      std::lock_guard<std::mutex> lk(pick_mutex_);
      pick_remote_[pidx] = pick;
      pick_remote_port_[pidx] = sender;
      pick_remote_set_[pidx] = true;
      break;
    }
    case NP_MSG_MU_ARENA:
      break;   // an arena room message (reserved id): this build has no arena rooms
    case NP_MSG_MU_GECKO: {
      // A gecko_online message ('M' 'U' 'G' ...), from a peer that announced it takes them. Kept as is
      // for the game thread, which decodes and checks it (gecko_online::decode).
      {
        std::lock_guard<std::mutex> lk(build_mutex_);
        if (!remote_build_[0].gecko) break;
      }
      const size_t head = 1;   // the id
      if (packet.size() <= head || packet.size() > 64 * 1024) break;
      std::lock_guard<std::mutex> lk(gecko_mutex_);
      if (gecko_inbox_.size() >= 16) { host::log("slippi: gecko inbox full, message dropped"); break; }
      gecko_inbox_.emplace_back(packet.data() + head, packet.data() + packet.size());
      break;
    }
    case NP_MSG_MU_WARMUP:
      break;   // a warm-up message (reserved id; 0xE5 from 0.9.215 and 0.9.216 arrives translated): no warm-up room here
    case NP_MSG_MU_SKINS: {
      // Opponents' skins (mu_skin_wire.h): kept for the sender's next 0x82, which applies it. Bound to
      // ENet's sender: the header and every record must name the port that connection negotiated.
      const int sender = SenderPort(peer);
      const uint8_t pidx = sender >= 0 ? PlayerIdxFromPort((uint8_t)sender) : (uint8_t)REMOTE_PLAYER_MAX;
      if (!host::skins::enabled()) { host::log("opponent skins: message from port %d ignored (opponent skins off)", sender); break; }
      skin_wire::Message m;
      std::string why;
      if (sender < 0 || sender == player_idx_ || pidx >= remote_player_count_ || packet.size() > 1 + skin_wire::kMaxPayloadBytes ||
          !skin_wire::decode(packet.data() + 1, packet.size() - 1, &m, &why) || !skin_wire::bound_to(m, sender, &why)) {
        host::log("opponent skins: message from port %d rejected (%u bytes): %s", sender, (unsigned)packet.size(),
                  why.empty() ? "not from a remote player" : why.c_str());
        break;
      }
      host::log("opponent skins: port %d sent %zu skin record(s) for its next pick", sender, m.records.size());
      std::lock_guard<std::mutex> lk(build_mutex_);
      skins_pending_[pidx].assign(packet.data() + 1, packet.data() + packet.size());
      skins_pending_set_[pidx] = true;
      break;
    }
    case NP_MSG_MU_ITEMS: {
      // 'M' 'U' 'I' version, sender port, choice. Bound to ENet's sender, and only from a peer whose
      // build said it reads the rule.
      uint8_t m, u, ic, version, port, choice;
      if (!(packet >> m >> u >> ic >> version >> port >> choice) || m != 'M' || u != 'U' || ic != 'I' || version != 1 || choice > 1) {
        host::log("slippi: invalid items message (%u bytes)", (unsigned)packet.size());
        break;
      }
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (port == player_idx_ || pidx >= remote_player_count_ || SenderPort(peer) != port) {
        host::log("slippi: items message for port %u not from that peer, dropped", port);
        break;
      }
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (!remote_build_[pidx].items_rule) { host::log("slippi: items message from port %u, whose build has no items rule, dropped", port); break; }
      if (remote_build_[pidx].items_choice != (int8_t)choice) host::log("slippi: opponent %u has Items %s", port, choice ? "on" : "off");
      remote_build_[pidx].items_choice = (int8_t)choice;
      break;
    }
    case NP_MSG_MU_CONTENT: {
      // Custom fighters online (mu_content_wire.h): the sender's list of added fighters. Bound to ENet's
      // sender; a later list replaces an earlier one.
      const int sender = SenderPort(peer);
      const uint8_t pidx = sender >= 0 ? PlayerIdxFromPort((uint8_t)sender) : (uint8_t)REMOTE_PLAYER_MAX;
      const uint8_t* body = packet.data() + 1;
      const size_t size = packet.size() - 1;
      std::string why;
      if (!host::online_fighters::enabled()) break;   // a build without custom fighters (the test switch)
      if (sender < 0 || sender == player_idx_ || pidx >= remote_player_count_ || size > content_wire::kMaxPayloadBytes) {
        host::log("custom fighters: message from port %d rejected (%zu bytes): not from a remote player", sender, size);
        break;
      }
      content_wire::Have have;
      if (content_wire::kind_of(body, size) != content_wire::kKindHave || !content_wire::decode_have(body, size, &have, &why) ||
          !content_wire::bound_to(have, sender, &why)) {
        host::log("custom fighters: message from port %d rejected (%zu bytes): %s", sender, size,
                  why.empty() ? "not a fighter list this build reads" : why.c_str());
        break;
      }
      host::online_fighters::peer_have(have);
      break;
    }
    case NP_MSG_MU_SQUAD: {
      // 'M' 'U' 'Q' version, sender port, choice, count, count * {kind, color}. Bound to ENet's sender,
      // only from a peer whose build said it plays Squad Strike in Direct, and a lineup only as
      // mu_squad_lineup_valid allows (count 0: none picked yet).
      uint8_t m, u, q, version, port, choice, count, pairs[8] = {};
      bool ok = (packet >> m >> u >> q >> version >> port >> choice >> count) && m == 'M' && u == 'U' && q == 'Q' &&
                version == 1 && choice <= 1 && count <= 4;
      for (uint8_t i = 0; ok && i < 2 * count; ++i) ok = (bool)(packet >> pairs[i]);
#if MU_SQUAD
      if (ok && count) {
        MuSquadSide side{};
        side.count = count;
        for (uint8_t i = 0; i < count; ++i) side.members[i] = {pairs[2 * i], pairs[2 * i + 1]};
        ok = mu_squad_lineup_valid(&side) != 0;
      }
#else
      ok = false;   // a build without Squad Strike never offers it, so nobody should send it here
#endif
      if (!ok) {
        host::log("slippi: invalid squad message (%u bytes)", (unsigned)packet.size());
        break;
      }
      const uint8_t pidx = PlayerIdxFromPort(port);
      if (port == player_idx_ || pidx >= remote_player_count_ || SenderPort(peer) != port) {
        host::log("slippi: squad message for port %u not from that peer, dropped", port);
        break;
      }
      std::lock_guard<std::mutex> lk(build_mutex_);
      RemoteBuild& b = remote_build_[pidx];
      if (!b.squad_rule) { host::log("slippi: squad message from port %u, whose build has no Squad Strike, dropped", port); break; }
      if (b.squad_choice != (int8_t)choice || b.squad_count != count || std::memcmp(b.squad_lineup, pairs, sizeof pairs) != 0) {
        std::string text;
        for (uint8_t i = 0; i < count; ++i) text += (i ? "," : " ") + std::to_string(pairs[2 * i]) + ":" + std::to_string(pairs[2 * i + 1]);
        host::log("slippi: opponent %u has Squad Strike %s, lineup%s", port, choice ? "on" : "off", count ? text.c_str() : " none yet");
      }
      b.squad_choice = (int8_t)choice;
      b.squad_count = count;
      std::memcpy(b.squad_lineup, pairs, sizeof pairs);
      break;
    }
    case NP_MSG_SLIPPI_CONN_SELECTED: break;
    case NP_MSG_SLIPPI_COMPLETE_STEP: {
      GamePrepStep step{};
      bool valid = true;
      for (auto& b : step) if (!(packet >> b)) { valid = false; break; }
      if (valid) game_prep_.receive(step);
      else host::log("slippi: incomplete GamePrep step");
      break;
    }
    case NP_MSG_SLIPPI_SYNCED_STATE: {
      uint8_t port;
      SyncedGameState state;
      if (!(packet >> port) || port >= PLAYER_COUNT_MAX || port == player_idx_ || SenderPort(peer) != port) break;
      const auto remote = PlayerIdxFromPort(port);
      if (remote >= remote_player_count_ || !(packet >> state.match_id >> state.game_index >>
          state.tiebreak_index >> state.seconds_remaining)) break;
      for (auto& fighter : state.fighters) packet >> fighter.stocks_remaining >> fighter.current_health;
      if (packet) {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        remote_sync_states_[remote] = std::move(state);
      }
      break;
    }
    default: host::log("slippi: unknown netplay message %u", mid); break;
  }
}

void NetplayClient::SendSyncedGameState(const SyncedGameState& state) {
  {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    recovering_ = true;
    local_sync_state_ = state;
  }
  auto packet = std::make_unique<Packet>();
  *packet << uint8_t(NP_MSG_SLIPPI_SYNCED_STATE) << player_idx_ << state.match_id
          << state.game_index << state.tiebreak_index << state.seconds_remaining;
  for (const auto& fighter : state.fighters) *packet << fighter.stocks_remaining << fighter.current_health;
  SendAsync(std::move(packet));
}

DesyncRecoveryResp NetplayClient::GetDesyncRecoveryState() {
  std::lock_guard<std::mutex> lock(sync_mutex_);
  DesyncRecoveryResp result;
  if (!recovering_) return result;
  result.is_recovering = true;
  result.state = local_sync_state_;
  for (unsigned i = 0; i < remote_player_count_; ++i) {
    const auto& other = remote_sync_states_[i];
    auto& state = result.state;
    if (other.match_id != state.match_id || other.game_index != state.game_index ||
        other.tiebreak_index != state.tiebreak_index) {
      result.is_waiting = true;
      return result;
    }
    if (std::max(other.seconds_remaining, state.seconds_remaining) -
        std::min(other.seconds_remaining, state.seconds_remaining) > 1) {
      result.is_error = true;
      return result;
    }
    state.seconds_remaining = std::max(other.seconds_remaining, state.seconds_remaining);
    for (unsigned j = 0; j < 4; ++j) {
      auto& local = state.fighters[j];
      const auto& remote = other.fighters[j];
      if (local.stocks_remaining != remote.stocks_remaining ||
          std::abs(int(local.current_health) - int(remote.current_health)) > 25) {
        result.is_error = true;
        return result;
      }
      local.current_health = std::min(local.current_health, remote.current_health);
    }
  }
  return result;
}

void NetplayClient::SendGamePrepStep(const GamePrepStep& step) {
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_SLIPPI_COMPLETE_STEP;
  for (uint8_t b : step) *p << b;
  SendAsync(std::move(p)); // reliable, ordered channel 0, like Slippi
}

void NetplayClient::SendBuild() {
  // Tests only: behave like Slippi Dolphin, which never sends a build.
  if (host::test_env::getenv("MELEE_TEST_NO_BUILD_MESSAGE")) { host::log("slippi: build message suppressed (test)"); return; }
  const auto& me = slippi::online::local_build();
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_BUILD << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'B' << (uint8_t)1 << player_idx_
     << (uint8_t)(me.mod_view ? 1 : 0);
  const std::string fp = me.mod_view ? me.fingerprint.substr(0, 64) : std::string();
  const std::string name = me.mod_view ? me.name.substr(0, 64) : std::string();
  *p << (uint8_t)fp.size();
  for (char ch : fp) *p << (uint8_t)ch;
  *p << (uint8_t)name.size();
  for (char ch : name) *p << (uint8_t)ch;
  // Capabilities: bit 0, this side takes early inputs. Older builds stop reading before this byte.
  const bool early = online::early_inputs_accept();
  // Bit 1: pre-match pings; bit 2: Static engine; bit 3: ranked rules digest follows.

  uint8_t caps = (uint8_t)((early ? 1 : 0) | 2 | (me.static_engine ? 4 : 0));
  // MU_SETUP_HASH_CAP: this side sends its match setup hash and waits for the peer's. Tests only:
  // MELEE_TEST_NO_SETUP_HASH plays a build from before it (no bit, no hash message).
  const bool setup_hash_on = !host::test_env::getenv("MELEE_TEST_NO_SETUP_HASH");
  if (setup_hash_on) caps |= MU_SETUP_HASH_CAP;
  caps |= MU_PICK_PREVIEW_CAP;   // takes live pick previews (NP_MSG_MU_PICK_PREVIEW)
  // caps3 0x20/0x40: the Direct pause rule and this player's preference (sent once a
  // connection; a change applies from the next one, so both sides always decide from the same bytes).
  const bool no_pause = !online::config().direct_pause.load();
  sent_no_pause_.store(no_pause);
  // caps3 0x80: this side takes skins messages, only while the "opponentskins" setting is on.
  const uint8_t caps3 = (uint8_t)(0x04 | (setup_hash_on ? 0x08 : 0) |
                                  MU_BUILD_CAPS3_PAUSE_RULE | (no_pause ? MU_BUILD_CAPS3_NO_PAUSE : 0) |
                                  (host::skins::enabled() ? MU_BUILD_CAPS3_SKINS : 0));
  // The first byte's high bits stay as they were: 0x10 and 0x80 (upstream's arena and warm-up) also
  // carry other meanings to older readers (a legacy 1xx build's Squad Strike bits). An arena or
  // warm-up message such a peer sends because of them is dropped on arrival.
    caps = (uint8_t)((caps & 0x0F) | 0x10 | (lcancel::automatic_active_online() ? 0x20 : 0) | 0x40 | MU_BUILD_CAPS_UPSTREAM_WARMUP);
  *p << caps;
  // Then the player's own shield colour ("Shield colours", Online): a byte 1 and the three RGB bytes, or
  // a byte 0. Builds before it stop reading at the capabilities byte.
  const uint32_t shield = gx::shield_color_online();
  if (shield) *p << (uint8_t)1 << (uint8_t)(shield >> 16) << (uint8_t)(shield >> 8) << (uint8_t)shield;
  else *p << (uint8_t)0;
  // Older builds stop reading at the shield bytes. MU_BUILD_CAPS2_AUTO_LCANCEL: automatic L-cancel
  // is on for this session (the opponent's character select says so; older builds ignore the bit).
  // MU_BUILD_CAPS2_ARENA and MU_BUILD_CAPS2_WARMUP are no longer set (no arena rooms, no warm-up room).
  *p << (uint8_t)(MU_BUILD_CAPS2_GONE_OWN_FRAMES |
                  (lcancel::automatic_active_online() ? MU_BUILD_CAPS2_AUTO_LCANCEL : 0) |
                  MU_BUILD_CAPS2_GECKO);
  *p << caps3;   // builds before it stop reading at the second byte
  // Then the port colour choices (1.0.2 on): first and second, each the colour + 1 (0 none). Older builds
  // stop reading at the third capability byte.
  {
    const int c0 = std::clamp(online::config().port_colour[0], -1, 3), c1 = std::clamp(online::config().port_colour[1], -1, 3);
    sent_port_colour_.store((c0 & 0xFF) | ((c1 & 0xFF) << 8));
    *p << (uint8_t)(c0 + 1) << (uint8_t)(c1 + 1);
  }
  // Then which product this is and its version (product_identity.h): a byte length and the word, a byte
  // length and the version. Melee Unlocked stops reading at the port colours, so it reads this message
  // as before; Slippi Dolphin ignores the whole message (an unknown id), whatever its length.
  {
#ifdef MELEE_PORT_VERSION
    const std::string version = MELEE_PORT_VERSION;
#else
    const std::string version = "0";
#endif
    const std::string word = product::kWireName;
    *p << (uint8_t)word.size();
    for (char ch : word) *p << (uint8_t)ch;
    *p << (uint8_t)version.size();
    for (char ch : version) *p << (uint8_t)ch;
  }
  // Then the fourth capability byte (1.0.6 on), the one caps4 byte, every bit of it here. Older Nucleus
  // builds stop reading at the version. MU_BUILD_CAPS4_ITEMS: this side reads the items rule.
  // MU_BUILD_CAPS4_CUSTOM_FIGHTERS: custom fighters online (this side sends and reads lists of added
  // fighters), unless they are off (MELEE_TEST_NO_CUSTOM_FIGHTERS plays a Nucleus from before them; a
  // release without Fighters, release_scope.h, sends no bit either).
  // 0x04 (custom stages) stays clear until they are built. MU_BUILD_CAPS4_SQUAD: this side plays Squad
  // Strike in Direct (builds with Squad Strike only).
  uint8_t caps4 = (uint8_t)(MU_BUILD_CAPS4_ITEMS | (host::online_fighters::enabled() ? MU_BUILD_CAPS4_CUSTOM_FIGHTERS : 0));
#if MU_SQUAD
  caps4 |= MU_BUILD_CAPS4_SQUAD;
#endif
  *p << caps4;
  host::log("slippi: sending build: %s%s%s, product %s", me.mod_view ? "mod " : "retail game", me.mod_view ? name.c_str() : "",
            early ? " (takes early inputs)" : "", product::kWireName);
  SendAsync(std::move(p));
}

// A pre-match ping to every opponent that answers them, until in-match pings take over (pad acks).
void NetplayClient::SendPrematchPing() {
  if (last_ping_ms_.load(std::memory_order_relaxed) > 0) return;
  bool anyone = false;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) if (remote_build_[i].pings) anyone = true;
  }
  if (!anyone) return;
  Packet p;
  p << (uint8_t)NP_MSG_MU_PING << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'P' << (uint8_t)1 << (uint8_t)0 << player_idx_ << (uint32_t)time_us();
  Send(p);
}

bool NetplayClient::PauseOff() {
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (remote_player_count_ == 0) return false;
  bool off = sent_no_pause_.load();
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i) {
    if (!remote_build_[i].received || !remote_build_[i].pause_rule) return false;
    off = off || remote_build_[i].no_pause;
  }
  return off;
}

void NetplayClient::SendItemsChoice() {
  std::lock_guard<std::mutex> lk(items_mutex_);
  const bool choice = online::items_choice();   // Off where the rule is not offered (a Vanilla run)
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_ITEMS << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'I' << (uint8_t)1 << player_idx_ << (uint8_t)(choice ? 1 : 0);
  SendAsync(std::move(p));   // Send() passes it only to peers that read the rule
  if (sent_items_choice_.exchange(choice ? 1 : 0) != (choice ? 1 : 0)) host::log("slippi: sending Items %s", choice ? "on" : "off");
}

NetplayClient::ItemsVerdict NetplayClient::Items() {
  ItemsVerdict v;
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (remote_player_count_ == 0) return v;
  bool on = sent_items_choice_.load() == 1;
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i) {
    const RemoteBuild& b = remote_build_[i];
    if (!b.received || !b.items_rule) {   // no such rule there: off, whatever anyone chose
      v.unsupported = true;
      v.blocker = match_info_.remote[i].player_idx;
      return v;
    }
    if (b.items_choice != 1 && on) {
      on = false;
      v.blocker = match_info_.remote[i].player_idx;
    }
  }
  v.on = on;
  return v;
}

bool NetplayClient::LocalLocked() {
  std::lock_guard<std::mutex> lk(selection_mutex_);
  return match_info_.local.is_character_selected;
}

bool NetplayClient::ItemsPending() {
  if (sent_items_choice_.load() != 1) return false;   // Items off here: no items whatever theirs
  std::lock_guard<std::mutex> lk(build_mutex_);
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i) {
    const RemoteBuild& b = remote_build_[i];
    if (b.received && b.items_rule && b.items_choice < 0) return true;
  }
  return false;
}

// ---- Squad Strike in Direct (NP_MSG_MU_SQUAD) ----
void NetplayClient::SendSquad() {
#if MU_SQUAD
  std::lock_guard<std::mutex> lk(squad_mutex_);
  const bool choice = online::squad_choice();   // Off where Squad Strike is not offered (a Vanilla run)
  uint8_t lineup[8] = {};
  // The online select's lineup, or a test's; none while Squad Strike is off here (it would not be used).
  const uint8_t count = choice ? online::squad_local_lineup(lineup) : 0;
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_SQUAD << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'Q' << (uint8_t)1 << player_idx_
     << (uint8_t)(choice ? 1 : 0) << count;
  for (uint8_t i = 0; i < 2 * count; ++i) *p << lineup[i];
  SendAsync(std::move(p));   // Send() passes it only to peers that play Squad Strike in Direct
  const bool lineup_changed = sent_squad_count_ != count || std::memcmp(sent_squad_lineup_, lineup, sizeof lineup) != 0;
  if (sent_squad_choice_.exchange(choice ? 1 : 0) != (choice ? 1 : 0) || lineup_changed) {
    std::string text;
    for (uint8_t i = 0; i < count; ++i) text += (i ? "," : " ") + std::to_string(lineup[2 * i]) + ":" + std::to_string(lineup[2 * i + 1]);
    host::log("slippi: sending Squad Strike %s, lineup%s", choice ? "on" : "off", count ? text.c_str() : " none yet");
  }
  sent_squad_count_ = count;
  std::memcpy(sent_squad_lineup_, lineup, sizeof lineup);
#endif
}

NetplayClient::SquadVerdict NetplayClient::Squad() {
  SquadVerdict v;
#if MU_SQUAD
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (remote_player_count_ == 0) return v;
  bool on = sent_squad_choice_.load() == 1;
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i) {
    const RemoteBuild& b = remote_build_[i];
    if (!b.received || !b.squad_rule) {   // no Squad Strike there: off, whatever anyone chose
      v.unsupported = true;
      v.blocker = match_info_.remote[i].player_idx;
      return v;
    }
    if (b.squad_choice != 1 && on) {
      on = false;
      v.blocker = match_info_.remote[i].player_idx;
    }
  }
  v.on = on && remote_player_count_ == 1;   // squads are 1v1
#endif
  return v;
}

bool NetplayClient::SquadPending() {
#if MU_SQUAD
  if (sent_squad_choice_.load() != 1) return false;   // Squad Strike off here: the match is normal whatever theirs
  std::lock_guard<std::mutex> lk(build_mutex_);
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i) {
    const RemoteBuild& b = remote_build_[i];
    if (b.received && b.squad_rule && b.squad_choice < 0) return true;
  }
#endif
  return false;
}

bool NetplayClient::SquadLineups(uint8_t counts[2], uint8_t lineups[2][8]) {
  if (!Squad().on) return false;
  const uint8_t local = player_idx_;
  uint8_t remote = 0xFF;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    remote = match_info_.remote[0].player_idx;
    if (local > 1 || remote > 1 || local == remote) return false;
    counts[remote] = remote_build_[0].squad_count;
    std::memcpy(lineups[remote], remote_build_[0].squad_lineup, 8);
  }
  std::lock_guard<std::mutex> lk(squad_mutex_);
  counts[local] = sent_squad_count_;
  std::memcpy(lineups[local], sent_squad_lineup_, 8);
  return true;
}

bool NetplayClient::PortColours(int8_t mine[2], int8_t theirs[2]) {
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (remote_player_count_ != 1 || !remote_build_[0].received) return false;
  const int sent = sent_port_colour_.load();
  mine[0] = (int8_t)(sent & 0xFF); mine[1] = (int8_t)((sent >> 8) & 0xFF);
  theirs[0] = remote_build_[0].port_colour[0]; theirs[1] = remote_build_[0].port_colour[1];
  return true;
}

bool NetplayClient::SendGecko(const std::vector<uint8_t>& message) {
  if (message.empty() || message.size() > 64 * 1024) return false;
  if (status_.load(std::memory_order_acquire) != ConnectStatus::CONNECTED) return false;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    if (remote_player_count_ != 1 || !remote_build_[0].received || !remote_build_[0].gecko) return false;
  }
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_GECKO;
  p->append(message.data(), message.size());
  SendAsync(std::move(p));
  return true;
}

bool NetplayClient::PopGecko(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lk(gecko_mutex_);
  if (gecko_inbox_.empty()) return false;
  out = std::move(gecko_inbox_.front());
  gecko_inbox_.pop_front();
  return true;
}

uint32_t NetplayClient::RemoteShieldColor(int slot) {
  std::lock_guard<std::mutex> lk(build_mutex_);
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i)
    if (remote_build_[i].received && remote_build_[i].port == slot) return remote_build_[i].shield_rgb;
  return 0;
}

NetplayClient::RemoteBuild NetplayClient::GetRemoteBuild(int index) {
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (index < 0 || index >= REMOTE_PLAYER_MAX) return {};
  return remote_build_[index];
}

bool NetplayClient::EveryRemoteReadsGoneOwnFrames() {
  std::lock_guard<std::mutex> lk(build_mutex_);
  PeerBuildRow rows[REMOTE_PLAYER_MAX];
  const int count = std::min<int>(remote_player_count_, REMOTE_PLAYER_MAX);
  for (int i = 0; i < count; ++i) rows[i] = {remote_build_[i].received, remote_build_[i].caps2};
  return slippi::EveryPeerReadsGoneOwnFrames(rows, count);
}

void NetplayClient::SendSetupHash(uint64_t inputs, uint64_t setup) {
  if (host::test_env::getenv("MELEE_TEST_NO_SETUP_HASH")) return;
  MuSetupHash h{player_idx_, inputs, setup};
  unsigned char bytes[MU_SETUP_HASH_WIRE];
  const size_t size = mu_setup_hash_encode(&h, bytes, sizeof bytes);
  if (size != MU_SETUP_HASH_WIRE) return;
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_SETUP_HASH;
  p->append(bytes, size);
  SendAsync(std::move(p));   // reliable, ordered channel 0, after the selections it was built from
}

NetplayClient::PeerSetupHash NetplayClient::GetSetupHash(int index) {
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (index < 0 || index >= REMOTE_PLAYER_MAX) return {};
  return setup_hash_[index];
}

void NetplayClient::SendSlippiPadSuggestion(int32_t target_frame, uint32_t seq, const uint8_t pad[host::early_input::kPadBytes]) {
  if (status_.load(std::memory_order_acquire) != ConnectStatus::CONNECTED) return;
  bool anyone = false;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) if (!remote_build_[i].early_pads) anyone = true;
  }
  if (!anyone) return;   // everyone on the other side takes this build's early pads
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_SLIPPI_PAD_SUGGESTION << player_idx_ << target_frame << seq;
  p->append(pad, PAD_DATA_SIZE);
  SendAsync(std::move(p));
}
void NetplayClient::SendEarlyPad(int32_t target, const uint8_t pad[host::early_input::kPadBytes]) {
  ConnectStatus st = status_.load(std::memory_order_acquire);
  if (st != ConnectStatus::CONNECTED) return;
  bool anyone = false;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) if (remote_build_[i].early_pads) anyone = true;
  }
  if (!anyone) return;   // nobody on the other side takes them: no message at all
  host::early_input::note_packet();
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_EARLY_PAD << (uint8_t)'M' << (uint8_t)'U' << (uint8_t)'E' << (uint8_t)1 << player_idx_ << target;
  for (int i = 0; i < host::early_input::kPadBytes; ++i) *p << pad[i];
  SendAsync(std::move(p));
}

void NetplayClient::EarlyPads(int index, int32_t latest, std::vector<host::early_input::Entry>& out) {
  out.clear();
  if (index < 0 || index >= remote_player_count_) return;
  std::lock_guard<std::mutex> lk(pad_mutex_);
  early_pads_[index].drop_through(latest);
  out = early_pads_[index].entries();
}

void NetplayClient::SendPickPreview(const MuPickPreview& pick, uint32_t frame) {
  if (status_.load(std::memory_order_acquire) != ConnectStatus::CONNECTED || !mu_pick_preview_valid(&pick)) return;
  bool locked_now;
  {
    std::lock_guard<std::mutex> lk(selection_mutex_);
    locked_now = match_info_.local.is_character_selected;
  }
  QueuePickPreview(pick, frame, locked_now);
}

// Caller holds selection_mutex_ (SetMatchSelections), so the preview is queued before the 0x82.
void NetplayClient::SendLockedPickPreview() {
  if (status_.load(std::memory_order_acquire) != ConnectStatus::CONNECTED) return;
  MuPickPreview pick;
  uint32_t frame;
  {
    std::lock_guard<std::mutex> lk(pick_mutex_);
    if (!pick_local_set_) return;   // no character screen frame yet: nothing to show
    pick = pick_local_;
    frame = pick_sender_.last_frame;
  }
  QueuePickPreview(pick, frame, true);
}

void NetplayClient::QueuePickPreview(MuPickPreview pick, uint32_t frame, bool locked_now) {
  // Once 0x82 said locked, every later preview says so too, until the selections change.
  if (locked_now) pick.locked = 1;
  // A custom fighter's id means another fighter (or none) on the other machine: shown as no fighter.
  if (pick.char_id != MU_PICK_PREVIEW_NONE && host::online_fighters::is_custom(pick.char_id)) pick.char_id = MU_PICK_PREVIEW_NONE;
  uint8_t peers = 0;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i)
      if (remote_build_[i].pick_previews) peers |= (uint8_t)(1u << i);
  }
  unsigned char bytes[MU_PICK_PREVIEW_WIRE];
  if (mu_pick_preview_encode(&pick, bytes, sizeof bytes) != sizeof bytes) return;
  {
    std::lock_guard<std::mutex> lk(pick_mutex_);
    pick_local_ = pick;
    pick_local_set_ = true;
    if (!peers) return;   // nobody takes them (Slippi Dolphin, an older build): no message at all
    // A peer whose build arrived since the last send gets the current pick at once; so does a
    // lock-in, which must precede its 0x82.
    const bool force = (peers & ~pick_sent_peers_) != 0 || (locked_now && pick_sender_.last[MU_PICK_PREVIEW_WIRE - 1] != 1);
    if (!mu_pick_preview_due(&pick_sender_, bytes, frame, force)) return;
    pick_sent_peers_ = peers;
  }
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_PICK_PREVIEW;
  p->append(bytes, sizeof bytes);
  SendAsync(std::move(p));
}

bool NetplayClient::RemotePickPreview(int index, MuPickPreview* out, uint8_t* port) {
  if (!out || !port || index < 0 || index >= remote_player_count_ || index >= REMOTE_PLAYER_MAX) return false;
  std::lock_guard<std::mutex> lk(pick_mutex_);
  if (!pick_remote_set_[index]) return false;
  // A player who left takes their preview with them.
  if (pick_remote_port_[index] >= PLAYER_COUNT_MAX ||
      !player_active_[pick_remote_port_[index]].load(std::memory_order_acquire)) {
    pick_remote_set_[index] = false;
    return false;
  }
  *out = pick_remote_[index];
  *port = pick_remote_port_[index];
  return true;
}

void NetplayClient::WriteSelections(Packet& p, const PlayerSelections& s) {
  p << (uint8_t)NP_MSG_SLIPPI_MATCH_SELECTIONS << s.character_id << s.character_color << s.is_character_selected << s.player_idx
    << s.stage_id << s.is_stage_selected << s.rng_offset << s.team_id << s.alt_stage_mode;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadSelections(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->character_id >> s->character_color >> s->is_character_selected >> s->player_idx >> s->stage_id >> s->is_stage_selected
        >> s->rng_offset >> s->team_id >> s->alt_stage_mode)) { host::log("slippi: invalid selection packet"); s->error = true; }
  return s;
}
std::unique_ptr<PlayerSelections> NetplayClient::ReadChatMessage(Packet& p) {
  auto s = std::make_unique<PlayerSelections>();
  if (!(p >> s->message_id) || !(p >> s->player_idx)) { s->error = true; return s; }
  static const int allowed[] = {136, 129, 130, 132, 34, 40, 33, 36, 72, 66, 68, 65, 24, 18, 20, 17, CHAT_MSG_CHAT_DISABLED};
  bool ok = false;
  for (int a : allowed) if (a == s->message_id) ok = true;
  if (!ok) { host::log("slippi: invalid chat message %d", s->message_id); s->error = true; }
  return s;
}

void NetplayClient::Send(Packet& packet) {
  for (size_t i = 0; i < server_.size(); ++i) {
    auto conn = active_connections_.find(peer_key(server_[i]));
    if (conn != active_connections_.end()) {
      auto p = conn->second.find(server_[i]);
      if (p != conn->second.end() && p->second.is_disconnected) continue;
    }
    uint8_t mid = packet.data()[0];
    enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
    uint8_t channel = 0;
    if (mid == NP_MSG_SLIPPI_PAD || mid == NP_MSG_SLIPPI_PAD_ACK) { flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1; }
    if (mid == NP_MSG_SLIPPI_PAD_SUGGESTION) {
      // A peer that takes this build's early pads gets those instead, as before.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i < (size_t)REMOTE_PLAYER_MAX && remote_build_[i].early_pads) continue;
      flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1;
    }
    if (mid == NP_MSG_MU_PING) {
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].pings) continue;
      flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1;
    }
    if (mid == NP_MSG_MU_PICK_PREVIEW) {
      // Reliable channel 0, ordered with 0x82, and only to a peer whose build said it takes them.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].pick_previews) continue;
    }
    if (mid == NP_MSG_MU_EARLY_PAD) {
      // Only to a peer whose build said it takes them; the rest never see the id.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].early_pads) continue;
      flags = ENET_PACKET_FLAG_UNSEQUENCED; channel = 1;
    }
    if (mid == NP_MSG_MU_GECKO) {
      // Only to a peer whose build said it takes them; reliable, on the control channel.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].gecko) continue;
    }
    if (mid == NP_MSG_MU_SKINS) {
      // Only to a peer whose build said it takes them; reliable, on the control channel with 0x82.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].skins) {
        host::log("opponent skins: not sent to remote %zu (its build message did not offer skins)", i);
        continue;
      }
      host::log("opponent skins: sent %u bytes to remote %zu", (unsigned)packet.size(), i);
    }
    if (mid == NP_MSG_MU_ITEMS) {
      // Only to a peer whose build said it reads the items rule (Slippi Dolphin and older builds never
      // see the id); reliable, on the control channel with 0x82.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].items_rule) continue;
    }
    if (mid == NP_MSG_MU_CONTENT) {
      // Only to a peer whose build said it takes custom fighters; reliable, on the control channel with 0x82.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].custom_fighters) {
        host::log("custom fighters: not sent to remote %zu (its build message did not offer them)", i);
        continue;
      }
    }
    if (mid == NP_MSG_MU_SQUAD) {
      // Squad Strike in Direct: only to a peer whose build said it plays it (MU_BUILD_CAPS4_SQUAD).
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i >= (size_t)REMOTE_PLAYER_MAX || !remote_build_[i].squad_rule) continue;
    }
    if (mid == NP_MSG_MU_SETUP_HASH) {
      // Melee Unlocked 0.9.216 and older read 0xE5 as warm-up (0.9.215 on) and know no setup hash: never
      // to a peer whose build message had no second capability byte.
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i < (size_t)REMOTE_PLAYER_MAX && remote_build_[i].received && remote_build_[i].layout == 0) continue;
    }
    // Tests only: MELEE_TEST_WIRE_LOG=1 logs the bytes of each build, selection and custom fighters
    // message as it goes to each remote, to show what a peer receives.
    if (mid == NP_MSG_MU_BUILD || mid == NP_MSG_SLIPPI_MATCH_SELECTIONS || mid == NP_MSG_MU_CONTENT) {
      static const bool wire_log = [] {
        const char* v = host::test_env::getenv("MELEE_TEST_WIRE_LOG");
        return v && v[0] == '1';
      }();
      if (wire_log) {
        std::string hex;
        char b[4];
        for (size_t k = 0; k < packet.size() && k < 160; ++k) { std::snprintf(b, sizeof b, "%02X", packet.data()[k]); hex += b; }
        host::log("wire: to remote %zu id %02X (%zu bytes): %s", i, mid, packet.size(), hex.c_str());
      }
    }
    ENetPacket* epac = enet_packet_create(packet.data(), packet.size(), flags);
    if (epac && mid == NP_MSG_MU_GECKO) {
      std::lock_guard<std::mutex> lk(build_mutex_);
      if (i < (size_t)REMOTE_PLAYER_MAX && remote_build_[i].layout == 0)   // 0.9.216 and older: 0xE4
        epac->data[0] = (uint8_t)NP_MSG_MU_PICK_PREVIEW;
    }
    enet_peer_send(server_[i], channel, epac);
  }
}

// ENet drops unreliable packets on purpose while the round trip time rises, and pads and acks are all
// unreliable: a latency rise became gaps in the inputs, which stall the game. A deceleration of 0 keeps
// the throttle at full, and ENet sends the setting to the peer too (Slippi 3.7.0).
static void disable_packet_throttle_drops(ENetPeer* peer) {
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
}

void NetplayClient::Disconnect() {
  status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  if (active_connections_.empty()) return;
  for (auto& conn : active_connections_)
    for (auto& peer : conn.second) enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
  ENetEvent ev;
  while (enet_host_service(client_, &ev, 3000) > 0) {
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) host::log("slippi: got peer disconnect");
  }
  for (auto& conn : active_connections_) for (auto& peer : conn.second) enet_peer_reset(peer.first);
  active_connections_.clear();
  for (auto& a : player_active_) a.store(false, std::memory_order_release);
  server_.clear();
  std::lock_guard<std::mutex> lk(pick_mutex_);
  for (bool& set : pick_remote_set_) set = false;
}

void NetplayClient::SendAsync(std::unique_ptr<Packet> packet) {
  if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) return;
  { std::lock_guard<std::mutex> lk(async_mutex_);
    async_queue_.push_back(std::move(packet));
  }
  wakeup_thread(client_);
}

void NetplayClient::ThreadFunc() {
  uint64_t start_time = time_ms();
  const uint64_t timeout = kConnectTimeoutMs;
  std::vector<bool> connections(remote_player_count_, false);
  std::vector<ENetAddress> remote_addrs;
  for (int i = 0; i < remote_player_count_; ++i) remote_addrs.push_back(server_[i] ? server_[i]->address : ENetAddress{});
  std::vector<bool> fallback_started(remote_player_count_,false);
  bool fallback_logged = false;

  while (status_.load(std::memory_order_acquire) == ConnectStatus::INITIATED) {
    ENetEvent ev;
    int net = enet_host_service(client_, &ev, 500);
    if (net > 0) {
      switch (ev.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          if (!ev.peer) break;
          Packet rpac(ev.packet->data, ev.packet->dataLength);
          OnData(rpac, ev.peer);
          enet_packet_destroy(ev.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
          if (ev.peer) host::log("slippi: peer disconnected while connecting");
          break;
        case ENET_EVENT_TYPE_CONNECT: {
          if (!ev.peer) break;
          disable_packet_throttle_drops(ev.peer);   // Slippi 3.7.0
          bool already = false;
          for (size_t i = 0; i < server_.size(); ++i)
            if (connections[i] && ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) {
              ActiveConnectionInfo info; info.player_idx = match_info_.remote[i].player_idx;
              active_connections_[peer_key(ev.peer)][ev.peer] = info;
              server_[i] = ev.peer; already = true; break;
            }
          if (already) break;
          // Exact address first: several players can share one host (local multi-instance
          // tests, or a LAN behind one address), and matching the host alone would hand this
          // peer another player's slot. Falls back to the host alone as before.
          int slot = -1;
          for (size_t i = 0; i < server_.size() && slot < 0; ++i)
            if (!connections[i] && remote_addrs[i].host == ev.peer->address.host && remote_addrs[i].port == ev.peer->address.port) slot = (int)i;
          for (size_t i = 0; i < server_.size() && slot < 0; ++i)
            if (!connections[i] && remote_addrs[i].host == ev.peer->address.host) slot = (int)i;
          // The peer's 2 s switch to our public endpoint can beat ours: accept its public address too.
          for (size_t i = 0; i < server_.size() && i < public_fallbacks_.size() && slot < 0; ++i) {
            ENetAddress pub{};
            if (!connections[i] && slippi::public_endpoint(public_fallbacks_[i], pub) && pub.host == ev.peer->address.host) slot = (int)i;
          }
          if (slot >= 0) {
            ActiveConnectionInfo info;
            info.player_idx = match_info_.remote[slot].player_idx;
            active_connections_[peer_key(ev.peer)][ev.peer] = info;
            player_active_[info.player_idx].store(true, std::memory_order_release);
            host::log("slippi: connected to peer");
            server_[slot] = ev.peer;
            connections[slot] = true;
          } else enet_peer_reset(ev.peer); // stale LAN connect after switching, or an unknown peer
          break;
        }
        default: break;
      }
    }
    // Internet-only sandboxes cannot reach LAN candidates. Keep the same eight-second
    // total budget and switch only unconnected seats, two seconds after the LAN attempt.
    for (int i = 0; i < remote_player_count_; ++i) {
      if (!public_fallback_due(time_ms()-start_time,connections[i],fallback_started[i],
          size_t(i) < public_fallbacks_.size() && !public_fallbacks_[i].empty())) continue;
      fallback_started[i] = true;
      ENetAddress address{};
      if (!slippi::public_endpoint(public_fallbacks_[i], address)) continue;
      if (!fallback_logged) { host::log("slippi: LAN connection pending; trying public address fallback"); fallback_logged = true; }
      if (server_[i]) {
        auto found = active_connections_.find(peer_key(server_[i]));
        if (found != active_connections_.end()) {
          found->second.erase(server_[i]);
          if (found->second.empty()) active_connections_.erase(found);
        }
        enet_peer_reset(server_[i]);
      }
      remote_addrs[i] = address;
      server_[i] = enet_host_connect(client_,&address,3,0);
      if (!server_[i]) { status_.store(ConnectStatus::FAILED,std::memory_order_release); return; }
      ActiveConnectionInfo info; info.player_idx = match_info_.remote[i].player_idx;
      active_connections_[peer_key(server_[i])][server_[i]] = info;
    }
    bool all = true;
    for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) all = false;
    if (all) {
      client_->intercept = intercept_callback;
      host::log("slippi: online connection successful");
      connected_at_ms_.store(time_ms(), std::memory_order_release);
      status_.store(ConnectStatus::CONNECTED, std::memory_order_release);
      SendBuild();
      break;
    }
    if (time_ms() - start_time >= timeout || !do_loop_.load()) {
      for (int i = 0; i < remote_player_count_; ++i) if (!connections[i]) failed_connections_.push_back(i);
      status_.store(ConnectStatus::FAILED, std::memory_order_release);
      host::log("slippi: online connection failed");
      return;
    }
  }

  // Test only. MELEE_NET_LAG_MS=N holds every received match packet for N ms before it is handled,
  // so two instances on one machine (tools/online_pair.py) see the late inputs that cause rollbacks,
  // which a loopback connection never produces. Unset, as it always is for a player, nothing changes.
  static const uint64_t lag_ms = [] {
    const char* s = host::test_env::getenv("MELEE_NET_LAG_MS");
    const long v = s ? std::atol(s) : 0;
    if (v > 0) host::log("slippi: MELEE_NET_LAG_MS: holding received packets %ld ms (testing)", v);
    return v > 0 ? (uint64_t)v : 0;
  }();
  // MELEE_NET_JITTER_MS=J (test only, with the lag): each packet is held lag + 0..J ms, never
  // overtaking the one before it, so the arrival times vary as on a real connection.
  static const uint64_t jitter_ms = [] {
    const char* s = host::test_env::getenv("MELEE_NET_JITTER_MS");
    const long v = s ? std::atol(s) : 0;
    if (v > 0) host::log("slippi: MELEE_NET_JITTER_MS: up to %ld ms more per packet (testing)", v);
    return v > 0 ? (uint64_t)v : 0;
  }();
  uint64_t jitter_state = 0x9E3779B97F4A7C15ull ^ (uint64_t)time_ms();
  struct Held { uint64_t due; std::vector<uint8_t> data; ENetPeer* peer; };
  std::deque<Held> held;
  auto deliver_due = [&] {
    while (!held.empty() && held.front().due <= time_ms()) {
      Packet rpac(held.front().data.data(), held.front().data.size());
      OnData(rpac, held.front().peer);
      held.pop_front();
    }
  };

  uint64_t next_ping_ms = 0;
  while (do_loop_.load()) {
    if (status_.load(std::memory_order_acquire) == ConnectStatus::DISCONNECTED) break;
    deliver_due();
    if (status_.load(std::memory_order_acquire) == ConnectStatus::CONNECTED && time_ms() >= next_ping_ms) {
      next_ping_ms = time_ms() + 100;
      SendPrematchPing();
    }
    // Everyone can be halted waiting on a dropped player, when no packet arrives to wake this thread.
    const bool waiting_on_drop = ProcessPlayerDrops();
    for (auto& conn : active_connections_) {
      for (auto& peer : conn.second) {
        if (peer.second.is_disconnected || player_active_[peer.second.player_idx].load(std::memory_order_acquire)) continue;
        enet_peer_disconnect(peer.first, pending_disconnect_reason_.load(std::memory_order_acquire));
        peer.second.is_disconnected = true;
      }
    }
    ENetEvent ev;
    int net = enet_host_service(client_, &ev, !held.empty() ? 1 : waiting_on_drop ? 10 : 250);
    for (;;) {
      std::unique_ptr<Packet> p;
      { std::lock_guard<std::mutex> lk(async_mutex_); if (async_queue_.empty()) break; p = std::move(async_queue_.front()); async_queue_.pop_front(); }
      Send(*p);
    }
    if (net <= 0) continue;
    switch (ev.type) {
      case ENET_EVENT_TYPE_RECEIVE: {
        if (lag_ms) {
          uint64_t due = time_ms() + lag_ms;
          if (jitter_ms) {
            jitter_state ^= jitter_state << 13; jitter_state ^= jitter_state >> 7; jitter_state ^= jitter_state << 17;
            due += jitter_state % (jitter_ms + 1);
            if (!held.empty() && due < held.back().due) due = held.back().due;
          }
          held.push_back({due, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength), ev.peer});
          enet_packet_destroy(ev.packet);
          break;
        }
        Packet rpac(ev.packet->data, ev.packet->dataLength);
        OnData(rpac, ev.peer);
        enet_packet_destroy(ev.packet);
        break;
      }
      case ENET_EVENT_TYPE_DISCONNECT: {
        held.clear();   // their peer is going away; nothing held for it may be handled after
        std::string key = peer_key(ev.peer);
        if (active_connections_.count(key) && active_connections_[key].count(ev.peer)) active_connections_[key][ev.peer].is_disconnected = true;
        bool all_peers_gone = AreAllPeersDisconnectedForKey(key);
        bool connected_client = false;
        for (size_t i = 0; i < server_.size(); ++i)
          if (ev.peer->address.host == server_[i]->address.host && ev.peer->address.port == server_[i]->address.port) connected_client = true;
        // Only a port this side is really connected to: losing a player makes everyone else drop them too.
        if (connected_client && all_peers_gone && active_connections_.count(key) && active_connections_[key].count(ev.peer))
          player_active_[active_connections_[key][ev.peer].player_idx].store(false, std::memory_order_release);
        if (connected_client && ev.data != 0) disconnect_reason_.store(ev.data, std::memory_order_release);
        host::log("slippi: disconnect (all peers gone: %d, connected client: %d)", all_peers_gone, connected_client);
        if (connected_client && all_peers_gone && AreAllConnectionsDisconnected()) do_loop_.store(false);
        break;
      }
      case ENET_EVENT_TYPE_CONNECT: {
        disable_packet_throttle_drops(ev.peer);
        int late_idx = 0;
        for (int i = 0; i < (int)server_.size(); ++i)
          if (server_[i]->address.host == ev.peer->address.host && server_[i]->address.port == ev.peer->address.port) { late_idx = i; break; }
        ActiveConnectionInfo info;
        info.player_idx = match_info_.remote[late_idx].player_idx;
        // A dropped player stays dropped: the other players have moved on without them.
        if (info.player_idx < PLAYER_COUNT_MAX && player_drop_times_ms_[info.player_idx] != 0) {
          info.is_disconnected = true;
          active_connections_[peer_key(ev.peer)][ev.peer] = info;
          enet_peer_disconnect(ev.peer, 0);
          break;
        }
        active_connections_[peer_key(ev.peer)][ev.peer] = info;
        player_active_[info.player_idx].store(true, std::memory_order_release);
        break;
      }
      default: break;
    }
  }
  Disconnect();
}

void NetplayClient::StartSlippiGame() {
  {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    recovering_ = false;
    for (auto& state : remote_sync_states_) state = {};
  }
  game_prep_.clear();
  local_pad_queue_.clear();
  { std::lock_guard<std::mutex> lk(pad_mutex_); for (auto& b : early_pads_) b.clear(); }
  {
    std::lock_guard<std::mutex> lk(timing_mutex_);
    has_game_started_ = false;
    for (int i = 0; i < remote_player_count_; ++i) last_frame_timing_[i] = {0, time_us()};
  }
  {
    std::lock_guard<std::mutex> lk(ack_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) { last_frame_acked_[i] = 0; ack_timers_[i].clear(); }
  }
  {
    // Previews belong to one character screen: the next one starts empty and sends at once.
    std::lock_guard<std::mutex> lk(pick_mutex_);
    pick_sender_ = {};
    pick_local_set_ = false;
    pick_sent_peers_ = 0;
    for (bool& set : pick_remote_set_) set = false;
  }
  { std::lock_guard<std::mutex> lk(selection_mutex_); match_info_.Reset();
  }
  {
    // A skins message belongs to the 0x82 right after it: one left over now has none.
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < REMOTE_PLAYER_MAX; ++i) { skins_pending_set_[i] = false; skins_pending_[i].clear(); }
  }
}

void NetplayClient::SendSlippiPad(std::unique_ptr<Pad> pad) {
  ConnectStatus st = status_.load(std::memory_order_acquire);
  if (st == ConnectStatus::FAILED || st == ConnectStatus::DISCONNECTED) return;
  std::unique_lock<std::mutex> pads_lock(pad_mutex_);
  if (pad) {
    local_pad_queue_.push_front(std::move(pad));
  }
  int min_ack = INT_MAX;
  {
    std::lock_guard<std::mutex> lk(ack_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) {
      if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
      min_ack = std::min(min_ack, last_frame_acked_[i]);
    }
  }
  if (!local_pad_queue_.empty()) min_ack = std::max(min_ack, local_pad_queue_.front()->frame - 128);
  while (!local_pad_queue_.empty() && local_pad_queue_.back()->frame < min_ack) local_pad_queue_.pop_back();
  if (local_pad_queue_.empty()) return;
  int32_t frame = local_pad_queue_.front()->frame;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_PAD << frame << player_idx_ << local_pad_queue_.front()->checksum_frame << local_pad_queue_.front()->checksum;
  for (auto& p : local_pad_queue_) spac->append(p->buf, PAD_WIRE_SIZE);
  SendAsync(std::move(spac));
  pads_lock.unlock();
  uint64_t t = time_us();
  {
    std::lock_guard<std::mutex> lk(timing_mutex_);
    has_game_started_ = true;
    remote_game_started_.fill(true);
    for (int i = 0; i < remote_player_count_; ++i) last_frame_timing_[i] = {frame, t};
  }
  {
    std::lock_guard<std::mutex> lk(ack_mutex_);
    for (int i = 0; i < remote_player_count_; ++i) ack_timers_[i].push_back({frame, t});
  }
}

void NetplayClient::SetMatchSelections(PlayerSelections& s) {
  auto spac = std::make_unique<Packet>();
  PlayerSelections sent;
  {
    std::lock_guard<std::mutex> lk(selection_mutex_);
    match_info_.local.Merge(s);
    match_info_.local.player_idx = player_idx_;
    // A lock-in's preview goes out before its 0x82 on the same reliable channel, so no preview
    // the peer gets after our 0x82 can still say "not locked".
    if (match_info_.local.is_character_selected) SendLockedPickPreview();
    sent = match_info_.local;
  }
  WriteSelections(*spac, WireSelections(sent));
  // Gecko codes (gecko_session.h): the lock goes just before each selection, on the same ordered
  // queue, so the opponent always has it by the time this selection can start a match.
  const std::vector<uint8_t> lock = gecko_session::lock_message();
  if (!lock.empty()) SendGecko(lock);
  SendLocalSkins(sent.is_character_selected, sent.character_id, sent.character_color);
  // Items: the choice that goes with this selection, just before it on the same ordered queue.
  SendItemsChoice();
  SendSquad();   // Squad Strike in Direct: the choice and lineup that go with it, the same way
  SendAsync(std::move(spac));
}
// The same selection again, with a fresh Gecko lock (codes were agreed after this side locked in).
void NetplayClient::ResendSelections() {
  auto spac = std::make_unique<Packet>();
  PlayerSelections sent;
  {
    std::lock_guard<std::mutex> lk(selection_mutex_);
    sent = match_info_.local;
  }
  WriteSelections(*spac, WireSelections(sent));
  const std::vector<uint8_t> lock = gecko_session::lock_message();
  if (!lock.empty()) SendGecko(lock);
  SendLocalSkins(sent.is_character_selected, sent.character_id, sent.character_color);
  // Items: the choice that goes with this selection, just before it on the same ordered queue.
  SendItemsChoice();
  SendSquad();   // Squad Strike in Direct
  SendAsync(std::move(spac));
}
// Custom fighters online. A pick of an added fighter goes on the wire only while every player has it
// (every remote's build takes them and every remote's list has its content hash); until then the 0x82
// says "no character locked", so no peer ever sees the id of a fighter it does not have
// (docs/online-custom-content.md). Judged on what arrived, not on the select screen's verdict: a pick
// locked before connecting is sent the moment the connection is made, before the screen's next look.
PlayerSelections NetplayClient::WireSelections(const PlayerSelections& s) {
  bool masked = false;
  PlayerSelections out = s;
  std::vector<uint8_t> ports;   // the other players: every index but this one's
  for (int i = 0; i <= remote_player_count_ && i <= REMOTE_PLAYER_MAX; ++i)
    if (i != player_idx_) ports.push_back((uint8_t)i);
  if (s.is_character_selected && host::online_fighters::is_custom(s.character_id) &&
      !(EveryRemoteTakesCustomFighters() && host::online_fighters::every_peer_has(s.character_id, ports))) {
    masked = true;
    out.is_character_selected = false;
    out.character_id = 0;
    out.character_color = 0;
  }
  const bool was = custom_masked_.exchange(masked, std::memory_order_acq_rel);
  if (masked && !was)
    host::log("custom fighters: this pick (id %u) is held back: sent as no character locked until every player has it", s.character_id);
  else if (!masked && was)
    host::log("custom fighters: this pick (id %u) goes out%s", s.character_id, s.is_character_selected ? "" : " (not locked)");
  return out;
}
bool NetplayClient::EveryRemoteTakesCustomFighters() {
  std::lock_guard<std::mutex> lk(build_mutex_);
  if (remote_player_count_ <= 0) return false;
  for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i)
    if (!remote_build_[i].received || !remote_build_[i].custom_fighters) return false;
  return true;
}
void NetplayClient::UnlockLocalCharacter() {
  std::lock_guard<std::mutex> lk(selection_mutex_);
  match_info_.local.is_character_selected = false;
}
void NetplayClient::SendFighterListIfDue() {
  if (!host::online_fighters::enabled()) return;   // no bit sent: no list either
  const uint32_t generation = host::online_fighters::have_generation();
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    bool due = false;
    for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i)
      if (remote_build_[i].received && remote_build_[i].custom_fighters && fighter_list_sent_[i] != generation) due = true;
    if (!due) return;
  }
  content_wire::Have have;
  have.sender_port = player_idx_;
  if (!host::online_fighters::local_have(&have)) return;   // still hashing: the next frame
  const std::vector<uint8_t> payload = content_wire::encode_have(have);
  if (payload.empty()) return;
  {
    std::lock_guard<std::mutex> lk(build_mutex_);
    for (int i = 0; i < remote_player_count_ && i < REMOTE_PLAYER_MAX; ++i)
      if (remote_build_[i].received && remote_build_[i].custom_fighters) fighter_list_sent_[i] = generation;
  }
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_CONTENT;
  p->append(payload.data(), payload.size());
  std::string list;
  for (const auto& e : have.entries) list += (list.empty() ? "" : ", ") + std::to_string(e.id) + " " + content_wire::hex(e.content).substr(0, 16);
  host::log("custom fighters: sent this side's list: %zu added fighter(s)%s%s", have.entries.size(), list.empty() ? "" : ": ", list.c_str());
  SendAsync(std::move(p));
}
// Opponents' skins: the skins message for the pick about to be sent, just before its 0x82 on the same
// ordered queue (Send() passes it only to peers that offered skins). Nothing unless the setting is on
// and there is a skin to name (host/opponent_skins.h: a test hook only, for now).
void NetplayClient::SendLocalSkins(bool selected, uint8_t character, uint8_t costume) {
  std::vector<uint8_t> payload;
  if (!selected || !host::skins::local_payload(player_idx_, character, costume, &payload)) return;
  auto p = std::make_unique<Packet>();
  *p << (uint8_t)NP_MSG_MU_SKINS;
  p->append(payload.data(), payload.size());
  host::log("opponent skins: queued a skins message (%zu bytes) for character %u costume %u", payload.size(), character, costume);
  SendAsync(std::move(p));
}
void NetplayClient::SendChatMessage(int message_id) {
  remote_sent_chat_message_id = (uint8_t)message_id;
  auto spac = std::make_unique<Packet>();
  *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << message_id << player_idx_;
  SendAsync(std::move(spac));
}
PlayerSelections NetplayClient::GetSlippiRemoteChatMessage(bool chat_enabled) {
  std::lock_guard<std::mutex> lk(chat_mutex_);
  PlayerSelections copied;
  if (remote_chat_message_selection_ && chat_enabled) {
    copied.message_id = remote_chat_message_selection_->message_id;
    copied.player_idx = remote_chat_message_selection_->player_idx;
    remote_chat_message_selection_->message_id = 0;
    remote_chat_message_selection_->player_idx = 0;
  } else {
    copied.message_id = 0; copied.player_idx = 0;
    if (remote_chat_message_selection_ && !chat_enabled && remote_chat_message_selection_->message_id > 0 &&
        remote_chat_message_selection_->message_id != CHAT_MSG_CHAT_DISABLED) {
      auto spac = std::make_unique<Packet>();
      *spac << (uint8_t)NP_MSG_SLIPPI_CHAT_MESSAGE << (int)CHAT_MSG_CHAT_DISABLED << player_idx_;
      SendAsync(std::move(spac));
      remote_sent_chat_message_id = 0;
      remote_chat_message_selection_ = nullptr;
    }
  }
  return copied;
}
uint8_t NetplayClient::GetSlippiRemoteSentChatMessage(bool chat_enabled) {
  if (!chat_enabled) return 0;
  uint8_t id = remote_sent_chat_message_id;
  remote_sent_chat_message_id = 0;
  return id;
}

std::unique_ptr<RemotePadOutput> NetplayClient::GetSlippiRemotePad(int index, int max_frame_count) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  auto out = std::make_unique<RemotePadOutput>();
  if (index < 0 || index >= RemotePortCount()) return out;
  const uint8_t port = match_info_.remote[index].player_idx;
  out->player_idx = port;
  out->is_disconnected = !player_active_[port].load(std::memory_order_acquire);
  out->despawn_frame = despawn_frames_[port].load(std::memory_order_acquire);
  if (remote_pads_[port].empty()) {
    Pad empty(0);
    out->latest_frame = 0;
    out->data.insert(out->data.end(), empty.buf, empty.buf + PAD_FULL_SIZE);
    return out;
  }
  out->checksum_frame = remote_checksums_[index].frame;
  out->checksum = remote_checksums_[index].value;
  out->latest_frame = 0;
  int count = 0;
  for (auto it = remote_pads_[port].rbegin(); it != remote_pads_[port].rend(); ++it) {
    out->latest_frame = std::max(out->latest_frame, (*it)->frame);
    out->data.insert(out->data.begin(), (*it)->buf, (*it)->buf + PAD_FULL_SIZE);
    if (++count >= max_frame_count) break;
  }
  return out;
}

void NetplayClient::DropOldRemoteInputs(int32_t finalized_frame) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  for (int i = 0; i < RemotePortCount(); ++i) {
    auto& queue = remote_pads_[match_info_.remote[i].player_idx];
    while (queue.size() > 1 && queue.back()->frame < finalized_frame) queue.pop_back();
  }
}
std::unordered_map<uint8_t, bool> NetplayClient::GetActivePlayerIndices() const {
  std::unordered_map<uint8_t, bool> r;
  for (int i = 0; i < remote_player_count_; ++i) {
    const uint8_t port = match_info_.remote[i].player_idx;
    if (player_active_[port].load(std::memory_order_acquire)) r[port] = true;
  }
  return r;
}
void NetplayClient::ForceDisconnectPlayer(uint8_t player_idx) {
  if (player_idx >= PLAYER_COUNT_MAX) return;
  if (player_idx == player_idx_) return;
  player_active_[player_idx].store(false, std::memory_order_release);
  bool any = false;
  for (uint8_t i = 0; i < remote_player_count_; ++i) if (player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) any = true;
  if (!any && status_.load(std::memory_order_acquire) == ConnectStatus::CONNECTED) {
    host::log("slippi: all remote players force-disconnected");
    status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  }
  if (client_) wakeup_thread(client_);
}
void NetplayClient::ForceDisconnect(DisconnectReason reason) {
  pending_disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  disconnect_reason_.store((uint32_t)reason, std::memory_order_release);
  // Before the players are marked gone: this side is the one leaving, so the network thread must not
  // take that for those players dropping and tell the others (ProcessPlayerDrops).
  status_.store(ConnectStatus::DISCONNECTED, std::memory_order_release);
  for (uint8_t i = 0; i < remote_player_count_; ++i) ForceDisconnectPlayer(match_info_.remote[i].player_idx);
}
// Network thread. Finds the players just lost and tells the others which inputs this side has of them.
// Those reports add to the player's inputs, so once they have had time to arrive the newest input is
// the one everyone has. Returns whether a report is still awaited.
bool NetplayClient::ProcessPlayerDrops() {
  bool waiting = false;
  const uint64_t now = time_ms();
  for (int j = 0; j < remote_player_count_; ++j) {
    const uint8_t port = match_info_.remote[j].player_idx;
    if (port >= PLAYER_COUNT_MAX || player_active_[port].load(std::memory_order_acquire)) continue;
    // ForceDisconnect sets the status before it marks the players: this side is leaving, nothing to report.
    if (status_.load(std::memory_order_acquire) != ConnectStatus::CONNECTED) return false;
    int32_t last_frame;
    { std::lock_guard<std::mutex> lk(pad_mutex_); last_frame = remote_pad_history_[port].empty() ? 0 : remote_pad_history_[port].front().frame; }
    if (player_drop_times_ms_[port] == 0) {
      host::log("slippi: lost player %u, last input frame %d", port, last_frame);
      player_drop_times_ms_[port] = now ? now : 1;
      SendPlayerDropReport(port);
    }
    if (despawn_frames_[port].load(std::memory_order_acquire) != 0) continue;
    if (now - player_drop_times_ms_[port] < DROP_REPORT_WAIT_MS) { waiting = true; continue; }
    host::log("slippi: settled on player %u: last input frame %d, despawn frame %d", port, last_frame, last_frame + DROP_DESPAWN_FRAMES);
    despawn_frames_[port].store(last_frame + DROP_DESPAWN_FRAMES, std::memory_order_release);
  }
  return waiting;
}
void NetplayClient::SendPlayerDropReport(uint8_t port) {
  Packet spac;
  spac << (uint8_t)NP_MSG_SLIPPI_PLAYER_DROP << player_idx_ << port;
  {
    std::lock_guard<std::mutex> lk(pad_mutex_);
    const auto& history = remote_pad_history_[port];
    spac << (history.empty() ? (int32_t)0 : history.front().frame) << (uint8_t)history.size();
    for (const auto& pad : history) spac.append(pad.buf, PAD_WIRE_SIZE);
  }
  Send(spac);
}
// Adds the inputs this side was missing from a report about a dropped player, newest first. They are
// the real inputs: the game rolls back if they differ from what it predicted.
void NetplayClient::AddPlayerDropReportInputs(uint8_t port, int32_t last_frame, uint8_t count, const uint8_t* inputs) {
  if (port >= PLAYER_COUNT_MAX) return;
  std::lock_guard<std::mutex> lk(pad_mutex_);
  auto& queue = remote_pads_[port];
  auto& history = remote_pad_history_[port];
  const int32_t head = queue.empty() ? 0 : queue.front()->frame;
  const int64_t missing = (int64_t)last_frame - (int64_t)head;
  if (missing > 128) { host::log("slippi: player drop packet with too many frames (%lld)", (long long)missing); return; }
  for (int64_t i = missing - 1; i >= 0; --i) {
    uint8_t pad[PAD_WIRE_SIZE] = {};
    if (i < count) std::copy_n(&inputs[i * PAD_WIRE_SIZE], PAD_WIRE_SIZE, pad);
    else {
      // The queue cannot have gaps: repeat an input. The desync detection catches a wrong one.
      host::log("slippi: player drop packet is missing frame %lld", (long long)((int64_t)last_frame - i));
      if (!queue.empty()) std::copy_n(queue.front()->buf, PAD_WIRE_SIZE, pad);
    }
    const int32_t frame = (int32_t)((int64_t)last_frame - i);
    queue.push_front(std::make_unique<Pad>(frame, pad));
    history.emplace_front(frame, pad);
    if (history.size() > 64) history.pop_back();
  }
}
// Game thread. A dropped player's settled last input repeats; these go in the same queue as real
// inputs since they are as final, so the game need not know the player is gone.
void NetplayClient::FillDroppedPlayerInputs(int32_t frame) {
  std::lock_guard<std::mutex> lk(pad_mutex_);
  for (int i = 0; i < remote_player_count_; ++i) {
    const uint8_t port = match_info_.remote[i].player_idx;
    if (despawn_frames_[port].load(std::memory_order_acquire) == 0) continue;
    auto& queue = remote_pads_[port];
    if (queue.empty()) queue.push_front(std::make_unique<Pad>(0));
    while (queue.front()->frame < frame) queue.push_front(std::make_unique<Pad>(queue.front()->frame + 1, queue.front()->buf));
  }
}
bool NetplayClient::AreAllPeersDisconnectedForKey(const std::string& key) {
  if (!active_connections_.count(key)) return true;
  for (auto& p : active_connections_[key]) if (!p.second.is_disconnected) return false;
  return true;
}
bool NetplayClient::AreAllConnectionsDisconnected() {
  for (auto& c : active_connections_) for (auto& p : c.second) if (!p.second.is_disconnected) return false;
  return true;
}
double NetplayClient::GetAndResetAvgPingMs() {
  uint64_t sum = ping_sample_sum_us_.exchange(0, std::memory_order_relaxed);
  uint64_t count = ping_sample_count_.exchange(0, std::memory_order_relaxed);
  return count ? (double)sum / (double)count / 1000.0 : 0.0;
}
int32_t NetplayClient::CalcTimeOffsetUs() {
  std::vector<int> offsets;
  for (int i = 0; i < remote_player_count_; ++i) {
    if (!player_active_[match_info_.remote[i].player_idx].load(std::memory_order_acquire)) continue;
    std::vector<int32_t> buf;
    { std::lock_guard<std::mutex> lk(timing_mutex_); buf = frame_offset_data_[i].buf; }
    if (buf.empty()) continue;
    std::sort(buf.begin(), buf.end());
    int n = (int)buf.size(), off = (int)((1.0f / 3.0f) * n), end = n - off;
    int sum = 0;
    for (int k = off; k < end; ++k) sum += buf[k];
    int count = end - off;
    if (count <= 0) return 0;
    offsets.push_back(sum / count);
  }
  if (offsets.empty()) return 0;
  return *std::min_element(offsets.begin(), offsets.end());
}

// ---------------------------------------------------------------- matchmaking
Matchmaking::LocalPeer Matchmaking::local_peer;
uint16_t Matchmaking::forced_port = 0;
bool Matchmaking::server_allowed = true;

Matchmaking::Matchmaking(User* user) : user_(user) {
}
Matchmaking::~Matchmaking() {
  is_mm_terminated_ = true;
  fail("Matchmaking shut down");
  if (thread_.joinable()) thread_.join();
  terminateMmConnection();
}
// The text is stored before the state that announces it, so a reader that sees the error state
// always finds its text.
void Matchmaking::fail(const std::string& message) {
  { std::lock_guard<std::mutex> lk(result_mutex_); error_msg_ = message; }
  state_ = ERROR_ENCOUNTERED;
}
bool Matchmaking::IsBlockedMatch(OnlinePlayMode mode, const std::vector<std::string>& blocked,
                                 const std::vector<UserInfo>& players, int local_index) {
  if (mode != UNRANKED || blocked.empty()) return false;
  const auto clean = [](const std::string& code) {
    std::string out;
    for (char c : code) if (!std::isspace((unsigned char)c)) out += (char)std::toupper((unsigned char)c);
    return out;
  };
  for (size_t i = 0; i < players.size(); ++i) {
    if ((int)i == local_index) continue;
    const std::string theirs = clean(players[i].connect_code);
    if (theirs.empty()) continue;
    for (const auto& code : blocked) if (!clean(code).empty() && clean(code) == theirs) return true;
  }
  return false;
}

void Matchmaking::FindMatch(MatchSearchSettings settings) {
  is_mm_connected_ = false;
  if (settings.blocked_codes.empty())
    for (const auto& code : slippi::online::config().blocked_codes)
      if (!code.empty()) settings.blocked_codes.push_back(code);
  search_settings_ = settings;
  { std::lock_guard<std::mutex> lk(result_mutex_); error_msg_.clear(); }
  if (!local_peer.enabled && !server_allowed) {
    fail("Matchmaking is off in automated runs");
    host::log("slippi: matchmaking blocked: hidden or scripted run without --allow-matchmaking");
    return;
  }
  // Version gate (slippi_version.h): Slippi's servers have named a newer version than the one this
  // build speaks, so it stays out of every online mode until it is updated. Local test peering has
  // no server and is not gated.
  const std::string latest = local_peer.enabled || !user_ ? std::string() : user_->GetUserInfo().latest_version;
  if (version_newer(latest, SLIPPI_SEMVER)) {
    fail("Slippi " + latest + " is out. Update " PRODUCT_NAME);
    host::log("slippi: matchmaking blocked: Slippi is on %s and this build speaks %s; update " PRODUCT_NAME, latest.c_str(), SLIPPI_SEMVER);
    return;
  }
  // A finished search's thread is still joinable: assigning a new one over it is std::terminate. Callers
  // replace a used client, but never end the process for it here either.
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  state_ = INITIALIZING;
  // The code is the opponent's (Direct): a public build logs a short hash of it (public_build.h).
  host::log("slippi: matchmaking started (mode %d, code '%s')", settings.mode,
            host::public_build::log_id(settings.connect_code).c_str());
  thread_ = std::thread(&Matchmaking::MatchmakeThread, this);
}
void Matchmaking::MatchmakeThread() {
  // Nothing may leave this thread as an exception (that would end the process): whatever a step
  // throws on unexpected server data becomes a failed search.
  try {
    while (IsSearching()) {
      if (is_mm_terminated_) break;
      switch (state_.load()) {
        case INITIALIZING: startMatchmaking(); break;
        case MATCHMAKING: handleMatchmaking(); break;
        case OPPONENT_CONNECTING: handleConnecting(); break;
        default: break;
      }
    }
  } catch (const std::exception& e) {
    host::log("slippi: matchmaking stopped on an error: %s", e.what());
    fail("Matchmaking failed on an unexpected response");
  } catch (...) {
    host::log("slippi: matchmaking stopped on an unknown error");
    fail("Matchmaking failed on an unexpected response");
  }
  terminateMmConnection();
}
void Matchmaking::disconnectFromServer() {
  is_mm_connected_ = false;
  if (!server_) return;
  enet_peer_disconnect(server_, 0);
  ENetEvent ev;
  while (enet_host_service(client_, &ev, 3000) > 0) {
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { server_ = nullptr; return; }
  }
  enet_peer_reset(server_);
  server_ = nullptr;
}
void Matchmaking::terminateMmConnection() {
  mm_session_.reset();   // leaves the queue if still in it
  disconnectFromServer();
  if (client_) { enet_host_destroy(client_); client_ = nullptr; }
}

static std::string local_address_string(ENetAddress* mm_address) {
#ifndef _WIN32
#endif
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return "";
  ENetAddress a;
  std::string out;
  if (enet_socket_connect(s, mm_address) != -1 && enet_socket_get_address(s, &a) != -1) {
    struct in_addr in; in.s_addr = a.host;
    char buf[32]; inet_ntop(AF_INET, &in, buf, sizeof buf);
    out = buf;
  }
  enet_socket_destroy(s);
  return out;
}

static int mm_send(ENetPeer* server, const json& msg) {
  std::string s = msg.dump();
  ENetPacket* p = enet_packet_create(s.c_str(), s.size(), ENET_PACKET_FLAG_RELIABLE);
  return enet_peer_send(server, 0, p);
}
// -1 also when `stop` turns true (the caller asks it why).
static int mm_receive(ENetHost* client, json& msg, int timeout_ms, const std::function<bool()>& stop = nullptr) {
  const int step = 250;
  int attempts = std::max(timeout_ms, step) / step;
  for (int i = 0; i < attempts; ++i) {
    if (stop && stop()) return -1;
    ENetEvent ev;
    int net = enet_host_service(client, &ev, step);
    if (net <= 0) continue;
    if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
      std::string s((const char*)ev.packet->data, ev.packet->dataLength);
      enet_packet_destroy(ev.packet);
      try { msg = json::parse(s); } catch (const std::exception& e) { host::log("slippi: bad matchmaking JSON"); return -3; }
      return 0;
    }
    if (ev.type == ENET_EVENT_TYPE_DISCONNECT) return -2;
  }
  return -1;
}

struct Matchmaking::Ticket { const json& resp; };

// Local test peering: the get-ticket-resp Slippi's matchmaking server would send this instance.
// Players are listed by port (1-based), the local one flagged isLocalPlayer; everyone is on
// 127.0.0.1 so ipAddress and ipAddressLan are the same. Player 1 (index 0) is the decider. A
// two-player match keeps the six-stage list the local harness always used; with more players
// "stages" is left out, so the parser's default for more than two players applies (no Fountain
// of Dreams).
static json local_peer_ticket(const UserInfo& me, Matchmaking::OnlinePlayMode mode) {
  using MM = Matchmaking;
  const auto& local_peer = MM::local_peer;
  const int n = (int)local_peer.remotes.size() + 1;
  json players = json::array();
  for (int i = 0, r = 0; i < n; ++i) {
    json p;
    const bool local = i == local_peer.local_index;
    std::string addr = local ? "127.0.0.1:" + std::to_string(local_peer.local_port)
                             : r < (int)local_peer.remotes.size() ? local_peer.remotes[r++] : std::string();
    if (local) {
      p["uid"] = me.uid; p["displayName"] = me.display_name; p["connectCode"] = me.connect_code;
    } else if (n == 2) {
      p["uid"] = "local-peer"; p["displayName"] = "Peer"; p["connectCode"] = "PEER#001";
    } else {
      p["uid"] = "local-peer-" + std::to_string(i + 1); p["displayName"] = "Peer " + std::to_string(i + 1);
      p["connectCode"] = "PEER#00" + std::to_string(i + 1);
    }
    p["port"] = i + 1;
    p["isLocalPlayer"] = local;
    p["ipAddress"] = addr;
    p["ipAddressLan"] = addr;
    players.push_back(p);
  }
  const char* mode_name = mode == MM::RANKED ? "ranked" : mode == MM::DIRECT ? "direct" : mode == MM::TEAMS ? "teams" : mode == MM::PARTY ? "party" : "unranked";
  json resp;
  resp["type"] = "get-ticket-resp";
  resp["matchId"] = "mode." + std::string(mode_name) + "-local-test";
  resp["players"] = players;
  resp["isHost"] = local_peer.local_index == 0;
  if (n == 2) resp["stages"] = {0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20};
  return resp;
}

// Public STUN servers asked for the netplay socket's public address, in order until two have answered.
// Two are needed to tell a cone NAT, which reuses one public port, from a symmetric one, which hands
// out a new port per destination. The later entries are for networks that block the first.
static const std::pair<const char*, uint16_t> kStunServers[] = {
    {"stun.l.google.com", 19302}, {"stun1.l.google.com", 19302}, {"stun.cloudflare.com", 3478}, {"global.stun.twilio.com", 3478}};
constexpr uint64_t kStunKeepaliveIntervalMs = 15000;

// A STUN binding request from the netplay socket; the reply names the public address the server saw.
bool Matchmaking::stunBindingRequest(uint32_t host, uint16_t port, mm_service::StunObservation* result) {
  uint8_t request[mm_service::kStunRequestLen];
  mm_service::stun_build_request(request);
  ENetAddress server; server.host = host; server.port = port;
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int sent = enet_socket_send(client_->socket, &server, &send_buf, 1);
    if (sent != (int)sizeof request) { host::log("slippi: STUN send failed (%d)", sent); return false; }
    const uint64_t start = time_ms();
    while (time_ms() - start < 500) {
      if (is_mm_terminated_) return false;
      enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(client_->socket, &condition, 100) != 0) { host::log("slippi: STUN wait failed"); return false; }
      if (!(condition & ENET_SOCKET_WAIT_RECEIVE)) continue;
      uint8_t response[512];
      ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
      ENetAddress from;
      const int received = enet_socket_receive(client_->socket, &from, &recv_buf, 1);
      if (received <= 0) continue;
      if (mm_service::stun_parse_response(request, response, (size_t)received, result)) return true;
    }
  }
  return false;
}
// Keeps the NAT mapping of the netplay socket alive while queued. Any reply is drained, so it is not
// left for the netplay host to find later.
void Matchmaking::sendStunKeepalive() {
  last_keepalive_ms_ = time_ms();
  if (!client_ || !has_stun_addr_) return;
  uint8_t request[mm_service::kStunRequestLen];
  mm_service::stun_build_request(request);
  ENetAddress server; server.host = stun_host_; server.port = stun_port_;
  ENetBuffer send_buf; send_buf.data = request; send_buf.dataLength = sizeof request;
  enet_socket_send(client_->socket, &server, &send_buf, 1);
  enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
  if (enet_socket_wait(client_->socket, &condition, 100) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE)) {
    uint8_t response[512];
    ENetBuffer recv_buf; recv_buf.data = response; recv_buf.dataLength = sizeof response;
    ENetAddress from;
    enet_socket_receive(client_->socket, &from, &recv_buf, 1);
  }
}

void Matchmaking::startMatchmaking() {
  if (!user_ || (!local_peer.enabled && !user_->IsLoggedIn())) {
    fail("Sign in to the Slippi Launcher to play online.");
    return;
  }
  if (!local_peer.enabled) {
    std::string error;
    if (!user_ || !user_->CheckSessionCompatibility(&error)) {
      fail(error.empty() ? "Could not check Slippi version. Try again." : error);
      return;
    }
    if (is_mm_terminated_ || state_ != INITIALIZING) return;
  }
  client_ = nullptr;
  UserInfo me = user_->GetUserInfo();

  if (local_peer.enabled) {
    // 2-4 local instances peer directly. The match is the get-ticket-resp the server would send,
    // run through the same parser as a real one (player list, local index, decider, addresses).
    host_port_ = local_peer.local_port;
    json resp = local_peer_ticket(me, search_settings_.mode);
    if (!ingest_ticket(Ticket{resp})) return;
    state_ = OPPONENT_CONNECTING;
    host::log("slippi: local peer test: %s, player %d of %zu (decider %d)", mm_result_.id.c_str(),
              local_player_index_, player_info_.size(), is_host_);
    return;
  }

  // All online modes use the authenticated Slippi route.
  // Local test peering returned above and keeps an unknown provider.
  const std::string error = openSlippiTicket(me);
  if (!error.empty()) { fail(error); return; }
  state_ = MATCHMAKING;
  host::log("slippi: matchmaking ticket created");
}

std::string Matchmaking::openSlippiTicket(const UserInfo& me) {
  if (agent_lock::blocked(agent_lock::Operation::Matchmaking)) return agent_lock::refusal;
  std::mt19937 rng((uint32_t)time_ms());
  if (!mm_test_server(nullptr, nullptr)) {
    // Slippi 3.7.0: the matchmaking service over a WebSocket (slippi_mm_service.h). The host is bound
    // to the port the match is played on: talking to the STUN servers through its socket makes the
    // NAT mapping the opponents will use and tells what it is.
    const auto cancelled = [&] { return is_mm_terminated_ || state_ != INITIALIZING; };
    {
      for (int retry = 0; !client_ && retry < 15; ++retry) {
        host_port_ = forced_port ? forced_port : std::uniform_int_distribution<int>(41000, 50999)(rng);
        ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)host_port_;
        client_ = enet_host_create(&addr, 1, 3, 0, 0);
      }
    }
    if (forced_port && host_port_ == forced_port)
      host::log(client_ ? "slippi: forced port %d bound" : "slippi: forced port %d bind failed", host_port_);
    if (!client_) return "Failed to create mm client";
    std::vector<ENetAddress> stun_addrs;
    {
      for (const auto& server : kStunServers) {
        ENetAddress a;
        if (enet_address_set_host(&a, server.first) != 0) { host::log("slippi: could not resolve STUN server %s", server.first); continue; }
        a.port = server.second;
        stun_addrs.push_back(a);
      }
    }
    has_stun_addr_ = !stun_addrs.empty();
    if (has_stun_addr_) { stun_host_ = stun_addrs[0].host; stun_port_ = stun_addrs[0].port; }
    std::string lan;
    if (has_stun_addr_) lan = local_address_string(&stun_addrs[0]);
    // Two servers are asked what public address they see, walking the list so a network that blocks
    // the first entries still gets an answer. The addresses themselves are never logged.
    mm_service::StunObservation stun[2];
    for (const auto& a : stun_addrs) {
      if (cancelled()) return "Cancelled";
      if (!stun[0].answered) {
        if (stunBindingRequest(a.host, a.port, &stun[0])) { stun_host_ = a.host; stun_port_ = a.port; }   // keepalives go to a server known to answer
      } else if (stunBindingRequest(a.host, a.port, &stun[1])) break;
    }
    last_keepalive_ms_ = time_ms();
    const int answers = (stun[0].answered ? 1 : 0) + (stun[1].answered ? 1 : 0);
    if (answers == 0) host::log("slippi: STUN lookup failed; the matchmaking service will use the address it observes");
    else host::log("slippi: STUN servers answered: %d of 2", answers);
    mm_service::Request request;
    request.mode = (uint8_t)search_settings_.mode;
    request.connect_code.assign(search_settings_.connect_code.begin(), search_settings_.connect_code.end());
    request.netplay_port = (uint16_t)host_port_;
    request.lan_addr = lan.empty() ? "" : lan + ":" + std::to_string(host_port_);
    request.stun[0] = stun[0]; request.stun[1] = stun[1];
    request.uid = me.uid; request.play_key = me.play_key; request.connect_code_self = me.connect_code;
    request.display_name = me.display_name; request.app_version = SLIPPI_SEMVER;
    // A cancel that came during STUN must not start a search nobody will poll.
    if (cancelled()) return "Cancelled";
    mm_session_ = std::make_unique<mm_service::Session>(std::move(request));
    return "";
  }
  // Tests only from here: the ENet stand-in for the matchmaking server Slippi had before 3.7.0.
  ENetAddress addr{};
  addr.port = 43113;
  bool resolved = false;
  std::string test_host;
  int test_port = 0;
  const bool test_mm = mm_test_server(&test_host, &test_port);
  if (test_mm) {
    addr.port = (enet_uint16)test_port;
    host::log("slippi: matchmaking stand-in at %s:%d (test)", test_host.c_str(), test_port);
  }
  {
    resolved = enet_address_set_host(&addr, test_mm ? test_host.c_str() : "mm.slippi.gg") == 0;
    if (!resolved) return "could not resolve the Slippi test server";
    for (int retry = 0; !client_ && retry < 15; ++retry) {
      host_port_ = forced_port ? forced_port : std::uniform_int_distribution<int>(41000, 50999)(rng);
      ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = (enet_uint16)host_port_;
      client_ = enet_host_create(&addr, 1, 3, 0, 0);
    }
  }
  if (!resolved) return "could not resolve the Slippi server";
  if (forced_port && host_port_ == forced_port)
    host::log(client_ ? "slippi: forced port %d bound" : "slippi: forced port %d bind failed", host_port_);
  if (!client_) return "Failed to create mm client";
  server_ = enet_host_connect(client_, &addr, 3, 0);
  if (!server_) return "Failed to start connection to mm server";
  int attempts = 0;
  while (!is_mm_connected_) {
    if (is_mm_terminated_) return "Cancelled";
    ENetEvent ev;
    int net = enet_host_service(client_, &ev, 500);
    if (net <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) {
      if (++attempts >= 20) { host::log("slippi: cannot reach mm.slippi.gg"); return "Failed to connect to mm server"; }
      continue;
    }
    client_->intercept = intercept_callback;
    is_mm_connected_ = true;
    host::log("slippi: connected to matchmaking server");
  }
  std::string lan;
  lan = local_address_string(&addr);
  std::string lan_addr = lan.empty() ? "" : lan + ":" + std::to_string(host_port_);
  std::vector<uint8_t> code_buf(search_settings_.connect_code.begin(), search_settings_.connect_code.end());
  json req;
  req["type"] = "create-ticket";
  req["user"] = {{"uid", me.uid}, {"playKey", ""}, {"connectCode", me.connect_code}, {"displayName", me.display_name}};
  req["search"] = {{"mode", (int)search_settings_.mode}, {"connectCode", code_buf}};
  req["appVersion"] = SLIPPI_SEMVER;
  req["ipAddressLan"] = lan_addr;
  mm_send(server_, req);
  json resp;
  if (mm_receive(client_, resp, 5000, [this] { return is_mm_terminated_.load(); }) != 0) return is_mm_terminated_ ? "Cancelled" : "Failed to join mm queue";
  if (str_or(resp, "type", "") != "create-ticket-resp") {
    // The whole response can name other players: a public build logs its type alone (public_build.h).
    if (host::public_build::kOn) host::log("slippi: mm response of type '%s'", str_or(resp, "type", "").c_str());
    else host::log("slippi: mm response: %s", resp.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    return "Invalid response when joining mm queue";
  }
  std::string err = str_or(resp, "error", "");
  if (!err.empty()) { host::log("slippi: mm error: %s", err.c_str()); return err; }
  return "";
}

// Ends the blend's matchmaking client without waiting three seconds for a disconnect acknowledgement.
std::string Matchmaking::GetPlayerName(uint8_t port) const {
  std::lock_guard<std::mutex> lk(result_mutex_);
  for (const auto& player : player_info_) if (player.port == port + 1) return player.display_name;
  return {};
}

void Matchmaking::handleMatchmaking() {
  if (state_ != MATCHMAKING) return;
  json resp;
  if (mm_session_) {
    switch (mm_session_->state()) {
      case mm_service::State::Connecting:
      case mm_service::State::Queued:
        if (has_stun_addr_ && time_ms() - last_keepalive_ms_ >= kStunKeepaliveIntervalMs) sendStunKeepalive();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return;
      case mm_service::State::Failed: {
        std::string message = mm_session_->error_message();
        // The service knows a newer release: remembered, so the version gate names it from now on.
        const std::string latest = mm_session_->latest_version();
        if (!latest.empty() && user_) user_->OverwriteLatestVersion(latest);
        mm_session_.reset();
        if (message.empty()) message = "Matchmaking failed";
        host::log("slippi: mm error: %s", message.c_str());
        fail(message);
        return;
      }
      case mm_service::State::Matched: break;
      default:
        // Ended without a result: only when it was cancelled from under this search.
        mm_session_.reset();
        fail("Matchmaking was cancelled");
        return;
    }
    std::string text;
    const bool have = mm_session_->take_result_json(&text);
    mm_session_.reset();
    resp = have ? json::parse(text, nullptr, false) : json();
    if (!have || resp.is_discarded() || !resp.is_object()) {
      host::log("slippi: the match result was missing or unreadable");
      fail("Invalid response when getting mm status");
      return;
    }
  } else {
  int r = mm_receive(client_, resp, 2000);
  if (r == -1) return;
  if (r != 0) { fail("Lost connection to the mm server"); return; }
  if (str_or(resp, "type", "") != "get-ticket-resp") { fail("Invalid response when getting mm status"); return; }
  }
  std::string err = str_or(resp, "error", "");
  std::string latest = str_or(resp, "latestVersion", "");
  if (!err.empty()) {
    if (!latest.empty()) user_->OverwriteLatestVersion(latest);
    fail(err);
    host::log("slippi: mm error: %s", err.c_str());
    return;
  }
  if (!ingest_ticket(Ticket{resp})) return;
  bool blocked = false;
  {
    std::lock_guard<std::mutex> lk(result_mutex_);
    blocked = IsBlockedMatch(search_settings_.mode, search_settings_.blocked_codes, player_info_, local_player_index_);
  }
  terminateMmConnection();
  if (blocked) {
    // Turned down: the opponent is on the player's Unranked block list. A new ticket keeps the
    // search going; the other player's client sees a failed connection and searches again.
    host::log("slippi: matched a blocked connect code in Unranked; searching again");
    is_mm_connected_ = false;
    state_ = INITIALIZING;
    return;
  }
  state_ = OPPONENT_CONNECTING;
  host::log("slippi: opponent found (decider: %d)", is_host_);
}

// The part of a get-ticket-resp that describes the match: players, local index, the address to
// reach each remote (LAN when on the same external IP), decider, stages and items. Everything is
// read type-checked and the player list is checked against the fixed per-player arrays (see
// ParsedTicket); a ticket that fails is refused whole.
static Matchmaking::ParsedTicket parse_ticket(const json& resp) {
  Matchmaking::ParsedTicket t;
  t.match_id = str_or(resp, "matchId", "");
  const json* queue = json_member(resp, "players");
  if (!queue || !queue->is_array()) { t.error = "no player list"; return t; }
  if (queue->size() < 2 || queue->size() > (size_t)PLAYER_COUNT_MAX) { t.error = std::to_string(queue->size()) + " players"; return t; }
  std::string local_external_ip;
  bool have_local = false, port_used[PLAYER_COUNT_MAX] = {};
  for (const auto& el : *queue) {
    if (!el.is_object()) { t.error = "a player entry is not an object"; return t; }
    UserInfo p;
    bool is_local = bool_or(el, "isLocalPlayer", false);
    p.uid = str_or(el, "uid", "");
    p.display_name = str_or(el, "displayName", "");
    p.connect_code = str_or(el, "connectCode", "");
    p.port = int_or(el, "port", 0);
    p.is_bot = bool_or(el, "isBot", false);
    if (const json* rank = json_member(el, "rank"))
      if (rank->is_object()) {
        auto number = [&](const char* key) { const json* v = json_member(*rank, key); return v && v->is_number() ? v->get<double>() : 0.0; };
        p.ranked_rating = (float)number("rating");
        p.ranked_update_count = (int)number("updateCount");
        p.ranked_global_placement = (int)number("globalPlacement");
        p.ranked_regional_placement = (int)number("regionalPlacement");
        p.ranked_mmr = (float)number("mmr");
      }
    if (p.port < 1 || p.port > PLAYER_COUNT_MAX) { t.error = "player port " + std::to_string(p.port); return t; }
    if (port_used[p.port - 1]) { t.error = "port " + std::to_string(p.port) + " used twice"; return t; }
    port_used[p.port - 1] = true;
    if (const json* cm = json_member(el, "chatMessages"))
      if (cm->is_array()) for (const auto& m : *cm) if (m.is_string()) p.chat_messages.push_back(m.get<std::string>());
    if (p.chat_messages.size() != 16) p.chat_messages = User::GetDefaultChatMessages();
    t.players.push_back(p);
    if (is_local) {
      std::string ip = str_or(el, "ipAddress", "1.1.1.1:123");
      local_external_ip = ip.substr(0, ip.find(':'));
      t.local_player_index = p.port - 1;
      have_local = true;
    }
  }
  if (!have_local) { t.error = "no local player"; return t; }
  for (const auto& el : *queue) {
    if (int_or(el, "port", 0) - 1 == t.local_player_index) continue;
    std::string ext = str_or(el, "ipAddress", "1.1.1.1:123");
    std::string lan = str_or(el, "ipAddressLan", "1.1.1.1:123");
    const auto address = remote_address(local_external_ip,ext,lan);
    t.remote_ips.push_back(address.first);
    t.public_fallbacks.push_back(address.fallback);
  }
  t.is_host = bool_or(resp, "isHost", false);
  if (const json* stages = json_member(resp, "stages"))
    if (stages->is_array()) for (const auto& s : *stages) if (s.is_number()) t.stages.push_back((uint16_t)s.get<int>());
  if (t.stages.empty()) {
    t.stages = {0x3, 0x8, 0x1C, 0x1F, 0x20};
    if (t.players.size() == 2) t.stages.push_back(0x2);
  }
  t.items = uint_or(resp, "items", 0u);
  t.ok = true;
  return t;
}

Matchmaking::ParsedTicket Matchmaking::ParseTicket(const std::string& json_text) {
  ParsedTicket t;
  try {
    json resp = json::parse(json_text, nullptr, false);
    if (resp.is_discarded()) { t.error = "not JSON"; return t; }
    return parse_ticket(resp);
  } catch (const std::exception& e) {
    t = ParsedTicket();
    t.error = e.what();
  }
  return t;
}

bool Matchmaking::ingest_ticket(const Ticket& ticket) {
  ParsedTicket t = parse_ticket(ticket.resp);
  host::log("slippi: match id %s", t.match_id.c_str());
  remote_ips_.clear();
  public_fallbacks_.clear();
  MatchmakeResult result;
  if (t.ok) { result.id = t.match_id; result.players = t.players; result.stages = t.stages; result.items = t.items; }
  // The result is swapped in complete. A netplay client left from an earlier attempt is destroyed
  // after the lock is released (its destructor waits on its thread).
  std::unique_ptr<NetplayClient> old_client;
  {
    std::lock_guard<std::mutex> lk(result_mutex_);
    old_client = std::move(netplay_client_);
    if (!t.ok) {
      player_info_.clear();
    } else {
      player_info_.swap(t.players);
      local_player_index_ = t.local_player_index;
      allowed_stages_.swap(t.stages);
      mm_result_ = std::move(result);
    }
  }
  old_client.reset();
  if (!t.ok) {
    host::log("slippi: matchmaking ticket refused: %s", t.error.c_str());
    fail("Invalid match details from the mm server");
    return false;
  }
  remote_ips_.swap(t.remote_ips);
  public_fallbacks_.swap(t.public_fallbacks);
  is_host_ = t.is_host;
  return true;
}

void Matchmaking::handleConnecting() {
  if (!local_peer.enabled &&
      agent_lock::blocked(agent_lock::Operation::PeerConnect)) { fail(agent_lock::refusal); return; }
  if (!local_peer.enabled) {
    std::string error;
    User* user = user_;
    if (!user || !user->CheckSessionCompatibility(&error)) {
      fail(error.empty() ? "Could not check Slippi version. Try again." : error);
      return;
    }
    if (is_mm_terminated_ || state_ != OPPONENT_CONNECTING) return;
  }
  {
    std::unique_ptr<NetplayClient> old_client;
    { std::lock_guard<std::mutex> lk(result_mutex_); old_client = std::move(netplay_client_); }
  }
  std::vector<std::string> addrs;
  std::vector<uint16_t> ports;
  for (auto& ip : remote_ips_) {
    size_t colon = ip.find(':');
    addrs.push_back(ip.substr(0, colon));
    ports.push_back((uint16_t)std::atoi(ip.substr(colon + 1).c_str()));
  }
  const uint8_t remote_count = (uint8_t)std::min<size_t>(remote_ips_.size(), (size_t)REMOTE_PLAYER_MAX);
  auto client = std::make_unique<NetplayClient>(addrs, ports, remote_count, (uint16_t)host_port_, is_host_, (uint8_t)local_player_index_, public_fallbacks_);
  bool handed_over = false;
  while (!handed_over) {
    auto st = client->GetSlippiConnectStatus();
    if (st == NetplayClient::ConnectStatus::INITIATED) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (state_ != OPPONENT_CONNECTING) return;
      continue;
    }
    if (st != NetplayClient::ConnectStatus::CONNECTED && client->GetRemoteBuild(0).received) {
      // It did connect: the opponent's build arrived, then they left (their build check refused
      // this one). Hand the client over so the build check can say why, instead of a plain failure.
      { std::lock_guard<std::mutex> lk(result_mutex_); netplay_client_ = std::move(client); }
      break;
    }
    if (st != NetplayClient::ConnectStatus::CONNECTED) {
      if (local_peer.enabled) { fail("Could not connect to the local peer"); return; }
      if (search_settings_.mode == TEAMS) { fail("Timed out waiting for other players to connect"); return; }
      host::log("slippi: connection attempt failed, searching again");
      state_ = INITIALIZING;
      return;
    }
    { std::lock_guard<std::mutex> lk(result_mutex_); netplay_client_ = std::move(client); }
    handed_over = true;
  }
  state_ = CONNECTION_SUCCESS;
}


}  // namespace slippi
