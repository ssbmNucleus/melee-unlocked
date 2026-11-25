// The in-game Mods menu's host half (Trophies' place in the main menu; the game's half is
// sourceport/game/shim/mu_mods_menu.c and mu_mods_gallery.c, through CMD_MOD_BROWSER 8 to 14 in
// source_host_mods_menu.cpp).
//
// Browse lists the site's posts, one row each, most downloaded first, as one continuous list: ten
// are read first and ten more each time the player nears the end of what is read (one request at a
// time, in the background; at most 2,000 posts a list), from the site's docs/public-api.md:
//   GET <api>/mods?type=costume|stage_skin&sort=downloads&limit=10[&q=<words>][&cursor=<next_cursor>]
// and a fighter's list the site's cards of that fighter, grouped by post (/mods's character filter
// misses the alias spellings, "C. Falcon" files under "Captain Falcon"; /cards's matches them all):
//   GET <api>/cards?type=costume&limit=100&character=<name>[&q=<words>][&cursor=]
// The cards the site hides as duplicate portraits stay hidden (mods_menu.cpp kListHiddenDuplicates).
// A post's "cards" are its character_dat (or stage_dat) files, narrowed to the fighter shown: one
// card, and the row is that card (A: the viewer); more, and the row is a pack ("12 skins"; A: a second
// list of its cards, read from /cards?mod_id= only then, each labelled by fighter and colour (the
// colour its file's name says), with what tells its file from another of that colour ("C. Falcon - Red
// - HALO"), in the game's roster order; B back to the posts where they were; its first row "Install
// all"). Nothing is polled: a screen is fetched when the player opens it and kept for the session. Ice Climbers costumes are two cards (Popo's file and Nana's): one row,
// and installing it installs both. Pages, pictures and installs each have their own queue and
// worker, so one slow request holds up nothing else (mod_browser.h has the time limits and retry).
//
// Pictures: the card's portrait (csp_url), else its preview; else the mod's first screenshots (from
// /mods/{mod_id}; the cards' small thumbnails are WebP, which is not read here). Only PNG and JPEG, at
// most 4 MB and 4096 pixels a side (stb_image, private to mods_menu.cpp, told the size before it
// decodes), from SSBM Nucleus hosts only (mod_browser's GET). A picture goes to the game as a small
// HSD archive: one textured square the trophy stand shows. A character skin's stock icon (stock_url;
// in Manage the one kept with the skin) stands beside its portrait's lower right corner, in the same
// picture. The fighters' lists' counts ("245 skins") are one request for every fighter,
//   GET <api>/cards/counts?type=costume|effects
// shown only once all are in, and not at all when the site has none.
//
// Nothing is downloaded to show a card. Its file is downloaded only when the player installs it:
// through the card's own download_url (mod_browser::install_file, which refuses a file whose size or
// SHA-256 differs from the card's), else through the site's per-file export, the small zip the site's
// "Add to Nucleus" button has the desktop app fetch (mod_browser::install_export; the card's
// export_url, else built from its mod and file ids). The mod's whole zip is never read. And
// Manage lists the skins installed from SSBM Nucleus (their origin is kept, host/opponent_skins.h) and
// removes one (host::cosmetics::remove_asset).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace host::mods_menu {

enum Status { kLoading = 0, kReady = 1, kError = 2 };
enum CardState { kNotInstalled = 0, kDownloading = 1, kInstalled = 2, kFailed = 3, kRemoving = 4, kRemoved = 5 };
constexpr int kPerPage = 100;

// Text the game's menu font draws: letters, digits, space and . , ' " : - (a Latin letter with a mark
// becomes the letter, curly quotes and dashes the plain ones; other characters become a space or are
// left out), at most `max` bytes. Exposed for tests.
std::string drawable(const std::string& text, size_t max);
// A picture as the game takes it: an HSD archive with one joint, "mu_mods_picture_joint", a square
// one unit tall (and as wide as the picture's shape) standing on its lower edge, textured with the
// RGBA8 pixels (`width` x `height`, multiples of 4). Exposed for tests.
std::vector<uint8_t> picture_archive(const std::vector<uint8_t>& gx_pixels, int width, int height);

