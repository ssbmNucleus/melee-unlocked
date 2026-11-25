// The Mods menu's Fighters (the category menu's fourth row); shared pieces in mods_menu_internal.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Browse Fighters lists the custom characters of SSBM Nucleus that play here as they are, one row a post,
// from one request to the site's list of fighter packages (docs/public-api.md, GET /fighters):
//   <api>/fighters?limit=100
// Each post there carries what the site's check found in its package ("fighter": where the fighter is in
// the zip, the zip's SHA-256 and size, and whether it brings code of its own). This port runs the game
// natively, so a fighter with code of its own plays only with a native plugin (sourceport/game/mexft):
// such a post is never listed and never downloaded (host/fighter_package.h needs_plugin decides, from the
// site's facts here and again from the files at install). A search ("Search" row) narrows the list here.
// The viewer shows a post's first screenshot; A downloads the whole package through its download_url
// (checked against the site's SHA-256) and installs it (fighter_package::install). The game reads the
// fighters folder again at its next change of mode, so the fighter is on the character select when the
// player gets there, offline: the online select never offers installed fighters.
// Manage Fighters lists the fighters installed from here (their folder's nucleus.json), removed with A;
// one that came another way (Wolf and Shulk come with the game) is not listed.
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

// A fighter's card id: this plus its post's id (kept apart from skins' file ids, which share the maps).
constexpr int kFighterCards = 1 << 30;

// The fighters folder's fighters installed from the Mods menu: post id -> folder.
std::map<int, std::string> fighter_origins(const fs::path& folder) {
  std::map<int, std::string> out;
  std::error_code ec;
  if (folder.empty()) return out;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    host::fighter_package::Origin origin;
    if (it->is_directory(ec) && host::fighter_package::read_origin(it->path(), &origin))
      out[origin.mod_id] = it->path().filename().u8string();
  }
  return out;
}

int fighter_state_locked(const Card& card) {
  if (g_mode == 1) {
    const auto it = g_removals.find(card.asset_id);
    return it != g_removals.end() ? it->second.state : kInstalled;
  }
  const auto it = g_progress.find(card.file_id);
  if (it != g_progress.end() && (it->second.state == kDownloading || it->second.state == kFailed)) return it->second.state;
  return g_fighter_installed.count(card.mod_id) ? kInstalled : kNotInstalled;
}

std::string fighter_what(const Card& c) {
  return c.character.empty() ? "New fighter" : "New fighter - " + c.character + "'s moves";
}

