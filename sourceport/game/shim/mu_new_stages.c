/* New stages from the Mods menu on the stage select, the game's half (port/runtime/host/new_stages.h,
 * MU_CMD_NEW_STAGES; the screen's own side is "newsel" in mn/mnstagesel.c).
 *
 * An installed new stage is a stage file made over a retail stage (its base). The stage select shows
 * the installed ones on the pages after the retail page; picking one starts a match on its base, and
 * the host serves the new stage's file for that match (and its Sudden Death). This file keeps the
 * visit's list, puts each one's picture on its icon (64x56 RGBA8 from the host, the post's picture),
 * writes its name where the retail screen shows a stage's name, and asks the host for the pick and for
 * Random's draws. Offline only: the host lists none online, in a replay or a Vanilla run.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>

#include "mu_disc.h"
#include "mu_native.h"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
extern const char* mu_ground_stage_file(int grkind);
extern int Stage_8022519C(int stkind);
int snprintf(char* dst, __SIZE_TYPE__ size, const char* format, ...);   /* the decomp's stdio has none */

enum { OP_LIST = 0, OP_ICON = 1, OP_PICK = 2, OP_SCENE = 3 };
enum { MAX = 44, BASE = 16, NAME = 40, BASE_NAME = 24, ENTRY = BASE + NAME + BASE_NAME, SLOTS = 22 };
#define ICON_W 64
#define ICON_H 56
#define ICON_BYTES (ICON_W * ICON_H * 4)
#define FONT 4   /* a SIS slot the stage select leaves free */

static unsigned char reply[4096] __attribute__((aligned(32)));
static unsigned int reply_size;

static int ask(const unsigned char* payload, unsigned int n)
{
    reply_size = 0;
    return mu_online_abi_command(MU_CMD_NEW_STAGES, payload, n, reply, sizeof reply, &reply_size) == 0 &&
           reply_size > 0 && reply[0] == 1;
}

/* ---- this visit's list ---- */

static struct {
    int count;
    int random;   /* Random may draw them */
    struct {
        unsigned char host;    /* the host's index */
        signed char icon;      /* 0 not asked, 1 here, -1 none (a plain tile shows) */
        short stkind;          /* the base's stage kind */
        char base[BASE];
        char name[NAME];
        char base_name[BASE_NAME];
    } stage[MAX];
} ns;
static unsigned char icon_pixels[MAX][ICON_BYTES] __attribute__((aligned(32)));

static int same_file(const char* a, const char* b)
{
    a += *a == '/';   /* the game's stage table names its files with a slash first ("/GrNLa.dat") */
    b += *b == '/';
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char) (*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char) (*b + 32) : *b;
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* The stage kind whose file is `base`: the versus stages' kinds come first (2 to 0x20), then the
 * Target Tests' (0x21 to 0x3A). -1 for none. */
static int stkind_of(const char* base)
{
    int stkind;
    for (stkind = 2; stkind <= 0x3A; stkind++) {
        const char* file = mu_ground_stage_file(Stage_8022519C(stkind));
        if (file != NULL && same_file(file, base)) {
            return stkind;
        }
    }
    return -1;
}

static void text_reset(void);
static void dress_reset(void);

/* The stage select is built (the retail screen, offline): the list read again (the Mods menu may have
 * installed or removed stages since), no pick. Returns how many it shows. */
