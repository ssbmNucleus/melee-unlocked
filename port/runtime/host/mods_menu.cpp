// The in-game Mods menu's host half: the session's state and the entry points (mods_menu.h). The
// parts are in mods_menu_*.cpp; mods_menu_internal.h lists what they share.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {
// The session's state, every variable the files share (mods_menu_internal.h declares them), defined
// here in the order the one file defined them, so they are built and torn down in that order.

std::mutex g_mutex;
Queue g_page_queue, g_picture_queue, g_install_queue;
std::atomic<bool> g_blocked{false};
int g_mode = 0, g_category = 0, g_filter = 0, g_page = 0;
int g_pack = 0;                           // Browse: the pack (mod id) whose cards are listed, 0 none
int g_pack_page = 0;
// Browse is one continuous list: g_page is its last batch read (page key g_page holds every post up to
// it); g_more the next batch being read for it while the player scrolls toward its end, -1 none.
int g_more = -1;
// Browse Characters starts with the list of fighters (g_picking): "All fighters" and each fighter in
// the game's roster order; A on one is its posts, B there back to the fighters. The last one chosen
// is where the fighters' list opens next, for the session.
bool g_picking = false;
int g_last_fighter = 0;
std::map<std::string, Page> g_pages;      // by page key, for the session
std::map<std::string, Stream> g_streams;  // Browse: by list key and source ("/m" /mods, "/c" /cards)
std::map<int, int> g_card_info;           // /cards/{id} asked for a post of /mods's one card: file id -> status
std::string g_last_opened;                // the viewer's card last logged (its key)
std::map<std::string, Picture> g_pictures;  // by card key, for the session (the oldest go past kPicturesKept)
std::map<int, Progress> g_progress;       // installs, by file id
std::map<std::string, Progress> g_removals;   // removals, by asset id
std::vector<std::pair<std::string, FileKind>> g_republish;   // disc files to publish again (take_republish)
std::map<int, std::string> g_installed;   // file id -> asset id, from the installed skins' origins
bool g_installed_known = false;
uint32_t g_picture_generation = 0;
Batch g_batch;
std::atomic<int> g_cards_api{kCardsUnknown};
// Browse's search (the game's name entry): "" none. Each search has its own id in the page keys.
std::string g_query;
int g_query_id = 0;
std::map<int, std::string> g_queries;
std::map<std::string, PackList> g_pack_lists;
std::map<int, Counts> g_counts;   // by category
// The installed cards' details file (mods_menu_install.cpp).
std::mutex g_details_mutex;
// The pictures' copies on disk (mods_menu_pictures.cpp).
std::mutex g_disk_mutex;
uint64_t g_disk_bytes = 0;
bool g_disk_counted = false;
std::atomic<bool> g_offline{false};   // tests: nothing is fetched, installed or removed
// Fighters (mods_menu_fighters.cpp).
std::string g_fighters_folder;                     // set_fighters_folder; empty: none this run
std::map<int, std::string> g_fighter_installed;    // post id -> folder, from the fighters' nucleus.json

}  // namespace menu_internal

void set_blocked(bool blocked) { g_blocked.store(blocked); }

void set_fighters_folder(const std::string& folder_utf8) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fighters_folder = folder_utf8;
}

void open(int mode, int category, bool again) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int m = mode == 1 ? 1 : 0, c = category_of(category);
  // Back from the search's name entry to the same list: its fighter, page and search stay (the search
  // sent from the name entry has already set them).
  if (again && m == g_mode && c == g_category) {
    host::log("mods menu: %s, %s again%s%s%s", g_mode ? "Manage" : "Browse", category_name(g_category),
              g_query.empty() ? "" : ", search \"", g_query.c_str(), g_query.empty() ? "" : "\"");
    return;
  }
  g_mode = m;
  g_category = c;
  // Browse Characters and Effects: the fighters first, on the one chosen last.
  g_picking = m == 0 && by_fighter(c);
  g_filter = g_picking ? g_last_fighter : 0;
  g_page = 0;
  g_more = -1;
  g_pack = 0;
  g_pack_page = 0;
  g_query.clear();   // a search lasts while its list is open
  g_query_id = 0;
  if (g_mode == 1) forget_pages_locked();   // Manage: what is installed now
  if (stages_open_locked()) return;         // Stages: the six first (mods_menu_stages.cpp)
  if (effects_open_locked()) return;        // Manage Effects: the fighters with effects (mods_menu_effect_slots.cpp)
  host::log("mods menu: %s, %s%s", g_mode ? "Manage" : "Browse", category_name(g_category),
            g_picking ? ", the fighters" : "");
}

