/* An m-ex table built here, for a disc that has none.
 *
 * A fighter m-ex adds is a row in MxDt.dat, the table file MexTool writes into a disc: its files,
 * its costumes, the default of each of its functions. The native game reads that table directly
 * (shim/mu_mex.c). A disc without the file had no way to carry an added fighter.
 *
 * The host program keeps the fighters installed beside the game (fighters\<name>\fighter.json, the
 * m-ex fighter format, with the files it names), puts their files on the disc it serves, and hands
 * each one over as a record (MU_CMD_MOD_BROWSER 3 and 4, port/runtime/host/roster.cpp). From those
 * records this file builds the table in memory, in the layout MxDt.dat has once loaded:
 *
 *   - the retail fighters keep their places, with the rows the added fighters' code is matched
 *     against: where each external id's fighter is, and the console address of each retail
 *     fighter's default functions (mu_mex_retail.inc, the same on every m-ex disc);
 *   - each installed fighter gets the next internal id from Ft_Kind_MasterH and the next external
 *     id from CKind_Playable_Count, as MexTool numbers them, and the special fighters move up;
 *   - every table the game reads for an added fighter has its row. The tables it reads for the
 *     whole game (stages, music, the menus, the sound banks) are left out: mu_mex_active() stays
 *     false for a table built here, so the game keeps its own.
 *
 * Two things an m-ex disc keeps outside MxDt.dat are built here as well, since a retail disc's
 * files do not have them: the fighter's skeleton tables (which joint is the head, the hands, and
 * so on: PlCo.dat on an m-ex disc, "boneDefinitions" in fighter.json), handed to Fighter_LoadCommon
 * by mu_mex_roster_parts; and its place in the game's effect file table, whose numbering the
 * table follows (a fighter made from a retail one names that fighter's own effect file).
 *
 * The retail character select has no icon or portrait for such a fighter either. The host hands
 * each one's pictures over as textures (icon.png and csp_<costume>.png of the fighter's folder),
 * kept here; mn/mncharsel.c shows them on the free corners of the retail icon grid, and a match's
 * stock icons are the fighter's own (stc_<costume>.png, akaneia/common/mu_ak_hud.c).
 *
 * A fighter's own sounds, the way an m-ex disc carries them (port/runtime/host/fighter_sound.h): the
 * host serves its bank as one more bank past the retail ones (56 and up, audio/us/nf_<hash>.ssm), a
 * smash2.sem with that bank's scripts, and nr_name.ssm with its name call as one more sound of bank
 * 51. What the record adds after its fields names them; the table gets the bank's row (mexData.ssm)
 * and the fighter's external id that bank, so the game's own loader brings it with the match
 * (lb/lbaudio_ax.c mu_ak_sound_view, mu_ak_sound_request). The fighter's data names its base
 * fighter's sounds; akaneia/common/mu_ak_sound.c plays its own bank's in their place. The game loads
 * smash2.sem and nr_name.ssm once, at boot: when the host serves others (the serial of ROSTER_INFO),
 * they are loaded again here, as a language change does. A bank whose row is wrong, or files that
 * did not load, leave the fighter its base fighter's sounds and call, with one log line. */
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <melee/ef/types.h>
#include <melee/lb/lb_0195.h>
#include <melee/lb/lblanguage.h>
#include <sysdolphin/baselib/axdriver.h>
#include <sysdolphin/baselib/synth.h>

#include <string.h>

#include "mu_disc.h"
#include "mu_mex_data.h"
#include "mu_native.h"

#include "mu_mex_retail.inc"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int response_capacity, unsigned int* response_size);

#define ROSTER_INFO 3
#define ROSTER_RECORD 4
#define ROSTER_IMAGE 5

#define RETAIL_INTERNAL 33
#define RETAIL_EXTERNAL 33
#define PLAYABLE_INTERNAL 27   /* Ft_Kind_MasterH */
#define PLAYABLE_EXTERNAL 26   /* CKind_Playable_Count */
#define FUNCTION_TABLES 46
#define TWO_ENTRY_TABLE 30     /* two words a fighter, numbers not addresses */
#define RETAIL_BANKS 56        /* bank ids below it are the retail banks (55: none) */
#define RETAIL_SONGS 98
#define RETAIL_EFFECTS 51      /* the game's effect file table: its entries, the empty last one included */
#define PARTS 0x36             /* the parts a skeleton table names */
#define JOINTS 140             /* the joints a fighter keeps (MAX_FT_PARTS, ft/ftparts.h) */
#define ACTIONS 2048           /* the action table entries a record may name (retail's largest: 479) */
#define ADDED_ITEMS 237        /* the first item kind m-ex adds */

/* The game's effect file table (ef/efasync.c): file, table symbol, loaded data. */
struct RosterEffectFile {
    char* file;
    char* symbol;
    void* data;
};
extern struct RosterEffectFile efAsync_DatEntries[EF_DAT_FILE_MAX + 1];
_Static_assert(sizeof(struct RosterEffectFile) == sizeof(EF_DAT_Entry), "the game's effect file entry");

