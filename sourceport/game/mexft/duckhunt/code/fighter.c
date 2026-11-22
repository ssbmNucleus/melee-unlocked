#include "../mex.h"

// ---- Duck Hunt's own states ------------------------------------------------------------------
#define B_BUTTON 0x200
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
#define PLAY_STATE_SOUND ((void (*)(FighterData *, int, int, int))0x80088510)   // ft_80088510: a sound the engine ends as his state changes
typedef struct { u8 delay; u16 gfx; } OwnFx;    // one generator of an effect of his own: frames after the effect starts, its number

static void Enter(GOBJ *gobj, int state)
{
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
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
// The generators of one of his own effects that are due `age` frames after it began, at a point of the stage.
// (His effects are played the way his scripts play them: from his own base joint, by how far the point is
// above his feet and ahead of him.)
static void Fx_At(GOBJ *gobj, const OwnFx *fx, int count, int age, Vec3 *at)
{
    FighterData *fd = gobj->userdata;
    Vec3 off, none = {0, 0, 0};
    int i;
    off.X = 0;
    off.Y = at->Y - fd->phys.pos.Y;
    off.Z = (at->X - fd->phys.pos.X) * fd->facing_direction;
    for (i = 0; i < count; i++)
        if (fx[i].delay == age)
            SPAWN_EFFECT(gobj, fx[i].gfx, 0, 0, 0, &off, &none, 0.0f);
}
// The item of his own, of one kind, that is out (in a state before the ones it ends in, which are its last), or 0.
static GOBJ *Own_Item(GOBJ *gobj, int which, int ended)
{
    int kind = MEX_GetFtItemID(gobj, which);
    GOBJ *g;
    for (g = (*stc_gobj_lookup)[MATCHPLINK_ITEM]; g; g = g->next)
    {
        ItemData *id = g->userdata;
        if (id->kind == kind && id->state < ended && (id->fighter_gobj == gobj || id->MEX.fighter_orig == gobj))
            return g;
    }
    return 0;
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
#define ST_FIRST_OWN 373

// ---- Duck Jump: the duck takes hold of the dog, flies up with a set number of wing beats, tires, and the two fall
#define ST_HI_START 373          // the duck takes hold (on the ground, in the air),
#define ST_HI_START_AIR 374
#define ST_HI_FLY 375              //   flies,
#define ST_HI_END 376              //   tires
// vl.prc, param_special_hi, by name; how they are put together is a READING of the order the game's state code
// reads them in.
#define HI_START_X 0.6000000238418579f            // start_speed_mul_x: what he keeps of his speed sideways as it starts
#define HI_AIR_START_Y 0.5f    // air_start_initial_speed_y: started in the air, the speed upward he is given
#define HI_START_FALL 0.07599999755620956f      // his own gravity times air_start_fall_accel_mul, while the duck takes hold
#define FLY_X0 0.5f                  // fly_initial_speed_x: the flight starts with this speed the way he faces
#define FLY_Y0 1.0f                  // fly_initial_speed_y
#define FLY_FALL 0.07199999690055847f             // normal_fall_accel times fly_fall_accel_mul: what the flight loses a frame
#define FLY_FALL_MAX 1.5f     // normal_fall_max_speed times fly_fall_max_speed_mul
#define FLAP_TIMES 7         // flap_times: the beats of the duck's wings in one flight
#define FLAP_INTERVAL 10   // flap_interval: frames from one beat to the next
#define FLAP_Y 0.8100000023841858f                 // flap_accel_y: what a beat adds to the speed upward
#define FLAP_X 0.3499999940395355f                 // flap_accel_x: and sideways, the way the stick is held
#define FLAP_STICK 0.1f                 // how far the stick must be held for that (the game's code: 0.1)
#define FLY_STICK 0.02500000037252903f           // stick_speed_mul: what the stick adds sideways each frame
#define FLY_DEC 0.009999999776482582f               // dec_speed_x: what the speed sideways loses each frame
#define FLY_MAX_X 1.0499999523162842f           // max_speed_x
#define HI_CANCEL 45           // script: the frame of the flight from which an attack or an air dodge ends it
#define HI_LANDING 30.0f         // landing_frame: frames of landing after it

static u8 flaps[6];                 // beats of the wings left in this flight
static u8 flap_wait[6];             // frames until the next
static u8 flap_now[6];              // a beat this frame
static u8 fly_age[6];               // frames of this flight
static u8 hi_spent[6];              // he has flown and not yet landed: whatever he does next in the air, he falls helpless after
static void (*air_anim[6])(GOBJ *); // the engine's own callback of the attack in the air that ended his flight

// out of a special: standing, or (in the air) falling; helpless once he has flown
static void Leave(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else if (hi_spent[fd->ply])
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, HI_LANDING);
    else
        Fighter_EnterFall(gobj);
}
void SpecialHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_HI_START);
    fd->phys.self_vel_ground.X *= HI_START_X;
}
void SpecialAirHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_HI_START_AIR);
    fd->phys.self_vel.X *= HI_START_X;
    fd->phys.self_vel.Y = HI_AIR_START_Y;
}
static void Fly_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    Enter(gobj, ST_HI_FLY);
    fd->phys.self_vel.X = FLY_X0 * fd->facing_direction;
    fd->phys.self_vel.Y = FLY_Y0;
    // (the frame it begins on is the flight's first: the engine runs the new state's movement on it already)
    flaps[p] = FLAP_TIMES;
    flap_wait[p] = FLAP_INTERVAL - 1;
    flap_now[p] = 0;
    fly_age[p] = 1;
    hi_spent[p] = 1;
}
void HiStart_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Fly_Begin(gobj);
}
void HiStart_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravity(fd, HI_START_FALL, fd->attr.terminal_velocity);
}
// (started on the ground he stays on it until the flight begins; started in the air, a floor he comes down on
// in those few frames is only stood on: the flight begins from it)
static void HiStart_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    ActionStateChange(fd->state.frame, 1, 0, gobj, ST_HI_START, 0x5000, 0);
}
void HiStart_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_IgnoreLedge(gobj, HiStart_Landed);
}
// The flight. A beat of the wings every FLAP_INTERVAL frames, FLAP_TIMES of them; an interval after the last, the
// duck tires. Each beat adds to the speed upward, and sideways the way the stick is held; between beats the pair
// loses speed upward. The stick steers a little every frame.
void Fly_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X;
    if (fly_age[p] < 250)
        fly_age[p]++;
    if (flap_wait[p] == 0)
    {
        if (flaps[p] == 0)
        {
            Enter(gobj, ST_HI_END);
            return;
        }
        flaps[p]--;
        flap_wait[p] = FLAP_INTERVAL;
        flap_now[p] = 1;
        // the pair turns to the way the stick is held as the wings beat
        if (sx * fd->facing_direction < -FLAP_STICK)
            Turn_Round(fd);
    }
    flap_wait[p]--;
}
void Fly_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X;
    float vx = fd->phys.self_vel.X, vy = fd->phys.self_vel.Y;
    if (flap_now[p])
    {
        flap_now[p] = 0;
        vy += FLAP_Y;
        if (sx > FLAP_STICK)
            vx += FLAP_X;
        else if (sx < -FLAP_STICK)
            vx -= FLAP_X;
    }
    vx += sx * FLY_STICK;
    Brake(&vx, FLY_DEC);
    if (vx > FLY_MAX_X)
        vx = FLY_MAX_X;
    if (vx < -FLY_MAX_X)
        vx = -FLY_MAX_X;
    vy -= FLY_FALL;
    if (vy < -FLY_FALL_MAX)
        vy = -FLY_FALL_MAX;
    fd->phys.self_vel.X = vx;
    fd->phys.self_vel.Y = vy;
}
// from a frame of the flight on, an attack in the air or an air dodge ends it (he is helpless once that is over).
// (The script opens the way on HI_CANCEL; what is asked for begins on the frame after, as in the game: published,
// frame 51 of the whole move.)
// (The attack keeps the engine's own code, with one thing added: on the frame it runs out he is helpless at once.
// Left to the engine he would be in an ordinary fall for that one frame, in which a second attack or a second
// Duck Jump could be begun.)
static void HiAttack_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    air_anim[fd->ply](gobj);
    if (fd->state_id >= ASID_FALL && fd->state_id <= ASID_FALLAERIALB)
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, HI_LANDING);
}
void Fly_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fly_age[fd->ply] <= HI_CANCEL)
        return;
    if (Fighter_IASACheck_AirAttack(gobj))
    {
        air_anim[fd->ply] = fd->cb.Anim;
        fd->cb.Anim = HiAttack_Anim;
        return;
    }
    Fighter_IASACheck_AirEscape(gobj);
}
static void Fly_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
// a ledge he faces is caught on the way (the engine's own rule for an up special); a floor is landed on
void Fly_Coll(GOBJ *gobj)
{
    Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Fly_Landed);
}
// tired: the same fall, the stick still steering, until the clip is over; then he falls helpless
void HiEnd_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj);
}
// He has flown. Until he stands again he has no jump left, and whatever he does in the air he falls helpless
// after: an ordinary fall he is found in is made the helpless one.
// Anything else (a ledge, a hit taken, a grab) gives him back what a fighter has after those.
static void Hi_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    if (!hi_spent[p])
        return;
    if (fd->phys.air_state == 0)
        hi_spent[p] = 0;
    else if (s >= ASID_FALL && s <= ASID_FALLAERIALB)
        Fighter_EnterSpecialFall(gobj, 1, 0, 1, 1.0f, HI_LANDING);
    else if (s >= ST_FIRST_OWN || (s >= ASID_ATTACKAIRN && s <= ASID_ATTACKAIRLW) || s == ASID_ESCAPEAIR
             || (s >= ASID_FALLSPECIAL && s <= ASID_FALLSPECIALB))
        fd->jump.jumps_used = fd->attr.max_jumps;
    else
        hi_spent[p] = 0;
}

