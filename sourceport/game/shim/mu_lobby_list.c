/* Doubles lobbies, the game's half (include/mu_lobby_list.h; the host's half is
 * port/runtime/host/lobby_directory.h, reached through MU_CMD_LOBBY).
 *
 * A lobby is a Slippi Teams code listed on SSBM Nucleus. This file only chooses which code the game
 * types: the search itself is the normal Teams search (mu_slippi_sss.c), so nothing reaching Slippi
 * or an opponent changes, and a player on stock Slippi joins a lobby by typing its code.
 *
 *   Online -> Teams   opens menu kind 36 (mn/mnmain.c) with "Lobbies", "Create" and "Enter code";
 *                     the labels are cut from the disc's own lettering like the Mods menu's
 *                     (shim/mu_mods_menu.c). "Host" cannot be cut: the wide lettering has no H.
 *   Lobbies           the Gallery's list screen (shim/mu_mods_gallery.c) lists the lobbies, a row each
 *                     ("<host> - <region>", "N waiting"); A on a row joins that lobby at once (there
 *                     is no lobby page: ty/tylist.c asks mu_lobby_list_press before the Gallery would
 *                     open its viewer). The bar under the list says "Joining <host>...", or why a
 *                     join failed (the lobby filled or closed), and the list is read again. A join
 *                     that came through leaves the Gallery and the Teams select opens with the code
 *                     staged; B while it is on its way gives it up (the seat is left if it came).
 *   Create            A lists a lobby (no rules to choose: the directory's ruleset field is always sent
 *                     as "competitive" and never shown); the Teams select opens with its code staged.
 *   Enter code        the Teams select as before: START opens the code entry.
 *   Teams select      with a staged code, START searches with it (the code is typed into the code
 *                     entry's text as a player would). The line under the prompts says "Lobby of
 *                     <host>" or "Hosting - N waiting". Z while searching closes or leaves the lobby;
 *                     a lobby that closed or filled stops the search and says so.
 * Off (the submenu is not shown and Teams opens the select at once) in a Vanilla run, a replay, with
 * the "doubleslobbies 0" setting, on a disc whose label pictures are not the 1.02 ones, and in a
 * peer-to-peer session. When SSBM Nucleus does not offer lobbies (404, or no JSON), "Lobbies" and
 * "Create" say so and only "Enter code" goes on.
 *
 * MU_CMD_LOBBY (0xC9), payload[0]:
 *   0 status        reply: on, availability (0 unknown, 1 yes, 2 no), phase (0 none, 1 creating,
 *                   2 joining, 3 hosting, 4 joined, 5 ended, 6 failed), waiting, flags (1 host,
 *                   2 matched, 4 entered, 8 seat given up), 0 (was the ruleset), code[10], host[16],
 *                   message[48]
 *   1 menu          as 0; the submenu opened: the host looks at the directory when it has not lately
 *   2 list, on      the lobby list opens (1) or closes (0): MU_CMD_MOD_BROWSER 9 to 14 are the lobbies'
 *   3 create        reply 1: asked for
 *   4 cancel, why   0 left, 1 Z, 2 B (on the submenu, or out of the list with a join on its way or
 *                   just come through): the lobby closed or left, a join on its way given up
 *   5 matched       the Teams match formed (CONNECTION_SUCCESS)
 *   6 search        the Teams search started with the lobby's code
 *   7 ack           an ended or failed lobby's words were shown
 *   8 entered       the Teams select opened with the lobby's code
 *   9 join, row:2, list:2
 *                   A on the list's row and the generation of the list it was drawn from (the head's
 *                   bytes 5-6; big-endian): reply 0 no lobby row (the Gallery's own), 1 joining, 2
 *                   refused (a lobby on its way already, that one is gone, or a list no longer kept).
 *                   The host joins the lobby that row showed, by its id, never what the row holds now.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/inlines.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>

#include <mu_lobby_list.h>
#include <mu_log_id.h>
#include <mu_mods_gallery.h>
#include <mu_native.h>
#include <mu_commands.h>

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);
int snprintf(char* buffer, __SIZE_TYPE__ size, const char* format, ...);

enum { REQ_STATUS, REQ_MENU, REQ_LIST, REQ_CREATE, REQ_CANCEL, REQ_MATCHED, REQ_SEARCH, REQ_ACK, REQ_ENTERED, REQ_JOIN };
enum { JOIN_NOT_A_LOBBY, JOIN_ASKED, JOIN_REFUSED };
enum { AVAIL_UNKNOWN, AVAIL_YES, AVAIL_NO };
enum { PH_NONE, PH_CREATING, PH_JOINING, PH_HOSTING, PH_JOINED, PH_ENDED, PH_FAILED };
enum { FL_HOST = 1, FL_MATCHED = 2, FL_ENTERED = 4, FL_RELEASED = 8 };
enum { ROW_LOBBIES, ROW_CREATE, ROW_CODE, ROWS };
enum { WHY_LEFT, WHY_Z, WHY_BACK };

typedef struct Status {
    unsigned char on, avail, phase, waiting, flags;
    char code[10], host[16], message[48];
} Status;

static unsigned char reply[4096];   /* the host answers into no less (MU_SLIPPI_RESPONSE_CAPACITY) */