int mu_new_stages_enter(void)
{
    unsigned char payload[2] = { OP_PICK, 0xFF };
    unsigned char list = OP_LIST;
    int i, n;
    ns.count = 0;
    ns.random = 0;
    text_reset();
    dress_reset();
    ask(payload, 2);
    if (mu_mex_active() || mu_slippi_on_online_sss() || !ask(&list, 1) || reply_size < 7) {
        return 0;
    }
    ns.random = reply[6] != 0;
    for (i = 0, n = 0; i < reply[1] && n < MAX && 7u + (unsigned int) (i + 1) * ENTRY <= reply_size; i++) {
        const unsigned char* at = reply + 7 + i * ENTRY;
        memcpy(ns.stage[n].base, at, BASE);
        ns.stage[n].base[BASE - 1] = '\0';
        memcpy(ns.stage[n].name, at + BASE, NAME);
        ns.stage[n].name[NAME - 1] = '\0';
        memcpy(ns.stage[n].base_name, at + BASE + NAME, BASE_NAME);
        ns.stage[n].base_name[BASE_NAME - 1] = '\0';
        ns.stage[n].stkind = (short) stkind_of(ns.stage[n].base);
        if (ns.stage[n].stkind < 0) {
            OSReport("new stages: %s: no stage kind loads %s; not shown\n", ns.stage[n].name, ns.stage[n].base);
            continue;
        }
        ns.stage[n].host = (unsigned char) i;
        ns.stage[n].icon = 0;
        n++;
    }
    ns.count = n;
    if (n > 0) {
        OSReport("new stages: the stage select shows %d new stage%s on %d page%s (Random %s them)\n", n,
                 n == 1 ? "" : "s", (n + SLOTS - 1) / SLOTS, n > SLOTS ? "s" : "",
                 ns.random ? "may draw" : "skips");
    }
    return n;
}

/* ---- the icons ---- */

/* One picture from the host, in parts (as mu_css_skins.c fetch). */
static int fetch_icon(int index)
{
    unsigned char payload[5];
    unsigned int at = 0;
    payload[0] = OP_ICON;
    payload[1] = ns.stage[index].host;
    while (at < ICON_BYTES) {
        unsigned int part;
        payload[2] = (unsigned char) (at >> 16);
        payload[3] = (unsigned char) (at >> 8);
        payload[4] = (unsigned char) at;
        if (!ask(payload, 5) || reply_size <= 8 || ((unsigned int) reply[1] << 8 | reply[2]) != ICON_W ||
            ((unsigned int) reply[3] << 8 | reply[4]) != ICON_H ||
            ((unsigned int) reply[5] << 16 | (unsigned int) reply[6] << 8 | reply[7]) != ICON_BYTES)
        {
            return 0;
        }
        part = reply_size - 8;
        if (part > ICON_BYTES - at) {
            return 0;
        }
        memcpy(icon_pixels[index] + at, reply + 8, part);
        at += part;
    }
    return 1;
}

/* A grey tile (GX RGBA8: per 4x4 block, 32 bytes of AR then 32 of GB) for a picture the host has not. */
static void plain_icon(int index)
{
    unsigned char* px = icon_pixels[index];
    int block;
    for (block = 0; block < ICON_BYTES / 64; block++) {
        memset(px + block * 64, 0, 32);
        memset(px + block * 64 + 32, 0x50, 32);
        {
            int i;
            for (i = 0; i < 16; i++) {
                px[block * 64 + i * 2] = 0xFF;          /* A */
                px[block * 64 + i * 2 + 1] = 0x50;      /* R */
            }
        }
    }
}

static const void* icon_of(int index)
{
    if (ns.stage[index].icon == 0) {
        ns.stage[index].icon = fetch_icon(index) ? 1 : -1;
        if (ns.stage[index].icon < 0) {
            OSReport("new stages: %s: no picture from the host; a plain tile shows\n", ns.stage[index].name);
            plain_icon(index);
        }
    }
    return icon_pixels[index];
}

/* Per page slot, the textures that show the stage's picture (an icon's animated textures, their
 * animation taken off once so nothing puts a retail picture back) and the image description each
 * now uses. */
static struct {
    HSD_JObj* icon;
    int count;
    HSD_TObj* tobj[2];
    HSD_ImageDesc image[2];
} dressed[SLOTS];

/* A new visit's icons are new models (one may sit where an old one was). */
static void dress_reset(void)
{
    memset(dressed, 0, sizeof dressed);
}

