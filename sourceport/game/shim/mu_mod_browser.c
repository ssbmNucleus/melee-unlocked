/* The mod browser: a list of mods from SSBM Nucleus (ssbmnucleus.net), drawn over the main menu,
 * where the player downloads one and it is installed without leaving the game.
 *
 * Opened with Y on the main menu's first page. While it is open it owns the menu's input
 * (mn/mnmain.c mn_8022DB10 asks mu_mod_browser_think first):
 *   L / R         the tab (Costumes, Stage skins, Fighters)
 *   Up / Down     the row; Up from the first row reaches the filters
 *   on a row      Left / Right the page, A download and install, B close
 *   on a filter   Left / Right the filter (Fighter, Sort, Search), A the next choice, X the one
 *                 before; on Search, A starts typing
 *   typing        Up / Down the letter, Right one more letter, Left one fewer, A search, B cancel
 *
 * This half only draws and asks. The catalog, the download and the installation are the host's
 * (port/runtime/host/mod_browser.h), reached through one command; the game never touches the
 * network or a file. An installed costume or stage skin is one of the choices the character or
 * stage select already offers with L and R, at once. A fighter's plugin is loaded at the next
 * change of mode (mexft/mexft_plugins.c).
 *
 * The design follows GurekamDhillon's Nucleus browser for his Melee build (GPL-3.0).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/inlines.h>
#include <sysdolphin/baselib/sislib.h>

#include "mu_native.h"
#include "mu_test_env.h"

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);

#define CMD_MOD_BROWSER 0xC5
enum { ROWS = 8, TABS = 3, SORTS = 3, POLL_FRAMES = 6, SEARCH_MAX = 24, FIGHTERS_MAX = 40 };
enum { ST_LOADING, ST_READY, ST_ERROR };
enum { ROW_NONE, ROW_QUEUED, ROW_DOWNLOADING, ROW_INSTALLED, ROW_FAILED, ROW_UNSUPPORTED };
enum { FILTER_FIGHTER, FILTER_SORT, FILTER_SEARCH, FILTERS };
/* Where the cursor is: a list row (0..count-1), or FOCUS_FILTERS (the line of filters). */
enum { FOCUS_FILTERS = -1 };

typedef struct BrowserRow {
    unsigned int id, downloads;
    int state;
    char title[44], author[20], note[40];
} BrowserRow;

static struct {
    int open;
    int tab, page, cursor, filter;
    int sort, fighter;
    char search[SEARCH_MAX + 1];
    int typing;                    /* the search is being typed: `draft` is on screen */
    char draft[SEARCH_MAX + 1];
    int status, pages, has_next, count;
    int poll;
    char message[64];
    BrowserRow rows[ROWS];
    char fighters[FIGHTERS_MAX][20];
    int fighter_count;
    HSD_Text* box;
    HSD_Text* text;
} mb;

