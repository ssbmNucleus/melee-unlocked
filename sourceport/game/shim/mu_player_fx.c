/* Per-player effect files, the game's half (prototype; the design and the hooks: mu_player_fx.h).
 *
 * Load: Fighter_Create (ft/fighter.c) calls mu_fx_player_prepare after the fighter's own effect file
 * is loaded. The host is asked whether this port has a variant of that file (MU_CMD_MOD_BROWSER 19,
 * port/runtime/host/player_fx_test.cpp; only with MELEE_TEST_PLAYER_FX set, and never online or in a
 * replay). The bytes go to the scene's HSD heap (heap 0) as the opponent skins' do (mu_port_costume.c),
 * pass the archive check, must have the file's table with the same generator ids and texture group
 * count as the loaded file, and are set up as particle bank `alias` (psInitDataBank) with the model
 * effects in efAsync_DatEntries[alias].data. efLib_Init forgets it all at the next scene; the heap is
 * made anew there.
 *
 * Trace: every spawn into a bank that has an alias is counted by the port it was traced to, and one
 * that cannot be traced is logged once per entry point and caller, with a count ("no player").
 * Spawns into such a bank made with an owner whose port differs from the port of the running proc
 * are logged the same way ("cross"; the owner wins), as a measure of what a proc-only context would
 * get wrong. The summary (every 1200 match frames and at the scene's end) has the counts by port and
 * entry point and the ids seen.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <Runtime/platform.h>
#include <dolphin/os.h>
#include <melee/ef/eflib.h>
#include <melee/ef/forward.h>
#include <melee/ef/types.h>
#include <melee/ft/forward.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/it/forward.h>
#include <melee/it/types.h>
#include <melee/lb/lbheap.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/particle.h>
#include <sysdolphin/baselib/psstructs.h>

#include <mu_commands.h>
#include <mu_player_fx.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
extern EF_DAT_Entry efAsync_DatEntries[EF_DAT_FILE_MAX + 1];
extern u8 ftData_UnkBytePerCharacter[];
extern HSD_PSFormGroup** psFormGroupArray[65];
extern s32 mu_ps_first_id[65];
extern HSD_GObjProc* HSD_GObj_CurrentInvokedProc;
extern HSD_GObj* HSD_GObj_CurrentInvokedProcGObj;

#define SUB_PLAYER_FX 19
enum { OP_INFO = 0, OP_BYTES = 1 };
enum { PORTS = 6, ALIAS_FIRST = 22, ALIAS_LAST = 29 };   /* 30 is the stage's bank (ground.c) */
enum { REPLY_MIN = 4096 };
#define kMaxBytes (4u << 20)       /* the largest retail fighter effect file is 0.2 MB */
#define kRoomAfter (2u << 20)      /* what the match keeps free in the HSD heap after a variant */

int mu_fx_on;
int mu_fx_internal;
signed char mu_fx_generation;

typedef struct Slot {
    s8 base;   /* the character's own bank (its effect file index); -1 none */
    s8 alias;  /* the variant's bank; -1 none */
    u32 size;
} Slot;
static Slot slots[PORTS] = { { -1, -1, 0 }, { -1, -1, 0 }, { -1, -1, 0 },
                             { -1, -1, 0 }, { -1, -1, 0 }, { -1, -1, 0 } };
static MuFxScope scope;   /* the open scope (active 1) */

typedef struct Note {
    u8 site;
    s8 bank;
    u16 gobj_class;    /* the running proc's gobj: class, 0xFFFF none */
    void* caller;      /* the outermost entry point's return address */
    void* proc;        /* the running proc's function */
    int first_id;
    int owner_port, proc_port;
    u32 count;
} Note;
enum { NOTES = 48 };
static Note misses[NOTES], crosses[NOTES];
static int n_misses, n_crosses;
static u32 traced[PORTS][MU_FX_SITES];   /* spawns into an aliased bank traced to a port: [port][site] */
static u32 remapped[PORTS];              /* ... of them sent to that port's variant */
static u64 seen_ids[PORTS];              /* ... which ids (id % 1000 below 64, one bit each) */
static u32 inherited;                    /* made by the particle system itself: the parent's bank */
static u32 last_block;                   /* the periodic summary's last 1200-frame block */

