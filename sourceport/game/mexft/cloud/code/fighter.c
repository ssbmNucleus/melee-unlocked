#include "../mex.h"

// ---- Cloud's own states ----------------------------------------------------------------------
#define ST_DIVE 373
#define ST_DIVE_LAND 374
#define ST_CHARGE_START 375
#define ST_CHARGE_LOOP 376
#define ST_CHARGE_END 377
#define ST_RISE 367
#define ST_RISE_AIR 368
#define B_BUTTON 0x200
#define CANCEL_BUTTONS 0xC70                            // shield (R, L), a jump (X, Y), a grab (Z)
#define CHARGE_AGAIN 8                     // frames into the charge before the special button ends it
#define N_START 341                               // the states the neutral special starts in, ground and air
#define N_START_AIR 345
#define N_GROUND_X 0.20000000298023224f                      // what he keeps of his speed as it starts (the game's own numbers)
#define N_AIR_X 0.4000000059604645f
#define N_AIR_Y 0.6499999761581421f
#define LIMIT_PER_FRAME (1.0f / 334.0f)     // the game: 334 frames of charging fill the gauge
#define LIMIT_PER_PERCENT (1.0f / 100.0f)   // the game: 100% taken fills it
#define LIMIT_PER_DEALT (1.0f / 150.0f)      // and 150% dealt
#define LIMIT_HOLD 900                         // frames a full gauge lasts unused (the game's own number)
// What the gauge looks like. As in the game it is part of the player's damage display, a bar by the damage
// number that is always there: flat rectangles drawn with the game's own routine for one, by an object of
// its own on the list the display's camera draws (the game's own displays are objects on that list). On
// the fighter: a blue light that grows as the gauge fills and beats once it is full, an aura of rising
// streaks while it charges or is full, and a gleam on the sword at the moment it fills.
#define HUD_LIST 11                                     // the drawing list of the damage displays (ifstatus.c)
#define HUD_PLACE ((Vec3 *(*)(int))0x802F3424)          // ifAll_802F3424: where a player's damage display sits
#define HUD_CAMERA ((GOBJ *(*)(void))0x802F3404)        // ifAll_802F3404: the object that holds the displays' camera
#define GAUGE_X -2.40f                           // the bar's left end, from the display's own point
#define GAUGE_Y 4.70f                           // its lower edge
#define GAUGE_WIDE 8.00f
#define GAUGE_TALL 0.60f
#define GAUGE_Z 0.00f                           // toward the camera from the display's own pieces
#define GAUGE_EDGE 0.14f
#define AURA_TALL 21.4f                      // how high the streaks of the aura rise
#define AURA_STREAKS 8
#define OVER_Y (AURA_TALL - 1.5f)                       // the bar over his head while he charges: just clear of his hair
#define OVER_WIDE 10.0f
#define OVER_TALL 0.9f
#define OVER_EDGE 0.25f
#define DRAW_RECT ((void (*)(Vec3 *, Vec3 *, GXColor *))0x80009DD4)                                 // lbColl_80009DD4
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834

static float limit[6];              // each player's Limit gauge, 0 to 1 (2 once it is full and has been seen full)
static float seen_percent[6];
static float their_percent[6][6];   // what each other fighter's damage was last frame (damage dealt fills the gauge)
static int limit_left[6];           // frames a full gauge has left
static u8 boosted[6];               // the fighter's numbers are the full gauge's
static u16 charge_age[6];           // frames since the charge now under way began
static u8 charge_armed[6];          // the stick has been let back to the middle since it began
static u8 was_charging[6];          // he was charging on the frame before this one
static int last_state[6];
static float plain[6][10];   // the fighter's own numbers, kept from when it loaded
// With a full gauge the fighter's own numbers change (the game keeps a table of them: which number, times what).
static void Boost(FighterData *fd, int on)
{
    int p = fd->ply;
    if (on == boosted[p])
        return;
    boosted[p] = on;
    fd->attr.aerial_drift_base = plain[p][0] * (on ? 1.2f : 1.0f);
    fd->attr.aerial_drift_max = plain[p][1] * (on ? 1.2f : 1.0f);
    fd->attr.aerial_drift_stick_mult = plain[p][2] * (on ? 1.2f : 1.0f);
    fd->attr.aerial_friction = plain[p][3] * (on ? 1.15f : 1.0f);
    fd->attr.dash_initial_velocity = plain[p][4] * (on ? 1.1f : 1.0f);
    fd->attr.dashrun_terminal_velocity = plain[p][5] * (on ? 1.1f : 1.0f);
    fd->attr.gravity = plain[p][6] * (on ? 1.1f : 1.0f);
    fd->attr.ground_friction = plain[p][7] * (on ? 1.15f : 1.0f);
    fd->attr.terminal_velocity = plain[p][8] * (on ? 1.1f : 1.0f);
    fd->attr.walk_maximum_velocity = plain[p][9] * (on ? 1.15f : 1.0f);
}
static u8 dive_asked[6];
static u8 glow_tick[6];             // counts frames, for the beat of the light
static u8 lit[6];                   // this code has its light on the fighter
static u8 aura[6];                  // 0 no aura, 1 the gauge is full, 2 it is being charged
static void (*drawn[6])(GOBJ *, int);       // the game's own drawing of each fighter: the aura is drawn after it

