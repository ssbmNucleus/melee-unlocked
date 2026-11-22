#include "../mex.h"

static void BCM_SideEnter(GOBJ *gobj,int state);
static void BCM_BowEnter(GOBJ *gobj,int state);
#define ST_COUNTER 381
#define ST_COUNTER_AIR 382

static void BCM_DownEnter(GOBJ *gobj,int state);
static void BCM_DownHurt(FighterData *fd);
static void BCM_DownCleanup(FighterData *fd);
static void BCM_NeutralEnter(GOBJ *gobj,int state);
static void BCM_SpecialHitProperties(FighterData *fd);
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

// ---- the neutral special: PLAIN (its clip, its script's hits, its end) ---------------------------
#define ST_N 373                        // its state on the ground,
#define ST_N_AIR 374                //   and in the air
#define N_END 27                   // the natural end, on the ground (NEVER: at the clip's end),
#define N_END_AIR 27           //   and in the air
void SpecialN(GOBJ *gobj)
{
    BCM_NeutralEnter(gobj, ST_N);
}
void SpecialAirN(GOBJ *gobj)
{
    BCM_NeutralEnter(gobj, ST_N_AIR);
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
#define S_END 59                   // the natural end, on the ground,
#define S_END_AIR 59           //   and in the air
void SpecialS(GOBJ *gobj)
{
    BCM_SideEnter(gobj, ST_S);
}
void SpecialAirS(GOBJ *gobj)
{
    BCM_SideEnter(gobj, ST_S_AIR);
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
#define HI_END 66                 // the natural end, on the ground,
#define HI_END_AIR 66         //   and in the air
#define HI_LANDING 29.0f         // frames of landing after the helpless fall (the game's list of motions)
void SpecialHi(GOBJ *gobj)
{
    BCM_BowEnter(gobj, ST_HI);
}
void SpecialAirHi(GOBJ *gobj)
{
    BCM_BowEnter(gobj, ST_HI_AIR);
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
#define LW_END 30000                 // the natural end, on the ground,
#define LW_END_AIR 30000         //   and in the air
void SpecialLw(GOBJ *gobj)
{
    BCM_DownEnter(gobj, ST_LW);
}
void SpecialAirLw(GOBJ *gobj)
{
    BCM_DownEnter(gobj, ST_LW_AIR);
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

static float SlapAbs(float x){return x < 0 ? -x : x;}
static float SlapMin(float a,float b){return a < b ? a : b;}
static float SlapMax(float a,float b){return a > b ? a : b;}
static float SlapClamp(float x,float lo,float hi){return x < lo ? lo : x > hi ? hi : x;}
static float SlapLerp(float a,float b,float t){return a+(b-a)*SlapClamp(t,0,1);}
static float SlapDiv(float a,float b){return b==0 ? 0 : a/b;}
static float SlapMoveTo(float a,float b,float step){return a+SlapClamp(b-a,-SlapMax(0,step),SlapMax(0,step));}


// Match the script writer's signed-byte range and nearest/even rounding.
// Always pass an immutable source offset or a freshly computed effective value.
static int Native_ShieldField(float value)
{
    int whole; float fraction;
    if(value>=127)return 127;
    if(value<=-128)return -128;
    whole=(int)value;fraction=value-whole;
    if(fraction>0.5f || (fraction==0.5f && whole%2))whole++;
    else if(fraction<-0.5f || (fraction==-0.5f && whole%2))whole--;
    return whole;
}


static void BCM_Hit(GOBJ *,GOBJ *,float,int);
static void BCM_Hurt(GOBJ *,GOBJ *,float,int);
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
            BCM_Hit(e->source,e->target,e->damage,e->shield);
        if (Contact_Owns(e->target))
            BCM_Hurt(e->target,e->source,e->damage,e->shield);
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

typedef struct {
    float cash,transaction,buy,sell,hurt_mult,last_hurt,damage_mult,kb_mult,frame;
    int action,state,hit_mode,hurt_bound,sold,dead,registered,prop;
} BCMResource;
static BCMResource bcm_resource[6];
static void BCM_Hit(GOBJ *owner,GOBJ *other,float damage,int shield)
{
    FighterData *fd = owner->userdata;
    BCMResource *r = &bcm_resource[fd->ply];
    if (r->action != fd->action_id || r->state != fd->state_id) return;
    if (r->hit_mode == 1 && !shield)
        r->cash = SlapMin(1000.0f,r->cash+r->buy+r->last_hurt*r->hurt_mult);
    if (r->hit_mode == 2 && !r->sold)
    {
        r->cash = SlapMax(0,r->cash-r->sell);
        r->sold = 1;
    }
}
static void BCM_Hurt(GOBJ *owner,GOBJ *other,float damage,int shield)
{
    FighterData *fd = owner->userdata;
    BCMResource *r = &bcm_resource[fd->ply];
    if (r->action != fd->action_id || r->state != fd->state_id || shield) return;
    r->last_hurt = damage;
    BCM_DownHurt(fd);
    if (r->hurt_bound)
        r->cash = SlapMin(1000.0f,r->cash+r->buy+damage*r->hurt_mult);
}
static int BCM_Crossed(BCMResource *r,float f,int at){return r->frame < at && f >= at;}
static void BCM_ResourceUpdate(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    BCMResource *r = &bcm_resource[fd->ply];
    float f = fd->state.frame;
    float charge = SlapClamp(fd->smash.frame / 60.0f,0,1);
    float full_charge = charge >= 1;
    int i;
    if (fd->flags.sleep) return;
    BCM_DownCleanup(fd);
    if (fd->flags.dead && !r->dead) r->cash *= 0.5f; // source moneydeath
    r->dead = fd->flags.dead;
    if (fd->flags.hitlag) return;
    if (r->action != fd->action_id || r->state != fd->state_id || f < r->frame)
    {
        r->action = fd->action_id; r->state = fd->state_id; r->frame = -1;
        r->hit_mode = r->hurt_bound = r->sold = r->prop = 0;
        r->last_hurt = 0; r->damage_mult = r->kb_mult = 1;
    }
    switch (fd->action_id)
    {
case 46:
if (BCM_Crossed(r,f,5) && 1) { r->transaction = 30.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 52:
if (BCM_Crossed(r,f,7) && 1) { r->transaction = 40.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 55:
if (BCM_Crossed(r,f,7) && 1) { r->transaction = 40.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 62:
if (BCM_Crossed(r,f,12) && 1) { r->damage_mult = (1.0f + SlapLerp(0.0f,0.25f,SlapClamp(charge,0.0f,1.0f))); }
if (BCM_Crossed(r,f,12) && (full_charge == 1.0f)) { r->kb_mult = 1.25f; }
if (BCM_Crossed(r,f,18) && ((r->cash >= 400.0f) && (((fd->input.held & PAD_TRIGGER_Z) != 0) == 0.0f))) { r->transaction = 400.0f; r->damage_mult *= 2.0f; r->sell = r->transaction; r->sold = 0; r->hit_mode = 2; r->prop = 1; }
break;
case 66:
if (BCM_Crossed(r,f,9) && 1) { r->damage_mult = (1.0f + SlapLerp(0.0f,0.25f,SlapClamp(charge,0.0f,1.0f))); }
if (BCM_Crossed(r,f,9) && (full_charge == 1.0f)) { r->kb_mult = 1.25f; }
if (BCM_Crossed(r,f,13) && ((r->cash >= 300.0f) && (((fd->input.held & PAD_TRIGGER_Z) != 0) == 0.0f))) { r->transaction = 300.0f; r->damage_mult *= 2.0f; r->sell = r->transaction; r->sold = 0; r->hit_mode = 2; }
break;
case 67:
if (BCM_Crossed(r,f,8) && 1) { r->damage_mult = (1.0f + SlapLerp(0.0f,0.25f,SlapClamp(charge,0.0f,1.0f))); }
if (BCM_Crossed(r,f,8) && (full_charge == 1.0f)) { r->kb_mult = 1.25f; }
if (BCM_Crossed(r,f,14) && 1) { r->transaction = (20.0f * (1.0f + charge)); r->buy = r->transaction; r->hit_mode = 1; }
break;
case 69:
if (BCM_Crossed(r,f,17) && ((r->cash >= 200.0f) && (((fd->input.held & PAD_TRIGGER_Z) != 0) == 0.0f))) { r->transaction = 200.0f; r->damage_mult = 1.6f; r->sell = r->transaction; r->sold = 0; r->hit_mode = 2; }
break;
case 71:
if (BCM_Crossed(r,f,11) && 1) { r->transaction = 15.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 72:
if (BCM_Crossed(r,f,11) && 1) { r->transaction = 10.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 245:
if (BCM_Crossed(r,f,5) && 1) { r->cash += 10.0f; r->cash = SlapClamp(r->cash,0.0f,1000.0f); }
break;
case 250:
if (BCM_Crossed(r,f,28) && 1) { r->buy = r->transaction; r->hit_mode = 1; r->cash += 30.0f; r->cash = SlapClamp(r->cash,0.0f,1000.0f); }
break;
case 295:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 296:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 297:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 299:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 300:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 301:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 306:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
case 315:
if (BCM_Crossed(r,f,9) && 1) { r->transaction = 50.0f; r->buy = r->transaction; r->hit_mode = 1; }
break;
    }
    // Reapply from source base values, never multiply an already modified box on later updates.
    switch (fd->action_id)
    {
case 46:
if (f >= 5 && f < 7) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 0; h->kb_growth = (int)(0 * r->kb_mult); h->kb = (int)(28 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 52:
if (f >= 7 && f < 10) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(8.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(60 * r->kb_mult); h->kb = (int)(66 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 55:
if (f >= 7 && f < 10) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(8.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(60 * r->kb_mult); h->kb = (int)(66 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 62:
if (f >= 18 && f < 22) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(12.0f*r->damage_mult+0.5f),gobj); h->angle = (r->prop ? 45 : 45); h->kb_growth = (int)((r->prop ? 66 : 66) * r->kb_mult); h->kb = (int)((r->prop ? 72 : 72) * r->kb_mult); h->wdsk = (r->prop ? 0 : 0); h->shield_dmg = Native_ShieldField((r->prop ? 12.0f : 0.0f) * r->damage_mult); }}
break;
case 66:
if (f >= 13 && f < 16) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(12.0f*r->damage_mult+0.5f),gobj); h->angle = 60; h->kb_growth = (int)(57 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 16 && f < 19) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(12.0f*r->damage_mult+0.5f),gobj); h->angle = 315; h->kb_growth = (int)(51 * r->kb_mult); h->kb = (int)(36 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 67:
if (f >= 14 && f < 15) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 7; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 15 && f < 16) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 7; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 16 && f < 17) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 10; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 17 && f < 18) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 10; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 18 && f < 19) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 10; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 19 && f < 20) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 10; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 20 && f < 21) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 35; h->kb_growth = (int)(87 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 69:
if (f >= 17 && f < 20) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(11.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(75 * r->kb_mult); h->kb = (int)(44 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 71:
if (f >= 11 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 90; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 13 && f < 15) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 90; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 15 && f < 17) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 90; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 80; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 17 && f < 18) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 20; h->kb_growth = (int)(60 * r->kb_mult); h->kb = (int)(64 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 72:
if (f >= 11 && f < 12) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 12 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 13 && f < 14) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 14 && f < 15) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 15 && f < 16) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 16 && f < 17) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 17 && f < 18) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 18 && f < 19) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 270; h->kb_growth = (int)(30 * r->kb_mult); h->kb = (int)(0 * r->kb_mult); h->wdsk = 60; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
if (f >= 19 && f < 20) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(2.0f*r->damage_mult+0.5f),gobj); h->angle = 330; h->kb_growth = (int)(72 * r->kb_mult); h->kb = (int)(40 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 245:

break;
case 250:
if (f >= 28 && f < 29) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(10.0f*r->damage_mult+0.5f),gobj); h->angle = 30; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(40 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(0.0f * r->damage_mult); }}
break;
case 295:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 296:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 297:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 299:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 300:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 301:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 306:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
case 315:
if (f >= 9 && f < 13) { fd->smash.dmg_mult = 1; for (i=0;i<4;i++) { ftHit *h = &fd->hitbox[i]; if (!h->active) continue; Fighter_UpdateHitboxDamage(h,(int)(6.0f*r->damage_mult+0.5f),gobj); h->angle = 45; h->kb_growth = (int)(54 * r->kb_mult); h->kb = (int)(56 * r->kb_mult); h->wdsk = 0; h->shield_dmg = Native_ShieldField(-3.0f * r->damage_mult); }}
break;
    }
    BCM_SpecialHitProperties(fd);
    r->frame = f;
}
static void BCM_ResourceLoad(FighterData *fd,GOBJ *gobj)
{
    BCMResource zero = {0};
    bcm_resource[fd->ply] = zero;
    bcm_resource[fd->ply].action = bcm_resource[fd->ply].state = -1;
    Contact_Load(fd,gobj);
}
static void BCM_ResourceFrame(FighterData *fd,GOBJ *gobj)
{
    if (!bcm_resource[fd->ply].registered)
    {
        bcm_resource[fd->ply].registered = 1;
        GObj_AddProc(gobj,BCM_ResourceUpdate,FTPRI_IASA);
    }
}

