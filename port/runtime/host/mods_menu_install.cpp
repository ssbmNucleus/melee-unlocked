// The Mods menu's installs and removals: the three job queues and their workers, the install and
// remove of a card, its pictures and details kept with it, Manage's installed list.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"
#include "mods_menu_stb.h"

namespace host::mods_menu {
namespace menu_internal {

// <settings dir>/CosmeticMods/nucleus-details.json (beside nucleus-origins.json): per installed
// asset, what its card said when it was installed (title, who made it, description, download count),
// so Manage shows it with no connection. Text only, each field length-limited when it is written and
// again when it is read; drawable() maps it to the font's characters on the way to the game.
fs::path details_path() {
  const fs::path cache = fs::u8path(host::skins::cache_folder());
  return cache.empty() ? fs::path() : cache.parent_path() / L"CosmeticMods" / L"nucleus-details.json";
}
json read_details_locked() {
  const fs::path path = details_path();
  std::error_code ec;
  if (path.empty() || fs::file_size(path, ec) > (4u << 20) || ec) return json::object();
  std::ifstream in(path, std::ios::binary);
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  json doc = json::parse(body, nullptr, false);
  return doc.is_object() ? doc : json::object();
}
void save_details(const std::vector<std::string>& assets, const Card& card, const std::string& title) {
  if (assets.empty()) return;
  std::lock_guard<std::mutex> lock(g_details_mutex);
  const fs::path path = details_path();
  if (path.empty()) return;
  json doc = read_details_locked();
  for (const auto& id : assets)
    doc[id] = {{"title", title.substr(0, 200)}, {"creator", card.creator.substr(0, 100)}, {"author", card.author.substr(0, 100)},
               {"description", card.description.substr(0, 500)}, {"downloads", card.downloads}};
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path staged = fs::path(path).concat(L".tmp");
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out << doc.dump(2) << "\n";
    if (!out) { fs::remove(staged, ec); return; }
  }
  fs::rename(staged, path, ec);
  if (ec) fs::remove(staged, ec);
}


// The installed skins that came from SSBM Nucleus: file id -> asset, and Manage's rows.
std::vector<Card> scan_installed(int category, std::map<int, std::string>* by_file) {
  std::vector<Card> rows;
  json details;
  {
    std::lock_guard<std::mutex> lock(g_details_mutex);
    details = read_details_locked();
  }
  for (const auto& asset : host::cosmetics::assets()) {
    slippi::skin_wire::FileRef ref;
    if (!host::skins::origin_of(asset.id, &ref) || !ref.file_id) continue;
    if (by_file) (*by_file)[(int)ref.file_id] = asset.id;
    const bool stage = asset.kind == "stage_visual";
    if (asset.kind != asset_kind(category)) continue;
    Card c;
    c.asset_id = asset.id;
    c.kind = asset.kind;
    c.mod_id = (int)ref.mod_id;
    c.file_id = (int)ref.file_id;
    c.title = asset.name;
    c.character = asset.character;
    c.color = asset.costume;
    c.stage = stage ? asset.target_path : std::string();
    c.file_name = stage ? std::string() : asset.target_path;   // "PlPpNr.dat": tells Popo and Nana apart
    // Its own portrait, else its preview; nothing from the site (a plain tile when it has neither).
    c.local_picture = !asset.csp_path.empty() ? asset.csp_path : asset.preview_path;
    if (!stage) c.local_stock = asset.stock_path;   // beside the portrait (fetch_picture)
    if (const auto d = details.find(asset.id); d != details.end() && d->is_object()) {
      c.saved = true;
      c.creator = text_of(*d, "creator", 100);
      c.author = text_of(*d, "author", 100);
      c.description = text_of(*d, "description", 500);
      c.downloads = number_of(*d, "downloads");
      // (the title is the skin's name, which the install gave it: a name the player gave it since stays)
    }
    rows.push_back(std::move(c));
  }
  menus_installed(category, rows, by_file);   // the menu mods (mods_menu_menus.cpp)
  new_stages_installed(category, rows, by_file);   // the new stages (mods_menu_new_stages.cpp)
  std::sort(rows.begin(), rows.end(), [](const Card& a, const Card& b) { return a.title < b.title; });
  // An Ice Climbers costume is one row here too (Popo's, with Nana's file under it): removing it
  // removes both.
  for (size_t i = 0; i < rows.size();) {
    if (climber_of(rows[i]) == 2) {
      const auto popo = std::find_if(rows.begin(), rows.end(), [&](const Card& c) {
        return climber_of(c) == 1 && c.mod_id == rows[i].mod_id && c.partners.empty() &&
               lower(base_of(c.file_name)).substr(4) == lower(base_of(rows[i].file_name)).substr(4);
      });
      if (popo != rows.end()) {
        popo->partners.push_back(rows[i]);
        rows.erase(rows.begin() + (std::ptrdiff_t)i);
        continue;
      }
    }
    ++i;
  }
  return rows;
}

// Skins installed before without the site's portrait or stock icon (an older rule withheld every
// picture the site marked portrait_shared): each picture of its own address is fetched and kept now,
// when the skin's post is seen again (its pack opened, or its row's screen read). A few per visit.
void repicture_installed(const std::vector<Card>& cards, const std::map<int, std::string>& by_file) {
  struct Want { std::string asset, url; const char* kind; int file_id; };
  std::vector<Want> wants;
  const auto assets = host::cosmetics::assets();
  for (const auto& c : cards) {
    const auto it = by_file.find(c.file_id);
    if (it == by_file.end()) continue;
    for (const auto& a : assets) {
      if (a.id != it->second) continue;
      if (a.csp_path.empty() && !c.withhold_csp && !c.csp_url.empty()) wants.push_back({a.id, c.csp_url, "csp", c.file_id});
      if (a.stock_path.empty() && !c.withhold_stock && !c.stock_url.empty())
        wants.push_back({a.id, c.stock_url, "stock", c.file_id});
    }
  }
  if (wants.size() > 8) wants.resize(8);
  for (const auto& w : wants) {
    if (g_blocked.load()) break;
    host::log("mods menu: card %d was installed without the site's %s, which is its own: kept now", w.file_id,
              std::string(w.kind) == "csp" ? "portrait" : "stock icon");
    attach_picture(w.asset, w.url, w.kind, w.file_id);
  }
}

// One card's file: through its own download_url when it has one, else through the site's per-file
// export (what "Add to Nucleus" has the desktop app fetch), which brings an Ice Climbers costume's
// other half too (`bundled`).
host::mod_browser::Installed install_one(const Card& c, const std::string& name, int partner_file_id, bool* bundled) {
  if (bundled) *bundled = false;
  // (an effect is installed unused: run_install uses it, or holds it while it would override another's moves)
  const bool effect = c.kind == "effect_visual";
  if (!c.download_url.empty())
    return host::mod_browser::install_file(c.mod_id, c.file_id, c.download_url, c.file_name, name, c.sha256, c.size,
                                           c.stage, c.stage_file, effect, !effect);
  return host::mod_browser::install_export(c.mod_id, c.file_id, c.export_url, c.file_name, name, partner_file_id, bundled,
                                           c.sha256, effect, !effect);
}

// A skin installed through its own download_url (a DAT, no pictures in it): the card's portrait and
// stock icon from the site, each fetched once (at most 4 MB), a PNG whose size the header gives and
// that the hardened reader decodes in full before it is kept (as the importer keeps an export's
// csp.png and stc.png). Only where the skin has no picture of that kind yet.
void attach_picture(const std::string& asset_id, const std::string& url, const char* kind, int file_id) {
  if (url.empty() || g_blocked.load()) return;
  std::string body, error;
  int status = 0;
  host::mod_browser::NetResult net;
  if (!host::mod_browser::get_text(url, &body, kMaxPictureBytes, &error, &status, host::mod_browser::kWaitPicture, &net)) {
    host::log("mods menu: card %d: its %s was not fetched (%s)", file_id, kind, error.c_str());
    return;
  }
  const auto* data = reinterpret_cast<const stbi_uc*>(body.data());
  int w = 0, h = 0, n = 0;
  bool ok = body.size() >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0 &&
            stbi_info_from_memory(data, (int)body.size(), &w, &h, &n) && w > 0 && h > 0 && w <= 1024 && h <= 1024;
  if (ok) {
    stbi_uc* px = stbi_load_from_memory(data, (int)body.size(), &w, &h, &n, 4);
    ok = px != nullptr;
    stbi_image_free(px);
  }
  if (!ok) {
    host::log("mods menu: card %d: its %s is not a PNG this reader takes (%zu bytes); not kept", file_id, kind, body.size());
    return;
  }
  std::error_code ec;
  const fs::path folder = fs::temp_directory_path(ec) / L"MeleeUnlockedNucleus" / (L"pic_" + std::to_wstring(file_id));
  fs::create_directories(folder, ec);
  const fs::path file = folder / (std::string(kind) == "csp" ? L"csp.png" : L"stc.png");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(body.data(), (std::streamsize)body.size());
  }
  const auto result = host::cosmetics::set_skin_portrait(asset_id, file.u8string(), kind);
  host::log("mods menu: card %d: its %s (%dx%d) %s%s", file_id, kind, w, h, result.ok ? "kept with " : "not kept: ",
            result.ok ? asset_id.c_str() : result.message.c_str());
  fs::remove_all(folder, ec);
}

