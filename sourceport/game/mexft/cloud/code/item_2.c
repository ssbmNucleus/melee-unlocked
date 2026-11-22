#include "../mex.h"

// A shot that holds what it hits: it flies straight until it hits, stops there, hits again and
// again (its second state's script) and ends.
#define HOLD_FRAMES 31.0f
#define HOLD_AHEAD 10.661462761873938f      // it stops short of what it hit by this much, and steps on to it
// (Its second state starts on the frame after the hit: a hitbox made during the hit itself would
// count what it has just hit as hit already.)
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
    {
        id->self_vel.X = 0;
        id->self_vel.Y = 0;
        id->itcmd_var.flag1 = 1;
    }
    return false;
}
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

// While it holds, each hit stops it for as much of the usual pause as the game gives these hits.
#define HOLD_LAG 0.5f
static void Hold_Lag(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->dmg.hitlag_remaining *= HOLD_LAG;
    if (id->dmg.hitlag_remaining < 1.0f)
        id->dmg.hitlag_remaining = 1.0f;
}

int Shot_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    Face(item);
    if (id->state == 0 && id->itcmd_var.flag1)
    {
        id->pos.X += HOLD_AHEAD * id->facing_direction;
        id->itcmd_var.flag1 = 0;
        ItemStateChange(item, 1, 2);
        // each hit stops its script for a few frames, so the script itself says when it is over
        // (it raises the same variable at its end); the timer is only there in case it never does
        Item_SetLifeTimer(item, 4.0f * HOLD_FRAMES);
        id->cb.on_enter_hitlag = Hold_Lag;
        return 0;
    }
    if (id->state == 1 && id->itcmd_var.flag1)
        return 1;
    return Item_DecLifeTimer(item);
}
void Shot_Phys(GOBJ *item) {}
int Shot_Coll(GOBJ *item) { return (Item_CollAir_NoCB(item) & ~1) != 0; }
int Hold_Coll(GOBJ *item) { return 0; }

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Shot_Anim, .physCallback = Shot_Phys, .collCallback = Shot_Coll},
    {.state = 1, .animCallback = Shot_Anim, .physCallback = Shot_Phys, .collCallback = Hold_Coll},
};
