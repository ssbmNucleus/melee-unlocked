/* The main menu's Trophies row as "Mods" (the in-game mod menu), in the menu's own look.
 *
 * Nothing new is drawn: the labels are the game's own label pictures, cut from the player's disc
 * (MnMaAll.usd) letter by letter the way rogue/mu_rogue_label.c builds Rogue Mode's heading, and the
 * rows are the game's own rows:
 *   main menu   Trophies -> "Mods" (label, italic panel heading, description)
 *   Mods        Gallery -> "Browse", Collection -> "Manage", Lottery hidden
 *   Browse and Manage open menu kind 35 (mnmain.c): "Characters", "Stages", "Particles" (the
 *     effects; the disc's label letters have no lowercase f, so "Effects" cannot be cut), "Fighters",
 *     "New Stages" and "Menus". The ones this release has off (the host's release switches,
 *     port/runtime/host/release_scope.h, MU_CMD_MOD_BROWSER 8's second byte) are dimmed in Browse,
 *     read "Coming soon." and open nothing; Manage leaves them out
 *   a category opens the Gallery's scenes as the mod list (ty/toy.c and ty/tylist.c, mu_mods_gallery.c)
 * Off in a Vanilla run (the host says so, MU_CMD_MOD_BROWSER 8) and whenever the disc's label pictures
 * are not the 1.02 ones the letters are cut from: Trophies then stays exactly as the game has it.
 * The letters are cut in mu_mods_lettering.c; what one category does differently (its label, its
 * words, its list's) is in that category's own file, listed in mu_mods_categories.c. */
#include <dolphin/os.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/tobj.h>
#include <string.h>

#include <mu_lobby_list.h>
#include <mu_mods_gallery.h>
#include <mu_native.h>
#include <mu_commands.h>

#include "mu_mods_lettering.h"

int mu_online_abi_command(unsigned int command, const unsigned char* payload, unsigned int size,
                          unsigned char* response, unsigned int capacity, unsigned int* response_size);

#define MODS_KIND 35            /* the category menu's kind (mnmain.c) */
#define SOON_TEXT 0x96          /* the catalog text every "Coming soon." row starts from (Characters' own) */

static int enabled;      /* this menu load: Mods in place of Trophies */
static int manage;       /* the category menu was opened from Manage */
static int gallery_mode; /* the Gallery scenes run as the mod list */
static int gallery_category;
/* The categories this release has, bit c category c (the host's release switches: MU_CMD_MOD_BROWSER
 * 8's second byte). A host without that byte has them all, as before the switches. */
static unsigned char categories_on = (1u << MU_MODS_CATEGORIES) - 1;

static unsigned char host_reply[4096];   /* the host answers into no less (MU_SLIPPI_RESPONSE_CAPACITY) */
static unsigned int host_got;

static int host_request(unsigned char ask)
{
    host_got = 0;
    return mu_online_abi_command(MU_CMD_MOD_BROWSER, &ask, 1, host_reply, sizeof host_reply, &host_got) == 0 &&
           host_got >= 1 && host_reply[0] == 1;
}

/* Apply any menu change that finished on the worker after leaving the Mods menu.
 * Run before the archive loader looks up either the file size or its data. */
void mu_mods_menu_sync_screens(void)
{
    host_request(19);
}

/* mnMain_Scene_OnEnter, once MnMaAll's symbols are in: the labels in place of Trophies' own. The
 * archive is the scene's, read again at each menu load, so its tables are set again each time. */
