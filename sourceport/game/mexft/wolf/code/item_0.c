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

// The Blaster's laser: straight ahead at one speed until it strikes, meets a wall or has flown its time. What it
// strikes with changes as it flies (its one flying state's script: the game's own three stretches).
#define SPEED 2.5999999046325684f         // vl.prc param_special_n blaster_shoot_speed
#define LIFE 35           // vl.prc param_blaster_bullet life: frames it flies
#define SOUND_FLY 11     // se_wolf_special_n01, by its place in his bank (0: not in it): its own script plays it as it is let go

bool OnGiveDamage(GOBJ *item) { return true; }
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
int Laser_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    if (Lost(item))
        return 1;
    if (id->state == 0)
    {
        if (ask)
        {
            id->self_vel.X = SPEED * id->facing_direction;
            id->self_vel.Y = 0;
            id->item_var.var2 = LIFE;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, 1, 2);
            if (SOUND_FLY)
                Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_FLY, 127, 64);
            // (its picture is made flying right: turned about when it flies left)
            model->rot.Y = id->facing_direction < 0 ? 3.14159265f : 0;
            JOBJ_SetMtxDirtySub(model);
        }
        else if (++id->item_var.var4 > 2)
            return 1;                                   // never let go: gone
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Laser_Phys(GOBJ *item) {}
int Laser_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
        return 0;
    return (Item_CollAir_NoCB(item) & ~1) != 0;         // a wall or a ceiling ends it; it passes along a floor
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Laser_Anim, .physCallback = Laser_Phys, .collCallback = Laser_Coll},
    {.state = 1, .animCallback = Laser_Anim, .physCallback = Laser_Phys, .collCallback = Laser_Coll},
};
