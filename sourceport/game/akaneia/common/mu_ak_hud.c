/* The match HUD of the added fighters: stock icons and the emblem behind the damage number.
 *
 * The retail HUD file animates both textures over the retail characters only. An m-ex disc's HUD
 * file (IfAll) carries two more symbols: "Stc_icns", a texture animation with one stock icon per
 * fighter and costume, and "Eblm_matanim_joint", one with every emblem. m-ex puts those
 * animations on the HUD's joints and picks the frame from the fighter's ids:
 *
 *     stock icon frame = reserved frames + costume * stride + m-ex internal fighter id
 *     emblem frame     = the insignia id MxDt gives the external fighter id
 *
 * On an m-ex disc every character's stock row takes the m-ex icon animation, as m-ex does: the
 * retail animation has no icon for a costume m-ex added to a retail character (ACE 2.0.1 adds two
 * to each, 50 icons) and drew the icon of the retail costume it is built on instead. The emblem
 * animation goes on an added character's damage display only; a retail character keeps the
 * retail emblem frame. */
#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/types.h>
#include <melee/if/ifall.h>
#include <melee/it/forward.h>
#include <melee/it/types.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lbspdisplay.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/jobj.h>

#include <stddef.h>

#include "mu_disc.h"
#include "../mu_ak_fighter.h"

#ifdef MU_AKANEIA_FIGHTERS

/* "Stc_icns" of an m-ex HUD file. */
typedef struct AkStockIcons {
    u16 reserved;   /* frames before the fighters' icons */
    u16 stride;     /* frames per costume row */
    DISC_PTR(HSD_MatAnimJoint) matanim;
} DISC_STRUCT AkStockIcons;

/* The frame layout of the stock icon animation last put on a HUD (kept as numbers: the HUD file
 * itself goes away with the match). stride 0 = none seen. */
static int ak_stock_reserved, ak_stock_stride;

/* Per HUD player: its stock row took the m-ex icon animation (mu_ak_hud_stock_icons). */
#define AK_HUD_PLAYERS 6
static u8 ak_stock_row[AK_HUD_PLAYERS];

/* The fighter kind of each retail character kind, in character kind order. */
static const s8 ak_retail_ftkind[CKind_Playable_Count] = {
    Ft_Kind_Captain, Ft_Kind_Donkey, Ft_Kind_Fox, Ft_Kind_GameWatch, Ft_Kind_Kirby, Ft_Kind_Koopa,
    Ft_Kind_Link, Ft_Kind_Luigi, Ft_Kind_Mario, Ft_Kind_Mars, Ft_Kind_Mewtwo, Ft_Kind_Ness,
    Ft_Kind_Peach, Ft_Kind_Pikachu, Ft_Kind_Popo, Ft_Kind_Purin, Ft_Kind_Samus, Ft_Kind_Yoshi,
    Ft_Kind_Zelda, Ft_Kind_Seak, Ft_Kind_Falco, Ft_Kind_CLink, Ft_Kind_DrMario, Ft_Kind_Emblem,
    Ft_Kind_Pichu, Ft_Kind_Ganon,
};

/* The m-ex stock icon frame of native fighter kind `ftkind` in costume `costume`: reserved +
 * costume * stride + the m-ex internal id, and the fixed frames m-ex gives the special fighters.
 * Returns 0 when there is none (no icon layout seen, or a kind m-ex does not know). */
static int ak_mex_stock_frame(int ftkind, int costume, float* frame)
{
    int internal;
    switch (ftkind) {
    case Ft_Kind_MasterH: *frame = 3.0f; return 1;
    case Ft_Kind_CrezyH: *frame = 2.0f; return 1;
    case Ft_Kind_Boy:
    case Ft_Kind_Girl: *frame = 1.0f; return 1;
    case Ft_Kind_GKoops: *frame = 5.0f; return 1;
    case Ft_Kind_Sandbag: *frame = 6.0f; return 1;
    default: break;
    }
    internal = mu_ak_mex_internal(ftkind);
    if (internal < 0 || ak_stock_stride == 0 || costume < 0) {
        return 0;
    }
    *frame = (float) (ak_stock_reserved + costume * ak_stock_stride + internal);
    return 1;
}

