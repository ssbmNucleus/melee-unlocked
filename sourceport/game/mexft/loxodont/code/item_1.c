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
bool OnTakeDamage(GOBJ *g){return true;}
bool OnHitShieldBounce(GOBJ *g){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *g){return true;}
bool OnClank(GOBJ *g){return true;}
bool OnReflect(GOBJ *g){return true;}
int PoolAnim(GOBJ *g)
{
    ItemData *id=g->userdata;JOBJ *model=g->hsd_object;
    if(Lost(g) || id->item_var.var8)return 1;
    if(id->is_hitlag)return 0;
    if(!id->state)
    {
        if(!id->item_var.var1)return ++id->item_var.var4>2;
        ItemStateChange(g,1,2);id->item_var.var1=0;
    }
    id->item_var.var2++;
    model->scale.X=id->item_var.var2>=90 ? 1.0f : 0.8f;
    JOBJ_SetMtxDirtySub(model);
    return id->item_var.var2>390;
}
void PoolPhys(GOBJ *g){ItemData *id=g->userdata;id->self_vel.X=id->self_vel.Y=0;}
int PoolColl(GOBJ *g){return 0;}
__attribute__((used)) static struct ItemState item_state_table[]={
 {.state=0,.animCallback=PoolAnim,.physCallback=PoolPhys,.collCallback=PoolColl},
 {.state=1,.animCallback=PoolAnim,.physCallback=PoolPhys,.collCallback=PoolColl},
};
