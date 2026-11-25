/* A stage's collision from a mod file names its own counts, ranges and indices, and the game
 * indexes its fixed heap arrays with them (mplib.c mpCollDataCheck, run before mpLibLoad copies
 * anything). A well-formed collision passes; a count one past its array, or an index one past a
 * count, is refused. */
#include <melee/mp/types.h>
#include <mu_disc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static size_t const groundCollVtx_count = 2048;
static size_t const groundCollLine_count = 1536;
static size_t const groundCollJoint_count = 256;

static Vec2_BE verts[3];
static MapLine lines[2];
static MapJoint joints[1];
static MapCollData cd;
static bool rooms_known = true;

void mu_addr32_failed(const void* ptr, const char* file, int line)
{
    printf("FAIL: %p (%s:%d) is not a 32-bit address\n", ptr, file, line);
    exit(2);
}

/* The data block each array lies in: the array itself, as when the file holds exactly it. */
mu_u32 mu_archive_room(const void* p)
{
    if (!rooms_known) return 0;
    if (p == verts) return sizeof verts;
    if (p == lines) return sizeof lines;
    if (p == joints) return sizeof joints;
    return 0;
}

#include "mpRangeOk_body.h"
#include "mpCollDataCheck_body.h"

/* Three vertices, two floor lines end to end, one joint holding them all. */
static void well_formed(void)
{
    int g;
    for (g = 0; g < 3; g++) {
        verts[g].x = (float) g;
        verts[g].y = 0.0F;
    }
    lines[0].v0_idx = 0; lines[0].v1_idx = 1;
    lines[0].prev_id0 = -1; lines[0].next_id0 = 1; lines[0].prev_id1 = -1; lines[0].next_id1 = -1;
    lines[1].v0_idx = 1; lines[1].v1_idx = 2;
    lines[1].prev_id0 = 0; lines[1].next_id0 = -1; lines[1].prev_id1 = -1; lines[1].next_id1 = -1;
    for (g = 0; g < MapLineGroup_Count; g++) {
        cd.ranges[g].start = 0;
        cd.ranges[g].count = g == MapLineGroup_Floor ? 2 : 0;
        joints[0].ranges[g] = cd.ranges[g];
    }
    joints[0].vtx_start = 0;
    joints[0].vtx_count = 3;
    DISC_SET(cd.verts, verts);
    DISC_SET(cd.lines, lines);
    DISC_SET(cd.joints, joints);
    cd.vert_count = 3;
    cd.line_count = 2;
    cd.joint_count = 1;
    rooms_known = true;
}

static int failures;
static void expect(bool refused, const char* what)
{
    const char* why = mpCollDataCheck(&cd);
    if ((why != NULL) != refused) {
        printf("FAIL: %s (%s)\n", what, why ? why : "passed");
        failures++;
    }
    well_formed();
}

int main(void)
{
    well_formed();
    expect(false, "a well-formed collision passes");
    rooms_known = false;
    expect(false, "a collision outside any noted block is held by its counts alone");

    cd.line_count = 3;
    cd.ranges[MapLineGroup_Floor].count = 3;
    expect(true, "a line count one past the file's line array is refused");
    cd.vert_count = (int) groundCollVtx_count + 1;
    rooms_known = false;
    expect(true, "a vertex count one past the game's vertex array is refused");
    cd.joint_count = 0;
    expect(true, "a collision with no joint is refused");
    cd.ranges[MapLineGroup_Floor].start = 1;
    expect(true, "a line group one past the line count is refused");
    joints[0].ranges[MapLineGroup_Ceiling].count = -1;
    expect(true, "a joint's line group with a negative count is refused");
    joints[0].vtx_count = 4;
    expect(true, "a joint's vertices one past the vertex count are refused");
    lines[1].v1_idx = 3;
    expect(true, "a line naming a vertex one past the count is refused");
    lines[0].next_id0 = 2;
    expect(true, "a line naming a neighbour one past the count is refused");
    lines[0].prev_id1 = -2;
    expect(true, "a line naming a negative neighbour other than -1 is refused");
    DISC_SET(cd.lines, NULL);
    expect(true, "lines named by a count but absent are refused");

    printf("native stage collision check: %d failures\n", failures);
    return failures ? 1 : 0;
}
