#include "../mex.h"

// ---- Greninja's own states ----------------------------------------------------------------------
#define B_BUTTON 0x200                  // the special button, in fd->input.held (down now) and fd->input.down (pressed this frame)
#define SHIELD_BUTTONS 0x60             // L and R (MexTK's PAD_TRIGGER_L 0x40, PAD_TRIGGER_R 0x20)
#define ST_FIRST_OWN 373
#define ST_LAST_OWN 404
#define XROT_JOINT 2               // the joint the engine pitches a fighter about
// what a script's effect command calls: an effect on one of his joints (his own are 6000 and on)
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
typedef struct { u8 delay; u16 gfx; } OwnFx;    // one generator of an effect of his own: frames after it starts, its number
static u8 g_age[6];                 // frames of the state of his own he is in (1 on the frame it begins)
static u8 g_helpless[6];            // he has used a special that leaves him helpless, and has not stood since
static u8 g_pitched[6];             // his body is pitched
static u8 g_hidden[6];              // a state of his has hidden him or made him unhurt

static void Enter(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
    g_age[fd->ply] = 1;
}
static void Hand_Over(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(fd->state.frame, 1, 0, gobj, state, 0x5000, 0);
}
// the clip of the state he is in, from its start again (a hold played for as long as it is held)
static void Again(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, fd->state_id, 0x5000, 0);
}
static int Age(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (g_age[fd->ply] < 250)
        g_age[fd->ply]++;
    return g_age[fd->ply];
}
static void Turn_Round(FighterData *fd)
{
    fd->facing_direction = -fd->facing_direction;
    Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
}
static void Face(FighterData *fd, float way)
{
    if (way * fd->facing_direction < 0)
        Turn_Round(fd);
}
static void Pitch(FighterData *fd, float by)
{
    Fighter_SetBoneRotX(fd, XROT_JOINT, by);
    g_pitched[fd->ply] = by != 0;
}
static void Hide(GOBJ *gobj, int hidden)
{
    FighterData *fd = gobj->userdata;
    fd->flags.invisible = hidden;
    Fighter_SetScriptHurtStatus(gobj, hidden ? 2 : 0);
    g_hidden[fd->ply] = hidden;
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
static void Fx_At(GOBJ *gobj, const OwnFx *fx, int count, int age, Vec3 *at)
{
    FighterData *fd = gobj->userdata;
    Vec3 off, none = {0, 0, 0};
    int i;
    off.X = 0;
    off.Y = at->Y - fd->phys.pos.Y;
    off.Z = (at->X - fd->phys.pos.X) * fd->facing_direction;
    for (i = 0; i < count; i++)
        if (fx[i].gfx && fx[i].delay == age)
            SPAWN_EFFECT(gobj, fx[i].gfx, 0, 0, 0, &off, &none, 0.0f);
}
// out of a special: standing, or (in the air) falling; helpless after the ones that leave him so
static void Leave(GOBJ *gobj, float landing)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else if (g_helpless[fd->ply])
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, landing);
    else
        Fighter_EnterFall(gobj);
}
// The same move's other form (ground for air, air for ground): the list `forms` of `build`, at the end of the file.
static int Other_Form(int state);
// on the ground he slows by his own friction; in the air he falls and drifts as in any attack in the air
void Plain_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
// the move goes on where it was as he walks off an edge or comes down
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

// ---- Water Shuriken: he gathers it, charges it while B is held, and throws it; fully charged it goes by itself
#define ST_N_START 373
#define ST_N_START_AIR 374
#define ST_N_HOLD 375
#define ST_N_HOLD_AIR 376
#define ST_N_SHOT 377
#define ST_N_SHOT_AIR 378
#define ST_N_MAX 379
#define ST_N_MAX_AIR 380
#define SHURIKEN 0                      // its item number
#define N_CHARGE 40             // vl.prc param_special_n charge_frame_max: frames of charge to the full one
#define N_LEVELS 4             // STAND-IN: the levels of charge below the full one (one state of the shuriken each)
#define N_FIRE 6                 // script: the frame the throw lets it go on (game_specialnshot's flag),
#define N_MAX_FIRE 6         //   and the full one's (game_specialnmaxshot's flag)
#define N_SHOT_END 32         // script: the frame he may act from after a throw, on the ground,
#define N_SHOT_END_AIR 25 //   and in the air;
#define N_MAX_END 28           //   after the full one, on the ground,
#define N_MAX_END_AIR 25   //   and in the air
#define N_AHEAD 10.668f               // READING: where it is let go: ahead of him,
#define N_HIGH 8.728f                 //   and above his feet
static u8 n_charge[6];              // frames he has charged it

