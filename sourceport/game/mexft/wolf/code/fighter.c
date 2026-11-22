#include "../mex.h"

// ---- Wolf's own states ------------------------------------------------------------------------
#define B_BUTTON 0x200
// what a script's effect command calls: an effect on one of his joints (his own are 6000 and on)
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
static u8 w_age[6];                 // frames of the state of his own he is in (1 on the frame it begins)
static u8 w_helpless[6];            // he has used a special that leaves him helpless, and has not stood since

// (the frame a state is entered on already runs its movement: it is the state's first)
static void Enter(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
    w_age[fd->ply] = 1;
}
// the same state's other form (ground for air, air for ground), on the frame its clip is at
static void Hand_Over(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(fd->state.frame, 1, 0, gobj, state, 0x5000, 0);
}
static void Turn_Round(FighterData *fd)
{
    fd->facing_direction = -fd->facing_direction;
    Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
}
static void Brake(float *v, float by)
{
    if (*v > by)
        *v -= by;
    else if (*v < -by)
        *v += by;
    else
        *v = 0;
}
static GOBJ *Make_Item(GOBJ *gobj, int which, Vec3 *at)
{
    FighterData *fd = gobj->userdata;
    SpawnItem spawn;
    GOBJ *item;
    spawn.parent_gobj = gobj;
    spawn.parent_gobj2 = gobj;
    spawn.it_kind = MEX_GetFtItemID(gobj, which);
    spawn.pos = *at;
    spawn.pos.Z = 0;
    spawn.pos2 = spawn.pos;
    spawn.vel.X = 0;
    spawn.vel.Y = 0;
    spawn.vel.Z = 0;
    spawn.facing_direction = fd->facing_direction;
    spawn.damage = 0;
    spawn.unk6 = 0;
    spawn.is_raycast_below = 1;
    item = Item_CreateItem1(&spawn);
    if (item)
    {
        ItemStateChange(item, 0, 2);                    // (an item does nothing until it is given a state)
        Item_CopyDevelopState(item, gobj);
    }
    return item;
}
// out of a special: standing, or (in the air) falling; helpless after the two that leave him so
static void Leave(GOBJ *gobj, float landing)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else if (w_helpless[fd->ply])
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, landing);
    else
        Fighter_EnterFall(gobj);
}
#define ST_FIRST_OWN 373

// ---- Blaster: a stab of the gun's blade, and a laser that flies a set time
#define ST_N 373
#define ST_N_AIR 374
#define LASER 0                   // the laser's item number
#define N_FIRE 16                 // script: the frame the laser is let go on
#define N_END 53                   // script: the frame he may act from
#define MUZZLE_X 12.801458889044284f             // script: where the flash of the shot is drawn, ahead of him,
#define MUZZLE_Y 9.50411373993063f             //   and above his feet (READING: the laser starts there)

static void Shoot(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    GOBJ *shot;
    at.X = fd->phys.pos.X + MUZZLE_X * fd->facing_direction;
    at.Y = fd->phys.pos.Y + MUZZLE_Y;
    at.Z = 0;
    shot = Make_Item(gobj, LASER, &at);
    if (shot)
        ((ItemData *)shot->userdata)->item_var.var1 = 1;        // its own code sends it on its way
}
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
    int p = fd->ply;
    if (w_age[p] < 250)
        w_age[p]++;
    if (w_age[p] == N_FIRE)
        Shoot(gobj);
    if (w_age[p] >= N_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0);
}
void N_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
    {
        // (READING: in the air he falls and drifts as he would in any attack in the air)
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
        Fighter_PhysAir_ApplyAerialDrift(fd);
    }
}
// the move goes on where it was when he walks off an edge or comes down
void N_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj, ST_N_AIR);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj, ST_N);
    }
}

