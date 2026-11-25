#include "../mex.h"

// ---- his own states: each special at its plainest -----------------------------------------------
#define B_BUTTON 0x200                  // the special button, in fd->input.held (down now) and fd->input.down (pressed this frame)
#define ST_FIRST_OWN 373            // the first state of his own (the donor's own come before it in the table)
#define NEVER 30000                   // a frame no clip reaches
static u16 t_frame[6];                  // by player: the frame of the state of his own he is in (1 on the frame it is entered)

// Into a state of his own. `ActionStateChange` puts him in it at the start of its clip; `Fighter_AdvanceScript`
// then runs the clip's and the script's first frame at once, as the game's own code does for a special. Without
// it the script starts a frame late. (The frame a state is entered on already runs the state's movement and its
// collision: it is the state's first.)
static void Enter(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
    t_frame[fd->ply] = 1;
}
// The same move's other form (ground for air, air for ground), on the frame its clip is at: 0x5000 keeps the
// body's colour and has the script go on from that frame, not from its start. (Hitboxes that were out are taken
// down by it, and nothing before that frame is played again. So hand over on a frame BEFORE the one the script
// makes its boxes on, never on it: those are taken down at once and never come out.)
static void Hand_Over(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(fd->state.frame, 1, 0, gobj, state, 0x5000, 0);
}
// The frame of the move this is. Called once, first thing, by a state's animation call: 2 the first time (the
// frame the state was entered on was its first), so it is the frame the script has just run.
static int Tick(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] < NEVER)
        t_frame[fd->ply]++;
    return t_frame[fd->ply];
}
// Out of a special: standing on the ground; in the air an ordinary fall, or the helpless fall with the frames of
// landing it ends in.
static void Leave(GOBJ *gobj, int helpless, float landing)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else if (helpless)
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, landing);
    else
        Fighter_EnterFall(gobj);
}
// The same move's other form: the air state for a ground one, the ground state for an air one. Written at the end
// of this file by `build`, from its list `forms` (a state you add that hands over to another form goes in it too).
static int Other_Form(int state);
// PHYSICS, each frame after the animation call. What a state does not do here is not done: on the ground this
// slows him by his own friction and hands on the speed he has (a clip that travels along the ground is given to
// him as speed by the route's own per-frame code where it can be: report.json `travel`); in the air the engine
// moves him by `self_vel`, and this gives him gravity (with fast fall) and drift, as an attack in the air has:
// `Fighter_PhysAir_ApplyGravityFastfall` does all three (the game's ft_80084DB0 calls the drift itself, so a
// second call of `Fighter_PhysAir_ApplyAerialDrift` after it adds nothing).
// KIND: a dash, a rise, a hang in the air: a physics call of the state's own (SPECIALS.md "A dash that hits",
// "A recovery that rises").
void Plain_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
// COLLISION, each frame after he has moved: what the stage may do to him. Here the move goes on where it was:
// off an edge he is in the air form, come down he is in the ground form, on the same frame of the clip.
// KIND: stop at an edge, land with lag, catch a ledge: another call (SPECIALS.md "What the collision call decides").
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

#define A_BUTTON 0x100
#define OWN_ARMOUR 4322.0f              // a hit's knockback under this does not stagger him (and how this code knows its own)
#define PLAY_STATE_SOUND ((void (*)(FighterData *, int, int, int))0x80088510)   // ft_80088510: ended as his state changes

// ---- Flex: its hit round him, no armour, a stop in the air -----------------------------------------
#define ST_N 373
#define ST_N_AIR 374
#define N_END 30000                   // the frame he may act from, on the ground,
#define N_END_AIR 30000           //   and in the air
#define N_AIR_RESET_TO 5 // exclusive; source spn moveFrames 1..3 reset speed before gravity
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
void N_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
    {
        // The source entry update finishes the previous movement; its next three updates reset speed.
        if (t_frame[fd->ply] > 1 && t_frame[fd->ply] < N_AIR_RESET_TO)
        {
            fd->phys.self_vel.X = 0;
            fd->phys.self_vel.Y = 0;
        }
        // allowMovement 0: no aerial steering or fastfall during Flex.
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
        Fighter_PhysAir_DecayXVelocity(fd, fd->attr.aerial_friction);
    }
}