static void Throw(GOBJ *gobj, int full)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    GOBJ *shot;
    int level = n_charge[fd->ply] * N_LEVELS / N_CHARGE;
    if (level >= N_LEVELS)
        level = N_LEVELS - 1;
    at.X = fd->phys.pos.X + N_AHEAD * fd->facing_direction;
    at.Y = fd->phys.pos.Y + N_HIGH;
    at.Z = 0;
    shot = Make_Item(gobj, SHURIKEN, &at);
    // the ask: 1 to N_LEVELS for a level of charge, N_LEVELS + 1 for the full one (its own code sends it on its way)
    if (shot)
        ((ItemData *)shot->userdata)->item_var.var1 = full ? N_LEVELS + 1 : level + 1;
}
static void N_To(GOBJ *gobj, int ground, int air)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, fd->phys.air_state == 0 ? ground : air);
}
void SpecialN(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    n_charge[fd->ply] = 0;
    Enter(gobj, ST_N_START);
}
void SpecialAirN(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    n_charge[fd->ply] = 0;
    Enter(gobj, ST_N_START_AIR);
}
// gathered: a hold while B is still down, the throw if it was let go
void NStart_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Age(gobj);
    if (FrameTimerCheck(gobj) == 0)
    {
        if (fd->input.held & B_BUTTON)
            N_To(gobj, ST_N_HOLD, ST_N_HOLD_AIR);
        else
            N_To(gobj, ST_N_SHOT, ST_N_SHOT_AIR);
    }
}
void NHold_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    Age(gobj);
    if (n_charge[p] < 250)
        n_charge[p]++;
    if (n_charge[p] >= N_CHARGE)
        N_To(gobj, ST_N_MAX, ST_N_MAX_AIR);
    else if (!(fd->input.held & B_BUTTON))
        N_To(gobj, ST_N_SHOT, ST_N_SHOT_AIR);
    else if (fd->input.down & SHIELD_BUTTONS)
        Leave(gobj, 0);                 // STAND-IN: a shield button lets the charge go, and nothing is thrown
    else if (FrameTimerCheck(gobj) == 0)
        Again(gobj);
}
void NShot_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (age == N_FIRE)
        Throw(gobj, 0);
    if (age >= (fd->phys.air_state == 0 ? N_SHOT_END : N_SHOT_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0);
}
void NMax_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (age == N_MAX_FIRE)
        Throw(gobj, 1);
    if (age >= (fd->phys.air_state == 0 ? N_MAX_END : N_MAX_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0);
}

// ---- Shadow Sneak: a shadow runs ahead of him while B is held; let go, he vanishes, comes out at the shadow and kicks
#define ST_S_HOLD 381
#define ST_S_HOLD_AIR 382
#define ST_S_GONE 383
#define ST_S_GONE_AIR 384
#define ST_S_F 385
#define ST_S_F_AIR 386
#define ST_S_B 387
#define ST_S_B_AIR 388
#define ST_S_END_F 389
#define ST_S_END_F_AIR 390
#define ST_S_END_B 391
#define ST_S_END_B_AIR 392
#define S_LEAST 5               // vl.prc hold_start_frame: frames it is held at the least
#define S_MOST 90                 // vl.prc life: frames it may be held (let go by itself then)
#define S_START 1.93961501121521f               // vl.prc x_offset (times the drawn scale): the shadow's distance as it starts
#define S_SPEED 1.7999999523162842f               // vl.prc warp_spd: what the shadow runs a frame
#define S_FAR 67.88652539253235f                   // vl.prc warp_distance_max (times the drawn scale): the farthest it goes
#define S_WALK 0.75f                 // vl.prc hold_x_speed: his speed on the ground, the stick held all the way
#define S_AIR_X 0.5f               // vl.prc hold_air_x_speed: and in the air
#define S_AIR_RISE 1.7999999523162842f         // vl.prc init_air_speed_y: his speed upward as it starts in the air
#define S_AIR_FALL 0.20000000298023224f         // vl.prc init_air_gravity: READING: what he gains downward a frame as he readies it in the air
#define S_MOVE 10                 // vl.prc warp_frame: frames he takes to cross to the shadow, unseen
#define S_GONE 17          // READING: frames he is unseen (published: the kick is out 24 frames after B is let go)
#define S_BACK 0.3f                 // READING: how far back the stick is held for the kick behind
#define S_END_F 19         // script: the frame he may act from after the kick ahead, on the ground,
#define S_END_F_AIR 19 //   and in the air;
#define S_END_B 19         //   after the kick behind, on the ground,
#define S_END_B_AIR 19 //   and in the air
#define S_LANDING 29.0f           // frames of landing after the helpless fall
#define S_SHADOW_EVERY 6        // READING: the shadow is drawn again every this many frames
static const OwnFx s_shadow_fx[] = {{0, 6114}};     // gekkouga_kageuchi_shadow, at a point
static float s_far[6];              // the shadow's distance ahead of him
static float s_step[6];             // what he crosses a frame, unseen