// ---- Trick Shot: he kicks a can out; the special button again has the unseen gunman shoot it, and it hops
#define ST_N 377
#define ST_N_AIR 378
#define CAN 0                       // the can's item number
#define THROW_JOINT 100             // the joint a thing he has made rides until he lets it go
#define N_KICK 16                 // script: the frame the can is kicked on
#define N_SHOT 4                 // frames from the button to the shot (published for the clay's shot: READING for the can's)
#define N_AGAIN 12               // published: with a can out it can be shot again every so many frames
// what the can's own code is asked for (its first variable), and the states of it this code tells apart
#define CAN_ASK_KICK 1
#define CAN_ASK_SHOT 2
#define CAN_HELD 0
#define CAN_ENDS 6

static u8 n_age[6];                 // frames of this neutral special
static u8 n_kicks[6];               // this one made the can: it is kicked on N_KICK
static const OwnFx target_fx[] = {{0, 6012}};        // the aiming reticle, and its flash as the shot goes off
static const OwnFx target_flash_fx[] = {{0, 6013}};

static void Throw_Place(FighterData *fd, Vec3 *at)
{
    JOBJ_GetWorldPosition(fd->bones[THROW_JOINT].joint, 0, at);
    at->Z = 0;
}
static void N_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    Enter(gobj, state);
    n_age[p] = 0;
    n_kicks[p] = 0;
    if (!Own_Item(gobj, CAN, CAN_ENDS))
    {
        Vec3 at;
        Throw_Place(fd, &at);
        if (Make_Item(gobj, CAN, &at))
            n_kicks[p] = 1;
    }
}
void SpecialN(GOBJ *gobj)
{
    N_Begin(gobj, ST_N);
}
void SpecialAirN(GOBJ *gobj)
{
    N_Begin(gobj, ST_N_AIR);
}
void N_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    GOBJ *can = Own_Item(gobj, CAN, CAN_ENDS);
    if (n_age[p] < 250)
        n_age[p]++;
    if (can)
    {
        ItemData *id = can->userdata;
        if (n_kicks[p] && n_age[p] == N_KICK)
            id->item_var.var1 = CAN_ASK_KICK;
        else if (!n_kicks[p] && n_age[p] == N_SHOT && id->state != CAN_HELD)
        {
            // the shot: the reticle shows on the can, flashes, and the can's own code makes it hop
            id->item_var.var1 = CAN_ASK_SHOT;
            Fx_At(gobj, target_fx, sizeof(target_fx) / sizeof(target_fx[0]), 0, &id->pos);
            Fx_At(gobj, target_flash_fx, sizeof(target_flash_fx) / sizeof(target_flash_fx[0]), 0, &id->pos);
        }
    }
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj);
}
// with a can out, the special button again starts the move again (the next shot)
void N_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X, sy = fd->input.lstick.Y;
    if (n_age[p] < N_AGAIN || !(fd->input.down & B_BUTTON) || sx > 0.5f || sx < -0.5f || sy > 0.5f || sy < -0.5f)
        return;
    if (n_kicks[p] && n_age[p] < N_KICK)
        return;
    if (Own_Item(gobj, CAN, CAN_ENDS))
        N_Begin(gobj, fd->phys.air_state == 0 ? ST_N : ST_N_AIR);
}
void N_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
    if (n_kicks[p] && n_age[p] < N_KICK)
    {
        // until the kick the can rides the joint the game carries it on
        GOBJ *can = Own_Item(gobj, CAN, CAN_ENDS);
        if (can && ((ItemData *)can->userdata)->state == CAN_HELD)
            Throw_Place(fd, &((ItemData *)can->userdata)->pos);
    }
}
void N_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            ActionStateChange(fd->state.frame, 1, 0, gobj, ST_N_AIR, 0x5000, 0);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        ActionStateChange(fd->state.frame, 1, 0, gobj, ST_N, 0x5000, 0);
    }
}

