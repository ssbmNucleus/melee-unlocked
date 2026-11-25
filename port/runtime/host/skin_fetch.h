// Opponents' skins online: one skin file fetched by its SSBM Nucleus ids, as an opponent named it.
//
// fetch_skin(mod_id, file_id, sha256) gets the file the way the mod browser installs it: it reads the
// post (<api>/mods/<mod>, one small GET) for that file's own download_url (the API's
// <api>/mods/<mod>/download?file=<file>, asked for with &view=1), else the site's per-file export of
// exactly that file (<site>/api/posts/<mod>/export-nucleus/<file>?view=1); either way a view, not
// counted as a download of the post (see kViewQuery), and downloads it through
// the mod browser's GET (host/mod_browser.h: HTTPS to ssbmnucleus.net and its subdomains only; the
// loopback stand-in only in a run started by automation), at most kMaxSkinBytes. The hash the
// opponent sent is the costume file's own (record_origin: the DAT that was installed), so the
// download passes when it is that file, or a zip (the export) holding a DAT of exactly that hash,
// which is then the file taken; nothing reads the bytes before that check. A file that passes is
// kept under its hash in a cache of its own (<settings dir>/OpponentSkins/<sha256>.skin), apart from
// the installed mods (CosmeticMods): it is never imported, offered on the character select or shown
// in the Mods tab. A cached file is hashed again each time it is used. What is in it is for
// host/opponent_skins.cpp to judge.
//
// fetch_stock_icon(mod_id, file_id, sha256) gets that skin's own stock icon the same way: the card's
// stock_url on the same post (what the mod browser keeps with the skin when it installs it, so the
// sender's machine shows the same picture), through the same GET, never from the other player. It is
// a picture of its own only when no other file of the post has the same address (the mod browser's
// judge_pictures withholds it then, and the sender's HUD shows the retail icon). A post is read once a
// run for both: fetch_skin's read notes every file's stock icon, so a skin fetched now costs one API
// request, not two; a cached skin's post is read for its icon only when the icon is not kept yet. The picture must be a
// PNG of at most kMaxStockBytes that the hardened reader decodes in full, at most kMaxStockSide pixels
// a side (the Mods menu's rule for a card's pictures). It is kept beside the skin, under the skin's
// hash: <sha256>.stock.png, or <sha256>.nostock (a text file, the reason) when the card has none or its
// picture is refused, so the post is not read again each match; a failed request keeps nothing.
//
// Blocking (network): worker threads only.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host::skins {

constexpr uint64_t kMaxSkinBytes = 8ull << 20;   // 8 MiB; the Vault's largest skin is 3.9 MB

struct Fetched {
  bool ok = false;
  bool from_cache = false;
  std::string path;            // the cached file (ok only)
  std::vector<uint8_t> bytes;  // its bytes, already checked against the hash (ok only)
  std::string reason;          // why not
};

// The cache folder: beside the settings file (configure) or any folder (tests).
void configure_cache(const std::string& settings_path_utf8);
void set_cache_folder(const std::string& folder_utf8);
std::string cache_folder();

// 64 lower-case hex digits.
bool valid_sha256(const std::string& text);
// The API's address of one post, in this run's API ("<api>/mods/<mod>").
std::string post_url(uint32_t mod_id);
// The view marker, on both routes a skin comes from. Only an opponent's skin being SHOWN sends it: the
// site serves the same bytes without counting a download of the post (an install from the Mods menu,
// mod_browser::install_export and install_file, never sends it and is refused an address that has it,
// so only installs count). Today's site ignores it on the export route and answers 400 to it on the
// download route; fetch_skin then asks that route once more without it (one log line), which the site
// counts, until the site takes it. Nothing is cached by address: the cache is keyed by the hash alone.
constexpr const char* kViewQuery = "?view=1";
constexpr const char* kViewParam = "view=1";
// The export address an opponent's skin is fetched from: this card's route (the card's own export_url
// when it is exactly that route on the site or the stand-in, bare or with kViewQuery, else the one
// built from the ids), with kViewQuery. Empty, with the reason, for any other address. No network.
std::string export_view_address(uint32_t mod_id, uint32_t file_id, const std::string& given, std::string* reason);
// The card's own download_url as an opponent's skin asks for it: with the view marker after its query
// ("&view=1"), or kViewQuery when it has none (before a '#'). An address that names a view already is
// taken as it is when that is view=1, else refused (empty, with the reason). Where it may lead is the
// mod browser's GET's rule (SSBM Nucleus's hosts only). No network.
std::string download_view_address(const std::string& download_url, std::string* reason);
// Where one file of a post is downloaded from (one GET of the post): its download_url with the view
// marker (download_view_address; `*unmarked` the card's address as it is, for a site that refuses the
// marker), else the export of exactly that file, export_view_address (*is_export, `*unmarked` empty).
// Empty, with the reason, when the post or file is not there or the address is refused.
std::string file_source(uint32_t mod_id, uint32_t file_id, bool* is_export, std::string* reason,
                        std::string* unmarked = nullptr);

Fetched fetch_skin(uint32_t mod_id, uint32_t file_id, const std::string& sha256);

// ---- the skin's own stock icon ----
constexpr uint64_t kMaxStockBytes = 4ull << 20;   // as the Mods menu's card pictures (kMaxPictureBytes)
constexpr int kMaxStockSide = 1024;

struct StockIcon {
  bool ok = false;           // `path` is the checked PNG
  bool none = false;         // the card has no stock icon of its own, or it was refused: the retail icon (kept)
  bool from_cache = false;
  std::string path;          // ok: <cache>/<sha256>.stock.png
  std::string reason;        // why not (none, or a failed request: nothing kept, tried again next time)
};
// The stock_url of one file of a post (the post as read this run, else one GET of it): empty with *none set when
// the card has none of its own (no address, one the reader does not take, or another file's too),
// empty with *none clear when the post could not be read (*reason).
std::string stock_source(uint32_t mod_id, uint32_t file_id, bool* none, std::string* reason);
// A stock icon's bytes pass: a PNG, at most kMaxStockBytes, 1..kMaxStockSide pixels a side from its
// header, and the whole picture decodes. *why when not.
bool valid_stock_png(const std::vector<uint8_t>& bytes, int* width, int* height, std::string* why);
StockIcon fetch_stock_icon(uint32_t mod_id, uint32_t file_id, const std::string& sha256);

}  // namespace host::skins
