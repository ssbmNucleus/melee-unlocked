/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Squad Strike idea: Chris's friend Tyler (Squad Strike Gecko codes).
 */
#if MU_SQUAD
/* Squad Strike on the VS character select (offline stock singles; included by mncharsel.c, whose
 * statics it reads). See include/mu_squad.h. It is the game's own coin, hand, sounds and labels:
 *
 *   - Z starts picking a squad for your door (holding a computer player's coin: for that player).
 *     The hand flashes as it does over a button, and the coin reads "1" in place of "P1": it is
 *     fighter 1. Coin in hand, put it down as usual; coin down, the hand takes the next token.
 *   - The hand then holds a "2" token, the coin's model with the number cut from the disc's own
 *     "P2" label: A on a fighter puts it down there (fighter 2) and the hand takes "3", then "4".
 *     The next token comes once the hand moves on from the one it put down (another portrait, or a
 *     few steps away), so the new one never covers it; until then A buzzes and B stops.
 *     Z again, or B holding a numbered token, stops: B throws the token in hand away, the ones
 *     already down stay.
 *   - While a door picks or has a squad, the banner over the portraits reads "Squad Strike" in its
 *     own lettering, where the rules' text ("4-man survival test!") is otherwise.
 *   - A on a numbered token that is down picks it up again, as on a coin: A on another fighter
 *     moves that pick there, B puts it back where it was, and carrying it off the portraits takes it
 *     out of the squad (the later ones move up a number).
 *   - X / Y: the colour of the fighter under the token in hand; with an empty hand, of the token
 *     put down last (the coin's own X / Y, skins too, when that was the coin).
 *   - The door's portrait shows the fighter under the token in hand, else the one put down last.
 *   - The squad fights in its numbers' order, one fighter a stock, with the stocks the rules give
 *     (they are not changed here): the last fighter keeps every stock left, and fighters past the
 *     stock count never come out (shim/mu_squad.c).
 *   - The error buzz: a fighter the squad has, an added fighter (squads are retail fighters), a
 *     squad that is full, more than two players (squads are 1v1), or squads too big to load.
 *   - The random corner works for a token too: a fighter the squad does not have.
 * Never in Vanilla, in Teams or in a timed match. Online only on the Direct select with the Squad
 * Strike row On and no peer keeping it off (shim/mu_online_options.c, gmSquad_DirectPickable): its one
 * door, door 0, picks the same way until this player locks in, and the lineup goes to the host on each
 * change (gmSquad_DirectLineup); the banner stays Slippi's there. Without Z the screen is the retail
 * one: no token exists until a door picks. */

int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
#include <string.h>

#include <melee/gm/gm_1601.h>
#include "mu_squad.h"
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbdvd.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>
#include <sysdolphin/baselib/wobj.h>
#include "mu_menu_label.h"

const char* mu_test_getenv(const char* name);

/* ---- state ------------------------------------------------------------------------------------ */

/* A numbered token: fighter k + 2 of a door's squad (gmSquad_rows[door][k]). Its gobj always draws
 * token k; where it is down outlives the screen (the gobj does not). */
typedef struct SquadToken {
    HSD_GObj* gobj;
    u8 door, slot, glow, timer;
    s8 icon;    /* the portrait it is down on, -1 none */
    f32 x, y;   /* where it is (the coin's x8, xC) */
    f32 dx, dy; /* where it is drawn (x10, x14) */
} SquadToken;
static SquadToken squad_tok[4][GMSQUAD_EXTRA];

static bool squad_visit_ready;  /* SquadReset ran for this screen; SquadExit ends it */
static u32 squad_frame;         /* frames since the screen opened */
static bool squad_pick[4];      /* the door is picking its squad (Z) */
static s8 squad_picker[4];      /* the hand picking it */
static s8 squad_hand[4];        /* the hand holding one of the door's tokens, -1 none */
static u8 squad_held[4];        /* ... which one */
static bool squad_held_new[4];  /* ... the next pick (not one that was down) */
static f32 squad_back_x[4], squad_back_y[4];
static s8 squad_back_icon[4];   /* a token picked up again: where it was */
static s8 squad_hover_icon[4];  /* the portrait under the token in hand, -1 none */
static u8 squad_hover_color[4]; /* ... and the colour it shows there */
static s8 squad_last[4];        /* the token put down last, -1 the coin */
static u16 squad_door_key[4];   /* the squad fighter the door shows (squad_door_show), 0 the game's */
static bool squad_in_show;      /* squad_door_show is drawing a door */
static u8 squad_coin_holder[4]; /* the coin's holder last frame (cursor + 1), 0 down */
/* Per door, the fighter and colour its coin was down on last frame (squad_coin_key; GMSQUAD_NONE:
 * in a hand, none, or the door closed). A pick that appears or changes with no hand dropping it (a
 * door opening on a coin, the game placing a random fighter) gets the budget check of a drop. */
static u8 squad_coin_seen[4];
static bool squad_coin_digit[4]; /* the coin's label shows "1" */
static u8 squad_logged[4][1 + 2 * GMSQUAD_SIZE];
/* Picking, a token (or the coin) just went down under the picker's hand: the next token waits until the
 * hand leaves that spot (squad_waiting), so it is not drawn over the one put down. */
static bool squad_wait[4];
static f32 squad_wait_x[4], squad_wait_y[4]; /* the hand where it put it down */
static s8 squad_wait_icon[4];                /* the portrait under its token spot then */
static HSD_Text* squad_banner;               /* the banner's text (catalog entry 0x4A), NULL none */
static bool squad_banner_ours;               /* ... it reads "Squad Strike" */

/* The tokens' numbers: the disc's "P1".."P4" token labels (32x24 IA4) without the P, centred. */
static u8 squad_digit_px[4][768] __attribute__((aligned(32)));
static HSD_ImageDesc squad_digit_desc[4];
static s8 squad_digits; /* 0 not cut yet, 1 cut, -1 the labels are not the retail ones */

/* ---- the screen ------------------------------------------------------------------------------- */

static PlayerInitData* squad_player(int door)
{
    return &mnCharSel_804D6CB0->vs.start.players[door];
}

static int squad_num_colors(u8 ck)
{
    return ck < 26 ? (int) mu_squad_colors(ck) : 1;
}

/* The online select in Direct, picking squads: the Squad Strike row is On and no peer keeps it off
 * (shim/mu_squad_online.c). Its one door is door 0, this player's. */
static bool squad_online(void)
{
    return mnCharSel_804D6CB0 != NULL && mu_slippi_css_online() && gmSquad_DirectPickable(squad_frame);
}

/* The offline VS select screen, or the online one above (the other online selects have no squads). */
static bool squad_screen(void)
{
    if (squad_online()) return true;
    return mnCharSel_804D6CB0 != NULL && mnCharSel_804D6CB0->match_type == VS_MELEE &&
           mnCharSel_804D6CF5 != 1 && !mu_slippi_css_online() && !mu_online_active() &&
           !mu_online_p2p_menus();
}

/* ... with rules squads play: stock singles, not Vanilla. Online the host agreed them (a Direct 1v1). */
static bool squad_rules(void)
{
    if (squad_online()) return !(mu_game_options() & MU_OPTION_VANILLA);
    return gmSquad_ModeAllowed() && !mnCharSel_804D6CB0->vs.start.rules.is_teams &&
           !(mu_game_options() & MU_OPTION_VANILLA);
}

static bool squad_live(void)
{
    return squad_visit_ready && squad_screen() && squad_rules();
}

static bool squad_door_open(int port)
{
    const u8 type = squad_player(port)->slot_type;
    if (port != 0 && mu_slippi_css_online()) return false;   /* the online select's one door */
    return type == Gm_PKind_Human || type == Gm_PKind_Cpu;
}

static int squad_players(void)
{
    int n = 0, p;
    for (p = 0; p < 4; p++) {
        n += squad_door_open(p);
    }
    return n;
}

/* The door's coin is in someone's hand. While it is, players[].ckind still holds the fighter it was
 * last on, but that is no pick until the coin goes down again. */
static bool squad_coin_held(int port)
{
    return mnCharSel_804A0BD0[port] != NULL && mnCharSel_804A0BD0[port]->x5 != 0;
}

/* The door's coin pick, or GMSQUAD_NONE while it has none (coin in hand). */
static u8 squad_coin_pick(int port)
{
    const u8 ck = (u8) squad_player(port)->ckind;
    return ck < 26 && !squad_coin_held(port) ? ck : GMSQUAD_NONE;
}

/* The fighter and colour the door shows for its coin (the portrait under a carried coin, X / Y
 * recolour it there). False if the door shows none. */
static bool squad_door_shows(int port, u8* ck, u8* color)
{
    struct CSSDoor* door = &mnCharSel_803F0DFC.doors[port];
    if (door->sel_icon >= 0x19) {
        return false;
    }
    *ck = icons[door->sel_icon].char_kind;
    *color = door->costume;
    return *ck < 26;
}

/* A coin pick's colour as the screen has it now: the door's costume when the door shows the coin
 * pick (players[].color follows only on leaving), else the last colour the game recorded; always one
 * the fighter has (mu_squad_color). */
static u8 squad_coin_color(int port)
{
    PlayerInitData* pl = squad_player(port);
    const u8 ck = (u8) pl->ckind;
    u8 shown_ck, shown_color;
    u8 color = pl->color;
    if (squad_door_shows(port, &shown_ck, &shown_color) && shown_ck == ck) {
        color = shown_color;
    }
    return ck < 26 ? mu_squad_color(ck, color) : color;
}

/* Another port has this fighter in this colour (coin pick or squad). */
static bool squad_clash(int port, u8 ck, u8 color)
{
    int p, i;
    for (p = 0; p < 4; p++) {
        if (p == port || !squad_door_open(p)) {
            continue;
        }
        if ((u8) squad_player(p)->ckind == ck && squad_coin_color(p) == color) {
            return true;
        }
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            if (gmSquad_rows[p][i].ckind == ck && gmSquad_rows[p][i].color == color) {
                return true;
            }
        }
    }
    return false;
}

