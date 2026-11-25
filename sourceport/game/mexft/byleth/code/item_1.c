#include "../mex.h"

// A shot: flies straight at the speed it was let go with until its life runs out (it goes on through what it hits).
bool OnGiveDamage(GOBJ *item) { return false; }
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}

// Its model points the way it flies (it is built pointing right).
static void Face(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    if (id->self_vel.X == 0)
        return;
    model->rot.Y = id->self_vel.X < 0 ? 3.14159265f : 0;
    JOBJ_SetMtxDirtySub(model);
}

int Shot_Anim(GOBJ *item)
{
    Face(item);
    return Item_DecLifeTimer(item);
}
void Shot_Phys(GOBJ *item) {}
int Shot_Coll(GOBJ *item) { return (Item_CollAir_NoCB(item)) != 0; }

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Shot_Anim, .physCallback = Shot_Phys, .collCallback = Shot_Coll},
};
