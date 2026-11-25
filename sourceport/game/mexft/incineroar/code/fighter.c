#include "../mex.h"

// ---- Incineroar's own states ------------------------------------------------------------------------------------
#define B_BUTTON 0x200
#define ST_FIRST_OWN 373
#define ST_LAST_OWN 385
#define NEVER 30000
#define STICK_DEAD 0.2f                 // READING: the stick nearer the middle than this is let go
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
static float Toward(float v, float want, float step)
{
    if (v < want - step)
        return v + step;
    if (v > want + step)
        return v - step;
    return want;
}
static float Stick_X(FighterData *fd)
{
    float sx = fd->input.lstick.X;
    return (sx > -STICK_DEAD && sx < STICK_DEAD) ? 0 : sx;
}

// ---- Darkest Lariat: he spins, and the stick carries him ----------------------------------------------------------
#define ST_N 373
#define ST_N_AIR 374
#define N_END 85                   // script (the game's list of motions): the frame he may act from
#define N_END_AIR 85
#define N_SPEED 0.5f               // READING: param_special_n's 0x1569266df0: his most speed along the ground
#define N_ACCEL 0.07500000298023224f               // READING: param_special_n's 0x1774a3156b: the speed the stick adds a frame
#define N_BRAKE 0.009999999776482582f               // vl.prc special_n_brake_x: the stick let go, he slows by this a frame
void SpecialN(GOBJ *gobj)
{
    Enter(gobj, ST_N);
}
void SpecialAirN(GOBJ *gobj)
{
    Enter(gobj, ST_N_AIR);
}
void N_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= (fd->phys.air_state == 0 ? N_END : N_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// along the ground the stick sets where his speed goes, and he gets there by N_ACCEL a frame; let go, he slows by
// N_BRAKE. In the air: gravity and drift as in any attack in the air.
void N_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        float sx = Stick_X(fd), v = fd->phys.self_vel_ground.X;
        fd->phys.self_vel_ground.X = sx == 0 ? Toward(v, 0, N_BRAKE) : Toward(v, sx * N_SPEED, N_ACCEL);
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}

// ---- Alolan Whip: a lunge with a grab; the one caught is pulled in and thrown forward ----------------------------
#define ST_S 375
#define ST_S_AIR 376
#define S_END 58                   // script: the frame he may act from, when he caught no one
#define S_GRAB_ON 15           // script: the grab box is made on this frame (Melee's takes hold the frame after:
#define S_GRAB_OFF 36         //   the game's first grabbing frame), and is gone on this one
#define S_GO 14                     // clip: the lunge goes from this frame
#define S_STOP 43                 //   to this one,
#define S_SPEED 1.1683f               //   this far a frame (the clip's root travel over it, drawn)
#define S_BALLS 4
// the game's grab as Melee's balls on joint 0: place (sideways, up, ahead), size, and who (1 ground, 2 air, 3 both)
static const float s_ball[S_BALLS][5] = {
    {0.0000f, 7.7585f, 7.0311f, 4.6066f, 1},
    {0.0000f, 7.7585f, 4.1217f, 4.6066f, 1},
    {0.0000f, 7.7585f, 9.1526f, 2.4851f, 3},
    {0.0000f, 7.7585f, 5.0309f, 2.4851f, 3}
};
#define GRABBER_PULL ((void (*)(GOBJ *))0x800D9CE8)            // fn_800D9CE8: the common grab's own: he pulls in
#define GRABBED_PULL ((void (*)(GOBJ *, GOBJ *))0x800DAADC)    // fn_800DAADC: the one caught is pulled in
#define TO_THROW ((void (*)(GOBJ *, int))0x800DD4B0)           // ftCo_800DD4B0: from a grab into a throw
#define THROW_FORWARD 219                                       // ftCo_MS_ThrowF
#define HIT_NEW ((void (*)(FighterData *, ftHit *))0x800768A0) // ftColl_800768A0: a hitbox made new
#define HIT_DAMAGE ((void (*)(ftHit *, int, GOBJ *))0x8007ABD0)
#define HIT_ANGLE ((void (*)(ftHit *, int, GOBJ *))0x8007AC9C)
#define HIT_PLACE ((void (*)(FighterData *, ftHit *))0x8007AD18)
#define HIT_AFTER ((void (*)(FighterData *))0x80080484)
#define GRAB_ELEMENT 8
static u8 s_whip[6];                    // by player: the grab he holds came from the whip: throw it at once

