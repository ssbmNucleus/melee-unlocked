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

// ---- the burn ------------------------------------------------------------------------------------------
#define BURN_EVERY 30         // observed eligible updates between two ticks of a burn
#define BURN_TICKS 5         // observed ticks a burn lasts
#define BURN_DAMAGE 1.0f       // observed % a tick
#define BURN_PAUSE_STUN 1 // source burn clock is suspended by hitstun/hitstop
#define BURN_COLOUR 11       // the engine's colour of a light fire hit, laid on him on each tick
#define FIRE 1                          // Melee's fire element: the reader gives it to his hits that set alight
#define PUDDLE_REACH 5.999999925656466f     // sideways from the puddle's middle that a fighter stands in it
#define PUDDLE_HIGH 3.0f       // and above it
#define SOUND_APPLIED 130     // the game's own sounds, by their place in his bank (0: not in it):
#define SOUND_CONSUME 100     //   Burn Applied, Consume, Burn End, Flame Puddle Create (SFX_Zetterburn),
#define SOUND_END 8             //   played from his code as the game's code plays them
#define SOUND_PUDDLE 9
static void Burn_Sound(FighterData *fd, int place)
{
    if (place)
        Fighter_PlaySFX(fd, 5000 + place, 127, 64);    // (ft_PlaySFX: not ended as his state changes)
}
static u8 burn_by[6];                   // by fighter: 1 + the player whose burn it is (0: not alight)
static u16 burn_left[6];                // frames of the burn left
static float was_percent[6];            // his damage last frame: a rise with no tick of ours is a hit
static float puddle_x[6], puddle_y[6];  // by player: his puddle, and its frames left
static u16 puddle_left[6];
static void Set_Alight(FighterData *fd, FighterData *od, int i, int me)
{
    Burn_Sound(fd, SOUND_APPLIED);
    burn_by[i] = me + 1;
    burn_left[i] = BURN_EVERY * BURN_TICKS;
    Fighter_ColAnim_Apply(od, BURN_COLOUR, 0);
}
// Inclusive source frame intervals include fractional playback within each frame (e.g. 33.33 is frame 33).
// A contact records the source predicate; IASA enters its branch after this update's animation.
// Moving state in OwnFrame would advance the new branch twice on its first update.
typedef struct {int lo,hi,hit_first,hit_last,first,last,next;} ConsumeRule;
static const ConsumeRule consume_rules[] = {{58,62,20,21,21,21,385},
    {63,63,10,12,13,13,386},
    {64,64,16,17,17,17,387},
    {219,219,32,33,33,33,388}};
