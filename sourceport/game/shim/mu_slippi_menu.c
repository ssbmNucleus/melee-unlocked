/* Native Slippi online menus: the core.
 *
 * Legacy gets the online menus from Slippi's codes in GALE01r2.ini (the Online/Menus, Online/Slippi
 * Online Scene and Online/Static folders). This file is their native form for the parts that are not
 * screen code: the state Slippi parks in r13 slots and code caves, the host-command wrappers and
 * codecs, the boot and main-menu entry, the online submenu, and the online major (GM_HANYU_CSS
 * turned into CSS / SSS / VS / Results / Splash). Each function names the code it stands for.
 * The public contract is run-source/s6-core-api.md.
 *
 * Nothing here runs unless the host set the Slippi-menus game option; the predicates answer 0
 * and the hook sites keep their original code.
 */
#include <string.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1B03.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmresult.h>
#include <melee/gm/gmscdata.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/gmvsmode.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/inlines.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbvector.h>
#include <mu_test_env.h>
#include <mu_native.h>
#include <mu_pick_preview.h>
#if MU_SQUAD
/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
#include "mu_squad.h"
#endif
#include <mu_ranked.h>
#include <mu_ranked_setup_online.h>
#include <mu_lobby_list.h>
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
void mu_online_abi_log(const char* text);
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
void mu_replay_apply_game_info(StartMeleeData* data, const unsigned char* info);
int mu_match_block_problem(const unsigned char* info, char* why, unsigned int size);
extern MenuKindData mn_803EB6B0[];

enum {
    REPLY_CAPACITY = 4096,   /* the bridge refuses a smaller reply buffer */
    MSRB_SIZE = 1030,        /* Slippi 3.7.0 (Online.s MSRB_SIZE): ranked info and bot flags appended */
    MSRB_MIN_SIZE = 962,     /* 3.6.4's, what the peer-to-peer host answers: the rest reads as zero */
    OSB_SIZE = 42,
    FIGHTER_NUM = 33,        /* GetFighterNum: "no fighter" */
    ICON_ZELDA = 0x12,
    ICON_SHEIK = 0x13,
};

/* ---- flag ---- */

static int menus_flag = -1;

int mu_slippi_menus_enabled(void)
{
    if (menus_flag < 0) {
        menus_flag = (mu_game_options() & MU_OPTION_SLIPPI_MENUS) != 0;
    }
    return menus_flag;
}
/* A Doubles console session boots from before main, where the host's options are all zero (the
 * canonical menus, the same on all four), so the latch reads 0 there and kept the online menus off
 * after the session: its results came back to VS. Mode. Each main-menu entry outside a session
 * takes the host's menus back. This only ever turns them on, never off. */
static void menus_relatch(void)
{
    if (menus_flag == 0 && (mu_game_options() & MU_OPTION_SLIPPI_MENUS) != 0) {
        menus_flag = 1;
        OSReport("online menu: the host's online menus are back after a console session\n");
    }
}

/* ---- state (Slippi's r13 slots and cave bytes) ---- */

static MuSlippiMenuState slp;
static MuMatchState last_match;
static unsigned char user_name[32];
static unsigned char user_code[11];
static unsigned char reply[REPLY_CAPACITY];
static int installed;         /* the online major replaced GM_HANYU_CSS (main.asm) */
static int zelda_sheik;       /* between the online major's OnLoad and OnUnload */
static unsigned online_loads;  /* OnLoad count: the chat shim's Sheik selector keeps its icon until the next one */
static int boot_oneshot;      /* boot.asm's one-shot "first menu = online submenu" */
static int first_boot = 1;    /* OnMenuLoad's DOFST_IS_FIRST_BOOT */
/* A peer-to-peer session with menus (mu_online_p2p_menus): the first online submenu confirms Direct
 * by itself. Set in the boot menu entry and cleared by that menu's think a few frames later, both
 * before any match exists, so every rollback snapshot holds 0 and a load cannot arm it again. */
static int p2p_auto_direct;
MuSlippiMenuState* mu_slippi_state(void)
{
    return &slp;
}

/* ---- host commands (FN_EXITransferBuffer) ---- */

int mu_slippi_cmd(unsigned int cmd, const void* payload, unsigned int size, void* out,
                  unsigned int out_cap, unsigned int* out_size)
{
    unsigned int got = 0;
    int result = mu_online_abi_command(cmd, (const unsigned char*) payload, size, reply,
                                       REPLY_CAPACITY, &got);
    if (result != 0) {
        got = 0;
    }
    if (out != NULL) {
        unsigned int n = got < out_cap ? got : out_cap;
        memcpy(out, reply, n);
    }
    if (out_size != NULL) {
        *out_size = got;
    }
    return result;
}

static u32 be32(const u8* p) { return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3]; }
static void put32(u8* p, u32 v) { p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v; }

static unsigned char network_policy;
/* UserDisplayFunctions FetchSlippiAppState: B9, 42 bytes {app state, name[31], code[10]}; the
 * app state goes to r13-0x505F. */
int mu_slippi_fetch_app_state(void)
{
    u8 osb[OSB_SIZE];
    memset(osb, 0, sizeof osb);
    mu_slippi_cmd(MU_CMD_GET_ONLINE_STATUS, NULL, 0, osb, sizeof osb, NULL);
    slp.app_state = osb[0];
    memcpy(user_name, osb + 1, 31);
    user_name[31] = 0;
    memcpy(user_code, osb + 32, 10);
    user_code[10] = 0;
    network_policy = 0;
    mu_slippi_cmd(MU_CMD_NATIVE_MENU_FLAGS, NULL, 0, &network_policy, sizeof network_policy, NULL);
    return slp.app_state;
}

int mu_slippi_ranked_available(void) { return slp.app_state == MU_SLP_APP_LOGGED_IN; }
int mu_slippi_menu_count(void) { return 8; }
int mu_slippi_app_state(void) { return slp.app_state; }
const char* mu_slippi_user_name(void) { return (const char*) user_name; }
const char* mu_slippi_user_code(void) { return (const char*) user_code; }