bool change(int op, int row) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Stages: the six, a stage's posts or pool (mods_menu_stages.cpp); the rest as for Characters' posts.
  if (const int staged = stages_change_locked(op, row); staged >= 0) return staged != 0;
  // Effects: a fighter's moves, a move's effects (mods_menu_effect_slots.cpp); the posts as Characters'.
  if (const int moved = effects_change_locked(op, row); moved >= 0) return moved != 0;
  // The fighters' list (Browse Characters' first): A on a row is that fighter's posts; nothing else
  // changes it. B in a fighter's posts (not in a pack) is back to it, the search ended.
  if (g_picking) {
    const auto& order = roster_filters();
    if (op != 9 || g_mode != 0 || !by_fighter(g_category) || row < 0 || row >= (int)order.size()) return false;
    g_filter = g_last_fighter = order[(size_t)row];
    g_picking = false;
    g_page = 0;
    g_more = -1;
    g_pack = 0;
    g_pack_page = 0;
    host::log("mods menu: fighter %s", g_filter > 0 ? characters()[(size_t)g_filter].c_str() : "all");
    return true;
  }
  if (op == 10) {
    if (g_mode != 0 || !by_fighter(g_category) || g_pack != 0) return false;
    g_picking = true;
    g_last_fighter = g_filter;
    g_page = 0;
    g_more = -1;
    g_query.clear();
    g_query_id = 0;
    host::log("mods menu: back to the fighters");
    return true;
  }
  Page& page = current_locked();
  const Post* pack = pack_locked(page);
  switch (op) {
  case 0:     // (a continuous list has no page before)
    if (pack) { if (g_pack_page == 0) return false; --g_pack_page; return true; }
    return false;
  case 1:
    if (pack) {
      if ((size_t)(g_pack_page + 1) * kPackPerPage >= pack->cards.size()) return false;
      ++g_pack_page;
      return true;
    }
    // More posts at the list's end, read in the background while the rows read so far stay: the
    // next batch only, one at a time ("Loading..." at the end meanwhile; the list grows when it is in).
    if (g_mode != 0 || page.status != kReady || !page.has_next) return false;
    if (g_more >= 0) {
      const std::string key = page_key(g_mode, g_category, g_filter, g_more);
      const auto it = g_pages.find(key);
      if (it == g_pages.end() || it->second.status != kError) return false;   // on its way
      g_pages.erase(it);   // it failed: ask again
      g_pages.emplace(key, Page{});
      queue_page_locked(key);
      return true;
    }
    g_more = g_page + 1;
    {
      const std::string key = page_key(g_mode, g_category, g_filter, g_more);
      g_pages.erase(key);
      g_pages.emplace(key, Page{});
      queue_page_locked(key);
    }
    return true;
  case 2: case 3: {   // L and R: the fighter before or after in the roster ("All fighters" first)
    if (g_mode != 0 || !by_fighter(g_category) || pack) return false;
    const auto& order = roster_filters();
    const int n = (int)order.size();
    g_filter = g_last_fighter = order[(size_t)((roster_row(g_filter) + (op == 3 ? 1 : n - 1)) % n)];
    g_page = 0;
    g_more = -1;
    return true;
  }
  case 4: {
    if (page.status != kError) return false;
    g_pages.erase(page_key(g_mode, g_category, g_filter, g_page));
    g_more = -1;
    current_locked();
    return true;
  }
  case 5: {   // A on a pack's row: its cards
    const auto rows = rows_locked(page);
    if (pack || row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowPack) return false;
    g_pack = rows[(size_t)row].post->mod_id;
    g_pack_page = 0;
    host::log("mods menu: pack %d \"%s\" (%zu cards on its row)", g_pack, rows[(size_t)row].post->title.c_str(),
              rows[(size_t)row].post->cards.size());
    // The pack's own list from /cards (its cards the site shows), read now, once;
    // until then (or on a site without it) the cards its row read.
    // (not New Stages: a custom stage's file is no card of the site's; the pack is its row's files)
    if (g_cards_api.load() != kCardsNo && g_category != kNewStages) {
      const std::string key = pack_key(g_category, g_filter, g_pack);
      const auto it = g_pack_lists.find(key);
      if (it == g_pack_lists.end() || it->second.status == kError) queue_pack_locked(key);
    }
    return true;
  }
  case 6:     // B in a pack's list: back to the posts
    if (g_pack == 0) return false;
    g_pack = 0;
    g_pack_page = 0;
    return true;
  case 7: {   // "Install all": the pack's cards not installed yet, one after another
    if (!kInstallAll || !pack || g_blocked.load() || g_batch.running) return false;
    g_batch = Batch{g_batch.id + 1, pack->mod_id, 0, 0, 0, 0, true};
    for (const auto& c : pack->cards) {
      const int state = state_locked(c);
      if (effect_refused_locked(c)) continue;   // an effect the checks refuse: not installable
      if (state == kInstalled || state == kDownloading || c.mod_id <= 0 || c.file_id <= 0) {
        ++g_batch.skipped;
        continue;
      }
      g_progress[c.file_id] = {kDownloading, "", false};
      Job job;
      job.kind = Job::kInstall;
      job.card = c;
      job.batch = g_batch.id;
      queue_install_locked(std::move(job));
      ++g_batch.total;
    }
    if (g_batch.total == 0) g_batch.running = false;
    host::log("mods menu: install all of pack %d: %d to install, %d installed already", pack->mod_id, g_batch.total,
              g_batch.skipped);
    return true;
  }
  }
  return false;
}

