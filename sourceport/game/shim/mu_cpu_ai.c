#include "mu_test_env.h"
/* The CPU AI picker. On the VS character select a human's hand points at a CPU panel's name plate and
 * presses A, as for a name tag: the panel's own name list opens with the AI styles in place of the
 * tags, driven by that player's pad, the hand kept in it as for a name tag. The pick shows in the name
 * plate the way a tag does. In an offline VS match each CPU then plays its port's pick:
 *   Default            Melee's own CPU, untouched (or the 20XX AI when "20XX CPUs" is on for all)
 *   Random DI          Melee's own CPU at the panel's level, DIing as the 20XX Hack Pack's "CPU DI:
 *                      Random" (shim/mu_20xx_cpu.c); always Melee's CPU, even with "20XX CPUs" on
 *   Punching bag, Aerial spammer, Runaway camper, Shield and punish
 *                      the Practice CPU's presets (shim/mu_practice_cpu.c), DIing as the 20XX Hack
 *                      Pack's "CPU DI: Random" (shim/mu_20xx_cpu.c); the CPU level does not apply, so
 *                      the panel's level slider is hidden while one is picked
 *   20XX AI            the 20XX Hack Pack CPU (shim/mu_20xx_ai.c) for this CPU only; it reads the level
 * The list shows them in that order (list_order); a style's number is its place in styles[].
 *
 * The match's choices go to the host as it starts (command 0xE7 op 2, before the recording's Game Start)
 * and ride in the replay ("muCpuAi", four bits a port). Playback gets the recorded word back, so the
 * same code runs on the same fighters and consumes the game's random numbers as it did (the recorded
 * inputs then replace the controller, as for any CPU). A port's last choice is kept in the settings
 * file (op 1, ini key cpu_ai). Never online, never with the vanilla game.
 *
 * Hooks, kept small: mn/mncharsel.c (scene enter, the tag list's think, the hand's limits and actions,
 * the A press on a name plate, the grab of a hidden level slider), gm/gmvs.c fn_8016E730 (the match
 * begins), shim/mu_practice_cpu.c (each fighter's input), shim/mu_20xx_ai.c (who plays the 20XX AI)
 * and shim/mu_20xx_cpu.c (who DIs the 20XX way). */
#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/forward.h>
#include <melee/ft/types.h>
#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/kinds/ftCommon/ftCo_0A01.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/types.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lblanguage.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/mn/forward.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/mnname.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/sislib.h>

#include "mu_native.h"
#include "mu_practice_cpu.h"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);

/* MU_CMD_CPU_AI (mu_commands.h): op 0: the remembered word; op 1 port, style; op 2 word: a match begins */

/* ---- the registry ---- */

enum {
    STYLE_DEFAULT,
    STYLE_PUNCHING_BAG,
    STYLE_AERIAL_SPAMMER,
    STYLE_RUNAWAY_CAMPER,
    STYLE_SHIELD_PUNISH,
    STYLE_20XX,
    STYLE_RANDOM_DI,
    STYLE_COUNT   /* at most 16: four bits a port */
};

typedef struct CpuAiStyle {
    unsigned char id;           /* in the settings file and in replays: never renumbered */
    const char* name;           /* the list row and the name plate */
    signed char preset;         /* the Practice CPU preset it plays, -1 none */
    unsigned char uses_level;   /* plays on Melee's CPU, so the CPU level slider counts */
    unsigned char one_on_one;   /* understands one opponent only: Default when there are more */
    unsigned char installed;    /* also for installed fighters (Wolf, Shulk...), not only the 26 of the disc */
    unsigned char di_20xx;      /* the 20XX Hack Pack's "CPU DI" mode it DIs with (mu_20xx_cpu.c), 0 its own */
    void (*on_select)(int port);
    void (*on_match_start)(int port);
    int (*on_frame)(Fighter* fp, int port, MuLabInput* out);   /* nonzero: it set the controller */
} CpuAiStyle;

static void practice_start(int port);
static int practice_frame(Fighter* fp, int port, MuLabInput* out);

/* The play styles DI the way the 20XX Hack Pack's "CPU DI: Random" does: a new random direction at
 * every hit. The 20XX AI reads the CPU level (mu_20xx_ai.c level_roll), so its slider stays. */
