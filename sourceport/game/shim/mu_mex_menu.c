/* m-ex's own main menu rows (MxMn.dat), read natively.
 *
 * An m-ex disc can carry MxMn.dat: an HSD archive whose public object "mexMenu" replaces the main
 * menu's tables (m-ex asm "MainMenu Expansion"; layout as in m-ex's asm Header.s):
 *   +00 the menu name frames, 3 loops per menu kind
 *   +04 the menu kinds, 0x14 bytes each: preview loops, unused, option rows, then the bytes
 *       count, previous menu, name frame, hue; the think callback (a console address)
 *   +08 the description streams (SIS), by text id
 *   +0C the option name MatAnimJoint (one texture animation: one label picture per frame)
 *   +10 the menu name MatAnimJoint
 * An option row is 8 bytes: kind (0 a menu, 1 a major scene), id, text id, name frame, preview.
 *
 * ACE 2.0.1 puts a menu of its own under VS Mode where Tournament Melee was ("Games": All-Star
 * Versus, Tag, Turbo Melee, Volleyball). Each of those is a major scene m-ex adds (scene table of
 * MxDt.dat, one file of compiled PowerPC each: ScMjIm.dat, ScMjTg.dat, ScMjTrb.dat, scVb.dat).
 * The native game runs no PowerPC, so a mode is offered only when this build has its own code:
 *   - scVb.dat (Volleyball): VS Mode's own scenes with the stage fixed to the court (/GrMVb.dat,
 *     native in mexstages/volleyball), no stage select, no time limit, fighters that do not hit
 *     each other (StartMeleeRules x5_2), exactly as scVb.dat's match prep sets them.
 *   - every other scene file: shown dimmed, A plays the error sound and nothing opens.
 * The menu itself is the game's own (mnmain.c, menu kind 34): rows, labels and words come from
 * MxMn.dat as they are on the disc. Without MxMn.dat (or with no added menu under VS Mode) nothing
 * here changes the game: Tournament Melee stays where it is. */
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>

#include <string.h>

#include <mu_native.h>
#include "mu_disc.h"
#include "mu_mex_data.h"

#define MXMN_VS_KIND 2      /* MENU_KIND_VS */
#define MXMN_VS_ROW 1       /* SEL_VS_TOURNAMENT: the row ACE gives its own menu */
#define MXMN_RETAIL_KINDS 34
#define MXMN_MAX_ROWS 8
#define MXMN_MAX_TEXT 0x80
#define MXMN_MAJOR_END 45   /* MJRKIND_NULL: the scene table's end */

typedef struct MxMnRow {
    u8 kind;    /* 0 a menu kind, 1 a major scene */
    u8 id;
    u8 text;    /* description stream id */
    u8 name;    /* frame of the option name animation */
    u8 preview;
    u8 pad[3];
} DISC_STRUCT MxMnRow;

typedef struct MxMnKind {
    DISC_PTR(void) preview;
    u32 unused;
    DISC_PTR(MxMnRow) rows;
    u8 count, prev, name, hue;
    u32 think;
} DISC_STRUCT MxMnKind;

typedef struct MxMnRoot {
    DISC_PTR(void) name_frames;
    DISC_PTR(MxMnKind) kinds;
    DISC_PTR(DISC_PTR(u8)) text;
    DISC_PTR(HSD_MatAnimJoint) option_names;
    DISC_PTR(HSD_MatAnimJoint) menu_names;
} DISC_STRUCT MxMnRoot;

/* MxDt scene table (mexData +0x30): the major scenes, 0x18 bytes each, file name at +0x14. */
typedef struct MexMajorRow {
    u8 preload;
    s8 id;
    u8 pad[2];
    u32 load, exit, boot;
    DISC_PTR(void) minors;
    DISC_PTR(char) file;
} DISC_STRUCT MexMajorRow;

typedef struct MexScenes {
    DISC_PTR(MexMajorRow) majors;
    DISC_PTR(void) minors;
} DISC_STRUCT MexScenes;

typedef struct MexDataScenes {
    u8 before[0x30];
    DISC_PTR(MexScenes) scenes;   /* mexData word 12 */
} DISC_STRUCT MexDataScenes;

