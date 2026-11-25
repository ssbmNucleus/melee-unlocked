/* The online character select's Options (Direct, Unranked, Ranked and Teams), in Melee's own look.
 *
 * The bar: the offline VS select's rules bar, in the same place. It is that select's own model (the
 * VS menu of MnSlChr's data table, with everything but the bar hidden) and its own text object and
 * size, reading "Options". The hand flashes over it as it does over the rules bar, and A opens the
 * menu with the sound the rules bar makes.
 *
 * The menu: the main menu's Options panel (its gold "Options" heading, frame and description box),
 * with rows as the Additional Rules screen draws them (MenMainConRl, MenMainCursorRl, the ON/OFF
 * value of MenMainCursorTr02), the menus' background and lights, all from the menus' own archive
 * (MnMaAll), drawn full screen by two cameras of their own over the select. It runs in place, on
 * top of the select, rather than as a menu scene of its own: the rules screen is a separate minor
 * scene, and leaving the online select ends the search or the connection. Under it the select keeps running
 * with no input reaching it (as a player who put the controller down), so a search goes on and a
 * connection stays; B closes the menu and the select is as it was, hand and coin included. Up and
 * down move as the rules screen does, left and right change the value, B (or START) goes back.
 *
 * The row labels are cut letter by letter from the disc's own label pictures (the rules screen's
 * "Rules", "Time Limit", "Stock", "Handicap" and the ON/OFF strip), as shim/mu_mods_menu.c does,
 * after a hash check of those pictures; on another disc the label stays empty. The description
 * under the rows is the main menus' own two-line text (the same stream codes, colour and box).
 *
 * Every option goes to the host as a setting (host command 0xDA with two bytes, port/app/source_host.cpp)
 * and nothing here touches the match or the wire. "Opponents' skins" is local to this machine. "Items"
 * (Direct and Teams only) is this player's choice; the host tells the peers and decides the match's
 * rule (on only when every player's game has the rule and every player has it on), and its answer
 * carries that state: the row's description says what the match will use and whose setting keeps
 * items off, the row is dimmed and Off against a game without the rule, and the bar reads "Options:
 * Items on" when they are on. Off in a Vanilla run and in replays (the host says so). "Squad Strike"
 * (Direct only, never a peer-to-peer session) is agreed exactly as Items is: on only when every
 * player's game plays it and every player has it on; then Z on the select picks a squad
 * (shim/mu_squad_css.c) and the bar reads "Options: Squad Strike on". */
#include <string.h>
#include <dolphin/os.h>
#include <melee/gm/gm_1A36.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbfile.h>
#include <melee/lb/lbheap.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/types.h>
#include <melee/sc/types.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/displayfunc.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>
#include <mu_native.h>
#include "mu_menu_label.h"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);
const void* mu_archive_block_start(const void* p);       /* shim/mu_data_guard.c */
int mu_slippi_css_online(void);                          /* shim/mu_slippi_css.c */
int mu_slippi_css_port(void);
int mu_slippi_css_scene_request(void);
int mu_slippi_css_chat_open(void);

/* MU_CMD_ONLINE_OPTION (mu_commands.h): with two bytes: an online select option (port/app/source_host.cpp) */

/* ---- the options ---- */

enum { KIND_TOGGLE, KIND_CHOICE };

/* A piece of a row label: columns left..right of a source picture, rows top..bottom, `flip` mirrored
 * left to right (Squad Strike's q is Handicap's p). src -2 is a space of `left` columns, -1 ends it. */
typedef struct Piece {
    signed char src;
    unsigned char left, right, top, bottom;
    unsigned char flip;
} Piece;

typedef struct Option {
    const char* label;          /* the words (logs); the row shows `cut` */
    const Piece* cut;           /* the label, cut from the disc's pictures */
    const char* desc[2];        /* the description's lines */
    int kind;
    int count;                  /* values; a toggle has 2: 0 off, 1 on */
    unsigned char host_id;      /* the host's number for it (source_host.cpp) */
} Option;

/* Source pictures of the labels: MenMainCursorRl joint 7's frames 0-3 (152x16 I4: "Rules",
 * "Time Limit", "Stock", "Handicap") and MenMainCursorTr02's ON/OFF strip (128x16 I4). */
enum { F_RULES, F_TIME, F_STOCK, F_HANDICAP, F_ONOFF, F_ITEM_SWITCH, F_FRIENDLY_FIRE, SOURCES, SPACE = -2, END = -1 };
enum { LW = 152, LH = 16, LBYTES = LW * LH / 2, SW = 128, SBYTES = SW * LH / 2, WIDE = 320 };
#define SOURCE_HASH 0x31FD5334u   /* FNV-1a of the 1.02 pictures above, in that order */
#define ITEM_SWITCH 6             /* the same animation's "Item Switch" picture (152x16 I4) */
#define ITEM_SWITCH_HASH 0x209CD4BBu   /* its FNV-1a on 1.02 */
#define FRIENDLY_FIRE 9           /* ... and its "Friendly Fire" picture: Squad Strike's r */
#define FRIENDLY_FIRE_HASH 0x0CA07DB8u /* its FNV-1a on 1.02 (MnMaAll.usd) */
#define LABEL_GAP 2               /* blank columns between letters, as the disc's labels have */
#define LABEL_FIT 150             /* the label's widest, as the game fits a long one */

static const Piece cut_opponents_skins[] = {
    {F_ONOFF, 16, 30, 0, 15},      /* O of ON */
    {F_HANDICAP, 83, 94, 0, 15},   /* p */
    {F_HANDICAP, 83, 94, 0, 15},   /* p */
    {F_STOCK, 26, 37, 0, 15},      /* o */
    {F_HANDICAP, 29, 39, 0, 15},   /* n */
    {F_RULES, 36, 47, 0, 15},      /* e */
    {F_HANDICAP, 29, 39, 0, 15},   /* n */
    {F_STOCK, 16, 24, 0, 15},      /* t */
    {F_RULES, 48, 58, 0, 15},      /* s */
    {F_RULES, 29, 33, 0, 4},       /* ', the top of the l */
    {SPACE, 5, 0, 0, 0},
    {F_RULES, 48, 58, 0, 15},      /* s */
    {F_STOCK, 53, 63, 0, 15},      /* k */
    {F_TIME, 15, 19, 0, 15},       /* i */
    {F_HANDICAP, 29, 39, 0, 15},   /* n */
    {F_RULES, 48, 58, 0, 15},      /* s */
    {END, 0, 0, 0, 0},
};

static const Piece cut_items[] = {
    {F_ITEM_SWITCH, 1, 45, 0, 15}, /* Item, of Item Switch */
    {F_RULES, 48, 58, 0, 15},      /* s */
    {END, 0, 0, 0, 0},
};

/* No q and no lowercase r in the rules screen's own labels: q is Handicap's p mirrored, r comes from
 * the same animation's "Friendly Fire" (the same lettering). Without that picture the label stays empty. */
static const Piece cut_squad_strike[] = {
    {F_STOCK, 1, 14, 0, 15},          /* S */
    {F_HANDICAP, 83, 94, 0, 15, 1},   /* q: p mirrored */
    {F_RULES, 16, 26, 0, 15},         /* u */
    {F_HANDICAP, 16, 27, 0, 15},      /* a */
    {F_HANDICAP, 41, 51, 0, 15},      /* d */
    {SPACE, 5, 0, 0, 0},
    {F_STOCK, 1, 14, 0, 15},          /* S */
    {F_STOCK, 16, 24, 0, 15},         /* t */
    {F_FRIENDLY_FIRE, 13, 20, 0, 15}, /* r, of Friendly */
    {F_TIME, 15, 18, 0, 15},          /* i */
    {F_STOCK, 53, 63, 0, 15},         /* k */
    {F_RULES, 36, 47, 0, 15},         /* e */
    {END, 0, 0, 0, 0},
};

/* The table. A new entry needs its label's pieces, its host number (and the host's handler) and
 * nothing else here. */