/* The door's squad has fighter ck other than at place `skip` (-1: anywhere). */
static bool squad_has(int port, u8 ck, int skip)
{
    int i;
    for (i = 0; i < GMSQUAD_EXTRA; i++) {
        if (i != skip && gmSquad_rows[port][i].ckind == ck) {
            return true;
        }
    }
    return false;
}

/* Give a member the first colour from its own (+step) on that nobody else has. True if its colour
 * changed; false if none is free. */
static bool squad_fix_color(int port, int index, int step)
{
    GmSquadMember* m = &gmSquad_rows[port][index];
    int n, tries;
    if (m->ckind >= ChKind_Max) {
        return false;
    }
    n = squad_num_colors(m->ckind);
    for (tries = 0; tries < n; tries++) {
        const u8 c = (u8) ((m->color + step + tries) % n);
        if (!squad_clash(port, m->ckind, c)) {
            if (c == m->color) {
                return false;
            }
            m->color = c;
            return true;
        }
    }
    return false;
}

/* The first colour of fighter ck from `from` on that no other port has. */
static u8 squad_free_color(int port, u8 ck, int from)
{
    const int n = squad_num_colors(ck);
    int t;
    for (t = 0; t < n; t++) {
        const u8 c = (u8) ((from + t) % n);
        if (!squad_clash(port, ck, c)) {
            return c;
        }
    }
    return (u8) (from % n);
}

/* Every squad and coin pick on the screen, as the match would load them, fits the memory budget,
 * with the facts both machines would share (mu_squad.c squad_fixed_budget). */
static bool squad_budget(int coin_port, u8 coin_ck, u8 coin_color)
{
    GmSquadMember members[GMSQUAD_PORTS * GMSQUAD_SIZE];
    int p, count = 0;
    for (p = 0; p < GMSQUAD_PORTS; ++p) {
        if (!squad_door_open(p)) continue;
        count += gmSquad_Lineup(p, p == coin_port ? coin_ck : squad_coin_pick(p),
                                p == coin_port ? coin_color : squad_coin_color(p), &members[count]);
    }
    return gmSquad_Budget(members, count, -1);
}

/* A pick is checked as it is made (never a silent trim after START): refused, the error buzz. */
static bool squad_fits(int target, int coin_port, u8 coin_ck, u8 coin_color)
{
    if (squad_budget(coin_port, coin_ck, coin_color)) return true;
    OSReport("squad: P%d pick refused, too big to load\n", target + 1);
    return false;
}

/* Move a member to the next (dir 1) or previous (dir -1) colour nobody else has. False if no other
 * colour is free or it would not fit. */
static bool squad_cycle_color(int port, int index, int dir)
{
    GmSquadMember* m = &gmSquad_rows[port][index];
    const u8 old = m->color;
    int n, t;
    if (m->ckind >= ChKind_Max) {
        return false;
    }
    n = squad_num_colors(m->ckind);
    for (t = 1; t < n; t++) {
        const u8 c = (u8) (((int) m->color + dir * t + n * t) % n);
        if (!squad_clash(port, m->ckind, c)) {
            m->color = c;
            if (!squad_fits(port, -1, GMSQUAD_NONE, 0)) {
                m->color = old;
                return false;
            }
            return true;
        }
    }
    return false;
}

/* Put the door's coin in cursor cport's hand, exactly as an A press on the coin does
 * (mnCharSel_CursorThink's pickup). */
static void squad_regrab(int door, int cport)
{
    struct CSSCharModel* coin = mnCharSel_804A0BD0[door];
    struct CSSCursorData* cursor;
    if (coin == NULL || cport < 0 || cport >= 4) return;
    cursor = mnCharSel_804A0BC0[cport];
    if (cursor == NULL) return;
    coin->x5 = (u8) (cport + 1);
    cursor->x5 = 1;
    cursor->x6 = (u8) door;
    mnCharSel_803F0DFC.doors[door].sel_icon = 0xD;
    HSD_GObjGXLink_803909D8(coin->gobj, cursor->gobj);
    cursor->xC = coin->x8 - 2.7f;
    cursor->x10 = coin->xC + 2.0f;
    squad_coin_holder[door] = (u8) (cport + 1);
}

/* The hand a refused coin that no hand dropped goes to: the door's own pointer, else the first free
 * one in port order (not carrying, not hidden); -1 if none (the setup trim still holds). */
static int squad_free_hand(int door)
{
    int i;
    for (i = -1; i < 4; ++i) {
        const int c = i < 0 ? door : i;
        struct CSSCursorData* cursor = c >= 0 && c < 4 ? mnCharSel_804A0BC0[c] : NULL;
        if (cursor != NULL && cursor->x5 != 1 && cursor->x5 != 3) return c;
    }
    return -1;
}

static u8 squad_coin_key(int p)
{
    const u8 ck = squad_coin_pick(p);
    return ck == GMSQUAD_NONE ? GMSQUAD_NONE : (u8) (ck * 8 + (squad_coin_color(p) & 7));
}

/* The retail portrait of a fighter the screen offers (unlocked), or -1. */
static int squad_icon_of(u8 ck)
{
    int icon;
    for (icon = 0; icon < 0x19; ++icon) {
        if (icons[icon].char_kind == ck) return icons[icon].state < 1 ? -1 : icon;
    }
    return -1;
}

/* The portrait a coin put down at (x, y) would take, as the game tests it (an added fighter's too),
 * or -1. */
static int squad_point_icon(f32 x, f32 y)
{
    int i;
    for (i = 0; i < CSS_NONE; i++) {
        if (x > icons[i].bound_l && x < icons[i].bound_r && y < icons[i].bound_u &&
            y > icons[i].bound_d && icons[i].state >= 1)
        {
            return i;
        }
    }
    return -1;
}

/* The random corners, as the game tests a coin put down there (mnCharSel_CursorThink). */
static bool squad_corner(f32 x, f32 y)
{
    int i;
#ifdef MU_NATIVE
    if ((mu_mex_css_codes & MEX_CSS_NO_RANDOM_CORNERS) != 0) return false;
#endif
    if (!(y < 6.0f && y > -1.0f &&
          ((x > -30.0f && x < -24.4f && roster_icons < 1) || (x > 24.4f && x < 30.2f && roster_icons < 2))))
    {
        return false;
    }
    for (i = 0; i < CSS_NONE; i++) {
        if (icons[i].state < 2) return false;
    }
    return true;
}

/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026).
 * Includes a fix from v1.0.1; the author's confirmation for that version is pending.
 * Who the player facing this door has at this place in their squad, so Random doesn't mirror them. */
static u8 squad_rival_at(int target, int pos)
{
    GmSquadMember line[GMSQUAD_SIZE];
    int p, n;
    for (p = 0; p < 4; p++) {
        if (p == target || !squad_door_open(p)) {
            continue;
        }
        n = gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), line);
        return pos < n ? line[pos].ckind : GMSQUAD_NONE;
    }
    return GMSQUAD_NONE;
}

/* A token on the random corner: a retail fighter the squad does not have (the coin's and the other
 * tokens'), not the rival's at the same place when there is a choice. Its portrait, or -1. The same
 * synced random stream as the game's random coin (gmSquad_Random). */
static int squad_random_icon(int d, int k)
{
    u8 pool[0x19];
    const u8 coin = squad_coin_pick(d);
    const u8 rival = squad_rival_at(d, k + 1);
    int n = 0, i;
    for (i = 0; i < 0x19; i++) {
        const u8 ck = icons[i].char_kind;
        if (icons[i].state >= 1 && ck < 26 && ck != coin && !squad_has(d, ck, k)) {
            pool[n++] = (u8) i;
        }
    }
    if (n > 1) {
        for (i = 0; i < n; i++) {
            if (icons[pool[i]].char_kind == rival) {
                pool[i] = pool[--n];
                break;
            }
        }
    }
    return n == 0 ? -1 : pool[gmSquad_Random(d, k) % (u32) n];
}

/* A portrait takes a pick: its flash, the announcer's call (mnCharSel_8025FB50's). */
static void squad_flash(int icon)
{
    HSD_JObj* jobj = NULL;
    mu_css_icon_jobj(mnCharSel_804D6CC0, &jobj, icon);
    if (jobj != NULL) {
        HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjReqAnim, AOBJ_ARG_AF, 10.0);
    }
    icons[icon].anim_timer = 0xC;
    mu_css_icon_call(icon);
    gm_80168C5C(icons[icon].char_kind);
}

/* ---- numbers ---------------------------------------------------------------------------------- */

/* The label texture of a coin model (its joint 4): the one with the port labels' image table. */
static HSD_TObj* squad_label_tobj(HSD_JObj* jobj)
{
    HSD_JObj* part = NULL;
    HSD_DObj* dobj;
    lb_80011E24(jobj, &part, 4, -1);
    for (dobj = part != NULL ? HSD_JObjGetDObj(part) : NULL; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* tobj;
        for (tobj = dobj->mobj != NULL ? dobj->mobj->tobj : NULL; tobj != NULL; tobj = tobj->next) {
            if (tobj->imagetbl != NULL && tobj->mu_n_imagetbl >= 5) return tobj;
        }
    }
    return NULL;
}

/* The retail labels (FNV-1a of each 768-byte image) and where each one's number starts, per row
 * (the P's right edge; on "P2" the 2's foot reaches one column further left). */
static const u32 squad_label_fnv[4] = { 0x5BA6A421u, 0xAAB14B96u, 0xCC334C2Du, 0xF5573553u };
static int squad_digit_cut(int k, int y)
{
    return k == 0 ? 17 : k == 1 ? (y < 16 ? 16 : 15) : 16;
}
static int squad_ia4_at(int x, int y)
{
    return ((y >> 2) * 4 + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7);
}

