/* What the files of the Mods menu's list and viewer (the Gallery's scenes, ty/tylist.c and ty/toy.c)
 * share; nothing outside them includes it. Their face to the game is include/mu_mods_gallery.h.
 *   mu_mods_gallery.c           the list: the host's rows, the window over them, open and close
 *   mu_mods_gallery_input.c     the list's keys: up and down, L and R, A on a row, B
 *   mu_mods_gallery_draw.c      a row's words and its box, the bar under the list
 *   mu_mods_gallery_viewer.c    the viewer: a card's or a pack's words, A to install or remove
 *   mu_mods_gallery_pictures.c  the picture on the stand, fetched and kept two at a time
 *   mu_mods_host.c              the requests to the host (MU_CMD_MOD_BROWSER 9 to 14)
 *   mu_mods_text.c              words into the game's text objects, in the letters its font draws
 * What one category does differently: mu_mods_categories.h.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MODS_GALLERY_INTERNAL_H
#define MU_MODS_GALLERY_INTERNAL_H

struct HSD_Text;

enum { SUB_OPEN = 9, SUB_LIST, SUB_CHANGE, SUB_DETAIL, SUB_PICTURE, SUB_ACTION };
enum { REPLY_MIN = 4096 };   /* the host's least reply buffer (MU_SLIPPI_RESPONSE_CAPACITY) */

enum { ST_LOADING, ST_READY, ST_ERROR };
enum { CARD_NOT_INSTALLED, CARD_DOWNLOADING, CARD_INSTALLED, CARD_FAILED, CARD_REMOVING, CARD_REMOVED };
/* A row of the host's list is a card (A: the viewer), a pack (a post of several cards; A: its own page in
 * the viewer, its post's picture and words, and A there: its list), a
 * pack's "Install all" or a fighter of the fighters' list; the others are the shim's own (Browse's
 * "Search" and, while a search is shown, the row that ends it; "Loading..." or "Could not load more"
 * at the end of a list still being read). */
enum { ROW_CARD, ROW_MESSAGE, ROW_PACK, ROW_INSTALL_ALL, ROW_SEARCH, ROW_END_SEARCH, ROW_MORE, ROW_FIGHTER };
enum { FLAG_FIGHTERS = 1, FLAG_AFTER = 2, FLAG_FILTER = 4, FLAG_PACK = 8, FLAG_SLOW = 16, FLAG_SEARCH = 32,
       FLAG_MORE_LOADING = 64, FLAG_MORE_FAILED = 128 };
enum { CHANGE_NEXT = 1, CHANGE_FIGHTER_BACK, CHANGE_FIGHTER_ON, CHANGE_RETRY, CHANGE_OPEN_PACK,
       CHANGE_LEAVE_PACK, CHANGE_INSTALL_ALL, CHANGE_SEARCH, CHANGE_PICK_FIGHTER, CHANGE_FIGHTERS };
/* The host's rows the list holds at once (from `mu_mods_base`), how far the window moves, how near its edge
 * the cursor gets before it does, and how near the end of what is read before more is asked for (a
 * screen: never more than one batch ahead of the cursor). */
enum { WINDOW = 240, SLIDE = 120, EDGE = 40, AHEAD = 10 };
enum { MAX_ROWS = WINDOW + 3 };
enum { HEAD_BYTES = 124, NAME = 40, RIGHT = 12, ROW_BYTES = NAME + RIGHT + 1, PER_ASK = 60 };
enum { LIST_POLL = 15, VIEW_POLL = 8 };
enum { LIST_LINES = 10 };      /* the rows the list shows at once */

typedef struct Head {
    unsigned char status, flags;
    int cards;                  /* the host's rows */
    int start;                  /* the row a list built anew starts on (the fighters': the last chosen) */
    int generation;             /* the host's list the rows come from (the lobby list's; Mods: 0) */
    char filter[20], message[48], title[48];
} Head;

typedef struct Row {
    unsigned char kind, state;
    short card;
    char name[NAME], right[RIGHT];
} Row;