// ---- Clay Shooting: he throws a clay pigeon; the special button again has the unseen gunman shoot it
#define ST_S 379
#define ST_S_AIR 380
#define CLAY 1                     // the clay's item number
#define S_THROW 17               // script: the frame the clay is let go on
#define S_SHOT 4                 // published: frames from the button to the shot
#define S_FLICK 4               // frames since the stick was flicked that still make it a hard throw (READING)
#define CLAY_ASK_THROW 1
#define CLAY_ASK_HARD 2
#define CLAY_ASK_SHOT 3
#define CLAY_ENDS 2

static u8 s_age[6];                 // frames of this side special
static u8 s_throws[6];              // this one throws a clay (none was out): 1, or 2 for a hard throw
static u8 s_shot_in[6];             // frames until the shot that was asked for (0: none)

static void S_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    int hard = fd->input.timer_lstick_smash_x < S_FLICK;
    Enter(gobj, state);
    s_age[p] = 0;
    s_throws[p] = 0;
    s_shot_in[p] = 0;
    if (Own_Item(gobj, CLAY, CLAY_ENDS))
        s_shot_in[p] = S_SHOT;                          // one is out: this press is a shot at it
    else
        s_throws[p] = hard ? 2 : 1;
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
    int p = fd->ply;
    if (s_age[p] < 250)
        s_age[p]++;
    if (s_throws[p] && s_age[p] == S_THROW)
    {
        // the clay leaves the joint the game carries it on
        Vec3 at;
        GOBJ *clay;
        Throw_Place(fd, &at);
        clay = Make_Item(gobj, CLAY, &at);
        if (clay)
            ((ItemData *)clay->userdata)->item_var.var1 = s_throws[p] == 2 ? CLAY_ASK_HARD : CLAY_ASK_THROW;
    }
    if (s_shot_in[p] && --s_shot_in[p] == 0)
    {
        GOBJ *clay = Own_Item(gobj, CLAY, CLAY_ENDS);
        if (clay)
        {
            ItemData *id = clay->userdata;
            id->item_var.var1 = CLAY_ASK_SHOT;
            Fx_At(gobj, target_fx, sizeof(target_fx) / sizeof(target_fx[0]), 0, &id->pos);
            Fx_At(gobj, target_flash_fx, sizeof(target_flash_fx) / sizeof(target_flash_fx[0]), 0, &id->pos);
        }
    }
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj);
}
// once the clay has left him, the special button (the stick anywhere) asks for a shot at it
void S_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if ((fd->input.down & B_BUTTON) && !s_shot_in[p] && (!s_throws[p] || s_age[p] >= S_THROW))
        s_shot_in[p] = S_SHOT;
}
void S_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void S_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            ActionStateChange(fd->state.frame, 1, 0, gobj, ST_S_AIR, 0x5000, 0);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        ActionStateChange(fd->state.frame, 1, 0, gobj, ST_S, 0x5000, 0);
    }
}