#if MU_AK_KIND_SLOTS < 16
#define ROSTER_MAX MU_AK_KIND_SLOTS
#else
#define ROSTER_MAX 16
#endif

/* The table's memory: in this library's image, so every address in it fits a 32-bit slot. */
static u8 pool[128 * 1024] __attribute__((aligned(32)));
static u32 pool_used;
static int pool_full;

static u8* take(u32 size)
{
    u8* out;
    size = (size + 3u) & ~3u;
    if (size > sizeof pool - pool_used) {
        pool_full = 1;
        return NULL;
    }
    out = pool + pool_used;
    pool_used += size;
    memset(out, 0, size);
    return out;
}

static void put32(u8* p, u32 v)
{
    if (p != NULL) {
        p[0] = (u8) (v >> 24);
        p[1] = (u8) (v >> 16);
        p[2] = (u8) (v >> 8);
        p[3] = (u8) v;
    }
}

static void put_ptr(u8* slot, const void* target)
{
    put32(slot, target != NULL ? mu_addr32(target) : 0);
}

/* ---- a record from the host ---- */

static unsigned char record[4096];
static unsigned int record_size, record_at;
static int record_bad;

static u32 rd(int bytes)
{
    u32 v = 0;
    if (record_at + (unsigned) bytes > record_size) {
        record_bad = 1;
        return 0;
    }
    while (bytes-- > 0) {
        v = v << 8 | record[record_at++];
    }
    return v;
}

/* A string of the record, copied into the table's memory; NULL for an empty one. */
static char* rd_str(void)
{
    const u32 length = rd(1);
    char* out;
    if (record_bad || record_at + length > record_size) {
        record_bad = 1;
        return NULL;
    }
    if (length == 0) {
        return NULL;
    }
    out = (char*) take(length + 1);
    if (out != NULL) {
        memcpy(out, record + record_at, length);
    }
    record_at += length;
    return out;
}

static int ask(unsigned char what, unsigned char index)
{
    unsigned char payload[2];
    payload[0] = what;
    payload[1] = index;
    record_size = 0;
    record_at = 0;
    record_bad = 0;
    return mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, what == ROSTER_RECORD ? 2 : 1, record, sizeof record,
                                 &record_size) == 0;
}

/* Two file names name the same disc file: compared up to the extension, ignoring case. */
static int same_file(const char* a, const char* b)
{
    for (; *a != '\0' && *a != '.' && *b != '\0' && *b != '.'; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char) (x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char) (y - 'A' + 'a');
        if (x != y) {
            return 0;
        }
    }
    return (*a == '\0' || *a == '.') && (*b == '\0' || *b == '.');
}

/* The effect table's entry for a file: the retail entry that names it, or the next free one past
 * the retail entries. 0xFF when there is no file or no room. */
static u8* effect_files;
static int effect_next;

static u8 effect_entry(const char* file, const char* symbol)
{
    int i;
    if (file == NULL || symbol == NULL) {
        return 0xFF;
    }
    for (i = 1; i < RETAIL_EFFECTS - 1; i++) {
        if (efAsync_DatEntries[i].file != NULL && same_file(efAsync_DatEntries[i].file, file)) {
            put_ptr(effect_files + 12 * i, file);
            put_ptr(effect_files + 12 * i + 4, symbol);
            return (u8) i;
        }
    }
    if (effect_next >= EF_DAT_FILE_MAX) {
        OSReport("[roster] no room in the effect file table for %s\n", file);
        return 0xFF;
    }
    put_ptr(effect_files + 12 * effect_next, file);
    put_ptr(effect_files + 12 * effect_next + 4, symbol);
    return (u8) effect_next++;
}

/* The skeleton tables of installed fighter k, as PlCo.dat has a fighter's: the parts table
 * {joint to part, part to joint, joint count} and the extra table {entries, count}. */
static u8* roster_parts[ROSTER_MAX];
static u8* roster_parts_ext[ROSTER_MAX];
static int roster_built;   /* a table built here is the one served */
static int roster_count;   /* its installed fighters, the empty slots included */
static u8 roster_series[ROSTER_MAX];
static u8 roster_ready[ROSTER_MAX];

/* ---- the select screen pictures ---- */

#define ICON_BYTES (64 * 56 * 4)
#define PORTRAIT_BYTES (136 * 188 * 4)
#define PORTRAITS 40
#define STOCK_BYTES (24 * 24 * 4)
#define STOCKS 96

static u8 icon_pixels[ROSTER_MAX][ICON_BYTES] __attribute__((aligned(32)));
static signed char icon_have[ROSTER_MAX];   /* 0 not asked, 1 here, -1 the host has none */
static u8 portrait_pixels[PORTRAITS][PORTRAIT_BYTES] __attribute__((aligned(32)));
static struct {
    short fighter, costume;
    signed char have;
} portrait_slot[PORTRAITS];
static int portrait_slots;
static u8 stock_pixels[STOCKS][STOCK_BYTES] __attribute__((aligned(32)));
static struct {
    short fighter, costume;
    signed char have;
} stock_slot[STOCKS];
static int stock_slots;

