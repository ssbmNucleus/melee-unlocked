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

// ---- Fire Mace: the swing (its script's hit), held for a bigger one, and the fireball it lets go ---------
#define ST_N 373
#define ST_N_AIR 374
#define N_END 30000                   // the frame she may act from, on the ground,
#define N_END_AIR 30000           //   and in the air
#define FIREBALL 0             // the fireball's item number,
#define FIREBALL_FULL 1   //   and a full charge's
#define N_FIRE 13                 // script: the frame the fireball is made on
#define MUZZLE_X 10.126076047255312f             // where it is made: the fire mace's place on that frame, ahead of her,
#define MUZZLE_Y 5.892068808856454f             //   and above her feet
#define SOUND_FIRE 37           // se_ittledew_fireball, by its place in her bank (0: not in it)
#define N_HOLD 6                 // script: the frame the swing stops on while B is held (its clip's rate 0)
#define N_MOST 15                 // the most the charge counts (the game's check: spn_charge, at least this)
#define N_SWING 9               // script: the frame the mace's hit is made
#define N_BIG 1.6f                   // a full charge's mace: its hit's size times this
static u8 n_held[6];                    // by player: what the charge has counted, plus one (0: not held)
static u8 n_full[6];                    // by player: the charge ran to its most
static void Shoot(GOBJ *gobj, int which)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    GOBJ *shot;
    at.X = fd->phys.pos.X + MUZZLE_X * fd->facing_direction;
    at.Y = fd->phys.pos.Y + MUZZLE_Y;
    at.Z = 0;
    shot = Make_Item(gobj, which, &at);
    if (shot)
        ((ItemData *)shot->userdata)->item_var.var1 = 1;        // its own code sends it on its way
}
static void N_Begin(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    n_held[fd->ply] = 0;
    n_full[fd->ply] = 0;
    Enter(gobj, state);
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
    int p = fd->ply, frame;
    if (n_held[p])
        return;                                         // held: the clip and its script stand still (N_IASA lets go)
    frame = Tick(gobj);
    if (frame == N_SWING && n_full[p])
        fd->hitbox[0].size *= N_BIG;
    if (frame == N_FIRE)
        Shoot(gobj, n_full[p] ? FIREBALL_FULL : FIREBALL);
    if (frame >= (fd->phys.air_state == 0 ? N_END : N_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
// The charge, counted once a frame from the frame the swing stops on (the game adds one and then asks, so its count
// is 1 on that frame): let go when B is up or the count reaches its most, and the clip and its script go on from
// the next frame, as the game's partial move starts on the frame its check lets go. (Counted from the frame after
// it stopped, round 6 swung on frame 22 for the game's 23.)
void N_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (!n_held[p] && t_frame[p] == N_HOLD && (fd->input.held & B_BUTTON))
    {
        n_held[p] = 1;
        Fighter_SetAnimRate(gobj, 0.0f);
    }
    if (!n_held[p])
        return;
    n_held[p]++;
    if (n_held[p] > N_MOST || !(fd->input.held & B_BUTTON))
    {
        n_full[p] = n_held[p] > N_MOST;
        n_held[p] = 0;
        Fighter_SetAnimRate(gobj, 1.0f);
    }
}
// as the template's, and held still through a hand-over to the other form (which plays the clip on at its rate)
void N_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Plain_Coll(gobj);
    if (n_held[fd->ply])
        Fighter_SetAnimRate(gobj, 0.0f);
}

// ---- Dynamite: the throw, and the bomb she throws (an item of hers) ---------------------------------------
#define ST_S 375
#define ST_S_AIR 376
#define S_END 30000                   // the frame she may act from, on the ground,
#define S_END_AIR 30000           //   and in the air
#define BOMB 2                     // the bomb's item number
#define S_THROW 17               // script: the frame it is let go
#define BOMB_X 7.64368919705849f                 // where it is made: her left fist on that frame and the game's offset, ahead,
#define BOMB_Y 8.67646119065214f                 //   and above her feet
#define THROW_X 0.38070639907012044f               // its speed as it leaves her, a frame: ahead of her,
#define THROW_STICK_X 0.5710595986051806f   //   and this times the stick across (not turned by her facing),
#define THROW_Y 1.1421191972103613f               //   up,
#define THROW_STICK_Y 0.38070639907012044f   //   and this times the stick up
#define BOMB_AGAIN 95         // frames from one bomb before another is thrown (the game's timer `bomb`)
static u16 bomb_wait[6];                // by player: frames since her last bomb
static void Throw(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    GOBJ *bomb;
    at.X = fd->phys.pos.X + BOMB_X * fd->facing_direction;
    at.Y = fd->phys.pos.Y + BOMB_Y;
    at.Z = 0;
    bomb = Make_Item(gobj, BOMB, &at);
    if (bomb)
    {
        ItemData *id = bomb->userdata;
        id->self_vel.X = THROW_X * fd->facing_direction + THROW_STICK_X * fd->input.lstick.X;
        id->self_vel.Y = THROW_Y + THROW_STICK_Y * fd->input.lstick.Y;
        id->item_var.var1 = 1;
        bomb_wait[fd->ply] = 0;
    }
}
void SpecialS(GOBJ *gobj)
{
    Enter(gobj, ST_S);
}
void SpecialAirS(GOBJ *gobj)
{
    Enter(gobj, ST_S_AIR);
}
void S_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame >= (fd->phys.air_state == 0 ? S_END : S_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void S_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] == S_THROW && bomb_wait[fd->ply] >= BOMB_AGAIN)
        Throw(gobj);                    // sample the throw frame's stick, after input refresh
}
static void S_Frame(FighterData *fd)
{
    if (bomb_wait[fd->ply] < 60000)
        bomb_wait[fd->ply]++;
}