static int ask(const unsigned char* p, unsigned int n)
{
    unsigned int got = 0;
    return mu_online_abi_command(MU_CMD_LOBBY, p, n, reply, sizeof reply, &got) == 0 ? (int) got : 0;
}

/* A field of the host's reply as text the menu font draws (shim/mu_mods_gallery.c drawable(): any
 * other byte would be read as half of a Shift-JIS pair); `code` keeps a code's '#'. */
static void text_of(char* out, const unsigned char* in, int width, int code)
{
    int i;
    for (i = 0; i < width - 1 && in[i] != 0; i++) {
        const char c = (char) in[i];
        const int letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out[i] = letter || (code && c == '#') || (!code && strchr(" .,'\":-", c) != NULL) ? c : ' ';
    }
    out[i] = 0;
}

static int read_status(Status* s, int request)
{
    const unsigned char p[1] = {(unsigned char) request};
    memset(s, 0, sizeof *s);
    if (ask(p, 1) < 80) return 0;
    s->on = reply[0];
    s->avail = reply[1];
    s->phase = reply[2];
    s->waiting = reply[3];
    s->flags = reply[4];
    text_of(s->code, reply + 6, 10, 1);
    text_of(s->host, reply + 16, 16, 0);
    text_of(s->message, reply + 32, 48, 0);
    return 1;
}

static void tell(int request, int value)
{
    const unsigned char p[2] = {(unsigned char) request, (unsigned char) value};
    ask(p, 2);
}

static int has_code(const Status* s)
{
    return (s->phase == PH_HOSTING || s->phase == PH_JOINED) && s->code[0] != 0;
}

/* ---- the labels, cut from the disc's lettering (as shim/mu_mods_menu.c) ---- */

enum { LW = 176, LH = 30, LBYTES = 176 * 32, HW = 168, HH = 28, HBYTES = 168 * 32 / 2 };
/* Cursor label frames (176x30 IA4) and the panel's italic headings (168x28 I4) the letters come from. */
static const unsigned char cursor_frames[] = {0, 4, 8, 14, 16, 19, 26, 30};
enum { C00, C04, C08, C14, C16, C19, C26, C30, CURSOR_SOURCES };
enum { P84_3, P85_3, P85_5, P85_10, P85_17, PANEL_SOURCES };
static const unsigned char panel_joint[PANEL_SOURCES] = {84, 85, 85, 85, 85};
static const unsigned char panel_frame[PANEL_SOURCES] = {3, 3, 5, 10, 17};
#define SOURCE_HASH 0x491F1DB3u   /* FNV-1a of the 1.02 pictures above, in this order */

typedef struct Piece {
    signed char src;          /* -1 ends the word */
    unsigned char left, right;
    unsigned char advance;    /* 0: right - left */
} Piece;

/* "Lobbies": L o of Lottery, b of Rumble twice, i s of Archives, e of 1-P Mode.
 * "Create": C of Classic, r of Archives, e of 1-P Mode, a of Data, t of Stadium.
 * "Enter code": Ent r of Name Entry, e and "ode" of 1-P Mode, c of Archives. */
static const Piece words[ROWS][8] = {
    {{C16, 37, 54}, {C16, 53, 70}, {C19, 98, 115}, {C19, 98, 115}, {C26, 93, 102}, {C00, 139, 157}, {C26, 136, 150},
     {-1}},
    {{C30, 32, 55}, {C26, 47, 60}, {C00, 139, 157}, {C04, 76, 92}, {C08, 51, 63}, {C00, 139, 157}, {-1}},
    {{C14, 95, 140}, {C00, 139, 157}, {C14, 140, 152}, {C00, 0, 0, 8}, {C26, 60, 77}, {C00, 107, 157}, {-1}},
};

/* The Gallery's italic headings: "Online" (O of Options, n i of Training, l of Rumble, e of Archives)
 * and "Teams" (T of Training, e s of Archives, a of Stadium, m of Rumble). */
typedef struct Cut { signed char src; unsigned char left, right; } Cut;
enum { HEAD_ONLINE, HEAD_TEAMS, HEADINGS };
static const Cut heading_cuts[HEADINGS][7] = {
    {{P84_3, 24, 44}, {P85_5, 77, 92}, {P85_10, 109, 116}, {P85_5, 67, 74}, {P85_5, 77, 92}, {P85_17, 113, 129}, {-1}},
    {{P85_5, 15, 32}, {P85_17, 113, 129}, {P85_3, 51, 66}, {P85_10, 66, 88}, {P85_17, 132, 145}, {-1}},
};
#define HEADING_SLOPE 0.3f

