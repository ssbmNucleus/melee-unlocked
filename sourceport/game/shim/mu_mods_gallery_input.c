/* The Mods menu's list (ty/tylist.c): its keys. Up and down move a row (the window moves with the
 * cursor; more posts are asked for near the end of what is read), L and R step through the fighters
 * (the lobby list: read it again), A on a row that is no card does that row's own thing (a fighter, a
 * pack, "Install all", the search, more posts, a retry), B leaves a pack's list or a fighter's posts.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <dolphin/os.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/controller.h>

#include <mu_lobby_list.h>
#include <mu_mods_gallery.h>
#include <mu_native.h>

#include "mu_mods_categories.h"
#include "mu_mods_gallery_internal.h"

/* A row's own A: a fighter, a pack, "Install all", the search, more posts or a retry. The result for
 * the list's scene. */
int mu_mods_row_action(const Row* r, int selected, int line)
{
    if (r->kind == ROW_SEARCH) {
        /* The game's name entry for the words (mn/mnnamenew.c); the list opens again after it. */
        lbAudioAx_80024030(1);
        mu_mods_menu_want_search();
        OSReport("[mods] search: to the name entry\n");
        return MU_MODS_LEAVE;
    }
    if (r->kind == ROW_END_SEARCH) {
        if (!mu_mods_gallery_search("")) return MU_MODS_SWALLOW;
        lbAudioAx_80024030(1);
        return mu_mods_changed();
    }
    if (r->kind == ROW_FIGHTER) {
        if (!mu_mods_host_change_row(CHANGE_PICK_FIGHTER, r->card)) return MU_MODS_SWALLOW;
        lbAudioAx_80024030(1);
        OSReport("[mods] list: fighter %s\n", r->name);
        return mu_mods_changed();
    }
    if (r->kind == ROW_INSTALL_ALL) {
        if (!mu_mods_host_change(CHANGE_INSTALL_ALL)) {
            lbAudioAx_80024030(3);   /* refused: all installed, or a run of them on its way */
            return MU_MODS_SWALLOW;
        }
        lbAudioAx_80024030(1);
        mu_mods_list_tick = LIST_POLL;   /* the rows show it at once */
        return MU_MODS_SWALLOW;
    }
    if (r->kind == ROW_PACK) {
        if (!mu_mods_host_change_row(CHANGE_OPEN_PACK, r->card)) return MU_MODS_SWALLOW;
        mu_mods_pack_row = selected;
        mu_mods_pack_line = line;
        mu_mods_pack_base = mu_mods_base;
        lbAudioAx_80024030(1);
        return mu_mods_changed();
    }
    if (r->kind == ROW_MORE) {
        /* "Could not load more": A asks again (while "Loading...", nothing to do). */
        if (!(mu_mods_head.flags & FLAG_MORE_FAILED) || !mu_mods_host_change(CHANGE_NEXT) || !mu_mods_sync_rows(1, NULL))
            return MU_MODS_SWALLOW;
        lbAudioAx_80024030(1);
        OSReport("[mods] list: more asked for again after %d rows\n", mu_mods_head.cards);
        return mu_mods_moved_to(r->card, selected, line);
    }
    if (r->kind == ROW_MESSAGE && mu_mods_head.status != ST_ERROR) return MU_MODS_SWALLOW;
    if (!mu_mods_host_change(CHANGE_RETRY)) return MU_MODS_SWALLOW;
    lbAudioAx_80024030(1);
    return mu_mods_changed();
}

int mu_mods_gallery_list_back(void)
{
    if (!mu_mods_gallery_running()) return 0;
    if (mu_mods_head.flags & FLAG_PACK) {
        /* B in a pack's list: the posts, where they were. */
        if (!mu_mods_host_change(CHANGE_LEAVE_PACK)) return 0;
        mu_mods_base = mu_mods_pack_base;
        mu_mods_changed_at(mu_mods_pack_row, mu_mods_pack_line);
        return 1;
    }
    /* B in a fighter's (or a stage's) posts: the category's own way back (mu_mods_categories.h). */
    return mu_mods_categories[mu_mods_category]->list_back();
}

/* B in a fighter's posts in Browse: the fighters, on that one. The way back of every category but
 * Stages (mu_mods_categories.h). */
int mu_mods_back_to_fighters(void)
{
    if (!mu_mods_manage && (mu_mods_head.flags & FLAG_FILTER) && !(mu_mods_head.flags & FLAG_FIGHTERS)) {
        /* B in a fighter's posts: the fighters, on that one. */
        if (!mu_mods_host_change(CHANGE_FIGHTERS)) return 0;
        OSReport("[mods] list: back to the fighters\n");
        mu_mods_changed();
        return 1;
    }
    return 0;
}

