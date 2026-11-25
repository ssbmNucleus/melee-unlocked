// SPDX-License-Identifier: GPL-2.0-or-later
#include "slippi_agent_lock.h"
#include "slippi_report.h"
#include "slippi_net.h"
#include "host.h"
#include "http_client.h"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <limits>

// The fork already uses stb for handoff-state compression.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
#include <functional>
#include <optional>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace slippi::report {
// The simulation starts a fresh buffer at the command table. Each queued report
// retains its own game while the next game starts on the same connection.
class ReplayData {
public:
  std::mutex mutex;
  std::condition_variable cv;
  std::string raw;
  bool ended = false;
};

std::shared_ptr<ReplayData> start_replay() { return std::make_shared<ReplayData>(); }
void append_replay(ReplayData& replay, const uint8_t* data, uint32_t size) {
  if (!size) return;
  std::lock_guard<std::mutex> lock(replay.mutex);
  if (replay.ended) return;
  replay.raw.append(reinterpret_cast<const char*>(data), size);
  if (data[0] == 0x39) { replay.ended = true; replay.cv.notify_one(); }
}

namespace {
using json = nlohmann::json;
// Requests name this program and the Slippi protocol it speaks (http_client.h, Agent::Slippi).
const char* ENDPOINT = "https://internal.slippi.gg/graphql";
constexpr int MAX_ATTEMPTS = 5;

struct StatusJob { std::string uid, play_key, match_id, status; };
struct Job { bool is_status = false; GameReport game; StatusJob status; int attempts = 0; };

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_queue;
std::thread g_thread;
bool g_quit = false;
std::string g_iso_path, g_cache_dir;
std::atomic<bool> g_hash_done{false};
std::string g_iso_hash;

// ---- HTTP: the host's one HTTP client (http_client.h)
thread_local HttpFailure http_failure{"http-get", 0};
// One request. True when an answer came, whatever its status (in *status), with its body (an error
// answer's too) in *response.
bool http(const char* method, const std::string& url, std::vector<std::string> headers, const std::string& body, int* status, std::string* response) {
  http_failure = {"http-get", 0};
  if (status) *status = 0;
  // Every caller of this private HTTP client is Slippi-only, including signed replay uploads.
  if (agent_lock::blocked(agent_lock::Operation::Http)) {
    http_failure = {"agent-locked", 0};
    if (response) response->clear();
    return false;
  }
  host::http::Request req;
  req.method = method;
  req.url = url;
  req.headers = std::move(headers);
  req.body = body;
  req.max_bytes = 4u << 20;   // Slippi's answers are a few KB
  req.agent = host::http::Agent::Slippi;
  // Windows' own proxy and auto-configuration, as browsers and the Slippi Launcher use them:
  // WinHTTP's default ignores both, and behind such a network every request timed out (error
  // 12002) while the launcher worked.
  req.system_proxy = true;
  req.resolve_ms = 5000;
  req.connect_ms = 5000;
  req.send_ms = 15000;
  req.receive_ms = 15000;
  req.keep_error_body = true;
  // Plain HTTP only to the ranked test's loopback stand-in (slippi_ranked_report_test); Slippi is HTTPS.
  req.allow_loopback_http = std::strncmp(ENDPOINT, "http://127.0.0.1:", 17) == 0;
  req.log_label = "slippi http";
  req.log_address = false;   // a signed upload address carries its credentials in the query
  host::http::Response r = host::http::fetch(req);
  if (status) *status = r.status;
  if (r.error != host::http::Error::None && r.error != host::http::Error::Status) {
    http_failure = {host::http::error_name(r.error), r.system_error};
    return false;
  }
  if (r.error == host::http::Error::Status) http_failure = {"http-status", (unsigned long)r.status};
  if (response) *response = std::move(r.body);
  return true;
}

}  // namespace
HttpFailure last_http_failure() { return http_failure; }
bool http_get(const std::string& url, std::string* response, int* status) { return http("GET", url, {}, "", status, response); }
namespace {
// A member of a JSON object, or null when j is not an object or has no such key. Never throws.
const json* json_member(const json& j, const char* key) {
  if (!j.is_object()) return nullptr;
  auto it = j.find(key);
  return it == j.end() ? nullptr : &*it;
}

// GraphQL POST; returns the `data` object or null (and logs) on failure.
json graphql(const std::string& query, const json& variables) {
  json body = {{"query", query}, {"variables", variables}};
  int status = 0; std::string response;
  if (!http("POST", ENDPOINT, {"Content-Type: application/json"}, body.dump(), &status, &response)) { host::log("slippi report: request failed (network)"); return nullptr; }
  json r = json::parse(response, nullptr, false);
  if (r.is_discarded()) { host::log("slippi report: bad response (HTTP %d): %s", status, response.substr(0, 200).c_str()); return nullptr; }
  const json* errors = json_member(r, "errors");
  if (errors && errors->is_array() && !errors->empty()) { host::log("slippi report: server returned %zu errors", errors->size()); return nullptr; }
  const json* data = json_member(r, "data");
  if (!data) return nullptr;
  return *data;
}

// The signed upload accepts gzip-compressed SLP data.
uint32_t crc32(const uint8_t* p, size_t n) {
  static uint32_t table[256]; static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
std::string gzip_replay(std::string& in) {
  if (in.size() > size_t(std::numeric_limits<int>::max())) return {};
  int size = 0;
  auto* zlib = stbi_zlib_compress(reinterpret_cast<unsigned char*>(in.data()), int(in.size()), &size, 5);
  if (!zlib) return {};
  // stb returns RFC 1950 zlib. Reuse its DEFLATE bytes in the RFC 1952 container.
  std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
  out.append(reinterpret_cast<const char*>(zlib + 2), size - 6);
  STBIW_FREE(zlib);
  uint32_t crc = crc32((const uint8_t*)in.data(), in.size()), isize = (uint32_t)in.size();
  for (int i = 0; i < 4; ++i) out.push_back((char)((crc >> (8 * i)) & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back((char)((isize >> (8 * i)) & 0xFF));
  return out;
}

// ---- ISO MD5 (BCrypt), cached by path, size and modification time.
std::string md5_file(const std::string& path) {
  BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) return "";
  std::string result;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
    FILE* f = _wfopen(std::filesystem::u8path(path).wstring().c_str(), L"rb");
    if (f) {
      std::vector<uint8_t> buf(4 << 20);
      size_t n;
      while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
      std::fclose(f);
      uint8_t digest[16];
      if (BCryptFinishHash(h, digest, 16, 0) >= 0) { char hex[33]; for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]); result = hex; }
    }
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return result;
}
void hash_iso() {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  std::string key;
  if (GetFileAttributesExA(g_iso_path.c_str(), GetFileExInfoStandard, &fa)) {
    char buf[128]; std::snprintf(buf, sizeof buf, "|%llu|%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
    key = g_iso_path + buf;
  }
  std::string cache = g_cache_dir + "/iso_md5_cache.txt";
  { std::ifstream in(cache); std::string line; while (std::getline(in, line)) { size_t eq = line.rfind('='); if (eq != std::string::npos && line.substr(0, eq) == key) { g_iso_hash = line.substr(eq + 1); g_hash_done = true; return; } } }
  auto t0 = std::chrono::steady_clock::now();
  g_iso_hash = md5_file(g_iso_path);
  host::log("slippi report: ISO md5 %s (%.1f s)", g_iso_hash.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  if (!g_iso_hash.empty() && !key.empty()) { std::ofstream out(cache, std::ios::app); out << key << "=" << g_iso_hash << "\n"; }
  g_hash_done = true;
}

void upload_replay(ReplayData& replay, const std::string& url) {
  std::string contents("{U\x03raw[$U#l", 11);
  {
    std::unique_lock<std::mutex> lock(replay.mutex);
    replay.cv.wait_for(lock, std::chrono::seconds(1), [&] { return replay.ended; });
    if (replay.raw.empty() || replay.raw.size() > UINT32_MAX) return;
    const uint32_t length = static_cast<uint32_t>(replay.raw.size());
    for (int shift = 24; shift >= 0; shift -= 8) contents.push_back(char(length >> shift));
    contents += replay.raw;
  }
  contents.append("U\x08metadata{}}", 13);
  std::string gz = gzip_replay(contents);
  if (gz.empty()) { host::log("slippi report: replay compression failed"); return; }
  int status = 0;
  bool ok = http("PUT", url, {"Content-Type: application/octet-stream", "Content-Encoding: gzip", "X-Goog-Content-Length-Range: 0,10000000"}, gz, &status, nullptr);
  host::log("slippi report: replay upload %s (HTTP %d, %zu bytes)", ok && status / 100 == 2 ? "done" : "failed", status, gz.size());
}

// Returns true when the report is finished (accepted or given up).
bool send_game(Job& job) {
  const GameReport& g = job.game;
  ++job.attempts;
  json players = json::array();
  for (auto& p : g.players)
    players.push_back({{"fbUid", p.uid}, {"slotType", p.slot_type}, {"damageDone", p.damage_done}, {"stocksRemaining", p.stocks_remaining},
                       {"characterId", p.character_id}, {"colorId", p.color_id}, {"startingStocks", p.starting_stocks}, {"startingPercent", p.starting_percent}});
  json payload = {{"fbUid", g.uid}, {"mode", g.online_mode}, {"players", players}, {"isoHash", g_iso_hash}, {"matchId", g.match_id}, {"playKey", g.play_key},
                  {"gameDurationFrames", g.duration_frames}, {"gameIndex", g.game_index}, {"tiebreakIndex", g.tiebreak_index}, {"winnerIdx", g.winner_index},
                  {"gameEndMethod", g.game_end_method}, {"lrasInitiator", g.lras_initiator}, {"stageId", g.stage_id}};
  json data = graphql("mutation ($report: OnlineGameReportInput!) { reportOnlineGame (report: $report) { success uploadUrl } }", {{"report", payload}});
  // Type-checked: the answer can be null, or an object without these fields, when the server refuses.
  const json* r = json_member(data, "reportOnlineGame");
  const json* ok = r ? json_member(*r, "success") : nullptr;
  bool success = ok && ok->is_boolean() && ok->get<bool>();
  if (success) {
    host::log("slippi report: game %u of %s reported", g.game_index, g.match_id.c_str());
    const json* url = json_member(*r, "uploadUrl");
    if (url && url->is_string() && g.replay) upload_replay(*g.replay, url->get<std::string>());
    return true;
  }
  if (job.attempts >= MAX_ATTEMPTS) { host::log("slippi report: giving up on game report for %s after %d attempts", g.match_id.c_str(), job.attempts); return true; }
  std::this_thread::sleep_for(std::chrono::milliseconds(100 * job.attempts));
  return false;
}

// Nothing may leave this thread as an exception (that would end the process): a report whose
// request or answer throws is logged and dropped, like one that ran out of attempts.
bool send_game_guarded(Job& job) {
  try {
    if (job.is_status) {
      const auto& s = job.status;
      json report = {{"matchId", s.match_id}, {"fbUid", s.uid}, {"playKey", s.play_key}, {"status", s.status}};
      const auto data = graphql("mutation ($report: OnlineMatchStatusReportInput!) { reportOnlineMatchStatus(report: $report) }", {{"report", report}});
      const auto* accepted = json_member(data, "reportOnlineMatchStatus");
      if (!(accepted && accepted->is_boolean() && accepted->get<bool>()))
        host::log("slippi: set status %s was not accepted", s.status.c_str());
      return true;
    }
    return send_game(job);
  } catch (const std::exception& e) {
    host::log("slippi report: game report for %s dropped: %s", job.game.match_id.c_str(), e.what());
  } catch (...) {
    host::log("slippi report: game report for %s dropped (unknown error)", job.game.match_id.c_str());
  }
  return true;
}

void worker() {
  try {
    hash_iso();
  } catch (const std::exception& e) {
    host::log("slippi report: ISO hash failed: %s", e.what());
    g_hash_done = true;
  } catch (...) {
    host::log("slippi report: ISO hash failed");
    g_hash_done = true;
  }
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(g_mutex);
      g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
      if (g_queue.empty()) return;
      job = std::move(g_queue.front());
      g_queue.pop_front();
      if (g_quit) job.attempts = MAX_ATTEMPTS - 1;   // one last attempt each on shutdown
    }
    bool done = send_game_guarded(job);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!done && !g_quit) g_queue.push_front(std::move(job));
  }
}
}  // namespace

