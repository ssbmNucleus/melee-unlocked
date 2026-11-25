// Custom fighters online: see online_fighters.h and docs/online-custom-content.md.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "online_fighters.h"

#include "fighter_package.h"
#include "host.h"
#include "http_client.h"
#include "release_scope.h"
#include "roster.h"
#include "test_env.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace host::online_fighters {
namespace {

namespace fs = std::filesystem;

constexpr size_t kRosterMax = 16;   // the game's roster table (mu_mex_roster.c ROSTER_MAX)

// One roster fighter, as last published.
struct Entry {
  fs::path path;
  std::string name;                                     // the player's words: printable, at most 24 letters
  std::vector<uint8_t> record;
  std::vector<std::pair<std::string, fs::path>> files;  // served name, file
  std::string why_not;                                  // "" when it may be offered online
  bool hashed = false;
  cw::Sha256 content{};
};

// What one peer said and is.
struct PeerState {
  bool have_set = false;
  std::vector<cw::HaveEntry> have;
};

std::mutex g_mutex;
bool g_usable = false;
std::vector<Entry> g_roster;
uint32_t g_generation = 1;                             // have_generation()
std::map<std::string, std::string> g_file_sha;         // "path|size|time" -> hex
PeerState g_peer[4];
std::vector<Peer> g_session;                           // update_session's list
uint64_t g_connected_ms = 0;                           // and how long ago the connection was made
constexpr uint64_t kBuildWaitMs = 3000, kListWaitMs = 10000;
std::string g_line;
bool g_spinner = false;
std::map<uint8_t, std::string> g_match;                // match_fighters()

// MELEE_TEST_NO_CUSTOM_FIGHTERS=1: a Nucleus from before custom fighters online, for tests. And the
// same whenever this release has no Fighters (release_scope.h): no capability bit, no list, nothing
// offered, so two such clients play as Nucleus did before custom fighters online.
bool test_off() {
  static const bool off = [] {
    const char* v = host::test_env::getenv("MELEE_TEST_NO_CUSTOM_FIGHTERS");
    return v && v[0] == '1';
  }();
  return off || !host::release_scope::on(host::release_scope::kFighters);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

// The fighter's name as the select screen's line can draw it.
std::string display_name(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (out.size() >= 24) break;
    if (std::isalnum((unsigned char)c) || c == ' ' || c == '.' || c == '-' || c == '\'') out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out.empty() ? std::string("This fighter") : out;
}

std::string short_hex(const cw::Sha256& h) { return cw::hex(h).substr(0, 16); }

bool from_hex(const std::string& text, cw::Sha256* out) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    uint8_t v = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = (char)std::tolower((unsigned char)text[i * 2 + k]);
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (d < 0) return false;
      v = (uint8_t)(v << 4 | d);
    }
    (*out)[i] = v;
  }
  return true;
}

// Caller holds g_mutex. The SHA-256 of a file, kept while its size and time stay.
bool file_sha_locked(const fs::path& path, std::string* hex, std::string* why) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec) { *why = "cannot read " + path.filename().u8string(); return false; }
  const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
  const std::string key = path.u8string() + "|" + std::to_string(size) + "|" + std::to_string((long long)time);
  if (const auto it = g_file_sha.find(key); it != g_file_sha.end()) { *hex = it->second; return true; }
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) { *why = "cannot read " + path.filename().u8string(); return false; }
  *hex = host::http::sha256_hex(bytes.data(), bytes.size());
  g_file_sha[key] = *hex;
  return true;
}

bool content_hash_locked(const std::vector<uint8_t>& record, std::vector<std::pair<std::string, fs::path>> files,
                         cw::Sha256* out, std::string* why) {
  files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f) { return presentation_only(f.first); }), files.end());
  for (auto& f : files) f.first = lower(f.first);   // the disc's names do not care about case
  std::sort(files.begin(), files.end());
  std::string buf = "nucleus-fighter-2";
  buf.push_back('\0');
  const auto put32 = [&buf](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) buf.push_back((char)(uint8_t)(v >> s));
  };
  put32((uint32_t)record.size());
  buf.append((const char*)record.data(), record.size());
  put32((uint32_t)files.size());
  for (const auto& [name, path] : files) {
    std::string hex;
    if (!file_sha_locked(path, &hex, why)) return false;
    buf += name;
    buf.push_back('\0');
    buf += lower(hex);
  }
  return from_hex(host::http::sha256_hex(buf.data(), buf.size()), out);
}

// Caller holds g_mutex: every offered fighter hashed. False while one cannot be (then it is not offered).
void prepare_locked() {
  bool changed = false;
  for (Entry& e : g_roster) {
    if (e.hashed || !e.why_not.empty()) continue;
    std::string why;
    if (content_hash_locked(e.record, e.files, &e.content, &why)) {
      e.hashed = true;
      host::log("online fighters: %s is content %s", e.name.c_str(), short_hex(e.content).c_str());
    } else {
      e.why_not = why;
      host::log("online fighters: %s is not offered online: %s", e.name.c_str(), why.c_str());
    }
    changed = true;
  }
  if (changed) ++g_generation;
}

