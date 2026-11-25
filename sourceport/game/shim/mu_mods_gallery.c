/* The Gallery's scenes as the Mods menu's list and viewer (shim/mu_mods_menu.c opens them).
 *
 * The Trophies Gallery runs as it is: its list screen (ty/tylist.c) is the list of mods and its trophy
 * viewer (ty/toy.c) shows one, on the trophy stand. What changes is what it reads:
 *   - the save's trophy count and flags (gm/gmmain_lib.c) are a copy this file owns while the menu runs
 *     the Gallery: one "trophy" per row of the list, so the scenes sort, scroll and page as they do,
 *     and nothing they write (the "new" marks, the visited flag) reaches the save;
 *   - each row's words are the mod's (name, then a pack's count or "Installed" where the count was;
 *     a row with nothing there has no box there either);
 *   - the viewer's model is the mod's picture, a flat square on the stand (an HSD archive the host
 *     builds from the picture: port/runtime/host/mods_menu.cpp picture_archive), and its words are
 *     the mod's name, what it is, who made it, its downloads and what A does;
 *   - A in the viewer installs the mod (Browse) or removes it (Manage: the first A asks, "Press A
 *     again to remove.", a second removes);
 *   - Browse Characters opens on the fighters ("All fighters", then each fighter in the roster's
 *     order, on the one chosen last); A on one lists its posts, B there goes back to the fighters,
 *     and L and R step to the fighter before or after (the bar names it, "L R: Fighter" at its left);
 *   - every list is one continuous list: up and down move a row, left and right (stick or D-pad) a
 *     screen, X and Y as the game's own jumps. Browse reads more posts in the background as the
 *     cursor nears the end of what it has ("Loading..." at the end meanwhile, "Could not load more"
 *     with A to try again), and goes round from its last row to its first, with the game's red line
 *     between them, only once it has them all; till then a move stops at its ends. A pack's list,
 *     Manage and the fighters are whole lists and go round as the trophy list does. The game's list
 *     holds at most TY_TROPHY_COUNT rows, so a longer list is a window of the host's rows that moves
 *     with the cursor (WINDOW rows, SLIDE at a time);
 *   - Browse's first row, "Search" ("Search: <words>" while a search is shown, and "Show all" under
 *     it), leaves for the game's name entry (mn/mnnamenew.c, nothing of it saved), whose words the
 *     host looks for; the list then opens again where it was.
 * All data comes from the host (MU_CMD_MOD_BROWSER 9 to 14, source_host.cpp), which never blocks: what
 * it does not have yet is "Loading...". Words are mapped once more to the letters the game's font
 * draws, so nothing from the site reaches the text encoder unchecked.
 * This file is the list itself: the host's rows, the window over them, open and close. The keys, the
 * drawing, the viewer, the pictures, the requests and the words are beside it, one file each
 * (mu_mods_gallery_internal.h), and what one category does differently is in that category's file
 * (mu_mods_categories.h).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/ty/forward.h>

#include <mu_mods_gallery.h>
#include <mu_native.h>

#include "mu_mods_categories.h"
#include "mu_mods_gallery_internal.h"

enum { KIND_PACK = 0x40, KIND_INSTALL_ALL = 0x80, STATE_BITS = 0x3F };
typedef char mu_window_fits[MAX_ROWS <= TY_TROPHY_COUNT ? 1 : -1];

int mu_mods_manage, mu_mods_category;
int mu_mods_active;              /* the menu's Gallery is open (set at open, cleared at the menu) */
int mu_mods_open_on_list;        /* the viewer's first frame goes to the list */
int mu_mods_fresh;          /* the next build starts at mu_mods_start_row, on line mu_mods_start_line */
int mu_mods_start_row, mu_mods_start_line;
int mu_mods_pack_row, mu_mods_pack_line, mu_mods_pack_base; /* where the posts' list was when a pack was opened: B goes back */
int mu_mods_leave_for_search;    /* the viewer's A on "Search": the list leaves for the name entry */
Head mu_mods_head;
Row mu_mods_rows[MAX_ROWS];
int mu_mods_row_count = 1;
int mu_mods_base;                /* the host's row the list's window starts at (a multiple of SLIDE) */
int mu_mods_top;                 /* the shim's own rows above the host's ("Search", "Show all"): mu_mods_base 0 only */
int mu_mods_asked_more_at = -1;  /* the host's row count when more was last asked for (once each) */
int mu_mods_list_tick;

