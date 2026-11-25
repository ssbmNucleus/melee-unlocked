/* The data blocks of the archives the game parsed most recently, so a loader can keep a count or
 * an offset it reads from a file inside that file.
 *
 * The archive check (HSD_ArchiveCheck, host/dat_check.h) proves only the words named in the
 * relocation table. Counts, indices and offsets private to a structure (a stage's collision
 * arrays, a particle bank's tables, a joint's particle list) are whatever the file says, and the
 * loaders index fixed arrays or write through them. HSD_ArchiveParse and lbArchiveRelocate note
 * each block here; mu_archive_room(p) answers how many bytes of the block that holds p follow p.
 *
 * Newest first: memory a freed archive used is reused by later ones, and the newest block that
 * holds a live archive's data is that archive's own. An address in no block noted lately (static
 * data, a block pushed out of the ring) answers 0, "unknown", and callers then keep only their
 * count checks, so retail data that is not in an archive behaves as before.
 *
 * Not simulation state: the ring only ever moves forward, and a rollback restoring an older ring
 * would bound a live archive by a dead one's block. It is left out of snapshots. */
#include "mu_native.h"

#define MU_DATA_BLOCKS 256

static struct {
    mu_uptr lo, hi;
} mu_data_blocks[MU_DATA_BLOCKS];
static unsigned int mu_data_block_count; /* blocks noted so far; the ring holds the last 256 */

void mu_archive_note_block(const void* data, mu_u32 size)
{
    unsigned int slot;
    if (data == NULL || size == 0) {
        return;
    }
    slot = mu_data_block_count++ % MU_DATA_BLOCKS;
    mu_data_blocks[slot].lo = (mu_uptr) data;
    mu_data_blocks[slot].hi = (mu_uptr) data + size;
}

mu_u32 mu_archive_room(const void* p)
{
    const mu_uptr a = (mu_uptr) p;
    unsigned int n = mu_data_block_count < MU_DATA_BLOCKS ? mu_data_block_count : MU_DATA_BLOCKS;
    unsigned int i;
    for (i = 1; i <= n; i++) {
        const unsigned int slot = (mu_data_block_count - i) % MU_DATA_BLOCKS;
        if (a >= mu_data_blocks[slot].lo && a < mu_data_blocks[slot].hi) {
            return (mu_u32) (mu_data_blocks[slot].hi - a);
        }
    }
    return 0;
}

/* The start of the newest block noted lately that holds p (an archive's data, just after its
 * header), or NULL. The online select's Options (shim/mu_online_options.c) finds the select's own
 * archive by an address inside it. */
const void* mu_archive_block_start(const void* p)
{
    const mu_uptr a = (mu_uptr) p;
    unsigned int n = mu_data_block_count < MU_DATA_BLOCKS ? mu_data_block_count : MU_DATA_BLOCKS;
    unsigned int i;
    for (i = 1; i <= n; i++) {
        const unsigned int slot = (mu_data_block_count - i) % MU_DATA_BLOCKS;
        if (a >= mu_data_blocks[slot].lo && a < mu_data_blocks[slot].hi) {
            return (const void*) mu_data_blocks[slot].lo;
        }
    }
    return NULL;
}

MU_EXCLUSIONS(data_guard, MU_EXCLUDE(mu_data_blocks), MU_EXCLUDE(mu_data_block_count))
