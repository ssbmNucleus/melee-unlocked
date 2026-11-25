// Doubles lobbies, the host's half (lobby_directory.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lobby_directory.h"
#include "host.h"
#include "http_client.h"
#include "public_build.h"
#include "test_env.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "nlohmann/json.hpp"

namespace host::lobby_directory {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using std::chrono::seconds;

constexpr size_t kMaxBody = 64 * 1024;   // a list of 50 lobbies is about 15 KB
constexpr int kListLimit = 25;
constexpr auto kListEvery = seconds(15);    // the directory's own limit for the list
constexpr auto kSeenFor = seconds(60);      // how long a look at the directory says whether it offers lobbies
constexpr auto kKeepRows = seconds(60);     // rows still shown while a refresh fails
constexpr auto kFilledGrace = seconds(10);  // a lobby seen playing before this side's match formed
constexpr int kBackoffFirst = 5, kBackoffMax = 60;

// The game's requests (CMD_LOBBY payload[0]); shim/mu_lobby_list.c has the same numbers.
enum Request : uint8_t { kStatus, kMenu, kList, kCreate, kCancel, kMatched, kSearch, kAck, kEntered, kJoinRow };
// kJoinRow's answers: the row is no lobby (the Gallery's own row answers A), the join is on its way, or
// it was refused (a lobby on its way already, or that one is gone).
enum JoinRow : uint8_t { kJoinNotALobby, kJoinAsked, kJoinRefused };
constexpr auto kNoticeFor = seconds(8);   // a failed join's reason on the list's bar
constexpr size_t kGenerationsKept = 8;    // the lists whose rows A may still name (State::generations)
enum Avail : uint8_t { kUnknown, kAvailable, kUnavailable };
enum Phase : uint8_t { kNone, kCreating, kJoining, kHosting, kJoined, kEnded, kFailed };
enum Flags : uint8_t { kFlagHost = 1, kFlagMatched = 2, kFlagEntered = 4, kFlagReleased = 8 };
enum ListStatus : uint8_t { kListLoading, kListReady, kListError };
// The Gallery's requests and states (shim/mu_mods_gallery.c).
enum Gallery : uint8_t { kOpen = 9, kRows, kChange, kDetail, kPicture, kAction };
enum : uint8_t { kChangeRetry = 4 };
enum : uint8_t { kCardIdle = 0, kCardBusy = 1, kCardFailed = 3 };
constexpr size_t kHeadBytes = 124, kRowName = 40, kRowRight = 12;

struct Lobby {
  std::string id, host_name, region, build, client;   // (the directory's ruleset is not read: never shown)
  std::vector<std::string> tags;
  int waiting = 0, slots = 4;
  int64_t created = 0;   // Unix seconds, 0 unknown
  uint32_t key = 0;      // FNV-1a of the id: the Gallery's row check
};

struct Session {
  Phase phase = kNone;
  bool host = false;
  uint64_t serial = 0;           // a new session each create or join; a stale answer is dropped
  bool sent = false;             // the create or join request went out
  bool cancelled = false;        // cancelled while its create or join was out: close it on arrival
  std::string id, code, token, host_name, message;
  int waiting = 0;
  int heartbeat_seconds = 30;
  Clock::time_point last_beat{}, next_beat{};
  int backoff = 0;
  bool matched = false;          // the game's Teams match formed
  bool playing_sent = false;     // host: the directory has the lobby as playing
  bool want_open = false;        // host: a new search after a match: listed again
  bool released = false;         // join: the seat was given up once the match formed
  bool entered = false;          // the game entered the Teams select with the code
  Clock::time_point filled_since{};
  Identity me;                   // who creates or joins (read on the game thread)
};

struct Delete {
  std::string path, token;
  int tries = 0;
  Clock::time_point due{};
};

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  bool worker_started = false;
  bool stop = false;
  std::atomic<bool> setting{true};
  bool allowed = false;
  std::string my_build;
  IdentitySource identity_source = nullptr;   // the game thread's (source_host_lobbies.cpp), for a join
  // the directory
  Avail avail = kUnknown;
  std::string unavailable_reason;
  Clock::time_point seen{};          // the last answer that said whether lobbies are offered
  bool probe_wanted = false;
  // the list
  bool list_open = false;
  bool refresh_wanted = false;
  ListStatus list_status = kListLoading;
  std::string list_message;
  std::vector<Lobby> lobbies;        // this build's, newest first
  int other_builds = 0;
  Clock::time_point list_fetched{}, list_tried{}, list_next{};
  int list_backoff = 0;
  std::map<std::string, std::string> join_failed;   // lobby id -> why the last join failed
  // The list's generation, in every head the game reads (bytes 5-6; never 0): a new one whenever the
  // lobbies (their ids, in order) change. The game sends back the one its rows were drawn from with A,
  // and `generations` keeps the ids of the last few, row by row: A joins the lobby the row showed.
  uint16_t list_generation = 0;
  std::deque<std::pair<uint16_t, std::vector<std::string>>> generations;
  std::string notice;                   // why the last join failed, on the bar until notice_until
  Clock::time_point notice_until{};
  // MELEE_TEST_LOBBY_RACE=<seconds> (an automated run only): after a look that changed the lobbies, the
  // game's head reads (kRows asking no rows) still get the list before it for that long, its row reads
  // the new one. That holds open the race of a look landing between the game's head read and its row
  // read (shim/mu_mods_gallery.c fetch_rows then refuses the rows and keeps drawing the old ones).
  std::vector<Lobby> race_old;
  uint16_t race_old_generation = 0;
  Clock::time_point race_until{};
  // the player's lobby
  Session session;
  uint64_t next_serial = 1;
  std::deque<Delete> deletes;
};
// Never freed: the worker may still run while the process ends.
State& g = *new State;

// ---- small checks ----

bool is_hex_lower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool uuid_ok(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
    else if (!is_hex_lower(s[i])) return false;
  }
  return true;
}

bool token_ok(const std::string& s) {
  if (s.size() < 20 || s.size() > 128) return false;
  for (char c : s)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}

