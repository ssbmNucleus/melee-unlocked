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

    }
    return state;                       // (a state not listed: a mistake; it stays the state it is)
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

static int Port_SegmentCircle(float ax,float ay,float bx,float by,float cx,float cy,float radius)
{
    float dx=bx-ax,dy=by-ay,length=dx*dx+dy*dy,t=0;
    if(radius<0)return 0;
    if(length>0.000001f)t=((cx-ax)*dx+(cy-ay)*dy)/length;
    if(t<0)t=0;if(t>1)t=1;
    dx=ax+t*dx-cx;dy=ay+t*dy-cy;
    return dx*dx+dy*dy<=radius*radius;
}
static void Port_FireFan(GOBJ *owner,int slot,Vec3 at,int count,float angle,float spread,float speed,float offset)
{
    FighterData *fd=owner->userdata;int i;float a,c,s;Vec3 point;GOBJ *item;
    if(count<1 || count>16)return;
    for(i=0;i<count;i++)
    {
        a=angle+(i-(count-1)*0.5f)*spread;c=cos(a);s=sin(a);point=at;
        point.X+=(offset-speed)*c*fd->facing_direction;
        point.Y+=(offset-speed)*s;
        item=Make_Item(owner,slot,&point);
        if(item)
        {
            ItemData *id=item->userdata;
            id->self_vel.X=speed*c*fd->facing_direction;id->self_vel.Y=speed*s;
            id->item_var.var1=1;
        }
    }
}

#define RN_G 373
#define RN_A 374
#define RN_TG 375
#define RN_TA 376
static int ranno_darts[6],ranno_thrown[6],ranno_volley[6],ranno_charge[6];
static const int ranno_stages[] = {9,33,57,81};
static void RannoLoad(FighterData *fd)
{
    ItemDesc **items=fd->ftData->items;
    ranno_darts[fd->ply]=1; ranno_thrown[fd->ply]=ranno_volley[fd->ply]=ranno_charge[fd->ply]=0;
    if (items && items[0]) MEX_IndexFighterItem(fd->kind,items[0],0);
    if (items && items[3]) MEX_IndexFighterItem(fd->kind,items[3],3);
}
static void RannoStart(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata;
    ranno_thrown[fd->ply]=0;
    Enter(gobj,state);
    if (ranno_charge[fd->ply] >= 9)
    {
        ActionStateChange(ranno_charge[fd->ply],1,0,gobj,state,0x5001,0);
        t_frame[fd->ply]=ranno_charge[fd->ply];
    }
}
void SpecialN(GOBJ *gobj) { RannoStart(gobj,RN_G); }
void SpecialAirN(GOBJ *gobj) { RannoStart(gobj,RN_A); }
void RannoChargeAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int i,frame=Tick(gobj);
    for(i=0;i<4;i++)
        if(frame>=ranno_stages[i]) ranno_darts[fd->ply]=i+1;
    ranno_charge[fd->ply]=frame < 81 ? frame : 81;
    if(frame>=105)
    {
        ActionStateChange(81,1,0,gobj,fd->state_id,0x5001,0);
        t_frame[fd->ply]=81;
    }
}
void RannoChargeIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(t_frame[fd->ply]>=9)
    {
        if(!fd->phys.air_state)
        {
            // ftCo_80091A4C is the native shield input check; it enters Guard on success.
            if(((int (*)(GOBJ *))0x80091A4C)(gobj) || Fighter_IASACheck_JumpF(gobj)) return;
        }
        else if(Fighter_IASACheck_JumpAerial(gobj) || Fighter_IASACheck_AirEscape(gobj)) return;
    }
    if(t_frame[fd->ply]>=9 && !(fd->input.held & B_BUTTON))
    {
        ranno_volley[fd->ply]=ranno_darts[fd->ply];
        ranno_thrown[fd->ply]=0;
        Enter(gobj,fd->phys.air_state ? RN_TA : RN_TG);
    }
}
static void RannoShoot(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int p=fd->ply,index=ranno_thrown[p]++,air=fd->phys.air_state!=0;
    float angle=(air ? -0.7853981633974483f : 0.0f) + index*(air ? 0.12217304763960307f : 0.12217304763960307f);
    float c=cos(angle),s=sin(angle);
    Vec3 at=fd->phys.pos;
    GOBJ *shot;
    // Melee integrates newly created items on this same update. The source article's
    // first observed frame is its launch position, then it advances on the next update.
    at.X+=((air ? 1.0f : 10.0f)+(10.0f-4.5f)*c)*fd->facing_direction;
    at.Y+=(air ? 5.0f : 5.0f)+(10.0f-4.5f)*s;
    at.Z=0;
    shot=Make_Item(gobj,0,&at);
    if(shot)
    {
        ItemData *id=shot->userdata;
        id->self_vel.X=4.5f*c*fd->facing_direction;
        id->self_vel.Y=4.5f*s;
        id->item_var.var1=1;
    }
    ranno_darts[p]=ranno_volley[p]-ranno_thrown[p];
    ranno_charge[p]=ranno_darts[p]>0 ? ranno_stages[ranno_darts[p]-1] : 0;
    if(ranno_darts[p]<1)ranno_darts[p]=1;
}
static void RannoStoredChargeFrame(FighterData *fd)
{
    if(fd->state_id < ASID_WAIT)
    {
        ranno_charge[fd->ply]=0;
        ranno_darts[fd->ply]=1;
    }
}
void RannoThrowAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int frame=Tick(gobj),offset=frame-3;
    if(offset>=0 && offset%2==0 && ranno_thrown[fd->ply]<ranno_volley[fd->ply])
        RannoShoot(gobj);
    if(frame>=(fd->phys.air_state ? 34 : 41)) Leave(gobj,0,0);
}
void RannoPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(fd->phys.air_state)
    {
        float mult=fd->attr.aerial_drift_stick_mult,base=fd->attr.aerial_drift_base;
        fd->attr.aerial_drift_stick_mult=0.045000000000000005f;
        fd->attr.aerial_drift_base=0;
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
        fd->attr.aerial_drift_stick_mult=mult;fd->attr.aerial_drift_base=base;
    }
    else Fighter_PhysGround_ApplyFriction(gobj);
}
void RannoColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int state=fd->state_id,charging=(state==RN_G || state==RN_A);
    if(!fd->phys.air_state)
    {
        if(!Fighter_CollGround_PassLedge(gobj))
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj,charging ? RN_A : RN_TA);
        }
    }
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))
    {
        Fighter_SetGrounded2(fd);
        if(charging || t_frame[fd->ply]<3) Hand_Over(gobj,charging ? RN_G : RN_TG);
        else Fighter_EnterSpecialLanding(gobj,0,7.0f);
    }
}
void RannoThrowIASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 34)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 26)
        Fighter_IASACheck_AllAerial(gobj);
}