bool fighters_fetch_page(const std::string& key, int mode, int category) {
  if (category != kFighters) return false;
  int query_id = 0;
  {
    int m = 0, c = 0, f = 0, p = 0;
    std::sscanf(key.c_str(), "%d/%d/%d/%d/%d", &m, &c, &f, &query_id, &p);
  }
  std::string folder_text, query;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    folder_text = g_fighters_folder;
    if (const auto it = g_queries.find(query_id); it != g_queries.end()) query = it->second;
  }
  const fs::path folder = fs::u8path(folder_text);
  Page result;
  result.status = kReady;
  result.has_next = false;
  const auto origins = fighter_origins(folder);
  if (mode == 1) {
    // Manage: the fighters installed from here, by their folder's name (the order of their ids).
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(folder, ec), end; !folder.empty() && !ec && it != end; it.increment(ec))
      if (it->is_directory(ec) && fs::is_regular_file(it->path() / "fighter.json", ec)) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
      // Only the ones installed from here: one that came another way (Wolf and Shulk come with the
      // game) is not this menu's to remove, so it is not listed.
      host::fighter_package::Origin origin;
      if (!host::fighter_package::read_origin(dir, &origin)) continue;
      Card c;
      c.kind = "fighter";
      c.file_name = dir.filename().u8string();
      try {
        std::ifstream in(dir / "fighter.json", std::ios::binary);
        const json j = json::parse(in);
        c.title = text_of(j, "name", 200);
      } catch (const std::exception&) {}
      if (c.title.empty()) c.title = c.file_name;
      if (fs::is_regular_file(dir / "csp_0.png", ec)) c.local_picture = (dir / "csp_0.png").u8string();
      c.asset_id = "fighter:" + c.file_name;
      c.mod_id = origin.mod_id;
      c.file_id = kFighterCards + origin.mod_id;
      c.character = origin.donor;
      // The post's title last: the menu's lettering drops its brackets ("Slippy (Fox Clone).").
      c.description = "It plays offline, with " + (origin.donor.empty() ? std::string("its base fighter") : origin.donor) +
                      "'s moves and sounds. From SSBM Nucleus: " + origin.title;
      result.cards.push_back(std::move(c));
    }
    result.total = (int)result.cards.size();
    result.per_page = std::max<int>(1, result.total);
    if (result.cards.empty()) result.message = folder.empty() ? "Fighters are off in this launch." : "No fighters installed yet.";
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fighter_installed = origins;
    g_pages[key] = std::move(result);
    if (key == page_key(g_mode, g_category, g_filter, g_page)) prefetch_screen_locked(0, 0);
    return true;
  }
  // Browse: the site's fighters that play here as they are.
  json j;
  std::string error;
  int status = 0;
  host::mod_browser::NetResult net;
  const bool got = !g_blocked.load() &&
                   get_json(host::mod_browser::api_address() + "/fighters?limit=100", &j, &status, &error, &net);
  int hidden = 0, plugin = 0, matched = 0;
  if (!got && (status == 400 || status == 404)) {
    result.message = "SSBM Nucleus does not list fighters yet.";
  } else if (!got || !j.is_object() || !j.contains("data") || !j["data"].is_array()) {
    result.status = kError;
    result.slow = net.timed_out;
    result.message = got ? "SSBM Nucleus sent no list." : error.empty() ? "SSBM Nucleus did not answer." : one_line(error, 47);
  } else {
    const std::string words = lower(query);
    for (const auto& item : j["data"]) {
      if (!item.is_object() || !item.contains("fighter") || !item["fighter"].is_object()) continue;
      const json& f = item["fighter"];
      const auto flag = [&](const char* k) { return f.contains(k) && f[k].is_boolean() && f[k].get<bool>(); };
      const auto count = [&](const char* k) { return f.contains(k) && f[k].is_number_integer() ? f[k].get<int>() : 0; };
      host::fighter_package::Facts facts;
      facts.own_code_file = flag("own_code_file");
      facts.functions_outside = count("functions_outside");
      facts.moves_outside = count("moves_outside");
      facts.own_move_table = flag("own_move_table");
      facts.items = count("items");
      facts.code_folder = flag("code_folder");
      facts.donor_unknown = text_of(f, "donor", 64).empty();
      const std::string sha = lower(text_of(f, "zip_sha256", 80));
      const int id = item.contains("id") && item["id"].is_number_integer() ? item["id"].get<int>() : 0;
      std::string reason = host::fighter_package::needs_plugin(facts);
      if (!reason.empty()) ++plugin;
      if (reason.empty() && (count("checker") < 1 || sha.size() != 64)) reason = "the site has not checked its package";
      // A zip this game's archive reader refuses (an absolute or repeated name inside it): it would fail.
      if (reason.empty() && !text_of(f, "archive_problem", 200).empty()) reason = "its package: " + text_of(f, "archive_problem", 200);
      if (!reason.empty()) {
        // Not offered and never downloaded: it would not play here.
        ++hidden;
        host::log("mods menu: fighter post %d \"%s\" is not listed: %s", id, text_of(item, "title", 60).c_str(), reason.c_str());
        continue;
      }
      Card c;
      c.kind = "fighter";
      c.mod_id = id;
      c.file_id = kFighterCards + id;
      c.title = text_of(item, "title", 200);
      c.description = "Offline only. " + text_of(item, "description", 2000);
      c.author = text_of(item, "author", 100);
      c.creator = text_of(item, "creator", 100);
      c.downloads = item.contains("download_count") && item["download_count"].is_number_integer() ? item["download_count"].get<int>() : 0;
      c.character = text_of(f, "donor", 64);
      c.download_url = text_of(item, "download_url", 1024);
      if (c.download_url.empty()) c.download_url = text_of(item, "zip_url", 1024);
      c.sha256 = sha;
      c.size = f.contains("zip_size") && f["zip_size"].is_number_integer() ? f["zip_size"].get<int64_t>() : -1;
      c.variant = text_of(f, "root", 512);   // where the fighter is in the zip (fighter_package::install)
      c.file_name = text_of(f, "name", 100);
      Post p;
      p.mod_id = id;
      p.title = c.title;
      p.author = c.author;
      p.creator = c.creator;
      p.downloads = c.downloads;
      p.description = c.description;
      p.count = 1;
      p.shots_known = true;
      if (item.contains("screenshots") && item["screenshots"].is_array())
        for (const auto& s : item["screenshots"])
          if (s.is_string()) {
            if (p.shots.size() < 3) p.shots.push_back(s.get<std::string>());
            add_picture(c, s.get<std::string>());
          }
      add_picture(c, text_of(item, "thumbnail_url", 1024));
      if (id <= 0 || c.title.empty() || c.download_url.empty()) continue;
      if (!words.empty() && lower(c.title).find(words) == std::string::npos && lower(c.creator).find(words) == std::string::npos &&
          lower(c.author).find(words) == std::string::npos)
        continue;
      ++matched;
      p.cards.push_back(std::move(c));
      result.posts.push_back(std::move(p));
    }
    result.total = (int)result.posts.size();
    result.per_page = std::max<int>(1, result.total);
    // The ones that need a plugin are counted, not listed ("3 fighters, 1 more needs a plugin").
    const std::string need = plugin == 1 ? " needs a plugin" : " need a plugin";
    if (!result.posts.empty()) result.message = counted(matched, "fighter") + (plugin > 0 ? ", " + std::to_string(plugin) + " more" + need : "");
    else if (!query.empty()) result.message = "Nothing matches the search.";
    else result.message = plugin > 0 ? "Nothing here yet: " + std::to_string(plugin) + need : "Nothing here yet.";
    host::log("mods menu: Fighters: %d listed, %d not (they need a plugin or the site's check)", matched, hidden);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fighter_installed = origins;
  g_pages[key] = std::move(result);
  if (key == page_key(g_mode, g_category, g_filter, g_page)) prefetch_screen_locked(0, 0);
  return true;
}

