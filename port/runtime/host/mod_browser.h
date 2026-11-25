// The in-game mod browser's host half: pages of the SSBM Nucleus catalog (ssbmnucleus.net) and the
// download and installation of one mod, all off the game thread. The game's half
// (sourceport/game/shim/mu_mod_browser.c) draws the list and asks through one host command
// (source_host_mods_menu.cpp, CMD_MOD_BROWSER).
//
// The design follows GurekamDhillon's Nucleus browser for his Melee build (pc/platform/gw_nucleus_*.h,
// GPL-3.0) and the rules its API's owner asked of a client: a User-Agent naming this program, no
// polling (a page is fetched when the player opens it and kept for ten minutes), downloads only
// through the API's own download_url (or, for a file with none, the site's counted per-file export,
// install_export below), and the credit line on the screen. Only ssbmnucleus.net and its subdomains
// are contacted, over HTTPS.
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
// Whether a kept plugin file will load: signed by Nucleus, or approved by the player (app/plugin_gate.h).
// Installing never approves one; an unapproved plugin's row says it waits for approval. Unset: not asked.
void set_plugin_check(bool (*would_load)(const std::string& path));
// Told of every costume file a download installed (or found already installed): the asset it became,
// the post's and the file's SSBM Nucleus ids, and the downloaded file, which is still there during the
// call (opponents' skins keep the ids and hash: host::skins::record_origin); for a file taken out of a
// zip, the file as taken out, and the zip's SHA-256 in `archive_sha256` (else empty). Worker thread.
// Unset: not told.
void set_skin_installed(void (*installed)(const std::string& asset_id, int mod_id, int file_id, const std::string& downloaded_path,
                                          const std::string& archive_sha256));

// For opponents' skins online (host/skin_fetch.h). The API address this run uses, and one blocking
// GET of a file into `out_utf8` by the rules above: SSBM Nucleus and its subdomains over HTTPS (the
// test stand-in only in a run started by automation), redirects that leave them refused, at most
// `limit` bytes (`too_large` is set when the body ran past it; `status`, optional, gets the HTTP
// status when the site answered). Not refused by set_blocked: it changes nothing installed. Worker
// threads only.
std::string api_address();
bool get_file(const std::string& url, const std::string& out_utf8, uint64_t limit, std::string* error, bool* too_large = nullptr,
              int* status = nullptr);
// How long a GET may wait for the answer's first byte (and each later read): a list page 20 s, a
// picture 12 s, a file 30 s; connecting 6 s. A GET the site did not answer in time, or could not be
// reached for, or whose connection dropped while the answer came, or answered 502/503/504/52x, is
// tried once more. Every attempt logs one line ("net: GET
// <host><path> -> <status, or the WinHTTP error code> ... in <seconds>"). Requests to the site's
// /api/ are kept to 30 a minute, and after a 429 none is sent until its Retry-After has passed.
enum Wait { kWaitList = 0, kWaitPicture = 1, kWaitFile = 2 };
struct NetResult {
  int status = 0;              // the HTTP status, 0 when none came
  unsigned long error = 0;     // the WinHTTP error code when none came
  bool timed_out = false;      // the site did not answer in time (twice)
  int retry_after = 0;         // a 429's wait, in seconds
};
// The same GET into memory (the in-game Mods menu's pages and pictures, host/mods_menu.h). `status`
// (optional) gets the HTTP status when the site answered. Worker threads only.
bool get_text(const std::string& url, std::string* body, uint64_t limit, std::string* error, int* status = nullptr,
              int wait = kWaitList, NetResult* net = nullptr);
// A picture's GET for the Mods menu's copies on disk (host/mods_menu.cpp), as get_text with kWaitPicture:
// `etag` (a copy's, may be empty) goes as If-None-Match; `*not_modified` is set when the site answered
// 304 (the copy stands, `body` stays empty); `*new_etag` is the answer's ETag (printable, at most 200
// characters, else empty). Every request of the run shares one WinHTTP session, so a host's connection
// is used again. Worker threads only.
bool get_picture(const std::string& url, const std::string& etag, std::string* body, uint64_t limit, std::string* error,
                 bool* not_modified, std::string* new_etag, NetResult* net = nullptr);

