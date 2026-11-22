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

// Hydro Pump's water: a burst that flies back from him along the jet for a set time, pushing what it meets.
#define LIFE 17                // vl.prc param_water life: frames it flies
#define SPEED 4.0f              // vl.prc param_water speed

bool OnGiveDamage(GOBJ *item) { return false; }        // (it goes on through what it struck)
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return false; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
// its variables: var1 the fighter's ask, var2 frames it has left, var4 frames before it was let go, var5 and var6 the
// way it flies times a thousand
int Water_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    if (Lost(item))
        return 1;
    if (id->state == 0)
    {
        if (ask)
        {
            id->self_vel.X = id->item_var.var5 * 0.001f * SPEED;
            id->self_vel.Y = id->item_var.var6 * 0.001f * SPEED;
            id->item_var.var2 = LIFE;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, 1, 2);
        }
        else if (++id->item_var.var4 > 2)
            return 1;
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Water_Phys(GOBJ *item) {}
int Water_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
        return 0;
    return (Item_CollAir_NoCB(item) & ~1) != 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Water_Anim, .physCallback = Water_Phys, .collCallback = Water_Coll},
    {.state = 1, .animCallback = Water_Anim, .physCallback = Water_Phys, .collCallback = Water_Coll},
};
