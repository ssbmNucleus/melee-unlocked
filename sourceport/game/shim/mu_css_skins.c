/* Installed skins as costume slots, the game's half: what the character select, the match HUD and
 * the results screen ask the host (port/runtime/host/costume_slots.h, MU_CMD_MOD_BROWSER 15 to 18).
 *
 * The select screen (mn/mncharsel.c) puts a fighter's installed skins after its retail colours in the
 * X / Y cycle. A skin keeps the retail costume it was made for as the door's costume, which is all
 * the game and the simulation ever see; the skin itself is the host's per-port record (sub 16 here,
 * served at fighter creation by shim/mu_port_costume.c). This file keeps, per select screen, each
 * fighter's slot list and the pictures: a slot's own portrait (136x188) for the door, and the stock
 * icon (24x24) of what each port's fighter wears for the HUD and the results. A picture the host has
 * not got is NULL, and the game keeps the retail one.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <melee/lb/lbspdisplay.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>

#include "mu_disc.h"
#include "mu_native.h"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);

enum { SUB_LIST = 15, SUB_PICK = 16, SUB_PICTURE = 17, SUB_WORN_STOCK = 18 };
enum { FIGHTERS = 26, SLOTS = 16, PORTS = 4, NAME = 24 };

#define PORTRAIT_BYTES (136 * 188 * 4)
#define STOCK_BYTES (24 * 24 * 4)
#define PORTRAITS 8   /* four doors' worth and a few more, the least recently shown replaced */

static unsigned char reply[4096] __attribute__((aligned(32)));
static unsigned int reply_size;

static int ask(const unsigned char* payload, unsigned int n)
{
    reply_size = 0;
    return mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, reply, sizeof reply, &reply_size) == 0 && reply_size > 0 &&
           reply[0] == 1;
}

/* ---- each fighter's slots, read once a screen ---- */

static struct {
    signed char known;                /* 0 not asked, 1 read, -1 none (or the host has none) */
    unsigned char count;
    unsigned int generation;
    unsigned char costume[SLOTS];
    unsigned char flags[SLOTS];       /* 1 a portrait, 2 a stock icon */
    char name[SLOTS][NAME];
} lists[FIGHTERS];

static void read_list(int ckind)
{
    unsigned char payload[2];
    int i;
    lists[ckind].known = -1;
    lists[ckind].count = 0;
    payload[0] = SUB_LIST;
    payload[1] = (unsigned char) ckind;
    if (!ask(payload, 2) || reply_size < 6) {
        return;
    }
    lists[ckind].generation = (unsigned int) reply[2] << 24 | (unsigned int) reply[3] << 16 | (unsigned int) reply[4] << 8 | reply[5];
    for (i = 0; i < reply[1] && i < SLOTS && 6u + (unsigned int) (i + 1) * (2 + NAME) <= reply_size; i++) {
        const unsigned char* at = reply + 6 + i * (2 + NAME);
        lists[ckind].costume[i] = at[0];
        lists[ckind].flags[i] = at[1];
        memcpy(lists[ckind].name[i], at + 2, NAME);
        lists[ckind].name[i][NAME - 1] = '\0';
    }
    lists[ckind].count = (unsigned char) i;
    lists[ckind].known = i > 0 ? 1 : -1;
    if (i > 0) {
        OSReport("costume slots: fighter %d has %d installed skin%s after its colours\n", ckind, i, i == 1 ? "" : "s");
    }
}

int mu_css_skins_count(int ckind)
{
    if (ckind < 0 || ckind >= FIGHTERS) {
        return 0;
    }
    if (lists[ckind].known == 0) {
        read_list(ckind);
    }
    return lists[ckind].known > 0 ? lists[ckind].count : 0;
}

int mu_css_skins_costume(int ckind, int index)
{
    if (index < 1 || index > mu_css_skins_count(ckind)) {
        return -1;
    }
    return lists[ckind].costume[index - 1];
}

const char* mu_css_skins_name(int ckind, int index)
{
    if (index < 1 || index > mu_css_skins_count(ckind)) {
        return "";
    }
    return lists[ckind].name[index - 1];
}

/* Changes whenever the installed skins do (any fighter's): a door's pick from an earlier visit of the
 * screen is only kept while it is the same. */
unsigned int mu_css_skins_generation(int ckind)
{
    return mu_css_skins_count(ckind) > 0 ? lists[ckind].generation : 0;
}

/* ---- the picks ---- */

int mu_css_skins_pick(int port, int ckind, int index, int online_local)
{
    unsigned char payload[5];
    payload[0] = SUB_PICK;
    payload[1] = (unsigned char) port;
    payload[2] = (unsigned char) ckind;
    payload[3] = (unsigned char) index;
    payload[4] = (unsigned char) (online_local ? 1 : 0);
    return ask(payload, 5);
}