static const char* const tab_name[TABS] = { "Costumes", "Stage skins", "Fighters" };
static const char* const sort_name[SORTS] = { "Most downloaded", "Newest", "Most liked" };
/* What a search is typed from, in the order Up steps through it. */
static const char letters[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/* Where the list is drawn, in the description canvas's units: left, top, text size, the step
 * between rows, and the box's width and height. MELEE_TEST_MOD_BROWSER overrides them for
 * measuring ("x,y,size,step,width,height"). */
typedef struct Layout { float x, y, size, step, w, h; } Layout;
static Layout layout(void)
{
    Layout l = { -12.3f, -7.6f, 0.021f, 0.95f, 25.2f, 14.6f };
    const char* v = mu_test_getenv("MELEE_TEST_MOD_BROWSER");
    float* f = &l.x;
    int n = 0;
    while (v != NULL && n < 6 && *v) {
        float sign = 1.0f, val = 0.0f, scale = 0.0f;
        int digits = 0;
        if (*v == '-') { sign = -1.0f; v++; }
        for (; (*v >= '0' && *v <= '9') || *v == '.'; v++) {
            if (*v == '.') { scale = 1.0f; continue; }
            val = val * 10.0f + (float) (*v - '0');
            if (scale > 0.0f) scale *= 10.0f;
            digits++;
        }
        if (!digits) break;
        f[n++] = sign * (scale > 0.0f ? val / scale : val);
        while (*v == ' ' || *v == ',') v++;
    }
    return l;
}

/* ---- the host ---- */

static unsigned int be32(const unsigned char* p)
{
    return (unsigned int) p[0] << 24 | (unsigned int) p[1] << 16 | (unsigned int) p[2] << 8 | p[3];
}

static void copy_text(char* to, const unsigned char* from, int width)
{
    memcpy(to, from, (unsigned int) width);
    to[width - 1] = '\0';
}

static void no_host(void)
{
    mb.status = ST_ERROR;
    mb.count = 0;
    snprintf(mb.message, sizeof mb.message, "This version of the program has no mod browser.");
}

static void ask_fighters(void)
{
    static unsigned char response[4096];
    unsigned char payload[1] = { 2 };
    unsigned int got = 0;
    int i;
    mb.fighter_count = 0;
    if (mu_online_abi_command(CMD_MOD_BROWSER, payload, 1, response, sizeof response, &got) != 0 || got < 1) {
        return;
    }
    mb.fighter_count = response[0] <= FIGHTERS_MAX ? response[0] : FIGHTERS_MAX;
    if (got < 1u + (unsigned int) mb.fighter_count * 20u) {
        mb.fighter_count = 0;
    }
    for (i = 0; i < mb.fighter_count; i++) {
        copy_text(mb.fighters[i], response + 1 + i * 20, 20);
    }
}

static void ask_page(void)
{
    static unsigned char response[4096];
    unsigned char payload[6 + SEARCH_MAX];
    unsigned int got = 0, i, n = (unsigned int) strlen(mb.search);
    const unsigned char* p;
    payload[0] = 0;
    payload[1] = (unsigned char) mb.tab;
    payload[2] = (unsigned char) (mb.page >> 8);
    payload[3] = (unsigned char) mb.page;
    payload[4] = (unsigned char) mb.sort;
    payload[5] = (unsigned char) mb.fighter;
    memcpy(payload + 6, mb.search, n);
    if (mu_online_abi_command(CMD_MOD_BROWSER, payload, 6 + n, response, sizeof response, &got) != 0 || got < 72) {
        no_host();
        return;
    }
    mb.status = response[0];
    mb.pages = (response[4] << 8) | response[5];
    mb.has_next = response[6];
    mb.count = response[7] <= ROWS ? response[7] : ROWS;
    copy_text(mb.message, response + 8, 64);
    if (got < 72u + (unsigned int) mb.count * 113u) {
        mb.count = 0;
    }
    for (i = 0, p = response + 72; i < (unsigned int) mb.count; i++, p += 113) {
        BrowserRow* r = &mb.rows[i];
        r->id = be32(p);
        r->state = p[4];
        r->downloads = be32(p + 5);
        copy_text(r->title, p + 9, 44);
        copy_text(r->author, p + 53, 20);
        copy_text(r->note, p + 73, 40);
    }
    /* Past the end of a shorter page: its last row. On the filters when the list is empty, but
     * only once it is known to be (a page still loading has no rows yet either). */
    if (mb.count > 0 && mb.cursor >= mb.count) {
        mb.cursor = mb.count - 1;
    } else if (mb.count == 0 && mb.status != ST_LOADING) {
        mb.cursor = FOCUS_FILTERS;
    }
}

static int ask_download(unsigned int id)
{
    static unsigned char response[4096];
    unsigned char payload[5];
    unsigned int got = 0;
    payload[0] = 1;
    payload[1] = (unsigned char) (id >> 24);
    payload[2] = (unsigned char) (id >> 16);
    payload[3] = (unsigned char) (id >> 8);
    payload[4] = (unsigned char) id;
    return mu_online_abi_command(CMD_MOD_BROWSER, payload, 5, response, sizeof response, &got) == 0 && got >= 1 &&
           response[0] != 0;
}

/* A new list: another tab, filter, search or page. */
static void reload(int first_page)
{
    if (first_page) {
        mb.page = 0;
    }
    if (mb.cursor != FOCUS_FILTERS) {
        mb.cursor = 0;
    }
    mb.poll = 0;
    mb.status = ST_LOADING;
    mb.count = 0;
}

/* ---- drawing ---- */

/* The menu font takes ASCII letters and digits as they are and Shift-JIS for punctuation. */
static void encode(char* out, int capacity, const char* ascii)
{
    int n = 0;
    for (; *ascii != '\0' && n < capacity - 3; ascii++) {
        unsigned int code = 0;
        switch (*ascii) {
        case '%': code = 0x93; break;
        case '/': code = 0x5e; break;
        case '(': code = 0x69; break;
        case ')': code = 0x6a; break;
        case '>': code = 0x84; break;
        case '<': code = 0x83; break;
        case '+': code = 0x7b; break;
        case '!': code = 0x49; break;
        case '?': code = 0x48; break;
        case ':': code = 0x46; break;
        case '-': code = 0x7c; break;
        case '.': code = 0x44; break;
        case '_': code = 0x51; break;
        case ',': code = 0x43; break;
        case '\'': code = 0x66; break;
        case '&': code = 0x95; break;
        case '#': code = 0x94; break;
        case '"': code = 0x68; break;
        case '[': code = 0x6d; break;
        case ']': code = 0x6e; break;
        case '=': code = 0x81; break;
        case '*': code = 0x96; break;
        }
        if (code != 0) {
            out[n++] = (char) 0x81;
            out[n++] = (char) code;
        } else if ((*ascii >= '0' && *ascii <= '9') || (*ascii >= 'A' && *ascii <= 'Z') ||
                   (*ascii >= 'a' && *ascii <= 'z') || *ascii == ' ')
        {
            out[n++] = *ascii;
        } else {
            out[n++] = ' ';
        }
    }
    out[n] = '\0';
}

static void line(const Layout* l, float x, float row, const GXColor* color, const char* ascii)
{
    char encoded[160];
    GXColor c = *color;
    int id;
    encode(encoded, sizeof encoded, ascii);
    id = HSD_SisLib_803A6B98(mb.text, x / l->size, (row * l->step) / l->size, "%s", encoded);
    HSD_SisLib_803A74F0(mb.text, id, &c);
}

static const char* state_text(const BrowserRow* r)
{
    switch (r->state) {
    case ROW_QUEUED: return "Waiting";
    case ROW_DOWNLOADING: return "Downloading";
    case ROW_INSTALLED: return "Installed";
    case ROW_FAILED: return "Failed";
    case ROW_UNSUPPORTED: return "Not supported";
    default: return "";
    }
}

static void destroy(void)
{
    if (mb.text != NULL) {
        HSD_SisLib_803A5CC4(mb.text);
    }
    if (mb.box != NULL) {
        HSD_SisLib_803A5CC4(mb.box);
    }
    mb.text = NULL;
    mb.box = NULL;
}

/* The whole screen again: a handful of lines, rebuilt when something on it changed. */
static void draw(int canvas)
{
    static const GXColor title = { 236, 200, 110, 255 }, ink = { 235, 238, 249, 255 }, dim = { 150, 158, 180, 255 },
                         pick = { 255, 255, 255, 255 }, good = { 120, 230, 150, 255 }, bad = { 255, 130, 120, 255 };
    const Layout l = layout();
    /* The columns, in the canvas's units at the default text size: they follow the size. */
    const float unit = l.size / 0.021f;
    const float col_state = 12.7f * unit, col_author = 18.9f * unit;
    const int filters = mb.cursor == FOCUS_FILTERS;
    const float list = 3.0f;   /* the first row's line */
    char text[160];
    int i;
    destroy();
    mb.box = HSD_SisLib_803A6754(0, canvas);
    if (mb.box == NULL) {
        return;
    }
    mb.box->pos_x = l.x - 0.6f;
    mb.box->pos_y = l.y - 0.6f;
    mb.box->pos_z = 17.0f;
    mb.box->font_size.x = mb.box->font_size.y = 1.0f;
    mb.box->box_size_x = l.w;
    mb.box->box_size_y = l.h;
    mb.box->bg_color.r = 8;
    mb.box->bg_color.g = 10;
    mb.box->bg_color.b = 24;
    mb.box->bg_color.a = 232;
    mb.text = HSD_SisLib_803A6754(0, canvas);
    if (mb.text == NULL) {
        return;
    }
    mb.text->pos_x = l.x;
    mb.text->pos_y = l.y;
    mb.text->pos_z = 17.0f;
    mb.text->font_size.x = mb.text->font_size.y = l.size;
    mb.text->default_kerning = 1;
    mb.text->default_alignment = 0;

    line(&l, 0.0f, 0.0f, &title, "Mods");
    for (i = 0; i < TABS; i++) {
        snprintf(text, sizeof text, i == mb.tab ? "[%s]" : " %s ", tab_name[i]);
        line(&l, (4.2f + 6.6f * (float) i) * unit, 0.0f, i == mb.tab ? &title : &dim, text);
    }

    /* The filters: one line, the one the cursor is on in white. The fighter filter narrows the
     * costumes only, so it is dim on the other tabs. */
    {
        const GXColor* c;
        const char* fighter = mb.fighter < mb.fighter_count ? mb.fighters[mb.fighter] : "All";
        c = filters && mb.filter == FILTER_FIGHTER ? &pick : mb.tab == 0 ? &ink : &dim;
        snprintf(text, sizeof text, "%sFighter: %.16s", filters && mb.filter == FILTER_FIGHTER ? ">" : " ",
                 mb.tab == 0 ? fighter : "-");
        line(&l, 0.0f, 1.4f, c, text);
        c = filters && mb.filter == FILTER_SORT ? &pick : &ink;
        snprintf(text, sizeof text, "%s%s", filters && mb.filter == FILTER_SORT ? ">" : " ", sort_name[mb.sort]);
        line(&l, 9.6f * unit, 1.4f, c, text);
        c = filters && mb.filter == FILTER_SEARCH ? &pick : &ink;
        if (mb.typing) {
            snprintf(text, sizeof text, ">Search: %s_", mb.draft);
            c = &good;
        } else {
            snprintf(text, sizeof text, "%sSearch: %s", filters && mb.filter == FILTER_SEARCH ? ">" : " ",
                     mb.search[0] != '\0' ? mb.search : "-");
        }
        line(&l, 17.0f * unit, 1.4f, c, text);
    }

    if (mb.status == ST_LOADING) {
        line(&l, 0.0f, list + 0.5f, &ink, "Loading...");
    } else if (mb.status == ST_ERROR) {
        line(&l, 0.0f, list + 0.5f, &bad, mb.message);
    } else if (mb.count == 0) {
        line(&l, 0.0f, list + 0.5f, &ink, "Nothing found.");
    }
    for (i = 0; i < mb.count && mb.status == ST_READY; i++) {
        const BrowserRow* r = &mb.rows[i];
        const int here = i == mb.cursor;
        snprintf(text, sizeof text, "%s %.26s", here ? ">" : " ", r->title);
        line(&l, 0.0f, list + (float) i, here ? &pick : r->state == ROW_UNSUPPORTED ? &dim : &ink, text);
        if (r->state != ROW_NONE) {
            line(&l, col_state, list + (float) i,
                 r->state == ROW_FAILED ? &bad : r->state == ROW_UNSUPPORTED ? &dim : &good, state_text(r));
        } else {
            snprintf(text, sizeof text, "%u", r->downloads);
            line(&l, col_state, list + (float) i, &dim, text);
        }
        snprintf(text, sizeof text, "%.11s", r->author);
        line(&l, col_author, list + (float) i, &dim, text);
    }
    if (mb.status == ST_READY && mb.count > 0) {
        if (mb.cursor >= 0 && mb.rows[mb.cursor].note[0] != '\0') {
            const BrowserRow* r = &mb.rows[mb.cursor];
            line(&l, 0.0f, list + 8.5f, r->state == ROW_FAILED ? &bad : r->state == ROW_UNSUPPORTED ? &dim : &good, r->note);
        }
        snprintf(text, sizeof text, "Page %d of %d", mb.page + 1, mb.pages > 0 ? mb.pages : mb.page + 1);
        line(&l, col_author, list + 8.5f, &dim, text);
    }
    if (mb.typing) {
        line(&l, 0.0f, list + 9.7f, &dim, "Up Down: letter   Right: add   Left: erase   A: search   B: cancel");
    } else if (filters) {
        line(&l, 0.0f, list + 9.7f, &dim, "Left Right: filter   A: change   X: back   Down: list   B: close");
    } else {
        line(&l, 0.0f, list + 9.7f, &dim, "A: install   B: close   L R: tab   Left Right: page   Up: filters");
    }
    line(&l, 0.0f, list + 10.7f, &dim, "Mods from SSBM Nucleus - ssbmnucleus.net");
}

/* ---- the menu's side ---- */

/* A new menu scene: the last one's texts went with it. */
void mu_mod_browser_forget(void)
{
    mb.text = NULL;
    mb.box = NULL;
    mb.open = 0;
    mb.typing = 0;
}

static int letter_index(char c)
{
    int i;
    for (i = 0; letters[i] != '\0'; i++) {
        if (letters[i] == c) {
            return i;
        }
    }
    return 0;
}

/* Typing the search with the stick: the last letter steps through the alphabet. */
static void typing_input(unsigned int buttons)
{
    const int count = (int) sizeof letters - 1;
    int n = (int) strlen(mb.draft);
    if (buttons & MenuInput_Back) {
        mb.typing = 0;
        lbAudioAx_80024030(0);
    } else if (buttons & (MenuInput_Confirm | MenuInput_StartButton)) {
        /* No spaces at either end: the site matches the words. */
        while (n > 0 && mb.draft[n - 1] == ' ') {
            mb.draft[--n] = '\0';
        }
        {
            const char* from = mb.draft;
            while (*from == ' ') {
                from++;
            }
            memmove(mb.search, from, strlen(from) + 1);
        }
        mb.typing = 0;
        reload(1);
        lbAudioAx_80024030(1);
        OSReport("[mod browser] search: \"%s\"\n", mb.search);
    } else if (buttons & (MenuInput_Up | MenuInput_Down)) {
        if (n == 0) {
            mb.draft[n++] = 'A';
            mb.draft[n] = '\0';
        } else {
            const int step = (buttons & MenuInput_Up) ? 1 : count - 1;
            mb.draft[n - 1] = letters[(letter_index(mb.draft[n - 1]) + step) % count];
        }
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Right) && n < SEARCH_MAX) {
        mb.draft[n] = n > 0 ? mb.draft[n - 1] : 'A';
        mb.draft[n + 1] = '\0';
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Left) && n > 0) {
        mb.draft[n - 1] = '\0';
        lbAudioAx_80024030(2);
    }
}

