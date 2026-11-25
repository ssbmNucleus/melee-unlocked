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

// A gunman's bullet: straight ahead at one speed until it strikes, meets a wall or has gone its distance. Its hit
// is the gunman's own (its states after the first are the five gunmen's, each with his hit).
#define SPEED 4.0f        // READING: the five values of param_gunman that are all 4 (names not resolved)
#define LIFE 12          // READING: frames it flies (param_gunman's one value of 12, name not resolved)

bool OnGiveDamage(GOBJ *item) { return true; }
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
int Bullet_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    if (Lost(item) || id->item_var.var3)
        return 1;                                       // (var3: the fighter's code has it strike his own can: spent)
    if (id->state == 0)
    {
        if (ask)
        {
            id->self_vel.X = SPEED * id->facing_direction;
            id->self_vel.Y = 0;
            id->item_var.var2 = LIFE;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, ask, 2);
            model->rot.Y = id->facing_direction < 0 ? 3.14159265f : 0;
            JOBJ_SetMtxDirtySub(model);
        }
        else if (++id->item_var.var4 > 2)
            return 1;                                   // never let go: gone
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Bullet_Phys(GOBJ *item) {}
int Bullet_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
        return 0;
    return (Item_CollAir_NoCB(item) & ~1) != 0;         // a wall or a ceiling ends it; it passes along a floor
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
    {.state = 1, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
    {.state = 2, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
    {.state = 3, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
    {.state = 4, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
    {.state = 5, .animCallback = Bullet_Anim, .physCallback = Bullet_Phys, .collCallback = Bullet_Coll},
};