// What the game can type into its Teams code entry: A-Z and 0-9, at most one '#' (a connect code,
// if the directory ever hands one out), 8 characters at most (the code entry's letters).
bool code_ok(const std::string& s) {
  if (s.empty() || s.size() > 8) return false;
  int hashes = 0;
  for (char c : s) {
    if (c == '#') { if (++hashes > 1) return false; }
    else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return s.front() != '#' && s.back() != '#';
}

bool name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
         c == '_' || c == '!' || c == '?' || c == '-';
}

bool build_ok(const std::string& s) {
  if (s == "retail") return true;
  if (s.size() < 8 || s.size() > 16) return false;
  return std::all_of(s.begin(), s.end(), is_hex_lower);
}

uint32_t fnv(const std::string& s) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char c : s) h = (h ^ c) * 0x01000193u;
  return h ? h : 1;
}

// "2026-11-23T18:00:00.000000Z" as Unix seconds; 0 when it is not that.
int64_t parse_time(const std::string& s) {
  int y, mo, d, h, mi, se;
  if (s.size() < 20 || s.size() > 40 || std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6)
    return 0;
  std::tm t{};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = se;
#ifdef _WIN32
  const int64_t v = (int64_t)_mkgmtime(&t);
#else
  const int64_t v = (int64_t)timegm(&t);
#endif
  return v > 0 ? v : 0;
}

bool get_string(const json& o, const char* key, std::string* out, size_t max) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_string()) return false;
  const std::string& s = it->get_ref<const std::string&>();
  if (s.size() > max) return false;
  *out = s;
  return true;
}

bool get_int(const json& o, const char* key, int* out, int lo, int hi) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_number_integer()) return false;
  const int64_t v = it->get<int64_t>();
  if (v < lo || v > hi) return false;
  *out = (int)v;
  return true;
}

// The text the game's menu font draws (shim/mu_mods_gallery.c drawable()): anything else a space.
std::string shown(const std::string& s, size_t max) {
  std::string out;
  for (char c : s) {
    if (out.size() >= max) break;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                    c == '.' || c == ',' || c == '\'' || c == ':' || c == '-';
    out += ok ? c : ' ';
  }
  return out;
}

const char* region_name(const std::string& r, bool longer) {
  if (r == "na") return longer ? "North America" : "NA";
  if (r == "sa") return longer ? "South America" : "SA";
  if (r == "eu") return longer ? "Europe" : "EU";
  if (r == "asia") return "Asia";
  if (r == "oce") return longer ? "Oceania" : "OCE";
  if (r == "mea") return longer ? "Middle East and Africa" : "MEA";
  return longer ? "Somewhere else" : "Other";
}

// ---- requests to the directory ----

struct Base {
  bool ok = false;
  std::string origin;   // "https://ssbmnucleus.net", or a test stand-in's "http://127.0.0.1:<port>"
  std::string prefix;   // a test stand-in's path before /api
};

// MELEE_TEST_LOBBY_API in an automated run: http://127.0.0.1:<port>[/<path>], nothing else.
Base base() {
  Base b;
  const char* v = host::test_env::automated_getenv("MELEE_TEST_LOBBY_API");
  if (v == nullptr || *v == 0) {
    b.ok = true;
    b.origin = "https://ssbmnucleus.net";
    return b;
  }
  const std::string s = v;
  const std::string head = "http://127.0.0.1:";
  if (s.rfind(head, 0) != 0) return b;
  size_t i = head.size();
  int port = 0, digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 6) port = port * 10 + (s[i++] - '0'), ++digits;
  if (digits == 0 || port <= 0 || port > 65535) return b;
  std::string prefix = s.substr(i);
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  if (!prefix.empty() && prefix[0] != '/') return b;
  for (char c : prefix)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_'))
      return b;
  b.ok = true;
  b.origin = head + std::to_string(port);
  b.prefix = prefix;
  return b;
}

struct Reply {
  int status = 0;              // 0: no answer
  unsigned long error = 0;     // the system's error (WinHTTP's) when no answer came
  bool timed_out = false;      // no answer in time
  std::string why;             // what went wrong, for the log
  int retry_after = 0;
  bool too_large = false;
  bool json_type = false;      // Content-Type application/json
  std::string body;
  int ms = 0;
};

// One request, through the host's one HTTP client (http_client.h). `body` empty: none. `bearer`: an
// owner or join token (never logged). No redirects (the API never sends one; nothing follows it
// elsewhere), no cookies; an error answer's body is kept (its error code). Worker thread, and shutdown().
Reply send(const char* method, const std::string& path, const std::string& body, const std::string& bearer,
           int timeout_ms) {
  Reply r;
  const Base b = base();
  if (!b.ok) {
    host::log("lobbies: MELEE_TEST_LOBBY_API is not http://127.0.0.1:<port>; nothing sent");
    return r;
  }
  host::http::Request req;
  req.method = method;
  req.url = b.origin + b.prefix + path;
  req.body = body;
  req.headers.push_back("Accept: application/json");
  if (!body.empty()) req.headers.push_back("Content-Type: application/json");
  if (!bearer.empty()) req.headers.push_back("Authorization: Bearer " + bearer);
  req.max_bytes = kMaxBody;
  req.resolve_ms = req.connect_ms = req.send_ms = req.receive_ms = timeout_ms;
  req.max_redirects = 0;
  req.keep_error_body = true;
  req.allow_loopback_http = b.origin.rfind("http://127.0.0.1:", 0) == 0;
  req.log = false;   // log_reply writes the request's one line
  host::http::Response got = host::http::fetch(req);
  r.status = got.status;
  r.error = got.system_error;
  r.timed_out = got.timed_out();
  r.why = got.message;
  r.retry_after = got.status == 429 ? got.retry_after : 0;
  r.too_large = got.error == host::http::Error::TooLarge;
  r.json_type = got.content_type.find("application/json") != std::string::npos;
  r.body = std::move(got.body);
  r.ms = got.ms;
  return r;
}