static unsigned char label_pixels[ROWS][LBYTES] __attribute__((aligned(32)));
static HSD_ImageDesc label_desc[ROWS];
static unsigned char heading_pixels[HEADINGS][HBYTES] __attribute__((aligned(32)));
static int composed;   /* 1 built, -1 the disc's pictures are not the ones the letters come from */

typedef struct Tables { HSD_TexAnim* anim[4]; int count; } Tables;

/* MatAnimJoint walk in the archive's order: every texture animation of joint number `want`. */
static int walk(HSD_MatAnimJoint* joint, int index, int want, Tables* out, int depth)
{
    int next = index + 1;
    if (joint == NULL || depth > 64) return next;
    if (index == want) {
        HSD_MatAnim* material;
        for (material = DP(joint->matanim); material != NULL; material = DP(material->next)) {
            HSD_TexAnim* texture;
            for (texture = DP(material->texanim); texture != NULL; texture = DP(texture->next))
                if (out->count < 4 && !DISC_NULL(texture->imagetbl)) out->anim[out->count++] = texture;
        }
    }
    if (!DISC_NULL(joint->child)) next = walk(DP(joint->child), next, want, out, depth + 1);
    if (!DISC_NULL(joint->next)) next = walk(DP(joint->next), next, want, out, depth + 1);
    return next;
}

static HSD_ImageDesc* frame_of(HSD_TexAnim* anim, int k, int w, int h, int format)
{
    HSD_ImageDesc* image;
    if (anim == NULL || k >= anim->n_imagetbl || (image = DP(DP(anim->imagetbl)[k])) == NULL) return NULL;
    if (image->width != w || image->height != h || image->format != format || DISC_NULL(image->image_ptr))
        return NULL;
    return image;
}

static unsigned fnv(const unsigned char* p, int n, unsigned h)
{
    int i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Row-major planes: IA4 as alpha << 4 | intensity, I4 as intensity. */
static unsigned char cursor_planes[CURSOR_SOURCES][LW * LH];
static unsigned char panel_planes[PANEL_SOURCES][HW * HH];

static void decode(const unsigned char* data, unsigned char* out, int ia4)
{
    int pos = 0, bx, by, x, y;
    const int w = ia4 ? LW : HW, h = ia4 ? LH : HH;
    if (ia4) {
        for (by = 0; by < 32; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; ++y)
                    for (x = 0; x < 8; ++x, ++pos)
                        if (by + y < h) out[(by + y) * w + bx + x] = data[pos];
    } else {
        for (by = 0; by < 32; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; ++y)
                    for (x = 0; x < 8; x += 2, ++pos)
                        if (by + y < h) {
                            out[(by + y) * w + bx + x] = data[pos] >> 4;
                            out[(by + y) * w + bx + x + 1] = data[pos] & 15;
                        }
    }
}

static void encode(const unsigned char* plane, unsigned char* data, int ia4)
{
    int pos = 0, bx, by, x, y;
    const int w = ia4 ? LW : HW, h = ia4 ? LH : HH;
    if (ia4) {
        for (by = 0; by < 32; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; ++y)
                    for (x = 0; x < 8; ++x)
                        data[pos++] = by + y < h ? plane[(by + y) * w + bx + x] : 0;
    } else {
        for (by = 0; by < 32; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; ++y)
                    for (x = 0; x < 8; x += 2)
                        data[pos++] = by + y < h ? (unsigned char) (plane[(by + y) * w + bx + x] << 4 |
                                                                    plane[(by + y) * w + bx + x + 1])
                                                 : 0;
    }
}

/* One cursor label: the pieces side by side, centred, each keeping only the pixels within two of its
 * own letter's fill (shim/mu_mods_menu.c compose). */
static void compose(const Piece* pieces, unsigned char* out)
{
    static unsigned char fill[LW * LH];
    int total = 0, cursor, i, x, y;
    for (i = 0; pieces[i].src >= 0; ++i) total += pieces[i].advance ? pieces[i].advance : pieces[i].right - pieces[i].left;
    memset(out, 0, LW * LH);
    cursor = (LW - total) / 2;
    for (i = 0; pieces[i].src >= 0; ++i) {
        const Piece* p = &pieces[i];
        const unsigned char* src = cursor_planes[p->src];
        for (y = 0; y < LH; ++y)
            for (x = 0; x < LW; ++x) {
                const unsigned v = src[y * LW + x];
                fill[y * LW + x] = (unsigned char) (x >= p->left && x < p->right && (v & 15) >= 8 && (v >> 4) >= 8);
            }
        for (y = 0; y < LH; ++y)
            for (x = p->left; x < p->right; ++x) {
                int dx, dy, near = 0, tx = cursor + x - p->left;
                unsigned v, o, va, vi, oa, oi;
                if (x >= LW || tx < 0 || tx >= LW) continue;
                for (dy = -2; dy <= 2 && !near; ++dy)
                    for (dx = -2; dx <= 2 && !near; ++dx)
                        if (y + dy >= 0 && y + dy < LH && x + dx >= 0 && x + dx < LW) near = fill[(y + dy) * LW + x + dx];
                if (!near) continue;
                v = src[y * LW + x];
                o = out[y * LW + tx];
                va = v >> 4; vi = v & 15; oa = o >> 4; oi = o & 15;
                out[y * LW + tx] = (unsigned char) ((va > oa ? va : oa) << 4 |
                                                    (va && oa ? (vi > oi ? vi : oi) : (va > oa ? vi : oi)));
            }
        cursor += p->advance ? p->advance : p->right - p->left;
    }
}

static int shift_of(int y)
{
    return (int) (HEADING_SLOPE * (float) (HH - 1 - y) + 0.5f);
}

/* One italic heading: each letter's slanted columns, 2 apart, centred (shim/mu_mods_menu.c cut_word
 * over whole pictures). */
static void cut_heading(const Cut* cuts, unsigned char* out)
{
    int total = 0, cursor, i, x, y;
    memset(out, 0, HW * HH);
    for (i = 0; cuts[i].src >= 0; ++i) total += cuts[i].right - cuts[i].left + 2;
    cursor = (HW - total) / 2;
    for (i = 0; cuts[i].src >= 0; ++i) {
        const Cut* c = &cuts[i];
        const unsigned char* p = panel_planes[c->src];
        for (y = 0; y < HH; ++y) {
            const int shift = shift_of(y);
            for (x = c->left + shift; x < c->right + shift; ++x) {
                const int tx = cursor + x - c->left;
                int dx, dy, near = 0;
                if (x < 0 || x >= HW || tx < 0 || tx >= HW) continue;
                for (dy = -2; dy <= 2 && !near; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= HH) continue;
                    for (dx = -2; dx <= 2 && !near; ++dx) {
                        const int xx = x + dx, s = shift_of(yy);
                        near = xx >= c->left + s && xx < c->right + s && xx >= 0 && xx < HW && p[yy * HW + xx] >= 8;
                    }
                }
                if (near && p[y * HW + x] > out[y * HW + tx]) out[y * HW + tx] = p[y * HW + x];
            }
        }
        cursor += c->right - c->left + 2;
    }
}

