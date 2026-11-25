#include "../mex.h"

// ---- Shulk's own states -------------------------------------------------------------------------------------------
#define B_BUTTON 0x200
#define ST_FIRST_OWN 373
#define ST_LAST_OWN 394
#define NEVER 30000
#define GRAB_ELEMENT 8
#define ABSF(x) ((x) < 0 ? -(x) : (x))
// his states, by special (they move when the donor or the rows change: never written by hand)
#define ST_N_FIRST 373            // the art's state: ST_N_FIRST + 2 * (art - 1), and + 1 in the air
#define ST_S 383
#define ST_S_AIR 384
#define ST_S_FALL 385
#define ST_S_LAND 386
#define ST_HI 387
#define ST_HI_AIR 388
#define ST_HI_ADD 389
#define ST_LW 390
#define ST_LW_AIR 391
#define ST_LW_HIT 392
#define ST_LW_HIT_AIR 393
#define ST_LW_F 394
static u16 t_frame[6];                  // by player: the frame of the state of his own he is in (1 on the frame it is entered)
static u8 w_helpless[6];                // by player: helpless since the up special, until he stands

static void Enter(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
    t_frame[fd->ply] = 1;
}
static void Hand_Over(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(fd->state.frame, 1, 0, gobj, state, 0x5000, 0);
}
static int Tick(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] < NEVER)
        t_frame[fd->ply]++;
    return t_frame[fd->ply];
}
static void Leave(GOBJ *gobj, int helpless, float landing)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else if (helpless)
    {
        w_helpless[fd->ply] = 1;
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, landing);
    }
    else
        Fighter_EnterFall(gobj);
}
static int Other_Form(int state);
void Plain_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Plain_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj, Other_Form(fd->state_id));
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj, Other_Form(fd->state_id));
    }
}
// The fighter nearest him (another player's, in the match), or 0.
static FighterData *Nearest(FighterData *fd)
{
    FighterData *best = 0;
    float best_d = 1e9f;
    int i;
    for (i = 0; i < 6; i++)
    {
        GOBJ *g;
        FighterData *t;
        float d;
        if (i == fd->ply)
            continue;
        g = Fighter_GetGObj(i);
        if (!g)
            continue;
        t = g->userdata;
        if (!t || t->state_id < ASID_WAIT)
            continue;
        d = (t->phys.pos.X - fd->phys.pos.X) * (t->phys.pos.X - fd->phys.pos.X)
            + (t->phys.pos.Y - fd->phys.pos.Y) * (t->phys.pos.Y - fd->phys.pos.Y);
        if (d < best_d)
        {
            best_d = d;
            best = t;
        }
    }
    return best;
}

// ---- Monado Arts: a MODE that changes his numbers for a time, then cools down ----------------------------------
// Arts are 1 Jump, 2 Speed, 3 Shield, 4 Buster, 5 Smash (the game's order); 0 is none. Each table is by art.
#define N_END 10                   // script: the frame he may act from in an art's clip (the rest is not played)
#define N_END_AIR 10
static const int ART_TIME[6] = {0, 360, 480, 360, 600, 480};          // vl.prc active_time_<art>, read as seconds: frames it is on
static const int ART_COOL[6] = {0, 1080, 960, 1080, 840, 960};          // vl.prc unavailable_time_<art>, as seconds: frames it cools
static const float ART_POWER[6] = {1.0000f, 1.0000f, 0.7000f, 0.5000f, 1.4000f, 0.3000f};      // vl.prc <art>_attack_power_mul: his hits' damage
static const float ART_BLOW[6] = {1.0000f, 1.0000f, 1.0000f, 0.8000f, 0.6500f, 1.2500f};        // vl.prc <art>_blow_mul: his hits' knockback
static const float ART_TAKEN[6] = {1.0000f, 1.3000f, 1.0000f, 0.5000f, 1.3000f, 1.0000f};      // vl.prc <art>_damage_mul: the damage of hits he takes
static const float ART_BLOWN[6] = {1.0000f, 1.0000f, 1.0000f, 0.6000f, 1.0000f, 1.2000f};      // vl.prc <art>_blown_mul: the knockback of hits he takes
static const float ART_WALK[6] = {1.0000f, 1.0000f, 1.7000f, 0.6000f, 1.0000f, 1.0000f};        // vl.prc <art>_walk_speed_mul
static const float ART_DASH[6] = {1.0000f, 1.0000f, 2.0000f, 0.7000f, 1.0000f, 1.0000f};        // vl.prc <art>_dash_speed_mul
static const float ART_RUN[6] = {1.0000f, 1.0000f, 2.0000f, 0.6000f, 1.0000f, 1.0000f};          // vl.prc <art>_run_speed_mul
static const float ART_BRAKE[6] = {1.0000f, 1.0000f, 2.0000f, 1.0000f, 1.0000f, 1.0000f};      // vl.prc <art>_ground_brake_mul (his ground friction)
static const float ART_JUMP[6] = {1.0000f, 1.4142f, 0.8367f, 0.8944f, 1.0000f, 1.0000f};        // the square root of vl.prc <art>_height_mul (a jump's height goes as its speed squared)
static const float ART_DRIFT[6] = {1.0000f, 1.7000f, 1.4000f, 0.6000f, 1.0000f, 1.0000f};      // vl.prc <art>_air_speed_x_mul (his most sideways speed in the air)
static const float ART_FALL[6] = {1.0000f, 1.4000f, 1.2000f, 1.0000f, 1.0000f, 1.0000f};        // vl.prc <art>_air_speed_y_mul (his falling speed, and fast fall)
static const float ART_HI[6] = {1.0000f, 1.2247f, 1.0000f, 1.0000f, 1.0000f, 1.0000f};            // the square root of vl.prc <art>_special_hi_height_mul (Air Slash)
// how the art is shown (ART_SHOW): the game's own effects for it, one row a generator. `point`: given at the
// point his body's middle was at as the art started (plus y), not held; else held by `joint`, x y z from it.
#define ART_SPAWN ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)   // ftCo_8009F834
#define ART_EVERY 12                   // READING: frames between givings of what is kept up while it is on
typedef struct { u16 art, delay, joint, point, gfx; float x, y, z; } ArtFx;
static const ArtFx ART_START_FX[] = {    {1, 0, 0, 1, 6192, 0.0f, 0.0f, 0.0f},
    {1, 46, 0, 1, 6193, 0.0f, 0.0f, 0.0f},
    {1, 0, 77, 0, 6194, 0.0f, 0.0f, 0.0f},
    {1, 0, 77, 0, 6195, 0.0f, 0.0f, 0.0f},
    {2, 0, 0, 1, 6200, 0.0f, 0.0f, 0.0f},
    {2, 46, 0, 1, 6201, 0.0f, 0.0f, 0.0f},
    {2, 0, 77, 0, 6202, 0.0f, 0.0f, 0.0f},
    {2, 0, 77, 0, 6203, 0.0f, 0.0f, 0.0f},
    {3, 0, 0, 1, 6208, 0.0f, 0.0f, 0.0f},
    {3, 46, 0, 1, 6209, 0.0f, 0.0f, 0.0f},
    {3, 0, 77, 0, 6210, 0.0f, 0.0f, 0.0f},
    {3, 0, 77, 0, 6211, 0.0f, 0.0f, 0.0f},
    {4, 0, 0, 1, 6215, 0.0f, 0.0f, 0.0f},
    {4, 46, 0, 1, 6216, 0.0f, 0.0f, 0.0f},
    {4, 0, 77, 0, 6217, 0.0f, 0.0f, 0.0f},
    {4, 0, 77, 0, 6218, 0.0f, 0.0f, 0.0f},
    {5, 0, 0, 1, 6223, 0.0f, 0.0f, 0.0f},
    {5, 46, 0, 1, 6224, 0.0f, 0.0f, 0.0f},
    {5, 0, 77, 0, 6225, 0.0f, 0.0f, 0.0f},
    {5, 0, 77, 0, 6226, 0.0f, 0.0f, 0.0f}};   // as it starts: its symbol over him, its sign on the Monado
