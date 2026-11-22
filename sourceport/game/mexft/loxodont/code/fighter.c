#include "../mex.h"

static int LoxDownLevel(FighterData *fd);
static void LoxEnter(GOBJ *gobj);
static void RG_Enter(GOBJ *gobj);
static int RG_IsHolding(GOBJ *gobj);

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
    LoxEnter(gobj);
    RG_Enter(gobj);
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

// ---- the neutral special: PLAIN (its clip, its script's hits, its end) ---------------------------
#define ST_N 373                        // its state on the ground,
#define ST_N_AIR 374                //   and in the air
#define N_END 90                   // the natural end, on the ground (NEVER: at the clip's end),
#define N_END_AIR 90           //   and in the air
void SpecialN(GOBJ *gobj)
{
    Enter(gobj, ST_N);
}
void SpecialAirN(GOBJ *gobj)
{
    Enter(gobj, ST_N_AIR);
}
// ANIMATION, each frame after the clip and the script have moved on: what the move does on this frame, and
// whether it is over.
void N_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    // KIND: a shot fired, a thing placed or thrown, a helper that appears: made here on the frame the game's
    // script lets it go (SPECIALS.md "A shot fired"):      if (frame == N_FIRE) Shoot(gobj);
    // KIND: a charge: a state that loops while B_BUTTON is held and counts its frames (SPECIALS.md "A charge").
    if (frame >= (fd->phys.air_state == 0 ? N_END : N_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void N_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 76)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 76)
        Fighter_IASACheck_AllAerial(gobj);
}

// ---- the side special: PLAIN ------------------------------------------------------------------------
#define ST_S 375
#define ST_S_AIR 376
#define S_END 86                   // the natural end, on the ground,
#define S_END_AIR 86           //   and in the air
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
    // KIND: a dash that hits: three states (he gathers himself, the dash, the arrival), each begun here by the
    // one before as its frames run out (SPECIALS.md "A dash that hits").
    // KIND: a move that vanishes and reappears: no example yet (SPECIALS.md "Kinds with no example").
    if (frame >= (fd->phys.air_state == 0 ? S_END : S_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void S_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 76)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 76)
        Fighter_IASACheck_AllAerial(gobj);
}

// ---- the up special: PLAIN (he does not rise: he is helpless after it in the air) -------------------
#define ST_HI 377
#define ST_HI_AIR 378
#define HI_END 59                 // the natural end, on the ground,
#define HI_END_AIR 59         //   and in the air
#define HI_LANDING 29.0f         // frames of landing after the helpless fall (the game's list of motions)
void SpecialHi(GOBJ *gobj)
{
    Enter(gobj, ST_HI);
}
void SpecialAirHi(GOBJ *gobj)
{
    Enter(gobj, ST_HI_AIR);
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    // KIND: a recovery that rises: he leaves the ground (Fighter_SetAirborne) and is given his speed as the
    // rise begins, with a physics call that keeps it and a collision call that catches a ledge
    // (SPECIALS.md "A recovery that rises").
    if (frame >= (fd->phys.air_state == 0 ? HI_END : HI_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 1, HI_LANDING);
}
void Hi_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 30000)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}

// ---- the down special: PLAIN ------------------------------------------------------------------------
#define ST_LW 379
#define ST_LW_AIR 380
#define LW_END 53                 // the natural end, on the ground,
#define LW_END_AIR 53         //   and in the air
void SpecialLw(GOBJ *gobj)
{
    Enter(gobj, ST_LW);
}
void SpecialAirLw(GOBJ *gobj)
{
    Enter(gobj, ST_LW_AIR);
}
void Lw_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    // KIND: a reflector: a bubble put up again on every frame it should stand (SPECIALS.md "A reflector").
    // KIND: a counter: no example on this route yet (SPECIALS.md "Kinds with no example").
    // KIND: a helper that appears and acts: an item made here (SPECIALS.md "A helper that appears and acts").
    if (frame >= (fd->phys.air_state == 0 ? LW_END : LW_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void Lw_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 34)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 34)
        Fighter_IASACheck_AllAerial(gobj);
}

// ---- once as he is made, and once a frame -----------------------------------------------------------
// As the fighter is made (the route calls the donor's own load first). Every variable kept by player is cleared
// here: a rematch starts with what the last match left in them otherwise.
// KIND: a module that makes things tells m-ex each item's number here (SPECIALS.md "A thing a special makes"):
//     ItemDesc **items = fd->ftData->items;
//     for (i = 0; items && i < ITEM_COUNT; i++) if (items[i]) MEX_IndexFighterItem(fd->kind, items[i], i);
static void OwnLoad(FighterData *fd)
{
    t_frame[fd->ply] = 0;
}
// Every frame, whatever state he is in, before the frame's clip moves on. For what no one state can see: a thing
// he made asking him for something, a meter, a rule that outlasts the special (no jump until he stands).
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
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
    case ST_HI: return ST_HI_AIR;
    case ST_HI_AIR: return ST_HI;
    case ST_LW: return ST_LW_AIR;
    case ST_LW_AIR: return ST_LW;
    }
    switch(state) {
    case 381: return 382;
    case 382: return 381;
    case 383: return 384;
    case 384: return 383;
    case 385: return 386;
    case 386: return 385;
    case 387: return 388;
    case 388: return 387;
    case 389: return 390;
    case 390: return 389;
    case 391: return 392;
    case 392: return 391;
    case 393: return 394;
    case 394: return 393;
    case 395: return 396;
    case 396: return 395;
    case 397: return 398;
    case 398: return 397;
    }
    return state;                       // (a state not listed: a mistake; it stays the state it is)
}