static void S_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    s_far[fd->ply] = S_START;
}
void SpecialS(GOBJ *gobj)
{
    S_Begin(gobj, ST_S_HOLD);
}
void SpecialAirS(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    S_Begin(gobj, ST_S_HOLD_AIR);
    fd->phys.self_vel.Y = S_AIR_RISE;
}
static void Shadow_Draw(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    at.X = fd->phys.pos.X + s_far[fd->ply] * fd->facing_direction;
    at.Y = fd->phys.pos.Y;
    at.Z = 0;
    Fx_At(gobj, s_shadow_fx, sizeof(s_shadow_fx) / sizeof(s_shadow_fx[0]), 0, &at);
}
static void Gone_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    Enter(gobj, fd->phys.air_state == 0 ? ST_S_GONE : ST_S_GONE_AIR);
    s_step[p] = s_far[p] / S_MOVE;
    Hide(gobj, 1);
    if (fd->phys.air_state != 0)
        g_helpless[p] = 1;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void SHold_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, age = Age(gobj);
    s_far[p] += S_SPEED;
    if (s_far[p] > S_FAR)
        s_far[p] = S_FAR;
    if (age % S_SHADOW_EVERY == 1)
        Shadow_Draw(gobj);
    if (age >= S_MOST || (age >= S_LEAST && !(fd->input.held & B_BUTTON)))
        Gone_Begin(gobj);
}
void SHold_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = fd->input.lstick.X * S_WALK;
        Fighter_PhysGround_ApplyVelocity(gobj);
        return;
    }
    fd->phys.self_vel.X = fd->input.lstick.X * S_AIR_X;
    fd->phys.self_vel.Y -= S_AIR_FALL;
    if (fd->phys.self_vel.Y < -fd->attr.terminal_velocity)
        fd->phys.self_vel.Y = -fd->attr.terminal_velocity;
}
static void S_Kick(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int air = fd->phys.air_state != 0;
    int back = fd->input.lstick.X * fd->facing_direction <= -S_BACK;
    Hide(gobj, 0);
    if (back)
        Enter(gobj, air ? ST_S_B_AIR : ST_S_B);
    else
        Enter(gobj, air ? ST_S_F_AIR : ST_S_F);
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void SGone_Anim(GOBJ *gobj)
{
    if (Age(gobj) > S_GONE)
        S_Kick(gobj);
}
// unseen, he crosses to the shadow: along the ground (an edge stops him) or straight through the air
void SGone_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float v = g_age[p] <= S_MOVE ? s_step[p] * fd->facing_direction : 0;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = v;
        Fighter_PhysGround_ApplyVelocity(gobj);
        return;
    }
    fd->phys.self_vel.X = v;
    fd->phys.self_vel.Y = 0;
}
void SGone_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge(gobj);
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj, ST_S_GONE);
    }
}
void SKick_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int s = fd->state_id;
    Age(gobj);
    if (FrameTimerCheck(gobj) == 0)
    {
        if (s == ST_S_F || s == ST_S_F_AIR)
            Enter(gobj, fd->phys.air_state == 0 ? ST_S_END_F : ST_S_END_F_AIR);
        else
            Enter(gobj, fd->phys.air_state == 0 ? ST_S_END_B : ST_S_END_B_AIR);
    }
}
void SEnd_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int s = fd->state_id, age = Age(gobj), end;
    if (s == ST_S_END_F || s == ST_S_END_F_AIR)
        end = fd->phys.air_state == 0 ? S_END_F : S_END_F_AIR;
    else
        end = fd->phys.air_state == 0 ? S_END_B : S_END_B_AIR;
    if (age >= end || FrameTimerCheck(gobj) == 0)
        Leave(gobj, S_LANDING);
}

// ---- Hydro Pump: he gathers himself, then two jets of water drive him the way the stick is held
#define ST_HI_START 393
#define ST_HI_START_AIR 394
#define ST_HI_JET 395
#define ST_HI_END 396
#define WATER 1                         // its item number
#define HI_START 14      // clip: frames he gathers himself (specialhistart)
#define HI_JET 10          // vl.prc move_time: frames each jet drives him
#define HI_WAIT 16               // vl.prc move_wait_frame: frames from the first jet's start to the second's
#define HI_SPEED 3.700000047683716f             // vl.prc warp_spd_add: the jet's speed
#define HI_SECOND 0.8999999761581421f           // vl.prc warp_second: the second jet's speed, times
#define HI_STICK 0.4000000059604645f             // vl.prc warp_stick: how far the stick is held to aim it (let go: straight up)
#define HI_LEAST_UP 0.3f       // READING: from the ground a jet goes at least this far up (radians)
#define HI_CLIP_ANGLE 1.5707963267948966f   // READING: the way the jet clip's body points (radians up from ahead)
#define HI_COAST 0.8f             // READING: what he keeps of his speed a frame between the jets
#define HI_END_KEEP 0.5f       // READING: what he keeps of his speed as the jets end
#define HI_WATER_1 2         // script: the frames of a jet its water is let go on (the articles)
#define HI_WATER_2 7
#define HI_MIDDLE 7.758f           // READING: the water starts this far above his feet
#define HI_LANDING 32.0f         // vl.prc landing_frame: frames of landing after the helpless fall
static float hi_angle[6];           // the way the jet goes: radians up from ahead, the way he faces
static u8 hi_jets[6];               // jets begun in this use

void SpecialHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    hi_jets[fd->ply] = 0;
    Enter(gobj, ST_HI_START);
}
void SpecialAirHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    hi_jets[fd->ply] = 0;
    Enter(gobj, ST_HI_START_AIR);
}
static void Water_Out(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float a = hi_angle[fd->ply];
    Vec3 at;
    GOBJ *water;
    at.X = fd->phys.pos.X;
    at.Y = fd->phys.pos.Y + HI_MIDDLE;
    at.Z = 0;
    water = Make_Item(gobj, WATER, &at);
    if (water)
    {
        ItemData *wd = water->userdata;
        // it flies back along the jet: the way, times a thousand, in var5 and var6
        wd->item_var.var5 = (int)(-cos(a) * fd->facing_direction * 1000.0f);
        wd->item_var.var6 = (int)(-sin(a) * 1000.0f);
        wd->item_var.var1 = 1;
    }
}
static void Jet_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X, sy = fd->input.lstick.Y, a;
    float speed = hi_jets[p] ? HI_SPEED * HI_SECOND : HI_SPEED;
    if (sx * sx + sy * sy < HI_STICK * HI_STICK)
    {
        sx = 0;
        sy = 1;
    }
    Face(fd, sx);
    a = atan2(sy, sx * fd->facing_direction);
    if (fd->phys.air_state == 0)
    {
        if (a < HI_LEAST_UP)
            a = HI_LEAST_UP;
        if (a > 3.14159265f - HI_LEAST_UP)
            a = 3.14159265f - HI_LEAST_UP;
        Fighter_SetAirborne(fd);
    }
    Enter(gobj, ST_HI_JET);
    hi_angle[p] = a;
    fd->phys.self_vel.X = speed * cos(a) * fd->facing_direction;
    fd->phys.self_vel.Y = speed * sin(a);
    Pitch(fd, HI_CLIP_ANGLE - a);
    hi_jets[p]++;
    g_helpless[p] = 1;
}
void HiStart_Anim(GOBJ *gobj)
{
    if (Age(gobj) > HI_START)
        Jet_Begin(gobj);
}
void HiStart_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    // READING: he hangs where he is as he gathers himself
    fd->phys.self_vel.X *= HI_COAST;
    fd->phys.self_vel.Y = 0;
}
static void HiEnd_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    Enter(gobj, ST_HI_END);
    fd->phys.self_vel.X *= HI_END_KEEP;
    fd->phys.self_vel.Y *= HI_END_KEEP;
}
void Jet_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, age = Age(gobj);
    if (age == HI_WATER_1 || age == HI_WATER_2)
        Water_Out(gobj);
    if (hi_jets[p] < 2 && age > HI_WAIT)
        Jet_Begin(gobj);
    else if (hi_jets[p] >= 2 && age > HI_JET)
        HiEnd_Begin(gobj);
    else if (FrameTimerCheck(gobj) == 0)
        Again(gobj);
}
// the jet keeps the speed it was given; after it he slows along the way he went
void Jet_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (g_age[fd->ply] > HI_JET)
    {
        fd->phys.self_vel.X *= HI_COAST;
        fd->phys.self_vel.Y *= HI_COAST;
    }
}
static void Hi_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
// a ledge he faces is caught on the way (the engine's own rule for an up special); a floor is landed on
void Jet_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
}
void HiEnd_Anim(GOBJ *gobj)
{
    Age(gobj);
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj, HI_LANDING);
}
void HiEnd_Phys(GOBJ *gobj)
{
    Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Lw_Caught(GOBJ *gobj);

// ---- Substitute: a pose that catches a hit; caught, he leaves a puff of smoke, vanishes, and kicks the one who struck
#define ST_LW 397
#define ST_LW_AIR 398
#define ST_LW_HIT 399
#define ST_LW_HIT_AIR 400
#define ST_LW_ATK 401
#define ST_LW_ATK_AIR 402
#define ST_LW_END 403
#define ST_LW_END_AIR 404
#define LW_ON 8                   // script: it catches a hit from this frame (its flag up)
#define LW_OFF 35                 //   to the frame before this one (its flag down)
#define LW_DONE 70               // script: the frame he may act from when nothing struck
#define LW_JOINT 5             // READING: the joint the ball that catches rides (his hip)
#define LW_HEIGHT 0.0f           // the ball above that joint
#define LW_RADIUS 10.086f           // READING: its size (vl.prc radius_size, drawn, and a third more)
#define LW_FALL_MAX 0.6000000238418579f       // vl.prc fall_speed_max: in the air he falls no faster than this,
#define LW_FALL_ACCEL 0.029999999329447746f   // vl.prc fall_speed_accel: gaining this a frame
#define LW_START_X 0.5f         // vl.prc start_x_speed_mul: what he keeps of his speed sideways, started in the air
#define LW_HIDE 6               // script: the frame of the caught clip he vanishes on (its flag)
#define LW_UNSEEN 14           // READING: the frame of the caught state he comes out on (published: the strike's hits 19 frames after)
#define LW_REACH 15.517f             // READING: how far from the middle of the one who struck he comes out
#define LW_MIDDLE 7.758f           // READING: the middle of a fighter, above its feet
#define LW_STICK 0.3f             // READING: how far the stick is held to choose the way
#define LW_KICK_ANGLE 0.0f   // READING: the way the strike clip's kick goes (radians up from ahead)
#define LW_HITS_FROM 5     // script: the frame the strike's hits come out on
#define LW_HURT_BACK 22     // script: the frame of the strike he may be hurt from again
#define LW_END_GROUND 31   // READING: the frame of the landing clip he may act from (published: the whole strike is 63 frames)
#define LW_END_AIR 31   // script: the frame of the closing clip in the air he may act from
#define CATCH ((void (*)(GOBJ *, ShieldDesc *, void *))0x8007B1B8)    // ftColl_8007B1B8: the engine's counter ball (Marth's)
static const ShieldDesc lw_ball = {LW_JOINT, 0, {0, LW_HEIGHT, 0}, LW_RADIUS, 0, 0, 0};
// The strike each way (the game's own five, the script's order): for its two hits, damage, angle, growth, base,
// size, how far along the leg; the first two ways with an ahead are also the way back (he faces the way he kicks).
static const float lw_way[5][2][6] = {
    {{13.0, 90.0, 95.0, 50.0, 8.5, 10.0}, {13.0, 90.0, 95.0, 50.0, 7.0, 2.5}},
    {{12.0, 60.0, 95.0, 62.0, 7.5, 10.0}, {12.0, 60.0, 95.0, 62.0, 6.0, 2.5}},
    {{11.0, 361.0, 85.0, 62.0, 6.0, 10.0}, {11.0, 361.0, 85.0, 62.0, 5.0, 2.5}},
    {{12.0, 25.0, 72.0, 62.0, 7.5, 10.0}, {12.0, 25.0, 72.0, 62.0, 6.0, 2.5}},
    {{13.0, 270.0, 65.0, 25.0, 8.5, 10.0}, {13.0, 45.0, 95.0, 50.0, 8.5, 10.0}}
};
#define LW_WRITTEN 4         // the way the driver's script already has (the last of the game's)
static GOBJ *lw_by[6];              // who struck him
static s8 lw_way_of[6];             // which way he strikes
static u8 lw_set[6];                // the strike's hits have been made that way

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
    lw_by[fd->ply] = 0;
    if (fd->phys.air_state != 0)
    {
        fd->phys.self_vel.X *= LW_START_X;
        fd->phys.self_vel.Y = 0;
    }
}
void SpecialLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW);
}
void SpecialAirLw(GOBJ *gobj)
{
    Lw_Begin(gobj, ST_LW_AIR);
}
// a hit touched the ball (called by the engine as it takes the hit)
void Lw_Caught(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    lw_by[fd->ply] = fd->shield.dmg_source;
    fd->flags.shield_enable = 0;
    Enter(gobj, fd->phys.air_state == 0 ? ST_LW_HIT : ST_LW_HIT_AIR);
    fd->cb.EnterHitlag = 0;             // (Marth's code clears the two beside it)
    fd->cb.ExitHitlag = 0;
    Fighter_SetScriptHurtStatus(gobj, 2);
    g_hidden[fd->ply] = 1;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void Lw_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (age >= LW_ON && age < LW_OFF)
    {
        if (!fd->flags.shield_enable)
            Lw_Catch(gobj);
    }
    else
        fd->flags.shield_enable = 0;
    if (age >= LW_DONE || FrameTimerCheck(gobj) == 0)
    {
        fd->flags.shield_enable = 0;
        Leave(gobj, 0);
    }
}
void Lw_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
    {
        fd->phys.self_vel.Y -= LW_FALL_ACCEL;
        if (fd->phys.self_vel.Y < -LW_FALL_MAX)
            fd->phys.self_vel.Y = -LW_FALL_MAX;
        Fighter_PhysAir_DecayXVelocity(fd, fd->attr.aerial_friction);
    }
    fd->shield_bubble.is_checked = 0;   // (Marth's own each frame: ftColl_8007AEE0, the ball follows its joint)
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int up = fd->flags.shield_enable;
    Plain_Coll(gobj);
    if (up && !fd->flags.shield_enable && (fd->state_id == ST_LW || fd->state_id == ST_LW_AIR))
        Lw_Catch(gobj);                 // (a change of state takes the ball down: put back up)
}
static void Lw_Strike(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, way, grounded = 0;
    GOBJ *by = lw_by[p];
    Vec3 at = fd->phys.pos;
    float face = -fd->facing_direction, dx, dy, a, sx = fd->input.lstick.X, sy = fd->input.lstick.Y;
    float floor = -1e9f;
    // where the one who struck is: a fighter, the fighter whose thing it was, or the thing itself
    if (by && by->p_link == MATCHPLINK_ITEM)
    {
        ItemData *it = by->userdata;
        if (it->fighter_gobj)
            by = it->fighter_gobj;
        else
        {
            at = it->pos;
            face = it->facing_direction;
            by = 0;
        }
    }
    if (by && by->p_link == MATCHPLINK_FIGHTER)
    {
        FighterData *e = by->userdata;
        at = e->phys.pos;
        face = e->facing_direction;
        grounded = e->phys.air_state == 0;
        floor = e->phys.pos.Y;
    }
    at.Y += LW_MIDDLE;
    // the way he strikes: the stick's, to the nearest eighth; let go, the way the one who struck faces (he comes out
    // at their back)
    if (sx * sx + sy * sy < LW_STICK * LW_STICK)
    {
        dx = face;
        dy = 0;
    }
    else
    {
        a = atan2(sy, sx);
        a = (float)(int)(a / 0.78539816f + (a < 0 ? -0.5f : 0.5f)) * 0.78539816f;
        dx = cos(a);
        dy = sin(a);
        if (dx > -0.01f && dx < 0.01f)
            dx = 0;
    }
    if (dx != 0)
        Face(fd, dx);
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    fd->phys.pos.X = at.X - dx * LW_REACH;
    fd->phys.pos.Y = at.Y - dy * LW_REACH - LW_MIDDLE;
    if (grounded && fd->phys.pos.Y < floor)
        fd->phys.pos.Y = floor;          // (READING: not under the floor the one who struck stands on)
    // the collision told where he now is, as the game's own moves do (Warp Star, Barrel: mpColl_80043680, written
    // out: its three places and its flag 0x20); else it sweeps from where he stood to here and lands him partway (he
    // came out 8 short, on the near side)
    fd->coll_data.topN_Curr = fd->phys.pos;
    fd->coll_data.topN_CurrCorrect = fd->phys.pos;
    fd->coll_data.topN_Prev = fd->phys.pos;
    fd->coll_data.flags |= 0x20;
    Enter(gobj, ST_LW_ATK_AIR);
    Hide(gobj, 0);
    Fighter_SetScriptHurtStatus(gobj, 2);
    g_hidden[p] = 1;
    a = atan2(dy, dx * fd->facing_direction);
    way = a > 1.1780972f ? 0 : a > 0.3926991f ? 1 : a > -0.3926991f ? 2 : a > -1.1780972f ? 3 : 4;
    lw_way_of[p] = way;
    lw_set[p] = 0;
    Pitch(fd, LW_KICK_ANGLE - a);
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void LwHit_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (age == LW_HIDE)
        fd->flags.invisible = 1;
    if (age >= LW_UNSEEN)
        Lw_Strike(gobj);
}
void LwHit_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
    {
        fd->phys.self_vel.X = 0;
        fd->phys.self_vel.Y = 0;
    }
}
// the strike's hits, put the way he strikes on the frame they come out (the driver's script has the last way's)
static void Lw_Ways(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, i, w = lw_way_of[p];
    for (i = 0; i < 2; i++)
    {
        ftHit *h = &fd->hitbox[i];
        const float *to = lw_way[w][i], *was = lw_way[LW_WRITTEN][i];
        if (!h->active)
            continue;
        Fighter_UpdateHitboxDamage(h, (int)to[0], gobj);
        h->angle = (int)to[1];
        h->kb_growth = (int)to[2];
        h->kb = (int)to[3];
        h->size *= to[4] / was[4];
        h->offset.X *= to[5] / was[5];
    }
    lw_set[p] = 1;
}
void LwAtk_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (!lw_set[fd->ply] && age >= LW_HITS_FROM && fd->hitbox[0].active)
        Lw_Ways(fd, gobj);
    if (age == LW_HURT_BACK)
    {
        Fighter_SetScriptHurtStatus(gobj, 0);
        g_hidden[fd->ply] = 0;
    }
    if (FrameTimerCheck(gobj) == 0)
    {
        Pitch(fd, 0);
        Hide(gobj, 0);
        Enter(gobj, fd->phys.air_state == 0 ? ST_LW_END : ST_LW_END_AIR);
    }
}
void LwAtk_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y -= LW_FALL_ACCEL;
    if (fd->phys.self_vel.Y < -LW_FALL_MAX)
        fd->phys.self_vel.Y = -LW_FALL_MAX;
}
void LwEnd_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int age = Age(gobj);
    if (age >= (fd->phys.air_state == 0 ? LW_END_GROUND : LW_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0);
}