void mu_mods_menu_load(HSD_MatAnimJoint* cursor_matanim, HSD_MatAnimJoint* panel_matanim,
                       HSD_MatAnimJoint* contop_matanim)
{
    Tables cursor = {0}, panel = {0}, panel85 = {0}, preview = {0};
    int i;
    static int logged;
    enabled = 0;
    if (!host_request(8)) {
        if (!logged++) OSReport("[mods] menu: off this run (Vanilla or a replay); Trophies as the game has it\n");
        return;
    }
    categories_on = host_got >= 2 ? host_reply[1] : (unsigned char) ((1u << MU_MODS_CATEGORIES) - 1);
    mu_mods_walk(cursor_matanim, 0, 3, &cursor, 0);
    mu_mods_walk(panel_matanim, 0, 84, &panel, 0);
    if (cursor.count == 0 || panel.count == 0) {
        if (!mu_mods_composed) OSReport("[mods] menu: the menu's label tables are not where 1.02 has them; Trophies kept\n");
        mu_mods_composed = mu_mods_composed ? mu_mods_composed : -1;
        return;
    }
    if (mu_mods_composed == 0) mu_mods_composed = mu_mods_build(cursor.anim[0], panel.anim[0]);
    if (mu_mods_composed < 0) return;
    for (i = 0; i < cursor.count; ++i) {
        mu_mods_put(cursor.anim[i], 2, &mu_mods_label_desc[MODS]);
        mu_mods_put(cursor.anim[i], 15, &mu_mods_label_desc[BROWSE]);
        mu_mods_put(cursor.anim[i], 18, &mu_mods_label_desc[MANAGE]);
        /* The categories' third row (menu kind 35, row 2) shows label frame 17, which 1.02 leaves as a
         * blank 8x4 picture (Trophies' hidden third row): "Particles" there. */
        mu_mods_put(cursor.anim[i], 17, &mu_mods_label_desc[PARTICLES]);
    }
    for (i = 0; i < panel.count; ++i) mu_mods_put(panel.anim[i], 2, &mu_mods_heading_desc);
    enabled = 1;
    if (!logged++) OSReport("[mods] menu: Mods in place of Trophies (categories on: %02x)\n", (unsigned) categories_on);
    /* The Mods row's preview and the Gallery's headings: without them (another disc's pictures) the
     * menu still works, with Trophies' preview and the Gallery's own headings. */
    mu_mods_walk(panel_matanim, 0, 85, &panel85, 0);
    mu_mods_walk(contop_matanim, 0, 22, &preview, 0);
    if (mu_mods_more_composed == 0)
        mu_mods_more_composed = panel85.count && preview.count ? mu_mods_build_more(panel.anim[0], panel85.anim[0], preview.anim[0]) : -1;
    if (mu_mods_more_composed > 0)
        for (i = 0; i < preview.count; ++i) mu_mods_put(preview.anim[i], 2, &mu_mods_preview_desc);
}

void mu_mods_gallery_panel(HSD_JObj* panel)
{
    if (mu_lobby_list_on()) {   /* the lobby list's own headings (shim/mu_lobby_list.c) */
        mu_lobby_list_panel(panel);
        return;
    }
    if (!gallery_mode || !enabled || mu_mods_composed <= 0 || mu_mods_more_composed <= 0 || panel == NULL) return;
    mu_mods_swap_panel_pictures(panel, 0);
}

int mu_mods_menu_on(void)
{
    return enabled;
}

/* The categories (kind 35's rows) are the table's, in its order (mu_mods_categories.h): Browse has
 * a row for each, Manage the ones this release has on only, so there a row is not its category. */
#define CATEGORY_ROWS MU_MODS_CATEGORIES

/* Off (the host's release switches): its row dimmed in Browse, its words "Coming soon.", nothing opens. */
static int category_on(int category)
{
    return category >= 0 && category < CATEGORY_ROWS && ((categories_on >> category) & 1);
}

/* A row's category: Browse has every category's row, Manage the ones on only. -1 for none. */
static int row_category(int row)
{
    int i, n = 0;
    if (row < 0) return -1;
    if (!manage) return row < CATEGORY_ROWS ? row : -1;
    for (i = 0; i < CATEGORY_ROWS; i++) {
        if (category_on(i) && n++ == row) return i;
    }
    return -1;
}

static int category_row(int category)
{
    int i, n = 0;
    if (!manage) return category;
    for (i = 0; i < CATEGORY_ROWS && i < category; i++) n += category_on(i);
    return n;
}

int mu_mods_menu_rows(void)
{
    int i, n = 0;
    if (!manage) return CATEGORY_ROWS;
    for (i = 0; i < CATEGORY_ROWS; i++) n += category_on(i);
    return n;
}

int mu_mods_menu_available(int row)
{
    return category_on(row_category(row));
}

/* Rows of the category menu (kind 35): their labels, set after the game set the row's frame. */
void mu_mods_menu_label(HSD_JObj* label, int menu_kind, int selection)
{
    HSD_DObj* dobj;
    HSD_ImageDesc* picture;
    const int category = row_category(selection);
    if (!enabled || label == NULL || menu_kind != MODS_KIND || category < 0) return;
    /* a category this release has off: its label dimmed, as the menus dim a locked row */
    picture = category_on(category) ? &mu_mods_label_desc[CHARACTERS + category] : &mu_mods_label_dim_desc[category];
    for (dobj = label->u.dobj; dobj != NULL; dobj = dobj->next) {
        HSD_TObj* texture;
        if (dobj->mobj == NULL) continue;
        for (texture = dobj->mobj->tobj; texture != NULL; texture = texture->next) {
            const HSD_ImageDesc* now = texture->imagedesc;
            if (now != NULL && now->width == LW && now->height == LH && now->format == GX_TF_IA4)
                texture->imagedesc = picture;
        }
    }
}