// ---- Wolf Flash: he gathers himself, crosses a set distance in two frames, and strikes where he arrives
#define ST_S_START 375
#define ST_S_START_AIR 376
#define ST_S_DASH 377
#define ST_S_END 378
#define S_START_FRAMES 16 // clip and script: frames before the dash (the clip's length at the script's speeds)
#define S_DASH_FRAMES 2   // clip: frames of the dash
#define S_DASH_X 18.420643629677496f             // clip: what its root travels ahead in one of them,
#define S_DASH_Y 8.704022732780459f             //   and upward, times illusion_rush_speed_mul
#define S_TILT 0.17453292519943295f                 // max_rush_degree, in radians: the stick held up or down turns the dash by this
#define S_STICK 0.5f               // min_stick_y: how far the stick must be held for that
#define S_START_X 0.6700000166893005f           // illusion_start_x_mul: what he keeps of his speed sideways as it starts
#define S_STOP_Y 13             // illusion_stop_y_frame: frames he hangs in the air as it starts
#define S_ACCEL_X 0.05000000074505806f           // illusion_accel_x: speed sideways he loses a frame while he gathers himself
#define S_ACCEL_Y 0.017000000923871994f           // illusion_accel_y: speed he gains downward a frame after he has hung
#define S_END_X 2.299999952316284f               // illusion_end_air_speed_x: his speed ahead as he arrives,
#define S_END_BRAKE 0.07000000029802322f       // illusion_end_air_brake_x: which loses this a frame,
#define S_END_FALL 0.07999999821186066f         // illusion_end_air_accel_y: and what he gains downward a frame
#define S_LANDING 29.0f           // frames of landing after it (see the build's note)

static void S_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    fd->phys.self_vel.X *= S_START_X;
    fd->phys.self_vel_ground.X *= S_START_X;
    if (fd->phys.air_state != 0)
        fd->phys.self_vel.Y = 0;
}
void SpecialS(GOBJ *gobj)
{
    S_Begin(gobj, ST_S_START);
}
void SpecialAirS(GOBJ *gobj)
{
    S_Begin(gobj, ST_S_START_AIR);
}
static void Dash_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float sy = fd->input.lstick.Y, tilt = 0, c, s;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    Enter(gobj, ST_S_DASH);
    // (READING: past min_stick_y the dash is turned by the whole of max_rush_degree)
    if (sy >= S_STICK)
        tilt = S_TILT;
    else if (sy <= -S_STICK)
        tilt = -S_TILT;
    c = cos(tilt);
    s = sin(tilt);
    fd->phys.self_vel.X = (S_DASH_X * c - S_DASH_Y * s) * fd->facing_direction;
    fd->phys.self_vel.Y = S_DASH_X * s + S_DASH_Y * c;
    w_helpless[fd->ply] = 1;
}
void SStart_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    if (w_age[p] > S_START_FRAMES)
        Dash_Begin(gobj);
}
void SStart_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    Brake(&fd->phys.self_vel.X, S_ACCEL_X);
    if (w_age[fd->ply] > S_STOP_Y)
        fd->phys.self_vel.Y -= S_ACCEL_Y;
    else
        fd->phys.self_vel.Y = 0;
}
void SStart_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj, ST_S_START_AIR);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj, ST_S_START);
    }
}
static void End_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_S_END);
    fd->phys.self_vel.X = S_END_X * fd->facing_direction;
    fd->phys.self_vel.Y = 0;
}
void Dash_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    if (w_age[p] > S_DASH_FRAMES)
        End_Begin(gobj);
}
void Dash_Phys(GOBJ *gobj)
{
}
static void S_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, S_LANDING);
}
// (the dash passes ledges; a floor it meets is landed on, a wall stops it)
void Dash_Coll(GOBJ *gobj)
{
    Fighter_CollAir_IgnoreLedge(gobj, S_Landed);
}
void SEnd_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (w_age[p] < 250)
        w_age[p]++;
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj, S_LANDING);
}
void SEnd_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Brake(&fd->phys.self_vel.X, S_END_BRAKE);
    fd->phys.self_vel.Y -= S_END_FALL;
    if (fd->phys.self_vel.Y < -fd->attr.terminal_velocity)
        fd->phys.self_vel.Y = -fd->attr.terminal_velocity;
}
// a ledge he faces is caught as he comes down (the engine's own rule for a recovery); a floor is landed on
void SEnd_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, S_Landed);
}

