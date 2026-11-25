/* The Mods menu's viewer: the picture on the stand, an HSD archive the host builds from the mod's
 * picture (port/runtime/host/mods_menu.cpp picture_archive), fetched once per generation into the
 * buffer the stand does not show, so the one shown stays until the next is in; the plain tile while
 * there is none; where it stands; and where the cursor is, so the host fetches the rows on screen first.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/jobj.h>

#include <mu_mods_gallery.h>
#include <mu_native.h>

#include "mu_mods_gallery_internal.h"

/* The host fetches the pictures of the rows on screen and of the next screen ahead of the viewer, the
 * cursor's row first: told where the cursor is (its host row and its line on the screen) whenever the
 * row, the line or the list changes (13 [row] 2 [line]). */
int mu_mods_hint_row = -2, mu_mods_hint_line = -1, mu_mods_hint_cards = -1;
void mu_mods_hint_prefetch(int h, int selected, int line)
{
    unsigned char p[5];
    unsigned int got;
    int at = h, on = line;
    if (!mu_mods_active || mu_mods_head.status != ST_READY || (mu_mods_head.flags & FLAG_FIGHTERS) || mu_mods_head.cards <= 0) return;
    if (at < 0) {
        /* on the shim's own rows ("Search"): the host's first row is further down the screen */
        at = 0;
        on = line + (mu_mods_top - selected);
    }
    if (on < 0) on = 0;
    if (on > LIST_LINES - 1) on = LIST_LINES - 1;
    if (at == mu_mods_hint_row && on == mu_mods_hint_line && mu_mods_head.cards == mu_mods_hint_cards) return;
    mu_mods_hint_row = at;
    mu_mods_hint_line = on;
    mu_mods_hint_cards = mu_mods_head.cards;
    p[0] = SUB_PICTURE;
    p[1] = (unsigned char) (at >> 8);
    p[2] = (unsigned char) at;
    p[3] = 2;
    p[4] = (unsigned char) on;
    mu_mods_ask(p, 5, mu_mods_reply, sizeof mu_mods_reply, &got);
}

#define PIC_CAP (264u * 1024u + 4096u)
#define TILE_CAP (32u * 1024u)

static unsigned char pic_bytes[2][PIC_CAP] __attribute__((aligned(32)));
static HSD_Archive pic_archive[2];
static HSD_Joint* pic_joint[2];
int mu_mods_pic_card[2] = {-1, -1};
static unsigned int pic_generation[2];
int mu_mods_pic_shown = -1;   /* the buffer the stand shows now; -1 the tile */

static unsigned char tile_bytes[TILE_CAP] __attribute__((aligned(32)));
static HSD_Archive tile_archive;
static HSD_Joint* tile_joint;
static int tile_tried;

/* A picture archive of the host's reply (1, generation, size, the archive) into `buffer`. */
static HSD_Joint* take_picture_of(unsigned char* buffer, unsigned int capacity, HSD_Archive* archive, int row,
                                  unsigned int* generation, int pack);
static HSD_Joint* take_picture(unsigned char* buffer, unsigned int capacity, HSD_Archive* archive, int row,
                               unsigned int* generation)
{
    return take_picture_of(buffer, capacity, archive, row, generation, 0);
}

/* `pack`: a pack's row's own picture (13 [row] 1). */
static HSD_Joint* take_picture_of(unsigned char* buffer, unsigned int capacity, HSD_Archive* archive, int row,
                                  unsigned int* generation, int pack)
{
    unsigned char p[4];
    unsigned int got = 0, size;
    HSD_Joint* joint;
    p[0] = SUB_PICTURE;
    p[1] = (unsigned char) (row >> 8);
    p[2] = (unsigned char) row;
    p[3] = 1;
    if (!mu_mods_ask(p, pack ? 4 : 3, buffer, capacity, &got) || got < 9 || buffer[0] != 1) return NULL;
    size = mu_mods_u32_at(buffer + 5);
    if (size < 0x20 || size > got - 9) return NULL;
    *generation = mu_mods_u32_at(buffer + 1);
    memmove(buffer, buffer + 9, size);
    if (HSD_ArchiveParse(archive, buffer, size) != 0) return NULL;
    joint = HSD_ArchiveGetPublicAddress(archive, "mu_mods_picture_joint");
    if (joint == NULL) OSReport("[mods] a picture without its joint (%u bytes)\n", size);
    return joint;
}