// The request's one log line: method, path (a lobby's id is public), the body's field names (never
// their values: a name is the player's), and how it ended.
void log_reply(const char* method, const std::string& path, const json* fields, const Reply& r, const std::string& more) {
  std::string names;
  if (fields)
    for (auto it = fields->begin(); it != fields->end(); ++it) names += (names.empty() ? "" : " ") + it.key();
  std::string outcome;
  if (r.status) {
    outcome = std::to_string(r.status);
    if (r.retry_after) outcome += " (Retry-After " + std::to_string(r.retry_after) + " s)";
    if (r.too_large) outcome += ", over " + std::to_string(kMaxBody) + " bytes";
  } else {
    outcome = "no answer (" + (r.why.empty() ? std::string("not sent") : r.why) + ")";
  }
  host::log("lobbies: %s %s%s%s%s -> %s, %d ms%s%s", method, path.c_str(), names.empty() ? "" : " (fields ",
            names.c_str(), names.empty() ? "" : ")", outcome.c_str(), r.ms, more.empty() ? "" : ", ", more.c_str());
}

// The directory's JSON answer, strictly: an object, from a JSON response.
bool parse_object(const Reply& r, json* out) {
  if (r.too_large || r.body.empty()) return false;
  json j = json::parse(r.body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  *out = std::move(j);
  return true;
}

std::string error_code(const Reply& r) {
  json j;
  if (!parse_object(r, &j)) return "";
  const auto e = j.find("error");
  std::string code;
  if (e != j.end() && e->is_object()) get_string(*e, "code", &code, 40);
  return code;
}

// A temporary failure: worth trying again later (no answer, 5xx, a 429 or a cut-off body).
bool temporary(const Reply& r) {
  return r.status == 0 || r.status == 429 || r.status >= 500 || r.too_large;
}

int next_backoff(int was) { return was == 0 ? kBackoffFirst : std::min(was * 2, kBackoffMax); }

// The interval a lobby's heartbeats keep: what the directory says, within reason (the stand-in's
// --fast-expiry says 3 s; the real directory 30 s).
int beat_seconds(int said, bool stand_in) {
  return std::clamp(said, stand_in ? 2 : 20, 120);
}

bool stand_in() {
  const char* v = host::test_env::automated_getenv("MELEE_TEST_LOBBY_API");
  return v != nullptr && *v != 0;
}

// ---- the list ----

bool parse_lobby(const json& o, Lobby* l) {
  if (!o.is_object()) return false;
  std::string created;
  if (!get_string(o, "id", &l->id, 36) || !uuid_ok(l->id)) return false;
  if (!get_string(o, "host_name", &l->host_name, 32) || l->host_name.empty()) return false;
  if (!get_string(o, "region", &l->region, 8)) l->region = "other";
  if (!get_string(o, "build", &l->build, 16) || !build_ok(l->build)) return false;
  if (!get_string(o, "client", &l->client, 40)) l->client.clear();
  if (!get_int(o, "netlee_waiting", &l->waiting, 0, 8)) l->waiting = 0;
  if (!get_int(o, "slots", &l->slots, 1, 8)) l->slots = 4;
  if (get_string(o, "created_at", &created, 40)) l->created = parse_time(created);
  const auto tags = o.find("tags");
  if (tags != o.end() && tags->is_array()) {
    static const char* const known[] = {"newcomers", "chill", "serious", "bo3", "bo5", "endless"};
    for (const auto& t : *tags) {
      if (!t.is_string() || l->tags.size() >= 3) continue;
      const std::string& s = t.get_ref<const std::string&>();
      for (const char* k : known)
        if (s == k) l->tags.push_back(s);
    }
  }
  l->key = fnv(l->id);
  return true;
}

// MELEE_TEST_LOBBY_RACE's seconds (State::race_old), 0 when not set.
int race_seconds() {
  const char* v = host::test_env::automated_getenv("MELEE_TEST_LOBBY_RACE");
  return v ? std::clamp(std::atoi(v), 0, 600) : 0;
}

// The lobbies may have changed (under the lock): a new generation when their ids, in order, are not
// the last generation's.
void lobbies_changed() {
  std::vector<std::string> ids;
  for (const Lobby& l : g.lobbies) ids.push_back(l.id);
  if (!g.generations.empty() && g.generations.back().second == ids) return;
  g.list_generation = g.list_generation >= 0xFFFF ? 1 : (uint16_t)(g.list_generation + 1);
  g.generations.emplace_back(g.list_generation, std::move(ids));
  while (g.generations.size() > kGenerationsKept) g.generations.pop_front();
}

void take_list_now(const Reply& r, Clock::time_point now);

// A look at the list ended (worker, under the lock).
void take_list(const Reply& r, Clock::time_point now) {
  const uint16_t was = g.list_generation;
  std::vector<Lobby> before = g.lobbies;
  take_list_now(r, now);
  lobbies_changed();
  if (const int hold = race_seconds(); hold > 0 && g.list_generation != was) {
    g.race_old = std::move(before);
    g.race_old_generation = was;
    g.race_until = now + seconds(hold);
    host::log("lobbies: test race: the list's head reads keep the %zu lobbies before this look for %d s", g.race_old.size(), hold);
  }
}

void take_list_now(const Reply& r, Clock::time_point now) {
  g.list_tried = now;
  json j;
  const bool object = r.status == 200 && parse_object(r, &j);
  const auto data = object ? j.find("data") : j.end();
  if (r.status == 200 && object && data != j.end() && data->is_array() && data->size() <= 50) {
    std::vector<Lobby> mine;
    int other = 0;
    for (const auto& o : *data) {
      Lobby l;
      if (!parse_lobby(o, &l)) continue;
      if (l.build == g.my_build) mine.push_back(std::move(l));
      else ++other;
    }
    // Newest first, so a row stays where it is while the list is refreshed.
    std::stable_sort(mine.begin(), mine.end(), [](const Lobby& a, const Lobby& b) {
      return a.created != b.created ? a.created > b.created : a.id < b.id;
    });
    g.lobbies = std::move(mine);
    g.other_builds = other;
    g.list_status = kListReady;
    g.list_fetched = now;
    g.list_backoff = 0;
    g.list_next = now + kListEvery;
    g.avail = kAvailable;
    g.seen = now;
    g.unavailable_reason.clear();
    const int n = (int)g.lobbies.size();
    g.list_message = n == 0 ? "No open lobbies." : std::to_string(n) + (n == 1 ? " open lobby" : " open lobbies");
    if (n > 0 && other > 0) g.list_message += ", " + std::to_string(other) + " for other games";
    return;
  }
  if (r.status == 404 || r.status == 410 || (r.status == 200 && !object) ||
      (r.status == 200 && (data == j.end() || !data->is_array()))) {
    // The site answers, without lobbies: it does not offer them (yet).
    g.avail = kUnavailable;
    g.seen = now;
    g.unavailable_reason = "SSBM Nucleus does not list lobbies yet.";
    g.lobbies.clear();
    g.list_status = kListError;
    g.list_message = g.unavailable_reason;
    g.list_next = now + kSeenFor;
    return;
  }
  // Slow, out of reach or asking to wait: the rows stay a while, then the list says so.
  if (r.status == 429) g.list_backoff = std::max(r.retry_after, (int)kListEvery.count());
  else g.list_backoff = next_backoff(g.list_backoff);
  g.list_next = now + seconds(std::max(g.list_backoff, (int)kListEvery.count()));
  if (g.list_status != kListReady || now - g.list_fetched > kKeepRows) {
    g.list_status = kListError;
    g.lobbies.clear();
    g.list_message = "SSBM Nucleus is slow right now.";
  }
}

// ---- the player's lobby ----

void end_session(const std::string& why) {
  Session& s = g.session;
  s.phase = kEnded;
  s.message = why;
  s.token.clear();
}

// Give the lobby up (hosting: close it; joined: leave it): queued for the worker.
void queue_close(Session& s) {
  if (s.token.empty() || s.id.empty()) return;
  Delete d;
  d.path = s.host ? "/api/lobbies/" + s.id : "/api/lobbies/" + s.id + "/join";
  d.token = s.token;
  d.due = Clock::now();
  g.deletes.push_back(std::move(d));
  s.token.clear();
}

const char* closed_words(const std::string& reason, bool host) {
  if (reason == "expired") return "The lobby expired.";
  if (reason == "removed") return "The lobby was removed.";
  return host ? "The lobby closed." : "The host closed the lobby.";
}

// A lobby state answer (heartbeat): what it changes (worker, under the lock).
void take_state(Session& s, const json& j, Clock::time_point now) {
  std::string state, reason;
  get_string(j, "state", &state, 16);
  get_string(j, "closed_reason", &reason, 24);
  int waiting = 0, beat = 0;
  if (get_int(j, "netlee_waiting", &waiting, 0, 8)) s.waiting = waiting;
  if (get_int(j, "heartbeat_seconds", &beat, 1, 3600)) s.heartbeat_seconds = beat_seconds(beat, stand_in());
  const auto players = j.find("players");
  if (!s.host && players != j.end() && players->is_array() && !players->empty() && (*players)[0].is_string()) {
    const std::string& h = (*players)[0].get_ref<const std::string&>();
    if (!h.empty() && h.size() <= 32) s.host_name = h;
  }
  if (state == "closed") {
    host::log("lobbies: lobby %.8s closed (%s)", s.id.c_str(), reason.empty() ? "no reason" : reason.c_str());
    end_session(closed_words(reason, s.host));
    return;
  }
  if (!s.host && state == "playing" && !s.matched) {
    // The host's match formed. This side's may form a moment later (all four are matched at once):
    // only after a grace without it is the lobby full for this player.
    if (s.filled_since == Clock::time_point{}) s.filled_since = now;
    if (now - s.filled_since >= kFilledGrace) {
      host::log("lobbies: lobby %.8s is playing without this player", s.id.c_str());
      queue_close(s);
      end_session("The lobby filled.");
    }
    return;
  }
  s.filled_since = {};
  if (s.host && state == "playing") s.playing_sent = true;
  if (s.host && state == "open") s.playing_sent = false;
}

// ---- the worker ----

struct Job {
  enum Kind { kNoJob, kDelete, kCreate, kJoin, kBeat, kFetch } kind = kNoJob;
  std::string method, path, body, token;
  json fields;
  uint64_t serial = 0;
  bool host = false;
};

// The next request due now (under the lock), or none.
Job next_job(Clock::time_point now) {
  Job job;
  for (auto it = g.deletes.begin(); it != g.deletes.end(); ++it) {
    if (it->due > now) continue;
    job.kind = Job::kDelete;
    job.method = "DELETE";
    job.path = it->path;
    job.token = it->token;
    return job;
  }
  Session& s = g.session;
  if (s.phase == kCreating && !s.sent) {
    s.sent = true;
    job.kind = Job::kCreate;
    job.method = "POST";
    job.path = "/api/lobbies";
    // The directory requires a ruleset; the game offers no choice (and never shows one).
    job.fields = {{"host_name", s.me.name}, {"ruleset", "competitive"},
                  {"build", s.me.build}, {"client", s.me.client}};
    job.body = job.fields.dump();
    job.serial = s.serial;
    job.host = true;
    return job;
  }
  if (s.phase == kJoining && !s.sent) {
    s.sent = true;
    job.kind = Job::kJoin;
    job.method = "POST";
    job.path = "/api/lobbies/" + s.id + "/join";
    job.fields = {{"name", s.me.name}};
    job.body = job.fields.dump();
    job.serial = s.serial;
    return job;
  }
  if ((s.phase == kHosting || s.phase == kJoined) && !s.released && !s.token.empty() && now >= s.next_beat) {
    job.kind = Job::kBeat;
    job.method = "POST";
    job.path = "/api/lobbies/" + s.id + (s.host ? "/heartbeat" : "/join/heartbeat");
    job.token = s.token;
    if (s.host && s.matched && !s.playing_sent) job.fields = {{"state", "playing"}};
    else if (s.host && s.want_open && s.playing_sent) job.fields = {{"state", "open"}};
    if (!job.fields.is_null()) job.body = job.fields.dump();
    job.serial = s.serial;
    job.host = s.host;
    return job;
  }
  const bool look = g.list_open || g.probe_wanted || g.refresh_wanted;
  if (look && g.allowed && g.setting && now >= g.list_next) {
    g.probe_wanted = false;
    g.refresh_wanted = false;
    job.kind = Job::kFetch;
    job.method = "GET";
    job.path = "/api/public/v1/lobbies?limit=" + std::to_string(kListLimit);
    return job;
  }
  return job;
}

void apply(const Job& job, const Reply& r, Clock::time_point now) {
  Session& s = g.session;
  json j;
  switch (job.kind) {
  case Job::kDelete: {
    auto it = std::find_if(g.deletes.begin(), g.deletes.end(), [&](const Delete& d) { return d.path == job.path; });
    if (it == g.deletes.end()) return;
    // 200, 404 (gone already), 401/403: done. A temporary failure: twice more, then the directory's
    // own 90 s expiry closes it.
    if (temporary(r) && ++it->tries < 3) {
      it->due = now + seconds(r.status == 429 && r.retry_after > 0 ? std::min(r.retry_after, 60) : kBackoffFirst * it->tries);
    } else {
      g.deletes.erase(it);
    }
    return;
  }
  case Job::kCreate: {
    std::string id, code, token;
    int beat = 30;
    const bool good = r.status == 201 && parse_object(r, &j) && get_string(j, "id", &id, 36) && uuid_ok(id) &&
                      get_string(j, "code", &code, 16) && code_ok(code) && get_string(j, "owner_token", &token, 128) &&
                      token_ok(token);
    if (job.serial != s.serial || s.cancelled) {
      // Given up while the request was out: a lobby made after all is closed at once.
      if (good) {
        Session gone;
        gone.host = true;
        gone.id = id;
        gone.token = token;
        queue_close(gone);
        host::log("lobbies: lobby %.8s was cancelled before it came; closing it", id.c_str());
      }
      if (job.serial == s.serial) s = Session{};
      return;
    }
    if (good) {
      get_int(j, "heartbeat_seconds", &beat, 1, 3600);
      s.id = id;
      s.code = code;
      s.token = token;
      s.host_name = s.me.name;
      s.waiting = 1;
      s.heartbeat_seconds = beat_seconds(beat, stand_in());
      s.last_beat = now;
      s.next_beat = now + seconds(s.heartbeat_seconds);
      s.phase = kHosting;
      // A lobby's code lets anyone into its Teams search: a short hash in a public build's log.
      host::log("lobbies: hosting lobby %.8s, code %s, heartbeat every %d s", id.c_str(),
                host::public_build::log_id(code).c_str(), s.heartbeat_seconds);
      return;
    }
    const std::string e = error_code(r);
    s.phase = kFailed;
    if (r.status == 404 || r.status == 405 || (r.status >= 200 && r.status < 300)) {
      s.message = "SSBM Nucleus does not host lobbies yet.";
    } else if (e == "already_hosting") {
      s.message = "This network hosts a lobby already.";
    } else if (e == "blocked" || e == "banned" || r.status == 403) {
      s.message = "SSBM Nucleus does not take lobbies from you now.";
    } else if (r.status == 401) {
      s.message = "SSBM Nucleus wants a sign-in to host.";
    } else if (r.status == 429) {
      s.message = "SSBM Nucleus asks to wait.";
    } else if (r.status == 400 || r.status == 409 || r.status == 413 || r.status == 415) {
      s.message = "SSBM Nucleus refused the lobby.";
    } else if (r.status == 0 && !r.timed_out) {
      s.message = "SSBM Nucleus could not be reached.";
    } else {
      s.message = "SSBM Nucleus is slow right now.";
    }
    return;
  }
  case Job::kJoin: {
    std::string id, code, token;
    int beat = 30;
    const std::string wanted = job.path.size() > 13 ? job.path.substr(13, 36) : std::string();   // /api/lobbies/<id>/join
    const bool good = r.status == 201 && parse_object(r, &j) && get_string(j, "id", &id, 36) && id == wanted &&
                      get_string(j, "code", &code, 16) && code_ok(code) && get_string(j, "join_token", &token, 128) &&
                      token_ok(token);
    if (job.serial != s.serial || s.cancelled) {
      if (good) {
        Session gone;
        gone.id = id;
        gone.token = token;
        queue_close(gone);
        host::log("lobbies: the join of lobby %.8s was cancelled before it came; leaving it", id.c_str());
      }
      if (job.serial == s.serial) s = Session{};
      return;
    }
    if (good) {
      get_int(j, "heartbeat_seconds", &beat, 1, 3600);
      s.code = code;
      s.token = token;
      s.heartbeat_seconds = beat_seconds(beat, stand_in());
      s.last_beat = now;
      s.next_beat = now + seconds(s.heartbeat_seconds);
      g.join_failed.erase(s.id);
      s.phase = kJoined;
      host::log("lobbies: joined lobby %.8s, code %s, heartbeat every %d s", id.c_str(),
                host::public_build::log_id(code).c_str(), s.heartbeat_seconds);
      return;
    }
    const std::string e = error_code(r);
    std::string why;
    if (e == "lobby_full") why = "The lobby filled.";
    else if (e == "lobby_not_open" || r.status == 404) why = "The host closed the lobby.";
    else if (e == "too_many_joins") why = "This network is in too many lobbies.";
    else if (r.status == 429) why = "SSBM Nucleus asks to wait.";
    else if (r.status == 0 && !r.timed_out) why = "SSBM Nucleus could not be reached.";
    else if (temporary(r)) why = "SSBM Nucleus is slow right now.";
    else why = "SSBM Nucleus refused the join.";
    g.join_failed[s.id] = why;
    host::log("lobbies: the join of lobby %.8s failed: %s", s.id.c_str(), why.c_str());
    s = Session{};
    // The list's bar says why for a while, and the list is read again as soon as the directory allows.
    g.notice = why;
    g.notice_until = now + kNoticeFor;
    if (g.list_open) g.list_next = std::min(g.list_next, std::max(now, g.list_tried + kListEvery));
    return;
  }
  case Job::kBeat: {
    if (job.serial != s.serial || (s.phase != kHosting && s.phase != kJoined)) return;
    s.last_beat = now;
    if (r.status == 200 && parse_object(r, &j)) {
      s.backoff = 0;
      if (job.host && job.fields.contains("state")) {
        const std::string sent = job.fields["state"].get<std::string>();
        s.playing_sent = sent == "playing";
        if (sent == "open") s.want_open = false;
      }
      take_state(s, j, now);
      s.next_beat = now + seconds(s.heartbeat_seconds);
      return;
    }
    if (r.status == 404 || r.status == 401 || r.status == 403) {
      host::log("lobbies: lobby %.8s is gone (%d)", s.id.c_str(), r.status);
      end_session(s.host ? "The lobby closed." : "The host closed the lobby.");
      return;
    }
    // Slow or asked to wait: the lobby lives 90 s without a heartbeat, so keep it and try again.
    s.backoff = r.status == 429 && r.retry_after > 0 ? std::min(r.retry_after, kBackoffMax) : next_backoff(s.backoff);
    s.next_beat = now + seconds(s.backoff);
    return;
  }
  case Job::kFetch:
    take_list(r, now);
    return;
  case Job::kNoJob:
    return;
  }
}

void worker() {
  std::unique_lock<std::mutex> lock(g.mutex);
  while (!g.stop) {
    const auto now = Clock::now();
    Job job = next_job(now);
    if (job.kind == Job::kNoJob) {
      g.wake.wait_for(lock, std::chrono::milliseconds(250));
      continue;
    }
    lock.unlock();
    const Reply r = send(job.method.c_str(), job.path, job.body, job.token, 8000);
    std::string more;
    if (job.kind == Job::kFetch && r.status == 200) more = "list";
    log_reply(job.method.c_str(), job.path, job.fields.is_object() ? &job.fields : nullptr, r, more);
    lock.lock();
    if (g.stop) break;
    apply(job, r, Clock::now());
  }
}

void start_worker() {
  if (g.worker_started) return;
  g.worker_started = true;
  std::thread(worker).detach();
}

// ---- answers to the game ----

void put_text(std::vector<uint8_t>& out, const std::string& text, size_t width) {
  const size_t at = out.size();
  out.resize(at + width, 0);
  for (size_t i = 0; i < text.size() && i + 1 < width; ++i) out[at + i] = (uint8_t)text[i];
}

void put_u16(std::vector<uint8_t>& out, int v) {
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)v);
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(v >> s));
}

