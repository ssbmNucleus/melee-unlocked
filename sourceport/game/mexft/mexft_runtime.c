/* The m-ex runtime calls a fighter's MexTK C makes, native (mexft/mex.h declares them under their
 * MexTK names). On the console these are m-ex's own code (0x803D7xxx), not the game's, so no decomp
 * function stands at their addresses: they go to the registry's services instead, the ones the
 * hand-ported ACE fighters call for the same jobs (ace/blastoise/blastoise.c). */
#include "mexft_adapter.h"

#include <melee/ft/inlines.h>
#include <melee/lb/types.h>

/* MEX_GetFtItemID (0x803D7088): the game's item kind of the fighter's article `local`. */
int mexft_GetFtItemID(HSD_GObj* gobj, int local)
{
    return mu_ak_item_kind(GET_FIGHTER(gobj)->kind, local);
}

/* MEX_IndexFighterItem (0x803D7058): article `local`'s data (an ItemDesc in the fighter file) for
 * the item code. */
void mexft_IndexFighterItem(int kind, void* article, int local)
{
    mu_ak_register_article((FighterKind) kind, article, local);
}

/* How many articles the fighter of `kind` has (MxDt's item lookup): mexft/mex.h reads that many
 * of the article pointers its fighter file holds. */
int mexft_ItemCount(int kind)
{
    int n = 0;
    while (n < 64 && mu_ak_item_kind(kind, n) >= 0) {
        n++;
    }
    return n;
}

/* A hitbox's console bytes +40 and +41 written whole (mexft/mex.h mexft_hit_x40/x41). On the
 * console they are one 16-bit bitfield counted from its top bit: +40 holds x40_b0..b3 and the top
 * four bits of x40_b4, +41 the bottom four bits of x40_b4 and x41_b4..b7. */
void mexft_HitSetX40(HitCapsule* h, int v)
{
    h->x40_b0 = v >> 7 & 1;
    h->x40_b1 = v >> 6 & 1;
    h->x40_b2 = v >> 5 & 1;
    h->x40_b3 = v >> 4 & 1;
    h->x40_b4 = (h->x40_b4 & 0x0F) | (v & 0x0F) << 4;
}

void mexft_HitSetX41(HitCapsule* h, int v)
{
    h->x40_b4 = (h->x40_b4 & 0xF0) | (v >> 4 & 0x0F);
    h->x41_b4 = v >> 3 & 1;
    h->x41_b5 = v >> 2 & 1;
    h->x41_b6 = v >> 1 & 1;
    h->x41_b7 = v & 1;
}
