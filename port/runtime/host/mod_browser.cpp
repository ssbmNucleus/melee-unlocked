// The in-game mod browser's host half (mod_browser.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_browser.h"
#include "cosmetic_mods.h"
#include "hackpack_ai.h"
#include "host.h"
#include "roster.h"
#include "archive_unpack.h"
#include "http_client.h"
#include "test_env.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#include "nlohmann/json.hpp"

namespace host::mod_browser {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char kApi[] = "https://ssbmnucleus.net/api/public/v1";
// The site has no kind of its own for fighters: they are posted as gameplay mods.
const char* const kTabType[kTabCount] = {"costume", "stage_skin", "gameplay_mod"};
const char* const kSortName[kSortCount] = {"downloads", "newest", "likes"};
const char kPluginExtension[] = ".mufighter";
constexpr uint64_t kMaxPluginBytes = 4u * 1024u * 1024u;
constexpr uint64_t kMaxPageBytes = 2u * 1024u * 1024u;
constexpr uint64_t kMaxFileBytes = 96u * 1024u * 1024u;
constexpr auto kPageLife = std::chrono::minutes(10);

// plugin: a fighter's built code. source: one file of its C (a "code" folder in the post).
// info: the post's fighter.json, which names the fighter.
// package: an m-ex fighter package (a zip: fighter.json, the fighter's files, its pictures), installed
// into the fighters folder as data (host/roster.h).
struct File { int id = 0; std::string name, url; bool plugin = false, source = false, info = false, package = false; };
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
using PageKey = std::pair<std::string, int>;   // (the query's address, page)
std::map<PageKey, Page> g_pages;
std::map<int, Mod> g_mods;                     // every mod a fetched page listed
std::deque<PageKey> g_page_jobs;
std::string g_fighters_folder;
std::deque<int> g_download_jobs;
std::atomic<bool> g_blocked{false};
bool g_worker_started = false;
// The plugin approval gate (app/plugin_gate.h), and how many plugins the download in progress kept that
// it will not load yet (worker thread only).
std::atomic<bool (*)(const std::string&)> g_plugin_check{nullptr};
int g_kept_waiting = 0;
// Opponents' skins: where an installed costume came from (set_skin_installed).
std::atomic<void (*)(const std::string&, int, int, const std::string&, const std::string&)> g_skin_installed{nullptr};

// Test runs only (MELEE_TEST_MOD_BROWSER_API=http://127.0.0.1:<port>/...): a stand-in for the site
// on this computer. Nothing but the loopback address is accepted, and only in a run started by
// automation (host/test_env.h), so a variable left in a player's environment cannot change where
// packages come from.
std::string test_api() {
  const char* v = host::test_env::automated_getenv("MELEE_TEST_MOD_BROWSER_API");
  const std::string api = v ? v : "";
  return api.rfind("http://127.0.0.1:", 0) == 0 ? api : std::string();
}
std::string api_base() {
  const std::string test = test_api();
  return test.empty() ? std::string(kApi) : test;
}

// Only the API's own host and its subdomains (the media host a download redirects to).
bool allowed_host(const std::string& host) {
  static const std::string site = "ssbmnucleus.net";
  if (host == "127.0.0.1" && !test_api().empty()) return true;
  if (host == site) return true;
  return host.size() > site.size() + 1 && host.compare(host.size() - site.size(), site.size(), site) == 0 &&
         host[host.size() - site.size() - 1] == '.';
}

// ---- requests to the site ----
//
// Every request goes through the host's one HTTP client (http_client.h), which logs one line for each
// attempt ("net: GET <host><path> -> <status or WinHTTP error> ... in <seconds>"). How long a request
// may wait depends on what it is for (kWaitList, kWaitPicture, kWaitFile in mod_browser.h): the
// connection a few seconds, the first byte of the answer 12 to 30 seconds. A request the site did not
// answer in time (or a connection that failed, or a 502/503/504/52x) is tried once more after a short
// pause. The site's API (/api/...) is asked at most kApiPerMinute times a minute, well under the
// site's own limit (about 60 a minute, docs/public-api.md), and after a 429 not at all until its
// Retry-After has passed.

constexpr int kApiPerMinute = 30;
std::mutex g_net_mutex;
std::deque<std::chrono::steady_clock::time_point> g_api_times;   // the API requests of the last minute
std::chrono::steady_clock::time_point g_api_pause_until{};       // a 429's Retry-After

// An ETag as sent back: printable ASCII, no line breaks, at most 200 characters (else none).
std::string clean_etag(const std::string& s) {
  if (s.empty() || s.size() > 200) return {};
  for (const char c : s)
    if (c < 0x20 || c > 0x7E) return {};
  return s;
}

bool is_api_path(const std::string& host, const std::string& path) {
  const bool site = host == "ssbmnucleus.net" || host == "www.ssbmnucleus.net" || (host == "127.0.0.1" && !test_api().empty());
  return site && path.rfind("/api/", 0) == 0;
}

// Waits for a free place among the API's requests of the last minute (at most `budget`); false when
// the site asked to wait (429) or no place came in time (`*why` says so, for the log).
bool api_slot(const std::chrono::milliseconds budget, std::string* why, int* wait_seconds) {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    std::chrono::steady_clock::time_point next{};
    {
      std::lock_guard<std::mutex> lock(g_net_mutex);
      const auto now = std::chrono::steady_clock::now();
      if (now < g_api_pause_until) {
        *wait_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(g_api_pause_until - now).count() + 1;
        *why = "the site asked to wait " + std::to_string(*wait_seconds) + " s more";
        return false;
      }
      while (!g_api_times.empty() && now - g_api_times.front() >= std::chrono::minutes(1)) g_api_times.pop_front();
      if ((int)g_api_times.size() < kApiPerMinute) {
        g_api_times.push_back(now);
        return true;
      }
      next = g_api_times.front() + std::chrono::minutes(1);
    }
    const auto now = std::chrono::steady_clock::now();
    if (next - start > budget) {
      *wait_seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(next - now).count() + 1;
      *why = "the site asked to wait " + std::to_string(*wait_seconds) + " s more";
      return false;
    }
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(next - now, std::chrono::seconds(1)));
  }
}