// ---- Wild Gunman: a gunman of the old game appears in front of him, waits, fires once and goes
#define ST_LW 381
#define ST_LW_AIR 382
#define GUNMAN 2                 // the first of the gunmen's item numbers: one item for each of the five
#define GUNMEN 5
#define BULLET 7
#define LW_APPEAR 6           // script: the frame the gunman appears on
#define LW_START_X 0.699999988079071f         // vl.prc param_special_lw start_speed_mul_x: what he keeps of his speed as it starts,
#define LW_START_Y 0.6000000238418579f         // start_speed_mul_y,
#define LW_AIR_Y 0.6000000238418579f             // air_start_add_speed_y: and, started in the air, the speed upward added

static u8 lw_age[6];                // frames of this down special
static u8 gun_used[6];              // which of the five have come in this round (wiki: each comes once a round, in no set order)

static GOBJ *Own_Gunman(GOBJ *gobj)
{
    int k;
    for (k = 0; k < GUNMEN; k++)
    {
        GOBJ *g = Own_Item(gobj, GUNMAN + k, 99);
        if (g)
            return g;
    }
    return 0;
}
void SpecialLw(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_LW);
    lw_age[fd->ply] = 0;
    fd->phys.self_vel_ground.X *= LW_START_X;
}
void SpecialAirLw(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_LW_AIR);
    lw_age[fd->ply] = 0;
    fd->phys.self_vel.X *= LW_START_X;
    fd->phys.self_vel.Y = fd->phys.self_vel.Y * LW_START_Y + LW_AIR_Y;
}
void Lw_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (lw_age[p] < 250)
        lw_age[p]++;
    if (lw_age[p] == LW_APPEAR && !Own_Gunman(gobj))
    {
        // one of the five that has not come yet this round, where the game's joint for a made thing is
        Vec3 at;
        int k, left = 0, pick;
        if (gun_used[p] == 0x1F)
            gun_used[p] = 0;
        for (k = 0; k < GUNMEN; k++)
            if (!(gun_used[p] & (1 << k)))
                left++;
        pick = HSD_Randi(left);
        for (k = 0; k < GUNMEN; k++)
            if (!(gun_used[p] & (1 << k)) && pick-- == 0)
                break;
        gun_used[p] |= 1 << k;
        Throw_Place(fd, &at);
        at.Y += 1.0f;                                   // (set down just over the floor: he comes to stand on it)
        Make_Item(gobj, GUNMAN + k, &at);
    }
    if (FrameTimerCheck(gobj) == 0)
        Leave(gobj);
}
void Lw_Phys(GOBJ *gobj)
{
    S_Phys(gobj);
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            ActionStateChange(fd->state.frame, 1, 0, gobj, ST_LW_AIR, 0x5000, 0);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
    {
        Fighter_SetGrounded2(fd);
        ActionStateChange(fd->state.frame, 1, 0, gobj, ST_LW, 0x5000, 0);
    }
}
// The gunman says when he fires (his third variable); the bullet is made here, at his gun, going the way he faces.
// A gunman's bullet that reaches his own can knocks it along, the way the bullet flies, and ends there.
// (wiki, Trick Shot: the can's flight can be changed "by simply attacking it or hitting it with projectiles", struck
// from its other side it goes the other way, and what sets it off is an opponent's touch or its time. ADJUSTMENT: how
// far the game throws a struck can is in its code; here it is one hop, as a shot's.)
// It is seen a frame ahead, where the bullet will be at the end of this frame, so that the two never meet as hits
// (which would set the can off).
#define CAN_KNOCK_R 3
#define CAN_KNOCK_L 4
#define CAN_HALF_W 2.4599264270219123f         // the can's own size about its middle
#define CAN_HALF_H 3.3618231938652343f
static const float bullet_reach[] = {1.163769006729126f, 1.163769006729126f, 1.163769006729126f, 3.87923002243042f, 3.87923002243042f};       // how far each gunman's shot reaches about the bullet
static void Knock_Frame(GOBJ *gobj)
{
    GOBJ *can = Own_Item(gobj, CAN, CAN_ENDS), *shot = Own_Item(gobj, BULLET, 99);
    ItemData *cd, *sd;
    float reach, from, to, dy;
    if (!can || !shot)
        return;
    cd = can->userdata;
    sd = shot->userdata;
    if (cd->state == 0 || sd->state == 0 || sd->state > GUNMEN || sd->item_var.var3)
        return;                                         // the can still carried; the bullet not let go yet, or spent
    reach = bullet_reach[sd->state - 1];
    from = sd->pos.X - cd->pos.X;
    to = from + sd->self_vel.X;
    if (from > to)
    {
        float t = from;
        from = to;
        to = t;
    }
    dy = sd->pos.Y - cd->pos.Y;
    if (to < -(CAN_HALF_W + reach) || from > CAN_HALF_W + reach || dy > CAN_HALF_H + reach || dy < -(CAN_HALF_H + reach))
        return;
    cd->item_var.var1 = sd->self_vel.X > 0 ? CAN_KNOCK_R : CAN_KNOCK_L;
    sd->item_var.var3 = 1;
}
static void Gun_Frame(FighterData *fd, GOBJ *gobj)
{
    GOBJ *g = Own_Gunman(gobj);
    if (g)
    {
        ItemData *id = g->userdata;
        if (id->item_var.var3 == 1)
        {
            Vec3 at = id->pos;
            GOBJ *shot;
            id->item_var.var3 = 0;
            at.X += (float)id->item_var.var5 * 0.01f * id->facing_direction;
            at.Y += (float)id->item_var.var6 * 0.01f;
            shot = Make_Item(gobj, BULLET, &at);
            if (shot)
            {
                ItemData *sd = shot->userdata;
                sd->facing_direction = id->facing_direction;
                sd->item_var.var1 = id->item_var.var7 + 1;
            }
        }
    }
}