bool search(const std::string& words) {
  std::string q;
  for (const unsigned char c : words) q += c < 0x20 || c >= 0x7F ? ' ' : (char)c;
  q = drawable(q, 24);
  while (!q.empty() && q.front() == ' ') q.erase(q.begin());
  std::lock_guard<std::mutex> lock(g_mutex);
  if (q == g_query) return false;
  g_query = q;
  g_query_id = 0;
  if (!q.empty()) {
    for (const auto& [id, text] : g_queries)
      if (text == q) g_query_id = id;
    if (g_query_id == 0) {
      g_query_id = g_queries.empty() ? 1 : g_queries.rbegin()->first + 1;
      g_queries[g_query_id] = q;
    }
  }
  g_mode = 0;
  g_page = 0;
  g_more = -1;
  g_pack = 0;
  g_pack_page = 0;
  g_picking = false;
  host::log("mods menu: search %s%s%s", q.empty() ? "ended" : "\"", q.c_str(), q.empty() ? "" : "\"");
  return true;
}


bool action(int row, int file_id) {
  if (g_blocked.load()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_picking) return false;
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  if (row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowCard) return false;
  const Card* c = rows[(size_t)row].card;
  if (file_id != 0 && c->file_id != file_id) {
    // The list changed under the viewer (a pack's own list came, a post turned out to be a pack): the
    // row is another card now, not the one the player saw.
    host::log("mods menu: A on row %d refused: the viewer showed card %d, the row is card %d now", row, file_id, c->file_id);
    return false;
  }
  host::log("mods menu: A on row %d \"%s\": card %d (%s)", row, drawable(c->label.empty() ? c->title : c->label, 60).c_str(),
            c->file_id, c->file_name.c_str());
  // An effect held for its notice (it would override another's moves): this A, asked twice, uses it.
  if (effect_held_locked(*c)) return effect_use_held_locked(*c);
  if (g_mode == 0 && effect_refused_locked(*c)) {
    host::log("mods menu: card %d is not installable: %s", c->file_id, effect_refused_locked(*c)->c_str());
    return false;
  }
  const int state = state_locked(*c);
  Job job;
  job.card = *c;
  if (g_mode == 1) {
    if (state == kRemoving || state == kRemoved || c->asset_id.empty()) return false;
    g_removals[c->asset_id] = {kRemoving, "", false};
    job.kind = Job::kRemove;
  } else {
    if (state == kDownloading || state == kInstalled || c->mod_id <= 0 || c->file_id <= 0) return false;
    g_progress[c->file_id] = {kDownloading, "", false};
    job.kind = Job::kInstall;
  }
  queue_install_locked(std::move(job));
  return true;
}