// ---- Bunjin Bull: he gathers himself, runs until a press, and strikes ------------------------------------------
#define ST_S 375
#define ST_S_AIR 376
#define ST_S_RUN 377
#define ST_S_ATK 378
#define ST_S_ATK_AIR 379
#define ST_S_HOP 385
#define S_RUN_FROM 21         // script: the frame the run begins (spf: spfrun)
#define S_DASH 1.1421f                 // the run's speed as it begins, a frame (x_vel: his floordashspeed)
#define S_RUN_MOST 2.2842f         // on the ground it goes toward this (floorrunspeed: twice his dash speed),
#define S_RUN_STEP 0.38071f         //   by this a frame (flooracc)
#define S_AIR_MOST 1.3705f         // in the air toward this (airspeed: 1.2 times his dash speed),
#define S_AIR_STEP 0.17766f         //   by this a frame (airacc)
#define S_STEER 0.8f               // source default TiltMag: opposite horizontal Far segment
#define S_HELPLESS 10         // script: from this frame of the strike he is helpless after it
#define S_ATK_END 43           // script: the frame the strike ends
#define S_FLOOR_BRAKE 0.08248638646519277f   // source floorbrake * floorfriction, in Melee units/update
#define S_AIR_BRAKE 0.006345106651168674f       // source airdecay, in Melee units/update
#define S_HOP_SPEED 1.5228256104320117f       // source jumpheight 4 before the delayed aerial Bell Ringer
#define S_LANDING 29.0f           // frames of landing after the helpless fall
void SpecialHi(GOBJ *gobj);
void SpecialAirHi(GOBJ *gobj);
static float S_Toward(float value, float target, float step)
{
    if (value < target) return value + step < target ? value + step : target;
    return value - step > target ? value - step : target;
}
static float s_speed[6];                // by player: signed world velocity, a frame
static float s_drive[6];                // facing before this update's input changes it
void SpecialS(GOBJ *gobj)
{
    Enter(gobj, ST_S);
}
void SpecialAirS(GOBJ *gobj)
{
    Enter(gobj, ST_S_AIR);
}
// allowMovement 0: no drift; friction on the ground, gravity in the air
void S_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        fd->phys.self_vel_ground.X = S_Toward(fd->phys.self_vel_ground.X, 0, S_FLOOR_BRAKE);
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
    else
    {
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
        fd->phys.self_vel.X = S_Toward(fd->phys.self_vel.X, 0, S_AIR_BRAKE);
    }
}
void S_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= S_RUN_FROM || FrameTimerCheck(gobj) == 0)
    {
        Enter(gobj, ST_S_RUN);
        s_drive[fd->ply] = fd->facing_direction;
        s_speed[fd->ply] = S_DASH * s_drive[fd->ply];
    }
}
void SRun_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    s_drive[fd->ply] = fd->facing_direction;
    Tick(gobj);
    if (FrameTimerCheck(gobj) == 0)
        ActionStateChange(0, 1, 0, gobj, ST_S_RUN, 0, 0);
}
void SRun_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float x = fd->input.lstick.X, y = fd->input.lstick.Y, ax = x < 0 ? -x : x;
    // Source acceleration uses the prior facing; turning changes facing now, momentum on later updates.
    if (ax >= (y < 0 ? -y : y) &&
        ((x >= S_STEER && fd->facing_direction < 0) || (x <= -S_STEER && fd->facing_direction > 0)))
        fd->facing_direction = -fd->facing_direction;
    // Input is refreshed after Anim. Both the source's ordinary jump and its follow-ups use this update.
    // A B+up follow-up is not a tap jump; explicit X/Y still has the source's jump priority.
    if ((!(fd->input.down & B_BUTTON) || (fd->input.down & 0xC00)) &&
        (fd->phys.air_state == 0 ? Fighter_IASACheck_JumpF(gobj) : Fighter_IASACheck_JumpAerial(gobj)))
        return;
    if (fd->input.down & B_BUTTON)
    {
        // StandardInputTrigger: outside AlmostNeutral (.15), North/NE/NW (within 67.5 degrees of up).
        if (fd->phys.air_state == 0 && (ax >= 0.15f || y >= 0.15f) && y > ax * 0.414213562f)
        {
            SpecialHi(gobj);
            return;
        }
        // spfrun's air branch uses the narrower UpNear|UpFar segment and requires an air jump.
        if (fd->phys.air_state != 0 && y > ax && fd->jump.jumps_used < fd->attr.max_jumps)
        {
            Enter(gobj, ST_S_HOP);
            fd->jump.jumps_used = fd->attr.max_jumps;
            fd->phys.self_vel.Y = S_HOP_SPEED;
            return;
        }
    }
    if (fd->input.down & (A_BUTTON | B_BUTTON))
    {
        Enter(gobj, fd->phys.air_state == 0 ? ST_S_ATK : ST_S_ATK_AIR);
        return;
    }
}
void SHop_Anim(GOBJ *gobj)
{
    SpecialAirHi(gobj);                // source: one update of doublejump, then spu
}
void SRun_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, grounded = fd->phys.air_state == 0;
    float most = grounded ? S_RUN_MOST : S_AIR_MOST, step = grounded ? S_RUN_STEP : S_AIR_STEP;
    // spfrun entry sets dash speed; its repeating acceleration begins on the following update.
    if (t_frame[p] > 1 || fd->state_id == ST_S_HOP)
        s_speed[p] = S_Toward(s_speed[p], most * s_drive[p], step);
    s_speed[p] = S_Toward(s_speed[p], 0, grounded ? S_FLOOR_BRAKE : S_AIR_BRAKE);
    if (grounded)
    {
        fd->phys.self_vel_ground.X = s_speed[p];
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
    else
    {
        fd->phys.self_vel.X = s_speed[p];
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
    }
}
// off an edge he runs on in the air (leaveedges 1), and on along a floor he comes down on
void SRun_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
            Fighter_SetAirborne(fd);
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
        Fighter_SetGrounded2(fd);
}
void SAtk_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= S_ATK_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, frame >= S_HELPLESS, S_LANDING);
}