/* Cut the numbers 1-4 out of the disc's labels once: what lies right of the P, moved to the middle.
 * A disc whose labels are not the retail ones keeps the port labels ("P2" for fighter 2). */
static void squad_digits_cut(HSD_TObj* tobj)
{
    DISC_PTR(struct HSD_ImageDesc)* table = (void*) tobj->imagetbl;
    int k;
    squad_digits = -1;
    for (k = 0; k < 4; k++) {
        HSD_ImageDesc* image = DP(table[k]);
        const u8* src;
        u32 hash = 0x811C9DC5u;
        int i, x, y, left = 32, right = -1, shift;
        if (image == NULL || image->width != 32 || image->height != 24 || image->format != GX_TF_IA4 ||
            DISC_NULL(image->image_ptr))
        {
            OSReport("squad: token label %d is not a 32x24 IA4 image: port labels kept\n", k);
            return;
        }
        src = DP(image->image_ptr);
        for (i = 0; i < 768; i++) {
            hash = (hash ^ src[i]) * 0x01000193u;
        }
        if (hash != squad_label_fnv[k]) {
            OSReport("squad: token label %d is not the retail one (%08x): port labels kept\n", k, hash);
            return;
        }
        for (y = 0; y < 24; y++) {
            for (x = squad_digit_cut(k, y); x < 32; x++) {
                if (src[squad_ia4_at(x, y)] >> 4) {
                    left = x < left ? x : left;
                    right = x > right ? x : right;
                }
            }
        }
        if (right < left) {
            OSReport("squad: token label %d has no number: port labels kept\n", k);
            return;
        }
        shift = left - (32 - (right - left + 1)) / 2;
        memset(squad_digit_px[k], 0, sizeof squad_digit_px[k]);
        for (y = 0; y < 24; y++) {
            for (x = squad_digit_cut(k, y); x < 32; x++) {
                if (x - shift >= 0 && x - shift < 32) {
                    squad_digit_px[k][squad_ia4_at(x - shift, y)] = src[squad_ia4_at(x, y)];
                }
            }
        }
        squad_digit_desc[k] = *image;
        DISC_SET(squad_digit_desc[k].image_ptr, (void*) squad_digit_px[k]);
    }
    squad_digits = 1;
    OSReport("squad: token numbers cut from the disc's port labels\n");
}

/* A coin model's label shows number n + 1 (n 0-3). Run after the model's animation. */
static void squad_label(HSD_JObj* jobj, int n)
{
    HSD_TObj* tobj = squad_label_tobj(jobj);
    if (tobj == NULL) return;
    if (squad_digits == 0) squad_digits_cut(tobj);
    if (squad_digits > 0) tobj->imagedesc = &squad_digit_desc[n];
}

/* ---- tokens ----------------------------------------------------------------------------------- */

/* A token that is down (not in a hand) and shown. */
static bool squad_token_down(int d, int k)
{
    const SquadToken* tok = &squad_tok[d][k];
    return tok->gobj != NULL && squad_door_open(d) && gmSquad_rows[d][k].ckind != GMSQUAD_NONE &&
           tok->icon >= 0 && !(squad_hand[d] >= 0 && squad_held[d] == k);
}

/* Tokens push each other and the coins' places apart as coins do, without the coins' random jitter
 * (a tie goes by token order), so a token never touches the game's random stream. */
static void squad_push(SquadToken* tok, f32 ox, f32 oy, bool after)
{
    f32 dx = tok->x - ox, dy = tok->y - oy, angle;
    if (dx == 0.0f && dy == 0.0f) {
        dx = after ? 0.05f : -0.05f;
        dy = 0.05f;
    }
    if (dx * dx + dy * dy >= 8.0f) return;
    if (dy < 0.01f && dy > -0.01f) {
        angle = (f32) (dx < 0.0f ? 1.5707963267948966 : -1.5707963267948966);
    } else {
        angle = (f32) ((dy < 0.0f ? 0.0 : 3.141592653589793) + atanf(dx / dy));
    }
    tok->x -= 0.01f * sinf(angle);
    tok->y -= 0.01f * cosf(angle);
}

/* A token that is down settles on its portrait as a coin does (fn_80262648). */
static void squad_settle(SquadToken* tok)
{
    const CSSIcon* icon = &icons[(int) tok->icon];
    const int me = tok->door * GMSQUAD_EXTRA + tok->slot;
    int it, q, s;
    for (it = 0; it < 0x14; it++) {
        for (q = 0; q < mnCharSel_804D6CF5 && q < 4; q++) {
            const struct CSSCharModel* coin = mnCharSel_804A0BD0[q];
            const CSSDoor* door = &mnCharSel_803F0DFC.doors[q];
            if (coin != NULL && coin->x5 == 0 && door->p_kind != 3 && door->sel_icon < CSS_NONE) {
                squad_push(tok, coin->x8, coin->xC, true);
            }
        }
        for (q = 0; q < 4; q++) {
            for (s = 0; s < GMSQUAD_EXTRA; s++) {
                const SquadToken* other = &squad_tok[q][s];
                if (other != tok && squad_token_down(q, s)) {
                    squad_push(tok, other->x, other->y, me > q * GMSQUAD_EXTRA + s);
                }
            }
        }
        if (tok->x < 1.5f + icon->bound_l) {
            tok->x += 0.02f;
            if (tok->x < icon->bound_l) tok->x = icon->bound_l;
        }
        if (tok->x > icon->bound_r - 1.5f) {
            tok->x -= 0.02f;
            if (tok->x > icon->bound_r) tok->x = icon->bound_r;
        }
        if (tok->y > icon->bound_u - 1.5f) {
            tok->y -= 0.02f;
            if (tok->y > icon->bound_u) tok->y = icon->bound_u;
        }
        if (tok->y < 1.5f + icon->bound_d) {
            tok->y += 0.02f;
            if (tok->y < icon->bound_d) tok->y = icon->bound_d;
        }
    }
}

/* Door d's pick that went down last (k: its token's slot, -1 the coin) while the hand that put it there
 * has not moved on (squad_wait): it is drawn in front of that hand, so the open hand does not hide its
 * number; then with the coins again. */
static bool squad_just_down(int d, int k)
{
    return squad_wait[d] && squad_last[d] == k;
}

/* In front of the hands: drawn after them (their gx link 3, after their priority 0x80) and nearer the
 * camera than their fingers (the hand stands at depth 0, its fingers well in front of a coin's 1, which
 * is how a coin goes under the fingers and over the cuff). To keep its place and size on screen it moves
 * along the line from the camera's eye and shrinks by the same ratio. */
#define SQUAD_FRONT_Z 12.0f
static bool squad_front_on[4][GMSQUAD_EXTRA + 1];      /* [door][slot + 1]: drawn in front now */
static Vec3 squad_front_scale[4][GMSQUAD_EXTRA + 1];   /* ... its scale before */
static bool squad_front_logged;

/* The ratio a pick at depth 1 shrinks by at SQUAD_FRONT_Z, and the eye it moves toward. False when the
 * select's camera is not one that ratio fits (then the pick stays where it is). */
static bool squad_front_ratio(Vec3* eye, f32* t)
{
    const HSD_CObjDesc* cam = MenMain_cam;
    if (cam == NULL || DISC_NULL(cam->common.eyepos)) return false;
    eye->x = DP(cam->common.eyepos)->pos.x;
    eye->y = DP(cam->common.eyepos)->pos.y;
    eye->z = DP(cam->common.eyepos)->pos.z;
    if (cam->common.projection_type == PROJ_ORTHO) {
        *t = 1.0f;
    } else if (cam->common.projection_type == PROJ_PERSPECTIVE && eye->z > SQUAD_FRONT_Z + 4.0f) {
        *t = (eye->z - SQUAD_FRONT_Z) / (eye->z - 1.0f);
    } else {
        return false;
    }
    if (!squad_front_logged) {
        squad_front_logged = true;
        OSReport("squad: a pick just put down is drawn in front of its hand (camera %d, eye %.1f %.1f %.1f, "
                 "ratio %.3f)\n",
                 (int) cam->common.projection_type, eye->x, eye->y, eye->z, *t);
    }
    return true;
}

/* Door d's pick k (-1 the coin) at (x, y), depth 1: in front of the hands (front) or with the coins. */
static void squad_draw_front(int d, int k, HSD_GObj* gobj, HSD_JObj* jobj, f32 x, f32 y, bool front)
{
    bool* on = &squad_front_on[d][k + 1];
    Vec3* scale = &squad_front_scale[d][k + 1];
    Vec3 eye;
    f32 t;
    if (gobj == NULL || jobj == NULL) return;
    if (front && squad_front_ratio(&eye, &t)) {
        if (!*on) {
            HSD_JObjGetScale(jobj, scale);
            HSD_GObjGXLink_80390908(gobj, 3, 0x81);
            *on = true;
        }
        HSD_JObjSetTranslateX(jobj, eye.x + (x - eye.x) * t);
        HSD_JObjSetTranslateY(jobj, eye.y + (y - eye.y) * t);
        HSD_JObjSetTranslateZ(jobj, SQUAD_FRONT_Z);
        HSD_JObjSetScaleX(jobj, scale->x * t);
        HSD_JObjSetScaleY(jobj, scale->y * t);
        HSD_JObjSetScaleZ(jobj, scale->z * t);
    } else if (*on) {
        HSD_JObjSetScale(jobj, scale);
        HSD_GObjGXLink_80390908(gobj, 2, 0x81);   /* after the portraits and coins, as it was */
        *on = false;
    }
}