/* LoadMatchState: B3 into a Match State Response Buffer. */
static void parse_match_state(MuMatchState* m, const u8* r)
{
    int i;
    memcpy(m->raw, r, MSRB_SIZE);
    m->connection_state = r[0];
    m->local_ready = r[1];
    m->remote_ready = r[2];
    m->local_index = r[3];
    m->remote_index = r[4];
    m->rng_offset = be32(r + 5);
    m->delay_frames = r[9];
    m->user_chat_msg = r[10];
    m->opp_chat_msg = r[11];
    m->chat_player_index = r[12];
    m->user_rank = r[13];
    m->opp_rank = r[14];
    memcpy(m->local_name, r + 15, 31);
    for (i = 0; i < 4; i++) {
        memcpy(m->names[i], r + 46 + 31 * i, 31);
        memcpy(m->codes[i], r + 201 + 10 * i, 10);
        memcpy(m->uids[i], r + 241 + 29 * i, 29);
    }
    memcpy(m->opp_name, r + 170, 31);
    memcpy(m->error, r + 357, 241);
    memcpy(m->game_info, r + 598, 312);
    memcpy(m->match_id, r + 910, 51);
    m->alt_stage_mode = r[961];
}

/* ED read beside every B3: the opponents' live picks (display only, never match setup). A host
 * without the command answers nothing and every slot stays invalid. */
static void load_pick_previews(MuMatchState* m)
{
    u8 op = MU_PICK_PREVIEW_OP_READ;
    u8 r[MU_PICK_PREVIEW_REPLY];
    MuPickPreviewSlot slots[MU_PICK_PREVIEW_REMOTES];
    unsigned int got = 0;
    int i;
    memset(m->opp_picks, 0, sizeof m->opp_picks);
    if (mu_slippi_cmd(MU_PICK_PREVIEW_COMMAND, &op, 1, r, sizeof r, &got) != 0 ||
        !mu_pick_preview_reply_decode(r, got, slots)) {
        return;
    }
    for (i = 0; i < MU_PICK_PREVIEW_REMOTES; i++) {
        MuOpponentPick* o = &m->opp_picks[i];
        const MuPickPreview* p = &slots[i].pick;
        if (!slots[i].valid) {
            continue;
        }
        o->valid = 1;
        o->port = slots[i].port;
        o->char_id = p->char_id;
        o->color = p->color;
        o->team_id = p->team_id;
        o->squad_count = p->squad_count;
        memcpy(o->kinds, p->kinds, sizeof o->kinds);
        memcpy(o->colors, p->colors, sizeof o->colors);
        o->locked = p->locked;
    }
}

/* ED send: the host keeps it only while connected and sends it on change, rate limited. */
void mu_pick_preview_send(unsigned char char_id, unsigned char color, unsigned char team_id,
                          unsigned char squad_count, const unsigned char kinds[3],
                          const unsigned char colors[3], unsigned char locked)
{
    MuPickPreview p;
    u8 b[1 + MU_PICK_PREVIEW_WIRE];
    unsigned int i;
    mu_pick_preview_clear(&p);
    p.char_id = char_id;
    p.color = color;
    p.team_id = team_id;
    p.squad_count = squad_count <= MU_PICK_PREVIEW_SLOTS && kinds != NULL && colors != NULL ? squad_count : 0;
    for (i = 0; i < p.squad_count; i++) {
        p.kinds[i] = kinds[i];
        p.colors[i] = colors[i];
    }
    p.locked = locked != 0;
    b[0] = MU_PICK_PREVIEW_OP_SEND;
    if (mu_pick_preview_encode(&p, b + 1, sizeof b - 1) != MU_PICK_PREVIEW_WIRE) {
        return;
    }
    mu_slippi_cmd(MU_PICK_PREVIEW_COMMAND, b, sizeof b, NULL, 0, NULL);
}

int mu_slippi_load_match_state(MuMatchState* out)
{
    u8 msrb[MSRB_SIZE];
    unsigned int got = 0;
    int result;
    memset(msrb, 0, sizeof msrb);
    result = mu_slippi_cmd(MU_CMD_GET_MATCH_STATE, NULL, 0, msrb, sizeof msrb, &got);
    parse_match_state(&last_match, msrb);
    load_pick_previews(&last_match);
    if (out != NULL && out != &last_match) {
        *out = last_match;
    }
    return result != 0 || got < MSRB_MIN_SIZE ? -1 : 0;
}

const MuMatchState* mu_slippi_match_state(void)
{
    return &last_match;
}

/* Find Match Transfer Buffer: mode, 18 Shift-JIS bytes. FN_TX_FIND_MATCH first resets the game
 * prep data's game and tiebreak counters. */
void mu_slippi_find_opponent(int mode, const unsigned char* code_sjis18)
{
    u8 b[19];
    mu_online_reset_game_index();
    memset(b, 0, sizeof b);
    b[0] = (u8) mode;
    if (code_sjis18 != NULL) {
        memcpy(b + 1, code_sjis18, 18);
    }
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    if (mode == MU_SLP_MODE_UNRANKED && gmSquad_OnlineChosen() &&
        (!gmSquad_OnlineBudget() || !gmSquad_OnlineSearchReady())) return;
#endif
    mu_slippi_cmd(MU_CMD_FIND_OPPONENT, b, sizeof b, NULL, 0, NULL);
}

/* Player Selections Transfer Buffer. */
void mu_slippi_set_selections(const MuSlippiSelections* s)
{
    u8 b[9];
    b[0] = s->team_id;
    b[1] = s->char_id;
    b[2] = s->char_color;
    b[3] = s->char_opt;
    b[4] = (u8) (s->stage_id >> 8);
    b[5] = (u8) s->stage_id;
    b[6] = s->stage_opt;
    b[7] = s->online_mode;
    b[8] = s->alt_stage_mode;
    mu_slippi_cmd(MU_CMD_SET_MATCH_SELECTIONS, b, sizeof b, NULL, 0, NULL);
}