enum { ROW_LOCKED, ROW_VOLLEYBALL };

static MxMnRoot* mxmn;
static int games_kind = -1;     /* the MxMn menu kind VS Mode's second row opens, -1 none */
static int games_count;
static MxMnRow games_vs_row;    /* VS Mode's own row that opens it */
static MxMnRow games_rows[MXMN_MAX_ROWS];
static u8 games_what[MXMN_MAX_ROWS];
static int volleyball_ext = -1; /* a Volleyball session: the court's external stage id */

/* Dimmed label pictures of the locked rows: rebuilt each time the menu scene loads MxMn.dat. */
static HSD_ImageDesc games_dim_desc[MXMN_MAX_ROWS];
static u8 games_dim_pixels[MXMN_MAX_ROWS][176 * 32 * 2] __attribute__((aligned(32)));
static u8 games_dim_ok[MXMN_MAX_ROWS];

static const char* major_file(int id)
{
    MexData* mex = mu_mex_root();
    const MexScenes* scenes;
    const MexMajorRow* row;
    int i;
    if (mex == NULL) {
        return NULL;
    }
    scenes = DP(((const MexDataScenes*) (const void*) mex)->scenes);
    if (scenes == NULL || (row = DP(scenes->majors)) == NULL) {
        return NULL;
    }
    for (i = 0; i < 96 && row[i].id != MXMN_MAJOR_END && row[i].id != -1; i++) {
        if (row[i].id == id) {
            return DP(row[i].file);
        }
    }
    return NULL;
}

/* The court's external stage id (scVb.dat's own lookup: the stage row whose file is /GrMVb.dat),
 * -1 when the disc has none or this build cannot load it. */
static int volleyball_stage(void)
{
    int ext, n = mu_mex_stage_ext_count();
    for (ext = 0; ext < n; ext++) {
        unsigned int words[13];
        const char* file = NULL;
        const int internal = mu_mex_stage_internal_of_external(ext);
        if (internal >= 0 && mu_mex_stage_desc(internal, words, &file) && file != NULL &&
            strcmp(file, "/GrMVb.dat") == 0)
        {
            return mu_mex_stage_usable(internal) ? ext : -1;
        }
    }
    return -1;
}

static HSD_TexAnim* option_texanim(void)
{
    HSD_MatAnimJoint* maj;
    HSD_MatAnim* ma;
    if (mxmn == NULL || (maj = DP(mxmn->option_names)) == NULL || (ma = DP(maj->matanim)) == NULL) {
        return NULL;
    }
    return DP(ma->texanim);
}

static HSD_ImageDesc* option_picture(int frame)
{
    HSD_TexAnim* ta = option_texanim();
    if (ta == NULL || frame < 0 || frame >= ta->n_imagetbl || DP(ta->imagetbl) == NULL) {
        return NULL;
    }
    return DP(DP(ta->imagetbl)[frame]);
}

/* A locked row's label: the same picture at under half its brightness. IA4 and IA8 only (the two
 * formats the menu labels use); anything else keeps its own picture. */
static void build_dim(int row)
{
    const HSD_ImageDesc* src = option_picture(games_rows[row].name);
    const u8* px;
    int w, h, size, i;
    games_dim_ok[row] = 0;
    if (src == NULL || (px = DP(src->image_ptr)) == NULL) {
        return;
    }
    w = src->width;
    h = src->height;
    if (src->format == GX_TF_IA4) {
        size = ((w + 7) & ~7) * ((h + 3) & ~3);
    } else if (src->format == GX_TF_IA8) {
        size = ((w + 3) & ~3) * ((h + 3) & ~3) * 2;
    } else {
        return;
    }
    if (size <= 0 || size > (int) sizeof games_dim_pixels[row]) {
        return;
    }
    for (i = 0; i < size; i++) {
        u8 v = px[i];
        if (src->format == GX_TF_IA4) {
            v = (u8) ((v & 0xF0) | ((v & 0x0F) * 2 / 5));
        } else if (i & 1) {
            v = (u8) (v * 2 / 5);   /* IA8: alpha, then intensity */
        }
        games_dim_pixels[row][i] = v;
    }
    games_dim_desc[row] = *src;
    DISC_SET(games_dim_desc[row].image_ptr, games_dim_pixels[row]);
    games_dim_ok[row] = 1;
}