// a grab box made as the engine's script command makes one (ftaction.c, ftAction_8007121C)
static void Grab_Box(GOBJ *gobj, int i)
{
    FighterData *fd = gobj->userdata;
    ftHit *h = &fd->hitbox[i];
    const float *b = s_ball[i];
    int who = (int)b[4];
    if (h->active == 0 || h->x4 != 0)
    {
        h->x4 = 0;
        h->active = 1;
        fd->flags.hitbox_active = 1;
        HIT_NEW(fd, h);
    }
    h->bone = fd->bones[0].joint;
    HIT_DAMAGE(h, 0, gobj);
    h->size = b[3];
    h->offset.X = b[0];
    h->offset.Y = b[1];
    h->offset.Z = b[2];
    HIT_ANGLE(h, 0, gobj);
    h->kb_growth = 0;
    h->wdsk = 0;
    h->kb = 0;
    h->attribute = GRAB_ELEMENT;
    h->shield_dmg = 0;
    h->hitsound_severity = 0;
    h->hitsound_kind = 0;
    // byte 0x40: clank 0x80, rebound 0x40, hits in the air 0x20, on the ground 0x10
    h->x40 = (char)(((who & 2) ? 0x20 : 0) | ((who & 1) ? 0x10 : 0));
    h->x41 = 0;
    // byte 0x42 as the command leaves it: b1, b5 and b7 on, the rest off; byte 0x43's b2 off
    *((u8 *)h + 0x42) = 0x40 | 0x04 | 0x01;
    *((u8 *)h + 0x43) &= (u8)~0x20;
    HIT_PLACE(fd, h);
    HIT_AFTER(fd);
}
// the one caught: the engine's own grab takes him (he pulls them in, then holds them), and the frame code throws
void S_Caught(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    s_whip[fd->ply] = 1;
    GRABBER_PULL(gobj);
}
static void S_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    s_whip[fd->ply] = 0;
    if (state == ST_S)
        Fighter_InitGrab(fd, 1, S_Caught, 0, GRABBED_PULL);
}
void SpecialS(GOBJ *gobj)
{
    S_Begin(gobj, ST_S);
}
void SpecialAirS(GOBJ *gobj)
{
    S_Begin(gobj, ST_S_AIR);
}
void S_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj), i;
    if (fd->state_id == ST_S && frame >= S_GRAB_ON && frame < S_GRAB_OFF)
        for (i = 0; i < S_BALLS; i++)
            Grab_Box(gobj, i);
    else if (frame == S_GRAB_OFF)
        Fighter_HitboxDisableAll(gobj);
    if (frame >= S_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// the lunge is the clip's own travel, given as speed (its root's travel is taken off the clip); then he slows
void S_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int f = t_frame[fd->ply];
    if (fd->phys.air_state == 0)
    {
        if (f >= S_GO && f < S_STOP)
        {
            fd->phys.self_vel_ground.X = S_SPEED * fd->facing_direction;
            Fighter_PhysGround_ApplyVelocity(gobj);
        }
        else
            Fighter_PhysGround_ApplyFriction(gobj);
    }
    else
    {
        if (f >= S_GO && f < S_STOP)
            fd->phys.self_vel.X = S_SPEED * fd->facing_direction;
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
    }
}
// a grab held from the whip is thrown forward as soon as it is held
static void Whip_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    if (!s_whip[p])
        return;
    // (0x1a58, `grab.attacker` in MexTK, is the other fighter of a grab from either side: the one he holds)
    if (s == ASID_CATCHWAIT && fd->grab.attacker)
    {
        s_whip[p] = 0;
        TO_THROW(gobj, THROW_FORWARD);
    }
    else if (s != ASID_CATCHDASHPULL && s != ASID_CATCHPULL && s != ST_S)
        s_whip[p] = 0;
}