/* The fighter kind of a character kind, -1 for none. */
static int ak_ftkind_of_ckind(int ckind)
{
    if (ckind >= 0 && ckind < CKind_Playable_Count) {
        return ak_retail_ftkind[ckind];
    }
    switch (ckind) {
    case CKind_MasterH: return Ft_Kind_MasterH;
    case CKind_Boy: return Ft_Kind_Boy;
    case CKind_Girl: return Ft_Kind_Girl;
    case CKind_GKoops: return Ft_Kind_GKoops;
    case CKind_CrezyH: return Ft_Kind_CrezyH;
    case ChKind_Sandbag: return Ft_Kind_Sandbag;
    case ChKind_Popo: return Ft_Kind_Popo;
    default: break;
    }
    if (MU_AK_CKIND(ckind)) {
        return MU_AK_KIND_BASE + (ckind - MU_AK_CKIND_BASE);
    }
    return -1;
}

static void* hud_symbol(const char* name)
{
    HSD_Archive** archive = ifAll_GetArchive();
    return archive != NULL && *archive != NULL ? HSD_ArchiveGetPublicAddress(*archive, name) : NULL;
}

/* The m-ex internal fighter id behind an added character kind, -1 when it has none. */
static int internal_of_ckind(int ckind)
{
    return MU_AK_CKIND(ckind) ?
               mu_ak_mex_internal(MU_AK_KIND_BASE + (ckind - MU_AK_CKIND_BASE)) : -1;
}

/* ifStock_802F98E8: `root` is the stock row a match HUD just created for `player`. On an m-ex
 * disc the row's seven icon joints (joints 1 to 7) take the m-ex icon animation, for every
 * character; gm_80168BF8 then gives the row its frame (mu_ak_hud_player_stock_frame). */
void mu_ak_hud_stock_icons(HSD_JObj* root, int player)
{
    AkStockIcons* icons;
    HSD_MatAnimJoint* matanim;
    int i;
    if (player < 0 || player >= AK_HUD_PLAYERS) {
        return;
    }
    ak_stock_row[player] = 0;
    if (!mu_mex_fighters_active()) {
        return;
    }
    icons = hud_symbol("Stc_icns");
    matanim = icons != NULL ? DP(icons->matanim) : NULL;
    if (matanim == NULL || icons->stride == 0) {
        OSReport("[ak] the HUD file has no Stc_icns: player %d keeps the retail stock icons\n",
                 player);
        return;
    }
    ak_stock_reserved = icons->reserved;
    ak_stock_stride = icons->stride;
    ak_stock_row[player] = 1;
    for (i = 0; i < 7; i++) {
        HSD_JObj* icon = NULL;
        lb_80011E24(root, &icon, i + 1, -1);
        if (icon != NULL) {
            HSD_JObjAddAnimAll(icon, NULL, matanim, NULL);
        }
    }
}

/* fn_802F94E0, as `player`'s stock row is drawn: a fighter installed on a disc with no m-ex table
 * (shim/mu_mex_roster.c) shows its own stock icon, a picture of its folder, on each of the row's
 * seven icon joints. The retail icon animation picks its own image again whenever it runs, so the
 * picture is put back at every draw. Nothing changes for any other fighter. */
void mu_ak_hud_roster_stock(HSD_JObj* root, int player)
{
    static HSD_ImageDesc image[AK_HUD_PLAYERS];
    const void* pixels;
    int ckind, i;
    if (root == NULL || player < 0 || player >= AK_HUD_PLAYERS || !mu_mex_roster_built()) {
        return;
    }
    ckind = Player_GetPlayerCharacter(player);
    if (!MU_AK_CKIND(ckind)) {
        return;
    }
    pixels = mu_mex_roster_stock(mu_ak_mex_external(ckind) - CKind_Playable_Count, (int) Player_GetCostumeId(player));
    if (pixels == NULL) {
        return;
    }
    for (i = 0; i < 7; i++) {
        HSD_JObj* icon = NULL;
        HSD_TObj* tobj;
        lb_80011E24(root, &icon, i + 1, -1);
        if (icon == NULL || (icon->flags & (JOBJ_PTCL | JOBJ_SPLINE)) || icon->u.dobj == NULL ||
            icon->u.dobj->mobj == NULL)
        {
            continue;
        }
        tobj = icon->u.dobj->mobj->tobj;
        if (tobj == NULL || tobj->imagedesc == NULL) {
            continue;
        }
        if (tobj->imagedesc != &image[player]) {
            image[player] = *tobj->imagedesc;
        }
        image[player].width = 24;
        image[player].height = 24;
        image[player].format = GX_TF_RGBA8;
        image[player].mipmap = 0;
        DISC_SET(image[player].image_ptr, (void*) pixels);
        tobj->imagedesc = &image[player];
    }
}