#define DI_20XX_RANDOM 2
static const CpuAiStyle styles[STYLE_COUNT] = {
    { STYLE_DEFAULT, "Default", -1, 1, 0, 1, 0, NULL, NULL, NULL },
    { STYLE_PUNCHING_BAG, "Punching bag", 1, 0, 0, 1, DI_20XX_RANDOM, NULL, practice_start, practice_frame },
    { STYLE_AERIAL_SPAMMER, "Aerial spammer", 2, 0, 0, 1, DI_20XX_RANDOM, NULL, practice_start, practice_frame },
    { STYLE_RUNAWAY_CAMPER, "Runaway camper", 3, 0, 0, 1, DI_20XX_RANDOM, NULL, practice_start, practice_frame },
    /* reacts to one opponent's attacks: with several it would shield only one of them */
    { STYLE_SHIELD_PUNISH, "Shield and punish", 5, 0, 1, 1, DI_20XX_RANDOM, NULL, practice_start, practice_frame },
    /* the 20XX AI refuses fighters past the disc's own (mu_20xx_ai: kind >= 0x1B) */
    { STYLE_20XX, "20XX AI", -1, 1, 0, 0, 0, NULL, NULL, NULL },
    /* Melee's CPU as it is, at its level; only the DI at the end of each hit is 20XX's random one */
    { STYLE_RANDOM_DI, "Random DI", -1, 1, 0, 1, DI_20XX_RANDOM, NULL, NULL, NULL },
};

/* The list's rows under Default, top to bottom: Random DI right after Default. */
static const unsigned char list_order[] = {
    STYLE_RANDOM_DI, STYLE_PUNCHING_BAG, STYLE_AERIAL_SPAMMER, STYLE_RUNAWAY_CAMPER, STYLE_SHIELD_PUNISH, STYLE_20XX,
};
_Static_assert(sizeof list_order == STYLE_COUNT - 1, "every style but Default has a row");

/* External character id (CharacterKind), -1 when the panel has no fighter yet. */
static int style_supports(int style, int ckind)
{
    if (style <= STYLE_DEFAULT || style >= STYLE_COUNT) {
        return 1;
    }
    return ckind < 0 || ckind < CKind_Playable_Count || styles[style].installed;
}

static int style_of(unsigned int word, int port)
{
    const int style = (int) ((word >> (4 * port)) & 0xFu);
    return style < STYLE_COUNT ? style : STYLE_DEFAULT;
}

static int offline(void)
{
    return !(mu_game_options() & MU_OPTION_VANILLA) && !mu_online_active() && !mu_online_pending() &&
           !mu_slippi_in_online_mode();
}

static int vs_mode(void)
{
    switch (gm_GetCurrentGameMode()) {
    case GM_VS:
    case GM_DEBUG_VS:
    case GM_SUPER_SUDDEN_DEATH_VS:
    case GM_INVISIBLE_VS:
    case GM_SLOMO_VS:
    case GM_LIGHTNING_VS:
    case GM_TINY_VS:
    case GM_GIANT_VS:
    case GM_STAMINA_VS:
    case GM_CAMERA_VS:
    case GM_SINGLE_BUTTON_VS:
        return 1;
    default:
        return 0;
    }
}

static unsigned char host_reply[4096];   /* the host's minimum reply capacity */

static unsigned int host_command(const unsigned char* payload, unsigned int size, int* ok)
{
    unsigned int got = 0;
    *ok = 0;
    if (mu_online_abi_command(MU_CMD_CPU_AI, payload, size, host_reply, sizeof host_reply, &got) != 0 || got != 4) {
        return 0;
    }
    *ok = 1;
    return (unsigned int) host_reply[0] << 24 | (unsigned int) host_reply[1] << 16 |
           (unsigned int) host_reply[2] << 8 | host_reply[3];
}

/* ---- the match ---- */

/* Game data, so a replay viewer's jump back and its snapshots keep them. */
static unsigned int match_word;           /* the choices this match plays with */
static int match_resolved;                /* the fallbacks below are worked out (first fighter input) */
static unsigned char match_style[4];      /* each port's style after them */
static MuPcConfig match_pc[4];
static MuPcState match_state[4];
static void* match_owner[4];
static int match_logs;

static unsigned char css_choice[4];       /* each port's pick, as the name plates show it */
static int css_loaded;

static void load_choices(void)
{
    const unsigned char op = 0;
    int ok, i;
    const unsigned int word = host_command(&op, 1, &ok);
    for (i = 0; i < 4; i++) {
        css_choice[i] = (unsigned char) style_of(ok ? word : 0u, i);
    }
    css_loaded = 1;
}

