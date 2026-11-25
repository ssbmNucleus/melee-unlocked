/* The Mods menu's viewer (ty/toy.c, the trophy viewer): one row of the list on the stand. A card's words
 * (its name, what it is, who made it, its downloads, what A does) and A to install it (Browse) or remove
 * it (Manage: the first A asks), a pack's own page (its post's words and picture; A lists its cards),
 * the words of the shim's own rows, and the spinner while the picture is on its way.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/sislib.h>

#include <mu_lobby_list.h>
#include <mu_mods_gallery.h>
#include <mu_native.h>

#include "mu_mods_categories.h"
#include "mu_mods_gallery_internal.h"

/* Manage: A in the viewer first asks ("Press A again to remove." where "A: Remove" was); a second A
 * removes. Another row, or the list again, asks anew. */
int mu_mods_remove_asked = -1;   /* the row asked about; -1 none */
int mu_mods_replace_asked = -1;  /* Browse Menus: the row whose first A asked "Press A again to replace it." */
int mu_mods_viewer_line;   /* the list's line the viewer was opened from (a pack's page: B from its list comes back there) */
View mu_mods_view = {-1};   /* (its fields: mu_mods_gallery_internal.h) */

/* While the viewer's picture is on its way the stand shows nothing, and the online character select's
 * spinner (shim/mu_slippi_css.c: '+' and 'x' in turn, blue) turns where the picture will stand. A text
 * of the viewer's own words' canvas (ty/toy.c _Toy_803084A0), so it goes with them when the viewer
 * closes (HSD_SisLib_803A5E70). */
extern s32 _Toy_sbss_804D6E74;
enum { PIC_POLL = 2, SPIN_FRAMES = 15 };
#define SPIN_X (-8.55f)
#define SPIN_Y (3.0f)
#define SPIN_SIZE 0.05f
static HSD_Text* spinner;
static int spinner_on, spinner_frames;

void mu_mods_spinner_gone(void)
{
    spinner = NULL;
    spinner_on = 0;
}

static void spinner_frame(int on)
{
    static const char glyph[2][3] = { { (char) 0x81, 0x7B, 0 }, { (char) 0x81, 0x7E, 0 } };
    GXColor blue = { 0x3C, 0xBC, 0xFF, 0xFF };
    if (on && spinner == NULL) {
        /* (a text with its own words buffer, as FG_CreateSubtext's: mu_slippi_css.c text_struct) */
        spinner = HSD_SisLib_803A6754(3, _Toy_sbss_804D6E74);
        if (spinner == NULL) return;
        spinner->pos_x = SPIN_X;
        spinner->pos_y = SPIN_Y;
        spinner->pos_z = 17.2f;
        spinner->font_size.x = SPIN_SIZE;
        spinner->font_size.y = SPIN_SIZE;
        spinner->default_kerning = 1;
        HSD_SisLib_803A6B98(spinner, 0.0f, 0.0f, "");
        HSD_SisLib_803A74F0(spinner, 0, &blue);
        spinner_on = 0;
        spinner_frames = 0;
    }
    if (spinner == NULL) return;
    if (!on) {
        if (spinner_on) HSD_SisLib_803A70A0(spinner, 0, (char*) "");
        spinner_on = 0;
        return;
    }
    if (!spinner_on || spinner_frames % SPIN_FRAMES == 0) {
        HSD_SisLib_803A70A0(spinner, 0, (char*) "%s", glyph[(spinner_frames / SPIN_FRAMES) & 1]);
    }
    spinner_on = 1;
    spinner_frames++;
}

/* The host's words for the viewer's row; 1 when they changed, -1 when the host's row is no longer a
 * card (the list changed under the viewer: a post turned out to be a pack). */
