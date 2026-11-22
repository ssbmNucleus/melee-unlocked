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
static void PM_Enter(GOBJ *gobj);
static void PM_Load(FighterData *fd);
static void Enter(GOBJ *gobj, int state)
{
    FighterData *fd = gobj->userdata;
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
    t_frame[fd->ply] = 1;
    PM_Enter(gobj);
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
#define N_END 45                   // the natural end, on the ground (NEVER: at the clip's end),
#define N_END_AIR 45           //   and in the air
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
        if (t_frame[fd->ply] >= 30000)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 30000)
        Fighter_IASACheck_AllAerial(gobj);
}

// ---- the side special: PLAIN ------------------------------------------------------------------------
#define ST_S 375
#define ST_S_AIR 376
#define S_END 51                   // the natural end, on the ground,
#define S_END_AIR 51           //   and in the air
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
        if (t_frame[fd->ply] >= 30000)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 30000)
        Fighter_IASACheck_AllAerial(gobj);
}

// ---- the up special: PLAIN (he does not rise: he is helpless after it in the air) -------------------
#define ST_HI 377
#define ST_HI_AIR 378
#define HI_END 51                 // the natural end, on the ground,
#define HI_END_AIR 51         //   and in the air
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
#define LW_END 41                 // the natural end, on the ground,
#define LW_END_AIR 41         //   and in the air
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
        if (t_frame[fd->ply] >= 30000)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 30000)
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
    PM_Load(fd);
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
    return state;                       // (a state not listed: a mistake; it stays the state it is)
}

static int PM_HasDue(GOBJ *gobj,int frame);
void PM_Anim(GOBJ *gobj)
{
FighterData *fd=gobj->userdata;int frame;
if(fd->flags.hitlag)return;frame=Tick(gobj);
if(PM_HasDue(gobj,frame))return;
switch(fd->state_id) {
case 373: if(frame>=45) { Leave(gobj,0,29.0f); } break;
case 374: if(frame>=45) { Leave(gobj,0,29.0f); } break;
case 375: if(frame>=51) { Leave(gobj,0,29.0f); } break;
case 376: if(frame>=51) { Leave(gobj,0,29.0f); } break;
case 377: if(frame>=51) { Leave(gobj,1,29.0f); } break;
case 378: if(frame>=51) { Leave(gobj,1,29.0f); } break;
case 379: if(frame>=41) { Leave(gobj,0,29.0f); } break;
case 380: if(frame>=41) { Leave(gobj,0,29.0f); } break;
}
}

static float PMAbs(float x){return x < 0 ? -x : x;}
static float PMMin(float a,float b){return a < b ? a : b;}
static float PMMax(float a,float b){return a > b ? a : b;}
static float PMClamp(float x,float lo,float hi){return x < lo ? lo : x > hi ? hi : x;}
static float PMLerp(float a,float b,float t){return a+(b-a)*PMClamp(t,0,1);}
static float PMDiv(float a,float b){return b==0 ? 0 : a/b;}
static float PMMoveTo(float a,float b,float step){return a+PMClamp(b-a,-PMMax(0,step),PMMax(0,step));}

static int PM_InsideSegment(float x,float y,int mask)
{
    float ax=PMAbs(x),ay=PMAbs(y);int region;
    if((mask&1)&&ax<0.01f&&ay<0.01f)return 1;
    if((mask&0x20000)&&ax<0.1f&&ay<0.1f)return 1;
    if(ay>ax)region=y>0 ? (ay>=0.5f?4:2) : (ay>=0.5f?16:8);
    else if(ax>0)region=x>0 ? (ax>=0.5f?64:32) : (ax>=0.5f?256:128);
    else return 0;
    return (mask&region)!=0;
}
static int PM_InputSegment(FighterData *fd,int segment,int comparison,float facing)
{
    int now=PM_InsideSegment(fd->input.lstick.X*facing,fd->input.lstick.Y,segment);
    int prev=PM_InsideSegment(fd->input.lstick_prev.X*facing,fd->input.lstick_prev.Y,segment);
    int status=now ? 2 : 1;
    if(now&&!prev)status|=4;
    if(!now&&prev)status|=8;
    return (status&comparison)!=0;
}


typedef struct {
    int ready, age, hits, bounces, contact;
    float distance, last_x, last_y, gx, gy, travel, return_gain;
} PortProjectile;
typedef char PortProjectileFits[(sizeof(PortProjectile)<=sizeof(((ItemData *)0)->item_var))?1:-1];
static void PP_Orient(GOBJ *g)
{
    ItemData *id=g->userdata;JOBJ *model=g->hsd_object;
    // own_items already converts the mesh into world-facing item axes. Undo
    // the donor's extra quarter turn after creation and native state changes.
    model->rot.Y=id->facing_direction<0 ? 3.14159265f : 0;
    JOBJ_SetMtxDirtySub(model);
}