#define RH_G 377
#define RH_A 378
#define RH_DG 379
#define RH_DA 380
#define RH_SG 381
#define RH_SA 382
#define RH_LG 383
#define RH_LA 384
static int RannoRecoveryKind(int state)
{
    if (state == RH_DG || state == RH_DA) return 1;
    if (state == RH_SG || state == RH_SA) return 2;
    if (state == RH_LG || state == RH_LA) return 3;
    return 0;
}
void SpecialHi(GOBJ *gobj) { Enter(gobj,RH_G); }
void SpecialAirHi(GOBJ *gobj) { Enter(gobj,RH_A); }
void RannoRecoveryAnim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int kind,frame;Vec3 at;
    if(fd->flags.hitlag)return;
    kind=RannoRecoveryKind(fd->state_id);frame=Tick(gobj);
    if(kind==2)
    {
        at=fd->phys.pos;at.X+=0.0f*fd->facing_direction;at.Y+=8.0f;at.Z=0;
        if(frame==10)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
if(frame==18)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
if(frame==26)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
if(frame==34)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
if(frame==42)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
if(frame==50)Port_FireFan(gobj,3,at,3,-0.6108652381980153f,0.4363323129985824f,4.5f,5.0f);
    }
    if (frame >= (kind == 1 ? 67 : kind == 2 ? 78 : kind == 3 ? 46 : 45))
        Leave(gobj,kind != 3,32.0f);
}
void RannoRecoveryIASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int kind = RannoRecoveryKind(fd->state_id), frame = t_frame[fd->ply];
    if (kind == 0)
    {
        if (((frame >= 22 && frame <= 26) || (frame >= 27 && frame <= 30)) && (fd->input.held & 0x100))
            Enter(gobj,fd->phys.air_state ? RH_SA : RH_SG);
        else if (((frame >= 18 && frame <= 26) || (frame >= 27 && frame <= 36)) && (fd->input.down & B_BUTTON) && fd->input.lstick.Y < -0.5f)
            Enter(gobj,fd->phys.air_state ? RH_DA : RH_DG);
    }
    else if (kind == 1 && ((frame >= 43 && frame <= 56) || (frame >= 57 && frame <= 66)))
        Fighter_IASACheck_JumpAerial(gobj);
    else if (kind == 3 && frame >= 38)
        Fighter_IASACheck_AllGrounded(gobj);
}
static void RannoRecoveryLanded(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int kind = RannoRecoveryKind(fd->state_id);
    Fighter_SetGrounded2(fd);
    if (kind == 1) Enter(gobj,RH_LG);
    else Fighter_EnterSpecialLanding(gobj,0,32.0f);
}
void RannoRecoveryColl(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int kind = RannoRecoveryKind(fd->state_id), frame = t_frame[fd->ply];
    if (!fd->phys.air_state)
    {
        if (!Fighter_CollGround_PassLedge(gobj))
        {
            Fighter_SetAirborne(fd);
            if (kind == 3) Fighter_EnterFall(gobj);
            else Hand_Over(gobj,kind == 0 ? RH_A : kind == 1 ? RH_DA : RH_SA);
        }
        return;
    }
    if (frame >= (kind == 1 ? 57 : kind == 2 ? 76 : 27))
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj,0,RannoRecoveryLanded);
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj)) RannoRecoveryLanded(gobj);
}
void RannoRecoveryBasePhys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply], drift = 1, ground_owned = 0;
    float gravity = fd->attr.gravity, limit = fd->attr.terminal_velocity;
    float accel = fd->attr.aerial_drift_stick_mult, maxspeed = fd->attr.aerial_drift_max;
    float old_accel = accel, old_base = fd->attr.aerial_drift_base, old_max = maxspeed;
    if (fd->flags.hitlag) return;
    switch (fd->action_id) {
    case 304:
        if (frame >= 1 && frame < 10) { gravity = 0.0f; drift = 0; }
        if (frame >= 10 && frame < 12) { gravity = 0.0f; drift = 0; }
        if (frame >= 12 && frame < 18) { gravity = 0.0f; drift = 0; }
        if (frame >= 18 && frame < 20) { gravity = 0.0f; drift = 0; }
        if (frame >= 20 && frame < 26) { gravity = 0.0f; drift = 0; }
        if (frame >= 26 && frame < 28) { gravity = 0.0f; drift = 0; }
        if (frame >= 28 && frame < 34) { gravity = 0.0f; drift = 0; }
        if (frame >= 34 && frame < 36) { gravity = 0.0f; drift = 0; }
        if (frame >= 36 && frame < 42) { gravity = 0.0f; drift = 0; }
        if (frame >= 42 && frame < 44) { gravity = 0.0f; drift = 0; }
        if (frame >= 44 && frame < 50) { gravity = 0.0f; drift = 0; }
        if (frame >= 50 && frame < 52) { gravity = 0.0f; drift = 0; }
        if (frame >= 52 && frame < 58) { gravity = 0.0f; drift = 0; }
        if (frame >= 58 && frame < 60) { gravity = 0.0f; drift = 0; }
        if (frame >= 60 && frame < 78) { accel = 0.024245187640190125f; }
        break;
    case 305:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; drift = 0; }
        if (frame >= 9 && frame < 57) { gravity = 0.0f; drift = 0; }
        break;
    case 311:
        break;
    case 313:
        if (frame >= 1 && frame < 10) { gravity = 0.0f; drift = 0; }
        if (frame >= 10 && frame < 12) { gravity = 0.0f; drift = 0; }
        if (frame >= 12 && frame < 18) { gravity = 0.0f; drift = 0; }
        if (frame >= 18 && frame < 20) { gravity = 0.0f; drift = 0; }
        if (frame >= 20 && frame < 26) { gravity = 0.0f; drift = 0; }
        if (frame >= 26 && frame < 28) { gravity = 0.0f; drift = 0; }
        if (frame >= 28 && frame < 34) { gravity = 0.0f; drift = 0; }
        if (frame >= 34 && frame < 36) { gravity = 0.0f; drift = 0; }
        if (frame >= 36 && frame < 42) { gravity = 0.0f; drift = 0; }
        if (frame >= 42 && frame < 44) { gravity = 0.0f; drift = 0; }
        if (frame >= 44 && frame < 50) { gravity = 0.0f; drift = 0; }
        if (frame >= 50 && frame < 52) { gravity = 0.0f; drift = 0; }
        if (frame >= 52 && frame < 58) { gravity = 0.0f; drift = 0; }
        if (frame >= 58 && frame < 60) { gravity = 0.0f; drift = 0; }
        if (frame >= 60 && frame < 78) { accel = 0.024245187640190125f; }
        break;
    case 314:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; drift = 0; }
        if (frame >= 9 && frame < 57) { gravity = 0.0f; drift = 0; }
        break;
    case 320:
        break;
    case 321:
        if (frame >= 1 && frame < 11) { gravity = 0.0f; drift = 0; }
        if (frame >= 11 && frame < 27) { drift = 0; }
        if (frame >= 27 && frame < 45) { accel = 0.04849037528038025f; }
        break;
    case 322:
        if (frame >= 1 && frame < 11) { gravity = 0.0f; drift = 0; }
        if (frame >= 11 && frame < 27) { drift = 0; }
        if (frame >= 27 && frame < 45) { accel = 0.04849037528038025f; }
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
    case 304:
        if (frame >= 1 && frame <= 9) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 9) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 10 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 10 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 12 && frame <= 17) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 12 && frame <= 17) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 18 && frame <= 19) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 18 && frame <= 19) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 20 && frame <= 25) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 20 && frame <= 25) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 26 && frame <= 27) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 26 && frame <= 27) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 28 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 28 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 34 && frame <= 35) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 34 && frame <= 35) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 36 && frame <= 41) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 36 && frame <= 41) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 42 && frame <= 43) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 42 && frame <= 43) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 44 && frame <= 49) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 44 && frame <= 49) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 50 && frame <= 51) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 50 && frame <= 51) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 52 && frame <= 57) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 52 && frame <= 57) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 58 && frame <= 59) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 58 && frame <= 59) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 305:
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 9 && frame <= 56) { if (fd->phys.air_state) fd->phys.self_vel.Y = -3.87923002243042f; }
        if (frame >= 9 && frame <= 56) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.6486727595329287f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.6486727595329287f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 311:
        break;
    case 313:
        if (frame >= 1 && frame <= 9) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 9) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 10 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 10 && frame <= 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 12 && frame <= 17) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 12 && frame <= 17) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 18 && frame <= 19) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 18 && frame <= 19) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 20 && frame <= 25) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 20 && frame <= 25) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 26 && frame <= 27) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 26 && frame <= 27) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 28 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 28 && frame <= 33) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 34 && frame <= 35) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 34 && frame <= 35) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 36 && frame <= 41) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 36 && frame <= 41) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 42 && frame <= 43) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 42 && frame <= 43) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 44 && frame <= 49) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 44 && frame <= 49) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 50 && frame <= 51) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 50 && frame <= 51) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 52 && frame <= 57) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 52 && frame <= 57) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 58 && frame <= 59) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 58 && frame <= 59) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 314:
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 9 && frame <= 56) { if (fd->phys.air_state) fd->phys.self_vel.Y = -3.87923002243042f; }
        if (frame >= 9 && frame <= 56) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.6486727595329287f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.6486727595329287f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 320:
        break;
    case 321:
        if (frame >= 1 && frame <= 10) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 10) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 11) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 3.87923002243042f; }
        if (frame == 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.1637690067291262f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.1637690067291262f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 322:
        if (frame >= 1 && frame <= 10) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 10) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame == 11) { if (!fd->phys.air_state) Fighter_SetAirborne(fd); if (fd->phys.air_state) fd->phys.self_vel.Y = 3.87923002243042f; }
        if (frame == 11) { if (fd->phys.air_state) fd->phys.self_vel.X = 1.1637690067291262f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 1.1637690067291262f * fd->facing_direction; ground_owned = 1; } }
        break;
    }
    if (ground_owned && !fd->phys.air_state)
    {
        fd->phys.horzitonal_velocity_queue_will_be_added_to_0xec = 0;
        fd->phys.vertical_velocity_queue_will_be_added_to_0xec = 0;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
}