void mu_slippi_cleanup_connections(void)
{
    mu_slippi_cmd(MU_CMD_CLEANUP_CONNECTION, NULL, 0, NULL, 0, NULL);
}

unsigned int mu_slippi_new_seed(void)
{
    u8 b[4] = { 0, 0, 0, 0 };
    mu_slippi_cmd(MU_CMD_GET_NEW_SEED, NULL, 0, b, sizeof b, NULL);
    return be32(b);
}

/* The bridge expects two payload bytes for BB. */
/* The Slippi Online mode picked on the menu (0-4; 2 = Direct): Melee Maker exchanges builds only in
 * Direct (maker/platform/online_sync.c). */
int mu_slippi_selected_mode(void)
{
    return slp.mode;
}

void mu_slippi_send_chat(int message_id)
{
    u8 b[2];
    b[0] = (u8) message_id;
    b[1] = 0;
    mu_slippi_cmd(MU_CMD_SEND_CHAT_MESSAGE, b, sizeof b, NULL, 0, NULL);
}

/* AutoComplete FetchSuggestion: 8x3 input, length, index u32, scroll, mode; the reply is found,
 * 8x3 suggestion, length, index u32. */
int mu_slippi_code_suggestion(const MuCodeSuggestionReq* in, MuCodeSuggestion* out)
{
    u8 b[31], r[30];
    int result;
    memcpy(b, in->input, 24);
    b[24] = in->length;
    put32(b + 25, in->index);
    b[29] = in->scroll;
    b[30] = in->mode;
    memset(r, 0, sizeof r);
    result = mu_slippi_cmd(MU_CMD_FETCH_CODE_SUGGESTION, b, sizeof b, r, sizeof r, NULL);
    if (out != NULL) {
        out->found = r[0];
        memcpy(out->text, r + 1, 24);
        out->length = r[25];
        out->index = be32(r + 26);
    }
    return result;
}

/* ---- scene predicates ---- */

int mu_slippi_in_online_mode(void)
{
    return mu_slippi_menus_enabled() && installed && gm_GetCurrentGameMode() == GM_HANYU_CSS;
}

int mu_slippi_online_state(void)
{
    return mu_slippi_in_online_mode() ? gm_GetCurrentSceneIndex() : -1;
}

int mu_slippi_on_online_css(void) { return mu_slippi_online_state() == MU_SLP_STATE_CSS; }
int mu_slippi_on_online_sss(void) { return mu_slippi_online_state() == MU_SLP_STATE_SSS; }
int mu_slippi_in_online_game(void) { return mu_slippi_online_state() == MU_SLP_STATE_VS; }
int mu_slippi_on_online_results(void) { return mu_slippi_online_state() == MU_SLP_STATE_RESULTS; }
int mu_slippi_on_online_splash(void) { return mu_slippi_online_state() == MU_SLP_STATE_SPLASH; }
int mu_slippi_zelda_is_sheik(void)
{
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
     * Squads keep the picker's exact Zelda/Sheik kinds, also once a friend room turns them on. */
    if (gmSquad_OnlineChosen()) return 0;
#endif
    return mu_slippi_menus_enabled() && zelda_sheik;
}
unsigned mu_slippi_online_load_count(void) { return online_loads; }

/* IncreaseTextHeap: the online CSS gets a 0x4800-byte text heap instead of 0x2400. */
int mu_slippi_css_text_heap(void)
{
    return mu_slippi_on_online_css() ? 0x4800 : 0;
}

/* ---- helpers (Online/Static) ---- */

/* GetTeamCostumeIndex. */
unsigned char mu_slippi_team_costume(int team, int ckind)
{
    switch (team) {
    case 1:
        return gm_80169264((u8) ckind);
    case 3:
        return gm_80169290((u8) ckind);
    default:
        return gm_801692BC((u8) ckind);
    }
}

int mu_slippi_option_unlocked(int sel)
{
    if (sel < 0 || sel >= mu_slippi_menu_count()) return 0;
    if (network_policy & 8) {
        if (sel == MU_SLP_OPT_LOGIN || sel == MU_SLP_OPT_UPDATE) return 0;
        if (!(network_policy & 16) && sel >= MU_SLP_MODE_RANKED && sel <= MU_SLP_MODE_PARTY) return 0;
#ifndef NUCLEUS_PUBLIC_BUILD
        /* Tests: an agent copy never logs in, so with local peering (16) Direct, the mode local peering
         * plays, opens without a sign-in: an automated pair reaches the online select through the menus.
         * Not in a public build (docs/client-release.md), whose host takes no --local-peer either. */
        if ((network_policy & 16) && slp.app_state != MU_SLP_APP_LOGGED_IN) return sel == MU_SLP_MODE_DIRECT;
#endif
    }
    if (slp.app_state == MU_SLP_APP_UPDATE) return sel == MU_SLP_OPT_UPDATE;
    if (slp.app_state != MU_SLP_APP_LOGGED_IN) return sel == MU_SLP_OPT_LOGIN;
    return sel >= MU_SLP_MODE_RANKED && sel <= MU_SLP_MODE_PARTY;
}

int mu_slippi_first_unlocked(void)
{
    int i;
    int count = mn_803EB6B0[MENU_KIND_8].selection_count;
    for (i = MU_SLP_MODE_RANKED; i < count; i++) {
        if (mn_80229938(MENU_KIND_8, i)) {
            return i;
        }
    }
    /* An agent copy or a logged-out native client can lock every row, including
     * Log in. Keep a real row for the renderer; Confirm still checks its lock. */
    OSReport("online menu: all rows locked (app state %d), keeping row %d\n",
             slp.app_state, MU_SLP_MODE_RANKED);
    return MU_SLP_MODE_RANKED;
}

/* ---- boot (boot.asm) and the main menu (Online/Menus/TitleMenu) ---- */

/* boot.asm: boot leaves straight to the main menu (no title, no opening movie), and the first
 * menu entry opens the online submenu. The Stadium transformation preloads are for the EXI
 * loader and have no native counterpart (disc reads are synchronous). */
