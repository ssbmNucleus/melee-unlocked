/* The match block (the game's start data in the console's layout: 0x60 bytes of rules, then six
 * players of 0x24, big-endian) reaches the game from outside it: a replay file, the online match
 * state the host negotiated with other players, the --match players. Before one starts, every value
 * the game looks up in a table of its own is checked against that table, so a block made elsewhere
 * can only describe a match this game has the data for. Built with and without the Slippi layer.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <melee/ft/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/pl/forward.h>
#include <melee/pl/player.h>

int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
int mu_stage_id_known(int stkind);   /* gr/stage.c */

enum {
    RULES_SIZE = 0x60,
    PLAYER_SIZE = 0x24,
    PLAYERS = 6,
    /* it_804D6D28->xFC has a spawn-time row for frequencies 0 to 4; -1 is no items. */
    ITEM_FREQ_MAX = 4,
    CPU_LEVEL_MAX = 9,
    /* A replay's first-frame fighters come for the four ports (mu_match_block_replay_problem). */
    RECORDED_PORTS = 4,
};

/* The character kind a player's byte becomes (convert_player in mu_replay.c, read_player in
 * mu_match_info.c): a disc's external id maps to its native kind, and one that names no fighter
 * of this view becomes "no fighter". */
static int block_ckind(int ext)
{
#ifdef MU_AKANEIA_FIGHTERS
    if (ext != ChKind_None) {
        const int ckind = mu_ak_ckind_from_mex(ext);
        return ckind >= 0 ? ckind : ChKind_None;
    }
#endif
    return ext;
}

/* A kind the per-character tables have a row for: the retail kinds, and an added fighter whose slot
 * the registry filled in this view (a roster or m-ex fighter). */
static int ckind_known(int ckind)
{
    if (ckind >= 0 && ckind < ChKind_None) {
        return 1;
    }
#ifdef MU_AKANEIA_FIGHTERS
    return MU_AK_CKIND(ckind) && mu_ak_mex_external(ckind) >= 0;
#else
    return 0;
#endif
}

/* The fighter id a recording gives a fighter of native kind `kind` (the internal character id of a
 * Slippi post-frame event, mu_replay_post_frame): the kind itself, or the disc's internal id while
 * added fighters are served (their ids and the special fighters' follow the disc's m-ex table). */
int mu_match_recorded_kind(int kind)
{
#ifdef MU_AKANEIA_FIGHTERS
    if (kind >= 0 && mu_mex_fighters_active()) {
        const int mex = mu_ak_mex_internal(kind);
        if (mex >= 0) {
            return mex;
        }
    }
#endif
    return kind;
}

/* 0 when the block can start as it is. Otherwise 1, with what is wrong written to `why`. An empty
 * slot makes no fighter, so only its slot type is looked at. Every other slot gets one (the Vs scene
 * creates a fighter for each slot that is not empty), so its character must be one this game has:
 * "no fighter" there, or an id that becomes it, has no fighter data to create. An added fighter is
 * one only while it is installed with everything it needs: an id past the retail ones names nothing
 * in a game without it, and an installed fighter whose code (its .mufighter plugin) or files are
 * missing stays locked. A kind with no costume table of its own (Master Hand and the other special
 * kinds) plays in costume 0. */
int mu_match_block_problem(const unsigned char* info, char* why, unsigned int size)
{
    const int stage = info[0xE] << 8 | info[0xF];
    const int item_freq = (signed char) info[0xB];
    int i;

    if (!mu_stage_id_known(stage)) {
        snprintf(why, size, "stage %d is not a stage of this game", stage);
        return 1;
    }
    if (item_freq < -1 || item_freq > ITEM_FREQ_MAX) {
        snprintf(why, size, "item frequency %d is not one of the game's", item_freq);
        return 1;
    }
    for (i = 0; i < PLAYERS; i++) {
        const unsigned char* p = info + RULES_SIZE + PLAYER_SIZE * i;
        int ckind, costumes;
        if (p[1] > Gm_PKind_Boss) {
            snprintf(why, size, "player %d's slot type %d is not one of the game's", i + 1, p[1]);
            return 1;
        }
        if (p[1] == Gm_PKind_NA) {
            continue;
        }
        ckind = block_ckind((signed char) p[0]);
        if (!ckind_known(ckind)) {
#ifdef MU_AKANEIA_FIGHTERS
            if ((signed char) p[0] != ChKind_None && mu_ak_ckind_locked_from_mex((signed char) p[0]) >= 0) {
                const char* name = mu_mex_fighter_name((signed char) p[0]);
                snprintf(why, size, "player %d's character %d (%s) is installed but locked: its code (a .mufighter "
                         "plugin) or its files are missing", i + 1, (signed char) p[0], name != NULL ? name : "?");
                return 1;
            }
#endif
            snprintf(why, size, "player %d's character %d is not in this game", i + 1, (signed char) p[0]);
            return 1;
        }
        costumes = gm_GetNumCostumesForCKind((u8) ckind);
        if (p[3] >= (costumes > 0 ? costumes : 1)) {
            snprintf(why, size, "player %d's costume %d is past the character's %d", i + 1, p[3],
                     costumes > 0 ? costumes : 1);
            return 1;
        }
        if (p[0xE] > CpuKind_29) {
            snprintf(why, size, "player %d's CPU kind %d is not one of the game's", i + 1, p[0xE]);
            return 1;
        }
        if (p[0xF] > CPU_LEVEL_MAX) {
            snprintf(why, size, "player %d's CPU level %d is past %d", i + 1, p[0xF], CPU_LEVEL_MAX);
            return 1;
        }
    }
    return 0;
}

/* A replay's block, with the fighter each of the four ports had on the replay's first frame as the
 * recording has it (`recorded`, -1 for a port it has none for). The block names characters by the
 * disc's external ids, and past Ganondorf those depend on the fighters the recording game had
 * installed: recorded with Shulk and Wolf installed (26 and 27), a replay names Master Hand and a
 * Wireframe to a game without them, and both are characters of this game (a replay always plays
 * without the installed fighters: the host serves them only outside replays). A port whose character
 * makes another fighter here than the one recorded would play the recorded inputs on that other
 * fighter, so such a replay is refused too. */
int mu_match_block_replay_problem(const unsigned char* info, const int* recorded, char* why, unsigned int size)
{
    int i;

    if (mu_match_block_problem(info, why, size)) {
        return 1;
    }
    for (i = 0; i < RECORDED_PORTS; i++) {
        const unsigned char* p = info + RULES_SIZE + PLAYER_SIZE * i;
        int here;
        if (p[1] == Gm_PKind_NA || recorded[i] < 0) {
            continue;
        }
        here = mu_match_recorded_kind(Player_800325C8((CharacterKind) block_ckind((signed char) p[0]), 0));
        if (here != recorded[i]) {
            /* A replay is played without the fighters installed beside the game (the host serves
             * them only outside replays), so a retail disc has no added fighters here. */
#ifdef MU_AKANEIA_FIGHTERS
            const int added_here = mu_mex_fighters_active();
#else
            const int added_here = 0;
#endif
            snprintf(why, size, "player %d's character %d is fighter %d in this game but fighter %d in the "
                     "replay, which was recorded with %s", i + 1, (signed char) p[0], here, recorded[i],
                     added_here ? "other added fighters" : "installed fighters, and a replay plays without them");
            return 1;
        }
    }
    return 0;
}