bool feature_on() { return g.allowed && g.setting && !g.my_build.empty(); }

void status_reply(std::vector<uint8_t>& reply) {
  const Session& s = g.session;
  reply.clear();
  reply.push_back(feature_on() ? 1 : 0);
  reply.push_back(g.avail);
  reply.push_back(s.phase);
  reply.push_back((uint8_t)std::clamp(s.waiting, 0, 8));
  reply.push_back((uint8_t)((s.host ? kFlagHost : 0) | (s.matched ? kFlagMatched : 0) | (s.entered ? kFlagEntered : 0) |
                            (s.released ? kFlagReleased : 0)));
  reply.push_back(0);   // (was the ruleset; kept so the fields after it stay where they are)
  put_text(reply, (s.phase == kHosting || s.phase == kJoined) ? s.code : "", 10);
  put_text(reply, shown(s.host_name, 15), 16);
  const std::string& why = (s.phase == kEnded || s.phase == kFailed) ? s.message
                           : g.avail == kUnavailable                  ? g.unavailable_reason
                                                                       : std::string();
  put_text(reply, shown(why, 47), 48);
}

// The look at the directory a menu asks for: once a minute at most, and never faster than the list.
void want_look(Clock::time_point now) {
  if (g.avail == kUnknown || now - g.seen > kSeenFor) {
    g.probe_wanted = true;
    g.list_next = std::max(g.list_next, g.list_tried + kListEvery);
    if (g.list_tried == Clock::time_point{}) g.list_next = now;
    g.wake.notify_all();
  }
}