static void filter_input(unsigned int buttons)
{
    const int step = (buttons & MenuInput_XButton) ? -1 : 1;
    if (buttons & MenuInput_Right) {
        mb.filter = (mb.filter + 1) % FILTERS;
        lbAudioAx_80024030(2);
    } else if (buttons & MenuInput_Left) {
        mb.filter = (mb.filter + FILTERS - 1) % FILTERS;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Down) && mb.count > 0) {
        mb.cursor = 0;
        lbAudioAx_80024030(2);
    } else if (buttons & (MenuInput_Confirm | MenuInput_XButton)) {
        if (mb.filter == FILTER_FIGHTER && mb.tab == 0 && mb.fighter_count > 0) {
            mb.fighter = (mb.fighter + mb.fighter_count + step) % mb.fighter_count;
            reload(1);
        } else if (mb.filter == FILTER_SORT) {
            mb.sort = (mb.sort + SORTS + step) % SORTS;
            reload(1);
        } else if (mb.filter == FILTER_SEARCH && (buttons & MenuInput_Confirm)) {
            mb.typing = 1;
            strcpy(mb.draft, mb.search);
        } else if (mb.filter == FILTER_SEARCH && mb.search[0] != '\0') {
            mb.search[0] = '\0';   /* X on the search clears it */
            reload(1);
        }
        lbAudioAx_80024030(2);
    }
}