static const ArtFx ART_ON_FX[] = {    {1, 0, 76, 0, 6196, 0.0f, 0.0f, 0.0f},
    {1, 0, 135, 0, 6197, 0.0f, 0.0f, 0.0f},
    {1, 0, 135, 0, 6198, 0.0f, 0.0f, 0.0f},
    {1, 0, 135, 0, 6199, 0.0f, 0.0f, 0.0f},
    {1, 0, 130, 0, 6197, 0.0f, 0.0f, 0.0f},
    {1, 0, 130, 0, 6198, 0.0f, 0.0f, 0.0f},
    {1, 0, 130, 0, 6199, 0.0f, 0.0f, 0.0f},
    {2, 0, 76, 0, 6204, 0.0f, 0.0f, 0.0f},
    {2, 0, 135, 0, 6205, 0.0f, 0.0f, 0.0f},
    {2, 0, 135, 0, 6206, 0.0f, 0.0f, 0.0f},
    {2, 0, 135, 0, 6207, 0.0f, 0.0f, 0.0f},
    {2, 0, 130, 0, 6205, 0.0f, 0.0f, 0.0f},
    {2, 0, 130, 0, 6206, 0.0f, 0.0f, 0.0f},
    {2, 0, 130, 0, 6207, 0.0f, 0.0f, 0.0f},
    {3, 0, 76, 0, 6212, 0.0f, 0.0f, 0.0f},
    {3, 0, 6, 0, 6213, 0.0f, 0.0f, 0.0f},
    {3, 0, 6, 0, 6214, 0.0f, 0.0f, 0.0f},
    {4, 0, 76, 0, 6219, 0.0f, 0.0f, 0.0f},
    {4, 0, 77, 0, 6220, 0.0f, 0.0f, 0.0f},
    {4, 0, 77, 0, 6221, 0.0f, 0.0f, 0.0f},
    {4, 0, 77, 0, 6222, 0.0f, 0.0f, 0.0f},
    {5, 0, 76, 0, 6227, 0.0f, 0.0f, 0.0f},
    {5, 0, 75, 0, 6228, 0.0f, 0.0f, 0.0f},
    {5, 0, 75, 0, 6229, 0.0f, 0.0f, 0.0f},
    {5, 0, 75, 0, 6230, 0.0f, 0.0f, 0.0f},
    {5, 0, 48, 0, 6228, 0.0f, 0.0f, 0.0f},
    {5, 0, 48, 0, 6229, 0.0f, 0.0f, 0.0f},
    {5, 0, 48, 0, 6230, 0.0f, 0.0f, 0.0f}};         // while it is on: its aura, the glow at his sword hand
typedef struct
{
    float walk, dash, run, brake, jump, hop, drift, fall, fast_fall;
} ArtBase;
static u8 art_on[6];                    // by player: the art that is on (0 none)
static u8 art_next[6];                  //   the art a press puts on next when none is on
static int art_left[6];                 //   frames before the art that is on ends
static int art_cool[6][6];              //   frames before each art may be put on again
static ArtBase art_base[6];             //   his own numbers, kept while an art changes them
static u16 art_age[6];                  //   frames since the art that is on started
static int art_state[6];                //   (shown) the state he was in when it was last given again
static Vec3 art_at[6];                  //   the middle of his body as it started
static u8 th_on[6][2];                  //   (throws) the throw's numbers were seen set
static int th_dmg[6][2], th_kbg[6][2], th_bkb[6][2], th_set[6][2], th_kbg_set[6][2];
static u8 hs_on[6][4];                  //   (hits) the hitbox was seen out
static int hs_dmg[6][4], hs_kbg[6][4], hs_bkb[6][4], hs_set[6][4];   //   its own numbers, and the damage given it
static int hs_kbg_set[6][4];