// The name an installed skin gets: its post's title, and in a pack what tells it from the others.
// (the fighter left out of a pack's label: the select screen shows the skin under its fighter, and the
// colour comes first where its name is cut short: "DBZ Pack Goku - Red - HALO")
std::string install_name(const Card& card) {
  std::string name = card.title;
  if (!card.label.empty()) {
    std::string label = card.label;
    const std::string who = (climber_of(card) ? std::string("Ice Climbers") : fighter_label(card.character)) + " - ";
    if (card.kind == "character_costume" && label.size() > who.size() && label.compare(0, who.size(), who) == 0)
      label = label.substr(who.size());
    name += " - " + label;
  } else if (card.multiple && !card.color.empty()) {
    name += " " + card.color;
  }
  return drawable(name, 60);
}

void run_install(const Card& card, int batch) {
  if (fighters_run_install(card)) return;   // a fighter's package (mods_menu_fighters.cpp)
  if (card.kind == "menu_visual") { run_menu_install(card, batch); return; }
  if (card.kind == "new_stage") { run_new_stage_install(card, batch); return; }
  host::log("mods menu: install card %d of mod %d (%s) through %s", card.file_id, card.mod_id, card.file_name.c_str(),
            card.download_url.empty() ? "the site's export" : "its download_url");
  std::vector<Card> partners = card.partners.empty() ? climbers_partners(card) : card.partners;
  const std::string name = install_name(card);
  std::vector<std::pair<int, std::string>> done;   // file id -> asset
  bool bundled = false;
  auto result = install_one(card, name, partners.empty() ? 0 : partners[0].file_id, &bundled);
  if (result.ok && !result.asset_id.empty()) done.emplace_back(card.file_id, result.asset_id);
  if (result.ok && bundled) {
    if (!partners.empty() && !result.partner_asset_id.empty()) done.emplace_back(partners[0].file_id, result.partner_asset_id);
    partners.clear();
  }
  for (const auto& c : partners) {
    if (!result.ok) break;
    result = install_one(c, name, 0, nullptr);
    if (result.ok && !result.asset_id.empty()) done.emplace_back(c.file_id, result.asset_id);
  }
  stage_set_installed(card, done, result.ok);   // a Stadium skin's four transformations (mods_menu_stages.cpp)
  host::log("mods menu: install card %d of mod %d: %s%s", card.file_id, card.mod_id,
            result.ok ? (result.already ? "already installed" : "installed") : "failed: ",
            result.ok ? "" : result.message.c_str());
  if (result.ok && !done.empty()) {
    // What Manage shows offline, and (the download_url route) the card's own pictures.
    std::vector<std::string> assets;
    for (const auto& item : done) assets.push_back(item.second);
    save_details(assets, card, name);
    // A portrait (stock icon) whose address another file of its post has too (judge_pictures) is not
    // this file's for sure: not kept as this skin's own (the export's csp.png or stc.png is taken off
    // again), so the select screen and the match show its slot's own instead. A picture of its own
    // address is kept: the export's, else fetched from the site (the download_url route brings none).
    if ((card.withhold_csp || card.withhold_stock) && !result.already)
      for (const auto& asset : assets) {
        std::string ignored;
        if (card.withhold_csp) host::cosmetics::clear_skin_portrait(asset, "csp", &ignored);
        if (card.withhold_stock) host::cosmetics::clear_skin_portrait(asset, "stock", &ignored);
      }
    if (card.withhold_csp || card.withhold_stock)
      host::log("mods menu: card %d: its %s the same picture as another file's of its post: installed without it%s",
                card.file_id,
                card.withhold_csp && card.withhold_stock ? "portrait and stock icon are"
                : card.withhold_csp                     ? "portrait is"
                                                        : "stock icon is",
                result.already ? " (installed before: left as it was)" : "");
    const std::string& own = done.front().second;
    bool has_csp = false, has_stock = false, has_preview = false;
    for (const auto& a : host::cosmetics::assets())
      if (a.id == own) { has_csp = !a.csp_path.empty(); has_stock = !a.stock_path.empty(); has_preview = !a.preview_path.empty(); }
    if (card.kind == "effect_visual") {
      if (!has_preview) attach_effect_preview(own, card);
    } else {
      if (!has_csp && !card.withhold_csp) attach_picture(own, card.csp_url, "csp", card.file_id);
      if (!has_stock && !card.withhold_stock) attach_picture(own, card.stock_url, "stock", card.file_id);
    }
    if (card.shares_portrait && !card.withhold_csp)
      host::log("mods menu: card %d: the site marks its portrait shared, but its address is its own file's: kept",
                card.file_id);
  }
  // An effect is served from its disc file (EfCaData.dat), which the game reads at each match. Used now
  // when it takes no move from another effect (else held, its card saying what it would override:
  // mods_menu_effect_slots.cpp), its file is published again (take_republish), so the next match has it
  // with no restart. Once per file.
  std::vector<std::string> effect_files;
  if (result.ok && card.kind == "effect_visual")
    for (const auto& item : done) {
      std::string target;
      if (effect_installed(card, item.second, &target) && !target.empty() &&
          std::find(effect_files.begin(), effect_files.end(), target) == effect_files.end())
        effect_files.push_back(target);
    }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& file : effect_files) g_republish.emplace_back(file, FileKind::Effect);
  Progress& p = g_progress[card.file_id];
  if (result.ok) {
    p.state = kInstalled;
    p.message = result.already ? "Already installed." : !effect_files.empty() ? "Installed and in use." : "Installed.";
  } else if (card.kind == "effect_visual" && host::cosmetics::effect_verdict_refuses(result.verdict)) {
    // The effect checks refuse its file (what --check-effect says of it): not installable, so no "Try
    // again"; its reason stands in the viewer (detail).
    effect_refuse_locked(card, result.verdict, result.message);
    p.state = kNotInstalled;
    p.message = result.message;
  } else {
    p.state = kFailed;
    p.message = result.message;
    p.slow = result.slow;
    // A file that is not the one the card names: the list is read again when it is next opened.
    if (result.mismatch)
      for (auto it = g_pages.begin(); it != g_pages.end();)
        it = it->first.rfind("0/", 0) == 0 && it->first != page_key(g_mode, g_category, g_filter, g_page) ? g_pages.erase(it)
                                                                                                       : std::next(it);
  }
  for (const auto& [file, asset] : done) g_installed[file] = asset;
  if (batch != 0 && batch == g_batch.id) {
    ++(result.ok ? g_batch.done : g_batch.failed);
    if (g_batch.done + g_batch.failed >= g_batch.total) {
      g_batch.running = false;
      host::log("mods menu: install all of mod %d: %d installed, %d failed, %d were installed already", g_batch.mod_id,
                g_batch.done, g_batch.failed, g_batch.skipped);
    }
  }
  forget_pages_locked();
}