// ---- Teleport: the hop, the ball she steers, and the jump to it ------------------------------------------
#define ST_HI 377
#define ST_HI_AIR 378
#define BALL 3                     // the ball's item number
#define HI_RISE 0.5897878173572685f               // speed up as it starts: to rise the game's jump height under the move's gravity
#define HI_GRAV 0.012690213302337348f               // the move's own gravity, a frame a frame,
#define HI_FALL_MOST 0.38070639907012044f     //   and its own fall speed, a frame
#define HI_BALL 21               // script: the frame the ball is made (ittle_reset_teleport)
#define HI_STAND 40             // the frame it is no longer steered (the game's call each frame is taken down)
#define HI_GO 41                   // script: the frame she goes to it (ittle_teleport_do: still, gravity 0)
#define HI_LEDGE_OFF 40     // script: from this frame no ledge is caught (grabedge 0),
#define HI_LEDGE_ON 43       //   and from this one again
#define HI_DRIFT 43             // script: from this frame she may drift (allowMovement),
#define HI_DRIFT_MULT 0.65f   //   this much of her own air speed
#define HI_END 71                 // the frame her helpless fall begins (specialfall)
#define HI_LANDING 29.0f         // frames of landing after the helpless fall
static GOBJ *hi_ball[6];                // by player: her ball while it is hers to take
static void Hi_Ask(GOBJ *gobj, int ask)
{
    FighterData *fd = gobj->userdata;
    GOBJ *ball = hi_ball[fd->ply];
    if (ball)
        ((ItemData *)ball->userdata)->item_var.var1 = ask;
}
static void Hi_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    hi_ball[fd->ply] = 0;
    Enter(gobj, ST_HI_AIR);
    fd->phys.self_vel.Y = HI_RISE;
}
void SpecialHi(GOBJ *gobj)
{
    Hi_Begin(gobj);
}
void SpecialAirHi(GOBJ *gobj)
{
    Hi_Begin(gobj);
}
// she is where the ball is: the collision told so, as the game's own moves that set a fighter down elsewhere do
// (mpColl_80043680, written out as own_gekkouga.py's Lw_Strike has it)
static void Hi_Go(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    GOBJ *ball = hi_ball[fd->ply];
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    if (!ball)
        return;
    fd->phys.pos.X = ((ItemData *)ball->userdata)->pos.X;
    fd->phys.pos.Y = ((ItemData *)ball->userdata)->pos.Y;
    fd->coll_data.topN_Curr = fd->phys.pos;
    fd->coll_data.topN_CurrCorrect = fd->phys.pos;
    fd->coll_data.topN_Prev = fd->phys.pos;
    fd->coll_data.flags |= 0x20;
    Hi_Ask(gobj, 3);                                    // the ball is gone
    hi_ball[fd->ply] = 0;
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame == HI_BALL)
    {
        hi_ball[fd->ply] = Make_Item(gobj, BALL, &fd->phys.pos);
        Hi_Ask(gobj, 1);
    }
    if (frame == HI_STAND)
        Hi_Ask(gobj, 2);
    if (frame == HI_GO)
        Hi_Go(gobj);
    if (frame >= HI_END || FrameTimerCheck(gobj) == 0)
    {
        // the helpless fall, its landing lag kept however slowly she lands (its second argument: the engine
        // otherwise lands a slow fall with none, `ftCo_FallSpecial.c:ftCo_80096D28`)
        if (fd->phys.air_state == 0)
            Fighter_EnterWait(gobj);
        else
            Fighter_EnterSpecialFall(gobj, 1, 1, 1, 1.0f, HI_LANDING);
    }
}
// a ledge she faces is caught on the way (the game lets her catch one: grabedge), save between HI_LEDGE_OFF and
// HI_LEDGE_ON; a floor ends it
static void Hi_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
void Hi_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply];
    float most;
    if (frame < HI_GO)
    {
        // the hop under the move's own gravity; no drift (allowMovement 0): what speed she had across dies away
        fd->phys.self_vel.Y -= HI_GRAV;
        if (fd->phys.self_vel.Y < -HI_FALL_MOST)
            fd->phys.self_vel.Y = -HI_FALL_MOST;
        Fighter_PhysAir_DecayXVelocity(fd, fd->attr.aerial_friction);
    }
    else if (frame >= HI_DRIFT)
    {
        // still (gravity 0), drifting at a share of her air speed
        most = fd->attr.aerial_drift_max * HI_DRIFT_MULT;
        Fighter_PhysAir_ApplyAerialDrift(fd);
        if (fd->phys.self_vel.X > most)
            fd->phys.self_vel.X = most;
        if (fd->phys.self_vel.X < -most)
            fd->phys.self_vel.X = -most;
        fd->phys.self_vel.Y = 0;
    }
}
void Hi_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply];
    if (frame <= 1)
        return;
    if (frame >= HI_LEDGE_OFF && frame < HI_LEDGE_ON)
    {
        if (Fighter_CollAir_IgnoreLedge_NoCB(gobj) != 0)
        {
            Fighter_SetGrounded2(fd);
            Hi_Landed(gobj);
        }
    }
    else
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
}

