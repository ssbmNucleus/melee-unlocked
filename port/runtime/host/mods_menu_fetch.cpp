// The Mods menu's requests for lists: Browse's pages and packs, a card's info, the fighters' counts.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {
const char* duplicates_param() { return kListHiddenDuplicates ? "&include_duplicates=1" : ""; }

bool get_json(const std::string& url, json* out, int* status, std::string* error, host::mod_browser::NetResult* net) {
  std::string body;
  if (!host::mod_browser::get_text(url, &body, kMaxPageBytes, error, status, host::mod_browser::kWaitList, net))
    return false;
  *out = json::parse(body, nullptr, false);
  if (out->is_discarded()) { *error = "SSBM Nucleus sent something that is not a list."; return false; }
  return true;
}

// What a list page that could not be read tells the player.
std::string page_error(int status, const host::mod_browser::NetResult& net) {
  if (net.timed_out) return "SSBM Nucleus is slow right now.";
  if (status == 429) return "SSBM Nucleus asks to wait.";
  if (status == 502 || status == 503 || status == 504 || (status >= 520 && status <= 524)) return "SSBM Nucleus is busy.";
  if (status != 0 && status != 200) return "SSBM Nucleus answered " + std::to_string(status) + ".";
  if (status == 200 && net.error != 0) return "The connection dropped.";
  return status == 200 ? "SSBM Nucleus sent no list." : "Could not reach SSBM Nucleus.";
}

