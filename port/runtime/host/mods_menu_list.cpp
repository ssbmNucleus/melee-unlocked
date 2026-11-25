// The Mods menu's list and viewer: page keys, the rows on screen, list(), detail() and pack_detail().
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

std::string list_key(int mode, int category, int filter) {
  return std::to_string(mode) + "/" + std::to_string(category) + "/" + std::to_string(filter) + "/" +
         std::to_string(mode == 0 ? g_query_id : 0);
}
std::string page_key(int mode, int category, int filter, int page) {
  return list_key(mode, category, filter) + "/" + std::to_string(page);
}
std::string pack_key(int category, int filter, int mod_id) {
  return std::to_string(category) + "/" + std::to_string(filter) + "/" + std::to_string(mod_id);
}
std::string card_key(const Card& c) {
  return c.asset_id.empty() ? "f" + std::to_string(c.file_id) : "a" + c.asset_id;
}

// The page on screen, fetched when it is not held. Pages are kept for the session (the site's own
// copies last ten minutes); "Try again" and a changed file read a page again. Locked.
Page& current_locked() {
  const std::string key = page_key(g_mode, g_category, g_filter, g_page);
  auto it = g_pages.find(key);
  if (it == g_pages.end()) {
    it = g_pages.emplace(key, Page{}).first;
    queue_page_locked(key);
  }
  return it->second;
}


// A post's own list from /cards?mod_id, when it was read; else null.
const Post* pack_list_locked(int mod_id) {
  const auto it = g_pack_lists.find(pack_key(g_category, g_filter, mod_id));
  return it != g_pack_lists.end() && it->second.status == kReady ? &it->second.post : nullptr;
}

const Post* pack_locked(const Page& page) {
  if (g_pack == 0 || g_mode != 0) return nullptr;
  for (const auto& post : page.posts)
    if (post.mod_id == g_pack) {
      const Post* full = pack_list_locked(post.mod_id);
      return full ? full : &post;
    }
  return nullptr;
}

// The skins a post's row says it has: its own list's once read, else what its row read.
int post_count_locked(const Post& post) {
  const Post* full = pack_list_locked(post.mod_id);
  return full ? full->count : post.count;
}

std::vector<ViewRow> rows_locked(const Page& page) {
  std::vector<ViewRow> rows;
  if (page.status != kReady) return rows;
  // Manage Stages, one of the six: "Vanilla" first (mods_menu_stages.cpp).
  if (g_mode == 1 && g_category == kStages && g_filter >= 1 && g_filter <= 6) rows.push_back({kRowInstallAll, nullptr, nullptr});
  if (g_mode == 1) {
    for (const auto& c : page.cards) rows.push_back({kRowCard, &c, nullptr});
    return rows;
  }
  if (const Post* pack = pack_locked(page)) {
    // (not Menus: one menu mod a screen, so installing every card of a pack would leave the last)
    if (kInstallAll && g_pack_page == 0 && g_category != kMenus) rows.push_back({kRowInstallAll, nullptr, pack});
    const size_t first = (size_t)g_pack_page * kPackPerPage;
    for (size_t i = first; i < pack->cards.size() && i < first + kPackPerPage; ++i)
      rows.push_back({kRowCard, &pack->cards[i], pack});
    return rows;
  }
  for (const auto& post : page.posts) {
    const bool one = post_count_locked(post) <= 1 && post.cards.size() == 1;
    rows.push_back({one ? kRowCard : kRowPack, one ? &post.cards[0] : nullptr, &post});
  }
  return rows;
}