#define SOUND_FALL 21      // se_duckhunt_special_h04, by its place in his bank (0: not in it)
static u8 fall_was[6];              // he was falling helpless on the frame before

// The sound of his helpless fall. (script: game_fallspecial plays it for as long as that fall lasts. The engine's
// helpless fall has no script of his, so it is begun here as the fall begins, as a sound the engine ends when his
// state changes: it dies away from the frame he lands on.)
static void Fall_Frame(FighterData *fd)
{
    int p = fd->ply;
    int falls = fd->state_id >= ASID_FALLSPECIAL && fd->state_id <= ASID_FALLSPECIALB;
    if (falls && !fall_was[p] && SOUND_FALL)
        PLAY_STATE_SOUND(fd, 5000 + SOUND_FALL, 127, 64);
    fall_was[p] = falls;
}
static void OwnLoad(FighterData *fd)
{
    ItemDesc **items = fd->ftData->items;
    int i;
    hi_spent[fd->ply] = 0;
    gun_used[fd->ply] = 0;
    fall_was[fd->ply] = 0;
    for (i = 0; items && i < 8; i++)
        if (items[i])
            MEX_IndexFighterItem(fd->kind, items[i], i);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Hi_Frame(fd, gobj);
    Fall_Frame(fd);
    Gun_Frame(fd, gobj);
    Knock_Frame(gobj);
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 11
static const s8 face_rest[FACE_GROUPS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {61, 9, 2},
    {62, 9, 0},
    {63, 9, 2},
    {64, 9, -1},
    {121, 7, 2},
    {122, 7, 1},
    {123, 7, 2},
    {124, 7, 0},
    {140, 9, 2},
    {141, 9, 0},
    {142, 9, 2},
    {143, 9, -1},
    {226, 9, 2},
    {227, 9, 0},
    {228, 9, 2},
    {229, 9, -1},
    {239, 7, 2},
    {240, 7, 1},
    {241, 7, 2},
    {242, 7, 0},
    {309, 9, 2},
    {310, 9, 0},
    {311, 9, 2},
    {312, 9, -1},
    {334, 7, 2},
    {335, 7, 1},
    {336, 7, 2},
    {337, 7, 0},
    {387, 9, 2},
    {388, 9, 0},
    {389, 9, 2},
    {390, 9, -1},
    {480, 9, 2},
    {481, 9, 0},
    {482, 9, 2},
    {483, 9, -1},
    {499, 7, 2},
    {500, 7, 1},
    {501, 7, 2},
    {502, 7, 0},
    {566, 9, 2},
    {567, 9, 0},
    {568, 9, 2},
    {569, 9, -1},
    {32767, 0, 0},
};
#define FACE_CLOCK_END 600
static const u16 face_left[] = {0, 0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 128, 0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 128, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 128, 128, 128, 128, 0, 0, 128, 0, 128, 128, 128, 128, 128, 0, 128, 0, 128, 0, 128};
// (a fighter made for a screen that is not a match plays the rows of its other table of actions)
static const u16 face_left_other[] = {0, 0, 0, 128, 0, 0, 128, 128, 128, 128, 128, 128, 128, 128};
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
    if (!(s == 377 || s == 378 || s == 379 || s == 380 || s == 381 || s == 382) || fd->state.frame > 4.0f)
        return;
    if (fd->input.lstick.X * fd->facing_direction < -0.5f)
    {
        fd->facing_direction = -fd->facing_direction;
        fd->phys.self_vel.X = -fd->phys.self_vel.X;
        Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
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
    {304, 0x340213, 0x14, 0x0, HiStart_Anim, 0, HiStart_Phys, HiStart_Coll, Fighter_UpdateCameraBox},    // 373: Duck Jump: the duck takes hold
    {313, 0x340613, 0x14, 0x0, HiStart_Anim, 0, HiStart_Phys, HiStart_Coll, Fighter_UpdateCameraBox},    // 374: Duck Jump: the duck takes hold, in the air
    {321, 0x340613, 0x14, 0x0, Fly_Anim, Fly_IASA, Fly_Phys, Fly_Coll, Fighter_UpdateCameraBox},    // 375: Duck Jump: the flight
    {305, 0x340613, 0x14, 0x0, HiEnd_Anim, 0, Fly_Phys, Fly_Coll, Fighter_UpdateCameraBox},    // 376: Duck Jump: the duck tires
    {306, 0x340211, 0x12, 0x0, N_Anim, N_IASA, N_Phys, N_Coll, Fighter_UpdateCameraBox},    // 377: Trick Shot
    {315, 0x340611, 0x12, 0x0, N_Anim, N_IASA, N_Phys, N_Coll, Fighter_UpdateCameraBox},    // 378: Trick Shot in the air
    {303, 0x340212, 0x13, 0x0, S_Anim, S_IASA, S_Phys, S_Coll, Fighter_UpdateCameraBox},    // 379: Clay Shooting
    {312, 0x340612, 0x13, 0x0, S_Anim, S_IASA, S_Phys, S_Coll, Fighter_UpdateCameraBox},    // 380: Clay Shooting in the air
    {323, 0x340214, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 381: Wild Gunman
    {325, 0x340614, 0x15, 0x0, Lw_Anim, 0, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 382: Wild Gunman in the air
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
