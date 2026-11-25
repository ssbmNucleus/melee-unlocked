/* The Mods menu's labels and headings, cut letter by letter from the player's disc (MnMaAll.usd) the
 * way rogue/mu_rogue_label.c builds Rogue Mode's heading: the main menu's "Mods", "Browse" and
 * "Manage", each category's row (its letters are in its own file, mu_mods_category_<name>.c), the Mods
 * row's preview, and the Gallery panel's headings and side label while it runs as the mod list.
 * shim/mu_mods_menu.c puts them in place.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <dolphin/os.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>
#include <string.h>

#include <mu_native.h>

#include "mu_mods_lettering.h"

#define SOURCE_HASH 0xC9939674u /* FNV-1a of the 1.02 pictures the letters come from */

/* Source pictures: cursor label frames (176x30 IA4) and the panel's italic headings (168x28 I4). */
static const unsigned char label_frames[] = {0, 2, 4, 5, 8, 14, 26, 30, 31, 54};

/* The main menu's labels; each category's is in its own file (mu_mods_categories.h). */
static const Piece words[CHARACTERS][11] = {
    /* Mods: "Mod" of 1-P Mode, the s of Trophies */
    {{F00, 82, 139}, {F02, 131, 147}, {-1}},
    /* Browse: B, r, o of Bonus Records; no w on the disc, so two v of Adventure; s, e */
    {{F54, 2, 19}, {F54, 137, 146}, {F54, 19, 33}, {F31, 51, 70, 11}, {F31, 51, 70}, {F02, 131, 147},
     {F00, 139, 157}, {-1}},
    /* Manage */
    {{F00, 82, 107}, {F04, 76, 92}, {F14, 112, 127}, {F04, 76, 92}, {F05, 34, 49}, {F00, 139, 157}, {-1}},
};
static const Piece heading_word[] = {{HEAD0, 68, 131, 63, 0.3f}, {HEAD2, 127, 168, 41, 0.25f}, {-1}};

static unsigned char label_pixels[LABELS][LBYTES] __attribute__((aligned(32)));
static unsigned char label_dim_pixels[MU_MODS_CATEGORIES][LBYTES] __attribute__((aligned(32)));
static unsigned char heading_pixels[HBYTES] __attribute__((aligned(32)));
HSD_ImageDesc mu_mods_label_desc[LABELS];
HSD_ImageDesc mu_mods_label_dim_desc[MU_MODS_CATEGORIES];
HSD_ImageDesc mu_mods_heading_desc;
int mu_mods_composed;     /* 1 built, -1 the disc's pictures are not the ones the letters come from */

/* MatAnimJoint walk in the archive's order (as rogue/mu_rogue_label.c): every texture animation of
 * joint number `want`. */
int mu_mods_walk(HSD_MatAnimJoint* joint, int index, int want, Tables* out, int depth)
{
    int next = index + 1;
    if (joint == NULL || depth > 64) return next;
    if (index == want) {
        HSD_MatAnim* material;
        for (material = DP(joint->matanim); material != NULL; material = DP(material->next)) {
            HSD_TexAnim* texture;
            for (texture = DP(material->texanim); texture != NULL; texture = DP(texture->next))
                if (out->count < 4 && !DISC_NULL(texture->imagetbl)) out->anim[out->count++] = texture;
        }
    }
    if (!DISC_NULL(joint->child)) next = mu_mods_walk(DP(joint->child), next, want, out, depth + 1);
    if (!DISC_NULL(joint->next)) next = mu_mods_walk(DP(joint->next), next, want, out, depth + 1);
    return next;
}

static HSD_ImageDesc* frame_of(HSD_TexAnim* anim, int k, int w, int h, int format)
{
    HSD_ImageDesc* image;
    if (anim == NULL || k >= anim->n_imagetbl || (image = DP(DP(anim->imagetbl)[k])) == NULL) return NULL;
    if (image->width != w || image->height != h || image->format != format || DISC_NULL(image->image_ptr))
        return NULL;
    return image;
}