/* ---- the portraits ---- */

static unsigned char portrait_pixels[PORTRAITS][PORTRAIT_BYTES] __attribute__((aligned(32)));
static struct {
    signed char ckind, index, have;   /* have: 1 here, -1 the host has none; ckind -1: a free entry */
    unsigned int used;
} portrait_slot[PORTRAITS];
static unsigned int portrait_clock;

/* One picture from the host, in parts: `payload` asks for it from offset 0 (its last three bytes are
 * the offset), `size` bytes of a `width` x `height` RGBA8 texture. */
static int fetch(unsigned char* payload, unsigned int n, unsigned char* out, unsigned int size, unsigned int width,
                 unsigned int height)
{
    unsigned int at = 0;
    while (at < size) {
        unsigned int part;
        payload[n - 3] = (unsigned char) (at >> 16);
        payload[n - 2] = (unsigned char) (at >> 8);
        payload[n - 1] = (unsigned char) at;
        if (!ask(payload, n) || reply_size <= 8 || ((unsigned int) reply[1] << 8 | reply[2]) != width ||
            ((unsigned int) reply[3] << 8 | reply[4]) != height ||
            ((unsigned int) reply[5] << 16 | (unsigned int) reply[6] << 8 | reply[7]) != size)
        {
            return 0;
        }
        part = reply_size - 8;
        if (part > size - at) {
            return 0;
        }
        memcpy(out + at, reply + 8, part);
        at += part;
    }
    return 1;
}

const void* mu_css_skins_portrait(int ckind, int index)
{
    unsigned char payload[7];
    int i, pick = -1;
    if (index < 1 || index > mu_css_skins_count(ckind) || !(lists[ckind].flags[index - 1] & 1)) {
        return NULL;
    }
    ++portrait_clock;
    for (i = 0; i < PORTRAITS; i++) {
        if (portrait_slot[i].ckind == ckind && portrait_slot[i].index == index) {
            portrait_slot[i].used = portrait_clock;
            return portrait_slot[i].have > 0 ? portrait_pixels[i] : NULL;
        }
    }
    /* A free entry, else the one shown longest ago (the doors show at most four, every frame). */
    for (i = 0; i < PORTRAITS; i++) {
        if (portrait_slot[i].ckind < 0) {
            pick = i;
            break;
        }
        if (pick < 0 || portrait_slot[i].used < portrait_slot[pick].used) {
            pick = i;
        }
    }
    portrait_slot[pick].ckind = (signed char) ckind;
    portrait_slot[pick].index = (signed char) index;
    portrait_slot[pick].used = portrait_clock;
    payload[0] = SUB_PICTURE;
    payload[1] = (unsigned char) ckind;
    payload[2] = (unsigned char) index;
    payload[3] = 1;
    portrait_slot[pick].have = fetch(payload, 7, portrait_pixels[pick], PORTRAIT_BYTES, 136, 188) ? 1 : -1;
    if (portrait_slot[pick].have < 0) {
        OSReport("costume slots: fighter %d slot %d: no portrait from the host; the retail one shows\n", ckind, index);
    }
    return portrait_slot[pick].have > 0 ? portrait_pixels[pick] : NULL;
}

/* A select screen is built: the slot lists are read again (the Mods menu may have installed or removed
 * skins since), the portraits likewise, and the host forgets every pick (the screen sends its
 * doors' picks again, so a port nobody picked a skin for, a CPU's included, wears its retail colour). */
void mu_css_skins_screen(void)
{
    unsigned char payload[2] = { SUB_PICK, 0xFF };
    int i;
    for (i = 0; i < FIGHTERS; i++) {
        lists[i].known = 0;
    }
    for (i = 0; i < PORTRAITS; i++) {
        portrait_slot[i].ckind = -1;
        portrait_slot[i].have = 0;
    }
    ask(payload, 2);
}

/* ---- the stock icon each port's fighter wears (the HUD, the results) ---- */

/* The player's own skin's icon (its costume slot), or an opponent's skin's, which the host fetches from
 * SSBM Nucleus once that skin is ready and may still be getting when the match starts: the host then
 * answers 2, and the icon is asked for again every STOCK_RETRY asks (the HUD asks about once a frame
 * per port) until it is there or the host says there is none. Nothing waits; the retail icon shows
 * meanwhile. */
#define STOCK_RETRY 30

static unsigned char stock_pixels[PORTS][STOCK_BYTES] __attribute__((aligned(32)));
static signed char stock_have[PORTS];   /* 0 ask, 1 here, -1 none, -2 on its way */
static unsigned char stock_wait[PORTS]; /* asks since the host said "on its way" */

/* shim/mu_port_costume.c, as a port's fighter is created: what it wears may have changed. */
void mu_css_skins_port_created(int port)
{
    if (port >= 0 && port < PORTS) {
        stock_have[port] = 0;
    }
}