static void BCM_MeterValue(FighterData *,int *,int *);
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
        BCM_MeterValue(fd,&value,&mode);
        if (value != m->value)
        {
            Text_SetText(text,m->sub,"Cash %d",value);
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
    m->sub = Text_AddSubtext(text,0,0,"Cash %d",0);
    text->viewport_scale.X = text->viewport_scale.Y = 0.05f;
    // Text_CreateText already installed its userdata; replace only its destructor,
    // then delegate that exact native cleanup in Meter_Destroy.
    text->gobj->destructor_function = (int)Meter_Destroy;
    GObj_AddProc(text->gobj,Meter_Update,17);
    Meter_Update(text->gobj);
}
static void BCM_MeterValue(FighterData *fd,int *value,int *mode)
{ BCMResource *r=&bcm_resource[fd->ply]; *value=(int)r->cash;
  *mode=(r->action==fd->action_id && r->state==fd->state_id) ? r->hit_mode : 0; }

typedef struct { int active,armed,loops,next,recovery,hits; } BCMDown;
static BCMDown bcm_down[6];
static void BCM_HandOver(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata;
    // Continue the source move: keep fastfall, current hitboxes/hit logs and script hurt status.
    ActionStateChange(fd->state.frame,1,0,gobj,state,0x500d,0);
}
// Melee's ordinary no-reaction flag, NOT no_reaction_always (the latter is boss-only).
// fighter.c clears x2220_b3 on every action-state change. Damage/hitlag still run normally.
static void BCM_Armour(FighterData *fd,int on)
{
    unsigned char *flags=(unsigned char *)&fd->flags.can_input_multijump;
    if (on) *flags |= 0x10; else *flags &= ~0x10;
}
static int BCM_IsDown(FighterData *fd) { return fd->state_id==ST_LW || fd->state_id==ST_LW_AIR; }
static void BCM_DownCleanup(FighterData *fd)
{
    if (bcm_down[fd->ply].active && !BCM_IsDown(fd))
    {
        bcm_down[fd->ply].active=0;
        bcm_resource[fd->ply].hurt_mult=0; // spd's SAOC, before the next move binds its own income
    }
}
static void BCM_DownSync(FighterData *fd)
{
    BCMResource *r=&bcm_resource[fd->ply];
    r->action=fd->action_id; r->state=fd->state_id; r->frame=fd->state.frame;
    BCM_Armour(fd,bcm_down[fd->ply].armed && !bcm_down[fd->ply].recovery);
}
static void BCM_DownEnter(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata;
    BCMDown zero={0}; BCMResource *r=&bcm_resource[fd->ply];
    bcm_down[fd->ply]=zero;
    bcm_down[fd->ply].active=1; bcm_down[fd->ply].loops=5;
    bcm_down[fd->ply].next=37;
    Enter(gobj,state);
    r->hit_mode=r->hurt_bound=r->sold=r->prop=0;
    r->last_hurt=0; r->damage_mult=r->kb_mult=1;
    BCM_DownSync(fd);
}
static void BCM_DownHurt(FighterData *fd)
{
    BCMDown *d=&bcm_down[fd->ply];
    if (BCM_IsDown(fd) && d->active && d->armed && !d->recovery) d->hits++;
}
void BCM_DownAnim(GOBJ *gobj) { Tick(gobj); }
void BCM_DownIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMDown *d=&bcm_down[fd->ply]; BCMResource *r=&bcm_resource[fd->ply];
    int f=t_frame[fd->ply];
    if (!d->active) return;
    if (d->recovery)
    {
        if (f>=d->next) { d->active=0; r->hurt_mult=0; Leave(gobj,0,0); }
        return;
    }
    if (!d->armed && f>=8)
    {
        d->armed=1; d->hits=0; r->transaction=r->buy=5.0f;
        r->hurt_mult=8.0f; r->hurt_bound=1; r->hit_mode=1;
    }
    BCM_Armour(fd,d->armed);
    if (f >= 13 && ((r->cash >= 200.0f) && ((fd->input.down & (B_BUTTON | 0x100)) != 0) && (d->hits > 0.0f))) { r->cash += -200.0f; r->cash = SlapClamp(r->cash,0.0f,1000.0f); BCM_NeutralEnter(gobj,fd->phys.air_state ? ST_COUNTER_AIR : ST_COUNTER); return; }
    if (f>=d->next)
    {
        if ((fd->input.held & B_BUTTON) && d->loops>0)
        {
            d->loops--; d->next+=16;
            ActionStateChange(20.0f,1,0,gobj,fd->state_id,0x5001,0);
            BCM_DownSync(fd); // clip rewinds do not restart the source program or its resource binding
            ((void (*)(FighterData *,int,int,int))0x80088510)(fd,5070,127,64);
        }
        else
        {
            d->recovery=1; d->next=f+25; d->loops=0;
            BCM_Armour(fd,0); r->hurt_bound=0; r->hit_mode=0;
            fd->phys.self_vel.X=0; fd->phys.self_vel_ground.X=0;
        }
    }
}
void BCM_DownPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if (fd->phys.air_state==0)
    {
        if (!bcm_down[fd->ply].recovery)
        {
            float stick=fd->input.lstick.X;
            Fighter_PhysGround_CalculateVelocity(fd,stick*fd->attr.walk_acceleration,
                stick*fd->attr.walk_maximum_velocity,fd->attr.ground_friction);
            Fighter_PhysGround_ApplyVelocity(gobj);
        }
        else Fighter_PhysGround_ApplyFriction(gobj);
    }
    else if (!bcm_down[fd->ply].recovery) Fighter_PhysAir_ApplyGravityFastfall(gobj);
    else if (fd->flags.is_fastfall) fd->phys.self_vel.Y=-fd->attr.fastfall_velocity;
    else Fighter_PhysAir_ApplyGravity(fd,fd->attr.gravity,fd->attr.terminal_velocity);
}
void BCM_DownColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if (fd->phys.air_state==0)
    {
        int grounded=bcm_down[fd->ply].recovery ? Fighter_CollGround_StopLedge(gobj) : Fighter_CollGround_PassLedge(gobj);
        if (!grounded)
        {
            Fighter_SetAirborne(fd); BCM_HandOver(gobj,ST_LW_AIR); BCM_DownSync(fd);
        }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj))
    {
        Fighter_SetGrounded2(fd); BCM_HandOver(gobj,ST_LW); BCM_DownSync(fd);
    }
}
static void BCM_DownLoad(FighterData *fd) { BCMDown zero={0}; bcm_down[fd->ply]=zero; }