enum { OPT_SKINS, OPT_ITEMS, OPT_SQUAD };
static const Option options[] = {
    {"Opponents' skins", cut_opponents_skins,
     {"With this on, you see your opponent's", "SSBM Nucleus skin, and they see yours."},
     KIND_TOGGLE, 2, 0},
    {"Items", cut_items,
     {"Items at Medium in Direct and Teams,", "when every player turns them on."},
     KIND_TOGGLE, 2, 1},
    {"Squad Strike", cut_squad_strike,
     {"Squad Strike in Direct: up to four", "fighters each, when all turn it on."},
     KIND_TOGGLE, 2, 2},
};
enum { OPTION_COUNT = (int) (sizeof options / sizeof options[0]) };

/* The host's Items status (port/runtime/hle/slippi_online.h ItemsStatus). */
enum { ITEMS_NO_PEERS, ITEMS_ON, ITEMS_OFF_MINE, ITEMS_OFF_THEIRS, ITEMS_UNSUPPORTED, ITEMS_OTHER_MODE };

/* Items is a Direct and Teams option: never shown in Ranked or Unranked. Squad Strike: Direct only
 * (the host also keeps it out of a peer-to-peer session). */
static int mode_offers(int i)
{
    const int mode = mu_slippi_state()->mode;
    if (i == OPT_SQUAD) return mode == MU_SLP_MODE_DIRECT && !mu_online_p2p_menus();
    return i != OPT_ITEMS || mode == MU_SLP_MODE_DIRECT || mode == MU_SLP_MODE_TEAMS;
}

/* The options every player must agree on (each player's choice goes to the peers; the host decides). */
static int agreed(int i)
{
    return i == OPT_ITEMS || i == OPT_SQUAD;
}

/* What the host says of an option besides its value (Items: what the match will use). */
typedef struct Extra {
    int status;     /* ITEMS_* */
    int blocker;    /* the port (0-3) of the player keeping items off, -1 none or this player */
    int locked;     /* this player is locked in: the choice waits */
} Extra;

/* The host's value of an option (0xFF asks), and whether it offers it at all. -1: no answer. */
static int host_option(int i, int value, int* shown, Extra* extra)
{
    static unsigned char reply[4096];   /* the host answers into no less */
    unsigned char payload[2];
    unsigned int got = 0;
    payload[0] = options[i].host_id;
    payload[1] = (unsigned char) value;
    if (extra) extra->status = ITEMS_NO_PEERS, extra->blocker = -1, extra->locked = 0;
    if (mu_online_abi_command(MU_CMD_ONLINE_OPTION, payload, sizeof payload, reply, sizeof reply, &got) != 0 || got < 2) {
        if (shown) *shown = 0;
        return -1;
    }
    if (shown) *shown = reply[1] != 0 && mode_offers(i);
    if (extra && got >= 5) {
        extra->status = reply[2];
        extra->blocker = (int) reply[3] - 1;
        extra->locked = reply[4] != 0;
    }
    return reply[0];
}

/* ---- the select's cursor (mncharsel.c's file-private cursors, by the pointer TM-CE reads) ---- */

typedef struct Cursor {
    HSD_GObj* gobj;
    u8 port, state, door, x7;
    u16 hover, timer;
    f32 x, y;
} Cursor;
extern void* mu_tmce_ref_stc_css_cursors;   /* &mnCharSel_804A0BC0 */

static Cursor* hand(void)
{
    Cursor** cursors = (Cursor**) mu_tmce_ref_stc_css_cursors;
    return cursors != NULL ? cursors[0] : NULL;
}

/* The rules bar's place on the VS select (mnCharSel_CursorThink). */
static int over_bar(const Cursor* c)
{
    return c != NULL && c->gobj != NULL && c->state != 3 && c->x > -17.0f && c->x < 15.0f && c->y > 22.0f;
}

/* ---- state ---- */

/* Gx links of the menu (above every link the select uses) and its cameras' render order (after
 * the select's camera and its text cameras). */
enum { L_LIGHTS = 20, L_FOG, L_BACK, L_PANEL, L_ROWS, L_DESC = 26, CAM_A_PRIO = 200, CAM_B_PRIO = 201 };
enum { ROW_PARTS = 17, OPEN_COOLDOWN = 5 };

static struct {
    int scene;                 /* an online select is up (set per visit, by its text init) */
    int built;                 /* the bar: 0 not yet, 1 shown, -1 not this visit */
    unsigned counter;
    HSD_GObj* bar;
    HSD_Text* bar_text;
    HSD_GObj* flash;
    int open;
    int swallow;               /* after closing: buttons held over stay with the menu until released */
    int cooldown;
    int row;
    int value[OPTION_COUNT];
    int shown[OPTION_COUNT];
    Extra extra[OPTION_COUNT];
    int bar_items;             /* what the bar names as on: 1 Items, 2 Squad Strike */
    /* this visit's MnMaAll and what it gave */
    HSD_Archive* archive;
    int loaded;                /* 1 loaded, -1 refused this visit */
    int labels;                /* 1 cut, -1 the disc's pictures differ */
    int canvas;                /* the description's SIS canvas, -1 none yet */
    StaticModelSyms back, panel, con, cursor, onoff;
    HSD_CObjDesc* cam;
    LightListSlot* lights;
    HSD_FogDesc* fogdesc;
    /* the open menu */
    HSD_GObj *cam_a, *cam_b, *light_gobj, *fog_gobj, *back_gobj, *panel_gobj, *con_gobj;
    HSD_LObj* lobj;
    HSD_Fog* fog;
    HSD_JObj* con_enter;       /* the container's joint 2: its entry */
    int entering;
    HSD_JObj* parts[OPTION_COUNT][ROW_PARTS];
    HSD_JObj* values[OPTION_COUNT];
    HSD_Text* desc;
} oo;

static void restore_think(HSD_GObj* gobj);
static void row_pictures(int i);

static unsigned char label_pixels[OPTION_COUNT][LBYTES] __attribute__((aligned(32)));
static HSD_ImageDesc label_desc[OPTION_COUNT];
/* A row the player cannot change (Items against a game without the rule): its label and value strip
 * at under half their brightness, as the menus dim a locked row (shim/mu_mex_menu.c). */
static unsigned char label_dim_pixels[OPTION_COUNT][LBYTES] __attribute__((aligned(32)));
static HSD_ImageDesc label_dim_desc[OPTION_COUNT];
static unsigned char strip_dim_pixels[SBYTES] __attribute__((aligned(32)));
static HSD_ImageDesc strip_dim_desc;
static HSD_ImageDesc* strip_desc;   /* the archive's own ON/OFF strip, to undo the dimming */

/* The rules screen's own tables (mnruleplus.c, mnmain.c). */
static AnimLoopSettings row_glow = {0.0f, 99.0f, 0.0f};          /* the selected row's highlight */
static AnimLoopSettings row_arrows = {200.0f, 399.0f, 200.0f};   /* its value arrows */
static AnimLoopSettings row_body[2] = {{100.0f, 124.0f, -0.1f}, {125.0f, 149.0f, -0.1f}};
static AnimLoopSettings value_right[3] = {{0.0f, 3.0f, -0.1f}, {20.0f, 23.0f, -0.1f}, {40.0f, 43.0f, -0.1f}};
static AnimLoopSettings value_left[3] = {{100.0f, 103.0f, -0.1f}, {120.0f, 123.0f, -0.1f}, {140.0f, 143.0f, -0.1f}};
static AnimLoopSettings con_entry = {30.0f, 49.0f, -0.1f};
static AnimLoopSettings panel_loop = {0.0f, 799.0f, 0.0f};
static const float label_frames[2] = {20.0f, 21.0f};             /* the first row's, not selected and selected */
/* The heading: the Options menu's ("Options", its gold light), mnmain.c's kind 4. The VS rules
 * screen's own would be {2900, 2999, 2920} with light 1 ("VS. Mode", "Custom Rules", red). */
static AnimLoopSettings heading_idle = {900.0f, 999.0f, 900.0f};
#define MENU_LIGHT 3