unsigned char mu_slippi_boot_mode(unsigned char mode)
{
    if (!mu_slippi_menus_enabled()) {
        return mode;
    }
    boot_oneshot = 1;
    return GM_MENU;
}

/* OnMenuPrep, every main-menu entry: the online submenu table, the 1P description of row 2, and
 * the app state (InitBuffers + FetchSlippiAppState). */
void mu_slippi_menu_prep(void)
{
    if (!mu_slippi_menus_enabled()) {
        return;
    }
    mu_mnmain_install_online_menu();
    mu_slippi_fetch_app_state();
}

/* The jump-table entries OnMenuPrep and boot.asm replace: previous mode 8 (OnReturnFromOnline)
 * and, once after boot, previous mode GM_BOOT (OnFirstMenuLoad). Returns 1 when it set the menu. */
int mu_slippi_menu_enter(int previous_mode, unsigned char* menu_kind, unsigned char* hovered)
{
    if (!mu_slippi_menus_enabled()) {
        return 0;
    }
    if (previous_mode == GM_HANYU_CSS) {
        mu_content_mode(-1);   /* back from online: the offline view again */
        if (mu_lobby_menu_return(menu_kind, hovered)) return 1;   /* to the Teams submenu it came from */
        *menu_kind = MENU_KIND_8;
        *hovered = mn_80229938(MENU_KIND_8, slp.mode) ? slp.mode : (u8) mu_slippi_first_unlocked();
        return 1;
    }
    if (previous_mode == GM_BOOT && boot_oneshot) {
        boot_oneshot = 0;
        mu_slippi_fetch_app_state();
        *menu_kind = MENU_KIND_8;
        *hovered = (u8) mu_slippi_first_unlocked();
        if (mu_online_p2p_menus()) {
            /* The session's players are already known: no choice of mode, straight to Direct's
             * character select. The row is only open when the host reports a signed-in state. */
            if (mn_80229938(MENU_KIND_8, MU_SLP_MODE_DIRECT)) {
                *hovered = MU_SLP_MODE_DIRECT;
                p2p_auto_direct = 1;
            } else {
                OSReport("[p2p] menus: Direct is locked (app state %d); staying on the online menu\n",
                         slp.app_state);
            }
        }
        return 1;
    }
    return 0;
}

/* OnMenuLoad, end of mnMain_Scene_OnEnter: the "Melee" voice on the first boot, then BA on every
 * entry. The title-screen user text is intentionally off in this build. */
void mu_slippi_main_menu_loaded(void)
{
    if (!mu_slippi_menus_enabled()) {
        return;
    }
    if (first_boot) {
        lbAudioAx_80026F2C(first_boot);
        lbAudioAx_8002702C(2, 8);
        lbAudioAx_80027168();
        lbAudioAx_80027648();
        lbAudioAx_800237A8(30005, 127, 64);
        first_boot = 0;
    }
    mu_slippi_cleanup_connections();
}

/* OnlineModeOptionSelected -> SwitchToOnlineMenu: open the online submenu from the 1P menu and
 * force the old menu to clear (AllowSwapToSameSubmenu). Runs from the 1P menu's think. */
void mu_slippi_switch_to_online_menu(void)
{
    mn_80229894(MENU_KIND_8, (u16) mu_slippi_first_unlocked(), 1);
    slp.force_clear = 1;
}

/* AllowSwapToSameSubmenu: the menu clears when the kinds differ or the force flag is set; either
 * way the flag is consumed. */
int mu_slippi_menu_clear_check(int menus_differ)
{
    if (!mu_slippi_menus_enabled()) {
        return menus_differ;
    }
    if (menus_differ || slp.force_clear) {
        slp.force_clear = 0;
        return 1;
    }
    return 0;
}

static u8 confirm_port(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (gm_GetButtonsTriggered((u8) i) & PAD_CONFIRM) {
            return (u8) i;
        }
    }
    return 0;
}

/* OnMenuPrep FN_OnlineSubmenuThink (ported from the regular-match think). The logout dialog is
 * unreachable: Log out is locked in every app state of this build. */
void mu_slippi_online_menu_think(HSD_GObj* gp)
{
    u32 buttons;
    MenuExitData* data;
    int auto_direct = 0;
    /* The menu takes no input while its entry cooldown runs (mn_80229624 answers 0 and counts it
     * down), so the frame it reaches 0 is the first one a player's own press could confirm on. */
    int accepts_input = mn_804D6BC8.cooldown == 0;
    (void) gp;

    buttons = mn_80229624(4);
    if (p2p_auto_direct && accepts_input) {
        /* Once per run: confirm Direct as a press on this frame would, whatever the pads hold. */
        p2p_auto_direct = 0;
        if (mu_online_p2p_menus() && mn_804A04F0.cur_menu == MENU_KIND_8 &&
            mn_80229938(MENU_KIND_8, MU_SLP_MODE_DIRECT))
        {
            auto_direct = 1;
            mn_804A04F0.hovered_selection = MU_SLP_MODE_DIRECT;
            buttons = MenuInput_Confirm;
            OSReport("[p2p] menus: entering the Direct character select (pad port %d)\n",
                     mu_online_p2p_port());
        }
    }
    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        if (!mu_slippi_option_unlocked(mn_804A04F0.hovered_selection)) return;
        mn_804D6BC8.cooldown = 5;
        mn_804A04F0.entering_menu = 1;
        gm_801677E8(auto_direct ? (s8) mu_online_p2p_port() : (s8) confirm_port());
        /* Teams opens its submenu of SSBM Nucleus's lobbies when that is on (shim/mu_lobby_list.c). */
        if (mn_804A04F0.hovered_selection == MU_SLP_MODE_TEAMS && !mu_online_p2p_menus() && mu_lobby_menu_enter()) {
            mn_804A04F0.entering_menu = 0;
            return;
        }
        switch (mn_804A04F0.hovered_selection) {
        case MU_SLP_MODE_RANKED:
        case MU_SLP_MODE_UNRANKED:
        case MU_SLP_MODE_DIRECT:
        case MU_SLP_MODE_TEAMS:
        case MU_SLP_MODE_PARTY:
            slp.mode = (u8) mn_804A04F0.hovered_selection;
            if (mu_online_p2p_menus()) {
                /* No game was played yet: neither winner nor loser. The character select gives the
                 * first game's stage pick to one player once the two are connected and their
                 * slots are known (mu_slippi_css.c). */
                slp.is_winner = MU_SLP_WINNER_NULL;
                slp.chose_stage = 0;
            }
            /* The files of the next scene follow the mode: retail online, the mod's in Direct when
             * the player uses it there (mu_content.c). Asked before the online major loads. */
            mu_content_mode(slp.mode);
            sfxForward();
            data = gm_GetCurrentSceneExitData();
            data->pending_mode = GM_HANYU_CSS;
            gm_801A4B60();
            break;
        case MU_SLP_OPT_LOGIN:
            sfxForward();
            mu_slippi_cmd(MU_CMD_OPEN_LOGIN, NULL, 0, NULL, 0, NULL);
            break;
        case MU_SLP_OPT_UPDATE:
            sfxForward();
            mu_slippi_cmd(MU_CMD_UPDATE_APP, NULL, 0, NULL, 0, NULL);
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_1P, SEL_1P_2, 3);
    } else if (buttons & MenuInput_Up) {
        sfxMove();
        mu_mn_move_selection(MENU_KIND_8, mu_slippi_menu_count(), -1);
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mu_mn_move_selection(MENU_KIND_8, mu_slippi_menu_count(), 1);
    }
}