// The rows on screen and the screen after them: the cursor's row `row` on line `line` of the list
// (kListLines show at once). Their pictures are asked for ahead of the viewer, the cursor's row first,
// then the rows below it, then those above; what was asked for ahead before and not begun is let go
// first. Not for the fighters' list, nor for a pack's row whose screenshots are not known (above).
void prefetch_screen_locked(int row, int line) {
  auto& jobs = g_picture_queue.jobs;
  for (auto it = jobs.begin(); it != jobs.end();) {
    if (!it->prefetch) { ++it; continue; }
    const auto pic = g_pictures.find(it->key);
    if (pic != g_pictures.end() && pic->second.status == kLoading && !pic->second.fetching) g_pictures.erase(pic);
    it = jobs.erase(it);
  }
  if (g_picking || g_blocked.load() || g_offline.load()) return;
  const auto found = g_pages.find(page_key(g_mode, g_category, g_filter, g_page));
  if (found == g_pages.end() || found->second.status != kReady) return;
  const auto rows = rows_locked(found->second);
  const int n = (int)rows.size();
  if (n == 0) return;
  row = std::clamp(row, 0, n - 1);
  const int first = std::max(0, row - std::clamp(line, 0, kListLines - 1));
  const int end = std::min(n, first + kListLines * kPrefetchScreens);
  const auto want = [&](int r) {
    const ViewRow& v = rows[(size_t)r];
    if (v.kind == kRowCard) want_row_picture_locked(0, v.card, v.post, true);
    else if (v.kind == kRowPack) want_row_picture_locked(1, nullptr, v.post, true);
  };
  for (int r = row; r < end; ++r) want(r);
  for (int r = row - 1; r >= first; --r) want(r);
}

int state_locked(const Card& card) {
  if (card.kind == "fighter") return fighter_state_locked(card);   // mods_menu_fighters.cpp
  // An effect installed and held for its notice: not in use yet, A (asked twice) uses it.
  if (effect_held_locked(card)) {
    const auto it = g_progress.find(card.file_id);
    return it != g_progress.end() && it->second.state == kDownloading ? kDownloading : kNotInstalled;
  }
  if (g_mode == 1) {
    const auto it = g_removals.find(card.asset_id);
    return it != g_removals.end() ? it->second.state : kInstalled;
  }
  const auto it = g_progress.find(card.file_id);
  if (it != g_progress.end() && it->second.state != kNotInstalled) {
    if (it->second.state == kInstalled && !g_installed.count(card.file_id)) return kNotInstalled;   // removed since
    return it->second.state;
  }
  return g_installed.count(card.file_id) ? kInstalled : kNotInstalled;
}

// A pack is "installed" when every one of its cards is.
int pack_state_locked(const Post& post) {
  for (const auto& c : post.cards)
    if (state_locked(c) != kInstalled) return kNotInstalled;
  return post.cards.empty() ? kNotInstalled : kInstalled;
}

// What the bar under a pack's list says while "Install all" runs and after.
std::string batch_words_locked(const Post& post) {
  if (g_batch.id == 0 || g_batch.mod_id != post.mod_id) return {};
  if (g_batch.running)
    return "Installing " + std::to_string(std::min(g_batch.done + g_batch.failed + 1, g_batch.total)) + " of " +
           std::to_string(g_batch.total) + "...";
  if (g_batch.total == 0) return "All were installed already.";
  if (g_batch.failed == 0) return "Installed " + things(g_category == kEffects ? kEffects : kCharacters, g_batch.done) + ".";
  return "Installed " + std::to_string(g_batch.done) + ", " + std::to_string(g_batch.failed) + " failed.";
}
// A list row's name (40 bytes; the game shows 32 characters and cuts a longer one in the middle): one
// longer than the row holds is cut here, at spaces where it can, its start and its last words around
// "...", so its end is not lost ("Ultra Long...Titles Squeeze").
std::string row_name(const std::string& text) {
  std::string all = drawable(text, 200);
  if (all.size() <= 39) return all;
  constexpr size_t kShown = 32, kEnd = 14;
  const size_t from = all.size() - kEnd, space = all.find(' ', from);
  std::string end = space != std::string::npos && space + 1 < all.size() ? all.substr(space + 1) : all.substr(from);
  const size_t room = kShown - 3 - end.size();
  std::string start = all.substr(0, room);
  const size_t cut = start.rfind(' ');
  if (cut != std::string::npos && cut >= room / 2) start.resize(cut);
  while (!start.empty() && (start.back() == ' ' || start.back() == '-' || start.back() == ',')) start.pop_back();
  return start + "..." + end;
}

}  // namespace menu_internal