static struct {
    s16 count;
    u16 category;
    u16 flags[TY_TROPHY_COUNT];
} shadow;
static int shadow_category_set;

/* ---- the save's trophy data, as the menu's copy ---- */

int mu_mods_gallery_running(void)
{
    return mu_mods_active && mu_mods_gallery() && gm_GetCurrentGameMode() == GM_TOY_GALLERY;
}

void* mu_mods_trophy_data(int which, void* real)
{
    if (!mu_mods_gallery_running()) return real;
    if (which == 1) {
        if (!shadow_category_set && real != NULL) {
            shadow.category = *(u16*) real;
            shadow_category_set = 1;
        }
        return &shadow.category;
    }
    return which == 2 ? (void*) &shadow.count : (void*) shadow.flags;
}

static void commit(void)
{
    int i;
    shadow.count = (s16) mu_mods_row_count;
    for (i = 0; i < TY_TROPHY_COUNT; i++) shadow.flags[i] = i < mu_mods_row_count ? 1 : 0;
}

/* ---- the list ---- */

static int read_head(Head* h, const unsigned char* p)
{
    h->status = p[0];
    h->cards = mu_mods_u16_at(p + 1);
    h->start = mu_mods_u16_at(p + 3);
    h->generation = mu_mods_u16_at(p + 5);
    h->flags = p[7];
    mu_mods_field(h->filter, p + 8, 20);
    mu_mods_field(h->message, p + 28, 48);
    mu_mods_field(h->title, p + 76, 48);
    return 1;
}

/* The same rows (in number and kind): only their words may differ. */
int mu_mods_same_layout(const Head* a, const Head* b)
{
    return a->status == b->status && a->cards == b->cards &&
           (a->flags & ~FLAG_SLOW) == (b->flags & ~FLAG_SLOW) && strcmp(a->filter, b->filter) == 0 &&
           strcmp(a->title, b->title) == 0;
}

static int same_head(const Head* a, const Head* b)
{
    return mu_mods_same_layout(a, b) && a->flags == b->flags && a->generation == b->generation &&
           strcmp(a->message, b->message) == 0;
}

/* The same list, read further (more rows, "Loading..." at its end or not): the cursor stays on its row. */
int mu_mods_same_list(const Head* a, const Head* b)
{
    const int kind = FLAG_FIGHTERS | FLAG_FILTER | FLAG_PACK | FLAG_SEARCH;
    return a->status == ST_READY && b->status == ST_READY && (a->flags & kind) == (b->flags & kind) &&
           a->cards >= b->cards && strcmp(a->filter, b->filter) == 0 && strcmp(a->title, b->title) == 0;
}

int mu_mods_poll_head(Head* h)
{
    unsigned char p[4] = {SUB_LIST, 0, 0, 0};
    unsigned int got;
    return mu_mods_ask(p, 4, mu_mods_reply, sizeof mu_mods_reply, &got) && got >= HEAD_BYTES && read_head(h, mu_mods_reply);
}

static void own_row(Row* r, int kind, const char* words)
{
    memset(r, 0, sizeof *r);
    r->kind = (unsigned char) kind;
    r->card = -1;
    strncpy(r->name, words, NAME - 1);
}

static void message_row(Row* r, const char* words) { own_row(r, ROW_MESSAGE, words); }

/* The row at the end of a list still being read: the host's row `card` is the next one to come. */
static void more_row(Row* r, const Head* h, int card)
{
    own_row(r, ROW_MORE, (h->flags & FLAG_MORE_FAILED) ? "Could not load more - A: Try again" : "Loading...");
    r->card = (short) card;
}

/* "Search", or "Search: <words>" while a search is shown (the words are the filter's quoted part). */
static void search_row(Row* r, const Head* h)
{
    const char* q = (h->flags & FLAG_SEARCH) ? strchr(h->filter, '"') : NULL;
    own_row(r, ROW_SEARCH, "Search");
    if (q != NULL && q[1] != 0) {
        int n = (int) strlen(r->name);
        r->name[n++] = ':';
        r->name[n++] = ' ';
        for (q++; *q && *q != '"' && n < NAME - 1; q++) r->name[n++] = *q;
        r->name[n] = 0;
    }
}