void init(const std::string& iso_path, const std::string& cache_dir) {
  if (agent_lock::blocked(agent_lock::Operation::Reporting)) return;
  g_quit = false;
  g_iso_path = iso_path; g_cache_dir = cache_dir;
  g_thread = std::thread(worker);
}

void join_rank_thread_for_shutdown();
void shutdown() {
  { std::lock_guard<std::mutex> lk(g_mutex); g_quit = true; }
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  join_rank_thread_for_shutdown();
}

void log_game(const GameReport& report) {
  if (agent_lock::blocked(agent_lock::Operation::Reporting)) return;
  { std::lock_guard<std::mutex> lk(g_mutex); Job j; j.game = report; g_queue.push_back(std::move(j)); }
  g_cv.notify_all();
}

void match_status(const std::string& uid, const std::string& play_key, const std::string& match_id, const std::string& status, bool background) {
  if (agent_lock::blocked(agent_lock::Operation::Reporting) || uid.empty() || play_key.empty() || match_id.find("mode.ranked") == std::string::npos) return;
  StatusJob s{uid, play_key, match_id, status};
  if (!background) { Job j; j.is_status = true; j.status = s; send_game_guarded(j); return; }
  { std::lock_guard<std::mutex> lk(g_mutex); Job j; j.is_status = true; j.status = s; g_queue.push_back(std::move(j)); }
  g_cv.notify_all();
}

