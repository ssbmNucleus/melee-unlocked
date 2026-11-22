/* The mod browser: a list of mods from SSBM Nucleus (ssbmnucleus.net), drawn over the main menu,
 * where the player downloads one and it is installed without leaving the game.
 *
 * Opened with Y on the main menu's first page. While it is open it owns the menu's input
 * (mn/mnmain.c mn_8022DB10 asks mu_mod_browser_think first):
 *   Up / Down     the row
 *   Left / Right  the page
 *   L / R         the tab (Costumes, Stage skins)
 *   A             download and install the row's mod
 *   B             close
 *
 * This half only draws and asks. The catalog, the download and the installation are the host's
 * (port/runtime/host/mod_browser.h), reached through one command; the game never touches the
 * network or a file. An installed costume or stage skin is one of the choices the character or
 * stage select already offers with L and R, at once.
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
enum { ROWS = 8, TABS = 2, POLL_FRAMES = 6 };
enum { ST_LOADING, ST_READY, ST_ERROR };
enum { ROW_NONE, ROW_QUEUED, ROW_DOWNLOADING, ROW_INSTALLED, ROW_FAILED, ROW_UNSUPPORTED };

typedef struct BrowserRow {
    unsigned int id, downloads;
    int state;
    char title[44], author[20], note[40];
} BrowserRow;

static struct {
    int open;
    int tab, page, cursor;
    int status, pages, has_next, count;
    int host_missing;   /* the host has no such command (an older program) */
    int poll;
    char message[64];
    BrowserRow rows[ROWS];
    HSD_Text* box;
    HSD_Text* text;
} mb;

static const char* const tab_name[TABS] = { "Costumes", "Stage skins" };

/* Where the list is drawn, in the description canvas's units: left, top, text size, the step
 * between rows, and the box's width and height. MELEE_TEST_MOD_BROWSER overrides them for
 * measuring ("x,y,size,step,width,height"). */