// ---- Fire Wolf: he gathers himself, flies the way the stick is held, and ends with a kick
#define ST_HI_HOLD 379
#define ST_HI_HOLD_AIR 380
#define ST_HI_RUSH 381
#define ST_HI_FALL 382
#define ST_HI_LANDING 383
#define XROT_JOINT 2               // the joint the engine pitches a fighter about
#define HI_HOLD_FRAMES 25 // clip: frames before the flight (published: its hits begin on 27, the second frame of the flight)
#define HI_RUSH_FRAMES 7 // fire_rush_frame: frames of the flight
#define HI_SPEED 3.799999952316284f             // fire_rush_speed
#define HI_START_X 0.6700000166893005f         // fire_start_x_mul: what he keeps of his speed sideways as it starts
#define HI_STOP_Y 1           // fire_stop_y_frame: frames he hangs in the air as it starts
#define HI_ACCEL_X 0.009999999776482582f         // fire_accel_x: speed sideways he loses a frame while he gathers himself
#define HI_ACCEL_Y 0.019999999552965164f         // fire_accel_y: speed he gains downward a frame after he has hung
#define HI_STICK 0.28f             // READING: how far the stick must be held to aim it (the engine's own dead zone for a special)
#define HI_END_AIR 0.800000011920929f         // end_speed_mul_air: what he keeps of the flight's speed as it ends in the air,
#define HI_END_GROUND 0.699999988079071f   // end_speed_mul_ground: and on the ground
#define HI_BRAKE 0.20000000298023224f             // fire_rush_brake: speed he loses a frame after the flight,
#define HI_BRAKE_FRAMES 4       // fire_rush_brake_frame: for this many frames
#define HI_FALL_X 0.6499999761581421f           // fire_fall_x_mul: what he keeps sideways once that is over
#define HI_END_FALL 0.10000000149011612f       // end_accel_y: speed he gains downward a frame from then on
#define HI_LANDING 36.0f  // fire_landing_frame: frames of landing after it (published: 36)
#define HI_GROUND_END 35   // script: the frame of the landing kick he may act from

static float hi_angle[6];           // the way the flight goes: radians up from straight ahead, the way he faces
static u8 hi_pitched[6];            // his body is turned to the flight

static void Pitch(FighterData *fd, float by)
{
    Fighter_SetBoneRotX(fd, XROT_JOINT, by);
    hi_pitched[fd->ply] = by != 0;
}