/* ---- the online major (Slippi Online Scene/main.asm) ----
 * GM_HANYU_CSS (unused in retail) becomes the online mode: states 0 CSS, 1 SSS, 2 VS, 3 Results,
 * 4 Splash. Slippi writes the raw next-state byte (id + 1); gm_SetNextGameModeStateId(id) is the
 * same write. State 5 runs the Ranked set setup. */

static CSSData online_css_data;   /* Slippi reuses the event CSS buffer; the choice is saved in vs.unk_530 */

/* SplashSceneInit: the negotiated match into the VS mode data (AdjustNullCharID is a no-op). */
void mu_slippi_splash_init(void)
{
    char why[96], line[200];
    mu_slippi_load_match_state(NULL);
    /* A block with a value this game has no table for (mu_match_check.c) is not applied; the match
     * start refuses it as well (mu_online_start_melee). */
    if (mu_match_block_problem(last_match.game_info, why, sizeof why)) {
        snprintf(line, sizeof line, "online: match state refused (%s); it is not applied", why);
        mu_online_abi_log(line);
        return;
    }
    mu_replay_apply_game_info(&gmVsMelee_GetVsData()->start, last_match.game_info);
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    if (gmSquad_OnlineChosen()) gmSquad_OnlineSetup(&gmVsMelee_GetVsData()->start);
    else gmSquad_DirectSetup(&gmVsMelee_GetVsData()->start);   /* Squad Strike in Direct, when agreed */
#endif
}

#if MU_SQUAD
/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
 * The opponent left between lock-in and the first frame: the host has cancelled the match and
 * says why. Back to the CSS, which restores the lineup, instead of a splash or a game against
 * nobody. */
static int squad_opponent_gone(const char* where)
{
    if (!gmSquad_OnlineLive(slp.mode)) return 0;
    if (!mu_slippi_load_match_state(NULL) &&
        last_match.connection_state == MU_SLP_MM_CONNECTION_SUCCESS) return 0;
    OSReport("squad: opponent gone at the %s (connection %u), back to the CSS\n", where,
             (unsigned) last_match.connection_state);
    return 1;
}
#endif

/* MajorSceneLoad. */
static void online_on_load(void)
{
    mu_ranked_set_reset();
    gmMainLib_804D3EE0->vs.unk_530.x6 = gm_801677F0();   /* the 1P port for the event CSS slot */
    zelda_sheik = 1;                                       /* the Zelda icon picks Sheik */
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    if (gmSquad_OnlineChosen()) zelda_sheik = 0; /* keep the picker's exact Zelda/Sheik kinds */
#endif
    online_loads++;
}

/* MajorSceneUnload. */
static void online_on_unload(void)
{
    mu_ranked_update_reset(); // leaving the online major abandons any unfinished set
    mu_ranked_set_reset();
    mu_lobby_left_online();   // and a lobby hosted or joined is closed or left (shim/mu_lobby_list.c)
    zelda_sheik = 0;
}

/* CSSScenePrep: event-style CSS with the saved choice, then clear the preload cache. */
static void css_prep(GameModeState* state)
{
    struct EventData* ev = &gmMainLib_804D3EE0->vs.unk_530;
    CSSData* css = gm_GetGameModeStateEnterData(state);
    gm_801B06B0(css, 0xE, ev->x2, 0, ev->x3, ev->nametag, 0, ev->x6);
    lbDvd_SetupVsPreloadCache();
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    gmSquad_DirectReset(); /* the last Direct squad match's setup; this visit picks again */
    gmSquad_OnlineRoom(slp.mode); /* a choice this mode or friend room doesn't carry is dropped */
    if (gmSquad_OnlineChosen()) gmSquad_OnlineCss();
#endif
}

