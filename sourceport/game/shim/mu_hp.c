/* 20XX Hack Pack, native: what is left once its debug menu and its host data (the 0xFA ops) are
 * gone. None of the pack's code runs.
 *
 * mu_hp_loaded() is live (lb/lbheap.c sizes one heap by it; shim/mu_hp_css.c gates the extra
 * characters by it). The stage rules below come only from the custom flag byte of the match in play:
 * the stage swap table, the stage select pages, the stage file variants and the music playlists
 * needed the pack's data from the host and were removed with it, so offline the byte is 0 and
 * nothing here changes a match. Playback of a replay recorded with the pack's stage state (option
 * word 3, MU_GAME_OPTION3_HP_STAGE_*, written by older builds) still gets that state's flag byte, so
 * the hazard and Pokemon Stadium hooks play it as it was recorded.
 *
 * Offline only. Online, while online is pending, in Slippi's online menus, and with the pack not
 * loaded, every function here leaves the game exactly as it is. */
#include <dolphin/os.h>
#include <melee/gm/gmvs.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>

#include "mu_hp.h"

/* The host ABI's values this file uses (port/runtime/abi/mu_host.h). That header brings in the C
 * library's types, which clash with the game's, so game-side files keep their own copy. */
#define MU_MOD_HACKPACK                      0x2u
#define MU_GAME_OPTION3_HP_STAGE_ON          0x00008000u
#define MU_GAME_OPTION3_HP_STAGE_FLAGS_MASK  0x3FC00000u
#define MU_GAME_OPTION3_HP_STAGE_FLAGS_SHIFT 22

#include <string.h>

unsigned int mu_mod_flags(void);       /* mu_entry.c */
unsigned int mu_game_options3(void);   /* mu_entry.c */
int mu_online_active(void);
int mu_online_pending(void);
int mu_slippi_in_online_mode(void);
int mu_replay_abi_active(void);

int mu_hp_loaded(void)
{
    return (mu_mod_flags() & MU_MOD_HACKPACK) != 0;
}

/* The running match: the pack's rules in effect, and the flag byte (Stadium sets 0x80 while playing). */
static int hp_on;
static int hp_flags;

/* The pack is loaded and its stage rules may change this game: offline, not in playback. */
static int hp_live(void)
{
    return mu_hp_loaded() && !mu_online_active() && !mu_online_pending() && !mu_slippi_in_online_mode() &&
           !mu_replay_abi_active();
}

int mu_hp_stage_flags(void)
{
    return hp_flags;
}

void mu_hp_stage_flags_or(int bits)
{
    hp_flags |= bits & 0xFF;
}

int mu_hp_stage_on(void)
{
    return hp_on;
}

void mu_hp_match_begin(int stkind)
{
    hp_on = 0;
    hp_flags = 0;
    if (mu_replay_abi_active()) {
        /* playback: the state the replay was recorded with */
        const unsigned int word = mu_game_options3();
        if (mu_hp_loaded() && (word & MU_GAME_OPTION3_HP_STAGE_ON)) {
            hp_on = 1;
            hp_flags = (int) ((word & MU_GAME_OPTION3_HP_STAGE_FLAGS_MASK) >> MU_GAME_OPTION3_HP_STAGE_FLAGS_SHIFT);
        }
        return;
    }
    if (!hp_live()) {
        return;
    }
    hp_on = 1;
    OSReport("[20xx-hp] match on stage %d\n", stkind);
}

void mu_hp_stage_leave(void)
{
    hp_on = 0;
    hp_flags = 0;
}

int mu_hp_stadium_no_transform(void)
{
    const int f = hp_flags;
    if (!(f & 0xF8)) {
        return 0;   /* the pack then tests its debug menu's Freeze Pokemon Stadium, not ported */
    }
    if (!(f & MU_HP_FLAG_OFF)) {
        return 0;   /* a fixed transformation that has not happened yet */
    }
    if (!(f & MU_HP_FLAG_FIXED)) {
        return 1;   /* transformations off */
    }
    return gm_GetFrameCount() != 0;   /* transformed: the fixed form stays */
}

int mu_hp_stadium_fixed(void)
{
    const int f = hp_flags;
    int r;
    if (f & 0x40) {
        r = 0;   /* fire */
    } else if (f & 0x20) {
        r = 1;   /* grass */
    } else if (f & 0x10) {
        r = 2;   /* rock */
    } else if (f & 0x08) {
        r = 3;   /* water */
    } else {
        return -1;
    }
    if (f < 0x80) {
        mu_hp_stage_flags_or(0x80);
    }
    return r;
}

int mu_hp_stadium_run_frozen(void)
{
    return (hp_flags & MU_HP_FLAG_FIXED) != 0 && gm_GetFrameCount() == 0;
}

const void* mu_hp_stadium_param(const void* param, unsigned int size)
{
    /* The engine's file patch (80018130) zeroes the first transformation's countdown (words +0
     * and +4) and the three phase lengths (+0x10, +0x14, +0x18) in the loaded stage file. The
     * file is left alone here and the stage reads a copy. */
    static unsigned int copy[0x100 / 4];
    if (param == NULL || !(hp_flags & MU_HP_FLAG_FIXED) || size > sizeof copy || size < 0x1C) {
        return param;
    }
    memcpy(copy, param, size);
    copy[0x00 / 4] = 0;
    copy[0x04 / 4] = 0;
    copy[0x10 / 4] = 0;
    copy[0x14 / 4] = 0;
    copy[0x18 / 4] = 0;
    return copy;
}