static int consume_pending[6], consume_origin[6], consume_target[6];
static float consume_contact_frame[6];
static GOBJ *throw_victim[6];
static void (*consume_iasa[6])(GOBJ *);
static int Consume_Followup(int state)
{
    return state == 385 || state == 386 || state == 387 || state == 388;
}
static int Burn_DirectHit(FighterData *fd)
{
    int state = fd->state_id;
    int frame = (int)fd->state.frame;
    return (state == 67 && frame >= 10 && frame <= 12) ||
        (state == 67 && frame >= 11 && frame <= 11) ||
        (state == 373 && frame >= 37 && frame <= 39) ||
        (state == 374 && frame >= 37 && frame <= 39) ||
        (state == 375 && frame >= 1 && frame <= 3) ||
        (state == 376 && frame >= 1 && frame <= 3) ||
        (state == 377 && frame >= 16 && frame <= 16) ||
        (state == 378 && frame >= 16 && frame <= 16) ||
        (state == 379 && frame >= 17 && frame <= 18) ||
        (state == 379 && frame >= 19 && frame <= 20) ||
        (state == 379 && frame >= 21 && frame <= 22) ||
        (state == 379 && frame >= 23 && frame <= 24) ||
        (state == 379 && frame >= 25 && frame <= 26) ||
        (state == 379 && frame >= 27 && frame <= 28) ||
        (state == 379 && frame >= 29 && frame <= 30) ||
        (state == 379 && frame >= 31 && frame <= 54) ||
        (state == 380 && frame >= 17 && frame <= 18) ||
        (state == 380 && frame >= 19 && frame <= 20) ||
        (state == 380 && frame >= 21 && frame <= 22) ||
        (state == 380 && frame >= 23 && frame <= 24) ||
        (state == 380 && frame >= 25 && frame <= 26) ||
        (state == 380 && frame >= 27 && frame <= 28) ||
        (state == 380 && frame >= 29 && frame <= 30) ||
        (state == 380 && frame >= 31 && frame <= 54) ||
        (state == 381 && frame >= 23 && frame <= 25) ||
        (state == 382 && frame >= 23 && frame <= 29) ||
        (state == 383 && frame >= 1 && frame <= 7) ||
        (state == 384 && frame >= 1 && frame <= 3);
}
static void Consume_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, pending = consume_pending[p], frame = (int)fd->state.frame;
    void (*original)(GOBJ *) = consume_iasa[p];
    if (pending)
    {
        const ConsumeRule *rule = &consume_rules[pending - 1];
        GOBJ *target = Fighter_GetGObj(consume_target[p]);
        if (fd->state_id != consume_origin[p] || fd->state.frame < consume_contact_frame[p] ||
            frame > rule->last || !target || ((FighterData *)target->userdata)->state_id < ASID_WAIT ||
            burn_by[consume_target[p]] != p + 1)
        {
            consume_pending[p] = 0;
            fd->cb.IASA = original;
        }
        else if (frame >= rule->first)
        {
            consume_pending[p] = 0;
            fd->cb.IASA = original;
            Enter(gobj, rule->next);
            return;
        }
    }
    if (original)
        original(gobj);
}
static void Consume_Queue(FighterData *fd, int target, int throw_release)
{
    int i, p = fd->ply, frame = (int)fd->state.frame;
    for (i = 0; i < sizeof(consume_rules) / sizeof(consume_rules[0]); i++)
    {
        const ConsumeRule *rule = &consume_rules[i];
        if (fd->state_id >= rule->lo && fd->state_id <= rule->hi &&
            ((throw_release && fd->state_id == ASID_THROWF && frame <= rule->last) ||
             (frame >= rule->hit_first && frame <= rule->hit_last)))
        {
            if (fd->cb.IASA != Consume_IASA)
                consume_iasa[p] = fd->cb.IASA;
            consume_pending[p] = i + 1;
            consume_origin[p] = fd->state_id;
            consume_target[p] = target;
            consume_contact_frame[p] = fd->state.frame;
            fd->cb.IASA = Consume_IASA;
            return;
        }
    }
}

static void Burn_Frame(FighterData *fd, GOBJ *gobj)
{
    int me = fd->ply, i;
    GOBJ *released = 0;
    // MexTK calls +0x1A58 grab.attacker; the engine calls it victim_gobj: who this fighter holds.
    // ftCo_800DDDE4 clears it when releasing, but does not replace dmg.hit_log.source.
    if (fd->state_id == ASID_THROWF)
    {
        if (fd->grab.attacker)
            throw_victim[me] = fd->grab.attacker;
        else
            released = throw_victim[me];
    }
    else
        throw_victim[me] = 0;
    if (consume_pending[me] && (fd->state_id != consume_origin[me] || fd->cb.IASA != Consume_IASA))
        consume_pending[me] = 0;
    if (puddle_left[me])
        puddle_left[me]--;
    for (i = 0; i < 6; i++)
    {
        GOBJ *other = Fighter_GetGObj(i);
        FighterData *od;
        float got;
        if (!other || other == gobj)
            continue;
        od = other->userdata;
        if (od->state_id < ASID_WAIT)                   // knocked out: a burn ends with him
        {
            burn_by[i] = 0;
            burn_left[i] = 0;
            was_percent[i] = od->dmg.percent;
            continue;
        }
        got = od->dmg.percent - was_percent[i];
        if (got > 0.0f && od->dmg.source_ply == me)     // struck by him (or by a thing of his) since last frame
        {
            if (od->dmg.hit_log.source == gobj || other == released)
            {
                if (Consume_Followup(fd->state_id) && burn_by[i] == me + 1)
                {
                    // The branch's real hitbox already dealt its damage and knockback.
                    Fighter_ColAnim_Apply(od, BURN_COLOUR + 2, 0);
                    Burn_Sound(fd, SOUND_CONSUME);
                    burn_by[i] = 0;
                    burn_left[i] = 0;
                }
                else
                {
                    if (burn_by[i] == me + 1)
                        Consume_Queue(fd, i, other == released);
                    if (Burn_DirectHit(fd))
                        Set_Alight(fd, od, i, me);
                }
            }
            else if (od->dmg.hit_log.attribute == FIRE)
                Set_Alight(fd, od, i, me);
            if (other == released)
                throw_victim[me] = 0;
        }
        // his puddle sets alight a fighter that stands in it
        if (puddle_left[me] && burn_by[i] == 0)
        {
            float dx = od->phys.pos.X - puddle_x[me], dy = od->phys.pos.Y - puddle_y[me];
            if (dx < PUDDLE_REACH && dx > -PUDDLE_REACH && dy > -1.0f && dy < PUDDLE_HIGH)
                Set_Alight(fd, od, i, me);
        }
        if (burn_by[i] == me + 1 && burn_left[i] &&
            !(BURN_PAUSE_STUN && (od->flags.hitstun || od->flags.hitlag)))
        {
            burn_left[i]--;
            if (burn_left[i] % BURN_EVERY == 0)
            {
                Fighter_GiveDamage(od, BURN_DAMAGE);
                Fighter_ColAnim_Apply(od, BURN_COLOUR, 0);
            }
            if (!burn_left[i])
            {
                burn_by[i] = 0;
                Burn_Sound(fd, SOUND_END);
            }
        }
        was_percent[i] = od->dmg.percent;
    }
}