void mu_mods_menu_describe(HSD_Text* text, int menu_kind, int selection)
{
    const char* words_now = NULL;
    if (!enabled || text == NULL) return;
    if (menu_kind == 0 && selection == 2) {
        words_now = "Skins and effects.";
    } else if (menu_kind == 3 && selection == 0) {
        words_now = "Find mods on SSBM Nucleus.";
    } else if (menu_kind == 3 && selection == 3) {
        words_now = "The mods you installed.";
    } else if (menu_kind == MODS_KIND) {
        /* the category's own words (mu_mods_categories.h); one this release has off is coming soon */
        const int category = row_category(selection);
        if (category >= 0 && !category_on(category)) {
            /* The words keep the form of the row's own catalog text (mu_slippi_menu_description), and
             * the rows start from different ones (mn/mnmain.c mu_mn_mods_text): Particles from 0x98, a
             * centered and larger line, the other three from 0x96, a smaller line at the box's left.
             * Every off row starts from the same one, so "Coming soon." looks the same on all four. */
            HSD_SisLib_803A6368(text, SOON_TEXT);
            words_now = "Coming soon.";
        } else if (category >= 0) {
            words_now = manage ? mu_mods_categories[category]->manage_words
                               : mu_mods_categories[category]->browse_words;
        }
    }
    if (words_now != NULL) mu_slippi_menu_description(text, words_now);
}

void mu_mods_menu_set_manage(int from_manage)
{
    manage = from_manage != 0;
}

int mu_mods_menu_manage(void)
{
    return manage;
}

static const char* category_name(int category)
{
    return category >= 0 && category < CATEGORY_ROWS ? mu_mods_categories[category]->name : "?";
}

/* A category's row picked: the Gallery's scenes start as the mod list of it. */
void mu_mods_menu_open(int row)
{
    const int category = row_category(row);
    gallery_mode = 1;
    gallery_category = category < 0 ? 0 : category;
    mu_mods_gallery_open(manage, gallery_category);
    OSReport("[mods] menu: %s, %s\n", manage ? "Manage" : "Browse", category_name(gallery_category));
}

/* The Teams submenu's Lobbies (shim/mu_lobby_list.c): the Gallery's scenes as the lobby list, which
 * the host answers for (MU_CMD_MOD_BROWSER 9 to 14 while it is open) as a whole list, as Manage's. */
void mu_mods_menu_open_lobbies(void)
{
    gallery_mode = 1;
    gallery_category = 0;
    mu_mods_gallery_open(1, 0);
}

int mu_mods_gallery(void)
{
    return gallery_mode;
}

int mu_mods_gallery_category(void)
{
    return gallery_category;
}

/* Back in the menus after the Gallery's scenes (gm/gmmenumode.c): the category menu, on that row
 * (or, when the list's "Search" row left it, the name entry: mnMain_Scene_OnEnter asks
 * mu_mods_menu_take_search). */
int mu_mods_menu_return(unsigned char* menu_kind, unsigned char* selection)
{
    if (!gallery_mode) return 0;
    if (mu_lobby_list_on()) {   /* the lobby list: back to the Teams submenu (shim/mu_lobby_list.c) */
        gallery_mode = 0;
        mu_mods_gallery_close();
        return mu_lobby_list_return(menu_kind, selection);
    }
    gallery_mode = 0;
    mu_mods_gallery_close();
    *menu_kind = MODS_KIND;
    *selection = (unsigned char) category_row(gallery_category);
    return 1;
}

/* ---- Browse's search: the Gallery is left for the menus' name entry, and comes back ---- */

static int search_pending;

void mu_mods_menu_want_search(void)
{
    search_pending = gallery_mode && !manage;
}

int mu_mods_menu_take_search(void)
{
    const int was = search_pending;
    search_pending = 0;
    return was && enabled;
}

void mu_mods_menu_reopen(void)
{
    gallery_mode = 1;
    mu_mods_gallery_reopen();
    OSReport("[mods] menu: back to Browse, %s\n", category_name(gallery_category));
}