void run_remove(const Card& card) {
  if (fighters_run_remove(card)) return;   // an installed fighter (mods_menu_fighters.cpp)
  if (card.kind == "menu_visual") { run_menu_remove(card); return; }
  if (card.kind == "new_stage") { run_new_stage_remove(card); return; }
  // The row's skin, and Nana's file with Popo's.
  std::vector<Card> all{card};
  for (const auto& p : card.partners) all.push_back(p);
  bool ok = true;
  std::string error;
  std::vector<std::pair<std::string, FileKind>> freed_slots;
  std::vector<int> removed_files;
  for (const auto& c : all) {
    std::string one_error, freed;
    const bool one = host::cosmetics::remove_asset(c.asset_id, &one_error, &freed);
    host::log("mods menu: remove %s (%s): %s", c.asset_id.c_str(), c.title.c_str(), one ? "done" : one_error.c_str());
    if (!one) { ok = false; if (error.empty()) error = one_error; continue; }
    removed_files.push_back(c.file_id);
    if (!freed.empty())
      freed_slots.emplace_back(freed, c.kind == "stage_visual"    ? FileKind::Stage
                                      : c.kind == "effect_visual" ? FileKind::Effect
                                                                  : FileKind::Costume);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Progress& p = g_removals[card.asset_id];
  p.state = ok ? kRemoved : kFailed;
  p.message = !ok ? error : "Removed.";
  for (const int file : removed_files) {
    g_installed.erase(file);
    g_progress.erase(file);
  }
  for (const auto& c : all) effect_removed_locked(c.asset_id);   // (an effect held for its notice is gone)
  for (auto& slot : freed_slots) g_republish.push_back(std::move(slot));
  // Manage keeps showing the row (as removed) until it is opened again.
}

void worker(Queue* queue) {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      queue->wake.wait(lock, [&] { return !queue->jobs.empty(); });
      job = std::move(queue->jobs.front());
      queue->jobs.pop_front();
      if (job.kind == Job::kPicture)
        if (const auto it = g_pictures.find(job.key.empty() ? card_key(job.card) : job.key); it != g_pictures.end())
          it->second.fetching = true;
    }
    switch (job.kind) {
    case Job::kPage: fetch_page(job.key); break;
    case Job::kPack: fetch_pack(job.key); break;
    case Job::kPicture:
      if (job.of_post) fetch_post_picture(job);
      else fetch_picture(job.card, job.post.shots_known ? &job.post : nullptr, job.prefetch);
      break;
    case Job::kInstall: run_install(job.card, job.batch); break;
    case Job::kRemove: run_remove(job.card); break;
    case Job::kCardInfo: fetch_card_info(job.card, job.batch); break;
    case Job::kCounts: fetch_counts(job.batch); break;
    case Job::kEffectUse: case Job::kEffectSet: run_effect_job(job); break;   // mods_menu_effect_slots.cpp
    }
  }
}


