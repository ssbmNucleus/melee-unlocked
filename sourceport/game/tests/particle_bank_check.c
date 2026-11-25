/* A particle bank pair from a mod file (a stage's map_ptcl / map_texg, an effect file) names its
 * own counts and offsets, and relocating the banks adds addresses to the words they name
 * (particle.c ps_banks_check, run before psInitDataBankLocate and psInitDataBankLoad). A
 * well-formed pair passes, as offsets and as relocated addresses; a count one past the file, or an
 * offset one past it, is refused. */
#include <sysdolphin/baselib/psstructs.h>
#include <mu_disc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mu_addr32_failed(const void* ptr, const char* file, int line)
{
    printf("FAIL: %p (%s:%d) is not a 32-bit address\n", ptr, file, line);
    exit(2);
}

/* A command bank: version 0, one command list at offset 12. */
enum { CMD_LIST = 12, CMD_SIZE = CMD_LIST + sizeof(HSD_PSCmdList) };
/* A texture bank: one group at offset 8 with one 32-byte texture after it. */
enum { TEX_GROUP = 8, TEX_IMAGE = TEX_GROUP + 0x1C, TEX_SIZE = TEX_IMAGE + 32 };
/* A form bank matching it: one group at offset 8 with one form after it. */
enum { FORM_GROUP = 8, FORM_DATA = FORM_GROUP + 8, FORM_SIZE = FORM_DATA + 16 };

static _Alignas(32) u8 cmd[CMD_SIZE];
static _Alignas(32) u8 tex[TEX_SIZE];
static _Alignas(32) u8 form[FORM_SIZE];
static bool rooms_known = true;

/* The data block each bank lies in: the bank itself, as when the file holds exactly it. */
mu_u32 mu_archive_room(const void* p)
{
    if (!rooms_known) return 0;
    if (p == cmd) return sizeof cmd;
    if (p == tex) return sizeof tex;
    if (p == form) return sizeof form;
    return 0;
}

#include "ps_word_body.h"
#include "ps_room_body.h"
#include "ps_offset_body.h"
#include "ps_banks_check_body.h"

static void word(u8* bank, int i, u32 v) { BEV(((be_u32*) bank)[i]) = v; }

/* The pair as a file holds it (offsets), or relocated (addresses). */
static void well_formed(bool located)
{
    const u32 c = located ? mu_addr32(cmd) : 0, t = located ? mu_addr32(tex) : 0,
              f = located ? mu_addr32(form) : 0;
    HSD_PSTexGroup* group = (HSD_PSTexGroup*) (tex + TEX_GROUP);
    HSD_PSFormGroup* forms = (HSD_PSFormGroup*) (form + FORM_GROUP);
    memset(cmd, 0, sizeof cmd);
    memset(tex, 0, sizeof tex);
    memset(form, 0, sizeof form);
    word(cmd, 1, 1);
    word(cmd, 2, c + CMD_LIST);
    word(tex, 0, 1);
    word(tex, 1, t + TEX_GROUP);
    group->num = 1;
    group->fmt = 4;
    group->width = group->height = 4;
    group->texTable[0].raw = t + TEX_IMAGE;
    word(form, 0, 1);
    word(form, 1, f + FORM_GROUP);
    forms->num = 1;
    forms->formTable[0].raw = f + FORM_DATA;
    rooms_known = true;
}

static int failures;
static void expect(bool refused, bool located, bool with_forms, const char* what)
{
    const char* why = ps_banks_check(cmd, tex, with_forms ? form : NULL, located);
    if ((why != NULL) != refused) {
        printf("FAIL: %s (%s)\n", what, why ? why : "passed");
        failures++;
    }
    well_formed(false);
}

int main(void)
{
    HSD_PSTexGroup* group = (HSD_PSTexGroup*) (tex + TEX_GROUP);
    HSD_PSFormGroup* forms = (HSD_PSFormGroup*) (form + FORM_GROUP);

    well_formed(false);
    expect(false, false, true, "a well-formed bank pair passes as offsets");
    well_formed(true);
    expect(false, true, true, "a well-formed bank pair passes relocated");
    rooms_known = false;
    expect(false, false, false, "banks outside any noted block are held by their counts alone");

    word(cmd, 1, CMD_SIZE / 4 - 1);
    expect(true, false, false, "a command list count one past the file is refused");
    word(cmd, 2, CMD_SIZE - sizeof(HSD_PSCmdList) + 4);
    expect(true, false, false, "a command list four bytes past the file is refused");
    BEV(*(be_u16*) cmd) = 0x0100;
    expect(true, false, false, "a command bank of an unknown version is refused");
    word(tex, 0, TEX_SIZE / 4);
    expect(true, false, false, "a texture group count one past the file is refused");
    word(tex, 1, TEX_SIZE - 0x18 + 4);
    expect(true, false, false, "a texture group four bytes past the file is refused");
    group->num = 1 + (TEX_SIZE - (TEX_GROUP + 0x18)) / 4;
    expect(true, false, false, "a texture table one entry past the file is refused");
    group->texTable[0].raw = TEX_SIZE;
    expect(true, false, false, "a texture one past the file is refused");
    group->palnum = 257;
    expect(true, false, false, "a texture group with 257 palettes is refused");
    word(form, 0, 2);
    expect(true, false, true, "a form bank whose group count differs from the texture bank's is refused");
    forms->num = 1 + (FORM_SIZE - (FORM_GROUP + 4)) / 4;
    expect(true, false, true, "a form table one entry past the file is refused");
    forms->formTable[0].raw = FORM_SIZE;
    expect(true, false, true, "a form one past the file is refused");
    well_formed(true);
    word(tex, 1, mu_addr32(tex) + TEX_SIZE);
    expect(true, true, false, "a relocated texture group past the file is refused");

    printf("native particle bank check: %d failures\n", failures);
    return failures ? 1 : 0;
}
