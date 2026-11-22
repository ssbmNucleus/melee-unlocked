// The in-game mod browser's host half: pages of the SSBM Nucleus catalog (ssbmnucleus.net) and the
// download and installation of one mod, all off the game thread. The game's half
// (sourceport/game/shim/mu_mod_browser.c) draws the list and asks through one host command
// (source_host.cpp, CMD_MOD_BROWSER).
//
// The design follows GurekamDhillon's Nucleus browser for his Melee build (pc/platform/gw_nucleus_*.h,
// GPL-3.0) and the rules its API's owner asked of a client: a User-Agent naming this program, no
// polling (a page is fetched when the player opens it and kept for ten minutes), downloads only
// through the API's own download_url, and the credit line on the screen. Only ssbmnucleus.net and
// its subdomains are contacted, over HTTPS.
//
// A downloaded costume or stage skin goes through host::cosmetics::import_file, the same importer
// the Mods tab uses, so it is checked the same way and is offered on the character or stage select
// (L / R) at once: nothing restarts.
//
// A fighter plugin (a post's .mufighter file: a fighter's native code, sourceport/game/mexft) goes
// into the fighters folder, where the game loads it at its next change of mode. Unlike a skin it is
// code that runs on this computer, so it is downloaded only when the player has allowed it
// (MELEE_ALLOW_PLUGIN_DOWNLOADS=1), and is checked to be a plugin file before it is kept.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

namespace host::mod_browser {

enum Tab { kCostumes = 0, kStageSkins = 1, kFighters = 2, kTabCount = 3 };
enum Sort { kMostDownloaded = 0, kNewest = 1, kMostLiked = 2, kSortCount = 3 };
enum RowState { kNone = 0, kQueued = 1, kDownloading = 2, kInstalled = 3, kFailed = 4, kUnsupported = 5 };
enum Status { kLoading = 0, kReady = 1, kError = 2 };

// What the player asked to see. `fighter` is an index into fighters() (0: all of them) and only
// narrows the costumes; `search` is matched by the site against the posts' titles.
struct Query {
  int tab = kCostumes;
  int sort = kMostDownloaded;
  int fighter = 0;
  std::string search;
};

struct Row {
  int id = 0;
  std::string title, author;
  int downloads = 0;
  int state = kNone;
  std::string note;   // why a download failed, or what an installed mod is offered as
};

struct View {
  int status = kLoading;
  std::string message;   // the error, when status is kError
  int page = 0, pages = 0;
  bool has_next = false;
  std::vector<Row> rows;
};

constexpr int kRowsPerPage = 8;
constexpr int kMaxSearch = 24;
inline const char* credit() { return "Mods from SSBM Nucleus - ssbmnucleus.net"; }
// The fighter filter's choices, as the site names them: "" (all) first.
const std::vector<std::string>& fighters();

// The page the game shows: starts fetching it when it is not held yet (or is older than ten
// minutes), and answers with what is known now. Game thread; never blocks.
View view(const Query& query, int page);
// Queues one mod of the pages viewed so far for download and installation. False when the id is
// not one of them or is already queued. Game thread; never blocks.
bool download(int id);
// Refuses everything while this is set: an online session keeps the cosmetic profile as it was.
void set_blocked(bool blocked);
// Where fighter plugins are kept: the folder the game library reads (mexft_plugins.c).
void set_fighters_folder(const std::string& folder);

}  // namespace host::mod_browser