// Game thread, never blocking. mode 0 Browse, 1 Manage; category 0 Characters, 1 Stages, 2 Effects
// (the site's type=effects: a fighter's effect files, listed by fighter as Characters is, installed
// through host::cosmetics::import_effect). `again`: the same list back from the search's name entry,
// its fighter, page and search kept.
void open(int mode, int category, bool again = false);
// 0 (none: a list has no page before), 1 Browse's next posts at the end of the list (or reading them
// again after a failure; false while they are on their way or when there are no more), 2 previous
// character, 3 next character, 4 try again, 5 open the pack of `row`, 6 back from a pack to the posts,
// 7 "Install all" of the pack shown, 9 the fighters' list (Browse Characters opens on it: "All
// fighters" and each fighter in the game's roster order): the fighter of `row`, 10 back to the
// fighters' list (on the fighter shown; a search ends). A pack's list and Manage are whole lists.
// Effects (mods_menu_effect_slots.cpp) answers 9 and 10 a level at a time: Browse, a fighter's moves
// ("Falcon Punch - Honduran Falcon Punch", "All effects - ..."), 9 on one its effects (each installed
// one that changes it, "Standard", "Get more": the fighter's posts as before), 9 on an effect uses it
// for that move; Manage, the fighters with effects installed and "All installed" (the cards, to remove),
// a fighter's moves, 9 on one its next effect. 10 is one level back.
bool change(int op, int row = -1);
// Browse's search (the game's name entry): the posts whose title or creator has `words` (the site's
// q), from the first page; "" ends it. The words are kept to what the menu font draws, at most 24.
// False when nothing changed.
bool search(const std::string& words);
// CMD_MOD_BROWSER replies (source_host_mods_menu.cpp documents the layouts).
void list(int first, int count, std::vector<uint8_t>& reply);
void detail(int row, std::vector<uint8_t>& reply);
void picture(int row, std::vector<uint8_t>& reply);
// A pack's row in the viewer: its post's words (detail) and its own picture, the post's first
// screenshot, else its first card's portrait (picture). 0xFF / not ready for a row that is not a pack.
void pack_detail(int row, std::vector<uint8_t>& reply);
void pack_picture(int row, std::vector<uint8_t>& reply);
// Where the list's cursor is (its row, on line `line` of the screen): the pictures of the rows on
// screen and of the next screen are fetched ahead of the viewer, that row's first.
void prefetch(int row, int line);
// A in the viewer: install (Browse) or remove (Manage) that row. `file_id` (0: not checked) is the card
// the viewer showed (detail's last field): a row that is another card now is refused. False when refused.
// An effect installed over moves another effect gives is held unused, its words naming what it would
// override ("This will override" and a line per effect); A on it then (after the game's own "Press A
// again") uses it.
bool action(int row, int file_id = 0);
void set_blocked(bool blocked);
// The fighters folder of this run (Browse and Manage Fighters install into it and list it; empty: none,
// and the category says fighters are off).
void set_fighters_folder(const std::string& folder_utf8);
// The disc files to publish again, once each: a slot a removal took a selected skin from (a costume
// slot or a stage), and an effect file an install or a removal changed. The game thread publishes them
// (host::cosmetics::republish_slot / republish_stage / republish_effect), so the next match has the
// change without a restart.
// Menu: a menu mod installed or removed (host/menu_mods.h), whose screen's file is published again.
enum class FileKind { Costume, Stage, Effect, Menu };
std::vector<std::pair<std::string, FileKind>> take_republish();

// Tests (port/tests/mods_menu_fuzz_test.cpp): nothing goes to the network (no worker starts), and
// Browse's first page is read from a /mods reply `body` as if it had just been fetched. False when
// the body is not a list of posts (the page is then the error a bad reply gives).
void set_offline_for_test(bool offline);
bool load_browse_page_for_test(int category, int filter, const std::string& body);
// The next batch of the Browse list shown (the one change(1) asked for) as if the site had just
// answered `body`: appended to the list, as list() then shows it.
bool load_more_for_test(const std::string& body);
// A pack's own list as if /cards?mod_id=`mod_id` had just answered `body` (the list shown's category
// and fighter); the name a row's card would be installed under; whether its portrait (1) and stock icon
// (2) would be left out at install (another file's of its post).
bool load_pack_for_test(int mod_id, const std::string& body);
// The fighters' counts of `category` as if /cards/counts had just answered `status` with `body`; false
// when they are not a list of counts (the fighters' list then shows none).
bool load_counts_for_test(int category, int status, const std::string& body);
std::string install_name_for_test(int row);
int withheld_for_test(int row);

}  // namespace host::mods_menu
