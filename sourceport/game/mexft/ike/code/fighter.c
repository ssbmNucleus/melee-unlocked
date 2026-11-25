#include "../mex.h"

// ---- Ike's own states --------------------------------------------------------------------------
#define ST_QD 373                      // Quick Draw: start, hold, dash, swing, end; then the same five in the air
#define ST_PLUNGE 383
#define ST_SLAM 384
#define ST_SIDE 349                  // the donor's states the side and up specials start in
#define ST_SIDE_AIR 358
#define ST_UP 367
#define ST_UP_AIR 368
#define B_BUTTON 0x200
#define QD_MAX 70                 // frames of holding that still add to it
#define QD_SPEED 4.0982878922041674f          // the dash's speed with no charge, on the ground and in the air,
#define QD_SPEED_AIR 3.512818259787791f
#define QD_GAIN 0.05854697123880148f            //   and what each frame of charge adds to it
#define QD_GAIN_AIR 0.03903131415920099f
#define QD_DAMAGE 9.0f        // the swing's damage with no charge, and what each frame of charge adds
#define QD_DAMAGE_GAIN 0.10000000149011612f
#define QD_REACH 8.782045882114245f          // how near ahead a fighter must be for him to swing, and how far above or below
#define QD_TALL 8.782045882114245f
#define QD_SWING_SPEED 0.9757828757904716f  // the speed forward the swing starts with (its script's own)
#define QD_END 0.5f                 // the end: what is left of the dash's speed as it begins, and what each
#define QD_BRAKE 0.29273487436937984f             //   frame then takes off it, on the ground and in the air
#define QD_BRAKE_AIR 0.09757828903307696f
#define PLUNGE_SPEED 5.854697254742829f  // the speed he comes down at (the plunge's script's own)
#define UP_LAST 63.0f            // the last frame of the rise
#define FALL_LAG 20.0f          // frames of landing after a special that leaves him helpless
#define RECOIL 10.0f              // what a full charge of the neutral special costs him
#define LEDGE_CHECK ((void *)0x80096CC8)    // what the donor's own up special looks for a ledge with (ftCo_80096CC8)

static u8 qd_charge[6];
static int last_state[6];

static void OwnLoad(FighterData *fd)
{
    qd_charge[fd->ply] = 0;
    last_state[fd->ply] = -1;
}
static void Enter(GOBJ *gobj, int state)
{
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
}
static int Qd(FighterData *fd, int step)
{
    return ST_QD + step + (fd->phys.air_state == 0 ? 0 : 5);
}
static float Qd_Speed(FighterData *fd)
{
    float c = (float)qd_charge[fd->ply];
    return fd->phys.air_state == 0 ? QD_SPEED + QD_GAIN * c : QD_SPEED_AIR + QD_GAIN_AIR * c;
}
// out of a special: standing, or (in the air) falling helpless, as the game leaves him
static void Leave(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, FALL_LAG);
}

