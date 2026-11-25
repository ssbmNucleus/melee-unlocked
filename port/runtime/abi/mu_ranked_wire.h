/* Ranked selection rules and presentation state for the native game.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MU_RANKED_WIRE_H
#define MU_RANKED_WIRE_H
/* Decomp units use MSL; importing the host CRT here conflicts with ssize_t. */
typedef unsigned char MuRankU8;
typedef unsigned short MuRankU16;
typedef unsigned int MuRankU32;
typedef int MuRankI32;
typedef char MuRankWidthCheck[(sizeof(MuRankU8) == 1 && sizeof(MuRankU16) == 2 && sizeof(MuRankU32) == 4 && sizeof(MuRankI32) == 4) ? 1 : -1];
#include <string.h>

enum { MU_RANK_RESULT_NONE, MU_RANK_RESULT_RECORDING, MU_RANK_RESULT_RECORDED,
       MU_RANK_RESULT_UNRATED, MU_RANK_RESULT_ERROR };
enum { MU_RANK_REASON_NONE, MU_RANK_REASON_UNAVAILABLE, MU_RANK_REASON_MALFORMED };

/* Ratings are in tenths. Slippi has three divisions per tier and Grandmaster.
 * Only a confirmed server result provides before/after values. */
typedef struct MuPlayerRank {
    MuRankU8 known, placed, tier, division;
    MuRankU16 order;
    MuRankI32 rating;
    MuRankU16 matches;
    MuRankU8 placement_total;
    char label[24];
} MuPlayerRank;
typedef struct MuRankResult {
    MuRankU8 status, reason;
    MuRankI32 before, after;
    MuPlayerRank rank_before, rank_after;
} MuRankResult;
typedef struct MuPlayerRanks {
    MuPlayerRank self, opponent;
    MuRankResult result;
} MuPlayerRanks;

/* Policy (50 bytes): 0 version (1); 1 available (0 refuses Ranked);
 * 2 starters[5] u16 stage IDs in UI order; 12 counterpicks[6] u16 in UI order;
 * 24 strike_players[3] (0 P1/1 P2); 27 strike_counts[3];
 * 30 color_players[2] (0 P1/1 P2); 32 counterpick_actors[4] (0 winner/1 loser:
 * ban, stage, character, character); 36 ban_count; 37 dsr (last won stage);
 * 38 color_ban (set-long same character/color restriction);
 * 39 color_seconds[2]; 41 strike_seconds[3]; 44 counterpick_seconds[4];
 * 48 grace_seconds; 49 wait_seconds (opponent timeout after grace).
 * Timeout fills missing stages with the first enabled entries in the pool order
 * (GameSetup.c:894-925); character/color timeout keeps the current legal choice.
 */
typedef struct MuRankedPolicy {
    MuRankU8 version, available;
    MuRankU16 starters[5], counterpicks[6];
    MuRankU8 strike_players[3], strike_counts[3], color_players[2], counterpick_actors[4];
    MuRankU8 ban_count, dsr, color_ban;
    MuRankU8 color_seconds[2], strike_seconds[3], counterpick_seconds[4];
    MuRankU8 grace_seconds, wait_seconds;
} MuRankedPolicy;

static inline void mu_rank_put16(MuRankU8* p, MuRankU16 v) { p[0] = (MuRankU8)(v >> 8); p[1] = (MuRankU8)v; }
static inline MuRankU16 mu_rank_get16(const MuRankU8* p) { return (MuRankU16)((p[0] << 8) | p[1]); }
/* Public Slippi stage and turn order from slippi-ssbm-c Scenes/Ranked/GameSetup.c. */
static inline MuRankedPolicy mu_slippi_ranked_policy(void) {
    const MuRankedPolicy policy = {
    1, 1,
    // Scenes/Ranked/GameSetup.c:510-518 (FoD, DL, YS, FD, BF).
    {2, 28, 8, 32, 31},
    // Scenes/Ranked/GameSetup.c:521-530: BF precedes FD in the counterpick list.
    {2, 28, 8, 31, 32, 3},
    // Scenes/Ranked/GameSetup.c:253-279: P1 strikes 1, P2 strikes 2, P1 strikes 1.
    {0, 1, 0}, {1, 2, 1},
    // Scenes/Ranked/GameSetup.c:225-245: P1 color, P2 color; skipped unless mirror color.
    {0, 1},
    // Scenes/Ranked/GameSetup.c:314-357: winner ban, loser stage, winner char, loser char.
    {0, 1, 0, 1}, 1,
    1, // last-win DSR: Scenes/Ranked/GameSetup.c:1099-1105.
    1, // set-long color ban: Scenes/Ranked/GameSetup.c:217-222,1476-1502.
    {25, 25}, // Scenes/Ranked/GameSetup.c:231,245.
    {30, 30, 10}, // Scenes/Ranked/GameSetup.c:256,267,279.
    {30, 30, 45, 45}, // Scenes/Ranked/GameSetup.c:317,328,344,357.
    3, 15 // Scenes/Ranked/GameSetup.h:16-19 (grace, opponent wait).
    };
    return policy;
}
#endif