// ---- Fire Dive: a hop, then a dive with its hit until she lands, and the landing --------------------
#define ST_LW 379
#define ST_LW_AIR 380
#define ST_LW_IMPACT 381
#define LW_RISE 1.5228256104320117f               // speed up as it starts: to rise the game's jump height of 4
#define LW_DIVE 21               // the frame the dive begins (the game's frame 20)
#define LW_FALL 0.10152170641869879f               // speed gained downward a frame in the dive (the game's gravity of 80)
#define LW_FALL_MOST 3.8070639907012045f     // the fastest she dives (the game's 50 units a second down)
#define LW_IMPACT_END 26   // frames of the landing (the game's 26 in specialdownimpact)
static void Lw_Begin(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    Enter(gobj, ST_LW_AIR);
    fd->phys.self_vel.Y = LW_RISE;
}
void SpecialLw(GOBJ *gobj)
{
    Lw_Begin(gobj);
}
void SpecialAirLw(GOBJ *gobj)
{
    Lw_Begin(gobj);
}
void Lw_Anim(GOBJ *gobj)
{
    Tick(gobj);
}
void Lw_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] < LW_DIVE)
    {
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
        return;
    }
    if (t_frame[fd->ply] == LW_DIVE)
    {
        fd->phys.self_vel.X *= 0.5f;                    // (the game's drift in the dive: half)
        if (fd->phys.self_vel.Y > 0)
            fd->phys.self_vel.Y = 0;
    }
    fd->phys.self_vel.Y -= LW_FALL;
    if (fd->phys.self_vel.Y < -LW_FALL_MOST)
        fd->phys.self_vel.Y = -LW_FALL_MOST;
}
static void Lw_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] < LW_DIVE)
        Fighter_EnterLanding(gobj);
    else
    {
        Fighter_SetGrounded2(fd);
        fd->phys.self_vel.X = 0;
        fd->phys.self_vel.Y = 0;
        Enter(gobj, ST_LW_IMPACT);
    }
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] > 1)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Lw_Landed);
}
void LwImpact_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= LW_IMPACT_END || FrameTimerCheck(gobj) == 0)
        Fighter_EnterWait(gobj);
}
void LwImpact_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
void LwImpact_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (Fighter_CollGround_PassLedge(gobj) == 0)
    {
        Fighter_SetAirborne(fd);
        Fighter_EnterFall(gobj);
    }
}

static void OwnLoad(FighterData *fd)
{
    ItemDesc **items = fd->ftData->items;
    int i, p = fd->ply;
    t_frame[p] = 0;
    n_held[p] = 0;
    n_full[p] = 0;
    bomb_wait[p] = BOMB_AGAIN;                          // (a bomb may be thrown at once)
    hi_ball[p] = 0;
    for (i = 0; items && i < 4; i++)
        if (items[i])
            MEX_IndexFighterItem(fd->kind, items[i], i);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    if (fd->state_id != ST_N && fd->state_id != ST_N_AIR)
        n_held[fd->ply] = 0;
    S_Frame(fd);
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
    }
    return state;                       // (a state not listed: a mistake; it stays the state it is)
}