// What the player is told when a request to the site failed.
std::string player_message(const host::http::Response& r) {
  using host::http::Error;
  switch (r.error) {
  case Error::None: return {};
  case Error::BadAddress: return "That address is not SSBM Nucleus.";
  case Error::Redirect: return "The download left SSBM Nucleus.";
  case Error::TooLarge: return "The file is too large.";
  case Error::Timeout: return "SSBM Nucleus is slow right now.";
  case Error::Tls: return "No secure connection to SSBM Nucleus.";
  case Error::Connect: return "SSBM Nucleus could not be reached.";
  case Error::Held: return "SSBM Nucleus asks to wait.";
  case Error::Status:
    return r.status == 429                    ? "SSBM Nucleus asks to wait."
           : r.status == 404 || r.status == 410 ? "SSBM Nucleus no longer offers this."
           : r.status >= 500                  ? "SSBM Nucleus had an error."
                                              : "SSBM Nucleus answered " + std::to_string(r.status) + ".";
  default: return "The download stopped before it finished.";   // dropped, cancelled, not written
  }
}

void to_net(const host::http::Response& r, NetResult* net) {
  net->status = r.error == host::http::Error::Held ? 429 : r.status;
  net->error = r.system_error;
  net->timed_out = r.timed_out();
  net->retry_after = r.retry_after;
}

// One blocking GET to SSBM Nucleus by the rules above: the body into the file `out`, else into `text`
// (`etag`, when not empty, goes as If-None-Match). Worker threads only.
host::http::Response site_get(const std::string& url, const fs::path* out, std::string* text, uint64_t limit, int wait,
                              const std::string& etag = {}) {
  host::http::Request req;
  req.url = url;
  req.max_bytes = limit;
  // Resolve and connect: 6 s. The answer's first byte (and each later read): by what it is for.
  req.resolve_ms = 6000;
  req.connect_ms = 6000;
  req.send_ms = 10000;
  req.receive_ms = wait == kWaitPicture ? 12000 : wait == kWaitFile ? 30000 : 20000;
  req.allow_loopback_http = !test_api().empty();
  req.host_allowed = allowed_host;   // every hop: still SSBM Nucleus, or nothing is kept
  req.if_none_match = clean_etag(etag);
  // (a connection the site dropped, before the answer or while it came: WinHTTP 12030 on the real
  // site, now and then, gone on the next try)
  req.attempts = 2;
  req.gate = [](const std::string& host, const std::string& path, std::string* why, int* wait_seconds) {
    return !is_api_path(host, path) || api_slot(std::chrono::seconds(20), why, wait_seconds);
  };
  host::http::Response r = out ? host::http::fetch_to_file(req, *out) : host::http::fetch(req);
  if (r.status == 429 || r.error == host::http::Error::Held) {
    std::lock_guard<std::mutex> lock(g_net_mutex);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::clamp(r.retry_after ? r.retry_after : 60, 10, 600));
    if (until > g_api_pause_until) g_api_pause_until = until;
  }
  if (text && r.ok()) *text = std::move(r.body);
  return r;
}

// One blocking GET, to a file or to text. Worker threads only. `too_large` (optional): set when the
// body ran past `limit`.
bool https_get(const std::string& url, const fs::path* out, std::string* text, uint64_t limit, std::string* error,
               bool* too_large = nullptr, int* status = nullptr, int wait = kWaitList, NetResult* net = nullptr) {
  if (text) text->clear();
  const host::http::Response r = site_get(url, out, text, limit, wait);
  if (too_large) *too_large = r.error == host::http::Error::TooLarge;
  if (status) *status = r.error == host::http::Error::Held ? 429 : r.status;
  if (net) to_net(r, net);
  if (!r.ok()) *error = player_message(r);
  return r.ok();
}

std::string text_of(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string safe_name(const std::string& name);

std::string url_encode(const std::string& s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

std::string extension(const std::string& name) {
  const auto dot = name.rfind('.');
  std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext;
}
bool game_file(const std::string& name) {
  const std::string ext = extension(name);
  return ext == ".dat" || ext == ".usd";
}
bool plugin_file(const std::string& name) { return extension(name) == kPluginExtension; }
std::string base_name(const std::string& name) {
  const auto slash = name.find_last_of("/\\");
  return slash == std::string::npos ? name : name.substr(slash + 1);
}
// A file of a fighter's C: directly in a folder named "code", C or a header or the hand edits.
bool source_file(const std::string& name) {
  std::string path = name;
  for (auto& c : path) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
  const auto slash = path.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
  const bool in_code = folder == "code" || (folder.size() > 5 && folder.compare(folder.size() - 5, 5, "/code") == 0);
  const std::string ext = extension(path);
  return in_code && (ext == ".c" || ext == ".h" || base_name(path) == "native.edits");
}
bool info_file(const std::string& name) {
  std::string base = base_name(name);
  for (auto& c : base) c = (char)std::tolower((unsigned char)c);
  return base == "fighter.json";
}

// The fighter tools: what builds a plugin from a fighter's C on this computer. One folder
// (MELEE_FIGHTER_TOOLS) holding the fighter plugin kit (kit), a MinGW-w64 GCC (gcc) and a Python
// (python). Every player must build with the same ones: the compiler decides the code, and two
// builds of one fighter that differ would not stay in step online (the plugin's digest in the
// state checksum says so on the first frame compared).
struct Tools { fs::path kit, gcc_bin, python; };
bool find_tools(Tools* tools, std::string* problem) {
  const char* v = std::getenv("MELEE_FIGHTER_TOOLS");
  if (!v || !*v) { *problem = "Building a fighter needs the fighter tools (MELEE_FIGHTER_TOOLS)"; return false; }
  const fs::path root = fs::u8path(v);
  tools->kit = root / L"kit";
  tools->gcc_bin = root / L"gcc" / L"bin";
  tools->python = root / L"python" / L"python.exe";
  std::error_code ec;
  if (!fs::is_regular_file(tools->kit / L"tools" / L"mexft_build.py", ec) || !fs::is_regular_file(tools->gcc_bin / L"gcc.exe", ec) ||
      !fs::is_regular_file(tools->python, ec)) {
    *problem = "The fighter tools folder is incomplete";
    return false;
  }
  return true;
}

// Runs a program to its end, its output to a file. False when it could not start, ran past the
// time allowed, or ended with an error.
bool run_program(const std::wstring& command, const fs::path& folder, const fs::path& output, int seconds) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE out = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) return false;
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = out;
  PROCESS_INFORMATION pi{};
  std::wstring line = command;
  const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi);
  CloseHandle(out);
  if (!started) return false;
  DWORD code = 1;
  if (WaitForSingleObject(pi.hProcess, (DWORD)seconds * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 1);   // the one this call started
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
#else
  (void)command; (void)folder; (void)output; (void)seconds;
  return false;
#endif
}

