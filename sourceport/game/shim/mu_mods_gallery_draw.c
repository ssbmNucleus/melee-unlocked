/* The Mods menu's list (ty/tylist.c), drawn: each row's words (the mod's name, and a pack's count or
 * "Installed" where the trophy count was) with the row's inset box only around words, and the bar
 * under the list (how many, and the legend where L and R step).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/sislib.h>

#include <mu_lobby_list.h>
#include <mu_mods_gallery.h>
#include <mu_native.h>

#include "mu_mods_categories.h"
#include "mu_mods_gallery_internal.h"

enum { SHOWN_NAME = 32 };      /* a row's name at most; a longer one is cut in the middle */
/* Where a row's two texts may go, in world units right of the name's start (tylist.c puts the name
 * there): the row's art (ToyFigureListBase_Top_joint) has an inset box from 14.66 to 18.07; the name
 * stays left of it and the right column ("50,079", "Installed", "6 skins") inside it, where the
 * numbers always ended, clear of the box's inner border on both sides. A text too wide for its part
 * is drawn narrower until it fits (the text renderer's own fitting, sysdolphin/baselib/hsd_3A76.c
 * sisFitLineToBox). */
#define ROW_NAME_RIGHT 14.40f
#define ROW_BOX_TEXT_LEFT 15.12f
#define ROW_BOX_TEXT_RIGHT 17.80f

static char count_words[112];
#define BAR_ROOM 36   /* characters the bar under the list shows in full */

/* ---- a row's inset box: only around words ---------------------------------------------------------
 * The row's art (ToyFigureListBase_Top_joint) is one triangle strip of 13 vertices over a 32x32 texture,
 * a strip of segments along x: the left end, the bar, the box's left wall, its inside, and its right
 * wall with the bar's right end (vertices 3/2, 1/0, 4/7 and 5/6 at x 8.00, 8.49, 11.40 and 12.15). A
 * row with nothing at its right draws the same strip from copies of its two vertex arrays in which the
 * box's left wall has no width (1/0 at 3/2's x), 1/0 to 4/7 draws one texel column of plain bar from
 * right of the box's right wall (the bar, its top and bottom edges), and 4/7 to 5/6 the bar's right end
 * at its retail size, so the bar runs on to its end with no box in it. Checked against the retail strip
 * first; any other art is left as it is. */
enum { BOX_VERTS = 12, BOX_U_BAR = 26112 /* texel 25's middle, 25.5/32 in 1.15 */ };
static HSD_VtxDescList* box_desc;           /* the retail list (box shown) */
static HSD_VtxDescList nobox_desc[3];       /* the copy (no box) */
static unsigned char nobox_pos[BOX_VERTS * 6] __attribute__((aligned(32)));
static unsigned char nobox_uv[BOX_VERTS * 4] __attribute__((aligned(32)));
static signed char box_ready;               /* 0 not looked at, 1 the copy is made, -1 other art */

static unsigned be16(const unsigned char* p) { return (unsigned) p[0] << 8 | p[1]; }

static int box_strip_is_retail(const HSD_PObj* p)
{
    static const unsigned char order[13] = { 10, 11, 9, 11, 8, 3, 2, 1, 0, 4, 7, 5, 6 };
    /* x of vertices 3, 1, 4, 5 in 4.11 fixed point: 8.00, 8.49, 11.40, 12.15 (to a hundredth) */
    static const int vert[4] = { 3, 1, 4, 5 };
    static const float x[4] = { 8.00f, 8.49f, 11.40f, 12.15f };
    const HSD_VtxDescList* v = p->verts;
    const unsigned char* dl = p->display;
    const unsigned char* pos;
    int i;
    if (v == NULL || dl == NULL || p->n_display < 1 || (int) v[0].attr != 9 || (int) v[0].attr_type != 2 ||
        (int) v[0].comp_type != 3 || v[0].frac != 11 || v[0].stride != 6 || (int) v[1].attr != 13 ||
        (int) v[1].attr_type != 2 || (int) v[1].comp_type != 2 || v[1].frac != 15 || v[1].stride != 4 ||
        (int) v[2].attr != 0xFF || dl[0] != 0x98 || be16(dl + 1) != 13)
    {
        return 0;
    }
    for (i = 0; i < 13; i++) {
        if (dl[3 + i * 2] != order[i] || dl[3 + i * 2 + 1] != order[i]) return 0;
    }
    pos = DISC_GET(const unsigned char, v[0].vertex);
    for (i = 0; i < 4; i++) {
        const float at = (float) (short) be16(pos + vert[i] * 6) / 2048.0f;
        if (at < x[i] - 0.01f || at > x[i] + 0.01f) return 0;
    }
    return 1;
}