void mu_cpu_ai_match_begin(struct StartMeleeData* data)
{
    unsigned char payload[5];
    unsigned int word = 0;
    int i, ok;
    const int vs = vs_mode() && offline();
    match_resolved = 0;
    match_word = 0;
    for (i = 0; i < 4; i++) {
        match_style[i] = STYLE_DEFAULT;
        match_owner[i] = NULL;
    }
    if (vs && data != NULL) {
        if (!css_loaded) {
            load_choices();
        }
        for (i = 0; i < 4; i++) {
            if (data->players[i].slot_type == Gm_PKind_Cpu) {
                word |= (unsigned int) css_choice[i] << (4 * i);
            }
        }
    }
    /* Every match, so the host's recording never carries the last match's word. In playback the host
     * answers with the replay's word; online and with the vanilla game with 0. */
    payload[0] = 2;
    payload[1] = (unsigned char) (word >> 24);
    payload[2] = (unsigned char) (word >> 16);
    payload[3] = (unsigned char) (word >> 8);
    payload[4] = (unsigned char) word;
    word = host_command(payload, sizeof payload, &ok);
    match_word = vs && ok ? word & 0xFFFFu : 0u;
}

/* Players on another team (anyone else outside Teams) in this match. */
static int opponents(int port)
{
    const int teams = gm_8016B168();
    int k, n = 0;
    for (k = 0; k < 6; k++) {
        if (k == port || Player_GetPlayerSlotType(k) == Gm_PKind_NA) {
            continue;
        }
        if (!teams || Player_GetTeam(k) != Player_GetTeam(port)) {
            n++;
        }
    }
    return n;
}

/* Once per match, at the first fighter's input: a port is a CPU, and a one-on-one style has one
 * opponent, else Default. In playback this reads the replay's players, so it decides as it did. */
static void resolve(void)
{
    int port;
    if (match_resolved) {
        return;
    }
    match_resolved = 1;
    for (port = 0; port < 4; port++) {
        int style = style_of(match_word, port);
        if (style != STYLE_DEFAULT && Player_GetPlayerSlotType(port) != Gm_PKind_Cpu) {
            style = STYLE_DEFAULT;
        }
        if (style != STYLE_DEFAULT && styles[style].one_on_one && opponents(port) > 1) {
            OSReport("[cpu ai] P%d: %s plays one opponent only; %d here, so Default\n", port + 1,
                     styles[style].name, opponents(port));
            style = STYLE_DEFAULT;
        }
        match_style[port] = (unsigned char) style;
        if (styles[style].on_match_start != NULL) {
            styles[style].on_match_start(port);
        }
        if (match_word != 0 && Player_GetPlayerSlotType(port) == Gm_PKind_Cpu) {
            OSReport("[cpu ai] P%d: %s%s (game mode %d)\n", port + 1, styles[style].name,
                     styles[style].preset >= 0 ? ", the Practice CPU's preset with 20XX random DI"
                     : style == STYLE_20XX ? ", this CPU only"
                     : styles[style].di_20xx ? ", Melee's CPU at its level with 20XX random DI" : ", Melee's CPU",
                     (int) gm_GetCurrentGameMode());
        }
    }
}

/* This port's style now: Default outside an offline VS match, for a fighter it does not support
 * (Squad Strike: the pick stays with the port and comes back with a fighter it supports). */
static int current_style(int port)
{
    int style;
    if (port < 0 || port > 3 || match_word == 0 || !vs_mode() || !offline()) {
        return STYLE_DEFAULT;
    }
    resolve();
    style = match_style[port];
    if (style != STYLE_DEFAULT && !style_supports(style, (int) Player_GetPlayerCharacter(port))) {
        return STYLE_DEFAULT;
    }
    return style;
}

int mu_cpu_ai_any_20xx(void)
{
    int port;
    if (match_word == 0) {
        return 0;
    }
    for (port = 0; port < 4; port++) {
        if (current_style(port) == STYLE_20XX) {
            return 1;
        }
    }
    return 0;
}

int mu_cpu_ai_wants_20xx(void* fighter, int global)
{
    const Fighter* fp = fighter;
    int style;
    if (fp == NULL) {
        return global;
    }
    style = current_style(fp->player_idx);
    if (style == STYLE_20XX) {
        return 1;
    }
    return style == STYLE_DEFAULT ? global : 0;   /* a Practice style owns this CPU, Random DI keeps Melee's */
}

/* The opponent, as the 20XX AI picks it (mu_20xx_ai.c find_opponent): from the player who last hit
 * this fighter (else the first fighter), the first one down the fighter list that is another player's,
 * not asleep, and not a team mate. */