void RannoRecoveryPhys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float stick,angle,x,y;
    RannoRecoveryBasePhys(gobj);
    if (RannoRecoveryKind(fd->state_id) || t_frame[fd->ply] != 11) return;
    // Native adaptation: tilt the source launch vector toward forward stick.
    stick = fd->input.lstick.X * fd->facing_direction;
    if (stick < 0) stick = 0;
    if (stick > 1) stick = 1;
    angle = -0.3534291735288517f * stick;
    x = fd->phys.self_vel.X * fd->facing_direction;
    y = fd->phys.self_vel.Y;
    fd->phys.self_vel.X = (x*cos(angle)-y*sin(angle))*fd->facing_direction;
    fd->phys.self_vel.Y = x*sin(angle)+y*cos(angle);
}
static void Poison_Contact(GOBJ *,GOBJ *,float,int);

static void Poison_NoHit(GOBJ *,GOBJ *,float,int);
static void Poison_NoHurt(GOBJ *,GOBJ *,float,int);
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
        Poison_Contact(e->source,e->target,e->damage,e->shield);
        if (Contact_Owns(e->source))
            Poison_NoHit(e->source,e->target,e->damage,e->shield);
        if (Contact_Owns(e->target))
            Poison_NoHurt(e->target,e->source,e->damage,e->shield);
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
static void RannoCaptureContact(GOBJ *,GOBJ *,float,int);

typedef struct {GOBJ *target; int kind,stacks; u16 attack;} PortPoison;
static PortPoison poison[12];
static GOBJ *poison_observer;
static u16 Poison_Attack(FighterData *fd)
{
    // ft_0881.c: +206C is the stale-move attack instance. MexTK's named
    // atk_instance field is +2088 and belongs to a different combat record.
    return *(u16 *)((u8 *)fd+0x206C);
}
static int Poison_Slot(GOBJ *gobj)
{
    int i;
    for(i=0;i<12;i++) if(Fighter_GetSubcharGObj(i/2,i%2)==gobj) return i;
    return -1;
}
static PortPoison *Poison_Target(GOBJ *gobj)
{
    int i=Poison_Slot(gobj);
    FighterData *fd;
    PortPoison *p;
    if(i<0) return 0;
    fd=gobj->userdata; p=&poison[i];
    if(p->target!=gobj || p->kind!=fd->kind)
    {
        p->target=gobj;p->kind=fd->kind;p->stacks=0;p->attack=Poison_Attack(fd);
    }
    if(fd->state_id<ASID_WAIT) p->stacks=0;
    return p;
}
static void Poison_Destroy(void *data)
{
    int i;
    if(poison_observer!=data) return;
    poison_observer=0;
    for(i=0;i<12;i++){poison[i].target=0;poison[i].stacks=0;}
}
static void Poison_Frame(GOBJ *unused)
{
    int i;
    for(i=0;i<12;i++)
    {
        GOBJ *target=Fighter_GetSubcharGObj(i/2,i%2);
        FighterData *fd;
        PortPoison *p;
        u16 attack;
        if(!target){poison[i].target=0;poison[i].stacks=0;continue;}
        fd=target->userdata;p=Poison_Target(target);attack=Poison_Attack(fd);
        if(!fd->flags.sleep && p->stacks && attack!=p->attack && fd->atk_kind>=2 && fd->atk_kind<=47)
            Fighter_GiveDamage(fd,1.0f);
        p->attack=attack;
    }
}
static int Poison_DirectAmount(FighterData *fd)
{
    int action=fd->action_id,frame=(int)fd->state.frame;
    if(action==62 && frame>=15 && frame<=15)return 2;
    if(action==62 && frame>=15 && frame<=16)return 2;
    if(action==62 && frame>=15 && frame<=18)return 2;
    if(action==66 && frame>=11 && frame<=11)return 2;
    if(action==66 && frame>=12 && frame<=13)return 2;
    if(action==66 && frame>=14 && frame<=14)return 2;
    if(action==67 && frame>=10 && frame<=14)return 2;
    if(action==297 && frame>=5 && frame<=7)return 1;
    if(action==297 && frame>=11 && frame<=13)return 1;
    if(action==297 && frame>=17 && frame<=19)return 1;
    if(action==297 && frame>=23 && frame<=25)return 1;
    if(action==297 && frame>=29 && frame<=31)return 1;
    if(action==297 && frame>=35 && frame<=37)return 1;
    if(action==304 && frame>=10 && frame<=11)return 1;
    if(action==304 && frame>=18 && frame<=19)return 1;
    if(action==304 && frame>=26 && frame<=27)return 1;
    if(action==304 && frame>=34 && frame<=35)return 1;
    if(action==304 && frame>=42 && frame<=43)return 1;
    if(action==304 && frame>=50 && frame<=51)return 1;
    if(action==304 && frame>=58 && frame<=59)return 1;
    if(action==305 && frame>=9 && frame<=9)return 1;
    if(action==305 && frame>=9 && frame<=56)return 1;
    if(action==311 && frame>=1 && frame<=1)return 1;
    if(action==313 && frame>=10 && frame<=11)return 1;
    if(action==313 && frame>=18 && frame<=19)return 1;
    if(action==313 && frame>=26 && frame<=27)return 1;
    if(action==313 && frame>=34 && frame<=35)return 1;
    if(action==313 && frame>=42 && frame<=43)return 1;
    if(action==313 && frame>=50 && frame<=51)return 1;
    if(action==313 && frame>=58 && frame<=59)return 1;
    if(action==314 && frame>=9 && frame<=9)return 1;
    if(action==314 && frame>=9 && frame<=56)return 1;
    if(action==320 && frame>=1 && frame<=1)return 1;
    if(action==321 && frame>=11 && frame<=11)return 1;
    if(action==321 && frame>=11 && frame<=22)return 1;
    if(action==321 && frame>=23 && frame<=26)return 1;
    if(action==322 && frame>=11 && frame<=11)return 1;
    if(action==322 && frame>=11 && frame<=22)return 1;
    if(action==322 && frame>=23 && frame<=26)return 1;
    return 0;
}
static int Poison_ItemAmount(GOBJ *source)
{
    int i;
    ItemData *item;
    if(!source || source->entity_class!=6 || !source->userdata) return 0;
    item=source->userdata;
    for(i=0;i<6;i++)
    {
        GOBJ *owner=contact_owners[i];
        if(!Contact_Owns(owner))continue;
        if(item->kind==MEX_GetFtItemID(owner,0))return 1;
        if(item->kind==MEX_GetFtItemID(owner,3))return 1;
        if(item->kind==MEX_GetFtItemID(owner,4))return 1;
    }
    return 0;
}
static void Poison_Contact(GOBJ *source,GOBJ *target,float damage,int shield)
{
    PortPoison *attacker,*victim;
    int amount=0;
    RannoCaptureContact(source,target,damage,shield);
    if(shield || damage<=0 || source==target)return;
    attacker=Poison_Target(source);
    if(attacker && attacker->stacks)attacker->stacks--;
    if(Contact_Owns(source))amount=Poison_DirectAmount(source->userdata);
    else amount=Poison_ItemAmount(source);
    if(!amount)return;
    victim=Poison_Target(target);
    if(!victim || ((FighterData *)target->userdata)->state_id<ASID_WAIT)return;
    victim->stacks+=amount;
    if(victim->stacks>4)victim->stacks=4;
}
static void Poison_NoHit(GOBJ *a,GOBJ *b,float damage,int shield){}
static void Poison_NoHurt(GOBJ *a,GOBJ *b,float damage,int shield){}
static void Poison_Load(FighterData *fd,GOBJ *gobj)
{
    if(fd->ftaction==(FtAction *)fd->ftData->x14)return;
    Contact_Load(fd,gobj);
    if(!poison_observer)
    {
        poison_observer=GObj_Create(0,0,0);
        GObj_AddUserData(poison_observer,0,Poison_Destroy,poison_observer);
        // After all animation/IASA, before fighter physics. One native attack
        // instance survives charge/release and ground/air continuation states.
        GObj_AddProc(poison_observer,Poison_Frame,4);
    }
}