void list(int first, int count, std::vector<uint8_t>& reply) {
  std::lock_guard<std::mutex> lock(g_mutex);
  reply.clear();
  if (g_blocked.load()) {
    reply.push_back(kError);
    put_u16(reply, 0); put_u16(reply, 0); put_u16(reply, 0);
    reply.push_back(0);
    put(reply, "", 20);
    put(reply, "Not during an online session.", 48);
    put(reply, "", 48);
    return;
  }
  g_last_opened.clear();   // (the list is on screen: the viewer's next card is logged)
  if (stages_list_locked(first, count, reply)) return;   // the six stages, or a stage's pool (mods_menu_stages.cpp)
  if (effects_list_locked(first, count, reply)) return;  // a fighter's moves, a move's effects (mods_menu_effect_slots.cpp)
  if (g_picking) {
    // The fighters: "All fighters" and each fighter in roster order, the one chosen last first under
    // the cursor (the u16 after the rows: the row to start on); the right column each fighter's count
    // ("All fighters": all of them), every row's or none: the site's counts (g_counts, one request)
    // once they are in, nothing while they are on their way or when the site has none.
    want_counts_locked(g_category);
    const auto counts = g_counts.find(g_category);
    const Counts* known = counts != g_counts.end() && counts->second.status == kReady ? &counts->second : nullptr;
    const char* word = g_category == kEffects ? "effect" : "skin";
    const auto& order = roster_filters();
    reply.push_back((uint8_t)kReady);
    put_u16(reply, (int)order.size());
    put_u16(reply, roster_row(g_filter));
    put_u16(reply, 0);
    reply.push_back((uint8_t)1);   // the fighters' list
    put(reply, "", 20);
    put(reply, "Choose a fighter", 48);
    put(reply, "", 48);
    count = std::clamp(count, 0, 60);
    for (int i = std::max(first, 0); i < first + count && i < (int)order.size(); ++i) {
      const int f = order[(size_t)i];
      std::string right;
      if (known && f > 0) {
        const auto n = known->by_fighter.find(fighter_key(characters()[(size_t)f]));
        right = row_counted(n != known->by_fighter.end() ? n->second : 0, word);
      } else if (known) {
        right = row_counted(known->total, word);
      }
      put(reply, f > 0 ? drawable(characters()[(size_t)f], 39) : "All fighters", 40);
      put(reply, drawable(right, 11), 12);
      reply.push_back(0);
    }
    return;
  }
  // Browse's next batch, once it is in, is the list (one batch longer: every row before stays where it
  // was); the shorter copies go.
  bool more_loading = false, more_failed = false;
  if (g_more >= 0) {
    const auto it = g_pages.find(page_key(g_mode, g_category, g_filter, g_more));
    if (it == g_pages.end()) {
      g_more = -1;
    } else if (it->second.status == kReady) {
      for (int p = 0; p < g_more; ++p) g_pages.erase(page_key(g_mode, g_category, g_filter, p));
      g_page = g_more;
      g_more = -1;
    } else {
      more_loading = it->second.status == kLoading;
      more_failed = it->second.status == kError;
    }
  }
  const Page& page = current_locked();
  const Post* pack = pack_locked(page);
  if (g_pack != 0 && page.status == kReady && !pack) g_pack = 0;   // the page was read again without it
  const auto rows = rows_locked(page);
  const int n = (int)rows.size();
  // One continuous list: no pages. `after`: more posts to read at its end (it does not wrap round
  // until its true end); a pack's list and Manage are whole.
  const bool after = !pack && g_mode == 0 && page.status == kReady && page.has_next;
  if (pack || !after) more_loading = more_failed = false;
  reply.push_back((uint8_t)page.status);
  put_u16(reply, n);
  put_u16(reply, 0);
  put_u16(reply, 0);
  reply.push_back((uint8_t)((after ? 2 : 0) | (g_mode == 0 && (by_fighter(g_category) || g_category == kStages) && !pack ? 4 : 0) | (pack ? 8 : 0) |
                            (page.slow ? 16 : 0) | (g_mode == 0 && !pack && !g_query.empty() ? 32 : 0) |
                            (more_loading ? 64 : 0) | (more_failed ? 128 : 0)));
  // The fighter shown, and the search's words when there is one.
  std::string filter = g_category == kStages ? (g_filter >= 1 && g_filter <= 6 ? kMenuStages[g_filter - 1].name : "All")
                       : g_filter > 0 ? characters()[g_filter] : "All";
  if (g_mode == 0 && !g_query.empty()) filter = (g_filter > 0 ? filter + " " : std::string()) + '"' + g_query + '"';
  put(reply, drawable(filter, 19), 20);
  std::string message = page.status == kLoading ? "Loading..." : page.message;
  if (g_mode == 0 && !g_query.empty() && message == "Nothing here yet.") message = "Nothing matches the search.";
  // A list with rows: how many it has in all, for the bar under it ("3,373 mods": the site's count of
  // posts; a fighter's list from /cards: of skins, which the site counts; Manage: what is installed).
  if (page.status == kReady && n > 0 && !pack && !(g_mode == 0 && g_category == kFighters)) {   // Fighters: its own words
    if (g_mode == 1) message = thousands(n) + " installed";
    else if (!page.has_next && page.posts.size() >= kMaxStreamPosts)   // the list ends there, the site's does not
      message = "First " + thousands((int)page.posts.size()) + " - the most a list keeps";
    else if (g_category == kMenus || g_category == kNewStages)   // the site counts every post; these list those they take
      message = thousands((int)page.posts.size()) + (page.has_next ? "+ mods" : " mods");
    else if (page.total > 0)
      message = counted(page.total, !page.from_cards ? "mod" : g_category == kEffects ? "effect" : "skin");
    else message = thousands((int)page.posts.size()) + (page.has_next ? "+ mods" : " mods");
  }
  if (pack) {
    message = batch_words_locked(*pack);
    if (message.empty()) message = things(g_category, (int)pack->cards.size());
  }
  put(reply, drawable(message, 47), 48);
  put(reply, pack ? drawable(pack->title, 47) : "", 48);
  count = std::clamp(count, 0, 60);
  for (int i = std::max(first, 0); i < first + count && i < n; ++i) {
    const ViewRow& r = rows[(size_t)i];
    std::string name, right;
    int state = kNotInstalled;
    if (r.kind == kRowInstallAll) {
      name = "Install all";
      if (g_batch.mod_id == r.post->mod_id && g_batch.running)
        right = std::to_string(g_batch.done + g_batch.failed) + " of " + std::to_string(g_batch.total);
      state = pack_state_locked(*r.post);
    } else if (r.kind == kRowPack) {
      // Its count: all its cards, the ones the site hides as duplicate portraits included.
      const Post* full = pack_list_locked(r.post->mod_id);
      name = r.post->title;
      right = things(g_category == kEffects ? kEffects : kCharacters, post_count_locked(*r.post));
      state = pack_state_locked(full ? *full : *r.post);
    } else if (g_mode == 1) {
      name = r.card->title;
      right = climber_of(*r.card) ? "ICs" : short_fighter(r.card->character);
      state = state_locked(*r.card);
    } else {
      // A single card's row has nothing at its right (its downloads are the viewer's): the box there
      // is for a pack's count and for "Installed".
      name = pack ? r.card->label : r.card->title;
      state = state_locked(*r.card);
      if (state == kNotInstalled && effect_refused_locked(*r.card)) right = "Unusable";   // (the viewer says why)
    }
    put(reply, row_name(name), 40);
    put(reply, drawable(right, 11), 12);
    reply.push_back((uint8_t)(state | (r.kind == kRowPack ? 0x40 : r.kind == kRowInstallAll ? 0x80 : 0)));
  }
}

