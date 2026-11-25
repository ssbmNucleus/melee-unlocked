/* Native Slippi online character select screen (the online CSS, major 8 state 0).
 *
 * Native form of the Online/Menus/CSS codes of GALE01r2.ini that are not tied to mncharsel.c's
 * file-static state: SceneLoadCSS (the CSS data table and slpCSS.dat), FetchMatchInfo (B3 once
 * per cursor think), HandleInputsOnCSS (the lock-in / search / cancel machine and its host
 * commands), LoadCSSText (user display, status text, spinners, error lines, mode title),
 * SkipReturnToCssSound and the unselect / colour-change guards. The B5/B4 senders and the
 * r13-0x5018 callback body are shared with name entry and the SSS (mu_slippi_sss.c).
 * mncharsel.c calls in from #ifdef MU_NATIVE blocks; the Teams button, cursor and token colour
 * and CSP overrides live there because they touch its statics.
 *
 * Nothing here runs unless mu_slippi_menus_enabled() and the online CSS predicate hold.
 * Chat (SlippiCSS.dat) lives in mu_slippi_chat.c: it reads this table's MSRB and slpCSS and sets
 * the chat-window byte through the accessors at the end of this file.
 */
#include <string.h>
#include <dolphin/dvd.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <mu_native.h>
#include <mu_test_env.h>
#include <mu_ranked.h>
#include <mu_rank_badge.h>
#include <mu_lobby_list.h>
#include <mu_commands.h>
#include "mu_squad.h"   /* Squad Strike in Direct: this player's lineup for the pick preview */

extern char mnNameNew_CurrentNameText[];

/* The online select's Options bar and menu (shim/mu_online_options.c). */
void mu_online_options_scene(void);
void mu_online_options_frame(void);
int mu_online_options_busy(void);

enum {
    SB_RAND = -2,   /* stage behavior: random */
    SB_NOTSEL = -1, /* stage behavior: use the opponent's */
    DISCONNECT_HOLD_DELAY = 0x30,
    SPINNER_TRANSITION_FRAMES = 15,
    FRAME_MAX = 2 * SPINNER_TRANSITION_FRAMES,
};

/* slpCSS symbol (Online.s "slpCSS Symbol Structure"). */
typedef struct SlpCss {
    DISC_PTR(void) chat_select;
    DISC_PTR(void) chat_msg;
    DISC_PTR(HSD_MatAnimJoint) mode;
    DISC_PTR(void) connect_help;
} DISC_STRUCT SlpCss;

/* The CSS data table (CSSDT). A new, zeroed one per CSS scene load, as SceneLoadCSS allocates. */
static struct {
    MuMatchState msrb; /* CSSDT_MSRB_ADDR: only the per-think B3 writes it */
    SlpCss* slpcss;
    HSD_Text* text;
    u8 spinner[3];
    u16 frame_counter;
    u8 prev_lock_in;
    u8 prev_connected;
    u8 z_hold_timer;
    u8 chat_window_opened; /* set by the chat window (mu_slippi_chat.c) */
    u8 team_idx;
} dt;

/* mncharsel.c state the codes read through r13 (addresses of its statics). */
static CSSData** css_ptr;         /* -0x49F0 mnCharSel_804D6CB0 */
static u8* char_chosen;           /* -0x49A9 mnCharSel_804D6CF7 */
static s8* ctrl_port;             /* -0x49B0 mnCharSel_804D6CF0 */
static u8* scene_request;         /* -0x49AA mnCharSel_804D6CF6 */
static s8* name_entry_port;       /* -0x49A7 mnCharSel_804D6CF9 */
static HSD_JObj** single_menu;    /* -0x49E0 mnCharSel_804D6CC0 */

/* LoadCSSText's user display (FG_UserDisplay LDB). */
static HSD_Text* user_text;
static u8 user_display_mode;

static const GXColor color_white = { 0xFF, 0xFF, 0xFF, 0xFF };
static const GXColor color_gray = { 0x8E, 0x91, 0x96, 0xFF };
static const GXColor color_red = { 0xFF, 0x00, 0x00, 0xFF };
static const GXColor spinner_done_color = { 0x33, 0xFF, 0x2F, 0xFF };
static const GXColor spinner_wait_color = { 0x3C, 0xBC, 0xFF, 0xFF };

static const char spinner_str[2][3] = { { (char) 0x81, 0x7B, 0 }, { (char) 0x81, 0x7E, 0 } };
static const char spinner_done_str[3] = { (char) 0x81, 0x7C, 0 };

/* The Friends lobby's 4-door select runs in the same online major but is Melee's VS select
 * (mu_friends4_css.inc), never this 1-door one. */
int mu_slippi_css_online(void)
{
    return mu_slippi_menus_enabled() && mu_slippi_on_online_css();
}

int mu_slippi_css_teams(void)
{
    return mu_slippi_css_online() && mu_slippi_state()->mode == MU_SLP_MODE_TEAMS;
}

int mu_slippi_css_local_ready(void)
{
    return dt.msrb.local_ready;
}

int mu_slippi_css_team_idx(void)
{
    return dt.team_idx;
}

/* InitTeamToggleButton: the data table and the injection's own byte (kept across CSS loads). */
void mu_slippi_css_set_team_idx(int team)
{
    dt.team_idx = (u8) team;
    mu_slippi_state()->team_idx = (u8) team;
}

/* 2 of every 3 bytes of the name-entry text: FMTB_OPP_CONNECT_CODE and the "--//--" template. */
static void code_sjis18(u8 out[18])
{
    const u8* src = (const u8*) mnNameNew_CurrentNameText;
    int i;
    for (i = 0; i < 9; i++) {
        out[i * 2] = src[i * 3];
        out[i * 2 + 1] = src[i * 3 + 1];
    }
}

/* ---- SceneLoadCSS (mnCharSel_Scene_OnEnter, after css_models) ---- */

void mu_slippi_css_enter(CSSData** css, u8* chosen, s8* port, u8* request, s8* name_port,
                         HSD_JObj** menu_root)
{
    MuSlippiMenuState* st = mu_slippi_state();
    css_ptr = css;
    char_chosen = chosen;
    ctrl_port = port;
    scene_request = request;
    name_entry_port = name_port;
    single_menu = menu_root;

    /* FN_InitBuffers: nothing to allocate natively. CSS data table zeroed. */
    memset(&dt, 0, sizeof dt);

    /* The injection byte starts at 1 (red) and only ever holds 1-3. */
    if (st->team_idx == 0) {
        st->team_idx = 1;
    }
    if (st->mode == MU_SLP_MODE_TEAMS) {
        dt.team_idx = st->team_idx;
    }

    dt.slpcss = mu_rank_badge_load_css();
    mu_slippi_sss_set_slpcss(dt.slpcss);   /* name entry's connect help */
}

