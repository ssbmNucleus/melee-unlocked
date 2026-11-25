// The Mods menu's Effects ("Particles") by move: a fighter's effect file is its moves (cosmetic_mods.h,
// "an effect file's slots": Falcon Punch, Falcon Kick, ...), each shown with the effect it uses now.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Browse: the fighters (as Characters), then a fighter's moves: "All effects" first, then each move
// as "Falcon Punch - Honduran Falcon Punch" (or "- Standard"). A on a move is its effects: each installed
// effect that changes it, "Standard" (the disc's own) and "Get more" (the fighter's posts from the site,
// as before; B there comes back to the move). A fighter whose effects have no moves known (Mr. Game &
// Watch) and "All fighters" are the posts, as before. These lists are the fighters' list's kind (flag 1:
// every row's A is change(9) with its row) with a filter (flag 4: B is change(10), one step back), and
// their title says which list it is (the game's words for its rows: mu_mods_category_effects.c).
// Manage: the fighters with effects installed, then a fighter's moves, where A steps the move to the next
// installed effect that changes it, then Standard; and "All installed": every effect, each marked "In
// use" or "Not used", A to remove one.
// An install that would take a move from another effect does not use it yet: the card says what it
// would override, one line per effect, and "A: Replace" (the game asks once more: "Press A again to
// replace it."); B keeps the effects as they are, the new one installed and not used.