// ---------------------------------------------------------------- rank
namespace {
std::mutex g_rank_mutex;
RankInfo g_rank;
RankFetchStatus g_rank_status = RankFetchStatus::Error;
std::condition_variable g_rank_cv;
std::thread g_rank_thread;
std::function<std::optional<RankInfo>()> g_rank_job;
unsigned g_rank_generation = 0;
bool g_stop_rank = false;

// Login and set-result fetches share one worker. A new account or request
// supersedes a pending result without stalling the game thread on HTTP.
void schedule_rank(std::function<std::optional<RankInfo>()> job) {
  std::lock_guard<std::mutex> lock(g_rank_mutex);
  ++g_rank_generation;
  g_rank_status = RankFetchStatus::Fetching;
  g_rank_job = std::move(job);
  if (!g_rank_thread.joinable()) {
    g_stop_rank = false;
    g_rank_thread = std::thread([] {
      std::unique_lock<std::mutex> lock(g_rank_mutex);
      for (;;) {
        g_rank_cv.wait(lock, [] { return g_stop_rank || bool(g_rank_job); });
        if (g_stop_rank) return;
        auto query = std::move(g_rank_job);
        g_rank_job = {};
        const unsigned generation = g_rank_generation;
        lock.unlock();
        std::optional<RankInfo> result;
        try { result = query(); } catch (const std::exception& e) { host::log("slippi rank: invalid response"); }
        lock.lock();
        if (generation == g_rank_generation) {
          g_rank_status = result ? RankFetchStatus::Fetched : RankFetchStatus::Error;
          g_rank = result.value_or(RankInfo{});
        }
      }
    });
  }
  g_rank_cv.notify_one();
}
}  // namespace

