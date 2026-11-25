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

// The can of Trick Shot. Kicked, it flies in an arc, lands and stands. Shot, it hops the way it was kicked: each
// shot further and faster, and falling faster after. It explodes when it touches a fighter or a shield, when its
// time runs out, and a set time after its last shot.
#define CAN_HELD 0                  // on the fighter's joint until the kick
#define CAN_FLY 1                   // in the air or rolling to a stop, its hitbox out
#define CAN_STAND 2                 // standing: what touches it sets it off
#define CAN_SHOT 3                  // a hop: the shot itself strikes around it first
#define CAN_MAX 4                   // shot for the last time: it flashes, and a timer runs
#define CAN_MAX_SHOT 5
#define CAN_EXPLODE 6
#define ASK_KICK 1
#define ASK_SHOT 2
#define ASK_KNOCK_R 3               // struck by one of his own gunmen's bullets flying right,
#define ASK_KNOCK_L 4               // or left
#define OWNER_FIRST 377             // the fighter's states that carry it before the kick
#define OWNER_LAST 378
#define SPAWN_EFFECT ((void (*)(GOBJ *, int, int, int, int, Vec3 *, Vec3 *, float))0x8009F834)      // ftCo_8009F834
// vl.prc, param_can, by name; how they are put together is a READING of the order the game's state code reads them in
#define KICK_X 0.5099999904632568f             // initial_speed_x: the kick, the way he faces
#define KICK_Y 1.399999976158142f             // initial_speed_y
#define FALL 0.05000000074505806f                 // fall_accel_y: what it loses a frame
#define FALL_MAX 1.2000000476837158f         // fall_max_speed_y
#define HOP_X 0.11999999731779099f               // hop_add_speed_x: each shot it hops this much faster sideways,
#define HOP_Y 0.15000000596046448f               // hop_add_speed_y: and upward,
#define HOP_FALL 0.020999999716877937f         // hop_add_fall_accel: and falls this much faster after
#define HOP_MAX_X 2.0f       // max_speed_x
#define HOP_MIN_Y 1.0f       // ground_hop_min_speed_y: a hop off the ground is at least this
#define FALL_LIMIT 0.30000001192092896f     // fall_accel_y_limit: the most it ever loses a frame,
#define FALL_MAX_LIMIT 3.0f     // fall_max_speed_y_limit: and the fastest it ever falls
#define SHOTS 8               // how many times it can be shot (the table's one value of 8; wiki: eight)
#define MAX_X 0.30000001192092896f               // limit_speed_x_mul: the last shot's hop keeps this much of its speed sideways,
#define MAX_Y 0.800000011920929f               // limit_speed_y: and goes up at this,
#define MAX_FALL 0.05999999865889549f         // limit_fall_accel
#define MAX_LIFE 75         // limit_life: frames from the last shot to the explosion (published: 76)
#define LIFE 600                 // life: frames a can lasts
#define SPIN 8.0f                 // min_rotate_speed: degrees it turns a frame in flight,
#define SPIN_HOP 20.0f         // hop_min_rotate_speed: and after a shot,
#define SPIN_DEC 0.10000000149011612f         // dec_rotate_speed_x: which loses this a frame
#define SHOT_FRAMES 2   // frames the shot's own hit is out before the can's (published for the smashes' shots)
#define EXPLODE_FRAMES 40           // frames the can is kept, unseen, after it goes off (its effect and sound run on)
// Its sounds, each by its place in his bank (0: not in it). What plays each in the game is its own state code.
#define SOUND_SHOT 17 // se_duckhunt_special_l03: the gun's own shot (the sound his smashes' shots play)
#define SOUND_RING 18 // se_duckhunt_special_n02: its ring as a shot makes it hop
#define SOUND_FLY 23   // se_duckhunt_special_n03: its whistle from the kick, ended as it lands, hops or goes off
#define SOUND_BANG 20 // se_duckhunt_special_n07: its explosion
#define MELEE_BANG 0x74             // STAND-IN where his bank has no place for that: Melee's own (bombs and capsules, it_80272A60)
#define PLAY_KEPT ((void (*)(ItemData *, int, int, int))0x8026AF0C)     // Item_8026AF0C: a sound the item can end
#define END_KEPT ((void (*)(ItemData *))0x8026B034)                     // Item_8026B034: ends it
typedef struct { u8 delay; u16 gfx; } OwnFx;
static const OwnFx bomb_fx[] = {{0, 6018}, {8, 6019}, {3, 6020}, {1, 6021}, {1, 6022}, {4, 6023}, {6, 6024}, {12, 6025}, {7, 6026}, {2, 6027}};      // the fighter's own explosion (duckhunt_trick_bomb), generator by generator