void fetch_page(const std::string& key) {
  int mode = 0, category = 0, filter = 0, query_id = 0, page = 0;
  if (std::sscanf(key.c_str(), "%d/%d/%d/%d/%d", &mode, &category, &filter, &query_id, &page) != 5) return;
  if (fighters_fetch_page(key, mode, category)) return;   // Fighters (mods_menu_fighters.cpp)
  const std::string character = by_fighter(category) && filter > 0 && filter < (int)characters().size() ? characters()[filter] : std::string();
  if (fetch_pool_page(key, mode, category, filter)) return;   // Manage Stages: a stage's pool
  Page result;
  if (mode == 1) {
    std::map<int, std::string> by_file;
    auto rows = scan_installed(category, &by_file);
    // A character skin with no online form (costume_slots.h): it plays offline as installed, and online
    // its slot is the standard costume. Said first in its description; nothing else changes.
    for (auto& c : rows) {
      std::string why;
      if (c.kind == "character_costume" && !c.file_name.empty() && !host::costume_slots::plays_online(c.asset_id, c.file_name, &why))
        c.description = "Offline only: online it plays as the standard costume. " + c.description;
    }
    // Manage is one whole list (what is installed), no pages.
    result.status = kReady;
    result.total = (int)rows.size();
    result.per_page = std::max<int>(1, (int)rows.size());
    result.cards = rows;
    result.has_next = false;
    if (rows.empty())
      result.message = category == kEffects ? "No effects installed yet."
                       : category == kMenus ? "No menu mods installed yet."
                       : category == kNewStages ? "No new stages installed yet."
                       : category         ? "No stage skins installed yet." : "No character skins installed yet.";
    std::lock_guard<std::mutex> lock(g_mutex);
    g_installed = std::move(by_file);
    g_installed_known = true;
    g_pages[key] = std::move(result);
    if (key == page_key(g_mode, g_category, g_filter, g_page)) prefetch_screen_locked(0, 0);
    return;
  }
  const std::string lkey = key.substr(0, key.rfind('/'));
  std::string query;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const auto it = g_queries.find(query_id); it != g_queries.end()) query = it->second;
  }
  const std::string what = std::string(category_name(category)) + " (" +
                           (character.empty() ? std::string("all") : character) +
                           (query.empty() ? std::string() : ", \"" + query + "\"") + ")";
  // Browse lists posts, one row each, kScreenPosts to a screen, a pack's cards read only when it is
  // opened (fetch_pack). "All fighters" and Stages read the site's posts (docs/public-api.md, GET /mods):
  //   <api>/mods?type=costume|stage_skin&sort=downloads&limit=10[&q=<words>][&cursor=]
  // A fighter's list reads the site's cards of that fighter, grouped by post here, because /mods's
  // character filter takes the name as it is written (it misses "C. Falcon" files under "Captain
  // Falcon") and /cards's takes every spelling (its "Fighter names"):
  //   <api>/cards?type=costume&limit=100&character=<name>[&q=<words>][&cursor=]
  // (with &include_duplicates=1 after the type while kListHiddenDuplicates)
  // Effects are read the same way, with type=effects (a fighter's from /cards).
  // A site that refuses that (400, 404, or a web page) has the fighter's posts from /mods instead, for
  // the session. One parameter each (the site refuses unknown or repeated ones). A list's posts are kept
  // for the session; a screen reads what it lacks, at most kRequestsPerScreen requests.
  const bool fighter = by_fighter(category) && !character.empty();
  const std::string type = site_type(category);
  std::string error;
  int status = 0, requests = 0;
  host::mod_browser::NetResult net;
  bool failed = false, from_cards = false;
  std::string skey;
  for (;;) {
    from_cards = fighter && g_cards_api.load() != kCardsNo;
    skey = lkey + (from_cards ? "/c" : "/m");
    std::string cursor;
    size_t have = 0;
    bool done = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      Stream& s = g_streams[skey];
      s.from_cards = from_cards;
      have = s.posts.size();
      done = s.done;
      cursor = s.next_cursor;
    }
    // A /cards post is whole once the next one has begun (or the list has ended).
    const size_t need = (size_t)(page + 1) * kScreenPosts + (from_cards ? 1 : 0);
    if (done || have >= need || requests >= kRequestsPerScreen + page || g_blocked.load()) break;
    if (have > 0 && cursor.empty()) break;
    const std::string url =
        host::mod_browser::api_address() +
        (from_cards ? "/cards?type=" + type + duplicates_param() + "&limit=" + std::to_string(kCardsPerPage)
                    : "/mods?" + mods_filter(category) + "&sort=downloads&limit=" + std::to_string(kScreenPosts)) +
        (fighter ? "&character=" + encode(character) : "") + stage_param(category, filter) +
        (query.empty() ? "" : "&q=" + encode(query)) +
        (cursor.empty() ? "" : "&cursor=" + encode(cursor));
    json j;
    error.clear();
    status = 0;
    net = host::mod_browser::NetResult{};
    ++requests;
    const bool got = get_json(url, &j, &status, &error, &net);
    if (category == kMenus && menus_refused(status)) continue;   // a site before type=menu (mods_menu_menus.cpp)
    if (from_cards && (status == 400 || status == 404 || (status == 200 && !got))) {
      g_cards_api.store(kCardsNo);
      host::log("mods menu: the site's /cards does not answer (HTTP %d): a fighter's posts come from /mods", status);
      continue;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Stream& s = g_streams[skey];
    if (!got || !stream_add(j, category, from_cards ? std::string() : character, s)) {
      if (got) error = "SSBM Nucleus sent no list.";
      failed = true;
      break;
    }
    if (category == kStages) narrow_stage_posts(s.posts, filter);   // that stage's files of each post
    if (from_cards && g_cards_api.exchange(kCardsYes) != kCardsYes)
      host::log("mods menu: the site's /cards answers: a fighter's list reads cards%s",
                kListHiddenDuplicates ? " (those hidden as duplicates too)" : "");
  }
  Page result_page;
  int cards = 0, packs = 0;
  size_t stream_posts = 0;
  bool stream_done = false;
  // The batch is read when the stream holds posts past the batches before it (a batch the site
  // failed on leaves the list as it was: the list says so at its end, and A asks again).
  size_t first_new = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Stream& s = g_streams[skey];
    stream_posts = s.posts.size();
    stream_done = s.done;
    if (!failed || stream_posts > (size_t)page * kScreenPosts) {
      const auto before = page > 0 ? g_pages.find(page_key(mode, category, filter, page - 1)) : g_pages.end();
      result_page = screen_of(s, category, page, &cards, &packs, before != g_pages.end() ? &before->second : nullptr);
      first_new = before != g_pages.end() && before->second.status == kReady ? before->second.posts.size() : 0;
    }
  }
  if (!failed || stream_posts > (size_t)page * kScreenPosts) {
    int hidden = 0, shared = 0, new_cards = 0;
    std::vector<Card> seen;
    for (size_t i = first_new > 0 ? first_new - 1 : 0; i < result_page.posts.size(); ++i)
      for (const auto& c : result_page.posts[i].cards) {
        hidden += c.hidden_duplicate ? 1 : 0;
        shared += c.shares_portrait ? 1 : 0;
        ++new_cards;
        seen.push_back(c);
      }
    host::log("mods menu: Browse %s, %zu posts listed from %s (%d packs, %d cards; %zu new: %d cards, %d hidden on the "
              "site as duplicate portraits, %d sharing a portrait); %d %s in all%s (%d requests now, %zu posts read)",
              what.c_str(), result_page.posts.size(), from_cards ? "/cards" : "/mods", packs, cards,
              result_page.posts.size() - std::min(result_page.posts.size(), first_new), new_cards, hidden, shared,
              result_page.total, from_cards ? "cards" : "posts", result_page.has_next ? ", more after" : "", requests,
              stream_posts);
    std::map<int, std::string> by_file;
    scan_installed(category, &by_file);
    repicture_installed(seen, by_file);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pages[key] = std::move(result_page);
    g_installed = std::move(by_file);
    g_installed_known = true;
    // The list's first look: its first rows' pictures are asked for now, before the game's cursor says
    // where it is (a batch read on while scrolling waits for that).
    if (page == 0 && key == page_key(g_mode, g_category, g_filter, g_page) && g_pack == 0 && !g_picking)
      prefetch_screen_locked(0, 0);
    return;
  }
  (void)stream_done;
  result.status = kError;
  result.slow = net.timed_out;
  result.message = error == "SSBM Nucleus sent no list." ? error : page_error(status, net);
  host::log("mods menu: Browse %s page %d: not read (%s%s): %s", what.c_str(), page + 1,
            status ? "HTTP " : "WinHTTP error ", std::to_string(status ? (unsigned long)status : net.error).c_str(),
            error.empty() ? result.message.c_str() : error.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pages[key] = std::move(result);
}

