/* The Mods menu's New Stages: new stages for versus, picked on the stage select (shim/mu_new_stages.c;
 * the host's side: port/runtime/host/mods_menu_new_stages.inc).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mu_mods_lettering.h"

/* New Stages: N of Name Entry, e, w as Browse's (two v of Adventure), then Stages as above */
static const Piece label[] =
    {{F14, 7, 30, 21}, {F00, 139, 157, 16}, {F31, 51, 70, 11}, {F31, 51, 70, 24}, {F08, 31, 51, 19},
     {F08, 51, 63}, {F08, 63, 79}, {F05, 34, 49}, {F00, 139, 157}, {F02, 131, 147}, {-1}};

const MuModsCategory mu_mods_category_new_stages = {
    .name = "New Stages",
    .label = label,
    .browse_words = "New stages for versus.",
    .manage_words = "New stages you installed.",
    .heading = H_STAGES,
    .legend = "L R: Fighter",
    .pack_words = "A pack of new stages.",
    .pack_row_words = "A pack of new stages. Press A for the list of them.",
    .install_all_words = "Press A to install every stage of this pack.",
    .fighter_row_words = "Press A for the skins of this fighter.",
    .list_back = mu_mods_back_to_fighters,
};
