/* Ranked tier marker and division label. SPDX-License-Identifier: GPL-3.0-or-later */
#include <mu_rank_badge.h>
#include "mu_rank_text.h"
#include <sysdolphin/baselib/gobj.h>
#include <dolphin/dvd.h>
#include <melee/lb/lbarchive.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/sislib.h>
#include <dolphin/gx.h>
#include <string.h>


struct MuRankBadge {
    HSD_Text* text;
    struct MuRankBadge* next;
    void (*text_destroy)(void*);
    MuPlayerRank rank;
    float x, y, size, offset, scale, alpha, glow;
    HSD_TexAnim* icons;
    int subtext, art_only;
};
static MuRankBadge* badges;

/* Slippi 3.7 uses the texture animation frames in slpCSS.rankIcons for both scenes.
 * Each scene owns its archive. Existing badge entities retain their own scene's image table. */
static HSD_TexAnim* scene_icons;
typedef struct {
    DISC_PTR(void) chat_select, chat_message, mode, connect_help;
    DISC_PTR(DynamicModelDesc) rank_icons;
} DISC_STRUCT BadgeCssDesc;

void* mu_rank_badge_load_css(void)
{
    const char* path = DVDConvertPathToEntrynum("SlippiCSS_gui.dat") >= 0 ? "SlippiCSS_gui.dat" :
                       DVDConvertPathToEntrynum("slpCSS.dat") >= 0 ? "slpCSS.dat" : NULL;
    HSD_Archive* archive = path ? lbArchive_LoadArchive(path) : NULL;
    BadgeCssDesc* css = archive ? HSD_ArchiveGetPublicAddress(archive, "slpCSS") : NULL;
    DynamicModelDesc* model = css ? DP(css->rank_icons) : NULL;
    HSD_MatAnimJointSlot* sets = model ? DP(model->matanims) : NULL;
    HSD_MatAnimJoint* joint = sets ? DP(sets[0]) : NULL;
    scene_icons = NULL;
    while (joint) {
        HSD_MatAnim* material = DP(joint->matanim);
        if (material && DP(material->texanim)) {
            scene_icons = DP(material->texanim);
            break;
        }
        joint = DP(joint->child);
    }
    return css;
}

/* Native v1 has no OS accessibility preference in its host ABI. This explicit
 * presentation-only override applies to the Ranked reveal.
 * It reads this one named flag; never enumerate the process environment. */
char* getenv(const char* name);
int mu_rank_badge_reduced_motion(void)
{
    const char* flag = getenv("MU_REDUCED_MOTION");
    return flag && (strcmp(flag, "1") == 0 || strcmp(flag, "true") == 0);
}

static MuRankBadge* badge_for_text(void* text)
{
    MuRankBadge* b;
    for (b = badges; b; b = b->next) if (b->text == text) return b;
    return NULL;
}

static void badge_destroy(void* data)
{
    MuRankBadge** link = &badges;
    while (*link) {
        MuRankBadge* b = *link;
        if (b->text == data) {
            void (*destroy)(void*) = b->text_destroy;
            *link = b->next;
            HSD_Free(b);
            destroy(data);
            return;
        }
        link = &b->next;
    }
}

static void badge_draw(void* object)
{
    HSD_GObj* gobj = object;
    MuRankBadge* b = badge_for_text(gobj->user_data);
    GXTexObj tex;
    GXTlutObj tlut;
    HSD_ImageDesc* image;
    HSD_TlutDesc* colors;
    int frame;
    GXColor white = {255, 255, 255, 255};
    float x, y, radius;
    if (!b || !b->rank.known || b->alpha <= 0 || !b->icons) return;
    frame = b->rank.order; /* SlippiRank: Unranked 0, Bronze I through Master III 1-18, Grandmaster 19. */
    if (frame < 0 || frame >= b->icons->n_imagetbl || frame >= b->icons->n_tluttbl ||
        !DP(b->icons->imagetbl) || !DP(b->icons->tluttbl)) return;
    image = DP(DP(b->icons->imagetbl)[frame]);
    colors = DP(DP(b->icons->tluttbl)[frame]);
    if (!image || !colors || !DP(image->image_ptr) || !DP(colors->lut)) return;
    GXInitTlutObj(&tlut, DP(colors->lut), colors->fmt, colors->n_entries);
    GXLoadTlut(&tlut, GX_TLUT0);
    GXInitTexObjCI(&tex, DP(image->image_ptr), image->width, image->height, image->format,
                   GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
    GXInitTexObjLOD(&tex, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&tex, GX_TEXMAP0);
    white.a = (u8)(255 * b->alpha);
    GXSetTevColor(GX_TEVREG0, white);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
    radius = 12 * b->size * b->scale;
    x = b->x + 12 * b->size; y = -(b->y + b->offset + 12 * b->size);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x-radius, y+radius, b->text->pos_z); GXTexCoord2f32(0,0);
    GXPosition3f32(x+radius, y+radius, b->text->pos_z); GXTexCoord2f32(1,0);
    GXPosition3f32(x+radius, y-radius, b->text->pos_z); GXTexCoord2f32(1,1);
    GXPosition3f32(x-radius, y-radius, b->text->pos_z); GXTexCoord2f32(0,1);
    GXEnd();
}

