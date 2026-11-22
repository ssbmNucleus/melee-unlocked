// The in-game mod browser's host half (mod_browser.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_browser.h"
#include "cosmetic_mods.h"
#include "host.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "nlohmann/json.hpp"

#ifndef MELEE_PORT_VERSION
#define MELEE_PORT_VERSION "dev"
#endif

namespace host::mod_browser {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char kApi[] = "https://ssbmnucleus.net/api/public/v1";
const char* const kTabType[kTabCount] = {"costume", "stage_skin"};
constexpr uint64_t kMaxPageBytes = 2u * 1024u * 1024u;
constexpr uint64_t kMaxFileBytes = 96u * 1024u * 1024u;
constexpr auto kPageLife = std::chrono::minutes(10);

struct File { int id = 0; std::string name, url; };
struct Mod : Row { std::vector<File> files; };
struct Page {
  int status = kLoading;
  std::string message, next_cursor;
  int total = 0;
  std::vector<int> ids;
  std::chrono::steady_clock::time_point fetched{};
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::map<std::pair<int, int>, Page> g_pages;   // (tab, page)
std::map<int, Mod> g_mods;                     // every mod a fetched page listed
std::deque<std::pair<int, int>> g_page_jobs;
std::deque<int> g_download_jobs;
std::atomic<bool> g_blocked{false};
bool g_worker_started = false;

// Only the API's own host and its subdomains (the media host a download redirects to).
bool allowed_host(const std::wstring& host) {
  static const std::wstring site = L"ssbmnucleus.net";
  if (host == site) return true;
  return host.size() > site.size() + 1 && host.compare(host.size() - site.size(), site.size(), site) == 0 &&
         host[host.size() - site.size() - 1] == L'.';
}

// One blocking HTTPS GET, to a file or to text. Worker thread only.
bool https_get(const std::string& url, const fs::path* out, std::string* text, uint64_t limit, std::string* error) {
#ifdef _WIN32
  const std::wstring wurl(url.begin(), url.end());
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256]{}, path[4096]{};
  uc.lpszHostName = host; uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || !allowed_host(host)) {
    *error = "That address is not SSBM Nucleus.";
    return false;
  }
  // The client the API's owner asked for: a name, a version and where the program comes from.
  const std::string agent = std::string("MeleeUnlocked/") + MELEE_PORT_VERSION + " (+https://github.com/Hero88go/melee-unlocked)";
  HINTERNET session = WinHttpOpen(std::wstring(agent.begin(), agent.end()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) { *error = "The internet could not be reached."; return false; }
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 120000);
  bool ok = false;
  HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE) : nullptr;
  if (req) {
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
      DWORD code = 0, size = sizeof code;
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                          &size, WINHTTP_NO_HEADER_INDEX);
      // Where the request ended after its redirects: still SSBM Nucleus, or nothing is kept.
      wchar_t final_url[4096]{};
      DWORD final_size = sizeof final_url;
      URL_COMPONENTS fc{};
      fc.dwStructSize = sizeof fc;
      wchar_t final_host[256]{};
      fc.lpszHostName = final_host; fc.dwHostNameLength = 256;
      const bool final_ok = WinHttpQueryOption(req, WINHTTP_OPTION_URL, final_url, &final_size) &&
                            WinHttpCrackUrl(final_url, 0, 0, &fc) && fc.nScheme == INTERNET_SCHEME_HTTPS &&
                            allowed_host(final_host);
      std::ofstream file;
      if (out) file.open(*out, std::ios::binary | std::ios::trunc);
      uint64_t total = 0;
      bool complete = final_ok && code == 200 && (!out || file.good());
      while (complete) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { complete = false; break; }
        if (!avail) break;
        std::string chunk(avail, 0);
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) { complete = false; break; }
        total += got;
        if (total > limit) { complete = false; break; }
        if (out) file.write(chunk.data(), got);
        else text->append(chunk.data(), got);
      }
      ok = complete && (!out || file.good());
      if (!ok) {
        *error = !final_ok ? "The download left SSBM Nucleus." :
                 code == 429 ? "SSBM Nucleus asks to wait a little." :
                 code == 404 || code == 410 ? "SSBM Nucleus no longer offers this." :
                 code == 200 ? "The download stopped before it finished." :
                               "SSBM Nucleus answered " + std::to_string(code) + ".";
      }
    } else {
      *error = "SSBM Nucleus could not be reached.";
    }
  } else {
    *error = "SSBM Nucleus could not be reached.";
  }
  if (req) WinHttpCloseHandle(req);
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return ok;
#else
  (void)url; (void)out; (void)text; (void)limit;
  *error = "The mod browser is not available on this system yet.";
  return false;