// The gauge, drawn with the player's damage display: a dark edge, a dim back, and the bar itself, which is
// bright and beats once the gauge is full. (Only when the camera drawing is the displays' own: an object on
// this list in another scene, the results screen say, is not theirs to draw. It is drawn in the last pass and
// after the display's own pieces, or the see-through emblem behind the number is drawn over it. The routine
// multiplies a colour by itself, so these are the roots of the colours meant.)
static void Gauge_Draw(GOBJ *gobj, int pass)
{
    int p = (int)gobj->userdata;
    GOBJ *camera = HUD_CAMERA();
    Vec3 *at = HUD_PLACE(p), a, b;
    float full = limit[p] >= 1.0f ? 1.0f : limit[p];
    GXColor edge = {0, 0, 0, 255}, back = {84, 92, 120, 255}, bar = {96, 190, 255, 255};
    if (pass != 2 || !camera || camera->hsd_object != (void *)COBJ_GetCurrent())
        return;
    if (limit[p] >= 1.0f)
    {
        int beat = glow_tick[p] % 20;
        int k = beat < 10 ? beat : 20 - beat;
        bar.r = 170 + 8 * k;
        bar.g = 225 + 3 * k;
    }
    a.X = at->X + GAUGE_X - GAUGE_EDGE;
    a.Y = at->Y + GAUGE_Y - GAUGE_EDGE;
    a.Z = at->Z + GAUGE_Z;
    b.X = at->X + GAUGE_X + GAUGE_WIDE + GAUGE_EDGE;
    b.Y = at->Y + GAUGE_Y + GAUGE_TALL + GAUGE_EDGE;
    b.Z = a.Z;
    DRAW_RECT(&a, &b, &edge);
    a.X = at->X + GAUGE_X;
    a.Y = at->Y + GAUGE_Y;
    b.X = a.X + GAUGE_WIDE;
    b.Y = a.Y + GAUGE_TALL;
    DRAW_RECT(&a, &b, &back);
    if (full > 0.0f)
    {
        b.X = a.X + GAUGE_WIDE * full;
        DRAW_RECT(&a, &b, &bar);
    }
}

static void Gauge_Make(int p)
{
    GOBJ *gobj = GObj_Create(0xE, 0xF, 0);              // the kind of object the game's own displays are
    gobj->userdata = (void *)p;
    GObj_AddGXLink(gobj, Gauge_Draw, HUD_LIST, 1);      // after the display's own pieces
}

