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

typedef struct {
    int ready, age, hits, bounces, contact;
    float distance, last_x, last_y, gx, gy, travel, return_gain;
} PortProjectile;
typedef char PortProjectileFits[(sizeof(PortProjectile)<=sizeof(((ItemData *)0)->item_var))?1:-1];
static void PP_Orient(GOBJ *g)
{
    ItemData *id=g->userdata;JOBJ *model=g->hsd_object;
    // own_items already converts the mesh into world-facing item axes. Undo
    // the donor's extra quarter turn after creation and native state changes.
    model->rot.Y=id->facing_direction<0 ? 3.14159265f : 0;
    JOBJ_SetMtxDirtySub(model);
}

static PortProjectile *PP_Data(GOBJ *g) {return (PortProjectile *)&((ItemData *)g->userdata)->item_var;}
bool OnGiveDamage(GOBJ *g) {return 0>0 && ++PP_Data(g)->hits>=0;}
bool OnHitShieldBounce(GOBJ *g) {return true;}
bool OnHitShieldDetermineDestroy(GOBJ *g) {return true;}
bool OnClank(GOBJ *g) {return true;}
bool OnReflect(GOBJ *g) {Item_ReflectVelocity(g);PP_Orient(g);return false;}
int Projectile_Anim(GOBJ *g)
{
    ItemData *id=g->userdata;PortProjectile *p=PP_Data(g);float x,y;
    if(Lost(g))return 1;
    if(id->state==0) {
        if(!p->ready)return ++p->age>2;
        p->age=0;p->last_x=id->pos.X;p->last_y=id->pos.Y;
        Item_SetAirborne(id);Item_ResetAllHitPlayers(id);ItemStateChange(g,1,2);PP_Orient(g);
        return 0;
    }
    x=id->pos.X-p->last_x;y=id->pos.Y-p->last_y;
    p->distance+=sqrtf(x*x+y*y);p->last_x=id->pos.X;p->last_y=id->pos.Y;
    return ++p->age>=120 || (0.0f>0 && p->distance>=0.0f);
}
void Projectile_Phys(GOBJ *g)
{
    ItemData *id=g->userdata;PortProjectile *p=PP_Data(g);
    if(id->state==1) {
p->travel+=1.0f/60.0f;
if(p->travel>0.10000000149011612f+0.30000001192092896f) {
    p->return_gain+=1.5f/60.0f;
    id->self_vel.X=-(3.4263575916310836f)*p->return_gain;id->self_vel.Y=0;
}
else if(p->travel>0.10000000149011612f) {
    id->self_vel.X=3.4263575916310836f*(1.0f-2.0f*(p->travel-0.10000000149011612f)/0.30000001192092896f);
    id->self_vel.Y=1.0405974252373749f;
}
else {id->self_vel.X=3.4263575916310836f;id->self_vel.Y=0;}
}
}
int Projectile_Coll(GOBJ *g)
{
    ItemData *id=g->userdata;PortProjectile *p=PP_Data(g);int touch,fresh;float speed,normal;
    if(id->state!=1)return 0;
    touch=Item_CollAir_NoCB(g)&15;
    fresh=touch & ~p->contact;p->contact=touch;
    if(!fresh)return 0;
    if(++p->bounces>=1)return 1;
    speed=sqrtf(id->self_vel.X*id->self_vel.X+id->self_vel.Y*id->self_vel.Y);
    normal=fresh&3 ? fabsf(id->self_vel.Y) : fabsf(id->self_vel.X);
    if(speed>0 && normal/speed>0.7071067811865476f)return 1;
    if(((fresh&1)&&id->self_vel.Y<0)||((fresh&2)&&id->self_vel.Y>0))id->self_vel.Y=-id->self_vel.Y;
    if(fresh&12)id->self_vel.X=-id->self_vel.X;
    id->self_vel.X*=1.0f;id->self_vel.Y*=1.0f;
    return 0;
}
__attribute__((used))
static struct ItemState item_state_table[]={
    {.state=0,.animCallback=Projectile_Anim,.physCallback=Projectile_Phys,.collCallback=Projectile_Coll},
    {.state=1,.animCallback=Projectile_Anim,.physCallback=Projectile_Phys,.collCallback=Projectile_Coll},
};
