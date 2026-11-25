/* The Mods menu's Effects, "Particles" on its row and heading (the disc's label letters have no
 * lowercase f): effects for the fighters (the host's side: port/runtime/host/mods_menu_effect_slots.cpp).
 * Browse opens on the fighters, as Characters does; A on a fighter lists its moves ("Falcon Punch -
 * Standard"), A on a move the effects for it ("Standard", each one installed, "Get more": the fighter's
 * posts). Manage opens on the fighters with effects installed (A on a move steps it to its next effect)
 * and "All installed" (A removes one). Those lists are the fighters' list's kind, and the host's title
 * says which one it is: "moves", "sources", "files", "cycle", "All installed".
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/sislib.h>

#include "mu_mods_gallery_internal.h"
#include "mu_mods_lettering.h"

/* Particles (the effects: no f on the disc): P of 1-P Mode, a of Data, r i c of Archives, t of
 * Stadium, l of Classic, e s */
static const Piece label[] =
    {{F00, 53, 76}, {F04, 76, 92}, {F26, 47, 60}, {F08, 51, 63}, {F26, 93, 102}, {F26, 60, 77}, {F30, 55, 64},
     {F00, 139, 157}, {F02, 131, 147}, {-1}};

/* B: one step back (a fighter's posts, its moves, a move's effects; Manage's moves and "All installed"),
 * the host knowing where; the first list (no filter) leaves, as the game's B does. */
static int list_back(void)
{
    if (!(mu_mods_head.flags & FLAG_FILTER) && strcmp(mu_mods_head.title, "All installed") != 0) return 0;
    if (!mu_mods_host_change(CHANGE_FIGHTERS)) return 0;
    OSReport("[mods] list: back from %s\n", mu_mods_head.filter[0] ? mu_mods_head.filter : mu_mods_head.title);
    mu_mods_changed();
    return 1;
}

/* The viewer's words on a move, a move's effect, and Manage's fighters; 0 for the fighters' list (the
 * category's words). */
static int row_words(HSD_Text* name, HSD_Text* about, HSD_Text* line1, HSD_Text* line2, const Row* r)
{
    static char words[600];
    const char* text;
    if (r->kind != ROW_FIGHTER) return 0;
    if (strcmp(mu_mods_head.title, "moves") == 0) {
        text = strncmp(r->name, "All effects", 11) == 0
                   ? "Every move of this fighter at once. Press A to choose one effect for all of them, or the "
                     "standard ones."
                   : "Press A to choose the effect this move shows: one you installed, the standard one, or more "
                     "from SSBM Nucleus.";
    } else if (strcmp(mu_mods_head.title, "sources") == 0) {
        text = strcmp(r->name, "Get more") == 0   ? "Press A for more effects of this fighter from SSBM Nucleus."
               : strcmp(r->name, "Standard") == 0 ? "The game's own effect. Press A to show it."
                                                  : "An effect you installed. Press A to show it for this move.";
    } else if (strcmp(mu_mods_head.title, "cycle") == 0) {
        text = "Press A to show the next effect for this move: each one you installed that changes it, then the "
               "standard one.";
    } else if (strcmp(mu_mods_head.title, "files") == 0) {
        text = strcmp(r->name, "All installed") == 0 ? "Every effect you installed. Press A for the list, to remove one."
                                                     : "Press A for this fighter's moves and the effect each one shows.";
    } else {
        return 0;
    }
    mu_mods_fit_name(name, r->name);
    mu_mods_wrap(words, 0, sizeof words, text, 44, 7);
    mu_mods_set_words(about, words);
    mu_mods_set_words(line1, r->right);
    mu_mods_set_words(line2, "");
    mu_mods_view.card = -1;
    return 1;
}

const MuModsCategory mu_mods_category_effects = {
    .name = "Effects",
    .label = label,
    .browse_words = "Effects for the fighters.",
    .manage_words = "Effects you installed.",
    .heading = H_PARTICLES,
    .legend = "L R: Fighter",
    .pack_words = "A pack of effects.",
    .pack_row_words = "A pack of effects. Press A for the list of them.",
    .install_all_words = "Press A to install every effect of this pack.",
    .fighter_row_words = "Press A for the effects of this fighter.",
    .list_back = list_back,
    .row_words = row_words,
};