static HSD_Joint* tile(void)
{
    unsigned int generation;
    if (!tile_tried) {
        tile_tried = 1;
        tile_joint = take_picture(tile_bytes, TILE_CAP, &tile_archive, 0xFFFF, &generation);
        if (tile_joint == NULL) OSReport("[mods] the stand's plain tile did not load\n");
    }
    return tile_joint;
}

HSD_Joint* mu_mods_gallery_joint(int row)
{
    if (!mu_mods_gallery_running()) return NULL;
    mu_mods_pic_shown = -1;
    if (row >= 0 && row < mu_mods_row_count && (mu_mods_rows[row].kind == ROW_CARD || mu_mods_rows[row].kind == ROW_PACK) &&
        mu_mods_view.pack == (mu_mods_rows[row].kind == ROW_PACK))
    {
        int b;
        for (b = 0; b < 2; b++) {
            if (pic_joint[b] != NULL && mu_mods_pic_card[b] == mu_mods_rows[row].card && mu_mods_view.card == mu_mods_rows[row].card &&
                pic_generation[b] == mu_mods_view.pic_generation)
            {
                mu_mods_pic_shown = b;
                return pic_joint[b];
            }
        }
    }
    return tile();
}

/* Where the picture stands: the trophy data's fields (Toy_803060BC) 0 to 5 and the panel icon (8). */
int mu_mods_gallery_placement(int field, float* value)
{
    if (!mu_mods_gallery_running()) return 0;
    switch (field) {
    case 0: case 2: case 5: *value = 0.0f; return 1;
    case 1: *value = 0.6f; return 1;
    case 3: *value = 11.0f; return 1;
    case 4: *value = 1.0f; return 1;
    case 8: *value = 0.0f; return 1;
    default: return 0;
    }
}

/* The row's picture (a card's, or a pack's own) into the free buffer when the stand does not show the
 * one the host has now (fetched once per generation); mu_mods_view.redraw when the stand should show it. */
static int load_picture(int row);
void mu_mods_want_picture(int row)
{
    const Row* r = &mu_mods_rows[row];
    const int b = mu_mods_pic_shown;
    if (mu_mods_view.pic_status == ST_LOADING) return;
    if (b < 0 || mu_mods_pic_card[b] != r->card || pic_generation[b] != mu_mods_view.pic_generation) {
        const int other = b == 0 ? 1 : 0;
        if (!(mu_mods_pic_card[other] == r->card && pic_generation[other] == mu_mods_view.pic_generation && pic_joint[other] != NULL)) {
            if (load_picture(row)) mu_mods_view.redraw = 1;
        } else {
            mu_mods_view.redraw = 1;
        }
    }
}

/* The free buffer gets the row's picture; 1 when the stand should show it now. */
static int load_picture(int row)
{
    const int b = mu_mods_pic_shown == 0 ? 1 : 0;
    unsigned int generation = 0;
    pic_joint[b] = NULL;
    mu_mods_pic_card[b] = -1;
    pic_joint[b] = take_picture_of(pic_bytes[b], PIC_CAP, &pic_archive[b], mu_mods_rows[row].card, &generation,
                                   mu_mods_rows[row].kind == ROW_PACK);
    if (pic_joint[b] == NULL) return 0;
    mu_mods_pic_card[b] = mu_mods_rows[row].card;
    pic_generation[b] = generation;
    mu_mods_view.pic_generation = generation;
    return 1;
}