static void RV_Jump(GOBJ *gobj,int state,int frame)
{
    FighterData *fd=gobj->userdata;
    ActionStateChange(frame-1,1,0,gobj,state,RG_IsHolding(gobj) ? 0x4000 : 0,0);
    Fighter_AdvanceScript(gobj);
    t_frame[fd->ply]=frame;
}
static int RV_Branch(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int frame=t_frame[fd->ply];
    if(fd->flags.hitlag)return 0;
    switch(fd->state_id) {case 373:
if(frame>=15 && frame<=44 && (!(fd->input.held & 0x200))) { RV_Jump(gobj,373,45); return 1; }
if(frame>=47 && frame<=47 && (fd->phys.air_state)) { RV_Jump(gobj,381,1); return 1; }
break;
case 374:
if(frame>=15 && frame<=44 && (!(fd->input.held & 0x200))) { RV_Jump(gobj,374,45); return 1; }
if(frame>=47 && frame<=47 && (fd->phys.air_state)) { RV_Jump(gobj,382,1); return 1; }
break;
case 375:
if(frame>=48 && frame<=48 && (fd->phys.air_state)) { RV_Jump(gobj,385,1); return 1; }
if(frame>=48 && frame<=85 && (fd->phys.air_state)) { Leave(gobj,0,29.0f); return 1; }
break;
case 376:
if(frame>=48 && frame<=48 && (fd->phys.air_state)) { RV_Jump(gobj,386,1); return 1; }
if(frame>=48 && frame<=85 && (fd->phys.air_state)) { Leave(gobj,0,29.0f); return 1; }
break;
case 377:
if(frame>=47 && frame<=58 && (!fd->phys.air_state)) { RV_Jump(gobj,387,1); return 1; }
break;
case 378:
if(frame>=47 && frame<=58 && (!fd->phys.air_state)) { RV_Jump(gobj,388,1); return 1; }
break;
case 379:
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==3) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,389,1); return 1; }
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==2) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,391,1); return 1; }
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==1) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,393,1); return 1; }
break;
case 380:
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==3) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,390,1); return 1; }
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==2) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,392,1); return 1; }
if(frame>=9 && frame<=9 && ((LoxDownLevel(fd)==1) && (!((fd->input.held & 0x60))))) { RV_Jump(gobj,394,1); return 1; }
break;
case 381:
break;
case 382:
break;
case 383:
break;
case 384:
break;
case 385:
if(frame>=1 && frame<=16 && (!fd->phys.air_state)) { Fighter_EnterSpecialLanding(gobj,0,29.0f); return 1; }
break;
case 386:
if(frame>=1 && frame<=16 && (!fd->phys.air_state)) { Fighter_EnterSpecialLanding(gobj,0,29.0f); return 1; }
break;
case 387:
if(frame>=1 && frame<=4 && (fd->phys.air_state)) { Leave(gobj,1,29.0f); return 1; }
if(frame>=5 && frame<=48 && (fd->phys.air_state)) { Leave(gobj,1,29.0f); return 1; }
break;
case 388:
if(frame>=1 && frame<=4 && (fd->phys.air_state)) { Leave(gobj,1,29.0f); return 1; }
if(frame>=5 && frame<=48 && (fd->phys.air_state)) { Leave(gobj,1,29.0f); return 1; }
break;
case 389:
break;
case 390:
break;
case 391:
break;
case 392:
break;
case 393:
break;
case 394:
break;
case 395:
if(frame>=2 && frame<=44 && (fd->phys.air_state)) { RV_Jump(gobj,397,1); return 1; }
break;
case 396:
if(frame>=2 && frame<=44 && (fd->phys.air_state)) { RV_Jump(gobj,398,1); return 1; }
break;
case 397:
if(frame>=1 && frame<=33 && (!fd->phys.air_state)) { Leave(gobj,0,29.0f); return 1; }
break;
case 398:
if(frame>=1 && frame<=33 && (!fd->phys.air_state)) { Leave(gobj,0,29.0f); return 1; }
break;}
    return 0;
}
void RV_Anim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int frame;
    if(fd->flags.hitlag)return;
    frame=Tick(gobj);
    if(RV_Branch(gobj))return;
    switch(fd->state_id) {case 373: if(frame>=90) { Leave(gobj,0,29.0f); } break;
case 374: if(frame>=90) { Leave(gobj,0,29.0f); } break;
case 375: if(frame>=86) { Leave(gobj,0,29.0f); } break;
case 376: if(frame>=86) { Leave(gobj,0,29.0f); } break;
case 377: if(frame>=59) { RV_Jump(gobj,377,47); } break;
case 378: if(frame>=59) { RV_Jump(gobj,378,47); } break;
case 379: if(frame>=53) { Leave(gobj,0,29.0f); } break;
case 380: if(frame>=53) { Leave(gobj,0,29.0f); } break;
case 381: if(frame>=44) { Leave(gobj,0,29.0f); } break;
case 382: if(frame>=44) { Leave(gobj,0,29.0f); } break;
case 383: if(frame>=31) { RV_Jump(gobj,395,1); } break;
case 384: if(frame>=31) { RV_Jump(gobj,396,1); } break;
case 385: if(frame>=17) { RV_Jump(gobj,385,1); } break;
case 386: if(frame>=17) { RV_Jump(gobj,386,1); } break;
case 387: if(frame>=49) { Leave(gobj,1,29.0f); } break;
case 388: if(frame>=49) { Leave(gobj,1,29.0f); } break;
case 389: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 390: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 391: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 392: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 393: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 394: if(frame>=39) { Leave(gobj,0,29.0f); } break;
case 395: if(frame>=45) { Leave(gobj,0,29.0f); } break;
case 396: if(frame>=45) { Leave(gobj,0,29.0f); } break;
case 397: if(frame>=34) { Leave(gobj,0,29.0f); } break;
case 398: if(frame>=34) { Leave(gobj,0,29.0f); } break;}
}
void RV_IASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int frame=t_frame[fd->ply];
    if(fd->flags.hitlag || RV_Branch(gobj))return;
    switch(fd->state_id) {case 373: if(frame>=76) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 374: if(frame>=76) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 375: if(frame>=76) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 376: if(frame>=76) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 379: if(frame>=34) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 380: if(frame>=34) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 381: if(frame>=30) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 382: if(frame>=30) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 387: if(frame>=37) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); } break;