/* One picture from the host, in parts; `size` bytes of a `width` x `height` RGBA8 texture. */
static int fetch_image(int k, int what, int index, u8* out, u32 size, u32 width, u32 height)
{
    u32 at = 0;
    while (at < size) {
        unsigned char payload[7];
        u32 part;
        payload[0] = ROSTER_IMAGE;
        payload[1] = (unsigned char) k;
        payload[2] = (unsigned char) what;
        payload[3] = (unsigned char) index;
        payload[4] = (unsigned char) (at >> 16);
        payload[5] = (unsigned char) (at >> 8);
        payload[6] = (unsigned char) at;
        record_size = 0;
        if (mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, 7, record, sizeof record, &record_size) != 0 ||
            record_size <= 8 || record[0] != 1 || ((u32) record[1] << 8 | record[2]) != width ||
            ((u32) record[3] << 8 | record[4]) != height ||
            ((u32) record[5] << 16 | (u32) record[6] << 8 | record[7]) != size)
        {
            return 0;
        }
        part = record_size - 8;
        if (part > size - at) {
            return 0;
        }
        memcpy(out + at, record + 8, part);
        at += part;
    }
    return 1;
}

/* ---- the fighters' own sounds ---- */

#define OWN_BANK_FIRST 56
#define OWN_BANK_LAST 126          /* lbaudio_ax.c mu_ak_sound_view fills rows below LB_SSM_CAP - 1 */
#define OWN_BANK_BYTES (1u << 20)  /* a bank's samples: the host's limit (the largest retail bank: 600 KB) */
#define OWN_FILE 32                /* a bank's file name, with its end */
#define NAME_CALLS 510000          /* bank 51: the announcer's calls */
#define RETAIL_CALLS 54            /* its scripts on the retail disc */

static struct {
    u8 base, own;   /* the base fighter's bank (55 none); its own bank, 0 none */
    u16 scripts;    /* the scripts of its own bank */
    u32 call, fallback;
} roster_sound[ROSTER_MAX];
static char own_files[ROSTER_MAX][OWN_FILE];
static int own_banks_high = OWN_BANK_FIRST;   /* the bank table's size: never lowered in a run */
static unsigned int sound_served;   /* what the host serves (0: the disc's own files) */
static unsigned int sound_loaded;   /* what smash2.sem and nr_name.ssm the game has loaded */
static unsigned int sound_failed;

/* The host serves other sound files: smash2.sem again, and in English the resident banks again
 * (nr_name.ssm's samples moved), as lbAudioAx_80027AB0 does for a language change. The fighter
 * banks loaded stay: every retail script and sample keeps its number. At a major mode load only,
 * where nothing of the next mode plays yet. */
static void roster_sound_sync(unsigned int serial)
{
    static const char* const resident[] = { "nr_name.ssm", "pokemon.ssm", "end.ssm" };
    char path[0x40];
    const int us = lbLang_IsSavedLanguageUS();
    const char* folder = us ? "/audio/us/" : "/audio/";
    int i;
    sound_served = serial;
    if (serial == sound_loaded || serial == sound_failed) {
        return;
    }
    strcpy(path, folder);
    strcat(path, "smash2.sem");
    if (DVDConvertPathToEntrynum(path) < 0) {
        sound_failed = serial;
        OSReport("[roster] sound: %s is not on the disc; the installed fighters keep their base fighters' sounds\n",
                 path);
        return;
    }
    HSD_AudioSFXKeyOffAll();
    AXDriver_8038DCFC();
    AXDriver_8038DA70(path, lb_800195D0);
    if (us) {
        HSD_SynthSFXUnloadBank(1);
        for (i = 0; i < (int) (sizeof resident / sizeof resident[0]); i++) {
            strcpy(path, folder);
            strcat(path, resident[i]);
            HSD_SynthSFXLoad(path, 1, NULL, 0);
            HSD_SynthSFXWaitForLoadCompletion(lb_800195D0);
        }
    }
    sound_loaded = serial;
    OSReport("[roster] sound: %08X loaded (smash2.sem%s)\n", serial, us ? ", nr_name.ssm" : "");
}