// The fire. The game's script lays the flight's fire on his Rot joint, which this code turns to the flight; the
// engine's held effects follow a joint's place and the way he faces, not the joint's turn, so the fire is made for
// each HI_FIRE_STEP of the flight's angle (from straight down to straight up) and the one nearest is played. Each
// row: the frame of the flight it starts on (the script's call plus the effect's own delay), the effect, whether it
// ends with the flight (the script's EFFECT_FOLLOW) or not (EFFECT_FOLLOW_NO_STOP). STAND-IN: the game's sheath of
// fire about his body (a 3D shape) is a soft glow held by each joint of his body (hi_sheath: the frame, the effect,
// the joint); the glow about him as he gathers himself the same (hi_glow: the frame of the gathering, the effect,
// on his base joint; on the ground and in the air).
#define HI_FIRE_ANGLES 13
#define HI_FIRE_STEP 0.2617993877991494f     // radians
#define HI_FIRE_COUNT 5
#define HI_SHEATH_COUNT 7
#define HI_GLOW_COUNT 1
#define ROT_JOINT 4           // the joint the script lays the flight's fire on
static const short hi_fire[HI_FIRE_ANGLES][HI_FIRE_COUNT][3] = {
    {{2, 6143, 0}, {2, 6144, 0}, {2, 6169, 1}, {2, 6170, 1}, {2, 6171, 1}},
    {{2, 6145, 0}, {2, 6146, 0}, {2, 6172, 1}, {2, 6173, 1}, {2, 6174, 1}},
    {{2, 6147, 0}, {2, 6148, 0}, {2, 6175, 1}, {2, 6176, 1}, {2, 6177, 1}},
    {{2, 6149, 0}, {2, 6150, 0}, {2, 6178, 1}, {2, 6179, 1}, {2, 6180, 1}},
    {{2, 6151, 0}, {2, 6152, 0}, {2, 6181, 1}, {2, 6182, 1}, {2, 6183, 1}},
    {{2, 6153, 0}, {2, 6154, 0}, {2, 6184, 1}, {2, 6185, 1}, {2, 6186, 1}},
    {{2, 6155, 0}, {2, 6156, 0}, {2, 6187, 1}, {2, 6188, 1}, {2, 6189, 1}},
    {{2, 6157, 0}, {2, 6158, 0}, {2, 6190, 1}, {2, 6191, 1}, {2, 6192, 1}},
    {{2, 6159, 0}, {2, 6160, 0}, {2, 6193, 1}, {2, 6194, 1}, {2, 6195, 1}},
    {{2, 6161, 0}, {2, 6162, 0}, {2, 6196, 1}, {2, 6197, 1}, {2, 6198, 1}},
    {{2, 6163, 0}, {2, 6164, 0}, {2, 6199, 1}, {2, 6200, 1}, {2, 6201, 1}},
    {{2, 6165, 0}, {2, 6166, 0}, {2, 6202, 1}, {2, 6203, 1}, {2, 6204, 1}},
    {{2, 6167, 0}, {2, 6168, 0}, {2, 6205, 1}, {2, 6206, 1}, {2, 6207, 1}}
};
static const short hi_sheath[HI_SHEATH_COUNT][3] = {
    {2, 6208, 10},
    {2, 6208, 7},
    {2, 6208, 5},
    {2, 6208, 110},
    {2, 6208, 104},
    {2, 6208, 111},
    {2, 6208, 105}
};
static const short hi_glow[2][HI_GLOW_COUNT][2] = {     // on the ground, in the air
    {{5, 6209}},
    {{5, 6210}}
};
static void Hi_Fire(GOBJ *gobj, int age)
{
    FighterData *fd = gobj->userdata;
    Vec3 none = {0, 0, 0};
    int i, k = (int)((hi_angle[fd->ply] + 1.5707963f) / HI_FIRE_STEP + 0.5f);
    if (k < 0)
        k = 0;
    if (k >= HI_FIRE_ANGLES)
        k = HI_FIRE_ANGLES - 1;
    for (i = 0; i < HI_FIRE_COUNT; i++)
        if (hi_fire[k][i][1] && age == hi_fire[k][i][0])
            SPAWN_EFFECT(gobj, hi_fire[k][i][1], ROT_JOINT, 0, hi_fire[k][i][2], &none, &none, 0.0f);
    for (i = 0; i < HI_SHEATH_COUNT; i++)
        if (hi_sheath[i][1] && age == hi_sheath[i][0])
            SPAWN_EFFECT(gobj, hi_sheath[i][1], hi_sheath[i][2], 0, 1, &none, &none, 0.0f);
}
static void Hi_Glow(GOBJ *gobj, int age)
{
    FighterData *fd = gobj->userdata;
    Vec3 none = {0, 0, 0};
    int i, air = fd->phys.air_state != 0;
    for (i = 0; i < HI_GLOW_COUNT; i++)
        if (hi_glow[air][i][1] && age == hi_glow[air][i][0])
            SPAWN_EFFECT(gobj, hi_glow[air][i][1], 0, 0, 1, &none, &none, 0.0f);
}
static void Hi_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    fd->phys.self_vel.X *= HI_START_X;
    fd->phys.self_vel_ground.X *= HI_START_X;
    if (fd->phys.air_state != 0)
        fd->phys.self_vel.Y = 0;
}
void SpecialHi(GOBJ *gobj)
{
    Hi_Begin(gobj, ST_HI_HOLD);
}
void SpecialAirHi(GOBJ *gobj)
{
    Hi_Begin(gobj, ST_HI_HOLD_AIR);
}
static void Rush_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X, sy = fd->input.lstick.Y, a;
    // the stick let go: straight up
    if (sx * sx + sy * sy < HI_STICK * HI_STICK)
    {
        sx = 0;
        sy = 1;
    }
    if (sx * fd->facing_direction < 0)
        Turn_Round(fd);
    a = atan2(sy, sx * fd->facing_direction);
    // from the ground, a flight aimed along it or into it runs along it
    if (fd->phys.air_state == 0 && sy > 0.1f)
        Fighter_SetAirborne(fd);
    Enter(gobj, ST_HI_RUSH);
    hi_angle[p] = a;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = HI_SPEED * fd->facing_direction;
        hi_angle[p] = 0;
    }
    else
    {
        fd->phys.self_vel.X = HI_SPEED * cos(a) * fd->facing_direction;
        fd->phys.self_vel.Y = HI_SPEED * sin(a);
    }
    Pitch(fd, -hi_angle[p]);
    w_helpless[p] = 1;
}
void HiHold_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    Hi_Glow(gobj, w_age[p]);
    if (w_age[p] > HI_HOLD_FRAMES)
        Rush_Begin(gobj);
}
void HiHold_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    Brake(&fd->phys.self_vel.X, HI_ACCEL_X);
    if (w_age[fd->ply] > HI_STOP_Y)
        fd->phys.self_vel.Y -= HI_ACCEL_Y;
    else
        fd->phys.self_vel.Y = 0;
}
void HiHold_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj, ST_HI_HOLD_AIR);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj, ST_HI_HOLD);
    }
}
static void HiLanding_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    if (fd->phys.air_state != 0)
        Fighter_SetGrounded2(fd);
    Enter(gobj, ST_HI_LANDING);
    fd->phys.self_vel_ground.X *= HI_END_GROUND;
}
static void HiFall_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    Enter(gobj, ST_HI_FALL);
    fd->phys.self_vel.X *= HI_END_AIR;
    fd->phys.self_vel.Y *= HI_END_AIR;
}
void Rush_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    Hi_Fire(gobj, w_age[p]);
    if (w_age[p] > HI_RUSH_FRAMES)
    {
        if (fd->phys.air_state == 0)
            HiLanding_Begin(gobj);
        else
            HiFall_Begin(gobj);
    }
}
// the flight keeps the speed and the way it began with (along the ground the engine moves a fighter by its speed
// along the ground, which a state's own movement call hands on each frame)
void Rush_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = HI_SPEED * fd->facing_direction;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
}
static void Rush_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    // (the game bounces him off a floor he flies into: here the flight ends in the landing kick)
    fd->phys.self_vel_ground.X = fd->phys.self_vel.X;
    HiLanding_Begin(gobj);
}
void Rush_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
            Fighter_SetAirborne(fd);
    }
    else
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Rush_Landed);
}
void HiFall_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (w_age[p] < 250)
        w_age[p]++;
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj, HI_LANDING);
}
void HiFall_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = w_age[fd->ply];
    float vx = fd->phys.self_vel.X, vy = fd->phys.self_vel.Y;
    if (age <= HI_BRAKE_FRAMES)
    {
        // the speed of the flight, lost along its own way
        float v = sqrtf(vx * vx + vy * vy);
        if (v > HI_BRAKE)
        {
            fd->phys.self_vel.X = vx * (v - HI_BRAKE) / v;
            fd->phys.self_vel.Y = vy * (v - HI_BRAKE) / v;
        }
        else
        {
            fd->phys.self_vel.X = 0;
            fd->phys.self_vel.Y = 0;
        }
        if (age == HI_BRAKE_FRAMES)
            fd->phys.self_vel.X *= HI_FALL_X;
        return;
    }
    fd->phys.self_vel.Y -= HI_END_FALL;
    if (fd->phys.self_vel.Y < -fd->attr.terminal_velocity)
        fd->phys.self_vel.Y = -fd->attr.terminal_velocity;
}
static void HiFall_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
void HiFall_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, HiFall_Landed);
}
void HiLanding_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (w_age[p] < 250)
        w_age[p]++;
    if (w_age[p] >= HI_GROUND_END || FrameTimerCheck(gobj) == 0)
    {
        w_helpless[p] = 0;
        Fighter_EnterWait(gobj);
    }
}
void HiLanding_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
void HiLanding_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
}

