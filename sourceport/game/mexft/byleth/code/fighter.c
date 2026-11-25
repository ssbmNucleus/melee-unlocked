#include "../mex.h"

// ---- Byleth's own states ---------------------------------------------------------------------
#define ST_LATCH 373
#define ST_KICK 374
#define ST_WHIP 367
#define ST_WHIP_AIR 368
#define ST_BOW_CANCEL 375
#define ST_AXE_CANCEL 376
#define ST_WALLJUMP 377
#define ST_SIDE 349
#define SHIELD_BUTTONS 0x60             // R and L
#define BOW_CANCEL_FROM 20      // the frames of drawing the bow between which it can be put away or left
#define BOW_CANCEL_TO 65          //   by a jump (the game's own numbers)
#define BOW_TURN_WAIT 9        // frames after turning round with the bow drawn before he can again
#define AXE_COMMIT 34.0f          // the frame the axe's swing is past stopping (where its armour starts)
#define AXE_TURN_FROM 12.0f    // the frames between which the axe, drawn, can be turned round (once)
#define AXE_TURN_TO 41.0f
#define STOMP_DAMAGE 6.5f      // the kick's damage with the whip caught close,
#define STOMP_MORE 2.0f        //   times this with it caught at its full reach
#define STOMP_AT 50.0f          // the caught fighter's damage from which the kick sends it down:
#define STOMP_ANGLE 260             //   the angle, growth and base of that
#define STOMP_KBG 110
#define STOMP_BKB 14
#define WHIP_REACH 46.97245694289568f        // how far from him the whip's tip gets
#define HOP_X 0.9081169720214849f                // the speeds he leaves a kick or a wall with (the game's own numbers)
#define HOP_Y 2.6234490035544176f
#define WALL_MAX 3                  // walls the whip may catch before he lands
#define WALL_FROM 11.0f           // the frames of the throw on which the whip is out
#define WALL_TO 24.0f
#define WALL_HAND 11.039999771118163f          // how high over his feet the whip leaves his hand
#define WALL_ACT 23.0f           // the frame of the jump off a wall from which he may act
#define WALL_LEFT ((int (*)(float, float, float, float, Vec3 *, int *, u32 *, Vec3 *, int, int))0x800501CC)    // mpCheckLeftWall
#define WALL_RIGHT ((int (*)(float, float, float, float, Vec3 *, int *, u32 *, Vec3 *, int, int))0x800509B8)   // mpCheckRightWall
#define LUNGE_SPEED 3.027056653594566f      // the side special asked for with a flick: the speed forward it starts with,
#define LUNGE_BRAKE 0.3430664243492502f      //   and what comes off that each frame
#define LUNGE_FLICK 6               // frames since the stick was flicked that still count
#define WHIP_START_Y 1.4000f  // the speed upward the air up special starts with (the game's own number)
#define TIP_JOINT 127                 // the whip's last joint
#define LATCH_ABOVE 3.027f        // how far above the caught fighter's feet his own end up, so the kick is on them

static Vec3 latch_at[6];                // where each player's whip caught something,
static s8 latch_on[6];                  // and which player it caught (-1: none, only a place)
static u8 latched[6];
static u8 axe_turned[6];                // the axe has been turned round in this swing
static u8 whip_started[6];              // the air up special now under way has been given its starting speed
static u16 bow_age[6];                  // frames the bow has been out
static u8 bow_wait[6];                  // frames until he can turn round with it again
static u8 wall_count[6];                // walls the whip has caught since he last stood
static u8 on_wall[6];                   // what the whip caught this time is a wall
static float wall_away[6];              // which way is off that wall
static float stomp[6];                  // how many times its damage the kick now coming does
static float lunge[6];                  // the speed forward the side special still has from its flick
static int last_state[6];