static void OwnLoad(FighterData *fd)
{
    int i;
    limit[fd->ply] = 0;
    seen_percent[fd->ply] = 0;
    limit_left[fd->ply] = 0;
    boosted[fd->ply] = 0;
    charge_age[fd->ply] = 0;
    charge_armed[fd->ply] = 0;
    was_charging[fd->ply] = 0;
    last_state[fd->ply] = -1;
    for (i = 0; i < 6; i++)
        their_percent[fd->ply][i] = -1.0f;
    plain[fd->ply][0] = fd->attr.aerial_drift_base;
    plain[fd->ply][1] = fd->attr.aerial_drift_max;
    plain[fd->ply][2] = fd->attr.aerial_drift_stick_mult;
    plain[fd->ply][3] = fd->attr.aerial_friction;
    plain[fd->ply][4] = fd->attr.dash_initial_velocity;
    plain[fd->ply][5] = fd->attr.dashrun_terminal_velocity;
    plain[fd->ply][6] = fd->attr.gravity;
    plain[fd->ply][7] = fd->attr.ground_friction;
    plain[fd->ply][8] = fd->attr.terminal_velocity;
    plain[fd->ply][9] = fd->attr.walk_maximum_velocity;
    dive_asked[fd->ply] = 0;
    lit[fd->ply] = 0;
    aura[fd->ply] = 0;
    drawn[fd->ply] = 0;
    Gauge_Make(fd->ply);
}

static void Limit_Draw(GOBJ *gobj, int pass)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    if (drawn[p])
        drawn[p](gobj, pass);
    if (fd->state_id < 12)                                    // nothing while he is gone
        return;
    if (pass == 2 && aura[p])
    {
        // The aura: thin streaks of light that rise round the fighter and fade as they rise, half of them in
        // front of him and half behind; twice as quick while he charges. (See-through shapes go in the last
        // pass. The routine multiplies a colour by itself, so these are the roots of the colour meant.)
        int i;
        for (i = 0; i < AURA_STREAKS; i++)
        {
            float t = (float)((glow_tick[p] * aura[p] + i * 11) & 31) / 32.0f;
            GXColor glow = {196, 234, 255, (u8)(210.0f * (1.0f - t))};
            Vec3 a, b;
            a.X = fd->phys.pos.X + (float)((i * 37) % 13) - 6.4f;
            b.X = a.X + 0.8f;
            a.Y = fd->phys.pos.Y + t * AURA_TALL;
            b.Y = a.Y + 3.0f + 4.0f * (1.0f - t);
            a.Z = fd->phys.pos.Z + ((i & 1) ? 5.0f : -5.0f);
            b.Z = a.Z;
            DRAW_RECT(&a, &b, &glow);
        }
    }
    if (pass == 2 && aura[p] == 2)
    {
        // While it is being charged the gauge is shown over the fighter as well as by his damage (the game
        // does both): the same bar, just above his head.
        float full = limit[p] >= 1.0f ? 1.0f : limit[p];
        GXColor edge = {0, 0, 0, 255}, back = {84, 92, 120, 255}, bar = {96, 190, 255, 255};
        Vec3 a, b;
        a.X = fd->phys.pos.X - OVER_WIDE * 0.5f - OVER_EDGE;
        a.Y = fd->phys.pos.Y + OVER_Y - OVER_EDGE;
        a.Z = fd->phys.pos.Z + 6.0f;
        b.X = fd->phys.pos.X + OVER_WIDE * 0.5f + OVER_EDGE;
        b.Y = fd->phys.pos.Y + OVER_Y + OVER_TALL + OVER_EDGE;
        b.Z = a.Z;
        DRAW_RECT(&a, &b, &edge);
        a.X += OVER_EDGE;
        a.Y += OVER_EDGE;
        b.X -= OVER_EDGE;
        b.Y -= OVER_EDGE;
        DRAW_RECT(&a, &b, &back);
        if (full > 0.0f)
        {
            b.X = a.X + OVER_WIDE * full;
            DRAW_RECT(&a, &b, &bar);
        }
    }
}

// The light: the fighter's second colour overlay, which the game draws with when none of its own colour
// animations is running (and which this leaves alone while the game is using it).
static void Limit_Light(FighterData *fd, int p, int charging)
{
    ColorOverlay *c = &fd->color[1];
    int a = 0;
    aura[p] = limit[p] >= 1.0f ? 1 : charging ? 2 : 0;
    if (c->colanim != 0)
        return;
    if (limit[p] >= 1.0f)
    {
        int beat = glow_tick[p] % 40;
        a = 40 + 2 * (beat < 20 ? beat : 40 - beat);
    }
    else if (charging)
        a = 20 + (int)(limit[p] * 40.0f) + ((glow_tick[p] & 7) < 4 ? 15 : 0);
    if (a)
    {
        // The game keeps the colour twice: as numbers it steps every frame (by "blend" amounts) and as the
        // bytes it draws with, which it writes from the numbers every frame. Setting the bytes alone shows
        // whatever the numbers were left at (a pale grey), so both are set and the steps are zeroed.
        c->color_red = 40.0f;
        c->color_green = 120.0f;
        c->color_blue = 255.0f;
        c->color_alpha = (float)a;
        c->colorblend_red = 0;
        c->colorblend_green = 0;
        c->colorblend_blue = 0;
        c->colorblend_alpha = 0;
        c->hex.r = 40;
        c->hex.g = 120;
        c->hex.b = 255;
        c->hex.a = a;
        c->color_enable = 1;
        lit[p] = 1;
    }
    else if (lit[p])
    {
        c->color_enable = 0;
        lit[p] = 0;
    }
}