// Minimum ordinal for each grade, as in slippi-rust-extensions/user/src/rank_fetcher/rank.rs.
int8_t decide_rank(float ordinal, uint16_t global, uint16_t regional, uint32_t updates) {
  if (updates < 5) return 0;
  if (ordinal >= 2191.75f && (global || regional)) return 19;
  constexpr float minimums[] = {765.43f, 913.72f, 1054.87f, 1188.88f, 1315.75f,
    1435.48f, 1548.07f, 1653.52f, 1751.83f, 1843.0f, 1927.03f, 2003.92f, 2073.67f,
    2136.28f, 2191.75f, 2275.f, 2350.f};
  return 1 + static_cast<int8_t>(std::upper_bound(std::begin(minimums), std::end(minimums), ordinal) - std::begin(minimums));
}

void clear_rank() {
  std::lock_guard<std::mutex> lock(g_rank_mutex);
  ++g_rank_generation;
  g_rank_job = {};
  g_rank = {};
  g_rank_status = RankFetchStatus::Error;
}

void join_rank_thread_for_shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_rank_mutex);
    g_stop_rank = true;
    g_rank_job = {};
  }
  g_rank_cv.notify_one();
  if (g_rank_thread.joinable()) g_rank_thread.join();
  clear_rank();
}