// A plugin file into the fighters folder, when it is one: a 64-bit object that names the plugin's
// record. Whole or not at all, since the game may look at the folder at any change of mode.
bool keep_plugin(const fs::path& path, const std::string& wanted_name, const std::string& fighters_folder, std::string* problem,
                 std::string* kept) {
  std::error_code ec;
  if (fighters_folder.empty()) { *problem = "Fighters are off in this launch"; return false; }
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  if (bytes.size() < 64 || bytes.size() > kMaxPluginBytes || (unsigned char)bytes[0] != 0x64 || (unsigned char)bytes[1] != 0x86 ||
      bytes.find("mu_fighter_plugin") == std::string::npos) {
    *problem = "That file is not a fighter plugin";
    return false;
  }
  std::string name;
  for (const char c : wanted_name) name += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_';
  if (name.empty()) name = "fighter";
  const fs::path target = fs::u8path(fighters_folder) / fs::u8path(name + kPluginExtension);
  const fs::path staged = fs::u8path(fighters_folder) / fs::u8path(name + ".part");
  fs::create_directories(target.parent_path(), ec);
  fs::copy_file(path, staged, fs::copy_options::overwrite_existing, ec);
  if (!ec) fs::rename(staged, target, ec);
  if (ec) { *problem = "The plugin could not be written to the fighters folder"; fs::remove(staged, ec); return false; }
  *kept = target.u8string();
  // Kept is not approved: the game library gets it only once Nucleus signed it or the player approved it.
  if (const auto check = g_plugin_check.load(); check && !check(*kept)) {
    ++g_kept_waiting;
    host::log("mod browser: %s is installed but not approved; it loads after the player approves it (next interactive start, "
              "or melee_source --approve-plugin)", kept->c_str());
  }
  return true;
}