static int BCM_IsCounter(FighterData *fd) { return fd->state_id==ST_COUNTER || fd->state_id==ST_COUNTER_AIR; }
static void BCM_NeutralEnter(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata; BCMResource *r=&bcm_resource[fd->ply];
    bcm_down[fd->ply].active=0; r->hurt_mult=0;
    Enter(gobj,state);
    r->action=fd->action_id; r->state=fd->state_id; r->frame=fd->state.frame;
    r->hit_mode=r->hurt_bound=r->sold=r->prop=0;
    r->last_hurt=0; r->damage_mult=r->kb_mult=1;
    fd->phys.self_vel.X *= 0.3f; fd->phys.self_vel_ground.X *= 0.3f;
    fd->phys.self_vel.Y *= 0.3f;
    if (BCM_IsCounter(fd)) Fighter_SetScriptHurtStatus(gobj,2);
}
void BCM_NeutralAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    int f=Tick(gobj);
    if (BCM_IsCounter(fd) && f>=17) Fighter_SetScriptHurtStatus(gobj,0);
    if (f>=27) Leave(gobj,0,0);
}
void BCM_NeutralPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if (!fd->phys.air_state) Fighter_PhysGround_ApplyFriction(gobj);
    else if (fd->flags.is_fastfall) fd->phys.self_vel.Y=-fd->attr.fastfall_velocity;
    else
    {
        Fighter_PhysAir_CheckFastfall(fd);
        if (fd->flags.is_fastfall) fd->phys.self_vel.Y=-fd->attr.fastfall_velocity;
        else Fighter_PhysAir_ApplyGravity(fd,fd->attr.gravity*0.6f,fd->attr.terminal_velocity);
    }
}
void BCM_NeutralColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata; BCMResource *r=&bcm_resource[fd->ply];
    int counter=BCM_IsCounter(fd),changed=0;
    if (!fd->phys.air_state)
    {
        if (!Fighter_CollGround_StopLedge(gobj))
        { Fighter_SetAirborne(fd); BCM_HandOver(gobj,counter ? ST_COUNTER_AIR : ST_N_AIR); changed=1; }
    }
    else if (Fighter_CollAir_IgnoreLedge_NoCB(gobj))
    { Fighter_SetGrounded2(fd); BCM_HandOver(gobj,counter ? ST_COUNTER : ST_N); changed=1; }
    if (changed)
    {
        r->action=fd->action_id; r->state=fd->state_id; r->frame=fd->state.frame;
        if (counter) Fighter_SetScriptHurtStatus(gobj,t_frame[fd->ply]<17 ? 2 : 0);
    }
}
static void BCM_SpecialHitProperties(FighterData *fd)
{
    int i;
    if (!BCM_IsCounter(fd)) return;
    // The resource updater writes the neutral base first. Apply the paid variant afterward.
    for(i=0;i<4;i++) if(fd->hitbox[i].active) {
        fd->hitbox[i].angle=55;
        fd->hitbox[i].shield_dmg=Native_ShieldField(fd->hitbox[i].dmg*0.20000004768371582f);
    }
}