typedef struct {
    GOBJ *target,*item;
    int item_kind,token,left,wiggle;
    float offset;
} PortHold;
static PortHold port_hold[12];
static void Hold_Phys(GOBJ *gobj);
static int Hold_Slot(GOBJ *gobj)
{
    int i;
    if(!gobj) return -1;
    for(i=0;i<12;i++) if(Fighter_GetSubcharGObj(i/2,i%2)==gobj) return i;
    return -1;
}
static ItemData *Hold_Item(PortHold *h)
{
    GOBJ *g;
    if(!h->item) return 0;
    for(g=(*stc_gobj_lookup)[MATCHPLINK_ITEM];g;g=g->next)
        if(g==h->item && g->userdata)
        {
            ItemData *id=g->userdata;
            if(id->kind==h->item_kind && id->item_var.var6==h->token) return id;
            return 0;
        }
    return 0;
}
static PortHold *Hold_Find(GOBJ *gobj)
{
    int slot=Hold_Slot(gobj);
    PortHold *h;
    FighterData *fd;
    if(slot<0) return 0;
    h=&port_hold[slot];fd=gobj->userdata;
    if(h->target!=gobj || !h->left) return 0;
    if(fd->state_id!=ASID_FALL || fd->cb.Phys!=Hold_Phys)
    {
        // A native hit/grab/KO already owns the new state. Leave it alone.
        h->target=0;h->left=0;h->item=0;
        return 0;
    }
    return h;
}
static void Hold_Release(GOBJ *gobj)
{
    PortHold *h=Hold_Find(gobj);
    FighterData *fd;
    if(!h) return;
    fd=gobj->userdata;h->left=0;h->target=0;h->item=0;
    fd->phys.self_vel.X=fd->phys.self_vel.Y=0;
    Fighter_EnterFall(gobj);
}
static void Hold_Anim(GOBJ *gobj)
{
    PortHold *h=Hold_Find(gobj);
    if(h && (!Hold_Item(h) || h->left<=1)) Hold_Release(gobj);
    else if(h) h->left--;
}
static void Hold_IASA(GOBJ *gobj)
{
    PortHold *h=Hold_Find(gobj);
    FighterData *fd=gobj->userdata;
    int wiggle;
    if(!h) return;
    if(fd->input.down & 0xF60) h->left-=4; // fresh buttons, including shoulder inputs
    wiggle=fd->input.lstick.X>0.5f ? 1 : fd->input.lstick.X< -0.5f ? -1 : 0;
    if(wiggle && wiggle!=h->wiggle) h->left-=3;
    h->wiggle=wiggle;
    if(h->left<=0) {h->left=1;Hold_Release(gobj);}
}
static void Hold_Phys(GOBJ *gobj)
{
    PortHold *h=Hold_Find(gobj);
    ItemData *id=h ? Hold_Item(h) : 0;
    FighterData *fd=gobj->userdata;
    if(!h) return;
    if(!id) {Hold_Release(gobj);return;}
    fd->phys.self_vel.X=fd->phys.self_vel.Y=fd->phys.self_vel.Z=0;
    fd->phys.kb_vel.X=fd->phys.kb_vel.Y=fd->phys.kb_vel.Z=0;
    fd->phys.pos.X=id->pos.X;
    fd->phys.pos.Y=id->pos.Y-h->offset;
    fd->phys.pos.Z=0;
}
static void Hold_Coll(GOBJ *gobj) {}
static int Hold_Begin(GOBJ *target,GOBJ *item,int duration,float offset)
{
    int slot=Hold_Slot(target);
    FighterData *fd;
    ItemData *id;
    PortHold *h;
    if(slot<0 || !item || !item->userdata || duration<=0) return 0;
    fd=target->userdata;id=item->userdata;
    if(fd->state_id<ASID_WAIT || fd->flags.sleep || fd->grab.attacker || fd->grab.item || Hold_Find(target)) return 0;
    h=&port_hold[slot];
    if(!fd->phys.air_state) Fighter_SetAirborne(fd);
    Fighter_EnterFall(target);
    h->target=target;h->item=item;h->item_kind=id->kind;h->token=id->item_var.var6;
    h->left=duration;h->offset=offset;h->wiggle=0;
    fd->cb.Anim=Hold_Anim;fd->cb.IASA=Hold_IASA;fd->cb.Phys=Hold_Phys;fd->cb.Coll=Hold_Coll;
    Hold_Phys(target);
    return 1;
}
static int Hold_Uses(GOBJ *target,GOBJ *item)
{
    PortHold *h=Hold_Find(target);
    return h && h->item==item && Hold_Item(h)!=0;
}

#define RB_G 385
#define RB_A 386
#define RB_EG 387
#define RB_EA 388
static int bubble_token;
static int bubble_air_used[6];
static void RannoCaptureContact(GOBJ *source,GOBJ *target,float damage,int shield)
{
    ItemData *id;
    GOBJ *owner;
    if(shield || damage<=0 || !source || source->entity_class!=6 || !source->userdata)return;
    id=source->userdata;owner=id->fighter_gobj;
    if(id->state!=1 || id->item_var.var5 || !Contact_Owns(owner) || target==owner)return;
    // Native item callbacks truncate dealt damage. A staled 1% hit can become
    // zero there; the shared observer retains its real body-contact record.
    if(id->kind==MEX_GetFtItemID(owner,1) || id->kind==MEX_GetFtItemID(owner,2))
        id->item_var.var5=(int)target;
}

static void RannoBubbleLoad(FighterData *fd)
{
    ItemDesc **items=fd->ftData->items;
    bubble_air_used[fd->ply]=0;
    if(items && items[1])MEX_IndexFighterItem(fd->kind,items[1],1);
}
static GOBJ *RannoBubble(GOBJ *owner)
{
    GOBJ *g;
    int kind=MEX_GetFtItemID(owner,1);
    for(g=(*stc_gobj_lookup)[MATCHPLINK_ITEM];g;g=g->next)
    {
        ItemData *id=g->userdata;
        if(id && id->kind==kind && id->fighter_gobj==owner && !id->item_var.var8)return g;
    }
    return 0;
}
static GOBJ *RannoMakeBubble(GOBJ *gobj,Vec3 *at)
{
    GOBJ *item=RannoBubble(gobj);
    if(!item)
    {
        FighterData *fd=gobj->userdata;
        item=Make_Item(gobj,1,at);
        if(item)
        {
            ItemData *id=item->userdata;
            if(++bubble_token<=0)bubble_token=1;
            id->item_var.var6=bubble_token;
            id->item_var.var1=1;
            if(fd->phys.air_state)bubble_air_used[fd->ply]=1;
        }
    }
    return item;
}
static void RannoBubbleFrame(FighterData *fd,GOBJ *gobj)
{
    GOBJ *item=RannoBubble(gobj),*target;
    ItemData *id;
    PortPoison *p;
    int stacks;
    if(!fd->phys.air_state || fd->state_id<ASID_WAIT)bubble_air_used[fd->ply]=0;
    if(!item)return;
    id=item->userdata;
    target=(GOBJ *)id->item_var.var5;
    if(target)
    {
        id->item_var.var5=0;
        if(id->state==1)ItemStateChange(item,2,2);
        p=Poison_Target(target);stacks=p ? p->stacks : 0;
        if(target!=gobj && Hold_Begin(target,item,80+30*stacks,9.0f))
        {
            if(p)p->stacks=0;
            id->item_var.var7=(int)target;
        }
        else id->item_var.var8=1;
    }
    target=(GOBJ *)id->item_var.var7;
    if(target && !Hold_Uses(target,item))id->item_var.var8=1;
    if(fd->state_id<ASID_WAIT)id->item_var.var8=1;
}
static void RannoBubbleStart(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int state=fd->phys.air_state ? RB_A : RB_G;
    if(!RannoBubble(gobj) && fd->phys.air_state && bubble_air_used[fd->ply])
    {Enter(gobj,RB_EA);return;}
    Enter(gobj,state);
    if(RannoBubble(gobj))
    {
        ActionStateChange(25-1,1,0,gobj,state,0,0);Fighter_AdvanceScript(gobj);
        t_frame[fd->ply]=25;
    }
}
void SpecialLw(GOBJ *gobj){RannoBubbleStart(gobj);}
void SpecialAirLw(GOBJ *gobj){RannoBubbleStart(gobj);}
void RannoBubbleAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int frame=Tick(gobj);
    Vec3 at=fd->phys.pos;
    if(frame==9 && (!fd->phys.air_state || !bubble_air_used[fd->ply]))
    {
        at.X+=0.0f*fd->facing_direction;at.Y+=25.0f;at.Z=0;
        RannoMakeBubble(gobj,&at);
    }
    if(frame>=41)
    {
        ActionStateChange(25,1,0,gobj,fd->state_id,0x5001,0);
        t_frame[fd->ply]=25;
    }
}
void RannoBubbleIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    GOBJ *item;
    if(t_frame[fd->ply]<25)return;
    item=RannoBubble(gobj);
    if(!(fd->input.held & B_BUTTON) || !item)
    {
        Enter(gobj,fd->phys.air_state ? RB_EA : RB_EG);
        return;
    }
    // Native adaptation: hold B and steer the bubble with the current stick.
    ((ItemData *)item->userdata)->self_vel.X=fd->input.lstick.X*1.15f;
    ((ItemData *)item->userdata)->self_vel.Y=fd->input.lstick.Y*1.15f;
}
void RannoBubbleEndAnim(GOBJ *gobj)
{
    if(Tick(gobj)>=15)Leave(gobj,0,0);
}
void RannoBubbleColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int ending=fd->state_id==RB_EG || fd->state_id==RB_EA;
    if(!fd->phys.air_state)
    {
        if(!Fighter_CollGround_PassLedge(gobj))
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj,ending ? RB_EA : RB_A);
        }
    }
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))
    {
        Fighter_SetGrounded2(fd);
        Hand_Over(gobj,ending ? RB_EG : RB_G);
    }
}
void RannoBubbleEndIASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 11)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 11)
        Fighter_IASACheck_AllAerial(gobj);
}
void RannoBubblePhys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply], drift = 1, ground_owned = 0;
    float gravity = fd->attr.gravity, limit = fd->attr.terminal_velocity;
    float accel = fd->attr.aerial_drift_stick_mult, maxspeed = fd->attr.aerial_drift_max;
    float old_accel = accel, old_base = fd->attr.aerial_drift_base, old_max = maxspeed;
    if (fd->flags.hitlag) return;
    switch (fd->action_id) {
    case 310:
        break;
    case 319:
        break;
    case 323:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; drift = 0; }
        if (frame >= 9 && frame < 25) { gravity = 0.0f; drift = 0; }
        if (frame >= 25 && frame < 41) { gravity = 0.006788652539253236f; limit = 0.969807505607605f; drift = 0; }
        break;
    case 325:
        if (frame >= 1 && frame < 9) { gravity = 0.0f; drift = 0; }
        if (frame >= 9 && frame < 25) { gravity = 0.0f; drift = 0; }
        if (frame >= 25 && frame < 41) { gravity = 0.006788652539253236f; limit = 0.969807505607605f; drift = 0; }
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
    case 310:
        break;
    case 319:
        break;
    case 323:
        if (frame >= 7 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 7 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 9 && frame <= 24) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 9 && frame <= 24) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 325:
        if (frame >= 7 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 7 && frame <= 8) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 9 && frame <= 24) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 9 && frame <= 24) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    }
    if (ground_owned && !fd->phys.air_state)
    {
        fd->phys.horzitonal_velocity_queue_will_be_added_to_0xec = 0;
        fd->phys.vertical_velocity_queue_will_be_added_to_0xec = 0;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
}
void RannoTongueFreeIASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 1)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 1)
        Fighter_IASACheck_AllAerial(gobj);
}
void RannoTongueBasePhys(GOBJ *gobj);