static void Enter(GOBJ *gobj, int state)
{
    ActionStateChange(0, 1, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
}

// Climhazzard, second half: hang for the turn of the sword, then straight down until the ground.
void Dive_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    fd->phys.self_vel.X = 0;
    fd->phys.self_vel.Y = fd->state.frame >= 12.0f ? -6.502f : 0;
}
static void Dive_Landed(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    Fighter_SetGrounded2(fd);
    Enter(gobj, ST_DIVE_LAND);
}
void Dive_Coll(GOBJ *gobj)
{
    Fighter_CollAir_IgnoreLedge(gobj, Dive_Landed);
}
void DiveLand_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Fighter_EnterWait(gobj);
}
void Ground_Phys(GOBJ *gobj)
{
    Fighter_PhysGround_ApplyFriction(gobj);
}
void Ground_Coll(GOBJ *gobj)
{
    Fighter_CollGround_StopLedge_EnterFall(gobj);
}

// Limit Charge: a start, a loop, an end. Once started it goes on by itself until the gauge is full. The special
// button again ends it (not in its first frames: that press started it), and so do shield, a jump, a grab and
// the stick to a side, which go straight to what was asked for where he can do it.
static void Charge_Leave(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_EnterWait(gobj);
    else
        Fighter_EnterFall(gobj);
}
static void Charge_Count(FighterData *fd)
{
    int p = fd->ply;
    float sx = fd->input.lstick.X;
    if (charge_age[p] < 60000)
        charge_age[p]++;
    if (sx < 0.3f && sx > -0.3f)
        charge_armed[p] = 1;
}
void ChargeStart_Anim(GOBJ *gobj)
{
    Charge_Count(gobj->userdata);
    if (FrameTimerCheck(gobj) == 0)
        Enter(gobj, ST_CHARGE_LOOP);
}
void ChargeLoop_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    int p = fd->ply;
    float sx = fd->input.lstick.X;
    Charge_Count(fd);
    limit[p] += LIMIT_PER_FRAME;
    if (limit[p] >= 1.0f || ((fd->input.down & B_BUTTON) && charge_age[p] >= CHARGE_AGAIN))
        Enter(gobj, ST_CHARGE_END);
    else if ((fd->input.down & CANCEL_BUTTONS) || fd->input.lstick.Y > 0.66f
             || (charge_armed[p] && (sx > 0.6f || sx < -0.6f)))
    {
        if (!(fd->phys.air_state == 0 ? Fighter_IASACheck_AllGrounded(gobj) : Fighter_IASACheck_AllAerial(gobj)))
            Enter(gobj, ST_CHARGE_END);
    }
    else if (FrameTimerCheck(gobj) == 0)
        Enter(gobj, ST_CHARGE_LOOP);
}
void ChargeEnd_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Charge_Leave(gobj);
}
void Charge_Phys(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_PhysGround_ApplyFriction(gobj);
    else
        Fighter_PhysAir_ApplyGravityFastfall(gobj);
}
void Charge_Coll(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state == 0)
        Fighter_CollGround_StopLedge_EnterFall(gobj);
    else
        Fighter_CollAir_IgnoreLedge(gobj, Fighter_EnterLanding);
}

