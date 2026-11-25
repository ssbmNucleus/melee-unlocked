#include "../mex.h"
#define ACTIVE 6
#define LIFE 180
#define BALLS 3
#define VISIBLE 0
static const float spread[ACTIVE] = {1.000000f, 5.499720f, 10.056631f, 14.613542f, 19.170453f, 23.727364f};
static const float places[ACTIVE][BALLS][2] = {{{-4.845601f, 6.183677f}, {1.615200f, -0.277125f}, {0.000000f, 0.000000f}}, {{-20.459207f, 21.797283f}, {17.228806f, -15.890730f}, {0.000000f, 0.000000f}}, {{-36.072812f, 37.410888f}, {32.842411f, -31.504335f}, {0.000000f, 0.000000f}}, {{-51.686417f, 53.024493f}, {48.456016f, -47.117941f}, {0.000000f, 0.000000f}}, {{-67.300022f, 68.638098f}, {64.069621f, -62.731546f}, {0.000000f, 0.000000f}}, {{-82.913628f, 84.251704f}, {79.683227f, -78.345151f}, {0.000000f, 0.000000f}}};

// The source article has hitlagself=0: contact must not pause its clock or its outward sweep.
static bool Contact(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->dmg.xca8 = 0;
    id->dmg.hitlag_requested = 0;
    return false;
}
bool OnGiveDamage(GOBJ *item) { return Contact(item); }
bool OnHitShieldBounce(GOBJ *item) { return Contact(item); }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return Contact(item); }
bool OnClank(GOBJ *item) { return Contact(item); }
bool OnReflect(GOBJ *item) { return false; }
int Wave_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *joint = item->hsd_object;
    int age = ++id->item_var.var2, i;
    float expand = spread[age < ACTIVE ? age : ACTIVE - 1];
    Vec3 scale = {expand, 1.0f, expand};
    if (age >= LIFE)
        return 1;
    if (id->state == 0)
        return 0;
    // A visual stand-in; the source attack coordinates below do not inherit its visual scale.
    joint->scale = scale;
    JOBJ_SetMtxDirtySub(joint);
    if (!VISIBLE)
        JOBJ_SetFlagsAll(joint, JOBJ_HIDDEN);
    if (age >= ACTIVE)
        Item_RemoveAllHitboxes(item);
    else
        for (i = 0; i < BALLS; i++)
        {
            itHit *h = &id->hitbox[i];
            h->bone = joint;
            h->offset.X = places[age][i][0] / expand;
            h->offset.Y = places[age][i][1];
            h->offset.Z = 0;
            h->can_reflect = 0;
            h->can_absorb = 0;
            h->can_deflect = 0;
        }
    return 0;
}
void Wave_Phys(GOBJ *item) {}
int Wave_Coll(GOBJ *item) { return 0; }
__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state=0, .animCallback=Wave_Anim, .physCallback=Wave_Phys, .collCallback=Wave_Coll},
    {.state=1, .animCallback=Wave_Anim, .physCallback=Wave_Phys, .collCallback=Wave_Coll},
};