case 388: if(frame>=37) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); } break;
case 389: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 390: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 391: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 392: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 393: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 394: if(frame>=20) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 395: if(frame>=18) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 396: if(frame>=18) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 397: if(frame>=11) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;
case 398: if(frame>=11) { if(!fd->phys.air_state) Fighter_IASACheck_AllGrounded(gobj); else Fighter_IASACheck_AllAerial(gobj); } break;}
}
void RV_BasePhys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply], drift = 1, ground_owned = 0;
    float gravity = fd->attr.gravity, limit = fd->attr.terminal_velocity;
    float accel = fd->attr.aerial_drift_stick_mult, maxspeed = fd->attr.aerial_drift_max;
    float old_accel = accel, old_base = fd->attr.aerial_drift_base, old_max = maxspeed;
    if (fd->flags.hitlag) return;
    switch (fd->action_id) {
    case 297:
        if (frame >= 1 && frame < 34) { gravity = 0.009698075056076052f; }
        break;
    case 301:
        if (frame >= 1 && frame < 34) { gravity = 0.009698075056076052f; }
        break;
    case 303:
        if (frame >= 1 && frame < 17) { gravity = 0.0f; }
        if (frame >= 17 && frame < 38) { gravity = 0.0f; }
        if (frame >= 48 && frame < 86) { gravity = 0.0f; }
        break;
    case 304:
        break;
    case 305:
        break;
    case 306:
        if (frame >= 15 && frame < 45) { accel = 0.027154610157012943f; }
        break;
    case 307:
        if (frame >= 1 && frame < 31) { drift = 0; }
        break;
    case 308:
        if (frame >= 1 && frame < 31) { drift = 0; }
        break;
    case 309:
        break;
    case 310:
        break;
    case 311:
        break;
    case 312:
        if (frame >= 1 && frame < 17) { gravity = 0.0f; }
        if (frame >= 17 && frame < 38) { gravity = 0.0f; }
        if (frame >= 48 && frame < 86) { gravity = 0.0f; }
        break;
    case 313:
        break;
    case 314:
        break;
    case 315:
        if (frame >= 15 && frame < 45) { accel = 0.027154610157012943f; }
        break;
    case 316:
        break;
    case 317:
        break;
    case 318:
        break;
    case 319:
        break;
    case 320:
        break;
    case 321:
        if (frame >= 1 && frame < 24) { gravity = 0.0f; }
        if (frame >= 33 && frame < 47) { gravity = 0.0f; accel = 0.11637690067291259f; }
        if (frame >= 47 && frame < 59) { drift = 0; }
        break;
    case 322:
        if (frame >= 1 && frame < 24) { gravity = 0.0f; }
        if (frame >= 33 && frame < 47) { gravity = 0.0f; accel = 0.11637690067291259f; }
        if (frame >= 47 && frame < 59) { drift = 0; }
        break;
    case 323:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; }
        if (frame >= 9 && frame < 12) { gravity = 0.0f; }
        if (frame >= 12 && frame < 53) { gravity = 0.03879230022430421f; limit = 0.5818845033645631f; }
        break;
    case 324:
        if (frame >= 1 && frame < 2) { gravity = 0.0f; }
        if (frame >= 2 && frame < 45) { gravity = 0.009698075056076052f; }
        break;
    case 325:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; }
        if (frame >= 9 && frame < 12) { gravity = 0.0f; }
        if (frame >= 12 && frame < 53) { gravity = 0.03879230022430421f; limit = 0.5818845033645631f; }
        break;
    case 326:
        if (frame >= 1 && frame < 2) { gravity = 0.0f; }
        if (frame >= 2 && frame < 45) { gravity = 0.009698075056076052f; }
        break;
    }
    if (fd->phys.air_state)
    {
        Fighter_PhysAir_ApplyGravity(fd, gravity, limit);
        if (drift)
        {
            fd->attr.aerial_drift_stick_mult = accel;
            fd->attr.aerial_drift_base = 0;
            fd->attr.aerial_drift_max = maxspeed;
            Fighter_PhysAir_ApplyAerialDrift(fd);
            fd->attr.aerial_drift_stick_mult = old_accel;
            fd->attr.aerial_drift_base = old_base;
            fd->attr.aerial_drift_max = old_max;
        }
    }
    else Fighter_PhysGround_ApplyFriction(gobj);
    switch (fd->action_id) {
    case 297:
        if (frame >= 1 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 301:
        if (frame >= 1 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 303:
        if (frame >= 1 && frame <= 16) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 16) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 17 && frame <= 37) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 17 && frame <= 37) { if (fd->phys.air_state) fd->phys.self_vel.X = 2.4245187640190125f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 2.4245187640190125f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 38) { if (fd->phys.air_state) fd->phys.self_vel.X *= 0.1f; else { fd->phys.self_vel_ground.X *= 0.1f; ground_owned = 1; } }
        break;
    case 304:
        break;
    case 305:
        break;
    case 306:
        break;
    case 307:
        if (frame >= 1 && frame <= 30) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        break;
    case 308:
        if (frame >= 1 && frame <= 30) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        break;
    case 309:
        break;
    case 310:
        break;
    case 311:
        if (frame >= 1 && frame <= 4) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 4) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 312:
        if (frame >= 1 && frame <= 16) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 16) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 17 && frame <= 37) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 17 && frame <= 37) { if (fd->phys.air_state) fd->phys.self_vel.X = 2.4245187640190125f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 2.4245187640190125f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 38) { if (fd->phys.air_state) fd->phys.self_vel.X *= 0.1f; else { fd->phys.self_vel_ground.X *= 0.1f; ground_owned = 1; } }
        break;
    case 313:
        if (frame >= 1 && frame <= 4) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 4) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 314:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 3.491307020187378f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.93961501121521f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.93961501121521f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 315:
        break;
    case 316:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 3.491307020187378f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.93961501121521f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.93961501121521f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 317:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 2.7154610157012944f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.745653510093689f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.745653510093689f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 318:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 2.7154610157012944f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.745653510093689f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.745653510093689f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 319:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 1.93961501121521f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.551692008972168f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.551692008972168f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 320:
        if (frame == 1) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 1.93961501121521f; }
        if (frame == 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.551692008972168f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.551692008972168f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 321:
        if (frame >= 1 && frame <= 23) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 23) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 24) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 7.273556292057037f; }
        if (frame == 24) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 33 && frame <= 46) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame == 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 48 && frame <= 58) { if (fd->phys.air_state) fd->phys.self_vel.Y = -7.273556292057037f; }
        if (frame >= 48 && frame <= 58) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 322:
        if (frame >= 1 && frame <= 23) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 23) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 24) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 7.273556292057037f; }
        if (frame == 24) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 33 && frame <= 46) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame == 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 48 && frame <= 58) { if (fd->phys.air_state) fd->phys.self_vel.Y = -7.273556292057037f; }
        if (frame >= 48 && frame <= 58) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 323:
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 9) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 9 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 324:
        if (frame >= 1 && frame <= 1) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 2 && frame <= 44) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 325:
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 9) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 9 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 326:
        if (frame >= 1 && frame <= 1) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 1) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 2 && frame <= 44) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    }
    if (ground_owned && !fd->phys.air_state)
    {
        fd->phys.horzitonal_velocity_queue_will_be_added_to_0xec = 0;
        fd->phys.vertical_velocity_queue_will_be_added_to_0xec = 0;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
}

