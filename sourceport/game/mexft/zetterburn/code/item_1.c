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

// The flame puddle of his down special: it lies where he made it for its time. It strikes no one: who stands in it
// is set alight by his own code (OwnFrame), which keeps where it is.
#define LIFE 300              // the game's Active window: 300 frames

bool OnGiveDamage(GOBJ *item) { return false; }
bool OnHitShieldBounce(GOBJ *item) { return false; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return false; }
bool OnClank(GOBJ *item) { return false; }
bool OnReflect(GOBJ *item) { return false; }
int Puddle_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (Lost(item))
        return 1;
    if (id->state == 0)
    {
        id->item_var.var2 = LIFE;
        ItemStateChange(item, 1, 2);
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Puddle_Phys(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
}
int Puddle_Coll(GOBJ *item) { return 0; }

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Puddle_Anim, .physCallback = Puddle_Phys, .collCallback = Puddle_Coll},
    {.state = 1, .animCallback = Puddle_Anim, .physCallback = Puddle_Phys, .collCallback = Puddle_Coll},
};