/* ---- FetchMatchInfo (mnCharSel_CursorThink, inside the no-scene-request branch) ---- */

void mu_slippi_css_poll(void)
{
    mu_slippi_load_match_state(NULL);
    dt.msrb = *mu_slippi_match_state();
}

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);

/* MU_CMD_CSS_FRAME (mu_commands.h): upstream 0xEC: MU_SQUAD_COMMAND in this build */
/* The menu camera (mn_8022BA1C, every menu frame): the eye before the C-stick turns it, then the
 * camera it turned to. The character select frame hands both to the host, which turns the easter
 * egg's faces with the picture. Reads only: the camera is not changed. */
static struct {
    int ok;                /* a camera after the turn this frame (and its eye before it) */
    float cam[13];         /* eye before, eye, interest, up, vertical field of view (degrees) */
} mu_menu_cam;

/* The menu camera's eye before the C-stick turned it this frame (mu_menu_camera above). */
int mu_menu_camera_neutral_eye(float out[3])
{
    out[0] = mu_menu_cam.cam[0];
    out[1] = mu_menu_cam.cam[1];
    out[2] = mu_menu_cam.cam[2];
    return out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f;
}

void mu_menu_camera(HSD_CObj* cobj, int tilted)
{
    Vec3 v;
    if (cobj == NULL) {
        return;
    }
    if (!tilted) {
        HSD_CObjGetEyePosition(cobj, &v);
        mu_menu_cam.cam[0] = v.x;
        mu_menu_cam.cam[1] = v.y;
        mu_menu_cam.cam[2] = v.z;
        mu_menu_cam.ok = 0;
        return;
    }
    HSD_CObjGetEyePosition(cobj, &v);
    mu_menu_cam.cam[3] = v.x;
    mu_menu_cam.cam[4] = v.y;
    mu_menu_cam.cam[5] = v.z;
    HSD_CObjGetInterest(cobj, &v);
    mu_menu_cam.cam[6] = v.x;
    mu_menu_cam.cam[7] = v.y;
    mu_menu_cam.cam[8] = v.z;
    HSD_CObjGetUpVector(cobj, &v);   /* 0, 1, 0 when the camera has none */
    mu_menu_cam.cam[9] = v.x;
    mu_menu_cam.cam[10] = v.y;
    mu_menu_cam.cam[11] = v.z;
    mu_menu_cam.cam[12] = HSD_CObjGetFov(cobj);   /* 0: not a perspective camera */
    mu_menu_cam.ok = mu_menu_cam.cam[12] > 0.0f;
}

/* mnCharSel_Scene_OnFrame, every mode's character select, offline and online: tells the host a
 * character select is on screen (its easter egg lives only there, drawn by the host, never in a
 * match), with the menu camera when there is one, and plays the menu sound the host asks for. Local
 * only: nothing in the match changes. */
void mu_css_frame(void)
{
    static unsigned char reply[4096];   /* the host's minimum response capacity */
    unsigned int got = 0;
    const int with_camera = mu_menu_cam.ok;
    if (mu_online_abi_command(MU_CMD_CSS_FRAME, with_camera ? (const unsigned char*) mu_menu_cam.cam : NULL,
                              with_camera ? (unsigned int) sizeof mu_menu_cam.cam : 0, reply, sizeof reply, &got) == 0 &&
        got >= 1 && reply[0] != 0) {
        lbAudioAx_80024030(reply[0]);
    }
    /* The online select's Options bar and menu (shim/mu_online_options.c), before the cursor reads
     * its pad: an open menu takes the pads from the select. */
    if (mu_slippi_css_online()) {
        mu_online_options_frame();
    }
}

/* Player_SetPlayerCharacter and the rogueMelee run: a human's Zelda starts as Sheik when the player
 * asked for it. The host answers 0 online and during replay playback. */
int mu_human_zelda_as_sheik(void)
{
    static unsigned char reply[4096];   /* the host's minimum response capacity */
    unsigned int got = 0;
    return mu_online_abi_command(MU_CMD_ZELDA_AS_SHEIK, NULL, 0, reply, sizeof reply, &got) == 0 && got >= 1 && reply[0] != 0;
}

/* The key a keyboard player presses for a GameCube button (HSD_PAD_Z, HSD_PAD_B), for the prompts
 * below: the keyboard's Z key is the pad's A and E is its Z, so "Hold Z to disconnect" sent keyboard
 * players to the wrong key (11-18). The host answers from the live key bindings, only for a port the
 * keyboard feeds; 0 leaves `out` alone and the prompt keeps the GameCube name. */
int mu_host_button_label(int port, unsigned button_mask, char* out, int n)
{
    static unsigned char reply[4096];   /* the host's minimum response capacity */
    unsigned char payload[3];
    unsigned int got = 0;
    if (out == NULL || n < 2 || port < 0 || port > 3) return 0;
    payload[0] = (unsigned char) port;
    payload[1] = (unsigned char) (button_mask >> 8);
    payload[2] = (unsigned char) button_mask;
    if (mu_online_abi_command(MU_CMD_BUTTON_LABEL, payload, sizeof payload, reply, sizeof reply, &got) != 0 ||
        got == 0 || got >= (unsigned int) n)
        return 0;
    memcpy(out, reply, got);
    out[got] = 0;
    return 1;
}

/* mnCharSel_CursorThink, after the hand moved: every character select's hands go to the host (x, y as
 * the screen's units, 0.1 steps) when they moved, for the easter egg's face. */
void mu_slippi_css_hand(int port, float x, float y)
{
    static unsigned char reply[4096];
    unsigned int got = 0;
    int ix, iy, packed;

    ix = (int) (x * 10.0f);
    iy = (int) (y * 10.0f);
    packed = (ix & 0xFFFF) << 16 | (iy & 0xFFFF);
    if (port >= 0 && port < 4) {
        /* Every hand on every character select, when it moved: the easter egg's face can be taken
         * hold of. The host only looks at it while the face is out. */
        static int last_any[4] = { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF };
        if (packed != last_any[port]) {
            unsigned char any[5];
            last_any[port] = packed;
            any[0] = (unsigned char) port;
            any[1] = (unsigned char) (ix >> 8);
            any[2] = (unsigned char) ix;
            any[3] = (unsigned char) (iy >> 8);
            any[4] = (unsigned char) iy;
            /* The character select frame command with five bytes: a hand (the port, x, y). */
            mu_online_abi_command(MU_CMD_CSS_FRAME, any, sizeof any, reply, sizeof reply, &got);
        }
    }
}