static void OwnLoad(FighterData *fd)
{
    latched[fd->ply] = 0;
    axe_turned[fd->ply] = 0;
    latch_on[fd->ply] = -1;
    bow_age[fd->ply] = 0;
    bow_wait[fd->ply] = 0;
    wall_count[fd->ply] = 0;
    on_wall[fd->ply] = 0;
    stomp[fd->ply] = 1.0f;
    lunge[fd->ply] = 0;
    last_state[fd->ply] = -1;
}

static void Turn_Round(FighterData *fd)
{
    fd->facing_direction = -fd->facing_direction;
    Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
}

static void Enter(GOBJ *gobj, int state)
{
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
}

// the whip has hit something: remember where the fighter it caught is (the nearest other one; failing
// that, where the whip's tip is)
static void Whip_Hit(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    float best = 1.0e12f;
    int i;
    JOBJ_GetWorldPosition(fd->bones[TIP_JOINT].joint, 0, &latch_at[fd->ply]);
    latch_on[fd->ply] = -1;
    on_wall[fd->ply] = 0;
    stomp[fd->ply] = 1.0f;
    for (i = 0; i < 6; i++)
    {
        GOBJ *other = Fighter_GetGObj(i);
        if (other && other != gobj)
        {
            FighterData *od = other->userdata;
            float dx = od->phys.pos.X - fd->phys.pos.X, dy = od->phys.pos.Y - fd->phys.pos.Y;
            if (dx * dx + dy * dy < best)
            {
                best = dx * dx + dy * dy;
                latch_on[fd->ply] = i;
                latch_at[fd->ply] = od->phys.pos;
                latch_at[fd->ply].Y += LATCH_ABOVE;
            }
        }
    }
    if (latch_on[fd->ply] >= 0)
    {
        // the further off the caught fighter was, the harder the kick (up to STOMP_MORE times at the whip's
        // full reach). (The whip is at its full length within a frame or two, so where its tip is says nothing.)
        float far = sqrtf(best) / WHIP_REACH;
        stomp[fd->ply] = 1.0f + (STOMP_MORE - 1.0f) * (far > 1.0f ? 1.0f : far);
    }
    latched[fd->ply] = 1;
}

// the catch: drawn to what was caught (wherever the hit has since sent it), to arrive as the clip ends
void Latch_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Vec3 *to = &latch_at[fd->ply];
    float left = 15.0f - fd->state.frame;
    if (latch_on[fd->ply] >= 0)
    {
        GOBJ *other = Fighter_GetGObj(latch_on[fd->ply]);
        if (other)
        {
            FighterData *od = other->userdata;
            *to = od->phys.pos;
            to->Y += LATCH_ABOVE;
        }
    }
    if (left < 1.0f)
        left = 1.0f;
    fd->phys.self_vel.X = (to->X - fd->phys.pos.X) / left;
    fd->phys.self_vel.Y = (to->Y - fd->phys.pos.Y) / left;
}
void Latch_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (FrameTimerCheck(gobj) != 0)
        return;
    if (on_wall[fd->ply])
    {
        // at the wall: he jumps off it, facing away
        float away = wall_away[fd->ply];
        on_wall[fd->ply] = 0;
        if (fd->facing_direction != away)
            Turn_Round(fd);
        Enter(gobj, ST_WALLJUMP);
        fd->phys.self_vel.X = HOP_X * away;
        fd->phys.self_vel.Y = HOP_Y;
    }
    else
        Enter(gobj, ST_KICK);
}
void WallJump_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->state.frame >= WALL_ACT || FrameTimerCheck(gobj) == 0)
        Fighter_EnterFall(gobj);
}
void WallJump_Phys(GOBJ *gobj)
{
    Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Own_AirColl(GOBJ *gobj)
{
    Fighter_CollAir_IgnoreLedge(gobj, Fighter_EnterLanding);
}

// the kick: he hangs there for it, then hops off and can act
// (The kick's hit is made by its script, after this code's per-frame work and before anything is struck: its
// numbers are set here, in the state's own step between the two. The game has three kicks and picks one by the
// caught fighter's damage; the script here holds the one that sends it up.)
void Kick_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, i, down = 0;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = 0;
    if (latch_on[p] >= 0)
    {
        GOBJ *other = Fighter_GetGObj(latch_on[p]);
        if (other)
            down = ((FighterData *)other->userdata)->dmg.percent >= STOMP_AT;
    }
    for (i = 0; i < 4; i++)
    {
        ftHit *hit = &fd->hitbox[i];
        if (!hit->active)
            continue;
        Fighter_UpdateHitboxDamage(hit, (int)(STOMP_DAMAGE * stomp[p] + 0.5f), gobj);
        if (down)
        {
            hit->angle = STOMP_ANGLE;
            hit->kb_growth = STOMP_KBG;
            hit->kb = STOMP_BKB;
        }
    }
}
void Kick_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (FrameTimerCheck(gobj) == 0)
    {
        Fighter_EnterFall(gobj);
        fd->phys.self_vel.X = HOP_X * fd->input.lstick.X;
        fd->phys.self_vel.Y = HOP_Y;
    }
}