#endif
}

std::string text_of(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string url_encode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

bool game_file(const std::string& name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = name.substr(dot);
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext == ".dat" || ext == ".usd";
}

void fetch_page(int tab, int page) {
  std::string cursor;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (page > 0) cursor = g_pages[{tab, page - 1}].next_cursor;
  }
  std::string url = std::string(kApi) + "/mods?type=" + kTabType[tab] + "&sort=downloads&limit=" + std::to_string(kRowsPerPage);
  if (!cursor.empty()) url += "&cursor=" + url_encode(cursor);
  std::string body, error;
  Page result;
  std::vector<Mod> mods;
  if (!https_get(url, nullptr, &body, kMaxPageBytes, &error)) {
    result.status = kError;
    result.message = error;
  } else {
    const json doc = json::parse(body, nullptr, false);
    if (!doc.is_object() || !doc.count("data") || !doc["data"].is_array()) {
      result.status = kError;
      result.message = "SSBM Nucleus sent a list this version cannot read.";
    } else {
      for (const auto& item : doc["data"]) {
        if (!item.is_object() || !item.count("id") || !item["id"].is_number_integer()) continue;
        Mod mod;
        mod.id = item["id"].get<int>();
        mod.title = text_of(item, "title");
        mod.author = text_of(item, "creator");
        if (mod.author.empty()) mod.author = text_of(item, "author");
        if (item.count("download_count") && item["download_count"].is_number_integer()) mod.downloads = item["download_count"].get<int>();
        if (item.count("files") && item["files"].is_array()) {
          for (const auto& f : item["files"]) {
            File file;
            file.name = text_of(f, "filename");
            file.url = text_of(f, "download_url");   // the API's own download address, never the media file's
            if (f.count("id") && f["id"].is_number_integer()) file.id = f["id"].get<int>();
            if (!file.url.empty() && game_file(file.name)) mod.files.push_back(file);
          }
        }
        // A post that is one archive, or holds no game file: nothing the importer can be handed yet.
        if (mod.files.empty()) { mod.state = kUnsupported; mod.note = "This post has no single game file to install"; }
        result.ids.push_back(mod.id);
        mods.push_back(std::move(mod));
      }
      result.status = kReady;
      result.next_cursor = text_of(doc, "next_cursor");
      if (doc.count("total") && doc["total"].is_number_integer()) result.total = doc["total"].get<int>();
    }
  }
  result.fetched = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& mod : mods) {
    const auto known = g_mods.find(mod.id);
    if (known != g_mods.end() && known->second.state != kNone && known->second.state != kFailed &&
        known->second.state != kUnsupported) {
      mod.state = known->second.state;   // a download in flight, or one already installed
      mod.note = known->second.note;
    }
    g_mods[mod.id] = std::move(mod);
  }
  host::log("mod browser: %s page %d: %s%s (%d mods of %d)", kTabType[tab], page + 1,
            result.status == kReady ? "ready" : "failed: ", result.status == kReady ? "" : result.message.c_str(),
            (int)result.ids.size(), result.total);
  g_pages[{tab, page}] = std::move(result);
}