/* gm_80168B34: the stock icon frame of an added character kind. */
float mu_ak_hud_stock_frame(int ckind, int costume)
{
    const int internal = internal_of_ckind(ckind);
    if (internal < 0 || ak_stock_stride == 0) {
        return 0.0f;
    }
    return (float) (ak_stock_reserved + costume * ak_stock_stride + internal);
}

/* gm_80168BF8: the frame of `player`'s stock row when it took the m-ex icon animation: the
 * fighter it plays now (Sheik's icon while Zelda is Sheik, as the retail frame does) in its own
 * costume, not the retail costume that costume is built on. Returns 0 (retail frame) otherwise. */
int mu_ak_hud_player_stock_frame(int player, float* frame)
{
    int ftkind;
    if (player < 0 || player >= AK_HUD_PLAYERS || !ak_stock_row[player]) {
        return 0;
    }
    ftkind = Player_80036394(player);
    if (ftkind < 0) {
        ftkind = ak_ftkind_of_ckind(Player_GetPlayerCharacter(player));
    }
    return ak_mex_stock_frame(ftkind, (int) Player_GetCostumeId(player), frame);
}

/* ifStock_802F96D0 and ifStock_802F9F48: `icon` is joint 1 of a stock model the single player
 * modes build for one fighter of a list (the icon grids of All-Star, Classic and Adventure). On
 * an m-ex disc the joint takes the m-ex icon animation and `frame` is the m-ex frame of character
 * kind `ckind` in costume `costume`, as m-ex draws every stock icon: the retail animation has no
 * icon for an added character or for a costume m-ex added. Returns 0 (retail animation and frame,
 * nothing changed) when there is no m-ex icon table or no m-ex frame for that character. */
int mu_ak_hud_list_icon(HSD_JObj* icon, int ckind, int costume, float* frame)
{
    AkStockIcons* icons;
    HSD_MatAnimJoint* matanim;
    if (icon == NULL || !mu_mex_fighters_active()) {
        return 0;
    }
    icons = hud_symbol("Stc_icns");
    matanim = icons != NULL ? DP(icons->matanim) : NULL;
    if (matanim == NULL || icons->stride == 0) {
        return 0;
    }
    ak_stock_reserved = icons->reserved;
    ak_stock_stride = icons->stride;
    if (!ak_mex_stock_frame(ak_ftkind_of_ckind(ckind), costume, frame)) {
        return 0;
    }
    HSD_JObjAddAnimAll(icon, NULL, matanim, NULL);
    {
        /* Evidence for test runs: a list's icons are built once per list, a few dozen at most. */
        static int logged;
        if (logged < 64) {
            logged++;
            OSReport("[ak] list stock icon: character kind %d, costume %d, m-ex frame %d\n",
                     ckind, costume, (int) *frame);
        }
    }
    return 1;
}

/* Squad Strike's lineup icons in `player`'s stock row: character kind `ckind` in costume
 * `costume`, in the m-ex frame layout when the row took the m-ex animation. 0 otherwise. */
int mu_ak_hud_row_stock_frame(int player, int ckind, int costume, float* frame)
{
    if (player < 0 || player >= AK_HUD_PLAYERS || !ak_stock_row[player]) {
        return 0;
    }
    return ak_mex_stock_frame(ak_ftkind_of_ckind(ckind), costume, frame);
}

/* ifStatus_802F61FC: `mark` is the emblem model of an added character's damage display. Puts
 * the m-ex emblem animation on it and gives the emblem's frame. Returns 0 when the HUD file has
 * no such animation (the caller keeps the retail frame). */