/* A token's think (after the hands, with the coins): its port's glow, its place, its number. */
static void squad_token_think(HSD_GObj* gobj)
{
    SquadToken* tok = gobj->user_data;
    HSD_JObj* jobj = GET_JOBJ(gobj);
    const int d = tok->door, k = tok->slot;
    const bool held = squad_visit_ready && squad_hand[d] >= 0 && squad_held[d] == k;
    struct CSSCursorData* cursor = held ? mnCharSel_804A0BC0[(int) squad_hand[d]] : NULL;
    u8 glow;
    if (!squad_live() || !squad_door_open(d) ||
        (held ? cursor == NULL : !squad_token_down(d, k)))
    {
        HSD_JObjSetFlagsAll(jobj, JOBJ_HIDDEN);
        return;
    }
    HSD_JObjClearFlagsAll(jobj, JOBJ_HIDDEN);
    glow = mnCharSel_803F0DFC.doors[d].p_kind == 1 ? 8 : (u8) d;
    tok->timer = (u8) (tok->timer + 1);
    if (glow != tok->glow || tok->timer > 0x27U) {
        HSD_JObj* part = NULL;
        animateCharModel(jobj, glow < 4 ? (f32) (d * 4) : 16.0f);
        lb_80011E24(jobj, &part, 3, -1);
        if (part != NULL) {
            HSD_ForeachAnim(part, JOBJ_TYPE, MOBJ_MASK, HSD_AObjReqAnim, AOBJ_ARG_AF, (f32) (glow * 0x28));
        }
        tok->glow = glow;
        tok->timer = 0;
    }
    if (held) {
        tok->x = 2.7f + cursor->xC;
        tok->y = -2.0f + cursor->x10;
    } else {
        squad_settle(tok);
    }
    {
        const f32 ex = tok->dx - tok->x, ey = tok->dy - tok->y;
        if (ex * ex + ey * ey < 4.0f) {
            tok->dx = tok->x;
            tok->dy = tok->y;
        } else {
            f32 angle;
            if (ey < 0.01f && ey > -0.01f) {
                angle = (f32) (ex < 0.0f ? 1.5707963267948966 : -1.5707963267948966);
            } else {
                angle = (f32) ((ey < 0.0f ? 0.0 : 3.141592653589793) + atanf(ex / ey));
            }
            tok->dx += 3.0f * sinf(angle);
            tok->dy += 3.0f * cosf(angle);
        }
    }
    HSD_JObjSetTranslateX(jobj, tok->dx);
    HSD_JObjSetTranslateY(jobj, tok->dy);
    HSD_JObjSetTranslateZ(jobj, 1.0f);
    HSD_JObjAnimAll(jobj);
    squad_draw_front(d, k, gobj, jobj, tok->dx, tok->dy, !held && squad_just_down(d, k));
    squad_label(jobj, k + 1);
}

static void squad_token_free(void* data)
{
    (void) data; /* squad_tok is static */
}

/* The door's tokens, made as the game makes a coin (mnCharSel_802640A0), hidden until used. A token
 * whose place does not fit its fighter any more starts at that fighter's coin spot. */
static void squad_tokens_make(int d)
{
    int k;
    if (css_models == NULL) return;
    for (k = 0; k < GMSQUAD_EXTRA; k++) {
        SquadToken* tok = &squad_tok[d][k];
        const u8 ck = gmSquad_rows[d][k].ckind;
        HSD_GObj* gobj;
        HSD_JObj* jobj;
        if (tok->gobj != NULL) continue;
        gobj = GObj_Create(4, 5, 0x80);
        if (gobj == NULL) return;
        jobj = HSD_JObjLoadJoint(DP(css_models->token.joint));
        HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
        GObj_InitUserData(gobj, 4, squad_token_free, tok);
        GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 2, 0x80);
        HSD_GObj_SetupProc(gobj, squad_token_think, 2);
        HSD_JObjAddAnimAll(jobj, DP(css_models->token.animjoint), DP(css_models->token.matanim_joint),
                           DP(css_models->token.shapeanim_joint));
        HSD_JObjReqAnimAll(jobj, 0.0f);
        HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV, 0, 0);
        HSD_JObjSetFlagsAll(jobj, JOBJ_HIDDEN);
        tok->gobj = gobj;
        tok->door = (u8) d;
        tok->slot = (u8) k;
        tok->glow = 0xFF;
        tok->timer = 0;
        if (ck != GMSQUAD_NONE &&
            (tok->icon < 0 || tok->icon >= 0x19 || icons[(int) tok->icon].char_kind != ck))
        {
            const int icon = squad_icon_of(ck);
            tok->icon = (s8) icon;
            if (icon >= 0) {
                tok->x = 3.4f + icons[icon].bound_l;
                tok->y = -3.0f + icons[icon].bound_u;
            }
        }
        tok->dx = tok->x;
        tok->dy = tok->y;
        if (ck != GMSQUAD_NONE && mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1] != NULL) {
            HSD_GObjGXLink_803909D8(gobj, mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1]->gobj);
        }
    }
}

/* ---- the door's portrait ---------------------------------------------------------------------- */

static u16 squad_show_key(u8 ck, u8 color)
{
    return (u16) (0x8000 | (ck << 4) | (color & 15));
}

/* The door shows a squad fighter: the game draws it (portrait, name) as if the coin were on that
 * fighter in that colour, and everything the coin's pick is made of is put back. */
static void squad_door_show(int d, u8 ck, u8 color)
{
    CSSDoor* door = &mnCharSel_803F0DFC.doors[d];
    const int icon = squad_icon_of(ck);
    u8 sel, prev, costume, pcolor;
    if (icon < 0 || mnCharSel_803F0E8C[d].data == NULL) return;
    sel = door->sel_icon;
    prev = door->sel_icon_prev;
    costume = door->costume;
    pcolor = squad_player(d)->color;
    door->sel_icon = door->sel_icon_prev = (u8) icon;
    door->costume = color;
    squad_door_key[d] = squad_show_key(ck, color);
    squad_in_show = true;
    mnCharSel_8025DB34((u8) d);
    squad_in_show = false;
    door->sel_icon = sel;
    door->sel_icon_prev = prev;
    door->costume = costume;
    squad_player(d)->color = pcolor;
}

/* The door shows the game's own again (its coin's pick). */
static void squad_door_game(int d)
{
    if (squad_door_key[d] == 0) return;
    squad_door_key[d] = 0;
    if (mnCharSel_804A0BD0[d] != NULL && mnCharSel_804A0BD0[d]->x5 == 0 &&
        mnCharSel_803F0DFC.doors[d].sel_icon < CSS_NONE && mnCharSel_803F0E8C[d].data != NULL)
    {
        squad_in_show = true;
        mnCharSel_8025DB34((u8) d);
        squad_in_show = false;
    }
}

/* What the door shows: the fighter under the token in hand (in the colour it would take), else the
 * token in hand's own pick, else the token put down last; the coin's while the coin is in a hand. */
static void squad_door_update(int d)
{
    u8 ck = GMSQUAD_NONE, color = 0;
    int row = -1;
    if (!squad_visit_ready || squad_in_show) return;
    if (!squad_live() || !squad_door_open(d)) {
        squad_door_game(d);
        return;
    }
    if (squad_hand[d] >= 0) {
        const int icon = squad_hover_icon[d];
        if (icon >= 0 && icons[icon].char_kind < 26) {
            ck = icons[icon].char_kind;
            color = squad_hover_color[d];
        } else if (!squad_held_new[d]) {
            row = squad_held[d];
        } else if (squad_last[d] >= 0 && squad_coin_pick(d) != GMSQUAD_NONE) {
            row = squad_last[d];
        }
    } else if (squad_last[d] >= 0 && squad_coin_pick(d) != GMSQUAD_NONE) {
        row = squad_last[d];
    }
    if (row >= 0) {
        ck = gmSquad_rows[d][row].ckind;
        color = gmSquad_rows[d][row].color;
    }
    if (ck >= 26) {
        squad_door_game(d);
        return;
    }
    if (squad_door_key[d] != squad_show_key(ck, color)) {
        squad_door_show(d, ck, color);
    }
}

/* ---- picking ---------------------------------------------------------------------------------- */

static void squad_stop(int d)
{
    if (squad_pick[d]) {
        OSReport("squad: P%d picking off\n", d + 1);
    }
    squad_pick[d] = false;
    squad_picker[d] = -1;
    squad_wait[d] = false;
}

/* The hand lets go of the door's token: the next pick is put away, one that was down goes back. */
static void squad_release(int d)
{
    SquadToken* tok;
    if (squad_hand[d] < 0) return;
    tok = &squad_tok[d][squad_held[d]];
    if (!squad_held_new[d]) {
        tok->x = squad_back_x[d];
        tok->y = squad_back_y[d];
        tok->icon = squad_back_icon[d];
        if (squad_visit_ready && tok->gobj != NULL && mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1] != NULL) {
            HSD_GObjGXLink_803909D8(tok->gobj, mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1]->gobj);
        }
        OSReport("squad: P%d token %d back\n", d + 1, squad_held[d] + 2);
    } else {
        OSReport("squad: P%d token %d put away\n", d + 1, squad_held[d] + 2);
    }
    squad_hand[d] = -1;
    squad_hover_icon[d] = -1;
}

static void squad_stop_all(void)
{
    int d;
    for (d = 0; d < 4; d++) {
        squad_release(d);
        squad_stop(d);
    }
}

/* Take the door's fighter k + 2 out of the squad: the later ones move up a number, their tokens with
 * them. */