/* The list as the host has it now, into `out`, from the host's row *from (a window of at most WINDOW
 * of them; *from is set back to 0 when the list has no such row); its row count, 0 when the host did
 * not answer. `own`: the shim's rows before the host's. Browse's posts (not a pack's cards, nor
 * Manage, nor the fighters) start with "Search", and while a search is shown, the row that ends it. */
static int fetch_rows(Head* h, Row* out, int* from, int* own)
{
    int n = 0, first, end;
    *own = 0;
    if (!mu_mods_poll_head(h)) return 0;
    if (h->status != ST_READY) {
        *from = 0;
        message_row(&out[0], h->status == ST_LOADING ? "Loading..." :
                             h->message[0] ? h->message : "Could not reach SSBM Nucleus.");
        return 1;
    }
    if (*from < 0 || *from >= h->cards) *from = 0;
    if (*from == 0 && !mu_mods_manage && !(h->flags & (FLAG_PACK | FLAG_FIGHTERS))) {
        search_row(&out[n++], h);
        if (h->flags & FLAG_SEARCH) own_row(&out[n++], ROW_END_SEARCH, "Show all");
    }
    *own = n;
    if (h->cards == 0) {
        /* Nothing yet: more on its way, or "Nothing here yet." */
        if (h->flags & FLAG_AFTER) more_row(&out[n++], h, 0);
        else message_row(&out[n++], h->message[0] ? h->message : "Nothing here yet.");
        return n;
    }
    end = h->cards - *from > WINDOW ? *from + WINDOW : h->cards;
    for (first = *from; first < end;) {
        unsigned char p[4];
        unsigned int got;
        int i, count = end - first < PER_ASK ? end - first : PER_ASK;
        Head again;
        p[0] = SUB_LIST;
        p[1] = (unsigned char) (first >> 8);
        p[2] = (unsigned char) first;
        p[3] = (unsigned char) count;
        if (!mu_mods_ask(p, 4, mu_mods_reply, sizeof mu_mods_reply, &got) || got < HEAD_BYTES + (unsigned int) count * ROW_BYTES ||
            !read_head(&again, mu_mods_reply) || !mu_mods_same_layout(&again, h) || again.generation != h->generation)
        {
            return 0;   /* the list changed under us: the next poll reads it whole */
        }
        for (i = 0; i < count; i++) {
            const unsigned char* q = mu_mods_reply + HEAD_BYTES + i * ROW_BYTES;
            const unsigned char bits = q[NAME + RIGHT];
            Row* r = &out[n++];
            memset(r, 0, sizeof *r);
            r->kind = (h->flags & FLAG_FIGHTERS) ? ROW_FIGHTER : (bits & KIND_INSTALL_ALL) ? ROW_INSTALL_ALL
                      : (bits & KIND_PACK) ? ROW_PACK : ROW_CARD;
            r->card = (short) (first + i);   /* the host's row */
            mu_mods_field(r->name, q, NAME);
            mu_mods_field(r->right, q + NAME, RIGHT);
            r->state = bits & STATE_BITS;
        }
        first += count;
    }
    if (end == h->cards && (h->flags & FLAG_AFTER) && (h->flags & (FLAG_MORE_LOADING | FLAG_MORE_FAILED)))
        more_row(&out[n++], h, h->cards);
    return n;
}

/* Read the list again (the window from `base`). With `recount` its rows may change in number (the
 * caller builds the list anew or moves it); without, only the words of the same rows change.
 * `differ` (may be NULL) says whether any row's or the bar's words did. */
int mu_mods_sync_rows(int recount, int* differ)
{
    static Row staged[MAX_ROWS];
    Head h;
    int n, i, from = mu_mods_base, own = 0;
    n = fetch_rows(&h, staged, &from, &own);
    if (n == 0 || (!recount && (n != mu_mods_row_count || from != mu_mods_base || !mu_mods_same_layout(&h, &mu_mods_head)))) return 0;
    if (differ != NULL) {
        *differ = !same_head(&h, &mu_mods_head);
        for (i = 0; i < n && !*differ; i++) {
            *differ = staged[i].kind != mu_mods_rows[i].kind || staged[i].state != mu_mods_rows[i].state ||
                      strcmp(staged[i].name, mu_mods_rows[i].name) != 0 || strcmp(staged[i].right, mu_mods_rows[i].right) != 0;
        }
    }
    memcpy(mu_mods_rows, staged, sizeof(Row) * n);
    mu_mods_head = h;
    if (recount) {
        mu_mods_row_count = n;
        mu_mods_base = from;
        mu_mods_top = own;
        commit();
    }
    return 1;
}