static void RV_Tilt(FighterData *fd,float angle)
{
    float stick=fd->input.lstick.X*fd->facing_direction,x,y;
    if(stick<0)stick=0;if(stick>1)stick=1;angle*=stick;
    x=fd->phys.self_vel.X*fd->facing_direction;y=fd->phys.self_vel.Y;
    fd->phys.self_vel.X=(x*cos(angle)-y*sin(angle))*fd->facing_direction;
    fd->phys.self_vel.Y=x*sin(angle)+y*cos(angle);
}
void RV_Phys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int frame=t_frame[fd->ply];
    if(fd->flags.hitlag)return;
    RV_BasePhys(gobj);
    if(fd->action_id==321 && (frame == 24)) RV_Tilt(fd,-0.3490658503988659f);
if(fd->action_id==322 && (frame == 24)) RV_Tilt(fd,-0.3490658503988659f);
}

static void RV_Landed(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int ledge=30000,helpless=0;
    switch(fd->state_id) {case 373: ledge=30000; helpless=0; break;
case 374: ledge=30000; helpless=0; break;
case 375: ledge=38; helpless=0; break;
case 376: ledge=38; helpless=0; break;
case 377: ledge=47; helpless=1; break;
case 378: ledge=47; helpless=1; break;
case 379: ledge=30000; helpless=0; break;
case 380: ledge=30000; helpless=0; break;
case 381: ledge=30000; helpless=0; break;
case 382: ledge=30000; helpless=0; break;
case 383: ledge=30000; helpless=0; break;
case 384: ledge=30000; helpless=0; break;
case 385: ledge=1; helpless=0; break;
case 386: ledge=1; helpless=0; break;
case 387: ledge=30000; helpless=1; break;
case 388: ledge=30000; helpless=1; break;
case 389: ledge=30000; helpless=0; break;
case 390: ledge=30000; helpless=0; break;
case 391: ledge=30000; helpless=0; break;
case 392: ledge=30000; helpless=0; break;
case 393: ledge=30000; helpless=0; break;
case 394: ledge=30000; helpless=0; break;
case 395: ledge=7; helpless=0; break;
case 396: ledge=7; helpless=0; break;
case 397: ledge=6; helpless=0; break;
case 398: ledge=6; helpless=0; break;}
    Fighter_SetGrounded2(fd);
    if(RV_Branch(gobj))return;
    if(helpless)Fighter_EnterSpecialLanding(gobj,0,29.0f);
    else Hand_Over(gobj,Other_Form(fd->state_id));
}
void RV_Coll(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int ledge=30000,helpless=0;
    switch(fd->state_id) {case 373: ledge=30000; helpless=0; break;
case 374: ledge=30000; helpless=0; break;
case 375: ledge=38; helpless=0; break;
case 376: ledge=38; helpless=0; break;
case 377: ledge=47; helpless=1; break;
case 378: ledge=47; helpless=1; break;
case 379: ledge=30000; helpless=0; break;
case 380: ledge=30000; helpless=0; break;
case 381: ledge=30000; helpless=0; break;
case 382: ledge=30000; helpless=0; break;
case 383: ledge=30000; helpless=0; break;
case 384: ledge=30000; helpless=0; break;
case 385: ledge=1; helpless=0; break;
case 386: ledge=1; helpless=0; break;
case 387: ledge=30000; helpless=1; break;
case 388: ledge=30000; helpless=1; break;
case 389: ledge=30000; helpless=0; break;
case 390: ledge=30000; helpless=0; break;
case 391: ledge=30000; helpless=0; break;
case 392: ledge=30000; helpless=0; break;
case 393: ledge=30000; helpless=0; break;
case 394: ledge=30000; helpless=0; break;
case 395: ledge=7; helpless=0; break;
case 396: ledge=7; helpless=0; break;
case 397: ledge=6; helpless=0; break;
case 398: ledge=6; helpless=0; break;}
    if(!fd->phys.air_state) {
        if(!Fighter_CollGround_PassLedge(gobj)) {
            Fighter_SetAirborne(fd);if(RV_Branch(gobj))return;Hand_Over(gobj,Other_Form(fd->state_id));
        }
    }
    else if(t_frame[fd->ply]>=ledge)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj,0,RV_Landed);
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))RV_Landed(gobj);
}

#define NG_PULL_OWNER ((void (*)(GOBJ *))0x800D9CE8)
#define NG_PULL_TARGET ((void (*)(GOBJ *,GOBJ *))0x800DAADC)
#define NG_HIT_NEW ((void (*)(FighterData *,ftHit *))0x800768A0)
#define NG_HIT_DAMAGE ((void (*)(ftHit *,int,GOBJ *))0x8007ABD0)
#define NG_HIT_ANGLE ((void (*)(ftHit *,int,GOBJ *))0x8007AC9C)
#define NG_HIT_PLACE ((void (*)(FighterData *,ftHit *))0x8007AD18)
#define NG_HIT_AFTER ((void (*)(FighterData *))0x80080484)
static int NativeGrab_Live(GOBJ *candidate)
{
    int p,s;
    if(!candidate)return 0;
    for(p=0;p<6;p++)for(s=0;s<2;s++)
        if(Fighter_GetSubcharGObj(p,s)==candidate)return 1;
    return 0;
}
static GOBJ *NativeGrab_Target(GOBJ *owner)
{
    FighterData *fd=owner->userdata,*target;
    GOBJ *other=fd->grab.attacker;
    if(!NativeGrab_Live(other))return 0;
    target=other->userdata;
    if(target->grab.attacker!=owner || target->flags.dead || target->flags.sleep)return 0;
    return other;
}
static void NativeGrab_Box(GOBJ *gobj,int slot,const float *b)
{
    FighterData *fd=gobj->userdata;
    ftHit *h=&fd->hitbox[slot];
    if(!h->active || h->x4)
    {h->x4=0;h->active=1;fd->flags.hitbox_active=1;NG_HIT_NEW(fd,h);}
    h->bone=fd->bones[0].joint;
    NG_HIT_DAMAGE(h,0,gobj);NG_HIT_ANGLE(h,0,gobj);
    h->offset.X=b[0];h->offset.Y=b[1];h->offset.Z=b[2];h->size=b[3];
    h->kb_growth=h->wdsk=h->kb=0;h->attribute=8;h->shield_dmg=0;
    h->hitsound_severity=h->hitsound_kind=0;h->x40=0x30;h->x41=0;
    *((u8 *)h+0x42)=0x45;*((u8 *)h+0x43)&=(u8)~0x20;
    NG_HIT_PLACE(fd,h);NG_HIT_AFTER(fd);
}
static void NativeGrab_Owner(GOBJ *gobj,int state,int anchor)
{
    FighterData *fd=gobj->userdata;
    // Common setup establishes ownership/attachment bookkeeping. Replace only
    // its pull animation with the adapter's hold, preserving throw bookkeeping.
    NG_PULL_OWNER(gobj);
    ActionStateChange(0,1,0,gobj,state,0x4000,0);
    fd->state_var.state_var7=(int)fd->bones[anchor].joint; // native capturedamage.x18
    Fighter_AdvanceScript(gobj);
    fd->phys.self_vel.X=fd->phys.self_vel.Y=fd->phys.self_vel_ground.X=0;
}
static void NativeGrab_Victim(GOBJ *victim,GOBJ *owner)
{
    FighterData *fd=victim->userdata;
    Fighter_SetAirborne(fd);
    NG_PULL_TARGET(victim,owner);
}
static int NativeGrab_Throw(GOBJ *owner,int damage,int angle,int growth,int base,int fixed)
{
    FighterData *fd=owner->userdata;
    GOBJ *victim=NativeGrab_Target(owner);
    ftHit zero={0},*h=&fd->throw_hitbox[0];
    if(!victim)return 0;
    *h=zero;NG_HIT_DAMAGE(h,damage,owner);NG_HIT_ANGLE(h,angle,owner);
    h->kb_growth=growth;h->kb=base;h->wdsk=fixed;
    Fighter_ThrownRelease_UpdateUnk(owner,victim);
    Fighter_ThrownApplyKnockback(victim,owner,0);
    return 1;
}
static void NativeGrab_Drop(GOBJ *owner)
{
    GOBJ *victim=NativeGrab_Target(owner);
    FighterData *target;
    if(!victim)return;
    target=victim->userdata;
    // An external hit/grab is authoritative; never replace its damage state.
    if(target->state_id!=ASID_CAPTUREPULLEDHI && target->state_id!=ASID_CAPTUREPULLEDLW)return;
    Fighter_BreakGrab(owner,victim);Fighter_EnterFall(victim);
}