static int build(HSD_TexAnim* cursor, HSD_TexAnim* const* panels)
{
    static unsigned char plane[LW * LH];
    const HSD_ImageDesc* src[CURSOR_SOURCES + PANEL_SOURCES];
    unsigned hash = 0x811C9DC5u;
    int i;
    for (i = 0; i < CURSOR_SOURCES + PANEL_SOURCES; ++i) {
        const int is_cursor = i < CURSOR_SOURCES;
        src[i] = is_cursor ? frame_of(cursor, cursor_frames[i], LW, LH, GX_TF_IA4)
                           : frame_of(panels[i - CURSOR_SOURCES], panel_frame[i - CURSOR_SOURCES], HW, HH, GX_TF_I4);
        if (src[i] == NULL) return -1;
        hash = fnv(DP(src[i]->image_ptr), is_cursor ? LBYTES : HBYTES, hash);
    }
    if (hash != SOURCE_HASH) {
        OSReport("[lobbies] menu: the disc's menu labels are not the 1.02 ones (%08x); Teams as before\n", hash);
        return -1;
    }
    for (i = 0; i < CURSOR_SOURCES; ++i) decode(DP(src[i]->image_ptr), cursor_planes[i], 1);
    for (i = 0; i < PANEL_SOURCES; ++i) decode(DP(src[CURSOR_SOURCES + i]->image_ptr), panel_planes[i], 0);
    for (i = 0; i < ROWS; ++i) {
        compose(words[i], plane);
        encode(plane, label_pixels[i], 1);
        label_desc[i] = *src[C00];
        DISC_SET(label_desc[i].image_ptr, label_pixels[i]);
    }
    for (i = 0; i < HEADINGS; ++i) {
        cut_heading(heading_cuts[i], plane);
        encode(plane, heading_pixels[i], 0);
    }
    return 1;
}

/* ---- the submenu (menu kind 36) ---- */

static HSD_Text* described_text;   /* the description text the words below are in (only compared) */
static char described[104];
static int last_row;           /* the row the submenu opens on */
static int from_submenu;        /* the Teams select was opened from the submenu (back returns to it) */
static int list_mode;           /* the Gallery's scenes run as the lobby list */
static int leaving_joined;      /* the list leaves because a join came through (mu_lobby_list_leaving) */