static void summary(const char* when);

static const char* const site_names[MU_FX_SITES] = {
    "direct hsd_8039F05C", "efSync_Spawn", "efAsync_Spawn", "efAsync_QueueProcessDeferred", "efLib_Create",
    "efLib_CreateGenerator", "efLib_SpawnParticleEffect", "efLib_Cb_DPtcl", "efLib_Cb_SPtcl",
};

/* ---- ports ---------------------------------------------------------------------------------- */

static int port_of(HSD_GObj* gobj, int depth)
{
    if (gobj == NULL || depth > 4) {
        return -1;
    }
    switch (gobj->classifier) {
    case HSD_GOBJ_CLASS_FIGHTER: {
        Fighter* fp = gobj->user_data;
        return fp != NULL && fp->player_idx < PORTS ? fp->player_idx : -1;
    }
    case HSD_GOBJ_CLASS_ITEM: {
        Item* ip = gobj->user_data;
        return ip != NULL ? port_of(ip->owner, depth + 1) : -1;
    }
    case HSD_GOBJ_CLASS_EFFECT:
        if (gobj->user_data_remove_func == efLib_remove_user_data && gobj->user_data != NULL) {
            EF_Effect* effect = gobj->user_data;
            if (effect->x2B == mu_fx_generation && effect->x2A >= 1 && effect->x2A <= PORTS) {
                return effect->x2A - 1;   /* the port it was made for (efLib_Create, this scene) */
            }
            return port_of(effect->parent_gobj, depth + 1);
        }
        return -1;
    }
    return -1;
}

static int proc_port(void)
{
    return port_of(HSD_GObj_CurrentInvokedProcGObj, 0);
}

/* A port's fighter's motion state, for the log; -1 none. */
static int motion_of(int port)
{
    HSD_GObj* gobj;
    Fighter* fp;
    if (port < 0 || port >= PORTS || Player_GetPlayerState(port) != 2 || (gobj = Player_GetEntity(port)) == NULL ||
        (fp = gobj->user_data) == NULL)
    {
        return -1;
    }
    return (int) fp->motion_id;
}

static Note* note(Note* list, int* n, int site, void* caller, int bank, int id, int owner_port, int proc,
                  const char* what)
{
    HSD_GObj* g = HSD_GObj_CurrentInvokedProcGObj;
    void* fn = HSD_GObj_CurrentInvokedProc != NULL ? (void*) HSD_GObj_CurrentInvokedProc->on_invoke : NULL;
    int i;
    for (i = 0; i < *n; i++) {
        if (list[i].site == site && list[i].caller == caller && list[i].bank == bank && list[i].proc == fn) {
            list[i].count++;
            if (list[i].count == 10 || list[i].count == 100 || list[i].count == 1000) {
                OSReport("[fx] %s: %s from %p (proc %p): %u so far\n", what, site_names[site], caller, fn,
                         list[i].count);
            }
            return &list[i];
        }
    }
    if (*n >= NOTES) {
        return NULL;
    }
    list[*n].site = (u8) site;
    list[*n].bank = (s8) bank;
    list[*n].caller = caller;
    list[*n].proc = fn;
    list[*n].gobj_class = g != NULL ? g->classifier : 0xFFFF;
    list[*n].first_id = id;
    list[*n].owner_port = owner_port;
    list[*n].proc_port = proc;
    list[*n].count = 1;
    OSReport("[fx] %s: %s from %p, bank %d id %d; running proc %p (gobj class %d), owner port %d (motion %d), "
             "proc port %d (motion %d) (match frame %u)\n",
             what, site_names[site], caller, bank, id, fn, g != NULL ? (int) g->classifier : -1, owner_port + 1,
             motion_of(owner_port), proc + 1, motion_of(proc), gm_GetFrameCount());
    return &list[(*n)++];
}