typedef struct {
    int launch,hold,hold_ground,hold_air,release_ground,release_air,first,last,release,anchor,count;
    int damage,angle,growth,base;
    float balls[4][4];
} RGSpec;
typedef struct {int spec,active,holding,boxes;} RGState;
static RGState rg_state[6];
static int RG_IsHolding(GOBJ *gobj){FighterData *fd=gobj->userdata;return rg_state[fd->ply].holding;}
static const RGSpec rg_specs[2]={{375,383,383,384,395,396,17,38,1,12,2,12,270,18,45,{{-0.1665169796223936f,7.969635990227397f,9.31718629929857f,3.999999925656465f},{5.272164691332427f,0.2629626250060613f,8.601933315489715f,3.999999925656465f}}},
{376,384,383,384,395,396,17,38,1,12,2,12,270,18,45,{{-0.16648689866738717f,7.969473925082656f,9.317972468677603f,3.999999925656465f},{5.272371020528314f,0.2611903413413685f,8.600959683964836f,3.999999925656465f}}}};
static void RG_Caught(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;RGState *r=&rg_state[fd->ply];const RGSpec *s;
    if(!r->active || r->spec<0 || r->spec>=2)return;
    s=&rg_specs[r->spec];
    NativeGrab_Owner(gobj,s->hold,s->anchor);
    t_frame[fd->ply]=1;r->holding=1;r->boxes=0;
}
static int RG_Find(int state)
{
    int i;for(i=0;i<2;i++)if(rg_specs[i].launch==state)return i;
    return -1;
}
static void RG_Enter(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;RGState *r=&rg_state[fd->ply];int index=RG_Find(fd->state_id);
    if(r->holding)NativeGrab_Drop(gobj);
    r->spec=index;r->active=index>=0;r->holding=r->boxes=0;
    if(index>=0)Fighter_InitGrab(fd,1,RG_Caught,0,NativeGrab_Victim);
}
static void RG_Frame(FighterData *fd,GOBJ *gobj)
{
    RGState *r=&rg_state[fd->ply];const RGSpec *s;int index;
    if(!r->active)return;
    s=&rg_specs[r->spec];
    if(r->holding) {
        if(fd->flags.dead || fd->flags.sleep ||
           (fd->state_id!=s->hold_ground && fd->state_id!=s->hold_air &&
            fd->state_id!=s->release_ground && fd->state_id!=s->release_air)) {
            NativeGrab_Drop(gobj);r->active=r->holding=r->boxes=0;
        }
        else if(!NativeGrab_Target(gobj))r->holding=0;
    }
    else {
        index=RG_Find(fd->state_id);
        if(index>=0 && index!=r->spec)RG_Enter(gobj);
        else if(index<0)r->active=r->boxes=0;
    }
}
static void RG_Load(FighterData *fd){RGState zero={0};rg_state[fd->ply]=zero;rg_state[fd->ply].spec=-1;}
void RG_Anim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;RGState *r=&rg_state[fd->ply];const RGSpec *s;int frame,i;
    if(fd->flags.hitlag)return;
    RG_Frame(fd,gobj);RV_Anim(gobj);RG_Frame(fd,gobj);
    if(!r->active)return;
    s=&rg_specs[r->spec];frame=t_frame[fd->ply];
    if(r->holding) {
        if(frame>=s->release && (fd->state_id==s->release_ground || fd->state_id==s->release_air)) {
            NativeGrab_Throw(gobj,s->damage,s->angle,s->growth,s->base,0);
            r->holding=0;
        }
    }
    else if(fd->state_id==s->launch) {
        if(frame>=s->first && frame<s->last) {
            for(i=0;i<s->count;i++)NativeGrab_Box(gobj,i,s->balls[i]);
            r->boxes=1;
        }
        else if(r->boxes){Fighter_HitboxDisableAll(gobj);r->boxes=0;}
    }
}
void RG_IASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(!rg_state[fd->ply].holding)RV_IASA(gobj);
}
void RG_Phys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    RV_Phys(gobj);
    if(rg_state[fd->ply].holding)
        fd->phys.self_vel.X=fd->phys.self_vel.Y=fd->phys.self_vel_ground.X=0;
}
void RG_Coll(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(!rg_state[fd->ply].holding)RV_Coll(gobj);
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

typedef struct {int value,limit;} PortResource;
static void Port_ResourceReset(PortResource *r,int limit){r->value=0;r->limit=limit<0 ? 0 : limit;}
static int Port_ResourceAdd(PortResource *r,int amount)
{
    if(amount<0)amount=0;
    if(amount>r->limit-r->value)amount=r->limit-r->value;
    r->value+=amount;return amount;
}
static int Port_ResourceSpend(PortResource *r,int amount)
{
    if(amount<0 || amount>r->value)return 0;
    r->value-=amount;return 1;
}

typedef struct {int ready,age,pool,expired,bounces;float charge;} LoxBoulder;
static PortResource lox_magma[6];
static int lox_down[6],lox_charge[6],lox_cast[6],lox_last_action[6],lox_last_instance[6],lox_powered[6],lox_burst[6];
static int LoxDownLevel(FighterData *fd){return lox_down[fd->ply];}
static void LoxLoad(FighterData *fd)
{
    ItemDesc **items=fd->ftData->items;int i,p=fd->ply;
    Port_ResourceReset(&lox_magma[p],3);lox_down[p]=lox_charge[p]=lox_cast[p]=lox_powered[p]=lox_burst[p]=0;
    lox_last_action[p]=-1;lox_last_instance[p]=-1;
    for(i=0;items && i<6;i++)if(items[i])MEX_IndexFighterItem(fd->kind,items[i],i);
}
static void LoxEnter(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int p=fd->ply;
    if(fd->state_id==373 || fd->state_id==374){lox_charge[p]=0;lox_cast[p]=0;}
    if(fd->state_id==379 || fd->state_id==380)
    {
        lox_down[p]=lox_magma[p].value;
        if(fd->input.held&0x60){Port_ResourceSpend(&lox_magma[p],lox_down[p]>0 ? 1 : 0);lox_down[p]=0;}
        else Port_ResourceSpend(&lox_magma[p],lox_down[p]);
    }
}
static GOBJ *LoxPool(GOBJ *owner)
{
    GOBJ *g;ItemData *id;int kind=MEX_GetFtItemID(owner,1);
    for(g=(*stc_gobj_lookup)[MATCHPLINK_ITEM];g;g=g->next)
    {
        id=g->userdata;
        if(id && id->kind==kind && id->fighter_gobj==owner && !id->item_var.var8)return g;
    }
    return 0;
}
static void LoxMakePool(GOBJ *owner,Vec3 at,int hot)
{
    GOBJ *g=LoxPool(owner);ItemData *id;
    if(g)((ItemData *)g->userdata)->item_var.var8=1;
    at.Y+=0.4f;at.Z=0;g=Make_Item(owner,1,&at);
    if(g){id=g->userdata;id->item_var.var1=1;id->item_var.var2=hot ? 90 : 0;}
}
static void LoxPoolFrame(FighterData *fd,GOBJ *owner)
{
    GOBJ *g,*pool,*other;ItemData *id;FighterData *target;LoxBoulder *b;Vec3 at;
    int near_owner=0,stunned=fd->state_id>=75 && fd->state_id<=91;
    for(g=(*stc_gobj_lookup)[MATCHPLINK_ITEM];g;g=g->next)
    {
        id=g->userdata;
        if(!id || id->fighter_gobj!=owner || id->kind!=MEX_GetFtItemID(owner,0))continue;
        b=(LoxBoulder *)&id->item_var;
        if(b->pool && !b->expired){b->expired=1;at=id->pos;at.Y-=5.4985941818613835f;LoxMakePool(owner,at,0);}
    }
    pool=LoxPool(owner);if(!pool)return;id=pool->userdata;
    near_owner=!fd->phys.air_state && fabsf(fd->phys.pos.X-id->pos.X)<12.0f && fabsf(fd->phys.pos.Y-id->pos.Y)<6.0f;
    if(near_owner)
    {
        if(stunned || (fd->state_id>=183 && fd->state_id<=198)){id->item_var.var8=1;return;}
        if(id->item_var.var2>=90 && lox_magma[fd->ply].value<3)
        {Port_ResourceAdd(&lox_magma[fd->ply],1);id->item_var.var8=1;}
        return;
    }
    for(other=(*stc_gobj_lookup)[MATCHPLINK_FIGHTER];other;other=other->next)
    {
        if(other==owner || !other->userdata)continue;target=other->userdata;
        if(!target->phys.air_state && fabsf(target->phys.pos.X-id->pos.X)<12.0f && fabsf(target->phys.pos.Y-id->pos.Y)<6.0f)
        {id->item_var.var8=1;return;}
    }
}
static void LoxShoot(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;Vec3 at=fd->phys.pos;GOBJ *g;ItemData *id;LoxBoulder *b;
    float q=(float)lox_charge[fd->ply]/30.0f,angle=1.10714872f-fd->input.lstick.X*fd->facing_direction*0.35f;
    float speed=3.3541019662496847f*(1.0f+q*0.6f);
    at.X+=12.0f*fd->facing_direction;at.Y+=0.0f;at.Z=0;
    if(!fd->phys.air_state && at.Y-fd->phys.pos.Y<5.4985941818613835f+0.2f)at.Y=fd->phys.pos.Y+5.4985941818613835f+0.2f;
    g=Make_Item(gobj,0,&at);if(!g)return;id=g->userdata;b=(LoxBoulder *)&id->item_var;
    b->ready=1;b->charge=q;
    id->self_vel.X=cos(angle)*speed*fd->facing_direction;id->self_vel.Y=sin(angle)*speed;
}
void LoxPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int p=fd->ply,frame=t_frame[p];
    if(!fd->flags.hitlag)
    {
        if((fd->state_id==373 || fd->state_id==374) && frame>=15 && frame<45 && (fd->input.held&B_BUTTON) && lox_charge[p]<30.0f)lox_charge[p]++;
        if(!lox_cast[p] && ((fd->state_id==373 && frame==47) || (fd->state_id==374 && frame==47) || (fd->state_id==381 && frame==1) || (fd->state_id==382 && frame==1))){lox_cast[p]=1;LoxShoot(gobj);}
        if(fd->state_id==395 && frame==1 && !fd->phys.air_state)LoxMakePool(gobj,fd->phys.pos,1);
if(fd->state_id==396 && frame==1 && !fd->phys.air_state)LoxMakePool(gobj,fd->phys.pos,1);
        if((fd->state_id==389 || fd->state_id==390) && frame==1)fd->jump.jumps_used=1;
    }
    RG_Phys(gobj);
}
static void LoxNormalFrame(FighterData *fd,GOBJ *gobj)
{
    int p=fd->ply,action=fd->action_id,frame=(int)fd->state.frame;Vec3 at;GOBJ *g;
    if(fd->flags.hitlag)return;
    if(fd->state_id<ASID_WAIT){Port_ResourceReset(&lox_magma[p],3);lox_powered[p]=lox_burst[p]=0;return;}
    LoxPoolFrame(fd,gobj);
    if(action!=lox_last_action[p] || fd->atk_instance!=lox_last_instance[p])
    {
        lox_powered[p]=0;lox_burst[p]=0;
        if((action==62 || action==66 || action==67) && lox_magma[p].value>0)
        {lox_powered[p]=1;Port_ResourceSpend(&lox_magma[p],1);}
    }
    lox_last_action[p]=action;lox_last_instance[p]=fd->atk_instance;
    if(!lox_powered[p])return;
    if(action==62 && frame>=33 && !(lox_burst[p]&1)) { lox_burst[p]|=1;at=fd->phys.pos;at.X+=14.0f*fd->facing_direction;at.Y+=5.0f;g=Make_Item(gobj,2,&at);if(g)((ItemData *)g->userdata)->item_var.var1=1; }
if(action==66 && frame>=19 && !(lox_burst[p]&1)) { lox_burst[p]|=1;at=fd->phys.pos;at.X+=0.0f*fd->facing_direction;at.Y+=14.0f;g=Make_Item(gobj,3,&at);if(g)((ItemData *)g->userdata)->item_var.var1=1; }
if(action==67 && frame>=28 && !(lox_burst[p]&1)) { lox_burst[p]|=1;at=fd->phys.pos;at.X+=14.0f*fd->facing_direction;at.Y+=5.0f;g=Make_Item(gobj,4,&at);if(g)((ItemData *)g->userdata)->item_var.var1=1; }
if(action==67 && frame>=28 && !(lox_burst[p]&2)) { lox_burst[p]|=2;at=fd->phys.pos;at.X+=-14.0f*fd->facing_direction;at.Y+=5.0f;g=Make_Item(gobj,5,&at);if(g)((ItemData *)g->userdata)->item_var.var1=1; }
}
static void LoxMeter(FighterData *fd,int *value,int *mode){*value=lox_magma[fd->ply].value;*mode=0;}