std::wstring quoted(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

// Builds a fighter's plugin from the C its post carries, with the fighter tools, and keeps it.
// `package`: the unpacked fighter package the C is in (its code folder), when the post is one;
// otherwise the post's own source files are downloaded.
bool build_from_source(const Mod& mod, const fs::path& folder, const std::string& fighters_folder, std::string* problem,
                       const host::roster::Installed* package = nullptr) {
  Tools tools;
  if (!find_tools(&tools, problem)) return false;
  // The fighter's name: the post's fighter.json says what the disc calls it, else the title.
  std::string fighter_name = mod.title;
  for (const auto& file : mod.files) {
    if (!file.info) continue;
    std::string body, error;
    if (https_get(file.url, nullptr, &body, kMaxPageBytes, &error)) {
      const json doc = json::parse(body, nullptr, false);
      if (doc.is_object() && !text_of(doc, "name").empty()) fighter_name = text_of(doc, "name");
    }
  }
  if (package) fighter_name = package->name;
  // The name goes into mexft.txt, one setting a line: a line break or other control character in
  // it would start a setting of the post's choosing, so each becomes a space.
  for (char& c : fighter_name)
    if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
  std::string ident;
  for (const char c : fighter_name) ident += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_';
  while (!ident.empty() && ident.back() == '_') ident.pop_back();
  if (ident.empty() || std::isdigit((unsigned char)ident[0])) ident = "f_" + ident;
  std::error_code ec;
  const fs::path stage = folder / fs::u8path(ident);
  fs::create_directories(stage / L"code", ec);
  int sources = 0;
  for (const auto& file : mod.files) {
    if (!file.source) continue;
    const std::string base = safe_name(base_name(file.name));
    // The hand edits sit beside the manifest, the C in its folder (tools/mexft/mexft_build.py).
    const fs::path path = base == "native.edits" ? stage / L"native.edits" : stage / L"code" / fs::u8path(base);
    std::string error;
    if (!https_get(file.url, &path, nullptr, kMaxPageBytes, &error)) { *problem = error; return false; }
    if (extension(base) == ".c") ++sources;
  }
  if (package) {
    for (fs::directory_iterator it(package->base / L"code", ec), end; !ec && it != end; it.increment(ec)) {
      const std::string base = it->path().filename().u8string();
      const std::string ext = extension(base);
      if (!it->is_regular_file(ec) || (ext != ".c" && ext != ".h" && base != "native.edits")) continue;
      fs::copy_file(it->path(), base == "native.edits" ? stage / L"native.edits" : stage / L"code" / fs::u8path(safe_name(base)),
                    fs::copy_options::overwrite_existing, ec);
      if (!ec && ext == ".c") ++sources;
    }
  }
  if (!sources) { *problem = "This post has no C source"; return false; }
  {
    std::ofstream manifest(stage / L"mexft.txt", std::ios::binary);
    manifest << "name " << fighter_name << "\nfile Pl" << ident << ".dat\nmex-name " << fighter_name << "\n";
  }
  const fs::path built = folder / fs::u8path(ident + kPluginExtension);
  const fs::path log = folder / L"build.txt";
  const std::wstring command = quoted(tools.python) + L" -I " + quoted(tools.kit / L"tools" / L"mexft_build.py") + L" --kit " +
                               quoted(tools.kit) + L" --gcc-bin " + quoted(tools.gcc_bin) + L" --fighter " + quoted(stage) +
                               L" --out " + quoted(built) + L" --work " + quoted(folder / L"work");
  if (!run_program(command, folder, log, 180) || !fs::is_regular_file(built, ec)) {
    // The last line the tools printed is the reason (a compile error, a missing export).
    std::ifstream in(log);
    std::string line, last;
    while (std::getline(in, line)) if (!line.empty()) last = line;
    host::log("mod browser: mod %d \"%s\": the build failed: %s", mod.id, mod.title.c_str(), last.c_str());
    *problem = "The fighter did not build: " + (last.empty() ? std::string("no output") : last);
    return false;
  }
  std::string kept;
  if (!keep_plugin(built, ident, fighters_folder, problem, &kept)) return false;
  host::log("mod browser: mod %d \"%s\": built %s from %d C files -> %s", mod.id, mod.title.c_str(), fighter_name.c_str(), sources,
            kept.c_str());
  return true;
}
bool plugins_allowed() {
  const char* v = std::getenv("MELEE_ALLOW_PLUGIN_DOWNLOADS");
  return v && v[0] == '1' && v[1] == '\0';
}

// The address of a query's first page: also what its pages are kept under.
std::string query_url(const Query& q) {
  std::string url = api_base() + "/mods?type=" + kTabType[q.tab] + "&sort=" + kSortName[q.sort] + "&limit=" +
                    std::to_string(kRowsPerPage);
  if (q.tab == kCostumes && q.fighter > 0 && q.fighter < (int)fighters().size())
    url += "&character=" + url_encode(fighters()[(size_t)q.fighter]);
  if (!q.search.empty()) url += "&q=" + url_encode(q.search);
  return url;
}

void fetch_page(const PageKey& key) {
  const int page = key.second;
  const bool fighters_tab = key.first.find("type=gameplay_mod") != std::string::npos;
  std::string cursor;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (page > 0) cursor = g_pages[{key.first, page - 1}].next_cursor;
  }
  std::string url = key.first;
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
            file.plugin = plugin_file(file.name);
            file.source = fighters_tab && source_file(file.name);
            file.info = fighters_tab && info_file(file.name);
            file.package = fighters_tab && extension(file.name) == ".zip";
            // On the fighters tab only a fighter's plugin, or the C to build one from, is something to
            // install: the rest of such a post is built for the console.
            if (!file.url.empty() &&
                (file.plugin || file.source || file.info || file.package || (!fighters_tab && game_file(file.name))))
              mod.files.push_back(file);
          }
        }
        // fighter.json alone is nothing to install.
        if (fighters_tab) {
          bool usable = false;
          for (const auto& f : mod.files) usable = usable || f.plugin || f.package || (f.source && extension(f.name) == ".c");
          if (!usable) mod.files.clear();
        }
        // A post that is one archive, or holds no file of a kind that can be handed on yet.
        if (mod.files.empty()) {
          mod.state = kUnsupported;
          mod.note = fighters_tab ? "No fighter package, plugin or C source in this post"
                                  : "This post has no single game file to install";
        }
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
  host::log("mod browser: %s page %d: %s%s (%d mods of %d)", key.first.substr(key.first.find('?') + 1).c_str(), page + 1,
            result.status == kReady ? "ready" : "failed: ", result.status == kReady ? "" : result.message.c_str(),
            (int)result.ids.size(), result.total);
  g_pages[key] = std::move(result);
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
  int installed = 0, present = 0, plugins = 0, fighters = 0;
  g_kept_waiting = 0;
  std::string problem, code_problem;
  std::error_code ec;
  const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / std::to_wstring(id);
  fs::create_directories(folder, ec);
  std::string fighters_folder;
  { std::lock_guard<std::mutex> lock(g_mutex); fighters_folder = g_fighters_folder; }
  for (const auto& file : mod.files) {
    if (g_blocked.load()) { problem = "Stopped: an online session started"; break; }
    const fs::path path = folder / fs::u8path(safe_name(file.name));
    std::string error;
    if (file.source || file.info) continue;   // built after the loop, when the post has no plugin
    if (file.package) {
      // A fighter as data: its files and pictures into the fighters folder. A zip that is not a
      // fighter package is left alone. Its C, when it brings any, is built as a plugin's is.
      const fs::path unpacked = folder / fs::u8path("package_" + std::to_string(file.id));
      host::roster::Installed done;
      if (fighters_folder.empty()) { problem = "Fighters are off in this launch"; continue; }
      if (!https_get(file.url, &path, nullptr, kMaxFileBytes, &error)) { problem = error; continue; }
      fs::create_directories(unpacked, ec);
      // What a fighter package may unpack to, with room to spare: a real one (Shulk) is 20 files and
      // 12 MB, each costume zip in it 3 files and 2 MB.
      static constexpr host::archive::Limits package_limits{600, 64ull << 20, 512ull << 20, 250}, costume_limits{16, 64ull << 20, 64ull << 20, 250};
      const auto unzip_costume = [](const fs::path& zip, const fs::path& dest) {
        std::string why;
        return host::archive::extract(zip, dest, costume_limits, &why);
      };
      std::string why;
      if (!host::archive::extract(path, unpacked, package_limits, &why)) {
        problem = "That archive could not be unpacked";
        host::log("mod browser: mod %d \"%s\": %s could not be unpacked: %s", id, mod.title.c_str(), file.name.c_str(), why.c_str());
        continue;
      }
      if (!host::roster::install_package(unpacked, fs::u8path(fighters_folder), unzip_costume, &done, &problem)) {
        host::log("mod browser: mod %d \"%s\": %s was not installed: %s", id, mod.title.c_str(), file.name.c_str(), problem.c_str());
        continue;
      }
      ++fighters;
      if (done.has_code && !g_blocked.load()) {
        if (!plugins_allowed()) code_problem = "its code needs MELEE_ALLOW_PLUGIN_DOWNLOADS=1";
        else if (build_from_source(mod, folder, fighters_folder, &code_problem, &done)) ++plugins;
      }
      continue;
    }
    if (file.plugin) {
      // A fighter's code, already built. Kept only when the player allowed it.
      if (!plugins_allowed()) { problem = "Fighter code: set MELEE_ALLOW_PLUGIN_DOWNLOADS=1 to allow it"; continue; }
      if (!https_get(file.url, &path, nullptr, kMaxPluginBytes, &error)) { problem = error; continue; }
      std::string kept;
      if (!keep_plugin(path, fs::u8path(safe_name(base_name(file.name))).stem().u8string(), fighters_folder, &problem, &kept)) continue;
      ++plugins;
      host::log("mod browser: mod %d \"%s\": fighter plugin %s -> %s", id, mod.title.c_str(), file.name.c_str(), kept.c_str());
      continue;
    }
    if (!https_get(file.url, &path, nullptr, kMaxFileBytes, &error)) { problem = error; continue; }
    const auto result = host::cosmetics::import_file(path.u8string());
    if (result.ok) {
      if (result.already_present) ++present; else ++installed;
      // Shown under the post's title, not the file's name; a post with several files says which.
      if (!result.already_present && !result.asset_id.empty()) {
        std::string title = mod.title;
        if (mod.files.size() > 1) title += " (" + fs::u8path(safe_name(file.name)).stem().u8string() + ")";
        std::string rename_error;
        host::cosmetics::rename_asset(result.asset_id, title, &rename_error);
      }
      // The ids and the downloaded file's hash, for opponents' skins (only the asset the import named:
      // a zip of several costumes records the one import_file reports).
      if (const auto installed_hook = g_skin_installed.load(); installed_hook && !result.asset_id.empty())
        installed_hook(result.asset_id, id, file.id, path.u8string(), std::string());
      host::log("mod browser: mod %d \"%s\": %s -> %s%s", id, mod.title.c_str(), file.name.c_str(), result.asset_id.c_str(),
                result.already_present ? " (already installed)" : "");
    } else {
      problem = result.message.empty() ? "This file cannot be used here" : result.message;
      host::log("mod browser: mod %d \"%s\": %s was not installed: %s", id, mod.title.c_str(), file.name.c_str(), problem.c_str());
    }
  }
  // No built plugin in the post, but its C: build it here.
  bool has_source = false;
  for (const auto& file : mod.files) has_source = has_source || file.source;
  if (plugins == 0 && has_source && !g_blocked.load()) {
    if (!plugins_allowed()) problem = "Fighter code: set MELEE_ALLOW_PLUGIN_DOWNLOADS=1 to allow it";
    else if (build_from_source(mod, folder, fighters_folder, &problem)) ++plugins;
  }
  fs::remove_all(folder, ec);
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& row = g_mods[id];
  if (fighters > 0) {
    row.state = kInstalled;
    // One short line: the browser shows about 38 characters. Why the code was not built is in the log.
    row.note = !code_problem.empty() ? "Installed; its code was not built"
               : g_kept_waiting      ? "Installed; code needs your approval"
                                     : "Installed: now on the select screen";
    if (!code_problem.empty()) host::log("mod browser: mod %d \"%s\": its code was not built: %s", id, row.title.c_str(), code_problem.c_str());
  } else if (plugins > 0) {
    row.state = kInstalled;
    row.note = g_kept_waiting ? "Installed: needs your approval to play" : "Installed: playable after the next menu change";
  } else if (installed + present > 0) {
    row.state = kInstalled;
    row.note = present && !installed ? "Already installed" : "Installed: pick it with L or R";
  } else {
    row.state = kFailed;
    row.note = problem.empty() ? "Nothing was installed" : problem;
  }
}

