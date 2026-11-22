// The in-game mod browser's host half (mod_browser.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mod_browser.h"
#include "cosmetic_mods.h"
#include "host.h"
#include "test_env.h"

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
struct File { int id = 0; std::string name, url; bool plugin = false, source = false, info = false; };
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

// Test runs only (MELEE_TEST_MOD_BROWSER_API=http://127.0.0.1:<port>/...): a stand-in for the site
// on this computer. Nothing but the loopback address is accepted, and never in a YouGame session.
std::string test_api() {
  const char* v = host::test_env::getenv("MELEE_TEST_MOD_BROWSER_API");
  const std::string api = v ? v : "";
  return api.rfind("http://127.0.0.1:", 0) == 0 ? api : std::string();
}
std::string api_base() {
  const std::string test = test_api();
  return test.empty() ? std::string(kApi) : test;
}

// Only the API's own host and its subdomains (the media host a download redirects to).
bool allowed_host(const std::wstring& host) {
  static const std::wstring site = L"ssbmnucleus.net";
  if (host == L"127.0.0.1" && !test_api().empty()) return true;
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
  const bool local = !test_api().empty() && url.rfind("http://127.0.0.1:", 0) == 0;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTPS && !local) || !allowed_host(host)) {
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
                                            local ? 0 : WINHTTP_FLAG_SECURE) : nullptr;
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
                            WinHttpCrackUrl(final_url, 0, 0, &fc) && (fc.nScheme == INTERNET_SCHEME_HTTPS || local) &&
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
  if (fighters_folder.empty()) { *problem = "This game has no fighters folder"; return false; }
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
  return true;
}

std::wstring quoted(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

// Builds a fighter's plugin from the C its post carries, with the fighter tools, and keeps it.
bool build_from_source(const Mod& mod, const fs::path& folder, const std::string& fighters_folder, std::string* problem) {
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
            // On the fighters tab only a fighter's plugin, or the C to build one from, is something to
            // install: the rest of such a post is built for the console.
            if (!file.url.empty() && (file.plugin || file.source || file.info || (!fighters_tab && game_file(file.name))))
              mod.files.push_back(file);
          }
        }
        // fighter.json alone is nothing to install.
        if (fighters_tab) {
          bool usable = false;
          for (const auto& f : mod.files) usable = usable || f.plugin || (f.source && extension(f.name) == ".c");
          if (!usable) mod.files.clear();
        }
        // A post that is one archive, or holds no file of a kind that can be handed on yet.
        if (mod.files.empty()) {
          mod.state = kUnsupported;
          mod.note = fighters_tab ? "No fighter plugin or C source in this post" : "This post has no single game file to install";
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
  int installed = 0, present = 0, plugins = 0;
  std::string problem;
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
  if (plugins > 0) {
    row.state = kInstalled;
    row.note = "Installed: playable after the next menu change";
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

}  // namespace host::mod_browser