#include "effect_slots.h"
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {
namespace {

using host::cosmetics::EffectSlots;

// The fighters' effect files that have moves (effect_slots.cpp's table): clones share one.
const char* table_file(const std::string& fighter) {
  static const std::map<std::string, const char*> files = {
      {"bowser", "EfKpData.dat"},     {"captainfalcon", "EfCaData.dat"}, {"donkeykong", "EfDkData.dat"},
      {"drmario", "EfMrData.dat"},    {"falco", "EfFxData.dat"},         {"fox", "EfFxData.dat"},
      {"ganondorf", "EfGnData.dat"},  {"iceclimbers", "EfIcData.dat"},   {"jigglypuff", "EfPrData.dat"},
      {"kirby", "EfKbData.dat"},      {"link", "EfLkData.dat"},          {"luigi", "EfLgData.dat"},
      {"mario", "EfMrData.dat"},      {"marth", "EfMsData.dat"},         {"mewtwo", "EfMtData.dat"},
      {"ness", "EfNsData.dat"},       {"peach", "EfPeData.dat"},         {"pichu", "EfPkData.dat"},
      {"pikachu", "EfPkData.dat"},    {"roy", "EfFeData.dat"},           {"samus", "EfSsData.dat"},
      {"sheik", "EfZdData.dat"},      {"yoshi", "EfYsData.dat"},         {"younglink", "EfLkData.dat"},
      {"zelda", "EfZdData.dat"}};
  const auto it = files.find(fighter_key(fighter));
  return it == files.end() ? nullptr : it->second;
}

std::string file_of_filter(int filter) {
  if (filter <= 0 || filter >= (int)characters().size()) return {};
  const char* f = table_file(characters()[(size_t)filter]);
  return f ? f : std::string();
}

// "Fox and Falco": the fighters of a file, in roster order.
std::string file_label(const std::string& target) {
  std::vector<std::pair<int, std::string>> who;
  for (size_t i = 1; i < characters().size(); ++i)
    if (const char* f = table_file(characters()[i]); f && lower(f) == lower(target))
      who.emplace_back(roster_index(characters()[i]), characters()[i]);
  std::sort(who.begin(), who.end());
  std::string out;
  for (size_t i = 0; i < who.size(); ++i) out += (i ? " and " : "") + who[i].second;
  return out.empty() ? target : out;
}

// What the lists show of one file: its moves and their effects now, and the installed effects' names.
struct FxView {
  bool ok = false;
  std::string target, error;
  EffectSlots slots;
  std::map<std::string, std::string> names;
};

FxView make_view(const std::string& target) {
  FxView v;
  v.target = target;
  v.ok = host::cosmetics::effect_slots_of(target, &v.slots, &v.error);
  for (const auto& a : host::cosmetics::assets())
    if (a.kind == "effect_visual" && a.available) v.names[a.id] = a.name;
  return v;
}

// Browse: 0 the fighters (g_picking) or a fighter's posts; 1 a fighter's moves; 2 a move's effects.
// Manage: 0 the fighters with effects; 1 a fighter's moves; 3 every effect installed (Manage's cards).
enum Level { kFxNone = 0, kFxMoves = 1, kFxSources = 2, kFxInstalled = 3 };
int g_level = kFxNone;
int g_move = 0;            // the moves' row chosen (0 "All effects")
int g_start = 0;           // the row the next list built starts on
bool g_from_more = false;  // Browse: the posts were opened by "Get more": B is the move's effects again
int g_busy = 0;            // changes on their way (the install queue)
FxView g_view;
std::vector<std::string> g_files;   // Manage: the files with effects installed, in roster order

// An install that would take moves from other effects: held, not used, until A (the notice).
struct Held { std::string asset_id, target; std::vector<std::string> lines; };
std::map<int, Held> g_held;   // by file id

std::string name_of(const FxView& v, const std::string& id) {
  const auto it = v.names.find(id);
  return it == v.names.end() ? id : it->second;
}

// "All effects - ": the effect served whole, the one effect of every move changed, "Standard" or "Mixed".
std::string all_source(const FxView& v) {
  if (!v.slots.whole_id.empty()) return name_of(v, v.slots.whole_id);
  std::string one;
  bool standard = false, mixed = false;
  for (const auto& s : v.slots.slots) {
    if (s.source_id.empty()) { standard = true; continue; }
    if (!one.empty() && one != s.source_id) mixed = true;
    one = s.source_id;
  }
  if (one.empty()) return "Standard";
  return mixed || standard ? "Mixed" : name_of(v, one);
}

struct FxRow {
  std::string name, right, slot, id;
  bool more = false;
};

// "Move - Source" in a row's 39 characters: a long source cut at its end ("Shared effects - American
// Falcon..."), so the move stays whole (row_name would cut the middle, the dash with it).
std::string move_and_source(const std::string& move, const std::string& source) {
  const std::string both = move + " - " + source;
  const size_t room = 39 > move.size() + 3 ? 39 - move.size() - 3 : 0;
  if (both.size() <= 39 || room < 8) return both;
  std::string start = source.substr(0, room - 3);
  const size_t cut = start.rfind(' ');
  if (cut != std::string::npos && cut >= start.size() / 2) start.resize(cut);
  while (!start.empty() && (start.back() == ' ' || start.back() == '-' || start.back() == ',')) start.pop_back();
  return move + " - " + start + "...";
}

std::vector<FxRow> move_rows(const FxView& v) {
  std::vector<FxRow> rows;
  if (!v.ok) return rows;
  rows.push_back({move_and_source("All effects", all_source(v)),
                  v.slots.effects.empty() ? "" : things(kEffects, (int)v.slots.effects.size()),
                  host::cosmetics::effect_slots::kAllSlots, ""});
  for (const auto& s : v.slots.slots) {
    const std::string source = !v.slots.whole_id.empty() ? name_of(v, v.slots.whole_id)
                             : s.source_id.empty()       ? "Standard"
                                                         : name_of(v, s.source_id);
    rows.push_back({move_and_source(s.name, source), s.choices.empty() ? "" : things(kEffects, (int)s.choices.size()), s.name, ""});
  }
  return rows;
}

// A move's effects: each installed one that changes it ("In use" on the one it shows), "Standard",
// "Get more" (Browse). "All effects": every installed effect of the file.
std::vector<FxRow> source_rows(const FxView& v, int move, bool more) {
  std::vector<FxRow> rows;
  if (!v.ok) return rows;
  const auto& all = host::cosmetics::effect_slots::kAllSlots;
  if (move <= 0) {
    bool any = !v.slots.whole_id.empty();
    for (const auto& s : v.slots.slots) any = any || !s.source_id.empty();
    for (const auto& id : v.slots.effects) {
      if (!v.names.count(id)) continue;
      int mine = 0, others = 0;
      for (const auto& s : v.slots.slots)
        if (!s.source_id.empty()) ++(s.source_id == id ? mine : others);
      const std::string right = v.slots.whole_id == id ? "In use" : mine > 0 && others == 0 ? "In use" : mine > 0 ? "Part used" : "";
      rows.push_back({name_of(v, id), right, all, id});
    }
    rows.push_back({"Standard", any ? "" : "In use", all, ""});
  } else if ((size_t)move <= v.slots.slots.size()) {
    const auto& s = v.slots.slots[(size_t)move - 1];
    const bool whole = !v.slots.whole_id.empty();
    for (const auto& id : s.choices)
      if (v.names.count(id)) rows.push_back({name_of(v, id), !whole && s.source_id == id ? "In use" : "", s.name, id});
    rows.push_back({"Standard", !whole && s.source_id.empty() ? "In use" : "", s.name, ""});
  }
  if (more) {
    FxRow get;
    get.name = "Get more";
    get.more = true;
    rows.push_back(std::move(get));
  }
  return rows;
}

// Manage: the files with effects installed that have moves, in roster order.
std::vector<std::string> files_installed() {
  std::vector<std::pair<int, std::string>> found;
  for (const auto& a : host::cosmetics::assets()) {
    if (a.kind != "effect_visual" || a.source == "disc") continue;
    bool known = false;
    int order = 100;
    for (size_t i = 1; i < characters().size(); ++i)
      if (const char* f = table_file(characters()[i]); f && lower(f) == lower(a.target_path)) {
        known = true;
        order = std::min(order, roster_index(characters()[i]));
      }
    if (known && std::none_of(found.begin(), found.end(), [&](const auto& f) { return lower(f.second) == lower(a.target_path); }))
      found.emplace_back(order, a.target_path);
  }
  std::sort(found.begin(), found.end());
  std::vector<std::string> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

int effects_of_file(const std::string& target) {
  int n = 0;
  for (const auto& a : host::cosmetics::assets())
    if (a.kind == "effect_visual" && a.source != "disc" && lower(a.target_path) == lower(target)) ++n;
  return n;
}

// A change of one move (or "All effects") on the install queue: the file composed again, then published.
void queue_set_locked(const std::string& target, const std::string& slot, const std::string& id) {
  Job job;
  job.kind = Job::kEffectSet;
  job.key = target;
  job.fx_slot = slot;
  job.card.asset_id = id;
  ++g_busy;
  queue_install_locked(std::move(job));
}

void head_of(std::vector<uint8_t>& reply, int status, int rows, int start, int flags, const std::string& filter,
             const std::string& message, const char* title) {
  reply.push_back((uint8_t)status);
  put_u16(reply, rows);
  put_u16(reply, std::clamp(start, 0, std::max(0, rows - 1)));
  put_u16(reply, 0);
  reply.push_back((uint8_t)flags);
  put(reply, drawable(filter, 19), 20);
  put(reply, drawable(message, 47), 48);
  put(reply, title, 48);
}

// The bar's words where the game shows the message alone (a list of fighter rows): what the list is
// of, then what to do ("Captain Falcon  -  Choose a move"), or the words alone when both do not fit
// the 36 characters the bar shows in full (mu_mods_gallery_draw.c BAR_ROOM).
std::string bar_words(const std::string& of, const std::string& words) {
  const std::string both = of + "  -  " + words;
  return !of.empty() && both.size() <= 36 ? both : words;
}

void rows_of(std::vector<uint8_t>& reply, const std::vector<FxRow>& rows, int first, int count) {
  count = std::clamp(count, 0, 60);
  for (int i = std::max(first, 0); i < first + count && i < (int)rows.size(); ++i) {
    put(reply, row_name(rows[(size_t)i].name), 40);
    put(reply, drawable(rows[(size_t)i].right, 11), 12);
    reply.push_back(0);
  }
}

}  // namespace

// The level-markers the game reads (head's title; mu_mods_category_effects.c).
constexpr const char* kTitleMoves = "moves";
constexpr const char* kTitleSources = "sources";
constexpr const char* kTitleFiles = "files";
constexpr const char* kTitleCycle = "cycle";
constexpr const char* kTitleInstalled = "All installed";

bool effects_open_locked() {
  g_level = kFxNone;
  g_from_more = false;
  g_start = 0;
  if (g_category != kEffects || g_mode != 1) return false;
  // Manage: the fighters with effects installed first.
  g_picking = true;
  g_files = files_installed();
  host::log("mods menu: Manage, Effects, %zu fighter file(s) with effects", g_files.size());
  return true;
}

int effects_change_locked(int op, int row) {
  if (g_category != kEffects) return -1;
  if (g_mode == 0) {
    if (g_picking && g_level == kFxNone) {
      // A fighter: its moves, when its effects have them (else its posts, as before).
      const auto& order = roster_filters();
      if (op != 9 || row < 0 || row >= (int)order.size()) return -1;
      const std::string target = file_of_filter(order[(size_t)row]);
      if (target.empty()) return -1;
      FxView v = make_view(target);
      if (!v.ok || v.slots.slots.empty()) {
        host::log("mods menu: %s: no moves (%s): its posts", target.c_str(), v.error.c_str());
        return -1;
      }
      g_filter = g_last_fighter = order[(size_t)row];
      g_view = std::move(v);
      g_level = kFxMoves;
      g_start = 0;
      host::log("mods menu: fighter %s: the moves of %s (%zu)", characters()[(size_t)g_filter].c_str(), target.c_str(),
                g_view.slots.slots.size());
      return 1;
    }
    if (g_picking && g_level == kFxMoves) {
      if (op == 10) {
        g_level = kFxNone;   // the fighters, on this one
        host::log("mods menu: back to the fighters");
        return 1;
      }
      const auto rows = move_rows(g_view);
      if (op != 9 || row < 0 || row >= (int)rows.size()) return 0;
      g_move = row;
      g_level = kFxSources;
      g_start = 0;
      const auto sources = source_rows(g_view, g_move, true);
      for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].right == "In use") g_start = (int)i;
      host::log("mods menu: move \"%s\" of %s", rows[(size_t)row].slot.c_str(), g_view.target.c_str());
      return 1;
    }
    if (g_picking && g_level == kFxSources) {
      if (op == 10) {
        g_level = kFxMoves;
        g_start = g_move;
        return 1;
      }
      const auto rows = source_rows(g_view, g_move, true);
      if (op != 9 || row < 0 || row >= (int)rows.size()) return 0;
      const FxRow& r = rows[(size_t)row];
      g_start = row;
      if (r.more) {
        // The fighter's posts (as before); B comes back here.
        g_picking = false;
        g_level = kFxNone;
        g_from_more = true;
        g_page = 0;
        g_more = -1;
        g_pack = 0;
        g_pack_page = 0;
        host::log("mods menu: get more for %s", characters()[(size_t)g_filter].c_str());
        return 1;
      }
      if (g_blocked.load() || g_busy > 0 || r.right == "In use") return 0;
      host::log("mods menu: %s of %s: %s", r.slot.c_str(), g_view.target.c_str(), r.id.empty() ? "Standard" : name_of(g_view, r.id).c_str());
      queue_set_locked(g_view.target, r.slot, r.id);
      return 1;
    }
    if (!g_picking && g_from_more && op == 10 && g_pack == 0) {
      // B in the posts "Get more" opened: the move's effects again (the ones installed since among them),
      // unless L and R went to another fighter's file.
      g_from_more = false;
      if (lower(file_of_filter(g_filter)) != lower(g_view.target)) return -1;
      g_view = make_view(g_view.target);
      g_picking = true;
      g_level = g_view.ok ? kFxSources : kFxNone;
      g_query.clear();
      g_query_id = 0;
      g_start = std::max(0, (int)source_rows(g_view, g_move, true).size() - 1);
      host::log("mods menu: back to the effects of a move");
      return 1;
    }
    return -1;
  }
  // Manage.
  if (g_level == kFxNone) {
    if (op != 9 || row < 0 || row > (int)g_files.size()) return 0;
    if (row == (int)g_files.size()) {
      // Every effect installed (Manage's cards, as before): A removes one.
      g_level = kFxInstalled;
      g_picking = false;
      g_filter = 0;
      forget_pages_locked();
      host::log("mods menu: Manage Effects, all installed");
      return 1;
    }
    FxView v = make_view(g_files[(size_t)row]);
    if (!v.ok) {
      host::log("mods menu: %s: no moves (%s)", g_files[(size_t)row].c_str(), v.error.c_str());
      return 0;
    }
    g_view = std::move(v);
    g_level = kFxMoves;
    g_start = 0;
    host::log("mods menu: Manage, the moves of %s", g_view.target.c_str());
    return 1;
  }
  if (op == 10) {
    const std::string was = g_level == kFxMoves ? g_view.target : std::string();
    g_level = kFxNone;
    g_picking = true;
    g_files = files_installed();
    g_start = (int)g_files.size();
    for (size_t i = 0; i < g_files.size(); ++i)
      if (lower(g_files[i]) == lower(was)) g_start = (int)i;
    return 1;
  }
  if (g_level == kFxInstalled) return op == 2 || op == 3 ? 0 : -1;
  // A fighter's moves: A steps a move to its next effect, then Standard, then its first again.
  const auto rows = move_rows(g_view);
  if (op != 9 || row < 0 || row >= (int)rows.size() || g_blocked.load() || g_busy > 0) return 0;
  const FxRow& r = rows[(size_t)row];
  const auto sources = source_rows(g_view, row, false);
  if (sources.size() < 2) return 0;   // Standard alone: nothing to step to
  size_t now = sources.size();
  for (size_t i = 0; i < sources.size(); ++i)
    if (sources[i].right == "In use") now = i;
  const FxRow& next = sources[now == sources.size() ? 0 : (now + 1) % sources.size()];
  g_start = row;
  host::log("mods menu: Manage, %s of %s: %s", r.slot.c_str(), g_view.target.c_str(),
            next.id.empty() ? "Standard" : name_of(g_view, next.id).c_str());
  queue_set_locked(g_view.target, next.slot, next.id);
  return 1;
}

