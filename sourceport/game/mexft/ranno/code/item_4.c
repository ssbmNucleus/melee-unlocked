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
bool OnGiveDamage(GOBJ *g){return true;}
bool OnHitShieldBounce(GOBJ *g){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *g){return true;}
bool OnClank(GOBJ *g){return true;}
bool OnReflect(GOBJ *g){Item_ReflectVelocity(g);return false;}
int PalmAnim(GOBJ *g)
{
    ItemData *id=g->userdata;
    if(Lost(g))return 1;
    if(id->is_hitlag)return 0;
    if(!id->item_var.var1)return ++id->item_var.var4>2;
    if(++id->item_var.var2==4){Item_SetAirborne(id);ItemStateChange(g,1,2);}
    if(id->item_var.var2==23+1)ItemStateChange(g,2,2);
    return id->item_var.var2>=28;
}
void PalmPhys(GOBJ *g){}
int PalmColl(GOBJ *g){return Item_CollAir_NoCB(g)!=0;}
__attribute__((used)) static struct ItemState item_state_table[]={
 {.state=0,.animCallback=PalmAnim,.physCallback=PalmPhys,.collCallback=PalmColl},
 {.state=1,.animCallback=PalmAnim,.physCallback=PalmPhys,.collCallback=PalmColl},
 {.state=2,.animCallback=PalmAnim,.physCallback=PalmPhys,.collCallback=PalmColl},
};