// He has gone up or sneaked from the air. Until he stands again he has no jump left, and an ordinary fall he is
// found in is made the helpless one. Anything else (a ledge, a hit taken, a grab) gives him back what a fighter
// has after those. A state of his that hid him, pitched him or made him unhurt, left another way, is undone.
static void Own_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    int mine = s >= ST_FIRST_OWN && s <= ST_LAST_OWN;
    if (g_pitched[p] && s != ST_HI_JET && s != ST_LW_ATK && s != ST_LW_ATK_AIR)
        Pitch(fd, 0);
    if (g_hidden[p] && !mine)
        Hide(gobj, 0);
    if (!g_helpless[p])
        return;
    if (fd->phys.air_state == 0)
        g_helpless[p] = 0;
    else if (mine || (s >= ASID_FALLSPECIAL && s <= ASID_FALLSPECIALB))
        fd->jump.jumps_used = fd->attr.max_jumps;
    else
        g_helpless[p] = 0;
}
static void OwnLoad(FighterData *fd)
{
    ItemDesc **items = fd->ftData->items;
    int i, p = fd->ply;
    g_age[p] = 0;
    g_helpless[p] = 0;
    g_pitched[p] = 0;
    g_hidden[p] = 0;
    n_charge[p] = 0;
    s_far[p] = 0;
    hi_jets[p] = 0;
    lw_by[p] = 0;
    lw_set[p] = 0;
    for (i = 0; items && i < 2; i++)
        if (items[i])
            MEX_IndexFighterItem(fd->kind, items[i], i);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Own_Frame(fd, gobj);
}