bool offered_locked(size_t k) {
  return g_usable && k < g_roster.size() && k < kRosterMax && g_roster[k].why_not.empty();
}

std::string player(uint8_t port) { return "P" + std::to_string(port + 1); }

// Caller holds g_mutex. The verdict on roster fighter k with the session as it is.
Verdict verdict_locked(size_t k, std::string* why) {
  const auto say = [why](const std::string& s) { if (why) *why = s; };
  if (!offered_locked(k)) { say("This fighter can't be played online."); return kRefused; }
  const Entry& e = g_roster[k];
  if (g_session.empty()) return kSelectable;
  if (!e.hashed) { say("Checking " + e.name + "'s files."); return kPending; }
  Verdict worst = kSelectable;
  std::string pending;
  for (const Peer& p : g_session) {
    if (!p.build_known && g_connected_ms < kBuildWaitMs) { worst = kPending; pending = "Checking " + player(p.port) + "'s fighters."; continue; }
    if (!p.takes) { say(player(p.port) + "'s game cannot use added fighters."); return kRefused; }
    const PeerState& s = g_peer[p.port & 3];
    if (!s.have_set) {
      if (g_connected_ms >= kListWaitMs) { say(player(p.port) + "'s game cannot use added fighters."); return kRefused; }
      worst = kPending;
      pending = "Checking " + player(p.port) + "'s fighters.";
      continue;
    }
    const bool has = std::any_of(s.have.begin(), s.have.end(), [&](const cw::HaveEntry& h) { return h.content == e.content; });
    if (!has) { say(player(p.port) + " does not have " + e.name + " installed."); return kRefused; }
  }
  if (worst == kPending) say(pending);
  return worst;
}

}  // namespace

bool presentation_only(const std::string& served_name) {
  const std::string n = lower(served_name);
  const size_t dot = n.rfind('.');
  const std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
  if (ext == "ssm" || ext == "sem" || ext == "hps" || ext == "thp" || ext == "mth") return true;
  return n.rfind("ftdemo", 0) == 0 || n.rfind("gmrst", 0) == 0;
}

bool enabled() { return !test_off(); }

void roster_published(bool usable) {
  const auto& list = host::roster::fighters();
  std::lock_guard<std::mutex> lock(g_mutex);
  usable = usable && !test_off();
  std::vector<Entry> next;
  std::set<std::string> earlier;
  for (size_t k = 0; k < list.size(); ++k) {
    const auto& f = list[k];
    Entry e;
    e.path = f.path;
    e.name = display_name(f.name);
    e.record = f.record;
    for (const auto& file : f.files) e.files.emplace_back(file.name, file.path);
    std::string clash;
    for (const auto& file : f.files)
      if (earlier.count(lower(file.name)) && clash.empty()) clash = "its file " + file.name + " clashes with another fighter's";
    for (const auto& file : f.files) earlier.insert(lower(file.name));
    // A fighter with code of its own (a .mufighter plugin, as Wolf and Shulk) waits for plugin trust:
    // what its fighter.json gives the game that is not the game's own, or code beside its files.
    std::string code;
    {
      std::ifstream in(f.path / "fighter.json", std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      int donor = -1;
      code = host::fighter_package::needs_plugin(host::fighter_package::manifest_facts(text, &donor));
      std::error_code ec;
      if (fs::is_directory(f.path / "code", ec)) code = "a code folder";
      for (fs::directory_iterator it(f.path, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string ext = lower(it->path().extension().u8string());
        if (ext == ".mufighter" || ext == ".c" || ext == ".h") code = "code beside its files";
      }
    }
    e.why_not = k >= kRosterMax ? "past the roster's 16 fighters"
              : !clash.empty() ? clash
              : !code.empty() ? "it brings code of its own (" + code + ")"
              : "";
    const bool said = std::any_of(g_roster.begin(), g_roster.end(), [&](const Entry& o) { return o.path == e.path && o.why_not == e.why_not; });
    if (!e.why_not.empty() && usable && !said)
      host::log("online fighters: %s is not offered online: %s", e.name.c_str(), e.why_not.c_str());
    // The hash carries over while the record and files are the same.
    for (const auto& old : g_roster)
      if (old.hashed && old.path == e.path && old.record == e.record && old.files == e.files) {
        e.hashed = true;
        e.content = old.content;
      }
    next.push_back(std::move(e));
  }
  // The game asks again at every screen that shows the roster: the list changes only with the set.
  bool same = g_roster.size() == next.size() && usable == g_usable;
  for (size_t k = 0; same && k < next.size(); ++k)
    same = g_roster[k].path == next[k].path && g_roster[k].record == next[k].record && g_roster[k].files == next[k].files &&
           g_roster[k].why_not == next[k].why_not && g_roster[k].hashed == next[k].hashed;
  g_usable = usable;
  if (same) return;
  g_roster = std::move(next);
  ++g_generation;
}

bool roster_usable() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_usable;
}

bool is_custom(uint8_t id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_usable && id >= cw::kFirstId && (size_t)(id - cw::kFirstId) < g_roster.size();
}

uint32_t offered_online() {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t bits = 0;
  for (size_t k = 0; k < g_roster.size() && k < kRosterMax; ++k)
    if (offered_locked(k)) bits |= 1u << k;
  return bits;
}

uint32_t installed_mask() {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t bits = 0;
  for (size_t k = 0; k < g_roster.size() && k < 32; ++k) bits |= 1u << k;
  return bits;
}

void prepare() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_usable) prepare_locked();
}