void mu_lobby_menu_load(HSD_MatAnimJoint* cursor_matanim, HSD_MatAnimJoint* panel_matanim)
{
    Tables cursor = {0}, p84 = {0}, p85 = {0};
    HSD_TexAnim* panels[PANEL_SOURCES];
    int i;
    described_text = NULL;
    if (composed != 0) return;
    walk(cursor_matanim, 0, 3, &cursor, 0);
    walk(panel_matanim, 0, 84, &p84, 0);
    walk(panel_matanim, 0, 85, &p85, 0);
    if (cursor.count == 0 || p84.count == 0 || p85.count == 0) {
        OSReport("[lobbies] menu: the menu's label tables are not where 1.02 has them; Teams as before\n");
        composed = -1;
        return;
    }
    for (i = 0; i < PANEL_SOURCES; ++i) panels[i] = panel_joint[i] == 84 ? p84.anim[0] : p85.anim[0];
    composed = build(cursor.anim[0], panels);
    if (composed > 0) OSReport("[lobbies] menu: labels cut (Lobbies, Create, Enter code; Online, Teams)\n");
}

void mu_lobby_menu_label(HSD_JObj* label, int menu_kind, int selection)
{
    HSD_DObj* dobj;
    if (composed <= 0 || label == NULL || menu_kind != MU_MENU_KIND_LOBBY || selection < 0 || selection >= ROWS) return;
    for (dobj = label->u.dobj; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* texture;
        if (dobj->mobj == NULL) continue;
        for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
            const HSD_ImageDesc* now = texture->imagedesc;
            if (now != NULL && now->width == LW && now->height == LH && now->format == GX_TF_IA4)
                texture->imagedesc = &label_desc[selection];
        }
    }
}

/* What the description says of `row` now. */
static const char* words_for(int row, const Status* s)
{
    static char buffer[104];
    const int unavailable = s->avail == AVAIL_NO;
    switch (row) {
    case ROW_LOBBIES:
        if (unavailable) {
            snprintf(buffer, sizeof buffer, "Unavailable. %s", s->message[0] ? s->message : "No lobbies on SSBM Nucleus.");
            return buffer;
        }
        return "Teams lobbies open on SSBM Nucleus.";
    case ROW_CREATE:
        if (s->phase == PH_CREATING) return "Creating a lobby...";
        if (s->phase == PH_FAILED && s->message[0]) {
            snprintf(buffer, sizeof buffer, "%s", s->message);
            return buffer;
        }
        if (unavailable) {
            snprintf(buffer, sizeof buffer, "Unavailable. %s", s->message[0] ? s->message : "No lobbies on SSBM Nucleus.");
            return buffer;
        }
        /* (no colon: the description font has none) */
        return "Host a doubles lobby.";
    default:
        return "Search with a Teams code you type.";
    }
}

/* The words go into the text only while the menu itself hands it over (as it makes it, and every
 * frame it shows it): it frees its texts when it moves, so none is kept here. */
static void describe(HSD_Text* text, int row)
{
    Status s;
    const char* w;
    read_status(&s, REQ_STATUS);
    w = words_for(row, &s);
    if (text == described_text && strcmp(w, described) == 0) return;
    strncpy(described, w, sizeof described - 1);
    described[sizeof described - 1] = 0;
    described_text = text;
    mu_slippi_menu_description(text, described);
}

void mu_lobby_menu_describe(HSD_Text* text, int menu_kind, int selection)
{
    if (menu_kind != MU_MENU_KIND_LOBBY || composed <= 0 || text == NULL || selection < 0 || selection >= ROWS) return;
    described_text = NULL;   /* a new text: whatever was in it is the game's */
    describe(text, selection);
}

void mu_lobby_menu_idle(HSD_Text* text, int menu_kind, int selection)
{
    if (menu_kind != MU_MENU_KIND_LOBBY || composed <= 0 || text == NULL || selection < 0 || selection >= ROWS) return;
    describe(text, selection);
}

static u8 confirm_port(void)
{
    int i;
    for (i = 0; i < 4; i++)
        if (gm_GetButtonsTriggered((u8) i) & PAD_CONFIRM) return (u8) i;
    return 0;
}

/* The online select in Teams, as the online menu's Teams row opens it (shim/mu_slippi_menu.c). */
static void enter_teams(u8 port)
{
    MenuExitData* data;
    mn_804D6BC8.cooldown = 5;
    mn_804A04F0.entering_menu = 1;
    gm_801677E8((s8) port);
    mu_slippi_state()->mode = MU_SLP_MODE_TEAMS;
    mu_content_mode(MU_SLP_MODE_TEAMS);
    from_submenu = 1;
    sfxForward();
    data = gm_GetCurrentSceneExitData();
    data->pending_mode = GM_HANYU_CSS;
    gm_801A4B60();
}

static int feature_open(void)
{
    Status s;
    return composed > 0 && !mu_online_p2p_menus() && read_status(&s, REQ_MENU) && s.on;
}