void worker() {
  for (;;) {
    PageKey page;
    bool have_page = false;
    int download = 0;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_wake.wait(lock, [] { return !g_page_jobs.empty() || !g_download_jobs.empty(); });
      // A page first: the player is looking at "Loading".
      if (!g_page_jobs.empty()) { page = g_page_jobs.front(); g_page_jobs.pop_front(); have_page = true; }
      else { download = g_download_jobs.front(); g_download_jobs.pop_front(); }
    }
    if (have_page) fetch_page(page);
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

void set_fighters_folder(const std::string& folder) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fighters_folder = folder;
}

void set_plugin_check(bool (*would_load)(const std::string& path)) { g_plugin_check = would_load; }
void set_skin_installed(void (*installed)(const std::string&, int, int, const std::string&, const std::string&)) {
  g_skin_installed = installed;
}

const std::vector<std::string>& fighters() {
  static const std::vector<std::string> names = {
      "", "Bowser", "Captain Falcon", "Donkey Kong", "Dr. Mario", "Falco", "Fox", "Ganondorf", "Ice Climbers",
      "Jigglypuff", "Kirby", "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Mr. Game & Watch", "Ness", "Peach",
      "Pichu", "Pikachu", "Roy", "Samus", "Sheik", "Yoshi", "Young Link", "Zelda"};
  return names;
}

View view(const Query& asked, int page_number) {
  View out;
  Query query = asked;
  if (query.tab < 0 || query.tab >= kTabCount) query.tab = 0;
  if (query.sort < 0 || query.sort >= kSortCount) query.sort = 0;
  if (query.fighter < 0 || query.fighter >= (int)fighters().size()) query.fighter = 0;
  if ((int)query.search.size() > kMaxSearch) query.search.resize(kMaxSearch);
  out.page = page_number < 0 ? 0 : page_number;
  if (g_blocked.load()) {
    out.status = kError;
    out.message = "Not available during an online session.";
    return out;
  }
  const std::string address = query_url(query);
  std::lock_guard<std::mutex> lock(g_mutex);
  const PageKey key{address, out.page};
  auto it = g_pages.find(key);
  const bool stale = it != g_pages.end() && it->second.status != kLoading &&
                     std::chrono::steady_clock::now() - it->second.fetched > kPageLife;
  // A page after the first needs the one before it (its cursor): the game only steps one at a time.
  const bool reachable = out.page == 0 || (g_pages.count({address, out.page - 1}) &&
                                            !g_pages[{address, out.page - 1}].next_cursor.empty());
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

// Opponents' skins online (host/skin_fetch.h).
std::string api_address() { return api_base(); }

bool get_file(const std::string& url, const std::string& out_utf8, uint64_t limit, std::string* error, bool* too_large,
              int* status) {
  const fs::path out = fs::u8path(out_utf8);
  return https_get(url, &out, nullptr, limit, error, too_large, status, kWaitFile);
}

bool get_text(const std::string& url, std::string* body, uint64_t limit, std::string* error, int* status, int wait,
              NetResult* net) {
  body->clear();
  return https_get(url, nullptr, body, limit, error, nullptr, status, wait, net);
}

bool get_picture(const std::string& url, const std::string& etag, std::string* body, uint64_t limit, std::string* error,
                 bool* not_modified, std::string* new_etag, NetResult* net) {
  body->clear();
  *not_modified = false;
  new_etag->clear();
  const std::string sent = clean_etag(etag);
  const host::http::Response r = site_get(url, nullptr, body, limit, kWaitPicture, sent);
  if (net) to_net(r, net);
  if (!r.ok()) { *error = player_message(r); return false; }
  *not_modified = r.not_modified;
  const std::string got = clean_etag(r.etag);
  *new_etag = got.empty() && *not_modified ? sent : got;
  return true;
}

// Letters and digits only, lower case, é as e: "Pokémon Stadium: Fire" -> "pokemonstadiumfire".
static std::string stage_key(const std::string& text) {
  std::string k;
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (c == 0xC3 && i + 1 < text.size() && ((unsigned char)text[i + 1] == 0xA9 || (unsigned char)text[i + 1] == 0x89)) {
      k += 'e';
      ++i;
    } else if (std::isalnum(c)) {
      k += (char)std::tolower(c);
    }
  }
  return k;
}

