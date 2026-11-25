#include "../mex.h"

// An item of his is taken away when he dies (or is gone), and when it leaves the stage's bounds.
static int Lost(GOBJ *item)
{
    ItemData *id = item->userdata;
    GOBJ *owner = id->fighter_gobj;
    if (!owner || ((FighterData *)owner->userdata)->state_id < ASID_REBIRTH)
        return 1;
    return id->pos.X < stc_stage->blastzoneLeft || id->pos.X > stc_stage->blastzoneRight
           || id->pos.Y < stc_stage->blastzoneBottom || id->pos.Y > stc_stage->blastzoneTop;
}

// The clay pigeon of Clay Shooting. Thrown, it flies level and far, sinking faster as it goes. Shot, it breaks into
// a burst of hits; so it does the frame after it strikes a fighter or a shield, and where it comes down. Left in the
// air it is gone when its time runs out.
// (ADJUSTMENT: the game lets a fast clay live through a shot and spring back from what it strikes, by rules in its
// code that the files do not give; here every shot and every touch breaks it.)
#define CLAY_NEW 0                  // just made: waiting for the throw
#define CLAY_FLY 1
#define CLAY_BREAK 2                // broken by a shot: the shot's own hit, then the burst
#define CLAY_BURST 3                // broken by what it met: the burst alone
#define ASK_THROW 1
#define ASK_HARD 2
#define ASK_SHOT 3
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
// vl.prc, param_clay, by name; how they are put together is a READING
#define THROW_X 1.399999976158142f            // initial_speed_x
#define THROW_Y 0.4000000059604645f            // initial_speed_y
#define HARD_X 1.899999976158142f        // initial_smash_speed_x: thrown with a flick of the stick
#define HARD_Y 0.25999999046325684f        // initial_smash_speed_y
#define FALL 0.012000000104308128f            // fall_accel_y: what it loses a frame,
#define FALL_ADD 0.00019999999494757503f    // fall_add_accel_y: which grows by this every frame
#define FALL_MAX 2.0f    // fall_max_speed_y
#define DEC_X 0.009999999776482582f            // dec_speed_x: speed forward lost a frame
#define LIFE 160            // life
#define TILT 21.0f            // rot_angle: degrees it is tipped as it flies
#define SHOT_FRAMES 2   // frames the shot's own hit is out
#define BREAK_FRAMES 31 // frames the burst lasts (its script, and the last hit's two frames)
#define SOUND_FLY 124  // the fighter's own sounds, by their place in his bank (0: not in it)
#define SOUND_BREAK 127
#define SOUND_SHOT 17  // se_duckhunt_special_l03: the gun's own shot (the sound his smashes' shots play), as a shot breaks it
typedef struct { u8 delay; u16 gfx; } OwnFx;
static const OwnFx break_fx[] = {{0, 6028}, {0, 6029}, {0, 6030}, {0, 6031}, {2, 6032}};      // the fighter's own effect of the burst (duckhunt_clay_break)

// its variables: var1 what the fighter asks, var2 frames it has flown, var3 it is to break, var4 frames in this state
static void Clay_Break(GOBJ *item, int shot)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
    Item_ResetAllHitPlayers(id);
    ItemStateChange(item, shot ? CLAY_BREAK : CLAY_BURST, 2);
    JOBJ_SetFlagsAll(item->hsd_object, JOBJ_HIDDEN);    // the clay itself is gone: the burst is its effect and its hits
    id->item_var.var4 = 0;
    if (shot && SOUND_SHOT)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_SHOT, 127, 64);
    if (SOUND_BREAK)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_BREAK, 127, 64);
}
// it has struck a fighter or a shield: it breaks on the next frame (the shot's own hit is part of the burst, not this)
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == CLAY_FLY)
        id->item_var.var3 = 1;
    return false;
}
bool OnHitShieldBounce(GOBJ *item)
{
    return OnGiveDamage(item);
}
bool OnHitShieldDetermineDestroy(GOBJ *item)
{
    return false;
}
bool OnClank(GOBJ *item)
{
    return false;
}
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
static void Clay_Landed(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
    id->item_var.var3 = 1;                              // it breaks where it comes down
}
static void Clay_Fell(GOBJ *item)
{
}
int Clay_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    GOBJ *owner = id->fighter_gobj;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    id->item_var.var4++;
    if (Lost(item))
        return 1;
    if (id->state >= CLAY_BREAK)
    {
        if (owner)
        {
            FighterData *fd = owner->userdata;
            Vec3 off, none = {0, 0, 0};
            int i;
            off.X = 0;
            off.Y = id->pos.Y - fd->phys.pos.Y;
            off.Z = (id->pos.X - fd->phys.pos.X) * fd->facing_direction;
            for (i = 0; i < (int)(sizeof(break_fx) / sizeof(break_fx[0])); i++)
                if (break_fx[i].delay == id->item_var.var4 - 1)
                    SPAWN_EFFECT(owner, break_fx[i].gfx, 0, 0, 0, &off, &none, 0.0f);
        }
        return id->item_var.var4 > BREAK_FRAMES;
    }
    if (id->state == CLAY_NEW)
    {
        if (ask == ASK_THROW || ask == ASK_HARD)
        {
            id->self_vel.X = (ask == ASK_HARD ? HARD_X : THROW_X) * id->facing_direction;
            id->self_vel.Y = ask == ASK_HARD ? HARD_Y : THROW_Y;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, CLAY_FLY, 2);
            id->item_var.var2 = 0;
            id->item_var.var4 = 0;
            if (SOUND_FLY)
                Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_FLY, 127, 64);
        }
        else if (id->item_var.var4 > 2)
            return 1;                                   // never thrown: gone
        return 0;
    }
    if (ask == ASK_SHOT || id->item_var.var3)
    {
        Clay_Break(item, ask == ASK_SHOT);
        return 0;
    }
    if (++id->item_var.var2 > LIFE)
        return 1;
    model->rot.Z = TILT * 0.017453292f * id->facing_direction;
    JOBJ_SetMtxDirtySub(model);
    return 0;
}
void Clay_Phys(GOBJ *item)
{
    ItemData *id = item->userdata;
    float g;
    if (id->state != CLAY_FLY || id->air_state == 0)
        return;
    g = FALL + FALL_ADD * (float)id->item_var.var2;
    id->self_vel.Y -= g;
    if (id->self_vel.Y < -FALL_MAX)
        id->self_vel.Y = -FALL_MAX;
    if (id->self_vel.X > DEC_X)
        id->self_vel.X -= DEC_X;
    else if (id->self_vel.X < -DEC_X)
        id->self_vel.X += DEC_X;
    else
        id->self_vel.X = 0;
}
int Clay_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state != CLAY_FLY)
        return 0;
    if (id->air_state == 0)
        Item_CollGround_PassLedge(item, Clay_Fell);
    else
        Item_CollAir_Land(item, Clay_Landed);
    return 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Clay_Anim, .physCallback = Clay_Phys, .collCallback = Clay_Coll},
    {.state = 1, .animCallback = Clay_Anim, .physCallback = Clay_Phys, .collCallback = Clay_Coll},
    {.state = 2, .animCallback = Clay_Anim, .physCallback = Clay_Phys, .collCallback = Clay_Coll},
    {.state = 3, .animCallback = Clay_Anim, .physCallback = Clay_Phys, .collCallback = Clay_Coll},
};