bool local_have(cw::Have* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  out->entries.clear();
  for (size_t k = 0; k < g_roster.size() && k < kRosterMax; ++k) {
    if (!offered_locked(k)) continue;
    if (!g_roster[k].hashed) return false;
    out->entries.push_back({(uint8_t)(cw::kFirstId + k), g_roster[k].content});
  }
  return true;
}

uint32_t have_generation() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_generation;
}

void peer_have(const cw::Have& have) {
  if (have.sender_port > 3) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  PeerState& s = g_peer[have.sender_port];
  if (s.have_set && s.have == have.entries) return;
  s.have_set = true;
  s.have = have.entries;
  std::string list;
  for (const auto& h : have.entries) list += (list.empty() ? "" : ", ") + std::to_string(h.id) + " " + short_hex(h.content);
  host::log("online fighters: %s has %zu added fighter(s)%s%s", player(have.sender_port).c_str(), have.entries.size(),
            list.empty() ? "" : ": ", list.c_str());
}

bool peer_list_known(uint8_t port) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return port < 4 && g_peer[port].have_set;
}

void update_session(const std::vector<Peer>& peers, uint64_t connected_ms) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!peers.empty() && g_usable) prepare_locked();
  g_session = peers;
  g_connected_ms = connected_ms;
}

void reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& p : g_peer) p = PeerState{};
  g_session.clear();
  g_connected_ms = 0;
  g_line.clear();
  g_spinner = false;
  g_match.clear();
}

Verdict verdict(uint8_t id, std::string* why) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (id < cw::kFirstId) return kSelectable;
  return verdict_locked((size_t)(id - cw::kFirstId), why);
}

uint32_t selectable_mask() {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t bits = 0;
  for (size_t k = 0; k < g_roster.size() && k < kRosterMax; ++k)
    if (verdict_locked(k, nullptr) == kSelectable) bits |= 1u << k;
  return bits;
}

std::string reason(int k) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string why;
  if (k < 0 || verdict_locked((size_t)k, &why) == kSelectable) return "";
  return why;
}

bool every_peer_has(uint8_t id, const std::vector<uint8_t>& ports) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (ports.empty() || id < cw::kFirstId) return false;
  const size_t k = (size_t)(id - cw::kFirstId);
  if (!offered_locked(k) || !g_roster[k].hashed) return false;
  for (uint8_t port : ports) {
    if (port > 3 || !g_peer[port].have_set) return false;
    const auto& have = g_peer[port].have;
    if (std::none_of(have.begin(), have.end(), [&](const cw::HaveEntry& h) { return h.content == g_roster[k].content; }))
      return false;
  }
  return true;
}

bool map_remote(uint8_t port, uint8_t sent_id, uint8_t* local_id, cw::Sha256* content) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (port > 3 || !g_usable || !g_peer[port].have_set) return false;
  const auto& have = g_peer[port].have;
  const auto it = std::find_if(have.begin(), have.end(), [sent_id](const cw::HaveEntry& h) { return h.id == sent_id; });
  if (it == have.end()) return false;
  for (size_t k = 0; k < g_roster.size() && k < kRosterMax; ++k)
    if (offered_locked(k) && g_roster[k].hashed && g_roster[k].content == it->content) {
      *local_id = (uint8_t)(cw::kFirstId + k);
      if (content) *content = it->content;
      return true;
    }
  return false;
}

bool content_of(uint8_t local_id, cw::Sha256* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (local_id < cw::kFirstId) return false;
  const size_t k = (size_t)(local_id - cw::kFirstId);
  if (!offered_locked(k)) return false;
  prepare_locked();
  if (!g_roster[k].hashed) return false;
  *out = g_roster[k].content;
  return true;
}

void set_match(const cw::Sha256* content, size_t ports) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::map<uint8_t, std::string> match;
  for (size_t port = 0; port < ports && port < 4; ++port) {
    if (content[port] == cw::Sha256{}) continue;
    std::string text = cw::hex(content[port]);
    for (const auto& e : g_roster)
      if (e.hashed && e.content == content[port]) { text += " " + e.name; break; }
    match[(uint8_t)port] = text;
  }
  if (match != g_match && !match.empty())
    for (const auto& m : match) host::log("online fighters: this match's P%u plays %s", m.first + 1u, m.second.c_str());
  g_match = std::move(match);
}

std::map<uint8_t, std::string> match_fighters() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_match;
}

void set_line(const std::string& text, bool spinner) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (text != g_line) host::log("online fighters: select screen line \"%s\"", text.c_str());
  g_line = text;
  g_spinner = spinner;
}

std::string line(bool* spinner) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (spinner) *spinner = g_spinner;
  return g_line;
}

}  // namespace host::online_fighters