const Lobby* lobby_with(const std::string& id) {
  for (const Lobby& l : g.lobbies)
    if (l.id == id) return &l;
  return nullptr;
}

uint8_t card_state(const Lobby& l) {
  if (g.session.phase == kJoining && g.session.id == l.id) return kCardBusy;
  if (g.join_failed.count(l.id)) return kCardFailed;
  return kCardIdle;
}

// The list's bar (the head's message): who is being joined, why the last join failed (for a few
// seconds), else how many lobbies are open.
std::string bar_words(Clock::time_point now) {
  if (g.session.phase == kJoining) return "Joining " + shown(g.session.host_name, 24) + "...";
  if (!g.notice.empty() && now < g.notice_until) return g.notice;
  return g.list_message;
}

// The join of lobby `l` (A on its row): false when it cannot start (a lobby on its way already, or
// this game cannot join that one).
bool start_join(const Lobby& l) {
  Session& s = g.session;
  if (!feature_on() || (s.phase != kNone && s.phase != kEnded && s.phase != kFailed) || g.identity_source == nullptr)
    return false;
  Identity me = g.identity_source();
  if (me.build.empty() || me.build != l.build) return false;
  start_worker();
  g.join_failed.erase(l.id);
  g.notice.clear();
  s = Session{};
  s.serial = g.next_serial++;
  s.phase = kJoining;
  s.id = l.id;
  s.host_name = l.host_name;
  s.waiting = l.waiting;
  s.me = std::move(me);
  g.wake.notify_all();
  host::log("lobbies: joining lobby %.8s", l.id.c_str());
  return true;
}