bool effects_list_locked(int first, int count, std::vector<uint8_t>& reply) {
  if (g_category != kEffects) return false;
  const std::string busy = g_busy > 0 ? "Changing..." : "";
  if (g_mode == 0) {
    if (!g_picking || g_level == kFxNone) return false;   // the fighters, a fighter's posts: as before
    const std::string fighter = g_filter > 0 && g_filter < (int)characters().size() ? characters()[(size_t)g_filter] : "";
    if (g_level == kFxMoves) {
      const auto rows = move_rows(g_view);
      head_of(reply, kReady, (int)rows.size(), g_start, 1 | 4, fighter_label(fighter),
              bar_words(fighter_label(fighter), !busy.empty() ? busy : "Choose a move"), kTitleMoves);
      rows_of(reply, rows, first, count);
      return true;
    }
    const auto moves = move_rows(g_view);
    const auto rows = source_rows(g_view, g_move, true);
    const std::string move = g_move > 0 && g_move < (int)moves.size() ? moves[(size_t)g_move].slot : "All effects";
    head_of(reply, kReady, (int)rows.size(), g_start, 1 | 4, move,
            bar_words(move, !busy.empty() ? busy : "Choose its effect"), kTitleSources);
    rows_of(reply, rows, first, count);
    return true;
  }
  if (g_level == kFxNone) {
    std::vector<FxRow> rows;
    int all = 0;
    for (const auto& f : g_files) {
      const int n = effects_of_file(f);
      all += n;
      rows.push_back({file_label(f), things(kEffects, n), "", ""});
    }
    for (const auto& a : host::cosmetics::assets())
      if (a.kind == "effect_visual" && a.source != "disc" &&
          std::none_of(g_files.begin(), g_files.end(), [&](const std::string& f) { return lower(f) == lower(a.target_path); }))
        ++all;
    rows.push_back({"All installed", std::to_string(all), "", ""});
    head_of(reply, kReady, (int)rows.size(), g_start, 1, "", g_files.empty() ? "No moves to choose" : "Choose a fighter",
            kTitleFiles);
    rows_of(reply, rows, first, count);
    return true;
  }
  if (g_level == kFxMoves) {
    const auto rows = move_rows(g_view);
    head_of(reply, kReady, (int)rows.size(), g_start, 1 | 4, file_label(g_view.target),
            bar_words(file_label(g_view.target), !busy.empty() ? busy : "A: Change"), kTitleCycle);
    rows_of(reply, rows, first, count);
    return true;
  }
  // Every effect installed: Manage's cards, each marked in use or not (B: the fighters).
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  const int n = (int)rows.size();
  std::map<std::string, bool> used;
  for (const auto& a : host::cosmetics::assets())
    if (a.kind == "effect_visual") used[a.id] = a.selected;
  std::string message = page.status == kLoading ? "Loading..." : page.message;
  if (page.status == kReady && n > 0) message = thousands(n) + " installed";
  head_of(reply, page.status, n, 0, page.slow ? 16 : 0, "", message, kTitleInstalled);
  count = std::clamp(count, 0, 60);
  for (int i = std::max(first, 0); i < first + count && i < n; ++i) {
    const Card& c = *rows[(size_t)i].card;
    const auto u = used.find(c.asset_id);
    put(reply, row_name(c.title), 40);
    put(reply, u == used.end() ? "" : u->second ? "In use" : "Not used", 12);
    reply.push_back((uint8_t)state_locked(c));
  }
  return true;
}

