#include "../mex.h"
#define ACTIVE 6
#define LIFE 30
#define BALLS 2
#define VISIBLE 1
static const float spread[ACTIVE] = {1.000000f, 1.333333f, 1.666667f, 2.000000f, 2.333333f, 2.666667f};
static const float places[ACTIVE][BALLS][2] = {{{-4.568477f, 2.284238f}, {4.568477f, 2.284238f}}, {{-6.091302f, 2.284238f}, {6.091302f, 2.284238f}}, {{-7.614128f, 2.284238f}, {7.614128f, 2.284238f}}, {{-9.136954f, 2.284238f}, {9.136954f, 2.284238f}}, {{-10.659779f, 2.284238f}, {10.659779f, 2.284238f}}, {{-12.182605f, 2.284238f}, {12.182605f, 2.284238f}}};

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