static void list_input(unsigned int buttons)
{
    if ((buttons & MenuInput_Right) && mb.status == ST_READY && mb.has_next) {
        mb.page++;
        reload(0);
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Left) && mb.page > 0) {
        mb.page--;
        reload(0);
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Down) && mb.count > 0) {
        mb.cursor = (mb.cursor + 1) % mb.count;
        lbAudioAx_80024030(2);
    } else if (buttons & MenuInput_Up) {
        mb.cursor = mb.cursor > 0 ? mb.cursor - 1 : FOCUS_FILTERS;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Confirm) && mb.status == ST_READY && mb.count > 0) {
        BrowserRow* r = &mb.rows[mb.cursor];
        if (r->state != ROW_QUEUED && r->state != ROW_DOWNLOADING && r->state != ROW_INSTALLED &&
            r->state != ROW_UNSUPPORTED && ask_download(r->id))
        {
            r->state = ROW_QUEUED;
            mb.poll = 0;
            lbAudioAx_80024030(1);
            OSReport("[mod browser] install asked: %u %s\n", r->id, r->title);
        } else {
            lbAudioAx_80024030(3);
        }
    }
}

/* Called by the main menu's first page before its own input, with the frame's buttons
 * (mn_80229624). Returns 1 when the browser is open and took them. */