/* ---- HandleInputsOnCSS helpers ---- */

/* FN_TX_LOCK_IN and FN_TX_FIND_MATCH are shared with name entry and the SSS (mu_slippi_sss.c),
 * so every B5/B4 payload comes from one place. */
static void tx_lock_in(int sb)
{
    mu_slippi_sss_lock_in(sb);
}

/* FN_LOCK_IN_AND_SEARCH. A lock-in Squad Strike refused (fewer than four fighters, a lineup the
 * host rejected) starts no search: its reason stays on screen and the row stays editable. */
static void lock_in_and_search(int sb)
{
    if (mu_slippi_sss_lock_in(sb)) mu_slippi_sss_find_match();
}

/* FN_LOAD_CODE_ENTRY */
static void load_code_entry(void)
{
    MuSlippiMenuState* st = mu_slippi_state();
    st->code_entry = 1;
    st->callback = MU_SLP_CB_CODE_LOCK_IN;
    *name_entry_port = *ctrl_port;
    *scene_request = 4;
}

/* The stage the loser already picked: the asm reads the u16 at +0x1E of the VS data's match struct
 * (gmMainLib_804D3EE0 + 1424 + 8), in the console's byte order. */
static int picked_stage(void)
{
    const u8* b = (const u8*) &gmVsMelee_GetVsData()->start.rules;
    return (b[0x1E] << 8) | b[0x1F];
}

/* HandleInputsOnCSS at fn_80262F44's `mnCharSel_804D6CF2 == 0 && START` test (only called when
 * mnCharSel_804D6CF2 == 0). Returns 1 when both players are ready: the caller continues into the
 * vanilla START path. */
int mu_slippi_css_inputs(unsigned int trigger)
{
    MuSlippiMenuState* st = mu_slippi_state();
    const MuMatchState* m = &dt.msrb;
    u8 prev, cur;

    /* Sounds: lock-in 1 -> 0 "back"; ANY -> ERROR "error"; CONNECTED -> ANY "back". */
    prev = dt.prev_lock_in;
    cur = m->local_ready;
    dt.prev_lock_in = cur;
    if (prev == 1 && cur == 0) {
        lbAudioAx_80024030(0);
    } else {
        prev = dt.prev_connected;
        cur = m->connection_state;
        dt.prev_connected = cur;
        if (prev != MU_SLP_MM_ERROR && cur == MU_SLP_MM_ERROR) {
            lbAudioAx_80024030(3);
        } else if (prev == MU_SLP_MM_CONNECTION_SUCCESS && cur != MU_SLP_MM_CONNECTION_SUCCESS) {
            lbAudioAx_80024030(0);
        }
    }

    switch (m->connection_state) {
    case MU_SLP_MM_IDLE:
        if (mu_online_p2p_menus()) {
            /* A peer-to-peer session has its opponent already: no search and no connect-code
             * entry. Its host never reports this state; a reply that could not be read does. */
            return 0;
        }
        if (dt.chat_window_opened || !(trigger & HSD_PAD_START) || gm_801A4BB8() == 0) {
            return 0;
        }
        st->is_winner = MU_SLP_WINNER_NULL;
        st->chose_stage = 0;
        if (*char_chosen == 0) {
            return 0;
        }
        switch (st->mode) {
        case MU_SLP_MODE_RANKED:
        case MU_SLP_MODE_UNRANKED:
        case MU_SLP_MODE_PARTY:
            lock_in_and_search(SB_RAND);
            break;
        case MU_SLP_MODE_DIRECT:
        case MU_SLP_MODE_TEAMS:
            /* A lobby's code (shim/mu_lobby_list.c): typed in for the player, the search as ever. */
            if (st->mode == MU_SLP_MODE_TEAMS && mu_lobby_css_search()) {
                lock_in_and_search(SB_RAND);
                break;
            }
            load_code_entry();
            break;
        default:
            break;   /* the console stalls (b 0x0) */
        }
        return 0;

    case MU_SLP_MM_INITIALIZING:
    case MU_SLP_MM_MATCHMAKING:
    case MU_SLP_MM_OPPONENT_CONNECTING:
    case MU_SLP_MM_ERROR:
        /* An error also clears on A or B: the keyboard's Z key is the pad's A (E is Z), so a keyboard
         * player pressing the "Z" the screen names was stuck there until the game was killed (11-17). */
        if ((trigger & HSD_PAD_Z) ||
            (m->connection_state == MU_SLP_MM_ERROR && (trigger & (HSD_PAD_A | HSD_PAD_B)))) {
            /* Z on a lobby's search also closes or leaves the lobby (shim/mu_lobby_list.c). */
            if ((trigger & HSD_PAD_Z) && m->connection_state != MU_SLP_MM_ERROR) mu_lobby_css_cancel();
            mu_slippi_cleanup_connections();
        }
        return 0;

    case MU_SLP_MM_CONNECTION_SUCCESS:
        break;

    default:
        return 0;
    }

    /* Connected: hold Z to disconnect. Not while the Options menu has the controller (it reads the
     * game's buttons, which the menu does not take). */
    if (!mu_online_options_busy() && ((u32) gm_GetButtonsPressed((u8) *ctrl_port) & HSD_PAD_Z)) {
        dt.z_hold_timer++;
        if (dt.z_hold_timer > DISCONNECT_HOLD_DELAY) {
            u8 reason = 1;   /* Same Slippi cleanup command, with the native CSS's Z reason. */
            dt.z_hold_timer = 0;
            mu_slippi_cmd(MU_CMD_CLEANUP_CONNECTION, &reason, 1, NULL, 0, NULL);
            return 0;
        }
    } else {
        dt.z_hold_timer = 0;
    }

    /* A peer-to-peer session's first game: the stage is picked, never drawn. The player in slot 0
     * (the one who sent the request) picks it on the stage select screen, exactly as the loser of
     * a game picks the next one, and the other player locks in without a stage. Decided here, once
     * the two are connected and the slots are known; later games follow winner and loser. */
    if (mu_online_p2p_menus() && st->is_winner == MU_SLP_WINNER_NULL &&
        m->connection_state == MU_SLP_MM_CONNECTION_SUCCESS)
    {
        st->is_winner = m->local_index == 0 ? MU_SLP_WINNER_LOST : MU_SLP_WINNER_WON;
        st->chose_stage = 0;
    }

    if (m->local_ready == 0) {
        int advance = (trigger & HSD_PAD_START) != 0;
        if (!advance && (st->mode == MU_SLP_MODE_DIRECT || st->mode == MU_SLP_MODE_TEAMS) &&
            st->is_winner == MU_SLP_WINNER_LOST && st->chose_stage == 1)
        {
            advance = 1;
        }
        if (advance && *char_chosen != 0 && gm_801A4BB8() != 0) {
            switch (st->mode) {
            case MU_SLP_MODE_UNRANKED:
            case MU_SLP_MODE_PARTY:
                tx_lock_in(SB_RAND);
                break;
            case MU_SLP_MODE_RANKED:
                if (mu_slippi_ranked_available()) {
                    tx_lock_in(SB_RAND);
                }
                break;
            case MU_SLP_MODE_DIRECT:
            case MU_SLP_MODE_TEAMS:
                if (st->is_winner == MU_SLP_WINNER_LOST) {
                    if (st->chose_stage != 0) {
                        tx_lock_in(picked_stage());
                    } else {
                        /* Teams bit for the doubles FoD rule, then leave for the SSS. */
                        CSSData* css = *css_ptr;
                        css->vs.start.rules.is_teams = st->mode == MU_SLP_MODE_TEAMS ? 1 : 0;
                        *scene_request = 1;
                        st->callback = MU_SLP_CB_SSS_LOCK_IN;
                        return 0;
                    }
                } else if (st->is_winner == MU_SLP_WINNER_WON) {
                    tx_lock_in(SB_NOTSEL);
                }
                /* any other value stalls on the console (b 0x0) */
                break;
            default:
                break;   /* Ranked: the console stalls (b 0x0) */
            }
        }
    }

    /* Both ready: start. Reads the table's MSRB, which the B5 above did not refresh. */
    return (m->local_ready & m->remote_ready) != 0;
}

