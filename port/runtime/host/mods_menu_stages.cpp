// The Mods menu's Stages (the category menu's second row); shared pieces in mods_menu_internal.h.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Browse Stages and Manage Stages start with the six legal stages in the stage select's order, as the
// fighters' list does for Characters: the same reply (flag 1) and change ops (9 a row, 10 back to the six,
// 2 and 3 the stage before or after, as L and R step through fighters). The stage is g_filter, 1 to 6.
// Browse: a stage is its posts from the site's /mods with its `stage` filter, one row each, as Characters
// lists them, each post narrowed to that stage's files. A Stadium post with one base file and each of the
// four transformations once installs the four with its base (Card::partners) and records them as its set
// (cosmetics::set_stadium_set). Manage: a stage is its pool (cosmetics::stage_pools): "Vanilla" first (the
// "Install all" row's kind, its box On or Off, A switches it: op 7), then each skin installed for it, which
// the viewer offers to remove with its transformations. Manage also has "Other stages" after the six when
// skins of other stages were installed before: those listed as Manage listed them, to be removed.

#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {
// (MenuStage and kMenuStages, the six, are in mods_menu_internal.h: the list shows their names.)
constexpr int kMenuStageCount = (int)(sizeof kMenuStages / sizeof kMenuStages[0]);
constexpr int kOtherStages = kMenuStageCount + 1;   // Manage: skins of the other stages
int g_last_stage = 1;   // the stage the six open on, for the session

const MenuStage* menu_stage(int filter) {
  return filter >= 1 && filter <= kMenuStageCount ? &kMenuStages[filter - 1] : nullptr;
}

std::string stage_param(int category, int filter) {
  const MenuStage* s = category == kStages ? menu_stage(filter) : nullptr;
  return s ? "&stage=" + encode(s->site) : std::string();
}

