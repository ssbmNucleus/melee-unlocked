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
bool OnGiveDamage(GOBJ *item) { return true; }
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item) { Item_ReflectVelocity(item); return false; }
int DartAnim(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(Lost(item))return 1;
    if(id->state==0)
    {
        if(id->item_var.var1)
        {
            id->item_var.var1=0;id->item_var.var2=24;
            Item_SetAirborne(id);Item_ResetAllHitPlayers(id);ItemStateChange(item,1,2);
        }
        else if(++id->item_var.var4>2)return 1;
        return 0;
    }
    return --id->item_var.var2<=0;
}
void DartPhys(GOBJ *item) {}
int DartColl(GOBJ *item)
{
    ItemData *id=item->userdata;
    return id->state ? Item_CollAir_NoCB(item)!=0 : 0;
}
__attribute__((used)) static struct ItemState item_state_table[]={
    {.state=0,.animCallback=DartAnim,.physCallback=DartPhys,.collCallback=DartColl},
    {.state=1,.animCallback=DartAnim,.physCallback=DartPhys,.collCallback=DartColl},
};