int mu_lobby_menu_enter(void)
{
    if (!feature_open()) return 0;
    sfxForward();
    mn_80229894(MU_MENU_KIND_LOBBY, (u16) last_row, 1);
    OSReport("[lobbies] menu: Teams submenu\n");
    return 1;
}

int mu_lobby_menu_return(unsigned char* menu_kind, unsigned char* hovered)
{
    const int was = from_submenu;
    from_submenu = 0;
    if (!was || composed <= 0 || mu_slippi_state()->mode != MU_SLP_MODE_TEAMS) return 0;
    *menu_kind = MU_MENU_KIND_LOBBY;
    *hovered = (unsigned char) last_row;
    return 1;
}

void mu_lobby_menu_think(HSD_GObj* gp)
{
    Status s;
    u32 buttons = mn_80229624(4);
    const int row = mn_804A04F0.hovered_selection < ROWS ? mn_804A04F0.hovered_selection : 0;
    (void) gp;
    mn_804A04F0.buttons = buttons;
    read_status(&s, REQ_STATUS);
    /* A lobby made or joined: the Teams select with its code (once; back from it is the submenu). The
     * player is the one who pressed A for it (Create here, A on the list's row). */
    if (has_code(&s) && !(s.flags & FL_ENTERED) && mn_804D6BC8.cooldown == 0) {
        tell(REQ_ENTERED, 0);
        last_row = row;
        OSReport("[lobbies] menu: to the Teams select with %s lobby\n", (s.flags & FL_HOST) ? "the hosted" : "a joined");
        enter_teams(gm_801677F0());
        return;
    }
    /* A failed Create's words stay on its row until the next press. */
    if (s.phase == PH_FAILED && (row != ROW_CREATE || (buttons & (MenuInput_Confirm | MenuInput_Back |
                                                                  MenuInput_Up | MenuInput_Down)))) {
        tell(REQ_ACK, 0);
        read_status(&s, REQ_STATUS);
    }
    if (buttons & MenuInput_Confirm) {
        MenuExitData* data;
        last_row = row;
        if (row == ROW_CODE) {
            OSReport("[lobbies] menu: Enter code\n");
            enter_teams(confirm_port());
            return;
        }
        if (s.avail == AVAIL_NO || !s.on || s.phase == PH_CREATING || s.phase == PH_JOINING) {
            lbAudioAx_80024030(3);
            return;
        }
        if (row == ROW_CREATE) {
            const unsigned char p[1] = {REQ_CREATE};
            if (ask(p, 1) < 1 || reply[0] != 1) {
                lbAudioAx_80024030(3);
                return;
            }
            gm_801677E8((s8) confirm_port());
            sfxForward();
            OSReport("[lobbies] menu: Create\n");
            return;
        }
        /* Lobbies: the Gallery's list screen, as the lobby list. */
        tell(REQ_LIST, 1);
        list_mode = 1;
        leaving_joined = 0;
        mn_804D6BC8.cooldown = 5;
        mn_804A04F0.entering_menu = 1;
        gm_801677E8((s8) confirm_port());
        sfxForward();
        mu_mods_menu_open_lobbies();
        OSReport("[lobbies] menu: Lobbies\n");
        data = gm_GetCurrentSceneExitData();
        data->pending_mode = GM_TOY_GALLERY;
        gm_801A4B60();
    } else if (buttons & MenuInput_Back) {
        /* Out of the submenu: a lobby still being made or joined is given up. */
        if (s.phase != PH_NONE) tell(REQ_CANCEL, WHY_BACK);
        last_row = row;
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_8, MU_SLP_MODE_TEAMS, 3);
    } else if (s.phase == PH_CREATING) {
        /* nothing else while the lobby is being made */
    } else if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection = (u16) ((row + ROWS - 1) % ROWS);
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection = (u16) ((row + 1) % ROWS);
    }
}

void mu_lobby_left_online(void)
{
    Status s;
    if (!read_status(&s, REQ_STATUS) || s.phase == PH_NONE) return;
    tell(REQ_CANCEL, WHY_LEFT);
}

/* ---- the lobby list (the Gallery's list screen, shim/mu_mods_gallery.c) ---- */

int mu_lobby_list_on(void)
{
    return list_mode && mu_mods_gallery();
}

int mu_lobby_list_leaving(void)
{
    Status s;
    const int joined = mu_lobby_list_on() && read_status(&s, REQ_STATUS) && s.phase == PH_JOINED && !(s.flags & FL_ENTERED);
    if (joined) leaving_joined = 1;
    return joined;
}