// One generator of how the art is shown (ART_START_FX, ART_ON_FX). The engine's own call for a fighter's effect;
// never ended on a change of state (what is held is given again by Art_Show, and ends by itself).
static void Art_Fx(GOBJ *gobj, const ArtFx *f)
{
    FighterData *fd = gobj->userdata;
    Vec3 off, none = {0, 0, 0};
    if (f->point)
    {
        off.X = 0;
        off.Y = art_at[fd->ply].Y + f->y - fd->phys.pos.Y;
        off.Z = (art_at[fd->ply].X - fd->phys.pos.X) * fd->facing_direction;
        ART_SPAWN(gobj, f->gfx, 0, 0, 0, &off, &none, 0.0f);
        return;
    }
    off.X = f->x;
    off.Y = f->y;
    off.Z = f->z;
    ART_SPAWN(gobj, f->gfx, f->joint, 0, 0, &off, &none, 0.0f);
}
// The art that is on, shown: what starts with it on its own frames, and what is kept up given again every ART_EVERY
// frames and as he changes state (a change of state may take off what his joints hold). Each frame an art is on.
static void Art_Show(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, a = art_on[p], i, again;
    if (!a)
        return;
    if (art_age[p] == 0)
        Fighter_GetECBCenter(gobj, &art_at[p]);
    for (i = 0; i < (int)(sizeof(ART_START_FX) / sizeof(ART_START_FX[0])); i++)
        if (ART_START_FX[i].gfx && ART_START_FX[i].art == a && ART_START_FX[i].delay == art_age[p])
            Art_Fx(gobj, &ART_START_FX[i]);
    again = art_age[p] % ART_EVERY == 0 || fd->state_id != art_state[p];
    art_state[p] = fd->state_id;
    if (again)
        for (i = 0; i < (int)(sizeof(ART_ON_FX) / sizeof(ART_ON_FX[0])); i++)
            if (ART_ON_FX[i].gfx && ART_ON_FX[i].art == a)
                Art_Fx(gobj, &ART_ON_FX[i]);
    if (art_age[p] < 60000)
        art_age[p]++;
}
// The art that is on ends: his own numbers back, and it cools down.
static void Art_Off(FighterData *fd)
{
    int p = fd->ply, a = art_on[p];
    ArtBase *b = &art_base[p];
    if (!a)
        return;
    fd->attr.walk_maximum_velocity = b->walk;
    fd->attr.dash_initial_velocity = b->dash;
    fd->attr.dashrun_terminal_velocity = b->run;
    fd->attr.ground_friction = b->brake;
    fd->attr.jump_v_initial_velocity = b->jump;
    fd->attr.hop_v_initial_velocity = b->hop;
    fd->attr.aerial_drift_max = b->drift;
    fd->attr.terminal_velocity = b->fall;
    fd->attr.fastfall_velocity = b->fast_fall;
    *(float *)&fd->dmg.behavior = 1.0f;
    art_cool[p][a] = ART_COOL[a];
    art_on[p] = 0;
}
// An art is put on: his own numbers are kept, and the art's laid over them.
static void Art_On(FighterData *fd, int a)
{
    int p = fd->ply;
    ArtBase *b = &art_base[p];
    Art_Off(fd);
    b->walk = fd->attr.walk_maximum_velocity;
    b->dash = fd->attr.dash_initial_velocity;
    b->run = fd->attr.dashrun_terminal_velocity;
    b->brake = fd->attr.ground_friction;
    b->jump = fd->attr.jump_v_initial_velocity;
    b->hop = fd->attr.hop_v_initial_velocity;
    b->drift = fd->attr.aerial_drift_max;
    b->fall = fd->attr.terminal_velocity;
    b->fast_fall = fd->attr.fastfall_velocity;
    fd->attr.walk_maximum_velocity = b->walk * ART_WALK[a];
    fd->attr.dash_initial_velocity = b->dash * ART_DASH[a];
    fd->attr.dashrun_terminal_velocity = b->run * ART_RUN[a];
    fd->attr.ground_friction = b->brake * ART_BRAKE[a];
    fd->attr.jump_v_initial_velocity = b->jump * ART_JUMP[a];
    fd->attr.hop_v_initial_velocity = b->hop * ART_JUMP[a];
    fd->attr.aerial_drift_max = b->drift * ART_DRIFT[a];
    fd->attr.terminal_velocity = b->fall * ART_FALL[a];
    fd->attr.fastfall_velocity = b->fast_fall * ART_FALL[a];
    art_on[p] = a;
    art_left[p] = ART_TIME[a];
    art_next[p] = a % 5 + 1;
    art_age[p] = 0;
    art_state[p] = -1;
}
// The art a press puts on: the next in the game's order after the one on (or after the last), passing over the
// one on and any still cooling down. 0: every one is cooling down.
static int Art_Pick(int p)
{
    int from = art_on[p] ? art_on[p] % 5 + 1 : art_next[p], k;
    if (from < 1 || from > 5)
        from = 1;
    for (k = 0; k < 5; k++)
    {
        int a = (from - 1 + k) % 5 + 1;
        if (a != art_on[p] && art_cool[p][a] <= 0)
            return a;
    }
    return 0;
}
static void N_Begin(GOBJ *gobj, int air)
{
    FighterData *fd = gobj->userdata;
    int a = Art_Pick(fd->ply);
    if (!a)
        return;                         // (every art is cooling down: the press does nothing)
    Art_On(fd, a);
    Enter(gobj, ST_N_FIRST + 2 * (a - 1) + air);
}
void SpecialN(GOBJ *gobj)
{
    N_Begin(gobj, 0);
}
void SpecialAirN(GOBJ *gobj)
{
    N_Begin(gobj, 1);
}
void N_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= (fd->phys.air_state == 0 ? N_END : N_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// The knockback of a hit he takes, times the art's: the engine has worked it out (`ftCo_Damage_CalcKnockback`) and
// calls this before it is used (fighter.c, `take_dmg_2_cb`). Every change of state clears the call: Arts_Hits sets
// it again each frame an art is on.
static void Arts_Blown(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int a = art_on[fd->ply];
    if (a && fd->dmg.hit_log.force_applied > 0)
        fd->dmg.hit_log.force_applied *= ART_BLOWN[a];
}
// His throw while an art is on: the throw's damage and knockback (`fd->throw_hitbox`, set by the throw's script and
// read as he lets go: `ftCo_800DDDE4`), worked out from the numbers the script set, kept when it set them.
static void Arts_Throw(FighterData *fd, GOBJ *gobj, int a)
{
    int p = fd->ply, i, d;
    for (i = 0; i < 2; i++)
    {
        ftHit *h = &fd->throw_hitbox[i];
        if (!th_on[p][i] || h->dmg != th_set[p][i] || h->kb_growth != th_kbg_set[p][i])
        {
            if (h->dmg == 0 && h->kb_growth == 0 && h->kb == 0)
                continue;               // (no throw set yet)
            th_on[p][i] = 1;            // (the script has set it again)
            th_dmg[p][i] = h->dmg;
            th_kbg[p][i] = h->kb_growth;
            th_bkb[p][i] = h->kb;
        }
        d = (int)((float)th_dmg[p][i] * ART_POWER[a] + 0.5f);
        if (d < 1 && th_dmg[p][i] > 0)
            d = 1;
        if (h->dmg != d)
            Fighter_UpdateHitboxDamage(h, d, gobj);
        h->kb_growth = (int)((float)th_kbg[p][i] * ART_BLOW[a] + 0.5f);
        h->kb = (int)((float)th_bkb[p][i] * ART_BLOW[a] + 0.5f);
        th_set[p][i] = h->dmg;
        th_kbg_set[p][i] = h->kb_growth;
    }
}
// His hits while an art is on: each hitbox of his, as it comes out (after the script has made it, before it strikes
// anything), gets the art's damage and knockback, worked out from its own numbers so nothing is laid on twice; so does
// his throw; and the damage and knockback he takes are the art's. Called from `Own_Hits` (the frame code), which the
// engine runs after his physics and collision and before the hits are looked at, in every state.
static void Arts_Hits(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, a = art_on[p], i;
    if (!a)
        return;
    *(float *)&fd->dmg.behavior = ART_TAKEN[a];
    if (ART_BLOWN[a] != 1.0f)
        fd->cb.OnTakeDamage2 = Arts_Blown;
    Arts_Throw(fd, gobj, a);
    if (fd->state_id == ST_LW_HIT || fd->state_id == ST_LW_HIT_AIR || fd->state_id == ST_LW_F)
        return;                         // (the counter's strike has its own damage: Lw_Strike_Phys)
    for (i = 0; i < 4; i++)
    {
        ftHit *h = &fd->hitbox[i];
        int d;
        if (!h->active || h->attribute == GRAB_ELEMENT)
        {
            hs_on[p][i] = 0;
            continue;
        }
        if (!hs_on[p][i] || h->dmg != hs_set[p][i] || h->kb_growth != hs_kbg_set[p][i])
        {
            hs_on[p][i] = 1;            // (a new hitbox, or one the script or code has just set again)
            hs_dmg[p][i] = h->dmg;
            hs_kbg[p][i] = h->kb_growth;
            hs_bkb[p][i] = h->kb;
        }
        d = (int)((float)hs_dmg[p][i] * ART_POWER[a] + 0.5f);
        if (d < 1 && hs_dmg[p][i] > 0)
            d = 1;
        if (h->dmg != d)
            Fighter_UpdateHitboxDamage(h, d, gobj);
        h->kb_growth = (int)((float)hs_kbg[p][i] * ART_BLOW[a] + 0.5f);
        h->kb = (int)((float)hs_bkb[p][i] * ART_BLOW[a] + 0.5f);
        hs_set[p][i] = h->dmg;
        hs_kbg_set[p][i] = h->kb_growth;
    }
}
static void Arts_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, a = art_on[p], i;
    for (i = 1; i <= 5; i++)
        if (art_cool[p][i] > 0)
            art_cool[p][i]--;
    if (!a)
        return;
    if (fd->state_id < ASID_WAIT || --art_left[p] <= 0)
    {
        Art_Off(fd);                    // (its time is up, or a KO ended it)
        return;
    }
    *(float *)&fd->dmg.behavior = ART_TAKEN[a];
    Art_Show(gobj);
}