static void squad_remove_row(int d, int k)
{
    int j;
    OSReport("squad: P%d token %d taken out (%u:%u)\n", d + 1, k + 2, gmSquad_rows[d][k].ckind,
             gmSquad_rows[d][k].color);
    for (j = k; j < GMSQUAD_EXTRA - 1; j++) {
        gmSquad_rows[d][j] = gmSquad_rows[d][j + 1];
        squad_tok[d][j].x = squad_tok[d][j + 1].x;
        squad_tok[d][j].y = squad_tok[d][j + 1].y;
        squad_tok[d][j].dx = squad_tok[d][j + 1].dx;
        squad_tok[d][j].dy = squad_tok[d][j + 1].dy;
        squad_tok[d][j].icon = squad_tok[d][j + 1].icon;
    }
    gmSquad_rows[d][GMSQUAD_EXTRA - 1].ckind = GMSQUAD_NONE;
    gmSquad_rows[d][GMSQUAD_EXTRA - 1].color = 0;
    squad_tok[d][GMSQUAD_EXTRA - 1].icon = -1;
    if (squad_last[d] == k) {
        squad_last[d] = -1;
    } else if (squad_last[d] > k) {
        squad_last[d]--;
    }
}

/* The portrait under the door's token in hand, and its colour there: on a new portrait, the colour
 * of the squad's own pick of that fighter (the coin's or a token's), else the first nobody else has;
 * sfxMove as on a coin (`sound`; not for the token the hand has just been given). */
static void squad_hover(int d, bool sound)
{
    struct CSSCursorData* cursor;
    int icon, i;
    if (squad_hand[d] < 0 || (cursor = mnCharSel_804A0BC0[(int) squad_hand[d]]) == NULL) return;
    icon = squad_point_icon(cursor->xC + 2.7f, cursor->x10 - 2.0f);
    if (icon == squad_hover_icon[d]) return;
    squad_hover_icon[d] = (s8) icon;
    if (icon >= 0 && icons[icon].char_kind < 26) {
        const u8 ck = icons[icon].char_kind;
        squad_hover_color[d] = squad_free_color(d, ck, 0);
        if (ck == squad_coin_pick(d)) {
            squad_hover_color[d] = squad_coin_color(d);
        }
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            if (gmSquad_rows[d][i].ckind == ck) squad_hover_color[d] = gmSquad_rows[d][i].color;
        }
        if (sound) sfxMove();
    }
}

/* Token k of the door goes into cursor c's hand: the next pick (`fresh`, at the hand), or one that
 * is down picked up again (the hand goes to it, as to a coin). */
static void squad_hand_token(int d, int c, int k, bool fresh)
{
    struct CSSCursorData* cursor = mnCharSel_804A0BC0[c];
    SquadToken* tok;
    squad_tokens_make(d);
    tok = &squad_tok[d][k];
    if (tok->gobj == NULL || cursor == NULL) return;
    squad_hand[d] = (s8) c;
    squad_held[d] = (u8) k;
    squad_held_new[d] = fresh;
    squad_hover_icon[d] = -1;
    if (fresh) {
        tok->x = tok->dx = 2.7f + cursor->xC;
        tok->y = tok->dy = -2.0f + cursor->x10;
        tok->icon = -1;
    } else {
        squad_back_x[d] = tok->x;
        squad_back_y[d] = tok->y;
        squad_back_icon[d] = tok->icon;
        cursor->xC = tok->x - 2.7f;
        cursor->x10 = tok->y + 2.0f;
        OSReport("squad: P%d token %d picked up\n", d + 1, k + 2);
    }
    HSD_GObjGXLink_803909D8(tok->gobj, cursor->gobj);
    squad_hover(d, false);
}

/* The hand is over the portraits (where a coin can be carried). */
static bool squad_in_grid(const struct CSSCursorData* cursor)
{
    return !(cursor->x10 < 0.2f) && !(cursor->x10 > 22.0f);
}

/* The picker's hand is still where it put the last pick down: the same portrait under its token spot
 * and under 3 units away. Once it is not, the wait is over. */
static bool squad_waiting(int d, const struct CSSCursorData* cursor)
{
    f32 dx, dy;
    if (!squad_wait[d] || cursor == NULL) return false;
    dx = cursor->xC - squad_wait_x[d];
    dy = cursor->x10 - squad_wait_y[d];
    if (dx * dx + dy * dy < 9.0f && squad_point_icon(cursor->xC + 2.7f, cursor->x10 - 2.0f) == squad_wait_icon[d]) {
        return true;
    }
    squad_wait[d] = false;
    return false;
}

/* Picking, door d's pick went down at (x, y) from hand c: the next token. Where the token would come
 * (the hand's token spot) covers that pick, it comes once the hand moves on (mnCharSel_SquadFrame);
 * elsewhere (the random corner sends a pick away) at once. */
static void squad_next_token(int d, int c, f32 x, f32 y)
{
    struct CSSCursorData* cursor = mnCharSel_804A0BC0[c];
    f32 dx, dy;
    squad_picker[d] = (s8) c;
    squad_wait[d] = false;
    if (cursor == NULL) return;
    dx = cursor->xC + 2.7f - x;
    dy = cursor->x10 - 2.0f - y;
    if (dx * dx + dy * dy >= 16.0f) {
        squad_hand_token(d, c, gmSquad_Count(d), true);
        return;
    }
    squad_wait[d] = true;
    squad_wait_x[d] = cursor->xC;
    squad_wait_y[d] = cursor->x10;
    squad_wait_icon[d] = (s8) squad_point_icon(cursor->xC + 2.7f, cursor->x10 - 2.0f);
}

/* The token in the door's hand goes down on portrait `icon` as fighter ck in `color`, at (x, y).
 * False (nothing changes) for a fighter the squad has or no retail one, a colour someone has, or
 * squads too big to load. */
static bool squad_place(int d, int icon, u8 ck, u8 color, f32 x, f32 y)
{
    const int k = squad_held[d];
    const GmSquadMember old = gmSquad_rows[d][k];
    SquadToken* tok = &squad_tok[d][k];
    if (tok->gobj == NULL || ck >= 26 || color >= squad_num_colors(ck) || ck == squad_coin_pick(d) ||
        squad_has(d, ck, k) || squad_clash(d, ck, color))
    {
        return false;
    }
    gmSquad_rows[d][k].ckind = ck;
    gmSquad_rows[d][k].color = color;
    if (!squad_fits(d, -1, GMSQUAD_NONE, 0)) {
        gmSquad_rows[d][k] = old;
        return false;
    }
    tok->x = x;
    tok->y = y;
    tok->icon = (s8) icon;
    if (mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1] != NULL) {
        HSD_GObjGXLink_803909D8(tok->gobj, mnCharSel_804A0BC0[mnCharSel_804D6CF5 - 1]->gobj);
    }
    OSReport("squad: P%d token %d %s on %u:%u\n", d + 1, k + 2, squad_held_new[d] ? "put down" : "moved",
             ck, color);
    squad_hand[d] = -1;
    squad_hover_icon[d] = -1;
    squad_last[d] = (s8) k;
    squad_flash(icon);
    lbAudioAx_800237A8(0xB8, 0x7F, 0x40);
    return true;
}

/* Z: picking starts (the door's coin in hand, or a free hand with its own coin down), or stops. */
static void squad_z(int c, struct CSSCursorData* cursor)
{
    const bool coin_in_hand = cursor->x5 == 1;
    int d = c;
    if (coin_in_hand) {
        const CSSDoor* door;
        if (cursor->x6 >= 4) return; /* a slider */
        d = cursor->x6;
        door = &mnCharSel_803F0DFC.doors[d];
        if (mu_hp_css_live() && (HSD_PadCopyStatus[c].trigger & 0xFFFF) == HSD_PAD_Z &&
            door->sel_icon < CSS_NONE && door->sel_icon == door->sel_icon_prev)
        {
            return; /* the 20XX Hack Pack's Z over a portrait (mu_hp_css_frame) */
        }
    }
    if (!squad_door_open(d)) return;
    HSD_PadCopyStatus[c].trigger &= ~HSD_PAD_Z;
    if (squad_pick[d]) {
        squad_release(d);
        squad_stop(d);
        sfxBack();
        return;
    }
    if (squad_players() > 2 || gmSquad_Count(d) >= GMSQUAD_EXTRA || squad_hand[d] >= 0 ||
        (!coin_in_hand && (squad_coin_pick(d) == GMSQUAD_NONE ||
                           mnCharSel_803F0DFC.doors[d].sel_icon >= 0x19)))
    {
        lbAudioAx_80024030(3);
        return;
    }
    squad_pick[d] = true;
    squad_picker[d] = (s8) c;
    OSReport("squad: P%d picking on (hand %d)\n", d + 1, c + 1);
    if (coin_in_hand || !squad_in_grid(cursor)) {
        sfxForward(); /* off the portraits, the token comes once the hand is over them */
    } else {
        squad_hand_token(d, c, gmSquad_Count(d), true);
        lbAudioAx_800237A8(0xB7, 0x7F, 0x40);
    }
}

