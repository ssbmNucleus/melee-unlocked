/* The 1-P menu's Event Match row as "Training Mode" while TM-CE is on (Event Match opens TM-CE's
 * event menu then: the Lab and the drills, shim/mu_tmce.c).
 *
 * Nothing new is drawn: "Training Mode" is cut from the player's disc (MnMaAll.usd) the way
 * shim/mu_mods_menu.c cuts "Mods", from "Training" (the Training row's own label) and "Mode" (of
 * 1-P Mode), then narrowed to the width Event Match's own picture has:
 *   the row's label (cursor frame 6), the italic heading over the description (panel joint 85
 *   frame 1), the line in 1-P Mode's preview on the main menu (preview frame 0, line 2).
 * The description line names TM-CE and its authors. With TM-CE off, or when the disc's pictures are
 * not the 1.02 ones the words are cut from, the row stays Event Match as the game has it. */
#include <dolphin/os.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>
#include <string.h>

#include <mu_native.h>

enum { LW = 176, LH = 30, LBYTES = 176 * 32, HW = 168, HH = 28, HBYTES = 168 * 32 / 2, VW = 128, VH = 104,
       VBYTES = 128 * 104 / 2, WIDE = 384 };
/* Sources, in the order of SOURCE_HASH: the cursor labels Training, 1-P Mode, Event Match; the panel
 * headings Training, 1-P Mode, Event Match; the previews of 1-P Mode and of Options. */
enum { C_TRAINING, C_ONE_P, C_EVENT, P_TRAINING, P_ONE_P, P_EVENT, V_ONE_P, V_OPTIONS, SOURCES };
#define SOURCE_HASH 0x33D4BE62u /* FNV-1a of the 1.02 pictures above, in this order */
#define MENU_KIND_1P 1
#define SEL_1P_EVENT 1

typedef struct Pic { const unsigned char* data; int w, h, ia4; } Pic;
/* A piece: columns left..right of a source at its baseline row `base` (0: the whole picture), slanted
 * by the italic slope; SPACE adds `left` blank columns; END ends the word. */
typedef struct Cut {
    signed char src;
    unsigned char left, right, base;
    float slope;
} Cut;
#define SPACE (-2)
#define END (-1)

static const Cut cursor_word[] = {{C_TRAINING, 30, 146, 0, 0.0f}, {SPACE, 6}, {C_ONE_P, 81, 158, 0, 0.0f}, {END}};
static const Cut panel_word[] = {{P_TRAINING, 12, 145, 0, 0.3f}, {SPACE, 1}, {P_ONE_P, 64, 150, 0, 0.3f}, {END}};
/* The preview's line: Training of the Training line, M e of Regular Match, o d of Sound. */
static const Cut preview_word[] = {{V_ONE_P, 0, 90, 98, 0.3f},  {V_ONE_P, 66, 80, 18, 0.3f},
                                   {V_OPTIONS, 12, 23, 37, 0.3f}, {V_OPTIONS, 47, 58, 37, 0.3f},
                                   {V_ONE_P, 11, 20, 18, 0.3f},  {END}};
#define PREVIEW_LINE_TOP 24     /* Event Match's line in 1-P Mode's preview: rows 24..50, baseline 45 */
#define PREVIEW_LINE_END 51
#define PREVIEW_BASE 45

static unsigned char label_pixels[LBYTES] __attribute__((aligned(32)));
static unsigned char heading_pixels[HBYTES] __attribute__((aligned(32)));
static unsigned char preview_pixels[VBYTES] __attribute__((aligned(32)));
static HSD_ImageDesc label_desc, heading_desc, preview_desc;
static unsigned char wide[VH * WIDE];
static int composed;  /* 1 built, -1 not the 1.02 pictures (Event Match keeps its own) */
static int shown;     /* this menu load shows Training Mode */

typedef struct Tables { HSD_TexAnim* anim[4]; int count; } Tables;

/* MatAnimJoint walk in the archive's order (as shim/mu_mods_menu.c). */
static int walk(HSD_MatAnimJoint* joint, int index, int want, Tables* out, int depth)
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
    if (!DISC_NULL(joint->child)) next = walk(DP(joint->child), next, want, out, depth + 1);
    if (!DISC_NULL(joint->next)) next = walk(DP(joint->next), next, want, out, depth + 1);
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

static void put(HSD_TexAnim* anim, int k, HSD_ImageDesc* picture)
{
    if (anim != NULL && k < anim->n_imagetbl) DISC_SET(DP(anim->imagetbl)[k], picture);
}