// ---- Cross Chop: a leap, a turn over at the top, a dive with fire, the blast as he lands ------------------------
#define ST_HI 377
#define ST_HI_AIR 378
#define ST_HI_TURN 379
#define ST_HI_FALL 380
#define ST_HI_BOUND 381
#define HI_UP 9                   // script: he leaves the ground on this frame (its flag)
#define HI_RISE_FROM 13     // clip: the leap rises from this frame of the state
#define HI_RISE_TO 27         //   to this one,
#define HI_RISE 3.4636f               //   this far a frame (the clip's root rise over it, drawn: taken off the clip)
#define HI_DRIFT 0.6f             // READING: the stick moves him sideways this fast through the leap and dive
#define HI_TURN_END 11       // clip: the frames of the turn over
#define HI_FALL_SPEED 3.5f   // READING: the dive's speed downward
#define HI_FALL_MOST 90     // READING: a dive that has not landed after this many frames ends, helpless
#define HI_BOUND_END 40     // clip: the frames of the blast as he lands (the game names no frame to act from)
#define HI_LANDING 29.0f         // the game's list of motions: frames of landing after the helpless fall
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
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
static void Hi_Drift(FighterData *fd)
{
    fd->phys.self_vel.X = Stick_X(fd) * HI_DRIFT;
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    // he leaves the ground as the rise begins (the script's flag is earlier: set airborne with no speed up yet, the
    // floor took him straight back into his landing, round 1)
    if (frame >= HI_UP && frame >= HI_RISE_FROM && fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    if (frame >= HI_RISE_TO || FrameTimerCheck(gobj) == 0)
    {
        Enter(gobj, ST_HI_TURN);
        fd->phys.self_vel.Y = 0;
    }
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
    if (f >= HI_RISE_FROM && f < HI_RISE_TO)
        fd->phys.self_vel.Y = HI_RISE;
    else
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
    Hi_Drift(fd);
}
// on the ground before he leaves it he stays at an edge; in the air a ledge he faces is caught, a floor landed on
void Hi_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
}
void HiTurn_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (Tick(gobj) >= HI_TURN_END || FrameTimerCheck(gobj) == 0)
    {
        Enter(gobj, ST_HI_FALL);
        fd->phys.self_vel.Y = -HI_FALL_SPEED;
    }
}
void HiTurn_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    fd->phys.self_vel.Y = 0;
    Hi_Drift(fd);
}
void HiFall_Anim(GOBJ *gobj)
{
    if (Tick(gobj) >= HI_FALL_MOST)
        Leave(gobj, 1, HI_LANDING);
}
void HiFall_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    fd->phys.self_vel.Y = -HI_FALL_SPEED;
    Hi_Drift(fd);
}
// the dive lands in the blast (the game's `specialhibound`, its own fire hits)
static void HiFall_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    Enter(gobj, ST_HI_BOUND);
    fd->phys.self_vel_ground.X = 0;
}
void HiFall_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, HiFall_Landed);
}
void HiBound_Anim(GOBJ *gobj)
{
    if (Tick(gobj) >= HI_BOUND_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, HI_LANDING);
}
void HiBound_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
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