/* CSSSceneDecide (runs the event CSS decide first to save the choice). */
static void css_decide(GameModeState* state)
{
    struct EventData* ev = &gmMainLib_804D3EE0->vs.unk_530;
    CSSData* css = gm_GetGameModeStateExitData(state);

    /* Record the guest's exit request before the main-menu load sends BA.
     * A cleanup line after returning to the menu does not explain the CSS exit. */
    OSReport("slippi CSS: exit mode %u request %u connection %u\n",
             (unsigned) slp.mode, (unsigned) css->pending_scene_change,
             (unsigned) last_match.connection_state);
    /* gmevent.c onExitCss */
    if (css->pending_scene_change == 2) {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        return;
    }
    gm_801B0730(css, &ev->x2, NULL, &ev->x3, &ev->nametag, NULL);
    ev->x8 = -1;
    ev->x9 = -1;
    ev->xA = -1;

    switch (slp.mode) {
    case MU_SLP_MODE_RANKED:
        /* asm Online/Slippi Online Scene/main.asm:420-447. */
        mu_ranked_set_reset();
        gm_SetNextGameModeStateId(MU_SLP_STATE_GAME_SETUP);
        return;
    case MU_SLP_MODE_DIRECT:
    case MU_SLP_MODE_TEAMS:
        /* The first match and the last winner go to the splash; the loser picks the stage unless
         * it already did. Any other value stalls on the console. */
        OSReport("online css: is_winner %d, chose_stage %d\n", (int) slp.is_winner, (int) slp.chose_stage);
        if (slp.is_winner == MU_SLP_WINNER_LOST && slp.chose_stage == 0) {
            gm_SetNextGameModeStateId(MU_SLP_STATE_SSS);
            return;
        }
        break;
    default:
        /* Unranked and Party. */
        break;
    }
#if MU_SQUAD
    if (squad_opponent_gone("CSS exit")) {
        gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);
        return;
    }
#endif
    mu_slippi_splash_init();
    gm_SetNextGameModeStateId(MU_SLP_STATE_SPLASH);
}

/* SSSScenePrep: the vanilla VS-mode SSS prep. */
static void sss_prep(GameModeState* state)
{
    gmVsMelee_EnterSss(state, gmVsMelee_GetVsData());
}

/* SSSSceneDecide. */
static void sss_decide(GameModeState* state)
{
    SSSData* sss = gm_GetGameModeStateExitData(state);
    if (!sss->start_game) {
        gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);
        return;
    }
    slp.chose_stage = 1;
    mu_slippi_load_match_state(NULL);
    if (last_match.local_ready == last_match.remote_ready) {
        mu_slippi_splash_init();
        gm_SetNextGameModeStateId(MU_SLP_STATE_SPLASH);
    } else {
        gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);
    }
}

/* VS ScenePrep (vanilla 801b1588), then InitOnlinePlay arms the scene as an online match. */
static void vs_prep(GameModeState* state)
{
    gmVsMelee_EnterVs(state, gmVsMelee_GetVsData(), NULL, NULL);
    if (!mu_ranked_setup_cpu()) mu_online_arm(slp.mode, gm_801677F0());
    else if (mu_ranked_set()->tiebreak_num && mu_ranked_set()->last_end_mode != 7) {
        gmVsMelee_StartData.rules.time_limit = 180;
        gmVsMelee_StartData.players[0].stocks = gmVsMelee_StartData.players[1].stocks = 1;
    }
}

/* CheckIfWonLastGame. */
static int won_last_game(int slot)
{
    MatchEnd* me = &gmVsMelee_VsExitInfo.match_end;
    struct MatchPlayerData* p = &me->player_standings[slot];

    if (me->outcome == 0) {
        return 0;
    }
    if (gm_GetStartMeleeRules()->is_teams != me->is_teams) {
        return 0;
    }
    if (p->pkind == Gm_PKind_NA) {
        return 0;
    }
    if (me->outcome == 7) {   /* LRA-Start: byte 0 is the player who pressed it */
        int lras = (int) (me->x0 >> 24);
        if (me->is_teams == 1) {
            if (lras >= GM_MAX_PLAYERS) {
                return 1;
            }
            return me->player_standings[lras].team != p->team;
        }
        return lras != slot;
    }
    if (me->is_teams == 1) {
        return fn_801654A0(me) == p->team;
    }
    return p->is_big_loser == 0;
}

/* VSSceneDecide (runs the vanilla VS decide first). */
static void vs_decide(GameModeState* state)
{
    MatchExitInfo* mei;
    MatchEnd* me = &gmVsMelee_VsExitInfo.match_end;
    int next = MU_SLP_STATE_CSS;
    int local;
    ssize_t i;

    /* gmvsmode.c onExitVs */
    gmVsMelee_ExitVs(state, gmVsMode_State_Results, gmVsMode_State_SuddenDeath);
    mei = gm_GetGameModeStateExitData(state);
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (mei->match_end.player_standings[i].pkind != Gm_PKind_NA) {
            gm_80162A98(mei->match_end.player_standings[i].x20);
            gm_RecordSelfDestructs(mei->match_end.player_standings[i].self_destructs);
            gm_80162A4C(mei->match_end.player_standings[i].x44);
        }
    }

    mu_slippi_load_match_state(NULL);
    local = last_match.local_index & 3;
    slp.game_local_index = (u8) local;   /* StaticData: read by the Party results screen */

    if (slp.mode == MU_SLP_MODE_PARTY) {
        /* Someone who chose to leave (Slippi 3.7.0) goes straight back to the CSS. */
        if (!mu_online_is_leaving()) {
            next = MU_SLP_STATE_RESULTS;
        }
        mu_slippi_cleanup_connections();   /* always disconnect after Party */
    }
    if (slp.mode == MU_SLP_MODE_RANKED) {
        int result;
        /* CPU hook plays an ordinary local VS game; online results are captured
         * by report_game while LGL/stocks/percent still exist. */
        if (mu_ranked_setup_cpu()) {
            int w0 = won_last_game(0), w1 = won_last_game(1);
            mu_ranked_setup_game_result(w0 != w1 ? (w0 ? 0 : 1) : -1, me->outcome, 0);
        }
        /* The stage the game ran, from its own VS data (the same game_info + 0xE): a failed
         * post-game read zeroes last_match, and the stage enters the set's DSR history. */
        result = mu_ranked_setup_finish_game(
            last_match.connection_state == MU_SLP_MM_CONNECTION_SUCCESS,
            gmVsMelee_GetVsData()->start.rules.stkind);
        /* asm main.asm:617-741: only conclusive games score; a tie reuses cur_game. */
        if (result == MU_SETUP_NEXT_GAME || result == MU_SETUP_TIEBREAK) {
            next = MU_SLP_STATE_GAME_SETUP;
        } else {
            u8 end_mode = result == MU_SETUP_SET_OVER ? 0 : 1;
            mu_slippi_cmd(MU_CMD_REPORT_SET_COMPLETE, &end_mode, 1, NULL, 0, NULL);
            mu_slippi_cleanup_connections();
        }
    }
    gm_SetNextGameModeStateId((u8) next);

    slp.is_winner = (s8) won_last_game(local);
    if (slp.mode == MU_SLP_MODE_TEAMS) {
        /* Port 0 always picks the stage. */
        slp.is_winner = local != 0 ? 1 : 0;
    } else {
        int winners = 0;
        for (i = 0; i < 4; i++) {
            winners += won_last_game((int) i) != 0;
        }
        if (winners != 1) {
            slp.is_winner = MU_SLP_WINNER_LOST;   /* a draw: both pick */
        }
    }

    /* The gold winner text: fake who LRA-started (byte 0) and player 1's loser flag. Not for
     * Party (its results screen breaks); forced off in Teams and Ranked. */
    if (slp.mode != MU_SLP_MODE_PARTY) {
        u32 who = won_last_game(local) ? 1 : 0;
        me->x0 = (me->x0 & 0x00FFFFFFu) | (who << 24);
        me->player_standings[0].is_big_loser = who ? 0 : 1;
        if (slp.mode == MU_SLP_MODE_TEAMS || slp.mode == MU_SLP_MODE_RANKED) {
            me->outcome = 0;
        }
    }

    slp.chose_stage = 0;
    /* A new seed, so both games do not random the same character after a match. */
    *HSD_RandSeedPtr = mu_slippi_new_seed();
}

