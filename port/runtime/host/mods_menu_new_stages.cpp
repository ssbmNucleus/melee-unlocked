// The Mods menu's New Stages (the category menu's fifth row): stages of their own from SSBM Nucleus,
// kept by host/new_stages.h and played from the stage select's next pages. Shared pieces in
// mods_menu_internal.h.
//
// Browse lists the site's custom stage posts, most downloaded first:
//   GET <api>/mods?type=custom_stage&sort=downloads&limit=10
// A card is one file of such a post that is a stage file made over a stage this game plays new stages
// over (new_stages::base_of: "Smashville_GrTCa.dat" over Captain Falcon's Target Test), with a download
// of its own. A link, a zip, an xdelta patch or an m-ex stage (a stage id the retail game does not have)
// is no card. Its picture is the post's screenshots. A installs it: the file through its download_url
// (at most new_stages::kMaxBytes), checked against the disc's own base file (new_stages::check) and kept
// with the post's first PNG screenshot, the icon's picture. Manage lists what is installed and removes
// one. Nothing is published: the stage select asks for the list each time it opens (MU_CMD_NEW_STAGES).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {

bool new_stage_card_file(const std::string& file_name, const std::string& post_stage) {
  return !host::new_stages::base_of(file_name, post_stage).empty();
}

// Manage's rows (New Stages) and, in every category, the installed new stages' file ids (Browse's state).
void new_stages_installed(int category, std::vector<Card>& rows, std::map<int, std::string>* by_file) {
  for (const auto& m : host::new_stages::list()) {
    if (by_file && m.file_id) (*by_file)[m.file_id] = "newstage:" + m.id;
    if (category != kNewStages) continue;
    Card c;
    c.asset_id = "newstage:" + m.id;
    c.kind = "new_stage";
    c.mod_id = m.mod_id;
    c.file_id = m.file_id;
    c.title = m.name;
    c.creator = m.creator;
    c.author = m.author;
    c.description = m.description;
    c.downloads = m.downloads;
    c.file_name = m.file_name;
    c.stage_file = m.base;
    c.local_picture = m.picture;
    c.saved = true;
    rows.push_back(std::move(c));
  }
}

// The viewer's second line: the stage it is played over.
std::string new_stage_what(const Card& c) {
  const std::string name = host::new_stages::base_name(c.stage_file);
  return name.empty() ? "New stage" : "Built on " + name;
}

void run_new_stage_install(const Card& card, int batch) {
  const std::string base = card.stage_file;
  host::log("mods menu: install new stage card %d of mod %d (%s) over %s", card.file_id, card.mod_id,
            card.file_name.c_str(), base.c_str());
  std::string error, body;
  bool ok = false, slow = false;
  if (base.empty() || host::new_stages::base_name(base).empty()) {
    error = host::new_stages::unsupported_reason(card.file_name, std::string());
  } else if (card.download_url.empty()) {
    error = "This file has no download of its own.";
  } else {
    int status = 0;
    host::mod_browser::NetResult net;
    if (!host::mod_browser::get_text(card.download_url, &body, host::new_stages::kMaxBytes + 1, &error, &status,
                                     host::mod_browser::kWaitFile, &net)) {
      slow = net.timed_out;
      if (error.empty()) error = "The file could not be downloaded.";
    } else if (body.size() > host::new_stages::kMaxBytes) {
      error = "The file is larger than this game takes.";
    } else if (card.size >= 0 && (int64_t)body.size() != card.size) {
      error = "The file is not the one the site lists.";
    } else if (card.sha256.size() == 64 && host::http::sha256_hex(body.data(), body.size()) != lower(card.sha256)) {
      error = "The file is not the one the site lists.";
    } else {
      std::vector<uint8_t> retail;
      uint32_t offset = 0, length = 0;
      if (host::disc_find_file(base, &offset, &length) && length && length <= 64u * 1024 * 1024) {
        retail.resize(length);
        if (!host::disc_read(offset, retail.data(), length)) retail.clear();
      }
      const std::vector<uint8_t> bytes(body.begin(), body.end());
      host::new_stages::Installed info;
      info.base = base;
      info.name = install_name(card);
      info.creator = card.creator;
      info.author = card.author;
      info.description = card.description;
      info.file_name = card.file_name;
      info.mod_id = card.mod_id;
      info.file_id = card.file_id;
      info.downloads = card.downloads;
      ok = host::new_stages::install(info, bytes, retail, menu_picture(card), &error);
    }
  }
  host::log("mods menu: install new stage card %d: %s%s", card.file_id, ok ? "installed" : "failed: ", ok ? "" : error.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  Progress& p = g_progress[card.file_id];
  if (ok) {
    p.state = kInstalled;
    p.message = "Installed: on the stage select's next page.";
    g_installed[card.file_id] = "newstage:s" + std::to_string(card.file_id);
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

void run_new_stage_remove(const Card& card) {
  const std::string id = card.asset_id.rfind("newstage:", 0) == 0 ? card.asset_id.substr(9) : std::string();
  std::string error;
  const bool ok = !id.empty() && host::new_stages::remove(id, &error);
  if (id.empty()) error = "It is not a new stage.";
  host::log("mods menu: remove %s (%s): %s", card.asset_id.c_str(), card.title.c_str(), ok ? "done" : error.c_str());
  std::lock_guard<std::mutex> lock(g_mutex);
  Progress& p = g_removals[card.asset_id];
  p.state = ok ? kRemoved : kFailed;
  p.message = ok ? "Removed." : error;
  if (!ok) return;
  g_installed.erase(card.file_id);
  g_progress.erase(card.file_id);
}

}  // namespace menu_internal
}  // namespace host::mods_menu