// ---- Back Slash: a leap; the blade is out as he falls, and strikes harder at a back ---------------------------------
#define S_UP 7                     // clip: the frame the leap leaves the ground (its root starts up), as the disc plays it
#define S_SPEED_X 1.100000023841858f           // vl.prc speed_x: his speed ahead through the leap,
#define S_SPEED_Y 1.899999976158142f           //   speed_y: up as it leaves the ground,
#define S_AIR_SPEED_X 1.100000023841858f   //   air_speed_x, air_speed_y: the same begun in the air
#define S_AIR_SPEED_Y 1.600000023841858f
#define S_GRAVITY 0.15000000596046448f           // vl.prc jump_accel_y: taken off his speed up each frame
#define S_FALL_MOST 4.5f // vl.prc jump_speed_y_max: his most speed down
#define S_FALL_FRAMES 120   // READING: a fall that has not landed in this many frames is an ordinary fall
#define S_LAND_END 41         // script: the frame he may act from in the landing strike
typedef struct { short action, front, back; } BackSide;
// For each action of the move: the angle of the game's front hits and of its back ones. The script makes both sides
// at once (ids 0 to 3 front, 4 to 7 back); the driver writes each side's balls, the back ones in a hit group of
// their own, so both would strike: the side not wanted is put out.
static const BackSide back_sides[] = {
    {303, 361, 140},
    {310, 361, 140},
    {312, 361, 140},
    {319, 361, 140}
};
static u8 s_back[6];                    // by player: 0 no side chosen yet in this action, 1 the front hits, 2 the back
static int s_chosen[6];                 //   the action the side was chosen in

void SpecialS(GOBJ *gobj)
{
    Enter(gobj, ST_S);
    s_back[((FighterData *)gobj->userdata)->ply] = 0;
}
void SpecialAirS(GOBJ *gobj)
{
    Enter(gobj, ST_S_AIR);
    s_back[((FighterData *)gobj->userdata)->ply] = 0;
}
// only the game's front hits or only its back ones, by whether the nearest fighter has his back to him as the
// action's hits first come out (chosen ONCE an action: a fighter struck turns, and the other side's hits are a hit
// group of their own that would strike him again): each hit of the other side that is out is put out (after the
// script made it, before hits are looked at)
static void S_Hits(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata, *t;
    int i, k, p = fd->ply, out = 0, back;
    for (i = 0; i < 4; i++)
        out |= fd->hitbox[i].active;
    if (!out)
        return;
    if (!s_back[p] || s_chosen[p] != fd->action_id)
    {
        t = Nearest(fd);
        s_back[p] = (t && t->facing_direction * (fd->phys.pos.X - t->phys.pos.X) < 0) ? 2 : 1;
        s_chosen[p] = fd->action_id;
    }
    back = s_back[p] == 2;
    for (k = 0; k < (int)(sizeof(back_sides) / sizeof(back_sides[0])); k++)
    {
        const BackSide *r = &back_sides[k];
        if (r->action != fd->action_id)
            continue;
        for (i = 0; i < 4; i++)
        {
            ftHit *h = &fd->hitbox[i];
            if (h->active && h->angle == (back ? r->front : r->back))
                h->active = 0;
        }
        break;
    }
}
static void S_Leap(FighterData *fd)
{
    int air = fd->state_id == ST_S_AIR;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    fd->phys.self_vel.X = (air ? S_AIR_SPEED_X : S_SPEED_X) * fd->facing_direction;
    fd->phys.self_vel.Y = air ? S_AIR_SPEED_Y : S_SPEED_Y;
}
void S_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame == S_UP)
        S_Leap(fd);
    if (FrameTimerCheck(gobj) == 0)
    {
        if (fd->phys.air_state == 0)
            Leave(gobj, 0, 0);
        else
        {
            float vx = fd->phys.self_vel.X, vy = fd->phys.self_vel.Y;
            Enter(gobj, ST_S_FALL);     // (the blade stays out as he falls: the fall's own script)
            fd->phys.self_vel.X = vx;
            fd->phys.self_vel.Y = vy;
        }
    }
}
void S_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else if (t_frame[fd->ply] < S_UP && fd->state_id != ST_S_FALL)
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
    else
    {
        fd->phys.self_vel.Y -= S_GRAVITY;
        if (fd->phys.self_vel.Y < -S_FALL_MOST)
            fd->phys.self_vel.Y = -S_FALL_MOST;
    }
}
static void S_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    Enter(gobj, ST_S_LAND);
    fd->phys.self_vel_ground.X = 0;
}
void S_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else if (t_frame[fd->ply] >= S_UP || fd->state_id == ST_S_FALL)
        Fighter_CollAir_IgnoreLedge(gobj, S_Landed);
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);       // (begun in the air, he comes down before the leap: it goes on standing)
        Hand_Over(gobj, ST_S);
    }
}
void SFall_Anim(GOBJ *gobj)
{
    if (Tick(gobj) >= S_FALL_FRAMES)
        Leave(gobj, 0, 0);
}
void SLand_Anim(GOBJ *gobj)
{
    if (Tick(gobj) >= S_LAND_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void SLand_Phys(GOBJ *gobj)
{
    Plain_Phys(gobj);
}
void SLand_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
}

// ---- Air Slash: a rising slash, and a second press for the follow-up -------------------------------------------
#define HI_RISE_FROM 12     // clip: he leaves the ground on this frame (its root's first rise)
// clip: his speed each frame of the slash and of the follow-up, ahead and up: the clip's root, frame by frame as the
// disc plays it, times the drawn scale (Air Slash rises 48 in 15 frames, then the clip itself comes down; an average
// over the whole clip rose 3.9 in the game)
static const float HI_V[][2] = {{0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {1.7210f, 0.0000f}, {1.7767f, 0.0000f}, {0.1052f, 6.4376f}, {0.0929f, 6.0169f}, {0.0815f, 5.5697f}, {0.0707f, 5.0963f}, {0.0607f, 4.5964f}, {0.0514f, 4.0702f}, {0.0428f, 3.5176f}, {0.0349f, 2.9386f}, {0.0278f, 2.3334f}, {0.0214f, 1.8538f}, {0.0158f, 1.5174f}, {0.0108f, 1.1807f}, {0.0066f, 0.8437f}, {0.0031f, 0.5065f}, {0.0003f, 0.1688f}, {-0.0017f, -0.0286f}, {-0.0030f, -0.0848f}, {-0.0036f, -0.1393f}, {-0.0035f, -0.1920f}, {-0.0026f, -0.2431f}, {-0.0010f, -0.2926f}, {-0.0111f, -0.3402f}, {-0.0323f, -0.3864f}, {-0.0522f, -0.4307f}, {-0.0708f, -0.4734f}, {-0.0879f, -0.5144f}, {-0.1037f, -0.5537f}, {-0.1181f, -0.5913f}, {-0.1311f, -0.6273f}, {-0.1428f, -0.6616f}, {-0.1531f, -0.6941f}, {-0.1620f, -0.7251f}, {-0.1695f, -0.7542f}, {-0.1757f, -0.7819f}, {-0.1805f, -0.8077f}, {-0.1839f, -0.8318f}, {-0.1860f, -0.8544f}, {-0.1867f, -0.8752f}, {-0.1860f, -0.8943f}, {-0.1839f, -0.9117f}, {-0.1805f, -0.9275f}, {-0.1757f, -0.9416f}, {-0.1695f, -0.9540f}, {-0.1620f, -0.9647f}, {-0.1531f, -0.9737f}, {-0.1428f, -0.9811f}, {-0.1311f, -0.9868f}, {-0.1181f, -0.9908f}, {-0.1037f, -0.9930f}, {-0.0879f, -0.9990f}, {-0.0708f, -1.0073f}, {-0.0522f, -1.0122f}, {-0.0323f, -1.0134f}, {-0.0111f, -1.0110f}};
static const float HI_ADD_V[][2] = {{0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0075f, -0.0032f}, {0.0559f, 0.0091f}, {0.2129f, 0.0497f}, {0.3063f, 0.1186f}, {0.6111f, 0.2158f}, {0.7446f, 0.3413f}, {0.8305f, 0.5636f}, {0.8487f, 0.5381f}, {0.7994f, 0.5124f}, {0.6963f, 0.4864f}, {0.5853f, 0.4603f}, {0.4757f, 0.4340f}, {0.3676f, 0.4074f}, {0.2608f, 0.3807f}, {0.1975f, 0.3537f}, {0.1770f, 0.3266f}, {0.1569f, 0.2993f}, {0.1371f, 0.2717f}, {0.1176f, 0.2440f}, {0.0985f, 0.2160f}, {0.0798f, 0.1879f}, {0.0614f, 0.1595f}, {0.0435f, 0.1310f}, {0.0258f, 0.1022f}, {0.0085f, 0.0733f}, {-0.0100f, 0.0441f}, {-0.0292f, 0.0147f}, {-0.0473f, -0.0171f}, {-0.0641f, -0.0513f}, {-0.0797f, -0.0854f}, {-0.0941f, -0.1193f}, {-0.1073f, -0.1532f}, {-0.1193f, -0.1870f}, {-0.1302f, -0.2207f}, {-0.1398f, -0.2543f}, {-0.1482f, -0.2878f}, {-0.1554f, -0.3212f}, {-0.1614f, -0.3546f}, {-0.1662f, -0.3878f}, {-0.1698f, -0.4209f}, {-0.1722f, -0.4540f}, {-0.1734f, -0.4870f}, {-0.1734f, -0.5198f}, {-0.1722f, -0.5526f}, {-0.1698f, -0.5853f}, {-0.1662f, -0.6178f}, {-0.1614f, -0.6503f}, {-0.1554f, -0.6827f}, {-0.1482f, -0.7150f}, {-0.1398f, -0.7472f}, {-0.1302f, -0.7793f}, {-0.1193f, -0.8113f}, {-0.1073f, -0.8433f}, {-0.0941f, -0.8751f}, {-0.0797f, -0.9069f}, {-0.0641f, -0.9385f}, {-0.0473f, -0.9701f}, {-0.0292f, -1.0015f}, {-0.0100f, -1.0329f}};
#define HI_FRAMES (int)(sizeof(HI_V) / sizeof(HI_V[0]))
#define HI_ADD_FRAMES (int)(sizeof(HI_ADD_V) / sizeof(HI_ADD_V[0]))
#define HI_ADD_FROM 20       // script: the follow-up may be asked for from this frame (its flag)
#define HI_ADD_TO 43           //   to this one (the flag let go)
#define HI_DRIFT 0.4000000059604645f             // vl.prc special_hi_fall_x_mul: of his most sideways speed, through the slash
#define HI_LANDING 30.0f         // vl.prc special_hi_landing_frame: frames of landing after it
#define HI_ADD_LANDING 30.0f // vl.prc add_hit_landing_frame: the same after the follow-up
void SpecialHi(GOBJ *gobj)
{
    Enter(gobj, ST_HI);
}
void SpecialAirHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_HI_AIR);
    fd->phys.self_vel.Y = 0;
}
static void Hi_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_EnterSpecialLanding(gobj, 0, fd->state_id == ST_HI_ADD ? HI_ADD_LANDING : HI_LANDING);
}
// his speed on a frame of the slash or the follow-up: the clip's (a rise times the art's), and the stick's drift
static void Hi_Move(FighterData *fd, const float (*v)[2], int frames, int f)
{
    float x = fd->input.lstick.X, up;
    if (x < 0.2f && x > -0.2f)
        x = 0;
    if (f < frames)
    {
        up = v[f][1];
        fd->phys.self_vel.Y = up > 0 ? up * ART_HI[art_on[fd->ply]] : up;
        fd->phys.self_vel.X = v[f][0] * fd->facing_direction + x * fd->attr.aerial_drift_max * HI_DRIFT;
    }
    else
    {
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
        fd->phys.self_vel.X = x * fd->attr.aerial_drift_max * HI_DRIFT;
    }
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= HI_RISE_FROM && fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    if (frame >= HI_ADD_FROM && frame < HI_ADD_TO && (fd->input.down & B_BUTTON) && fd->phys.air_state != 0)
    {
        Enter(gobj, ST_HI_ADD);
        fd->phys.self_vel.Y = 0;
        return;
    }
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, HI_LANDING);
}
void Hi_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int f = t_frame[fd->ply];
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    Hi_Move(fd, HI_V, HI_FRAMES, f);
}
void Hi_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
}
void HiAdd_Anim(GOBJ *gobj)
{
    Tick(gobj);
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, HI_ADD_LANDING);
}
void HiAdd_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Hi_Move(fd, HI_ADD_V, HI_ADD_FRAMES, t_frame[fd->ply]);
}
// no jump while helpless after it, until he stands (the engine's helpless fall keeps a jump he had)
static void Helpless_Frame(FighterData *fd)
{
    int p = fd->ply, s = fd->state_id;
    if (!w_helpless[p])
        return;
    if (fd->phys.air_state == 0)
        w_helpless[p] = 0;
    else if ((s >= ST_FIRST_OWN && s <= ST_LAST_OWN) || (s >= ASID_FALLSPECIAL && s <= ASID_FALLSPECIALB))
        fd->jump.jumps_used = fd->attr.max_jumps;
    else
        w_helpless[p] = 0;
}