#define RT_G 389
#define RT_START_A 390
#define RT_A 391
#define RT_HG 392
#define RT_HA 393
#define RT_SG 394
#define RT_SA 395
#define RT_SR 396
static Vec3 ranno_anchor[6];
static int RannoSlingState(int state){return state==RT_SG || state==RT_SA || state==RT_SR;}
static void RannoSlingBegin(GOBJ *gobj,Vec3 at)
{
    FighterData *fd=gobj->userdata;
    ranno_anchor[fd->ply]=at;
    Enter(gobj,fd->phys.air_state ? RT_SA : RT_SG);
    fd->phys.self_vel.X=fd->phys.self_vel.Y=0;
}
static void RannoSlingRelease(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    float vx=fd->phys.self_vel.X;
    if(!fd->phys.air_state)Fighter_SetAirborne(fd);
    Enter(gobj,RT_SR);
    vx*=0.8f;if(vx>1.8f)vx=1.8f;if(vx< -1.8f)vx=-1.8f;
    fd->phys.self_vel.X=vx;
    fd->phys.self_vel.Y=3.2600000000000002f;
}
static void RannoTongueLoad(FighterData *fd)
{
    ItemDesc **items=fd->ftData->items;
    ranno_anchor[fd->ply]=fd->phys.pos;
    if(items && items[2])MEX_IndexFighterItem(fd->kind,items[2],2);
}
static int RannoTongueState(int state)
{
    return state==RT_G || state==RT_START_A || state==RT_A;
}
static void RannoTongueFrame(FighterData *fd,GOBJ *gobj)
{
    GOBJ *item,*target,*bubble;
    ItemData *id,*bid;
    Vec3 at;
    int kind=MEX_GetFtItemID(gobj,2);
    float start_x,start_y;
    if(fd->flags.hitlag)return;
    for(item=(*stc_gobj_lookup)[MATCHPLINK_ITEM];item;item=item->next)
    {
        id=item->userdata;
        if(!id || id->kind!=kind || id->fighter_gobj!=gobj)continue;
        target=(GOBJ *)id->item_var.var5;
        id->item_var.var5=0;
        if(!RannoTongueState(fd->state_id)){id->item_var.var8=1;continue;}
        if(!target)
        {
            if(id->item_var.var3)
            {
                RannoSlingBegin(gobj,id->pos);id->item_var.var8=1;continue;
            }
            bubble=RannoBubble(gobj);
            if(id->state==1 && bubble)
            {
                bid=bubble->userdata;
                start_x=fd->phys.pos.X+6.800000000000001f*fd->facing_direction;
                start_y=fd->phys.pos.Y+(id->item_var.var9 ? 6.2f : 7.800000000000001f);
                if(Port_SegmentCircle(start_x,start_y,id->pos.X,id->pos.Y,bid->pos.X,bid->pos.Y,8.728267550468445f))
                {
                    at=bid->pos;
                    if(bid->item_var.var7)Hold_Release((GOBJ *)bid->item_var.var7);
                    bid->item_var.var8=1;id->item_var.var8=1;
                    RannoSlingBegin(gobj,at);continue;
                }
            }
        }
        if(!target || Hold_Slot(target)<0 || target==gobj)continue;
        if(id->state==1)ItemStateChange(item,2,2);
        // One bubble per owner: a new capture releases the previous occupant.
        bubble=RannoBubble(gobj);
        if(bubble)
        {
            bid=bubble->userdata;
            if(bid->item_var.var7)Hold_Release((GOBJ *)bid->item_var.var7);
            bid->item_var.var8=1;
        }
        at=((FighterData *)target->userdata)->phys.pos;at.Y+=9;at.Z=0;
        bubble=RannoMakeBubble(gobj,&at);
        if(bubble)
        {
            bid=bubble->userdata;bid->item_var.var1=0;bid->item_var.var2=900;
            Item_SetAirborne(bid);ItemStateChange(bubble,2,2);
            bid->item_var.var5=(int)target;
            Enter(gobj,fd->phys.air_state ? RT_HA : RT_HG);
        }
        id->item_var.var8=1;
    }
}
static void RannoTongueShoot(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    Vec3 at=fd->phys.pos;
    GOBJ *item;
    at.X+=6.800000000000001f*fd->facing_direction;at.Y+=fd->phys.air_state ? 6.2f : 7.800000000000001f;at.Z=0;
    item=Make_Item(gobj,2,&at);
    if(item)
    {
        ItemData *id=item->userdata;
        id->item_var.var1=1;id->item_var.var9=fd->phys.air_state;
    }
}
void SpecialS(GOBJ *gobj){Enter(gobj,RT_G);}
void SpecialAirS(GOBJ *gobj){Enter(gobj,RT_START_A);}
void RannoTongueAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int frame,state;
    GOBJ *bubble;
    ItemData *id;
    float dx,dy;
    if(fd->flags.hitlag)return;
    frame=Tick(gobj);state=fd->state_id;
    if(RannoSlingState(state))
    {
        if(state==RT_SR){if(frame>=25)Leave(gobj,0,0);}
        else if(frame>=17)RannoSlingRelease(gobj);
        return;
    }
    if(state==RT_HG || state==RT_HA)
    {
        bubble=RannoBubble(gobj);
        if(bubble)
        {
            id=bubble->userdata;
            if(frame<16)
            {
                dx=fd->phys.pos.X+18.6f*fd->facing_direction-id->pos.X;
                dy=fd->phys.pos.Y+15.600000000000001f-id->pos.Y;
                id->self_vel.X=dx*0.24f;id->self_vel.Y=dy*0.24f;
            }
            else id->self_vel.X=id->self_vel.Y=0;
        }
        if(frame>=65)Leave(gobj,0,0);
        return;
    }
    if(state!=RT_A && frame==21)
    {
        if(fd->phys.air_state)Enter(gobj,RT_A);
        RannoTongueShoot(gobj);
        return;
    }
    if(frame>=(state==RT_A ? 85 : 93))Leave(gobj,0,0);
}
void RannoTongueIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(RannoSlingState(fd->state_id))
    {
        if(fd->state_id==RT_SR && t_frame[fd->ply]>=4)RannoTongueFreeIASA(gobj);
        return;
    }
    if(fd->state_id==RT_HG || fd->state_id==RT_HA)return;
    if(t_frame[fd->ply]>=(fd->state_id==RT_A ? 71 : 83))
        RannoTongueFreeIASA(gobj);
}
void RannoTongueColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int state=fd->state_id,hit=state==RT_HG || state==RT_HA;
    if(RannoSlingState(state))
    {
        if(!fd->phys.air_state)
        {
            if(!Fighter_CollGround_PassLedge(gobj))Fighter_SetAirborne(fd);
        }
        else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))
        {
            Fighter_SetGrounded2(fd);
            if(state==RT_SR)Fighter_EnterWait(gobj);
            else if(t_frame[fd->ply]>=4)RannoSlingRelease(gobj);
        }
        return;
    }
    if(!fd->phys.air_state)
    {
        if(!Fighter_CollGround_PassLedge(gobj))
        {
            Fighter_SetAirborne(fd);
            if(hit)Hand_Over(gobj,RT_HA);
            else if(t_frame[fd->ply]<21)Hand_Over(gobj,RT_START_A);
        }
    }
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))
    {
        Fighter_SetGrounded2(fd);
        if(hit)Hand_Over(gobj,RT_HG);
        else Fighter_EnterSpecialLanding(gobj,0,7.0f);
    }
}
void RannoTonguePhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;Vec3 at;float dx,dy,length,speed;
    if(!RannoSlingState(fd->state_id)){RannoTongueBasePhys(gobj);return;}
    if(fd->flags.hitlag)return;
    if(fd->state_id==RT_SR)
    {
        if(fd->phys.air_state)
        {
            Fighter_PhysAir_ApplyGravityFastfall(gobj);
            fd->phys.self_vel.X=fd->phys.self_vel.X*0.95f+fd->input.lstick.X*0.08f;
        }
        else Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    if(t_frame[fd->ply]<4){fd->phys.self_vel.X=fd->phys.self_vel.Y=0;return;}
    at=ranno_anchor[fd->ply];dx=at.X-fd->phys.pos.X;dy=at.Y+4.0f-fd->phys.pos.Y;
    length=sqrtf(dx*dx+dy*dy);
    if(length<5.0f){RannoSlingRelease(gobj);return;}
    speed=length<5.8f ? length : 5.8f;
    if(!fd->phys.air_state)Fighter_SetAirborne(fd);
    fd->phys.self_vel.X=dx/length*speed;fd->phys.self_vel.Y=dy/length*speed;
}
void RannoTongueBasePhys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply], drift = 1, ground_owned = 0;
    float gravity = fd->attr.gravity, limit = fd->attr.terminal_velocity;
    float accel = fd->attr.aerial_drift_stick_mult, maxspeed = fd->attr.aerial_drift_max;
    float old_accel = accel, old_base = fd->attr.aerial_drift_base, old_max = maxspeed;
    if (fd->flags.hitlag) return;
    switch (fd->action_id) {
    case 303:
        if (frame >= 1 && frame < 21) { gravity = 0.0f; drift = 0; }
        if (frame >= 21 && frame < 41) { gravity = 0.0f; drift = 0; }
        if (frame >= 41 && frame < 51) { gravity = 0.0f; drift = 0; }
        if (frame >= 51 && frame < 93) { gravity = 0.07273556292057039f; limit = 1.745653510093689f; }
        break;
    case 308:
        if (frame >= 1 && frame < 16) { gravity = 0.0f; drift = 0; }
        if (frame >= 16 && frame < 40) { gravity = 0.0f; drift = 0; }
        if (frame >= 40 && frame < 65) { gravity = 0.0f; drift = 0; }
        break;
    case 309:
        break;
    case 312:
        if (frame >= 1 && frame < 21) { gravity = 0.0f; drift = 0; }
        if (frame >= 21 && frame < 41) { gravity = 0.0f; drift = 0; }
        if (frame >= 41 && frame < 51) { gravity = 0.0f; drift = 0; }
        if (frame >= 51 && frame < 93) { gravity = 0.07273556292057039f; limit = 1.745653510093689f; }
        break;
    case 317:
        if (frame >= 1 && frame < 16) { gravity = 0.0f; drift = 0; }
        if (frame >= 16 && frame < 40) { gravity = 0.0f; drift = 0; }
        if (frame >= 40 && frame < 65) { gravity = 0.0f; drift = 0; }
        break;
    case 318:
        if (frame >= 1 && frame < 21) { gravity = 0.0f; drift = 0; }
        if (frame >= 21 && frame < 31) { gravity = 0.0f; drift = 0; }
        if (frame >= 31 && frame < 85) { gravity = 0.07273556292057039f; limit = 1.745653510093689f; accel = 0.03103384017944336f; }
        break;
    case 324:
        if (frame >= 1 && frame < 17) { gravity = 0.0f; drift = 0; }
        break;
    case 326:
        if (frame >= 1 && frame < 17) { gravity = 0.0f; drift = 0; }
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
    case 303:
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 21 && frame <= 40) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 21 && frame <= 40) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 41 && frame <= 50) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 41 && frame <= 50) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 308:
        if (frame >= 1 && frame <= 15) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 15) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 16 && frame <= 39) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 16 && frame <= 39) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 40 && frame <= 64) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 40 && frame <= 64) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 309:
        break;
    case 312:
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 21 && frame <= 40) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 21 && frame <= 40) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 41 && frame <= 50) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 41 && frame <= 50) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 317:
        if (frame >= 1 && frame <= 15) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 15) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 16 && frame <= 39) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 16 && frame <= 39) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 40 && frame <= 64) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 40 && frame <= 64) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 318:
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 1 && frame <= 20) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        if (frame >= 21 && frame <= 30) { if (fd->phys.air_state) fd->phys.self_vel.Y = 0.0f; }
        if (frame >= 21 && frame <= 30) { if (fd->phys.air_state) fd->phys.self_vel.X = 0.0f * fd->facing_direction; else { fd->phys.self_vel_ground.X = 0.0f * fd->facing_direction; ground_owned = 1; } }
        break;
    case 324:
        break;
    case 326:
        break;
    }
    if (ground_owned && !fd->phys.air_state)
    {
        fd->phys.horzitonal_velocity_queue_will_be_added_to_0xec = 0;
        fd->phys.vertical_velocity_queue_will_be_added_to_0xec = 0;
        Fighter_PhysGround_ApplyVelocity(gobj);
    }
}
void RannoRapidFreeIASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 17)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 17)
        Fighter_IASACheck_AllAerial(gobj);
}

