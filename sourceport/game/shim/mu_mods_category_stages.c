/* The Mods menu's Stages: skins for the stages (the host's side: port/runtime/host/mods_menu_stages.inc).
 * Browse and Manage both open on the six stages (the fighters' list of the others); A on one lists its
 * posts (Browse) or its pool (Manage), whose "Vanilla" row puts the standard stage in the pool or takes
 * it out; L and R step through the six, and B goes back to them.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <sysdolphin/baselib/sislib.h>

#include "mu_mods_gallery_internal.h"
#include "mu_mods_lettering.h"

/* Stages: S t a of Stadium, g of Regular Match */
static const Piece label[] =
    {{F08, 31, 51}, {F08, 51, 63}, {F08, 63, 79}, {F05, 34, 49}, {F00, 139, 157}, {F02, 131, 147}, {-1}};

/* B in a stage's posts (Browse) or its pool (Manage): back to the six stages. */
static int list_back(void)
{
    if ((mu_mods_head.flags & FLAG_FILTER) && !(mu_mods_head.flags & FLAG_FIGHTERS)) {
        /* B in a stage's posts (Browse) or its pool (Manage): the six stages, on that one. */
        if (!mu_mods_host_change(CHANGE_FIGHTERS)) return 0;
        OSReport("[mods] list: back to the stages\n");
        mu_mods_changed();
        return 1;
    }
    return 0;
}

/* The viewer's words on one of the six stages, and on a stage pool's "Vanilla" (Manage); 0 for the
 * other rows that are no card (their words are the row's fields). */
static int row_words(HSD_Text* name, HSD_Text* about, HSD_Text* line1, HSD_Text* line2, const Row* r)
{
    static char words[600];
    if (r->kind == ROW_FIGHTER || (mu_mods_manage && r->kind == ROW_INSTALL_ALL)) {
        /* Stages: one of the six, and a stage pool's "Vanilla" (host/mods_menu_stages.inc). */
        mu_mods_fit_name(name, r->kind == ROW_INSTALL_ALL ? "Vanilla - the standard stage" : r->name);
        mu_mods_wrap(words, 0, sizeof words,
             r->kind == ROW_INSTALL_ALL
                 ? "Each match on this stage shows one of its pool at random. Press A to put the standard "
                   "stage in the pool or take it out. With nothing in the pool, the standard stage is shown."
             : !mu_mods_manage ? "Press A for the skins of this stage."
             : strcmp(r->name, "Other stages") == 0 ? "Press A for the skins of the other stages."
                                                    : "Press A for this stage's pool: each match on it shows one "
                                                      "of the pool at random.",
             44, 7);
        mu_mods_set_words(about, words);
        mu_mods_set_words(line1, r->kind == ROW_INSTALL_ALL ? (strcmp(r->right, "Off") == 0 ? "Not in the pool" : "In the pool")
                                                    : r->right);
        mu_mods_set_words(line2, "");
        mu_mods_view.card = -1;
        return 1;
    }
    return 0;
}

const MuModsCategory mu_mods_category_stages = {
    .name = "Stages",
    .label = label,
    .browse_words = "Skins for the stages.",
    .manage_words = "Stage skins you installed.",
    .heading = H_STAGES,
    .legend = "L R: Stage",
    .pack_words = "A pack of skins.",
    .pack_row_words = "A pack of skins. Press A for the list of them.",
    .install_all_words = "Press A to install every skin of this pack.",
    .fighter_row_words = "Press A for the skins of this fighter.",
    .list_back = list_back,
    .row_words = row_words,
};