/* A press of the hand that holds door d's token: every button is the squad's. */
static void squad_input_token(int c, struct CSSCursorData* cursor, int d, u32 trig)
{
    HSD_PadCopyStatus[c].trigger &=
        ~(HSD_PAD_A | HSD_PAD_B | HSD_PAD_X | HSD_PAD_Y | HSD_PAD_Z | HSD_PAD_L | HSD_PAD_R);
    if (trig & HSD_PAD_Z) {
        squad_release(d);
        squad_stop(d);
        sfxBack();
        return;
    }
    if (trig & HSD_PAD_B) {
        /* The next pick is put away (picking stops); one that was down goes back where it was. */
        squad_release(d);
        squad_stop(d);
        sfxBack();
        return;
    }
    if (trig & HSD_PAD_A) {
        const bool fresh = squad_held_new[d];
        const f32 x = cursor->xC + 2.7f, y = cursor->x10 - 2.0f;
        int icon;
        bool placed;
        squad_hover(d, true);
        if (squad_corner(x, y)) {
            icon = squad_random_icon(d, squad_held[d]);
            placed = icon >= 0 &&
                     squad_place(d, icon, icons[icon].char_kind, squad_free_color(d, icons[icon].char_kind, 0),
                                 3.4f + icons[icon].bound_l, -3.0f + icons[icon].bound_u);
        } else {
            icon = squad_hover_icon[d];
            placed = icon >= 0 && squad_place(d, icon, icons[icon].char_kind, squad_hover_color[d], x, y);
        }
        if (!placed) {
            lbAudioAx_80024030(3);
            return;
        }
        if (fresh && squad_pick[d]) {
            if (gmSquad_Count(d) < GMSQUAD_EXTRA) {
                const SquadToken* down = &squad_tok[d][squad_last[d] >= 0 ? squad_last[d] : 0];
                squad_next_token(d, c, down->x, down->y);
            } else {
                squad_stop(d); /* four fighters */
            }
        }
        return;
    }
    if (trig & (HSD_PAD_X | HSD_PAD_Y)) {
        const int dir = (trig & HSD_PAD_X) ? 1 : -1;
        const int icon = squad_hover_icon[d];
        int row = -1;
        if (icon >= 0 && icons[icon].char_kind < 26) {
            const u8 ck = icons[icon].char_kind;
            const int n = squad_num_colors(ck);
            int t;
            for (t = 1; t < n; t++) {
                const u8 next = (u8) (((int) squad_hover_color[d] + dir * t + n * t) % n);
                if (!squad_clash(d, ck, next)) {
                    squad_hover_color[d] = next;
                    sfxMove();
                    return;
                }
            }
            lbAudioAx_80024030(3);
            return;
        }
        if (!squad_held_new[d]) {
            row = squad_held[d];
        } else if (squad_last[d] >= 0) {
            row = squad_last[d];
        }
        if (row < 0) {
            mnCharSel_CostumeChange(d, trig & (HSD_PAD_X | HSD_PAD_Y)); /* the coin's colour, skins too */
        } else if (squad_cycle_color(d, row, dir)) {
            sfxMove();
        } else {
            lbAudioAx_80024030(3);
        }
    }
}

/* A on a numbered token that is down, closer than any coin the hand could take (the game's own
 * pickup test, mnCharSel_CursorThink): the hand takes it. */
static bool squad_grab(int c, struct CSSCursorData* cursor)
{
    f32 best = 9.0f, coin_best = 9.0f;
    int bd = -1, bk = -1, q, k;
    if (cursor->x10 < 0.2f || cursor->x10 > 22.0f) return false;
    for (q = 0; q < mnCharSel_804D6CF5 && q < 4; q++) {
        const CSSDoor* door = &mnCharSel_803F0DFC.doors[q];
        const struct CSSCharModel* coin = mnCharSel_804A0BD0[q];
        if (coin != NULL && door->p_kind != 3 && door->sel_icon < CSS_NONE && (door->p_kind != 0 || c == q) &&
            coin->x5 == 0)
        {
            const f32 ddx = 3.8f + (cursor->xC - coin->x8), ddy = -2.6f + (cursor->x10 - coin->xC);
            if (ddx * ddx + ddy * ddy < coin_best) coin_best = ddx * ddx + ddy * ddy;
        }
    }
    for (q = 0; q < 4; q++) {
        const u8 pk = mnCharSel_803F0DFC.doors[q].p_kind;
        if (squad_hand[q] >= 0 || pk == 3 || (pk == 0 && c != q)) continue;
        for (k = 0; k < GMSQUAD_EXTRA; k++) {
            const SquadToken* tok = &squad_tok[q][k];
            f32 ddx, ddy;
            if (!squad_token_down(q, k)) continue;
            ddx = 3.8f + (cursor->xC - tok->x);
            ddy = -2.6f + (cursor->x10 - tok->y);
            if (ddx * ddx + ddy * ddy < best) {
                best = ddx * ddx + ddy * ddy;
                bd = q;
                bk = k;
            }
        }
    }
    if (bd < 0 || best >= coin_best) return false;
    squad_hand_token(bd, c, bk, false);
    lbAudioAx_800237A8(0xB7, 0x7F, 0x40);
    return true;
}

/* One hand's presses this frame, before the hands' own think reads them. */
static void squad_input(int c)
{
    struct CSSCursorData* cursor = mnCharSel_804A0BC0[c];
    u32 trig;
    int d;
    if (cursor == NULL || cursor->x5 == 3) return;
    trig = HSD_PadCopyStatus[c].trigger;
    for (d = 0; d < 4; d++) {
        if (squad_hand[d] == c) {
            squad_input_token(c, cursor, d, trig);
            return;
        }
    }
    if (trig & HSD_PAD_Z) {
        squad_z(c, cursor);
        return;
    }
    for (d = 0; d < 4; d++) {
        if (squad_pick[d] && squad_picker[d] == c && squad_hand[d] < 0 && squad_waiting(d, cursor)) {
            /* Just put a pick down, the next token not in hand yet: A buzzes as it would with the token
             * over the fighter the squad now has, B stops picking (not the game's B, which would take the
             * coin back). */
            if (trig & HSD_PAD_B) {
                HSD_PadCopyStatus[c].trigger &= ~HSD_PAD_B;
                squad_stop(d);
                sfxBack();
                return;
            }
            if (trig & HSD_PAD_A) {
                HSD_PadCopyStatus[c].trigger &= ~HSD_PAD_A;
                lbAudioAx_80024030(3);
                return;
            }
            break;
        }
    }
    if (cursor->x5 == 1) {
        if (cursor->x6 >= 4) return;
        d = cursor->x6;
        if ((trig & HSD_PAD_B) && squad_pick[d]) {
            squad_stop(d); /* and the game puts the coin back */
        } else if ((trig & HSD_PAD_A) && (squad_pick[d] || gmSquad_Count(d) > 0)) {
            /* The coin is fighter 1 of a squad: never on a fighter the squad has, nor an added one. */
            const int icon = squad_point_icon(cursor->xC + 2.7f, cursor->x10 - 2.0f);
            if (icon >= 0 && (icons[icon].char_kind >= 26 || squad_has(d, icons[icon].char_kind, -1))) {
                HSD_PadCopyStatus[c].trigger &= ~HSD_PAD_A;
                lbAudioAx_80024030(3);
            }
        }
        return;
    }
    if (trig & HSD_PAD_A) {
        if (squad_grab(c, cursor)) HSD_PadCopyStatus[c].trigger &= ~HSD_PAD_A;
        return;
    }
    if ((trig & (HSD_PAD_X | HSD_PAD_Y | HSD_PAD_L | HSD_PAD_R)) && squad_door_open(c) && squad_last[c] >= 0 &&
        squad_door_key[c] != 0 && squad_coin_pick(c) != GMSQUAD_NONE)
    {
        /* The door shows the token put down last: X / Y are its colour (L / R, the coin's skins,
         * do nothing here). */
        HSD_PadCopyStatus[c].trigger &= ~(HSD_PAD_X | HSD_PAD_Y | HSD_PAD_L | HSD_PAD_R);
        if (trig & (HSD_PAD_X | HSD_PAD_Y)) {
            if (squad_cycle_color(c, squad_last[c], (trig & HSD_PAD_X) ? 1 : -1)) {
                sfxMove();
            } else {
                lbAudioAx_80024030(3);
            }
        }
    }
}

/* Once a frame per open door, before the presses: the coin went down since last frame. On a squad
 * it is fighter 1: on a fighter the squad has, an added fighter or squads too big to load it comes
 * back to the hand with the buzz. Picking, the hand then takes the next token. A pick the coin takes
 * with no hand dropping it gets the same budget check. */
static void squad_coin_watch(int p)
{
    struct CSSCharModel* coin = mnCharSel_804A0BD0[p];
    const u8 was = squad_coin_holder[p];
    const u8 seen = squad_coin_seen[p];
    u8 ck;
    squad_coin_holder[p] = coin != NULL && coin->x5 > 0 ? coin->x5 : 0;
    ck = squad_coin_pick(p);
    squad_coin_seen[p] = squad_coin_key(p);
    if (was != 0 && squad_coin_holder[p] == 0) {
        const int hand = was - 1;
        if ((u8) squad_player(p)->ckind >= 26 && mnCharSel_803F0DFC.doors[p].sel_icon < CSS_NONE &&
            (gmSquad_Count(p) > 0 || squad_pick[p]))
        {
            lbAudioAx_80024030(3); /* an added fighter (the random coin can give one) */
            squad_regrab(p, hand);
            squad_coin_seen[p] = GMSQUAD_NONE;
            return;
        }
        if (ck == GMSQUAD_NONE) return; /* taken off the portraits: no pick */
        if (gmSquad_Count(p) > 0 && (squad_has(p, ck, -1) || !squad_fits(p, -1, GMSQUAD_NONE, 0))) {
            lbAudioAx_80024030(3);
            squad_regrab(p, hand);
            squad_coin_seen[p] = GMSQUAD_NONE;
            return;
        }
        squad_last[p] = -1;
        if (squad_pick[p]) {
            if (gmSquad_Count(p) < GMSQUAD_EXTRA && hand >= 0 && hand < 4 && coin != NULL) {
                squad_next_token(p, hand, coin->x8, coin->xC);
            } else if (gmSquad_Count(p) < GMSQUAD_EXTRA && hand >= 0 && hand < 4) {
                squad_picker[p] = (s8) hand;
                squad_hand_token(p, hand, gmSquad_Count(p), true);
            } else {
                squad_stop(p);
            }
        }
        return;
    }
    if (squad_coin_holder[p] == 0 && ck != GMSQUAD_NONE && squad_coin_seen[p] != seen && gmSquad_Any() &&
        !squad_fits(p, -1, GMSQUAD_NONE, 0))
    {
        const int hand = squad_free_hand(p);
        lbAudioAx_80024030(3);
        if (hand >= 0) {
            squad_regrab(p, hand);
            squad_coin_seen[p] = GMSQUAD_NONE;
        }
    }
}