#define RJ_LOOP 397
#define RJ_END 398
static int ranno_palm_instance[6],ranno_palm_action[6],ranno_palm_fired[6];
static void RannoNormalsLoad(FighterData *fd)
{
    ItemDesc **items=fd->ftData->items;
    ranno_palm_instance[fd->ply]=ranno_palm_action[fd->ply]=-1;ranno_palm_fired[fd->ply]=0;
    if(items && items[4])MEX_IndexFighterItem(fd->kind,items[4],4);
}
static void RannoNormalsFrame(FighterData *fd,GOBJ *gobj)
{
    int p=fd->ply;Vec3 at;
    if(fd->flags.hitlag)return;
    if(fd->action_id!=ranno_palm_action[p] || fd->atk_instance!=ranno_palm_instance[p])ranno_palm_fired[p]=0;
    ranno_palm_action[p]=fd->action_id;ranno_palm_instance[p]=fd->atk_instance;
    if(fd->action_id==62 && fd->state.frame>=15 && !ranno_palm_fired[p])
    {
        ranno_palm_fired[p]=1;at=fd->phys.pos;at.X+=19.5f*fd->facing_direction;at.Y+=10.5f;
        Port_FireFan(gobj,4,at,1,0,0,3.2f,0);
    }
    if(fd->action_id==48 && fd->state_id==46 && fd->state.frame>=7 && (fd->input.held&HSD_BUTTON_A))
        Enter(gobj,RJ_LOOP);
}
void RannoRapidAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int frame;
    if(fd->flags.hitlag)return;
    frame=Tick(gobj);
    if(fd->state_id==RJ_END){if(frame>=29)Leave(gobj,0,0);return;}
    if(frame>=41)
    {
        if(fd->input.held&HSD_BUTTON_A){ActionStateChange(0,1,0,gobj,RJ_LOOP,0x5001,0);Fighter_AdvanceScript(gobj);t_frame[fd->ply]=1;}
        else Enter(gobj,RJ_END);
    }
}
void RannoRapidIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(fd->state_id==RJ_END)RannoRapidFreeIASA(gobj);
}
void RannoRapidPhys(GOBJ *gobj){Fighter_PhysGround_ApplyFriction(gobj);}
void RannoRapidColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(!Fighter_CollGround_PassLedge(gobj)){Fighter_SetAirborne(fd);Fighter_EnterFall(gobj);}
}
// The face: parts of the model shown and hidden. The scripts switch what their own clips switch; here the
// fighter's defaults (its resting face) are set, and the resting clip's own switches (a blink) are played on
// a clock that runs on through every action, for the groups an action's clip leaves alone.
#define FACE_GROUPS 2
static const s8 face_rest[FACE_GROUPS] = {0, 0};
typedef struct { short frame; s8 group, choice; } FaceKey;
static const FaceKey face_clock[] = {
    {32767, 0, 0},
};
#define FACE_CLOCK_END 0
static const u16 face_left[] = {0};
// (a fighter made for a screen that is not a match plays the rows of its other table of actions)
static const u16 face_left_other[] = {0};
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