/* ---- scopes and the bank ------------------------------------------------------------------- */

MuFxScope mu_fx_scope_begin(int site, HSD_GObj* owner, int port_hint, void* caller)
{
    MuFxScope saved = scope;
    int owner_port, proc, port;
    if (!mu_fx_on) {
        saved.active = -1;   /* nothing to restore */
        return saved;
    }
    owner_port = port_of(owner, 0);
    proc = proc_port();
    port = owner_port;
    if (port < 0 && port_hint >= 0 && port_hint < PORTS) {
        port = port_hint;
    }
    if (port < 0 && scope.active == 1) {
        port = scope.port;
    }
    if (port < 0) {
        port = proc;
    }
    if (scope.active != 1) {
        scope.site = (u8) site;
        scope.caller = caller;
        scope.owner_port = (s8) owner_port;
        scope.proc_port = (s8) proc;
    }
    scope.active = 1;
    scope.port = (s8) port;
    saved.active = saved.active == 1 ? 1 : 0;
    return saved;
}

void mu_fx_scope_end(MuFxScope* saved)
{
    if (saved->active < 0) {
        return;
    }
    scope = *saved;
}

int mu_fx_scope_port(void)
{
    return mu_fx_on && scope.active == 1 ? scope.port : -1;
}

/* Some port has a variant of `bank`. */
static int aliased(int bank)
{
    int i;
    for (i = 0; i < PORTS; i++) {
        if (slots[i].alias >= 0 && slots[i].base == bank) {
            return 1;
        }
    }
    return 0;
}

static int crossed(void)
{
    return scope.active == 1 && scope.owner_port >= 0 && scope.proc_port >= 0 && scope.owner_port != scope.proc_port;
}

int mu_fx_queue_port(int gfx_id)
{
    if (!mu_fx_on) {
        return -1;
    }
    if (crossed() && aliased(gfx_id / 1000)) {
        /* queued now, made later for the owner: noted here, where the proc is still the other's */
        note(crosses, &n_crosses, MU_FX_SITE_ASYNC, scope.caller, gfx_id / 1000, gfx_id, scope.owner_port,
             scope.proc_port, "cross");
    }
    return mu_fx_scope_port();
}

int mu_fx_remap(int bank, int id)
{
    int port, site;
    if (!aliased(bank)) {
        return bank;
    }
    if (mu_fx_internal) {
        inherited++;
        return bank;
    }
    {
        /* the summary every 1200 match frames as well, for a run that stops inside the match (the
           scene's count, not a player's: Player_GetMatchFrameCount stays -1 for a CPU) */
        const u32 frame = gm_GetFrameCount();
        const u32 block = frame < 0x80000000u ? frame / 1200 : 0;
        if (block != last_block) {
            last_block = block;
            if (block != 0) {
                summary("so far");
            }
        }
    }
    if (scope.active == 1) {
        port = scope.port;
        site = scope.site;
    } else {
        port = proc_port();
        site = MU_FX_SITE_DIRECT;
    }
    if (port < 0 || port >= PORTS) {
        note(misses, &n_misses, site, scope.active == 1 ? scope.caller : NULL, bank, id, -1, proc_port(),
             "no player");
        return bank;
    }
    traced[port][site]++;
    if (id >= 0 && id % 1000 < 64) {
        seen_ids[port] |= (u64) 1 << (id % 1000);
    }
    if (crossed()) {
        /* the spawn's owner is one player, the proc running is another's: the owner wins */
        note(crosses, &n_crosses, site, scope.caller, bank, id, scope.owner_port, scope.proc_port, "cross");
    }
    if (slots[port].base == bank && slots[port].alias >= 0) {
        remapped[port]++;
        return slots[port].alias;
    }
    return bank;
}