// ---- Bell Ringer: still, then the clip's root carries him up (or up and ahead), a hop, helpless ------------------
#define ST_HI 380
#define ST_HI_AIR 381
#define ST_HI_SIDE 382
#define HI_GO 9                   // script: the frame the root takes him (SAPRA) and the side rise is chosen
#define HI_SIDE_STICK 0.3f   // script: the stick across past this on that frame: the side rise
#define HI_HOP_AT 31           // script: his gravity back, a hop, ledges caught (grabedge 1)
#define HI_HOP 1.0768f                 //   the hop's speed up, a frame (jumpheight 2 under his gravity)
#define HI_END 41                 // script: the frame his helpless fall begins (specialfall)
#define HI_LANDING 29.0f
// clip: his speed each frame of the rise, ahead and up: the clip's root frame by frame as the disc plays it, times
// the drawn scale, from the frame the root takes him (the straight rise's, and the side rise's)
static const float HI_V[][2] = {{0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {-0.0000f, 0.0380f}, {-0.0000f, 0.1173f}, {-0.0000f, 0.2020f}, {-0.0000f, 0.2925f}, {-0.0000f, 0.3898f}, {-0.0000f, 0.4947f}, {-0.0000f, 0.6084f}, {-0.0000f, 0.7321f}, {-0.0000f, 0.8675f}, {-0.0000f, 1.0167f}, {-0.0000f, 1.1822f}, {-0.0000f, 1.3677f}, {-0.0000f, 1.5773f}, {-0.0000f, 1.8171f}, {-0.0000f, 2.0955f}, {-0.0000f, 2.4243f}, {-0.0000f, 2.8214f}, {-0.0000f, 3.3142f}, {-0.0000f, 3.9471f}, {-0.0000f, 4.7934f}, {-0.0000f, 5.9142f}};
static const float HI_SIDE_V[][2] = {{0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.0000f, 0.0000f}, {0.2169f, 0.0380f}, {0.6532f, 0.1173f}, {1.0761f, 0.2020f}, {1.4582f, 0.2925f}, {1.7695f, 0.3898f}, {1.9843f, 0.4947f}, {2.0892f, 0.6083f}, {2.0848f, 0.7321f}, {1.9859f, 0.8675f}, {1.8146f, 1.0167f}, {1.5948f, 1.1823f}, {1.3474f, 1.3677f}, {1.0884f, 1.5773f}, {0.8292f, 1.8169f}, {0.5772f, 2.0955f}, {0.3363f, 2.4243f}, {0.1089f, 2.8214f}, {0.0000f, 3.3142f}, {0.0000f, 3.9476f}, {-0.0000f, 4.7935f}, {0.0000f, 5.9135f}};
#define HI_FRAMES (int)(sizeof(HI_V) / sizeof(HI_V[0]))
#define HI_SIDE_FRAMES (int)(sizeof(HI_SIDE_V) / sizeof(HI_SIDE_V[0]))
static u8 hi_side[6];
static void Hi_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, state);
    hi_side[fd->ply] = 0;
    fd->phys.self_vel.X = 0;                            // script: x_vel 0, y_vel 0, gravity 0
    fd->phys.self_vel.Y = 0;
    fd->phys.self_vel_ground.X = 0;
}
void SpecialHi(GOBJ *gobj)
{
    Hi_Begin(gobj, ST_HI);
}
void SpecialAirHi(GOBJ *gobj)
{
    Hi_Begin(gobj, ST_HI_AIR);
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, frame = Tick(gobj);
    float x = fd->input.lstick.X;
    // (handed over the frame BEFORE he goes: a hand-over takes down the boxes out and goes on after the frame it is
    // made on, so made on HI_GO, the frame the script makes his boxes, he struck nothing: the round of 2026-11-13)
    // (off the ground on HI_GO itself, as before: his collision lands him on a frame before it)
    if (frame == HI_GO && fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    if (frame == HI_GO - 1)
    {
        if (x >= HI_SIDE_STICK || x <= -HI_SIDE_STICK)
        {
            if ((x > 0) != (fd->facing_direction > 0))
                fd->facing_direction = -fd->facing_direction;
            hi_side[p] = 1;
            Hand_Over(gobj, ST_HI_SIDE);
        }
        else if (fd->state_id == ST_HI)
            Hand_Over(gobj, ST_HI_AIR);
    }
    if (frame >= HI_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, HI_LANDING);
}
void Hi_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, f = t_frame[p];
    const float (*v)[2] = hi_side[p] ? HI_SIDE_V : HI_V;
    int n = hi_side[p] ? HI_SIDE_FRAMES : HI_FRAMES;
    if (f < HI_GO)
    {
        if (fd->phys.air_state == 0)
            Fighter_PhysGround_ApplyFriction(gobj);
        else
        {
            fd->phys.self_vel.X = 0;
            fd->phys.self_vel.Y = 0;
        }
    }
    else if (f < HI_HOP_AT)
    {
        fd->phys.self_vel.X = (f < n ? v[f][0] : 0) * fd->facing_direction;
        fd->phys.self_vel.Y = f < n ? v[f][1] : 0;
    }
    else
    {
        if (f == HI_HOP_AT)
            fd->phys.self_vel.Y = HI_HOP;
        Fighter_PhysAir_ApplyGravity(fd, fd->attr.gravity, fd->attr.terminal_velocity);
        Fighter_PhysAir_DecayXVelocity(fd, fd->attr.aerial_friction);
    }
}
static void Hi_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
void Hi_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int f = t_frame[fd->ply];
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else if (f >= HI_HOP_AT)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0 && (f < HI_GO || f > HI_GO + 2) && fd->phys.self_vel.Y <= 0)
    {
        Fighter_SetGrounded2(fd);
        Hi_Landed(gobj);
    }
}

