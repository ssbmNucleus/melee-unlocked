/* The added fighters' own sounds (CREATION_LAYER_PLAN.md step 8).
 *
 * A fighter m-ex adds has its own sound bank (wolf.ssm is bank 62 on Akaneia), named for its
 * external fighter id in MxDt. The bank is loaded with the match by the game's own loader
 * (lb/lbaudio_ax.c, mu_ak_sound_view and mu_ak_sound_request). The fighter's code, its animation
 * scripts and its data name a sound of that bank by a number relative to the fighter:
 *
 *     5000 + n   sound n of the owner's bank
 *
 * and the game's own id for it is bank * 10000 + n, for a retail bank as for an added one (the
 * retail ids of bank b start at b * 10000). This file does that translation, for a sound
 * a fighter plays (ft_80087D0C) and for one an item it created plays (Item_8026AE84 and its two
 * siblings). Ids below 5000 are the common bank's and ids from 10000 on are already complete:
 * both pass through unchanged. A Kirby who holds the ability of an added fighter plays 5000 + n
 * from that fighter's bank (m-ex hooks at 80087D28, 8026AEBC, 8026AF2C and 8026AFC0); the bank is
 * in memory because that fighter is in the match. */
#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/types.h>
#include <melee/it/forward.h>
#include <melee/it/types.h>
#include <sysdolphin/baselib/gobj.h>

#include <stddef.h>

#include "../mu_ak_fighter.h"

#ifdef MU_AKANEIA_FIGHTERS

#define AK_SOUND_FIRST 5000
#define AK_SOUND_END 10000
#define AK_SOUND_LOG 16   /* remapped ids that go in the log, per run */

int mu_ak_sound_loaded(int bank);                 /* lb/lbaudio_ax.c */
Fighter* mu_ak_effect_owner(HSD_GObj* gobj);      /* mu_ak_effects.c */

/* The sound bank of an added character kind, -1 when it has none. Mostly a bank the disc adds
 * (past 55); a fighter built on a retail one may name that fighter's retail bank (ACE: B. Falcon
 * 6, Metal Mario 18, Giga Bowser 12). 55 is the game's "no bank". */
int mu_ak_sound_bank(int ckind)
{
    int ext, bank;
    if (!MU_AK_CKIND(ckind) || !mu_mex_fighters_active()) {
        return -1;
    }
    ext = mu_ak_mex_external(ckind);
    bank = ext >= 0 ? mu_mex_fighter_ssm(ext) : -1;
    return bank >= 0 && bank != 55 && bank < mu_mex_ssm_count() ? bank : -1;
}

/* A fighter installed beside the game with its own bank (shim/mu_mex_roster.c): its data names its
 * base fighter's sounds (that bank's ids, b * 10000 + n) or its own (5000 + n); sound n of its own
 * bank plays in their place, the way the bank was made from the base fighter's. Its own bank not
 * loaded (no room in the match): the base fighter's sound, said once a fighter. -1: not such a sound. */
static int roster_sound_id(Fighter* fp, int kind, int id)
{
    static unsigned int said;   /* a bit per installed fighter */
    static int logged;
    int base, own, scripts, n = -1, out;
    const int ext = mu_ak_mex_external(mu_ak_ckind_from_kind(kind));
    if (ext < 0 || !mu_mex_roster_sound(ext, &base, &own, &scripts) || own == 0) {
        return -1;
    }
    if (id >= AK_SOUND_FIRST && id < AK_SOUND_END) {
        n = id - AK_SOUND_FIRST;
    } else if (kind == (int) fp->kind && base < 55 && id >= base * 10000 && id < base * 10000 + scripts) {
        n = id - base * 10000;   /* a Kirby's own sounds stay Kirby's */
    }
    if (n < 0) {
        return -1;
    }
    if (n < scripts && mu_ak_sound_loaded(own)) {
        out = own * 10000 + n;
    } else {
        out = base < 55 ? base * 10000 + n : id;
        if (ext - 26 >= 0 && ext - 26 < 32 && !(said & (1u << (ext - 26)))) {
            said |= 1u << (ext - 26);
            OSReport("[ak] sound: fighter kind %d: its own bank %d is %s; its base fighter's sounds (bank %d)\n", kind,
                     own, mu_ak_sound_loaded(own) ? "loaded but has no such sound" : "not loaded", base);
        }
    }
    if (logged < AK_SOUND_LOG) {
        logged++;
        OSReport("[ak] sound: fighter kind %d id %d -> %d (own bank %d %s)\n", kind, id, out, own,
                 mu_ak_sound_loaded(own) ? "loaded" : "NOT loaded");
    }
    return out;
}

/* A fighter-relative sound id of added fighter `fp`, or of a Kirby who copied one, as the game's
 * own id. */
int mu_ak_sound_id(Fighter* fp, int id)
{
    static int logged;
    int bank, out, kind;
    if (fp == NULL) {
        return id;
    }
    kind = MU_AK_KIND(fp->kind) ? (int) fp->kind : mu_ak_kirby_copy_kind(fp);
    if (!MU_AK_KIND(kind)) {
        return id;
    }
    if ((out = roster_sound_id(fp, kind, id)) >= 0) {
        return out;
    }
    if (id < AK_SOUND_FIRST || id >= AK_SOUND_END) {
        return id;
    }
    bank = mu_ak_sound_bank(mu_ak_ckind_from_kind(kind));
    if (bank < 0) {
        return id;
    }
    out = bank * 10000 + (id - AK_SOUND_FIRST);
    if (logged < AK_SOUND_LOG) {
        logged++;
        OSReport("[ak] sound: fighter kind %d id %d -> %d (bank %d %s)\n", kind, id, out,
                 bank, mu_ak_sound_loaded(bank) ? "loaded" : "NOT loaded");
    }
    return out;
}

/* The same for a sound of an item: the bank is its creator's (an installed fighter's own bank
 * stands in for its base fighter's ids too, so every id goes there). */
int mu_ak_sound_item_id(Item* ip, int id)
{
    if (ip == NULL || ip->entity == NULL || id < AK_SOUND_FIRST) {
        return id;
    }
    return mu_ak_sound_id(mu_ak_effect_owner(ip->entity), id);
}

#endif /* MU_AKANEIA_FIGHTERS */