// Quick Draw. Where he stands he slows; in the air he falls while he crouches and charges.
void Qd_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Qd_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_IgnoreLedge(gobj, Fighter_EnterLanding);
}
void QdStart_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Enter(gobj, Qd(gobj->userdata, 1));
}
// held as long as the button is: the charge grows to its most and stays there
void QdHold_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (qd_charge[fd->ply] < QD_MAX)
        qd_charge[fd->ply]++;
    if (!(fd->input.held & B_BUTTON))
        Enter(gobj, Qd(fd, 2));
    else if (FrameTimerCheck(gobj) == 0)
        Enter(gobj, Qd(fd, 1));
}
// the dash: straight ahead at the speed the charge gives; on the ground he stops at an edge
void QdDash_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float v = Qd_Speed(fd) * fd->facing_direction;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = v + fd->attr.ground_friction * fd->facing_direction;
        Fighter_PhysGround_ApplyFriction(gobj);
    }
    else
    {
        fd->phys.self_vel.X = v;
        fd->phys.self_vel.Y = 0;
    }
}
void QdDash_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, LEDGE_CHECK, Fighter_EnterLanding);
}
// he swings at the first fighter he reaches; with no one in his way the dash runs out
void QdDash_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float reach = QD_REACH + Qd_Speed(fd);
    int i;
    for (i = 0; i < 6; i++)
    {
        GOBJ *other = Fighter_GetGObj(i);
        if (other && other != gobj)
        {
            FighterData *od = other->userdata;
            float dx = (od->phys.pos.X - fd->phys.pos.X) * fd->facing_direction;
            float dy = od->phys.pos.Y - fd->phys.pos.Y;
            if (dx > -2.0f && dx < reach && dy > -QD_TALL && dy < QD_TALL)
            {
                Enter(gobj, Qd(fd, 3));
                if (fd->phys.air_state == 0)
                    fd->phys.self_vel_ground.X = QD_SWING_SPEED * fd->facing_direction;
                else
                    fd->phys.self_vel.X = QD_SWING_SPEED * fd->facing_direction;
                return;
            }
        }
    }
    if (FrameTimerCheck(gobj) == 0)
    {
        Enter(gobj, Qd(fd, 4));
        if (fd->phys.air_state == 0)
            fd->phys.self_vel_ground.X = Qd_Speed(fd) * fd->facing_direction * QD_END;
        else
            fd->phys.self_vel.X = Qd_Speed(fd) * fd->facing_direction * QD_END;
    }
}
// the swing: its script makes the hit, and the charge sets the hit's damage here (after the script has made it
// and before it strikes anything)
void QdSwing_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int i, damage = (int)(QD_DAMAGE + QD_DAMAGE_GAIN * (float)qd_charge[fd->ply] + 0.5f);
    Qd_Phys(gobj);
    for (i = 0; i < 4; i++)
        if (fd->hitbox[i].active)
            Fighter_UpdateHitboxDamage(&fd->hitbox[i], damage, gobj);
}
// the end: he keeps part of the dash's speed and is braked to a stop (in the air he falls as he is braked)
static void Qd_Brake(float *v, float by)
{
    if (*v > by)
        *v -= by;
    else if (*v < -by)
        *v += by;
    else
        *v = 0;
}
void QdEnd_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        // (the game's friction step is what turns ground speed into movement: the brake is that friction and the rest)
        Qd_Brake(&fd->phys.self_vel_ground.X, QD_BRAKE - fd->attr.ground_friction);
        Fighter_PhysGround_ApplyFriction(gobj);
    }
    else
    {
        Qd_Brake(&fd->phys.self_vel.X, QD_BRAKE_AIR);
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
    }
}
void QdEnd_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj);
}

// Aether after the rise: straight down with the sword until he lands, catching a ledge he faces on the way
void Plunge_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = -PLUNGE_SPEED;
}
void Plunge_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Enter(gobj, ST_PLUNGE);
}
static void Plunge_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    Enter(gobj, ST_SLAM);
}
void Plunge_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, LEDGE_CHECK, Plunge_Landed);
}
void Slam_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Fighter_EnterWait(gobj);
}
void Slam_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
void Slam_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
}

static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id, was = last_state[p];
    last_state[p] = s;
    // the side special: the donor's state is left at once for the fighter's own
    if (s == ST_SIDE || s == ST_SIDE_AIR)
    {
        qd_charge[p] = 0;
        Enter(gobj, Qd(fd, 0));
        last_state[p] = fd->state_id;
        return;
    }
    // the up special: where the rise ends, the plunge begins
    if ((s == ST_UP || s == ST_UP_AIR) && fd->state.frame >= UP_LAST)
    {
        if (fd->phys.air_state == 0)
            Fighter_SetAirborne(fd);
        Enter(gobj, ST_PLUNGE);
        last_state[p] = fd->state_id;
        return;
    }
    // the neutral special let go at its fullest costs him some damage of his own
    if ((s == 344 || s == 348) && s != was)
        Fighter_GiveDamage(fd, RECOIL);
}