// ---- Vision: a counter whose window shrinks with each use in a row -----------------------------------------------
#define LW_ON 7                   // script: it catches a hit from this frame (its flag)
#define LW_WINDOW 35.0f           // vl.prc counter_time: frames it catches for, the first time
#define LW_WINDOW_MIN 5.0f   // vl.prc counter_time_min: no fewer than this
#define LW_SHRINK 0.6600000262260437f           // vl.prc counter_time_mul: the window times this for each use in a row
#define LW_REGROW 0.35f           // STAND-IN: frames the window grows back by, each frame
#define LW_DONE 70               // script: the frame he may act from when nothing struck
#define LW_DONE_AIR 80
#define LW_STRIKE_END 51   // script: the frame he may act from after the strike
#define LW_STRIKE_END_AIR 51
#define LW_F_END 57
#define LW_FALL_MOST 0.20000000298023224f     // vl.prc vision_speed_y_max: his most falling speed while he waits in the air
#define LW_JOINT 5             // READING: the joint the ball rides (his hip: a newer game's counter holds the body)
#define LW_HEIGHT 0.0f
#define LW_RADIUS 11.638f           // READING: the ball's size: round his whole body from the hip
#define LW_MUL 1.2999999523162842f                 // vl.prc special_lw_attack_mul: the strike, for the hit's damage times this,
#define LW_MOST 50               //   vl.prc special_lw_attack_max: no more than this,
#define LW_LEAST 9             //   no less than the script's own (its hits' damage)
#define LW_GO_FROM 1         // clip: the strike carries him ahead from this frame (as the disc plays it)
#define LW_GO_TO 31             //   to this one,
#define LW_GO 1.6378f                   //   this far a frame (the clip's root; the driver takes it off the clip)
#define LW_GO_AIR_FROM 1 // the same, struck in the air
#define LW_GO_AIR_TO 46
#define LW_GO_AIR 0.7237f
#define STICK_FORWARD 0.5f              // READING: the stick this far ahead as he strikes gives the dash past him
#define CATCH ((void (*)(GOBJ *, ShieldDesc *, void *))0x8007B1B8)    // ftColl_8007B1B8: the engine's counter ball (Marth's)
static const ShieldDesc lw_ball = {LW_JOINT, 0, {0, LW_HEIGHT, 0}, LW_RADIUS, 0, 0, 0};
static int lw_took[6];                  // by player: the damage of the hit he caught
static float lw_window[6];              //   the window the next use has
static float lw_now[6];                 //   the window of the use going on

