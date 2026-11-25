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

// A fireball of Fire Mace: straight ahead at one speed until it has struck its hits, meets a wall or has gone its
// distance.
#define SPEED 2.2842383944207225f            // the game's speed (SASA Velocity), in Melee's units a frame
#define LIFE 100              // frames to go the game's distance (SmashProjectile deactivateAfterDist)
#define HITS 6              // the hits it is gone after (deactivateAfterHits)

// its variables: var1 what the fighter asks, var2 frames left, var3 hits struck, var4 frames it has waited
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id = item->userdata;
    return ++id->item_var.var3 >= HITS;
}
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
int Fireball_Anim(GOBJ *item)
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
            id->self_vel.X = SPEED * id->facing_direction;
            id->self_vel.Y = 0;
            id->item_var.var2 = LIFE;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, 1, 2);
        }
        else if (++id->item_var.var4 > 2)
            return 1;                                   // never let go: gone
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Fireball_Phys(GOBJ *item) {}
int Fireball_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
        return 0;
    return (Item_CollAir_NoCB(item) & ~1) != 0;         // a wall or a ceiling ends it; it passes along a floor
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Fireball_Anim, .physCallback = Fireball_Phys, .collCallback = Fireball_Coll},
    {.state = 1, .animCallback = Fireball_Anim, .physCallback = Fireball_Phys, .collCallback = Fireball_Coll},
};
