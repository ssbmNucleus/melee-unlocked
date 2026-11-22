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

static void Port_EnableBatHurtbox(GOBJ *item,float radius)
{
    ItemData *id=item->userdata;
    // Item_CreateHurtbox only writes geometry. Projectile templates have zero
    // hurtboxes, so register the count and root joint before enabling contact.
    // MexTK declares hurt_num as int; GALE01 loads a BYTE at 0xAC8.
    // Store through a byte alias so big-endian PPC does not read zero.
    *((unsigned char *)&id->hurt_num)=1;id->it_hurt[0].jobj=item->hsd_object;
    id->it_hurt[0].is_updated=0;
    Item_CreateHurtbox(item,0,0,0,0,0,0,radius);
    Item_SetHurtboxTangibility(item,0);id->xdce1=1;
}
static int Port_BatItem(GOBJ *item,float *charge,float speed_min,float speed_max)
{
    ItemData *id=item->userdata;GOBJ *hitter=id->dmg.source_fighter;
    float damage=(float)id->dmg.recent,scale,angle,direction=id->dmg.takedmg_direction;
    if(damage<=0)return 0;
    scale=(damage-8.0f)/7.0f;if(scale<0)scale=0;if(scale>1)scale=1;
    *charge+=0.25f+0.5f*scale;if(*charge>1)*charge=1;
    if(hitter && hitter->userdata)
        direction=((FighterData *)hitter->userdata)->phys.pos.X>id->pos.X ? -1.0f : 1.0f;
    if(direction==0)direction=id->facing_direction;
    angle=(id->dmg.angle>=0 && id->dmg.angle<=360 ? id->dmg.angle : 45)*0.01745329252f;
    id->self_vel.X=cos(angle)*(speed_min+(speed_max-speed_min)*scale)*direction;
    id->self_vel.Y=sin(angle)*(speed_min+(speed_max-speed_min)*scale);
    id->facing_direction=id->self_vel.X<0 ? -1 : 1;
    Item_ResetAllHitPlayers(id);
    return 1;
}

typedef struct {int ready,age,pool,expired,bounces;float charge;} LoxBoulder;
static LoxBoulder *LB(GOBJ *g){return (LoxBoulder *)&((ItemData *)g->userdata)->item_var;}
static void BoulderHits(GOBJ *g)
{
    ItemData *id=g->userdata;LoxBoulder *b=LB(g);int i;
    for(i=0;i<4;i++)if(id->hitbox[i].active)
    {
        Item_UpdateHitboxDamage(&id->hitbox[i],(int)(6.0f+6.0f*b->charge),g);
        id->hitbox[i].kb_growth=(int)(45.0f+20.0f*b->charge);
        id->hitbox[i].size=5.999999888484698f*(1.0f+0.35f*b->charge);
    }
}
bool OnTakeDamage(GOBJ *g)
{
    LoxBoulder *b=LB(g);ItemData *id=g->userdata;
    if(b->expired || b->pool || id->state!=1)return false;
    if(Port_BatItem(g,&b->charge,4.0f,8.0f)){b->bounces=0;BoulderHits(g);}
    return false;
}
bool OnGiveDamage(GOBJ *g){return false;}
bool OnHitShieldBounce(GOBJ *g){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *g){return true;}
bool OnClank(GOBJ *g){return false;}
bool OnReflect(GOBJ *g){Item_ReflectVelocity(g);return false;}
int BoulderAnim(GOBJ *g)
{
    ItemData *id=g->userdata;LoxBoulder *b=LB(g);
    if(Lost(g) || b->expired)return 1;
    if(id->is_hitlag)return 0;
    if(!id->state)
    {
        if(!b->ready)return ++b->age>2;
        b->ready=0;b->age=0;Item_SetAirborne(id);ItemStateChange(g,1,2);
        Port_EnableBatHurtbox(g,7.75846004486084f);
    }
    BoulderHits(g);
    return ++b->age>300;
}
void BoulderPhys(GOBJ *g)
{
    ItemData *id=g->userdata;
    if(!id->is_hitlag && id->state==1 && !LB(g)->pool)id->self_vel.Y-=0.12f;
}
int BoulderColl(GOBJ *g)
{
    ItemData *id=g->userdata;LoxBoulder *b=LB(g);int touch;float x=id->self_vel.X,y=id->self_vel.Y;
    if(!id->state || b->pool)return 0;
    touch=Item_CollAir_NoCB(g);
    if((touch&1) && y<0)
    {
        if(!b->bounces){b->bounces=1;Item_SetAirborne(id);id->self_vel.Y=-y*0.55f;id->self_vel.X=x*0.8f;}
        else {b->pool=1;id->self_vel.X=id->self_vel.Y=0;Item_RemoveAllHitboxes(g);}
    }
    if(touch&2)id->self_vel.Y=-y*0.6f;
    if(touch&12)id->self_vel.X=-x*0.6f;
    return 0;
}
__attribute__((used)) static struct ItemState item_state_table[]={
 {.state=0,.animCallback=BoulderAnim,.physCallback=BoulderPhys,.collCallback=BoulderColl},
 {.state=1,.animCallback=BoulderAnim,.physCallback=BoulderPhys,.collCallback=BoulderColl},
};