// ---- the neutral special: a burst of fire about him, charged by holding the button ------------------
#define ST_N 373
#define ST_N_AIR 374
#define ST_NU 375
#define ST_NU_AIR 376
#define N_CHARGE_FIRST 2 // the game's Charging window: its first frame,
#define N_CHARGE_LAST 36   //   and its last (the charged burst comes out on the next)
#define N_END 64                   // the charged burst: the frame he may act from (the clip's end)
#define NU_END 24                 // the burst let go early
#define N_SLOW 40                 // the charged burst's recovery window begins: in the air he falls slowly
#define NU_SLOW 3               //   (and the early burst's)
#define N_GRAVITY 0.06999999869898814f           // the game's Gravity 0.7 there,
#define N_FALL_MOST 0.5999999888484698f       //   and its MaxFallSpeed 6
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
    if (frame >= N_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void N_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = t_frame[fd->ply];
    // Melee refreshes held input after Anim. Source SpecialReleased uses this update's button.
    if (frame >= N_CHARGE_FIRST && frame <= N_CHARGE_LAST && !(fd->input.held & B_BUTTON))
        Enter(gobj, fd->phys.air_state == 0 ? ST_NU : ST_NU_AIR);
}
void NU_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= NU_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void N_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int s = fd->state_id;
    int slow = (s == ST_NU || s == ST_NU_AIR) ? NU_SLOW : N_SLOW;
    if (fd->phys.air_state == 0)
    {
        Fighter_PhysGround_ApplyFriction(gobj);
        return;
    }
    if (t_frame[fd->ply] >= slow)
    {
        fd->phys.self_vel.Y -= N_GRAVITY;               // (no drift in the game's recovery windows: NoDrift)
        if (fd->phys.self_vel.Y < -N_FALL_MOST)
            fd->phys.self_vel.Y = -N_FALL_MOST;
        return;
    }
    Fighter_PhysAir_ApplyGravityFastfall(gobj);
}