static unsigned fnv(const unsigned char* p, int n, unsigned h)
{
    int i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Tiled pixels: IA4 as alpha << 4 | intensity, I4 as intensity. */
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
    if (x < 0 || y < 0 || x >= p->w || y >= p->h) return;
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

/* One word into `out` (shaped as `shape`), its baseline at row dbase (0: where the source has it),
 * from column x0 (-1: centered). The pieces are laid side by side `gap` apart as shim/mu_mods_menu.c
 * cut_word() lays them (a piece keeps only the pixels near its own letter), then the word is narrowed
 * to at most maxw columns, each column the coverage-weighted mean of the columns it covers. Only
 * pixels that come out brighter than what `out` has are written. */
static void cut_line(const Cut* cuts, const Pic* const* src, const Pic* shape, unsigned char* out, int gap,
                     int x0, int dbase, int maxw)
{
    const int ia4 = shape->ia4, h = shape->h;
    Pic dst = *shape;
    int cursor = 16, a = WIDE, b = 0, n, tw, start, i, x, y, dx;
    dst.data = out;
    memset(wide, 0, sizeof wide);
    for (i = 0; cuts[i].src != END; ++i) {
        const Cut* c = &cuts[i];
        const Pic* p;
        int base, lo, hi, to;
        if (c->src == SPACE) {
            cursor += c->left;
            continue;
        }
        p = src[c->src];
        base = c->base ? c->base : p->h;
        lo = c->base ? (base - 18 > 0 ? base - 18 : 0) : 0;
        hi = c->base ? (base + 6 < p->h ? base + 6 : p->h) : p->h;
        to = dbase ? dbase : base;
        for (y = lo; y < hi; ++y) {
            const int shift = cut_shift(c, base, y);
            for (x = c->left + shift; x < c->right + shift; ++x) {
                const int tx = cursor + x - c->left, ty = y - base + to;
                int ddx, ddy, near = 0;
                unsigned v, o;
                if (x < 0 || x >= p->w || tx < 0 || tx >= WIDE || ty < 0 || ty >= h) continue;
                for (ddy = -2; ddy <= 2 && !near; ++ddy) {
                    const int yy = y + ddy;
                    if (yy < lo || yy >= hi) continue;
                    for (ddx = -2; ddx <= 2 && !near; ++ddx) {
                        const int xx = x + ddx, s = cut_shift(c, base, yy);
                        near = xx >= c->left + s && xx < c->right + s && strong(p, pic_get(p, xx, yy));
                    }
                }
                if (!near) continue;
                v = pic_get(p, x, y);
                o = wide[ty * WIDE + tx];
                if (!ia4) {
                    wide[ty * WIDE + tx] = (unsigned char) (v > o ? v : o);
                } else {
                    const unsigned va = v >> 4, vi = v & 15, oa = o >> 4, oi = o & 15;
                    const unsigned al = va > oa ? va : oa;
                    const unsigned in = va && oa ? (vi > oi ? vi : oi) : (va > oa ? vi : oi);
                    wide[ty * WIDE + tx] = (unsigned char) (al << 4 | in);
                }
            }
        }
        cursor += c->right - c->left + gap;
    }
    for (y = 0; y < h; ++y)
        for (x = 0; x < WIDE; ++x)
            if (wide[y * WIDE + x]) {
                if (x < a) a = x;
                if (x + 1 > b) b = x + 1;
            }
    if (b <= a) return;
    n = b - a;
    tw = n < maxw ? n : maxw;
    start = x0 >= 0 ? x0 : (shape->w - tw) / 2;
    for (dx = 0; dx < tw; ++dx) {
        const float s0 = (float) a + (float) (dx * n) / (float) tw;
        const float s1 = (float) a + (float) ((dx + 1) * n) / (float) tw;
        for (y = 0; y < h; ++y) {
            float sa = 0.0f, si = 0.0f, sw = 0.0f;
            unsigned v, o;
            for (x = (int) s0; (float) x < s1; ++x) {
                const float hi_edge = (float) (x + 1) < s1 ? (float) (x + 1) : s1;
                const float lo_edge = (float) x > s0 ? (float) x : s0;
                const float weight = hi_edge - lo_edge;
                const unsigned w = wide[y * WIDE + x];
                const unsigned va = ia4 ? w >> 4 : w, vi = ia4 ? w & 15 : w;
                sa += (float) va * weight;
                si += (float) (va * vi) * weight;
                sw += weight;
            }
            if (sw <= 0.0f) continue;
            if (ia4) {
                const unsigned al = (unsigned) (sa / sw + 0.5f);
                const unsigned in = sa > 0.0f ? (unsigned) (si / sa + 0.5f) : 0;
                v = al << 4 | (in > 15 ? 15 : in);
            } else {
                v = (unsigned) (sa / sw + 0.5f);
            }
            o = pic_get(&dst, start + dx, y);
            if (v > o) pic_set(&dst, out, start + dx, y, v);
        }
    }
}

static int build(HSD_TexAnim* cursor, HSD_TexAnim* panel84, HSD_TexAnim* panel85, HSD_TexAnim* preview)
{
    static const unsigned char frames[SOURCES] = {9, 0, 6, 4, 0, 1, 0, 3};
    const HSD_ImageDesc* desc[SOURCES];
    Pic pics[SOURCES];
    const Pic* src[SOURCES];
    const Pic label_shape = {NULL, LW, LH, 1}, heading_shape = {NULL, HW, HH, 0}, preview_shape = {NULL, VW, VH, 0};
    unsigned hash = 0x811C9DC5u;
    int i, x, y;
    for (i = 0; i < SOURCES; ++i) {
        const int label = i <= C_EVENT, heading = i >= P_TRAINING && i <= P_EVENT;
        HSD_TexAnim* table = label ? cursor : heading ? (i == P_ONE_P ? panel84 : panel85) : preview;
        desc[i] = label     ? frame_of(table, frames[i], LW, LH, GX_TF_IA4)
                  : heading ? frame_of(table, frames[i], HW, HH, GX_TF_I4)
                            : frame_of(table, frames[i], VW, VH, GX_TF_I4);
        if (desc[i] == NULL) {
            OSReport("[tmce] menu: the menu's label pictures are not where 1.02 has them; Event Match kept\n");
            return -1;
        }
        pics[i].data = DP(desc[i]->image_ptr);
        pics[i].w = label ? LW : heading ? HW : VW;
        pics[i].h = label ? LH : heading ? HH : VH;
        pics[i].ia4 = label;
        src[i] = &pics[i];
        hash = fnv(pics[i].data, label ? LBYTES : heading ? HBYTES : VBYTES, hash);
    }
    if (hash != SOURCE_HASH) {
        OSReport("[tmce] menu: the disc's menu labels are not the 1.02 ones (%08x); Event Match kept\n", hash);
        return -1;
    }
    memset(label_pixels, 0, sizeof label_pixels);
    cut_line(cursor_word, src, &label_shape, label_pixels, 0, -1, 0, 166);
    label_desc = *desc[C_EVENT];
    DISC_SET(label_desc.image_ptr, label_pixels);

    memset(heading_pixels, 0, sizeof heading_pixels);
    cut_line(panel_word, src, &heading_shape, heading_pixels, 0, -1, 0, 152);
    heading_desc = *desc[P_EVENT];
    DISC_SET(heading_desc.image_ptr, heading_pixels);

    /* 1-P Mode's preview with its second line (Event Match) cleared and cut again. */
    memcpy(preview_pixels, pics[V_ONE_P].data, VBYTES);
    for (y = PREVIEW_LINE_TOP; y < PREVIEW_LINE_END; ++y)
        for (x = 0; x < VW; ++x) pic_set(&preview_shape, preview_pixels, x, y, 0);
    cut_line(preview_word, src, &preview_shape, preview_pixels, 2, 1, PREVIEW_BASE, 118);
    preview_desc = *desc[V_ONE_P];
    DISC_SET(preview_desc.image_ptr, preview_pixels);
    return 1;
}

/* mnMain_Scene_OnEnter, after shim/mu_mods_menu.c set its own: the archive is the scene's, read
 * again at each menu load, so its tables are set again each time. */
void mu_tmce_menu_load(HSD_MatAnimJoint* cursor_matanim, HSD_MatAnimJoint* panel_matanim,
                       HSD_MatAnimJoint* contop_matanim)
{
    Tables cursor = {0}, panel84 = {0}, panel85 = {0}, preview = {0};
    static int logged;
    int i;
    shown = 0;
    if (!mu_tmce_active()) return;
    walk(cursor_matanim, 0, 3, &cursor, 0);
    walk(panel_matanim, 0, 84, &panel84, 0);
    walk(panel_matanim, 0, 85, &panel85, 0);
    walk(contop_matanim, 0, 22, &preview, 0);
    if (composed == 0) {
        composed = cursor.count && panel84.count && panel85.count && preview.count
                       ? build(cursor.anim[0], panel84.anim[0], panel85.anim[0], preview.anim[0])
                       : -1;
    }
    /* The description names TM-CE even when the pictures could not be cut. */
    shown = 1;
    if (composed < 0) return;
    for (i = 0; i < cursor.count; ++i) put(cursor.anim[i], 6, &label_desc);
    for (i = 0; i < panel85.count; ++i) put(panel85.anim[i], 1, &heading_desc);
    for (i = 0; i < preview.count; ++i) put(preview.anim[i], 0, &preview_desc);
    if (!logged++) OSReport("[tmce] menu: Training Mode in place of Event Match\n");
}

void mu_tmce_menu_describe(HSD_Text* text, int menu_kind, int selection)
{
    if (!shown || text == NULL || menu_kind != MENU_KIND_1P || selection != SEL_1P_EVENT) return;
    /* As short as the game's own lines: a longer one is squeezed until its letters run together. */
    mu_slippi_menu_description(text, "TM-CE by Aitch, from UnclePunch's mod.");
}