static void PP_Spawn(GOBJ *gobj,int which,int bone,
    float lx,float ly,float lz,float wx,float wy,float wz,float vx,float vy,float gx,float gy)
{
    FighterData *fd=gobj->userdata;GOBJ *item;ItemData *id;PortProjectile *p;
    SpawnItem spawn={0};Vec3 local={lx,ly,lz},at;
    JOBJ_GetWorldPosition(fd->bones[bone].joint,&local,&at);
    at.X+=wx*fd->facing_direction;at.Y+=wy;at.Z=0;
    spawn.parent_gobj=gobj;spawn.parent_gobj2=gobj;
    spawn.it_kind=MEX_GetFtItemID(gobj,which);
    spawn.pos=at;spawn.pos2=at;spawn.vel.X=spawn.vel.Y=spawn.vel.Z=0;
    spawn.facing_direction=fd->facing_direction;
    spawn.damage=0;spawn.unk6=0;spawn.is_raycast_below=1;
    item=Item_CreateItem1(&spawn);if(!item)return;
    ItemStateChange(item,0,2);Item_CopyDevelopState(item,gobj);
    id=item->userdata;p=(PortProjectile *)&id->item_var;
    p->ready=1;p->age=p->hits=p->bounces=p->contact=0;
    p->distance=0;p->last_x=at.X;p->last_y=at.Y;p->gx=gx;p->gy=gy;
    p->travel=0;p->return_gain=1;
    id->self_vel.X=vx;id->self_vel.Y=vy;
    PP_Orient(item);
}

