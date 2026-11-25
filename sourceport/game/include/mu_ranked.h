/* Shared Ranked client state, owned by the online major. GPL-3.0-or-later. */
#ifndef MU_RANKED_H
#define MU_RANKED_H
#include <mu_ranked_wire.h>

/* Native equivalent of Slippi Online.s:517-546 / GameSetup.h:22-37.
 * No function pointer or console packing: this is native UI bookkeeping, not a
 * wire/simulation block. cur_game is 1-based, prev_winner/results are port 0/1;
 * 0xFF means no winner/result. last_stage_win is a stage ID (0xFFFF unset).
 * Pips show the game's own counts, as Slippi does. Slippi reports the official rating. Never copy these fields into match state.
 */
typedef struct MuRankedSet {
    MuRankU8 max_games;
    MuRankU16 cur_game;
    MuRankU8 score[2], prev_winner, tiebreak_num, game_results[9];
    MuRankU16 last_stage_win[2];
    struct { MuRankU8 active, character, color; } color_ban;
    MuRankU8 last_end_mode, result_requested;
} MuRankedSet;

/* Mutable access for track A; const access for score/badge consumers. Reset at
 * online-major entry/exit; A also resets when a fresh pair starts a new set. */
MuRankedSet* mu_ranked_set_mut(void);
const MuRankedSet* mu_ranked_set(void);
void mu_ranked_set_reset(void);
int mu_ranked_read_ranks(MuPlayerRanks* out);
int mu_ranked_read_policy(MuRankedPolicy* out);

#endif