/* mnMain_Scene_OnEnter, with the menu's own archive: MxMn.dat into the same scene heap. */
void mu_mex_menu_load(void)
{
    HSD_Archive* ar;
    MxMnKind* kinds;
    const MxMnRow* vs_rows;
    const MxMnRow* rows;
    int i, kind;
    mxmn = NULL;
    games_kind = -1;
    games_count = 0;
    if (!mu_mex_active() || DVDConvertPathToEntrynum("MxMn.dat") < 0) {
        return;
    }
    ar = lbArchive_LoadArchive("MxMn.dat");
    mxmn = ar != NULL ? HSD_ArchiveGetPublicAddress(ar, "mexMenu") : NULL;
    if (mxmn == NULL || (kinds = DP(mxmn->kinds)) == NULL) {
        mxmn = NULL;
        return;
    }
    /* VS Mode keeps the game's own rows; only its second row may open a menu m-ex adds. */
    if (kinds[MXMN_VS_KIND].count <= MXMN_VS_ROW || (vs_rows = DP(kinds[MXMN_VS_KIND].rows)) == NULL) {
        return;
    }
    games_vs_row = vs_rows[MXMN_VS_ROW];
    kind = games_vs_row.id;
    if (games_vs_row.kind != 0 || kind < MXMN_RETAIL_KINDS || kind > 0x40 ||
        (rows = DP(kinds[kind].rows)) == NULL || kinds[kind].count == 0 ||
        kinds[kind].count > MXMN_MAX_ROWS)
    {
        return;
    }
    for (i = 0; i < kinds[kind].count; i++) {
        const char* file;
        games_rows[i] = rows[i];
        if (rows[i].kind != 1) {
            return;   /* a row this layer cannot place: leave Tournament Melee as it is */
        }
        file = major_file(rows[i].id);
        games_what[i] = file != NULL && strcmp(file, "scVb.dat") == 0 && volleyball_stage() >= 0
                            ? ROW_VOLLEYBALL
                            : ROW_LOCKED;
        if (games_what[i] == ROW_LOCKED) {
            build_dim(i);
        }
        OSReport("[mex] menu: VS Mode row %d, %s: scene %d (%s), %s\n", MXMN_VS_ROW + 1,
                 "added menu", (int) rows[i].id, file != NULL ? file : "no file",
                 games_what[i] == ROW_VOLLEYBALL ? "native" : "locked (no native code in this build)");
    }
    games_kind = kind;
    games_count = kinds[kind].count;
}

/* The rows of the added menu under VS Mode, 0 when there is none (Tournament Melee stays). */
int mu_mex_games(void)
{
    return games_kind >= 0 ? games_count : 0;
}

int mu_mex_games_locked(int row)
{
    return row < 0 || row >= mu_mex_games() || games_what[row] == ROW_LOCKED;
}

/* A label of VS Mode's added row or of the added menu (menu kind 34): the disc's own picture,
 * dimmed when the row is locked. Called after the game set the label's frame. */
void mu_mex_games_label(HSD_JObj* label, int menu_kind, int selection)
{
    HSD_ImageDesc* picture;
    HSD_DObj* dobj;
    if (label == NULL || mu_mex_games() == 0) {
        return;
    }
    if (menu_kind == MXMN_VS_KIND && selection == MXMN_VS_ROW) {
        picture = option_picture(games_vs_row.name);
    } else if (menu_kind == MXMN_RETAIL_KINDS && selection >= 0 && selection < games_count) {
        picture = games_what[selection] == ROW_LOCKED && games_dim_ok[selection]
                      ? &games_dim_desc[selection]
                      : option_picture(games_rows[selection].name);
    } else {
        return;
    }
    if (picture == NULL) {
        return;
    }
    for (dobj = label->u.dobj; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* texture;
        if (dobj->mobj == NULL) {
            continue;
        }
        for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
            const HSD_ImageDesc* now = texture->imagedesc;
            /* the option name picture: the same size on the disc and in the game's menu file */
            if (now != NULL && now->width == picture->width && now->height == picture->height &&
                (now->format == GX_TF_IA4 || now->format == GX_TF_IA8))
            {
                texture->imagedesc = picture;
            }
        }
    }
}