// ---- Revenge: a counter that strikes back and keeps power for his next hit ---------------------------------------
#define ST_LW 382
#define ST_LW_AIR 383
#define ST_LW_HIT 384
#define ST_LW_HIT_AIR 385
#define LW_ON 3                   // script: it catches a hit from this frame (its first flag)
#define LW_OFF 28                 //   to the frame before this one (its second)
#define LW_DONE 50               // script: the frame he may act from when nothing struck
#define LW_STRIKE_END 27   // script: the frame he may act from after the strike
#define LW_JOINT 5             // READING: the joint the ball rides (his hip: a newer game's counter holds the body)
#define LW_HEIGHT 0.0f
#define LW_RADIUS 11.638f           // READING: the ball's size: round his whole body from the hip
#define LW_MUL 0.4000000059604645f                 // vl.prc special_lw_hit_damage_mul: the strike, for the hit's damage times this,
#define LW_LEAST 2             //   no less than the script's own (its hits' damage)
#define REV_GAIN 0.05f             // STAND-IN: the power gained for each point of damage taken,
#define REV_MOST 1.6f             //   at most this
#define REV_TIME 3600             // vl.prc special_lw_revenge_time, read as seconds: the power is spent after it
#define CATCH ((void (*)(GOBJ *, ShieldDesc *, void *))0x8007B1B8)    // ftColl_8007B1B8: the engine's counter ball (Marth's)
static const ShieldDesc lw_ball = {LW_JOINT, 0, {0, LW_HEIGHT, 0}, LW_RADIUS, 0, 0, 0};
static int lw_took[6];                  // by player: the damage of the hit he caught
static float rev_power[6];              // by player: what his next hit's damage is multiplied by (1: none kept)
static int rev_left[6];                 //   frames before it is spent
static int rev_set[6][4];               //   the damage each hitbox was given (-1: none given)
static void (*rev_step[6])(GOBJ *);     //   the state's own physics call, wrapped

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
    Enter(gobj, state);
    lw_took[fd->ply] = 0;
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
// a hit touched the ball (called by the engine as it takes the hit): he strikes back, and keeps power
void Lw_Caught(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, took = fd->shield.dmg_taken2;     // (Marth's reads the same: x19A4)
    float power;
    fd->flags.shield_enable = 0;
    lw_took[p] = took > 0 ? took : 0;
    power = 1.0f + (float)lw_took[p] * REV_GAIN;
    if (power > REV_MOST)
        power = REV_MOST;
    if (power > rev_power[p])
        rev_power[p] = power;
    rev_left[p] = REV_TIME;
    Enter(gobj, fd->phys.air_state == 0 ? ST_LW_HIT : ST_LW_HIT_AIR);
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
    if (frame >= LW_ON && frame < LW_OFF)
    {
        if (!fd->flags.shield_enable)
            Lw_Catch(gobj);
    }
    else
        fd->flags.shield_enable = 0;
    if (frame >= LW_DONE || FrameTimerCheck(gobj) == 0)
    {
        fd->flags.shield_enable = 0;
        Leave(gobj, 0, 0);
    }
}
void Lw_Phys(GOBJ *gobj)
{
    Plain_Phys(gobj);
    ((FighterData *)gobj->userdata)->shield_bubble.is_checked = 0;  // (Marth's own each frame: the ball follows its joint)
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
    if (Tick(gobj) >= LW_STRIKE_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// the strike's damage, put on its hits after the script has made them and before they strike anything
void LwHit_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int i, damage = (int)((float)lw_took[fd->ply] * LW_MUL + 0.5f);
    Plain_Phys(gobj);
    if (damage < LW_LEAST)
        damage = LW_LEAST;
    for (i = 0; i < 4; i++)
        if (fd->hitbox[i].active && fd->hitbox[i].dmg != damage)
            Fighter_UpdateHitboxDamage(&fd->hitbox[i], damage, gobj);
}
// the power kept: laid on each hitbox of his as it comes out (after the script has made it, before it strikes),
// and spent when one of them has struck something, or when its time runs out
static void Rev_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, i;
    if (rev_step[p])
        rev_step[p](gobj);
    if (rev_power[p] <= 1.0f)
        return;
    for (i = 0; i < 4; i++)
    {
        ftHit *h = &fd->hitbox[i];
        if (!h->active)
        {
            rev_set[p][i] = -1;
            continue;
        }
        if (h->victims[0].data)
        {
            rev_power[p] = 1.0f;        // (it struck: spent)
            return;
        }
        if (h->dmg != rev_set[p][i] && h->attribute != GRAB_ELEMENT)
        {
            Fighter_UpdateHitboxDamage(h, (int)((float)h->dmg * rev_power[p] + 0.5f), gobj);
            rev_set[p][i] = h->dmg;
        }
    }
}
static void Rev_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply;
    if (rev_power[p] <= 1.0f)
        return;
    if (--rev_left[p] <= 0 || fd->state_id < ASID_WAIT)
    {
        rev_power[p] = 1.0f;
        return;
    }
    // (the counter's own strike is not given it: its damage is set by LwHit_Phys)
    if (fd->state_id == ST_LW_HIT || fd->state_id == ST_LW_HIT_AIR)
        return;
    if ((void *)fd->cb.Phys != (void *)Rev_Phys)
    {
        int i;
        rev_step[p] = fd->cb.Phys;
        fd->cb.Phys = Rev_Phys;
        for (i = 0; i < 4; i++)
            if (!fd->hitbox[i].active)
                rev_set[p][i] = -1;
    }
}

// ---- once as he is made, and once a frame -------------------------------------------------------------------------
static void OwnLoad(FighterData *fd)
{
    int p = fd->ply, i;
    t_frame[p] = 0;
    w_helpless[p] = 0;
    s_whip[p] = 0;
    lw_took[p] = 0;
    rev_power[p] = 1.0f;
    rev_left[p] = 0;
    rev_step[p] = 0;
    for (i = 0; i < 4; i++)
        rev_set[p][i] = -1;
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Helpless_Frame(fd);
    Whip_Frame(fd, gobj);
    Rev_Frame(fd, gobj);
}

