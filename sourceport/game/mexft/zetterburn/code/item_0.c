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
typedef struct {int ready,frame,age;float facing,speed;} RivalsArticle;
typedef char RivalsArticleFits[(sizeof(RivalsArticle)<=sizeof(((ItemData *)0)->item_var))?1:-1];
static RivalsArticle *RA_Data(GOBJ *g){return (RivalsArticle *)&((ItemData *)g->userdata)->item_var;}
static const int ra_lengths[]={0,30};
static const int ra_next[]={0,0};
static const int ra_keep[]={0,0};
static void RA_Enter(GOBJ *g,int state){
    ItemData *id=g->userdata;RivalsArticle *p=RA_Data(g);p->frame=0;
    if(ra_keep[state]){if(id->state!=state)ItemStateChange(g,state,2|0x10);}
    else {Item_ResetAllHitPlayers(id);ItemStateChange(g,state,2);}
}
bool OnGiveDamage(GOBJ *g){return 1;}
bool OnHitShieldBounce(GOBJ *g){return true;}
bool OnHitShieldDetermineDestroy(GOBJ *g){return true;}
bool OnClank(GOBJ *g){return true;}
bool OnReflect(GOBJ *g){
    ItemData *id=g->userdata;RivalsArticle *p=RA_Data(g);
    float before=sqrtf(id->self_vel.X*id->self_vel.X+id->self_vel.Y*id->self_vel.Y),after;
    Item_ReflectVelocity(g);
    after=sqrtf(id->self_vel.X*id->self_vel.X+id->self_vel.Y*id->self_vel.Y);
    if(before>0.00001f)p->speed*=after/before;
    p->facing=id->facing_direction;
    return false;
}
int RA_Anim(GOBJ *g){
    ItemData *id=g->userdata;RivalsArticle *p=RA_Data(g);int next;
    if(Lost(g))return 1;
    if(id->is_hitlag)return 0;
    if(!id->state){
        if(!p->ready)return ++p->age>2;
        p->age=0;p->facing=id->facing_direction;p->speed=1;
        Item_SetAirborne(id);RA_Enter(g,1);return 0;
    }
    if(++p->age>=600)return 1;
    if(++p->frame>=ra_lengths[id->state]){
        next=ra_next[id->state];if(!next)return 1;
        RA_Enter(g,next);
    }
    return 0;
}
void RA_Phys(GOBJ *g){
    ItemData *id=g->userdata;RivalsArticle *p=RA_Data(g);
    if(id->is_hitlag)return;
    switch(id->state){case 1: if(id->self_vel.Y>-999.9000000000001f) { id->self_vel.Y-=0.0f; if(id->self_vel.Y<-999.9000000000001f)id->self_vel.Y=-999.9000000000001f; }if(id->self_vel.X>0){id->self_vel.X-=0.0f;if(id->self_vel.X<0)id->self_vel.X=0;}else if(id->self_vel.X<0){id->self_vel.X+=0.0f;if(id->self_vel.X>0)id->self_vel.X=0;}if(p->frame>=0 && p->frame<=29)id->self_vel.X=3.0f*p->speed*p->facing; break;}
}
int RA_Coll(GOBJ *g){
    ItemData *id=g->userdata;
    return id->state && (Item_CollAir_NoCB(g)&15)!=0;
}
__attribute__((used)) static struct ItemState item_state_table[]={{.state=0,.animCallback=RA_Anim,.physCallback=RA_Phys,.collCallback=RA_Coll},{.state=1,.animCallback=RA_Anim,.physCallback=RA_Phys,.collCallback=RA_Coll}};