/* ---- mu_mods_gallery.c: the list ---- */
extern int mu_mods_manage, mu_mods_category;    /* Browse (0) or Manage (1); the category (mu_mods_categories.h) */
extern int mu_mods_active;              /* the menu's Gallery is open (set at open, cleared at the menu) */
extern int mu_mods_open_on_list;        /* the viewer's first frame goes to the list */
extern int mu_mods_fresh;               /* the list's next build starts at mu_mods_start_row, on line mu_mods_start_line */
extern int mu_mods_start_row, mu_mods_start_line;
extern int mu_mods_pack_row, mu_mods_pack_line, mu_mods_pack_base; /* where the posts' list was when a pack was opened: B goes back */
extern int mu_mods_leave_for_search;    /* the viewer's A on "Search": the list leaves for the name entry */
extern Head mu_mods_head;
extern Row mu_mods_rows[MAX_ROWS];
extern int mu_mods_row_count;
extern int mu_mods_base;                /* the host's row the list's window starts at (a multiple of SLIDE) */
extern int mu_mods_top;                 /* the shim's own rows above the host's ("Search", "Show all"): mu_mods_base 0 only */
extern int mu_mods_asked_more_at;       /* the host's row count when more was last asked for (once each) */
extern int mu_mods_list_tick;
int mu_mods_gallery_running(void);
int mu_mods_same_layout(const Head* a, const Head* b);
int mu_mods_same_list(const Head* a, const Head* b);
int mu_mods_poll_head(Head* h);
int mu_mods_sync_rows(int recount, int* differ);
int mu_mods_host_row(int row);
int mu_mods_changed_at(int row, int line);
int mu_mods_changed(void);
int mu_mods_moved_to(int h, int selected, int line);

/* ---- mu_mods_gallery_input.c: the list's keys ---- */
int mu_mods_row_action(const Row* r, int selected, int line);

/* ---- mu_mods_gallery_viewer.c: the viewer ---- */
typedef struct View {
    int row, tick, redraw;
    int card;                       /* the card of the words below; -1 none */
    unsigned char state, pic_status;
    unsigned int pic_generation;
    unsigned int file_id;           /* the site's id of the card shown (A sends it back); 0 unknown */
    char name[48], what[48], author[48], downloads[48], line[48], about[400];
    int pack;                       /* `card` is a pack's row (its post's words and picture) */
    char count[48];                 /* a pack's: "6 skins" */
    int confirm;                    /* the host asks the first A to only ask (a menu mod replacing another) */
} View;
extern View mu_mods_view;
extern int mu_mods_remove_asked;        /* Manage: the row whose first A asked "Press A again to remove."; -1 none */
extern int mu_mods_replace_asked;       /* Browse Menus: the row whose first A asked "Press A again to replace it." */
extern int mu_mods_viewer_line;         /* the list's line the viewer was opened from */
void mu_mods_spinner_gone(void);

/* ---- mu_mods_gallery_pictures.c: the picture on the stand ---- */
extern int mu_mods_hint_row;            /* the host's row the host was last told the cursor is on; -2: tell it again */
extern int mu_mods_pic_card[2];
extern int mu_mods_pic_shown;           /* the buffer the stand shows now; -1 the tile */
void mu_mods_hint_prefetch(int h, int selected, int line);
void mu_mods_want_picture(int row);

/* ---- mu_mods_host.c: the requests ---- */
extern unsigned char mu_mods_reply[REPLY_MIN];
int mu_mods_ask(const unsigned char* payload, unsigned int n, unsigned char* out, unsigned int capacity,
                unsigned int* got);
int mu_mods_u16_at(const unsigned char* p);
unsigned int mu_mods_u32_at(const unsigned char* p);
int mu_mods_host_change_row(int op, int row);
int mu_mods_host_change(int op);

/* ---- mu_mods_text.c: the words ---- */
char mu_mods_drawable(char c);
void mu_mods_field(char* out, const unsigned char* in, int width);
void mu_mods_set_words(struct HSD_Text* text, const char* message);
int mu_mods_wrap(char* out, int at, int cap, const char* in, int width, int max_lines);
void mu_mods_middle_cut(char* out, const char* in, int room);
void mu_mods_fit_name(struct HSD_Text* text, const char* title);

#endif
