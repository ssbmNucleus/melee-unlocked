#include <melee/lb/lb_013B.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbcommand.h>
#include <melee/lb/types.h>
#include <sysdolphin/baselib/rumble.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* This test never emits a background command. */
void lbBgFlash_80021C48(u32 a, u32 b) { (void) a; (void) b; }

/* lb_013B.c, linked for the colour-overlay interpreter (lb_80014258), also loads the rumble
 * table and drives the pads; this test does neither. */
bool lbArchive_80017040(HSD_Archive** dst, const char* filename, void* symbols, ...)
{
    (void) dst; (void) filename; (void) symbols;
    return false;
}
int HSD_PadRumbleAdd(u8 no, int id, int frame, int pri, void* listp)
{
    (void) no; (void) id; (void) frame; (void) pri; (void) listp;
    return 0;
}
void HSD_PadRumbleRemove(u8 no) { (void) no; }
void HSD_PadRumbleOn(u8 no) { (void) no; }

/* A colour script's fighter commands (opcodes 0x15-0x17) go to the interpreter's callback, which
 * steps over them: a graphic effect is five words, as retail hit-effect scripts carry them. */
static int effect_commands;
static void colour_command(Fighter_GObj* gobj, CommandInfo* cmd, int opcode)
{
    (void) gobj;
    cmd->u += opcode == 0x15 ? 5 : opcode == 0x16 ? 3 : 1;
    ++effect_commands;
}

/* A subroutine's target is a 32-bit disc address, big-endian: the scripts live in the low image. */
static bool put_target(unsigned char* word, const unsigned char* target)
{
    u32 address = (u32) (uintptr_t) target;
    if ((uintptr_t) address != (uintptr_t) target) return false;
    word[0] = (u8) (address >> 24); word[1] = (u8) (address >> 16);
    word[2] = (u8) (address >> 8); word[3] = (u8) address;
    return true;
}

static int reports;
void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    ++reports;
}

/* Runs a script until it ends or 64 commands; returns the number executed, -1 on an opcode
 * lbcommand does not handle. */
static int run(CommandInfo* info, unsigned char* bytes)
{
    int steps = 0;
    info->u = (CmdUnion*) bytes;
    while (info->u && steps++ < 64) {
        if (!Command_Execute(info, info->u->Command_00.code)) return -1;
    }
    return steps;
}