// putting a weapon away: the clip, then free
void Cancel_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (FrameTimerCheck(gobj) == 0)
    {
        if (fd->phys.air_state == 0)
            Fighter_EnterWait(gobj);
        else
            Fighter_EnterFall(gobj);
    }
}
void Cancel_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Cancel_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_IgnoreLedge(gobj, Fighter_EnterLanding);
}

static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id, was = last_state[p];
    last_state[p] = s;
    if (fd->phys.air_state == 0)
        wall_count[p] = 0;                              // the whip may catch walls again once he has stood
    // the bow: between two frames of drawing it shield puts it away and a jump leaves it; once it is drawn,
    // the stick held the other way turns him round
    if (s == 341 || s == 342 || s == 345 || s == 346)
    {
        if (bow_age[p] < 60000)
            bow_age[p]++;
        if (bow_age[p] >= BOW_CANCEL_FROM && bow_age[p] <= BOW_CANCEL_TO)
        {
            if (fd->input.down & SHIELD_BUTTONS)
            {
                Enter(gobj, ST_BOW_CANCEL);
                return;
            }
            if (fd->phys.air_state == 0 ? Fighter_IASACheck_JumpF(gobj) : Fighter_IASACheck_JumpAerial(gobj))
                return;
        }
        if (bow_wait[p])
            bow_wait[p]--;
        if ((s == 342 || s == 346) && !bow_wait[p] && fd->input.lstick.X * fd->facing_direction < -0.5f)
        {
            bow_wait[p] = BOW_TURN_WAIT;
            Turn_Round(fd);
        }
    }
    else
    {
        bow_age[p] = 0;
        bow_wait[p] = 0;
    }
    // the axe: shield puts it away before its swing is past stopping; held back, it can be turned round once,
    // the stick the other way. (The game does this in its code, not in the move's script: the frames are the
    // published ones.)
    if (s == 369 || s == 371)
    {
        if (fd->state.frame < 2.0f)
            axe_turned[p] = 0;
        if ((fd->input.down & SHIELD_BUTTONS) && fd->state.frame < AXE_COMMIT)
        {
            Enter(gobj, ST_AXE_CANCEL);
            return;
        }
        if (!axe_turned[p] && fd->state.frame >= AXE_TURN_FROM && fd->state.frame <= AXE_TURN_TO
            && fd->input.lstick.X * fd->facing_direction < -0.5f)
        {
            axe_turned[p] = 1;
            Turn_Round(fd);
        }
    }
    // the side special on the ground, asked for with a flick of the stick: he dashes forward as it starts
    if (s == ST_SIDE)
    {
        if (s != was)
            lunge[p] = fd->input.timer_lstick_smash_x < LUNGE_FLICK ? LUNGE_SPEED : 0.0f;
        if (lunge[p] > 0.0f && fd->phys.air_state == 0)
        {
            fd->phys.self_vel_ground.X = (lunge[p] + fd->attr.ground_friction) * fd->facing_direction;
            lunge[p] -= LUNGE_BRAKE;
        }
    }
    else
        lunge[p] = 0;
    if (fd->state_id != ST_WHIP && fd->state_id != ST_WHIP_AIR)
    {
        whip_started[p] = 0;
        return;
    }
    // thrown in the air, the whip starts with a speed upward of the fighter's own (the game sets it as the
    // special starts: without it the fighter falls all through the throw, out of the whip's reach of a ledge)
    if (fd->state_id == ST_WHIP_AIR && !whip_started[p])
    {
        whip_started[p] = 1;
        fd->phys.self_vel.Y = WHIP_START_Y;
    }
    if (fd->state.frame < 2.0f)
        latched[p] = 0;
    fd->cb.OnGiveDamage = Whip_Hit;
    if (latched[p])
    {
        latched[p] = 0;
        if (fd->phys.air_state == 0)
            Fighter_SetAirborne(fd);
        Enter(gobj, ST_LATCH);
    }
    else if (fd->state_id == ST_WHIP_AIR && wall_count[p] < WALL_MAX
             && fd->state.frame >= WALL_FROM && fd->state.frame <= WALL_TO)
    {
        // the whip's tip has gone into a wall: he is drawn to where it struck and jumps off (the game's code
        // does this; a wall is looked for along the line from his hand to the tip)
        Vec3 tip, hit, normal;
        int line;
        u32 flags;
        float ax = fd->phys.pos.X, ay = fd->phys.pos.Y + WALL_HAND;
        JOBJ_GetWorldPosition(fd->bones[TIP_JOINT].joint, 0, &tip);
        if (WALL_LEFT(ax, ay, tip.X, tip.Y, &hit, &line, &flags, &normal, -1, -1)
            || WALL_RIGHT(ax, ay, tip.X, tip.Y, &hit, &line, &flags, &normal, -1, -1))
        {
            wall_away[p] = hit.X < ax ? 1.0f : -1.0f;
            latch_at[p].X = hit.X + wall_away[p] * 3.0f;
            latch_at[p].Y = hit.Y - WALL_HAND;
            latch_at[p].Z = fd->phys.pos.Z;
            latch_on[p] = -1;
            on_wall[p] = 1;
            wall_count[p]++;
            Enter(gobj, ST_LATCH);
        }
    }
}