static int read_detail(int row)
{
    unsigned char p[3];
    unsigned int got, file_id;
    const Row* r = &mu_mods_rows[row];
    char name[48], what[48], author[48], downloads[48], line[48];
    static char about[400];
    int was_state = mu_mods_view.state, changed_words;
    if (r->kind != ROW_CARD) return 0;
    p[0] = SUB_DETAIL;
    p[1] = (unsigned char) (r->card >> 8);
    p[2] = (unsigned char) r->card;
    if (!mu_mods_ask(p, 3, mu_mods_reply, sizeof mu_mods_reply, &got) || got < 1) return 0;
    if (mu_mods_reply[0] == 0xFF) return -1;
    if (got < 6 + 5 * 48 + 400) return 0;
    mu_mods_field(name, mu_mods_reply + 6, 48);
    mu_mods_field(what, mu_mods_reply + 54, 48);
    mu_mods_field(author, mu_mods_reply + 102, 48);
    mu_mods_field(downloads, mu_mods_reply + 150, 48);
    mu_mods_field(line, mu_mods_reply + 198, 48);
    mu_mods_field(about, mu_mods_reply + 246, 400);
    file_id = got >= 6 + 5 * 48 + 400 + 4 ? mu_mods_u32_at(mu_mods_reply + 6 + 5 * 48 + 400) : 0;
    changed_words = mu_mods_view.card != r->card || was_state != mu_mods_reply[0] || strcmp(line, mu_mods_view.line) != 0 ||
                    strcmp(name, mu_mods_view.name) != 0 || strcmp(what, mu_mods_view.what) != 0 || file_id != mu_mods_view.file_id;
    mu_mods_view.confirm = got >= 6 + 5 * 48 + 400 + 5 && mu_mods_reply[6 + 5 * 48 + 400 + 4] == 1;
    if (mu_mods_view.pack) changed_words = 1;   /* (the words were a pack's) */
    mu_mods_view.pack = 0;
    mu_mods_view.card = r->card;
    mu_mods_view.file_id = file_id;
    mu_mods_view.state = mu_mods_reply[0];
    mu_mods_view.pic_status = mu_mods_reply[1];
    mu_mods_view.pic_generation = mu_mods_u32_at(mu_mods_reply + 2);
    strcpy(mu_mods_view.name, name);
    strcpy(mu_mods_view.what, what);
    strcpy(mu_mods_view.author, author);
    strcpy(mu_mods_view.downloads, downloads);
    strcpy(mu_mods_view.line, line);
    strcpy(mu_mods_view.about, about);
    if (changed_words && r->state != mu_mods_view.state) {
        mu_mods_rows[row].state = mu_mods_view.state;   /* the list shows it when it is back */
    }
    return changed_words;
}

/* A pack's row: its post's words and its own picture's state (12 [row] 1: 1, picture status, generation,
 * name, by, downloads, count, description). 1 when they changed, -1 when the row is no longer a pack. */
static int read_pack_detail(int row)
{
    unsigned char p[4];
    unsigned int got;
    const Row* r = &mu_mods_rows[row];
    char name[48], author[48], downloads[48], count[48];
    static char about[400];
    int changed_words;
    if (r->kind != ROW_PACK) return 0;
    p[0] = SUB_DETAIL;
    p[1] = (unsigned char) (r->card >> 8);
    p[2] = (unsigned char) r->card;
    p[3] = 1;
    if (!mu_mods_ask(p, 4, mu_mods_reply, sizeof mu_mods_reply, &got) || got < 1) return 0;
    if (mu_mods_reply[0] == 0xFF) return -1;
    if (got < 6 + 4 * 48 + 400) return 0;
    mu_mods_field(name, mu_mods_reply + 6, 48);
    mu_mods_field(author, mu_mods_reply + 54, 48);
    mu_mods_field(downloads, mu_mods_reply + 102, 48);
    mu_mods_field(count, mu_mods_reply + 150, 48);
    mu_mods_field(about, mu_mods_reply + 198, 400);
    changed_words = mu_mods_view.card != r->card || !mu_mods_view.pack || strcmp(name, mu_mods_view.name) != 0 ||
                    strcmp(author, mu_mods_view.author) != 0 || strcmp(downloads, mu_mods_view.downloads) != 0 ||
                    strcmp(count, mu_mods_view.count) != 0 || strcmp(about, mu_mods_view.about) != 0;
    mu_mods_view.card = r->card;
    mu_mods_view.pack = 1;
    mu_mods_view.file_id = 0;
    mu_mods_view.state = r->state;
    mu_mods_view.pic_status = mu_mods_reply[1];
    mu_mods_view.pic_generation = mu_mods_u32_at(mu_mods_reply + 2);
    strcpy(mu_mods_view.name, name);
    mu_mods_view.what[0] = 0;
    strcpy(mu_mods_view.author, author);
    strcpy(mu_mods_view.downloads, downloads);
    strcpy(mu_mods_view.count, count);
    mu_mods_view.line[0] = 0;
    strcpy(mu_mods_view.about, about);
    return changed_words;
}

