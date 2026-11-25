/* Opponents' skins, the game's half: one port's fighter wears its own costume file.
 *
 * The game keeps one costume per (fighter, colour) and every port that picks it shares it
 * (ft/ftdata.c, CostumeListsForeachCharacter). Here a fighter can have a record of its own instead:
 * the archive, its joint and its material animation, read from bytes the host hands over for that
 * port (port/runtime/host/opponent_skins.h served_costume: an opponent's skin it fetched and checked,
 * the local player's own skin, or the retail file where this machine's own skin would otherwise
 * show on another port), or the installed skin the port's door picked on the select screen
 * (port/runtime/host/costume_slots.h, shim/mu_css_skins.c). Fighter_Create calls mu_port_costume_prepare after the slot's own costume is
 * loaded; Fighter_UnkUpdateCostumeJoint_800686E4 (ft/fighter.c) and ftAnim_80070308 (ft/ftanim.c) then
 * take this fighter's joint and material animation. Nothing else changes: fp->costume_id and
 * everything indexed by costume number stay the slot's, and Jigglypuff's hats stay the retail ones.
 *
 * What the simulation reads from a costume is its skeleton (rest poses feed hurtboxes, joint-bound
 * hitboxes and the ECB). The host only serves a file whose every skeleton joint is the slot's retail
 * one bit for bit (costume_served_form.h), so both machines simulate the same fighter whatever each
 * draws. The bytes pass the archive check every file passes when it is loaded (HSD_ArchiveParse:
 * HSD_ArchiveCheck and the relocation check); a file that fails, a symbol that is missing, too little
 * room in the heap or anything the host does not have ready means the default costume. Nothing waits:
 * the host answers at once with what it has.
 *
 * Memory: the file and its archive header come from the scene's HSD heap (heap 0), where the game
 * puts any archive it was not able to preload (lb/lbarchive.c loadArchive), and only while at least
 * kRoomAfter bytes stay free after it. The heap is made anew at the next scene (lbHeap_80015900), which
 * returns the memory; the record is forgotten when its fighter is (Fighter_Unload_8006DABC), never
 * freed by hand while the fighter's models may still point into it.
 *
 * The online VS splash (gm/gm_1832.c, Slippi's use of the Classic intro) poses each player's model
 * before the match. Its fighters are demo fighters in the splash's own player slots, so they are
 * handed what the match port they stand for will wear (shim/mu_slippi_menu.c splash_prep keeps which
 * port each slot shows): the same question to the host as the match's fighter asks a scene later,
 * so the same served form or the same retail fallback. A splash record is marked and forgotten
 * before any later fighter is made (the splash's heap is gone by then), so the match and the results
 * find the records free exactly as they did before the splash asked.
 *
 * With the "opponentskins" setting off and no installed skin picked on the select screen, the host
 * answers "none" for every port and nothing here allocates or changes anything.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <melee/ft/ftdata.h>
#include <melee/ft/types.h>
#include <melee/lb/lbheap.h>
#include <sysdolphin/baselib/archive.h>

#include "mu_native.h"
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
extern Fighter_CostumeStrings* ftData_803C2360[];

#define SUB_PORT_COSTUME 7
enum { OP_INFO = 0, OP_BYTES = 1 };
enum { RECORDS = 8 };                          /* four ports, and Nana beside her port's Popo */
enum { REPLY_MIN = 4096 };                     /* the host's least reply buffer (MU_SLIPPI_RESPONSE_CAPACITY) */
#define kMaxBytes (16u << 20)                  /* far past any costume (the largest Vault skin is 3.9 MB) */
#define kRoomAfter (6u << 20)                  /* what the match keeps free in the HSD heap after a costume */

typedef struct PortCostume {
    Fighter* fp;                               /* the fighter wearing it; NULL: a free record */
    HSD_Archive* archive;
    HSD_Joint* joint;
    HSD_MatAnimJoint* matanim;
    int splash;                                /* made for the online VS splash's model */
} PortCostume;

static PortCostume mu_port_costumes[RECORDS];

static PortCostume* find(const Fighter* fp)
{
    int i;
    for (i = 0; i < RECORDS; i++) {
        if (mu_port_costumes[i].fp == fp && fp != NULL) {
            return &mu_port_costumes[i];
        }
    }
    return NULL;
}

/* {7, op, port, the slot's file name}; 0 when the name does not fit. */
static unsigned int ask(unsigned char* payload, int op, int port, const char* slot)
{
    const unsigned int n = (unsigned int) strlen(slot);
    if (n == 0 || n > 31) {
        return 0;
    }
    payload[0] = SUB_PORT_COSTUME;
    payload[1] = (unsigned char) op;
    payload[2] = (unsigned char) port;
    memcpy(payload + 3, slot, n);
    return 3 + n;
}

/* The splash's records end with its scene, whose fighters are never unloaded one by one. */
static void forget_splash(void)
{
    int i;
    for (i = 0; i < RECORDS; i++) {
        if (mu_port_costumes[i].splash) {
            memset(&mu_port_costumes[i], 0, sizeof mu_port_costumes[i]);
        }
    }
}

void mu_port_costume_splash_begin(void)
{
    forget_splash();
}

/* `port`: whose costume the host is asked for (the fighter's own player slot everywhere but on the
 * splash). */