// ---- Deep Breath: a punch held up to 30 seconds, and the shockwave it lets go ------------------------------------
#define ST_LW 383
#define ST_LW_AIR 384
#define LW_MOST 1800               // script: the most it is held, frames (spd_charge counts seconds: 30)
#define LW_ARMOUR_FROM 1195 // script: held this many frames, a hit does not stagger him (p_def_s_armor)
#define LW_WAVE_AT 5         // script: frames from letting go to the wave (fbjspdwave),
#define LW_DONE 65
#define LW_END_AIR 47
#define LW_LEVELS 8
#define LW_ITEMS 10
#define LW_AHEAD 10.507497808268575f
#define LW_UP 0.0f
static const float lw_after[LW_LEVELS] = {29.500000f, 20.000000f, 10.000000f, 5.000000f, 3.000000f, 2.000000f, 1.000000f, 0.000000f};
static const int lw_first_item[LW_LEVELS] = {0, 2, 4, 5, 6, 7, 8, 9};
static const int lw_parts[LW_LEVELS] = {2, 2, 1, 1, 1, 1, 1, 1};
#define LW_SOUNDS 7
static const int lw_sound_at[LW_SOUNDS] = {55, 115, 175, 295, 595, 1195, 1765};       // frames held
static const int lw_sound[LW_SOUNDS] = {40, 43, 46, 91, 121, 124, 127};             // places in his bank (0: not in it)
static u16 lw_held[6];
static float lw_charge[6];              // source spd_charge accumulates single-precision seconds
static u16 lw_since[6];
static u8 lw_level[6];
static void Wave_Part(GOBJ *gobj, int which, int group)
{
    FighterData *fd = gobj->userdata;
    SpawnItem spawn;
    GOBJ *item;
    spawn.parent_gobj = gobj;
    spawn.parent_gobj2 = gobj;
    spawn.it_kind = MEX_GetFtItemID(gobj, which);
    spawn.pos = fd->phys.pos;
    spawn.pos.X += LW_AHEAD * fd->facing_direction;
    spawn.pos.Y += LW_UP;
    spawn.pos.Z = 0;
    spawn.pos2 = spawn.pos;
    spawn.vel.X = spawn.vel.Y = spawn.vel.Z = 0;
    // Source wave tracks use world X, even when its owner faces left.
    spawn.facing_direction = 1.0f;
    spawn.damage = 0;
    // SpawnItem+0x40 -> ItemData+0xAC4: native shared hit-exception ID (itcoll.c).
    spawn.unk6 = group;
    spawn.is_raycast_below = 0;
    item = Item_CreateItem1(&spawn);
    if (item)
    {
        ItemData *id = item->userdata;
        ItemStateChange(item, 1, 2);
        id->item_var.var2 = -1;
        Item_SetAirborne(id);
        Item_CopyDevelopState(item, gobj);
    }
}
static void Wave_Make(GOBJ *gobj, int level)
{
    int i, group = Item_GenerateHitExceptionID();
    for (i = 0; i < lw_parts[level]; i++)
        Wave_Part(gobj, lw_first_item[level] + i, group);
}
void SpecialLw(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_LW);
    lw_held[fd->ply] = 1;
    lw_charge[fd->ply] = 0;
    lw_since[fd->ply] = 0;
    Fighter_SetAnimRate(gobj, 0.0f);                    // script: the clip stands still while it is held (rate 0)
}
void SpecialAirLw(GOBJ *gobj)
{
    Enter(gobj, ST_LW_AIR);
}
void Lw_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, n, i;
    if (lw_held[p])
    {
        n = lw_held[p]++;
        lw_charge[p] += 1.0f / 60.0f;
        for (i = 0; i < LW_SOUNDS; i++)
            if (n == lw_sound_at[i] && lw_sound[i])
                PLAY_STATE_SOUND(fd, 5000 + lw_sound[i], 127, 64);
        if (n >= LW_ARMOUR_FROM)
            fd->dmg.armor = OWN_ARMOUR;
        return;
    }
    Tick(gobj);
    n = lw_since[p]++;
    if (n == LW_WAVE_AT)
        Wave_Make(gobj, lw_level[p]);
    if (n >= LW_DONE || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, 0);
}
void Lw_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, level;
    // Melee refreshes fd->input after Anim. Read the release here, on the actual input update.
    if (lw_held[p] && (lw_charge[p] >= LW_MOST / 60.0f || !(fd->input.held & B_BUTTON)))
    {
        for (level = 0; level < LW_LEVELS - 1 && lw_charge[p] < lw_after[level]; level++)
            ;
        lw_level[p] = level;
        lw_held[p] = 0;
        lw_since[p] = 1;
        Fighter_SetAnimRate(gobj, 1.0f);
    }
}
void Lw_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
// (leaveedges 0: he stops at an edge)
void Lw_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
}
void LwAir_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= LW_END_AIR || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
static void LwAir_Landed(GOBJ *gobj)
{
    Fighter_EnterLanding(gobj);
}
void LwAir_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] > 1)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, LwAir_Landed);
}