std::string stage_slot_for(const std::string& file_name, const std::string& stage) {
  if (file_name.size() < 4 || stage_key(file_name.substr(file_name.size() - 4)) != "dat") return {};
  const std::string file = stage_key(file_name.substr(0, file_name.size() - 4));
  const auto slots = host::cosmetics::stage_slots();
  std::string best;
  size_t best_length = 0;
  for (const auto& s : slots) {
    const std::string disc = stage_key(s.target_path.substr(0, s.target_path.size() >= 4 ? s.target_path.size() - 4 : 0));
    if (disc.size() >= 4 && file.compare(0, disc.size(), disc) == 0 && disc.size() > best_length) {
      best = s.target_path;
      best_length = disc.size();
    }
  }
  // (a file named just as the disc's: its import failed for another reason, which this does not mend)
  if (best_length == file.size()) return {};
  if (!best.empty()) return best;
  const std::string want = stage_key(stage);
  if (want.size() < 3) return {};
  std::string only;
  int matches = 0;
  for (const auto& s : slots) {
    const std::string label = stage_key(s.name);
    if (label == want) return s.target_path;
    if (label.compare(0, want.size(), want) == 0) {
      only = s.target_path;
      ++matches;
    }
  }
  return matches == 1 ? only : std::string();
}

// Whether an address's query has a parameter named "view": the marker an opponent's skin is shown with
// (skin_fetch.h download_view_address), which an install never sends.
static bool names_view(const std::string& url) {
  const size_t q = url.find('?');
  if (q == std::string::npos) return false;
  const std::string query = url.substr(q + 1, url.find('#', q) == std::string::npos ? std::string::npos : url.find('#', q) - q - 1);
  for (size_t at = 0; at <= query.size();) {
    size_t amp = query.find('&', at);
    if (amp == std::string::npos) amp = query.size();
    const std::string param = query.substr(at, amp - at);
    if (param.substr(0, param.find('=')) == "view") return true;
    at = amp + 1;
  }
  return false;
}

Installed install_file(int mod_id, int file_id, const std::string& url, const std::string& file_name,
                       const std::string& title, const std::string& expected_sha256, int64_t expected_size,
                       const std::string& stage, const std::string& stage_file, bool effect, bool use_effect) {
  Installed out;
  if (g_blocked.load()) { out.message = "Not during an online session."; return out; }
  if (names_view(url)) {
    host::log("mods menu: card %d of mod %d: refused download address %s (an install is never a view)", file_id, mod_id,
              url.c_str());
    out.message = "Not an SSBM Nucleus link.";
    return out;
  }
  std::error_code ec;
  const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / (L"card_" + std::to_wstring(file_id));
  fs::create_directories(folder, ec);
  std::string name = safe_name(base_name(file_name));
  if (name.empty()) name = "card_" + std::to_string(file_id) + ".dat";
  const fs::path path = folder / fs::u8path(name);
  std::string error;
  NetResult net;
  if (!https_get(url, &path, nullptr, kMaxFileBytes, &error, nullptr, nullptr, kWaitFile, &net)) {
    host::log("mods menu: card %d of mod %d (%s): not installed, the download failed (%s%s): %s", file_id, mod_id,
              name.c_str(), net.status ? "HTTP " : "WinHTTP error ",
              std::to_string(net.status ? (unsigned long)net.status : net.error).c_str(), error.c_str());
    out.message = error;
    out.slow = net.timed_out;
    fs::remove_all(folder, ec);
    return out;
  }
  // The site's size and hash of the file, when it has read it: a download that differs (replaced since
  // the card was read, or broken on the way) is not installed.
  if (expected_size >= 0 || expected_sha256.size() == 64) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string want = expected_sha256;
    for (auto& c : want) c = (char)std::tolower((unsigned char)c);
    const bool size_ok = expected_size < 0 || (int64_t)bytes.size() == expected_size;
    const bool hash_ok = want.size() != 64 || host::hackpack_ai::rules::sha256_hex(bytes.data(), bytes.size()) == want;
    if (!size_ok || !hash_ok) {
      host::log("mods menu: card %d of mod %d: the download (%zu bytes) is not the file the site lists (%s)", file_id,
                mod_id, bytes.size(), !size_ok ? "size" : "sha256");
      out.mismatch = true;
      out.message = "The file changed on the site. Try again.";
      fs::remove_all(folder, ec);
      return out;
    }
  }
  // The site's stage_file (the disc file its export would name it, a stage slot of this game's) when the
  // card gives one and the file is not named so already; else the file as it is named.
  const auto lower_name = [](std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  const bool by_stage_file = !effect && !stage_file.empty() && lower_name(name) != lower_name(stage_file);
  // An effects card: its file through the effect import (its disc file from its roots or its name).
  auto result = effect          ? host::cosmetics::import_effect(path.u8string(), use_effect)
                : by_stage_file ? host::cosmetics::import_stage_dat(path.u8string(), stage_file)
                                : host::cosmetics::import_file(path.u8string());
  if (by_stage_file)
    host::log("mods menu: card %d of mod %d (%s): installed as %s, the site's stage_file%s", file_id, mod_id, name.c_str(),
              stage_file.c_str(), result.ok ? "" : " (refused)");
  // A stage file named for its post ("GrNLaWaffle.dat") rather than for the disc's file: the stage its
  // name starts with, else the card's own stage, as the desktop app's "replace which stage" would.
  const std::string slot = result.ok || by_stage_file || effect ? std::string() : stage_slot_for(name, stage);
  if (!slot.empty()) {
    host::log("mods menu: card %d of mod %d (%s): no disc file of that name; installed as %s", file_id, mod_id,
              name.c_str(), slot.c_str());
    result = host::cosmetics::import_stage_dat(path.u8string(), slot);
  }
  if (result.ok) {
    out.ok = true;
    out.already = result.already_present;
    out.asset_id = result.asset_id;
    if (out.asset_id.empty()) {
      // A stage file's import names no asset: the one it made holds these very bytes.
      std::ifstream in(path, std::ios::binary);
      const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const std::string digest = host::hackpack_ai::rules::sha256_hex(bytes.data(), bytes.size());
      for (const auto& asset : host::cosmetics::assets())
        if (asset.sha256 == digest && asset.source == "import") { out.asset_id = asset.id; break; }
    }
    if (!result.already_present && !out.asset_id.empty() && !title.empty()) {
      std::string rename_error;
      host::cosmetics::rename_asset(out.asset_id, title, &rename_error);
    }
    // (an effect's origin is only for the Mods menu's lists: online, only a port's costume is announced)
    if (const auto installed_hook = g_skin_installed.load(); installed_hook && !out.asset_id.empty())
      installed_hook(out.asset_id, mod_id, file_id, path.u8string(), std::string());
    host::log("mods menu: card %d of mod %d (%s) -> %s%s", file_id, mod_id, name.c_str(), out.asset_id.c_str(),
              result.already_present ? " (already installed)" : "");
  } else {
    out.message = result.message.empty() ? "This file cannot be used here." : result.message;
    out.verdict = result.verdict;
    host::log("mods menu: card %d of mod %d (%s) was not installed: %s", file_id, mod_id, name.c_str(), out.message.c_str());
  }
  fs::remove_all(folder, ec);
  return out;
}