static void box_copy(HSD_VtxDescList* retail)
{
    const unsigned char* pos = DISC_GET(const unsigned char, retail[0].vertex);
    const unsigned char* uv = DISC_GET(const unsigned char, retail[1].vertex);
    int i;
    memcpy(nobox_pos, pos, sizeof nobox_pos);
    memcpy(nobox_uv, uv, sizeof nobox_uv);
    memcpy(nobox_desc, retail, sizeof nobox_desc);
    /* 1 and 0 (the box's left wall's right side) at 3 and 2's x: the wall has no width */
    nobox_pos[1 * 6] = nobox_pos[0 * 6] = pos[3 * 6];
    nobox_pos[1 * 6 + 1] = nobox_pos[0 * 6 + 1] = pos[3 * 6 + 1];
    /* 1, 0, 4, 7: a texel of plain bar right of the box's right wall, so 1/0 to 4/7 is plain bar; 4/7
     * moves right to where that texel is drawn in the retail strip, so 4/7 to 5/6 is the bar's right
     * end at its own size */
    {
        const int u4 = (int) be16(uv + 4 * 4), u5 = (int) be16(uv + 5 * 4);
        const int x4 = (short) be16(pos + 4 * 6), x5 = (short) be16(pos + 5 * 6);
        const int x = u5 > u4 ? x5 - (u5 - BOX_U_BAR) * (x5 - x4) / (u5 - u4) : x4;
        nobox_pos[4 * 6] = nobox_pos[7 * 6] = (unsigned char) ((x >> 8) & 0xFF);
        nobox_pos[4 * 6 + 1] = nobox_pos[7 * 6 + 1] = (unsigned char) (x & 0xFF);
    }
    for (i = 0; i < 4; i++) {
        static const int at[4] = { 1, 0, 4, 7 };
        nobox_uv[at[i] * 4] = (unsigned char) (BOX_U_BAR >> 8);
        nobox_uv[at[i] * 4 + 1] = (unsigned char) (BOX_U_BAR & 0xFF);
    }
    DISC_SET(nobox_desc[0].vertex, nobox_pos);
    DISC_SET(nobox_desc[1].vertex, nobox_uv);
}

static void row_box(HSD_JObj* jobj, int shown)
{
    HSD_DObj* d = jobj != NULL ? HSD_JObjGetDObj(jobj) : NULL;
    HSD_PObj* p = d != NULL ? d->pobj : NULL;
    if (p == NULL || box_ready < 0) return;
    if (p->verts != box_desc && p->verts != nobox_desc) {
        /* first row seen, or the list's file was loaded again (somewhere else) since */
        if (!box_strip_is_retail(p)) {
            if (box_ready == 0) {
                box_ready = -1;
                OSReport("[mods] list: the row art is not the retail one; its box stays on every row\n");
            }
            return;
        }
        box_desc = p->verts;
        box_copy(box_desc);
        box_ready = 1;
    }
    p->verts = shown ? box_desc : nobox_desc;
}

void mu_mods_gallery_row_words(HSD_Text* name, HSD_Text* mark, HSD_Text* count, int row, HSD_JObj* jobj)
{
    char shown[SHOWN_NAME + 8];
    const char* right;
    const Row* r;
    if (!mu_mods_gallery_running() || row < 0 || row >= mu_mods_row_count) {
        /* the game's own list (or a row past the end): its art as it is */
        HSD_DObj* d = jobj != NULL ? HSD_JObjGetDObj(jobj) : NULL;
        if (d != NULL && d->pobj != NULL && d->pobj->verts == nobox_desc) d->pobj->verts = box_desc;
        return;
    }
    r = &mu_mods_rows[row];
    if (r->kind == ROW_PACK || r->kind == ROW_INSTALL_ALL) {
        right = r->kind == ROW_PACK && r->state == CARD_INSTALLED ? "Installed" : r->right;
    } else if (r->kind == ROW_FIGHTER) {
        right = r->right;   /* its count: every fighter's once the site's counts are in, else none */
    } else if (r->kind != ROW_CARD) {
        right = "";
    } else if (mu_mods_manage) {
        right = r->state == CARD_REMOVED ? "Removed" : r->right;
    } else {
        right = r->state == CARD_INSTALLED ? "Installed" : r->state == CARD_DOWNLOADING ? "Installing"
              : r->state == CARD_FAILED ? "Failed" : r->right;
    }
    /* The box only around words (a pack's count, Installed and the like); a row without has none, and
     * its name may run on where the box would be. */
    row_box(jobj, right[0] != '\0');
    /* Each text in its own part of the row (ROW_NAME_RIGHT and the box), narrower when it is too
     * wide for it; a very long name is cut in the middle first. */
    if (name != NULL && name->font_size.x > 0.0f) {
        name->box_size_x = (right[0] != '\0' || box_ready <= 0 ? ROW_NAME_RIGHT : ROW_BOX_TEXT_RIGHT) / name->font_size.x;
        name->default_fitting = 1;
    }
    if (count != NULL && name != NULL && count->font_size.x > 0.0f) {
        count->pos_x = name->pos_x + ROW_BOX_TEXT_LEFT;
        count->box_size_x = (ROW_BOX_TEXT_RIGHT - ROW_BOX_TEXT_LEFT) / count->font_size.x;
        count->default_fitting = 1;
    }
    /* (the shim's own rows, "Search: <words>" and "Could not load more", are only narrowed) */
    if (r->kind == ROW_CARD || r->kind == ROW_PACK) mu_mods_middle_cut(shown, r->name, SHOWN_NAME);
    else strcpy(shown, r->name);
    mu_mods_set_words(name, shown);
    mu_mods_set_words(mark, "");
    mu_mods_set_words(count, right);
}