/* A pack's own page: its title, its post's description, who made it and its downloads, its count and
 * what A does. 0 when the host has none of it yet (the words of before stand). */
static int pack_words(HSD_Text* name, HSD_Text* about, HSD_Text* line1, HSD_Text* line2, int row)
{
    static char words[600];
    char both[100];
    const Row* r = &mu_mods_rows[row];
    if (mu_mods_view.card != r->card || !mu_mods_view.pack) read_pack_detail(row);
    if (mu_mods_view.card != r->card || !mu_mods_view.pack) return 0;
    mu_mods_fit_name(name, mu_mods_view.name);
    if (mu_mods_view.about[0]) {
        mu_mods_wrap(words, 0, sizeof words, mu_mods_view.about, 44, 7);
    } else {
        strcpy(words, mu_mods_categories[mu_mods_category]->pack_words);
    }
    mu_mods_set_words(about, words);
    strcpy(both, mu_mods_view.author);
    if (mu_mods_view.author[0] && mu_mods_view.downloads[0]) strcat(both, "  -  ");
    strcat(both, mu_mods_view.downloads);
    mu_mods_set_words(line1, both);
    strcpy(both, mu_mods_view.count[0] ? mu_mods_view.count : r->right);
    strcat(both, "  -  A: Show the list");
    mu_mods_set_words(line2, both);
    return 1;
}

/* The viewer on a pack's row: its words and picture polled (the picture every PIC_POLL frames while it
 * is on its way), the spinner meanwhile. */
static int pack_viewer(int row, int inspecting)
{
    int result = MU_MODS_RETAIL;
    spinner_frame(!inspecting && (mu_mods_view.card != mu_mods_rows[row].card || !mu_mods_view.pack || mu_mods_view.pic_status == ST_LOADING));
    if (++mu_mods_view.tick >= (mu_mods_view.pic_status == ST_LOADING || mu_mods_view.card != mu_mods_rows[row].card ? PIC_POLL : VIEW_POLL)) {
        int words;
        mu_mods_view.tick = 0;
        words = read_pack_detail(row);
        if (words < 0) {
            OSReport("[mods] viewer: row %d is no longer a pack; back to the list\n", row);
            mu_mods_view.card = -1;
            if (!mu_mods_sync_rows(1, NULL)) return MU_MODS_RETAIL;
            mu_mods_fresh = 1;
            mu_mods_start_row = row < mu_mods_row_count ? row : 0;
            mu_mods_start_line = row < LIST_LINES ? row : LIST_LINES - 1;
            mu_mods_list_tick = 0;
            return MU_MODS_TO_LIST_NEW;
        }
        if (words > 0) result = MU_MODS_WORDS;
        if (mu_mods_view.pack && mu_mods_view.card == mu_mods_rows[row].card) mu_mods_want_picture(row);
    }
    if (mu_mods_view.redraw && !inspecting) {
        mu_mods_view.redraw = 0;
        return MU_MODS_REDRAW;
    }
    return result;
}

