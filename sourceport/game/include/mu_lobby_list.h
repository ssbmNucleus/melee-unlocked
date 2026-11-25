/* Doubles lobbies, the game's half: shim/mu_lobby_list.c (the host's half: port/runtime/host/
 * lobby_directory.h). Online -> Teams opens a submenu of its own (menu kind 36): "Lobbies" (SSBM
 * Nucleus's open Teams lobbies, on the Gallery's list screen), "Create" (host one) and "Enter code"
 * (the code entry, as Teams always had). A lobby is only a Teams code: it is typed into the normal
 * Teams search. Every function is a no-op answer while the feature is off.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_LOBBY_LIST_H
#define MU_LOBBY_LIST_H

struct HSD_GObj;
struct HSD_JObj;
struct HSD_Text;
struct HSD_MatAnimJoint;

#define MU_MENU_KIND_LOBBY 36   /* mn/mnmain.c: the Teams submenu */

/* mn/mnmain.c */
void mu_lobby_menu_load(struct HSD_MatAnimJoint* cursor_matanim, struct HSD_MatAnimJoint* panel_matanim);
void mu_lobby_menu_label(struct HSD_JObj* label, int menu_kind, int selection);
void mu_lobby_menu_describe(struct HSD_Text* text, int menu_kind, int selection);  /* a new description */
void mu_lobby_menu_idle(struct HSD_Text* text, int menu_kind, int selection);      /* every frame it shows */
void mu_lobby_menu_think(struct HSD_GObj* gp);

/* shim/mu_slippi_menu.c */
int mu_lobby_menu_enter(void);   /* Teams confirmed on the online menu: 1 when the submenu took it */
int mu_lobby_menu_return(unsigned char* menu_kind, unsigned char* hovered);   /* back from the online major */
void mu_lobby_left_online(void); /* the online major unloads: the lobby is closed or left */

/* shim/mu_mods_menu.c and shim/mu_mods_gallery.c: the Gallery's list screen as the lobby list */
void mu_mods_menu_open_lobbies(void);   /* shim/mu_mods_menu.c: the Gallery's scenes as the lobby list */
int mu_lobby_list_on(void);
int mu_lobby_list_leaving(void); /* a join came through: the list leaves for the Teams select */
void mu_lobby_list_panel(struct HSD_JObj* panel);
int mu_lobby_list_return(unsigned char* menu_kind, unsigned char* selection);
/* ty/tylist.c: A or START on the list's row `row` joins that lobby at once (there is no lobby page). 1
 * when the press was the lobby list's (joining, or refused with the buzz), 0 for a row that is no
 * lobby (the Gallery's own: "Loading...", a retry), which the Gallery answers as before. */
int mu_lobby_list_press(int row);

/* shim/mu_slippi_css.c */
int mu_lobby_css_search(void);   /* START in Teams: 1 when the lobby's code was typed and is searched */
int mu_lobby_css_staged(void);   /* a lobby's code waits for START */
void mu_lobby_css_cancel(void);  /* Z while searching: the lobby is closed or left */
/* Every frame of the online select (connection state): the status line under the prompts, or NULL. */
const char* mu_lobby_css_frame(int connection_state);

#endif
