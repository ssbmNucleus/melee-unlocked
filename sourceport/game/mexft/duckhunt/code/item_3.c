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

// A gunman of Wild Gunman: the tall cowboy. He appears, stands (or sinks slowly to the floor, made in the air), fires once
// after his own wait and is gone a while after. He is a flat picture of the game's own figure (see own_items.slab_picture).
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
#define KIND 1             // which of the five he is (the place of his hit in the bullet's own script)
#define FIRE_WAIT 85       // published: frames from his appearing to his shot
#define GONE 181                 // published: frames from his appearing to his going
#define FALL 0.019999999552965164f             // vl.prc param_gunman fall_accel
#define FALL_MAX 0.5f     // fall_max_speed
#define MUZZLE_X 3.4836262575043975f         // where his gun is from his feet (READING: the front edge of his figure, three fifths up it)
#define MUZZLE_Y 10.497952785780626f
// the fighter's own sounds, by their place in his bank (0: not in it)
#define SOUND_APPEAR 100     // se_duckhunt_special_l02: his coming (the game plays it once in each gunman's own script)
#define SOUND_FIRE 17         // se_duckhunt_special_l03: his shot (the sound his smashes' shots play)
typedef struct { u8 delay; u16 gfx; } OwnFx;
static const OwnFx flash_fx[] = {{7, 6033}, {5, 6034}, {5, 6035}, {5, 6036}, {5, 6037}, {7, 6038}};        // the fighter's own effects: the flash of his gun,
static const OwnFx end_fx[] = {{0, 6039}};            // and the smoke he goes in

// his variables: var3 he fires this frame (the fighter's code makes the bullet), var4 frames since he appeared,
// var5 and var6 where his gun is (hundredths), var7 which gunman he is
static void Fx(GOBJ *item, const OwnFx *fx, int count, int age, float x, float y)
{
    ItemData *id = item->userdata;
    GOBJ *owner = id->fighter_gobj;
    if (owner)
    {
        FighterData *fd = owner->userdata;
        Vec3 off, none = {0, 0, 0};
        int i;
        off.X = 0;
        off.Y = id->pos.Y + y - fd->phys.pos.Y;
        off.Z = (id->pos.X + x * id->facing_direction - fd->phys.pos.X) * fd->facing_direction;
        for (i = 0; i < count; i++)
            if (fx[i].delay == age)
                SPAWN_EFFECT(owner, fx[i].gfx, 0, 0, 0, &off, &none, 0.0f);
    }
}
bool OnGiveDamage(GOBJ *item) { return false; }
bool OnHitShieldBounce(GOBJ *item) { return false; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return false; }
bool OnClank(GOBJ *item) { return false; }
bool OnReflect(GOBJ *item) { return false; }
static void Gun_Landed(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
}
static void Gun_Fell(GOBJ *item)
{
}
int Gun_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    int age = ++id->item_var.var4;
    if (Lost(item))
        return 1;
    id->item_var.var5 = (int)(MUZZLE_X * 100.0f);
    id->item_var.var6 = (int)(MUZZLE_Y * 100.0f);
    id->item_var.var7 = KIND;
    // (his picture is made facing right: turned about when he faces left)
    model->rot.Y = id->facing_direction < 0 ? 3.14159265f : 0;
    JOBJ_SetMtxDirtySub(model);
    if (age == 1 && SOUND_APPEAR)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_APPEAR, 127, 64);
    // (he asks for the bullet a frame early: the fighter's code makes it on the next frame, the frame of his flash and
    // his shot's sound, and its hit is out the frame after that, which is the published one)
    if (age == FIRE_WAIT - 1)
        id->item_var.var3 = 1;
    if (age == FIRE_WAIT && SOUND_FIRE)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_FIRE, 127, 64);
    if (age >= FIRE_WAIT)
        Fx(item, flash_fx, sizeof(flash_fx) / sizeof(flash_fx[0]), age - FIRE_WAIT, MUZZLE_X, MUZZLE_Y);
    if (age >= GONE)
    {
        Fx(item, end_fx, sizeof(end_fx) / sizeof(end_fx[0]), age - GONE, 0.0f, MUZZLE_Y);
        return 1;
    }
    return 0;
}
void Gun_Phys(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->air_state == 0)
        return;
    id->self_vel.Y -= FALL;
    if (id->self_vel.Y < -FALL_MAX)
        id->self_vel.Y = -FALL_MAX;
}
int Gun_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->air_state == 0)
        Item_CollGround_PassLedge(item, Gun_Fell);
    else
        Item_CollAir_Land(item, Gun_Landed);
    return 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Gun_Anim, .physCallback = Gun_Phys, .collCallback = Gun_Coll},
};