static void find_pictures(HSD_JObj* j, int depth, int slot)
{
    HSD_JObj* child;
    if (j == NULL || depth > 6) {
        return;
    }
    if (!(j->flags & (JOBJ_PTCL | JOBJ_SPLINE))) {
        HSD_DObj* dobj;
        for (dobj = j->u.dobj; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* tobj;
            for (tobj = dobj->mobj != NULL ? dobj->mobj->tobj : NULL; tobj != NULL; tobj = tobj->next) {
                if (tobj->aobj != NULL && tobj->imagedesc != NULL && dressed[slot].count < 2) {
                    const int k = dressed[slot].count++;
                    if (slot == 0 && k == 0) {
                        OSReport("new stages: an icon's picture is %dx%d, format %d\n", tobj->imagedesc->width,
                                 tobj->imagedesc->height, (int) tobj->imagedesc->format);
                    }
                    HSD_TObjRemoveAnim(tobj);
                    dressed[slot].image[k] = *tobj->imagedesc;
                    dressed[slot].image[k].width = ICON_W;
                    dressed[slot].image[k].height = ICON_H;
                    dressed[slot].image[k].format = GX_TF_RGBA8;
                    dressed[slot].image[k].mipmap = 0;
                    dressed[slot].image[k].minLOD = 0.0F;
                    dressed[slot].image[k].maxLOD = 0.0F;
                    dressed[slot].tobj[k] = tobj;
                    tobj->imagedesc = &dressed[slot].image[k];
                }
            }
        }
    }
    for (child = j->child; child != NULL; child = child->next) {
        find_pictures(child, depth + 1, slot);
    }
}

/* The icon at page slot `slot` (a stacked icon, mnstagesel.c) shows new stage `index`'s picture. */
void mu_new_stages_dress(HSD_JObj* icon, int slot, int index)
{
    const void* pixels;
    int k;
    if (icon == NULL || slot < 0 || slot >= SLOTS || index < 0 || index >= ns.count) {
        return;
    }
    if (dressed[slot].icon != icon) {
        dressed[slot].icon = icon;
        dressed[slot].count = 0;
        find_pictures(icon, 0, slot);
        if (dressed[slot].count == 0) {
            OSReport("new stages: icon slot %d has no picture texture\n", slot);
        }
    }
    pixels = icon_of(index);
    for (k = 0; k < dressed[slot].count; k++) {
        DISC_SET(dressed[slot].image[k].image_ptr, (void*) pixels);
        dressed[slot].tobj[k]->imagedesc = &dressed[slot].image[k];
    }
}

/* ---- the name and the page line ---- */

static struct {
    int canvas;            /* -1 none yet */
    HSD_Text* name;        /* the shown stage's base and name */
    HSD_Text* page;        /* the page line */
    int name_shown;        /* the index the name shows, -1 none */
    int page_shown;        /* the page the page line shows, -1 none */
} tx = { -1, NULL, NULL, -1, -1 };

static void text_reset(void)
{
    /* The last visit's texts went with its scene (the SIS pool is new at every scene). */
    tx.canvas = -1;
    tx.name = NULL;
    tx.page = NULL;
    tx.name_shown = -1;
    tx.page_shown = -1;
}

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

static int canvas(void)
{
    if (tx.canvas >= 0) {
        return 1;
    }
    if (HSD_SisLib_804D1124[FONT] == NULL) {
        HSD_SisLib_803A62A0(FONT, "SdMenu.usd", "SIS_MenuData");
    }
    if (HSD_SisLib_804D1124[FONT] == NULL) {
        return 0;
    }
    /* Its own camera: the 640x480 screen, drawn over the select screen (as maker/platform does). */
    tx.canvas = HSD_SisLib_803A611C(FONT, NULL, 9, 0x14, 0, 0xF, 0, 0x13);
    return tx.canvas >= 0;
}

static HSD_Text* new_text(float size)
{
    HSD_Text* t = HSD_SisLib_803A6754(FONT, tx.canvas);
    if (t != NULL) {
        t->pos_x = 0.0F;
        t->pos_y = 0.0F;
        t->pos_z = 0.0F;
        t->font_size.x = t->font_size.y = size;
        t->default_kerning = 1;
        t->default_alignment = 0;
    }
    return t;
}