// A refused join's reason on the list's bar, and the list read again as soon as the directory allows.
void refuse_row(const char* why, Clock::time_point now) {
  g.notice = why;
  g.notice_until = now + kNoticeFor;
  g.refresh_wanted = true;
  g.list_next = std::min(g.list_next, std::max(now, g.list_tried + kListEvery));
  g.wake.notify_all();
}

// A on the list's row `row` of the list of generation `generation`, the one the game drew that row
// from: the lobby that row showed, by its id, whatever the list holds at `row` since (a refresh may
// have landed between the game's reads; the review of 2026-11-24). A lobby gone since then, or a list
// this side no longer knows, says so on the bar, and the list is read again: never another lobby.
uint8_t join_row(int row, uint16_t generation, Clock::time_point now) {
  if (!g.list_open) return kJoinNotALobby;
  const auto drawn = std::find_if(g.generations.begin(), g.generations.end(),
                                  [&](const auto& e) { return e.first == generation; });
  if (generation == 0 || drawn == g.generations.end() || row < 0 || row >= (int)drawn->second.size()) {
    host::log("lobbies: A on row %d of list %u: no such row in the lists kept (now list %u); refused", row,
              (unsigned)generation, (unsigned)g.list_generation);
    refuse_row("The list changed.", now);
    return kJoinRefused;
  }
  const std::string& id = drawn->second[(size_t)row];
  const Lobby* l = lobby_with(id);
  if (l == nullptr) {
    host::log("lobbies: A on row %d of list %u: lobby %.8s is no longer listed", row, (unsigned)generation, id.c_str());
    refuse_row("That lobby is no longer open.", now);
    return kJoinRefused;
  }
  if (generation != g.list_generation) {
    const auto& ids = g.generations.back().second;
    host::log("lobbies: A on row %d of list %u, drawn before list %u: lobby %.8s as drawn (row %d now holds %.8s)", row,
              (unsigned)generation, (unsigned)g.list_generation, id.c_str(), row,
              row < (int)ids.size() ? ids[(size_t)row].c_str() : "nothing");
  }
  return start_join(*l) ? kJoinAsked : kJoinRefused;
}

}  // namespace