// The down special: Finishing Touch (the donor's down special states, which play it) with a full
// gauge, the charge otherwise.
static void DownSpecial(GOBJ *gobj, int finishing)
{
    FighterData *fd = gobj->userdata;
    if (limit[fd->ply] >= 1.0f)
    {
        limit[fd->ply] = 0;
        fd->phys.self_vel.Y = 0;
        fd->ftcmd_var.flag1 = 0;                        // the donor's own flag for this state: its window stays shut
        Enter(gobj, finishing);
    }
    else
    {
        charge_age[fd->ply] = 0;
        charge_armed[fd->ply] = 0;
        Enter(gobj, ST_CHARGE_START);
    }
}
void SpecialLw(GOBJ *gobj)
{
    DownSpecial(gobj, 369);
}
void SpecialAirLw(GOBJ *gobj)
{
    DownSpecial(gobj, 371);
}

// ---- the Limit versions of the other three specials ---------------------------------------------
#define ST_LIMIT_N 378
#define ST_LIMIT_N_AIR 379
#define ST_LIMIT_S1 380
#define ST_LIMIT_S2 381
#define ST_LIMIT_S3 382
#define ST_LIMIT_S1_AIR 383
#define ST_LIMIT_S2_AIR 384
#define ST_LIMIT_S3_AIR 385
#define ST_LIMIT_RISE 386
#define ST_LIMIT_RISE_AIR 387
#define ST_S1 349
#define ST_S1_AIR 358
#define LIMIT_RISE 1.600f              // how much higher the Limit up special goes

static u8 beam_limit[6];            // the neutral special now under way lets the Limit beam go
static u8 slash_hit[6];             // the side special's first swing has hit something
static int seen_state[6];

static void EnterAt(GOBJ *gobj, int state, float rate)
{
    ActionStateChange(0, rate, 0, gobj, state, 0, 0);
    Fighter_AdvanceScript(gobj);
}
static void Slash_Hit(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    slash_hit[fd->ply] = 1;
}

// Limit Cross Slash: once the first swing has hit, the rest follow by themselves
void LimitS1_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (slash_hit[fd->ply] && fd->state.frame >= 13.0f)
        EnterAt(gobj, fd->phys.air_state == 0 ? ST_LIMIT_S2 : ST_LIMIT_S2_AIR, 1.0000f);
    else if (FrameTimerCheck(gobj) == 0)
        Charge_Leave(gobj);
}
void LimitS2_Anim(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->state.frame >= 7.0f)
        EnterAt(gobj, fd->phys.air_state == 0 ? ST_LIMIT_S3 : ST_LIMIT_S3_AIR, 1.3333f);
    else if (FrameTimerCheck(gobj) == 0)
        Charge_Leave(gobj);
}
void LimitEnd_Anim(GOBJ *gobj)
{
    if (FrameTimerCheck(gobj) == 0)
        Charge_Leave(gobj);
}

// Limit Climhazzard: the donor's own rise, with what its clip gives each frame made that much more
static void Rise_More(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    if (fd->phys.air_state != 0 && fd->ftcmd_var.flag0 && fd->phys.self_vel.Y > 0)
        fd->phys.self_vel.Y *= LIMIT_RISE;
}
void LimitRise_Phys(GOBJ *gobj)
{
    ((void (*)(GOBJ *))0x80138638)(gobj);
    Rise_More(gobj);
}
void LimitRiseAir_Phys(GOBJ *gobj)
{
    ((void (*)(GOBJ *))0x8013873c)(gobj);
    Rise_More(gobj);
}