// ---- once as he is made, and once a frame -----------------------------------------------------------------------
static void OwnLoad(FighterData *fd)
{
    int p = fd->ply, i;
    ItemDesc **items = fd->ftData->items;
    for (i = 0; items && i < LW_ITEMS; i++)
        if (items[i]) MEX_IndexFighterItem(fd->kind, items[i], i);
    t_frame[p] = 0;
    s_speed[p] = 0;
    s_drive[p] = 0;
    hi_side[p] = 0;
    lw_held[p] = 0;
    lw_charge[p] = 0;
    lw_since[p] = 0;
    lw_level[p] = 0;
}
// his armour is his own only while its move lasts (a grab or a throw takes him out of it with the armour on)
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    if (fd->dmg.armor != OWN_ARMOUR)
        return;
    if (s == ST_LW && lw_held[p] > LW_ARMOUR_FROM)
        return;
    if (s == ST_LW && lw_since[p] && lw_since[p] < LW_WAVE_AT)
        return;
    fd->dmg.armor = 0;
}

// ---- the other form of each state that has one (from `forms` in `build`) ------------------------------
static int Other_Form(int state)
{
    switch (state)
    {
    case ST_N: return ST_N_AIR;
    case ST_N_AIR: return ST_N;
    case ST_S: return ST_S_AIR;
    case ST_S_AIR: return ST_S;
    case ST_S_ATK: return ST_S_ATK_AIR;
    case ST_S_ATK_AIR: return ST_S_ATK;
    }
    return state;                       // (a state not listed: a mistake; it stays the state it is)
}

