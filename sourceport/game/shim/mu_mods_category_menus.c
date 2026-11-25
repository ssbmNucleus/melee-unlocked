/* The Mods menu's Menus: menu mods, character select screens (the host's side:
 * port/runtime/host/mods_menu_menus.inc). Its list's heading reads "Screens" (the lettering has no u
 * for "Menus"). A menu mod for a screen another one has asks before it replaces it: the host's word,
 * mu_mods_gallery_viewer.c's replace_asked.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mu_mods_lettering.h"

/* Menus: M of 1-P Mode, e, n of Name Entry, u of Regular Match, s */
static const Piece label[] =
    {{F00, 82, 107}, {F00, 139, 157}, {F14, 112, 127}, {F05, 48, 63}, {F02, 131, 147}, {-1}};

const MuModsCategory mu_mods_category_menus = {
    .name = "Menus",
    .label = label,
    .browse_words = "Character select screens.",
    .manage_words = "Menu mods you installed.",
    .heading = H_SCREENS,
    .legend = "L R: Fighter",
    .pack_words = "A pack of menu mods.",
    .pack_row_words = "A pack of menu mods. Press A for the list of them.",
    .install_all_words = "Press A to install every skin of this pack.",
    .fighter_row_words = "Press A for the skins of this fighter.",
    .list_back = mu_mods_back_to_fighters,
};