static Fighter* find_opponent(Fighter* fp)
{
    HSD_GObj* gobj = NULL;
    const int slot = fp->dmg.x18c4_source_ply;
    if (slot >= 0 && slot < 6) {
        gobj = Player_GetPtrForSlot(slot)->player_entity[0];
    }
    if (gobj == NULL) {
        gobj = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER];
    }
    for (; gobj != NULL; gobj = gobj->next) {
        Fighter* other = gobj->user_data;
        if (other == NULL || other == fp || other->player_idx == fp->player_idx ||
            other->motion_id == ftCo_MS_Sleep)
        {
            continue;
        }
        if (gm_GetCurrentGameMode() == GM_GIANT_VS || !gm_8016B168() || other->team != fp->team) {
            return other;
        }
    }
    /* Started at the last attacker and found nobody after it: look from the list's head once more. */
    if (slot >= 0 && slot < 6) {
        for (gobj = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_FIGHTER]; gobj != NULL; gobj = gobj->next) {
            Fighter* other = gobj->user_data;
            if (other == NULL || other == fp || other->player_idx == fp->player_idx ||
                other->motion_id == ftCo_MS_Sleep)
            {
                continue;
            }
            if (gm_GetCurrentGameMode() == GM_GIANT_VS || !gm_8016B168() || other->team != fp->team) {
                return other;
            }
        }
    }
    return NULL;
}

/* The Practice CPU's preset for this port. Its own DI is off (the stick neutral through hitlag): the
 * 20XX Hack Pack's DI hook sets the stick the game's DI reads (mu_cpu_ai_di_mode). */
static void practice_start(int port)
{
    MuPcConfig* c = &match_pc[port];
    __builtin_memset(c, 0, sizeof *c);
    c->enabled = 1;
    mu_pc_preset(styles[match_style[port]].preset, c);
    c->enabled = 1;
    c->tools = 0;
    c->rng_mode = MU_PC_RNG_GAME;   /* the model's own numbers; the game's seed is never written */
    __builtin_memset(c->di_odds, 0, sizeof c->di_odds);
    c->di_odds[MU_PC_DI_NONE] = 100;
    /* Knocked off the stage it comes back, high or low at random each time, and does nothing as hitstun
     * ends: the presets' nair or air dodge there left too little time, or no way back (measured on Final
     * Destination with Fox, Marth and Captain Falcon). The punching bag never attacks: no nair after a
     * hit, no getup or ledge attack. */
    c->recovery = MU_PC_RECOVER_MIX;
    __builtin_memset(c->after_odds, 0, sizeof c->after_odds);
    c->after_odds[MU_PC_AFTER_NOTHING] = 100;
    if (match_style[port] == STYLE_PUNCHING_BAG) {
        c->no_attacks = 1;
    }
    match_owner[port] = NULL;   /* a fresh model state on the first frame */
}

/* mu_20xx_cpu.c mu_20xx_cpu_di, at each DI calculation: the 20XX "CPU DI" mode for this fighter's
 * style when the Game tab's own "CPU DI" is off, else 0. In playback the replay's word gives the same
 * style, so the hook takes the game's random numbers there as it did. */
int mu_cpu_ai_di_mode(void* fighter)
{
    const Fighter* fp = fighter;
    if (fp == NULL || match_word == 0) {
        return 0;
    }
    return styles[current_style(fp->player_idx)].di_20xx;
}

static int practice_frame(Fighter* fp, int port, MuLabInput* out)
{
    return mu_practice_cpu_drive_state(fp, find_opponent(fp), &match_pc[port], &match_state[port],
                                       &match_owner[port], out);
}

/* MELEE_CPU_AI_TRACE=1 (tests): each CPU's state every second, to compare the styles. */
static void trace(Fighter* fp, int style)
{
    static int on = -1;
    const int frame = (int) gm_GetFrameCount();
    if (on < 0) {
        on = mu_test_getenv("MELEE_CPU_AI_TRACE") != NULL;
    }
    if (!on || frame <= 0 || frame % 60 != 0 || match_logs > 2000) {
        return;
    }
    match_logs++;
    OSReport("[cpu ai] frame %d P%d %s: motion %d x %.1f y %.1f damage %.0f%s%s\n", frame, fp->player_idx + 1,
             styles[style].name, (int) fp->motion_id, fp->cur_pos.x, fp->cur_pos.y, fp->dmg.x1830_percent,
             styles[style].preset >= 0 ? " plan " : "",
             styles[style].preset >= 0 ? mu_pc_plan_name(match_state[fp->player_idx].plan) : "");
}

int mu_cpu_ai_frame(void* fighter, MuLabInput* out)
{
    Fighter* fp = fighter;
    int style;
    if (fp == NULL || match_word == 0 || fp->is_sub_fighter || !ftCo_IsCpuControlled(fp)) {
        return 0;
    }
    style = current_style(fp->player_idx);
    trace(fp, style);
    if (styles[style].on_frame == NULL) {
        return 0;
    }
    return styles[style].on_frame(fp, fp->player_idx, out);
}

/* ---- the character select ---- */