// ---- the side special: the mane's swipe, and the fireball thrown after it ----------------------------
#define ST_S 377
#define ST_S_AIR 378
#define S_END 47                   // the natural end of the source window chain, on the ground,
#define S_END_AIR 47           //   and in the air
#define S_FIRE 17                 // the frame the fireball is made on (the game's Recovery window, frame 0)
#define MUZZLE_X 11.0f             // where (the game's InitialOffsetFromOwner): ahead of him,
#define MUZZLE_Y 7.0f             //   and above his feet
static void Shoot(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Vec3 at;
    GOBJ *shot;
    at.X = fd->phys.pos.X + MUZZLE_X * fd->facing_direction;
    at.Y = fd->phys.pos.Y + MUZZLE_Y;
    at.Z = 0;
    shot = Make_Item(gobj, 0, &at);
    if (shot)
        ((ItemData *)shot->userdata)->item_var.var1 = 1;        // its own code sends it on its way
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
    if (frame == S_FIRE)
        Shoot(gobj);
    if (frame >= (fd->phys.air_state == 0 ? S_END : S_END_AIR) || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}

// ---- the up special: flames gather and strike about him, then he flies along the stick ---------------
#define ST_HI 379
#define ST_HI_AIR 380
#define HI_TRAVEL 31           // the game's Travel Active window: its first frame,
#define HI_TRAVEL_END 54   //   and its last
#define HI_END 80                 // the end of its Recovery window: the helpless fall from here
#define HI_AHEAD 4.5f             // READING: the flight's speed in its own axes, ahead (the game's 45 cm),
#define HI_ACROSS -0.10500000000000001f           //   and up from its line (the game's -1.05 cm)
#define HI_KEEP 0.3f               // kept of his speed as the flight ends (the game's MultiplyVelocity 0.3)
#define HI_LANDING 15.0f         // frames of the helpless landing (the game's Special Land: IasaFrame 15)
#define HI_STICK 0.3f                   // the stick nearer the middle than this: straight up
#define XROT_JOINT 2               // the joint the engine pitches a fighter about
static float hi_angle[6];
static u8 hi_pitched[6], w_helpless[6];
static void Turn_Round(FighterData *fd)
{
    fd->facing_direction = -fd->facing_direction;
    Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
}
static void Pitch(FighterData *fd, float by)
{
    Fighter_SetBoneRotX(fd, XROT_JOINT, by);
    hi_pitched[fd->ply] = by != 0;
}
void SpecialHi(GOBJ *gobj)
{
    Enter(gobj, ST_HI);
}
void SpecialAirHi(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_HI_AIR);
    fd->phys.self_vel.X = 0;                            // he hangs where he is (the game's ConstVelocity 0, 0)
    fd->phys.self_vel.Y = 0;
}
static void Hi_Fly(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X, sy = fd->input.lstick.Y;
    if (sx * sx + sy * sy < HI_STICK * HI_STICK)
    {
        sx = 0;
        sy = 1;
    }
    if (fd->phys.air_state == 0 && sy < 0.1f)           // from the ground, never into it
        sy = 0.1f;
    if (sx * fd->facing_direction < -0.01f)
        Turn_Round(fd);
    hi_angle[p] = atan2(sy, sx * fd->facing_direction);
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    Pitch(fd, -hi_angle[p]);
    w_helpless[p] = 1;
}
void Hi_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int frame = Tick(gobj);
    if (frame == HI_TRAVEL_END + 1)
        Pitch(fd, 0);
    if (frame >= HI_END || FrameTimerCheck(gobj) == 0)
    {
        Pitch(fd, 0);
        if (fd->phys.air_state == 0)
            Fighter_EnterWait(gobj);
        else
            Fighter_EnterSpecialFall(gobj, 1, 1, 1, 1.0f, HI_LANDING);
    }
}
void Hi_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (t_frame[fd->ply] == HI_TRAVEL)
        Hi_Fly(gobj);                // the launch update's stick, after input refresh
}
void Hi_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, frame = t_frame[p];
    if (frame < HI_TRAVEL)
    {
        if (fd->phys.air_state == 0)
            Fighter_PhysGround_ApplyFriction(gobj);
        else
        {
            fd->phys.self_vel.X = 0;
            fd->phys.self_vel.Y = 0;
        }
        return;
    }
    if (frame <= HI_TRAVEL_END)
    {
        float a = hi_angle[p], c = cos(a), s = sin(a);
        fd->phys.self_vel.X = (HI_AHEAD * c - HI_ACROSS * s) * fd->facing_direction;
        fd->phys.self_vel.Y = HI_AHEAD * s + HI_ACROSS * c;
        return;
    }
    if (frame == HI_TRAVEL_END + 1)
    {
        fd->phys.self_vel.X *= HI_KEEP;
        fd->phys.self_vel.Y *= HI_KEEP;
    }
    Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