/* ---- the label, cut from the disc's pictures ---- */

typedef struct Pic { const unsigned char* data; int w; } Pic;

static unsigned fnv(const unsigned char* p, int n, unsigned h)
{
    int i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static unsigned i4_get(const Pic* p, int x, int y)
{
    const unsigned v = p->data[((y >> 3) * (p->w >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    return x & 1 ? v & 15 : v >> 4;
}

static void i4_set(unsigned char* data, int w, int x, int y, unsigned v)
{
    unsigned char* b = &data[((y >> 3) * (w >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    *b = (unsigned char) (x & 1 ? (*b & 0xF0) | (v & 15) : (*b & 0x0F) | (v & 15) << 4);
}

/* MatAnimJoint walk in the archive's order (as shim/mu_mods_menu.c): the first texture animation
 * with pictures of joint number `want`. */
static int find_texanim(HSD_MatAnimJoint* joint, int index, int want, HSD_TexAnim** out, int depth)
{
    int next = index + 1;
    if (joint == NULL || depth > 64) return next;
    if (index == want && *out == NULL) {
        HSD_MatAnim* material;
        for (material = DP(joint->matanim); material != NULL && *out == NULL; material = DP(material->next)) {
            HSD_TexAnim* texture;
            for (texture = DP(material->texanim); texture != NULL; texture = DP(texture->next))
                if (!DISC_NULL(texture->imagetbl)) { *out = texture; break; }
        }
    }
    if (!DISC_NULL(joint->child)) next = find_texanim(DP(joint->child), next, want, out, depth + 1);
    if (!DISC_NULL(joint->next)) next = find_texanim(DP(joint->next), next, want, out, depth + 1);
    return next;
}

/* The first picture of a joint tree, by its descriptions. */
static HSD_ImageDesc* first_picture(HSD_Joint* joint, int depth)
{
    for (; joint != NULL && depth < 64; joint = DP(joint->next)) {
        const u32 flags = joint->flags;
        if (!(flags & (JOBJ_PTCL | JOBJ_SPLINE))) {
            HSD_DObjDesc* dobj;
            for (dobj = DP(joint->u.dobjdesc); dobj != NULL; dobj = DP(dobj->next)) {
                HSD_MObjDesc* mobj = DP(dobj->mobjdesc);
                HSD_TObjDesc* tobj;
                for (tobj = mobj ? DP(mobj->texdesc) : NULL; tobj != NULL; tobj = DP(tobj->next))
                    if (!DISC_NULL(tobj->imagedesc)) return DP(tobj->imagedesc);
            }
        }
        if (!(flags & JOBJ_INSTANCE) && !DISC_NULL(joint->child)) {
            HSD_ImageDesc* found = first_picture(DP(joint->child), depth + 1);
            if (found != NULL) return found;
        }
    }
    return NULL;
}

static int is_i4(const HSD_ImageDesc* image, int w, int h)
{
    return image != NULL && image->width == w && image->height == h && image->format == GX_TF_I4 &&
           !DISC_NULL(image->image_ptr);
}

/* One label: the pieces side by side on a wide plane (each copied only inside its rows), then
 * narrowed to LABEL_FIT columns as the game fits a long label, from column 1 as the disc's have it. */
static void cut_label(const Piece* pieces, const Pic* src, unsigned char* out)
{
    static unsigned char wide[LH][WIDE];
    int total = 0, cursor = 0, i, x, y;
    memset(wide, 0, sizeof wide);
    for (i = 0; pieces[i].src != END; ++i) {
        const Piece* p = &pieces[i];
        if (p->src == SPACE) {
            cursor += p->left;
            continue;
        }
        for (y = p->top; y <= p->bottom && y < LH; ++y)
            for (x = p->left; x < p->right; ++x) {
                const int tx = cursor + x - p->left;
                const unsigned v = i4_get(&src[p->src], p->flip ? p->right - 1 - (x - p->left) : x, y);
                if (tx >= 0 && tx < WIDE && v > wide[y][tx]) wide[y][tx] = (unsigned char) v;
            }
        cursor += p->right - p->left + LABEL_GAP;
        total = cursor - LABEL_GAP;
    }
    memset(out, 0, LBYTES);
    {
        const int width = total > LABEL_FIT ? LABEL_FIT : total;
        const float step = total > LABEL_FIT ? (float) total / (float) LABEL_FIT : 1.0f;
        for (y = 0; y < LH; ++y)
            for (x = 0; x < width; ++x) {
                const float x0 = (float) x * step, x1 = (float) (x + 1) * step;
                float sum = 0.0f;
                int c;
                for (c = (int) x0; (float) c < x1 && c < total; ++c) {
                    const float lo = x0 > (float) c ? x0 : (float) c;
                    const float hi = x1 < (float) (c + 1) ? x1 : (float) (c + 1);
                    sum += (float) wide[y][c] * (hi - lo);
                }
                {
                    int v = (int) (sum / step + 0.5f);
                    i4_set(out, LW, 1 + x, y, (unsigned) (v > 15 ? 15 : v));
                }
            }
    }
}

/* A dimmed copy of an I4 picture: each pixel at 2/5 of its intensity (shim/mu_mex_menu.c's locked rows). */
static void dim_copy(const unsigned char* from, unsigned char* to, int bytes)
{
    int i;
    for (i = 0; i < bytes; ++i)
        to[i] = (unsigned char) (((from[i] >> 4) * 2 / 5) << 4 | ((from[i] & 15) * 2 / 5));
}

/* One more picture of the label animation, checked on its own (its FNV-1a); 1 when it is the 1.02 one. */
static int extra_source(HSD_TexAnim* anim, int index, unsigned want, Pic* out)
{
    HSD_ImageDesc* pic;
    if (anim->n_imagetbl <= index) return 0;
    pic = DP(DP(anim->imagetbl)[index]);
    if (!is_i4(pic, LW, LH) || fnv(DP(pic->image_ptr), LBYTES, 0x811C9DC5u) != want) return 0;
    out->data = DP(pic->image_ptr);
    out->w = LW;
    return 1;
}

/* The labels into this visit's MnMaAll: every picture of the row label's animation becomes the first
 * option's label, so a row shows one at whatever frame the rules screen's animation picks; each row
 * then swaps in its own (row_pictures). "Item" comes from the same animation's "Item Switch" picture
 * and Squad Strike's r from its "Friendly Fire", each checked on its own: on another disc the row whose
 * letters it gives keeps an empty label. */
static int make_labels(void)
{
    HSD_TexAnim* anim = NULL;
    HSD_ImageDesc* frames[4];
    HSD_ImageDesc* strip;
    Pic src[SOURCES];
    int have[SOURCES];
    unsigned hash = 0x811C9DC5u;
    int i, k, item_switch = 0;
    find_texanim(oo.cursor.matanim_joint, 0, 7, &anim, 0);
    strip = first_picture(oo.onoff.joint, 0);
    if (anim == NULL || anim->n_imagetbl < 4 || !is_i4(strip, SW, LH)) return -1;
    for (i = 0; i < 4; ++i) {
        frames[i] = DP(DP(anim->imagetbl)[i]);
        if (!is_i4(frames[i], LW, LH)) return -1;
        src[i].data = DP(frames[i]->image_ptr);
        src[i].w = LW;
        hash = fnv(src[i].data, LBYTES, hash);
    }
    src[F_ONOFF].data = DP(strip->image_ptr);
    src[F_ONOFF].w = SW;
    hash = fnv(src[F_ONOFF].data, SBYTES, hash);
    if (hash != SOURCE_HASH) {
        OSReport("[options] the disc's rule labels are not the 1.02 ones (%08x); rows without labels\n", hash);
        return -1;
    }
    for (i = 0; i < SOURCES; ++i) have[i] = i < F_ITEM_SWITCH;
    item_switch = have[F_ITEM_SWITCH] = extra_source(anim, ITEM_SWITCH, ITEM_SWITCH_HASH, &src[F_ITEM_SWITCH]);
    have[F_FRIENDLY_FIRE] = extra_source(anim, FRIENDLY_FIRE, FRIENDLY_FIRE_HASH, &src[F_FRIENDLY_FIRE]);
    if (!item_switch) OSReport("[options] the disc's \"Item Switch\" label differs; the Items row has no label\n");
    if (!have[F_FRIENDLY_FIRE])
        OSReport("[options] the disc's \"Friendly Fire\" label differs; the Squad Strike row has no label\n");
    for (i = 0; i < OPTION_COUNT; ++i) {
        const Piece* cut = options[i].cut;
        for (k = 0; cut[k].src != END; ++k)
            if (cut[k].src >= 0 && !have[cut[k].src]) break;
        if (cut[k].src == END) cut_label(cut, src, label_pixels[i]);
        else memset(label_pixels[i], 0, LBYTES);
        dim_copy(label_pixels[i], label_dim_pixels[i], LBYTES);
        label_desc[i] = *frames[0];
        DISC_SET(label_desc[i].image_ptr, label_pixels[i]);
        label_dim_desc[i] = *frames[0];
        DISC_SET(label_dim_desc[i].image_ptr, label_dim_pixels[i]);
    }
    dim_copy(src[F_ONOFF].data, strip_dim_pixels, SBYTES);
    strip_dim_desc = *strip;
    DISC_SET(strip_dim_desc.image_ptr, strip_dim_pixels);
    strip_desc = strip;
    for (k = 0; k < anim->n_imagetbl; ++k) DISC_SET(DP(anim->imagetbl)[k], &label_desc[0]);
    return 1;
}

/* Every w x h I4 picture of a joint tree becomes `to` (with `only`, just the ones showing `only` or
 * `to`'s dimmed twin). Run after the frame's animations, which may set the pictures again. */
static void swap_pictures(HSD_JObj* j, int w, int h, HSD_ImageDesc* to, const HSD_ImageDesc* only, int depth)
{
    HSD_JObj* child;
    if (j == NULL || to == NULL || depth > 32) return;
    if (!(j->flags & (JOBJ_PTCL | JOBJ_SPLINE))) {
        HSD_DObj* dobj;
        for (dobj = j->u.dobj; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* texture;
            if (dobj->mobj == NULL) continue;
            for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
                const HSD_ImageDesc* now = texture->imagedesc;
                if (now == NULL || now->width != w || now->height != h || now->format != GX_TF_I4) continue;
                if (only == NULL || now == only || now == &strip_dim_desc) texture->imagedesc = to;
            }
        }
    }
    if (!(j->flags & JOBJ_INSTANCE))
        for (child = j->child; child != NULL; child = child->next) swap_pictures(child, w, h, to, only, depth + 1);
}

/* ---- this visit's MnMaAll ---- */

static int load_menus(void)
{
    const size_t size = lbFileGetSize("MnMaAll");
    long room;
    if (oo.loaded != 0) return oo.loaded;
    room = lbHeap_HsdFreeBytes(0);
    if (size == 0 || room < 0 || (unsigned long) room < (unsigned long) size + 0x80000ul) {
        OSReport("[options] no room for the menus' archive (%u bytes, %ld free); Options stays shut\n",
                 (unsigned) size, room);
        return oo.loaded = -1;
    }
    oo.archive = lbArchive_LoadArchive("MnMaAll");
    if (oo.archive == NULL) return oo.loaded = -1;
    lbArchive_LoadSections(
        oo.archive, (void**) &oo.back.joint, "MenMainBack_Top_joint", &oo.back.animjoint, "MenMainBack_Top_animjoint",
        &oo.back.matanim_joint, "MenMainBack_Top_matanim_joint", &oo.back.shapeanim_joint,
        "MenMainBack_Top_shapeanim_joint", &oo.panel.joint, "MenMainPanel_Top_joint", &oo.panel.animjoint,
        "MenMainPanel_Top_animjoint", &oo.panel.matanim_joint, "MenMainPanel_Top_matanim_joint",
        &oo.panel.shapeanim_joint, "MenMainPanel_Top_shapeanim_joint", &oo.con.joint, "MenMainConRl_Top_joint",
        &oo.con.animjoint, "MenMainConRl_Top_animjoint", &oo.con.matanim_joint, "MenMainConRl_Top_matanim_joint",
        &oo.con.shapeanim_joint, "MenMainConRl_Top_shapeanim_joint", &oo.cursor.joint, "MenMainCursorRl_Top_joint",
        &oo.cursor.animjoint, "MenMainCursorRl_Top_animjoint", &oo.cursor.matanim_joint,
        "MenMainCursorRl_Top_matanim_joint", &oo.cursor.shapeanim_joint, "MenMainCursorRl_Top_shapeanim_joint",
        &oo.onoff.joint, "MenMainCursorTr02_Top_joint", &oo.onoff.animjoint, "MenMainCursorTr02_Top_animjoint",
        &oo.onoff.matanim_joint, "MenMainCursorTr02_Top_matanim_joint", &oo.onoff.shapeanim_joint,
        "MenMainCursorTr02_Top_shapeanim_joint", &oo.cam, "ScMenMain_cam_int1_camera", &oo.lights,
        "ScMenMain_scene_lights", &oo.fogdesc, "ScMenMain_fog", NULL);
    if (oo.back.joint == NULL || oo.panel.joint == NULL || oo.con.joint == NULL || oo.cursor.joint == NULL ||
        oo.onoff.joint == NULL || oo.cam == NULL || oo.lights == NULL || oo.fogdesc == NULL) {
        OSReport("[options] the menus' archive lacks the rules screen's parts; Options stays shut\n");
        return oo.loaded = -1;
    }
    oo.labels = make_labels();
    OSReport("[options] menus' archive read (%u bytes, %ld were free), labels %s\n", (unsigned) size, room,
             oo.labels > 0 ? "cut" : "left empty");
    return oo.loaded = 1;
}

/* ---- the bar ---- */

/* The select's own archive (MnSlChr), by the camera the select took from it, and in it the data
 * table: the VS menu model is its models' fourth (MnSelectChrDataTable + 0x40). */
static StaticModelDesc* vs_menu_model(void)
{
    const u8* data = (const u8*) mu_archive_block_start(MenMain_cam);
    HSD_Archive a;
    u32 offset;
    u8* table;
    if (data == NULL) return NULL;
    memset(&a, 0, sizeof a);
    memcpy(&a.header, data - sizeof(HSD_ArchiveHeader), sizeof(HSD_ArchiveHeader));
    if (a.header.data_size == 0 || mu_archive_room(data) != a.header.data_size || a.header.nb_public == 0 ||
        a.header.nb_public > 4096 || a.header.nb_extern > 4096 || a.header.nb_reloc > 0x100000)
        return NULL;
    a.data = (u8*) data;
    offset = a.header.data_size + a.header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    a.public_info = (HSD_ArchivePublicInfo*) (data + offset);
    offset += a.header.nb_public * sizeof(HSD_ArchivePublicInfo) + a.header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    if (offset + sizeof(HSD_ArchiveHeader) >= a.header.file_size) return NULL;
    a.symbols = (char*) (data + offset);
    table = HSD_ArchiveGetPublicAddress(&a, "MnSelectChrDataTable");
    if (table == NULL || DP(*(DISC_PTR(HSD_CObjDesc)*) table) != MenMain_cam) return NULL;
    return (StaticModelDesc*) (table + 0x40);
}

/* A text object's words as one stream of the game's own (as shim/mu_menu_label.c builds them): an
 * optional head of commands, the lines' glyphs (the game's encoder, glyphs and spaces only) with
 * `between` after each but the last, an optional tail. The text object owns and frees it. */
static void set_stream(HSD_Text* text, const u8* head, int head_n, const char* const* lines, int n_lines,
                       const u8* between, int between_n, const u8* tail, int tail_n)
{
    static u8 encoded[512], stream[1024];
    SisBuffer* alloc;
    int n = 0, line, i;
    if (text == NULL) return;
    if (head_n > 0) memcpy(stream, head, head_n), n = head_n;
    for (line = 0; line < n_lines; ++line) {
        const int length = strlen(lines[line]) < 200 ? HSD_SisLib_803A67EC(encoded, (u8*) lines[line]) : 0;
        for (i = 0; i < length;) {
            const unsigned op = encoded[i];
            const int size = op >= 0x20 ? 2 : mu_menu_sis_command_size(op);
            if ((op >= 0x20 || op == 0x1A) && n + size < (int) sizeof stream - 64) {
                memcpy(stream + n, encoded + i, size);
                n += size;
            }
            i += size;
        }
        if (line + 1 < n_lines && between_n > 0) memcpy(stream + n, between, between_n), n += between_n;
    }
    if (tail_n > 0) memcpy(stream + n, tail, tail_n), n += tail_n;
    stream[n] = 0;
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (alloc == NULL) return;
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (alloc->data == NULL) {
        HSD_SisLib_Free(alloc);
        return;
    }
    memcpy(alloc->data, stream, n + 1);
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data != NULL) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    HSD_SisLib_803A6368(text, 0);   /* the drawing state a premade line starts from (colour, scale, ...) */
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}

static void hide_all_but(HSD_JObj* j, int* index, int keep)
{
    for (; j != NULL; j = j->next) {
        if ((*index)++ != keep) HSD_JObjSetFlags(j, JOBJ_HIDDEN);
        if (!(j->flags & JOBJ_INSTANCE)) hide_all_but(j->child, index, keep);
    }
}

/* The hand over the bar flashes as over the rules bar (updateCursorDisplay's hover colour), after
 * the cursor's own think set its colour for the frame. */
static void flash_think(HSD_GObj* gobj)
{
    Cursor* c = hand();
    HSD_JObj* color = NULL;
    (void) gobj;
    if (oo.open || mu_slippi_css_scene_request() != 0 || !over_bar(c)) return;
    lb_80011E24((HSD_JObj*) c->gobj->hsd_obj, &color, 3, -1);
    if (color == NULL) return;
    HSD_ForeachAnim(color, JOBJ_TYPE, TOBJ_MASK, HSD_AObjReqAnim, AOBJ_ARG_AF,
                    (f32) ((oo.counter & 3) + (unsigned) mu_slippi_css_port() * 4));
    HSD_JObjAnimAll(color);
    HSD_ForeachAnim(color, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV, 0, 0);
}

/* The bar's text box (the VS select's rules line). The bar has the top of the screen to itself: the
 * opponent's name and picks ("Found:", "Playing:") are in the status block at the bottom right
 * (shim/mu_slippi_css.c), so the words stay centered whoever is connected. */
#define BAR_X -12.0f
#define BAR_BOX_W 450.0f
#define BAR_SIZE 0.07f

/* The bar's words: "Options", or what every player agreed on (the host's ITEMS_ON: peers known, every
 * game has the rule, every player has it on): "Options: Items on", "Options: Squad Strike on" or both. */
static void bar_words(void)
{
    static const char* const words[4][1] = {
        {"Options"}, {"Options: Items on"}, {"Options: Squad Strike on"}, {"Options: Items, Squad Strike on"}};
    const int on = (oo.shown[OPT_ITEMS] && oo.extra[OPT_ITEMS].status == ITEMS_ON ? 1 : 0) |
                   (oo.shown[OPT_SQUAD] && oo.extra[OPT_SQUAD].status == ITEMS_ON ? 2 : 0);
    if (oo.bar_text == NULL || on == oo.bar_items) return;
    oo.bar_items = on;
    set_stream(oo.bar_text, NULL, 0, words[on], 1, NULL, 0, NULL, 0);
    OSReport("[options] bar: %s\n", words[on][0]);
}

static void build_bar(void)
{
    StaticModelDesc* model;
    HSD_JObj* root;
    HSD_JObj* bar = NULL;
    HSD_DObj* dobj;
    HSD_Text* text;
    int i, any = 0, index = 0;
    oo.built = -1;
    for (i = 0; i < OPTION_COUNT; ++i) {
        oo.value[i] = host_option(i, 0xFF, &oo.shown[i], &oo.extra[i]);
        any |= oo.shown[i] && oo.value[i] >= 0;
    }
    if (!any) {
        OSReport("[options] the host offers no online options (Vanilla, a replay, or an older host); no bar\n");
        return;
    }
    model = vs_menu_model();
    if (model == NULL || DISC_NULL(model->joint)) {
        OSReport("[options] the select's VS menu model was not found; no bar\n");
        return;
    }
    root = HSD_JObjLoadJoint(DP(model->joint));
    HSD_JObjAddAnimAll(root, DP(model->animjoint), DP(model->matanim_joint), DP(model->shapeanim_joint));
    HSD_JObjReqAnimAll(root, 0.0f);
    HSD_JObjAnimAll(root);
    HSD_ForeachAnim(root, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV, 0, 0);
    hide_all_but(root, &index, 1);
    lb_80011E24(root, &bar, 1, -1);
    /* joint 1 holds the bar's bracket (its second DObj) between the VS select's "VS" header and
     * the shade under its top row; only the bracket stays */
    for (i = 0, dobj = bar != NULL ? bar->u.dobj : NULL; dobj != NULL; dobj = dobj->next, ++i)
        if (i != 1) dobj->flags |= DOBJ_HIDDEN;
    oo.bar = GObj_Create(4, 5, 0x80);
    HSD_GObjObject_80390A70(oo.bar, HSD_GObj_JObjKind, root);
    GObj_SetupGXLink(oo.bar, HSD_GObj_JObjCallback, 1, 0x80);

    /* The words: the rules bar's own text object (mnCharSel_802640A0: place, box, size, centred,
     * fitted), its line plain glyphs as the VS select's rules line is (SdSlChr's 0x4A). */
    text = HSD_SisLib_803A5ACC(0, 0, BAR_X, -23.3f, 0.0f, BAR_BOX_W, 32.0f);
    text->default_alignment = 1;
    text->default_kerning = 1;
    text->default_fitting = 1;
    text->font_size.x = BAR_SIZE;
    text->font_size.y = BAR_SIZE;
    {
        static const char* const words[1] = {"Options"};
        set_stream(text, NULL, 0, words, 1, NULL, 0, NULL, 0);
    }
    oo.bar_text = text;
    oo.bar_items = 0;
    bar_words();

    oo.flash = GObj_Create(4, 5, 0x80);
    HSD_GObj_SetupProc(oo.flash, flash_think, 4);
    HSD_GObj_SetupProc(GObj_Create(4, 5, 0x80), restore_think, 0x18);
    oo.built = 1;
    OSReport("[options] bar on the online select; %s %s; %s %s (status %d); %s %s (status %d)\n",
             options[OPT_SKINS].label, oo.value[OPT_SKINS] ? "on" : "off", options[OPT_ITEMS].label,
             !oo.shown[OPT_ITEMS] ? "not offered" : oo.value[OPT_ITEMS] ? "on" : "off", oo.extra[OPT_ITEMS].status,
             options[OPT_SQUAD].label, !oo.shown[OPT_SQUAD] ? "not offered" : oo.value[OPT_SQUAD] ? "on" : "off",
             oo.extra[OPT_SQUAD].status);
}

/* ---- the menu ---- */

static void cam_a_render(HSD_GObj* gobj, intptr_t pass)
{
    (void) pass;
    if (HSD_CObjSetCurrent(gobj->hsd_obj)) {
        if (oo.fog != NULL) {
            HSD_SetEraseColor(oo.fog->color.r, oo.fog->color.g, oo.fog->color.b, oo.fog->color.a);
        } else {
            HSD_SetEraseColor(0, 0, 0, 0xFF);
        }
        HSD_CObjEraseScreen(gobj->hsd_obj, 1, 0, 1);
        HSD_GObj_80390ED0(gobj, 7);
        HSD_CObjEndCurrent();
    }
}

static AnimLoopSettings* value_anim(int value, int left)
{
    /* mn_80232458 for a two-value row: right goes to the value's own frames, left to its others */
    return left ? &value_left[1 - value] : &value_right[value == 0 ? 1 : 0];
}

static void set_row(int i, int selected, int first)
{
    HSD_JObj** parts = oo.parts[i];
    if (first || selected) {
        HSD_JObjReqAnim(parts[7], label_frames[selected]);
        HSD_JObjAnim(parts[7]);
    }
    if (selected) {
        HSD_JObjClearFlagsAll(parts[16], JOBJ_HIDDEN);
        HSD_JObjReqAnimAll(parts[16], row_glow.start_frame);
        HSD_JObjClearFlagsAll(parts[13], JOBJ_HIDDEN);
        HSD_JObjReqAnimAll(parts[13], row_arrows.start_frame);
    } else {
        HSD_JObjSetFlagsAll(parts[16], JOBJ_HIDDEN);
        HSD_JObjSetFlagsAll(parts[13], JOBJ_HIDDEN);
    }
    HSD_JObjReqAnimAll(parts[2], row_body[selected].start_frame);
    HSD_JObjAnimAll(parts[2]);
    if (oo.labels <= 0) HSD_JObjSetFlagsAll(parts[7], JOBJ_HIDDEN);
    row_pictures(i);
}

/* The menus' two-line description in its own form (SdMenu 0x9F, the Data menu's "View photo album
 * of pictures / taken in camera mode."): #AAAAAA, 0.7 wide and 0.5 high, raised half a line, each
 * line opened by the same origin command, in the menus' one-line box. */
static const u8 desc_head[] = {0x12, 0x0C, 0xAA, 0xAA, 0xAA, 0x0E, 0x00, 0xB3, 0x00, 0x80, 0x16,
                               0x07, 0x00, 0x00, 0xFF, 0xEC, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00, 0x18};
static const u8 desc_break[] = {0x19, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00, 0x18};
static const u8 desc_tail[] = {0x19, 0x0F, 0x0D};

/* Items or Squad Strike against a game without it: the row is dimmed, Off, and left and right do
 * nothing (the error sound). */
static int greyed(int i)
{
    return agreed(i) && oo.extra[i].status == ITEMS_UNSUPPORTED;
}

/* Squad Strike's row: what the match will be and whose setting or game keeps squads off. */
static int squad_lines(const Extra* e, const char* lines[2], char* first, char* second, size_t size)
{
    const int who = e->blocker + 1;
    switch (e->status) {
    case ITEMS_ON:
        lines[0] = "Every player has Squad Strike on:";
        lines[1] = "Z picks up to four fighters.";
        break;
    case ITEMS_OFF_MINE:
        lines[0] = "No squads: your Squad Strike is off.";
        lines[1] = "On when every player turns it on.";
        break;
    case ITEMS_OFF_THEIRS:
        snprintf(first, size, "No squads: P%d has Squad Strike off.", who);
        lines[0] = first;
        lines[1] = "On when every player turns it on.";
        break;
    case ITEMS_UNSUPPORTED:
        snprintf(first, size, "No squads: P%d's game has no Squad", who);
        snprintf(second, size, "Strike option, so squads stay off.");
        lines[0] = first;
        lines[1] = second;
        return 2;
    default:
        break;
    }
    if (e->locked) lines[1] = "Unlock your character to change it.";
    return 2;
}

/* The row's description: an option's own two lines, or for Items what the match will use and whose
 * setting keeps items off (letters, digits, spaces and . , ' : only: the game draws nothing else). */
static int row_lines(int i, const char* lines[2])
{
    static char first[48], second[48];
    const Extra* e = &oo.extra[i];
    const int who = e->blocker + 1;
    lines[0] = options[i].desc[0];
    lines[1] = options[i].desc[1];
    if (i == OPT_SQUAD) return squad_lines(e, lines, first, second, sizeof first);
    if (i != OPT_ITEMS) return 2;
    switch (e->status) {
    case ITEMS_ON:
        lines[0] = "Every player has Items on:";
        lines[1] = "this match has items at Medium.";
        break;
    case ITEMS_OFF_MINE:
        lines[0] = "No items: your Items is off.";
        lines[1] = "On when every player turns them on.";
        break;
    case ITEMS_OFF_THEIRS:
        snprintf(first, sizeof first, "No items: P%d has Items off.", who);
        lines[0] = first;
        lines[1] = "On when every player turns them on.";
        break;
    case ITEMS_UNSUPPORTED:
        snprintf(first, sizeof first, "No items: P%d's game has no Items", who);
        snprintf(second, sizeof second, "option, so items stay off.");
        lines[0] = first;
        lines[1] = second;
        return 2;
    default:
        break;
    }
    if (e->locked) lines[1] = "Unlock your character to change it.";
    return 2;
}

static void describe(int i)
{
    HSD_Text* text;
    const char* lines[2];
    if (oo.desc != NULL) {
        HSD_SisLib_803A5CC4(oo.desc);
        oo.desc = NULL;
    }
    /* where and how big the menus put their description (mnmain.c, the Options menu's box) */
    text = HSD_SisLib_803A5ACC(0, oo.canvas, -9.5f, 9.1f, 17.0f, 364.68332f, 38.38772f);
    text->font_size.x = 0.0521f;
    text->font_size.y = 0.0521f;
    set_stream(text, desc_head, sizeof desc_head, lines, row_lines(i, lines), desc_break, sizeof desc_break,
               desc_tail, sizeof desc_tail);
    oo.desc = text;
}

/* A row's own label (every row's animation shows the first option's) and, for a greyed row, its
 * label and value dimmed. After the frame's animations. */
static void row_pictures(int i)
{
    if (oo.labels <= 0) return;
    if (oo.parts[i][7] != NULL)
        swap_pictures(oo.parts[i][7], LW, LH, greyed(i) ? &label_dim_desc[i] : &label_desc[i], NULL, 0);
    if (oo.values[i] != NULL && strip_desc != NULL)   /* only the ON/OFF strip itself */
        swap_pictures(oo.values[i], SW, LH, greyed(i) ? &strip_dim_desc : strip_desc, strip_desc, 0);
}

/* The value strip straight to a value (no slide), as the menu opens with it. */
static void show_value(int i)
{
    if (oo.values[i] == NULL) return;
    HSD_JObjReqAnimAll(oo.values[i], (oo.value[i] ? &value_right[0] : &value_right[1])->end_frame);
    HSD_JObjAnimAll(oo.values[i]);
}

static HSD_JObj* load_model(const StaticModelSyms* m, f32 frame)
{
    HSD_JObj* jobj = HSD_JObjLoadJoint(m->joint);
    HSD_JObjAddAnimAll(jobj, m->animjoint, m->matanim_joint, m->shapeanim_joint);
    HSD_JObjReqAnimAll(jobj, frame);
    HSD_JObjAnimAll(jobj);
    return jobj;
}

static void open_menu(void)
{
    HSD_GObj* g;
    HSD_JObj* jobj;
    HSD_JObj* slots[10];
    HSD_JObj* part;
    HSD_LObj* point;
    u16 map[ROW_PARTS];
    int i, visible = 0;

    if (load_menus() <= 0) return;
    for (i = 0; i < OPTION_COUNT; ++i) oo.value[i] = host_option(i, 0xFF, &oo.shown[i], &oo.extra[i]);
    for (i = 0; i < OPTION_COUNT; ++i) visible += oo.shown[i] && oo.value[i] >= 0;
    if (visible == 0) return;
    for (i = 0; i < ROW_PARTS; ++i) map[i] = (u16) i;

    /* lights (mn_8022C304), the menu's colour at once */
    g = oo.light_gobj = GObj_Create(3, 4, 0x80);
    GObj_SetupGXLink(g, HSD_GObj_LObjCallback, L_LIGHTS, 0x80);
    oo.lobj = lb_80011AC4(oo.lights);
    HSD_GObjObject_80390A70(g, HSD_GObj_LightKind, oo.lobj);
    for (point = oo.lobj; point != NULL && !(point->flags & LOBJ_POINT); point = point->next) {
    }
    if (point != NULL) {
        const GXColor* color = mn_8022BFBC(MENU_LIGHT);
        point->color.r = color->r;
        point->color.g = color->g;
        point->color.b = color->b;
    }
    /* fog (mn_8022BCF8) */
    g = oo.fog_gobj = GObj_Create(1, 2, 0x80);
    oo.fog = HSD_FogLoadDesc(oo.fogdesc);
    HSD_GObjObject_80390A70(g, HSD_GObj_FogKind, oo.fog);
    GObj_SetupGXLink(g, HSD_GObj_FogCallback, L_FOG, 0x80);
    /* the two cameras (mn_8022BE34, mn_8022BEDC): the first clears the screen to the fog's colour */
    g = oo.cam_a = GObj_Create(2, 3, 0x80);
    HSD_GObjObject_80390A70(g, HSD_GObj_CameraKind, HSD_CObjLoadDesc(oo.cam));
    GObj_SetupGXLinkMax(g, cam_a_render, CAM_A_PRIO);
    g->gxlink_prios = (1ull << L_LIGHTS) | (1ull << L_FOG) | (1ull << L_BACK) | (1ull << L_PANEL) | (1ull << L_ROWS);
    g = oo.cam_b = GObj_Create(2, 3, 0x80);
    HSD_GObjObject_80390A70(g, HSD_GObj_CameraKind, HSD_CObjLoadDesc(oo.cam));
    GObj_SetupGXLinkMax(g, HSD_GObj_803910D8, CAM_B_PRIO);
    g->gxlink_prios = 1ull << L_DESC;
    if (oo.canvas < 0) oo.canvas = HSD_SisLib_803A611C(0, g, 7, 8, 0x80, L_DESC, 0x80, 0);
    /* background (mn_80229B2C) */
    g = oo.back_gobj = GObj_Create(4, 5, 0x80);
    HSD_GObjObject_80390A70(g, HSD_GObj_JObjKind, load_model(&oo.back, 0.0f));
    GObj_SetupGXLink(g, HSD_GObj_JObjCallback, L_BACK, 0x80);
    /* panel (mn_80229DC0), the heading at rest */
    g = oo.panel_gobj = GObj_Create(5, 6, 0x80);
    jobj = HSD_JObjLoadJoint(oo.panel.joint);
    HSD_GObjObject_80390A70(g, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(g, HSD_GObj_JObjCallback, L_PANEL, 0x80);
    HSD_JObjAddAnimAll(jobj, oo.panel.animjoint, oo.panel.matanim_joint, oo.panel.shapeanim_joint);
    part = NULL;
    lb_80011E24(jobj, &part, 4, -1);
    if (part != NULL) {
        HSD_JObjReqAnimAll(part, panel_loop.start_frame);
        HSD_JObjAnimAll(part);
    }
    part = NULL;
    lb_80011E24(jobj, &part, 0x29, -1);
    if (part != NULL) {
        HSD_JObjReqAnimAll(part, heading_idle.start_frame);
        HSD_JObjAnimAll(part);
    }
    /* the rows' container (mn_80233218), coming in */
    g = oo.con_gobj = GObj_Create(6, 7, 0x80);
    jobj = load_model(&oo.con, 0.0f);
    HSD_GObjObject_80390A70(g, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(g, HSD_GObj_JObjCallback, L_ROWS, 0x80);
    for (i = 0; i < 10; ++i) {
        slots[i] = NULL;
        lb_80011E24(jobj, &slots[i], i, -1);
    }
    oo.con_enter = slots[2];
    if (oo.con_enter != NULL) {
        HSD_JObjReqAnim(oo.con_enter, con_entry.start_frame);
        HSD_JObjAnim(oo.con_enter);
        oo.entering = 1;
    }
    {
        int slot = 0;
        if (oo.row >= OPTION_COUNT || !oo.shown[oo.row]) oo.row = 0;
        for (i = 0; i < OPTION_COUNT; ++i) {
            HSD_JObj* row;
            HSD_JObj* value;
            if (!oo.shown[i] || oo.value[i] < 0) continue;
            row = slots[3 + slot++];
            HSD_JObjReqAnim(row, (f32) visible);
            HSD_JObjAnim(row);
            jobj = load_model(&oo.cursor, 0.0f);
            lb_8001204C(jobj, oo.parts[i], map, ROW_PARTS);
            HSD_JObjSetFlagsAll(oo.parts[i][8], JOBJ_HIDDEN);   /* no sub-menu arrow */
            set_row(i, i == oo.row, 1);
            HSD_JObjAddChild(row, jobj);
            value = HSD_JObjLoadJoint(oo.onoff.joint);
            HSD_JObjAddAnimAll(value, oo.onoff.animjoint, oo.onoff.matanim_joint, oo.onoff.shapeanim_joint);
            HSD_JObjReqAnimAll(value, value_anim(oo.value[i], 0)->end_frame);
            HSD_JObjReqAnimAll(value, (oo.value[i] ? &value_right[0] : &value_right[1])->end_frame);
            HSD_JObjAnimAll(value);
            HSD_JObjAddChild(row, value);
            oo.values[i] = value;
            row_pictures(i);
        }
    }
    describe(oo.row);
    oo.open = 1;
    oo.cooldown = OPEN_COOLDOWN;
    lbAudioAx_80024030(1);   /* sfxForward, as the rules bar */
    OSReport("[options] menu open\n");
}

static void close_menu(int sound)
{
    if (oo.desc != NULL) HSD_SisLib_803A5CC4(oo.desc);
    if (oo.con_gobj != NULL) HSD_GObjFree(oo.con_gobj);
    if (oo.panel_gobj != NULL) HSD_GObjFree(oo.panel_gobj);
    if (oo.back_gobj != NULL) HSD_GObjFree(oo.back_gobj);
    if (oo.cam_b != NULL) HSD_GObjFree(oo.cam_b);
    if (oo.cam_a != NULL) HSD_GObjFree(oo.cam_a);
    if (oo.fog_gobj != NULL) HSD_GObjFree(oo.fog_gobj);
    if (oo.light_gobj != NULL) HSD_GObjFree(oo.light_gobj);
    oo.desc = NULL;
    oo.con_gobj = oo.panel_gobj = oo.back_gobj = oo.cam_b = oo.cam_a = oo.fog_gobj = oo.light_gobj = NULL;
    oo.fog = NULL;
    oo.lobj = NULL;
    oo.con_enter = NULL;
    memset(oo.parts, 0, sizeof oo.parts);
    memset(oo.values, 0, sizeof oo.values);
    oo.open = 0;
    oo.swallow = 1;
    if (sound >= 0) lbAudioAx_80024030(sound);
    OSReport("[options] menu closed\n");
}

static int next_row(int from, int step)
{
    int i, row = from;
    for (i = 0; i < OPTION_COUNT; ++i) {
        row = (row + step + OPTION_COUNT) % OPTION_COUNT;
        if (oo.shown[row] && oo.value[row] >= 0) return row;
    }
    return from;
}

static void menu_input(int port)
{
    const u64 down = gm_GetButtonsTriggered((u8) port);
    const u64 repeat = gm_801A36C0((u8) port);
    const Option* o;
    int was, now;
    if (oo.cooldown > 0) {
        --oo.cooldown;
        return;
    }
    if (down & PAD_BUTTON_START) {
        close_menu(1);   /* sfxForward: the rules screen's START takes its rules back to the select */
        return;
    }
    if (down & PAD_CANCEL) {
        close_menu(0);   /* sfxBack */
        return;
    }
    if (repeat & (PAD_ANY_UP | PAD_ANY_DOWN)) {
        const int to = next_row(oo.row, (repeat & PAD_ANY_UP) ? -1 : 1);
        lbAudioAx_80024030(2);   /* sfxMove, even with one row, as the rules screen wraps */
        if (to != oo.row) {
            set_row(oo.row, 0, 0);
            oo.row = to;
            set_row(oo.row, 1, 0);
            describe(oo.row);
        }
        return;
    }
    if (!(repeat & (PAD_ANY_LEFT | PAD_ANY_RIGHT))) return;
    o = &options[oo.row];
    was = oo.value[oo.row];
    if (greyed(oo.row) || (agreed(oo.row) && oo.extra[oo.row].locked)) {
        /* a game without the rule, or locked in: the choice cannot change (the host would refuse) */
        lbAudioAx_80024030(3);   /* the error sound */
        OSReport("[options] %s: no change (%s)\n", o->label,
                 greyed(oo.row) ? "a player's game does not have this option" : "locked in");
        return;
    }
    now = (repeat & PAD_ANY_LEFT) ? (was + o->count - 1) % o->count : (was + 1) % o->count;
    lbAudioAx_80024030(2);   /* sfxMove */
    now = host_option(oo.row, now, NULL, &oo.extra[oo.row]);
    if (now < 0 || now == was) {
        if (agreed(oo.row)) describe(oo.row);   /* the host's reason (locked in since, a new peer) */
        return;
    }
    oo.value[oo.row] = now;
    OSReport("[options] %s: %s\n", o->label, now ? "On" : "Off");
    if (oo.values[oo.row] != NULL) {
        HSD_JObjReqAnimAll(oo.values[oo.row], value_anim(now, (repeat & PAD_ANY_LEFT) != 0)->start_frame);
        HSD_JObjAnimAll(oo.values[oo.row]);
    }
    if (agreed(oo.row)) describe(oo.row);
}

/* The host's state of an agreed option now and then (a peer joins, changes theirs, locks in): the
 * row's value, dimming and description, and the bar's words. */
static void bar_words(void);
static void agreed_refresh(int i)
{
    Extra was;
    int value, shown;
    was = oo.extra[i];
    value = host_option(i, 0xFF, &shown, &oo.extra[i]);
    if (value < 0) return;
    oo.shown[i] = shown;
    bar_words();
    if (!oo.open || oo.values[i] == NULL) {
        oo.value[i] = value;
        return;
    }
    if (value != oo.value[i]) {
        oo.value[i] = value;
        show_value(i);
    }
    if (memcmp(&was, &oo.extra[i], sizeof was) != 0) {
        OSReport("[options] %s: status %d, P%d, %s\n", options[i].label, oo.extra[i].status, oo.extra[i].blocker + 1,
                 oo.extra[i].locked ? "locked in" : "not locked");
        if (oo.row == i) describe(i);
    }
}

/* Items and Squad Strike, every 20 frames (and at once when asked). */
static void items_refresh(int now)
{
    if (!now && oo.counter % 20 != 0) return;
    agreed_refresh(OPT_ITEMS);
    agreed_refresh(OPT_SQUAD);
}

/* Each frame the menu is up: the rules screen's own animations. */
static void menu_animate(void)
{
    int i;
    if (oo.lobj != NULL) HSD_LObjAnimAll(oo.lobj);
    if (oo.back_gobj != NULL) HSD_JObjAnimAll((HSD_JObj*) oo.back_gobj->hsd_obj);
    if (oo.panel_gobj != NULL) {
        HSD_JObj* root = (HSD_JObj*) oo.panel_gobj->hsd_obj;
        HSD_JObj* part = NULL;
        lb_80011E24(root, &part, 0x29, -1);
        if (part != NULL) mn_8022ED6C(part, &heading_idle);
        part = NULL;
        lb_80011E24(root, &part, 4, -1);
        if (part != NULL) mn_8022ED6C(part, &panel_loop);
    }
    if (oo.entering && oo.con_enter != NULL) {
        if (mn_8022F298(oo.con_enter) >= con_entry.end_frame) oo.entering = 0;
        HSD_JObjAnim(oo.con_enter);
    }
    for (i = 0; i < OPTION_COUNT; ++i) {
        HSD_JObj** parts = oo.parts[i];
        if (parts[0] == NULL) continue;
        if (i == oo.row) {
            mn_8022ED6C(parts[16], &row_glow);
            mn_8022ED6C(parts[13], &row_arrows);
        }
        mn_8022ED6C(parts[2], &row_body[i == oo.row]);
        if (oo.values[i] != NULL) {   /* mn_80232660: run the value's segment to its end */
            const f32 frame = mn_8022F298(oo.values[i]);
            AnimLoopSettings* seg = NULL;
            int k;
            for (k = 0; k < 3 && seg == NULL; ++k) {
                if (value_left[k].start_frame <= frame && frame <= value_left[k].end_frame) seg = &value_left[k];
                else if (value_right[k].start_frame <= frame && frame <= value_right[k].end_frame) seg = &value_right[k];
            }
            if (seg != NULL) mn_8022ED6C(oo.values[i], seg);
        }
        row_pictures(i);
    }
}

/* The select sees no input from any controller while the menu is up (and after it, until the
 * buttons that closed it are let go): its cursor, coin, lock-in, chat and camera stay as they were.
 * The pads are emptied for this frame's readers and put back at the frame's end (restore_think),
 * so the pad library's next frame (presses, held buttons, repeats) goes on from the real ones. */
static HSD_PadStatus saved_pads[2][4];
static int pads_saved;

static void swallow(void)
{
    int i;
    if (!pads_saved) {
        memcpy(saved_pads[0], HSD_PadCopyStatus, sizeof saved_pads[0]);
        memcpy(saved_pads[1], HSD_PadGameStatus, sizeof saved_pads[1]);
        pads_saved = 1;
    }
    for (i = 0; i < 4; ++i) {
        HSD_PadStatus* pads[2];
        int k;
        pads[0] = &HSD_PadCopyStatus[i];
        pads[1] = &HSD_PadGameStatus[i];
        for (k = 0; k < 2; ++k) {
            const s8 err = pads[k]->err;
            memset(pads[k], 0, sizeof *pads[k]);
            pads[k]->err = err;
        }
    }
}

/* The last proc of the frame (after the cursor, the select's menus and its camera). */
static void restore_think(HSD_GObj* gobj)
{
    (void) gobj;
    if (!pads_saved) return;
    memcpy(HSD_PadCopyStatus, saved_pads[0], sizeof saved_pads[0]);
    memcpy(HSD_PadGameStatus, saved_pads[1], sizeof saved_pads[1]);
    pads_saved = 0;
}

/* ---- the select's calls (shim/mu_slippi_css.c) ---- */

/* A new online select (its text init): everything of the last one went with its scene. */
void mu_online_options_scene(void)
{
    memset(&oo, 0, sizeof oo);
    pads_saved = 0;   /* a select left with the menu up: its pads went with it */
    oo.scene = 1;
    oo.canvas = -1;
}

int mu_online_options_busy(void)
{
    return oo.scene && (oo.open || oo.swallow);
}

/* mnCharSel_Scene_OnFrame's first call (mu_css_frame), online selects only: before the cursor and
 * the select's own input. */
void mu_online_options_frame(void)
{
    const int port = mu_slippi_css_port() & 3;
    if (!oo.scene || !mu_slippi_css_online()) return;
    ++oo.counter;
    if (oo.built == 0) build_bar();
    if (oo.built > 0) items_refresh(0);
    if (oo.open) {
        if (mu_slippi_css_scene_request() != 0) {   /* the select is leaving under the menu */
            close_menu(-1);
            return;
        }
        menu_input(port);
        if (oo.open) menu_animate();
        swallow();
        return;
    }
    if (oo.swallow) {
        if (HSD_PadCopyStatus[port].button != 0) {
            swallow();
            return;
        }
        oo.swallow = 0;
    }
    if (oo.built > 0 && oo.loaded >= 0 && mu_slippi_css_scene_request() == 0 && !mu_slippi_css_chat_open() &&
        over_bar(hand()) && (HSD_PadCopyStatus[port].trigger & HSD_PAD_A)) {
        open_menu();
        if (oo.open) swallow();
    }
}