// ---- the other form of each state that has one (from `forms` in `build`) ------------------------------
static int Other_Form(int state)
{
    switch (state)
    {
    case ST_N_START: return ST_N_START_AIR;
    case ST_N_START_AIR: return ST_N_START;
    case ST_N_HOLD: return ST_N_HOLD_AIR;
    case ST_N_HOLD_AIR: return ST_N_HOLD;
    case ST_N_SHOT: return ST_N_SHOT_AIR;
    case ST_N_SHOT_AIR: return ST_N_SHOT;
    case ST_N_MAX: return ST_N_MAX_AIR;
    case ST_N_MAX_AIR: return ST_N_MAX;
    case ST_S_HOLD: return ST_S_HOLD_AIR;
    case ST_S_HOLD_AIR: return ST_S_HOLD;
    case ST_S_GONE: return ST_S_GONE_AIR;
    case ST_S_GONE_AIR: return ST_S_GONE;
    case ST_S_F: return ST_S_F_AIR;
    case ST_S_F_AIR: return ST_S_F;
    case ST_S_B: return ST_S_B_AIR;
    case ST_S_B_AIR: return ST_S_B;
    case ST_S_END_F: return ST_S_END_F_AIR;
    case ST_S_END_F_AIR: return ST_S_END_F;
    case ST_S_END_B: return ST_S_END_B_AIR;
    case ST_S_END_B_AIR: return ST_S_END_B;
    case ST_HI_START: return ST_HI_START_AIR;
    case ST_HI_START_AIR: return ST_HI_START;
    case ST_LW: return ST_LW_AIR;
    case ST_LW_AIR: return ST_LW;
    case ST_LW_HIT: return ST_LW_HIT_AIR;
    case ST_LW_HIT_AIR: return ST_LW_HIT;
    case ST_LW_ATK: return ST_LW_ATK_AIR;
    case ST_LW_ATK_AIR: return ST_LW_ATK;
    case ST_LW_END: return ST_LW_END_AIR;
    case ST_LW_END_AIR: return ST_LW_END;
    }
    return state;
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 3
static const s8 face_rest[FACE_GROUPS] = {0, 0, 0};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {40, 2, 4},
    {41, 2, 2},
    {42, 2, 4},
    {43, 2, 0},
    {273, 2, 4},
    {274, 2, 2},
    {275, 2, 4},
    {276, 2, 0},
    {387, 2, 4},
    {388, 2, 2},
    {389, 2, 4},
    {390, 2, 0},
    {552, 2, 4},
    {553, 2, 2},
    {554, 2, 4},
    {555, 2, 0},
    {32767, 0, 0},
};
#define FACE_CLOCK_END 600
static const u16 face_left[] = {0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 4};
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
    if (!(s == 373 || s == 374 || s == 381 || s == 382 || s == 393 || s == 394 || s == 397 || s == 398) || fd->state.frame > 4.0f)
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
    if (fd->phys.air_state != 0 || !(a == 304))
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
    {238, 20, 0, {229, 255, 255, 0}, {229, 255, 255, 229}},
    {238, 27, 12, {229, 255, 255, 229}, {229, 255, 255, 0}},
    {238, 39, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {303, 3, 0, {255, 255, 255, 0}, {255, 255, 255, 26}},
    {303, 5, 10, {255, 255, 255, 26}, {255, 255, 255, 178}},
    {303, 15, 0, {255, 255, 255, 178}, {0, 0, 0, 0}},
    {323, 7, 0, {255, 255, 255, 0}, {255, 255, 255, 191}},
    {323, 8, 0, {255, 255, 255, 191}, {178, 178, 178, 128}},
    {323, 10, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {323, 12, 0, {171, 0, 199, 79}, {0, 0, 0, 0}},
    {323, 14, 0, {178, 178, 178, 0}, {178, 178, 178, 128}},
    {323, 16, 0, {178, 178, 178, 128}, {171, 0, 199, 79}},
    {323, 17, 0, {171, 0, 199, 79}, {0, 0, 0, 0}},
    {324, 1, 0, {255, 255, 255, 0}, {255, 255, 255, 0}},
    {324, 19, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {326, 1, 0, {255, 255, 255, 0}, {255, 255, 255, 0}},
    {326, 19, 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
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
    {295, 0x340211, 0x12, 0x0, NStart_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 373: Water Shuriken: he gathers it
    {299, 0x340611, 0x12, 0x0, NStart_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 374: Water Shuriken: he gathers it, in the air
    {296, 0x340211, 0x12, 0x0, NHold_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: Water Shuriken: charged
    {300, 0x340611, 0x12, 0x0, NHold_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: Water Shuriken: charged, in the air
    {297, 0x340211, 0x12, 0x0, NShot_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 377: Water Shuriken: thrown
    {301, 0x340611, 0x12, 0x0, NShot_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 378: Water Shuriken: thrown, in the air
    {306, 0x340211, 0x12, 0x0, NMax_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 379: Water Shuriken: the full one thrown
    {315, 0x340611, 0x12, 0x0, NMax_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 380: Water Shuriken: the full one thrown, in the air
    {303, 0x340212, 0x13, 0x0, SHold_Anim, 0, SHold_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 381: Shadow Sneak: readied
    {303, 0x340612, 0x13, 0x0, SHold_Anim, 0, SHold_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 382: Shadow Sneak: readied, in the air
    {303, 0x340212, 0x13, 0x0, SGone_Anim, 0, SGone_Phys, SGone_Coll, Fighter_UpdateCameraBox},    // 383: Shadow Sneak: unseen
    {303, 0x340612, 0x13, 0x0, SGone_Anim, 0, SGone_Phys, SGone_Coll, Fighter_UpdateCameraBox},    // 384: Shadow Sneak: unseen, in the air
    {304, 0x340212, 0x13, 0x0, SKick_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 385: Shadow Sneak: the kick ahead
    {313, 0x340612, 0x13, 0x0, SKick_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 386: Shadow Sneak: the kick ahead, in the air
    {305, 0x340212, 0x13, 0x0, SKick_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 387: Shadow Sneak: the kick behind
    {314, 0x340612, 0x13, 0x0, SKick_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 388: Shadow Sneak: the kick behind, in the air
    {307, 0x340212, 0x13, 0x0, SEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 389: Shadow Sneak: after the kick ahead
    {316, 0x340612, 0x13, 0x0, SEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 390: Shadow Sneak: after the kick ahead, in the air
    {308, 0x340212, 0x13, 0x0, SEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 391: Shadow Sneak: after the kick behind
    {317, 0x340612, 0x13, 0x0, SEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 392: Shadow Sneak: after the kick behind, in the air
    {321, 0x340213, 0x14, 0x0, HiStart_Anim, 0, HiStart_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 393: Hydro Pump: he gathers himself
    {318, 0x340613, 0x14, 0x0, HiStart_Anim, 0, HiStart_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 394: Hydro Pump: he gathers himself, in the air
    {322, 0x340613, 0x14, 0x0, Jet_Anim, 0, Jet_Phys, Jet_Coll, Fighter_UpdateCameraBox},    // 395: Hydro Pump: a jet
    {319, 0x340613, 0x14, 0x0, HiEnd_Anim, 0, HiEnd_Phys, Jet_Coll, Fighter_UpdateCameraBox},    // 396: Hydro Pump: the jets spent
    {323, 0x340214, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 397: Substitute: the pose
    {325, 0x340614, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 398: Substitute: the pose, in the air
    {311, 0x340214, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 399: Substitute: struck, he goes
    {311, 0x340614, 0x15, 0x0, LwHit_Anim, 0, LwHit_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 400: Substitute: struck, he goes, in the air
    {324, 0x340214, 0x15, 0x0, LwAtk_Anim, 0, LwAtk_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 401: Substitute: the strike
    {326, 0x340614, 0x15, 0x0, LwAtk_Anim, 0, LwAtk_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 402: Substitute: the strike, in the air
    {310, 0x340214, 0x15, 0x0, LwEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 403: Substitute: after it
    {320, 0x340614, 0x15, 0x0, LwEnd_Anim, 0, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 404: Substitute: after it, in the air
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
