// The Mods menu's host half, shared by its files: limits, types, the session's state and the helpers
// more than one file uses. mods_menu.h is the menu's interface; nothing outside mods_menu*.cpp includes this.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mods_menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#include "cosmetic_mods.h"
#include "costume_slots.h"
#include "fighter_package.h"
#include "host.h"
#include "http_client.h"
#include "menu_mods.h"
#include "mod_browser.h"
#include "new_stages.h"
#include "opponent_skins.h"
#include "skin_fetch.h"

#include "nlohmann/json.hpp"

namespace host::mods_menu {
namespace menu_internal {
using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr uint64_t kMaxPageBytes = 8u * 1024u * 1024u;   // a page of posts with every file of each
constexpr uint64_t kMaxPictureBytes = 4u * 1024u * 1024u;
constexpr int kMaxPictureSide = 256;          // what the stand shows; a CSP (136 x 188) stays as it is
constexpr int kScreenPosts = 10;              // Browse: posts (rows) per screen, as many as the list shows
constexpr int kCardsPerPage = 100;            // cards per request of /cards (the site's most)
constexpr int kRequestsPerScreen = 6;         // Browse: list requests one screen may make, at most
constexpr size_t kMaxStreamPosts = 2000;      // Browse: posts one list keeps for the session, at most
constexpr size_t kPicturesKept = 96;          // the session's pictures (about 100 KB each)
constexpr size_t kPictureQueue = 4;           // pictures asked for and not fetched yet: the newest few
constexpr size_t kMaxCardsPerPost = 400;      // a post's cards read, at most
constexpr size_t kPackPerPage = kMaxCardsPerPost;   // a pack's list is whole: one page (and "Install all")
// "Install all" in a pack's list: its first row. false leaves the row out (and change(7) refuses).
constexpr bool kInstallAll = true;
// Pictures ahead of the viewer (prefetch: the game says where the list's cursor is): the cursor's row
// first, then the rest of its screen, then the next screen; fetched by a few workers at once, the
// viewer's own picture always first in line. A post of a fighter's list (/cards) that is a pack is not
// fetched ahead: its picture needs the API (/mods/{id}), which is kept for what the player opens.
constexpr int kPictureWorkers = 3;
constexpr size_t kPrefetchQueue = 40;
constexpr int kPrefetchScreens = 2;
constexpr int kListLines = 10;                // the rows the game's list shows at once (mu_mods_gallery.c)
// A post's screenshot (a pack's picture; a stage's or an effect's): read up to 8 MB and at most 4096
// pixels a side and 8.5 million pixels in all, then made small here (fit) like every picture.
constexpr uint64_t kMaxShotBytes = 8u * 1024u * 1024u;
constexpr int64_t kMaxPicturePixels = 3840LL * 2200LL;
// The picture files read from the site, kept on disk between runs (ModsMenuPictures beside the
// settings), 64 MB at most, the least recently used going first. A copy is used as it is for a day,
// then asked for again with its ETag (If-None-Match: a 304 keeps it).
constexpr uint64_t kDiskCacheBytes = 64u * 1024u * 1024u;
constexpr auto kDiskFresh = std::chrono::hours(24);

struct Card {
  int mod_id = 0, file_id = 0, downloads = 0;   // file_id: the card's `id` (the file's id in its mod)
  int64_t size = -1;                             // the file's size and SHA-256 when the site has read it
  std::string sha256;
  std::string title, description, author, creator, character, color, stage, file_name, download_url, asset_id, kind;
  std::string export_url;              // the site's per-file export, when the API gives it (else built from the ids)
  std::vector<std::string> pictures;   // addresses to try, best first
  std::string csp_url, stock_url;      // the file's own portrait and stock icon on the site, when it has them
  std::string local_picture;           // Manage: the installed skin's own validated PNG
  std::string local_stock;             // Manage: the installed skin's own stock icon (a validated PNG), shown beside it
  bool saved = false;                 // Manage: the details below were kept at install (works offline)
  bool shares_portrait = false;        // its picture is also another file's of the post: it may not show this one
  // The site's portrait (stock icon) is the same address as another file's of its post, a file that is
  // not this one: it is not kept with the skin, and the viewer says the picture may differ. A picture
  // of its own address is kept, whatever portrait_shared says (judge_pictures).
  bool withhold_csp = false, withhold_stock = false;
  bool same_colour = false;            // its pack has another card of this fighter and colour (its row says the variant)
  bool hidden_duplicate = false;       // hidden on the site as a duplicate portrait (kListHiddenDuplicates)
  std::string stage_file;              // /cards: the disc file a stage card replaces, as the site's export names it
  int mod_cards = 0;                   // /cards: its mod's cards, as the site counts them (0 unknown)
  bool multiple = false;               // its post has other cards in this list
  std::string variant;                 // its file's folder, when its post has another card of the same slot
  std::string label;                   // its row in its pack's list: what tells it from the pack's other cards
  std::vector<Card> partners;          // Ice Climbers: Nana's file, installed with Popo's
};

// Browse: one post of the site, one row; a post with several cards in this list is a pack.
struct Post {
  int mod_id = 0, downloads = 0;
  std::string title, author, creator;
  std::vector<Card> cards;             // in the game's roster order, then colour
  int count = 0;                       // its skins as its row says them ("5 skins"): more than 1 is a pack
  int hidden_more = 0;                 // a post of /mods: its cards the site hides as duplicate portraits
                                       // (learnt from /cards/{id} when its one card is viewed)
  // A pack's own page in the viewer: the post's words and its picture, its first screenshot (from
  // /mods; a post of /cards learns them from /mods/{id} when its page is opened), else its first card's.
  std::string description;
  std::vector<std::string> shots;
  bool shots_known = false;
};

// Browse's posts of one list (category, fighter, search) in the site's order, read a request at a time
// and shown kScreenPosts at a time: "All fighters" and Stages from /mods (one post per item), a
// fighter's list from /cards grouped by post (/mods's character filter misses the alias spellings).
struct Stream {
  std::vector<Post> posts;             // their cards as read (finished per screen: finish_post)
  std::string next_cursor;
  bool done = false;
  bool from_cards = false;
  int total = -1;                      // the site's total: posts (/mods) or cards (/cards)
  int requests = 0;
};

struct Page {
  int status = kLoading;
  std::string message;
  std::vector<Post> posts;   // Browse
  std::vector<Card> cards;   // Manage
  int total = -1;            // rows (posts, or installed skins) in the whole list, -1 unknown
  int per_page = kScreenPosts;    // what `total` counts per page
  bool from_cards = false;   // Browse read the site's /cards (else /mods)
  bool has_next = false;
  bool slow = false;         // an error because the site did not answer in time
};

struct Picture {
  int status = kLoading;     // kReady: `archive` holds it; kError: none could be read (a plain tile is served)
  std::vector<uint8_t> archive;
  uint32_t generation = 0;
  std::chrono::steady_clock::time_point used{};
  bool retry = false;        // kError because the site did not answer: asked again after a while
  bool fetching = false;     // its job left the queue: the worker is on it (not to be asked for twice)
  // For the log: when the viewer first asked for it, and whether its hand-over to the game was logged.
  std::chrono::steady_clock::time_point asked{};
  bool shown_logged = false;
  // When the viewer opened its row (each opening): the log's time from opening to the picture handed over.
  std::chrono::steady_clock::time_point viewed{};
};

struct Job {
  // kEffectUse: a held effect used (card.asset_id); kEffectSet: one move of the effect file `key` from
  // card.asset_id ("" the disc's own), fx_slot the move (mods_menu_effect_slots.cpp).
  enum Kind { kPage, kPicture, kInstall, kRemove, kPack, kCardInfo, kCounts, kEffectUse, kEffectSet } kind = kPage;
  std::string key;   // page key (kPack: pack key; kPicture: the picture's key, card_key or a post's)
  std::string fx_slot;
  Card card;
  int batch = 0;     // an install of "Install all": its batch
  bool prefetch = false;   // kPicture: asked for ahead of the viewer (dropped first, and when the cursor moves)
  Post post;               // kPicture of a pack's own page: its post (post_picture_key)
  bool of_post = false;
};

struct Progress { int state = kNotInstalled; std::string message; bool slow = false; };

// "Install all" in a pack: its cards one after another, with the same checks as one.
struct Batch {
  int id = 0, mod_id = 0, total = 0, done = 0, failed = 0, skipped = 0;
  bool running = false;
};

// Three queues, each with its own worker, so a slow list page, a picture or an install never holds
// up the others: pages (the one on screen), pictures (the newest few asked for), installs and removals.
struct Queue {
  std::deque<Job> jobs;
  std::condition_variable wake;
  bool started = false;
};

// The site's newer /cards (docs/public-api.md, GET /cards and "Packs and duplicate portraits"): mod_id
// one post's cards, character a fighter's, q a search. A site that answers 400 to those has Browse read
// /mods as before.
enum CardsApi { kCardsUnknown = 0, kCardsYes = 1, kCardsNo = 2 };
// The cards the site hides as duplicate portraits (most are one skin uploaded once per colour slot,
// from when only some slots could be replaced) stay hidden here too, the owner's choice: a pack is its
// post's visible cards, counted as mod_card_count, and "Install all" installs those. true lists them
// again (include_duplicates=1 on every /cards request, mod_card_count_with_duplicates as the count).
// Skins installed from such a card earlier stay installed either way.
constexpr bool kListHiddenDuplicates = false;
// A pack's cards read from /cards?mod_id= (Browse from /cards only), by pack key.
struct PackList {
  int status = kLoading;
  Post post;
};
// The fighters' lists' counts (Browse Characters and Effects): every fighter's at once, one request
//   <api>/cards/counts?type=costume|effects  ->  {"data": {"<fighter>": n, ...}, "total": n}
// (the cards /cards lists without include_duplicates; the site's docs/public-api.md), asked for when the
// category's fighters' list is first shown, kept for the session. The list shows them only once they
// are all in, and none at all when the site has no such list or did not answer (asked again a minute
// later, when the list is shown, after a failure the site may get over: no answer, 429, 5xx).
struct Counts {
  int status = kLoading;
  std::map<std::string, int> by_fighter;   // fighter_key -> cards
  int total = 0;
  bool retry = false;
  std::chrono::steady_clock::time_point when{};
};

// The menu's categories (the game's row under Browse and Installed): what the site calls each
// (docs/public-api.md: a card's type and a /mods file's file_type) and the catalog kind each installs as.
// Fighters (the game's fourth row, mods_menu_fighters.cpp): custom characters, installed as fighter
// packages into the run's fighters folder.
// Menus (the game's sixth row, mods_menu_menus.cpp): menu screens (host/menu_mods.h), the site's
// type=menu (its posts with a menu screen file), installed as menu_visual into host::menu_mods.
// New Stages (the fifth row, mods_menu_new_stages.cpp): stages of their own (host/new_stages.h), the
// site's type=custom_stage, installed as new_stage into host::new_stages.
enum Category { kCharacters = 0, kStages = 1, kEffects = 2, kFighters = 3, kNewStages = 4, kMenus = 5 };
// Browse's rows: a post with one card is that card; a post with more is a pack. Inside a pack: an
// "Install all" row, then its cards (kPackPerPage at a time).
enum RowKind { kRowCard = 0, kRowPack = 1, kRowInstallAll = 2 };
struct ViewRow {
  int kind = kRowCard;
  const Card* card = nullptr;
  const Post* post = nullptr;
};
struct MenuStage { const char* target; const char* name; const char* site; };
// (the site files Dream Land as "Dreamland"; the menu font has no é)
inline constexpr MenuStage kMenuStages[] = {
    {"GrSt.dat", "Yoshi's Story", "Yoshi's Story"},     {"GrIz.dat", "Fountain of Dreams", "Fountain of Dreams"},
    {"GrPs.dat", "Pokemon Stadium", "Pokemon Stadium"}, {"GrNBa.dat", "Battlefield", "Battlefield"},
    {"GrNLa.dat", "Final Destination", "Final Destination"}, {"GrOp.dat", "Dream Land", "Dreamland"}};

// ---- the session's state (defined in mods_menu.cpp, in the order the one file had them) ----
extern std::mutex g_mutex;
extern Queue g_page_queue, g_picture_queue, g_install_queue;
extern std::atomic<bool> g_blocked;
extern int g_mode, g_category, g_filter, g_page;
extern int g_pack;
extern int g_pack_page;
extern int g_more;
extern bool g_picking;
extern int g_last_fighter;
extern std::map<std::string, Page> g_pages;
extern std::map<std::string, Stream> g_streams;
extern std::map<int, int> g_card_info;
extern std::string g_last_opened;
extern std::map<std::string, Picture> g_pictures;
extern std::map<int, Progress> g_progress;
extern std::map<std::string, Progress> g_removals;
extern std::vector<std::pair<std::string, FileKind>> g_republish;
extern std::map<int, std::string> g_installed;
extern bool g_installed_known;
extern uint32_t g_picture_generation;
extern Batch g_batch;
extern std::atomic<int> g_cards_api;
extern std::string g_query;
extern int g_query_id;
extern std::map<int, std::string> g_queries;
extern std::map<std::string, PackList> g_pack_lists;
extern std::map<int, Counts> g_counts;
extern std::mutex g_details_mutex;
extern std::mutex g_disk_mutex;
extern uint64_t g_disk_bytes;
extern bool g_disk_counted;
extern std::atomic<bool> g_offline;
extern std::string g_fighters_folder;
extern std::map<int, std::string> g_fighter_installed;

// ---- mods_menu_categories.cpp ----
int category_of(int c);
const char* site_type(int c);
const char* file_type(int c);
const char* asset_kind(int c);
const char* category_name(int c);
bool by_fighter(int c);
std::string things(int c, int n);

// ---- mods_menu_text.cpp ----
std::string one_line(const std::string& text, size_t room);
void put(std::vector<uint8_t>& out, const std::string& text, size_t width);
void put_u16(std::vector<uint8_t>& out, int v);
void put_u32(std::vector<uint8_t>& out, uint32_t v);
std::string thousands(int n);
std::string counted(int n, const char* word);
std::string row_counted(int n, const char* word);

// ---- mods_menu_list.cpp ----
std::string list_key(int mode, int category, int filter);
std::string page_key(int mode, int category, int filter, int page);
std::string pack_key(int category, int filter, int mod_id);
std::string card_key(const Card& c);
Page& current_locked();
const Post* pack_locked(const Page& page);
std::vector<ViewRow> rows_locked(const Page& page);
void prefetch_screen_locked(int row, int line);
int state_locked(const Card& card);
std::string row_name(const std::string& text);

// ---- mods_menu_cards.cpp ----
int number_of(const json& j, const char* key);
std::string text_of(const json& j, const char* key, size_t max = 2048);
std::string log_host(const std::string& url);
std::string encode(const std::string& s);
std::string lower(std::string s);
bool picture_address(const std::string& url);
void add_picture(Card& card, const std::string& url);
void cards_of_post(const json& post, int category, const std::string& character, std::vector<Card>& out);
std::string stem_of(const Card& c);
void post_words_of(const json& item, Post& post);
bool card_of(const json& j, int category, Card& c);
void finish_post(Post& post, int category);
bool stream_add(const json& j, int category, const std::string& character, Stream& s);
Page screen_of(const Stream& s, int category, int page, int* cards, int* packs, const Page* before = nullptr);

// ---- mods_menu_fetch.cpp ----
bool get_json(const std::string& url, json* out, int* status, std::string* error, host::mod_browser::NetResult* net = nullptr);
void fetch_page(const std::string& key);
void fetch_pack(const std::string& key);
void fetch_card_info(const Card& card, int category);
void learn_post_words_locked(int mod_id, const Post& words);
void forget_pages_locked();
Counts counts_of(const json& j, bool got, int status);
void fetch_counts(int category);

// ---- mods_menu_pictures.cpp ----
void fetch_picture(const Card& card, const Post* known = nullptr, bool prefetch = false);
void fetch_post_picture(const Job& job);
std::string post_picture_key(const Post& post);
Picture* want_row_picture_locked(int kind, const Card* card, const Post* post, bool prefetch);

// ---- mods_menu_install.cpp ----
std::vector<Card> scan_installed(int category, std::map<int, std::string>* by_file);
void repicture_installed(const std::vector<Card>& cards, const std::map<int, std::string>& by_file);
void attach_picture(const std::string& asset_id, const std::string& url, const char* kind, int file_id);
std::string install_name(const Card& card);
void start_locked(Queue& queue);
void queue_page_locked(const std::string& key);
void queue_pack_locked(const std::string& key);
void queue_card_info_locked(const Card& card, int category);
void want_counts_locked(int category);
void queue_install_locked(Job job);

// ---- mods_menu_characters.cpp ----
const std::vector<std::string>& characters();
std::string fighter_key(const std::string& name);
int roster_index(const std::string& character);
const std::vector<int>& roster_filters();
int roster_row(int filter);
std::string fighter_label(const std::string& name);
std::string base_of(const std::string& name);
std::string colour_text(const Card& c);
int colour_index(const Card& c);
int climber_of(const Card& c);
void pair_climbers(std::vector<Card>& cards);
std::string short_fighter(const std::string& name);
std::vector<Card> climbers_partners(const Card& card);

// ---- mods_menu_effects.cpp ----
void attach_effect_preview(const std::string& asset_id, const Card& card);
// An effect card the effect checks refused (broken, not_an_effect): its short reason, else nullptr. Such a
// card is not installable: A is refused, Install all skips it, its viewer shows the reason.
const std::string* effect_refused_locked(const Card& card);
void effect_refuse_locked(const Card& card, const std::string& verdict, const std::string& reason);

// ---- mods_menu_effect_slots.cpp: Effects by move ----
bool effects_open_locked();
int effects_change_locked(int op, int row);
bool effects_list_locked(int first, int count, std::vector<uint8_t>& reply);
bool effect_installed(const Card& card, const std::string& asset_id, std::string* target);
bool effect_held_locked(const Card& card);
bool effect_notice_locked(const Card& card, std::string* about);
bool effect_use_held_locked(const Card& card);
void effect_removed_locked(const std::string& asset_id);
void run_effect_job(const Job& job);

// ---- mods_menu_stages.cpp ----
std::string stage_of_file(const std::string& file_name, const std::string& post_stage);
std::string stage_slot_of(const std::string& stage_file, std::string* name);
std::string stage_param(int category, int filter);
void narrow_stage_cards(std::vector<Card>& cards, int filter);
void narrow_stage_posts(std::vector<Post>& posts, int filter);
void stage_set_installed(const Card& card, const std::vector<std::pair<int, std::string>>& done, bool ok);
bool fetch_pool_page(const std::string& key, int mode, int category, int filter);
bool stages_open_locked();
int stages_change_locked(int op, int row);
bool stages_list_locked(int first, int count, std::vector<uint8_t>& reply);

// ---- mods_menu_fighters.cpp ----
int fighter_state_locked(const Card& card);
std::string fighter_what(const Card& c);
bool fighters_fetch_page(const std::string& key, int mode, int category);
bool fighters_run_install(const Card& card);
bool fighters_run_remove(const Card& card);

// ---- mods_menu_menus.cpp ----
bool menu_card_file(const std::string& file_name);
std::string mods_filter(int category);
bool menus_refused(int status);
void menus_installed(int category, std::vector<Card>& rows, std::map<int, std::string>* by_file);
std::string menu_what(const Card& c, bool in_pack);
std::string menu_replaces(const Card& c);
std::string menu_picture(const Card& card);
void run_menu_install(const Card& card, int batch);
void run_menu_remove(const Card& card);

// ---- mods_menu_new_stages.cpp ----
bool new_stage_card_file(const std::string& file_name, const std::string& post_stage);
void new_stages_installed(int category, std::vector<Card>& rows, std::map<int, std::string>* by_file);
std::string new_stage_what(const Card& c);
void run_new_stage_install(const Card& card, int batch);
void run_new_stage_remove(const Card& card);

}  // namespace menu_internal

using namespace menu_internal;   // as the one file's unnamed namespace was: its names are the menu's own

}  // namespace host::mods_menu