/* PreventA/BPressCharUnselect: no unselect while chat is open or locked in. */
int mu_slippi_css_block_unselect(void)
{
    return mu_slippi_css_online() && (dt.chat_window_opened != 0 || dt.msrb.local_ready != 0);
}

/* PreventColorChange: never in Teams (the colour follows the team), else not while locked in. */
int mu_slippi_css_block_costume_change(void)
{
    if (!mu_slippi_css_online()) {
        return 0;
    }
    return mu_slippi_state()->mode == MU_SLP_MODE_TEAMS || dt.msrb.local_ready != 0;
}

/* SkipReturnToCssSound (mnCharSel_802640A0, before the voice of any CSS with match_type != 0,
 * whenever the menus flag is on, as on Legacy). Returns 1 to skip the voice. */
int mu_slippi_css_skip_return_sound(void)
{
    MuSlippiMenuState* st = mu_slippi_state();
    MuMatchState tmp;
    if (st->code_entry != 0) {
        /* Back from connect-code entry. The autocomplete buffers are name entry's own
         * (code_scratch); nothing to free natively. */
        st->code_entry = 0;
        return 1;
    }
    /* Back from the SSS locked in: "choose your character" would be wrong. Its own B3 buffer. */
    mu_slippi_load_match_state(&tmp);
    return tmp.local_ready != 0;
}

/* ---- LoadCSSText ---- */

/* FG_CreateSubtext, no outlines. */
static int create_subtext(HSD_Text* t, const GXColor* color, f32 scale, f32 x, f32 y)
{
    GXColor c = *color;
    int idx = HSD_SisLib_803A6B98(t, x, y, "");
    HSD_SisLib_803A7548(t, idx, scale, scale);
    HSD_SisLib_803A74F0(t, idx, &c);
    HSD_SisLib_803A70A0(t, idx, (char*) "");
    return idx;
}

static void set_text(int idx, const char* s)
{
    HSD_SisLib_803A70A0(dt.text, idx, (char*) "%s", s);
}

static void set_text_fmt(int idx, const char* fmt, const char* arg)
{
    HSD_SisLib_803A70A0(dt.text, idx, (char*) fmt, arg);
}

static HSD_Text* text_struct(f32 z, f32 scale)
{
    HSD_Text* t = HSD_SisLib_803A6754(0, 0);
    t->default_kerning = 1;
    t->default_alignment = 0;
    t->pos_z = z;
    t->font_size.x = scale;
    t->font_size.y = scale;
    return t;
}

/* FN_UserTextUpdate (one B9 per CSS load). The major is 8 here, so no submenu test. */
static void user_text_update(void)
{
    char buf[32];
    int i;
    mu_slippi_fetch_app_state();
    for (i = 0; i < 4; i++) {
        HSD_SisLib_803A70A0(user_text, i, (char*) "");
    }
    if (mu_slippi_app_state() != MU_SLP_APP_LOGGED_IN) {
        return;
    }
    HSD_SisLib_803A70A0(user_text, 0, (char*) "User");
    memcpy(buf, mu_slippi_user_name(), 31);
    buf[31] = 0;
    HSD_SisLib_803A70A0(user_text, 1, (char*) "%s", buf);
    if (user_display_mode == 2) {
        HSD_SisLib_803A70A0(user_text, 2, (char*) "Connect Code");
        memcpy(buf, mu_slippi_user_code(), 10);
        buf[10] = 0;
        HSD_SisLib_803A70A0(user_text, 3, (char*) "%s", buf);
    }
}

/* FN_InitUserDisplay at (-112, 20), z 0, scale 0.1. */
static void user_display_init(int mode)
{
    f32 x = -112.0f, y = 20.0f;
    static const f32 steps[3] = { 20.0f, 25.0f, 20.0f };
    int i;
    user_display_mode = (u8) mode;
    user_text = text_struct(0.0f, 0.1f);
    for (i = 0; i < 4; i++) {
        int is_label = (i & 1) == 0;
        int idx = HSD_SisLib_803A6B98(user_text, x, y, "");
        GXColor c = is_label ? color_gray : color_white;
        f32 size = is_label ? 0.4f : 0.5f;
        HSD_SisLib_803A7548(user_text, idx, size, size);
        HSD_SisLib_803A74F0(user_text, idx, &c);
        if (i < 3) {
            y += steps[i];
        }
    }
    user_text_update();
}

enum {
    ST_HEADER = 0,
    ST_LINE1 = 1,
    ST_SPIN1 = 2,
    ST_LINE3 = 5,
    ST_PRESS_Z = 7,
    ST_PRESS_D = 8,
    ST_PLAYING_LABEL = 9,
    ST_PLAYING_OPP = 10,
    ST_ERR1 = 11,
    ST_ERR4 = 14,
    ST_RANK_OPPONENT = 15,
};

static void set_header(const char* service, const char* mode)
{
    HSD_SisLib_803A70A0(dt.text, ST_HEADER, (char*) "%s %s", service, mode);
}


/* The live pick preview, every frame while connected (display only; the host sends it on change):
 * the fighter under this player's coin while it is down, the costume the portrait shows, the team,
 * the Squad Strike lineup as it stands and whether they locked in. The online CSS has one door, 0. */
