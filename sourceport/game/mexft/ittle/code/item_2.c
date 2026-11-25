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

// The bomb of Dynamite. Thrown, it falls under the game's gravity for it, bounces off a floor it strikes fast
// enough (keeping some of its speed), slides to a stop along one, bounces back off walls, and touches whoever it
// meets for a small hit; when its fuse is out it bursts: a big hit about it for a few frames, then it is gone.
#define GRAV 0.07614127981402409f                // its gravity (IttleBomb gravity), a frame a frame
#define BOUNCE_MIN 0.22842383944207229f    // it bounces off a floor it strikes falling faster than this (groundbouncespeed),
#define BOUNCE_KEEP 0.800000011920929f  //   keeping this much of its speed (groundbouncespdmult; and off walls)
#define DRAG 0.0025380426604674697f                // on a floor it slows by this a frame (xDrag)
#define FUSE 90                // frames from the throw to the burst (fuselength)
#define BLAST 6              // frames the burst strikes for (explosionduration)
#define SOUND_BLAST 40        // se_ittledew_explosion, by its place in her bank (0: not in it)
#define MELEE_BANG 0x74                 // STAND-IN where her bank has no place for it: Melee's own (it_80272A60)
#define BLAST_LOOK 0x40E                // STAND-IN: Melee's own bomb burst (Link's bombs: it_80272A60); the game's is particles
#define ST_NEW 0
#define ST_FUSE 1
#define ST_BLAST 2

// its variables: var1 what the fighter asks, var2 frames of fuse left, var3 frames of burst left, var4 frames it has
// waited, var5 frames before a wall may turn it again
bool OnGiveDamage(GOBJ *item) { return false; }        // its touch does not end it
bool OnHitShieldBounce(GOBJ *item) { return false; }
bool OnHitShieldDetermineDestroy(GOBJ *item) { return false; }
bool OnClank(GOBJ *item) { return false; }
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
static void Bomb_Burst(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
    Item_ResetAllHitPlayers(id);
    ItemStateChange(item, ST_BLAST, 2);
    JOBJ_SetFlagsAll(item->hsd_object, JOBJ_HIDDEN);   // the bomb itself is gone: the burst is its look and its hit
    Effect_SpawnSync(BLAST_LOOK, item, &id->pos);
    Item_PlayOnDestroySFXAgain(id, SOUND_BLAST ? 5000 + SOUND_BLAST : MELEE_BANG, 127, 64);
    id->item_var.var3 = BLAST;
}
int Bomb_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    if (Lost(item))
        return 1;
    if (id->state == ST_NEW)
    {
        if (ask)
        {
            // (its speed is the fighter's: she set it as she let it go)
            id->item_var.var2 = FUSE;
            Item_SetAirborne(id);
            Item_ResetAllHitPlayers(id);
            ItemStateChange(item, ST_FUSE, 2);
        }
        else if (++id->item_var.var4 > 2)
            return 1;                                   // never let go: gone
        return 0;
    }
    if (id->state == ST_BLAST)
        return --id->item_var.var3 < 0;
    if (id->item_var.var5 > 0)
        id->item_var.var5--;
    if (--id->item_var.var2 <= 0)
        Bomb_Burst(item);
    return 0;
}
void Bomb_Phys(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == ST_FUSE)
        id->self_vel.Y -= GRAV;
}
int Bomb_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    int touch;
    if (id->state != ST_FUSE)
        return 0;
    touch = Item_CollAir_NoCB(item);                    // 1 a floor, 2 a ceiling, 4 and 8 a wall
    if (touch & 1)
    {
        if (id->self_vel.Y < -BOUNCE_MIN)
        {
            id->self_vel.X *= BOUNCE_KEEP;
            id->self_vel.Y *= -BOUNCE_KEEP;
        }
        else
        {
            if (id->self_vel.Y < 0)
                id->self_vel.Y = 0;
            if (id->self_vel.X > DRAG)
                id->self_vel.X -= DRAG;
            else if (id->self_vel.X < -DRAG)
                id->self_vel.X += DRAG;
            else
                id->self_vel.X = 0;
        }
    }
    if ((touch & 2) && id->self_vel.Y > 0)
        id->self_vel.Y *= -BOUNCE_KEEP;
    if ((touch & 0xC) && id->item_var.var5 == 0)
    {
        id->self_vel.X *= -BOUNCE_KEEP;
        id->item_var.var5 = 3;                          // (pushed out of the wall over the next frames: not turned again)
    }
    return 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Bomb_Anim, .physCallback = Bomb_Phys, .collCallback = Bomb_Coll},
    {.state = 1, .animCallback = Bomb_Anim, .physCallback = Bomb_Phys, .collCallback = Bomb_Coll},
    {.state = 2, .animCallback = Bomb_Anim, .physCallback = Bomb_Phys, .collCallback = Bomb_Coll},
};