/* The host's row of the list's row `row`; -1 for the shim's own rows above the host's. */
int mu_mods_host_row(int row)
{
    return row >= mu_mods_top && row < mu_mods_row_count ? mu_mods_rows[row].card : -1;
}

/* The lobby list's A (shim/mu_lobby_list.c): mu_mods_rows[] and mu_mods_head are only ever replaced
 * together (mu_mods_sync_rows), every row read checked against the head's generation (fetch_rows), so
 * the generation named here is the one of the words on the screen. */
int mu_mods_gallery_drawn_row(int row, int* host, unsigned int* generation)
{
    if (!mu_mods_gallery_running() || mu_mods_head.status != ST_READY || row < mu_mods_top || row >= mu_mods_row_count ||
        mu_mods_rows[row].kind != ROW_CARD)
        return 0;
    *host = mu_mods_rows[row].card;
    *generation = (unsigned int) mu_mods_head.generation;
    return 1;
}

/* The list's row of the host's row `h`, or -1 when the window does not hold it. */
static int list_row(int h)
{
    const int row = mu_mods_top + h - mu_mods_base;
    return h >= mu_mods_base && row >= mu_mods_top && row < mu_mods_row_count && mu_mods_rows[row].card == h ? row : -1;
}

/* The list's last row is the true end of the list: nothing more to read, the window holds the host's
 * last row. The game's red line marks it. */
int mu_mods_gallery_end_shown(void)
{
    if (!mu_mods_gallery_running() || mu_mods_head.status != ST_READY) return 1;
    if (mu_mods_head.flags & FLAG_AFTER) return 0;
    return mu_mods_base + (mu_mods_row_count - mu_mods_top) >= mu_mods_head.cards;
}

/* The list goes round from its last row to its first, as the trophy list does: only when it holds
 * the whole list (a pack's, Manage, the fighters, Browse once it is read to its end). */
int mu_mods_gallery_wraps(void)
{
    return !mu_mods_gallery_running() || (mu_mods_base == 0 && mu_mods_gallery_end_shown());
}

/* The line a list built from `row` puts it on: `line` where it can be; a list that does not go round
 * shows no row before its first or after its last. */
static int fit_line(int row, int line)
{
    if (mu_mods_row_count <= LIST_LINES) return row;
    if (line < 0) line = 0;
    if (line > LIST_LINES - 1) line = LIST_LINES - 1;
    if (!mu_mods_gallery_wraps()) {
        if (line > row) line = row;
        if (row + (LIST_LINES - 1 - line) > mu_mods_row_count - 1) line = LIST_LINES - 1 - (mu_mods_row_count - 1 - row);
    }
    return line;
}

/* The rows of another list (a fighter, a pack, the search, a retry), or the posts back from a pack:
 * built anew (the window from `base`), the cursor on its row `row` and line `line`. */
int mu_mods_changed_at(int row, int line)
{
    mu_mods_asked_more_at = -1;
    mu_mods_hint_row = -2;   /* another list: the host hears where the cursor is on it */
    if (!mu_mods_sync_rows(1, NULL)) {
        message_row(&mu_mods_rows[0], "Loading...");
        mu_mods_row_count = 1;
        mu_mods_base = mu_mods_top = 0;
        mu_mods_head.status = ST_LOADING;
        commit();
    }
    mu_mods_fresh = 1;
    mu_mods_start_row = row;
    mu_mods_start_line = line;
    mu_mods_list_tick = 0;
    return MU_MODS_REBUILD;
}

/* A list built anew from its first row: its top, or the row the host names (the fighter chosen last). */
int mu_mods_changed(void)
{
    mu_mods_base = 0;
    mu_mods_changed_at(0, 0);
    if (mu_mods_head.status == ST_READY && mu_mods_head.start > 0 && mu_mods_top + mu_mods_head.start < mu_mods_row_count) {
        mu_mods_start_row = mu_mods_top + mu_mods_head.start;
        mu_mods_start_line = mu_mods_start_row;
    }
    return MU_MODS_REBUILD;
}

/* The same list read again (more rows, or its window moved): the cursor stays on the host's row `h`
 * (or the shim's own row `selected` when h < 0) and on its line where it can. */