/* Results: the vanilla VS-mode results prep and decide (back to CSS, state 0). */
static void results_prep(GameModeState* state)
{
    gmVsMelee_EnterResults(state);
}

static void results_decide(GameModeState* state)
{
    gmVsMelee_ExitResults(state, gmVsMelee_GetVsData(), MU_SLP_STATE_CSS);
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}

/* The Classic splash's enter data, as bytes (gmclassic.c gmClassicIntroData: two s32, then u8s
 * at the console offsets). */
typedef struct MuSplashData {
    s32 x00;   /* pvp type (Slippi writes the low byte of the big-endian word) */
    s32 x04;   /* event mode */
    u8 b[0x18];
} MuSplashData;
/* Classic's intro buffer is the first part of gmClassic_rt natively (gm/gmclassic.c). */
extern unsigned char gmClassic_rt[];
#define gmClassicIntroDataBuffer (*(MuSplashData*) gmClassic_rt)

/* The match port each of the splash's player slots shows: gm_1832.c fn_80185E34 gives the left
 * side's places slots 0.., fn_80185F5C the right side's the next ones, each skipping an empty place
 * (0x21). The splash's models wear their port's skin through it (shim/mu_port_costume.c). */
static signed char splash_slot_ports[6] = { -1, -1, -1, -1, -1, -1 };

int mu_slippi_splash_slot_port(int slot)
{
    if (slot < 0 || slot >= 6 || !mu_slippi_on_online_splash()) {
        return -1;
    }
    return splash_slot_ports[slot];
}