const char* mu_mods_gallery_count_words(void)
{
    char page[sizeof mu_mods_head.message];
    if (!mu_mods_gallery_running()) return NULL;
    /* The bar under the list (its sort tabs are hidden, ty/tylist.c): the fighter L and R step
     * through, and how many the list has in all, as the host counts them ("3,373 mods": the site's
     * count of posts; "245 skins"; "12 installed"); in a pack, its count (or how "Install all" goes);
     * on the fighters, "Choose a fighter". */
    page[0] = 0;
    if (mu_mods_head.status == ST_ERROR) {
        strcpy(page, "A: Try again");
    } else if (mu_mods_head.status == ST_READY && (mu_mods_head.cards > 0 || (mu_mods_head.flags & FLAG_FIGHTERS))) {
        strcpy(page, mu_mods_head.message);
    }
    /* (the message is at most 47 characters, the filter 19: count_words holds both) */
    count_words[0] = 0;
    if (mu_mods_head.flags & FLAG_FIGHTERS) {
        strcpy(count_words, page);
        return count_words;
    }
    if (mu_mods_head.flags & FLAG_PACK) {
        /* The pack's name before its count ("12 skins"), cut in the middle to fit the bar; what
         * "Install all" is doing, alone. */
        if (mu_mods_head.status == ST_READY && mu_mods_head.message[0] >= '0' && mu_mods_head.message[0] <= '9') {
            const int room = BAR_ROOM - (int) strlen(mu_mods_head.message) - 5;
            char name[sizeof mu_mods_head.title];
            if (room >= 8 && mu_mods_head.title[0]) {
                mu_mods_middle_cut(name, mu_mods_head.title, room < (int) sizeof name - 1 ? room : (int) sizeof name - 1);
                strcat(count_words, name);
                strcat(count_words, "  -  ");
            }
        }
        if (mu_mods_head.status == ST_READY) strcat(count_words, mu_mods_head.message);
        return count_words;
    }
    if (mu_mods_head.flags & (FLAG_FILTER | FLAG_SEARCH)) {
        /* the fighter, the search's words in quotes after it ("Mario "sun"") */
        strcat(count_words, strcmp(mu_mods_head.filter, "All") == 0 ? "All fighters" : mu_mods_head.filter);
    }
    if (count_words[0] && page[0]) strcat(count_words, "  -  ");
    strcat(count_words, page);
    return count_words;
}

/* The bar's legend, at its left end, where a fighter's posts are shown: L and R step through the
 * fighters. (The menu font has no "/".) */
static const char* legend_words(void)
{
    if (mu_lobby_list_on()) return "L R: Refresh";   /* the lobby list (shim/mu_lobby_list.c) */
    /* (the category's words: "L R: Fighter"; Stages' "L R: Stage", they step through the six) */
    return (mu_mods_head.flags & FLAG_FILTER) && !(mu_mods_head.flags & (FLAG_FIGHTERS | FLAG_PACK)) ? mu_mods_categories[mu_mods_category]->legend
                                                                                      : NULL;
}

/* The bar's units (its texts are right-aligned at their x): its count ends at 290, about 1.47 pixels
 * of a 1280-wide screen a unit, x 0 at its middle. The legend ends at LEGEND_RIGHT (it starts near the
 * bar's left end); beside it the count is narrowed to COUNT_ROOM units when longer (the text
 * renderer's own fitting), so the two never meet. */
#define LEGEND_RIGHT (-190.0f)
#define COUNT_ROOM 400.0f
#define BAR_BOX 640.0f   /* the bar's box as HSD_SisLib_803A6754 makes it */

/* The bar's words written anew, as the list wrote them first (HSD_SisLib_803A6B98: its position,
 * colour and scale, then the words): the count and, at the left, the legend. */
void mu_mods_gallery_bar(HSD_Text* bar)
{
    const char* words = mu_mods_gallery_count_words();
    const char* legend;
    SisBuffer* alloc;
    if (words == NULL || bar == NULL || (alloc = (SisBuffer*) bar->alloc_data) == NULL || alloc->data == NULL) return;
    legend = legend_words();
    alloc->end = alloc->data;
    alloc->data[0] = 0;
    alloc->count = 0;
    bar->sis_buffer = alloc->data;
    bar->x60 = NULL;
    bar->box_size_x = legend != NULL ? COUNT_ROOM : BAR_BOX;
    bar->default_fitting = legend != NULL;
    HSD_SisLib_803A6B98(bar, 290.0f, 320.0f, "%s", words);
    if (legend != NULL) HSD_SisLib_803A6B98(bar, LEGEND_RIGHT, 320.0f, "%s", legend);
}