typedef struct {int action,state,first,end,target,neutral;} JabWindow;
static const JabWindow jab_windows[] = {
    {46, 44, 11, 21, 45, 1},
    {47, 45, 11, 29, 46, 1},
};
static void (*jab_original[6])(GOBJ *);
static void Jab_Load(FighterData *fd)
{
    jab_original[fd->ply] = 0;
}
static void Jab_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int i;
    if (fd->flags.hitlag)
        return;
    // Disable the donor's independently timed buffer and latched gate, including stale early presses.
    fd->hitlag_mult = 0;
    fd->flags.x2218_2 = 0;
    fd->state_var.state_var1 = 0;
    for (i = 0; i < sizeof(jab_windows) / sizeof(jab_windows[0]); i++)
    {
        const JabWindow *w = &jab_windows[i];
        if (fd->action_id != w->action || fd->state_id != w->state ||
            fd->state.frame < w->first || fd->state.frame >= w->end || !(fd->input.down & HSD_BUTTON_A))
            continue;
        // Slap City InputValidator.InsideSegmentSC: AlmostNeutral uses strict NeutralMag (0.15).
        if (w->neutral && (fd->input.lstick.X <= -0.15f || fd->input.lstick.X >= 0.15f ||
                           fd->input.lstick.Y <= -0.15f || fd->input.lstick.Y >= 0.15f))
            continue;
        // Retail ftCo_Attack1.c initialization, including item pickup and state bookkeeping.
        if (w->target == 45)
            ((void (*)(GOBJ *))0x8008AE30)(gobj); // doAttack12
        else
            ((void (*)(GOBJ *))0x8008B194)(gobj); // doAttack13
        return;
    }
    if (jab_original[fd->ply])
        jab_original[fd->ply](gobj);
}
static void Jab_Frame(FighterData *fd)
{
    int i, applies = 0;
    for (i = 0; i < sizeof(jab_windows) / sizeof(jab_windows[0]); i++)
        if (fd->action_id == jab_windows[i].action && fd->state_id == jab_windows[i].state)
            applies = 1;
    if (applies)
    {
        if (fd->cb.IASA != Jab_IASA)
        {
            jab_original[fd->ply] = fd->cb.IASA;
            fd->cb.IASA = Jab_IASA;
        }
    }
    else if (fd->cb.IASA == Jab_IASA)
        fd->cb.IASA = jab_original[fd->ply];
}

// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseInput(FighterData *fd, GOBJ *gobj)
{
    int s = fd->state_id;
    if (!(s == 373 || s == 374 || s == 375 || s == 376 || s == 377 || s == 378 || s == 379 || s == 380) || fd->state.frame > 4.0f)
        return;
    if (fd->input.lstick.X * fd->facing_direction < -0.5f)
    {
        fd->facing_direction = -fd->facing_direction;
        fd->phys.self_vel.X = -fd->phys.self_vel.X;
        Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
    }
}

void Reverse_IASA_373(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        N_IASA(gobj);
}

void Reverse_IASA_374(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        N_IASA(gobj);
}

void Reverse_IASA_375(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        S_IASA(gobj);
}

void Reverse_IASA_376(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        S_IASA(gobj);
}

void Reverse_IASA_377(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
}

void Reverse_IASA_379(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
}

void Reverse_IASA_380(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    Jab_Load(fd);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    Jab_Frame(fd);
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
    {306, 0x340211, 0x12, 0x0, N_Anim, Reverse_IASA_373, Plain_Phys, N_Coll, Fighter_UpdateCameraBox},    // 373: Fire Mace
    {315, 0x340611, 0x12, 0x0, N_Anim, Reverse_IASA_374, Plain_Phys, N_Coll, Fighter_UpdateCameraBox},    // 374: Fire Mace in the air
    {303, 0x340212, 0x13, 0x0, S_Anim, Reverse_IASA_375, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: Dynamite
    {312, 0x340612, 0x13, 0x0, S_Anim, Reverse_IASA_376, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: Dynamite in the air
    {321, 0x340213, 0x14, 0x0, Hi_Anim, Reverse_IASA_377, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 377: Teleport
    {322, 0x340613, 0x14, 0x0, Hi_Anim, Reverse_IASA_378, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 378: Teleport in the air
    {323, 0x340214, 0x15, 0x0, Lw_Anim, Reverse_IASA_379, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 379: Fire Dive
    {325, 0x340614, 0x15, 0x0, Lw_Anim, Reverse_IASA_380, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 380: Fire Dive in the air
    {305, 0x340214, 0x15, 0x0, LwImpact_Anim, 0, LwImpact_Phys, LwImpact_Coll, Fighter_UpdateCameraBox},    // 381: Fire Dive: the landing
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