static void prepare(Fighter* fp, int port, int splash)
{
    static unsigned char info[REPLY_MIN];
    unsigned char payload[3 + 32];
    unsigned int n, got = 0, size, capacity;
    const Fighter_CostumeStrings* names;
    PortCostume* record;
    u8* data;
    HSD_Archive* archive;
    HSD_Joint* joint;
    HSD_MatAnimJoint* matanim = NULL;
    long room;
    int i;

    if (!splash) {
        forget_splash();
    }
    mu_css_skins_port_created(port);   /* the HUD asks again what this port wears */
    /* A record left by a fighter at this address that was never released is not this one's. */
    if ((record = find(fp)) != NULL) {
        memset(record, 0, sizeof *record);
    }
    names = &ftData_803C2360[fp->kind][fp->costume_id];
    if (names->dat_filename == NULL || names->joint_name == NULL ||
        (n = ask(payload, OP_INFO, port, names->dat_filename)) == 0)
    {
        return;
    }
    if (mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, info, sizeof info, &got) != 0 || got < 5 ||
        info[0] != 1)
    {
        return;   /* the host has nothing for this port: the slot's own costume */
    }
    size = (u32) info[1] << 24 | (u32) info[2] << 16 | (u32) info[3] << 8 | info[4];
    if (size < 0x20 || size > kMaxBytes) {
        OSReport("opponent skins: port %d: %s of %u bytes refused; the default costume\n", port,
                 names->dat_filename, size);
        return;
    }
    capacity = OSRoundUp32B(size);
    if (capacity < REPLY_MIN) {
        capacity = REPLY_MIN;
    }
    room = lbHeap_HsdFreeBytes(0);
    if (room < 0 || (unsigned long) room < (unsigned long) capacity + sizeof(HSD_Archive) + kRoomAfter) {
        OSReport("opponent skins: port %d: no room for %s (%u bytes, %ld free); the default costume\n",
                 port, names->dat_filename, size, room);
        return;
    }
    for (i = 0; i < RECORDS && mu_port_costumes[i].fp != NULL; i++) {
    }
    if (i == RECORDS) {
        return;
    }
    record = &mu_port_costumes[i];
    data = lbHeap_80015BD0(0, capacity);
    archive = lbHeap_80015BD0(0, sizeof(HSD_Archive));
    if (data == NULL || archive == NULL) {
        if (data != NULL) {
            lbHeap_80015CA8(0, data);
        }
        if (archive != NULL) {
            lbHeap_80015CA8(0, archive);
        }
        OSReport("opponent skins: port %d: %s could not be allocated; the default costume\n", port,
                 names->dat_filename);
        return;
    }
    n = ask(payload, OP_BYTES, port, names->dat_filename);
    got = 0;
    /* The same archive check every file the game loads passes (HSD_ArchiveParse); then the slot's own
     * symbols, which the host renamed the skin to. */
    if (mu_online_abi_command(MU_CMD_MOD_BROWSER, payload, n, data, capacity, &got) != 0 || got != size ||
        HSD_ArchiveParse(archive, data, size) != 0 ||
        (joint = HSD_ArchiveGetPublicAddress(archive, names->joint_name)) == NULL ||
        (names->matanim_joint_name != NULL &&
         (matanim = HSD_ArchiveGetPublicAddress(archive, names->matanim_joint_name)) == NULL))
    {
        OSReport("opponent skins: port %d: %s (%u bytes, %u received) did not load; the default costume\n",
                 port, names->dat_filename, size, got);
        lbHeap_80015CA8(0, archive);
        lbHeap_80015CA8(0, data);
        return;
    }
    record->fp = fp;
    record->archive = archive;
    record->joint = joint;
    record->matanim = matanim;
    record->splash = splash;
    if (splash) {
        OSReport("opponent skins: the VS splash shows port %d in its own %s (%u bytes; %ld bytes free in the heap before)\n",
                 port, names->dat_filename, size, room);
        return;
    }
    OSReport("opponent skins: port %d wears its own %s (%u bytes; %ld bytes free in the heap before)\n",
             port, names->dat_filename, size, room);
}

void mu_port_costume_prepare(Fighter* fp)
{
    if (fp != NULL) {
        prepare(fp, fp->player_idx, 0);
    }
}

/* The results screen's fighters (gm/gm_1798.c fn_8017A67C) are made by ftDemo_CreateFighter, each in
 * its match port's player slot: while one is made, it is handed what that port's fighter wore in the
 * match, which also makes the results' pictures of it (rendered from that model) show the skin. Every
 * other demo fighter (intros, movies, trophies) keeps its slot's own costume, but for the online VS
 * splash's (above), which stand for the match port mu_slippi_splash_slot_port names. */
static int demo_is_port;

void mu_port_costume_demo(int on)
{
    demo_is_port = on;
}

void mu_port_costume_prepare_demo(Fighter* fp)
{
    int port;
    if (fp == NULL) {
        return;
    }
    if (demo_is_port) {
        mu_port_costume_prepare(fp);
    } else if (mu_slippi_splash_active()) {
        if ((port = mu_slippi_splash_slot_port(fp->player_idx)) >= 0) {
            prepare(fp, port, 1);
        }
    } else {
        forget_splash();
    }
}

HSD_Joint* mu_port_costume_joint(Fighter* fp, HSD_Joint* standard)
{
    const PortCostume* record = find(fp);
    return record != NULL ? record->joint : standard;
}

HSD_MatAnimJoint* mu_port_costume_matanim(Fighter* fp, HSD_MatAnimJoint* standard)
{
    const PortCostume* record = find(fp);
    return record != NULL ? record->matanim : standard;
}

void mu_port_costume_release(Fighter* fp)
{
    PortCostume* record = find(fp);
    if (record != NULL) {
        memset(record, 0, sizeof *record);
    }
}
