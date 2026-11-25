/* Online-major Ranked UI data. GPL-3.0-or-later. */
#include <mu_native.h>
#include <mu_ranked.h>
#include <string.h>
#include <mu_commands.h>

static MuRankedSet ranked_set;
MuRankedSet* mu_ranked_set_mut(void) { return &ranked_set; }
const MuRankedSet* mu_ranked_set(void) { return &ranked_set; }
void mu_ranked_set_reset(void)
{
    memset(&ranked_set, 0, sizeof ranked_set);
    /* Practice vs CPU plays the same best of three as Ranked (striking, game 1, then bans and
     * counterpicks), so it teaches the real set (10-31: a one-game practice read as Ranked). */
    ranked_set.max_games = 3;
    ranked_set.cur_game = 1;
    ranked_set.prev_winner = 0xFF;
    memset(ranked_set.game_results, 0xFF, sizeof ranked_set.game_results);
    ranked_set.last_stage_win[0] = ranked_set.last_stage_win[1] = 0xFFFF;
}
static unsigned read_u32(const unsigned char* bytes)
{
    return ((unsigned)bytes[0] << 24) | ((unsigned)bytes[1] << 16) |
           ((unsigned)bytes[2] << 8) | bytes[3];
}

static float read_float(const unsigned char* bytes)
{
    unsigned bits = read_u32(bytes);
    float number;
    memcpy(&number, &bits, sizeof(number));
    return number;
}

static void describe_rank(MuPlayerRank* rank, int index, float ordinal, unsigned updates)
{
    static const char* labels[] = {
        "Unranked", "Bronze I", "Bronze II", "Bronze III",
        "Silver I", "Silver II", "Silver III", "Gold I", "Gold II", "Gold III",
        "Platinum I", "Platinum II", "Platinum III", "Diamond I", "Diamond II", "Diamond III",
        "Master I", "Master II", "Master III", "Grandmaster"
    };
    if (index < 0 || index > 19 || !(ordinal >= -1000000 && ordinal <= 1000000)) return;
    rank->known = 1;
    rank->placed = index > 0;
    rank->tier = index ? 1 + (index - 1) / 3 : 0;
    rank->division = index && index < 19 ? 1 + (index - 1) % 3 : 0;
    rank->order = index;
    rank->rating = (int)(ordinal * 10 + (ordinal < 0 ? -.5f : .5f));
    rank->placement_total = 5;
    rank->matches = updates > 65535 ? 65535 : updates;
    strcpy(rank->label, labels[index]);
}

int mu_ranked_read_ranks(MuPlayerRanks* out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
#ifndef MU_NO_SLIPPI
    unsigned char e3[16];
    unsigned got = 0;
    const MuMatchState* ms = mu_slippi_match_state();
    if (mu_slippi_cmd(MU_CMD_GET_RANK, NULL, 0, e3, sizeof e3, &got) || got != sizeof e3) return 0;
    if ((e3[0] & 2) && ms->connection_state == MU_SLP_MM_CONNECTION_SUCCESS && ms->remote_index < 2) {
        /* 3.7 match-state extension: four 16-byte port records after the original 962 bytes. */
        const unsigned char* remote = ms->raw + 962 + 16 * ms->remote_index;
        describe_rank(&out->opponent, ms->remote_index ? ms->opp_rank : ms->user_rank,
                      read_float(remote), read_u32(remote + 4));
    }
    if (!(e3[0] & 1)) return 1;
    if (e3[1] == 1) describe_rank(&out->self, e3[2], read_float(e3 + 3), read_u32(e3 + 7));
    if (ranked_set.result_requested) {
        MuRankResult* result = &out->result;
        result->status = e3[1] == 0 ? MU_RANK_RESULT_RECORDING : MU_RANK_RESULT_ERROR;
        if (e3[1] == 1 && out->self.known) {
            const unsigned updates = read_u32(e3 + 7);
            const float change = read_float(e3 + 11);
            describe_rank(&result->rank_before, (int)e3[2] - (signed char)e3[15],
                          read_float(e3 + 3) - change, updates ? updates - 1 : 0);
            result->rank_after = out->self;
            result->before = result->rank_before.rating;
            result->after = out->self.rating;
            result->status = MU_RANK_RESULT_RECORDED;
        }
    }
    return 1;
#else
    return 0;
#endif
}

int mu_ranked_read_policy(MuRankedPolicy* out)
{
    if (!out) return 0;
    *out = mu_slippi_ranked_policy();
    return 1;
}
