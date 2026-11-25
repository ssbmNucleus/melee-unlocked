/* MexTK for fighters compiled from their C source (mexft/, tools/mexft/mexft_prepare.py).
 *
 * The same native MexTK that Training Mode CE's modules build against (tmce/MexTK: struct
 * layouts regenerated for the native game, MexTK calls renamed to the decomp functions at link
 * time). A port's own `#include "../mex.h"` is pointed here when its source is prepared.
 *
 * Below: names that newer m-ex MexTK headers give fields MU's copy also has under an older name.
 * Each is the same field, at the same offset. */
#ifndef MU_MEXFT_MEX_H
#define MU_MEXFT_MEX_H

/* The build's overlay of tmce/MexTK (tools/mexft/mexft_overlay.py: the same headers plus the newer
 * member names of tools/mexft/compat_fields.txt), found on the mexft modules' include path. */
#include "MexTK/mex.h"

/* FighterData flags at +0x221B bit 0x40 (the decomp's x221B_b1, which Marth's counter sets);
 * m-ex commit c9f25da named it shield_always_refract. */
#define shield_always_refract shield_x40

/* ---- m-ex's own runtime (its code at 0x803D7xxx, not the game's), native: mexft_runtime.c ---- */
int MEX_GetFtItemID(GOBJ *f, int item_id) __asm__("mexft_GetFtItemID");
void MEX_IndexFighterItem(int fighter_kind, ItemDesc *itemdesc, int item_id) __asm__("mexft_IndexFighterItem");
int mexft_item_count(int fighter_kind) __asm__("mexft_ItemCount");

/* fd->ftData->items, which a port reads as an array of ItemDesc pointers. The fighter file holds
 * 4-byte console-order slots; this is the same list as native pointers (one list per module, which
 * builds one fighter). mexft_prepare.py points the port's reads of that field here. */
#define MEXFT_ITEMS_MAX 64
static inline ItemDesc **mexft_ft_items(FighterData *fd)
{
    static ItemDesc *host[MEXFT_ITEMS_MAX];
    unsigned int base = fd->ftData->items;
    int n = 0;
    if (base != 0) {
        n = mexft_item_count(fd->kind);
        n = n < MEXFT_ITEMS_MAX ? n : MEXFT_ITEMS_MAX;
    }
    for (int i = 0; i < MEXFT_ITEMS_MAX; i++) {
        host[i] = i < n ? (ItemDesc *) MEX_DP_AT((void *) (uintptr_t) base, i) : 0;
    }
    return base != 0 ? host : 0;
}

#ifndef fabsf
#define fabsf __builtin_fabsf
#endif

/* MexTK globals MU's native MexTK still defines by a console r13 address (text.h), which natively
 * is only a number: the two lists to the decomp's objects (sislib.c; TextCanvas and Text have the
 * decomp's native layout), the text heap to a compile error until it has a native meaning. */
extern TextCanvas *mexft_textcanvas_first __asm__("HSD_SisLib_804D797C");
extern Text *mexft_text_first __asm__("HSD_SisLib_804D7978");
#define stc_textcanvas_first (&mexft_textcanvas_first)
#define stc_text_first (&mexft_text_first)
#define stc_textheap_size mexft_unmapped_r13_global_stc_textheap_size
#define stc_textheap_start mexft_unmapped_r13_global_stc_textheap_start
#define stc_textheap_next mexft_unmapped_r13_global_stc_textheap_next
#define stc_textheap_first mexft_unmapped_r13_global_stc_textheap_first

/* MexTK functions declared with the wrong argument (tools/mexft/mexft_abi_audit.py finds them):
 * the item's data where the game function takes the item's GObj. On the console such a call reads
 * a stray address and goes on; natively it is an access violation. Here a call passes whichever
 * the port has, and the game function gets the GObj. */
void mexft_it_8027146C(GOBJ *item) __asm__("it_8027146C");
static inline void mexft_reset_hit_data(ItemData *id) { mexft_it_8027146C(id->item); }
static inline void mexft_reset_hit_gobj(GOBJ *item) { mexft_it_8027146C(item); }
#define Item_ResetAllHitPlayers(x) \
    _Generic((x), ItemData *: mexft_reset_hit_data, GOBJ *: mexft_reset_hit_gobj)(x)
/* And the fighter's GObj where the game function takes its data. */
#define MEXFT_FD(x) _Generic((x), GOBJ *: (FighterData *) ((GOBJ *) (x))->userdata, FighterData *: (x))
int mexft_ftColl_80076640(FighterData *fd, float *dmg) __asm__("ftColl_80076640");
void mexft_ftCommon_8007E2F4(FighterData *fd, short val) __asm__("ftCommon_8007E2F4");
void mexft_ftCo_8009E7B4(FighterData *fd, void *dynamics) __asm__("ftCo_8009E7B4");
#define Fighter_IncPercent(f, dmg) ((void) mexft_ftColl_80076640(MEXFT_FD(f), (dmg)))
#define Fighter_SetGrabbableFlag(f, v) mexft_ftCommon_8007E2F4(MEXFT_FD(f), (short) (v))
#define Fighter_UpdateDynamics(f, d) mexft_ftCo_8009E7B4(MEXFT_FD(f), (void *) (d))

/* ftHit's bytes +40 and +41 written whole (newer MexTK's x40 and x41): on the console, a 16-bit
 * bitfield counted from the top bit, which a native byte store would not reach in the same bits.
 * mexft_prepare.py turns `h->x40 = v;` into mexft_hit_x40(h, v), which sets the decomp's named
 * bits (mexft_runtime.c); _Generic refuses it on anything but an ftHit. */
void mexft_hit_set_x40(ftHit *h, int v) __asm__("mexft_HitSetX40");
void mexft_hit_set_x41(ftHit *h, int v) __asm__("mexft_HitSetX41");
#define mexft_hit_x40(h, v) mexft_hit_set_x40(_Generic((h), ftHit *: (h)), (v))
#define mexft_hit_x41(h, v) mexft_hit_set_x41(_Generic((h), ftHit *: (h)), (v))

/* The game's bool is an int (Runtime/platform.h) and MexTK's a one-byte _Bool. A port's function
 * the game calls and reads as a bool (an article's OnGiveDamage, a state's IASA test) sets only
 * the low byte as a _Bool, and the game would read the rest of the register as well. From here
 * on, the port's own code returns and keeps full ints; the MexTK declarations above stay as
 * they were. */
#undef bool
#define bool int

#endif
