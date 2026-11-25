/* The Mods menu's Characters: skins for the fighters (the host's side: port/runtime/host/mods_menu.cpp).
 * Browse opens on the fighters ("All fighters", then each one); A on one lists its posts, L and R step
 * to the fighter before or after, B goes back to the fighters. Its row in mu_mods_categories.h's table.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mu_mods_lettering.h"

/* Characters: C of Classic, h r c of Archives, a t of Data and Stadium */
static const Piece label[] =
    {{F30, 32, 55}, {F26, 77, 94}, {F04, 76, 92}, {F26, 47, 60}, {F04, 76, 92}, {F26, 60, 77}, {F08, 51, 63},
     {F00, 139, 157}, {F26, 47, 60}, {F02, 131, 147}, {-1}};

const MuModsCategory mu_mods_category_characters = {
    .name = "Characters",
    .label = label,
    .browse_words = "Skins for the fighters.",
    .manage_words = "Character skins you installed.",
    .heading = H_CHARACTERS,
    .legend = "L R: Fighter",
    .pack_words = "A pack of skins.",
    .pack_row_words = "A pack of skins. Press A for the list of them.",
    .install_all_words = "Press A to install every skin of this pack.",
    .fighter_row_words = "Press A for the skins of this fighter.",
    .list_back = mu_mods_back_to_fighters,
};