static void pick_preview_frame(void)
{
    MuSlippiMenuState* st = mu_slippi_state();
    u8 port = gm_801677F0();   /* the port mu_slippi_sss_lock_in sends */
    u8 ck = 0xFF, color = 0, team = 0xFF, count = 0;
    u8 kinds[3] = { 0xFF, 0xFF, 0xFF }, colors[3] = { 0, 0, 0 };

    if (css_ptr == NULL || *css_ptr == NULL || port >= 4 ||
        dt.msrb.connection_state != MU_SLP_MM_CONNECTION_SUCCESS) {
        return;
    }
    mu_css_pick_preview(0, port, &ck, &color);
#ifdef MU_AKANEIA_FIGHTERS
    if (ck != 0xFF) {
        const int ext = mu_ak_mex_external(ck);   /* the external id 0x82 sends */
        ck = ext >= 0 ? (u8) ext : 0xFF;
    }
#endif
    if (st->mode == MU_SLP_MODE_TEAMS && st->team_idx >= 1 && st->team_idx <= 3) {
        team = (u8) (st->team_idx - 1);
    }
#if MU_SQUAD
    /* Squad Strike in Direct, while this select picks squads: fighters 2-4 of the lineup (fighter 1 is
     * the coin pick above). */
    if (ck != 0xFF && gmSquad_CssPickCount() > 1) {
        GmSquadMember lineup[GMSQUAD_SIZE];
        u8 coin_ck, coin_color;
        const int n = gmSquad_CssPreview(&coin_ck, &coin_color, lineup);
        int i;
        for (i = 1; i < n && count < 3; i++) {
            kinds[count] = lineup[i].ckind;
            colors[count] = lineup[i].color;
            count++;
        }
    }
#endif
    mu_pick_preview_send(ck, color, team, count, kinds, colors, dt.msrb.local_ready != 0);
}

/* "Playing:" (or "Found:"), the opponent and the lines under them are the last lines of the status
 * block at the bottom right, under the Z and chat lines: the top of the screen, where Slippi draws
 * them, is the Options bar's (shim/mu_online_options.c). The lines under "Playing:" are created
 * last, one after the other, so the other indexes stay put. */
#define PLAYING_X 70.0f
#define PLAYING_Y 172.0f         /* a line under the chat line (152.5), in the block's own step */
#define PLAYING_NAME_X 134.0f    /* the name beside "Playing:" */
#define FOUND_NAME_X 122.0f      /* ... and beside "Found:" */
#define PLAYING_LINES 3
#define PLAYING_LINE_SIZE 80
#define PLAYING_SIZE 0.35f       /* the size of those lines */
#define PLAYING_FIT 34           /* the letters of one that fit the block's width at that size */
static int st_opp_picks = -1;
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);

static const char* const pick_names[26] = {
    "Falcon", "DK",     "Fox",   "Game & Watch", "Kirby", "Bowser", "Link",
    "Luigi",  "Mario",  "Marth", "Mewtwo",       "Ness",  "Peach",  "Pikachu",
    "Ice Climbers", "Jigglypuff", "Samus", "Yoshi", "Zelda", "Sheik", "Falco",
    "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf",
};

static const char* pick_name(u8 ck)
{
    return ck < 26 ? pick_names[ck] : "a fighter";
}

/* The lines under "Playing:" while connected. An opponent's pick is not shown: who they are on, and
 * whether they locked in, stays theirs until the match (the owner's decision for 0.1.0; the pick
 * previews still arrive, nothing sent to peers changed).
 * One opponent (Direct, Unranked, Ranked): the name is beside "Playing:" and nothing goes under it,
 * except while a Squad Strike match is being set up in Direct, where the players agree on squads:
 * then the first line is their squad in fighting order ("Fox, Marth, Falco, Sheik").
 * Teams: a line for every other connected player, the name alone. */
static void playing_lines(const MuMatchState* m, int teams, char out[PLAYING_LINES][PLAYING_LINE_SIZE])
{
    int i, line = 0;
    for (i = 0; i < PLAYING_LINES; i++) out[i][0] = 0;
    if (teams) {
        int port;
        for (port = 0; port < 4 && line < PLAYING_LINES; port++) {
            int there = m->names[port][0] != 0;
            if (port == m->local_index) continue;
            for (i = 0; i < 3; i++) {
                if (m->opp_picks[i].valid && m->opp_picks[i].port == port) there = 1;
            }
            if (!there) continue;   /* nobody there */
            if (m->names[port][0]) {
                memcpy(out[line], m->names[port], 31);
                out[line][31] = 0;
            } else {
                snprintf(out[line], PLAYING_LINE_SIZE, "P%d", port + 1);
            }
            line++;
        }
        return;
    }
    for (i = 0; i < 3; i++) {
        const MuOpponentPick* o = &m->opp_picks[i];
        size_t used = 0;
        int k, n;
        if (!o->valid) continue;
        if (o->char_id == 0xFF || o->squad_count == 0) return;   /* no Squad Strike lineup: the name alone */
        n = snprintf(out[0], PLAYING_LINE_SIZE, "%s", pick_name(o->char_id));
        if (n > 0) used = (size_t) n < PLAYING_LINE_SIZE ? (size_t) n : PLAYING_LINE_SIZE - 1;
        for (k = 0; k < o->squad_count && k < 3 && used + 1 < PLAYING_LINE_SIZE; k++) {
            n = snprintf(out[0] + used, PLAYING_LINE_SIZE - used, ", %s", pick_name(o->kinds[k]));
            if (n > 0) used += (size_t) n < PLAYING_LINE_SIZE - used ? (size_t) n : PLAYING_LINE_SIZE - used - 1;
        }
        return;
    }
}

/* ---- custom fighters online (port/runtime/host/online_fighters.h, docs/online-custom-content.md) ---- */

char mu_mods_drawable(char c);   /* shim/mu_mods_text.c: a letter the font draws, else a space */

/* The host's answer to op 1: online allowed, then the shown, installed and selectable bits (u32 BE). */
static int roster_bits(unsigned int out[3])
{
    unsigned char payload = 1, reply[16];
    unsigned int got = 0;
    int i;
    if (mu_slippi_cmd(MU_CMD_ONLINE_CONTENT, &payload, 1, reply, sizeof reply, &got) != 0 || got < 13) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        const unsigned char* b = reply + 1 + i * 4;
        out[i] = (unsigned int) b[0] << 24 | (unsigned int) b[1] << 16 | (unsigned int) b[2] << 8 | b[3];
    }
    return reply[0];
}