void mu_mods_gallery_words(HSD_Text* name, HSD_Text* about, HSD_Text* line1, HSD_Text* line2, int row)
{
    static char words[600];
    const Row* r;
    int at = 0;
    if (!mu_mods_gallery_running() || row < 0 || row >= mu_mods_row_count) return;
    r = &mu_mods_rows[row];
    if (r->kind == ROW_PACK && pack_words(name, about, line1, line2, row)) return;
    if (r->kind != ROW_CARD) {
        char now[sizeof mu_mods_head.filter + 8];
        now[0] = 0;
        if ((r->kind == ROW_SEARCH || r->kind == ROW_END_SEARCH) && (mu_mods_head.flags & FLAG_SEARCH)) {
            strcpy(now, "Now: ");
            strcat(now, mu_mods_head.filter);
        }
        if (mu_mods_categories[mu_mods_category]->row_words != NULL &&
            mu_mods_categories[mu_mods_category]->row_words(name, about, line1, line2, r))
        {
            return;   /* the category's own (Stages: its six, and a stage pool's "Vanilla") */
        }
        mu_mods_fit_name(name, r->name);
        mu_mods_set_words(about, r->kind == ROW_MESSAGE ? (mu_mods_head.status == ST_ERROR ? "Press A to try again." : "") :
                         r->kind == ROW_PACK ? mu_mods_categories[mu_mods_category]->pack_row_words :
                         r->kind == ROW_INSTALL_ALL ? mu_mods_categories[mu_mods_category]->install_all_words :
                         r->kind == ROW_SEARCH ? "Press A to look for posts by words in their title or creator." :
                         r->kind == ROW_END_SEARCH ? "Press A to list all the posts again." :
                         r->kind == ROW_FIGHTER ? mu_mods_categories[mu_mods_category]->fighter_row_words :
                         (mu_mods_head.flags & FLAG_MORE_FAILED) ? "Press A to try again." : "");
        mu_mods_set_words(line1, r->kind == ROW_PACK ? r->right : now);
        mu_mods_set_words(line2, "");
        mu_mods_view.card = -1;
        return;
    }
    if (mu_mods_view.pack) mu_mods_view.card = -1;   /* (the words were a pack's) */
    if (mu_mods_view.card != r->card) read_detail(row);
    if (mu_mods_view.card != r->card) {
        mu_mods_fit_name(name, r->name);
        mu_mods_set_words(about, "Loading...");
        mu_mods_set_words(line1, "");
        mu_mods_set_words(line2, mu_mods_remove_asked == row ? "Press A again to remove." : "");
        return;
    }
    mu_mods_fit_name(name, mu_mods_view.name);
    /* A failure's reason leads the words ("SSBM Nucleus is slow right now."); A tries again. */
    if (mu_mods_view.state == CARD_FAILED && mu_mods_view.line[0]) {
        at = mu_mods_wrap(words, 0, sizeof words, mu_mods_view.line, 44, 2);
        words[at++] = '\n';
    }
    if (mu_mods_view.what[0]) {
        const int n = (int) strlen(mu_mods_view.what);
        memcpy(words + at, mu_mods_view.what, n);
        at += n;
        if (mu_mods_view.about[0]) words[at++] = '\n';
    }
    mu_mods_wrap(words, at, sizeof words, mu_mods_view.about, 44, mu_mods_view.state == CARD_FAILED ? 5 : 7);
    mu_mods_set_words(about, words);
    if (mu_mods_view.author[0] && mu_mods_view.downloads[0]) {
        char both[100];
        strcpy(both, mu_mods_view.author);
        strcat(both, "  -  ");
        strcat(both, mu_mods_view.downloads);
        mu_mods_set_words(line1, both);
    } else {
        mu_mods_set_words(line1, mu_mods_view.author[0] ? mu_mods_view.author : mu_mods_view.downloads);
    }
    mu_mods_set_words(line2, mu_mods_remove_asked == row    ? "Press A again to remove."
                     : mu_mods_replace_asked == row ? "Press A again to replace it."
                     : mu_mods_view.state == CARD_FAILED ? "A: Try again" : mu_mods_view.line);
}