int mu_lobby_list_press(int row)
{
    unsigned char p[5];
    int host_row;
    unsigned int generation;
    /* The row as drawn: the host's row and the generation of the list it came from, so the host joins
     * the lobby that row shows even when a refresh changed the list since (lobby_directory.cpp
     * join_row). Any other row (Loading..., a retry) is the Gallery's own. */
    if (!mu_lobby_list_on() || !mu_mods_gallery_drawn_row(row, &host_row, &generation) || host_row < 0 ||
        host_row > 0xFFFF)
        return 0;
    p[0] = REQ_JOIN;
    p[1] = (unsigned char) (host_row >> 8);
    p[2] = (unsigned char) host_row;
    p[3] = (unsigned char) (generation >> 8);
    p[4] = (unsigned char) generation;
    if (ask(p, 5) < 1 || reply[0] == JOIN_NOT_A_LOBBY) return 0;
    if (reply[0] == JOIN_ASKED) {
        sfxForward();
        OSReport("[lobbies] list: A on row %d (list %u): joining that lobby\n", row, generation);
    } else {
        lbAudioAx_80024030(3);   /* refused: a lobby on its way already, or that one is gone */
        OSReport("[lobbies] list: A on row %d (list %u): refused\n", row, generation);
    }
    return 1;
}

int mu_lobby_list_return(unsigned char* menu_kind, unsigned char* selection)
{
    Status s;
    /* Out of the list by B (not for a join that came through): a join still on its way is given up,
     * and one that came through in the same moment is left, as the submenu's B and leaving the online
     * select do (REQ_CANCEL), so the submenu never takes the player into Teams for it. */
    if (!leaving_joined && read_status(&s, REQ_STATUS) &&
        (s.phase == PH_JOINING || (s.phase == PH_JOINED && !(s.flags & FL_ENTERED))))
    {
        tell(REQ_CANCEL, WHY_BACK);
        OSReport("[lobbies] list: back with a join %s: given up\n",
                 s.phase == PH_JOINING ? "on its way" : "that just came through");
    }
    leaving_joined = 0;
    list_mode = 0;
    tell(REQ_LIST, 0);
    *menu_kind = MU_MENU_KIND_LOBBY;
    *selection = ROW_LOBBIES;
    last_row = ROW_LOBBIES;
    return 1;
}

/* The Gallery's panel: "Online" and "Teams" for "Trophies" and "Gallery", "LIST" for "TROPHY LIST"
 * (the 1.02 pictures only; anything else stays). */
#define TROPHIES_HASH 0x3C4ADAEFu
#define GALLERY_HASH 0x6ED9A696u
#define TROPHY_LIST_HASH 0x62D92C65u
enum { SW = 160, SH = 12, SBYTES = 160 * 12 };
static HSD_ImageDesc panel_desc[2];
static HSD_ImageDesc list_desc;
static unsigned char list_pixels[SBYTES] __attribute__((aligned(32)));

/* "LIST" of "TROPHY LIST" (IA4, 160x12): columns 96 to 146, slanted 0.7, centred, keeping only the
 * pixels within two of its letters' fill (shim/mu_mods_menu.c list_word, the same cut). */
static int list_at(int x, int y)
{
    return ((y >> 2) * (SW >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7);
}

static int list_shift(int y)
{
    return (int) (0.7f * (float) (SH - 1 - y) + 0.5f);
}

static void cut_list(const unsigned char* data)
{
    enum { LEFT = 96, RIGHT = 146 };
    int x, y, cursor = (SW - (RIGHT - LEFT)) / 2;
    memset(list_pixels, 0, sizeof list_pixels);
    for (y = 0; y < SH; ++y) {
        for (x = LEFT + list_shift(y); x < RIGHT + list_shift(y) && x < SW; ++x) {
            const int tx = cursor + x - LEFT;
            int dx, dy, near = 0;
            if (tx < 0 || tx >= SW) continue;
            for (dy = -2; dy <= 2 && !near; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= SH) continue;
                for (dx = -2; dx <= 2 && !near; ++dx) {
                    const int xx = x + dx;
                    unsigned v;
                    if (xx < LEFT + list_shift(yy) || xx >= RIGHT + list_shift(yy) || xx < 0 || xx >= SW) continue;
                    v = data[list_at(xx, yy)];
                    near = (v & 15) >= 8 && (v >> 4) >= 8;
                }
            }
            if (near) list_pixels[list_at(tx, y)] = data[list_at(x, y)];
        }
    }
}