#define PM_LENGTH 4.568476788841445f
#define PM_SPEED (PM_LENGTH / 60.0f)
#define PM_ACCEL (PM_LENGTH / 3600.0f)
#define PM_JOBS 50
#define PM_HOOKS 11
#define PM_CONTACT_HOOKS 2
#define PM_TRIGGERS 2
typedef struct {int job, live; float at;} PMEvent;
typedef struct {int job,live,tag;float born,until;} PMHook;
typedef struct {int job,shield;} PMHit;
typedef struct {int job,live,order;float until;} PMTrigger;
typedef struct {
    float gravity,fall,drift,accel,speed,x,y,ground,facing,ledge,data[11];
    int count,helpless; float landing;
    int state,hit_count,pending_count,input_mask,next,trigger_order,jump_dirty;
    float jumps;
    float hit_shield,input_until;
    int hit_jobs[PM_CONTACT_HOOKS];
    PMHit pending[PM_CONTACT_HOOKS*12];
    PMEvent events[PM_JOBS];
    PMHook hooks[PM_HOOKS];
    PMTrigger triggers[PM_TRIGGERS];
} PortMotion;
static PortMotion port_motion[6];
static int PM_Form(PortMotion *r,int state,int air)
{
    // A hard transition can enter a source program whose usual input uses a
    // different aerial program. Preserve this program when landing/leaving ledges.
    switch(r->state) { case 375: return air ? 376 : 375; case 376: return air ? 376 : 375; case 377: return air ? 378 : 377; case 378: return air ? 378 : 377; case 379: return air ? 380 : 379; case 380: return air ? 380 : 379; }
    return Other_Form(state);
}
static void PM_Arm(PortMotion *r,int slot,int job,float duration,float now)
{
    PMTrigger *t=&r->triggers[slot];
    t->job=job;t->live=1;t->order=++r->trigger_order;
    t->until=duration>0 ? now+duration : 0;
}
static void PM_BindHit(PortMotion *r,int job)
{
    if(r->hit_count<PM_CONTACT_HOOKS)r->hit_jobs[r->hit_count++]=job;
}
static void PM_OnHit(GOBJ *owner,GOBJ *other,float damage,int shield)
{
    FighterData *fd=owner->userdata;PortMotion *r=&port_motion[fd->ply];int i;
    if(fd->state_id!=r->state && fd->state_id!=PM_Form(r,r->state,fd->phys.air_state))return;
    for(i=0;i<r->hit_count && r->pending_count<PM_CONTACT_HOOKS*12;i++) {
        r->pending[r->pending_count].job=r->hit_jobs[i];
        r->pending[r->pending_count++].shield=shield;
    }
}
static void PM_NoHurt(GOBJ *owner,GOBJ *other,float damage,int shield){}
#ifndef PM_NATIVE_CHECK
// Melee GALE01 common IASA routines, from its symbol map and ftCo_Wait_IASA.
#define PM_NATIVE_CHECK(address,gobj) (((int (*)(GOBJ *))(address))(gobj))
#endif
static int PM_InputSpecial(GOBJ *gobj,int mask)
{
    FighterData *fd=gobj->userdata;int direction=0,state;
    const int states[8]={373,374,377,378,379,380,375,376};
    if(!(fd->input.down&0x200))return 0;
    if(fd->input.lstick.Y>=.5f)direction=1;
    else if(fd->input.lstick.Y<=-.5f)direction=2;
    else if(PMAbs(fd->input.lstick.X)>=.5f)direction=3;
    if(!(mask&(4<<direction)))return 0;
    state=states[direction*2+(fd->phys.air_state ? 1 : 0)];
    if(!state)return 0;
    Enter(gobj,state);
    return 1;
}
static int PM_InputCancel(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;PortMotion *r=&port_motion[fd->ply];int state=fd->state_id;
    if(fd->flags.hitlag || (r->helpless && fd->phys.air_state))return 0;
    if(r->input_until>0 && t_frame[fd->ply]>=r->input_until)r->input_mask=0;
    if(r->input_mask&2) {
        if(fd->phys.air_state)Fighter_IASACheck_AllAerial(gobj);
        else Fighter_IASACheck_AllGrounded(gobj);
    }
    else {
        int mask=r->input_mask;
        if(PM_InputSpecial(gobj,mask))return 1;
        if(!fd->phys.air_state) {
            if((mask&32768)&&PM_NATIVE_CHECK(0x800D8990,gobj))return 1; // grab
            if((mask&4096)&&PM_NATIVE_CHECK(0x8008BFC4,gobj))return 1; // forward smash
            if((mask&1024)&&PM_NATIVE_CHECK(0x8008C830,gobj))return 1; // up smash
            if((mask&2048)&&PM_NATIVE_CHECK(0x8008CB44,gobj))return 1; // down smash
            if((mask&512)&&PM_NATIVE_CHECK(0x8008B658,gobj))return 1; // forward tilt
            if((mask&128)&&PM_NATIVE_CHECK(0x8008B980,gobj))return 1; // up tilt
            if((mask&256)&&PM_NATIVE_CHECK(0x8008BB44,gobj))return 1; // down tilt
            if((mask&64)&&PM_NATIVE_CHECK(0x8008A9F8,gobj))return 1; // jab
            if((mask&16384)&&PM_NATIVE_CHECK(0x80099794,gobj))return 1; // spot/roll dodge
            if((mask&8192)&&PM_NATIVE_CHECK(0x80091A4C,gobj))return 1; // shield
            if(mask&1)Fighter_IASACheck_JumpF(gobj);
        }
        else {
            if(mask&65536)Fighter_IASACheck_AirAttack(gobj);
            if(fd->state_id!=state)return 1;
            if(mask&1)Fighter_IASACheck_JumpAerial(gobj);
            if(fd->state_id!=state)return 1;
            if(mask&16384)Fighter_IASACheck_AirEscape(gobj);
        }
    }
    return fd->state_id!=state;
}
static void PM_Listen(PortMotion *r,int job,int tag,float duration,float now)
{
    int i;for(i=0;i<PM_HOOKS;i++)if(!r->hooks[i].live) {
        r->hooks[i].job=job;r->hooks[i].tag=tag;r->hooks[i].born=now;
        r->hooks[i].until=duration<=0 ? 0 : now+duration;r->hooks[i].live=1;return;
    }
}
static void PM_Unlisten(PortMotion *r,int index,int tag)
{
    int i;if(index<0)return;
    for(i=0;i<PM_HOOKS;i++)if(r->hooks[i].live&&r->hooks[i].tag==tag)
        if(index--==0){r->hooks[i].live=0;return;}
}
static void PM_Schedule(PortMotion *r,int job,float at)
{
    if(r->count < PM_JOBS) {
        r->events[r->count].job=job; r->events[r->count].at=at;
        r->events[r->count].live=1; r->count++;
    }
}
static void PM_Run(GOBJ *gobj,int job,float now)
{
    FighterData *fd=gobj->userdata;
    PortMotion *r=&port_motion[fd->ply];
    switch(job) {
case 0: {
if ((r->data[4] == 1.0f)) {
r->data[5] = PMClamp(fd->input.lstick.Y,0.0f,1.0f);
if ((r->data[5] > r->data[2])) {
r->data[2] = PMMoveTo(r->data[2],r->data[5],(3.5f)/60.0f);
}
}
if ((r->ground == 0.0f)) {
r->drift = 1.0f;
} else {
r->drift = 0.0f;
}
break; }
case 1: {
if (((fd->input.held_prev & 512) && !(fd->input.held & 512))) {
r->data[3] = 1.0f;
}
break; }
case 2: {
if (PM_InputSegment(fd,256,6,r->facing)) {
r->facing *= -1.0f;
PM_Unlisten(r,0,1);
}
break; }
case 3: {
r->drift = 0.0f;
r->data[1] = 0.0f;
r->data[2] = 0.0f;
r->data[3] = 0.0f;
r->data[4] = 1.0f;
PM_Listen(r,0,0,0.0f,now);
PM_Listen(r,1,0,7.0f,now);
if ((55.0f == 0.0f)) {
PM_Listen(r,2,1,0.0f,now);
}
break; }
case 4: {
if ((r->ground == 0.0f)) {
if ((r->data[6] <= 0.0f)) {
if (((fd->input.held & 512) != 0)) {
r->data[0] = 3.0f;
r->data[6] = 1.0f;
if ((55.0f == 0.0f)) {
r->ground = 0; r->y = sqrtf(PMMax(0, 2 * r->gravity * r->data[0]));
}
}
}
}
break; }
case 5: {
PM_Unlisten(r,0,1);
break; }
case 6: {
PP_Spawn(gobj,0, 29, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, ((r->facing * PMLerp(30.0f,18.0f,r->data[2])))*PM_SPEED, (PMLerp(20.0f,45.0f,r->data[2]))*PM_SPEED, (0.0f)*PM_ACCEL, (-150.0f)*PM_ACCEL);
r->data[4] = 0.0f;
break; }
case 7: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
case 8: {
if ((r->ground == 0.0f)) {
if ((r->data[2] == 0.0f)) {
r->drift = 1.0f;
} else {
r->drift = 1.0f;
}
} else {
if ((r->data[2] == 0.0f)) {
r->drift = 0.0f;
} else {
r->drift = 0.0f;
}
}
break; }
case 9: {
if (((fd->input.held_prev & 512) && !(fd->input.held & 512))) {
r->data[3] = 1.0f;
}
break; }
case 10: {
r->drift = 0.0f;
r->data[1] = 0.0f;
r->data[2] = 0.0f;
r->data[3] = 0.0f;
PM_Listen(r,8,0,0.0f,now);
PM_Listen(r,9,0,7.0f,now);
break; }
case 11: {
if (PM_InputSegment(fd,256,2,r->facing)) {
r->facing *= -1.0f;
}
break; }
case 12: {
if (((((fd->input.held & 512) != 0) == 1.0f) && (r->data[3] == 0.0f))) {
r->data[2] = 1.0f;
}
break; }
case 13: {
if ((r->data[2] == 0.0f)) {
PP_Spawn(gobj,1, 20, -0.0f, 0.0f, 0.0f, 4.568476788841445f, 0.0f, 0.0f, ((r->facing * 30.0f))*PM_SPEED, (20.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (-150.0f)*PM_ACCEL);
}
break; }
case 14: {
if ((r->data[2] == 1.0f)) {
PP_Spawn(gobj,1, 20, -0.0f, 0.0f, 0.0f, 4.568476788841445f, 0.0f, 0.0f, ((r->facing * 20.0f))*PM_SPEED, (45.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (-150.0f)*PM_ACCEL);
}
break; }
case 15: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
case 16: {
if ((r->ground == 0.0f)) {
r->drift = 1.0f;
} else {
r->drift = 0.0f;
}
break; }
case 17: {
r->drift = 0.0f;
if ((1.0f == 0.0f)) {
r->facing *= -1.0f;
}
PM_Listen(r,16,0,0.0f,now);
break; }
case 18: {
if ((r->ground > 0.0f)) {
if ((r->facing > 0.0f)) {
PP_Spawn(gobj,2, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
} else {
PP_Spawn(gobj,3, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (-45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
}
} else {
if ((r->facing > 0.0f)) {
PP_Spawn(gobj,4, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
} else {
PP_Spawn(gobj,5, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (-45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
}
}
break; }
case 19: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
case 20: {
if ((r->ground == 0.0f)) {
r->drift = 1.0f;
} else {
r->drift = 0.0f;
}
break; }
case 21: {
r->drift = 0.0f;
if ((1.0f == 0.0f)) {
r->facing *= -1.0f;
}
PM_Listen(r,20,0,0.0f,now);
break; }
case 22: {
if ((r->ground > 0.0f)) {
if ((r->facing > 0.0f)) {
PP_Spawn(gobj,2, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
} else {
PP_Spawn(gobj,3, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (-45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
}
} else {
if ((r->facing > 0.0f)) {
PP_Spawn(gobj,4, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
} else {
PP_Spawn(gobj,5, 20, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, (-45.0f)*PM_SPEED, (0.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (0.0f)*PM_ACCEL);
}
}
break; }
case 23: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
case 24: {
r->data[7] = r->gravity;
if ((r->ground > 0.0f)) {
r->data[0] = 3.0f;
r->ground = 0; r->y = sqrtf(PMMax(0, 2 * r->gravity * r->data[0]));
} else {
r->y = 0.0f;
r->gravity = 0.0f;
}
r->drift = 1.0f;
r->ground = 0.0f;
break; }
case 25: {
PP_Spawn(gobj,6, 6, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, ((r->x * r->facing))*PM_SPEED, (-30.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (-80.0f)*PM_ACCEL);
r->data[0] = 10.0f;
r->ground = 0.0f;
r->gravity = r->data[7];
r->speed = 15.0f;
r->accel = 70.0f;
r->data[8] = 0.0f;
r->ground = 0; r->y = sqrtf(PMMax(0, 2 * r->gravity * r->data[0]));
break; }
case 26: {
if ((PM_InputSegment(fd,16,6,r->facing) && (r->data[8] == 0.0f) && (r->y > 0.0f))) {
r->y *= -1.0f;
r->data[9] = 1.0f;
}
break; }
case 27: {
PM_Listen(r,26,0,0.0f,now);
break; }
case 28: {
r->ledge = 1.0f;
break; }
case 29: {
r->next = r->ground>.5f ? -2 : -2; return;
break; }
case 30: {
r->data[7] = r->gravity;
if ((r->ground > 0.0f)) {
r->data[0] = 3.0f;
r->ground = 0; r->y = sqrtf(PMMax(0, 2 * r->gravity * r->data[0]));
} else {
r->y = 0.0f;
r->gravity = 0.0f;
}
r->drift = 1.0f;
r->ground = 0.0f;
break; }
case 31: {
PP_Spawn(gobj,6, 6, -0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, ((r->x * r->facing))*PM_SPEED, (-30.0f)*PM_SPEED, (0.0f)*PM_ACCEL, (-80.0f)*PM_ACCEL);
r->data[0] = 10.0f;
r->ground = 0.0f;
r->gravity = r->data[7];
r->speed = 15.0f;
r->accel = 70.0f;
r->data[8] = 0.0f;
r->ground = 0; r->y = sqrtf(PMMax(0, 2 * r->gravity * r->data[0]));
break; }
case 32: {
if ((PM_InputSegment(fd,16,6,r->facing) && (r->data[8] == 0.0f) && (r->y > 0.0f))) {
r->y *= -1.0f;
r->data[9] = 1.0f;
}
break; }
case 33: {
PM_Listen(r,32,0,0.0f,now);
break; }
case 34: {
r->ledge = 1.0f;
break; }
case 35: {
r->next = r->ground>.5f ? -2 : -2; return;
break; }
case 36: {
r->drift = 0.0f;
r->data[10] = 0.0f;
break; }
case 37: {
if (((r->hit_shield == 1.0f) || (0.0f == 1.0f) || (0.0f == 1.0f))) {
r->data[10] = 0.0f;
} else {
r->data[10] = 1.0f;
}
break; }
case 38: {
PM_BindHit(r,37);
break; }
case 39: {
if (((fd->input.down & 512) != 0)) {
{ float value=fd->attr.max_jumps-r->jumps; value = 0.0f; r->jumps=PMMax(0,PMMin(fd->attr.max_jumps,fd->attr.max_jumps-(int)value)); }
r->jump_dirty=1;
r->next = r->ground>.5f ? 377 : 378; return;
}
break; }
case 40: {
if ((r->ground == 1.0f)) {
r->input_mask=1161; r->input_until=0;
} else {
r->input_mask=1153; r->input_until=0;
if (((fd->attr.max_jumps-r->jumps) != 0.0f)) {
if (PM_InputSegment(fd,6,6,r->facing)) {
PM_Arm(r,0,39,0.0f,now);
}
}
}
break; }
case 41: {
if ((r->data[10] == 1.0f)) {
PM_Listen(r,40,0,0.0f,now);
}
break; }
case 42: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
case 43: {
r->drift = 0.0f;
r->data[10] = 0.0f;
break; }
case 44: {
if (((r->hit_shield == 1.0f) || (0.0f == 1.0f) || (0.0f == 1.0f))) {
r->data[10] = 0.0f;
} else {
r->data[10] = 1.0f;
}
break; }
case 45: {
PM_BindHit(r,44);
break; }
case 46: {
if (((fd->input.down & 512) != 0)) {
{ float value=fd->attr.max_jumps-r->jumps; value = 0.0f; r->jumps=PMMax(0,PMMin(fd->attr.max_jumps,fd->attr.max_jumps-(int)value)); }
r->jump_dirty=1;
r->next = r->ground>.5f ? 377 : 378; return;
}
break; }
case 47: {
if ((r->ground == 1.0f)) {
r->input_mask=1161; r->input_until=0;
} else {
r->input_mask=1153; r->input_until=0;
if (((fd->attr.max_jumps-r->jumps) != 0.0f)) {
if (PM_InputSegment(fd,6,6,r->facing)) {
PM_Arm(r,1,46,0.0f,now);
}
}
}
break; }
case 48: {
if ((r->data[10] == 1.0f)) {
PM_Listen(r,47,0,0.0f,now);
}
break; }
case 49: {
r->next = r->ground>.5f ? -1 : -1; return;
break; }
    }
}
static void PM_Load(FighterData *fd)
{
    PortMotion *r=&port_motion[fd->ply]; int i;
    r->count=0;r->next=0;r->trigger_order=0;
    for(i=0;i<PM_TRIGGERS;i++)r->triggers[i].live=0;
    r->state=-1;r->hit_count=r->pending_count=r->input_mask=0;
    for(i=0;i<PM_HOOKS;i++)r->hooks[i].live=0;
    for(i=0;i<11;i++)r->data[i]=0;
}
static void PM_Enter(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    PortMotion *r=&port_motion[fd->ply];int i;
    r->count=0;r->next=0;r->trigger_order=0;
    for(i=0;i<PM_TRIGGERS;i++)r->triggers[i].live=0;
    r->helpless=0; r->landing=29.0f;
    r->state=fd->state_id;r->hit_count=r->pending_count=r->input_mask=0;r->input_until=0;
    for(i=0;i<PM_HOOKS;i++)r->hooks[i].live=0;
    r->gravity=fd->attr.gravity/PM_ACCEL;
    r->fall=fd->attr.terminal_velocity/PM_SPEED;
    r->accel=fd->attr.aerial_drift_stick_mult/PM_ACCEL;
    r->speed=fd->attr.aerial_drift_max/PM_SPEED;
    r->drift=1; r->ledge=1;
    switch(fd->state_id) {
case 373: r->helpless=0; PM_Schedule(r,3,1.0f); PM_Schedule(r,4,6.0f); PM_Schedule(r,5,9.0f); PM_Schedule(r,6,23.0f); PM_Schedule(r,7,45.0f); break;
case 374: r->helpless=0; PM_Schedule(r,10,1.0f); PM_Schedule(r,11,9.0f); PM_Schedule(r,12,9.0f); PM_Schedule(r,13,11.0f); PM_Schedule(r,14,33.0f); PM_Schedule(r,15,45.0f); break;
case 375: r->helpless=0; PM_Schedule(r,17,1.0f); PM_Schedule(r,18,21.0f); PM_Schedule(r,19,51.0f); break;
case 376: r->helpless=0; PM_Schedule(r,21,1.0f); PM_Schedule(r,22,21.0f); PM_Schedule(r,23,51.0f); break;
case 377: r->helpless=1; PM_Schedule(r,24,1.0f); PM_Schedule(r,25,21.0f); PM_Schedule(r,27,31.0f); PM_Schedule(r,28,41.0f); PM_Schedule(r,29,51.0f); break;
case 378: r->helpless=1; PM_Schedule(r,30,1.0f); PM_Schedule(r,31,21.0f); PM_Schedule(r,33,31.0f); PM_Schedule(r,34,41.0f); PM_Schedule(r,35,51.0f); break;
case 379: r->helpless=0; PM_Schedule(r,36,1.0f); PM_Schedule(r,38,6.0f); PM_Schedule(r,41,17.0f); PM_Schedule(r,42,41.0f); break;
case 380: r->helpless=0; PM_Schedule(r,43,1.0f); PM_Schedule(r,45,6.0f); PM_Schedule(r,48,17.0f); PM_Schedule(r,49,41.0f); break;
    }
}
static void PM_Write(GOBJ *gobj,PortMotion *r)
{
    FighterData *fd=gobj->userdata;
    if(r->ground<0.5f && !fd->phys.air_state)Fighter_SetAirborne(fd);
    if((r->facing<0 ? -1 : 1)!=fd->facing_direction) {
        fd->facing_direction=r->facing<0 ? -1 : 1;
        Fighter_SetBoneRotY(fd,0,1.5707963f*fd->facing_direction);
    }
    if(r->jump_dirty)fd->jump.jumps_used=(int)PMMax(0,PMMin(fd->attr.max_jumps,r->jumps));r->jumps=fd->jump.jumps_used;r->jump_dirty=0;
    if(fd->phys.air_state) {
        fd->phys.self_vel.X=r->x*PM_SPEED*fd->facing_direction;
        fd->phys.self_vel.Y=r->y*PM_SPEED;
    }
    else fd->phys.self_vel_ground.X=r->x*PM_SPEED*fd->facing_direction;
}
static int PM_HasDue(GOBJ *gobj,int frame)
{
    FighterData *fd=gobj->userdata;PortMotion *r=&port_motion[fd->ply];int i;
    for(i=0;i<r->count;i++)if(r->events[i].live && r->events[i].at<=frame)return 1;
    return 0;
}
static int PM_Transfer(GOBJ *gobj,PortMotion *r,int budget)
{
    int state=r->next;
    PM_Write(gobj,r);
    if(budget<=0) { Leave(gobj,r->helpless,r->landing); return 0; }
    if(state>0) { Enter(gobj,state); return 1; }
    r->next=0;r->state=-1;r->hit_count=r->pending_count=r->input_mask=0;
    if(state==-3)Fighter_EnterSpecialLanding(gobj,0,r->landing);
    else if(state==-4)Fighter_EnterLanding(gobj);
    else Leave(gobj,state==-2,r->landing);
    return 0;
}
void PM_Phys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    PortMotion *r=&port_motion[fd->ply];
    int i, next, order, previous, transitions=8, budget=PM_JOBS; float at,now=t_frame[fd->ply];
    float old_accel,old_base,old_max;
    if(fd->flags.hitlag)return;
    r->facing=fd->facing_direction;
    r->ground=!fd->phys.air_state;
    r->x=(fd->phys.air_state ? fd->phys.self_vel.X : fd->phys.self_vel_ground.X)/PM_SPEED*r->facing;
    r->y=fd->phys.self_vel.Y/PM_SPEED;
    r->jumps=fd->jump.jumps_used;r->jump_dirty=0;
dispatch:
    now=t_frame[fd->ply];budget=PM_JOBS;
    for(i=0;i<r->pending_count;i++) {
        r->hit_shield=r->pending[i].shield;
        PM_Run(gobj,r->pending[i].job,now);
        if(r->next)goto transfer;
    }
    r->pending_count=0;
    for(i=0;i<PM_HOOKS;i++)if(r->hooks[i].live && now>r->hooks[i].born) {
        if(r->hooks[i].until>0 && now>=r->hooks[i].until)r->hooks[i].live=0;
        else PM_Run(gobj,r->hooks[i].job,now);
        if(r->next)goto transfer;
    }
    while(budget--) {
        next=-1; at=now+1;
        for(i=0;i<r->count;i++)
            if(r->events[i].live && r->events[i].at<=now && r->events[i].at<at) {
                next=i;at=r->events[i].at;
            }
        if(next<0)break;
        r->events[next].live=0;
        PM_Run(gobj,r->events[next].job,at);
        if(r->next)goto transfer;
    }
    previous=r->trigger_order+1;
    for(;;) {
        next=-1;order=0;
        for(i=0;i<PM_TRIGGERS;i++)if(r->triggers[i].live) {
            if(r->triggers[i].until>0 && now>=r->triggers[i].until)r->triggers[i].live=0;
            else if(r->triggers[i].order<previous && r->triggers[i].order>order) {
                next=i;order=r->triggers[i].order;
            }
        }
        if(next<0)break;
        previous=order;PM_Run(gobj,r->triggers[next].job,now);
        if(r->next)goto transfer;
    }
    PM_Write(gobj,r);
    if(fd->phys.air_state) {
        fd->phys.self_vel.X=r->x*PM_SPEED*fd->facing_direction;
        fd->phys.self_vel.Y=r->y*PM_SPEED;
        Fighter_PhysAir_ApplyGravity(fd,PMMax(0,r->gravity)*PM_ACCEL,PMMax(0,r->fall)*PM_SPEED);
        if(r->drift>0) {
            old_accel=fd->attr.aerial_drift_stick_mult;
            old_base=fd->attr.aerial_drift_base;old_max=fd->attr.aerial_drift_max;
            fd->attr.aerial_drift_stick_mult=PMMax(0,r->accel)*PM_ACCEL*r->drift;
            fd->attr.aerial_drift_base=0;
            fd->attr.aerial_drift_max=PMMax(0,r->speed)*PM_SPEED*r->drift;
            Fighter_PhysAir_ApplyAerialDrift(fd);
            fd->attr.aerial_drift_stick_mult=old_accel;
            fd->attr.aerial_drift_base=old_base;fd->attr.aerial_drift_max=old_max;
        }
    }
    else {
        fd->phys.self_vel_ground.X=r->x*PM_SPEED*fd->facing_direction;
        Fighter_PhysGround_ApplyFriction(gobj);
    }
    return;
transfer:
    if(PM_Transfer(gobj,r,transitions--))goto dispatch;
}
static void PM_Landed(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    PortMotion *r=&port_motion[fd->ply];
    Fighter_SetGrounded2(fd);
    if(r->helpless)Fighter_EnterSpecialLanding(gobj,0,r->landing);
    else Hand_Over(gobj,PM_Form(r,fd->state_id,0));
}
void PM_Coll(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    PortMotion *r=&port_motion[fd->ply];
    if(!fd->phys.air_state) {
        if(!Fighter_CollGround_PassLedge(gobj)) {
            Fighter_SetAirborne(fd);Hand_Over(gobj,PM_Form(r,fd->state_id,1));
        }
    }
    else if(r->ledge>0 && fd->phys.self_vel.Y<=0)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj,0,PM_Landed);
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))PM_Landed(gobj);
}

void PM_N_IASA(GOBJ *gobj) { if(!PM_InputCancel(gobj)) N_IASA(gobj); }

void PM_S_IASA(GOBJ *gobj) { if(!PM_InputCancel(gobj)) S_IASA(gobj); }

void PM_Hi_IASA(GOBJ *gobj) { if(!PM_InputCancel(gobj)) Hi_IASA(gobj); }

void PM_Lw_IASA(GOBJ *gobj) { if(!PM_InputCancel(gobj)) Lw_IASA(gobj); }


static void PM_OnHit(GOBJ *,GOBJ *,float,int);
static void PM_NoHurt(GOBJ *,GOBJ *,float,int);
// Fighter_Create uses p_link 8. HSD orders processes by priority then p_link.
// This helper runs at p_link 0 / priority 14: after ALL collision (13), before fighter damage/reset (14).
typedef struct {GOBJ *source,*target; float damage; int shield;} PortContact;
static GOBJ *contact_observer;
static GOBJ *contact_owners[6];
static int contact_kinds[6];
static int Contact_Owns(GOBJ *gobj)
{
    int i;
    for (i = 0; i < 6; i++)
        if (gobj && contact_owners[i] == gobj && Fighter_GetGObj(i) == gobj &&
            ((FighterData *)gobj->userdata)->kind == contact_kinds[i])
            return 1;
    return 0;
}
static void Contact_Destroy(void *data)
{
    int i;
    if (contact_observer == data)
    {
        contact_observer = 0;
        for (i = 0; i < 6; i++)
            contact_owners[i] = 0;
    }
}
static void Contact_Observe(GOBJ *unused)
{
    PortContact events[24];
    int i,count = 0;
    // Snapshot all records first: callbacks may update resources but must not alter the engine's damage
    // fields or change action state here. Queue any resulting state transition for a later IASA update.
    for (i = 0; i < 12; i++)
    {
        GOBJ *target = Fighter_GetSubcharGObj(i / 2,i % 2);
        FighterData *fd;
        if (!target)
            continue;
        fd = target->userdata;
        if (fd->flags.sleep)
            continue; // the native damage process also skips sleeping fighters and its reset
        if (fd->dmg.percent_temp > 0.0f && fd->dmg.hit_log.source &&
            (fd->dmg.tip_force_applied == 0.0f || fd->dmg.hit_log.force_applied > 0.0f))
        {
            events[count].source = fd->dmg.hit_log.source;
            events[count].target = target;
            events[count].damage = fd->dmg.percent_temp;
            events[count++].shield = 0;
        }
        if (fd->shield.dmg_taken2 > 0 && fd->shield.dmg_source)
        {
            events[count].source = fd->shield.dmg_source;
            events[count].target = target;
            events[count].damage = fd->shield.dmg_taken2;
            events[count++].shield = 1;
        }
    }
    for (i = 0; i < count; i++)
    {
        PortContact *e = &events[i];
        if (Contact_Owns(e->source))
            PM_OnHit(e->source,e->target,e->damage,e->shield);
        if (Contact_Owns(e->target))
            PM_NoHurt(e->target,e->source,e->damage,e->shield);
    }
}
static void Contact_Load(FighterData *fd,GOBJ *gobj)
{
    contact_owners[fd->ply] = gobj;
    contact_kinds[fd->ply] = fd->kind;
    if (!contact_observer)
    {
        contact_observer = GObj_Create(0,0,0);
        GObj_AddUserData(contact_observer,0,Contact_Destroy,contact_observer);
        GObj_AddProc(contact_observer,Contact_Observe,FTPRI_DMGAPPLY);
    }
}


typedef struct {int action,state,first,end,target,neutral;} JabWindow;
static const JabWindow jab_windows[] = {
    {46, 44, 1, 1, 45, 0},
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
        PM_N_IASA(gobj);
}

void Reverse_IASA_374(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_N_IASA(gobj);
}

void Reverse_IASA_375(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_S_IASA(gobj);
}

void Reverse_IASA_376(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_S_IASA(gobj);
}

void Reverse_IASA_377(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_Hi_IASA(gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_Hi_IASA(gobj);
}

void Reverse_IASA_379(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_Lw_IASA(gobj);
}

void Reverse_IASA_380(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        PM_Lw_IASA(gobj);
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    { ItemDesc **items=fd->ftData->items; int i;
    for(i=0;items && i<7;i++)if(items[i])MEX_IndexFighterItem(fd->kind,items[i],i); }
    Contact_Load(fd,gobj);
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
    {306, 0x340211, 0x12, 0x0, PM_Anim, Reverse_IASA_373, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 373: the neutral special (plain)
    {315, 0x340611, 0x12, 0x0, PM_Anim, Reverse_IASA_374, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 374: the neutral special in the air (plain)
    {303, 0x340212, 0x13, 0x0, PM_Anim, Reverse_IASA_375, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 375: the side special (plain)
    {312, 0x340612, 0x13, 0x0, PM_Anim, Reverse_IASA_376, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 376: the side special in the air (plain)
    {321, 0x340213, 0x14, 0x0, PM_Anim, Reverse_IASA_377, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 377: the up special (plain)
    {322, 0x340613, 0x14, 0x0, PM_Anim, Reverse_IASA_378, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 378: the up special in the air (plain)
    {323, 0x340214, 0x15, 0x0, PM_Anim, Reverse_IASA_379, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 379: the down special (plain)
    {325, 0x340614, 0x15, 0x0, PM_Anim, Reverse_IASA_380, PM_Phys, PM_Coll, Fighter_UpdateCameraBox},    // 380: the down special in the air (plain)
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