void Lw_Caught(GOBJ *gobj);
static void Lw_Catch(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    CATCH(gobj, (ShieldDesc *)&lw_ball, Lw_Caught);
    fd->flags.shield_always_refract = 1;    // (Marth's code sets this beside it)
}
static void Lw_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    Enter(gobj, state);
    lw_took[p] = 0;
    lw_now[p] = lw_window[p];
    lw_window[p] *= LW_SHRINK;          // (the next use in a row has less)
    if (lw_window[p] < LW_WINDOW_MIN)
        lw_window[p] = LW_WINDOW_MIN;
    if (fd->phys.air_state != 0)
        fd->phys.self_vel.Y = 0;
}
void SpecialLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW);
}
void SpecialAirLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW_AIR);
}
// a hit touched the ball (called by the engine as it takes the hit): he faces the nearest fighter and strikes
void Lw_Caught(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata, *t = Nearest(fd);
    int p = fd->ply, took = fd->shield.dmg_taken2;     // (Marth's reads the same: x19A4)
    float ahead = fd->input.lstick.X * fd->facing_direction;
    fd->flags.shield_enable = 0;
    lw_took[p] = took > 0 ? took : 0;
    if (t && (t->phys.pos.X - fd->phys.pos.X) * fd->facing_direction < 0)
        fd->facing_direction = -fd->facing_direction;
    if (fd->phys.air_state != 0)
        Enter(gobj, ST_LW_HIT_AIR);
    else
        Enter(gobj, ahead >= STICK_FORWARD ? ST_LW_F : ST_LW_HIT);
    fd->cb.EnterHitlag = 0;             // (Marth's code clears the two beside it)
    fd->cb.ExitHitlag = 0;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void Lw_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= LW_ON && (float)(frame - LW_ON) < lw_now[fd->ply])
    {
        if (!fd->flags.shield_enable)
            Lw_Catch(gobj);
    }
    else
        fd->flags.shield_enable = 0;
    if (frame >= (fd->phys.air_state == 0 ? LW_DONE : LW_DONE_AIR) || FrameTimerCheck(gobj) == 0)
    {
        fd->flags.shield_enable = 0;
        Leave(gobj, 0, 0);
    }
}
void Lw_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, LW_FALL_MOST);
    fd->shield_bubble.is_checked = 0;   // (Marth's own each frame: the ball follows its joint)
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int up = fd->flags.shield_enable;
    Plain_Coll(gobj);
    if (up && !fd->flags.shield_enable && (fd->state_id == ST_LW || fd->state_id == ST_LW_AIR))
        Lw_Catch(gobj);                 // (a change of state takes the ball down: put back up)
}
void LwHit_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int end = fd->state_id == ST_LW_F ? LW_F_END : (fd->phys.air_state == 0 ? LW_STRIKE_END : LW_STRIKE_END_AIR);
    if (Tick(gobj) >= end || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// the strike's damage, put on its hits after the script has made them and before they strike anything
void LwHit_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int i, f = t_frame[fd->ply], damage = (int)((float)lw_took[fd->ply] * LW_MUL + 0.5f);
    Plain_Phys(gobj);
    // (the strike's travel ahead is the clip's: the driver took it off the clip, so it is given here)
    if (fd->state_id == ST_LW_HIT && fd->phys.air_state == 0 && f >= LW_GO_FROM && f < LW_GO_TO)
    {
        fd->phys.self_vel_ground.X = LW_GO * fd->facing_direction;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
    else if (fd->state_id == ST_LW_HIT_AIR && fd->phys.air_state != 0 && f >= LW_GO_AIR_FROM && f < LW_GO_AIR_TO)
        fd->phys.self_vel.X = LW_GO_AIR * fd->facing_direction;
    if (damage < LW_LEAST)
        damage = LW_LEAST;
    if (damage > LW_MOST)
        damage = LW_MOST;
    for (i = 0; i < 4; i++)
        if (fd->hitbox[i].active && fd->hitbox[i].dmg != damage)
            Fighter_UpdateHitboxDamage(&fd->hitbox[i], damage, gobj);
}
static void Lw_Frame(FighterData *fd)
{
    int p = fd->ply;
    if (fd->state_id != ST_LW && fd->state_id != ST_LW_AIR && lw_window[p] < LW_WINDOW)
    {
        lw_window[p] += LW_REGROW;
        if (lw_window[p] > LW_WINDOW)
            lw_window[p] = LW_WINDOW;
    }
}

// ---- once as he is made, and once a frame -------------------------------------------------------------------------
static void OwnLoad(FighterData *fd)
{
    int p = fd->ply, i;
    t_frame[p] = 0;
    w_helpless[p] = 0;
    art_on[p] = 0;
    art_next[p] = 1;
    art_left[p] = 0;
    art_age[p] = 0;
    art_state[p] = -1;
    for (i = 0; i < 2; i++)
        th_on[p][i] = 0;
    for (i = 0; i < 6; i++)
        art_cool[p][i] = 0;
    for (i = 0; i < 4; i++)
        hs_on[p][i] = 0;
    s_back[p] = 0;
    lw_took[p] = 0;
    lw_window[p] = LW_WINDOW;
    lw_now[p] = LW_WINDOW;
}
// his hits as they come out, in every state: Back Slash's side, then an art's numbers. The engine runs
// `Accessory_Persist` after his COLLISION call (a state entered on landing, Back Slash's strike, makes its hits there,
// after its own physics has run) and before hits are looked at; it outlasts a change of state, and a KO clears it
static void Own_Hits(GOBJ *gobj)
{
    S_Hits(gobj);
    Arts_Hits(gobj);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Arts_Frame(fd, gobj);
    Helpless_Frame(fd);
    Lw_Frame(fd);
    if ((void *)fd->cb.Accessory_Persist != (void *)Own_Hits)
        fd->cb.Accessory_Persist = Own_Hits;
}

static int Other_Form(int state)
{
    switch (state)
    {
    case ST_N_FIRST + 0: return ST_N_FIRST + 1;
    case ST_N_FIRST + 1: return ST_N_FIRST + 0;
    case ST_N_FIRST + 2: return ST_N_FIRST + 3;
    case ST_N_FIRST + 3: return ST_N_FIRST + 2;
    case ST_N_FIRST + 4: return ST_N_FIRST + 5;
    case ST_N_FIRST + 5: return ST_N_FIRST + 4;
    case ST_N_FIRST + 6: return ST_N_FIRST + 7;
    case ST_N_FIRST + 7: return ST_N_FIRST + 6;
    case ST_N_FIRST + 8: return ST_N_FIRST + 9;
    case ST_N_FIRST + 9: return ST_N_FIRST + 8;
    case ST_LW: return ST_LW_AIR;
    case ST_LW_AIR: return ST_LW;
    case ST_LW_HIT: return ST_LW_HIT_AIR;
    case ST_LW_HIT_AIR: return ST_LW_HIT;
    case ST_LW_F: return ST_LW_HIT_AIR;
    }
    return state;
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 6
static const s8 face_rest[FACE_GROUPS] = {0, 0, 0, 0, 0, -1};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {62, 3, 5},
    {63, 3, 3},
    {68, 3, 5},
    {69, 3, 0},
    {138, 3, 5},
    {139, 3, 3},
    {144, 3, 5},
    {145, 3, 0},
    {261, 3, 5},
    {262, 3, 3},
    {267, 3, 5},
    {268, 3, 0},
    {392, 3, 5},
    {393, 3, 3},
    {398, 3, 5},
    {399, 3, 0},
    {538, 3, 5},
    {539, 3, 3},
    {544, 3, 5},
    {545, 3, 0},
    {32767, 0, 0},
};
#define FACE_CLOCK_END 600
static const u16 face_left[] = {0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 8};
// (a fighter made for a screen that is not a match plays the rows of its other table of actions)
static const u16 face_left_other[] = {0, 8, 0, 8, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8};
static short face_time[6];
static void FacesLoad(GOBJ *gobj)
{
    int g;
    for (g = 0; g < FACE_GROUPS; g++)
        Fighter_SetVisGroupDefault(gobj, g, face_rest[g]);
}
static void FacesFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, a = fd->action_id, g, i;
    u16 left = 0;
    s8 now[FACE_GROUPS];
    for (g = 0; g < FACE_GROUPS; g++)
    {
        if (fd->dobj_toggle[g].prev_value != face_rest[g])
            Fighter_SetVisGroupDefault(gobj, g, face_rest[g]);
        now[g] = face_rest[g];
    }
    if (fd->ftaction == fd->ftData->ftaction)
    {
        if (a >= 0 && a < (int)(sizeof(face_left) / sizeof(face_left[0])))
            left = face_left[a];
    }
    else if (a >= 0 && a < (int)(sizeof(face_left_other) / sizeof(face_left_other[0])))
        left = face_left_other[a];
    if (++face_time[p] > FACE_CLOCK_END)
        face_time[p] = 0;
    if (!left)
        return;
    for (i = 0; face_clock[i].frame <= face_time[p]; i++)
        now[face_clock[i].group] = face_clock[i].choice;
    for (g = 0; g < FACE_GROUPS; g++)
        if (left & (1 << g))
            Fighter_SetVisGroupCurrent(gobj, g, now[g]);
}


// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (!(s == 383 || s == 384 || s == 387 || s == 388 || s == 390 || s == 391) || fd->state.frame > 4.0f)
        return;
    if (fd->input.lstick.X * fd->facing_direction < -0.5f)
    {
        fd->facing_direction = -fd->facing_direction;
        fd->phys.self_vel.X = -fd->phys.self_vel.X;
        Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
    }
}

// Moves whose clip travels along the ground, in donor states that only slow the fighter down. The action
// carries the engine's flag, so the clip's travel is taken out of the picture each frame; here the fighter
// is given that travel as its ground speed (plus what the state's friction is about to take off it).
static void TravelFrame(FighterData *fd)
{
    int a = fd->action_id;
    if (fd->phys.air_state != 0 || !(a == 55 || a == 59 || a == 303))
        return;
    float go = fd->transN_offset.Z * fd->facing_direction;
    if (go > 0)
        go += fd->attr.ground_friction;
    else if (go < 0)
        go -= fd->attr.ground_friction;
    fd->phys.self_vel_ground.X = go;
}

// The colour a move lays over the body, on the frames its script gives (a flash as a counter starts, a
// pulse while a charge builds): the colour it starts from, the one it goes to and how many frames that takes.
// The game draws it with the fighter's second colour overlay, which this leaves alone while the game is
// using it. This runs before the frame is drawn and before its clip moves on: the frame about to be shown
// is the second of an action the first time this sees it.
typedef struct { short action, frame, fade; u8 from[4], to[4]; } FlashKey;
static const FlashKey flash_keys[] = {
    {323, 6, 0, {255, 255, 255, 0}, {255, 255, 255, 191}},
    {323, 7, 0, {255, 255, 255, 191}, {0, 0, 0, 0}},
    {325, 6, 0, {255, 255, 255, 0}, {255, 255, 255, 191}},
    {325, 7, 0, {255, 255, 255, 191}, {0, 0, 0, 0}},
};
static int flash_action[6], flash_state[6], flash_count[6];
static u8 flash_lit[6];
static void (*flash_anim[6])(GOBJ *);
// Melee lays a colour animation of its own (the third: a green flicker) over a fighter whose script says it
// cannot be hurt, and draws that in place of this overlay. A move that gives its own colour for those frames
// shows its own: the game's is taken off after the frame's script has run (the state's own step is called
// through this one, which is put in its place while an action with a colour of its own plays).
static void Flash_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    void (*then)(GOBJ *) = flash_anim[fd->ply];
    if (flash_lit[fd->ply] && fd->color[0].colanim == 3)
        ColAnim_Disable(&fd->color[0]);
    if (then)
        then(gobj);
}
static void FlashFrame(FighterData *fd)
{
    int p = fd->ply, i, t, has = 0;
    const FlashKey *k = 0;
    ColorOverlay *c = &fd->color[1];
    if (fd->action_id != flash_action[p] || fd->state_id != flash_state[p])
    {
        flash_action[p] = fd->action_id;
        flash_state[p] = fd->state_id;
        flash_count[p] = 1;
    }
    if (fd->dmg.hitlag_frames <= 0.0f)        // a frame frozen by a hit is not a frame of the move
        flash_count[p]++;
    t = flash_count[p];
    for (i = 0; i < (int)(sizeof(flash_keys) / sizeof(flash_keys[0])); i++)
        if (flash_keys[i].action == fd->action_id)
        {
            has = 1;
            if (flash_keys[i].frame <= t)
                k = &flash_keys[i];
        }
    if (has && (void *)fd->cb.Anim != (void *)Flash_Anim)
    {
        flash_anim[p] = fd->cb.Anim;
        fd->cb.Anim = Flash_Anim;
    }
    if (k && c->colanim == 0)
    {
        float s = k->fade > 0 ? (float)(t - k->frame) / (float)k->fade : 1.0f;
        float v[4];
        if (s > 1.0f)
            s = 1.0f;
        for (i = 0; i < 4; i++)
            v[i] = (float)k->from[i] + ((float)k->to[i] - (float)k->from[i]) * s;
        if (v[3] >= 1.0f)
        {
            if (fd->color[0].colanim == 3)        // (see Flash_Anim)
                ColAnim_Disable(&fd->color[0]);
            // (the game keeps the colour twice: numbers it steps every frame and the bytes it draws with)
            c->color_red = v[0];
            c->color_green = v[1];
            c->color_blue = v[2];
            c->color_alpha = v[3];
            c->colorblend_red = 0;
            c->colorblend_green = 0;
            c->colorblend_blue = 0;
            c->colorblend_alpha = 0;
            c->hex.r = (u8)v[0];
            c->hex.g = (u8)v[1];
            c->hex.b = (u8)v[2];
            c->hex.a = (u8)v[3];
            c->color_enable = 1;
            flash_lit[p] = 1;
            return;
        }
    }
    if (flash_lit[p])
    {
        c->color_enable = 0;
        flash_lit[p] = 0;
    }
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    FacesLoad(gobj);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    FacesFrame(fd, gobj);
    ReverseFrame(fd);
    TravelFrame(fd);
    FlashFrame(fd);
}