// The neutral special lets an item of the fighter's own go when the move's script raises a flag.
#define SHOT_JOINT 75
typedef struct ShotAttr { float speed; float life; } ShotAttr;   // the item's own attributes (items.py)

// A shot that crosses the stage in a few frames is seen as a beam of light, not as a thing that flies: a
// streak from where it was let go to where it has got to, which fades once the shot is gone. It is drawn
// after the fighter itself, in the fighter's own drawing (see-through shapes go in the last pass).
#define BEAM_ITEM 1
#define BEAM_SPEED 26.322232316101374f
#define BEAM_LIFE 10.0f               // frames the shot flies
#define BEAM_FADE 16.0f               // frames the streak takes to fade after that
#define BEAM_HALF 2.8954455547711513f               // half the streak's height
#ifndef DRAW_RECT
#define DRAW_RECT ((void (*)(Vec3 *, Vec3 *, GXColor *))0x80009DD4)                                 // lbColl_80009DD4
#endif
static Vec3 beam_from[6];
static float beam_way[6];
static u8 beam_age[6];                  // frames since the shot was let go (0: no streak)
static void (*beam_drawn[6])(GOBJ *, int);      // the drawing of the fighter that this goes after

static void Beam_Draw(GOBJ *gobj, int pass)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply, i;
    if (beam_drawn[p])
        beam_drawn[p](gobj, pass);
    if (pass != 2 || !beam_age[p])
        return;
    {
        float age = (float)beam_age[p];
        float reach = BEAM_SPEED * (age < BEAM_LIFE ? age : BEAM_LIFE);
        float fade = age <= BEAM_LIFE ? 1.0f : 1.0f - (age - BEAM_LIFE) / BEAM_FADE;
        float x0 = beam_from[p].X, x1 = x0 + beam_way[p] * reach;
        static const float half[3] = {BEAM_HALF, BEAM_HALF * 0.55f, BEAM_HALF * 0.2f};
        GXColor c[3] = {{255, 218, 106, 185}, {255, 237, 195, 235}, {255, 255, 244, 255}};
        if (fade < 0.0f)
            fade = 0.0f;
        for (i = 0; i < 3; i++)
        {
            Vec3 a, b;
            c[i].a = (u8)((float)c[i].a * fade);
            a.X = x0 < x1 ? x0 : x1;
            b.X = x0 < x1 ? x1 : x0;
            a.Y = beam_from[p].Y - half[i];
            b.Y = beam_from[p].Y + half[i];
            a.Z = fd->phys.pos.Z + 4.0f + 0.2f * (float)i;
            b.Z = a.Z;
            DRAW_RECT(&a, &b, &c[i]);
        }
    }
}