typedef struct {int active,ground_start,launched;float angle;} BCMBow;
static BCMBow bcm_bow[6];
static void (*bcm_bow_fall_iasa[6])(GOBJ *);
static int BCM_IsBow(FighterData *fd){return fd->state_id==ST_HI || fd->state_id==ST_HI_AIR;}
static void BCM_BowReset(FighterData *fd)
{
    bcm_bow[fd->ply].active=0;
    Fighter_SetBoneRotX(fd,2,0);
}
static void BCM_BowAim(FighterData *fd)
{
    BCMBow *b=&bcm_bow[fd->ply];
    float x=fd->input.lstick.X,y=fd->input.lstick.Y,angle,delta;
    if(x*x+y*y<=0.25f)return;
    angle=atan2(y,x);
    if(fd->input.held & PAD_TRIGGER_Z)
    {
        delta=angle-b->angle;
        if(delta>3.14159265f)delta-=6.2831853f;
        if(delta< -3.14159265f)delta+=6.2831853f;
        b->angle+=SlapClamp(delta,-0.04363323f,0.04363323f);
    }
    else b->angle=angle;
}
static void BCM_BowEnter(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata;
    BCMBow *b=&bcm_bow[fd->ply];
    int ground=!fd->phys.air_state;
    Enter(gobj,state);
    b->active=1;b->ground_start=ground;b->launched=0;b->angle=1.5707963f;
    if(ground)Fighter_SetAirborne(fd);
    fd->phys.self_vel.X=fd->phys.self_vel_ground.X=0;
    fd->phys.self_vel.Y=ground ? 1.2921603788412228f : 0;
    BCM_BowAim(fd);
}
void BCM_BowAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(Tick(gobj)>=66)Leave(gobj,1,20.0f);
}
void BCM_BowIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMBow *b=&bcm_bow[fd->ply];
    BCMResource *r=&bcm_resource[fd->ply];
    int f=t_frame[fd->ply];
    if(!b->launched)
    {
        BCM_BowAim(fd);
        if(f>=38)
        {
            b->launched=1;
            fd->phys.self_vel.X=cos(b->angle)*2.5888035136768193f;
            fd->phys.self_vel.Y=sin(b->angle)*2.5888035136768193f;
            if(fd->phys.self_vel.X>0.01f)fd->facing_direction=1;
            else if(fd->phys.self_vel.X< -0.01f)fd->facing_direction=-1;
            Fighter_SetBoneRotY(fd,0,1.5707963f*fd->facing_direction);
            r->action=fd->action_id;r->state=fd->state_id;r->frame=fd->state.frame;
            r->hit_mode=1;r->buy=50.0f;r->hurt_bound=0;
        }
    }
    if(f>=38 && ((r->cash >= 200.0f) && ((fd->input.down & (B_BUTTON | 0x100)) != 0)))
    {
        r->cash += -200.0f; r->cash = SlapClamp(r->cash,0.0f,1000.0f);
        BCM_BowReset(fd);
        BCM_NeutralEnter(gobj,fd->phys.air_state ? ST_COUNTER_AIR : ST_COUNTER);
    }
}
void BCM_BowPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMBow *b=&bcm_bow[fd->ply];
    float angle;
    if(fd->flags.hitlag)return;
    if(b->launched || b->ground_start)
        Fighter_PhysAir_ApplyGravity(fd,fd->attr.gravity,fd->attr.terminal_velocity);
    else fd->phys.self_vel.Y=0;
    if(!b->launched)fd->phys.self_vel.X=0;
    angle=b->launched ? atan2(fd->phys.self_vel.Y,fd->phys.self_vel.X*fd->facing_direction)
                     : atan2(sin(b->angle),cos(b->angle)*fd->facing_direction);
    // The source pose points upward; XRot rotates it into the aimed trajectory.
    Fighter_SetBoneRotX(fd,2,1.5707963f-angle);
}
static void BCM_BowLand(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCM_BowReset(fd);Fighter_SetGrounded2(fd);
    Fighter_EnterSpecialLanding(gobj,0,20.0f);
}
void BCM_BowColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(bcm_bow[fd->ply].launched && fd->phys.self_vel.Y<0)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj,0,BCM_BowLand);
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))BCM_BowLand(gobj);
}
static void BCM_BowFallIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMResource *r=&bcm_resource[fd->ply];
    if(((r->cash >= 200.0f) && ((fd->input.down & B_BUTTON) != 0)))
    {
        r->cash += -200.0f; r->cash = SlapClamp(r->cash,0.0f,1000.0f);
        BCM_BowReset(fd);BCM_NeutralEnter(gobj,ST_COUNTER_AIR);return;
    }
    if(bcm_bow_fall_iasa[fd->ply])bcm_bow_fall_iasa[fd->ply](gobj);
}
static void BCM_BowFrame(FighterData *fd)
{
    if(!bcm_bow[fd->ply].active || BCM_IsBow(fd))return;
    if(fd->state_id>=ASID_FALLSPECIAL && fd->state_id<=ASID_FALLSPECIALB)
    {
        if(fd->cb.IASA!=BCM_BowFallIASA)
        {
            bcm_bow_fall_iasa[fd->ply]=fd->cb.IASA;
            fd->cb.IASA=BCM_BowFallIASA;
        }
        return;
    }
    BCM_BowReset(fd);
}
static void BCM_BowLoad(FighterData *fd)
{
    BCMBow zero={0};bcm_bow[fd->ply]=zero;bcm_bow_fall_iasa[fd->ply]=0;
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


#define ST_BCM_SPIT 383
typedef struct {int active,holding,released,turned,hop_used;} BCMSide;
static BCMSide bcm_side[6];
static const float bcm_grab_balls[2][4]={{-5.248877754146388e-06f,10.849293933684356f,3.1558376245226687f,2.969509912746939f},{4.1745984937795633e-10f,13.516519010225347f,2.6908018734284425f,2.969509912746939f}};
static int BCM_IsSide(FighterData *fd){return fd->state_id==ST_S || fd->state_id==ST_S_AIR;}
static void BCM_SideCaught(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMSide *s=&bcm_side[fd->ply];
    NativeGrab_Owner(gobj,ST_BCM_SPIT,34);
    t_frame[fd->ply]=1;s->holding=1;s->released=0;s->turned=0;
    if(!fd->phys.air_state)Fighter_SetAirborne(fd);
}
static void BCM_SideEnter(GOBJ *gobj,int state)
{
    FighterData *fd=gobj->userdata;
    BCMSide *s=&bcm_side[fd->ply];int ground=!fd->phys.air_state;
    Enter(gobj,state);
    s->active=1;s->holding=s->released=s->turned=0;
    Fighter_InitGrab(fd,1,BCM_SideCaught,0,NativeGrab_Victim);
    if(ground)
    {
        Fighter_SetAirborne(fd);
        fd->phys.self_vel.X=fd->attr.dash_initial_velocity*1.15f*fd->facing_direction;
    }
    if(ground || !s->hop_used){fd->phys.self_vel.Y=0.6852715183262167f;s->hop_used=1;}
}
void BCM_SideAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;int f=Tick(gobj),i;
    if(f>=21 && f<27)
        for(i=0;i<2;i++)NativeGrab_Box(gobj,i,bcm_grab_balls[i]);
    else if(f==27)Fighter_HitboxDisableAll(gobj);
    if(f>=59)Leave(gobj,0,0);
}
void BCM_SidePhys(GOBJ *gobj){Fighter_PhysAir_ApplyGravityFastfall(gobj);}
static void BCM_SideLand(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    float lag=fd->state_id==ST_BCM_SPIT || t_frame[fd->ply]<21 ? 4.0f : 14.0f;
    NativeGrab_Drop(gobj);bcm_side[fd->ply].holding=0;
    Fighter_SetGrounded2(fd);Fighter_EnterSpecialLanding(gobj,0,lag);
}
void BCM_SideColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(fd->phys.self_vel.Y<0)Fighter_CollAir_GrabFacingLedgeWalljump(gobj,0,BCM_SideLand);
    else if(Fighter_CollAir_IgnoreLedge_NoCB(gobj))BCM_SideLand(gobj);
}
void BCM_SpitAnim(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMSide *s=&bcm_side[fd->ply];int f=Tick(gobj);
    if(s->holding && !NativeGrab_Target(gobj)){s->holding=0;s->released=1;}
    if(!s->released && f>=21)
    {
        NativeGrab_Throw(gobj,10,325,60,32,0);
        s->holding=0;s->released=1;
    }
    if(f>=41){NativeGrab_Drop(gobj);Leave(gobj,0,0);}
}
void BCM_SpitIASA(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    BCMSide *s=&bcm_side[fd->ply];int f=t_frame[fd->ply];
    if(!s->turned && f>=16 && f<21 && fd->input.lstick.X*fd->facing_direction< -0.5f)
    {fd->facing_direction=-fd->facing_direction;Fighter_SetBoneRotY(fd,0,1.5707963f*fd->facing_direction);s->turned=1;}
}
void BCM_SpitPhys(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(bcm_side[fd->ply].holding)fd->phys.self_vel.X=fd->phys.self_vel.Y=0;
    else Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void BCM_SpitColl(GOBJ *gobj)
{
    FighterData *fd=gobj->userdata;
    if(!bcm_side[fd->ply].holding)BCM_SideColl(gobj);
}
static void BCM_SideFrame(FighterData *fd,GOBJ *gobj)
{
    BCMSide *s=&bcm_side[fd->ply];
    if(!fd->phys.air_state || fd->flags.dead)s->hop_used=0;
    if(s->active && !BCM_IsSide(fd) && fd->state_id!=ST_BCM_SPIT)
    {NativeGrab_Drop(gobj);s->active=s->holding=0;}
}
static void BCM_SideLoad(FighterData *fd){BCMSide zero={0};bcm_side[fd->ply]=zero;}


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
    if (fd->state_id == state)
        BCM_BowIASA(gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        BCM_BowIASA(gobj);
}

void Reverse_IASA_379(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        BCM_DownIASA(gobj);
}

void Reverse_IASA_380(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        BCM_DownIASA(gobj);
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    Meter_Load(fd);
    BCM_ResourceLoad(fd,gobj);
    BCM_DownLoad(fd);
    BCM_BowLoad(fd);
    BCM_SideLoad(fd);
    Jab_Load(fd);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    Meter_Frame(fd,gobj);
    BCM_ResourceFrame(fd,gobj);
    BCM_BowFrame(fd);
    BCM_SideFrame(fd,gobj);
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
    {306, 0x340211, 0x12, 0x0, BCM_NeutralAnim, Reverse_IASA_373, BCM_NeutralPhys, BCM_NeutralColl, Fighter_UpdateCameraBox},    // 373: BCM neutral-special source entry and recovery
    {315, 0x340611, 0x12, 0x0, BCM_NeutralAnim, Reverse_IASA_374, BCM_NeutralPhys, BCM_NeutralColl, Fighter_UpdateCameraBox},    // 374: BCM neutral-special source entry and recovery
    {303, 0x340212, 0x13, 0x0, BCM_SideAnim, Reverse_IASA_375, BCM_SidePhys, BCM_SideColl, Fighter_UpdateCameraBox},    // 375: BCM lunging command grab
    {312, 0x340612, 0x13, 0x0, BCM_SideAnim, Reverse_IASA_376, BCM_SidePhys, BCM_SideColl, Fighter_UpdateCameraBox},    // 376: BCM lunging command grab
    {321, 0x340213, 0x14, 0x0, BCM_BowAnim, Reverse_IASA_377, BCM_BowPhys, BCM_BowColl, Fighter_UpdateCameraBox},    // 377: BCM aimed recovery
    {322, 0x340613, 0x14, 0x0, BCM_BowAnim, Reverse_IASA_378, BCM_BowPhys, BCM_BowColl, Fighter_UpdateCameraBox},    // 378: BCM aimed recovery
    {323, 0x340214, 0x15, 0x0, BCM_DownAnim, Reverse_IASA_379, BCM_DownPhys, BCM_DownColl, Fighter_UpdateCameraBox},    // 379: BCM down-special hold and recovery
    {325, 0x340614, 0x15, 0x0, BCM_DownAnim, Reverse_IASA_380, BCM_DownPhys, BCM_DownColl, Fighter_UpdateCameraBox},    // 380: BCM down-special hold and recovery
    {306, 0x340211, 0x12, 0x0, BCM_NeutralAnim, N_IASA, BCM_NeutralPhys, BCM_NeutralColl, Fighter_UpdateCameraBox},    // 381: BCM paid neutral counter
    {315, 0x340611, 0x12, 0x0, BCM_NeutralAnim, N_IASA, BCM_NeutralPhys, BCM_NeutralColl, Fighter_UpdateCameraBox},    // 382: BCM paid neutral counter
    {304, 0x340612, 0x13, 0x0, BCM_SpitAnim, BCM_SpitIASA, BCM_SpitPhys, BCM_SpitColl, Fighter_UpdateCameraBox},    // 383: BCM captured spit
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