// ---- Reflector: a hit as it opens, then a bubble that sends shots back for as long as the button is held
#define ST_LW_START 384
#define ST_LW_START_AIR 385
#define ST_LW_LOOP 386
#define ST_LW_LOOP_AIR 387
#define ST_LW_HIT 388
#define ST_LW_HIT_AIR 389
#define ST_LW_END 390
#define ST_LW_END_AIR 391
#define LW_START_FRAMES 8       // clip and script: frames before the bubble is up
#define LW_MIN 10                 // READING: the fewest frames it is held up (published: the whole move is 30 at its shortest)
#define LW_END 13          // script: the frame of the closing clip he may act from
#define LW_HIT_FRAMES 18   // clip: frames of the clip played as a shot is sent back
#define LW_AIR_X 0.5f             // reflector_air_start_x_mul: what he keeps of his speed sideways, started in the air
#define LW_AIR_FALL 0.12999999523162842f       // reflector_air_accel_y: speed he gains downward a frame while it is up
#define LW_RADIUS 8.243363797664642f           // param_reflector size, in the world's units
#define LW_HEIGHT 6.303748786449432f           // script: how far above his feet the bubble's effect is drawn
#define LW_LIMIT 50             // param_reflector limit: a shot stronger than this breaks through
#define LW_DAMAGE 1.5f           // param_reflector attack_mul
#define LW_SPEED 1.7999999523162842f             // param_reflector speed_mul