// ---- the identity's parts ----

std::string host_name_from(const std::string& display_name, const std::string& connect_code) {
  std::string out;
  for (char c : display_name) {
    if (!name_char(c)) continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  if (out.size() > 15) out.resize(15);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  auto upper = [](std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
  };
  std::string code_letters;
  for (char c : connect_code)
    if (c != '#') code_letters += c;
  // A player with no display name shows the connect code (slippi_online local_identity): never sent.
  if (out.empty() || (!connect_code.empty() && (upper(out) == upper(connect_code) || upper(out) == upper(code_letters))))
    return "Player";
  return out;
}

std::string build_string(bool mod_view, const std::string& fingerprint) {
  if (!mod_view) return "retail";
  std::string out;
  for (char c : fingerprint) {
    if (out.size() >= 16) break;
    const char l = (char)std::tolower((unsigned char)c);
    if (!is_hex_lower(l)) return "";
    out += l;
  }
  return out.size() >= 8 ? out : "";
}

std::string client_string(const char* wire_name, const char* version) {
  int a = 0, b = 0, c = 0;
  char rest = 0;
  const bool ok = version != nullptr && std::sscanf(version, "%4d.%4d.%4d%c", &a, &b, &c, &rest) == 3 && a >= 0 &&
                  b >= 0 && c >= 0;
  std::string name = wire_name ? wire_name : "netlee";
  for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
  return name + "/" + (ok ? std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) : "0.0.0");
}

void set_enabled(bool on) { g.setting = on; }
bool enabled() { return g.setting; }

void command(const uint8_t* p, size_t n, bool allowed, IdentitySource identity, std::vector<uint8_t>& reply) {
  std::lock_guard<std::mutex> lock(g.mutex);
  const auto now = Clock::now();
  g.allowed = allowed;
  if (identity) g.identity_source = identity;
  if (n < 1) { reply = {0}; return; }
  Session& s = g.session;
  switch (p[0]) {
  case kStatus:
    status_reply(reply);
    return;
  case kMenu:
    // The lobby menu opened: who this player is (the build the list keeps) and whether the site
    // offers lobbies.
    if (identity) g.my_build = identity().build;
    if (feature_on()) {
      start_worker();
      want_look(now);
    }
    status_reply(reply);
    return;
  case kList:
    if (n < 2) { reply = {0}; return; }
    if (p[1] == 1 && feature_on()) {
      if (identity) g.my_build = identity().build;
      start_worker();
      g.list_open = true;
      g.join_failed.clear();
      g.notice.clear();
      // Rows a minute old or less show at once; the list is read again when 15 s have passed.
      if (g.list_status != kListReady || now - g.list_fetched > kKeepRows) {
        if (g.avail != kUnavailable) g.list_status = kListLoading;
      }
      g.list_next = g.list_tried == Clock::time_point{} ? now : std::max(now, g.list_tried + kListEvery);
      g.wake.notify_all();
      host::log("lobbies: list open");
    } else if (p[1] == 0 && g.list_open) {
      g.list_open = false;
      host::log("lobbies: list closed");
    }
    reply = {1};
    return;
  case kCreate: {
    if (!feature_on() || g.avail == kUnavailable || (s.phase != kNone && s.phase != kEnded && s.phase != kFailed)) {
      reply = {0};
      return;
    }
    Identity me = identity ? identity() : Identity{};
    if (me.build.empty() || me.name.empty()) { reply = {0}; return; }
    start_worker();
    s = Session{};
    s.serial = g.next_serial++;
    s.phase = kCreating;
    s.host = true;
    s.me = std::move(me);
    g.wake.notify_all();
    reply = {1};
    return;
  }
  case kCancel:
    if (s.phase == kCreating || s.phase == kJoining) {
      s.cancelled = true;   // closed as soon as it arrives
      if (!s.sent) s = Session{};
      else s.phase = kNone, s.message.clear();
    } else if (s.phase == kHosting || s.phase == kJoined) {
      host::log("lobbies: %s lobby %.8s (%s)", s.host ? "closing" : "leaving", s.id.c_str(),
                n >= 2 && p[1] == 1 ? "Z" : n >= 2 && p[1] == 2 ? "quit" : "left");
      queue_close(s);
      s = Session{};
    } else {
      s = Session{};
    }
    g.wake.notify_all();
    reply = {1};
    return;
  case kMatched:
    if ((s.phase == kHosting || s.phase == kJoined) && !s.matched) {
      s.matched = true;
      s.filled_since = {};
      host::log("lobbies: lobby %.8s: the Teams match formed (%s)", s.id.c_str(), s.host ? "host" : "joined");
      if (s.host) {
        // Not listed any more, on the next heartbeat the directory takes (20 s apart).
        s.want_open = false;
        s.next_beat = std::max(now, s.last_beat + seconds(s.heartbeat_seconds * 2 / 3));
      } else {
        // The directory's part is done: the seat is given up, the code stays for a search again.
        queue_close(s);
        s.released = true;
      }
      g.wake.notify_all();
    }
    reply = {1};
    return;
  case kSearch:
    if (s.phase == kHosting || s.phase == kJoined) {
      host::log("lobbies: Teams search with lobby code %s (%s, lobby %.8s)", host::public_build::log_id(s.code).c_str(),
                s.host ? "host" : "joined", s.id.c_str());
      if (s.matched) {
        // Searching again after a match (a disconnect): the host lists the lobby again.
        s.matched = false;
        if (s.host) {
          s.want_open = true;
          s.next_beat = std::max(now, s.last_beat + seconds(s.heartbeat_seconds * 2 / 3));
        }
      }
      g.wake.notify_all();
    }
    reply = {1};
    return;
  case kAck:
    if (s.phase == kEnded || s.phase == kFailed) s = Session{};
    reply = {1};
    return;
  case kEntered:
    if (s.phase == kHosting || s.phase == kJoined) s.entered = true;
    reply = {1};
    return;
  case kJoinRow:
    // A on the lobby list's row (ty/tylist.c, before the Gallery would open its viewer): that lobby is
    // joined at once. There is no lobby page; its row shows the host, the region and who waits. The
    // row and the generation of the list it was drawn from (big-endian u16 each).
    reply = {n >= 5 ? join_row((int)p[1] << 8 | p[2], (uint16_t)((int)p[3] << 8 | p[4]), now) : (uint8_t)kJoinRefused};
    return;
  default:
    reply = {0};
    return;
  }
}