const void* mu_css_skins_worn_stock(int port)
{
    unsigned char payload[5];
    int waiting;
    if (port < 0 || port >= PORTS) {
        return NULL;
    }
    waiting = stock_have[port] == -2;
    if (waiting && ++stock_wait[port] < STOCK_RETRY) {
        return NULL;
    }
    if (waiting || stock_have[port] == 0) {
        payload[0] = SUB_WORN_STOCK;
        payload[1] = (unsigned char) port;
        if (fetch(payload, 5, stock_pixels[port], STOCK_BYTES, 24, 24)) {
            stock_have[port] = 1;
            OSReport("costume slots: port %d: its skin's own stock icon%s\n", port, waiting ? " (it came during the match)" : "");
        } else if (reply_size >= 1 && reply[0] == 2) {
            if (!waiting) {
                OSReport("costume slots: port %d: its skin's stock icon is on its way; the retail one meanwhile\n", port);
            }
            stock_have[port] = -2;
            stock_wait[port] = 0;
        } else {
            stock_have[port] = -1;
        }
    }
    return stock_have[port] > 0 ? stock_pixels[port] : NULL;
}

/* One texture shows the 24x24 picture: its image description is a copy of the one it had, kept in
 * `image`, with the picture's pixels. */
static void show_stock(HSD_TObj* tobj, HSD_ImageDesc* image, const void* pixels)
{
    if (tobj == NULL || tobj->imagedesc == NULL) {
        return;
    }
    if (tobj->imagedesc != image) {
        *image = *tobj->imagedesc;
    }
    image->width = 24;
    image->height = 24;
    image->format = GX_TF_RGBA8;
    image->mipmap = 0;
    DISC_SET(image->image_ptr, (void*) pixels);
    tobj->imagedesc = image;
}

/* ifstock.c fn_802F94E0, as `player`'s stock row is drawn: the row's seven icons (joints 1 to 7) show
 * the skin's own stock icon. The retail animation picks its own image again whenever it runs, so the
 * picture is put back at every draw (as akaneia/common/mu_ak_hud.c does for an installed fighter). */
void mu_css_skins_hud_stock(HSD_JObj* root, int player)
{
    static HSD_ImageDesc image[PORTS];
    const void* pixels;
    int i;
    if (root == NULL || player < 0 || player >= PORTS || (pixels = mu_css_skins_worn_stock(player)) == NULL) {
        return;
    }
    for (i = 0; i < 7; i++) {
        HSD_JObj* icon = NULL;
        lb_80011E24(root, &icon, i + 1, -1);
        if (icon == NULL || (icon->flags & (JOBJ_PTCL | JOBJ_SPLINE)) || icon->u.dobj == NULL || icon->u.dobj->mobj == NULL) {
            continue;
        }
        show_stock(icon->u.dobj->mobj->tobj, &image[player], pixels);
    }
}

/* The stock icon textures of `j` and the joints under it (not its siblings): a texture the size of
 * a stock icon, its animation taken off so nothing puts the retail picture back. */
static int results_walk(HSD_JObj* j, int depth, HSD_ImageDesc* image, const void* pixels)
{
    int changed = 0;
    HSD_JObj* child;
    if (j == NULL || depth > 8) {
        return 0;
    }
    if (!(j->flags & (JOBJ_PTCL | JOBJ_SPLINE))) {
        HSD_DObj* dobj;
        for (dobj = j->u.dobj; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* tobj = dobj->mobj != NULL ? dobj->mobj->tobj : NULL;
            if (tobj != NULL && tobj->imagedesc != NULL &&
                ((tobj->imagedesc->width == 24 && tobj->imagedesc->height == 24) || tobj->imagedesc == image))
            {
                HSD_TObjRemoveAnim(tobj);
                show_stock(tobj, image, pixels);
                changed++;
            }
        }
    }
    for (child = j->child; child != NULL; child = child->next) {
        changed += results_walk(child, depth + 1, image, pixels);
    }
    return changed;
}

/* gmresultplayer.c, once a panel's stock icon (`jobj`, the panel's joint 7) has its frame: the
 * panel of `port` shows the skin's own stock icon. */
void mu_css_skins_results_stock(HSD_JObj* jobj, int port)
{
    static HSD_ImageDesc image[PORTS];
    const void* pixels;
    int changed;
    if (jobj == NULL || port < 0 || port >= PORTS || (pixels = mu_css_skins_worn_stock(port)) == NULL) {
        return;
    }
    changed = results_walk(jobj, 0, &image[port], pixels);
    OSReport("costume slots: results panel %d shows its skin's stock icon (%d texture%s)\n", port, changed,
             changed == 1 ? "" : "s");
}