static u8 lw_up[6];                 // frames the bubble has been up in this use
static u8 lw_sent[6];               // a shot was sent back this frame
static const ReflectDesc lw_bubble = {0, LW_LIMIT, {0, LW_HEIGHT, 0}, LW_RADIUS, LW_DAMAGE, LW_SPEED, 0};

static void Lw_Reflected(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    lw_sent[fd->ply] = 1;
}
static void Bubble_Up(GOBJ *gobj)
{
    Fighter_CreateReflect(gobj, (ReflectDesc *)&lw_bubble, Lw_Reflected);
}
static void Lw_To(GOBJ *gobj, int ground, int air)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, fd->phys.air_state == 0 ? ground : air);
}
static void Lw_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    lw_up[fd->ply] = 0;
    lw_sent[fd->ply] = 0;
    if (fd->phys.air_state != 0)
        fd->phys.self_vel.X *= LW_AIR_X;
}
void SpecialLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW_START);
}
void SpecialAirLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW_START_AIR);
}
void LwStart_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    if (w_age[p] > LW_START_FRAMES)
    {
        Lw_To(gobj, ST_LW_LOOP, ST_LW_LOOP_AIR);
        Bubble_Up(gobj);
    }
}
void LwLoop_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (lw_up[p] < 250)
        lw_up[p]++;
    if (lw_sent[p])
    {
        lw_sent[p] = 0;
        Lw_To(gobj, ST_LW_HIT, ST_LW_HIT_AIR);
        Bubble_Up(gobj);
        return;
    }
    if (lw_up[p] >= LW_MIN && !(fd->input.held & B_BUTTON))
    {
        Lw_To(gobj, ST_LW_END, ST_LW_END_AIR);
        return;
    }
    Bubble_Up(gobj);                    // (a change of state takes the bubble down: it is put up again each frame)
}
void LwHit_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    if (lw_up[p] < 250)
        lw_up[p]++;
    lw_sent[p] = 0;
    if (w_age[p] > LW_HIT_FRAMES)
    {
        Lw_To(gobj, ST_LW_LOOP, ST_LW_LOOP_AIR);
    }
    Bubble_Up(gobj);
}
void LwEnd_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    w_age[p]++;
    if (w_age[p] >= LW_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0);
}
void Lw_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    fd->phys.self_vel.Y -= LW_AIR_FALL;
    if (fd->phys.self_vel.Y < -fd->attr.terminal_velocity)
        fd->phys.self_vel.Y = -fd->attr.terminal_velocity;
    Fighter_PhysAir_DecayXVelocity(fd, fd->attr.aerial_friction);
}
// each of its states has a form for the ground and one for the air: the same frame of the other, as he leaves
// the ground or comes down
static int Lw_Other(int state)
{
    switch (state)
    {
    case ST_LW_START: return ST_LW_START_AIR;
    case ST_LW_START_AIR: return ST_LW_START;
    case ST_LW_LOOP: return ST_LW_LOOP_AIR;
    case ST_LW_LOOP_AIR: return ST_LW_LOOP;
    case ST_LW_HIT: return ST_LW_HIT_AIR;
    case ST_LW_HIT_AIR: return ST_LW_HIT;
    case ST_LW_END: return ST_LW_END_AIR;
    }
    return ST_LW_END;
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int s = fd->state_id, up = fd->flags.reflect_enable;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) != 0)
            return;
        Fighter_SetAirborne(fd);
    }
    else
    {
        if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) == 0)
            return;
        Fighter_SetGrounded2(fd);
    }
    Hand_Over(gobj, Lw_Other(s));
    if (up)
        Bubble_Up(gobj);
}

