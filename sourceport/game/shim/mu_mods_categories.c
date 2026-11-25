/* The Mods menu's categories in order (mu_mods_categories.h): a category's place here is its row in
 * Browse and its number to the host. One row a category, each in its own file. (Declared with its size
 * in the header, so a row too many or too few does not compile.)
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mu_mods_categories.h"

const MuModsCategory* const mu_mods_categories[] = {
    &mu_mods_category_characters,   /* 0 */
    &mu_mods_category_stages,       /* 1 */
    &mu_mods_category_effects,      /* 2 ("Particles" on its row) */
    &mu_mods_category_fighters,     /* 3 */
    &mu_mods_category_new_stages,   /* 4 */
    &mu_mods_category_menus,        /* 5 */
};