static void swap_panel(HSD_JObj* jobj, int depth)
{
    for (; jobj != NULL && depth < 64; jobj = jobj->next) {
        HSD_DObj* dobj;
        for (dobj = union_type_dobj(jobj) ? jobj->u.dobj : NULL; dobj != NULL; dobj = dobj->next) {
            HSD_TObj* texture;
            if (dobj->mobj == NULL) continue;
            for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
                HSD_ImageDesc* now = texture->imagedesc;
                const unsigned char* data;
                unsigned hash;
                if (now == NULL || DISC_NULL(now->image_ptr)) continue;
                data = DP(now->image_ptr);
                if (now->width == HW && now->height == HH && now->format == GX_TF_I4) {
                    hash = fnv(data, HBYTES, 0x811C9DC5u);
                    if (hash == TROPHIES_HASH || hash == GALLERY_HASH) {
                        HSD_ImageDesc* to = &panel_desc[hash == GALLERY_HASH];
                        *to = *now;
                        DISC_SET(to->image_ptr, heading_pixels[hash == GALLERY_HASH ? HEAD_TEAMS : HEAD_ONLINE]);
                        texture->imagedesc = to;
                    }
                } else if (now->width == SW && now->height == SH && now->format == GX_TF_IA4 &&
                           fnv(data, SBYTES, 0x811C9DC5u) == TROPHY_LIST_HASH) {
                    cut_list(data);
                    list_desc = *now;
                    DISC_SET(list_desc.image_ptr, list_pixels);
                    texture->imagedesc = &list_desc;
                }
            }
        }
        if (!(jobj->flags & JOBJ_INSTANCE)) swap_panel(jobj->child, depth + 1);
    }
}

void mu_lobby_list_panel(HSD_JObj* panel)
{
    if (composed <= 0 || panel == NULL) return;
    swap_panel(panel, 0);
}

/* ---- the Teams select (shim/mu_slippi_css.c) ---- */
#ifndef MU_NO_SLIPPI

extern char mnNameNew_CurrentNameText[];

static char ended_words[48];   /* why the lobby ended, shown a while */
static int ended_frames;

int mu_lobby_css_staged(void)
{
    Status s;
    return mu_slippi_state()->mode == MU_SLP_MODE_TEAMS && read_status(&s, REQ_STATUS) && has_code(&s);
}

/* The code into the code entry's text, as typed there (full-width letters, 3 bytes each). */
static void type_code(const char* code)
{
    int i;
    memset(mnNameNew_CurrentNameText, 0, 0x20);
    for (i = 0; code[i] != 0 && i < 8; i++) {
        const char c = code[i];
        unsigned char lead = 0x82, trail;
        if (c >= 'A' && c <= 'Z') trail = (unsigned char) (0x60 + (c - 'A'));
        else if (c >= '0' && c <= '9') trail = (unsigned char) (0x4F + (c - '0'));
        else if (c == '#') lead = 0x81, trail = 0x94;
        else break;
        mnNameNew_CurrentNameText[i * 3] = (char) lead;
        mnNameNew_CurrentNameText[i * 3 + 1] = (char) trail;
    }
}

int mu_lobby_css_search(void)
{
    Status s;
    if (mu_slippi_state()->mode != MU_SLP_MODE_TEAMS || !read_status(&s, REQ_STATUS) || !has_code(&s)) return 0;
    type_code(s.code);
    tell(REQ_SEARCH, 0);
    ended_frames = 0;
    {
        char id[16];   /* a lobby's code lets anyone into its search: a short hash in a public build's log */
        OSReport("[lobbies] select: Teams search with the lobby's code %s\n", mu_log_id(s.code, id));
    }
    return 1;
}

void mu_lobby_css_cancel(void)
{
    Status s;
    if (!read_status(&s, REQ_STATUS) || !has_code(&s)) return;
    tell(REQ_CANCEL, WHY_Z);
    memset(mnNameNew_CurrentNameText, 0, 0x20);
    snprintf(ended_words, sizeof ended_words, "%s", (s.flags & FL_HOST) ? "The lobby closed." : "You left the lobby.");
    ended_frames = 60 * 6;
}

const char* mu_lobby_css_frame(int conn)
{
    static char line[64];
    Status s;
    if (mu_slippi_state()->mode != MU_SLP_MODE_TEAMS || !read_status(&s, REQ_STATUS)) return NULL;
    if (has_code(&s)) {
        if (!(s.flags & FL_ENTERED)) tell(REQ_ENTERED, 0);
        if (conn == MU_SLP_MM_CONNECTION_SUCCESS && !(s.flags & FL_MATCHED)) tell(REQ_MATCHED, 0);
        if (s.flags & FL_HOST) snprintf(line, sizeof line, "Hosting - %d waiting", s.waiting ? s.waiting : 1);
        else snprintf(line, sizeof line, "Lobby of %s", s.host[0] ? s.host : "a player");
        return line;
    }
    if (s.phase == PH_ENDED || s.phase == PH_FAILED) {
        /* Closed, filled or expired: a search still looking stops (one already connecting goes on). */
        if (conn == MU_SLP_MM_INITIALIZING || conn == MU_SLP_MM_MATCHMAKING) {
            OSReport("[lobbies] select: %s; the search stops\n", s.message);
            mu_slippi_cleanup_connections();
        }
        memset(mnNameNew_CurrentNameText, 0, 0x20);
        snprintf(ended_words, sizeof ended_words, "%s", s.message[0] ? s.message : "The lobby closed.");
        ended_frames = 60 * 8;
        tell(REQ_ACK, 0);
    }
    if (ended_frames > 0) {
        ended_frames--;
        return ended_words;
    }
    return NULL;
}

#endif