/* A hand that holds a token and can't any more (unplugged, holding a coin, off the portraits) lets
 * go of it, and the door's picking ends. A token that was down, carried off the portraits, is taken
 * out of the squad (the later ones move up a number); otherwise it goes back where it was. */
static void squad_hold_check(void)
{
    int d;
    for (d = 0; d < 4; d++) {
        struct CSSCursorData* cursor;
        if (squad_hand[d] < 0) continue;
        cursor = mnCharSel_804A0BC0[(int) squad_hand[d]];
        if (cursor != NULL && cursor->x5 != 3 && cursor->x5 != 1 && !squad_in_grid(cursor) && !squad_held_new[d]) {
            squad_hand[d] = -1;
            squad_hover_icon[d] = -1;
            squad_remove_row(d, squad_held[d]);
            squad_stop(d);
            sfxBack();
        } else if (cursor == NULL || cursor->x5 == 3 || cursor->x5 == 1 || !squad_in_grid(cursor)) {
            squad_release(d);
            squad_stop(d);
        }
    }
}

/* "squad: P1 lineup 9:0,2:1,20:0" when a door's squad changes (kind:colour in fight order). */
static void squad_log_lineups(void)
{
    int p, i;
    for (p = 0; p < 4; p++) {
        GmSquadMember order[GMSQUAD_SIZE];
        u8 now[1 + 2 * GMSQUAD_SIZE];
        char text[80];
        int n, used;
        memset(now, 0, sizeof now);
        n = squad_door_open(p) && gmSquad_Count(p) > 0 ?
                gmSquad_Lineup(p, squad_coin_pick(p), squad_coin_color(p), order) : 0;
        now[0] = (u8) n;
        for (i = 0; i < n; i++) {
            now[1 + 2 * i] = order[i].ckind;
            now[2 + 2 * i] = order[i].color;
        }
        if (memcmp(now, squad_logged[p], sizeof now) == 0) continue;
        memcpy(squad_logged[p], now, sizeof now);
        used = snprintf(text, sizeof text, "squad: P%d lineup", p + 1);
        for (i = 0; i < n && used > 0 && used < (int) sizeof text; i++) {
            const int wrote = snprintf(text + used, sizeof text - (size_t) used, "%s%u:%u", i ? "," : " ",
                                       order[i].ckind, order[i].color);
            if (wrote < 0) break;
            used += wrote;
        }
        OSReport("%s%s\n", text, n == 0 ? " none" : "");
    }
}

/* ---- tests ------------------------------------------------------------------------------------ */

/* MELEE_TEST_SQUAD_HAND="F:P:target;..." (tests only): on this screen's frame F, port P's hand goes
 * to `target`, where a coin or token it carries lands, or where A takes one: c<kind> the middle of
 * that fighter's portrait, k<door> that door's coin, s<door><number> that door's token (s02: door 0's
 * "2"), t<door> that door's HMN / CPU button, r / l the right / left random corner. */
#define SQUAD_TEST_MAX 64
typedef struct SquadTestHand {
    u32 frame;
    u8 port, a, b;
    char kind;
} SquadTestHand;
static SquadTestHand squad_tests[SQUAD_TEST_MAX];
static int squad_test_count;

static const char* squad_test_int(const char* s, u32* out)
{
    u32 v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32) (*s - '0');
        ++s;
        ++digits;
    }
    *out = v;
    return digits != 0 ? s : NULL;
}

static void squad_test_load(void)
{
    const char* s = mu_test_getenv("MELEE_TEST_SQUAD_HAND");
    squad_test_count = 0;
    while (s != NULL && *s != '\0' && squad_test_count < SQUAD_TEST_MAX) {
        SquadTestHand t;
        u32 v;
        memset(&t, 0, sizeof t);
        if ((s = squad_test_int(s, &v)) == NULL || *s != ':') break;
        t.frame = v;
        if ((s = squad_test_int(s + 1, &v)) == NULL || *s != ':' || v < 1 || v > 4) break;
        t.port = (u8) (v - 1);
        t.kind = *++s;
        ++s;
        if (t.kind == 's') {
            if (s[0] < '0' || s[0] > '3' || s[1] < '2' || s[1] > '4') break;
            t.a = (u8) (s[0] - '0');
            t.b = (u8) (s[1] - '0');
            s += 2;
        } else if (t.kind == 'c' || t.kind == 'k' || t.kind == 't') {
            if ((s = squad_test_int(s, &v)) == NULL) break;
            t.a = (u8) v;
        } else if (t.kind != 'r' && t.kind != 'l') {
            break;
        }
        squad_tests[squad_test_count++] = t;
        if (*s == ';') {
            ++s;
        } else if (*s != '\0') {
            break;
        }
    }
    if (squad_test_count != 0) OSReport("squad-test: %d hand moves\n", squad_test_count);
}

static void squad_test_hands(void)
{
    int i;
    for (i = 0; i < squad_test_count; i++) {
        const SquadTestHand* t = &squad_tests[i];
        struct CSSCursorData* cursor = mnCharSel_804A0BC0[t->port];
        f32 x, y;
        if (t->frame != squad_frame || cursor == NULL) continue;
        if (t->kind == 'c') {
            const int icon = squad_icon_of(t->a);
            if (icon < 0) continue;
            x = (icons[icon].bound_l + icons[icon].bound_r) * 0.5f - 2.7f;
            y = (icons[icon].bound_u + icons[icon].bound_d) * 0.5f + 2.0f;
        } else if (t->kind == 'k') {
            const struct CSSCharModel* coin = t->a < 4 ? mnCharSel_804A0BD0[t->a] : NULL;
            if (coin == NULL) continue;
            x = coin->x8 - 3.8f;
            y = coin->xC + 2.6f;
        } else if (t->kind == 's') {
            const SquadToken* tok = &squad_tok[t->a][t->b - 2];
            x = tok->x - 3.8f;
            y = tok->y + 2.6f;
        } else if (t->kind == 't') {
            if (t->a >= 4) continue;
            x = (mnCharSel_803F0DFC.doors[t->a].togglebtn_left + mnCharSel_803F0DFC.doors[t->a].togglebtn_right) *
                0.5f;
            y = -2.2f;
        } else {
            x = (t->kind == 'r' ? 27.0f : -27.0f) - 2.7f;
            y = 2.5f + 2.0f;
        }
        cursor->xC = x;
        cursor->x10 = y;
        OSReport("squad-test: frame %u P%d %c %d %d -> (%.2f, %.2f)\n", squad_frame, t->port + 1, t->kind, (int) t->a,
                 (int) t->b, x, y);
    }
}

/* The banner's words as the game's own glyphs, in a stream the text owns (its destructor frees it). The
 * stream has no codes of its own, as the game's stock-mode entry starts straight with its digits: the
 * text object's alignment, size and fitting draw it (mu_slippi_menu_description wants a prefix to copy,
 * and finds none in that entry). */
static void squad_banner_words(HSD_Text* text, const char* words)
{
    u8 encoded[256], stream[256];
    SisBuffer* alloc;
    int length, n = 0, i, k;
    length = HSD_SisLib_803A67EC(encoded, (u8*) words);
    for (i = 0; i < length && i < (int) sizeof encoded;) {
        const unsigned op = encoded[i];
        const int size = op >= 0x20 ? 2 : mu_menu_sis_command_size(op);
        if ((op >= 0x20 || op == 0x1a) && n + size < (int) sizeof stream) {
            for (k = 0; k < size; k++) stream[n++] = encoded[i + k];   /* glyphs and spaces only */
        }
        i += size;
    }
    stream[n] = 0;
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (alloc == NULL) return;
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (alloc->data == NULL) {
        HSD_SisLib_Free(alloc);
        return;
    }
    for (k = 0; k <= n; k++) alloc->data[k] = stream[k];
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data != NULL) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}

/* The banner over the portraits reads "Squad Strike" while a door picks or has a squad, in the banner's
 * own text object and lettering. Otherwise it shows the catalog entry again, which the game composed
 * from the rules (mnCharSel_8025BD30), untouched. */
static void squad_banner_update(void)
{
    bool want = false;
    int p;
    if (squad_banner == NULL) return;
    if (squad_live() && !mu_slippi_css_online()) {   /* online the banner stays Slippi's */
        for (p = 0; p < 4; p++) {
            want |= squad_door_open(p) && (squad_pick[p] || gmSquad_Count(p) > 0);
        }
    }
    if (want == squad_banner_ours) return;
    if (want) {
        squad_banner_words(squad_banner, "Squad Strike");
        squad_banner_ours = squad_banner->alloc_data != NULL;
        return;
    }
    if (squad_banner->alloc_data != NULL) {
        HSD_SisLib_Free(squad_banner->alloc_data->data);
        HSD_SisLib_Free(squad_banner->alloc_data);
        squad_banner->alloc_data = NULL;
    }
    HSD_SisLib_803A6368(squad_banner, 0x4A);
    squad_banner_ours = false;
}

/* ---- the select screen's hooks ---------------------------------------------------------------- */

/* The scene made its banner (catalog entry 0x4A, after SquadReset). */
static void mnCharSel_SquadBanner(HSD_Text* text)
{
    squad_banner = text;
    squad_banner_ours = false;
}

static void mnCharSel_SquadReset(int canvas)
{
    int d, k;
    (void) canvas;
    gmSquad_Disable();
    gmSquad_DirectVisit();   /* the online select asks the host about squads at once */
    squad_visit_ready = true;
    squad_frame = 0;
    squad_in_show = false;
    squad_banner = NULL;
    squad_banner_ours = false;
    memset(squad_front_on, 0, sizeof squad_front_on);   /* the scene's coins and tokens are new */
    for (d = 0; d < 4; d++) {
        squad_pick[d] = false;
        squad_picker[d] = -1;
        squad_wait[d] = false;
        squad_hand[d] = -1;
        squad_held[d] = 0;
        squad_held_new[d] = false;
        squad_hover_icon[d] = -1;
        squad_last[d] = -1;
        squad_door_key[d] = 0;
        squad_coin_holder[d] = 0;
        squad_coin_seen[d] = GMSQUAD_NONE;
        squad_coin_digit[d] = false;
        memset(squad_logged[d], 0, sizeof squad_logged[d]);
        for (k = 0; k < GMSQUAD_EXTRA; k++) {
            squad_tok[d][k].gobj = NULL; /* freed with the last screen; where they were stays */
        }
    }
    squad_test_load();
}