static int Other_Form(int state)
{
    switch (state)
    {
    case ST_N: return ST_N_AIR;
    case ST_N_AIR: return ST_N;
    case ST_S: return ST_S_AIR;
    case ST_S_AIR: return ST_S;
    case ST_LW: return ST_LW_AIR;
    case ST_LW_AIR: return ST_LW;
    case ST_LW_HIT: return ST_LW_HIT_AIR;
    case ST_LW_HIT_AIR: return ST_LW_HIT;
    }
    return state;
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 11
static const s8 face_rest[FACE_GROUPS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {57, 3, 1},
    {63, 3, 2},
    {64, 3, 0},
    {119, 3, 2},
    {120, 3, 1},
    {125, 3, 0},
    {262, 3, 1},
    {268, 3, 2},
    {269, 3, 0},
    {357, 3, 2},
    {358, 3, 1},
    {364, 3, 0},
    {507, 3, 1},
    {513, 3, 2},
    {514, 3, 0},
    {32767, 0, 0},
};
#define FACE_CLOCK_END 600
static const u16 face_left[] = {0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 0, 8, 8, 8, 8, 0, 8, 0, 8};
// (a fighter made for a screen that is not a match plays the rows of its other table of actions)
static const u16 face_left_other[] = {0, 0, 0, 8, 0, 0, 8, 8, 8, 8, 8, 8, 8, 8};
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
    if (!(s == 373 || s == 374 || s == 375 || s == 376 || s == 377 || s == 378 || s == 382 || s == 383) || fd->state.frame > 4.0f)
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
    if (fd->phys.air_state != 0 || !(a == 55 || a == 58 || a == 59 || a == 304))
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
    {309, 8, 0, {255, 255, 255, 0}, {255, 255, 255, 191}},
    {309, 9, 0, {255, 255, 255, 191}, {178, 178, 178, 128}},
    {309, 11, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {309, 13, 0, {171, 0, 199, 79}, {0, 0, 0, 0}},
    {309, 15, 0, {178, 178, 178, 0}, {178, 178, 178, 128}},
    {309, 17, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {309, 19, 0, {171, 0, 199, 79}, {178, 178, 178, 128}},
    {309, 21, 0, {178, 178, 178, 128}, {0, 0, 0, 0}},
    {318, 8, 0, {255, 255, 255, 0}, {255, 255, 255, 191}},
    {318, 9, 0, {255, 255, 255, 191}, {178, 178, 178, 128}},
    {318, 11, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {318, 13, 0, {171, 0, 199, 79}, {0, 0, 0, 0}},
    {318, 15, 0, {178, 178, 178, 0}, {178, 178, 178, 128}},
    {318, 17, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {318, 19, 0, {171, 0, 199, 79}, {178, 178, 178, 128}},
    {318, 21, 0, {178, 178, 178, 128}, {0, 0, 0, 0}},
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
    {306, 0x340211, 0x12, 0x0, N_Anim, 0, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 373: Darkest Lariat
    {315, 0x340611, 0x12, 0x0, N_Anim, 0, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 374: Darkest Lariat, in the air
    {304, 0x340212, 0x13, 0x0, S_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: Alolan Whip: the lunge
    {313, 0x340612, 0x13, 0x0, S_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: Alolan Whip: the lunge, in the air
    {305, 0x340213, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 377: Cross Chop: the leap
    {314, 0x340613, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 378: Cross Chop: the leap, in the air
    {307, 0x340613, 0x14, 0x0, HiTurn_Anim, 0, HiTurn_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 379: Cross Chop: over at the top
    {316, 0x340613, 0x14, 0x0, HiFall_Anim, 0, HiFall_Phys, HiFall_Coll, Fighter_UpdateCameraBox},    // 380: Cross Chop: the dive
    {308, 0x340213, 0x14, 0x0, HiBound_Anim, 0, Plain_Phys, HiBound_Coll, Fighter_UpdateCameraBox},    // 381: Cross Chop: the blast as he lands
    {309, 0x340214, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 382: Revenge: the pose
    {318, 0x340614, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 383: Revenge: the pose, in the air
    {323, 0x340214, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 384: Revenge: the strike back
    {325, 0x340614, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 385: Revenge: the strike back, in the air
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