/* The gobj whose model a joint belongs to: a fighter, an effect or an item (its root joint). */
HSD_GObj* mu_fx_jobj_owner(HSD_JObj* jobj)
{
    static const int plinks[] = { HSD_GOBJ_PLINK_FIGHTER, HSD_GOBJ_PLINK_EFFECT_UNK0, HSD_GOBJ_PLINK_EFFECT_UNK1,
                                  HSD_GOBJ_PLINK_ITEM };
    HSD_JObj* root = jobj;
    int i, guard = 0;
    if (!mu_fx_on || jobj == NULL) {
        return NULL;
    }
    while (root->parent != NULL && guard++ < 256) {
        root = root->parent;
    }
    for (i = 0; i < (int) (sizeof plinks / sizeof plinks[0]); i++) {
        HSD_GObj* g;
        for (g = HSD_GObjPLinkHead[plinks[i]]; g != NULL; g = g->next) {
            if (g->hsd_obj == root) {
                return g;
            }
        }
    }
    return NULL;
}

/* ---- load ---------------------------------------------------------------------------------- */

typedef struct MuFxTable {
    DISC_PTR(void) cmd;
    DISC_PTR(void) tex;
    u8 data[4];
} DISC_STRUCT MuFxTable;

static unsigned int ask(unsigned char* payload, int op, int port, int base, const char* file)
{
    const unsigned int n = (unsigned int) strlen(file);
    if (n == 0 || n > 31) {
        return 0;
    }
    payload[0] = SUB_PLAYER_FX;
    payload[1] = (unsigned char) op;
    payload[2] = (unsigned char) port;
    payload[3] = (unsigned char) base;
    memcpy(payload + 4, file, n);
    return 4 + n;
}

static bool alias_free(int bank)
{
    int i;
    if (efAsync_DatEntries[bank].ef_DAT_file != NULL || efAsync_DatEntries[bank].data != NULL ||
        psCmdListArray[bank] != 0 || ptclref_804D0E5C[bank] != NULL || psTexGroupArray[bank] != NULL)
    {
        return false;
    }
#if defined(MU_AKANEIA_FIGHTERS)
    {
        extern int mu_ak_effect_bank(int bank);
        if (mu_ak_effect_bank(bank)) {
            return false;   /* an added fighter's file numbers its generators from this bank */
        }
    }
#endif
    for (i = 0; i < PORTS; i++) {
        if (slots[i].alias == bank) {
            return false;
        }
    }
    return true;
}

static void clear_bank(int bank)
{
    psCmdListArray[bank] = 0;
    ptclref_804D0E5C[bank] = NULL;
    psTexGroupArray[bank] = NULL;
    psNumCmdList[bank] = NULL;
    psFormGroupArray[bank] = NULL;
    mu_ps_first_id[bank] = 0;
    efAsync_DatEntries[bank].data = NULL;
}