typedef struct { float frame, alpha; } OpacityKey;
static float OpacityAt(const OpacityKey *keys, int count, float at)
{
    int i;
    if (!(at > keys[0].frame)) return keys[0].alpha;
    for (i = 1; i < count; ++i)
        if (at < keys[i].frame)
            return keys[i-1].alpha + (keys[i].alpha - keys[i-1].alpha) *
                (at - keys[i-1].frame) / (keys[i].frame - keys[i-1].frame);
    return keys[count-1].alpha;
}
static const OpacityKey opacity_keys_0[] = {{0.0f, 1.0f}, {12.0f, 1.0f}, {13.0f, 0.500000649606955f}, {14.0f, 0.0f}, {53.0f, 0.0f}, {65.0f, 1.0f}, {67.0f, 1.0f}};
static const OpacityKey opacity_keys_1[] = {{0.0f, 1.0f}, {5.0f, 1.0f}, {6.0f, 0.9841463414634164f}, {7.0f, 0.8073170731707319f}, {8.0f, 0.4516260162601611f}, {9.0f, 0.12682926829268543f}, {10.0f, 0.0f}, {30.0f, 0.0f}, {31.0f, 0.03939965482500397f}, {43.0f, 0.9774859294099604f}, {44.0f, 1.0f}, {65.0f, 1.0f}};
static const OpacityKey opacity_keys_2[] = {{0.0f, 0.0f}, {96.0f, 0.0f}, {97.0f, 0.24999866865590023f}, {98.0f, 0.7500001323530702f}, {99.0f, 1.0f}, {120.0f, 1.0f}};
static const OpacityKey opacity_keys_3[] = {{0.0f, 0.849999286363947f}, {2.0f, 0.15000039500079781f}, {3.0f, 0.0f}, {41.0f, 0.0f}};
static const OpacityKey opacity_keys_4[] = {{0.0f, 0.0f}, {4.0f, 0.0f}, {5.0f, 0.025000160660188092f}, {6.0f, 0.05000011204373041f}, {7.0f, 0.050000108636933015f}, {8.0f, 0.10000031840674592f}, {9.0f, 0.14999999954557394f}, {10.0f, 0.2000001506817203f}, {11.0f, 0.2500000990910651f}, {12.0f, 0.32604187307313154f}, {13.0f, 0.40000023011374153f}, {14.0f, 0.4531252482342232f}, {15.0f, 0.5000003463635004f}, {16.0f, 0.5500003500004546f}, {17.0f, 0.6260421251043488f}, {18.0f, 0.7000004122158369f}, {19.0f, 0.7531253981443955f}, {20.0f, 0.8000004920457161f}, {22.0f, 0.9000001943187146f}, {23.0f, 0.9500002477216326f}, {24.0f, 0.9500000482954627f}, {25.0f, 1.0f}, {29.0f, 1.0f}};
static const OpacityKey opacity_keys_5[] = {{0.0f, 3.4200481238151706e-06f}, {1.0f, 0.0f}, {2.0f, 0.049999344003431784f}, {3.0f, 0.21249929066580342f}, {4.0f, 0.399999449999936f}, {5.0f, 0.5999989919783213f}, {6.0f, 0.7999998110026221f}, {7.0f, 0.9499990880060122f}, {8.0f, 1.0f}, {46.0f, 1.0f}};
static const OpacityKey opacity_keys_6[] = {{0.0f, 0.0f}, {17.0f, 0.0f}, {18.0f, 0.025000100333540373f}, {19.0f, 0.05833339433316467f}, {20.0f, 0.10000002f}, {21.0f, 0.1750002399977675f}, {22.0f, 0.24999958000239905f}, {23.0f, 0.3500000200000027f}, {25.0f, 0.549999549999655f}, {26.0f, 0.6531246549995671f}, {27.0f, 0.7499998249997163f}, {28.0f, 0.832812479791501f}, {29.0f, 0.9000001f}, {30.0f, 0.9432292697916123f}, {31.0f, 0.9750000666665702f}, {32.0f, 1.0f}, {34.0f, 1.0f}};
static const OpacityKey opacity_keys_7[] = {{0.0f, 0.8f}, {4.0f, 0.0f}, {26.0f, 0.0f}, {27.0f, 0.024999955000060185f}, {28.0f, 0.0729166354166281f}, {29.0f, 0.12499998200001908f}, {31.0f, 0.22499998500015347f}, {32.0f, 0.2999997840021188f}, {34.0f, 0.44999991600080713f}, {35.0f, 0.5500000524994408f}, {37.0f, 0.6999997679999198f}, {38.0f, 0.764583169166389f}, {39.0f, 0.8249999090000207f}, {41.0f, 0.9249999149998421f}, {42.0f, 0.9666666376665871f}, {43.0f, 1.0f}, {45.0f, 1.0f}};
static const OpacityKey opacity_keys_8[] = {{0.0f, 1.0f}, {5.0f, 0.0f}, {19.0f, 0.0f}, {20.0f, 0.1428573885839739f}, {23.0f, 0.8928576928039438f}, {24.0f, 1.0f}, {41.0f, 1.0f}};
static const OpacityKey opacity_keys_9[] = {{0.0f, 1.0f}, {5.0f, 0.0f}, {19.0f, 0.0f}, {20.0f, 0.14285741412881325f}, {23.0f, 0.8928576959970334f}, {24.0f, 1.0f}, {41.0f, 1.0f}};
static const OpacityKey opacity_keys_10[] = {{0.0f, 0.0f}, {64.0f, 0.0f}};
static const OpacityKey opacity_keys_11[] = {{0.0f, 0.0f}, {38.0f, 0.0f}, {39.0f, 0.16666412252286095f}, {44.0f, 0.9999973693060505f}, {45.0f, 1.0f}, {64.0f, 1.0f}};
static void OpacityFrame(FighterData *fd, GOBJ *gobj)
{
    int a = fd->action_id;
    float at = fd->state.frame;
    if (!fd->dobj_lookup) return;
    float opacity_0 = 1.0f;
    if (fd->ftaction == fd->ftData->ftaction)
        switch (a)
        {
        case 62:
            opacity_0 = OpacityAt(opacity_keys_0, 7, at);
            break;
        case 67:
            opacity_0 = OpacityAt(opacity_keys_1, 12, at);
            break;
        case 238:
            opacity_0 = OpacityAt(opacity_keys_2, 6, at);
            break;
        case 297:
            opacity_0 = OpacityAt(opacity_keys_3, 4, at);
            break;
        case 298:
            opacity_0 = OpacityAt(opacity_keys_3, 4, at);
            break;
        case 301:
            opacity_0 = OpacityAt(opacity_keys_4, 23, at);
            break;
        case 302:
            opacity_0 = OpacityAt(opacity_keys_4, 23, at);
            break;
        case 311:
            opacity_0 = OpacityAt(opacity_keys_5, 10, at);
            break;
        case 316:
            opacity_0 = OpacityAt(opacity_keys_6, 17, at);
            break;
        case 320:
            opacity_0 = OpacityAt(opacity_keys_5, 10, at);
            break;
        case 321:
            opacity_0 = OpacityAt(opacity_keys_7, 17, at);
            break;
        case 322:
            opacity_0 = OpacityAt(opacity_keys_7, 17, at);
            break;
        case 323:
            opacity_0 = OpacityAt(opacity_keys_8, 7, at);
            break;
        case 325:
            opacity_0 = OpacityAt(opacity_keys_9, 7, at);
            break;
        }
    if (fd->ftaction == (FtAction *)(fd->ftData->x14))
        switch (a)
        {
        case 0:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 1:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 2:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 3:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 5:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 6:
            opacity_0 = OpacityAt(opacity_keys_10, 2, at);
            break;
        case 7:
            opacity_0 = OpacityAt(opacity_keys_11, 6, at);
            break;
        }
    Fighter_SetVisGroupCurrent(gobj, 1, opacity_0 > 0.0f ? 0 : -1);
    if (fd->dobj_lookup[0] && fd->dobj_lookup[0]->mobj && fd->dobj_lookup[0]->mobj->mat)
        fd->dobj_lookup[0]->mobj->mat->alpha = opacity_0;
    if (fd->dobj_lookup[5] && fd->dobj_lookup[5]->mobj && fd->dobj_lookup[5]->mobj->mat)
        fd->dobj_lookup[5]->mobj->mat->alpha = opacity_0;
}
static void OpacityProc(GOBJ *gobj)
{
    OpacityFrame(gobj->userdata, gobj);
}
static void DemoContinue(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if (fd->ftaction != (FtAction *)(fd->ftData->x14)) return;
    switch (fd->action_id)
    {
    case 0:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,1,0,0);
        break;
    case 1:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,3,0,0);
        break;
    case 2:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,1,0,0);
        break;
    case 3:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,6,0,0);
        break;
    case 5:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,1,0,0);
        break;
    case 6:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,7,0,0);
        break;
    case 7:
        if (fd->state.frame >= 64.0f)
            ActionStateChange(fd->state.frame-64.0f,1,0,gobj,8,0,0);
        break;
    }
}