namespace menu_internal {
// A name for the viewer (48 bytes): whole when it fits, else cut at a space with "..." after it (the
// game puts a long one on two lines).
std::string viewer_name(const std::string& text) {
  std::string all = drawable(text, 200);
  if (all.size() <= 47) return all;
  size_t cut = all.rfind(' ', 44);
  if (cut == std::string::npos || cut < 24) cut = 44;
  all.resize(cut);
  while (!all.empty() && (all.back() == ' ' || all.back() == '-' || all.back() == ',')) all.pop_back();
  return all + "...";
}
}  // namespace menu_internal

void detail(int row, std::vector<uint8_t>& reply) {
  std::lock_guard<std::mutex> lock(g_mutex);
  reply.clear();
  if (g_picking) { reply.push_back(0xFF); return; }   // the fighters' rows are not cards
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  if (row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowCard) { reply.push_back(0xFF); return; }
  const Card* c = rows[(size_t)row].card;
  const int state = state_locked(*c);
  // Its picture: first in line when it is not held yet (the rows near it may be on their way already:
  // prefetch), and the time from this opening to its hand-over logged (picture()).
  Picture* pic = want_row_picture_locked(0, c, rows[(size_t)row].post, false);
  if (pic && card_key(*c) != g_last_opened) {
    pic->viewed = std::chrono::steady_clock::now();
    pic->shown_logged = false;
  }
  reply.push_back((uint8_t)state);
  reply.push_back((uint8_t)(pic ? pic->status : kLoading));
  put_u32(reply, pic ? pic->generation : 0);
  // A post of /mods with one card: what /mods does not say of it (fetch_card_info), asked for once.
  const ViewRow& viewed = rows[(size_t)row];
  if (g_mode == 0 && !page.from_cards && g_pack == 0 && viewed.post && viewed.post->cards.size() == 1 && g_category != kFighters)
    queue_card_info_locked(*c, g_category);
  // The viewer's lines: the name, what the card is, who made it and what A does now. In a pack the
  // name is the card's row ("C. Falcon - Red - HALO": its colour first), and the next line its file's
  // own name when the pack has another card of that fighter and colour, else the pack's title.
  const bool in_pack = g_mode == 0 && !c->label.empty();
  const std::string name = in_pack ? c->label : c->title;
  const std::string stem = stem_of(*c);
  std::string what;
  if (g_category == 0) {
    const std::string who = climber_of(*c) ? "Ice Climbers" : c->character;
    std::string colour = colour_text(*c);
    // A card whose colour only the site says, and may be another file's: its own file's name ("PlMrSc").
    const bool unknown = colour.empty() && c->shares_portrait;
    if (unknown) colour = stem;
    if (in_pack && !unknown) what = c->same_colour ? "File: " + stem : c->title;
    else what = who.empty() ? "Character skin" : who + (colour.empty() ? "" : " - " + colour);
  } else if (g_category == kEffects) {
    // Whose effects ("Fox effects"; the common ones "Common effects"), and in a pack as a skin's.
    if (in_pack) what = c->same_colour ? "File: " + stem : c->title;
    else what = c->character.empty() ? "Effects" : c->character + " effects";
  } else if (g_category == kFighters) {
    what = fighter_what(*c);   // "New fighter - Fox's moves" (mods_menu_fighters.cpp)
  } else if (g_category == kMenus) {
    what = menu_what(*c, in_pack);   // "Character select screen" (mods_menu_menus.cpp)
  } else if (g_category == kNewStages) {
    what = new_stage_what(*c);   // "Built on Battlefield" (mods_menu_new_stages.cpp)
  } else {
    if (in_pack) what = c->same_colour ? "File: " + stem : c->title;
    else what = c->stage.empty() ? "Stage skin" : c->stage;
  }
  if (card_key(*c) != g_last_opened) {
    g_last_opened = card_key(*c);
    host::log("mods menu: row %d \"%s\" opened: card %d of mod %d, file %s%s%s", row, drawable(name, 60).c_str(),
              c->file_id, c->mod_id, c->file_name.empty() ? "?" : c->file_name.substr(0, 120).c_str(),
              colour_text(*c).empty() ? "" : ", colour ", colour_text(*c).c_str());
  }
  std::string line, note;
  bool slow = false;
  if (g_mode == 1) {
    if (const auto it = g_removals.find(c->asset_id); it != g_removals.end()) note = it->second.message;
  } else if (const auto it = g_progress.find(c->file_id); it != g_progress.end()) {
    note = it->second.message;
    slow = it->second.slow;
  }
  if (g_mode == 1) {
    line = state == kRemoving ? "Removing..." : state == kRemoved ? "Removed." : state == kFailed ? "Not removed: " + note : "A: Remove";
  } else {
    // A failure: its reason (the game shows it above the description, with "A: Try again" below).
    line = state == kDownloading ? "Downloading..." : state == kInstalled ? (note.empty() ? "Installed." : note)
         : state == kFailed ? (slow ? std::string("SSBM Nucleus is slow right now.") : "Failed: " + one_line(note, 36))
         : "A: Download and install";
  }
  // Menus: a card for a screen another menu mod is installed for says so; the game's first A then asks
  // (the last byte of the reply). The line is "A: Replace" and the mod it replaces opens the description:
  // a name after "A: Replace" did not fit the line's box (the font squeezed it).
  std::string replaces, replaces_words;
  if (g_mode == 0 && g_category == kMenus && (state == kNotInstalled || state == kFailed)) {
    replaces = menu_replaces(*c);
    if (!replaces.empty() && state == kNotInstalled) {
      line = "A: Replace";
      replaces_words = "Replaces " + replaces + ". ";
    }
  }
  // Effects: one installed and held, as it would override moves of others: "This will override" and a
  // line per effect ("Honduran Falcon Punch - Falcon Punch") open the description; the game's first A
  // asks again ("Press A again to replace it."), the second uses it (mods_menu_effect_slots.cpp).
  // An effect the effect checks refused (its file broken, or no effect): not installable, its reason first.
  const std::string* refused = g_mode == 0 && state == kNotInstalled ? effect_refused_locked(*c) : nullptr;
  if (refused) line = "Cannot be installed.";
  std::string notice;
  if (state == kNotInstalled && !refused && effect_notice_locked(*c, &notice)) line = "A: Replace";
  // Credit: who made it (the site's `creator`), else the account that posted it.
  const std::string creator = c->creator.empty() ? c->author : c->creator;
  const std::string by = creator.empty() ? "" : "by " + creator;
  put(reply, viewer_name(name), 48);
  put(reply, drawable(what, 47), 48);
  put(reply, drawable(by, 47), 48);
  // Manage: the count kept at install (none for a skin installed before details were kept).
  put(reply, g_mode == 1 && !c->saved ? "" : drawable(thousands(c->downloads) + " downloads", 47), 48);
  put(reply, drawable(line, 47), 48);
  // A portrait (or the stock icon beside it) whose address another file of its post has too
  // (judge_pictures) may not be this one's: said before the description.
  const bool shared = c->withhold_csp || (c->withhold_stock && c->kind == "character_costume");
  const std::string caveat = (refused ? *refused + " " : std::string()) + (shared ? "Picture may differ. " : "");
  // (the notice's lines are each drawable already: its line ends stay, the game breaks its lines there)
  put(reply, !notice.empty() ? notice : drawable(replaces_words + caveat + c->description, 399), 400);
  // The card's file id: A sends it back, and a row that is no longer this card is refused (action).
  put_u32(reply, (uint32_t)c->file_id);
  // 1: the first A only asks ("Press A again to replace.": another menu mod has this card's screen, or
  // an effect would override another's moves).
  reply.push_back(replaces.empty() && notice.empty() ? 0 : 1);
}