/* Runs at the top of mnCharSel_Scene_OnFrame, before the hands' and coins' thinks and before the
 * game hands this frame's picks to the preloader. */
static void mnCharSel_SquadFrame(void)
{
    int p, i;
    bool frozen;
    if (mnCharSel_804D6CB0 == NULL || !squad_visit_ready) {
        return;
    }
    ++squad_frame;
    /* The online select: this player's lineup as it stood after the last frame, to the host on change
     * (Direct; squads hidden or not, the host uses it only when both players have Squad Strike on). */
    if (mu_slippi_css_online()) gmSquad_DirectLineup(squad_coin_pick(0), squad_coin_color(0));
    if (!squad_screen()) {
        squad_stop_all();
        for (p = 0; p < 4; p++) {
            squad_door_update(p);   /* a door that showed a squad fighter shows its coin's again */
        }
        squad_banner_update();
        return;
    }
    if (!squad_rules()) {
        /* Teams, a timed match or Vanilla: no squads. */
        squad_stop_all();
        gmSquad_ClearAll();
        for (p = 0; p < 4; p++) {
            squad_door_update(p);
        }
        lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = gmSquad_Any() ? gmSquad_Key() : 0;
        squad_banner_update();
        return;
    }
    /* Online, locked in (or chatting): the lineup stays as it is, a token in hand goes back. */
    frozen = squad_online() && mu_slippi_css_block_unselect();
    if (frozen) squad_stop_all();
    squad_test_hands();
    for (p = 0; p < 4; p++) {
        if (!squad_door_open(p)) {
            squad_release(p);
            squad_stop(p);
            gmSquad_Clear(p); /* a closed door has no squad */
            squad_coin_seen[p] = GMSQUAD_NONE; /* opening on a coin is a new pick */
            squad_coin_holder[p] = 0;
            continue;
        }
        gmSquad_coin_pos[p] = 0; /* the coin is always fighter 1 */
        if (mnCharSel_804D6CF6 == 0) squad_coin_watch(p);
    }
    if (mnCharSel_804D6CF6 == 0 && !frozen) {
        squad_hold_check();
        for (p = 0; p < 4; p++) {
            squad_hover(p, true);
        }
        for (p = 0; p < 4; p++) {
            squad_input(p);
        }
    }
    if (squad_players() > 2) {
        bool any = gmSquad_Any();
        for (p = 0; p < 4; p++) {
            any |= squad_pick[p] || squad_hand[p] >= 0;
        }
        if (any) {
            /* A third player joined: squads are for 1v1s. */
            squad_stop_all();
            gmSquad_ClearAll();
            lbAudioAx_80024030(3);
            OSReport("squad: more than two players, squads cleared\n");
        }
    }
    /* No two fighters share a fighter and colour: when a coin pick or another member moves onto a
     * member's, that member's colour moves on. */
    for (p = 0; p < 4; p++) {
        for (i = 0; i < GMSQUAD_EXTRA; i++) {
            squad_fix_color(p, i, 0);
        }
    }
    for (p = 0; p < 4; p++) {
        if (squad_last[p] >= gmSquad_Count(p)) squad_last[p] = -1;
        if (squad_pick[p] && squad_hand[p] < 0 && !squad_coin_held(p)) {
            /* Picking with the coin down and no token in hand (Z off the portraits, or a pick just put
             * down): the picker's hand takes the next token once it is over them, as the game gives a
             * hand its coin, and has left the pick it put down (squad_waiting). */
            struct CSSCursorData* cursor =
                squad_picker[p] >= 0 && squad_picker[p] < 4 ? mnCharSel_804A0BC0[(int) squad_picker[p]] : NULL;
            bool busy = false;
            for (i = 0; i < 4; i++) {
                busy |= squad_hand[i] == squad_picker[p];
            }
            if (squad_coin_pick(p) == GMSQUAD_NONE || gmSquad_Count(p) >= GMSQUAD_EXTRA || cursor == NULL ||
                cursor->x5 == 1 || cursor->x5 == 3)
            {
                squad_stop(p);
            } else if (!busy && !frozen && mnCharSel_804D6CF6 == 0 && squad_in_grid(cursor) && !squad_waiting(p, cursor)) {
                squad_hand_token(p, squad_picker[p], gmSquad_Count(p), true);
                lbAudioAx_800237A8(0xB7, 0x7F, 0x40);
            }
        }
        if (squad_door_open(p) && (squad_pick[p] || gmSquad_Count(p) > 0)) squad_tokens_make(p);
        squad_door_update(p);
    }
    lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = gmSquad_Any() ? gmSquad_Key() : 0;
    squad_log_lineups();
    squad_banner_update();
}

/* A deliberate return to the main menu leaves normal VS rules behind. Name/rules modals and travel to
 * the stage select keep the squads for this match. */
static void mnCharSel_SquadExit(void)
{
    squad_visit_ready = false; /* the screen's objects go with it */
    squad_banner = NULL;       /* the banner too */
    squad_stop_all();
    if (mnCharSel_804D6CF6 == CSSPendingSceneChange_2) {
        gmSquad_ClearAll();
        gmSquad_Disable();
        lbDvd_GetPreloadCacheScene()->game_cache.mu_squad_key = 0;
    }
}

/* Before a hand draws (mnCharSel_CursorThink): the hand picking a squad flashes as over a button. */
static void mnCharSel_SquadHand(struct CSSCursorData* cursor)
{
    int d;
    if (!squad_visit_ready || cursor == NULL) return;
    for (d = 0; d < 4; d++) {
        if (squad_pick[d] && squad_picker[d] == cursor->x4) {
            cursor->x8 = 1;
            return;
        }
    }
}

/* The hand of this port holds a token: it draws as holding (updateCursorDisplay). */
static bool mnCharSel_SquadHolding(int port)
{
    int d;
    if (!squad_visit_ready) return false;
    for (d = 0; d < 4; d++) {
        if (squad_hand[d] == port) return true;
    }
    return false;
}

/* For the ready banner: the hand holds a squad token, or just put one down and takes the next once it
 * moves on (squad_waiting). Hidden for both, as for a hand holding a coin. */
static bool mnCharSel_SquadHandBusy(int port)
{
    int d;
    if (mnCharSel_SquadHolding(port)) return true;
    if (!squad_visit_ready || port < 0 || port >= 4) return false;
    for (d = 0; d < 4; d++) {
        if (squad_pick[d] && squad_picker[d] == port && squad_hand[d] < 0 &&
            squad_waiting(d, mnCharSel_804A0BC0[port]))
        {
            return true;
        }
    }
    return false;
}

/* After a coin's think (fn_80262648): a squad's coin reads "1"; afterwards its port label again. */
static void mnCharSel_SquadCoin(struct CSSCharModel* model, HSD_JObj* jobj)
{
    const int d = model->x4;
    bool front = false;
    if (d >= 4) return;
    if (squad_live() && squad_door_open(d) && (squad_pick[d] || gmSquad_Count(d) > 0) && squad_digits >= 0) {
        squad_label(jobj, 0);
        squad_coin_digit[d] = squad_digits > 0;
        front = squad_visit_ready && squad_just_down(d, -1) && !squad_coin_held(d);   /* just put down */
    } else if (squad_coin_digit[d]) {
        squad_coin_digit[d] = false;
        animateCharModel(jobj, model->x6 < 4 ? (f32) (d * 4) : 16.0f);
    }
    squad_draw_front(d, -1, model->gobj, jobj, model->x10, model->x14, front);
}

/* After mnCharSel_8025DB34 drew a door: if the door showed a squad fighter, the game drew its own over
 * it; the squad's goes back up (or the door's skin picture, which the squad's kept off). */
static void mnCharSel_SquadDoor(int door)
{
    if (!squad_visit_ready || squad_in_show || door < 0 || door >= 4 || squad_door_key[door] == 0) return;
    squad_door_key[door] = 0;
    squad_door_update(door);
#ifdef MU_NATIVE
    if (squad_door_key[door] == 0) mu_css_door_portrait(door);
#endif
}

/* The door shows a squad fighter: its coin's skin picture stays off (mu_css_door_portrait). */
static bool mnCharSel_SquadDoorBusy(int door)
{
    return squad_visit_ready && !squad_in_show && door >= 0 && door < 4 && squad_door_key[door] != 0;
}

/* The live pick preview (shim/mu_slippi_css.c, display only): the online door's coin pick
 * (GMSQUAD_NONE while carried) and its colour, and the lineup as it stands. Returns how many
 * members out holds (0-4). */
int gmSquad_CssPreview(u8* coin_ck, u8* coin_color, GmSquadMember out[GMSQUAD_SIZE])
{
    *coin_ck = squad_coin_pick(0);
    *coin_color = squad_coin_color(0);
    return gmSquad_Lineup(0, *coin_ck, *coin_color, out);
}

/* For the css-state line (shim/mu_css_state.c): how many fighters the online select's squad has (the
 * coin pick included), or -1 when that select does not pick squads (not Direct, or the row is off). */
int gmSquad_CssPickCount(void)
{
    GmSquadMember out[GMSQUAD_SIZE];
    u8 ck, color;
    if (!squad_visit_ready || !squad_online()) return -1;
    return gmSquad_CssPreview(&ck, &color, out);
}

#endif