bool gallery(const uint8_t* p, size_t n, std::vector<uint8_t>& reply) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!g.list_open || n < 1) return false;
  const auto now = Clock::now();
  const int row = n >= 3 ? (int)p[1] << 8 | p[2] : -1;
  switch (p[0]) {
  case kOpen:
    reply = {1};
    return true;
  case kRows: {
    // The head (shim/mu_mods_gallery.c read_head), then the rows asked for.
    const int first = n >= 4 ? row : 0;
    const int count = n >= 4 ? p[3] : 0;
    // (MELEE_TEST_LOBBY_RACE: a head read alone still gets the list before the last look.)
    const bool race = count == 0 && now < g.race_until;
    const std::vector<Lobby>& listed = race ? g.race_old : g.lobbies;
    reply.clear();
    reply.push_back(g.list_status);
    put_u16(reply, g.list_status == kListReady ? (int)listed.size() : 0);
    put_u16(reply, 0);
    put_u16(reply, race ? g.race_old_generation : g.list_generation);   // the rows' generation (join_row)
    reply.push_back(0);   // flags: one whole list, nothing more to read
    put_text(reply, "", 20);
    // The bar under the list: "Joining <host>...", a failed join's reason, or how many are open (with
    // no lobby listed it is the list's one row instead).
    put_text(reply, g.list_status == kListLoading ? "" : shown(g.list_status == kListReady ? bar_words(now) : g.list_message, 47), 48);
    put_text(reply, "Lobbies", 48);
    for (int i = first; g.list_status == kListReady && i < first + count && i < (int)g.lobbies.size(); ++i) {
      const Lobby& l = g.lobbies[i];
      // Everything about a lobby is on its row (there is no lobby page): the host, the region, and on
      // the right everyone waiting, the host too; empty only when the directory gave no count (its box
      // is drawn only around words).
      put_text(reply, shown(l.host_name, 24) + " - " + region_name(l.region, false), kRowName);
      put_text(reply, l.waiting >= 1 ? std::to_string(l.waiting) + " waiting" : std::string(), kRowRight);
      reply.push_back(card_state(l));
    }
    return true;
  }
  case kChange:
    // CHANGE_RETRY: A on the list's error, or L and R: the list again, when 15 s have passed since the
    // last look.
    if (n >= 2 && p[1] == kChangeRetry) {
      g.refresh_wanted = true;
      g.list_next = std::max(now, g.list_tried + kListEvery);
      if (g.list_status == kListError && g.avail != kUnavailable) g.list_status = kListLoading;
      g.wake.notify_all();
      reply = {1};
    } else {
      reply = {0};
    }
    return true;
  case kDetail:
    // There is no lobby page: A on a row joins (kJoinRow) before the Gallery would open its viewer.
    // Should the viewer still open on a lobby row (a row read while the list changed), "no longer a
    // card" sends it straight back to the list (shim/mu_mods_gallery.c read_detail).
    reply = {0xFF};
    return true;
  case kPicture:
    if (row == 0xFFFF) return false;   // the plain tile: the Mods menu's own (host/mods_menu.h)
    reply = {0};
    return true;
  case kAction:
    reply = {0};   // the viewer's A: no viewer opens on a lobby, so there is nothing to act on
    return true;
  default:
    return false;
  }
}

void shutdown() {
  std::vector<Delete> last;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    Session& s = g.session;
    if ((s.phase == kHosting || s.phase == kJoined) && !s.token.empty()) queue_close(s);
    s = Session{};
    for (const auto& d : g.deletes) last.push_back(d);
    g.deletes.clear();
    g.stop = true;
    g.list_open = false;
  }
  g.wake.notify_all();
  for (const auto& d : last) {
    const Reply r = send("DELETE", d.path, "", d.token, 1500);
    log_reply("DELETE", d.path, nullptr, r, "on the way out");
  }
}

}  // namespace host::lobby_directory