typedef struct Layout { float x, y, size, step, w, h; } Layout;
static Layout layout(void)
{
    Layout l = { -12.3f, -7.0f, 0.021f, 0.95f, 25.2f, 13.4f };
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

static void ask_page(void)
{
    static unsigned char response[4096];
    unsigned char payload[4];
    unsigned int got = 0, i;
    const unsigned char* p;
    payload[0] = 0;
    payload[1] = (unsigned char) mb.tab;
    payload[2] = (unsigned char) (mb.page >> 8);
    payload[3] = (unsigned char) mb.page;
    if (mu_online_abi_command(CMD_MOD_BROWSER, payload, 4, response, sizeof response, &got) != 0 || got < 72) {
        mb.host_missing = 1;
        mb.status = ST_ERROR;
        mb.count = 0;
        snprintf(mb.message, sizeof mb.message, "This version of the program has no mod browser.");
        return;
    }
    mb.host_missing = 0;
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
    if (mb.cursor >= mb.count) {
        mb.cursor = mb.count > 0 ? mb.count - 1 : 0;
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
    const float col_state = 12.7f * (l.size / 0.021f), col_author = 18.9f * (l.size / 0.021f);
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

    snprintf(text, sizeof text, "Mods    %s%s%s    %s%s%s", mb.tab == 0 ? "[" : " ", tab_name[0], mb.tab == 0 ? "]" : " ",
             mb.tab == 1 ? "[" : " ", tab_name[1], mb.tab == 1 ? "]" : " ");
    line(&l, 0.0f, 0.0f, &title, text);
    if (mb.status == ST_LOADING) {
        line(&l, 0.0f, 2.0f, &ink, "Loading...");
    } else if (mb.status == ST_ERROR) {
        line(&l, 0.0f, 2.0f, &bad, mb.message);
    } else if (mb.count == 0) {
        line(&l, 0.0f, 2.0f, &ink, "Nothing here.");
    }
    for (i = 0; i < mb.count && mb.status == ST_READY; i++) {
        const BrowserRow* r = &mb.rows[i];
        const int here = i == mb.cursor;
        snprintf(text, sizeof text, "%s %.26s", here ? ">" : " ", r->title);
        line(&l, 0.0f, 1.5f + (float) i, here ? &pick : r->state == ROW_UNSUPPORTED ? &dim : &ink, text);
        if (r->state != ROW_NONE) {
            line(&l, col_state, 1.5f + (float) i,
                 r->state == ROW_FAILED ? &bad : r->state == ROW_UNSUPPORTED ? &dim : &good, state_text(r));
        } else {
            snprintf(text, sizeof text, "%u", r->downloads);
            line(&l, col_state, 1.5f + (float) i, &dim, text);
        }
        snprintf(text, sizeof text, "%.11s", r->author);
        line(&l, col_author, 1.5f + (float) i, &dim, text);
    }
    if (mb.status == ST_READY && mb.count > 0) {
        const BrowserRow* r = &mb.rows[mb.cursor];
        if (r->note[0] != '\0') {
            line(&l, 0.0f, 10.0f, r->state == ROW_FAILED ? &bad : &good, r->note);
        }
        snprintf(text, sizeof text, "Page %d of %d", mb.page + 1, mb.pages > 0 ? mb.pages : mb.page + 1);
        line(&l, col_author, 10.0f, &dim, text);
    }
    line(&l, 0.0f, 11.2f, &dim, "A: install    B: close    L R: tab    Left Right: page");
    line(&l, 0.0f, 12.2f, &dim, "Mods from SSBM Nucleus - ssbmnucleus.net");
}

/* ---- the menu's side ---- */

/* A new menu scene: the last one's texts went with it. */
void mu_mod_browser_forget(void)
{
    mb.text = NULL;
    mb.box = NULL;
    mb.open = 0;
}

/* Called by the main menu's first page before its own input, with the frame's buttons
 * (mn_80229624). Returns 1 when the browser is open and took them. */
int mu_mod_browser_think(int canvas, unsigned int buttons)
{
    static unsigned int shown;   /* a digest of what is on screen: redraw only on a change */
    unsigned int now;
    int i;
    if (!mb.open) {
        if (!(buttons & MenuInput_YButton) || canvas < 0) {
            return 0;
        }
        mb.open = 1;
        mb.cursor = 0;
        mb.poll = 0;
        mb.status = ST_LOADING;
        mb.count = 0;
        shown = 0;
        lbAudioAx_80024030(1);
        OSReport("[mod browser] opened\n");
    } else if (buttons & MenuInput_Back) {
        mb.open = 0;
        destroy();
        lbAudioAx_80024030(0);
        OSReport("[mod browser] closed\n");
        return 1;
    } else if (buttons & (MenuInput_LTrigger | MenuInput_RTrigger)) {
        mb.tab = (mb.tab + ((buttons & MenuInput_RTrigger) ? 1 : TABS - 1)) % TABS;
        mb.page = mb.cursor = mb.poll = 0;
        mb.status = ST_LOADING;
        mb.count = 0;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Right) && mb.status == ST_READY && mb.has_next) {
        mb.page++;
        mb.cursor = mb.poll = 0;
        mb.status = ST_LOADING;
        mb.count = 0;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Left) && mb.page > 0) {
        mb.page--;
        mb.cursor = mb.poll = 0;
        mb.status = ST_LOADING;
        mb.count = 0;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Down) && mb.count > 0) {
        mb.cursor = (mb.cursor + 1) % mb.count;
        lbAudioAx_80024030(2);
    } else if ((buttons & MenuInput_Up) && mb.count > 0) {
        mb.cursor = (mb.cursor + mb.count - 1) % mb.count;
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
    if (mb.poll-- <= 0) {
        mb.poll = POLL_FRAMES;
        ask_page();
    }
    now = (unsigned int) (mb.tab * 131 + mb.page * 31 + mb.cursor * 7 + mb.status * 3 + mb.count) + 1u;
    for (i = 0; i < mb.count; i++) {
        now = now * 33u + mb.rows[i].id + (unsigned int) mb.rows[i].state * 977u;
    }
    if (now != shown || mb.text == NULL) {
        shown = now;
        draw(canvas);
    }
    return 1;
}