MuRankBadge* mu_rank_badge_create(struct HSD_GObj* parent, float x, float y, float scale, const MuPlayerRank* rank)
{
    MuRankBadge* b;
    /* SIS owns the drawing entity independently, as in the shared fallback.
     * Callers may free before their parent; scene teardown also frees it. */
    (void)parent;
    if (scale <= 0) return NULL;
    b = HSD_MemAlloc(sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof *b);
    b->icons = scene_icons;
    b->text = mu_rank_text_create();
    if (!b->text) { HSD_Free(b); return NULL; }
    b->text->pos_x = x; b->text->pos_y = y;
    b->text->box_size_x = b->text->box_size_y = 20;
    b->x = x; b->y = y; b->size = scale; b->scale = b->alpha = 1;
    b->text->x34.x = b->text->x34.y = .35f*scale;
    b->text->default_kerning = 1;
    /* The division numeral sits centered under the art (only the setup screen draws it; CSS and
     * reveal are art only). At 80 units to the right it fell off the 640-wide setup screen for
     * the right-hand badge and floated under the left player's name (build 31, 11-01).
     * Build 37: at .35*scale it crossed the setup header's bottom border (glyph about y 180-207
     * px, border about 195), so it is a size smaller and sits between the art and that border.
     * SIS draws a glyph about 35*(its scale) below its row (measured on build 31's frames). */
    b->text->default_alignment = 1;
    b->subtext = HSD_SisLib_803A6B98(b->text, 12*scale, 11.4f*scale, "%s", "");
    HSD_SisLib_803A7548(b->text, b->subtext, .25f*scale, .25f*scale);
    b->text_destroy = b->text->entity->user_data_remove_func;
    b->text->entity->user_data_remove_func = badge_destroy;
    b->text->render_callback = badge_draw;
    b->next = badges; badges = b;
    mu_rank_badge_set(b, rank);
    return b;
}

void mu_rank_badge_set(MuRankBadge* b, const MuPlayerRank* rank)
{
    static const char* const roman[] = {"", "I", "II", "III", "IV"};
    const char* numeral = "";
    if (!b) return;
    if (rank) b->rank = *rank;
    else memset(&b->rank, 0, sizeof b->rank);
    if (!b->rank.known) numeral = "?";
    else if (b->rank.placed && b->rank.tier >= 1 && b->rank.tier <= 6 && b->rank.division <= 3)
        numeral = roman[b->rank.division];
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", b->art_only ? "" : numeral);
}

/* CSS has a separate full label; keep enlarged art within its column rather
 * than repeating the division and Practice word on the badge's right edge. */
void mu_rank_badge_art_only(MuRankBadge* b)
{
    if (!b) return;
    b->art_only = 1;
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", "");
}

/* Private to Track C: offsets in SIS world units; other callers retain the
 * shared create/set/free interface. Alpha/scale affect the division too. */
void mu_rank_badge_pose(MuRankBadge* b, float y, float scale, float alpha, float glow)
{
    GXColor white = {255,255,255,255};
    if (!b) return;
    b->offset = y; b->scale = scale; b->alpha = alpha; b->glow = glow;
    b->text->pos_y = b->y+y;
    b->text->x34.x = b->text->x34.y = .35f*b->size*scale;
    white.a = (u8)(alpha*255);
    HSD_SisLib_803A74F0(b->text, b->subtext, &white);
}

void mu_rank_badge_free(MuRankBadge* b)
{
    if (b) HSD_SisLib_803A5CC4(b->text);
}