// its variables: var1 what the fighter asks, var2 shots taken, var3 it is to go off, var4 frames in this state,
// var5 frames left after the last shot, var6 frames of life left; var7 its turning speed in hundredths of a degree
static float Fall_Of(ItemData *id)
{
    float g = id->state >= CAN_MAX ? MAX_FALL : FALL + HOP_FALL * (float)id->item_var.var2;
    return g > FALL_LIMIT ? FALL_LIMIT : g;
}
static void Can_State(GOBJ *item, int state)
{
    ItemData *id = item->userdata;
    Item_ResetAllHitPlayers(id);
    ItemStateChange(item, state, 2);
    id->item_var.var4 = 0;
}
static void Can_Explode(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
    Can_State(item, CAN_EXPLODE);
    JOBJ_SetFlagsAll(item->hsd_object, JOBJ_HIDDEN);    // the can itself is gone: the explosion is its effect and its hit
    END_KEPT(id);
    Item_PlayOnDestroySFXAgain(id, SOUND_BANG ? 5000 + SOUND_BANG : MELEE_BANG, 127, 64);
    id->item_var.var3 = 0;
}
// (the engine takes what this gives back as "the item is to be removed")
static int Can_Touched(GOBJ *item)
{
    ((ItemData *)item->userdata)->item_var.var3 = 1;
    return 0;
}
// it has struck a fighter: it goes off on the next frame (the shot's own hit, out for a hop's first frames, does not set it off)
bool OnGiveDamage(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (!((id->state == CAN_SHOT || id->state == CAN_MAX_SHOT) && id->item_var.var4 < SHOT_FRAMES))
        id->item_var.var3 = 1;
    return false;
}
bool OnHitShieldBounce(GOBJ *item)
{
    ((ItemData *)item->userdata)->item_var.var3 = 1;
    return false;
}
bool OnHitShieldDetermineDestroy(GOBJ *item)
{
    ((ItemData *)item->userdata)->item_var.var3 = 1;
    return false;
}
bool OnClank(GOBJ *item)
{
    ((ItemData *)item->userdata)->item_var.var3 = 1;
    return false;
}
bool OnReflect(GOBJ *item)
{
    Item_ReflectVelocity(item);
    return false;
}
static void Can_Landed(GOBJ *item)
{
    ItemData *id = item->userdata;
    id->self_vel.X = 0;
    id->self_vel.Y = 0;
    END_KEPT(id);
    Can_State(item, id->state >= CAN_MAX ? CAN_MAX : CAN_STAND);
}
static void Can_Fell(GOBJ *item)
{
    ItemData *id = item->userdata;
    Can_State(item, id->state >= CAN_MAX ? CAN_MAX : CAN_FLY);
}
static void Can_Hop(GOBJ *item)
{
    ItemData *id = item->userdata;
    float n, vx, vy;
    if (SOUND_SHOT)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_SHOT, 127, 64);
    if (id->state >= CAN_MAX)
    {
        id->item_var.var3 = 1;                          // shot once more after its last: it goes off
        return;
    }
    END_KEPT(id);
    if (SOUND_RING)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_RING, 127, 64);
    id->item_var.var2++;
    n = (float)id->item_var.var2;
    vx = KICK_X + HOP_X * n;
    vy = KICK_Y + HOP_Y * n;
    if (vx > HOP_MAX_X)
        vx = HOP_MAX_X;
    if (vy < HOP_MIN_Y)
        vy = HOP_MIN_Y;
    if (id->item_var.var2 >= SHOTS)
    {
        vx *= MAX_X;
        vy = MAX_Y;
        id->item_var.var5 = MAX_LIFE;
    }
    id->self_vel.X = vx * id->facing_direction;
    id->self_vel.Y = vy;
    Item_SetAirborne(id);
    Can_State(item, id->item_var.var2 >= SHOTS ? CAN_MAX_SHOT : CAN_SHOT);
    id->item_var.var7 = (int)(SPIN_HOP * 100.0f);
}
// Struck by one of his own gunmen's bullets: knocked along the way the bullet flew, as a shot from that side
// knocks it. It is not one of its shots, and after its last shot it is left as it is.
static void Can_Knock(GOBJ *item, float way)
{
    ItemData *id = item->userdata;
    float n = (float)id->item_var.var2, vx, vy;
    if (id->state >= CAN_MAX)
        return;
    END_KEPT(id);
    if (SOUND_RING)
        Item_PlayOnDestroySFXAgain(id, 5000 + SOUND_RING, 127, 64);
    vx = KICK_X + HOP_X * n;
    vy = KICK_Y + HOP_Y * n;
    if (vx > HOP_MAX_X)
        vx = HOP_MAX_X;
    if (vy < HOP_MIN_Y)
        vy = HOP_MIN_Y;
    id->facing_direction = way;
    id->self_vel.X = vx * way;
    id->self_vel.Y = vy;
    Item_SetAirborne(id);
    Can_State(item, CAN_FLY);
    id->item_var.var7 = (int)(SPIN_HOP * 100.0f);
}
int Can_Anim(GOBJ *item)
{
    ItemData *id = item->userdata;
    JOBJ *model = item->hsd_object;
    GOBJ *owner = id->fighter_gobj;
    int ask = id->item_var.var1;
    id->item_var.var1 = 0;
    id->item_var.var4++;
    if (Lost(item))
        return 1;
    if (id->state == CAN_EXPLODE)
    {
        // the explosion: the fighter's own effect where the can was, then the can is taken away
        if (owner)
        {
            FighterData *fd = owner->userdata;
            Vec3 off, none = {0, 0, 0};
            int i;
            off.X = 0;
            off.Y = id->pos.Y - fd->phys.pos.Y;
            off.Z = (id->pos.X - fd->phys.pos.X) * fd->facing_direction;
            for (i = 0; i < (int)(sizeof(bomb_fx) / sizeof(bomb_fx[0])); i++)
                if (bomb_fx[i].delay == id->item_var.var4 - 1)
                    SPAWN_EFFECT(owner, bomb_fx[i].gfx, 0, 0, 0, &off, &none, 0.0f);
        }
        return id->item_var.var4 > EXPLODE_FRAMES;
    }
    if (id->state == CAN_HELD)
    {
        // carried until the kick; let fall if the fighter is knocked out of the move first
        FighterData *fd = owner ? owner->userdata : 0;
        if (ask == ASK_KICK)
        {
            id->self_vel.X = KICK_X * id->facing_direction;
            id->self_vel.Y = KICK_Y;
            id->item_var.var6 = LIFE;
            id->item_var.var7 = (int)(SPIN * 100.0f);
            Item_SetAirborne(id);
            Can_State(item, CAN_FLY);
            if (SOUND_FLY)
                PLAY_KEPT(id, 5000 + SOUND_FLY, 127, 64);
        }
        else if (!fd || fd->state_id < OWNER_FIRST || fd->state_id > OWNER_LAST)
        {
            id->item_var.var6 = LIFE;
            Item_SetAirborne(id);
            Can_State(item, CAN_FLY);
        }
        return 0;
    }
    if (ask == ASK_SHOT)
        Can_Hop(item);
    if (ask == ASK_KNOCK_R || ask == ASK_KNOCK_L)
        Can_Knock(item, ask == ASK_KNOCK_R ? 1.0f : -1.0f);
    if (id->state == CAN_STAND)
        id->cb.on_detect = (void *)Can_Touched;
    // it goes off: touched, out of time, or its time after the last shot run out
    if (id->item_var.var6 > 0 && --id->item_var.var6 == 0)
        id->item_var.var3 = 1;
    if (id->item_var.var5 > 0 && --id->item_var.var5 == 0)
        id->item_var.var3 = 1;
    if (id->item_var.var3)
    {
        Can_Explode(item);
        return 0;
    }
    // it turns over as it flies (slower as a hop goes on) and stands upright on the ground
    if (id->air_state == 0)
        model->rot.Z = 0;
    else
    {
        if (id->item_var.var7 > (int)(SPIN * 100.0f))
            id->item_var.var7 -= (int)(SPIN_DEC * 100.0f);
        model->rot.Z -= (float)id->item_var.var7 * 0.01f * 0.017453292f * id->facing_direction;
    }
    JOBJ_SetMtxDirtySub(model);
    return 0;
}
void Can_Phys(GOBJ *item)
{
    ItemData *id = item->userdata;
    float most;
    if (id->state == CAN_HELD || id->state == CAN_EXPLODE || id->air_state == 0)
        return;
    most = id->item_var.var2 ? FALL_MAX_LIMIT : FALL_MAX;
    id->self_vel.Y -= Fall_Of(id);
    if (id->self_vel.Y < -most)
        id->self_vel.Y = -most;
}
int Can_Coll(GOBJ *item)
{
    ItemData *id = item->userdata;
    if (id->state == CAN_HELD || id->state == CAN_EXPLODE)
        return 0;
    if (id->air_state == 0)
        Item_CollGround_PassLedge(item, Can_Fell);
    else
        Item_CollAir_Land(item, Can_Landed);
    return 0;
}

__attribute__((used))
static struct ItemState item_state_table[] = {
    {.state = 0, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 1, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 2, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 3, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 4, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 5, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
    {.state = 6, .animCallback = Can_Anim, .physCallback = Can_Phys, .collCallback = Can_Coll},
};