int mu_mod_browser_think(int canvas, unsigned int buttons)
{
    static unsigned int shown;   /* a digest of what is on screen: redraw only on a change */
    unsigned int now;
    const char* c;
    int i;
    if (!mb.open) {
        if (!(buttons & MenuInput_YButton) || canvas < 0) {
            return 0;
        }
        mb.open = 1;
        mb.typing = 0;
        mb.cursor = 0;
        reload(1);
        shown = 0;
        if (mb.fighter_count == 0) {
            ask_fighters();
        }
        lbAudioAx_80024030(1);
        OSReport("[mod browser] opened\n");
    } else if (mb.typing) {
        typing_input(buttons);
    } else if (buttons & MenuInput_Back) {
        mb.open = 0;
        destroy();
        lbAudioAx_80024030(0);
        OSReport("[mod browser] closed\n");
        return 1;
    } else if (buttons & (MenuInput_LTrigger | MenuInput_RTrigger)) {
        mb.tab = (mb.tab + ((buttons & MenuInput_RTrigger) ? 1 : TABS - 1)) % TABS;
        mb.cursor = 0;   /* a new tab starts on its list, also after an empty one left the cursor on the filters */
        reload(1);
        lbAudioAx_80024030(2);
    } else if (mb.cursor == FOCUS_FILTERS) {
        filter_input(buttons);
    } else {
        list_input(buttons);
    }
    if (mb.poll-- <= 0) {
        mb.poll = POLL_FRAMES;
        ask_page();
    }
    now = (unsigned int) (mb.tab * 131 + mb.page * 31 + (mb.cursor + 2) * 7 + mb.status * 3 + mb.count + mb.sort * 1009 +
                          mb.fighter * 2003 + mb.filter * 4001 + mb.typing * 8009) + 1u;
    for (i = 0; i < mb.count; i++) {
        now = now * 33u + mb.rows[i].id + (unsigned int) mb.rows[i].state * 977u;
    }
    for (c = mb.typing ? mb.draft : mb.search; *c != '\0'; c++) {
        now = now * 33u + (unsigned char) *c;
    }
    if (now != shown || mb.text == NULL) {
        shown = now;
        draw(canvas);
    }
    return 1;
}