static void LoxMeter(FighterData *,int *,int *);
typedef struct {Text *text; GOBJ *owner; int kind,sub,value,mode;} PortMeter;
static PortMeter port_meter[6];
static void Meter_Destroy(void *data)
{
    int i;
    for (i=0;i<6;i++)
        if (port_meter[i].text == data)
        {
            port_meter[i].text = 0;
            port_meter[i].owner = 0;
        }
    // Preserve the text library's native userdata destructor and linked-list removal.
    Text_DestroyAllAlloc((Text *)data);
}
static void Meter_Update(GOBJ *gobj)
{
    Text *text = gobj->userdata;
    int i,value,mode;
    for (i=0;i<6;i++)
    {
        PortMeter *m = &port_meter[i];
        FighterData *fd;
        Vec3 *pos;
        if (m->text != text) continue;
        if (!m->owner || Fighter_GetGObj(i) != m->owner ||
            ((FighterData *)m->owner->userdata)->kind != m->kind)
        {
            Text_Destroy(text);
            return;
        }
        fd = m->owner->userdata;
        text->hidden = fd->flags.sleep || stc_matchhud->element_data[i].is_hidden;
        pos = Match_GetPlayerHUDPos(i);
        // SIS glyphs use a downward Y axis. Position the viewport in HUD units;
        // scaling individual subtexts leaves the default 32-unit baseline unscaled.
        text->trans.X = pos->X+3.0f;
        text->trans.Y = -pos->Y-8.0f;
        LoxMeter(fd,&value,&mode);
        if (value != m->value)
        {
            Text_SetText(text,m->sub,"Magma %d",value);
            m->value = value;
        }
        if (mode != m->mode)
        {
            GXColor white = {255,255,255,255};
            GXColor buy = {112,255,144,255};
            GXColor sell = {255,216,96,255};
            Text_SetColor(text,m->sub,mode == 1 ? &buy : mode == 2 ? &sell : &white);
            m->mode = mode;
        }
        return;
    }
}
static int Meter_Canvas(void)
{
    TextCanvas *c;
    int index = 0;
    // Share an existing match-HUD canvas, including one made by another port module.
    // No canvas or font is destroyed by a fighter; the scene owns those shared objects.
    for (c=*stc_textcanvas_first;c;c=c->next)
        if (c->sis_idx == 2)
        {
            if (!c->cam_gobj && c->p_link == 15 && c->gx_link == 11 && c->gx_pri == 0)
                return index;
            index++;
        }
    return Text_CreateCanvas(2,(int)stc_matchhud->cam_gobj,14,15,0,11,0,19);
}
static void Meter_Load(FighterData *fd)
{
    if (port_meter[fd->ply].text)
        Text_Destroy(port_meter[fd->ply].text);
}
static void Meter_Frame(FighterData *fd,GOBJ *owner)
{
    PortMeter *m;
    Text *text;
    if (fd->ply < 0 || fd->ply >= 6 || Fighter_GetGObj(fd->ply) != owner) return;
    m = &port_meter[fd->ply];
    if (m->text || !stc_matchhud->cam_gobj || !stc_sis_data[2]) return;
    text = Text_CreateText(2,Meter_Canvas());
    m->text = text; m->owner = owner; m->kind = fd->kind;
    m->value = -2147483647; m->mode = -1;
    text->align = 1; text->kerning = 1;
    m->sub = Text_AddSubtext(text,0,0,"Magma %d",0);
    text->viewport_scale.X = text->viewport_scale.Y = 0.05f;
    // Text_CreateText already installed its userdata; replace only its destructor,
    // then delegate that exact native cleanup in Meter_Destroy.
    text->gobj->destructor_function = (int)Meter_Destroy;
    GObj_AddProc(text->gobj,Meter_Update,17);
    Meter_Update(text->gobj);
}


