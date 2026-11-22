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

// The ball of Teleport: made at her feet, it flies the way her stick points (else the way it last went: at first
// ahead and up) at one speed, the stage stopping it, until she asks it to stand; she then takes its place and it is
// gone. It is gone too as soon as she is out of the move.
#define SPEED 2.664944793490843f              // a frame (IttleDewTeleportCC speed)
#define DIR_X 0.7071067811865475f              // its way before she steers it (defaultDir, made one long): ahead,
#define DIR_Y 0.7071067811865475f              //   and up
#define DEAD 0.2f                // a stick nearer the middle than this is let go (READING: the game asks for
                                        //   no direction at all; Melee's sticks rest a little off the middle)
#define OWNER_FIRST 377                // her states of the move
#define OWNER_LAST 378
#define ASK_GO 1
#define ASK_STAND 2
#define ASK_GONE 3

// its variables: var1 what the fighter asks, var4 frames it has waited
bool OnGiveDamage(GOBJ *item) { return false; }
bool OnHitShieldBounce(GOBJ *item) { return false; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return false; }
bool OnClank(GOBJ *item) { return false; }
bool OnReflect(GOBJ *item) { return false; }
int Ball_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    GOBJ *owner = id->fighter_gobj;
    FighterData *fd = owner ? owner->userdata : 0;
    int ask = id->item_var.var1;
    float sx, sy, n;
    id->item_var.var1 = 0;
    if (Lost(item) || ask == ASK_GONE || !fd || fd->state_id < OWNER_FIRST || fd->state_id > OWNER_LAST)
        return 1;
    if (id->state == 0)
    {
        if (ask == ASK_GO)
        {
            id->self_vel.X = DIR_X * SPEED * fd->facing_direction;
            id->self_vel.Y = DIR_Y * SPEED;
            Item_SetAirborne(id);
            ItemStateChange(item, 1, 2);
        }
        else if (++id->item_var.var4 > 2)
            return 1;
        return 0;
    }
    if (ask == ASK_STAND)
    {
        id->self_vel.X = 0;
        id->self_vel.Y = 0;
        ItemStateChange(item, 2, 2);
    }
    if (id->state == 1)
    {
        sx = fd->input.lstick.X;
        sy = fd->input.lstick.Y;
        n = sqrtf(sx * sx + sy * sy);
        if (n >= DEAD)
        {
            id->self_vel.X = sx / n * SPEED;
            id->self_vel.Y = sy / n * SPEED;
        }
    }
    return 0;
}
void Ball_Phys(GOBJ *item) {}
int Ball_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state != 0)
        Item_CollAir_NoCB(item);                        // the stage stops it (its box: her size)
    return 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Ball_Anim, .physCallback = Ball_Phys, .collCallback = Ball_Coll},
    {.state = 1, .animCallback = Ball_Anim, .physCallback = Ball_Phys, .collCallback = Ball_Coll},
    {.state = 2, .animCallback = Ball_Anim, .physCallback = Ball_Phys, .collCallback = Ball_Coll},
};