/* SplashScenePrep. */
static void splash_prep(GameModeState* state)
{
    static const u8 layout[16] = { 0x01, 0x78, 0x01, 0x01, 0x01, 0xFF, 0x21, 0x21,
                                   0xFF, 0x21, 0x21, 0xEE, 0x00, 0x00, 0xEE, 0x00 };
    StartMeleeData* start = &gmVsMelee_GetVsData()->start;
    MuSplashData* sp = &gmClassicIntroDataBuffer;
    u8* d = (u8*) sp;    /* console offsets from the start of the struct */
    PreloadedGameModeState* cache;
    const u8* gi;
    int i, is_teams, local, local_team, left = 0, right = 0;
    signed char side_port[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
    int slots = 0;
    u64 mask = 0;
    (void) state;

    mu_slippi_load_match_state(NULL);
    gi = last_match.game_info;

    if (slp.mode == MU_SLP_MODE_TEAMS) {
        for (i = 0; i < 4; i++) {
            start->players[i].color =
                mu_slippi_team_costume(start->players[i].team + 1, start->players[i].ckind);
        }
    }

    memcpy(d + 8, layout, sizeof layout);
    sp->x04 = 0;
    sp->x00 = 0;
    is_teams = gi[8];
    d[0x0A] = 2;
    d[0x0E] = 1;
    d[0x0F] = 1;
    d[0x11] = 1;
    d[0x12] = 1;
    d[0x14] = 1;
    d[0x15] = 1;
    d[0x17] = 1;
    sp->x00 = is_teams;   /* the announcer says "Team ..." */

    local = last_match.local_index;
    local_team = gi[0x60 + 0x24 * (local & 3) + 9];
    for (i = 0; i < 4; i++) {
        PlayerInitData* p = &start->players[i];
        int go_left;
        if (p->slot_type >= 3) {
            continue;
        }
        go_left = i == local || (is_teams && p->team == local_team);
        /* The splash poses each fighter with its Classic intro motion, and the intro archive
         * (IrAls.dat) has those for the retail characters only: a mod's added fighter (ACE,
         * Akaneia) had none and the splash stopped the game as it built the model. Its place on
         * the splash stays empty (0x21, as an unused place); the name call and the match keep it. */
        if (MU_AK_CKIND(p->ckind)) {
            OSReport("[ak] online splash: character kind %d has no intro motion; its model is left out\n",
                     (int) p->ckind);
        }
        if (go_left) {
            d[0x0D + left] = MU_AK_CKIND(p->ckind) ? 0x21 : (u8) p->ckind;
            d[0x13 + left] = p->color;
            if (left < 3) side_port[0][left] = (signed char) i;
            left++;
        } else {
            d[0x10 + right] = MU_AK_CKIND(p->ckind) ? 0x21 : (u8) p->ckind;
            d[0x16 + right] = p->color;
            if (right < 3) side_port[1][right] = (signed char) i;
            right++;
        }
    }
    d[0x0B] = (u8) left;
    d[0x0C] = (u8) right;
    /* A side with no player keeps the layout's 0xFF in its first place, and a player whose
     * character is unset reads as -1: the splash built a fighter of kind -1 for it and stopped the
     * game (a player's 0.9.56 report, 136,000 frames into an online session). Anything that is not a
     * character is an empty place (0x21), which the splash skips. */
    for (i = 0; i < 3; i++) {
        if (d[0x0D + i] > 0x21 || d[0x10 + i] > 0x21) {
            OSReport("online splash: place %d holds no character (%d, %d); left empty\n", i,
                     (int) d[0x0D + i], (int) d[0x10 + i]);
        }
        if (d[0x0D + i] > 0x21) d[0x0D + i] = 0x21;
        if (d[0x10 + i] > 0x21) d[0x10 + i] = 0x21;
    }
    /* Which match port each model stands for, in the order the splash makes them. */
    memset(splash_slot_ports, -1, sizeof splash_slot_ports);
    for (i = 0; i < 3 && i < left; i++) {
        if (d[0x0D + i] != 0x21) splash_slot_ports[slots++] = side_port[0][i];
    }
    for (i = 0; i < 3 && i < right; i++) {
        if (d[0x10 + i] != 0x21) splash_slot_ports[slots++] = side_port[1][i];
    }
    mu_port_costume_splash_begin();

    /* Preload the fighters (p3/p4 only when the negotiated slot is used) and the stage. */
    cache = lbDvd_GetPreloadCacheScene();
    for (i = 0; i < 4; i++) {
        if (i >= 2 && gi[0x60 + 0x24 * i + 1] >= 3) {
            continue;
        }
        cache->game_cache.entries[i].char_id = start->players[i].ckind;
        cache->game_cache.entries[i].color = start->players[i].color;
    }
    cache->game_cache.stkind = start->rules.stkind;
    lbDvd_80018254();
    lbDvd_80018C2C(199);
    lbDvd_80017700(4);

    /* The fighters' and the stage's sound banks. */
    lbAudioAx_80026F2C(0x1C);
    for (i = 0; i < 6; i++) {
        if (start->players[i].ckind != FIGHTER_NUM) {
            mask |= lbAudioAx_80026E84((CharacterKind) start->players[i].ckind);
        }
    }
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    if (gmSquad_OnlineReady()) mask |= gmSquad_SoundMask();
#endif
    mask |= lbAudioAx_80026EBC((StKind) start->rules.stkind);
    lbAudioAx_8002702C(4, mask);
    lbAudioAx_80027168();
}

/* SplashSceneDecide: on to VS. */
static void splash_decide(GameModeState* state)
{
    (void) state;
#if MU_SQUAD
    if (squad_opponent_gone("splash")) {
        gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);
        return;
    }
#endif
    gm_SetNextGameModeStateId(MU_SLP_STATE_VS);
}

extern UNK_T gmClassic_804D68D0;

/* asm main.asm:1360-1434: invalidate character preloads except on replay. */
static void setup_prep(GameModeState* state)
{
    (void)state;
    if (!mu_ranked_set()->tiebreak_num) lbDvd_SetupVsPreloadCache();
}
static void setup_decide(GameModeState* state)
{
    (void)state;
    if (mu_slippi_load_match_state(NULL) ||
        last_match.connection_state != MU_SLP_MM_CONNECTION_SUCCESS ||
        mu_ranked_setup_model()->status != MU_SETUP_COMPLETE) {
        u8 end_mode = 1;
        mu_online_abi_log("ranked setup: left before both seats finished it");
        mu_slippi_cmd(MU_CMD_REPORT_SET_COMPLETE, &end_mode, 1, NULL, 0, NULL);
        gm_SetNextGameModeStateId(MU_SLP_STATE_CSS);
        return;
    }
    mu_slippi_splash_init();
    gm_SetNextGameModeStateId(mu_ranked_set()->tiebreak_num ? MU_SLP_STATE_VS : MU_SLP_STATE_SPLASH);
}

static GameModeState online_states[] = {
    { MU_SLP_STATE_CSS, lbDvdPreload_3, 0, css_prep, css_decide,
      { GS_CSS, &online_css_data, &online_css_data } },
    { MU_SLP_STATE_SSS, lbDvdPreload_3, 0, sss_prep, sss_decide,
      { GS_SSS, &gmVsMelee_SssData, &gmVsMelee_SssData } },
    { MU_SLP_STATE_VS, lbDvdPreload_3, 0, vs_prep, vs_decide,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { MU_SLP_STATE_RESULTS, lbDvdPreload_3, 0, results_prep, results_decide,
      { GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL } },
    { MU_SLP_STATE_SPLASH, lbDvdPreload_3, 0, splash_prep, splash_decide,
      { GS_INTRO_EASY, &gmClassicIntroDataBuffer, &gmClassic_804D68D0 } },
    /* Ranked game prep: rules/transport, then A2 screen callbacks. */
    { MU_SLP_STATE_GAME_SETUP, lbDvdPreload_3, 0, setup_prep, setup_decide,
      { GS_SLIPPI_GAME_SETUP, NULL, NULL } },
    { GM_GAMEMODESTATE_TERMINATE },
};

/* main.asm, once at boot: the online major, and the online bytes cleared. The '#' key of the name
 * entry belongs to the name-entry owner (see s6-core-api.md). */
void mu_slippi_scene_install(void)
{
    GameMode* mode;
    if (!mu_slippi_menus_enabled()) {
        return;
    }
    slp.code_entry = 0;
    slp.pause = 0;
    for (mode = gm_GetAllGameModes(); mode->kind != GM_COUNT; mode++) {
        if (mode->kind == GM_HANYU_CSS) {
            mode->preloaded = true;
            mode->on_load = online_on_load;
            mode->on_unload = online_on_unload;
            mode->states = online_states;
            installed = 1;
            break;
        }
    }
}