static void LimitFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply, s = fd->state_id;
    int entered = s != seen_state[p] && fd->state.frame < 2.0f;
    if (entered && limit[p] >= 1.0f)
    {
        // a special started with a full gauge is its Limit version, and the gauge is spent
        int to = 0;
        if (s == 343 || s == 344)
            to = ST_LIMIT_N;
        else if (s == 347 || s == 348)
            to = ST_LIMIT_N_AIR;
        else if (s == ST_S1)
            to = ST_LIMIT_S1;
        else if (s == ST_S1_AIR)
            to = ST_LIMIT_S1_AIR;
        else if (s == ST_RISE)
            to = ST_LIMIT_RISE;
        else if (s == ST_RISE_AIR)
            to = ST_LIMIT_RISE_AIR;
        if (to)
        {
            limit[p] = 0;
            if (to == ST_LIMIT_N || to == ST_LIMIT_N_AIR)
                beam_limit[p] = 1;
            EnterAt(gobj, to, (to == ST_LIMIT_S1 || to == ST_LIMIT_S1_AIR) ? 1.0000f : 1.0f);
            s = to;
        }
    }
    if (!(s == 341 || s == 342 || s == 343 || s == 344 || s == 345 || s == 346 || s == 347 || s == 348 || s == 378 || s == 379))
        beam_limit[p] = 0;
    if (s == ST_S1 || s == ST_S1_AIR || s == ST_LIMIT_S1 || s == ST_LIMIT_S1_AIR)
    {
        if (entered)
            slash_hit[p] = 0;
        fd->cb.OnGiveDamage = Slash_Hit;
        fd->cb.OnShieldHit = Slash_Hit;
        // the plain one goes on only once its first swing has hit: its window is passed on then
        if (s == ST_S1 || s == ST_S1_AIR)
            fd->ftcmd_var.flag0 = (fd->ftcmd_var.flag3 && slash_hit[p]) ? 1 : 0;
    }
    seen_state[p] = fd->state_id;
}

static void OwnFrame(FighterData *fd, GOBJ *gobj)
{
    int p = fd->ply;
    LimitFrame(fd, gobj);
    // damage taken fills the gauge; taken while he was charging it, it takes that much off it instead
    if (fd->dmg.percent > seen_percent[p])
    {
        float by = (fd->dmg.percent - seen_percent[p]) * LIMIT_PER_PERCENT;
        if (!was_charging[p])
            limit[p] += by;
        else if (limit[p] < 1.0f)
            limit[p] = limit[p] > by ? limit[p] - by : 0.0f;
    }
    seen_percent[p] = fd->dmg.percent;
    // the neutral special: as it starts he keeps only part of the speed he had
    if (fd->state_id != last_state[p])
    {
        if (fd->state_id == N_START)
            fd->phys.self_vel_ground.X *= N_GROUND_X;
        else if (fd->state_id == N_START_AIR)
        {
            fd->phys.self_vel.X *= N_AIR_X;
            fd->phys.self_vel.Y *= N_AIR_Y;
        }
    }
    was_charging[p] = fd->state_id >= ST_CHARGE_START && fd->state_id <= ST_CHARGE_END;
    last_state[p] = fd->state_id;
    // and damage dealt: what another fighter's damage went up by, when this one was the one that hit it
    {
        int i;
        for (i = 0; i < 6; i++)
        {
            GOBJ *other = Fighter_GetGObj(i);
            if (other && other != gobj)
            {
                FighterData *od = other->userdata;
                float was = their_percent[p][i];
                if (was >= 0.0f && od->dmg.percent > was && od->dmg.source_ply == p && limit[p] < 1.0f)
                    limit[p] += (od->dmg.percent - was) * LIMIT_PER_DEALT;
                their_percent[p][i] = od->dmg.percent;
            }
            else
                their_percent[p][i] = -1.0f;
        }
    }
    if (limit[p] >= 1.0f)
    {
        if (limit[p] != 2.0f)
        {
            limit_left[p] = LIMIT_HOLD;
            Vec3 none = {0, 0, 0};
            SPAWN_EFFECT(gobj, 1011, 75, 0, 0, &none, &none, 0.0f);
        }
        limit[p] = 2.0f;
        // a full gauge left unused runs out
        if (--limit_left[p] <= 0)
            limit[p] = 0;
    }
    Boost(fd, limit[p] >= 1.0f);
    glow_tick[p]++;
    Limit_Light(fd, p, fd->state_id >= ST_CHARGE_START && fd->state_id <= ST_CHARGE_END);
    if ((void *)gobj->gx_cb != (void *)Limit_Draw)
    {
        drawn[p] = (void (*)(GOBJ *, int))gobj->gx_cb;
        gobj->gx_cb = (void (*)())Limit_Draw;
    }
    // the up special: B again once it is rising asks for the dive, which starts where the rise ends
    if (fd->state_id == ST_RISE || fd->state_id == ST_RISE_AIR || fd->state_id == ST_LIMIT_RISE || fd->state_id == ST_LIMIT_RISE_AIR)
    {
        if (fd->state.frame < 2.0f)
            dive_asked[p] = 0;
        else if (fd->state.frame >= 11.0f && (fd->input.down & B_BUTTON))
            dive_asked[p] = 1;
        if (dive_asked[p] && fd->state.frame >= 24.0f)
        {
            dive_asked[p] = 0;
            Fighter_SetAirborne(fd);
            Enter(gobj, ST_DIVE);
        }
    }
}