void fetch_user_rank(const std::string& uid) {
  if (uid.empty() || agent_lock::blocked(agent_lock::Operation::Http)) return;
  schedule_rank([uid]() -> std::optional<RankInfo> {
    std::string url = "https://users-rest-dot-slippi.uc.r.appspot.com/user/" + uid + "?additionalFields=chatMessages,rank";
    int status = 0; std::string response;
    if (!http("GET", url, {}, "", &status, &response)) { host::log("slippi rank: user fetch failed (network)"); return std::nullopt; }
    json j = json::parse(response, nullptr, false);
    if (status != 200 || j.is_discarded() || !j.is_object() || !j.count("rank") || !j["rank"].is_object()) { host::log("slippi rank: user fetch HTTP %d, no rank in response", status); return std::nullopt; }
    auto& r = j["rank"];
    RankInfo info;
    info.rating_ordinal = r["ratingOrdinal"].is_number() ? r["ratingOrdinal"].get<float>() : 0;
    info.global_placing = (uint16_t)(r.count("dailyGlobalPlacement") && r["dailyGlobalPlacement"].is_number() ? r["dailyGlobalPlacement"].get<int>() : 0);
    info.regional_placing = (uint16_t)(r.count("dailyRegionalPlacement") && r["dailyRegionalPlacement"].is_number() ? r["dailyRegionalPlacement"].get<int>() : 0);
    info.rating_update_count = r["ratingUpdateCount"].is_number_unsigned() ? r["ratingUpdateCount"].get<uint32_t>() : 0;
    info.rank = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
    host::log("slippi rank: rating %.1f, %u rated games, rank index %d", info.rating_ordinal, info.rating_update_count, info.rank);
    return info;
  });
}

void fetch_match_result(const std::string& match_id, const std::string& uid, const std::string& play_key) {
  if (agent_lock::blocked(agent_lock::Operation::Reporting) || uid.empty() || play_key.empty() || match_id.find("mode.ranked") == std::string::npos) return;
  schedule_rank([match_id, uid, play_key]() -> std::optional<RankInfo> {
    for (int attempt = 0; attempt < 3; ++attempt) {
      json vars = {{"request", {{"matchId", match_id}, {"fbUid", uid}, {"playKey", play_key}}}};
      json data = graphql("query ($request: OnlineMatchRequestInput!) { getRankedMatchPersonalResult(request: $request) { participant { ordinal dailyGlobalPlacement dailyRegionalPlacement ratingUpdateCount ratingChange } } }", vars);
      if (data.is_null() || !data.count("getRankedMatchPersonalResult") || !data["getRankedMatchPersonalResult"].is_object()) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
      auto& p = data["getRankedMatchPersonalResult"]["participant"];
      if (!p.is_object()) continue;
      bool has_change = p.count("ratingChange") && p["ratingChange"].is_number();
      if (!has_change && attempt < 2) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
      RankInfo info;
      info.rating_ordinal = p.count("ordinal") && p["ordinal"].is_number() ? p["ordinal"].get<float>() : 0.0f;
      info.global_placing = (uint16_t)(p.count("dailyGlobalPlacement") && p["dailyGlobalPlacement"].is_number() ? p["dailyGlobalPlacement"].get<int>() : 0);
      info.regional_placing = (uint16_t)(p.count("dailyRegionalPlacement") && p["dailyRegionalPlacement"].is_number() ? p["dailyRegionalPlacement"].get<int>() : 0);
      info.rating_update_count = p.count("ratingUpdateCount") && p["ratingUpdateCount"].is_number() ? p["ratingUpdateCount"].get<uint32_t>() : 0;
      info.rating_change = has_change ? p["ratingChange"].get<float>() : 0.0f;
      int8_t prev = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
      info.rating_ordinal += info.rating_change;
      info.rating_update_count += has_change ? 1 : 0;
      info.rank = decide_rank(info.rating_ordinal, info.global_placing, info.regional_placing, info.rating_update_count);
      info.rank_change = (int8_t)(info.rank - prev);
      host::log("slippi rank: match result: rating %.1f (%+.1f), rank index %d (%+d)", info.rating_ordinal, info.rating_change, info.rank, info.rank_change);
      return info;
    }
    host::log("slippi rank: match result fetch failed");
    return std::nullopt;
  });
}

RankFetchStatus rank_info(RankInfo* out) {
  std::lock_guard<std::mutex> lock(g_rank_mutex);
  if (out) *out = g_rank;
  return g_rank_status;
}
}  // namespace slippi::report