int mu_mods_moved_to(int h, int selected, int line)
{
    int row;
    if (h >= mu_mods_base + (mu_mods_row_count - mu_mods_top) && h < mu_mods_head.cards && mu_mods_head.cards - mu_mods_base > WINDOW) {
        mu_mods_base += SLIDE;   /* the row is past the window now ("Loading..." became rows the window ends before) */
        mu_mods_sync_rows(1, NULL);
    }
    row = h < 0 ? selected : list_row(h);
    if (row < 0) row = h >= mu_mods_base ? mu_mods_row_count - 1 : mu_mods_top;
    if (row >= mu_mods_row_count) row = mu_mods_row_count - 1;
    if (row < 0) row = 0;
    mu_mods_fresh = 1;
    mu_mods_start_row = row;
    mu_mods_start_line = line;
    mu_mods_list_tick = 0;
    return MU_MODS_MOVED;
}

/* `again`: back from the search's name entry, the host keeps the fighter, the list and the search. */
static void open_list(int again)
{
    unsigned char p[4];
    unsigned int got;
    p[0] = SUB_OPEN;
    p[1] = (unsigned char) mu_mods_manage;
    p[2] = (unsigned char) mu_mods_category;
    p[3] = 1;
    mu_mods_active = mu_mods_ask(p, again ? 4 : 3, mu_mods_reply, sizeof mu_mods_reply, &got) && got >= 1 && mu_mods_reply[0] == 1;
    mu_mods_leave_for_search = 0;
    mu_mods_open_on_list = 1;
    mu_mods_fresh = 1;
    mu_mods_start_row = mu_mods_start_line = 0;
    mu_mods_pack_row = mu_mods_pack_line = mu_mods_pack_base = 0;
    mu_mods_base = mu_mods_top = 0;
    mu_mods_asked_more_at = -1;
    mu_mods_hint_row = -2;
    mu_mods_spinner_gone();
    mu_mods_list_tick = 0;
    shadow_category_set = 0;
    memset(&mu_mods_head, 0, sizeof mu_mods_head);
    /* What the host holds already (a list seen a moment ago) shows at once; else "Loading...". */
    if (!mu_mods_active || !mu_mods_sync_rows(1, NULL)) {
        message_row(&mu_mods_rows[0], mu_mods_active ? "Loading..." : "Could not reach SSBM Nucleus.");
        mu_mods_row_count = 1;
        commit();
    } else if (mu_mods_head.status == ST_READY && mu_mods_head.start > 0 && mu_mods_top + mu_mods_head.start < mu_mods_row_count) {
        mu_mods_start_row = mu_mods_start_line = mu_mods_top + mu_mods_head.start;   /* the fighters' list: on the one chosen last */
    }
}

void mu_mods_gallery_open(int from_manage, int which)
{
    mu_mods_manage = from_manage != 0;
    /* the categories, in their table's order (mu_mods_categories.c) */
    mu_mods_category = which >= 0 && which < MU_MODS_CATEGORIES ? which : 0;
    open_list(0);
}

void mu_mods_gallery_reopen(void)
{
    open_list(1);
}

int mu_mods_gallery_list_start(int* row, int* line)
{
    if (!mu_mods_gallery_running()) return 0;
    if (!mu_mods_fresh) {
        mu_mods_sync_rows(0, NULL);   /* back from the viewer: an install shows in its row */
        return 0;
    }
    mu_mods_fresh = 0;
    /* A row past the end (the rows changed meanwhile) is the first; with no more rows than the list
     * shows, a row stands on its own line. */
    *row = mu_mods_start_row >= 0 && mu_mods_start_row < mu_mods_row_count ? mu_mods_start_row : 0;
    *line = fit_line(*row, mu_mods_start_line);
    return 1;
}

/* Back in the menus (gm/gmmenumode.c through mu_mods_menu_return): nothing of the list is kept but
 * what the host caches. */
void mu_mods_gallery_close(void)
{
    mu_mods_active = 0;
    mu_mods_leave_for_search = 0;
    mu_mods_view.row = -1;
    mu_mods_view.card = -1;
    mu_mods_view.pack = 0;
    mu_mods_pic_shown = -1;
    mu_mods_pic_card[0] = mu_mods_pic_card[1] = -1;
    mu_mods_spinner_gone();
}