// The neutral special lets an item of the fighter's own go when the move's script raises a flag.
#define SHOT_JOINT 75
typedef struct ShotAttr { float speed; float life; } ShotAttr;   // the item's own attributes (items.py)

void OnLoad(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    ((void (*)(GOBJ *))0x801364ac)(gobj);                 // the donor's own
    OwnLoad(fd);
    ItemDesc **items = fd->ftData->items;
    if (!items)
        return;
    if (items[0])
        MEX_IndexFighterItem(fd->kind, items[0], 0);
    if (items[1])
        MEX_IndexFighterItem(fd->kind, items[1], 1);
    if (items[2])
        MEX_IndexFighterItem(fd->kind, items[2], 2);
    if (items[3])
        MEX_IndexFighterItem(fd->kind, items[3], 3);
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
}

static void ShotFrame(FighterData *fd, GOBJ *gobj)
{
    int s = fd->state_id;
    if (!fd->ftcmd_var.flag1)
        return;
    if ((s == 343 || s == 344) && (beam_limit[fd->ply]) == 0)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 0, 5.65f);
        return;
    }
    if ((s == 347 || s == 348) && (beam_limit[fd->ply]) == 0)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 1, 5.65f);
        return;
    }
    if ((s == 378 || s == 343 || s == 344) && (beam_limit[fd->ply]) == 1)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 2, 5.45f);
        return;
    }
    if ((s == 379 || s == 347 || s == 348) && (beam_limit[fd->ply]) == 1)
    {
        fd->ftcmd_var.flag1 = 0;
        Shoot(gobj, 3, 5.45f);
        return;
    }
}

// The donor shuts its chain for good when the button is pressed before a step's window opens.
// The fighter ported here has no such rule: what the donor notes of an early press is wiped each frame.
static void ChainFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (s == 349 || s == 350 || s == 351 || s == 352 || s == 353 || s == 354 || s == 358 || s == 359 || s == 360 || s == 361 || s == 362 || s == 363)
        fd->ftcmd_var.flag1 = 0;
}

// A special can be turned around as it starts: the stick held the other way on its first frames
// (the game lets any special be reversed like this; in the air what speed he has turns with him).
static void ReverseFrame(FighterData *fd)
{
    int s = fd->state_id;
    if (!(((s == 341 || s == 345 || s == 349 || s == 358 || s == 367 || s == 368 || s == 369 || s == 371 || s == 375 || s == 380 || s == 383 || s == 386 || s == 387 || s == 342 || s == 346) && fd->state.frame <= 4.0f) || ((s == 343 || s == 344 || s == 347 || s == 348) && fd->state.frame <= 3.0f) || ((s == 378 || s == 379) && fd->state.frame <= 2.0f)))
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
    if (fd->phys.air_state != 0 || !(a == 55 || a == 58 || a == 59 || a == 67 || a == 303 || a == 328))
        return;
    float go = fd->transN_offset.Z * fd->facing_direction;
    if (go > 0)
        go += fd->attr.ground_friction;
    else if (go < 0)
        go -= fd->attr.ground_friction;
    fd->phys.self_vel_ground.X = go;
}