static void Hi_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Pitch(fd, 0);
    Fighter_EnterSpecialLanding(gobj, 0, HI_LANDING);
}
void Hi_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (Fighter_CollGround_PassLedge(gobj) == 0)
        {
            Fighter_SetAirborne(fd);
            Hand_Over(gobj, ST_HI_AIR);
        }
        return;
    }
    // from the flight on, a ledge he faces is caught (the game's CanGrabLedgeOnFrame 0) and a floor ends it
    if (t_frame[fd->ply] >= HI_TRAVEL)
        Fighter_CollAir_GrabFacingLedgeWalljump(gobj, 0, Hi_Landed);
    else
        Fighter_CollAir_IgnoreLedge_NoCB(gobj);
}
// helpless until he stands: no jump (the engine's helpless fall keeps one he had)
static void Helpless_Frame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    if (hi_pitched[p] && s != ST_HI && s != ST_HI_AIR)
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

// ---- the down special: the slam and its flame puddle; in the air a hop, a dive, and the slam ----------
#define ST_LW 381
#define ST_LW_AIR 382
#define ST_LW_DIVE 383
#define ST_LW_SLAM 384
#define LW_END 67                 // the slam on the ground: the frame he may act from (IasaFrame)
#define LW_PUDDLE 23           // the frame the puddle is made on: the slam's (its Active window)
#define LW_SLAM_END 45       // the slam after a dive: the frame he may act from
#define LW_STARTUP_END 23 // the ground slam's Startup: walked off an edge before it ends, the hop
#define LW_DIVE_FROM 23     // the hop's Startup window ends: the dive from this frame
#define LW_DIVE_LEN 7       // the dive's window (a loop of its own: its hits again each time round)
#define LW_HOP_X 2.2600000000000002f             // the hop's speed as it starts (the game's SetVelocity 22.6, 20 cm),
#define LW_HOP_Y 2.0f
#define LW_HOP_FALL_MOST 1.099999979555528f // its MaxFallSpeed 11
#define LW_HOP_BRAKE 0.0849999984201999f     // its FrictionAir 0.85
#define LW_DIVE_X 1.1300000000000001f           // the dive's speed (the game's ConstVelocity 11.3, -42.5 cm)
#define LW_DIVE_Y -4.25f
#define LW_LANDING 7.0f         // landing in the hop, before the dive (the game's LandingLagFrames)
static void Puddle(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    Vec3 at = fd->phys.pos;
    at.Z = 0;
    Make_Item(gobj, 1, &at);
    puddle_x[p] = at.X;
    puddle_y[p] = at.Y;
    puddle_left[p] = 300;
    Burn_Sound(fd, SOUND_PUDDLE);
}
static void Lw_Hop(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_SetAirborne(fd);
    Enter(gobj, ST_LW_AIR);
    fd->phys.self_vel.X = LW_HOP_X * fd->facing_direction;
    fd->phys.self_vel.Y = LW_HOP_Y;
}
static void Lw_Dive(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Enter(gobj, ST_LW_DIVE);
    fd->phys.self_vel.X = LW_DIVE_X * fd->facing_direction;
    fd->phys.self_vel.Y = LW_DIVE_Y;
}
void SpecialLw(GOBJ *gobj)
{
    Enter(gobj, ST_LW);
}
void SpecialAirLw(GOBJ *gobj)
{
    Lw_Hop(gobj);
}
void Lw_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame == LW_PUDDLE)
        Puddle(gobj);
    if (frame >= LW_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void Lw_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
void Lw_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (Fighter_CollGround_PassLedge(gobj) == 0)
    {
        Fighter_SetAirborne(fd);
        if (t_frame[fd->ply] < LW_STARTUP_END)
            Lw_Hop(gobj);                               // (the game's Aerial cancel: the air form from its start)
        else
            Fighter_EnterFall(gobj);
    }
}
void LwAir_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= LW_DIVE_FROM)
        Lw_Dive(gobj);
}
void LwAir_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float *vx = &fd->phys.self_vel.X;
    if (*vx > LW_HOP_BRAKE)                              // no drift (NoDrift): his speed ahead only brakes
        *vx -= LW_HOP_BRAKE;
    else if (*vx < -LW_HOP_BRAKE)
        *vx += LW_HOP_BRAKE;
    else
        *vx = 0;
    fd->phys.self_vel.Y -= fd->attr.gravity;
    if (fd->phys.self_vel.Y < -LW_HOP_FALL_MOST)
        fd->phys.self_vel.Y = -LW_HOP_FALL_MOST;
}
static void LwHop_Landed(GOBJ *gobj)
{
    Fighter_EnterSpecialLanding(gobj, 0, LW_LANDING);
}
void LwAir_Coll(GOBJ *gobj)
{
    Fighter_CollAir_IgnoreLedge(gobj, LwHop_Landed);
}
void LwDive_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame > LW_DIVE_LEN)
        Lw_Dive(gobj);                                  // round again: its hits are made afresh
}
void LwDive_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    fd->phys.self_vel.X = LW_DIVE_X * fd->facing_direction;
    fd->phys.self_vel.Y = LW_DIVE_Y;
}
static void LwDive_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    Enter(gobj, ST_LW_SLAM);                            // (the game's Grounded cancel: the slam's Active window)
    Puddle(gobj);
}
void LwDive_Coll(GOBJ *gobj)
{
    Fighter_CollAir_IgnoreLedge(gobj, LwDive_Landed);
}
void LwSlam_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= LW_SLAM_END || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void LwSlam_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (Fighter_CollGround_PassLedge(gobj) == 0)
    {
        Fighter_SetAirborne(fd);
        Fighter_EnterFall(gobj);
    }
}
void burn_fs_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= 46 || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void burn_fs_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 32)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}
void burn_hi_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= 45 || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void burn_hi_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 36)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}
void burn_lw_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= 42 || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void burn_lw_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 32)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}
void burn_throw_Anim(GOBJ *gobj)
{
    int frame = Tick(gobj);
    if (frame >= 42 || FrameTimerCheck(gobj) == 0)
        Leave(gobj, 0, 0);
}
void burn_throw_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 32)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}
void NU_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 24)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 24)
        Fighter_IASACheck_AllAerial(gobj);
}
void S_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 36)
            Fighter_IASACheck_AllGrounded(gobj);
    }
    else if (t_frame[fd->ply] >= 36)
        Fighter_IASACheck_AllAerial(gobj);
}
void Lw_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 58)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}
void LwSlam_IASA(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
    {
        if (t_frame[fd->ply] >= 36)
            Fighter_IASACheck_AllGrounded(gobj);
    }
}