// A pack's cards from the site (A on its row: read then, once for the session): the ones the site
// shows (kListHiddenDuplicates), in the order the mod lists its files, narrowed to the fighter shown
// as its row was:
//   <api>/cards?type=costume|stage_skin&mod_id=<id>&limit=100[&character=<name>][&cursor=]
// at most kMaxCardsPerPost. Until it is read (or when it cannot be), the pack shows the cards its row
// had.
void fetch_pack(const std::string& key) {
  int category = 0, filter = 0, mod_id = 0;
  if (std::sscanf(key.c_str(), "%d/%d/%d", &category, &filter, &mod_id) != 3 || mod_id <= 0) return;
  const std::string character = by_fighter(category) && filter > 0 && filter < (int)characters().size() ? characters()[filter] : std::string();
  Post post;
  post.mod_id = mod_id;
  std::string cursor, error;
  bool ok = true;
  int requests = 0, status = 0;
  do {
    const std::string url = host::mod_browser::api_address() + "/cards?type=" + site_type(category) +
                            "&mod_id=" + std::to_string(mod_id) + duplicates_param() + "&limit=" +
                            std::to_string(kCardsPerPage) + (character.empty() ? "" : "&character=" + encode(character)) +
                            (cursor.empty() ? "" : "&cursor=" + encode(cursor));
    json j;
    status = 0;
    host::mod_browser::NetResult net;
    if (!get_json(url, &j, &status, &error, &net) || !j.is_object() || !j.contains("data") || !j["data"].is_array()) {
      ok = false;
      break;
    }
    for (const auto& item : j["data"]) {
      Card c;
      if (post.cards.size() >= kMaxCardsPerPost || !card_of(item, category, c) || c.mod_id != mod_id) continue;
      if (post.title.empty()) {
        post.title = c.title;
        post.author = c.author;
        post.creator = c.creator;
        post.downloads = c.downloads;
        post.description = c.description;
      }
      post.cards.push_back(std::move(c));
    }
    cursor = text_of(j, "next_cursor", 512);
  } while (!cursor.empty() && post.cards.size() < kMaxCardsPerPost && ++requests < 4 && !g_blocked.load());
  if (category == kStages) narrow_stage_cards(post.cards, filter);   // that stage's files of the pack
  int hidden = 0;
  for (const auto& c : post.cards) hidden += c.hidden_duplicate ? 1 : 0;
  const size_t listed = post.cards.size();
  finish_post(post, category);
  post.count = (int)post.cards.size();
  if (ok && post.cards.empty()) ok = false;
  int withheld = 0;
  for (const auto& c : post.cards) withheld += c.withhold_csp ? 1 : 0;
  host::log("mods menu: pack %d from /cards?mod_id: %s%zu cards (%d hidden on the site as duplicate portraits, %d with "
            "a portrait another file's shares), %d rows%s",
            mod_id, ok ? "" : "not read, ", listed, hidden, withheld, post.count,
            ok ? "" : (" (HTTP " + std::to_string(status) + "; the pack keeps its row's cards)").c_str());
  if (ok) {
    std::map<int, std::string> by_file;
    scan_installed(category, &by_file);
    repicture_installed(post.cards, by_file);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  // The post's own words and screenshots, when its row (/mods) or its page had them already.
  for (const auto& [pkey, page] : g_pages)
    for (const auto& known : page.posts)
      if (known.mod_id == mod_id && known.shots_known && !post.shots_known) {
        post.shots = known.shots;
        post.shots_known = true;
        if (!known.description.empty()) post.description = known.description;
      }
  PackList& list = g_pack_lists[key];
  list.status = ok ? kReady : kError;
  if (ok) list.post = std::move(post);
  if (ok && g_pack == mod_id && key == pack_key(g_category, g_filter, mod_id)) prefetch_screen_locked(0, 0);
}

// A post of /mods with one card, viewed: what /mods does not say of it, from its card
// (docs/public-api.md, GET /cards/{id}):
//   <api>/cards/<id>
// whether its portrait is marked shared; and, only while kListHiddenDuplicates (then with
// ?include_duplicates=1), whether its post has cards the site hides as duplicate portraits
// (mod_card_count_with_duplicates over mod_card_count): then its row becomes the pack it is ("5 skins"),
// whose list A reads. Once per card for the session.
void fetch_card_info(const Card& card, int category) {
  json j;
  std::string error;
  int status = 0;
  const std::string url = host::mod_browser::api_address() + "/cards/" + std::to_string(card.file_id) +
                          (kListHiddenDuplicates ? "?include_duplicates=1" : "");
  Card info;
  const bool got = get_json(url, &j, &status, &error) && j.is_object() && j.contains("data") &&
                   card_of(j["data"], category, info) && info.file_id == card.file_id && info.mod_id == card.mod_id;
  const int with_duplicates =
      got && kListHiddenDuplicates ? number_of(j["data"], "mod_card_count_with_duplicates") : 0;
  const int cards = got ? number_of(j["data"], "mod_card_count") : 0;
  const int more = with_duplicates > cards && cards > 0 ? with_duplicates - cards : 0;
  host::log("mods menu: card %d of mod %d from /cards/{id}: %s%s", card.file_id, card.mod_id,
            got ? (info.shares_portrait ? "portrait marked shared" : "portrait its own") : "not read",
            got && more ? (", and " + std::to_string(more) + " more cards hidden on the site as duplicate portraits: a pack").c_str() : "");
  std::lock_guard<std::mutex> lock(g_mutex);
  g_card_info[card.file_id] = got ? kReady : kError;
  if (!got) return;
  const auto update = [&](Post& post) {
    for (auto& c : post.cards) {
      if (c.file_id != card.file_id) continue;
      c.shares_portrait = c.shares_portrait || info.shares_portrait;
      if (c.stage_file.empty()) c.stage_file = info.stage_file;
      if (c.export_url.empty()) c.export_url = info.export_url;
      post.hidden_more = std::max(post.hidden_more, more);
      post.count = std::max(post.count, (int)post.cards.size() + post.hidden_more);
    }
  };
  for (auto& [key, page] : g_pages)
    for (auto& post : page.posts) update(post);
  for (auto& [key, stream] : g_streams)
    for (auto& post : stream.posts) update(post);
}

// A post's screenshots and words learnt from /mods/{id} (a pack of a fighter's list, read from /cards):
// every copy of the post the session holds gets them, so they are not asked for again. Locked.
void learn_post_words_locked(int mod_id, const Post& words) {
  const auto update = [&](Post& post) {
    if (post.mod_id != mod_id || post.shots_known) return;
    post.shots = words.shots;
    post.shots_known = true;
    if (!words.description.empty()) post.description = words.description;   // the post's own (/mods)
  };
  for (auto& [key, page] : g_pages)
    for (auto& post : page.posts) update(post);
  for (auto& [key, stream] : g_streams)
    for (auto& post : stream.posts) update(post);
  for (auto& [key, list] : g_pack_lists) update(list.post);
}

void forget_pages_locked() {
  // An install or a removal changes what Manage lists; Browse's pages only mark their rows anew.
  for (auto it = g_pages.begin(); it != g_pages.end();)
    it = it->first.rfind("1/", 0) == 0 ? g_pages.erase(it) : std::next(it);
}

// The fighters' counts of one category (g_counts): one request for all of them. Each name the site
// gives is read as fighter_key reads a name, so its spellings add up to one fighter. A reply that is
// not such a list (an older site: 404; or a web page) leaves the counts off for the session.
Counts counts_of(const json& j, bool got, int status) {
  Counts c;
  c.when = std::chrono::steady_clock::now();
  bool ok = got && j.is_object() && j.contains("data") && j["data"].is_object() && j["data"].size() <= 1000 &&
            j.contains("total") && j["total"].is_number_integer() && j["total"].get<int64_t>() >= 0;
  if (ok) {
    for (const auto& item : j["data"].items()) {
      const json& n = item.value();
      if (!n.is_number_integer() || n.get<int64_t>() < 0) { ok = false; break; }
      const std::string key = fighter_key(item.key().substr(0, 64));
      if (key.empty()) continue;
      int& sum = c.by_fighter[key];
      sum = (int)std::min<int64_t>((int64_t)sum + n.get<int64_t>(), INT32_MAX);
    }
    c.total = number_of(j, "total");
  }
  c.status = ok ? kReady : kError;
  c.retry = !ok && (status == 0 || status == 429 || status >= 500);
  if (!ok) c.by_fighter.clear();
  return c;
}

void fetch_counts(int category) {
  const std::string url = host::mod_browser::api_address() + "/cards/counts?type=" + site_type(category);
  json j;
  std::string error;
  int status = 0;
  host::mod_browser::NetResult net;
  const bool got = get_json(url, &j, &status, &error, &net);
  Counts c = counts_of(j, got, status);
  const bool ok = c.status == kReady;
  host::log("mods menu: %s fighters' counts: %s", category_name(category),
            ok ? ("read, " + std::to_string(c.by_fighter.size()) + " fighters, " + std::to_string(c.total) + " in all").c_str()
               : ("none (HTTP " + std::to_string(status) + (c.retry ? "; asked again later" : "; not asked again") + ")").c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counts[category] = std::move(c);
}

}  // namespace menu_internal
}  // namespace host::mods_menu