// Moves that cannot be knocked out of: armour while their clips are between the frames the game gives.
#define ARMOUR 4321.0f      // more than any hit's knockback; also how this code knows its own
static void ArmourFrame(FighterData *fd)
{
    int on = 0, a = fd->action_id;
    float at = fd->state.frame + 1.0f;      // this runs before the frame's clip moves on: the frame a hit will find
    if ((a == 301) && at >= 6.0f && at <= 10.0f)
        on = 1;
    if ((a == 297) && at >= 6.0f && at <= 10.0f)
        on = 1;
    if ((a == 322) && at >= 18.0f && at <= 38.0f)
        on = 1;
    if ((a == 321) && at >= 18.0f && at <= 38.0f)
        on = 1;
    if (on)
        fd->dmg.armor = ARMOUR;
    else if (fd->dmg.armor == ARMOUR)
        fd->dmg.armor = 0;
}

// A counter strikes back for what it took, times the fighter's own number and no less than its least. The
// donor keeps what it took (times the number in its data) and its script makes the hit: the damage is put on
// that hit in the state's own step, after the script has made it and before it strikes anything.
#define COUNTER_LEAST 10
static void (*counter_step[6])(GOBJ *);
static void Counter_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int i, damage = fd->state_var.state_var1;
    if (counter_step[fd->ply])
        counter_step[fd->ply](gobj);
    if (damage < COUNTER_LEAST)
        damage = COUNTER_LEAST;
    for (i = 0; i < 4; i++)
        if (fd->hitbox[i].active)
            Fighter_UpdateHitboxDamage(&fd->hitbox[i], damage, gobj);
}
static void CounterFrame(FighterData *fd)
{
    int a = fd->action_id;
    if ((a == 324 || a == 326) && (void *)fd->cb.Phys != (void *)Counter_Phys)
    {
        counter_step[fd->ply] = fd->cb.Phys;
        fd->cb.Phys = Counter_Phys;
    }
}

// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (!(s == 341 || s == 345 || s == 349 || s == 358 || s == 367 || s == 368 || s == 369 || s == 371 || s == 373 || s == 378) || fd->state.frame > 4.0f)
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
    if (fd->phys.air_state != 0 || !(a == 67))
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
    {296, 1, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 2, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 10, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 12, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 13, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 21, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 23, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 24, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 32, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 34, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 35, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 43, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 45, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 46, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 54, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 56, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 57, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 65, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 67, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 68, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 76, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 78, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 79, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 87, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 89, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 90, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 98, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 100, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 101, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 109, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 111, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 112, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 120, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 122, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 123, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 131, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 133, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 134, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 142, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 144, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 145, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 153, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 155, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 156, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 164, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 166, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 167, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 175, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 177, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {296, 178, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {296, 186, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {296, 188, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {296, 189, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {296, 193, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {296, 194, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {296, 198, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {296, 199, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {296, 203, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {296, 204, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {296, 208, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {296, 209, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {296, 213, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {296, 214, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {296, 218, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {296, 219, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {296, 223, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {296, 224, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {297, 46, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {298, 46, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 1, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 2, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 10, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 12, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 13, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 21, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 23, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 24, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 32, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 34, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 35, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 43, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 45, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 46, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 54, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 56, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 57, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 65, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 67, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 68, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 76, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 78, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 79, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 87, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 89, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 90, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 98, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 100, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 101, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 109, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 111, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 112, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 120, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 122, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 123, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 131, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 133, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 134, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 142, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 144, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 145, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 153, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 155, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 156, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 164, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 166, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 167, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 175, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 177, 0, {255, 144, 128, 0}, {255, 144, 128, 80}},
    {300, 178, 8, {255, 144, 128, 80}, {255, 0, 0, 0}},
    {300, 186, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {300, 188, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {300, 189, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {300, 193, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {300, 194, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {300, 198, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {300, 199, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {300, 203, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {300, 204, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {300, 208, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {300, 209, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {300, 213, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {300, 214, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {300, 218, 0, {255, 192, 128, 0}, {255, 192, 128, 150}},
    {300, 219, 4, {255, 192, 128, 150}, {255, 0, 0, 0}},
    {300, 223, 0, {255, 128, 0, 0}, {255, 128, 0, 180}},
    {300, 224, 4, {255, 128, 0, 180}, {255, 0, 0, 0}},
    {301, 46, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {302, 46, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {323, 11, 0, {255, 255, 255, 0}, {255, 255, 255, 190}},
    {323, 12, 0, {255, 255, 255, 190}, {180, 180, 180, 130}},
    {323, 14, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {323, 16, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {323, 18, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {323, 20, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {323, 22, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {323, 24, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {323, 26, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {323, 28, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {323, 30, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {323, 32, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {323, 34, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {324, 3, 0, {255, 255, 255, 0}, {255, 255, 255, 220}},
    {324, 4, 0, {255, 255, 255, 220}, {180, 180, 180, 80}},
    {324, 4, 11, {180, 180, 180, 80}, {0, 0, 0, 0}},
    {324, 15, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {325, 11, 0, {255, 255, 255, 0}, {255, 255, 255, 190}},
    {325, 12, 0, {255, 255, 255, 190}, {180, 180, 180, 130}},
    {325, 14, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {325, 16, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {325, 18, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {325, 20, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {325, 22, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {325, 24, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {325, 26, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {325, 28, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {325, 30, 0, {180, 180, 180, 0}, {180, 180, 180, 130}},
    {325, 32, 0, {180, 180, 180, 130}, {170, 0, 200, 80}},
    {325, 34, 0, {170, 0, 200, 80}, {0, 0, 0, 0}},
    {326, 3, 0, {255, 255, 255, 0}, {255, 255, 255, 220}},
    {326, 4, 0, {255, 255, 255, 220}, {180, 180, 180, 80}},
    {326, 4, 11, {180, 180, 180, 80}, {0, 0, 0, 0}},
    {326, 15, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
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
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    ArmourFrame(fd);
    CounterFrame(fd);
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
    {304, 0x340213, 0x13, 0x0, QdStart_Anim, 0, Qd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 373: Quick Draw starts
    {305, 0x340213, 0x13, 0x0, QdHold_Anim, 0, Qd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 374: Quick Draw is held
    {306, 0x340213, 0x13, 0x0, QdDash_Anim, 0, QdDash_Phys, QdDash_Coll, Fighter_UpdateCameraBox},    // 375: Quick Draw dashes
    {307, 0x340213, 0x13, 0x0, QdEnd_Anim, 0, QdSwing_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 376: Quick Draw swings
    {308, 0x340213, 0x13, 0x0, QdEnd_Anim, 0, QdEnd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 377: Quick Draw ends
    {313, 0x340613, 0x13, 0x0, QdStart_Anim, 0, Qd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 378: Quick Draw starts in the air
    {314, 0x340613, 0x13, 0x0, QdHold_Anim, 0, Qd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 379: Quick Draw is held in the air
    {315, 0x340613, 0x13, 0x0, QdDash_Anim, 0, QdDash_Phys, QdDash_Coll, Fighter_UpdateCameraBox},    // 380: Quick Draw dashes in the air
    {316, 0x340613, 0x13, 0x0, QdEnd_Anim, 0, QdSwing_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 381: Quick Draw swings in the air
    {317, 0x340613, 0x13, 0x0, QdEnd_Anim, 0, QdEnd_Phys, Qd_Coll, Fighter_UpdateCameraBox},    // 382: Quick Draw ends in the air
    {318, 0x340613, 0x14, 0x0, Plunge_Anim, 0, Plunge_Phys, Plunge_Coll, Fighter_UpdateCameraBox},    // 383: Aether's plunge
    {309, 0x340213, 0x14, 0x0, Slam_Anim, 0, Slam_Phys, Slam_Coll, Fighter_UpdateCameraBox},    // 384: Aether's slam
};

// The fighter's own sword trail: where the game's trail runs along the blade, in its trail's colour.
static AfterImageDesc trail = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {255, 232, 175, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 2.368, .offset_top = 12.092,
};

AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    return &trail;
}