// ---- once as he is made, and once a frame -----------------------------------------------------------
static void OwnLoad(FighterData *fd)
{
    ItemDesc **items = fd->ftData->items;
    int i, p = fd->ply;
    t_frame[p] = 0;
    consume_pending[p] = 0;
    consume_iasa[p] = 0;
    throw_victim[p] = 0;
    hi_pitched[p] = 0;
    w_helpless[p] = 0;
    puddle_left[p] = 0;
    for (i = 0; i < 6; i++)
    {
        burn_by[i] = 0;
        burn_left[i] = 0;
        was_percent[i] = 0;
    }
    for (i = 0; items && i < 2; i++)
        if (items[i])
            MEX_IndexFighterItem(fd->kind, items[i], i);
}
static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    Helpless_Frame(fd, gobj);
    Burn_Frame(fd, gobj);
}

// ---- the other form of each state that has one (from `forms` in `build`) ------------------------------
static int Other_Form(int state)
{
    switch (state)
    {
    case ST_N: return ST_N_AIR;
    case ST_N_AIR: return ST_N;
    case ST_NU: return ST_NU_AIR;
    case ST_NU_AIR: return ST_NU;
    case ST_S: return ST_S_AIR;
    case ST_S_AIR: return ST_S;
    }
    return state;                       // (a state not listed: a mistake; it stays the state it is)
}


