#include "../mex.h"
#define ACTIVE 6
#define LIFE 120
#define BALLS 3
#define VISIBLE 0
static const float spread[ACTIVE] = {1.000000f, 3.928371f, 6.913934f, 9.899496f, 12.885058f, 15.870621f};
static const float places[ACTIVE][BALLS][2] = {{{-4.845601f, 6.183678f}, {1.615200f, -0.277125f}, {0.000000f, 0.000000f}}, {{-15.075204f, 16.413282f}, {11.844803f, -10.506729f}, {0.000000f, 0.000000f}}, {{-25.304807f, 26.642886f}, {22.074406f, -20.736333f}, {0.000000f, 0.000000f}}, {{-35.534410f, 36.872490f}, {32.304008f, -30.965938f}, {0.000000f, 0.000000f}}, {{-45.764012f, 47.102095f}, {42.533611f, -41.195542f}, {0.000000f, 0.000000f}}, {{-55.993615f, 57.331699f}, {52.763214f, -51.425146f}, {0.000000f, 0.000000f}}};

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