static int sis_size(unsigned op)
{
    switch (op) {
    case 5: return 3;
    case 6: case 7: case 8: case 9: case 10: case 14: return 5;
    case 12: return 4;
    default: return op >= 0x20 ? 2 : 1;
    }
}

/* The description of VS Mode's added row or of an added menu row: the disc's own words (their
 * stream, formatting and all). A locked row says so instead. */
void mu_mex_games_describe(HSD_Text* text, int menu_kind, int selection)
{
    const u8* stream;
    SisBuffer* alloc;
    int id, n;
    if (text == NULL || mu_mex_games() == 0) {
        return;
    }
    if (menu_kind == MXMN_VS_KIND && selection == MXMN_VS_ROW) {
        id = games_vs_row.text;
    } else if (menu_kind == MXMN_RETAIL_KINDS && selection >= 0 && selection < games_count) {
        if (games_what[selection] == ROW_LOCKED) {
            mu_slippi_menu_description(text, "Locked: this mode does not run in this version.");
            return;
        }
        id = games_rows[selection].text;
    } else {
        return;
    }
    if (id >= MXMN_MAX_TEXT || DP(mxmn->text) == NULL || (stream = DP(DP(mxmn->text)[id])) == NULL) {
        return;
    }
    /* Up to the end command; a jump (8, 9) carries an address and is not copied. */
    for (n = 0; n < 512 && stream[n] != 0; n += sis_size(stream[n])) {
        if (stream[n] == 8 || stream[n] == 9) {
            return;
        }
    }
    if (n >= 512) {
        return;
    }
    alloc = (SisBuffer*) HSD_SisLib_Alloc(sizeof *alloc);
    if (alloc == NULL) {
        return;
    }
    alloc->data = (u8*) HSD_SisLib_Alloc(n + 1);
    if (alloc->data == NULL) {
        HSD_SisLib_Free(alloc);
        return;
    }
    memcpy(alloc->data, stream, n);
    alloc->data[n] = 0;
    alloc->end = alloc->data + n;
    alloc->size = n + 1;
    alloc->count = 0;
    if (text->alloc_data) {
        HSD_SisLib_Free(text->alloc_data->data);
        HSD_SisLib_Free(text->alloc_data);
    }
    text->alloc_data = alloc;
    text->sis_buffer = alloc->data;
    text->x60 = NULL;
}

/* A of the added menu: 1 when the row starts (the caller then leaves for VS Mode), 0 locked. */
int mu_mex_games_start(int row)
{
    if (mu_mex_games_locked(row)) {
        OSReport("[mex] menu: added row %d is locked in this build\n", row + 1);
        return 0;
    }
    volleyball_ext = volleyball_stage();
    OSReport("[mex] menu: Volleyball (stage ext %d)\n", volleyball_ext);
    return volleyball_ext >= 0;
}

/* VS Mode's scenes while a Volleyball session runs: the court's external id, -1 otherwise. */
int mu_mex_volleyball(void)
{
    return volleyball_ext;
}

void mu_mex_volleyball_end(void)
{
    volleyball_ext = -1;
}

/* scVb.dat's match prep after gmVsMelee_EnterVs: no time limit (the timer counts up), the fighters
 * do not hit each other, the court, and the court in the preload cache. */
void mu_mex_volleyball_rules(struct StartMeleeData* start)
{
    if (volleyball_ext < 0 || start == NULL) {
        return;
    }
    start->rules.match_kind = 0;
    start->rules.x0_3 = 0;
    start->rules.timer_enabled = 0;
    start->rules.timer_counts_up = 1;
    start->rules.x5_2 = 1;
    start->rules.stkind = (u16) volleyball_ext;
    lbDvd_GetPreloadCacheScene()->game_cache.stkind = volleyball_ext;
    lbDvd_80018254();
    OSReport("[mex] Volleyball: match on stage ext %d\n", volleyball_ext);
}