static struct {
    CSSTag* tags;
    CSSDoor* doors;
    HSD_JObj** root;
    CSSIcon** icons;
    unsigned char* icon_count;
    CSSData* data;
    const unsigned char* gnw_name;
    int active;
} css;
static signed char list_owner[4] = { -1, -1, -1, -1 };   /* the hand driving door i's list */
static unsigned char list_rows[4][STYLE_COUNT];           /* the styles on its rows 1.. */
static unsigned char list_count[4];
static float hand_y[4];
static signed char plate_shown[4] = { -1, -1, -1, -1 };   /* the style the plate shows, -1 none of ours */
static unsigned char prev_kind[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static unsigned char level_hidden[4];   /* the door's level slider is hidden by this file */

static const GXColor color_white = { 255, 255, 255, 255 };
static const GXColor color_gray = { 100, 100, 100, 255 };
/* The list's second row as the scene builds it ("NAME ENTRY"), put back when the AI list closes. */
static char name_entry_text[] = "\x82\x6d\x82\x60\x82\x6c\x82\x64\x20\x82\x64\x82\x6d\x82\x73\x82\x71\x82\x78";

void mu_cpu_ai_css_enter(struct CSSTag* tags, struct CSSDoor* doors, struct HSD_JObj** root, struct CSSIcon** icons,
                         unsigned char* icon_count, struct CSSData* data, const unsigned char* gnw_name, int door_count)
{
    int i;
    css.tags = tags;
    css.doors = doors;
    css.root = root;
    css.icons = icons;
    css.icon_count = icon_count;
    css.data = data;
    css.gnw_name = gnw_name;
    css.active = door_count == 4 && data != NULL && data->match_type <= VS_SLOWMO && offline() &&
                 !mu_slippi_css_online();
    for (i = 0; i < 4; i++) {
        list_owner[i] = -1;
        plate_shown[i] = -1;
        prev_kind[i] = 0xFF;
        level_hidden[i] = 0;   /* a new scene: its joints carry no flag of ours */
    }
    if (css.active) {
        load_choices();
    }
}

static int door_fighter(int door)
{
    const unsigned int sel = css.doors[door].sel_icon;
    if (sel >= *css.icon_count) {
        return -1;
    }
    return (*css.icons)[sel].char_kind;
}

static int joint_pos(int joint, Vec3* out)
{
    HSD_JObj* jobj = NULL;
    if (css.root == NULL || *css.root == NULL) {
        return 0;
    }
    lb_80011E24(*css.root, &jobj, joint, -1);
    if (jobj == NULL) {
        return 0;
    }
    lb_8000B1CC(jobj, NULL, out);
    return 1;
}

static void set_choice(int door, int style)
{
    unsigned char payload[3];
    int ok;
    css_choice[door] = (unsigned char) style;
    payload[0] = 1;
    payload[1] = (unsigned char) door;
    payload[2] = (unsigned char) style;
    host_command(payload, sizeof payload, &ok);
    if (styles[style].on_select != NULL) {
        styles[style].on_select(door);
    }
}

/* A row of the list, narrowed to the box. The list text fits a line wider than its box (154 units)
 * to exactly the box, but a row starts 10 units in, so a long pick lost its last letters; a row is
 * narrowed below that instead (the list draws a letter 32 units wide and a space 16). The entry keeps
 * the scale until it is set again, so the game's own names get the text's scale back (name NULL). */
static void fit(HSD_Text* text, int entry, const char* name)
{
    const float room = 128.0f;
    float width = 0.0f, squeeze = 1.0f;
    for (; name != NULL && *name != '\0'; name++) {
        width += *name == ' ' ? 16.0f : 32.0f;
    }
    if (width > room) {
        squeeze = room / width;
    }
    HSD_SisLib_803A7548(text, entry, text->x34.x * squeeze, text->x34.y);
}

/* The plate as the game draws it without a pick: the fighter's name, or a name tag the player kept. */
static void plate_fighter(int door, CSSTagData* tag)
{
    const unsigned int sel = css.doors[door].sel_icon;
    const int nametag = css.data->vs.start.players[door].nametag;
    if (nametag != 0x78) {
        HSD_SisLib_803A70A0(tag->text, 0, GetNameText(nametag));
        tag->text->default_kerning = 0;
        tag->text->hidden = css.doors[door].p_kind == 3;
        tag->use_tag = 1;
        return;
    }
    tag->use_tag = 0;
    if (sel < *css.icon_count) {
        const int ckind = (*css.icons)[sel].char_kind;
        if (lbLang_IsSavedLanguageUS() && ckind == CKind_GameWatch) {
            HSD_SisLib_803A70A0(tag->text, 0, (char*) css.gnw_name);
        } else {
            HSD_SisLib_803A70A0(tag->text, 0, (char*) gm_80160980((u8) ckind));
        }
        tag->text->default_kerning = 1;
        tag->text->hidden = css.doors[door].p_kind == 3;
    } else {
        tag->text->hidden = 1;
    }
}

/* A pick in the plate, the way a name tag shows there. */
static void plate_style(CSSTagData* tag, int style)
{
    HSD_SisLib_803A70A0(tag->text, 0, (char*) styles[style].name);
    tag->text->default_kerning = 1;
    tag->text->hidden = 0;
    tag->use_tag = 1;
}

/* A CPU panel whose pick ignores the CPU level: the level is hidden and its knob cannot be taken
 * (mncharsel.c asks mu_cpu_ai_level_locked before a hand grabs it). */
static int level_unused(int door)
{
    return css.active && door >= 0 && door <= 3 && css.doors[door].p_kind == 1 &&
           !styles[css_choice[door]].uses_level;
}

int mu_cpu_ai_level_locked(int door)
{
    return level_unused(door);
}

/* The panel's level slider hidden or shown. Without handicap that is the whole slider (its "CPU Level"
 * label, bar and numbered knob); with handicap the panel also shows the handicap there, so only the
 * level's knob goes. The game shows and hides the slider by its animation frame, never by this flag,
 * so only what this file hid is shown again. */
static void level_slider(int door, int hide)
{
    unsigned char joints[2];
    int i, n = 0;
    if (gmMainLib_GetGameRules()->handicap != 0) {
        joints[n++] = css.doors[door].cpuslider2_joint;
    } else {
        joints[n++] = css.doors[door].slidername_joint;
        joints[n++] = css.doors[door].cpuslider_joint;
    }
    for (i = 0; i < n; i++) {
        HSD_JObj* jobj = NULL;
        lb_80011E24(*css.root, &jobj, joints[i], -1);
        if (jobj == NULL) {
            continue;
        }
        if (hide) {
            HSD_JObjSetFlagsAll(jobj, JOBJ_HIDDEN);
        } else {
            HSD_JObjClearFlagsAll(jobj, JOBJ_HIDDEN);
        }
    }
    level_hidden[door] = (unsigned char) hide;
}

/* Every frame, each panel: a panel that stops being a CPU, or takes a fighter its pick does not support,
 * goes back to Default; the plate shows the pick of a CPU panel, and the level slider follows it. */
static void housekeeping(int door, CSSTagData* tag)
{
    const int kind = css.doors[door].p_kind;
    const int was = prev_kind[door];
    int want;
    prev_kind[door] = (unsigned char) kind;
    if (level_unused(door)) {
        level_slider(door, 1);   /* every frame: the slider's own animation may run again */
    } else if (level_hidden[door]) {
        level_slider(door, 0);
    }
    if (list_owner[door] >= 0) {
        return;
    }
    if (css_choice[door] != STYLE_DEFAULT && was == 1 && kind != 1) {
        set_choice(door, STYLE_DEFAULT);
    }
    if (css_choice[door] != STYLE_DEFAULT && kind == 1 && !style_supports(css_choice[door], door_fighter(door))) {
        set_choice(door, STYLE_DEFAULT);
    }
    want = kind == 1 ? css_choice[door] : STYLE_DEFAULT;
    if (want != STYLE_DEFAULT) {
        if (plate_shown[door] != want || tag->use_tag == 0) {
            plate_style(tag, want);
            plate_shown[door] = (signed char) want;
        }
    } else if (plate_shown[door] >= 0) {
        plate_fighter(door, tag);
        plate_shown[door] = -1;
    }
}

static void build_rows(int door)
{
    const int ckind = door_fighter(door);
    unsigned int i;
    int n = 0;
    for (i = 0; i < sizeof list_order; i++) {
        if (style_supports(list_order[i], ckind)) {
            list_rows[door][n++] = list_order[i];
        }
    }
    list_count[door] = (unsigned char) n;
}

static void list_arrows(int door, int open)
{
    HSD_JObj* jobj = NULL;
    lb_80011E24(*css.root, &jobj, css.tags[door].x7, -1);
    if (jobj != NULL) {
        HSD_JObjSetFlags(jobj, JOBJ_HIDDEN);
    }
    jobj = NULL;
    lb_80011E24(*css.root, &jobj, css.tags[door].kostar_text_joint, -1);
    if (jobj != NULL) {
        if (open || css.tags[door].data->next_tag <= 7) {
            HSD_JObjSetFlags(jobj, JOBJ_HIDDEN);
        } else {
            HSD_JObjClearFlags(jobj, JOBJ_HIDDEN);   /* as the scene builds it */
        }
    }
}

/* The retail list's states (fn_802633B0): 1 opens, 2 waits for the box, 3 takes the pad, 4 closes,
 * 5 waits for the box; then 0. Rows are 32 text units (2 select units) apart from the header row. */
static void list_think(int door, CSSTagData* tag)
{
    HSD_JObj* list_jobj = NULL;
    const int hand = list_owner[door];
    GXColor color;
    int j;
    lb_80011E24(*css.root, &list_jobj, css.tags[door].x4, -1);
    switch ((int) tag->state) {
    case 1:
        mnCharSel_8025D1C4(door, 1);
        build_rows(door);
        tag->x8 = 0.0f;
        tag->scroll_amt = 0.0f;
        tag->scroll_force = 0.0f;
        HSD_SisLib_803A70A0(tag->name_ls, 0, (char*) styles[STYLE_DEFAULT].name);
        HSD_SisLib_803A746C(tag->name_ls, 0, 0.0f, 0.0f);
        HSD_SisLib_803A70A0(tag->name_ls, 1, NULL);
        for (j = 0; j < 9; j++) {
            if (j >= 1 && j <= list_count[door]) {
                HSD_SisLib_803A70A0(tag->name_ls, j + 2, (char*) styles[list_rows[door][j - 1]].name);
                fit(tag->name_ls, j + 2, styles[list_rows[door][j - 1]].name);
                color = color_white;
                HSD_SisLib_803A74F0(tag->name_ls, j + 2, &color);
            } else {
                HSD_SisLib_803A70A0(tag->name_ls, j + 2, NULL);
            }
            HSD_SisLib_803A746C(tag->name_ls, j + 2, 10.0f, 32.0f * (f32) j);
        }
        color = color_gray;
        HSD_SisLib_803A74F0(tag->text, 0, &color);
        if (list_jobj != NULL) {
            HSD_ForeachAnim(list_jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjReqAnim, AOBJ_ARG_AF, 0.0f);
        }
        tag->state = 2;
        tag->timer = 0;
        lbAudioAx_80024030(2);
        return;
    case 2:
        if (++tag->timer > 10) {
            if (list_jobj != NULL) {
                HSD_ForeachAnim(list_jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV, 0, 0);
            }
            tag->name_ls->hidden = 0;
            tag->state = 3;
        }
        return;
    case 3: {
        unsigned int trigger;
        list_arrows(door, 1);
        if (hand < 0 || hand > 3 || HSD_PadCopyStatus[hand].err != 0 || css.doors[door].p_kind != 1) {
            tag->state = 4;   /* its player left, or the panel is no CPU any more */
            return;
        }
        trigger = HSD_PadCopyStatus[hand].trigger;
        if (trigger & HSD_PAD_A) {
            Vec3 list;
            if (joint_pos(css.tags[door].list_joint, &list)) {
                const int row = (int) (0.5f * (0.8f + (list.y - hand_y[hand])));
                int style = -1;
                if (row == 0) {
                    style = STYLE_DEFAULT;
                } else if (row >= 1 && row <= list_count[door]) {
                    style = list_rows[door][row - 1];
                }
                if (style >= 0) {
                    set_choice(door, style);
                    if (style != STYLE_DEFAULT) {
                        plate_style(tag, style);
                        plate_shown[door] = (signed char) style;
                    } else {
                        plate_fighter(door, tag);
                        plate_shown[door] = -1;
                    }
                    OSReport("[cpu ai] P%d picks %s for the CPU in panel %d\n", hand + 1, styles[style].name,
                             door + 1);
                    tag->state = 4;
                    lbAudioAx_80024030(2);
                    return;
                }
            }
        }
        if (trigger & HSD_PAD_B) {
            tag->state = 4;
            lbAudioAx_80024030(0);
        }
        return;
    }
    case 4:
        if (list_jobj != NULL) {
            HSD_ForeachAnim(list_jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjReqAnim, AOBJ_ARG_AF, 20.0f);
        }
        tag->name_ls->hidden = 1;
        HSD_SisLib_803A70A0(tag->name_ls, 1, name_entry_text);
        for (j = 2; j < 11; j++) {
            fit(tag->name_ls, j, NULL);   /* the name tag list's own scale again */
        }
        list_arrows(door, 0);
        tag->state = 5;
        tag->timer = 0;
        return;
    case 5:
        if (++tag->timer > 10) {
            mnCharSel_8025D1C4(door, 2);
            color = color_white;
            HSD_SisLib_803A74F0(tag->text, 0, &color);
            tag->state = 0;
            list_owner[door] = -1;
        }
        return;
    default:
        list_owner[door] = -1;
        return;
    }
}

int mu_cpu_ai_tag_think(struct CSSTagData* tag)
{
    const int door = tag != NULL ? tag->port : -1;
    if (!css.active || door < 0 || door > 3 || css.tags == NULL) {
        return 0;
    }
    if (prev_kind[door] == 0xFF && mu_test_getenv("MELEE_CPU_AI_TRACE") != NULL) {
        Vec3 plate, list;   /* tests: where a script's hand finds this panel's plate */
        if (joint_pos(css.tags[door].name_jointl, &plate) && joint_pos(css.tags[door].list_joint, &list)) {
            OSReport("[cpu ai] panel %d: kind %d, plate %.1f %.1f, list %.1f %.1f, pick %s\n", door + 1,
                     (int) css.doors[door].p_kind, plate.x, plate.y, list.x, list.y, styles[css_choice[door]].name);
        }
    }
    housekeeping(door, tag);
    if (list_owner[door] < 0) {
        return 0;
    }
    list_think(door, tag);
    return 1;
}

static int door_driven_by(int hand)
{
    int door;
    for (door = 0; door < 4; door++) {
        if (list_owner[door] == hand) {
            return door;
        }
    }
    return -1;
}

int mu_cpu_ai_hand_busy(int hand)
{
    return css.active && hand >= 0 && hand <= 3 && door_driven_by(hand) >= 0;
}

int mu_cpu_ai_confine(int hand, float* x, float* y)
{
    Vec3 list;
    int door;
    if (!css.active || hand < 0 || hand > 3) {
        return 0;
    }
    door = door_driven_by(hand);
    if (door < 0) {
        hand_y[hand] = *y;
        if (list_owner[hand] < 0) {
            return 0;
        }
        /* This hand's own panel is a CPU whose list another hand drives: not the list's limits, the
         * screen's (as mncharsel.c's free hand). */
        if (*y > 25.0f) *y = 25.0f;
        if (*y < -22.0f) *y = -22.0f;
        if (*x > ((mu_mex_css_codes & MEX_CSS_CURSOR_RIGHT_29) != 0 ? 29.0f : 26.0f)) {
            *x = (mu_mex_css_codes & MEX_CSS_CURSOR_RIGHT_29) != 0 ? 29.0f : 26.0f;
        }
        if (*x < -35.0f) *x = -35.0f;
        return 1;
    }
    if (joint_pos(css.tags[door].list_joint, &list)) {
        const float top = list.y + 0.8f;
        const float bottom = top - 2.0f * (f32) (list_count[door] + 1) + 0.1f;
        if (*y > top) *y = top;
        if (*y < bottom) *y = bottom;
        if (*x > list.x + 4.3f) *x = list.x + 4.3f;
        if (*x < list.x - 5.3f) *x = list.x - 5.3f;
    }
    hand_y[hand] = *y;
    return 1;
}

void mu_cpu_ai_plate_click(int hand, int hand_state, float* x, float* y)
{
    int door;
    if (!css.active || hand < 0 || hand > 3 || hand_state == 1 || css.tags[hand].data->state != 0 ||
        door_driven_by(hand) >= 0)
    {
        return;   /* holding a coin or a slider, or its own name list just opened */
    }
    for (door = 0; door < 4; door++) {
        CSSTagData* tag = css.tags[door].data;
        Vec3 plate, list;
        int row = 0, i;
        if (css.doors[door].p_kind != 1 || tag->state != 0 || list_owner[door] >= 0 ||
            css.doors[door].is_hold_cpu_slider || (door == 3 && css.data->match_type == VS_CAMERA))
        {
            continue;   /* not a CPU, busy, or another hand holds its level (a pick may hide it) */
        }
        if (!joint_pos(css.tags[door].name_jointl, &plate)) {
            continue;
        }
        if (!(*x > plate.x - 4.7f && *x < plate.x + 5.2f && *y < plate.y + 2.0f && *y > plate.y - 1.0f)) {
            continue;
        }
        build_rows(door);
        for (i = 0; i < list_count[door]; i++) {
            if (list_rows[door][i] == css_choice[door]) {
                row = i + 1;
            }
        }
        list_owner[door] = (signed char) hand;
        tag->state = 1;
        tag->timer = 0;
        if (joint_pos(css.tags[door].list_joint, &list)) {
            *x = list.x;
            *y = list.y + 0.8f - 2.0f * (f32) row - 1.0f;
        }
        hand_y[hand] = *y;
        OSReport("[cpu ai] P%d opens the AI list of the CPU in panel %d (plate %.1f %.1f, list %.1f %.1f)\n",
                 hand + 1, door + 1, plate.x, plate.y, list.x, list.y);
        return;
    }
}

/* Snapshot exclusions: the log count and the character select's view are not match state. */
MU_EXCLUSIONS(cpu_ai,
              MU_EXCLUDE(match_logs),
              MU_EXCLUDE(css),
              MU_EXCLUDE(css_loaded),
              MU_EXCLUDE(list_owner),
              MU_EXCLUDE(plate_shown),
              MU_EXCLUDE(prev_kind),
              MU_EXCLUDE(level_hidden),
              MU_EXCLUDE(hand_y),
              MU_EXCLUDE(host_reply))