int main(void)
{
    /* Repeat a five-frame timer three times, then terminate. */
    _Alignas(CmdUnion) unsigned char bytes[] = {
        0x0C, 0x00, 0x00, 0x03,
        0x04, 0x00, 0x00, 0x05,
        0x10, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    CommandInfo info = {0};
    int steps = 0;
    info.u = (CmdUnion*) bytes;
    info.frame_count = 123.0f;
    while (info.u && steps++ < 12) {
        if (!Command_Execute(&info, info.u->Command_00.code)) return 1;
    }
    if (info.u || steps != 8 || info.timer != 15.0f ||
        info.loop_count != 0 || info.frame_count != 123.0f) return 2;

    /* A loop inside a subroutine uses three of the stack's slots and runs as before. The
     * subroutine's target is a 32-bit disc address: the script lives in the low image. */
    {
        static _Alignas(CmdUnion) unsigned char script[] = {
            0x14, 0x00, 0x00, 0x00, 0, 0, 0, 0,   /* subroutine, target patched below */
            0x00, 0x00, 0x00, 0x00,               /* end */
            0x0C, 0x00, 0x00, 0x02,               /* target: loop twice */
            0x04, 0x00, 0x00, 0x01,               /*   timer 1 */
            0x10, 0x00, 0x00, 0x00,               /* loop end */
            0x18, 0x00, 0x00, 0x00,               /* return */
        };
        CommandInfo ci = {0};
        u32 target = (u32) (uintptr_t) (script + 12);
        if ((uintptr_t) target != (uintptr_t) (script + 12)) return 8;
        script[4] = (u8) (target >> 24); script[5] = (u8) (target >> 16);
        script[6] = (u8) (target >> 8); script[7] = (u8) target;
        reports = 0;
        if (run(&ci, script) != 8 || ci.timer != 2.0f || ci.loop_count != 0 || reports != 0)
            return 3;
    }

    /* A colour overlay's script, shaped as the retail hit-effect ones (PlCo.dat colour
     * animations 0x0B-0x1E and 0x23-0x26: fire, electric, darkness and the like): a loop around
     * a subroutine that runs a loop of its own. That is five stack words, all the console has,
     * and the overlay has five slots: the script runs to its end on frame 17, with no report and
     * nothing written past the stack. */
    {
        static _Alignas(CmdUnion) unsigned char script[] = {
            /*   0 */ 0x0C, 0x00, 0x00, 0x02,                      /* loop twice */
            /*   4 */ 0x54, 0x00, 0x00, 0x00, 0, 0, 0, 0,          /*   graphic effect, */
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          /*   five words */
            /*  24 */ 0x14, 0x00, 0x00, 0x00, 0, 0, 0, 0,          /*   subroutine flash */
            /*  32 */ 0x10, 0x00, 0x00, 0x00,                      /* loop end */
            /*  36 */ 0x0C, 0x00, 0x00, 0x03,                      /* loop three times */
            /*  40 */ 0x14, 0x00, 0x00, 0x00, 0, 0, 0, 0,          /*   subroutine pulse */
            /*  48 */ 0x10, 0x00, 0x00, 0x00,                      /* loop end */
            /*  52 */ 0x28, 0x00, 0x00, 0x00,                      /* end: the overlay is reset */
            /*  56 */ 0x48, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x80, /* flash: colour */
            /*  64 */ 0x2C, 0x00, 0x00, 0x01,                      /*   wait 1 */
            /*  68 */ 0x30, 0x00, 0x00, 0x00,                      /*   colour off */
            /*  72 */ 0x2C, 0x00, 0x00, 0x01,                      /*   wait 1 */
            /*  76 */ 0x18, 0x00, 0x00, 0x00,                      /*   return */
            /*  80 */ 0x0C, 0x00, 0x00, 0x02,                      /* pulse: loop twice */
            /*  84 */ 0x48, 0x00, 0x00, 0x00, 0x80, 0x00, 0xC0, 0x60, /*   colour */
            /*  92 */ 0x2C, 0x00, 0x00, 0x01,                      /*   wait 1 */
            /*  96 */ 0x48, 0x00, 0x00, 0x00, 0x20, 0x00, 0x40, 0xC0, /*   colour */
            /* 104 */ 0x2C, 0x00, 0x00, 0x01,                      /*   wait 1 */
            /* 108 */ 0x10, 0x00, 0x00, 0x00,                      /* loop end */
            /* 112 */ 0x18, 0x00, 0x00, 0x00,                      /* return */
        };
        ColorOverlay co;
        void* const sentinel = (void*) (uintptr_t) 0x5A5A5A5A;
        int frame, ended = 0, deepest = 0;
        if (!put_target(script + 28, script + 56) || !put_target(script + 44, script + 80))
            return 13;
        memset(&co, 0, sizeof co);
        co.x8_ptr1 = (union ColorOverlay_x8_t*) script;
        co.x28_colanim.ptr = sentinel;
        reports = effect_commands = 0;
        for (frame = 1; frame <= 40 && !ended; frame++) {
            if (lb_80014258(NULL, &co, colour_command)) ended = frame;
            if (co.xC_loop > deepest) deepest = co.xC_loop;
            /* Frame 5 is the first in the pulse: five slots in use, the pulse's first colour. */
            if (frame == 5 && (co.xC_loop != 5 || !co.x7C_color_enable || co.x2C_hex.r != 0x80 ||
                               co.x2C_hex.g != 0x00 || co.x2C_hex.b != 0xC0 || co.x2C_hex.a != 0x60))
                return 9;
        }
        if (ended != 17 || deepest != 5 || co.xC_loop != 0 || effect_commands != 2 || reports != 0 ||
            co.x28_colanim.ptr != sentinel)
            return 10;
    }

    /* Three loops nested need six slots: the third ends the script with one report instead of
     * writing past the stack. */
    {
        _Alignas(CmdUnion) unsigned char script[] = {
            0x0C, 0x00, 0x00, 0x02,
            0x0C, 0x00, 0x00, 0x02,
            0x0C, 0x00, 0x00, 0x02,
            0x04, 0x00, 0x00, 0x01,
            0x10, 0x00, 0x00, 0x00,
            0x10, 0x00, 0x00, 0x00,
            0x10, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        CommandInfo ci = {0};
        reports = 0;
        if (run(&ci, script) != 3 || ci.u != NULL || ci.loop_count != 4 || reports != 1) return 4;
        /* Reported once per run: a second bad script stops silently. */
        ci = (CommandInfo) {0};
        if (run(&ci, script) != 3 || ci.u != NULL || reports != 1) return 5;
    }

    /* A return or a loop end with nothing pushed ends the script. */
    {
        _Alignas(CmdUnion) unsigned char ret[] = { 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        _Alignas(CmdUnion) unsigned char end[] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        CommandInfo ci = {0};
        if (run(&ci, ret) != 1 || ci.u != NULL || ci.loop_count != 0) return 6;
        ci = (CommandInfo) {0};
        if (run(&ci, end) != 1 || ci.u != NULL || ci.loop_count != 0) return 7;
    }

    /* A colour script one level deeper than the console's five words still stops, at five of
     * five slots, without touching what follows the stack. (The report was made above: once a
     * run.) */
    {
        static _Alignas(CmdUnion) unsigned char script[] = {
            /*  0 */ 0x0C, 0x00, 0x00, 0x02,             /* loop twice */
            /*  4 */ 0x14, 0x00, 0x00, 0x00, 0, 0, 0, 0, /*   subroutine a */
            /* 12 */ 0x10, 0x00, 0x00, 0x00,             /* loop end */
            /* 16 */ 0x28, 0x00, 0x00, 0x00,             /* end */
            /* 20 */ 0x0C, 0x00, 0x00, 0x02,             /* a: loop twice */
            /* 24 */ 0x14, 0x00, 0x00, 0x00, 0, 0, 0, 0, /*   subroutine b: a sixth slot */
            /* 32 */ 0x10, 0x00, 0x00, 0x00,             /* loop end */
            /* 36 */ 0x18, 0x00, 0x00, 0x00,             /* return */
            /* 40 */ 0x2C, 0x00, 0x00, 0x01,             /* b: wait 1 */
            /* 44 */ 0x18, 0x00, 0x00, 0x00,             /*   return */
        };
        ColorOverlay co;
        void* const sentinel = (void*) (uintptr_t) 0x5A5A5A5A;
        int before = reports;
        if (!put_target(script + 8, script + 20) || !put_target(script + 28, script + 40)) return 11;
        memset(&co, 0, sizeof co);
        co.x8_ptr1 = (union ColorOverlay_x8_t*) script;
        co.x28_colanim.ptr = sentinel;
        if (lb_80014258(NULL, &co, colour_command) || co.x8_ptr1 != NULL || co.xC_loop != 5 ||
            co.x28_colanim.ptr != sentinel || reports != before)
            return 12;
    }
    return 0;
}