typedef struct {int action,state,first,end,target,neutral;} JabWindow;
static const JabWindow jab_windows[] = {
    {46, 44, 14, 36, 45, 0},
    {47, 45, 1, 1, 46, 0},
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


// Installed after this fighter's existing IASA process, before the engine's physics priority.
// HSD_GObjProc_8038FAA8 inserts a new same-priority process after this GObj's existing one.
typedef struct {int action,first,last,x_kind,y_kind; float x,y;} MomentumEvent;
static const MomentumEvent momentum_events[] = {
    {52, 1, 15, 2, 0, 1.2607497572898865f, 0.0f},
    {62, 21, 27, 1, 0, 1.745653510093689f, 0.0f},
};
static u8 momentum_registered[6];
static int momentum_state[6],momentum_action[6];
static float momentum_frame[6];
static void (*momentum_original[6])(GOBJ *);
static void Momentum_Load(FighterData *fd)
{
    int p = fd->ply;
    momentum_registered[p] = 0;
    momentum_state[p] = -1;
    momentum_action[p] = -1;
    momentum_frame[p] = 0;
    momentum_original[p] = 0;
}
static void Momentum_Change(float *speed, int kind, float amount, float facing)
{
    if (kind == 1)
        *speed = amount * facing;
    else if (kind == 2)
        *speed += amount * facing;
    else if (kind == 3)
        *speed *= amount;
    else if (kind == 4)
        *speed = amount * facing;
}
static void Momentum_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, state = fd->state_id, action = fd->action_id, i, grounded;
    float now = fd->state.frame, before = momentum_frame[p];
    if (fd->flags.hitlag)
        return;
    grounded = fd->phys.air_state == 0;
    // Set/add/multiply are entry impulses. Constant velocity is enforced after ordinary friction/gravity.
    for (i = 0; i < sizeof(momentum_events) / sizeof(momentum_events[0]); i++)
    {
        const MomentumEvent *e = &momentum_events[i];
        if (e->action != action || !(before < e->first && now >= e->first))
            continue;
        if (e->x_kind && e->x_kind != 4)
            Momentum_Change(grounded ? &fd->phys.self_vel_ground.X : &fd->phys.self_vel.X,
                            e->x_kind, e->x, fd->facing_direction);
        if (!grounded && e->y_kind && e->y_kind != 4)
            Momentum_Change(&fd->phys.self_vel.Y, e->y_kind, e->y, 1.0f);
    }
    if (momentum_original[p])
        momentum_original[p](gobj);
    if (fd->state_id != state || fd->action_id != action)
        return;
    for (i = 0; i < sizeof(momentum_events) / sizeof(momentum_events[0]); i++)
    {
        const MomentumEvent *e = &momentum_events[i];
        if (e->action != action || now < e->first || now >= e->last + 1.0f)
            continue;
        if (e->x_kind == 4)
        {
            Momentum_Change(grounded ? &fd->phys.self_vel_ground.X : &fd->phys.self_vel.X,
                            4, e->x, fd->facing_direction);
            if (grounded)
            {
                // Ground friction is queued at +0xE4, then added to velocity after this callback.
                // A constant owns this axis; retain independent knockback, but remove control acceleration.
                fd->phys.horzitonal_velocity_queue_will_be_added_to_0xec = 0;
                fd->phys.vertical_velocity_queue_will_be_added_to_0xec = 0;
                Fighter_PhysGround_ApplyVelocity(gobj);
            }
        }
        if (!grounded && e->y_kind == 4)
            Momentum_Change(&fd->phys.self_vel.Y, 4, e->y, 1.0f);
    }
    momentum_frame[p] = now;
}
static void Momentum_AfterInput(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, i, carries = 0;
    if (fd->flags.hitlag)
        return;
    if (fd->state_id != momentum_state[p] || fd->action_id != momentum_action[p] ||
        fd->state.frame < momentum_frame[p])
        momentum_frame[p] = 0;
    momentum_state[p] = fd->state_id;
    momentum_action[p] = fd->action_id;
    for (i = 0; i < sizeof(momentum_events) / sizeof(momentum_events[0]); i++)
        if (momentum_events[i].action == fd->action_id)
            carries = 1;
    if (carries)
    {
        if (fd->cb.Phys != Momentum_Phys)
        {
            momentum_original[p] = fd->cb.Phys;
            fd->cb.Phys = Momentum_Phys;
        }
    }
    else
    {
        if (fd->cb.Phys == Momentum_Phys)
            fd->cb.Phys = momentum_original[p];
        momentum_frame[p] = fd->state.frame;
    }
}
static void Momentum_Register(FighterData *fd, GOBJ *gobj)
{
    if (!momentum_registered[fd->ply])
    {
        // Registration happens in the first OnFrame, after the engine installed all fighter processes.
        GObj_AddProc(gobj, Momentum_AfterInput, FTPRI_IASA);
        momentum_registered[fd->ply] = 1;
    }
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
        RG_IASA(gobj);
}