std::vector<std::pair<std::string, FileKind>> take_republish() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return std::exchange(g_republish, {});
}

void set_offline_for_test(bool offline) { g_offline.store(offline); }

bool load_browse_page_for_test(int category, int filter, const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  const int n = (int)characters().size();
  filter = filter > 0 && filter < n ? filter : 0;
  const std::string character = filter > 0 ? characters()[filter] : std::string();
  Page page;
  int cards = 0, packs = 0;
  // A /cards reply (its items have mod_id) or a /mods one.
  const bool cards_body = !j.is_discarded() && j.is_object() && j.contains("data") && j["data"].is_array() &&
                          !j["data"].empty() && j["data"][0].is_object() && j["data"][0].contains("mod_id");
  Stream stream;
  stream.from_cards = cards_body;
  const bool ok = !j.is_discarded() && stream_add(j, category, cards_body ? std::string() : character, stream);
  if (ok) {
    page = screen_of(stream, category, 0, &cards, &packs);
  } else {
    page.status = kError;
    page.message = "SSBM Nucleus sent no list.";
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mode = 0;
  g_category = category_of(category);
  g_filter = filter;
  g_page = 0;
  g_more = -1;
  g_picking = false;
  g_pack = 0;
  g_pack_page = 0;
  g_pack_lists.clear();
  g_streams[list_key(g_mode, g_category, g_filter) + (cards_body ? "/c" : "/m")] = std::move(stream);
  g_pages[page_key(g_mode, g_category, g_filter, g_page)] = std::move(page);
  g_installed_known = true;
  return ok;
}

bool load_more_for_test(const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (j.is_discarded() || g_mode != 0) return false;
  const int page = g_more >= 0 ? g_more : g_page + 1;
  const std::string lkey = list_key(g_mode, g_category, g_filter);
  auto stream = g_streams.find(lkey + "/m");
  if (stream == g_streams.end()) stream = g_streams.find(lkey + "/c");
  if (stream == g_streams.end()) return false;
  const std::string character = g_filter > 0 ? characters()[g_filter] : std::string();
  if (!stream_add(j, g_category, stream->second.from_cards ? std::string() : character, stream->second)) return false;
  const auto before = g_pages.find(page_key(g_mode, g_category, g_filter, page - 1));
  int cards = 0, packs = 0;
  g_pages[page_key(g_mode, g_category, g_filter, page)] =
      screen_of(stream->second, g_category, page, &cards, &packs, before != g_pages.end() ? &before->second : nullptr);
  g_more = page;
  return true;
}

bool load_pack_for_test(int mod_id, const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("data") || !j["data"].is_array()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  Post post;
  post.mod_id = mod_id;
  for (const auto& item : j["data"]) {
    Card c;
    if (!card_of(item, g_category, c) || c.mod_id != mod_id) continue;
    if (post.title.empty()) post.title = c.title;
    post.cards.push_back(std::move(c));
  }
  finish_post(post, g_category);
  PackList& list = g_pack_lists[pack_key(g_category, g_filter, mod_id)];
  list.status = post.cards.empty() ? kError : kReady;
  list.post = std::move(post);
  return list.status == kReady;
}

bool load_counts_for_test(int category, int status, const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  Counts c = counts_of(j, status == 200 && !j.is_discarded(), status);
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool ok = c.status == kReady;
  g_counts[category_of(category)] = std::move(c);
  return ok;
}

std::string install_name_for_test(int row) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  if (row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowCard) return {};
  return install_name(*rows[(size_t)row].card);
}

int withheld_for_test(int row) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  if (row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowCard) return -1;
  const Card& c = *rows[(size_t)row].card;
  return (c.withhold_csp ? 1 : 0) | (c.withhold_stock ? 2 : 0);
}

}  // namespace host::mods_menu
