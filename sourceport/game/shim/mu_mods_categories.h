/* The Mods menu's categories (menu kind 35's rows; the list the Gallery's scenes show for each), one
 * file each (shim/mu_mods_category_<name>.c, a MuModsCategory row), listed in order by the one table in
 * shim/mu_mods_categories.c. A category's number is its place there: its row in Browse and its number
 * to the host (MU_CMD_MOD_BROWSER 9, port/runtime/host/mods_menu.cpp: 0 Characters, 1 Stages, 2 Effects,
 * 3 Fighters, 4 New Stages, 5 Menus). What the game side does differently for one category is a field
 * of its row; everything else is the same for all of them (shim/mu_mods_menu.c, shim/mu_mods_gallery*.c).
 * Which categories a release has is the host's one table, port/runtime/host/release_scope.h (handed
 * over by MU_CMD_MOD_BROWSER 8, decided by the settings file without a rebuild). A category that is
 * off: Browse shows its row dimmed, its description reads "Coming soon.", and A on it gives only the
 * menus' error sound (mnmain.c): no list, no request to the site, no install. Manage shows only the
 * categories that are on, so there a row is not its category (mu_mods_menu.c row_category).
 * Adding one: its file (copy the nearest), its line in the table and MU_MODS_CATEGORIES one more, its
 * host side and its line in release_scope.h's table, and mnmain.c's three per-row tables
 * (docs/guide/game-recipes.md).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MODS_CATEGORIES_H
#define MU_MODS_CATEGORIES_H

struct HSD_Text;
struct Piece;
struct Row;

enum { MU_MODS_CATEGORIES = 6 };

typedef struct MuModsCategory {
    /* The category menu (shim/mu_mods_menu.c, shim/mu_mods_lettering.c) */
    const char* name;                 /* in the log: "[mods] menu: Browse, Characters" */
    const struct Piece* label;        /* its row's label, cut from the disc's lettering ({-1} ends it) */
    const char* browse_words;         /* its row's description line in Browse (off: "Coming soon.") */
    const char* manage_words;         /* ... and in Manage */
    int heading;                      /* the Gallery panel's heading in Browse (H_..., mu_mods_lettering.h) */
    /* Its list and viewer (shim/mu_mods_gallery*.c) */
    const char* legend;               /* the bar's legend where L and R step through the fighters' list */
    const char* pack_words;           /* a pack's page whose post has no description */
    const char* pack_row_words;       /* the viewer on a pack's row, its words not read yet */
    const char* install_all_words;    /* the viewer on a pack's "Install all" */
    const char* fighter_row_words;    /* the viewer on a row of the fighters' list */
    int (*list_back)(void);           /* B in the list, but in a pack's: 1 when it went back, 0 the game's B */
    /* The viewer's words on a row that is no card (NULL: the words above): 1 when it wrote them all. */
    int (*row_words)(struct HSD_Text* name, struct HSD_Text* about, struct HSD_Text* line1,
                     struct HSD_Text* line2, const struct Row* r);
} MuModsCategory;

extern const MuModsCategory* const mu_mods_categories[MU_MODS_CATEGORIES];
extern const MuModsCategory mu_mods_category_characters, mu_mods_category_stages, mu_mods_category_effects,
    mu_mods_category_fighters, mu_mods_category_new_stages, mu_mods_category_menus;

/* B in a fighter's posts in Browse: back to the fighters (shim/mu_mods_gallery_input.c). Every
 * category's list_back but Stages'. */
int mu_mods_back_to_fighters(void);

#endif