/* A line at (x, y) on the 640x480 screen, scaled `scale` times the text's size, with a dark shadow. */
static void line(HSD_Text* t, float x, float y, float scale, const GXColor* color, const char* ascii)
{
    static const GXColor shadow = { 0, 0, 0, 200 };
    const float size = t->font_size.x;
    char encoded[96];
    GXColor c;
    int id;
    encode(encoded, sizeof encoded, ascii);
    c = shadow;
    id = HSD_SisLib_803A6B98(t, (x + 2.0F) / size, (y + 2.0F) / size, "%s", encoded);
    HSD_SisLib_803A7548(t, id, scale, scale);
    HSD_SisLib_803A74F0(t, id, &c);
    c = *color;
    id = HSD_SisLib_803A6B98(t, x / size, y / size, "%s", encoded);
    HSD_SisLib_803A7548(t, id, scale, scale);
    HSD_SisLib_803A74F0(t, id, &c);
}

/* Every frame (mnstagesel.c, the hover frame's proc): the name of new stage `index` (-1: none, the
 * retail screen's own name model shows a retail stage's) and the page line of `page` of `pages`.
 * Texts are written again only when what they show changed. */
void mu_new_stages_show(int index, int page, int pages)
{
    static const GXColor white = { 255, 255, 255, 255 }, soft = { 200, 214, 236, 255 }, gold = { 240, 206, 110, 255 };
    char text[96];
    if (ns.count <= 0 || pages <= 1 || !canvas()) {
        return;
    }
    if (index >= ns.count) {
        index = -1;
    }
    if (index != tx.name_shown) {
        tx.name_shown = index;
        if (tx.name != NULL) {
            HSD_SisLib_803A5CC4(tx.name);
            tx.name = NULL;
        }
        if (index >= 0 && (tx.name = new_text(0.6F)) != NULL) {
            const int len = (int) strlen(ns.stage[index].name);
            const float big = len <= 14 ? 1.7F : len <= 20 ? 1.35F : len <= 28 ? 1.05F : 0.85F;
            snprintf(text, sizeof text, "%s", ns.stage[index].base_name);
            line(tx.name, 62.0F, 296.0F, 1.0F, &soft, text);
            line(tx.name, 56.0F, 316.0F, big, &white, ns.stage[index].name);
        }
    }
    if (page != tx.page_shown) {
        tx.page_shown = page;
        if (tx.page != NULL) {
            HSD_SisLib_803A5CC4(tx.page);
            tx.page = NULL;
        }
        if ((tx.page = new_text(0.6F)) != NULL) {
            if (page <= 0) {
                snprintf(text, sizeof text, "R: New Stages (%d)", ns.count);
            } else if (pages > 2) {
                snprintf(text, sizeof text, "New Stages %d/%d   L/R: Page", page, pages - 1);
            } else {
                snprintf(text, sizeof text, "New Stages   L/R: Page");
            }
            line(tx.page, 352.0F, 374.0F, 0.9F, &gold, text);
        }
    }
}

/* ---- the pick, Random ---- */

/* New stage `index` is picked: the host serves it in the next match. Its base's stage kind, or -1
 * (the host refused: the error sound plays). */
int mu_new_stages_pick(int index)
{
    unsigned char payload[2];
    if (index < 0 || index >= ns.count) {
        return -1;
    }
    payload[0] = OP_PICK;
    payload[1] = ns.stage[index].host;
    if (!ask(payload, 2)) {
        OSReport("new stages: the host refused %s\n", ns.stage[index].name);
        return -1;
    }
    OSReport("new stages: %s picked; the match is on stage kind %d (%s)\n", ns.stage[index].name,
             (int) ns.stage[index].stkind, ns.stage[index].base);
    return ns.stage[index].stkind;
}

/* Random, with `retail` retail stages it may draw: a new stage's index when the setting lets Random
 * draw them and the draw lands on one (each stage, new or retail, as likely), else -1. */
int mu_new_stages_random(int retail)
{
    int r;
    if (!ns.random || ns.count <= 0 || retail < 0) {
        return -1;
    }
    r = HSD_Randi(retail + ns.count);
    return r < ns.count ? r : -1;
}

/* ---- the match boundary ---- */

/* gm_1A3F.c preloadState, a scene that is no match and no Sudden Death: a new stage picked earlier
 * (and its match played, or the screen left) is let go, so a later match on its base is retail. */
void mu_new_stages_scene(void)
{
    unsigned char op = OP_SCENE;
    ask(&op, 1);
}