// ---- installs: an effect used at once, or held for its notice ----

// After an install (the install worker, g_mutex not held): true when the effect is in use now (its file
// to publish again); false when it would take a move from another effect (held: the card's notice) or
// could not be used.
bool effect_installed(const Card& card, const std::string& asset_id, std::string* target) {
  std::vector<std::string> slots;
  bool whole = false;
  std::vector<host::cosmetics::EffectTaken> taken;
  std::string error;
  for (const auto& a : host::cosmetics::assets())
    if (a.id == asset_id) *target = a.target_path;
  if (host::cosmetics::effect_takes(asset_id, &slots, &whole, &taken, &error) && !taken.empty()) {
    Held held;
    held.asset_id = asset_id;
    held.target = *target;
    for (const auto& t : taken) {
      std::string moves;
      for (const auto& s : t.slots) moves += (moves.empty() ? "" : ", ") + s;
      held.lines.push_back(drawable(t.name + " - " + moves, 120));
      host::log("mods menu: card %d would override %s (%s): held until A", card.file_id, t.name.c_str(), moves.c_str());
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_held[card.file_id] = std::move(held);
    return false;
  }
  if (!host::cosmetics::use_effect(asset_id, &error)) {
    host::log("mods menu: card %d: installed, not used: %s", card.file_id, error.c_str());
    return false;
  }
  host::log("mods menu: card %d: in use (%zu move(s)%s)", card.file_id, slots.size(), whole ? ", the whole file" : "");
  return true;
}

bool effect_held_locked(const Card& card) { return g_mode == 0 && g_held.count(card.file_id) != 0; }

// The viewer's words on a held card: "This will override", one line per effect it takes moves from.
bool effect_notice_locked(const Card& card, std::string* about) {
  const auto it = g_held.find(card.file_id);
  if (g_mode != 0 || it == g_held.end()) return false;
  std::string text = "This will override";
  for (const auto& line : it->second.lines) text += "\n" + line;
  text += "\n" + drawable(card.description, 399);
  if (text.size() > 399) text.resize(399);
  *about = text;
  return true;
}

// A on a held card (its notice asked twice by the game): it is used now.
bool effect_use_held_locked(const Card& card) {
  const auto it = g_held.find(card.file_id);
  if (g_mode != 0 || it == g_held.end() || g_blocked.load()) return false;
  if (const auto p = g_progress.find(card.file_id); p != g_progress.end() && p->second.state == kDownloading) return false;
  Job job;
  job.kind = Job::kEffectUse;
  job.card = card;
  job.card.asset_id = it->second.asset_id;
  g_progress[card.file_id] = {kDownloading, "", false};
  queue_install_locked(std::move(job));
  return true;
}

void effect_removed_locked(const std::string& asset_id) {
  for (auto it = g_held.begin(); it != g_held.end();) it = it->second.asset_id == asset_id ? g_held.erase(it) : std::next(it);
}

// The install worker's effect jobs.
void run_effect_job(const Job& job) {
  std::string error, target = job.key;
  bool ok = false;
  if (job.kind == Job::kEffectUse) {
    for (const auto& a : host::cosmetics::assets())
      if (a.id == job.card.asset_id) target = a.target_path;
    ok = host::cosmetics::use_effect(job.card.asset_id, &error);
    host::log("mods menu: card %d: %s%s", job.card.file_id, ok ? "used, as the notice said" : "not used: ", error.c_str());
  } else {
    ok = host::cosmetics::set_effect_slot(job.key, job.fx_slot, job.card.asset_id, &error);
    if (!ok) host::log("mods menu: %s of %s not changed: %s", job.fx_slot.c_str(), job.key.c_str(), error.c_str());
  }
  FxView view = make_view(target);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (ok && !target.empty()) g_republish.emplace_back(target, FileKind::Effect);
  if (job.kind == Job::kEffectUse) {
    Progress& p = g_progress[job.card.file_id];
    p.state = ok ? kInstalled : kFailed;
    p.message = ok ? "Installed and in use." : error;
    if (ok) g_held.erase(job.card.file_id);
  } else if (g_busy > 0) {
    --g_busy;
  }
  if (lower(g_view.target) == lower(target)) g_view = std::move(view);
}

}  // namespace menu_internal
}  // namespace host::mods_menu
