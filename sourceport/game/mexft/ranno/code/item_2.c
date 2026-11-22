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
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(id->state==1 && id->hit_fighter && id->hit_fighter!=id->fighter_gobj)
    {
        id->item_var.var5=(int)id->hit_fighter;
        ItemStateChange(item,2,2);
    }
    return false;
}
bool OnHitShieldBounce(GOBJ *item){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *item){return true;}
bool OnClank(GOBJ *item){return true;}
bool OnReflect(GOBJ *item){return true;}
int TongueAnim(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(Lost(item) || id->item_var.var8)return 1;
    if(id->state==0)
    {
        if(id->item_var.var1)
        {
            id->item_var.var1=0;id->item_var.var2=0;
            Item_SetAirborne(id);Item_ResetAllHitPlayers(id);ItemStateChange(item,1,2);
        }
        else if(++id->item_var.var4>2)return 1;
        return 0;
    }
    if(++id->item_var.var2==20)ItemStateChange(item,2,2);
    return id->item_var.var2>=30;
}
void TonguePhys(GOBJ *item)
{
    ItemData *id=item->userdata;
    FighterData *fd=id->fighter_gobj->userdata;
    JOBJ *model=item->hsd_object;
    int age=id->item_var.var2;
    float reach=age<20 ? age*2.4f : (30-age)*2.4f*20/10;
    float cx=id->item_var.var9 ? 0.70710678f : 1.0f;
    float cy=id->item_var.var9 ? -0.70710678f : 0.0f;
    if(id->item_var.var3)return;
    if(reach<0.1f)reach=0.1f;
    id->self_vel.X=id->self_vel.Y=0;
    id->pos.X=fd->phys.pos.X+(6.800000000000001f+reach*cx)*id->facing_direction;
    id->pos.Y=fd->phys.pos.Y+(id->item_var.var9 ? 6.2f : 7.800000000000001f)+reach*cy;id->pos.Z=0;
    model->scale.X=reach;
    model->rot.Y=id->facing_direction<0 ? 3.14159265f : 0;
    model->rot.Z=id->item_var.var9 ? -0.78539816f : 0;
    JOBJ_SetMtxDirtySub(model);
}
int TongueColl(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(id->state==1 && !id->item_var.var3 && Item_CollAir_NoCB(item)!=0)
    {
        id->item_var.var3=1;id->self_vel.X=id->self_vel.Y=0;
        ItemStateChange(item,2,2);
    }
    return 0;
}
__attribute__((used)) static struct ItemState item_state_table[]={
    {.state=0,.animCallback=TongueAnim,.physCallback=TonguePhys,.collCallback=TongueColl},
    {.state=1,.animCallback=TongueAnim,.physCallback=TonguePhys,.collCallback=TongueColl},
    {.state=2,.animCallback=TongueAnim,.physCallback=TonguePhys,.collCallback=TongueColl},
};