void mu_fx_player_prepare(Fighter* fp)
{
    static unsigned char info[REPLY_MIN];
    unsigned char payload[4 + 32];
    unsigned int n, got = 0, size, capacity;
    int port, base, alias = -1, i;
    const char* file;
    const char* table_name;
    long before, after;
    u8* data;
    HSD_Archive* archive;
    MuFxTable* table;
    void* cmd;
    void* tex;
    u16 version;
    s32 first, count, groups;

    if (fp == NULL || fp->player_idx >= PORTS) {
        return;
    }
    port = fp->player_idx;
    base = ftData_UnkBytePerCharacter[fp->kind];
    if (base <= 0 || base >= EF_DAT_FILE_MAX || base == 0xFF) {
        return;   /* no file of its own (0 is the common file: never per player) */
    }
    file = efAsync_DatEntries[base].ef_DAT_file;
    table_name = efAsync_DatEntries[base].effDataTable_name;
    if (file == NULL || table_name == NULL || efAsync_DatEntries[base].data == NULL) {
        return;
    }
    if (slots[port].alias >= 0 && slots[port].base == base) {
        return;   /* Nana beside her port's Popo: the same variant */
    }
    if ((n = ask(payload, OP_INFO, port, base, file)) == 0 ||
        mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, info, sizeof info, &got) != 0 || got < 5 ||
        info[0] != 1)
    {
        return;   /* nothing for this port: the switch is off */
    }
    size = (u32) info[1] << 24 | (u32) info[2] << 16 | (u32) info[3] << 8 | info[4];
    if (size < 0x20 || size > kMaxBytes) {
        OSReport("[fx] port %d: %s variant of %u bytes refused\n", port + 1, file, size);
        return;
    }
    for (i = ALIAS_FIRST; i <= ALIAS_LAST && !alias_free(i); i++) {
    }
    if (i > ALIAS_LAST) {
        OSReport("[fx] port %d: no free effect bank for its %s\n", port + 1, file);
        return;
    }
    alias = i;
    capacity = OSRoundUp32B(size);
    if (capacity < REPLY_MIN) {
        capacity = REPLY_MIN;
    }
    before = lbHeap_HsdFreeBytes(0);
    if (before < 0 || (unsigned long) before < (unsigned long) capacity + sizeof(HSD_Archive) + kRoomAfter) {
        OSReport("[fx] port %d: no room for its %s (%u bytes, %ld free in heap 0)\n", port + 1, file, size, before);
        return;
    }
    data = lbHeap_80015BD0(0, capacity);
    archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    if (data == NULL || archive == NULL) {
        goto fail_free;
    }
    n = ask(payload, OP_BYTES, port, base, file);
    got = 0;
    if (mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, data, capacity, &got) != 0 || got != size ||
        HSD_ArchiveParse(archive, data, size) != 0 ||
        (table = HSD_ArchiveGetPublicAddress(archive, table_name)) == NULL)
    {
        OSReport("[fx] port %d: its %s (%u bytes, %u received) did not load\n", port + 1, file, size, got);
        goto fail_free;
    }
    cmd = DP(table->cmd);
    tex = DP(table->tex);
    if (cmd == NULL || tex == NULL) {
        OSReport("[fx] port %d: its %s has no particle banks\n", port + 1, file);
        goto fail_free;
    }
    /* The same ids as the loaded file: generators first..first+count, the same texture groups. */
    version = BEV(*(const be_u16*) cmd);
    first = version == 0 ? 0 : BEV(((const be_s32*) cmd)[1]);
    count = version == 0 ? BEV(((const be_s32*) cmd)[1]) : BEV(((const be_s32*) cmd)[2]);
    groups = BEV(*(const be_s32*) tex);
    if (first != mu_ps_first_id[base] || first + count != psCmdListArray[base] ||
        groups != ((s32*) psFormGroupArray)[base])
    {
        OSReport("[fx] port %d: its %s has generators %d+%d and %d texture groups, the loaded one %d+%d and %d; "
                 "refused\n",
                 port + 1, file, first, count, groups, mu_ps_first_id[base], psCmdListArray[base] - mu_ps_first_id[base],
                 ((s32*) psFormGroupArray)[base]);
        goto fail_free;
    }
    psInitDataBank(alias, cmd, tex, NULL, NULL);
    if (ptclref_804D0E5C[alias] == NULL || psCmdListArray[alias] != psCmdListArray[base] ||
        mu_ps_first_id[alias] != mu_ps_first_id[base])
    {
        OSReport("[fx] port %d: its %s failed the particle bank check\n", port + 1, file);
        clear_bank(alias);
        goto fail_free;
    }
    efAsync_DatEntries[alias].data = (void*) table->data;
    slots[port].base = (s8) base;
    slots[port].alias = (s8) alias;
    slots[port].size = size;
    if (!mu_fx_on) {
        /* the first variant this scene: a new tag, so an effect left from an earlier scene (or one
           made before now, its x2A/x2B whatever the allocator left) is never read as tagged */
        mu_fx_generation = (signed char) (mu_fx_generation % 127 + 1);
    }
    mu_fx_on = 1;
    after = lbHeap_HsdFreeBytes(0);
    OSReport("[fx] port %d: its own %s (%u bytes) in effect bank %d for bank %d (generators %d..%d, %d texture "
             "groups); heap 0 free %ld -> %ld bytes; loaded at fighter setup, match frame %u\n",
             port + 1, file, size, alias, base, first, first + count - 1, groups, before, after,
             gm_GetFrameCount());
    return;

