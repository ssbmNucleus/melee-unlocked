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
bool OnGiveDamage(GOBJ *g){return false;}
bool OnHitShieldBounce(GOBJ *g){return false;}
bool OnHitShieldDetermineDestroy(GOBJ *g){return false;}
bool OnClank(GOBJ *g){return false;}
bool OnReflect(GOBJ *g){return true;}
int BurstAnim(GOBJ *g)
{
    ItemData *id=g->userdata;
    if(Lost(g))return 1;
    if(id->is_hitlag)return 0;
    if(!id->state)
    {
        if(!id->item_var.var1)return ++id->item_var.var4>2;
        id->item_var.var1=0;id->item_var.var2=0;ItemStateChange(g,1,2);
    }
    return ++id->item_var.var2>=20;
}
void BurstPhys(GOBJ *g){ItemData *id=g->userdata;id->self_vel.X=id->self_vel.Y=0;}
int BurstColl(GOBJ *g){return 0;}
__attribute__((used)) static struct ItemState item_state_table[]={
 {.state=0,.animCallback=BurstAnim,.physCallback=BurstPhys,.collCallback=BurstColl},
 {.state=1,.animCallback=BurstAnim,.physCallback=BurstPhys,.collCallback=BurstColl},
};