namespace {

// One card's export: a DAT or two of a few MB and two pictures. The real exports are a few hundred KB.
constexpr uint64_t kMaxExportBytes = 32u * 1024u * 1024u;

// The site itself: the API's address without /api/public/v1 (the test stand-in's likewise).
std::string site_root() {
  std::string api = api_base();
  const std::string tail = "/api/public/v1";
  if (api.size() > tail.size() && api.compare(api.size() - tail.size(), tail.size(), tail) == 0) api.resize(api.size() - tail.size());
  return api;
}

std::string lower_text(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// The costume slot a file's name gives (PlXxYy: the fighter and colour codes), lower case, or "".
std::string slot_code(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
  for (size_t i = 0; i + 6 <= base.size(); ++i)
    if ((base[i] == 'P' || base[i] == 'p') && (base[i + 1] == 'l' || base[i + 1] == 'L') &&
        std::isalpha((unsigned char)base[i + 2]) && std::isalpha((unsigned char)base[i + 3]) &&
        std::isupper((unsigned char)base[i + 4]) && std::islower((unsigned char)base[i + 5]))
      return lower_text(base.substr(i, 6));
  return {};
}

bool is_dat(const fs::path& p) {
  const std::string e = lower_text(p.extension().u8string());
  return e == ".dat" || e == ".usd";
}

std::string file_sha256(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return host::hackpack_ai::rules::sha256_hex(bytes.data(), bytes.size());
}

bool is_zip(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char magic[4]{};
  in.read(magic, 4);
  return in.gcount() == 4 && std::memcmp(magic, "PK\x03\x04", 4) == 0;
}

// The export's DATs (it has no folders: "PlCaNrMod.dat", "GrNBa.dat").
std::vector<fs::path> dats_in(const fs::path& root) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && is_dat(it->path())) out.push_back(it->path());
  std::sort(out.begin(), out.end());
  return out;
}

// The installed asset that holds exactly these bytes (an import that made several, or a stage's,
// names one or none).
std::string asset_holding(const std::string& digest) {
  for (const auto& asset : host::cosmetics::assets())
    if (asset.sha256 == digest && asset.source == "import") return asset.id;
  return {};
}

}  // namespace

std::string export_url(int mod_id, int file_id) {
  return site_root() + "/api/posts/" + std::to_string(mod_id) + "/export-nucleus/" + std::to_string(file_id);
}

