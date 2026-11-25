/* The Gallery's scenes (ty/toy.c, ty/tylist.c) as the Mods menu's list and viewer:
 * shim/mu_mods_gallery*.c (shim/mu_mods_gallery_internal.h lists them; what one category does
 * differently: shim/mu_mods_categories.h). Every function here is a no-op answer unless
 * mu_mods_gallery() is set (shim/mu_mods_menu.c sets it when a Mods category is picked and clears it
 * back in the menus).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MODS_GALLERY_H
#define MU_MODS_GALLERY_H

struct HSD_Text;
struct HSD_Joint;
struct HSD_JObj;

/* What the list and the viewer ask of their scene functions. */
enum {
    MU_MODS_RETAIL = 0,   /* nothing of the Mods menu's: the game goes on as it would */
    MU_MODS_REBUILD,      /* the list's rows changed: build the list again (sorting first) */
    MU_MODS_SWALLOW,      /* the key was the Mods menu's: nothing more this frame */
    MU_MODS_TO_LIST,      /* the viewer: to the list */
    MU_MODS_TO_LIST_NEW,  /* the viewer: to the list, whose rows changed (sorting first) */
    MU_MODS_REDRAW,       /* the viewer: the picture and the words again */
    MU_MODS_WORDS,        /* the viewer: the words again */
    MU_MODS_LEAVE,        /* the list: leave the Gallery as B does (the search's name entry follows) */
    MU_MODS_MOVED         /* the list: the same list with more rows (or its window moved): the
                           * cursor stays on its row (mu_mods_gallery_list_start says where) */
};

/* gm/gmmain_lib.c: the save's trophy count (2), category flags (1) and trophy flags (0) while the
 * Mods menu runs the Gallery: a copy the menu owns, so nothing it does reaches the save. */
void* mu_mods_trophy_data(int which, void* real);

/* shim/mu_mods_menu.c, when a category is picked: Browse (0) or Manage (1), the category's number
 * (its place in shim/mu_mods_categories.c). */
void mu_mods_gallery_open(int manage, int category);
/* ... and back in the menus. */
void mu_mods_gallery_close(void);
/* Back from the search's name entry: the same list, the host keeping its fighter, page and search. */
void mu_mods_gallery_reopen(void);
/* Browse's search: the host lists the posts with `words` (at most 24 letters), "" ends it. 1 when the
 * list changed. */
int mu_mods_gallery_search(const char* words);

/* ty/tylist.c. The list's input (`selected` row, the cursor on `line`; `left` and `right` are L and R):
 * MU_MODS_RETAIL, _REBUILD, _MOVED, _SWALLOW or _WORDS (the same rows, new words: lay them out again,
 * and the bar). */
int mu_mods_gallery_list(int selected, int line, unsigned int trigger, int left, int right);
/* B in a pack's list (back to the posts) or in a fighter's posts (back to the fighters): 1 when it
 * went back (build the list again), 0 for the game's B. */
int mu_mods_gallery_list_back(void);
/* The list goes round from its last row to its first (it holds the whole list); else a move stops at
 * its ends. */
int mu_mods_gallery_wraps(void);
/* Its last row is the list's true end: the game's red line under it is shown. */
int mu_mods_gallery_end_shown(void);
/* As the list is built: 1 when its rows changed, with the row to select and the line it stands on. */
int mu_mods_gallery_list_start(int* row, int* line);
/* A row's words, and its art: the inset box at its right shows only on a row with words for it. */
void mu_mods_gallery_row_words(struct HSD_Text* name, struct HSD_Text* mark, struct HSD_Text* count, int row,
                               struct HSD_JObj* jobj);
const char* mu_mods_gallery_count_words(void);
void mu_mods_gallery_bar(struct HSD_Text* bar);
/* The lobby list (shim/mu_lobby_list.c): when the list's row `row` is one of the host's rows as drawn
 * now, 1 with the host's row and the generation of the host's list those rows came from (the head's
 * bytes 5-6, read with them; the Mods menu's host sends 0 there); 0 for any other row. */
int mu_mods_gallery_drawn_row(int row, int* host_row, unsigned int* generation);

/* ty/toy.c */
/* Toy_80307470, the panel just loaded (shim/mu_mods_menu.c): its headings read "Mods" and the section
 * (the category's heading, or Manage), its list's side label "LIST". */
void mu_mods_gallery_panel(struct HSD_JObj* panel);
struct HSD_Joint* mu_mods_gallery_joint(int row);
int mu_mods_gallery_placement(int field, float* value);
void mu_mods_gallery_words(struct HSD_Text* name, struct HSD_Text* about, struct HSD_Text* line1,
                           struct HSD_Text* line2, int row);
int mu_mods_gallery_viewer(int row, int inspecting, unsigned int trigger);

#endif
