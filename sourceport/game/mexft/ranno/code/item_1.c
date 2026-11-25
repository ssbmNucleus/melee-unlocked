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
// var5: pending native contact; var6: identity token; var7: held fighter;
// var8: expire. The fighter module owns capture and poison consumption.
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(id->state==1 && id->hit_fighter && id->hit_fighter!=id->fighter_gobj)
    {
        id->item_var.var5=(int)id->hit_fighter;
        ItemStateChange(item,2,2); // remove the hitbox before another contact
    }
    return false;
}
bool OnHitShieldBounce(GOBJ *item){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *item){return true;}
bool OnClank(GOBJ *item){return true;}
bool OnReflect(GOBJ *item){return true;}
int BubbleAnim(GOBJ *item)
{
    ItemData *id=item->userdata;
    if(Lost(item) || id->item_var.var8)return 1;
    if(id->state==0)
    {
        if(id->item_var.var1)
        {
            id->item_var.var1=0;id->item_var.var2=900;
            Item_SetAirborne(id);Item_ResetAllHitPlayers(id);ItemStateChange(item,1,2);
        }
        else if(++id->item_var.var4>2)return 1;
        return 0;
    }
    return --id->item_var.var2<=0;
}
void BubblePhys(GOBJ *item)
{
    ItemData *id=item->userdata;
    id->self_vel.X*=0.96f;id->self_vel.Y*=0.96f;
}
int BubbleColl(GOBJ *item)
{
    ItemData *id=item->userdata;
    int touch;
    float x=id->self_vel.X,y=id->self_vel.Y;
    if(!id->state)return 0;
    touch=Item_CollAir_NoCB(item);
    if(touch & 3)id->self_vel.Y=-y;
    if(touch & 12)id->self_vel.X=-x;
    return 0;
}
__attribute__((used)) static struct ItemState item_state_table[]={
    {.state=0,.animCallback=BubbleAnim,.physCallback=BubblePhys,.collCallback=BubbleColl},
    {.state=1,.animCallback=BubbleAnim,.physCallback=BubblePhys,.collCallback=BubbleColl},
    {.state=2,.animCallback=BubbleAnim,.physCallback=BubblePhys,.collCallback=BubbleColl},
};