bool fighters_run_install(const Card& card) {
  if (card.kind != "fighter") return false;
  std::string folder_text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    folder_text = g_fighters_folder;
  }
  host::log("mods menu: install fighter of mod %d \"%s\" (%lld bytes, from %s)", card.mod_id, card.title.c_str(),
            (long long)card.size, log_host(card.download_url).c_str());
  std::string message;
  bool ok = false;
  std::error_code ec;
  const fs::path zip = fs::temp_directory_path(ec) / L"NucleusFighters" / (L"download-" + std::to_wstring(card.mod_id) + L".zip");
  if (folder_text.empty()) {
    message = "Fighters are off in this launch.";
  } else if (g_blocked.load()) {
    message = "Not during an online session.";
  } else {
    fs::create_directories(zip.parent_path(), ec);
    std::string error;
    bool too_large = false;
    const uint64_t limit = card.size > 0 ? (uint64_t)card.size + 1 : 256ull << 20;
    if (!host::mod_browser::get_file(card.download_url, zip.u8string(), limit, &error, &too_large)) {
      message = too_large ? "The package is larger than the site said." : error.empty() ? "The download failed." : error;
    } else {
      std::string bytes;
      {
        std::ifstream in(zip, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      const std::string sha = host::http::sha256_hex(bytes.data(), bytes.size());
      if ((card.size >= 0 && (int64_t)bytes.size() != card.size) || (card.sha256.size() == 64 && sha != card.sha256)) {
        host::log("mods menu: fighter of mod %d: the download (%zu bytes, %s) is not the package the site checked (%lld, %s)",
                  card.mod_id, bytes.size(), sha.c_str(), (long long)card.size, card.sha256.c_str());
        message = "The download is not the package the site checked.";
      } else {
        const auto done = host::fighter_package::install(zip, fs::u8path(folder_text), card.variant, card.mod_id, card.title, sha);
        ok = done.ok;
        message = ok ? "Installed: on the character select now." : done.message;
      }
    }
  }
  fs::remove(zip, ec);
  host::log("mods menu: install fighter of mod %d: %s%s", card.mod_id, ok ? "installed" : "failed: ", ok ? "" : message.c_str());
  const auto origins = fighter_origins(fs::u8path(folder_text));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fighter_installed = origins;
  Progress& p = g_progress[card.file_id];
  p.state = ok ? kInstalled : kFailed;
  p.message = message;
  forget_pages_locked();
  return true;
}

bool fighters_run_remove(const Card& card) {
  if (card.kind != "fighter") return false;
  std::string folder_text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    folder_text = g_fighters_folder;
  }
  std::string error;
  const std::string name = card.asset_id.rfind("fighter:", 0) == 0 ? card.asset_id.substr(8) : std::string();
  const bool ok = host::fighter_package::remove(fs::u8path(folder_text), name, &error);
  host::log("mods menu: remove fighter %s (%s): %s", name.c_str(), card.title.c_str(), ok ? "done" : error.c_str());
  const auto origins = fighter_origins(fs::u8path(folder_text));
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fighter_installed = origins;
  Progress& p = g_removals[card.asset_id];
  p.state = ok ? kRemoved : kFailed;
  p.message = ok ? "Removed." : error;
  if (ok) g_progress.erase(card.file_id);
  return true;
}

}  // namespace menu_internal
}  // namespace host::mods_menu