static void BeamFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply;
    if (!beam_drawn[p] && gobj->gx_cb)              // once: put after the drawing the fighter has now
    {
        beam_drawn[p] = (void (*)(GOBJ *, int))gobj->gx_cb;
        gobj->gx_cb = (void (*)())Beam_Draw;
    }
    if (beam_age[p] && ++beam_age[p] > (u8)(BEAM_LIFE + BEAM_FADE))
        beam_age[p] = 0;
}

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    beam_age[fd->ply] = 0;
    beam_drawn[fd->ply] = 0;
    ItemDesc **items = fd->ftData->items;
    if (!items)
        return;
    if (items[0])
        MEX_IndexFighterItem(fd->kind, items[0], 0);
    if (items[1])
        MEX_IndexFighterItem(fd->kind, items[1], 1);
}

static void Shoot(GOBJ *gobj, int which, float rise)
{
    FighterData *fd = gobj->userdata;
    Vec3 from, centre;
    JOBJ_GetWorldPosition(fd->bones[SHOT_JOINT].joint, 0, &from);
    Fighter_GetECBPosition(gobj, &centre);
    SpawnItem spawn;
    spawn.parent_gobj = gobj;
    spawn.parent_gobj2 = gobj;
    spawn.it_kind = MEX_GetFtItemID(gobj, which);
    spawn.pos = centre;
    spawn.pos2.X = from.X;
    spawn.pos2.Y = rise < 0 ? from.Y : fd->phys.pos.Y + rise;      // at the joint, or standing on the fighter's ground
    spawn.pos2.Z = 0;
    spawn.vel.X = 0;
    spawn.vel.Y = 0;
    spawn.vel.Z = 0;
    spawn.facing_direction = fd->facing_direction;
    spawn.damage = 0;
    spawn.unk6 = 0;
    spawn.is_raycast_below = 1;
    GOBJ *item = Item_CreateItem1(&spawn);
    if (!item)
        return;
    ItemData *id = item->userdata;
    ShotAttr *attr = (ShotAttr *)id->itData->param_ext;
    id->self_vel.X = attr->speed * id->facing_direction;
    id->self_vel.Y = 0;
    id->self_vel.Z = 0;
    // it moves twice before its hitbox is first checked: start it that far back, so the first check is where it leaves
    id->pos.X -= 2.0f * id->self_vel.X;
    Item_SetLifeTimer(item, attr->life);
    ItemStateChange(item, 0, 2);
    Item_CopyDevelopState(item, gobj);
    Item_UpdatePhysAndColl(item);
    JOBJ *model = item->hsd_object;                 // built pointing right: turned when it is let go to the left
    model->rot.Y = id->self_vel.X < 0 ? 3.14159265f : 0;
    JOBJ_SetMtxDirtySub(model);
    if (which == BEAM_ITEM)
    {
        beam_from[fd->ply] = spawn.pos2;
        beam_way[fd->ply] = fd->facing_direction;
        beam_age[fd->ply] = 1;
    }
}