// A name the file system and the importer both take: the mod's own file name, without a path.
std::string safe_name(const std::string& name) {
  std::string out;
  for (const char c : name) {
    const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                     (unsigned char)c < 32;
    out += bad ? '_' : c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? "mod.dat" : out;
}

void run_download(int id) {
  Mod mod;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_mods.find(id);
    if (it == g_mods.end()) return;
    it->second.state = kDownloading;
    it->second.note.clear();
    mod = it->second;
  }
  int installed = 0, present = 0;
  std::string problem;
  std::error_code ec;
  const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / std::to_wstring(id);
  fs::create_directories(folder, ec);
  for (const auto& file : mod.files) {
    if (g_blocked.load()) { problem = "Stopped: an online session started"; break; }
    const fs::path path = folder / fs::u8path(safe_name(file.name));
    std::string error;
    if (!https_get(file.url, &path, nullptr, kMaxFileBytes, &error)) { problem = error; continue; }
    const auto result = host::cosmetics::import_file(path.u8string());
    if (result.ok) {
      if (result.already_present) ++present; else ++installed;
      host::log("mod browser: mod %d \"%s\": %s -> %s%s", id, mod.title.c_str(), file.name.c_str(), result.asset_id.c_str(),
                result.already_present ? " (already installed)" : "");
    } else {
      problem = result.message.empty() ? "This file cannot be used here" : result.message;
      host::log("mod browser: mod %d \"%s\": %s was not installed: %s", id, mod.title.c_str(), file.name.c_str(), problem.c_str());
    }
  }
  fs::remove_all(folder, ec);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& row = g_mods[id];
  if (installed + present > 0) {
    row.state = kInstalled;
    row.note = present && !installed ? "Already installed" : "Installed: pick it with L or R";
  } else {
    row.state = kFailed;
    row.note = problem.empty() ? "Nothing was installed" : problem;
  }
}

void worker() {
  for (;;) {
    std::pair<int, int> page{-1, -1};
    int download = 0;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_wake.wait(lock, [] { return !g_page_jobs.empty() || !g_download_jobs.empty(); });
      // A page first: the player is looking at "Loading".
      if (!g_page_jobs.empty()) { page = g_page_jobs.front(); g_page_jobs.pop_front(); }
      else { download = g_download_jobs.front(); g_download_jobs.pop_front(); }
    }
    if (page.first >= 0) fetch_page(page.first, page.second);
    else run_download(download);
  }
}

void start_worker_locked() {
  if (g_worker_started) return;
  g_worker_started = true;
  std::thread(worker).detach();
}

}  // namespace

void set_blocked(bool blocked) { g_blocked.store(blocked); }

View view(int tab, int page) {
  View out;
  out.tab = tab < 0 || tab >= kTabCount ? 0 : tab;
  out.page = page < 0 ? 0 : page;
  if (g_blocked.load()) {
    out.status = kError;
    out.message = "Not available during an online session.";
    return out;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::pair<int, int> key{out.tab, out.page};
  auto it = g_pages.find(key);
  const bool stale = it != g_pages.end() && it->second.status != kLoading &&
                     std::chrono::steady_clock::now() - it->second.fetched > kPageLife;
  // A page after the first needs the one before it (its cursor): the game only steps one at a time.
  const bool reachable = out.page == 0 || (g_pages.count({out.tab, out.page - 1}) &&
                                            !g_pages[{out.tab, out.page - 1}].next_cursor.empty());
  if ((it == g_pages.end() || stale || it->second.status == kError) && reachable) {
    const bool retry = it != g_pages.end() && it->second.status == kError &&
                       std::chrono::steady_clock::now() - it->second.fetched < std::chrono::seconds(15);
    if (!retry) {
      if (it == g_pages.end() || it->second.status == kError) g_pages[key] = Page{};   // stale pages stay on screen while they refresh
      g_page_jobs.push_back(key);
      start_worker_locked();
      g_wake.notify_one();
      it = g_pages.find(key);
      if (stale) it->second.fetched = std::chrono::steady_clock::now();   // one refresh, not one per frame
    }
  }
  if (it == g_pages.end()) {
    out.status = kError;
    out.message = "There is no such page.";
    return out;
  }
  const Page& p = it->second;
  out.status = p.status;
  out.message = p.message;
  out.has_next = !p.next_cursor.empty();
  out.pages = (p.total + kRowsPerPage - 1) / kRowsPerPage;
  for (const int id : p.ids) {
    const auto mod = g_mods.find(id);
    if (mod != g_mods.end()) out.rows.push_back(mod->second);
  }
  return out;
}

bool download(int id) {
  if (g_blocked.load()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_mods.find(id);
  if (it == g_mods.end() || it->second.files.empty() || it->second.state == kQueued || it->second.state == kDownloading) return false;
  it->second.state = kQueued;
  it->second.note.clear();
  g_download_jobs.push_back(id);
  start_worker_locked();
  g_wake.notify_one();
  return true;
}

}  // namespace host::mod_browser