int mu_mods_gallery_list(int selected, int line, unsigned int trigger, int left, int right)
{
    Head now;
    int differ = 0, h;
    if (!mu_mods_gallery_running()) return MU_MODS_RETAIL;
    mu_mods_spinner_gone();   /* the list's scene: the viewer's texts went with the viewer */
    mu_mods_remove_asked = -1;   /* (Manage: back from the viewer, a removal asks again) */
    mu_mods_replace_asked = -1;  /* (Browse Menus: so does a replacement) */
    if (mu_lobby_list_leaving()) return MU_MODS_LEAVE;   /* a lobby joined: to its Teams select */
    if (mu_mods_leave_for_search) {
        mu_mods_leave_for_search = 0;
        mu_mods_menu_want_search();
        OSReport("[mods] search: to the name entry\n");
        return MU_MODS_LEAVE;
    }
    if (left || right) {
        /* The lobby list: L and R read it again (at most every 15 s, the host keeps to that). */
        if (mu_lobby_list_on()) {
            if (mu_mods_host_change(CHANGE_RETRY)) lbAudioAx_80024030(1);
            mu_mods_list_tick = LIST_POLL;
            return MU_MODS_SWALLOW;
        }
        /* L and R step through the fighters (a fighter's posts in Browse Characters); elsewhere they
         * do nothing. */
        if ((mu_mods_head.flags & FLAG_FILTER) == 0 || (mu_mods_head.flags & (FLAG_FIGHTERS | FLAG_PACK)) != 0 ||
            !mu_mods_host_change(right ? CHANGE_FIGHTER_ON : CHANGE_FIGHTER_BACK))
        {
            return MU_MODS_SWALLOW;
        }
        lbAudioAx_80024030(1);
        return mu_mods_changed();
    }
    if (trigger & (HSD_PAD_A | HSD_PAD_START)) {
        const Row* r = selected >= 0 && selected < mu_mods_row_count ? &mu_mods_rows[selected] : NULL;
        if (r == NULL || r->kind == ROW_CARD) return MU_MODS_RETAIL;   /* the viewer, as the game does */
        if (r->kind == ROW_PACK) {
            /* A pack's own page in the viewer (its post's picture and words); A there lists its cards. */
            mu_mods_viewer_line = line;
            return MU_MODS_RETAIL;
        }
        return mu_mods_row_action(r, selected, line);
    }
    h = mu_mods_host_row(selected);
    mu_mods_hint_prefetch(h, selected, line);
    /* The window moves with the cursor: SLIDE rows on when it nears the window's end with more of the
     * list after it, back when it nears the start of a window that is not the list's first. */
    if (h >= 0 && ((h >= mu_mods_base + WINDOW - EDGE && mu_mods_head.cards > mu_mods_base + WINDOW) || (mu_mods_base > 0 && h < mu_mods_base + EDGE))) {
        const int was = mu_mods_base;
        mu_mods_base += h >= mu_mods_base + WINDOW - EDGE ? SLIDE : -SLIDE;
        if (mu_mods_base < 0) mu_mods_base = 0;
        if (!mu_mods_sync_rows(1, NULL)) {
            mu_mods_base = was;
            return MU_MODS_RETAIL;
        }
        OSReport("[mods] list: rows %d to %d of %d held, row %d selected\n", mu_mods_base,
                 mu_mods_base + (mu_mods_row_count - mu_mods_top) - 1, mu_mods_head.cards, h);
        return mu_mods_moved_to(h, selected, line);
    }
    /* More posts: asked for once the cursor is a screen from the end of what is read (one batch ahead
     * at most; after a failure only A on "Could not load more" asks again). */
    if (mu_mods_head.status == ST_READY && (mu_mods_head.flags & FLAG_AFTER) &&
        !(mu_mods_head.flags & (FLAG_MORE_LOADING | FLAG_MORE_FAILED)) && mu_mods_asked_more_at != mu_mods_head.cards &&
        (h < 0 ? 0 : h) >= mu_mods_head.cards - AHEAD)
    {
        mu_mods_asked_more_at = mu_mods_head.cards;
        if (mu_mods_host_change(CHANGE_NEXT) && mu_mods_sync_rows(1, NULL)) {
            OSReport("[mods] list: more asked for after %d rows (row %d selected)\n", mu_mods_head.cards, h);
            return mu_mods_moved_to(h, selected, line);
        }
    }
    if (++mu_mods_list_tick < LIST_POLL) return MU_MODS_RETAIL;
    mu_mods_list_tick = 0;
    if (!mu_mods_poll_head(&now)) return MU_MODS_RETAIL;
    if (mu_mods_same_layout(&now, &mu_mods_head)) {
        /* The same rows: their words again where they changed (an install, "Install all"'s count). */
        return mu_mods_sync_rows(0, &differ) && differ ? MU_MODS_WORDS : MU_MODS_RETAIL;
    }
    if (mu_mods_same_list(&now, &mu_mods_head)) {
        /* More of it read (or "Loading..." at its end turned to "Could not load more"): the cursor
         * stays where it is. */
        const int was = mu_mods_head.cards;
        if (!mu_mods_sync_rows(1, NULL)) return MU_MODS_RETAIL;
        if (mu_mods_head.cards != was) OSReport("[mods] list: %d rows now (were %d)\n", mu_mods_head.cards, was);
        return mu_mods_moved_to(h, selected, line);
    }
    /* Another list (the first look at it, a fighter's, a retry's): from its top. */
    mu_mods_changed();
    return MU_MODS_REBUILD;
}