typedef struct {int action,state,first,end,target,neutral;} JabWindow;
static const JabWindow jab_windows[] = {
    {46, 44, 11, 30, 45, 0},
    {47, 45, 8, 36, 46, 0},
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
    {46, 5, 6, 2, 0, 0.387923002243042f, 0.0f},
    {47, 1, 3, 2, 0, 0.775846004486084f, 0.0f},
    {48, 1, 5, 2, 0, 1.1637690067291262f, 0.0f},
    {52, 7, 9, 1, 0, 1.93961501121521f, 0.0f},
    {62, 17, 19, 4, 0, 5.81884503364563f, 0.0f},
    {62, 20, 21, 1, 0, 0.0f, 0.0f},
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
    if (!(s == 373 || s == 374 || s == 377 || s == 378 || s == 379 || s == 380 || s == 381 || s == 382) || fd->state.frame > 4.0f)
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

void Reverse_IASA_377(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        S_IASA(gobj);
}

void Reverse_IASA_378(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        S_IASA(gobj);
}

void Reverse_IASA_379(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        Hi_IASA(gobj);
}

void Reverse_IASA_380(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        Hi_IASA(gobj);
}

void Reverse_IASA_381(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int state = fd->state_id;
    ReverseInput(fd, gobj);
    if (fd->state_id == state)
        Lw_IASA(gobj);
}

void Reverse_IASA_382(GOBJ *gobj)
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
    Momentum_Load(fd);
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
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
    {306, 0x340211, 0x12, 0x0, N_Anim, Reverse_IASA_373, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 373: the fire burst (charging, charged)
    {315, 0x340611, 0x12, 0x0, N_Anim, Reverse_IASA_374, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 374: the fire burst in the air
    {307, 0x340211, 0x12, 0x0, NU_Anim, NU_IASA, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 375: the fire burst let go early
    {316, 0x340611, 0x12, 0x0, NU_Anim, NU_IASA, N_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 376: the fire burst let go early, in the air
    {303, 0x340212, 0x13, 0x0, S_Anim, Reverse_IASA_377, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 377: the mane swipe and fireball
    {312, 0x340612, 0x13, 0x0, S_Anim, Reverse_IASA_378, Plain_Phys, Plain_Coll, Fighter_UpdateCameraBox},    // 378: the mane swipe and fireball in the air
    {321, 0x340213, 0x14, 0x0, Hi_Anim, Reverse_IASA_379, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 379: the flame burst
    {322, 0x340613, 0x14, 0x0, Hi_Anim, Reverse_IASA_380, Hi_Phys, Hi_Coll, Fighter_UpdateCameraBox},    // 380: the flame burst in the air
    {323, 0x340214, 0x15, 0x0, Lw_Anim, Reverse_IASA_381, Lw_Phys, Lw_Coll, Fighter_UpdateCameraBox},    // 381: the slam
    {325, 0x340614, 0x15, 0x0, LwAir_Anim, Reverse_IASA_382, LwAir_Phys, LwAir_Coll, Fighter_UpdateCameraBox},    // 382: the hop before the dive
    {314, 0x340614, 0x15, 0x0, LwDive_Anim, 0, LwDive_Phys, LwDive_Coll, Fighter_UpdateCameraBox},    // 383: the dive
    {305, 0x340214, 0x15, 0x0, LwSlam_Anim, LwSlam_IASA, Lw_Phys, LwSlam_Coll, Fighter_UpdateCameraBox},    // 384: the slam after the dive
    {308, 0x240a09, 0xa, 0x0, burn_fs_Anim, burn_fs_IASA, Plain_Phys, LwSlam_Coll, (void *)0x800761c8},    // 385: Fstrong: consuming follow-up
    {309, 0x240a0a, 0xb, 0x0, burn_hi_Anim, burn_hi_IASA, Plain_Phys, LwSlam_Coll, (void *)0x800761c8},    // 386: Ustrong: consuming follow-up
    {310, 0x240a0b, 0xc, 0x0, burn_lw_Anim, burn_lw_IASA, Plain_Phys, LwSlam_Coll, (void *)0x800761c8},    // 387: Dstrong: consuming follow-up
    {311, 0x240a09, 0x35, 0x0, burn_throw_Anim, burn_throw_IASA, Plain_Phys, LwSlam_Coll, (void *)0x800761c8},    // 388: Fthrow: consuming follow-up
};

// The donor keeps its sword trail description in its special attributes; the port carries those.
AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    return (AfterImageDesc *)((u8 *)fd->special_attributes + 0x78);
}