// Letters and digits, lower case: "Pokemon Stadium" and "Pokémon Stadium" and "pokemon-stadium" alike.
std::string stage_word(const std::string& text) {
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

// A stage file's name with Stadium's English .usd read as its .dat ("GrPs.usd" -> "grps.dat").
std::string stage_file_key(const std::string& name) {
  std::string base = lower(base_of(name));
  if (base.size() > 4 && base.compare(base.size() - 4, 4, ".usd") == 0) base = base.substr(0, base.size() - 4) + ".dat";
  return base;
}

int stadium_form_of(const std::string& slot) {
  const std::string k = lower(slot);
  return k.size() == 9 && k.compare(0, 4, "grps") == 0 && k[4] >= '1' && k[4] <= '4' && k.compare(5, 4, ".dat") == 0 ? k[4] - '1' : -1;
}

// The stage file a stage card replaces, as its install would put it (mod_browser::install_file): the
// site's stage_file; the file's own name when it is a disc file's ("GrNBa.dat", "GrPs.usd", "GrPs2.dat");
// else the stage its name starts with ("GrNLaWaffle.dat"), or its post's stage.
std::string card_slot(const Card& c) {
  if (!c.stage_file.empty()) return c.stage_file;
  static const std::vector<host::cosmetics::StageSlot> slots = host::cosmetics::stage_slots();
  const std::string base = stage_file_key(c.file_name);
  for (const auto& s : slots)
    if (lower(s.target_path) == base) return s.target_path;
  return host::mod_browser::stage_slot_for(base_of(c.file_name), c.stage);
}

// One stage's cards of a post: the others left out. Each keeps the stage file it goes to (stage_file, so
// its install puts it there whatever its name), and in Pokemon Stadium's list a post with one base file
// and each transformation once has the four go with the base (installed after it, as Nana's file goes
// with Popo's). Any other mix of transformations is left out: a skin brings all four or none.
void narrow_stage_cards(std::vector<Card>& cards, int filter) {
  const MenuStage* s = menu_stage(filter);
  if (!s) return;
  const bool stadium = lower(s->target) == "grps.dat";
  std::vector<Card> kept, forms[4];
  for (auto& c : cards) {
    std::string slot = card_slot(c);
    // (a file the name rules do not place, in a post filed under this stage: this stage's own file)
    if (slot.empty() && (stage_word(c.stage) == stage_word(s->site) || stage_word(c.stage) == stage_word(s->name)))
      slot = s->target;
    const int form = stadium_form_of(slot);
    if (stadium && form >= 0) {
      forms[form].push_back(std::move(c));
      continue;
    }
    if (lower(slot) != lower(s->target)) continue;
    if (c.stage_file.empty() && stage_file_key(c.file_name) != lower(slot)) c.stage_file = slot;
    c.stage = s->name;
    kept.push_back(std::move(c));
  }
  if (stadium && kept.size() == 1 && kept[0].partners.empty() &&
      std::all_of(std::begin(forms), std::end(forms), [](const std::vector<Card>& f) { return f.size() == 1; }))
  {
    for (auto& f : forms) {
      f[0].stage = s->name;
      kept[0].partners.push_back(std::move(f[0]));
    }
    kept[0].description = "Comes with its own four transformations. " + kept[0].description;
  }
  cards = std::move(kept);
}

void narrow_stage_posts(std::vector<Post>& posts, int filter) {
  if (!menu_stage(filter)) return;
  for (auto& post : posts) narrow_stage_cards(post.cards, filter);
  posts.erase(std::remove_if(posts.begin(), posts.end(), [](const Post& p) { return p.cards.empty(); }), posts.end());
}

// A Stadium skin installed with its four transformations (narrow_stage_cards): recorded as its set, so its
// pool serves the five together.
void stage_set_installed(const Card& card, const std::vector<std::pair<int, std::string>>& done, bool ok) {
  if (!ok || card.kind != "stage_visual" || card.partners.size() != 4 || done.size() != 5 ||
      done[0].first != card.file_id)
    return;
  std::vector<std::string> forms;
  for (size_t i = 0; i < 4; ++i) {
    if (done[i + 1].first != card.partners[i].file_id) return;
    forms.push_back(done[i + 1].second);
  }
  std::string error;
  const bool set = host::cosmetics::set_stadium_set(done[0].second, forms, &error);
  host::log("mods menu: card %d: its four transformations %s%s", card.file_id, set ? "come with it" : "not kept with it: ",
            set ? "" : error.c_str());
}

// Manage's page of one stage: its pool's skins (Vanilla is a row of its own, rows_locked), each with what
// the site said of it at install when it came from there; "Other stages": the skins of the rest.
bool fetch_pool_page(const std::string& key, int mode, int category, int filter) {
  if (mode != 1 || category != kStages) return false;
  std::map<int, std::string> by_file;
  const std::vector<Card> installed = scan_installed(kStages, &by_file);
  const auto known = [&](const std::string& id) -> const Card* {
    for (const auto& c : installed)
      if (c.asset_id == id) return &c;
    return nullptr;
  };
  Page result;
  result.status = kReady;
  if (const MenuStage* s = menu_stage(filter)) {
    for (const auto& pool : host::cosmetics::stage_pools()) {
      if (lower(pool.target_path) != lower(s->target)) continue;
      for (const auto& m : pool.skins) {
        const Card* found = known(m.asset_id);
        Card c = found ? *found : Card{};
        c.asset_id = m.asset_id;
        c.kind = "stage_visual";
        c.title = m.name;
        c.stage = s->name;
        for (const auto& form : m.forms) {
          const Card* f = known(form);
          Card part = f ? *f : Card{};
          part.asset_id = form;
          part.kind = "stage_visual";
          if (part.title.empty()) part.title = m.name;
          c.partners.push_back(std::move(part));
        }
        if (!m.forms.empty()) c.description = "Comes with its own four transformations. " + c.description;
        // (a skin added another way than the site: its picture, when it has one)
        if (!found)
          for (const auto& a : host::cosmetics::assets())
            if (a.id == m.asset_id) c.local_picture = !a.csp_path.empty() ? a.csp_path : a.preview_path;
        result.cards.push_back(std::move(c));
      }
    }
  } else if (filter == kOtherStages) {
    for (const auto& c : installed)
      if (!host::cosmetics::is_pool_stage(c.stage) && stadium_form_of(c.stage) < 0) result.cards.push_back(c);
    if (result.cards.empty()) result.message = "No skins of other stages installed.";
  }
  result.total = (int)result.cards.size();
  result.per_page = std::max<int>(1, result.total);
  result.has_next = false;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_installed = std::move(by_file);
  g_installed_known = true;
  g_pages[key] = std::move(result);
  return true;
}

// Manage: installed skins of stages that are not the six (listed in Manage before the pools).
int other_stage_skins() {
  int n = 0;
  for (const auto& a : host::cosmetics::assets())
    if (a.kind == "stage_visual" && a.source != "disc" && !host::cosmetics::is_pool_stage(a.target_path) &&
        stadium_form_of(a.target_path) < 0)
      ++n;
  return n;
}

// Browse and Manage Stages open on the six (the one chosen last under the cursor). True when it was Stages.
bool stages_open_locked() {
  if (g_category != kStages) return false;
  g_picking = true;
  g_filter = g_last_stage;
  host::log("mods menu: %s, Stages, the six stages", g_mode ? "Manage" : "Browse");
  return true;
}

// Stages' own change ops; -1 for the ones Browse's posts share with Characters (more, retry, packs).
int stages_change_locked(int op, int row) {
  if (g_category != kStages) return -1;
  const int rows = kMenuStageCount + (g_mode == 1 && other_stage_skins() > 0 ? 1 : 0);
  if (g_picking) {
    if (op != 9 || row < 0 || row >= rows) return 0;
    g_filter = g_last_stage = row + 1;
    g_picking = false;
    g_page = 0;
    g_more = -1;
    g_pack = 0;
    g_pack_page = 0;
    if (g_mode == 1) forget_pages_locked();   // the pool as it is now
    const MenuStage* s = menu_stage(g_filter);
    host::log("mods menu: stage %s", s ? s->name : "other stages");
    return 1;
  }
  if (op == 10 || op == 2 || op == 3) {
    if (g_pack != 0) return 0;
    if (op == 10) {
      g_picking = true;
      g_query.clear();
      g_query_id = 0;
      host::log("mods menu: back to the stages");
    } else {
      // L and R: the stage before or after (Manage's "Other stages" too, when it is there).
      g_filter = (g_filter - 1 + (op == 3 ? 1 : rows - 1)) % rows + 1;
      if (g_mode == 1) forget_pages_locked();
      const MenuStage* s = menu_stage(g_filter);
      host::log("mods menu: stage %s", s ? s->name : "other stages");
    }
    g_last_stage = g_filter;
    g_page = 0;
    g_more = -1;
    return 1;
  }
  if (op == 7 && g_mode == 1) {
    // "Vanilla": the standard stage in or out of the pool.
    const MenuStage* s = menu_stage(g_filter);
    if (!s || g_blocked.load()) return 0;
    bool on = true;
    for (const auto& pool : host::cosmetics::stage_pools())
      if (lower(pool.target_path) == lower(s->target)) on = pool.vanilla_on;
    std::string error;
    const bool set = host::cosmetics::set_pool_vanilla(s->target, !on, &error);
    host::log("mods menu: %s: Vanilla %s%s%s", s->name, !on ? "on" : "off", set ? "" : ", not saved: ", error.c_str());
    return set ? 1 : 0;
  }
  return -1;
}


// The list's reply for the six (Browse and Manage) and for a Manage stage (its pool); false for Browse's
// posts of a stage, which list() writes as it writes a fighter's.
bool stages_list_locked(int first, int count, std::vector<uint8_t>& reply) {
  if (g_category != kStages || (!g_picking && g_mode != 1)) return false;
  count = std::clamp(count, 0, 60);
  const auto pools = host::cosmetics::stage_pools();
  const auto pool_of = [&](const MenuStage& s) -> const host::cosmetics::StagePool* {
    for (const auto& p : pools)
      if (lower(p.target_path) == lower(s.target)) return &p;
    return nullptr;
  };
  // "3 in pool": its skins and Vanilla while it is on. In a row's box "Pool 3": the box squeezes longer
  // words until their letters run together.
  const auto pool_words = [&](const host::cosmetics::StagePool* p) {
    return p ? std::to_string(p->skins.size() + (p->vanilla_on ? 1 : 0)) + " in pool" : std::string();
  };
  const auto pool_box = [&](const host::cosmetics::StagePool* p) {
    return p ? "Pool " + std::to_string(p->skins.size() + (p->vanilla_on ? 1 : 0)) : std::string();
  };
  if (g_picking) {
    const int others = g_mode == 1 ? other_stage_skins() : 0;
    const int n = kMenuStageCount + (others > 0 ? 1 : 0);
    if (g_filter < 1 || g_filter > n) g_filter = 1;
    reply.push_back((uint8_t)kReady);
    put_u16(reply, n);
    put_u16(reply, g_filter - 1);
    put_u16(reply, 0);
    reply.push_back((uint8_t)1);   // the six, as the fighters' list
    put(reply, "", 20);
    put(reply, "Choose a stage", 48);
    put(reply, "", 48);
    for (int i = std::max(first, 0); i < first + count && i < n; ++i) {
      std::string name, right;
      if (i < kMenuStageCount) {
        const MenuStage& s = kMenuStages[i];
        name = s.name;
        // Browse shows no counts here: the site counts cards by fighter (/cards/counts), not posts by
        // stage, and a count only for the stages already opened read as some known, some not.
        if (g_mode == 1) right = pool_box(pool_of(s));
      } else {
        name = "Other stages";
        right = std::to_string(others);
      }
      put(reply, drawable(name, 39), 40);
      put(reply, drawable(right, 11), 12);
      reply.push_back(0);
    }
    return true;
  }
  // Manage: one stage's pool (or the other stages' skins).
  const Page& page = current_locked();
  const auto rows = rows_locked(page);
  const int n = (int)rows.size();
  const MenuStage* s = menu_stage(g_filter);
  const host::cosmetics::StagePool* pool = s ? pool_of(*s) : nullptr;
  reply.push_back((uint8_t)page.status);
  put_u16(reply, n);
  put_u16(reply, 0);
  put_u16(reply, 0);
  reply.push_back((uint8_t)(4 | (page.slow ? 16 : 0)));   // a stage shown: L and R step, B back to the six
  put(reply, s ? s->name : "Other stages", 20);
  std::string message = page.status == kLoading ? "Loading..." : page.message;
  if (page.status == kReady) message = pool ? pool_words(pool) : n > 0 ? thousands(n) + " installed" : message;
  put(reply, drawable(message, 47), 48);
  put(reply, "", 48);
  for (int i = std::max(first, 0); i < first + count && i < n; ++i) {
    const ViewRow& r = rows[(size_t)i];
    if (r.kind == kRowInstallAll) {
      put(reply, "Vanilla", 40);
      put(reply, pool && !pool->vanilla_on ? "Off" : "On", 12);
      reply.push_back((uint8_t)(kInstalled | 0x80));
      continue;
    }
    put(reply, row_name(r.card->title), 40);   // (a long title cut in the middle, as list() cuts it)
    put(reply, s ? "" : drawable(stage_of_file(r.card->stage, r.card->stage), 11), 12);   // ("Other stages": its stage)
    reply.push_back((uint8_t)state_locked(*r.card));
  }
  return true;
}

// A stage file's stage: the game's own name for it ("GrNBa.dat" -> "Battlefield"), else the post's.
std::string stage_of_file(const std::string& file_name, const std::string& post_stage) {
  const std::string base = lower(base_of(file_name));
  static const std::vector<host::cosmetics::StageSlot> slots = host::cosmetics::stage_slots();
  for (const auto& s : slots) {
    const std::string target = lower(s.target_path);
    const std::string stem = target.substr(0, target.find('.'));
    if (base == target || (!stem.empty() && base.rfind(stem + ".", 0) == 0)) return s.name;
  }
  return post_stage;
}

// The site's stage_file ("GrNBa.dat"; null when its export would refuse the file) as one of this
// game's stage slots, spelled as the slot is; "" when it names none. `name` gets the slot's stage.
std::string stage_slot_of(const std::string& stage_file, std::string* name) {
  if (stage_file.empty() || stage_file.size() > 32) return {};
  static const std::vector<host::cosmetics::StageSlot> slots = host::cosmetics::stage_slots();
  // (the site files Pokemon Stadium as its English file, "GrPs.usd": the slot is the .dat's, which serves both)
  std::string wanted = lower(stage_file);
  if (wanted.size() > 4 && wanted.compare(wanted.size() - 4, 4, ".usd") == 0) wanted = wanted.substr(0, wanted.size() - 4) + ".dat";
  for (const auto& s : slots)
    if (lower(s.target_path) == wanted) {
      if (name) *name = s.name;
      return s.target_path;
    }
  return {};
}

}  // namespace menu_internal
}  // namespace host::mods_menu