void pack_detail(int row, std::vector<uint8_t>& reply) {
  std::lock_guard<std::mutex> lock(g_mutex);
  reply.clear();
  if (g_picking) { reply.push_back(0xFF); return; }
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  if (row < 0 || row >= (int)rows.size() || rows[(size_t)row].kind != kRowPack || !rows[(size_t)row].post) {
    reply.push_back(0xFF);
    return;
  }
  const Post* post = rows[(size_t)row].post;
  const Post* full = pack_list_locked(post->mod_id);
  Picture* pic = want_row_picture_locked(1, nullptr, post, false);
  const std::string key = post_picture_key(*post);
  if (key != g_last_opened) {
    g_last_opened = key;
    if (pic) {
      pic->viewed = std::chrono::steady_clock::now();
      pic->shown_logged = false;
    }
    host::log("mods menu: row %d pack \"%s\" opened: mod %d, %d in it, %s", row, drawable(post->title, 60).c_str(),
              post->mod_id, post_count_locked(*post),
              post->shots_known ? (std::to_string(post->shots.size()) + " screenshots listed").c_str()
                                : "its screenshots not known yet");
  }
  const std::string creator = post->creator.empty() ? post->author : post->creator;
  const std::string& description = !post->description.empty() || !full ? post->description : full->description;
  reply.push_back(1);
  reply.push_back((uint8_t)(pic ? pic->status : kLoading));
  put_u32(reply, pic ? pic->generation : 0);
  put(reply, viewer_name(post->title), 48);
  put(reply, creator.empty() ? "" : drawable("by " + creator, 47), 48);
  put(reply, drawable(thousands(post->downloads) + " downloads", 47), 48);
  put(reply, drawable(things(g_category == kEffects ? kEffects : kCharacters, post_count_locked(*post)), 47), 48);
  put(reply, drawable(description, 399), 400);
}

}  // namespace host::mods_menu