void OnFrame(GOBJ *gobj)
{
    FighterData *fd = gobj->userdata;
    OwnFrame(fd, gobj);
    ShotFrame(fd, gobj);
    ChainFrame(fd);
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
    {318, 0x340613, 0x14, 0x0, 0, 0, Dive_Phys, Dive_Coll, Fighter_UpdateCameraBox},    // 373: the dive
    {309, 0x340213, 0x14, 0x0, DiveLand_Anim, 0, Ground_Phys, Ground_Coll, Fighter_UpdateCameraBox},    // 374: its landing
    {310, 0x340213, 0x15, 0x0, ChargeStart_Anim, 0, Charge_Phys, Charge_Coll, Fighter_UpdateCameraBox},    // 375: Limit Charge starts
    {311, 0x340213, 0x15, 0x0, ChargeLoop_Anim, 0, Charge_Phys, Charge_Coll, Fighter_UpdateCameraBox},    // 376: Limit Charge
    {319, 0x340213, 0x15, 0x0, ChargeEnd_Anim, 0, Charge_Phys, Charge_Coll, Fighter_UpdateCameraBox},    // 377: Limit Charge ends
    {327, 0x340211, 0x12, 0x0, (void *)0x80136ee4, (void *)0x8013713c, (void *)0x80137144, (void *)0x80137184, (void *)0x800761c8},    // 378: Limit Blade Beam
    {332, 0x340611, 0x12, 0x0, (void *)0x80137010, (void *)0x80137140, (void *)0x80137164, (void *)0x801371c0, (void *)0x800761c8},    // 379: Limit Blade Beam in the air
    {328, 0x340212, 0x13, 0x0, LimitS1_Anim, 0, (void *)0x80137618, Charge_Coll, (void *)0x800761c8},    // 380: Limit Cross Slash, first swing
    {330, 0x340212, 0x13, 0x0, LimitS2_Anim, 0, (void *)0x80137870, Charge_Coll, (void *)0x800761c8},    // 381: Limit Cross Slash, second
    {331, 0x340212, 0x13, 0x0, LimitEnd_Anim, 0, (void *)0x80137bf4, Charge_Coll, (void *)0x800761c8},    // 382: Limit Cross Slash, the rest
    {333, 0x340612, 0x13, 0x0, LimitS1_Anim, 0, (void *)0x80137618, Charge_Coll, (void *)0x800761c8},    // 383: Limit Cross Slash in the air, first swing
    {335, 0x340612, 0x13, 0x0, LimitS2_Anim, 0, (void *)0x80137870, Charge_Coll, (void *)0x800761c8},    // 384: Limit Cross Slash in the air, second
    {336, 0x340612, 0x13, 0x0, LimitEnd_Anim, 0, (void *)0x80137bf4, Charge_Coll, (void *)0x800761c8},    // 385: Limit Cross Slash in the air, the rest
    {329, 0x340213, 0x14, 0x0, (void *)0x801382e8, (void *)0x801383a8, LimitRise_Phys, (void *)0x801388b4, (void *)0x800761c8},    // 386: Limit Climhazzard
    {334, 0x340613, 0x14, 0x0, (void *)0x80138348, (void *)0x801384f0, LimitRiseAir_Phys, (void *)0x80138940, (void *)0x800761c8},    // 387: Limit Climhazzard in the air
};

// The fighter's own sword trail: where the game's trail runs along the blade, in its trail's colour.
static AfterImageDesc trail = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {83, 194, 131, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 1.220, .offset_top = 16.564,
};
static AfterImageDesc trail_1 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {94, 195, 150, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 1.220, .offset_top = 16.564,
};
static AfterImageDesc trail_2 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {97, 159, 212, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 1.220, .offset_top = 16.564,
};
static AfterImageDesc trail_3 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {143, 186, 101, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 1.220, .offset_top = 16.564,
};
static AfterImageDesc trail_4 = {
    .x0 = 0.20000000298023224, .x4 = 1.0, .start_alpha = 255, .end_alpha = 0,
    .in_col = {196, 96, 129, 0}, .out_col = {255, 255, 255, 0}, .x12 = 0, .x13 = 0,
    .bone_index = 75, .offset_bottom = 1.220, .offset_top = 16.564,
};

AfterImageDesc *GetTrailData(GOBJ *fighter)
{
    FighterData *fd = fighter->userdata;
    int a = fd->action_id;
    if (a == 71)
        return &trail_1;
    if (a == 323 || a == 325 || a == 327 || a == 328 || a == 329 || a == 330 || a == 331 || a == 332 || a == 333 || a == 334 || a == 335 || a == 336)
        return &trail_2;
    if (a == 303 || a == 304 || a == 305 || a == 306 || a == 307 || a == 308 || a == 312 || a == 313 || a == 314 || a == 315 || a == 316 || a == 317)
        return &trail_3;
    if (a == 321 || a == 322)
        return &trail_4;
    return &trail;
}
