/* The decomp side of a fighter compiled from MexTK C (mexft/, tools/mexft/mexft_prepare.py).
 *
 * The fighter's C is built as a module of its own (tmce_module: its globals private, its m-ex
 * exports named tmce_mexft_<fighter>_<Export>). The descriptor the registry reads
 * (akaneia/mu_ak_fighter.h) is generated next to it, and uses the conversions here for the two
 * exports that are data, not events:
 *
 *   move_logic    MexTK FtState rows -> decomp MotionState rows (same size; the move id and the
 *                 first flag byte go into the word as the console has them)
 *   GetTrailData  a native AfterImageDesc -> the console-order SwordAttrs the afterimage code
 *                 reads for every sword trail
 *
 * Every other ftFunction export is an event with the decomp table's own signature, stored as is,
 * and so is each article's itFunction table (an ItemLogicTable, same order). */
#ifndef MU_MEXFT_ADAPTER_H
#define MU_MEXFT_ADAPTER_H

#include <stddef.h>

#include <melee/ft/types.h>
#include <melee/ft/inlines.h>
#include <melee/ft/kinds/ftMars/types.h>
#include <melee/it/kinds/types.h>

#include "../akaneia/mu_ak_fighter.h"
#include "mu_disc.h"

/* MexTK's FtState (tmce/MexTK/include/fighter.h), native layout. */
typedef struct MexftState {
    int action_id;
    int flags;
    char move_id;
    char bitflags1;
    void* animation_callback;
    void* iasa_callback;
    void* physics_callback;
    void* collision_callback;
    void* camera_callback;
} MexftState;
_Static_assert(sizeof(MexftState) == sizeof(MotionState), "FtState and MotionState differ in size");
_Static_assert(offsetof(MexftState, animation_callback) == offsetof(MotionState, anim_cb),
               "FtState and MotionState callbacks are at different offsets");

/* MexTK's ItemState (tmce/MexTK/include/item.h), native layout: the decomp's ItemStateTable row
 * for row, so an article's item_state_table is used as it is. */
typedef struct MexftItemState {
    int state;
    void* animCallback;
    void* physCallback;
    void* collCallback;
} MexftItemState;
_Static_assert(sizeof(MexftItemState) == sizeof(ItemStateTable), "ItemState and ItemStateTable differ");
_Static_assert(offsetof(MexftItemState, collCallback) == offsetof(ItemStateTable, collided),
               "ItemState and ItemStateTable callbacks are at different offsets");

/* MexTK's AfterImageDesc (tmce/MexTK/include/fighter.h), native layout: SwordAttrs' fields,
 * host order. */
typedef struct MexftTrail {
    float x0, x4;
    unsigned char bytes[12];   /* alphas, inner and outer colors, two more: +08 to +13 */
    int bone_index;
    float offset_bottom, offset_top;
} MexftTrail;
_Static_assert(sizeof(MexftTrail) == 0x20, "AfterImageDesc is 0x20 bytes");

/* Fill a decomp state table from MexTK rows (a constructor of the descriptor's file, before
 * the registry reads it). */
static inline void mexft_states(MotionState* dst, const MexftState* src, int count)
{
    for (int i = 0; i < count; i++) {
        dst[i].anim_id = src[i].action_id;
        dst[i].x4_flags = src[i].flags;
        dst[i]._ = (u32) (u8) src[i].move_id << 24 | (u32) (u8) src[i].bitflags1 << 16;
        dst[i].anim_cb = (HSD_GObjEvent) src[i].animation_callback;
        dst[i].input_cb = (HSD_GObjEvent) src[i].iasa_callback;
        dst[i].phys_cb = (HSD_GObjEvent) src[i].physics_callback;
        dst[i].coll_cb = (HSD_GObjEvent) src[i].collision_callback;
        dst[i].cam_cb = (HSD_GObjEvent) src[i].camera_callback;
    }
}

/* A trail block in console order, from a native one. The afterimage code reads the result
 * right away (the draw and the record of one fighter), so a short ring of copies is enough. */
static inline struct SwordAttrs* mexft_trail(const MexftTrail* d)
{
    static struct SwordAttrs ring[8];
    static unsigned next;
    if (d == NULL) {
        return NULL;
    }
    struct SwordAttrs* s = &ring[next++ & 7];
    s->x0 = d->x0;
    s->x4 = d->x4;
    s->x8 = d->bytes[0];
    s->x9 = d->bytes[1];
    s->xA = d->bytes[2];
    s->xB = d->bytes[3];
    s->xC = d->bytes[4];
    s->xD = d->bytes[5];
    s->xE = d->bytes[6];
    s->xF = d->bytes[7];
    s->x10 = d->bytes[8];
    s->pad_x11[0] = d->bytes[9];
    s->pad_x11[1] = d->bytes[10];
    s->pad_x11[2] = d->bytes[11];
    s->x14 = d->bone_index;
    s->x18 = d->offset_bottom;
    s->x1C = d->offset_top;
    return s;
}

#endif