void Reverse_IASA_374(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_375(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_376(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_377(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_379(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void Reverse_IASA_380(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RG_IASA(gobj);
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    RG_Load(fd);
    LoxLoad(fd);
    Meter_Load(fd);
    Jab_Load(fd);
    Momentum_Load(fd);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    RG_Frame(fd,gobj);
    LoxNormalFrame(fd,gobj);
    Meter_Frame(fd,gobj);
    Jab_Frame(fd);
    Momentum_Register(fd, gobj);
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
    {306, 0x340211, 0x12, 0x0, RG_Anim, Reverse_IASA_373, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 373: the neutral special (plain)
    {315, 0x340611, 0x12, 0x0, RG_Anim, Reverse_IASA_374, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 374: the neutral special in the air (plain)
    {303, 0x340212, 0x13, 0x0, RG_Anim, Reverse_IASA_375, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 375: the side special (plain)
    {312, 0x340612, 0x13, 0x0, RG_Anim, Reverse_IASA_376, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 376: the side special in the air (plain)
    {321, 0x340213, 0x14, 0x0, RG_Anim, Reverse_IASA_377, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 377: the up special (plain)
    {322, 0x340613, 0x14, 0x0, RG_Anim, Reverse_IASA_378, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 378: the up special in the air (plain)
    {323, 0x340214, 0x15, 0x0, RG_Anim, Reverse_IASA_379, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 379: the down special (plain)
    {325, 0x340614, 0x15, 0x0, RG_Anim, Reverse_IASA_380, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 380: the down special in the air (plain)
    {304, 0x340211, 0x12, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 381: neutral: Active Air
    {305, 0x340611, 0x12, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 382: neutral: Active Air
    {307, 0x340212, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 383: side: Grabbed Opponent
    {308, 0x340612, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 384: side: Grabbed Opponent
    {309, 0x340212, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 385: side: Looping Special Fall
    {310, 0x340612, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 386: side: Looping Special Fall
    {311, 0x340213, 0x14, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 387: up: Landing Active
    {313, 0x340613, 0x14, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 388: up: Landing Active
    {314, 0x340214, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 389: down: level 3 active
    {316, 0x340614, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 390: down: level 3 active
    {317, 0x340214, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 391: down: level 2 active
    {318, 0x340614, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 392: down: level 2 active
    {319, 0x340214, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 393: down: level 1 active
    {320, 0x340614, 0x15, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 394: down: level 1 active
    {324, 0x340212, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 395: side: Slam Active
    {326, 0x340612, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 396: side: Slam Active
    {297, 0x340212, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 397: side: Slam Recovery (Air)
    {301, 0x340612, 0x13, 0x0, RG_Anim, RG_IASA, LoxPhys, RG_Coll, Fighter_UpdateCameraBox},    // 398: side: Slam Recovery (Air)
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