static unsigned fnv(const unsigned char* p, int n, unsigned h)
{
    int i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Row-major planes of the source pictures: IA4 as alpha << 4 | intensity, I4 as intensity. */
static unsigned char planes[SOURCES][LW * LH];

static void decode(const unsigned char* data, unsigned char* out, int ia4)
{
    int pos = 0, bx, by, x, y;
    const int w = ia4 ? LW : HW, h = ia4 ? LH : HH;
    if (ia4) {
        for (by = 0; by < 32; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; ++y)
                    for (x = 0; x < 8; ++x, ++pos)
                        if (by + y < h) out[(by + y) * w + bx + x] = data[pos];
    } else {
        for (by = 0; by < 32; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; ++y)
                    for (x = 0; x < 8; x += 2, ++pos)
                        if (by + y < h) {
                            out[(by + y) * w + bx + x] = data[pos] >> 4;
                            out[(by + y) * w + bx + x + 1] = data[pos] & 15;
                        }
    }
}

/* A piece's source pixel, its clip (clip_x, clip_y) applied. */
static unsigned piece_pixel(const Piece* p, const unsigned char* src, int w, int x, int y)
{
    if (p->clip_y > 0 && x >= p->clip_x && y >= p->clip_y) y = p->clip_y - 1;
    return src[y * w + x];
}

/* One word: the pieces side by side, centred. A piece keeps only the pixels within two of its own
 * letter's fill, so no edge of a neighbouring letter comes along; overlaps keep the brighter. */
static void compose(const Piece* pieces, unsigned char* out, int ia4)
{
    const int w = ia4 ? LW : HW, h = ia4 ? LH : HH;
    static unsigned char fill[LW * LH];
    int total = 0, cursor, i, x, y;
    for (i = 0; pieces[i].src >= 0; ++i)
        total += pieces[i].advance ? pieces[i].advance : pieces[i].right - pieces[i].left;
    for (i = 0; i < w * h; ++i) out[i] = 0;
    cursor = (w - total) / 2;
    for (i = 0; pieces[i].src >= 0; ++i) {
        const Piece* p = &pieces[i];
        const unsigned char* src = planes[p->src];
        for (y = 0; y < h; ++y) {
            const int shift = (int) (p->slope * (h - 1 - y) + 0.5f);
            for (x = 0; x < w; ++x) {
                const unsigned v = piece_pixel(p, src, w, x, y);
                const int in = x >= p->left + shift && x < p->right + shift;
                fill[y * w + x] = (unsigned char) (in && (ia4 ? (v & 15) >= 8 && (v >> 4) >= 8 : v >= 8));
            }
        }
        for (y = 0; y < h; ++y) {
            const int shift = (int) (p->slope * (h - 1 - y) + 0.5f);
            for (x = p->left + shift; x < p->right + shift; ++x) {
                int dx, dy, near = 0, tx = cursor + x - p->left;
                unsigned v, o;
                if (x < 0 || x >= w || tx < 0 || tx >= w) continue;
                for (dy = -2; dy <= 2 && !near; ++dy)
                    for (dx = -2; dx <= 2 && !near; ++dx)
                        if (y + dy >= 0 && y + dy < h && x + dx >= 0 && x + dx < w) near = fill[(y + dy) * w + x + dx];
                if (!near) continue;
                v = piece_pixel(p, src, w, x, y);
                o = out[y * w + tx];
                if (!ia4) {
                    out[y * w + tx] = (unsigned char) (v > o ? v : o);
                } else {
                    const unsigned va = v >> 4, vi = v & 15, oa = o >> 4, oi = o & 15;
                    const unsigned a = va > oa ? va : oa;
                    const unsigned in = va && oa ? (vi > oi ? vi : oi) : (va > oa ? vi : oi);
                    out[y * w + tx] = (unsigned char) (a << 4 | in);
                }
            }
        }
        cursor += p->advance ? p->advance : p->right - p->left;
    }
}

static void encode(const unsigned char* plane, unsigned char* data, int ia4)
{
    int pos = 0, bx, by, x, y;
    const int w = ia4 ? LW : HW, h = ia4 ? LH : HH;
    if (ia4) {
        for (by = 0; by < 32; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; ++y)
                    for (x = 0; x < 8; ++x)
                        data[pos++] = by + y < h ? plane[(by + y) * w + bx + x] : 0;
    } else {
        for (by = 0; by < 32; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; ++y)
                    for (x = 0; x < 8; x += 2)
                        data[pos++] = by + y < h ? (unsigned char) (plane[(by + y) * w + bx + x] << 4 |
                                                                    plane[(by + y) * w + bx + x + 1])
                                                 : 0;
    }
}

int mu_mods_build(HSD_TexAnim* cursor, HSD_TexAnim* panel)
{
    static unsigned char plane[LW * LH];
    const HSD_ImageDesc* src[SOURCES];
    unsigned hash = 0x811C9DC5u;
    int i;
    for (i = 0; i < SOURCES; ++i) {
        src[i] = i < HEAD0 ? frame_of(cursor, label_frames[i], LW, LH, GX_TF_IA4)
                           : frame_of(panel, i == HEAD0 ? 0 : 2, HW, HH, GX_TF_I4);
        if (src[i] == NULL) return -1;
        hash = fnv(DP(src[i]->image_ptr), i < HEAD0 ? LBYTES : HBYTES, hash);
    }
    if (hash != SOURCE_HASH) {
        OSReport("[mods] menu: the disc's menu labels are not the 1.02 ones (%08x); Trophies kept\n", hash);
        return -1;
    }
    for (i = 0; i < SOURCES; ++i) decode(DP(src[i]->image_ptr), planes[i], i < HEAD0);
    for (i = 0; i < LABELS; ++i) {
        compose(i < CHARACTERS ? words[i] : mu_mods_categories[i - CHARACTERS]->label, plane, 1);
        encode(plane, label_pixels[i], 1);
        mu_mods_label_desc[i] = *src[F00];
        DISC_SET(mu_mods_label_desc[i].image_ptr, label_pixels[i]);
        if (i >= CHARACTERS) {   /* the category's dimmed twin: IA4, alpha << 4 | intensity */
            unsigned char* dim = label_dim_pixels[i - CHARACTERS];
            int k;
            for (k = 0; k < LBYTES; ++k)
                dim[k] = (unsigned char) ((label_pixels[i][k] & 0xF0) | ((label_pixels[i][k] & 0x0F) * 2 / 5));
            mu_mods_label_dim_desc[i - CHARACTERS] = *src[F00];
            DISC_SET(mu_mods_label_dim_desc[i - CHARACTERS].image_ptr, dim);
        }
    }
    compose(heading_word, plane, 0);
    encode(plane, heading_pixels, 0);
    mu_mods_heading_desc = *src[HEAD0];
    DISC_SET(mu_mods_heading_desc.image_ptr, heading_pixels);
    return 1;
}

void mu_mods_put(HSD_TexAnim* anim, int k, HSD_ImageDesc* picture)
{
    if (anim != NULL && k < anim->n_imagetbl) DISC_SET(DP(anim->imagetbl)[k], picture);
}

/* ---- The newer words, cut straight from the tiled pictures: the Mods row's preview on the main
 * menu ("Browse", "Manage" where Trophies lists Gallery, Lottery, Collection), the Gallery's
 * headings ("Mods" and "Characters", "Stages" or "Manage" in place of "Trophies" and "Gallery") and
 * its list's side label ("LIST" of "TROPHY LIST"). The same cut as compose(): a piece is the columns
 * left..right at its baseline row, slanted by the italic slope, keeping only the pixels near its own
 * letter. A piece of a list picture is one line of it (baseline `base`); base 0 is the whole
 * picture. ---- */
typedef struct Pic { const unsigned char* data; int w, h, ia4; } Pic;
typedef struct Cut {
    signed char src;   /* -1 ends the word */
    unsigned char left, right, base;
    float slope;
} Cut;

static unsigned pic_get(const Pic* p, int x, int y)
{
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return 0;
    if (p->ia4) return p->data[((y >> 2) * (p->w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)];
    {
        const unsigned v = p->data[((y >> 3) * (p->w >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
        return x & 1 ? v & 15 : v >> 4;
    }
}

static void pic_set(const Pic* p, unsigned char* data, int x, int y, unsigned v)
{
    if (p->ia4) {
        data[((y >> 2) * (p->w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)] = (unsigned char) v;
    } else {
        unsigned char* b = &data[((y >> 3) * (p->w >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
        *b = (unsigned char) (x & 1 ? (*b & 0xF0) | (v & 15) : (*b & 0x0F) | (v & 15) << 4);
    }
}

static int strong(const Pic* p, unsigned v)
{
    return p->ia4 ? (v & 15) >= 8 && (v >> 4) >= 8 : v >= 8;
}

static int cut_shift(const Cut* c, int base, int y)
{
    return (int) (c->slope * (float) (base - 1 - y) + 0.5f);
}

/* One word into `out` (shaped as `shape`, cleared by the caller): `gap` columns between letters,
 * from column x0 (-1: centred) with its baseline at row dbase (0: where the source has it). */
static void cut_word(const Cut* cuts, const Pic* const* src, const Pic* shape, unsigned char* out, int gap,
                     int x0, int dbase)
{
    Pic dst = *shape;
    int total = 0, cursor, i, x, y;
    dst.data = out;
    for (i = 0; cuts[i].src >= 0; ++i) total += cuts[i].right - cuts[i].left + gap;
    cursor = x0 >= 0 ? x0 : (shape->w - total) / 2;
    for (i = 0; cuts[i].src >= 0; ++i) {
        const Cut* c = &cuts[i];
        const Pic* p = src[c->src];
        const int base = c->base ? c->base : p->h;
        const int lo = c->base ? (base - 18 > 0 ? base - 18 : 0) : 0;
        const int hi = c->base ? (base + 6 < p->h ? base + 6 : p->h) : p->h;
        const int to = dbase ? dbase : base;
        for (y = lo; y < hi; ++y) {
            const int shift = cut_shift(c, base, y);
            for (x = c->left + shift; x < c->right + shift; ++x) {
                const int tx = cursor + x - c->left, ty = y - base + to;
                int dx, dy, near = 0;
                unsigned v, o;
                if (x < 0 || x >= p->w || tx < 0 || tx >= shape->w || ty < 0 || ty >= shape->h) continue;
                for (dy = -2; dy <= 2 && !near; ++dy) {
                    const int yy = y + dy;
                    if (yy < lo || yy >= hi) continue;
                    for (dx = -2; dx <= 2 && !near; ++dx) {
                        const int xx = x + dx, s = cut_shift(c, base, yy);
                        near = xx >= c->left + s && xx < c->right + s && strong(p, pic_get(p, xx, yy));
                    }
                }
                if (!near) continue;
                v = pic_get(p, x, y);
                o = pic_get(&dst, tx, ty);
                if (!p->ia4) {
                    pic_set(&dst, out, tx, ty, v > o ? v : o);
                } else {
                    const unsigned va = v >> 4, vi = v & 15, oa = o >> 4, oi = o & 15;
                    const unsigned a = va > oa ? va : oa;
                    const unsigned in = va && oa ? (vi > oi ? vi : oi) : (va > oa ? vi : oi);
                    pic_set(&dst, out, tx, ty, a << 4 | in);
                }
            }
        }
        cursor += c->right - c->left + gap;
    }
}

enum { P_STADIUM, P_TRAINING, P_ARCHIVES, P_CUSTOM, P_ONE_P, V_REGULAR, V_TROPHIES, V_SPECIAL, V_RECORDS,
       V_CLASSIC, NEW_SOURCES };
#define NEW_SOURCE_HASH 0xDA2B889Du /* FNV-1a of the 1.02 pictures below, in this order */
enum { VW = 128, VH = 104, VBYTES = 128 * 104 / 2, SW = 160, SH = 12, SBYTES = 160 * 12 };

/* "Browse" over "Manage" where 1-P Mode's preview has Classic over Adventure: B of Bonus Records,
 * r e y of Gallery, o n of Collection, w of How to Play, s a of Classic, M of Special Movie, g of
 * Regular Match. */
static const Cut preview_browse[] = {{V_RECORDS, 1, 11, 59, 0.3f},  {V_TROPHIES, 52, 59, 29, 0.3f},
                                     {V_TROPHIES, 14, 23, 89, 0.3f}, {V_SPECIAL, 24, 38, 81, 0.3f},
                                     {V_CLASSIC, 33, 42, 40, 0.3f},  {V_TROPHIES, 40, 50, 29, 0.3f}, {-1}};
static const Cut preview_manage[] = {{V_SPECIAL, 73, 87, 38, 0.3f}, {V_CLASSIC, 21, 30, 40, 0.3f},
                                     {V_TROPHIES, 91, 100, 89, 0.3f}, {V_CLASSIC, 21, 30, 40, 0.3f},
                                     {V_REGULAR, 21, 29, 18, 0.3f},  {V_TROPHIES, 40, 50, 29, 0.3f}, {-1}};
/* The headings: C of Custom Rules, h r c e s of Archives, S t a of Stadium, n g of Training, M of 1-P
 * Mode. */
#define HC(src, left, right) {src, left, right, 0, 0.3f}
static const Cut heading_words[HEADINGS][11] = {
    {HC(P_CUSTOM, 0, 14), HC(P_ARCHIVES, 68, 83), HC(P_STADIUM, 51, 66), HC(P_ARCHIVES, 37, 48),
     HC(P_STADIUM, 51, 66), HC(P_ARCHIVES, 49, 64), HC(P_STADIUM, 38, 48), HC(P_ARCHIVES, 113, 129),
     HC(P_ARCHIVES, 37, 48), HC(P_ARCHIVES, 132, 145), {-1}},
    {HC(P_STADIUM, 18, 35), HC(P_STADIUM, 38, 48), HC(P_STADIUM, 51, 66), HC(P_TRAINING, 124, 140),
     HC(P_ARCHIVES, 113, 129), HC(P_ARCHIVES, 132, 145), {-1}},
    {HC(P_ONE_P, 69, 92), HC(P_STADIUM, 51, 66), HC(P_TRAINING, 77, 92), HC(P_STADIUM, 51, 66),
     HC(P_TRAINING, 124, 140), HC(P_ARCHIVES, 113, 129), {-1}},
    /* Particles: P of 1-P Mode, a t of Stadium, r i c e s of Archives, l of Custom Rules */
    {HC(P_ONE_P, 41, 58), HC(P_STADIUM, 51, 66), HC(P_ARCHIVES, 37, 48), HC(P_STADIUM, 38, 48),
     HC(P_ARCHIVES, 86, 93), HC(P_ARCHIVES, 49, 64), HC(P_CUSTOM, 126, 131), HC(P_ARCHIVES, 113, 129),
     HC(P_ARCHIVES, 132, 145), {-1}},
    /* Screens (Menus' list; the lettering has no u for "Menus"): S of Stadium, c r e s of Archives, n
     * of Training */
    {HC(P_STADIUM, 18, 35), HC(P_ARCHIVES, 49, 64), HC(P_ARCHIVES, 37, 48), HC(P_ARCHIVES, 113, 129),
     HC(P_ARCHIVES, 113, 129), HC(P_TRAINING, 77, 92), HC(P_ARCHIVES, 132, 145), {-1}},
};
#undef HC
/* LIST of TROPHY LIST (no M or D in that lettering, so no "MOD LIST"). */
static const Cut list_word[] = {{0, 96, 146, 0, 0.7f}, {-1}};

static unsigned char preview_pixels[VBYTES] __attribute__((aligned(32)));
static unsigned char heading_word_pixels[HEADINGS][HBYTES] __attribute__((aligned(32)));
static unsigned char list_pixels[SBYTES] __attribute__((aligned(32)));
HSD_ImageDesc mu_mods_preview_desc;
int mu_mods_more_composed;  /* 1 built, -1 not the 1.02 pictures (the preview and Gallery keep theirs) */

int mu_mods_build_more(HSD_TexAnim* panel84, HSD_TexAnim* panel85, HSD_TexAnim* contop22)
{
    static const unsigned char frames[NEW_SOURCES] = {3, 5, 17, 8, 0, 0, 2, 10, 11, 13};
    const HSD_ImageDesc* desc[NEW_SOURCES];
    Pic pics[NEW_SOURCES];
    const Pic* src[NEW_SOURCES];
    Pic heading_shape = {NULL, HW, HH, 0}, preview_shape = {NULL, VW, VH, 0};
    unsigned hash = 0x811C9DC5u;
    int i;
    for (i = 0; i < NEW_SOURCES; ++i) {
        const int panel = i <= P_ONE_P;
        desc[i] = panel ? frame_of(i == P_ONE_P ? panel84 : panel85, frames[i], HW, HH, GX_TF_I4)
                        : frame_of(contop22, frames[i], VW, VH, GX_TF_I4);
        if (desc[i] == NULL) return -1;
        pics[i].data = DP(desc[i]->image_ptr);
        pics[i].w = panel ? HW : VW;
        pics[i].h = panel ? HH : VH;
        pics[i].ia4 = 0;
        src[i] = &pics[i];
        hash = fnv(pics[i].data, panel ? HBYTES : VBYTES, hash);
    }
    if (hash != NEW_SOURCE_HASH) {
        OSReport("[mods] menu: the disc's preview and heading pictures are not the 1.02 ones (%08x)\n", hash);
        return -1;
    }
    memset(preview_pixels, 0, sizeof preview_pixels);
    cut_word(preview_browse, src, &preview_shape, preview_pixels, 2, 1, 40);
    cut_word(preview_manage, src, &preview_shape, preview_pixels, 2, 1, 79);
    mu_mods_preview_desc = *desc[V_CLASSIC];
    DISC_SET(mu_mods_preview_desc.image_ptr, preview_pixels);
    for (i = 0; i < HEADINGS; ++i) {
        memset(heading_word_pixels[i], 0, HBYTES);
        cut_word(heading_words[i], src, &heading_shape, heading_word_pixels[i], 2, -1, 0);
    }
    return 1;
}

/* Toy_80307470 (ty/toy.c), the Gallery's panel just loaded: while it runs as the mod list, its
 * headings read "Mods" and the section, and its list's side label "LIST". Each picture is swapped
 * only when it is the 1.02 one, on this load's objects (the archive's own stay as they are). */
#define TROPHIES_HASH 0x3C4ADAEFu
#define GALLERY_HASH 0x6ED9A696u
#define TROPHY_LIST_HASH 0x62D92C65u

static HSD_ImageDesc gallery_desc[2];  /* "Mods", the section */
static HSD_ImageDesc list_desc;

void mu_mods_swap_panel_pictures(HSD_JObj* jobj, int depth)
{
    for (; jobj != NULL && depth < 64; jobj = jobj->next) {
        HSD_DObj* dobj;
        for (dobj = union_type_dobj(jobj) ? jobj->u.dobj : NULL; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* texture;
            if (dobj->mobj == NULL) continue;
            for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
                HSD_ImageDesc* now = texture->imagedesc;
                const unsigned char* data;
                unsigned hash;
                if (now == NULL || DISC_NULL(now->image_ptr)) continue;
                data = DP(now->image_ptr);
                if (now->width == HW && now->height == HH && now->format == GX_TF_I4) {
                    hash = fnv(data, HBYTES, 0x811C9DC5u);
                    if (hash == TROPHIES_HASH || hash == GALLERY_HASH) {
                        HSD_ImageDesc* to = &gallery_desc[hash == GALLERY_HASH];
                        const unsigned char* pixels = hash == TROPHIES_HASH ? heading_pixels
                                                      : heading_word_pixels[mu_mods_menu_manage() ? H_MANAGE
                                                                            /* Browse: the category's own */
                                                                            : mu_mods_categories[mu_mods_gallery_category()]
                                                                                  ->heading];
                        *to = *now;
                        DISC_SET(to->image_ptr, pixels);
                        texture->imagedesc = to;
                    }
                } else if (now->width == SW && now->height == SH && now->format == GX_TF_IA4 &&
                           fnv(data, SBYTES, 0x811C9DC5u) == TROPHY_LIST_HASH) {
                    const Pic list = {data, SW, SH, 1}, shape = {NULL, SW, SH, 1};
                    const Pic* src[1] = {&list};
                    memset(list_pixels, 0, sizeof list_pixels);
                    cut_word(list_word, src, &shape, list_pixels, 0, -1, 0);
                    list_desc = *now;
                    DISC_SET(list_desc.image_ptr, list_pixels);
                    texture->imagedesc = &list_desc;
                }
            }
        }
        if (!(jobj->flags & JOBJ_INSTANCE)) mu_mods_swap_panel_pictures(jobj->child, depth + 1);
    }
}