fail_free:
    if (archive != NULL) {
        lbHeap_80015CA8(0, archive);
    }
    if (data != NULL) {
        lbHeap_80015CA8(0, data);
    }
}

static void summary(const char* when)
{
    int i, s;
    {
        OSReport("[fx] summary (%s, match frame %u): %d no-player site(s), %d cross site(s), %u particle-system "
                 "spawns inherited\n",
                 when, gm_GetFrameCount(), n_misses, n_crosses, inherited);
        for (i = 0; i < PORTS; i++) {
            u32 total = 0;
            for (s = 0; s < MU_FX_SITES; s++) {
                total += traced[i][s];
            }
            if (total == 0) {
                continue;
            }
            OSReport("[fx]   port %d: %u spawns traced, %u into its variant (sync %u, async %u, deferred %u, create %u, "
                     "generator %u, particle %u, dptcl %u, sptcl %u, direct %u)\n",
                     i + 1, total, remapped[i], traced[i][MU_FX_SITE_SYNC], traced[i][MU_FX_SITE_ASYNC],
                     traced[i][MU_FX_SITE_DEFERRED], traced[i][MU_FX_SITE_CREATE], traced[i][MU_FX_SITE_GENERATOR],
                     traced[i][MU_FX_SITE_PARTICLE], traced[i][MU_FX_SITE_DPTCL], traced[i][MU_FX_SITE_SPTCL],
                     traced[i][MU_FX_SITE_DIRECT]);
            {
                char ids[64 * 3 + 1];
                int k, at = 0;
                for (k = 0; k < 64; k++) {
                    if (seen_ids[i] >> k & 1) {
                        ids[at++] = ' ';
                        if (k >= 10) {
                            ids[at++] = (char) ('0' + k / 10);
                        }
                        ids[at++] = (char) ('0' + k % 10);
                    }
                }
                ids[at] = '\0';
                OSReport("[fx]   port %d: ids seen (id %% 1000; model effects and generators alike):%s\n", i + 1, ids);
            }
        }
        for (i = 0; i < n_misses; i++) {
            OSReport("[fx]   no player: %s from %p, bank %d first id %d, proc %p (gobj class %d): %u\n",
                     site_names[misses[i].site], misses[i].caller, misses[i].bank, misses[i].first_id, misses[i].proc,
                     misses[i].gobj_class == 0xFFFF ? -1 : (int) misses[i].gobj_class, misses[i].count);
        }
        for (i = 0; i < n_crosses; i++) {
            OSReport("[fx]   cross: %s from %p, bank %d first id %d, owner port %d, proc port %d, proc %p: %u\n",
                     site_names[crosses[i].site], crosses[i].caller, crosses[i].bank, crosses[i].first_id,
                     crosses[i].owner_port + 1, crosses[i].proc_port + 1, crosses[i].proc, crosses[i].count);
        }
    }
}

/* efLib_Init, at every scene's start: the summary of the scene that ends, then nothing aliased. */
void mu_fx_player_reset(void)
{
    int i;
    if (!mu_fx_on) {
        return;   /* nothing was loaded: nothing here changed since the last reset */
    }
    summary("scene end");
    for (i = 0; i < PORTS; i++) {
        slots[i].base = -1;
        slots[i].alias = -1;
        slots[i].size = 0;
    }
    memset(&scope, 0, sizeof scope);
    memset(traced, 0, sizeof traced);
    memset(remapped, 0, sizeof remapped);
    memset(seen_ids, 0, sizeof seen_ids);
    last_block = 0;
    n_misses = n_crosses = 0;
    inherited = 0;
    mu_fx_on = 0;
    mu_fx_internal = 0;
}
