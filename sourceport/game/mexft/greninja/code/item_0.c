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

// Water Shuriken's shuriken. Thrown at a level of charge it flies straight ahead at that level's speed for that
// level's time and is gone when it strikes; the full one spins in place where it first strikes, striking again and
// again, and ends with a last, harder hit. Its states: 0 new; 1 to LEVELS the levels; FULL the full one; LAST its
// last hit (each state's hit is in its own script).
#define LEVELS 4
#define FULL (LEVELS + 1)
#define LAST (LEVELS + 2)
#define HITS 5                 // vl.prc param_shuriken vanish_hit_count: the full one's hits before its last
#define HELD 30                 // READING: frames it goes on striking after its first hit (vl.prc vanish_frame, times four)
#define HELD_SPEED 0.8f     // READING: its speed once it has struck (about what its hits push a target)
#define LAST_FRAMES 2                   // READING: frames of its last hit (published: the full one's last hit on 42)
#define SPIN 0.6f                 // READING: radians it turns a frame
#define SOUND_FLY 109           // se_gekkouga_special_n01, by its place in his bank (0: not in it)
static const float speed[FULL] = {5.0f, 4.090000015497208f, 3.180000030994415f, 2.270000046491623f, 2.200000047683716f};
static const short life[FULL] = {16, 22, 29, 36, 36};
static const float scale[FULL] = {0.33333332505491076f, 0.5499999944120647f, 0.7666666637692188f, 0.9833333331263728f, 1.0f};

bool OnGiveDamage(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == FULL)
    {
        id->item_var.var3++;
        return false;
    }
    return id->state != LAST;
}
bool OnHitShieldBounce(GOBJ *item) { return true; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return true; }
bool OnClank(GOBJ *item) { return true; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
// its variables: var1 the fighter's ask, var2 frames it has left, var3 its hits (the full one), var4 frames before it
// was let go, var5 frames it has been held where it struck
int Shuriken_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    if (Lost(item))
        return 1;
    if (id->state == 0)
    {
        if (ask >= 1 && ask <= FULL)
        {
            float s = scale[ask - 1];
            id->self_vel.X = speed[ask - 1] * id->facing_direction;
            id->self_vel.Y = 0;
            id->item_var.var2 = life[ask - 1];
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, ask, 2);
            if (SOUND_FLY)
                Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_FLY, 127, 64);
            model->scale.X = s;
            model->scale.Y = s;
            model->scale.Z = s;
            JOBJ_SetMtxDirtySub(model);
        }
        else if (++id->item_var.var4 > 2)
            return 1;                                   // never let go: gone
        return 0;
    }
    model->rot.Z += SPIN;
    JOBJ_SetMtxDirtySub(model);
    if (id->state == FULL)
    {
        // once it has struck it creeps on at about the speed its hits push a target, so the target stays in it (a
        // still one was left behind by the first push: one hit of five, 2026-11-13)
        if (id->item_var.var3 > 0)
        {
            id->self_vel.X = HELD_SPEED * id->facing_direction;
            id->item_var.var5++;
        }
        if (id->item_var.var3 >= HITS || id->item_var.var5 > HELD || --id->item_var.var2 <= 0)
        {
            id->self_vel.X = 0;
            id->item_var.var2 = LAST_FRAMES;
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, LAST, 2);
        }
        return 0;
    }
    return --id->item_var.var2 <= 0;
}
void Shuriken_Phys(GOBJ *item) {}
int Shuriken_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == 0)
        return 0;
    return (Item_CollAir_NoCB(item) & ~1) != 0;         // a wall or a ceiling ends it; it passes along a floor
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 1, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 2, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 3, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 4, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 5, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll},
    {.state = 6, .animCallback = Shuriken_Anim, .physCallback = Shuriken_Phys, .collCallback = Shuriken_Coll}
};