/* Online, added fighters are offered in Direct and Teams only, never in a peer-to-peer session. */
static int roster_online_mode(void)
{
    const int mode = mu_slippi_state()->mode;
    return !mu_online_p2p_menus() && (mode == MU_SLP_MODE_DIRECT || mode == MU_SLP_MODE_TEAMS);
}

/* Which installed fighters (bit k: roster fighter k) a character select shows (mncharsel.c
 * mu_css_roster_setup). Offline every one installed beside the game; online, in Direct and Teams
 * only, every one that brings no code of its own. */
unsigned int mu_css_roster_offered(void)
{
    unsigned int bits[3];
    const int online = mu_slippi_css_online();
    const int allowed = roster_bits(bits);
    if (allowed < 0) {
        return online ? 0u : 0xFFFFFFFFu;
    }
    if (!online) {
        return bits[1];
    }
    return allowed && roster_online_mode() ? bits[0] : 0u;
}

/* Online: the shown fighters every player in the session has (the others are greyed out, mncharsel.c
 * mu_css_roster_grey_frame). Before anyone is connected, every shown one. */
unsigned int mu_css_roster_selectable(void)
{
    unsigned int bits[3];
    const int allowed = roster_bits(bits);
    if (!mu_slippi_css_online()) {
        /* Tests only: MELEE_TEST_ROSTER_GREY=<mask> greys these roster fighters on the offline select,
         * so a capture shows the greyed-out icon (agent copies cannot log in to reach Direct). */
        static int read = 0;
        static unsigned int grey = 0;
        if (!read) {
            const char* v = mu_test_getenv("MELEE_TEST_ROSTER_GREY");
            read = 1;
            for (; v != NULL && *v >= '0' && *v <= '9'; ++v) grey = grey * 10u + (unsigned int) (*v - '0');
        }
        return ~grey;
    }
    return allowed > 0 && roster_online_mode() ? bits[2] : 0u;
}

/* mncharsel.c, every frame: the roster fighter whose greyed-out icon the local hand is over, or -1. */
static int roster_hover = -1;
void mu_css_roster_hover(int fighter)
{
    roster_hover = fighter;
}

/* The line for the greyed-out fighter under the hand ("P2 does not have Slippy installed."), else the
 * host's line about this player's own pick (why it was unlocked, or what it waits on), else "". */
static const char* content_line(int connected, int* spinner)
{
    static char line[64];
    unsigned char payload[2], reply[64];
    unsigned int got = 0, i;
    line[0] = 0;
    *spinner = 0;
    if (roster_hover >= 0 && roster_hover < 32) {
        payload[0] = 2;
        payload[1] = (unsigned char) roster_hover;
        if (mu_slippi_cmd(MU_CMD_ONLINE_CONTENT, payload, 2, reply, sizeof reply, &got) != 0) {
            got = 0;
        }
        for (i = 0; i < got && i < sizeof line - 1; i++) {
            line[i] = mu_mods_drawable((char) reply[i]);
        }
        line[i] = 0;
        if (line[0]) {
            return line;
        }
    }
    if (!connected) {
        return line;
    }
    payload[0] = 0;
    if (mu_slippi_cmd(MU_CMD_ONLINE_CONTENT, payload, 1, reply, sizeof reply, &got) != 0 || got < 1) {
        return line;
    }
    *spinner = reply[0] != 0;
    for (i = 1; i < got && i - 1 < sizeof line - 1; i++) {
        line[i - 1] = mu_mods_drawable((char) reply[i]);
    }
    line[i - 1] = 0;
    return line;
}

