/* The Mods menu's labels and headings, cut from the disc's own lettering (shim/mu_mods_lettering.c):
 * what shim/mu_mods_menu.c puts in place, and what a category's label is made of (its row in
 * shim/mu_mods_category_<name>.c).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MODS_LETTERING_H
#define MU_MODS_LETTERING_H

#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>

#include "mu_mods_categories.h"

enum { LW = 176, LH = 30, LBYTES = 176 * 32, HW = 168, HH = 28, HBYTES = 168 * 32 / 2 };
/* The labels: the main menu's three, then one a category in the table's order; the Effects
 * category's ("Particles") is also the one the categories' third row shows (mu_mods_menu_load). */
enum { MODS, BROWSE, MANAGE, CHARACTERS, PARTICLES = CHARACTERS + 2, LABELS = CHARACTERS + MU_MODS_CATEGORIES };

/* Source pictures: cursor label frames (176x30 IA4) and the panel's italic headings (168x28 I4). */
enum { F00, F02, F04, F05, F08, F14, F26, F30, F31, F54, HEAD0 = 10, HEAD2, SOURCES };

typedef struct Piece {
    signed char src;          /* -1 ends the word */
    unsigned char left, right;
    unsigned char advance;    /* 0: right - left */
    float slope;              /* italic cut: x(y) = edge + slope * (h - 1 - y) */
    /* clip_y > 0: from row clip_y down, the columns from clip_x on are row clip_y - 1's (an E without
     * its bottom arm is an F: the label letters' only capital F is the narrow one of Fixed-Camera Mode) */
    unsigned char clip_x, clip_y;
} Piece;

/* The Gallery panel's headings (mu_mods_lettering.c mu_mods_swap_panel_pictures). */
enum { H_CHARACTERS, H_STAGES, H_MANAGE, H_PARTICLES, H_SCREENS, HEADINGS };

/* mu_mods_walk()'s finds: the texture animations of one joint. */
typedef struct Tables { HSD_TexAnim* anim[4]; int count; } Tables;

extern HSD_ImageDesc mu_mods_label_desc[LABELS];
/* A category's label dimmed, for its row while the release has it off (as the menus dim a locked row,
 * shim/mu_mex_menu.c): each pixel at 2/5 of its intensity, its alpha kept. */
extern HSD_ImageDesc mu_mods_label_dim_desc[MU_MODS_CATEGORIES];
extern HSD_ImageDesc mu_mods_heading_desc;
extern int mu_mods_composed;       /* 1 built, -1 the disc's pictures are not the ones the letters come from */
extern HSD_ImageDesc mu_mods_preview_desc;
extern int mu_mods_more_composed;  /* 1 built, -1 not the 1.02 pictures (the preview and Gallery keep theirs) */
int mu_mods_walk(HSD_MatAnimJoint* joint, int index, int want, Tables* out, int depth);
int mu_mods_build(HSD_TexAnim* cursor, HSD_TexAnim* panel);
void mu_mods_put(HSD_TexAnim* anim, int k, HSD_ImageDesc* picture);
int mu_mods_build_more(HSD_TexAnim* panel84, HSD_TexAnim* panel85, HSD_TexAnim* contop22);
/* Toy_80307470's panel (mu_mods_gallery_panel): its headings and its list's side label. */
void mu_mods_swap_panel_pictures(HSD_JObj* jobj, int depth);

#endif