int mu_ak_hud_emblem(HSD_JObj* mark, int ckind, float* frame)
{
    HSD_MatAnimJoint* matanim = hud_symbol("Eblm_matanim_joint");
    HSD_JObj* joint = NULL;
    if (!MU_AK_CKIND(ckind) || matanim == NULL) {
        return 0;
    }
    lb_80011E24(mark, &joint, 1, -1);
    if (joint == NULL) {
        return 0;
    }
    HSD_JObjAddAnimAll(joint, NULL, matanim, NULL);
    *frame = (float) mu_mex_insignia(mu_ak_mex_external(ckind));
    return 1;
}

/* ---- the results screen ---- */

/* Whether this results screen's panels took the HUD file's stock icon and emblem animations. */
static int ak_results_stock, ak_results_emblem;

/* fn_80175DC8: `root` is the results panel model, its animations added but not yet started.
 * The disc's results file has placeholder stock icon and emblem textures: as m-ex does, the
 * four panels' stock icon joints (25 to 28, second display object) and emblem joints (66 to 69)
 * take the HUD file's animations, for every character. The HUD file is not this scene's own:
 * it is used only if the game still holds it loaded. */
void mu_ak_hud_results_build(HSD_JObj* root)
{
    HSD_Archive* archive;
    AkStockIcons* icons;
    HSD_MatAnimJoint* joint;
    int i;
    ak_results_stock = 0;
    ak_results_emblem = 0;
    if (!mu_mex_fighters_active()) {
        return;
    }
    archive = lbDvd_8001819C("IfAll");
    if (archive == NULL) {
        OSReport("[ak] results: the HUD file is not loaded; retail stock icons and emblems\n");
        return;
    }
    icons = HSD_ArchiveGetPublicAddress(archive, "Stc_icns");
    joint = icons != NULL ? DP(icons->matanim) : NULL;
    if (joint != NULL && DP(joint->matanim) != NULL) {
        ak_stock_reserved = icons->reserved;
        ak_stock_stride = icons->stride;
        for (i = 0; i < 4; i++) {
            HSD_JObj* stock = NULL;
            lb_80011E24(root, &stock, 25 + i, -1);
            if (stock != NULL && stock->u.dobj != NULL && stock->u.dobj->next != NULL) {
                HSD_DObjAddAnimAll(stock->u.dobj->next, DP(joint->matanim), NULL);
                ak_results_stock = 1;
            }
        }
    }
    joint = HSD_ArchiveGetPublicAddress(archive, "Eblm_matanim_joint");
    if (joint != NULL && DP(joint->matanim) != NULL) {
        for (i = 0; i < 4; i++) {
            HSD_JObj* emblem = NULL;
            lb_80011E24(root, &emblem, 66 + i, -1);
            if (emblem != NULL && emblem->u.dobj != NULL) {
                HSD_DObjAddAnimAll(emblem->u.dobj, DP(joint->matanim), NULL);
                ak_results_emblem = 1;
            }
        }
    }
    OSReport("[ak] results: stock icons %s, emblems %s (reserved %d, stride %d)\n",
             ak_results_stock ? "from the HUD file" : "retail",
             ak_results_emblem ? "from the HUD file" : "retail", ak_stock_reserved,
             ak_stock_stride);
}

/* The stock icon frame of native fighter kind `ftkind` in costume `costume` on a results
 * screen that took the HUD file's icon animation: reserved + costume * stride + the m-ex
 * internal id, and the fixed frames m-ex gives the special fighters. Returns 0 (frame left
 * alone) when this results screen kept the retail animation. */
int mu_ak_hud_results_stock(int ftkind, int costume, float* frame)
{
    if (!ak_results_stock) {
        return 0;
    }
    return ak_mex_stock_frame(ftkind, costume, frame);
}

/* The emblem frame of character kind `ckind` on such a results screen: the insignia MxDt gives
 * its external id. Returns 0 when this results screen kept the retail animation. */
int mu_ak_hud_results_emblem(int ckind, float* frame)
{
    int ext;
    if (!ak_results_emblem) {
        return 0;
    }
    ext = mu_ak_mex_external(ckind);
    if (ext < 0) {
        return 0;
    }
    *frame = (float) mu_mex_insignia(ext);
    return 1;
}

#endif /* MU_AKANEIA_FIGHTERS */