static void text_think(HSD_GObj* gobj)
{
    MuSlippiMenuState* st = mu_slippi_state();
    MuMatchState* m = &dt.msrb;
    char opp_code[19] = "--//--//--//--//00";
    char opp_name[32];
    /* The keys the prompts name: the bound key for a keyboard player, else the GameCube button. */
    char z_key[16] = "Z", b_key[16] = "B", start_key[16] = "START", prompt[64];
    const char* s;
    int idx, i;
    u8 conn = m->connection_state;
    /* A peer-to-peer session with menus: other words for the same screen (no code, no chat). */
    int p2p = mu_online_p2p_menus();
    /* ... and its opponent is not connected yet. The host reports 3 here; 0 is the table before
     * its first B3, where "enter code" would be just as wrong. */
    int p2p_connecting = p2p && conn < MU_SLP_MM_CONNECTION_SUCCESS;
    /* A lobby's line ("Lobby of <host>", "Hosting - 2 waiting", or why it ended), shim/mu_lobby_list.c. */
    const char* lobby_line = p2p ? NULL : mu_lobby_css_frame(conn);

    (void) gobj;
    pick_preview_frame();
    if (ctrl_port != NULL) {
        mu_host_button_label(*ctrl_port, HSD_PAD_Z, z_key, sizeof z_key);
        mu_host_button_label(*ctrl_port, HSD_PAD_B, b_key, sizeof b_key);
        mu_host_button_label(*ctrl_port, HSD_PAD_START, start_key, sizeof start_key);
    }
    code_sjis18((u8*) opp_code);

    /* Mode title: slpCSS mode matanim on SingleMenu child 36, MELEE (0) or TEAM MATCH (16). */
    if (dt.slpcss != NULL && single_menu != NULL && *single_menu != NULL) {
        HSD_JObj* j = NULL;
        lb_80011E24(*single_menu, &j, 36, -1);
        HSD_JObjRemoveAnim(j);
        HSD_JObjAddAnim(j, NULL, DP(dt.slpcss->mode), NULL);
        HSD_JObjReqAnim(j, st->mode == MU_SLP_MODE_TEAMS ? 16.0f : 0.0f);
        HSD_JObjAnim(j);
    }

    /* Header */
    if (conn > MU_SLP_MM_CONNECTION_SUCCESS) {
        if (st->mode == MU_SLP_MODE_RANKED) {
            HSD_SisLib_803A7548(dt.text, ST_HEADER, 0.5f, 0.5f);   /* the Ranked header may have shrunk */
        }
        set_text(ST_HEADER, "Error");
    } else {
        switch (st->mode) {
        case MU_SLP_MODE_UNRANKED:
            set_header("Slippi", "Unranked"); break;
        case MU_SLP_MODE_DIRECT:
            if (p2p) {
                set_text_fmt(ST_HEADER, "%s Mode", "P2P");
            } else {
                set_header("Slippi", "Direct");
            }
            break;
        case MU_SLP_MODE_RANKED: set_header("Slippi", "Ranked"); break;
        case MU_SLP_MODE_TEAMS:
            set_header("Slippi", "Teams");
            break;
        case MU_SLP_MODE_PARTY: set_header("Slippi", "Party"); break;
        default: set_text(ST_HEADER, "Error"); break;
        }
    }

    if (st->mode == MU_SLP_MODE_RANKED && mu_slippi_ranked_available()) {
        for (i = 0; i < 5; i++) {
            set_text(ST_RANK_OPPONENT + i, "");
        }
    }

    /* Playing label, opponent name, D-pad hint. Connected in Teams the other players have a line
     * each under the label (playing_lines), so no name goes beside it. */
    memcpy(opp_name, m->opp_name, 31);
    opp_name[31] = 0;
    set_text(ST_PLAYING_LABEL, conn == MU_SLP_MM_CONNECTION_SUCCESS ? "Playing:" :
                             conn == MU_SLP_MM_OPPONENT_CONNECTING ? "Found:" : "");
    HSD_SisLib_803A746C(dt.text, ST_PLAYING_OPP,
                        conn == MU_SLP_MM_OPPONENT_CONNECTING ? FOUND_NAME_X : PLAYING_NAME_X, PLAYING_Y);
    set_text(ST_PLAYING_OPP, (conn == MU_SLP_MM_CONNECTION_SUCCESS && st->mode != MU_SLP_MODE_TEAMS) ||
                           conn == MU_SLP_MM_OPPONENT_CONNECTING ? opp_name : "");
    if (conn == MU_SLP_MM_CONNECTION_SUCCESS && !p2p) {
        /* A keyboard has no D-pad: name its four chat keys (up, left, right, down) instead. */
        char du[16], dl[16], dr[16], dd[16], chat[80];
        if (ctrl_port != NULL && mu_host_button_label(*ctrl_port, HSD_PAD_DPADUP, du, sizeof du) &&
            mu_host_button_label(*ctrl_port, HSD_PAD_DPADLEFT, dl, sizeof dl) &&
            mu_host_button_label(*ctrl_port, HSD_PAD_DPADRIGHT, dr, sizeof dr) &&
            mu_host_button_label(*ctrl_port, HSD_PAD_DPADDOWN, dd, sizeof dd)) {
            snprintf(chat, sizeof chat, "Chat: %s %s %s %s", du, dl, dr, dd);
            set_text(ST_PRESS_D, chat);
        } else {
            set_text(ST_PRESS_D, "Use D-Pad to Chat");
        }
    } else {
        set_text(ST_PRESS_D, lobby_line != NULL ? lobby_line : "");
    }
    if (st_opp_picks >= 0) {
        char lines[PLAYING_LINES][PLAYING_LINE_SIZE];
        if (conn == MU_SLP_MM_CONNECTION_SUCCESS) {
            playing_lines(m, st->mode == MU_SLP_MODE_TEAMS, lines);
        } else {
            for (i = 0; i < PLAYING_LINES; i++) lines[i][0] = 0;
        }
        for (i = 0; i < PLAYING_LINES; i++) {
            /* A long line (a squad of long fighter names) is narrowed to stay on screen. */
            const size_t length = strlen(lines[i]);
            const f32 wide = length > PLAYING_FIT ? PLAYING_SIZE * (f32) PLAYING_FIT / (f32) length : PLAYING_SIZE;
            HSD_SisLib_803A7548(dt.text, st_opp_picks + i, wide, PLAYING_SIZE);
            set_text(st_opp_picks + i, lines[i]);
        }
    }

    /* Press / hold Z (the keyboard player's key for it) */
    prompt[0] = 0;
    if (conn == MU_SLP_MM_CONNECTION_SUCCESS) {
        snprintf(prompt, sizeof prompt, "Hold %s to disconnect", z_key);
    } else if (conn == MU_SLP_MM_ERROR) {
        snprintf(prompt, sizeof prompt, "Press %s to clear error", z_key);
    } else if (conn > MU_SLP_MM_IDLE) {
        snprintf(prompt, sizeof prompt, "Press %s to cancel", z_key);
    }
    set_text(ST_PRESS_Z, prompt);

    /* Clear lines, spinners and error lines */
    for (idx = ST_LINE1; idx <= ST_LINE3; idx += 2) {
        set_text(idx, "");
    }
    dt.spinner[0] = dt.spinner[1] = dt.spinner[2] = 0;
    for (idx = ST_ERR1; idx <= ST_ERR4; idx++) {
        set_text(idx, "");
    }

    if (conn == MU_SLP_MM_ERROR
    ) {
        /* Wrap the error at 30 letters on the last space (Shift-JIS pairs count as one). The line
         * index state carries over between lines exactly as the asm's registers do. */
        u8 buf[241 + 4];
        int sub = ST_ERR1, line_idx = 0, line_len = 0, last_space = 0, start = 0;
        u8 c;
        memset(buf, 0, sizeof buf);
        memcpy(buf, m->error, 241);
        for (;;) {
            int set = 0;
            int pos = start + line_idx;
            c = pos < (int) sizeof buf ? buf[pos] : 0;
            if (c & 0x80) {
                line_idx += 2;
                line_len++;
            } else {
                if (c == 0x20) {
                    last_space = line_idx;
                }
                line_idx++;
                line_len++;
            }
            if (c == 0) {
                set = 1;
            } else if (line_len > 30) {
                if (start + last_space < (int) sizeof buf) {
                    buf[start + last_space] = 0;
                }
                set = 1;
            }
            if (set) {
                set_text(sub, start < (int) sizeof buf ? (const char*) &buf[start] : "");
                sub++;
                start += last_space + 1;
                line_idx = 0;
                line_len = 0;
            }
            if (c == 0 || start + line_idx >= 241 || sub > ST_ERR4) {
                break;
            }
        }
    } else {
        /* Custom fighters online: why the fighter under the hand is greyed out ("P2 does not have
         * Slippy installed."), or why this player's pick was unlocked, takes the last free line, as
         * Melee's own status text. */
        int cspin = 0;
        const char* cline = p2p ? "" : content_line(conn == MU_SLP_MM_CONNECTION_SUCCESS, &cspin);
        idx = ST_LINE1;
        if (*char_chosen == 0) {
            set_text(idx, "Select your character");
            dt.spinner[0] = 1;
            if (p2p_connecting) {
                /* The fighter can be chosen meanwhile; the second line says what is awaited. */
                set_text_fmt(idx + 2, "Connecting to %s", "opponent");
                dt.spinner[1] = 1;
            } else if (cline[0]) {
                set_text(idx + 2, cline);
                dt.spinner[1] = cspin;
            }
            goto spinners;
        }
        set_text(idx, "Character selected");
        idx += 2;
        dt.spinner[0] = 2;

        if (p2p_connecting) {
            /* In place of "Press START to enter code": START does nothing until connected. */
            set_text_fmt(idx, "Connecting to %s", "opponent");
            dt.spinner[1] = 1;
            goto spinners;
        }
        if (m->local_ready == 0) {
            const char* action;
            if ((st->mode == MU_SLP_MODE_DIRECT || st->mode == MU_SLP_MODE_TEAMS) &&
                conn == MU_SLP_MM_CONNECTION_SUCCESS && st->is_winner == MU_SLP_WINNER_LOST &&
                st->chose_stage == 0)
            {
                action = "select stage";
            } else if (conn == MU_SLP_MM_CONNECTION_SUCCESS) {
                action = "lock in";
            } else if ((st->mode == MU_SLP_MODE_DIRECT || st->mode == MU_SLP_MODE_TEAMS) &&
                       !mu_lobby_css_staged()) {
                action = "enter code";
            } else {
                action = "search";
            }
            snprintf(prompt, sizeof prompt, "Press %s to %s", start_key, action);
            set_text(idx, prompt);
            dt.spinner[1] = 1;
            if (cline[0]) {
                set_text(idx + 2, cline);
                dt.spinner[2] = cspin;
            }
            goto spinners;
        }
        set_text(idx, "Locked in");
        idx += 2;
        dt.spinner[1] = 2;

        {
            /* p2p: "opponent" too, there is no typed code to show. */
            int generic = st->mode == MU_SLP_MODE_UNRANKED || st->mode == MU_SLP_MODE_RANKED ||
                          st->mode == MU_SLP_MODE_PARTY || p2p;
            if (conn == MU_SLP_MM_CONNECTION_SUCCESS && cline[0]) {
                set_text(idx, cline);
            } else if (conn == MU_SLP_MM_CONNECTION_SUCCESS) {
                set_text_fmt(idx, "Waiting on %s", opp_name[0] ? opp_name : "opponent");
            } else if (conn == MU_SLP_MM_OPPONENT_CONNECTING) {
                set_text_fmt(idx, "Connecting to %s", opp_name[0] ? opp_name :
                             generic ? "opponent" : opp_code);
            } else {
                set_text_fmt(idx, "Searching for %s", generic ? "opponent" : opp_code);
            }
        }
        dt.spinner[2] = 1;
    }

spinners:
    for (i = 0, idx = ST_SPIN1; i < 3; i++, idx += 2) {
        if (dt.spinner[i] == 1) {
            GXColor c = spinner_wait_color;
            s = spinner_str[(dt.frame_counter / SPINNER_TRANSITION_FRAMES) & 1];
            set_text(idx, s);
            HSD_SisLib_803A74F0(dt.text, idx, &c);
        } else if (dt.spinner[i] == 2) {
            GXColor c = spinner_done_color;
            set_text(idx, spinner_done_str);
            HSD_SisLib_803A74F0(dt.text, idx, &c);
        } else {
            set_text(idx, "");
        }
    }

    dt.frame_counter++;
    if (dt.frame_counter >= FRAME_MAX) {
        dt.frame_counter = 0;
    }
}

