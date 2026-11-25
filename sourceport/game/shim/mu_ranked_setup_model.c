/* GPL-3.0-or-later. Source references: project-slippi/slippi-ssbm-c d7174ca,
 * Scenes/Ranked/GameSetup.c unless otherwise named. No engine dependencies. */
#include <mu_ranked_setup_model.h>
#include <string.h>

static int matching(unsigned a, unsigned ac, unsigned b, unsigned bc)
{
    /* Game/Characters.c:3-15: Sheik (0x13) shares Zelda (0x12) costumes. */
    return (a == 0x13 ? 0x12 : a) == (b == 0x13 ? 0x12 : b) && ac == bc;
}
unsigned mu_ranked_setup_color_count(unsigned c)
{
    /* GameSetup.c:1426-1473, external CKIND ids (not internal fighter kinds). */
    static const MuRankU8 colors[26] = {
        6,5,4,4,6,4,5,4,5,5,4,4,5,4,4,5,5,6,5,5,4,5,5,5,4,5
    };
    return c < 26 ? colors[c] : 1;
}
static int fighter_valid(unsigned c, unsigned color)
{
    return c < 26 && color < mu_ranked_setup_color_count(c);
}
static int policy_valid(const MuRankedPolicy* p)
{
    unsigned i, j, sum = 0;
    if (!p || p->version != 1 || !p->available || !p->ban_count || p->ban_count > 2)
        return 0;
    for (i = 0; i < 6; ++i) {
        if (!p->counterpicks[i] || p->counterpicks[i] > 255) return 0;
        for (j = 0; j < i; ++j) if (p->counterpicks[i] == p->counterpicks[j]) return 0;
    }
    for (i = 0; i < 5; ++i) {
        int found = 0;
        for (j = 0; j < 6; ++j) found |= p->starters[i] == p->counterpicks[j];
        if (!found) return 0;
        for (j = 0; j < i; ++j) if (p->starters[i] == p->starters[j]) return 0;
    }
    for (i = 0; i < 3; ++i) {
        if (p->strike_players[i] > 1 || !p->strike_counts[i] || p->strike_counts[i] > 2) return 0;
        sum += p->strike_counts[i];
    }
    for (i = 0; i < 2; ++i) if (p->color_players[i] > 1) return 0;
    if (p->color_players[0] == p->color_players[1]) return 0;
    for (i = 0; i < 4; ++i) if (p->counterpick_actors[i] > 1) return 0;
    return sum == 4;
}
const MuRankedSetupStep* mu_ranked_setup_current(const MuRankedSetupModel* m)
{
    return m && m->status == MU_SETUP_ACTIVE && m->step < m->step_count ? &m->steps[m->step] : 0;
}
int mu_ranked_setup_stage_allowed(const MuRankedSetupModel* m, MuRankU16 stage)
{
    const MuRankedSetupStep* s = mu_ranked_setup_current(m);
    unsigned i, n;
    const MuRankU16* pool;
    int found = 0;
    if (!s || (s->type != MU_SETUP_STAGE && s->type != MU_SETUP_STRIKE)) return 0;
    n = m->set->cur_game == 1 ? 5 : 6;
    pool = n == 5 ? m->policy.starters : m->policy.counterpicks;
    for (i = 0; i < n; ++i) found |= pool[i] == stage;
    if (!found) return 0;
    for (i = 0; i < m->removed_count; ++i) if (m->removed[i] == stage) return 0;
    /* :1099-1105 applies last-win DSR to BOTH ban and counterpick steps. */
    if (m->set->cur_game > 1 && m->policy.dsr) {
        unsigned loser = !m->set->prev_winner;
        if (m->set->score[loser] && m->set->last_stage_win[loser] == stage) return 0;
    }
    return 1;
}
int mu_ranked_setup_color_allowed(const MuRankedSetupModel* m, unsigned player,
                                 unsigned c, unsigned color)
{
    const MuRankedFighter* other;
    if (!m || player > 1 || !fighter_valid(c, color)) return 0;
    /* :1476-1502 checks opponent's choice, including their uncompleted step. */
    if (m->policy.color_ban && m->set->color_ban.active &&
        matching(c, color, m->set->color_ban.character, m->set->color_ban.color)) return 0;
    other = &m->result.fighters[!player];
    return !matching(c, color, other->character, other->color);
}
unsigned mu_ranked_setup_next_color(const MuRankedSetupModel* m, unsigned player,
                                   unsigned c, unsigned color, int direction)
{
    unsigned i, n = mu_ranked_setup_color_count(c);
    int v = (int)(color % n), d = direction < 0 ? -1 : 1;
    if (!direction && mu_ranked_setup_color_allowed(m, player, c, (unsigned)v)) return (unsigned)v;
    /* :1403-1423: wrap and skip forbidden costumes. Bounded for invalid chars. */
    for (i = 0; i < n; ++i) {
        v = (v + d + (int)n) % (int)n;
        if (mu_ranked_setup_color_allowed(m, player, c, (unsigned)v)) return (unsigned)v;
    }
    return n;
}
int mu_ranked_setup_model_init(MuRankedSetupModel* m, const MuRankedPolicy* p,
    MuRankedSet* set, unsigned local, const MuRankedSetupResult* previous)
{
    unsigned i;
    int collision;
    memset(m, 0, sizeof(*m));
    m->status = MU_SETUP_INVALID;
    if (!policy_valid(p) || !set || !previous || local > 1 || !set->cur_game ||
        (set->cur_game > 1 && set->prev_winner > 1)) return 0;
    m->policy = *p; m->set = set; m->local_player = (MuRankU8)local;
    m->result = *previous;
    /* :205-212 clamps extended costumes from CSS to vanilla palette. */
    for (i = 0; i < 2; ++i) {
        if (previous->fighters[i].character >= 26) return 0;
        if (m->result.fighters[i].color >= mu_ranked_setup_color_count(previous->fighters[i].character))
            m->result.fighters[i].color = 0;
        m->initial[i] = m->result.fighters[i];
    }
    m->status = MU_SETUP_ACTIVE;
    if (set->tiebreak_num) { m->status = MU_SETUP_COMPLETE; return 1; }
    if (set->cur_game == 1) {
        /* :193-293. Keep wire indices of skipped colour steps intact. */
        m->step_count = 5;
        collision = matching(m->initial[0].character, m->initial[0].color,
                             m->initial[1].character, m->initial[1].color);
        if (collision && p->color_ban) {
            set->color_ban.active = 1;
            set->color_ban.character = m->initial[0].character;
            set->color_ban.color = m->initial[0].color;
        }
        for (i = 0; i < 2; ++i) {
            m->steps[i].type = MU_SETUP_COLOR; m->steps[i].player = p->color_players[i];
            m->steps[i].count = 1; m->steps[i].seconds = p->color_seconds[i];
        }
        for (i = 0; i < 3; ++i) {
            m->steps[i+2].type = MU_SETUP_STRIKE; m->steps[i+2].player = p->strike_players[i];
            m->steps[i+2].count = p->strike_counts[i]; m->steps[i+2].seconds = p->strike_seconds[i];
        }
        if (!collision) m->step = 2;
    } else {
        /* :295-364: ban, counterpick, winner character, loser character. */
        static const MuRankU8 types[4] = { MU_SETUP_STRIKE, MU_SETUP_STAGE, MU_SETUP_CHARACTER, MU_SETUP_CHARACTER };
        m->step_count = 4;
        for (i = 0; i < 4; ++i) {
            m->steps[i].type = types[i];
            m->steps[i].player = p->counterpick_actors[i] ? !set->prev_winner : set->prev_winner;
            m->steps[i].count = i == 0 ? p->ban_count : 1;
            m->steps[i].seconds = p->counterpick_seconds[i];
        }
    }
    return 1;
}
int mu_ranked_setup_packet(const MuRankedSetupModel* m, const MuRankedSetupChoice* draft,
                          int timeout, MuRankU8 out[5])
{
    const MuRankedSetupStep* s = mu_ranked_setup_current(m);
    MuRankedSetupChoice c;
    unsigned i, j, n;
    const MuRankU16* pool;
    if (!s || !out) return 0;
    memset(&c, 0, sizeof c);
    c.character = m->result.fighters[s->player].character;
    c.color = m->result.fighters[s->player].color;
    if (draft) c = *draft;
    memset(out, 0, 5); out[0] = m->step;
    if (s->type == MU_SETUP_COLOR || s->type == MU_SETUP_CHARACTER) {
        if (s->type == MU_SETUP_COLOR && c.character != m->initial[s->player].character) return 0;
        if (!fighter_valid(c.character, c.color)) return 0;
        if (!mu_ranked_setup_color_allowed(m, s->player, c.character, c.color)) {
            if (!timeout) return 0;
            c.color = (MuRankU8)mu_ranked_setup_next_color(m, s->player, c.character, c.color, 1);
            if (!mu_ranked_setup_color_allowed(m, s->player, c.character, c.color)) return 0;
        }
        out[1] = c.character; out[2] = c.color;
    } else {
        if (c.count > s->count || (!timeout && c.count != s->count)) return 0;
        for (i = 0; i < c.count; ++i) {
            if (!mu_ranked_setup_stage_allowed(m, c.stages[i])) return 0;
            for (j = 0; j < i; ++j) if (c.stages[i] == c.stages[j]) return 0;
        }
        n = m->set->cur_game == 1 ? 5 : 6;
        pool = n == 5 ? m->policy.starters : m->policy.counterpicks;
        /* Stock CompleteCurrentStep emits selected icons in display order.
         * On timeout it then appends enabled icons, starting again at the
         * first one. A selected icon is still enabled. */
        unsigned committed = 0;
        for (i = 0; i < n; ++i) {
            for (j = 0; j < c.count; ++j) {
                if (pool[i] == c.stages[j]) out[3 + committed++] = (MuRankU8)pool[i];
            }
        }
        for (i = 0; i < n && committed < s->count; ++i) {
            if (mu_ranked_setup_stage_allowed(m, pool[i]))
                out[3 + committed++] = (MuRankU8)pool[i];
        }
        if (committed != s->count) return 0;
    }
    return 1;
}
int mu_ranked_setup_apply(MuRankedSetupModel* m, unsigned player, const MuRankU8 packet[5])
{
    const MuRankedSetupStep* s = mu_ranked_setup_current(m);
    unsigned i;
    if (!s || !packet || player != s->player || packet[0] != m->step) return 0;
    if (s->type == MU_SETUP_STAGE || s->type == MU_SETUP_STRIKE) {
        for (i = 0; i < s->count; ++i)
            if (!mu_ranked_setup_stage_allowed(m, packet[3 + i])) return 0;
        if (s->count == 2 && packet[3] == packet[4]) {
            /* A stock partial timeout can repeat only its first enabled icon. */
            const MuRankU16* pool = m->set->cur_game == 1 ? m->policy.starters : m->policy.counterpicks;
            unsigned n = m->set->cur_game == 1 ? 5 : 6;
            for (i = 0; i < n && !mu_ranked_setup_stage_allowed(m, pool[i]); ++i) {}
            if (i == n || packet[3] != pool[i]) return 0;
        }
        if (s->type == MU_SETUP_STRIKE)
            for (i = 0; i < s->count; ++i) m->removed[m->removed_count++] = packet[3 + i];
        else m->result.stage = packet[3];
    } else {
        if (s->type == MU_SETUP_COLOR && packet[1] != m->initial[player].character) return 0;
        if (!mu_ranked_setup_color_allowed(m, player, packet[1], packet[2])) return 0;
        m->result.fighters[player].character = packet[1];
        m->result.fighters[player].color = packet[2];
    }
    ++m->step; m->frames = 0;
    if (m->step == m->step_count) {
        /* :985-1031: one starter remains, or use the explicit counterpick. */
        if (m->set->cur_game == 1) {
            for (i = 0; i < 5; ++i) {
                unsigned j; int removed = 0;
                for (j = 0; j < m->removed_count; ++j) removed |= m->removed[j] == m->policy.starters[i];
                if (!removed) m->result.stage = m->policy.starters[i];
            }
        }
        m->status = MU_SETUP_COMPLETE;
    }
    return 1;
}
int mu_ranked_setup_remote(MuRankedSetupModel* m, const MuRankU8 response[6])
{
    MuRankU8 packet[5];
    const MuRankedSetupStep* s = mu_ranked_setup_current(m);
    if (!s || s->player == m->local_player || !response || !response[0]) return 0;
    packet[0] = m->step;
    if (response[1]) {
        /* :878-883: skip means deterministic automatic selection. */
        if (!mu_ranked_setup_packet(m, 0, 1, packet)) return 0;
    } else memcpy(packet+1, response+2, 4);
    return mu_ranked_setup_apply(m, s->player, packet);
}
int mu_ranked_setup_tick(MuRankedSetupModel* m)
{
    const MuRankedSetupStep* s = mu_ranked_setup_current(m);
    unsigned limit;
    if (!s) return MU_SETUP_WAIT;
    if (m->frames != 0xFFFFFFFFu) ++m->frames;
    limit = 60u * ((unsigned)s->seconds + m->policy.grace_seconds);
    if (s->player != m->local_player) limit += 60u * m->policy.wait_seconds;
    return m->frames > limit ? (s->player == m->local_player ? MU_SETUP_LOCAL_TIMEOUT : MU_SETUP_REMOTE_TIMEOUT) : MU_SETUP_WAIT;
}
void mu_ranked_setup_disconnect(MuRankedSetupModel* m)
{
    if (m) m->status = MU_SETUP_DISCONNECTED;
}
int mu_ranked_setup_record_game(MuRankedSet* s, int winner, unsigned end_mode,
                               MuRankU16 stage, int disconnected)
{
    /* slippi-ssbm-asm Online/Slippi Online Scene/main.asm:617-741. */
    s->last_end_mode = (MuRankU8)end_mode;
    if (disconnected || winner < -1 || winner > 1 || !s->cur_game || s->cur_game > 9)
        return MU_SETUP_SET_ABORTED;
    if (winner < 0) {
        if (s->tiebreak_num < 255) ++s->tiebreak_num;
        return MU_SETUP_TIEBREAK;
    }
    s->prev_winner = (MuRankU8)winner;
    s->game_results[s->cur_game-1] = (MuRankU8)winner;
    ++s->score[winner]; s->last_stage_win[winner] = stage;
    if (s->score[winner] >= (s->max_games+1)/2) return MU_SETUP_SET_OVER;
    ++s->cur_game; s->tiebreak_num = 0;
    return MU_SETUP_NEXT_GAME;
}