/* A file name the host gave a bank: "nf_<hash>.ssm" letters, short enough for the game's path. */
static int own_file_ok(const char* name, u32 length)
{
    u32 i;
    if (length < 5 || length >= OWN_FILE) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

/* What the record of installed fighter k adds for its sounds (after the record's own fields):
 * 1, its own bank (0 none), the bank's file name, its sample bytes, its row, its script count, then
 * its call and its base fighter's call. Returns its own bank when that can be used, else 0, with one
 * log line saying why. */
static int read_sound_tail(int k, const char* name, u8 row[4], u32* sample_bytes)
{
    u32 own, length, scripts, call, fallback, i;
    char file[OWN_FILE];
    const char* why = NULL;
    roster_sound[k].own = 0;
    roster_sound[k].scripts = 0;
    roster_sound[k].call = roster_sound[k].fallback = 0;
    own_files[k][0] = '\0';
    if (record_bad || record_at >= record_size) {
        return 0;   /* nothing added: its base fighter's bank, no call */
    }
    if (rd(1) != 1) {
        return 0;
    }
    own = rd(1);
    length = rd(1);
    file[0] = '\0';
    if (!record_bad && length < OWN_FILE && record_at + length <= record_size) {
        memcpy(file, record + record_at, length);
        file[length] = '\0';
        record_at += length;
    } else {
        record_bad = 1;
    }
    *sample_bytes = rd(4);
    for (i = 0; i < 4; i++) {
        row[i] = (u8) rd(1);
    }
    scripts = rd(2);
    call = rd(4);
    fallback = rd(4);
    if (record_bad) {
        OSReport("[roster] fighter %d (%s): what its record adds for its sounds is not readable; its base fighter's sounds\n",
                 k, name != NULL ? name : "?");
        return 0;
    }
    /* Only one of the disc's own calls stands in for the fighter's. */
    roster_sound[k].fallback = fallback >= NAME_CALLS && fallback < NAME_CALLS + RETAIL_CALLS ? fallback : 0;
    roster_sound[k].call = call >= NAME_CALLS && call < NAME_CALLS + 10000 ? call : roster_sound[k].fallback;
    if (own == 0) {
        return 0;
    }
    if (own < OWN_BANK_FIRST || own > OWN_BANK_LAST) {
        why = "its bank number is not one of the added banks";
    } else if (!own_file_ok(file, length)) {
        why = "its bank's file name is not one the host gives";
    } else if (*sample_bytes == 0 || *sample_bytes > OWN_BANK_BYTES) {
        why = "its bank's sample size is not 1 byte to 1 MB";
    } else if (row[0] != 4 || row[1] > 4 || row[2] > 4 || row[3] >= 0x80) {
        why = "its bank's row is not a fighter bank's";
    } else if (scripts == 0 || scripts > 4096) {
        why = "its bank's script count is not 1 to 4096";
    } else if (sound_served == 0 || sound_loaded != sound_served) {
        why = "the sound files with its bank did not load";
    } else {
        char path[0x40];
        strcpy(path, lbLang_IsSavedLanguageUS() ? "/audio/us/" : "/audio/");
        strcat(path, file);
        if (DVDConvertPathToEntrynum(path) < 0) {
            why = "its bank's file is not on the disc";
        }
    }
    if (why != NULL) {
        OSReport("[roster] fighter %d (%s): %s; it keeps its base fighter's sounds\n", k, name != NULL ? name : "?", why);
        return 0;
    }
    memcpy(own_files[k], file, length + 1);
    roster_sound[k].own = (u8) own;
    roster_sound[k].scripts = (u16) scripts;
    return (int) own;
}

int mu_mex_roster_sound(int ext, int* base_bank, int* own_bank, int* own_scripts)
{
    const int k = ext - PLAYABLE_EXTERNAL;
    if (!mu_mex_roster_ready(k)) {
        return 0;
    }
    *base_bank = roster_sound[k].base;
    *own_bank = roster_sound[k].own;
    *own_scripts = roster_sound[k].scripts;
    return 1;
}

/* Its own call in English, with the files that have it loaded; its base fighter's call otherwise
 * (the Japanese calls are the disc's own). */
int mu_mex_roster_call(int ext)
{
    const int k = ext - PLAYABLE_EXTERNAL;
    u32 call;
    if (!mu_mex_roster_ready(k)) {
        return -1;
    }
    call = roster_sound[k].call;
    if (call >= NAME_CALLS + RETAIL_CALLS &&
        (!lbLang_IsSavedLanguageUS() || sound_served == 0 || sound_loaded != sound_served))
    {
        call = roster_sound[k].fallback;
    }
    return (int) call;
}

/* How many installed fighters the table built here has; 0 when the table is the disc's own. */
int mu_mex_roster_fighters(void)
{
    return roster_built ? roster_count : 0;
}

/* Whether installed fighter k has its row, and the retail character whose series emblem it wears
 * (-1: a series the retail game has no emblem for). */
int mu_mex_roster_ready(int k)
{
    return roster_built && k >= 0 && k < roster_count && roster_ready[k];
}

int mu_mex_roster_emblem(int k)
{
    int c;
    if (!mu_mex_roster_ready(k)) {
        return -1;
    }
    for (c = 0; c < PLAYABLE_EXTERNAL; c++) {
        if (mu_mex_retail_insignia[c] == roster_series[k]) {
            return c;
        }
    }
    return -1;
}

/* Installed fighter k's icon, a 64x56 RGBA8 texture; NULL when the host has none. */
const void* mu_mex_roster_icon(int k)
{
    if (!mu_mex_roster_ready(k)) {
        return NULL;
    }
    if (icon_have[k] == 0) {
        icon_have[k] = fetch_image(k, 0, 0, icon_pixels[k], ICON_BYTES, 64, 56) ? 1 : -1;
    }
    return icon_have[k] > 0 ? icon_pixels[k] : NULL;
}

/* The stock icon of installed fighter k's costume, a 24x24 RGBA8 texture; NULL when its folder has
 * none (stc_<costume>.png). */
const void* mu_mex_roster_stock(int k, int costume)
{
    int i;
    if (!mu_mex_roster_ready(k) || costume < 0 || costume > 255) {
        return NULL;
    }
    for (i = 0; i < stock_slots; i++) {
        if (stock_slot[i].fighter == k && stock_slot[i].costume == costume) {
            return stock_slot[i].have > 0 ? stock_pixels[i] : NULL;
        }
    }
    if (stock_slots >= STOCKS) {
        return NULL;
    }
    i = stock_slots++;
    stock_slot[i].fighter = (short) k;
    stock_slot[i].costume = (short) costume;
    stock_slot[i].have = fetch_image(k, 2, costume, stock_pixels[i], STOCK_BYTES, 24, 24) ? 1 : -1;
    return stock_slot[i].have > 0 ? stock_pixels[i] : NULL;
}

/* The portrait of installed fighter k's costume, a 136x188 RGBA8 texture; NULL when there is none. */
const void* mu_mex_roster_portrait(int k, int costume)
{
    int i;
    if (!mu_mex_roster_ready(k) || costume < 0 || costume > 255) {
        return NULL;
    }
    for (i = 0; i < portrait_slots; i++) {
        if (portrait_slot[i].fighter == k && portrait_slot[i].costume == costume) {
            return portrait_slot[i].have > 0 ? portrait_pixels[i] : NULL;
        }
    }
    if (portrait_slots >= PORTRAITS) {
        return NULL;
    }
    i = portrait_slots++;
    portrait_slot[i].fighter = (short) k;
    portrait_slot[i].costume = (short) costume;
    portrait_slot[i].have = fetch_image(k, 1, costume, portrait_pixels[i], PORTRAIT_BYTES, 136, 188) ? 1 : -1;
    return portrait_slot[i].have > 0 ? portrait_pixels[i] : NULL;
}

/* Fighter_LoadCommon: fills the two PlCo.dat slots of installed fighter k. Returns 0 when the
 * table served is the disc's own (its PlCo.dat has every fighter's tables), or k has none. */
int mu_mex_roster_parts(int k, void* parts_slot, void* ext_slot)
{
    if (!roster_built || k < 0 || k >= ROSTER_MAX || roster_parts[k] == NULL) {
        return 0;
    }
    put_ptr(parts_slot, roster_parts[k]);
    put_ptr(ext_slot, roster_parts_ext[k]);
    return 1;
}

/* Whether a pointer slot's value lies in the table built here. */
int mu_mex_roster_holds(unsigned int raw)
{
    const u32 base = mu_addr32(pool);
    return roster_built && raw >= base && raw < base + sizeof pool;
}

/* Whether the fighter table served was built here: the disc's PlCo.dat is then the retail one,
 * with the special fighters where the retail game has them. */
int mu_mex_roster_built(void)
{
    return roster_built;
}

void mu_mex_roster_drop(void)
{
    roster_built = 0;
}

/* How many fighters the host has installed for a disc without m-ex, and a number that changes
 * whenever that set does. The sound files it serves for them are loaded here when they changed. */
int mu_mex_roster_count(unsigned int* generation)
{
    int count;
    if (!ask(ROSTER_INFO, 0) || record_size < 5) {
        *generation = 0;
        return 0;
    }
    count = (int) rd(1);
    *generation = rd(4);
    roster_sound_sync(record_size >= 9 ? rd(4) : 0);
    return count < ROSTER_MAX ? count : ROSTER_MAX;
}

static int shifted(int internal, int added)
{
    return internal < PLAYABLE_INTERNAL ? internal : internal + added;
}

/* The table for the host's `count` installed fighters, NULL when it cannot be built. */
MexData* mu_mex_roster_build(int count)
{
    const int internals = RETAIL_INTERNAL + count, externals = RETAIL_EXTERNAL + count;
    u8 *root, *meta, *ft, *fns, *effect, *kirby;
    u8 *names, *pl_files, *insignia, *ext_map, *costume_info, *costume_files, *ftdemo, *anim_files, *anim_num,
        *effect_index, *result_file, *result_scale, *victory, *announcer, *ssm_files, *walljump, *item_lookup;
    u8 *cap_files, *cap_effects;
    u8 *ssm, *ssm_names, *ssm_sizes, *ssm_rows;
    u8* fn_table[FUNCTION_TABLES];
    int i, k, t, built = 0, item_next = 0;

    if (count <= 0 || count > ROSTER_MAX) {
        return NULL;
    }
    pool_used = 0;
    pool_full = 0;
    root = take(0x30);
    meta = take(0x3C);
    ft = take(21 * 4);
    fns = take(64 * 4);
    effect = take(4);
    effect_files = take(12u * (EF_DAT_FILE_MAX + 1));
    effect_next = RETAIL_EFFECTS;
    roster_built = 0;
    roster_count = 0;
    memset(roster_parts, 0, sizeof roster_parts);
    memset(roster_parts_ext, 0, sizeof roster_parts_ext);
    memset(roster_ready, 0, sizeof roster_ready);
    memset(icon_have, 0, sizeof icon_have);
    portrait_slots = 0;
    stock_slots = 0;
    kirby = take(0x18);
    names = take(4u * externals);
    pl_files = take(8u * internals);
    insignia = take((u32) externals);
    ext_map = take(3u * externals);
    costume_info = take(4u * externals);
    costume_files = take(4u * internals);
    ftdemo = take(4u * internals);
    anim_files = take(4u * internals);
    anim_num = take(8u * internals);
    effect_index = take((u32) internals);
    result_file = take(4u * externals);
    result_scale = take(4u * externals);
    victory = take(4u * externals);
    announcer = take(4u * externals);
    ssm_files = take(16u * externals);
    walljump = take((u32) internals);
    item_lookup = take(8u * internals);
    cap_files = take(8u * internals);
    cap_effects = take((u32) internals);
    /* The sound bank table (mexData.ssm): rows only for the installed fighters' own banks; the
     * retail rows stay the game's (lb/lbaudio_ax.c mu_ak_sound_view fills 56 and up). */
    ssm = take(16);
    ssm_names = take(4u * (OWN_BANK_LAST + 1));
    ssm_sizes = take(8u * (OWN_BANK_LAST + 1));
    ssm_rows = take(4u * (OWN_BANK_LAST + 1));
    memset(roster_sound, 0, sizeof roster_sound);
    for (t = 0; t < FUNCTION_TABLES; t++) {
        fn_table[t] = take((t == TWO_ENTRY_TABLE ? 8u : 4u) * internals);
    }
    if (pool_full) {
        return NULL;
    }

    put_ptr(root + 0x00, meta);
    put_ptr(root + 0x08, ft);
    put_ptr(root + 0x0C, fns);
    put_ptr(root + 0x10, ssm);
    put_ptr(ssm + 0x0, ssm_names);
    put_ptr(ssm + 0x4, ssm_sizes);
    put_ptr(ssm + 0x8, ssm_rows);
    put_ptr(root + 0x18, effect);
    put_ptr(root + 0x20, kirby);
    meta[0] = 1;
    put32(meta + 0x04, (u32) internals);
    put32(meta + 0x08, (u32) externals);
    put32(meta + 0x1C, RETAIL_BANKS);
    put_ptr(ft + 4 * 0, names);
    put_ptr(ft + 4 * 1, pl_files);
    put_ptr(ft + 4 * 2, insignia);
    put_ptr(ft + 4 * 3, ext_map);
    put_ptr(ft + 4 * 4, costume_info);
    put_ptr(ft + 4 * 5, costume_files);
    put_ptr(ft + 4 * 6, ftdemo);
    put_ptr(ft + 4 * 7, anim_files);
    put_ptr(ft + 4 * 8, anim_num);
    put_ptr(ft + 4 * 9, effect_index);
    put_ptr(ft + 4 * 10, result_file);
    put_ptr(ft + 4 * 11, result_scale);
    put_ptr(ft + 4 * 12, victory);
    put_ptr(ft + 4 * 13, announcer);
    put_ptr(ft + 4 * 14, ssm_files);
    put_ptr(ft + 4 * 17, walljump);
    put_ptr(ft + 4 * 19, item_lookup);
    put_ptr(effect, effect_files);
    put_ptr(kirby + 0x00, cap_files);
    put_ptr(kirby + 0x10, cap_effects);
    memset(effect_index, 0xFF, (size_t) internals);
    memset(cap_effects, 0xFF, (size_t) internals);

    /* The retail rows: the special fighters and the ids past the playable ones move up. */
    for (i = 0; i < RETAIL_EXTERNAL; i++) {
        u8* row = ext_map + 3 * (i < PLAYABLE_EXTERNAL ? i : i + count);
        row[0] = (u8) shifted(mu_mex_retail_ext_map[i][0], count);
        row[1] = mu_mex_retail_ext_map[i][1] == 0xFF ? 0xFF : (u8) shifted(mu_mex_retail_ext_map[i][1], count);
        row[2] = mu_mex_retail_ext_map[i][2];
        insignia[i < PLAYABLE_EXTERNAL ? i : i + count] = mu_mex_retail_insignia[i];
    }
    for (t = 0; t < FUNCTION_TABLES; t++) {
        const u32 stride = t == TWO_ENTRY_TABLE ? 8 : 4;
        put_ptr(fns + 4 * t, fn_table[t]);
        for (i = 0; i < RETAIL_INTERNAL; i++) {
            put32(fn_table[t] + stride * (u32) shifted(i, count), mu_mex_retail_functions[t][i]);
        }
    }

    for (k = 0; k < count; k++) {
        const int internal = PLAYABLE_INTERNAL + k, ext = PLAYABLE_EXTERNAL + k;
        char *name, *file, *symbol, *anim, *result, *effect_file, *effect_symbol, *cap_file, *cap_symbol,
            *cap_effect_file, *cap_effect_symbol;
        u8 *demo, *list, *ids, *parts, *joint_to_part, *part_to_joint, *parts_ext, *ext_entries;
        u32 costumes, items, word, joints, ext_count, actions, team[3];
        /* A value the game would index with past its table: the fighter is left out. */
        const char* wrong = NULL;
        int c, own = 0;
        u8 own_row[4] = { 0, 0, 0, 0 };
        u32 own_bytes = 0;
        ext_map[3 * ext] = (u8) internal;
        ext_map[3 * ext + 1] = 0xFF;
        if (!ask(ROSTER_RECORD, (unsigned char) k) || record_size < 2 || rd(1) != 1) {
            OSReport("[roster] fighter %d: the host has no record for it; its slot stays empty\n", k);
            continue;
        }
        name = rd_str();
        file = rd_str();
        symbol = rd_str();
        anim = rd_str();
        result = rd_str();
        effect_file = rd_str();
        effect_symbol = rd_str();
        cap_file = rd_str();
        cap_symbol = rd_str();
        cap_effect_file = rd_str();
        cap_effect_symbol = rd_str();
        demo = take(16);
        for (i = 0; i < 4; i++) {   /* result, intro, ending, wait */
            put_ptr(demo != NULL ? demo + 4 * i : NULL, rd_str());
        }
        actions = rd(4);
        if (actions == 0 || actions > ACTIONS) {
            wrong = "its action count is not 1 to 2048";
        }
        put32(anim_num + 8 * internal + 4, actions);
        insignia[ext] = (u8) rd(1);
        walljump[internal] = (u8) rd(1);
        word = rd(1);   /* the sound bank: its base fighter's, a retail one, or none */
        ssm_files[16 * ext] = word < RETAIL_BANKS ? (u8) word : 55;
        roster_sound[k].base = ssm_files[16 * ext];
        (void) rd(4);   /* the call of fighter.json is a sound id of another disc: the host's, after the record */
        word = rd(4);
        put32(victory + 4 * ext, word < RETAIL_SONGS ? word : 0xFFFFFFFFu);
        put32(result_scale + 4 * ext, rd(4));
        /* The team colors (red, blue, green): costumes of this fighter. */
        for (i = 0; i < 3; i++) {
            team[i] = rd(1);
            costume_info[4 * ext + 1 + i] = (u8) team[i];
        }
        costumes = rd(1);
        for (i = 0; i < 3; i++) {
            if (costumes > 0 && team[i] >= costumes) {
                wrong = "a team color is not one of its costumes";
            }
        }
        list = take(16u * (costumes > 0 ? costumes : 1));
        for (c = 0; c < (int) costumes && !record_bad && list != NULL; c++) {
            put_ptr(list + 16 * c, rd_str());
            put_ptr(list + 16 * c + 4, rd_str());
            put_ptr(list + 16 * c + 8, rd_str());
            put32(list + 16 * c + 12, rd(1));
        }
        items = rd(1);
        ids = take(2u * (items > 0 ? items : 1));
        for (c = 0; c < (int) items && !record_bad && ids != NULL; c++) {
            /* The fighter's articles: the next item kinds past the retail ones, as MexTool
             * numbers them on a disc (the record's own numbers are another disc's). */
            const u32 id = (u32) (ADDED_ITEMS + item_next++);
            (void) rd(2);
            ids[2 * c] = (u8) (id >> 8);
            ids[2 * c + 1] = (u8) id;
        }
        for (t = 0; t < FUNCTION_TABLES; t++) {
            put32(fn_table[t] + (t == TWO_ENTRY_TABLE ? 8u : 4u) * (u32) internal, rd(4));
        }
        put32(fn_table[TWO_ENTRY_TABLE] + 8u * (u32) internal + 4, rd(4));
        /* The skeleton: the joint count, the joint of each part (0xFF: none), the extra entries. */
        joints = rd(2);
        if (joints > JOINTS) {
            wrong = "its skeleton has more joints than the game keeps";
            joints = 0;   /* nothing taken for it */
        }
        parts = take(12);
        part_to_joint = take(PARTS + 2);
        joint_to_part = take(joints > 0 ? joints : 1);
        if (part_to_joint != NULL && joint_to_part != NULL && parts != NULL) {
            memset(joint_to_part, 0xFF, joints);
            for (i = 0; i < PARTS; i++) {
                const u32 joint = rd(1);
                part_to_joint[i] = (u8) joint;
                if (joint < joints) {
                    joint_to_part[joint] = (u8) i;
                } else if (joint != 0xFF) {   /* 0xFF: the fighter has no such part */
                    wrong = "its skeleton puts a part past its joints";
                }
            }
            part_to_joint[PARTS] = part_to_joint[PARTS + 1] = 0xFF;
            put_ptr(parts, joint_to_part);
            put_ptr(parts + 4, part_to_joint);
            put32(parts + 8, joints);
        }
        ext_count = rd(1);
        parts_ext = NULL;
        if (ext_count > 0) {
            parts_ext = take(8);
            ext_entries = take(4u * ext_count);
            for (i = 0; i < (int) (4u * ext_count) && ext_entries != NULL; i++) {
                ext_entries[i] = (u8) rd(1);
            }
            put_ptr(parts_ext, ext_entries);
            put32(parts_ext != NULL ? parts_ext + 4 : NULL, ext_count);
        }
        /* Its sounds (what the host adds after the record): a fault there costs only them. */
        if (!record_bad) {
            own = read_sound_tail(k, name, own_row, &own_bytes);
            record_bad = 0;
        }
        if (!record_bad && !pool_full && wrong != NULL) {
            OSReport("[roster] fighter %d (%s): %s; its slot stays empty\n", k, name != NULL ? name : "?", wrong);
            continue;
        }
        if (!record_bad && joints == 0) {
            OSReport("[roster] fighter %d (%s): its fighter.json has no skeleton (boneDefinitions); its slot stays empty\n",
                     k, name != NULL ? name : "?");
            continue;
        }
        if (record_bad || pool_full || file == NULL) {
            OSReport("[roster] fighter %d (%s): its record is %s; its slot stays empty\n", k, name != NULL ? name : "?",
                     pool_full ? "too large for the table" : "not readable");
            if (pool_full) {
                return NULL;
            }
            continue;
        }
        put_ptr(names + 4 * ext, name);
        put_ptr(pl_files + 8 * internal, file);
        put_ptr(pl_files + 8 * internal + 4, symbol);
        put_ptr(anim_files + 4 * internal, anim);
        put_ptr(result_file + 4 * ext, result);
        put_ptr(ftdemo + 4 * internal, demo);
        costume_info[4 * ext] = (u8) costumes;
        put_ptr(costume_files + 4 * internal, list);
        put32(item_lookup + 8 * internal, items);
        put_ptr(item_lookup + 8 * internal + 4, ids);
        effect_index[internal] = effect_entry(effect_file, effect_symbol);
        put_ptr(cap_files + 8 * internal, cap_file);
        put_ptr(cap_files + 8 * internal + 4, cap_symbol);
        cap_effects[internal] = effect_entry(cap_effect_file, cap_effect_symbol);
        roster_parts[k] = parts;
        roster_parts_ext[k] = parts_ext;
        roster_series[k] = insignia[ext];
        roster_ready[k] = 1;
        built++;
        OSReport("[roster] %s (%s): internal %d, external %d, %u costumes\n", name != NULL ? name : "?", file, internal,
                 ext, (unsigned) costumes);
        put32(announcer + 4 * ext, roster_sound[k].fallback);
        if (own != 0) {
            const u32 length = (u32) strlen(own_files[k]);
            char* bank_file = (char*) take(length + 1);
            if (bank_file == NULL) {
                OSReport("[roster] %s: no room left in the table for its sound bank; its base fighter's sounds\n",
                         name != NULL ? name : "?");
                roster_sound[k].own = 0;
            } else {
                memcpy(bank_file, own_files[k], length + 1);
                ssm_files[16 * ext] = (u8) own;
                put_ptr(ssm_names + 4 * own, bank_file);
                put32(ssm_sizes + 8 * own, own_bytes);
                memcpy(ssm_rows + 4 * own, own_row, 4);
                if (own + 1 > own_banks_high) {
                    own_banks_high = own + 1;
                }
                OSReport("[roster] %s: its own sound bank %d (%s, %u sample bytes, %u scripts) in place of bank %d\n",
                         name != NULL ? name : "?", own, bank_file, (unsigned) own_bytes,
                         (unsigned) roster_sound[k].scripts, (int) roster_sound[k].base);
            }
        }
        OSReport("[roster] %s: name call %u (base fighter's %u)\n", name != NULL ? name : "?",
                 (unsigned) roster_sound[k].call, (unsigned) roster_sound[k].fallback);
    }
    OSReport("[roster] this disc has no m-ex table: one built here for %d installed fighters (%u bytes)\n", built,
             (unsigned) pool_used);
    put32(meta + 0x24, (u32) effect_next);
    put32(meta + 0x1C, (u32) own_banks_high);
    roster_built = built > 0;
    roster_count = built > 0 ? count : 0;
    return built > 0 ? (MexData*) root : NULL;
}