int mu_mods_gallery_viewer(int row, int inspecting, unsigned int trigger)
{
    const Row* r;
    int result = MU_MODS_RETAIL;
    if (!mu_mods_gallery_running()) return MU_MODS_RETAIL;
    if (mu_mods_open_on_list) {
        mu_mods_open_on_list = 0;
        return MU_MODS_TO_LIST;
    }
    if (mu_lobby_list_leaving()) return MU_MODS_TO_LIST;   /* a lobby joined: the list, which leaves */
    if (row < 0 || row >= mu_mods_row_count) return MU_MODS_RETAIL;
    r = &mu_mods_rows[row];
    if (row != mu_mods_view.row) {
        mu_mods_view.row = row;
        mu_mods_view.tick = VIEW_POLL;   /* ask at once */
        mu_mods_view.redraw = 0;
        mu_mods_remove_asked = -1;
        mu_mods_replace_asked = -1;
    }
    if (!inspecting && (trigger & HSD_PAD_A) && r->kind == ROW_CARD && !mu_mods_manage && mu_mods_replace_asked != row &&
        mu_mods_view.card == r->card && mu_mods_view.confirm && mu_mods_view.state == CARD_NOT_INSTALLED)
    {
        /* Browse Menus: a menu mod for a screen another one has; the first A only asks (the words say
         * which: "A: Install in place of ..."). */
        mu_mods_replace_asked = row;
        lbAudioAx_80024030(1);
        OSReport("[mods] viewer: row %d: A once, \"Press A again to replace it.\" (nothing installed yet)\n", row);
        return MU_MODS_WORDS;
    }
    if (!inspecting && (trigger & HSD_PAD_A) && r->kind == ROW_CARD && mu_mods_manage && mu_mods_remove_asked != row &&
        !mu_lobby_list_on() &&   /* the lobby list opens as a whole list, as Manage: there A joins at once */
        !(mu_mods_view.card == r->card && (mu_mods_view.state == CARD_REMOVING || mu_mods_view.state == CARD_REMOVED)))
    {
        /* Manage: the first A only asks. */
        mu_mods_remove_asked = row;
        lbAudioAx_80024030(1);
        OSReport("[mods] viewer: row %d: A once, \"Press A again to remove.\" (nothing removed yet)\n", row);
        return MU_MODS_WORDS;
    }
    if (!inspecting && (trigger & HSD_PAD_A)) {
        if (r->kind == ROW_CARD) {
            /* The card the words show (its file id): the host refuses a row that is another card now. */
            unsigned char p[7];
            unsigned int got;
            const unsigned int id = mu_mods_view.card == r->card ? mu_mods_view.file_id : 0;
            mu_mods_remove_asked = -1;
            mu_mods_replace_asked = -1;
            p[0] = SUB_ACTION;
            p[1] = (unsigned char) (r->card >> 8);
            p[2] = (unsigned char) r->card;
            p[3] = (unsigned char) (id >> 24);
            p[4] = (unsigned char) (id >> 16);
            p[5] = (unsigned char) (id >> 8);
            p[6] = (unsigned char) id;
            if (mu_mods_ask(p, id != 0 ? 7 : 3, mu_mods_reply, sizeof mu_mods_reply, &got) && got >= 1 && mu_mods_reply[0] == 1) {
                lbAudioAx_80024030(1);
                mu_mods_view.tick = VIEW_POLL;
            } else {
                lbAudioAx_80024030(3);   /* refused (installed already, or on its way) */
            }
        } else if (r->kind == ROW_INSTALL_ALL) {
            mu_mods_row_action(r, row, 0);
        } else if (r->kind == ROW_SEARCH) {
            /* The list first, which then leaves for the name entry. */
            lbAudioAx_80024030(1);
            mu_mods_leave_for_search = 1;
            return MU_MODS_TO_LIST;
        } else if (r->kind != ROW_MESSAGE || mu_mods_head.status == ST_ERROR) {
            const int done = mu_mods_row_action(r, row, r->kind == ROW_PACK ? mu_mods_viewer_line : 0);
            if (done == MU_MODS_REBUILD || done == MU_MODS_MOVED) return MU_MODS_TO_LIST_NEW;
        }
    }
    if (r->kind == ROW_PACK) return pack_viewer(row, inspecting);
    if (r->kind != ROW_CARD) {
        spinner_frame(0);
        return MU_MODS_RETAIL;
    }
    /* The spinner while the picture is on its way; the host asked every PIC_POLL frames meanwhile. */
    spinner_frame(!inspecting && (mu_mods_view.card != r->card || mu_mods_view.pack || mu_mods_view.pic_status == ST_LOADING));
    if (++mu_mods_view.tick >= (mu_mods_view.card != r->card || mu_mods_view.pic_status == ST_LOADING ? PIC_POLL : VIEW_POLL)) {
        int words;
        mu_mods_view.tick = 0;
        words = read_detail(row);
        if (words < 0) {
            /* The row is no longer a card (its post is a pack after all): back to the list, built
             * anew, on the same row. */
            OSReport("[mods] viewer: row %d is no longer a card; back to the list\n", row);
            mu_mods_view.card = -1;
            if (!mu_mods_sync_rows(1, NULL)) return MU_MODS_RETAIL;
            mu_mods_fresh = 1;
            mu_mods_start_row = row < mu_mods_row_count ? row : 0;
            mu_mods_start_line = row < LIST_LINES ? row : LIST_LINES - 1;
            mu_mods_list_tick = 0;
            return MU_MODS_TO_LIST_NEW;
        }
        if (words) result = MU_MODS_WORDS;
        /* A picture the stand does not show yet: fetched once per generation. */
        if (mu_mods_view.card == r->card && !mu_mods_view.pack) mu_mods_want_picture(row);
    }
    if (mu_mods_view.redraw && !inspecting) {
        mu_mods_view.redraw = 0;
        return MU_MODS_REDRAW;
    }
    return result;
}
