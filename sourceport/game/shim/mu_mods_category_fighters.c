/* The Mods menu's Fighters: new fighters to play offline, custom characters that play without code of
 * their own (the host's side: port/runtime/host/mods_menu_fighters.inc). Its list's heading reads
 * "Characters": the lettering has no capital F.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mu_mods_lettering.h"

/* Fighters: F the E of Name Entry without its bottom arm, i h r of Archives, g of Regular Match,
 * t of Stadium, e s */
static const Piece label[] =
    {{F14, 94, 112, 16, 0, 102, 20}, {F26, 93, 102}, {F05, 34, 49}, {F26, 77, 94}, {F08, 51, 63},
     {F00, 139, 157}, {F26, 47, 60}, {F02, 131, 147}, {-1}};

const MuModsCategory mu_mods_category_fighters = {
    .name = "Fighters",
    .label = label,
    .browse_words = "New fighters to play offline.",
    .manage_words = "Fighters you installed.",
    .heading = H_CHARACTERS,
    .legend = "L R: Fighter",
    .pack_words = "A pack of skins.",
    .pack_row_words = "A pack of skins. Press A for the list of them.",
    .install_all_words = "Press A to install every skin of this pack.",
    .fighter_row_words = "Press A for the skins of this fighter.",
    .list_back = mu_mods_back_to_fighters,
};