// The donor's own special states, as the game has them, then this port's.
__attribute__((used)) static struct FtState move_logic[] = {
    {295, 0x340211, 0x12, 0x0, (void *)0x80136844, (void *)0x80136914, (void *)0x8013691c, (void *)0x801369a4, (void *)0x800761c8},    // 341
    {296, 0x340211, 0x12, 0x0, (void *)0x80136adc, (void *)0x80136c8c, (void *)0x80136cfc, (void *)0x80136d3c, (void *)0x800761c8},    // 342
    {297, 0x340211, 0x12, 0x0, (void *)0x80136ee4, (void *)0x8013713c, (void *)0x80137144, (void *)0x80137184, (void *)0x800761c8},    // 343
    {298, 0x340211, 0x12, 0x0, (void *)0x80136ee4, (void *)0x8013713c, (void *)0x80137144, (void *)0x80137184, (void *)0x800761c8},    // 344
    {299, 0x340611, 0x12, 0x0, (void *)0x801368ac, (void *)0x80136918, (void *)0x8013695c, (void *)0x801369e0, (void *)0x800761c8},    // 345
    {300, 0x340611, 0x12, 0x0, (void *)0x80136bb4, (void *)0x80136cc4, (void *)0x80136d1c, (void *)0x80136d78, (void *)0x800761c8},    // 346
    {301, 0x340611, 0x12, 0x0, (void *)0x80137010, (void *)0x80137140, (void *)0x80137164, (void *)0x801371c0, (void *)0x800761c8},    // 347
    {302, 0x340611, 0x12, 0x0, (void *)0x80137010, (void *)0x80137140, (void *)0x80137164, (void *)0x801371c0, (void *)0x800761c8},    // 348
    {303, 0x340212, 0x13, 0x0, (void *)0x80137558, (void *)0x801375b8, (void *)0x80137618, (void *)0x8013767c, (void *)0x800761c8},    // 349
    {304, 0x340212, 0x13, 0x0, (void *)0x801377b0, (void *)0x80137810, (void *)0x80137870, (void *)0x801378d4, (void *)0x800761c8},    // 350
    {305, 0x340212, 0x13, 0x0, (void *)0x801377b0, (void *)0x80137810, (void *)0x80137870, (void *)0x801378d4, (void *)0x800761c8},    // 351
    {306, 0x340212, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 352
    {307, 0x340212, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 353
    {308, 0x340212, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 354
    {309, 0x340212, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 355
    {310, 0x340212, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 356
    {311, 0x340212, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 357
    {312, 0x340612, 0x13, 0x0, (void *)0x80137558, (void *)0x801375b8, (void *)0x80137618, (void *)0x8013767c, (void *)0x800761c8},    // 358
    {313, 0x340612, 0x13, 0x0, (void *)0x801377b0, (void *)0x80137810, (void *)0x80137870, (void *)0x801378d4, (void *)0x800761c8},    // 359
    {314, 0x340612, 0x13, 0x0, (void *)0x801377b0, (void *)0x80137810, (void *)0x80137870, (void *)0x801378d4, (void *)0x800761c8},    // 360
    {315, 0x340612, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 361
    {316, 0x340612, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 362
    {317, 0x340612, 0x13, 0x0, (void *)0x80137b34, (void *)0x80137b94, (void *)0x80137bf4, (void *)0x80137c50, (void *)0x800761c8},    // 363
    {318, 0x340612, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 364
    {319, 0x340612, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 365
    {320, 0x340612, 0x13, 0x0, (void *)0x80137ecc, (void *)0x80137f2c, (void *)0x80137f30, (void *)0x80137f8c, (void *)0x800761c8},    // 366
    {321, 0x340213, 0x14, 0x0, (void *)0x801382e8, (void *)0x801383a8, (void *)0x80138638, (void *)0x801388b4, (void *)0x800761c8},    // 367
    {322, 0x340613, 0x14, 0x0, (void *)0x80138348, (void *)0x801384f0, (void *)0x8013873c, (void *)0x80138940, (void *)0x800761c8},    // 368
    {323, 0x340214, 0x15, 0x0, (void *)0x80138aa8, (void *)0x80138c20, (void *)0x80138c28, (void *)0x80138cc0, (void *)0x800761c8},    // 369
    {324, 0x340214, 0x15, 0x0, (void *)0x80138e68, (void *)0x80138fc0, (void *)0x80138fc8, (void *)0x80139008, (void *)0x800761c8},    // 370
    {325, 0x340614, 0x15, 0x0, (void *)0x80138b64, (void *)0x80138c24, (void *)0x80138c5c, (void *)0x80138cfc, (void *)0x800761c8},    // 371
    {326, 0x340614, 0x15, 0x0, (void *)0x80138f14, (void *)0x80138fc4, (void *)0x80138fe8, (void *)0x80139044, (void *)0x800761c8},    // 372
    {304, 0x340211, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 373: Monado Arts: jump
    {313, 0x340611, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 374: Monado Arts: jump, in the air
    {305, 0x340211, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: Monado Arts: speed
    {314, 0x340611, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: Monado Arts: speed, in the air
    {306, 0x340211, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 377: Monado Arts: shield
    {315, 0x340611, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 378: Monado Arts: shield, in the air
    {307, 0x340211, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 379: Monado Arts: buster
    {316, 0x340611, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 380: Monado Arts: buster, in the air
    {309, 0x340211, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 381: Monado Arts: smash
    {318, 0x340611, 0x12, 0x0, N_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 382: Monado Arts: smash, in the air
    {303, 0x340212, 0x13, 0x0, S_Anim, 0, S_Phys, S_Coll, Fighter_UpdateCameraBox},    // 383: Back Slash
    {312, 0x340612, 0x13, 0x0, S_Anim, 0, S_Phys, S_Coll, Fighter_UpdateCameraBox},    // 384: Back Slash, begun in the air
    {319, 0x340612, 0x13, 0x0, SFall_Anim, 0, S_Phys, S_Coll, Fighter_UpdateCameraBox},    // 385: Back Slash: the fall, blade out
    {310, 0x340212, 0x13, 0x0, SLand_Anim, 0, SLand_Phys, SLand_Coll, Fighter_UpdateCameraBox},    // 386: Back Slash: the landing strike
    {321, 0x340213, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 387: Air Slash
    {322, 0x340613, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 388: Air Slash, in the air
    {320, 0x340613, 0x14, 0x0, HiAdd_Anim, 0, HiAdd_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 389: Air Slash: the follow-up
    {323, 0x340214, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 390: Vision: waiting
    {325, 0x340614, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 391: Vision: waiting, in the air
    {324, 0x340214, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 392: Vision: the strike
    {326, 0x340614, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 393: Vision: the strike, in the air
    {308, 0x340214, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 394: Vision: the dash past him
};

// The fighter's own sword trail: where the game's trail runs along the blade, in its trail's colour.
static AfterImageDesc trail = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {82, 172, 254, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 77, .offset_bottom = 2.637, .offset_top = 18.637,
};
static AfterImageDesc trail_1 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {64, 172, 251, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 77, .offset_bottom = 2.637, .offset_top = 18.637,
};
static AfterImageDesc trail_2 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {70, 187, 254, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 77, .offset_bottom = 2.637, .offset_top = 18.637,
};
static AfterImageDesc trail_3 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {255, 81, 144, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 77, .offset_bottom = 2.637, .offset_top = 18.637,
};
static AfterImageDesc trail_4 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {255, 142, 207, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 77, .offset_bottom = 2.637, .offset_top = 18.637,
};

AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    int a = fd->action_id;
    if (a == 48 || a == 247)
        return &trail_1;
    if (a == 55 || a == 58 || a == 221 || a == 222)
        return &trail_2;
    if (a == 303 || a == 312 || a == 319)
        return &trail_3;
    if (a == 320 || a == 321 || a == 322)
        return &trail_4;
    return &trail;
}
