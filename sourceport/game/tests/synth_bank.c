/* Run the production bank-removal path with overlapping IDs and hash collisions.
 * Static allocations let us inspect freed-node reachability without dereferencing
 * released storage. Only the allocator and diagnostic sink are substituted. */
#include "sysdolphin/baselib/synth.c"

extern int printf(const char*, ...);
static void* freed[8];
static int free_count, failures;
void mu_native_free(void* p) { freed[free_count++] = p; }
void OSReport(char* fmt, ...) { (void) fmt; }

static void check(int condition, const char* why)
{
    if (!condition) { printf("FAIL: %s\n", why); failures++; }
}

static void scenario(int remove_newest)
{
    struct SfxEntryHost older[2] = { 0 }, newer[2] = { 0 }, collision = { 0 };
    struct SfxBankNode first = { 0 }, second = { 0 };
    int i;
    memset(hsd_SynthSFXDataHash, 0, sizeof(hsd_SynthSFXDataHash));
    memset(HSD_Synth_804C2AE0, 0, sizeof(HSD_Synth_804C2AE0));
    free_count = 0;
    first.entrynum = 101; first.base = 295; first.count = 2; first.entries = older;
    second.entrynum = 102; second.base = 295; second.count = 2; second.entries = newer;
    first.next = &second;
    HSD_Synth_804C2AE0[3] = &first;
    collision.unk4 = 327;
    hsd_SynthSFXDataHash[327 & 31] = &collision;
    for (i = 0; i < 2; i++) {
        int id = 295 + i;
        older[i].unk4 = newer[i].unk4 = id;
        older[i].next = hsd_SynthSFXDataHash[id & 31];
        newer[i].next = &older[i];
        hsd_SynthSFXDataHash[id & 31] = &newer[i];
    }
    HSD_SynthSFXGroupDataRemove(remove_newest ? 102 : 101);
    for (i = 0; i < 2; i++) {
        struct SfxEntryHost* survivor = remove_newest ? &older[i] : &newer[i];
        check(hsd_SynthSFXDataHash[(295 + i) & 31] == survivor,
              "removing a bank preserves the other bank's same-ID entry");
        check(survivor->next == (i ? NULL : &collision),
              "no freed bank entry remains reachable; collision survives");
    }
    check(free_count == 2 && freed[0] == (void*)(remove_newest ? newer : older),
          "removed bank releases its own entries and descriptor");
    HSD_SynthSFXGroupDataRemove(remove_newest ? 101 : 102);
    check(free_count == 4 && HSD_Synth_804C2AE0[3] == NULL,
          "both descriptors removed exactly once");
    check(hsd_SynthSFXDataHash[295 & 31] == &collision &&
          hsd_SynthSFXDataHash[296 & 31] == NULL,
          "all bank entries removed with collision retained");
    HSD_SynthSFXDataUnlink(327);
    check(hsd_SynthSFXDataHash[327 & 31] == NULL,
          "explicit unlink by ID retains its original behavior");
}

static void put32(u8* p, u32 v)
{
    p[0] = (u8) (v >> 24); p[1] = (u8) (v >> 16); p[2] = (u8) (v >> 8); p[3] = (u8) v;
}

/* A bank's head and sound table: a well-formed two-sound bank passes; one number past what it
 * holds is refused instead of sizing memory from it. */
static void bank_checks(void)
{
    u8 head[0x20] = { 0 }, table[2 * 0x48] = { 0 };
    const u64 file = 0xA0 + 0x40, aram = 0x100000;
    put32(head, 0x90); put32(head + 4, 0x40); put32(head + 8, 2);
    check(synth_sfx_head_check((const be_u32*) head, file, aram) == NULL, "well-formed bank head passes");
    check(synth_sfx_head_check((const be_u32*) head, 0, aram) == NULL, "unknown file length is not refused");
    put32(head + 8, 3);
    check(synth_sfx_head_check((const be_u32*) head, file, aram) != NULL, "a sound count one past the table is refused");
    put32(head + 8, 2); put32(head + 4, 0x41);
    check(synth_sfx_head_check((const be_u32*) head, file, aram) != NULL, "a sample size off the block size is refused");
    put32(head + 4, 0x40);
    check(synth_sfx_head_check((const be_u32*) head, file - 1, aram) != NULL, "samples one byte past the file are refused");
    check(synth_sfx_head_check((const be_u32*) head, file, 0x20) != NULL, "samples past the sound memory are refused");
    put32(head, 0x100004);
    check(synth_sfx_head_check((const be_u32*) head, 0, aram) != NULL, "a table past 1 MB is refused");

    put32(table, 1); put32(table + 4, 32000);
    put32(table + 0x48, 1); put32(table + 0x48 + 4, 48000);
    check(synth_sfx_table_check(table, sizeof(table), 2) == NULL, "well-formed sound table passes");
    check(synth_sfx_table_check(table, sizeof(table), 3) != NULL, "a third sound past the table is refused");
    put32(table + 0x48 + 4, 48001);
    check(synth_sfx_table_check(table, sizeof(table), 2) != NULL, "a rate one past 48 kHz is refused");
    put32(table + 0x48 + 4, 3999);
    check(synth_sfx_table_check(table, sizeof(table), 2) != NULL, "a rate under 4 kHz is refused");
    put32(table + 0x48 + 4, 48000); put32(table + 0x48, 3);
    check(synth_sfx_table_check(table, sizeof(table), 2) != NULL, "a sound with three channels is refused");
    put32(table + 0x48, 2);
    check(synth_sfx_table_check(table, sizeof(table), 2) != NULL, "a second channel past the table is refused");

    /* A music block fills one 64 KB stream slot at most, in whole 32-byte transfers. */
    BEV(*(be_s32*) &pstHakoHeader[1].x0) = 0x10000;
    check(synth_hako_ok(1), "a 64 KB music block passes");
    BEV(*(be_s32*) &pstHakoHeader[1].x0) = 0x10020;
    check(!synth_hako_ok(1), "a music block one transfer past 64 KB is refused");
    BEV(*(be_s32*) &pstHakoHeader[1].x0) = 0x1F;
    check(!synth_hako_ok(1), "a music block off the transfer size is refused");
    BEV(*(be_s32*) &pstHakoHeader[1].x0) = 0;
    check(!synth_hako_ok(1), "an empty music block is refused");
}

int main(void)
{
    scenario(0);
    scenario(1);
    bank_checks();
    printf("native sound banks: %d failures\n", failures);
    return failures ? 1 : 0;
}