// He has dashed or flown. Until he stands again he has no jump left, and an ordinary fall he is found in is made
// the helpless one. Anything else (a ledge, a hit taken, a grab) gives him back what a fighter has after those.
static void Helpless_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    if (hi_pitched[p] && s != ST_HI_RUSH)
        Pitch(fd, 0);
    if (!w_helpless[p])
        return;
    if (fd->phys.air_state == 0)
        w_helpless[p] = 0;
    else if (s >= ST_FIRST_OWN || (s >= ASID_FALLSPECIAL && s <= ASID_FALLSPECIALB))
        fd->jump.jumps_used = fd->attr.max_jumps;
    else
        w_helpless[p] = 0;
}
static void OwnLoad(FighterData *fd)
{
    ItemDesc **items = fd->ftData->items;
    int i;
    w_helpless[fd->ply] = 0;
    hi_pitched[fd->ply] = 0;
    for (i = 0; items && i < 1; i++)
        if (items[i])
            MEX_IndexFighterItem(fd->kind, items[i], i);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Helpless_Frame(fd, gobj);
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 8
static const s8 face_rest[FACE_GROUPS] = {0, 0, 0, 0, 0, -1, -1, -1};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {51, 2, 1},
    {52, 2, 2},
    {54, 2, 1},
    {55, 2, 0},
    {184, 2, 1},
    {185, 2, 2},
    {187, 2, 1},
    {188, 2, 0},
    {298, 2, 1},
    {299, 2, 2},
    {301, 2, 1},
    {302, 2, 0},
    {490, 2, 1},
    {491, 2, 2},
    {493, 2, 1},
    {494, 2, 0},
    {32767, 0, 0},
};
#define FACE_CLOCK_END 600
static const u16 face_left[] = {0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 0, 0, 0, 4, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 0, 4};
// (a fighter made for a screen that is not a match plays the rows of its other table of actions)
static const u16 face_left_other[] = {0, 4, 0, 4, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4};
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
    if (!(s == 373 || s == 374 || s == 375 || s == 376 || s == 379 || s == 380 || s == 384 || s == 385) || fd->state.frame > 4.0f)
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
    if (fd->phys.air_state != 0 || !(a == 55 || a == 58 || a == 59 || a == 303))
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
    {307, 5, 0, {128, 102, 255, 0}, {128, 102, 255, 178}},
    {307, 6, 2, {128, 102, 255, 178}, {77, 0, 255, 0}},
    {307, 8, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {311, 1, 0, {77, 0, 255, 96}, {77, 0, 255, 96}},
    {316, 5, 0, {128, 102, 255, 0}, {128, 102, 255, 178}},
    {316, 6, 2, {128, 102, 255, 178}, {77, 0, 255, 0}},
    {316, 8, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {320, 1, 0, {77, 0, 255, 96}, {77, 0, 255, 96}},
    {324, 1, 0, {255, 153, 204, 0}, {255, 153, 204, 178}},
    {324, 2, 3, {255, 153, 204, 178}, {255, 0, 51, 0}},
    {324, 5, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {326, 1, 0, {255, 153, 204, 0}, {255, 153, 204, 178}},
    {326, 2, 3, {255, 153, 204, 178}, {255, 0, 51, 0}},
    {326, 5, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
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
    {306, 0x340211, 0x12, 0x0, N_Anim, 0, N_Phys, N_Coll, Fighter_UpdateCameraBox},    // 373: Blaster
    {315, 0x340611, 0x12, 0x0, N_Anim, 0, N_Phys, N_Coll, Fighter_UpdateCameraBox},    // 374: Blaster in the air
    {304, 0x340212, 0x13, 0x0, SStart_Anim, 0, SStart_Phys, SStart_Coll, Fighter_UpdateCameraBox},    // 375: Wolf Flash: he gathers himself
    {313, 0x340612, 0x13, 0x0, SStart_Anim, 0, SStart_Phys, SStart_Coll, Fighter_UpdateCameraBox},    // 376: Wolf Flash: he gathers himself, in the air
    {303, 0x340612, 0x13, 0x0, Dash_Anim, 0, Dash_Phys, Dash_Coll, Fighter_UpdateCameraBox},    // 377: Wolf Flash: the dash
    {314, 0x340612, 0x13, 0x0, SEnd_Anim, 0, SEnd_Phys, SEnd_Coll, Fighter_UpdateCameraBox},    // 378: Wolf Flash: he arrives
    {307, 0x340213, 0x14, 0x0, HiHold_Anim, 0, HiHold_Phys, HiHold_Coll, Fighter_UpdateCameraBox},    // 379: Fire Wolf: he gathers himself
    {316, 0x340613, 0x14, 0x0, HiHold_Anim, 0, HiHold_Phys, HiHold_Coll, Fighter_UpdateCameraBox},    // 380: Fire Wolf: he gathers himself, in the air
    {321, 0x340613, 0x14, 0x0, Rush_Anim, 0, Rush_Phys, Rush_Coll, Fighter_UpdateCameraBox},    // 381: Fire Wolf: the flight
    {308, 0x340613, 0x14, 0x0, HiFall_Anim, 0, HiFall_Phys, HiFall_Coll, Fighter_UpdateCameraBox},    // 382: Fire Wolf: the kick as it ends in the air
    {309, 0x340213, 0x14, 0x0, HiLanding_Anim, 0, HiLanding_Phys, HiLanding_Coll, Fighter_UpdateCameraBox},    // 383: Fire Wolf: the kick as it ends on the ground
    {323, 0x340214, 0x15, 0x0, LwStart_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 384: Reflector: it opens
    {325, 0x340614, 0x15, 0x0, LwStart_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 385: Reflector: it opens, in the air
    {311, 0x340214, 0x15, 0x0, LwLoop_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 386: Reflector: held up
    {320, 0x340614, 0x15, 0x0, LwLoop_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 387: Reflector: held up, in the air
    {324, 0x340214, 0x15, 0x0, LwHit_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 388: Reflector: a shot sent back
    {326, 0x340614, 0x15, 0x0, LwHit_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 389: Reflector: a shot sent back, in the air
    {310, 0x340214, 0x15, 0x0, LwEnd_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 390: Reflector: it closes
    {319, 0x340614, 0x15, 0x0, LwEnd_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 391: Reflector: it closes, in the air
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