// The in-game Mods menu's install of one file of a post (one card): downloaded through the API's own
// download_url, imported as every download is (host::cosmetics::import_file), named after `title`, and
// its origin told to the set_skin_installed hook. When the site gave the file's size and SHA-256
// (`expected_size` >= 0, `expected_sha256` 64 hex digits), a download that differs is not installed
// (`mismatch` set). An install never sends the view marker an opponent's skin is fetched with
// (skin_fetch.h download_view_address): an address whose query names a view is refused, so every
// install counts. Refused while set_blocked. Worker threads only.
struct Installed {
  bool ok = false, already = false, mismatch = false;
  bool slow = false;              // the site did not answer in time

  std::string asset_id, message;
  std::string partner_asset_id;   // install_export: the other half the export brought, when it did
  std::string verdict;            // an effect refused: its check's verdict (host::cosmetics::EffectVerdict)
};
// `stage_file` (a stage card's, from the site's /cards: the disc file it replaces, already checked to
// be one of this game's stage slots) is the slot it goes to when its own name is another; else the
// slot is worked out as below. `effect`: an effects card's file, imported by
// host::cosmetics::import_effect (no stage slots); `use_effect` false: installed only, not used yet
// (the Mods menu uses it, or first says what it would override).
Installed install_file(int mod_id, int file_id, const std::string& url, const std::string& file_name,
                       const std::string& title, const std::string& expected_sha256 = {},
                       int64_t expected_size = -1, const std::string& stage = {},
                       const std::string& stage_file = {}, bool effect = false, bool use_effect = true);
// The stage slot (disc file, "GrNLa.dat") a stage file goes to when its own name is not a disc file's:
// the disc name it starts with ("GrNLaWaffle.dat"), else the card's stage ("Final Destination") when it
// names one slot. Empty when neither does, or when the name is a disc file's own. Exposed for tests.
std::string stage_slot_for(const std::string& file_name, const std::string& stage);
// The same for a card with no download of its own (a file of a mod uploaded through the site: most
// mods since early 2026). Fetched as the site's "Add to Nucleus" button has the SSBM Nucleus desktop
// app fetch it (nucleus://import?url=...): the site's per-file export,
//   GET https://ssbmnucleus.net/api/posts/<mod_id>/export-nucleus/<file_id>
// (export_url below; `url` is the card's own export_url from the API, taken only when it is exactly
// that address, or empty), counted as a download of the mod: an install never sends the view parameter
// an opponent's skin is fetched with (skin_fetch.h kViewQuery; an address with it is refused here,
// as any other). It answers with a small zip of that
// card's DAT ("PlCaNrMod.dat"; a stage's "GrNBa.dat"), its portrait "csp.png" and stock icon
// "stc.png"; for the Ice Climbers both halves with "<code>_csp.png" and "<code>_stc.png" each. At
// most 32 MB and 16 files, it is imported as a zip by the same importer as every skin. The card's own
// DAT is the one of its slot (PlXxYy in `card_file_name`), else the only one; the hook is told its
// asset, the ids, the DAT and the export zip's SHA-256 (and the other half's, under
// `partner_file_id`, when there is one). `bundled` is set when the export brought the other half.
// The DAT in the export is the card's file renamed, so when the card gives its SHA-256
// (`expected_sha256`, 64 hex digits) a DAT that differs is not installed (`mismatch` set).
// The mod's whole zip is never read: the cards' files sit in it at paths of their own, often in
// nested zips. Refused while set_blocked. Worker threads only.
std::string export_url(int mod_id, int file_id);
// `effect`: an effects card's export (its file under its own name and "preview.<ext>"): that file
// through host::cosmetics::import_effect, and a PNG preview as the effect's picture.
Installed install_export(int mod_id, int file_id, const std::string& url, const std::string& card_file_name,
                         const std::string& title, int partner_file_id = 0, bool* bundled = nullptr,
                         const std::string& expected_sha256 = {}, bool effect = false, bool use_effect = true);

}  // namespace host::mod_browser