/* LoadCSSText's init (mnCharSel_802640A0, after the SingleMenu joint loads). The portrait BG word
 * (mnCharSel_804D50D8) is reset by the caller. */
void mu_slippi_css_text_init(void)
{
    HSD_GObj* gobj;
    void* ud;
    HSD_Text* t;
    int i;

    /* Mode 2 adds the "Connect Code" rows; a peer-to-peer session has no code to show. */
    user_display_init(mu_slippi_state()->mode == MU_SLP_MODE_DIRECT && !mu_online_p2p_menus() ? 2 : 1);

    gobj = GObj_Create(4, 5, 0x80);
    ud = HSD_MemAlloc(4);
    GObj_InitUserData(gobj, 4, HSD_Free, ud);
    HSD_GObj_SetupProc(gobj, text_think, 4);

    t = text_struct(0.0f, 0.1f);
    dt.text = t;

    create_subtext(t, &color_white, 0.5f, 70.0f, 23.0f);                  /* header */
    {
        static const f32 line_y[3] = { 52.0f, 75.0f, 98.0f };
        for (i = 0; i < 3; i++) {
            create_subtext(t, &color_white, 0.4f, 90.0f, line_y[i]);      /* line */
            create_subtext(t, &color_white, 0.45f, 70.0f, line_y[i]);     /* spinner */
        }
    }
    create_subtext(t, &color_gray, 0.4f, 70.0f, 132.5f);                  /* press Z */
    create_subtext(t, &color_gray, 0.4f, 70.0f, 152.5f);                  /* use D-pad */
    /* "Playing:" and the opponent: under the chat line, in the block's own column and line size. */
    create_subtext(t, &color_gray, 0.4f, PLAYING_X, PLAYING_Y);           /* Playing: */
    create_subtext(t, &color_white, 0.4f, PLAYING_NAME_X, PLAYING_Y);     /* opponent */
    {
        static const f32 err_y[4] = { 52.0f, 70.0f, 88.0f, 106.0f };
        for (i = 0; i < 4; i++) {
            create_subtext(t, &color_red, 0.4f, 70.0f, err_y[i]);
        }
    }
    if (mu_slippi_state()->mode == MU_SLP_MODE_RANKED && mu_slippi_ranked_available()) {
        for (i = 0; i < 5; i++) {
            create_subtext(t, &color_white, 0.32f, 70.0f, 178.0f + (float) i * 18.0f);
        }
    }
    for (i = 0; i < PLAYING_LINES; i++) {                                 /* the lines under "Playing:" */
        const int idx = create_subtext(t, &color_white, PLAYING_SIZE, PLAYING_X, PLAYING_Y + 18.0f + 15.0f * (f32) i);
        if (i == 0) st_opp_picks = idx;
    }
    mu_online_options_scene();   /* a new select: the Options bar is built on its first frame */
}

/* ---- chat (mu_slippi_chat.c): the CSS data table as SlippiCSS.dat reads it ---- */

const MuMatchState* mu_slippi_css_msrb(void)
{
    return &dt.msrb;
}

void* mu_slippi_css_slpcss(void)
{
    return dt.slpcss;
}

void mu_slippi_css_set_chat_open(int open)
{
    dt.chat_window_opened = (u8) (open != 0);
}

int mu_slippi_css_chat_open(void)
{
    return dt.chat_window_opened != 0;
}

/* mnCharSel_804D6CF6: nonzero while the CSS leaves or hands over to name entry. */
int mu_slippi_css_scene_request(void)
{
    return scene_request != NULL ? *scene_request : 0;
}

/* mnCharSel_804D6CF0 read as a byte, as the module does. */
int mu_slippi_css_port(void)
{
    return ctrl_port != NULL ? (u8) *ctrl_port : 0;
}