typedef struct {int action,state,first,end,target,neutral;} JabWindow;
static const JabWindow jab_windows[] = {
    {46, 44, 10, 19, 45, 0},
    {47, 45, 10, 21, 46, 0},
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
    {48, 1, 4, 2, 0, 2.230557262897492f, 0.0f},
    {52, 1, 8, 4, 0, 3.103384017944336f, 0.0f},
    {52, 9, 11, 1, 0, 0.0f, 0.0f},
    {58, 3, 8, 2, 0, -1.1637690067291262f, 0.0f},
    {62, 9, 14, 1, 0, 2.376028388738632f, 0.0f},
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
    if (!(s == 373 || s == 374 || s == 377 || s == 378 || s == 385 || s == 386 || s == 389 || s == 390) || fd->state.frame > 4.0f)
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
        RannoChargeIASA(gobj);
}

void Reverse_IASA_374(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoChargeIASA(gobj);
}

void Reverse_IASA_377(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoRecoveryIASA(gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoRecoveryIASA(gobj);
}

void Reverse_IASA_385(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoBubbleIASA(gobj);
}

void Reverse_IASA_386(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoBubbleIASA(gobj);
}

void Reverse_IASA_389(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoTongueIASA(gobj);
}

void Reverse_IASA_390(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        RannoTongueIASA(gobj);
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    RannoLoad(fd);
    Poison_Load(fd,gobj);
    RannoBubbleLoad(fd);
    RannoTongueLoad(fd);
    RannoNormalsLoad(fd);
    FacesLoad(gobj);
    GObj_AddProc(gobj, OpacityProc, 23);
    if (fd->ftaction == (FtAction *)(fd->ftData->x14)) GObj_AddProc(gobj,DemoContinue,2);
    Jab_Load(fd);
    Momentum_Load(fd);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    RannoStoredChargeFrame(fd);
    RannoBubbleFrame(fd,gobj);
    RannoTongueFrame(fd,gobj);
    RannoNormalsFrame(fd,gobj);
    FacesFrame(fd, gobj);
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
    {306, 0x340211, 0x12, 0x0, RannoChargeAnim, Reverse_IASA_373, RannoPhys, RannoColl, Fighter_UpdateCameraBox},    // 373: Ranno needle specialn
    {315, 0x340611, 0x12, 0x0, RannoChargeAnim, Reverse_IASA_374, RannoPhys, RannoColl, Fighter_UpdateCameraBox},    // 374: Ranno needle specialairn
    {307, 0x340211, 0x12, 0x0, RannoThrowAnim, RannoThrowIASA, RannoPhys, RannoColl, Fighter_UpdateCameraBox},    // 375: Ranno needle branch_nspecial_throw_startup_d711c748
    {316, 0x340611, 0x12, 0x0, RannoThrowAnim, RannoThrowIASA, RannoPhys, RannoColl, Fighter_UpdateCameraBox},    // 376: Ranno needle branch_nspecialair_throw_startup_c601ea25
    {321, 0x340213, 0x14, 0x0, RannoRecoveryAnim, Reverse_IASA_377, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 377: Ranno recovery rise
    {322, 0x340613, 0x14, 0x0, RannoRecoveryAnim, Reverse_IASA_378, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 378: Ranno recovery rise_air
    {305, 0x340213, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 379: Ranno recovery dive
    {314, 0x340613, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 380: Ranno recovery dive_air
    {304, 0x340213, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 381: Ranno recovery spin
    {313, 0x340613, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 382: Ranno recovery spin_air
    {311, 0x340213, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 383: Ranno recovery land
    {320, 0x340613, 0x14, 0x0, RannoRecoveryAnim, RannoRecoveryIASA, RannoRecoveryPhys, RannoRecoveryColl, Fighter_UpdateCameraBox},    // 384: Ranno recovery land_air
    {323, 0x340214, 0x15, 0x0, RannoBubbleAnim, Reverse_IASA_385, RannoBubblePhys, RannoBubbleColl, Fighter_UpdateCameraBox},    // 385: Ranno bubble ground
    {325, 0x340614, 0x15, 0x0, RannoBubbleAnim, Reverse_IASA_386, RannoBubblePhys, RannoBubbleColl, Fighter_UpdateCameraBox},    // 386: Ranno bubble air
    {310, 0x340214, 0x15, 0x0, RannoBubbleEndAnim, RannoBubbleEndIASA, RannoBubblePhys, RannoBubbleColl, Fighter_UpdateCameraBox},    // 387: Ranno bubble end_ground
    {319, 0x340614, 0x15, 0x0, RannoBubbleEndAnim, RannoBubbleEndIASA, RannoBubblePhys, RannoBubbleColl, Fighter_UpdateCameraBox},    // 388: Ranno bubble end_air
    {303, 0x340212, 0x13, 0x0, RannoTongueAnim, Reverse_IASA_389, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 389: Ranno tongue ground
    {312, 0x340612, 0x13, 0x0, RannoTongueAnim, Reverse_IASA_390, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 390: Ranno tongue air_start
    {318, 0x340612, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 391: Ranno tongue air
    {308, 0x340212, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 392: Ranno tongue hit_ground
    {317, 0x340612, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 393: Ranno tongue hit_air
    {324, 0x340212, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 394: Ranno tongue sling_ground
    {326, 0x340612, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 395: Ranno tongue sling_air
    {309, 0x340212, 0x13, 0x0, RannoTongueAnim, RannoTongueIASA, RannoTonguePhys, RannoTongueColl, Fighter_UpdateCameraBox},    // 396: Ranno tongue sling_recovery
    {297, 0x2c0203, 0x4, 0x0, RannoRapidAnim, RannoRapidIASA, RannoRapidPhys, RannoRapidColl, Fighter_UpdateCameraBox},    // 397: Ranno rapid branch_jab_startup_4_117e944c
    {301, 0x2c0203, 0x4, 0x0, RannoRapidAnim, RannoRapidIASA, RannoRapidPhys, RannoRapidColl, Fighter_UpdateCameraBox},    // 398: Ranno rapid branch_jab_recovery_4_717738f4
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