void start_locked(Queue& queue) {
  if (!queue.started && !g_offline.load()) {
    queue.started = true;
    // Pictures: a few at once (kPictureWorkers); the other queues one at a time.
    const int workers = &queue == &g_picture_queue ? kPictureWorkers : 1;
    for (int i = 0; i < workers; ++i) std::thread(worker, &queue).detach();
  }
}

void queue_page_locked(const std::string& key) {
  if (std::any_of(g_page_queue.jobs.begin(), g_page_queue.jobs.end(), [&](const Job& j) { return j.key == key; })) return;
  Job job;
  job.kind = Job::kPage;
  job.key = key;
  start_locked(g_page_queue);
  g_page_queue.jobs.push_back(std::move(job));
  g_page_queue.wake.notify_one();
}

void queue_pack_locked(const std::string& key) {
  if (std::any_of(g_page_queue.jobs.begin(), g_page_queue.jobs.end(),
                  [&](const Job& j) { return j.kind == Job::kPack && j.key == key; }))
    return;
  g_pack_lists[key] = PackList{};
  Job job;
  job.kind = Job::kPack;
  job.key = key;
  start_locked(g_page_queue);
  g_page_queue.jobs.push_back(std::move(job));
  g_page_queue.wake.notify_one();
}

// /cards/{id} for a post of /mods whose one card is viewed (fetch_card_info), once per card.
void queue_card_info_locked(const Card& card, int category) {
  if (g_offline.load() || g_card_info.count(card.file_id) || g_cards_api.load() == kCardsNo) return;
  if (category == kMenus) return;   // a menu screen's file is no card of the site's
  if (category == kNewStages) return;   // nor a custom stage's
  g_card_info[card.file_id] = kLoading;
  Job job;
  job.kind = Job::kCardInfo;
  job.card = card;
  job.batch = category;
  start_locked(g_page_queue);
  g_page_queue.jobs.push_back(std::move(job));
  g_page_queue.wake.notify_one();
}

// The fighters' counts of `category` (g_counts), asked for once for the session, first in line (the
// fighters' list makes no other request); after a failure the site may get over, again a minute
// later. Not while the hidden duplicates are listed: the counts leave them out.
void want_counts_locked(int category) {
  if (g_offline.load() || kListHiddenDuplicates || !by_fighter(category)) return;
  const auto now = std::chrono::steady_clock::now();
  const auto it = g_counts.find(category);
  if (it != g_counts.end() &&
      (it->second.status != kError || !it->second.retry || now - it->second.when < std::chrono::seconds(60)))
    return;
  Counts& c = g_counts[category];
  c = Counts{};
  c.when = now;
  Job job;
  job.kind = Job::kCounts;
  job.batch = category;
  start_locked(g_page_queue);
  g_page_queue.jobs.push_front(std::move(job));
  g_page_queue.wake.notify_one();
}

void queue_install_locked(Job job) {
  start_locked(g_install_queue);
  g_install_queue.jobs.push_back(std::move(job));
  g_install_queue.wake.notify_one();
}

}  // namespace menu_internal
}  // namespace host::mods_menu