static void ShotFrame(FighterData *fd, GOBJ *gobj)
{
    int s = fd->state_id;
    if (!fd->ftcmd_var.flag1)
        return;
    if (s == 343 || s == 347)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 0, -1.00f);
        return;
    }
    if (s == 344 || s == 348)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 1, -1.00f);
        return;
    }
}

// A special that cannot be knocked out of its swing: armour while its clip is between two frames.
#define ARMOUR 4321.0f      // more than any hit's knockback; also how this code knows its own
static void ArmourFrame(FighterData *fd)
{
    if ((fd->state_id == 369 || fd->state_id == 371) && fd->state.frame + 1.0f >= 34.0f && fd->state.frame + 1.0f <= 66.0f)
        fd->dmg.armor = ARMOUR;
    else if (fd->dmg.armor == ARMOUR)
        fd->dmg.armor = 0;
}

// A whip thrown towards a ledge: while it is out, the fighter's ledge catch reaches as far as the whip does.
// (The donor's own air up special catches a ledge when its script flags say so; the script raises them.)
#define TETHER_X 18.028246608426517f
static void TetherFrame(FighterData *fd)
{
    FtCollDesc *own = fd->ftData->coll;
    if (fd->state_id == 368 && fd->state.frame >= 9.0f && fd->state.frame <= 24.0f)
    {
        fd->coll_data.cliffgrab_width = TETHER_X;
        fd->coll_data.cliffgrab_y_offset = 27.207519542483585f;
        fd->coll_data.cliffgrab_height = 54.41503908496717f;
    }
    else if (fd->coll_data.cliffgrab_width == TETHER_X)
    {
        fd->coll_data.cliffgrab_width = own->cliffgrab_width;
        fd->coll_data.cliffgrab_y_offset = own->cliffgrab_y_offset;
        fd->coll_data.cliffgrab_height = own->cliffgrab_height;
    }
}

// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (!(s == 341 || s == 345 || s == 349 || s == 358 || s == 367 || s == 368 || s == 369 || s == 371) || fd->state.frame > 4.0f)
        return;
    if (fd->input.lstick.X * fd->facing_direction < -0.5f)
    {
        fd->facing_direction = -fd->facing_direction;
        fd->phys.self_vel.X = -fd->phys.self_vel.X;
        Fighter_SetBoneRotY(fd, 0, 1.5707963f * fd->facing_direction);
    }
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    ShotFrame(fd, gobj);
    BeamFrame(fd, gobj);
    ArmourFrame(fd);
    TetherFrame(fd);
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
    {318, 0x340613, 0x14, 0x0, Latch_Anim, 0, Latch_Phys, Own_AirColl, Fighter_UpdateCameraBox},    // 373: the whip has caught something
    {320, 0x340613, 0x14, 0x0, Kick_Anim, 0, Kick_Phys, Own_AirColl, Fighter_UpdateCameraBox},    // 374: the kick off it
    {304, 0x340213, 0x15, 0x0, Cancel_Anim, 0, Cancel_Phys, Cancel_Coll, Fighter_UpdateCameraBox},    // 375: the bow put away
    {305, 0x340213, 0x15, 0x0, Cancel_Anim, 0, Cancel_Phys, Cancel_Coll, Fighter_UpdateCameraBox},    // 376: the axe put away
    {319, 0x340613, 0x14, 0x0, WallJump_Anim, 0, WallJump_Phys, Own_AirColl, Fighter_UpdateCameraBox},    // 377: the jump off a wall the whip caught
};

// The fighter's own sword trail: where the game's trail runs along the blade, in its trail's colour.
static AfterImageDesc trail = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {252, 136, 98, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 0.877, .offset_top = 15.352,
};

AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    return &trail;
}
