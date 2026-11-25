// The Mods menu's Menus (the category menu's sixth row): menu screens from SSBM Nucleus, kept and served
// by host/menu_mods.h. Shared pieces in mods_menu_internal.h.
//
// Browse lists the site's posts that have a menu screen file this game takes (menu_mods::target_of: the
// character select's MnSlChr), most downloaded first, from
//   GET <api>/mods?type=menu&sort=downloads&limit=10
// (type=menu: the site's posts with a menu screen file, whatever their own type). A site that refuses it
// (400: before type=menu) is read with the tag its character select posts carry instead, for the session:
//   GET <api>/mods?type=other&tags=character select screen&sort=downloads&limit=10
// A card is one such file; its picture the post's screenshots. A installs it: the file through its
// download_url (at most menu_mods::kMaxBytes), checked (menu_mods::check) and kept with the post's first
// PNG screenshot; a card for a screen another menu mod has says so ("A: Replace", "Replaces <its name>."
// opening the description) and
// the game's first A asks. Manage lists what is installed (menu_mods::list) and removes one. An install
// or a removal publishes the screen's file again (FileKind::Menu): its next load shows it, no restart.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"
#include "mods_menu_stb.h"

namespace host::mods_menu {
namespace menu_internal {

std::atomic<int> g_menu_api{kCardsUnknown};   // the site's type=menu: kCardsYes / kCardsNo as for /cards

bool menu_card_file(const std::string& file_name) {
  return !host::menu_mods::target_of(file_name).empty();
}

std::string mods_filter(int category) {
  if (category != kMenus) return std::string("type=") + site_type(category);
  return g_menu_api.load() == kCardsNo ? "type=other&tags=" + encode("character select screen") : std::string("type=menu");
}

// 400: today's site (no type=menu). 502: a site whose Function takes type=menu before its database does.
// Before any 200, either switches Menus to the tag for the session.
bool menus_refused(int status) {
  if (status == 200 && g_menu_api.load() == kCardsUnknown) g_menu_api.store(kCardsYes);
  if ((status != 400 && !(status == 502 && g_menu_api.load() == kCardsUnknown)) ||
      g_menu_api.exchange(kCardsNo) == kCardsNo)
    return false;
  host::log("mods menu: the site refuses type=menu (HTTP %d): Menus reads its character select posts by their tag", status);
  return true;
}

// Manage's rows (Menus) and, in every category, the installed menu mods' file ids (Browse's state).
void menus_installed(int category, std::vector<Card>& rows, std::map<int, std::string>* by_file) {
  for (const auto& m : host::menu_mods::list()) {
    if (by_file && m.file_id) (*by_file)[m.file_id] = "menu:" + m.id;
    if (category != kMenus) continue;
    Card c;
    c.asset_id = "menu:" + m.id;
    c.kind = "menu_visual";
    c.mod_id = m.mod_id;
    c.file_id = m.file_id;
    c.title = m.name;
    c.creator = m.creator;
    c.author = m.author;
    c.description = m.description;
    c.downloads = m.downloads;
    c.file_name = m.file_name;
    c.local_picture = m.picture;
    c.saved = true;
    rows.push_back(std::move(c));
  }
}

std::string menu_what(const Card& c, bool in_pack) {
  if (in_pack) return c.title;
  const std::string kind = host::menu_mods::kind_of(c.file_name);
  return kind.empty() ? "Menu screen" : kind;
}

// The name of the menu mod installed for this card's screen, when it is another card's.
std::string menu_replaces(const Card& c) {
  host::menu_mods::Installed m;
  const std::string target = host::menu_mods::target_of(c.file_name);
  if (target.empty() || !host::menu_mods::installed_for(target, &m) || m.file_id == c.file_id) return {};
  return m.name;
}

// The first of the card's pictures that is a PNG the hardened reader decodes in full (the post's
// screenshot: Manage shows it offline).
std::string menu_picture(const Card& card) {
  for (size_t i = 0; i < card.pictures.size() && i < 4 && !g_blocked.load(); ++i) {
    std::string body, error;
    int status = 0;
    if (!host::mod_browser::get_text(card.pictures[i], &body, kMaxPictureBytes, &error, &status, host::mod_browser::kWaitPicture))
      continue;
    const auto* data = reinterpret_cast<const stbi_uc*>(body.data());
    int w = 0, h = 0, n = 0;
    if (body.size() < 8 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0 ||
        !stbi_info_from_memory(data, (int)body.size(), &w, &h, &n) || w <= 0 || h <= 0 || w > 4096 || h > 4096)
      continue;
    stbi_uc* px = stbi_load_from_memory(data, (int)body.size(), &w, &h, &n, 4);
    if (!px) continue;
    stbi_image_free(px);
    return body;
  }
  return {};
}

void run_menu_install(const Card& card, int batch) {
  const std::string target = host::menu_mods::target_of(card.file_name);
  host::log("mods menu: install menu card %d of mod %d (%s) for %s", card.file_id, card.mod_id, card.file_name.c_str(),
            target.c_str());
  std::string error, body, replaced;
  bool ok = false, slow = false;
  if (target.empty()) {
    error = host::menu_mods::unsupported_reason(card.file_name);
  } else if (card.download_url.empty()) {
    error = "This file has no download of its own.";
  } else {
    int status = 0;
    host::mod_browser::NetResult net;
    if (!host::mod_browser::get_text(card.download_url, &body, host::menu_mods::kMaxBytes + 1, &error, &status,
                                     host::mod_browser::kWaitFile, &net)) {
      slow = net.timed_out;
      if (error.empty()) error = "The file could not be downloaded.";
    } else if (body.size() > host::menu_mods::kMaxBytes) {
      error = "The file is larger than a menu screen's.";
    } else if (card.size >= 0 && (int64_t)body.size() != card.size) {
      error = "The file is not the one the site lists.";
    } else if (card.sha256.size() == 64 && host::http::sha256_hex(body.data(), body.size()) != lower(card.sha256)) {
      error = "The file is not the one the site lists.";
    } else {
      std::vector<uint8_t> retail;
      uint32_t offset = 0, length = 0;
      if (host::disc_find_file(target, &offset, &length) && length && length <= host::menu_mods::kMaxBytes) {
        retail.resize(length);
        if (!host::disc_read(offset, retail.data(), length)) retail.clear();
      }
      const std::vector<uint8_t> bytes(body.begin(), body.end());
      host::menu_mods::Installed info;
      info.target = target;
      info.name = install_name(card);
      info.creator = card.creator;
      info.author = card.author;
      info.description = card.description;
      info.file_name = card.file_name;
      info.mod_id = card.mod_id;
      info.file_id = card.file_id;
      info.downloads = card.downloads;
      ok = host::menu_mods::install(info, bytes, retail, menu_picture(card), &error, &replaced);
    }
  }
  host::log("mods menu: install menu card %d: %s%s", card.file_id, ok ? "installed" : "failed: ", ok ? "" : error.c_str());
  std::map<int, std::string> now;   // the menu mods installed now, by file id
  std::vector<Card> unused;
  menus_installed(kMenus, unused, &now);
  std::lock_guard<std::mutex> lock(g_mutex);
  Progress& p = g_progress[card.file_id];
  if (ok) {
    p.state = kInstalled;
    p.message = replaced.empty() ? "Installed." : "Replaced " + replaced + ".";
    // The one it replaced is no longer installed (Browse shows its card as not installed again).
    for (auto it = g_installed.begin(); it != g_installed.end();)
      it = it->second.rfind("menu:", 0) == 0 && !now.count(it->first) ? g_installed.erase(it) : std::next(it);
    g_installed[card.file_id] = "menu:m" + std::to_string(card.file_id);
    g_republish.emplace_back(target, FileKind::Menu);
  } else {
    p.state = kFailed;
    p.message = error;
    p.slow = slow;
  }
  if (batch != 0 && batch == g_batch.id) {
    ++(ok ? g_batch.done : g_batch.failed);
    if (g_batch.done + g_batch.failed >= g_batch.total) g_batch.running = false;
  }
  forget_pages_locked();
}

void run_menu_remove(const Card& card) {
  const std::string id = card.asset_id.rfind("menu:", 0) == 0 ? card.asset_id.substr(5) : std::string();
  std::string error, target;
  const bool ok = !id.empty() && host::menu_mods::remove(id, &error, &target);
  if (id.empty()) error = "It is not a menu mod.";
  host::log("mods menu: remove %s (%s): %s", card.asset_id.c_str(), card.title.c_str(), ok ? "done" : error.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  Progress& p = g_removals[card.asset_id];
  p.state = ok ? kRemoved : kFailed;
  p.message = ok ? "Removed." : error;
  if (!ok) return;
  g_installed.erase(card.file_id);
  g_progress.erase(card.file_id);
  g_republish.emplace_back(target, FileKind::Menu);
}

}  // namespace menu_internal
}  // namespace host::mods_menu