// Moves that cannot be knocked out of: armour while their clips are between the frames the game gives.
#define ARMOUR 4321.0f      // more than any hit's knockback; also how this code knows its own
static void ArmourFrame(FighterData *fd)
{
    int on = 0, a = fd->action_id;
    float at = fd->state.frame + 1.0f;      // this runs before the frame's clip moves on: the frame a hit will find
    if ((a == 66) && at >= 15.0f && at <= 24.0f)
        on = 1;
    if ((a == 62) && at >= 31.0f && at <= 39.0f)
        on = 1;
    if (on)
        fd->dmg.armor = ARMOUR;
    else if (fd->dmg.armor == ARMOUR)
        fd->dmg.armor = 0;
}

// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (!(s == 373 || s == 374 || s == 375 || s == 376 || s == 380 || s == 381 || s == 383 || s == 384) || fd->state.frame > 4.0f)
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
    ReverseFrame(fd);
    TravelFrame(fd);
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
    {306, 0x340211, 0x12, 0x0, N_Anim, 0, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 373: Flex
    {315, 0x340611, 0x12, 0x0, N_Anim, 0, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 374: Flex in the air
    {303, 0x340212, 0x13, 0x0, S_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: Bunjin Bull: he gathers himself
    {312, 0x340612, 0x13, 0x0, S_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: Bunjin Bull: he gathers himself, in the air
    {304, 0x340212, 0x13, 0x0, SRun_Anim, SRun_IASA, SRun_Phys, SRun_Coll, Fighter_UpdateCameraBox},    // 377: Bunjin Bull: the run
    {307, 0x340212, 0x13, 0x0, SAtk_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 378: Bunjin Bull: the strike
    {316, 0x340612, 0x13, 0x0, SAtk_Anim, 0, S_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 379: Bunjin Bull: the strike in the air
    {321, 0x340213, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 380: Bell Ringer
    {322, 0x340613, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 381: Bell Ringer in the air
    {313, 0x340613, 0x14, 0x0, Hi_Anim, 0, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 382: Bell Ringer: the side rise
    {323, 0x340214, 0x15, 0x0, Lw_Anim, Lw_IASA, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 383: Deep Breath
    {325, 0x340614, 0x15, 0x0, LwAir_Anim, 0, Plain_Phys, LwAir_Coll, Fighter_UpdateCameraBox},    // 384: Deep Breath in the air: the punch
    {18, 0x340613, 0x14, 0x0, SHop_Anim, 0, SRun_Phys, SRun_Coll, Fighter_UpdateCameraBox},    // 385: Bunjin Bull: jump into Bell Ringer
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