Installed install_export(int mod_id, int file_id, const std::string& url, const std::string& card_file_name,
                         const std::string& title, int partner_file_id, bool* bundled, const std::string& expected_sha256,
                         bool effect, bool use_effect) {
  Installed out;
  if (bundled) *bundled = false;
  if (g_blocked.load()) { out.message = "Not during an online session."; return out; }
  if (mod_id <= 0 || file_id <= 0) { out.message = "This skin has no download."; return out; }
  // Only the export route of this very card, on the site (or the test stand-in): an export_url the
  // API gives is taken when it is exactly that, whatever else it says.
  const std::string tail = "/api/posts/" + std::to_string(mod_id) + "/export-nucleus/" + std::to_string(file_id);
  const std::string address = url.empty() ? export_url(mod_id, file_id) : url;
  if (address != export_url(mod_id, file_id) && address != "https://ssbmnucleus.net" + tail &&
      address != "https://www.ssbmnucleus.net" + tail) {
    host::log("mods menu: card %d of mod %d: refused export address %s", file_id, mod_id, address.c_str());
    out.message = "Not an SSBM Nucleus link.";
    return out;
  }
  std::error_code ec;
  const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / (L"card_" + std::to_wstring(file_id));
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  const auto finish = [&](Installed result) { fs::remove_all(folder, ec); return result; };

  // The export, as the site's "Add to Nucleus" button has the SSBM Nucleus desktop app fetch it
  // (nucleus://import?url=<this>): a small zip, counted as a download of the mod.
  const fs::path zip = folder / L"export.zip";
  std::string error;
  int status = 0;
  bool too_large = false;
  NetResult net;
  if (!https_get(address, &zip, nullptr, kMaxExportBytes, &error, &too_large, &status, kWaitFile, &net)) {
    // The route's own answers (the site's docs/public-api.md): 404 the mod is no longer public, 400 the
    // file cannot be packaged, 500 the file is missing from storage (a 5xx is temporary).
    out.message = too_large ? "The skin is too large." :
                  status == 404 || status == 410 ? "It is no longer offered." :
                  status == 400 ? "The site cannot pack it." :
                  status == 429 ? "SSBM Nucleus asks to wait." :
                  status >= 500 ? "SSBM Nucleus had an error." : error;
    out.slow = net.timed_out;
    host::log("mods menu: card %d of mod %d: not installed, the export was not fetched (%s%s): %s", file_id, mod_id,
              status ? "HTTP " : "WinHTTP error ", std::to_string(status ? (unsigned long)status : net.error).c_str(),
              out.message.c_str());
    return finish(out);
  }
  if (!is_zip(zip)) {
    host::log("mods menu: card %d of mod %d: the export is not a zip", file_id, mod_id);
    out.message = "The site sent no skin.";
    return finish(out);
  }
  // Unpacked within limits only to see which DATs it brings; the importer reads the zip itself.
  const fs::path unpacked = folder / L"export";
  constexpr host::archive::Limits limits{16, 64ull << 20, 64ull << 20, 250};
  std::string why;
  if (!host::archive::extract(zip, unpacked, limits, &why)) {
    host::log("mods menu: card %d of mod %d: not installed, the export did not unpack: %s", file_id, mod_id, why.c_str());
    out.message = "The download is damaged.";
    return finish(out);
  }
  const auto dats = dats_in(unpacked);
  if (dats.empty()) {
    host::log("mods menu: card %d of mod %d: not installed, the export holds no DAT", file_id, mod_id);
    out.message = "No skin file in it.";
    return finish(out);
  }
  // The card's own DAT: the one of its costume slot (an Ice Climbers export holds both halves), else
  // the only one.
  // An effects card's export: the file under its own name (and "preview.<ext>"), so the DAT of the
  // card's file name when there are several.
  const std::string want_slot = effect ? std::string() : slot_code(card_file_name);
  fs::path own;
  fs::path partner;
  if (effect)
    for (const auto& d : dats)
      if (lower_text(d.filename().u8string()) == lower_text(base_name(card_file_name))) own = d;
  for (const auto& d : dats)
    if (!want_slot.empty() && slot_code(d.filename().u8string()) == want_slot) own = d;
  if (own.empty() && dats.size() == 1) own = dats[0];
  if (own.empty() && dats.size() > 1 && want_slot.empty() && !effect) {
    // A stage export with Pokemon Stadium's forms: the base stage (the one whose name has no digit).
    for (const auto& d : dats) {
      const std::string stem = d.stem().u8string();
      if (std::none_of(stem.begin(), stem.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        if (!own.empty()) { own.clear(); break; }
        own = d;
      }
    }
  }
  if (own.empty()) {
    host::log("mods menu: card %d of mod %d (%s): the export's %zu files name no single one for it", file_id, mod_id,
              card_file_name.c_str(), dats.size());
    out.message = "Cannot tell its file.";
    return finish(out);
  }
  for (const auto& d : dats)
    if (d != own && !slot_code(d.filename().u8string()).empty() && !want_slot.empty()) partner = d;
  if (bundled) *bundled = !partner.empty();
  // The DAT in the export is the card's file renamed: when the site has read that file, its hash
  // must be this one's (else it was replaced since the card was read, or broken on the way).
  const std::string own_sha = file_sha256(own), zip_sha = file_sha256(zip);
  std::string want_sha = expected_sha256;
  for (auto& c : want_sha) c = (char)std::tolower((unsigned char)c);
  if (want_sha.size() == 64 && own_sha != want_sha) {
    host::log("mods menu: card %d of mod %d: the export's file is not the one the site lists (sha256 %s)", file_id, mod_id,
              own_sha.c_str());
    out.mismatch = true;
    out.message = "The file changed on the site. Try again.";
    return finish(out);
  }

  if (effect) {
    // The card's own file through the effect import, and the export's picture (its preview.png, the
    // mod's first screenshot, when it is a PNG) as the effect's picture in the lists.
    const auto result = host::cosmetics::import_effect(own.u8string(), use_effect);
    if (!result.ok) {
      out.message = result.message.empty() ? "This file cannot be used here." : result.message;
      out.verdict = result.verdict;
      host::log("mods menu: card %d of mod %d (%s) from the site's export was not installed: %s", file_id, mod_id,
                card_file_name.c_str(), out.message.c_str());
      return finish(out);
    }
    out.ok = true;
    out.already = result.already_present;
    out.asset_id = result.asset_id;
    if (!result.already_present && !out.asset_id.empty() && !title.empty()) {
      std::string rename_error;
      host::cosmetics::rename_asset(out.asset_id, title, &rename_error);
    }
    if (const auto installed_hook = g_skin_installed.load(); installed_hook && !out.asset_id.empty())
      installed_hook(out.asset_id, mod_id, file_id, own.u8string(), zip_sha);
    fs::path picture;
    for (fs::recursive_directory_iterator it(unpacked, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec) || lower_text(it->path().extension().u8string()) != ".png") continue;
      if (picture.empty() || lower_text(it->path().stem().u8string()) == "preview") picture = it->path();
    }
    std::string picture_note = "no PNG picture";
    if (!picture.empty() && !out.asset_id.empty()) {
      const auto set = host::cosmetics::set_effect_preview(out.asset_id, picture.u8string());
      picture_note = set.ok ? "picture " + picture.filename().u8string() : "picture refused: " + set.message;
    }
    host::log("mods menu: card %d of mod %d (%s) from the site's export -> %s%s, %s (DAT sha256 %s, export zip sha256 %s)",
              file_id, mod_id, card_file_name.c_str(), out.asset_id.c_str(), result.already_present ? " (already installed)" : "",
              picture_note.c_str(), own_sha.c_str(), zip_sha.c_str());
    return finish(out);
  }

  // The normal import of a zip: the same checks as any skin, with its portrait and stock icon.
  const auto result = host::cosmetics::import_file(zip.u8string());
  if (!result.ok) {
    out.message = result.message.empty() ? "This file cannot be used here." : result.message;
    host::log("mods menu: card %d of mod %d was not installed: %s", file_id, mod_id, out.message.c_str());
    return finish(out);
  }
  out.ok = true;
  out.already = result.already_present;
  out.asset_id = asset_holding(own_sha);
  if (out.asset_id.empty() && dats.size() == 1) out.asset_id = result.asset_id;
  if (!result.already_present && !out.asset_id.empty() && !title.empty()) {
    std::string rename_error;
    host::cosmetics::rename_asset(out.asset_id, title, &rename_error);
  }
  const auto installed_hook = g_skin_installed.load();
  if (installed_hook && !out.asset_id.empty()) installed_hook(out.asset_id, mod_id, file_id, own.u8string(), zip_sha);
  std::string partner_asset;
  if (!partner.empty()) {
    partner_asset = asset_holding(file_sha256(partner));
    if (!result.already_present && !partner_asset.empty() && !title.empty()) {
      std::string rename_error;
      host::cosmetics::rename_asset(partner_asset, title, &rename_error);
    }
    if (installed_hook && !partner_asset.empty() && partner_file_id > 0)
      installed_hook(partner_asset, mod_id, partner_file_id, partner.u8string(), zip_sha);
    out.partner_asset_id = partner_asset;
  }
  host::log("mods menu: card %d of mod %d (%s) from the site's export -> %s%s%s%s (DAT sha256 %s, export zip sha256 %s)",
            file_id, mod_id, card_file_name.c_str(), out.asset_id.c_str(), partner_asset.empty() ? "" : " + ",
            partner_asset.c_str(), result.already_present ? " (already installed)" : "", own_sha.c_str(), zip_sha.c_str());
  return finish(out);
}

}  // namespace host::mod_browser
