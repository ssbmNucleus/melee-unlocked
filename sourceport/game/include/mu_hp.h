/* 20XX Hack Pack content features, as native C (shim/mu_hp.c, shim/mu_hp_css.c): what is left of
 * milestones 1 to 7 of run-source/rel09-hackpack/PLAN.md section 3 once the pack's debug menu, its
 * training displays and the host data behind them (the 0xFA ops) were removed (2026-11). One row per
 * code library mod in run-source/rel09-hackpack/ledger_M2.md and ledger_M7.md.
 *
 * None of the pack's code runs. The host loads the pack's disc as a file overlay and says so
 * (MU_MOD_HACKPACK in mod_flags). The functions here are called from the decompiled statement each
 * of the pack's hook addresses lands on and leave the game alone unless the pack's files are loaded
 * and the game is offline. A replay recorded with the pack's stage state (option word 3) plays it
 * back as recorded. */
#ifndef MU_HP_H
#define MU_HP_H

/* The pack's files are loaded (whatever the mode). lb/lbheap.c sizes one heap by it at boot. */
int mu_hp_loaded(void);

/* ---- stages (ledger_M2.md) ---- */

/* The custom flag byte of the stage in play (the pack's byte at 803FA2E5), 0 when none. The stage
 * swap table that set it offline was the host's data and is gone, so only playback of a replay
 * recorded with it sets it now. */
int mu_hp_stage_flags(void);
/* Pokemon Stadium's fixed transformation sets the 0x80 bit once it has transformed. */
void mu_hp_stage_flags_or(int bits);
/* The pack's stage rules are in effect for the match in play (offline with the pack, or playback of
 * a replay recorded with it). For the stage mods that test the game mode, not the flag byte. */
int mu_hp_stage_on(void);
/* gm/gmvs.c, start of a match: takes the stage state for the match (from the replay in playback). */
void mu_hp_match_begin(int stkind);
/* gm/gm_1A3F.c, 801A4160: a match state (id 2) was left for another state through an explicit
 * next state. The pack clears its flag byte there. */
void mu_hp_stage_leave(void);

/* The pack's stage flag bits, as its hooks test them. */
#define MU_HP_FLAG_OFF     0x80   /* hazard off: lava, ship spawns, bullet, logs, background change */
#define MU_HP_FLAG_GUN     0x40   /* Corneria: Great Fox's gun off */
#define MU_HP_FLAG_FIXED   0x78   /* Stadium: a fixed transformation (0x40 fire, 0x20 grass, 0x10 rock, 0x08 water) */

/* "Pokemon Stadium - Disable Transforms via Custom Flag", 801D45F0 (grStadium_801D4548, the
 * countdown test): 1 when this frame must not start a transformation. */
int mu_hp_stadium_no_transform(void);
/* "Pokemon Stadium - Fixed Transformation Stage", 801D463C (same function, in place of the random
 * pick): 0..3 the transformation to take, -1 for the game's own random pick. */
int mu_hp_stadium_fixed(void);
/* "Pokemon Stadium Frozen Transform Immediately", 801D1538 (grStadium_801D1520): 1 when the
 * transformation logic must run although the stage is frozen. */
int mu_hp_stadium_run_frozen(void);
/* The Stage Swap Engine's stage file patch for a fixed transformation stage (80018130, in
 * lbDvd_GetPreloadedArchive): the transformation timings zeroed so it transforms at once. Returns
 * `param`, or a copy with those words zeroed. gr/grpstadium.c, where OnInit takes the parameters. */
const void* mu_hp_stadium_param(const void* param, unsigned int size);

/* The 20XX TE features the pack's debug menu used to switch without a TE save. The host still passes
 * these bits alone with the pack loaded (port/app/source_host.cpp, kHpTeOptions and kHpTeOptions2),
 * and mu_te.c accepts them then, and in playback of a replay that recorded them. The 0x780000 bits
 * are the L-cancel flash choices (mu_lcancel_flash.h). */
#define MU_HP_TE_OPTIONS  (MU_TE_NO_STAR_KO | MU_TE_TAUNT_CANCEL | MU_TE_FIXED_CAMERA)
#define MU_HP_TE_OPTIONS2 (MU_TE2_NO_SCREEN_RUMBLE | MU_TE2_LCANCEL_FLASH | 0x00780000u | MU_TE2_BUBBLES | \
                           MU_TE2_INPUT_DISPLAY | MU_TE2_COLOR_OVERLAYS)

struct Fighter;

/* ---- extra characters (ledger_M7.md, shim/mu_hp_css.c) ---- */

/* The pack's character select features are in effect: the pack is loaded, the normal game,
 * offline, outside replay playback. */
int mu_hp_css_live(void);
/* The pack's every frame code on the character select screen (the branch at 80263350, the part at
 * 80FD03EC): Z pressed alone by a player who holds a coin over one of seven icons swaps that
 * icon's character with its hidden one. `icon` is the icon's index in the game's table, `ckind`
 * the character it shows now; returns the character to write into the icon, -1 for none. */
int mu_hp_css_swap(int icon, int ckind);
/* The character an icon goes back to when the screen opens without the pack's features (an
 * online screen must never offer a hidden fighter): the pair's normal character for the six non
 * playable ones, `ckind` itself for anything else. Works whatever the mode. */
int mu_hp_css_base(int ckind);
/* "Disable X/Y Alt Costumes for Extra Characters", 802600D8 (mnCharSel_CostumeChange), and "CSS B
 * Button Return Cursor Fix", 8025FE50 (mnCharSel_8025FDEC): 1 for a character past the playable
 * ones (Master Hand to Popo) while the pack's features are in effect. */
int mu_hp_css_extra(int ckind);
/* "Extra Chars Don't Reset Port If Unavailable", 80264EEC (mnCharSel_802640A0, where a port whose
 * character is on no icon is emptied): the icon the port's coin goes to instead, -1 for the game's
 * own way. `slot_type` is the port's (3 is closed). */
int mu_hp_css_keep_icon(int ckind, int slot_type);
/* "Extra Character Nametag Changes" (data, 803D4F7C and on): the pack's name for a character, NULL
 * for the game's own. `english` is the saved language. gm/gm_1601.c, where a character's name is
 * looked up. */
const char* mu_hp_ckind_name(int ckind, int english);
/* "Fighting Wireframes And Popo Announcer", 80168C70 (gm_80168C5C): the announcer call for a
 * character the game has none for, 0 for the game's own. */
int mu_hp_announce(int ckind);
/* "Giga-Bowser & Sandbag Always Fall On Match Start", 80069328 (Fighter_Create, where the entry
 * is picked): 1 when this fighter starts the match falling instead of on its entry. */
int mu_hp_spawn_falling(const struct Fighter* fp);
/* "MasterHand & CrazyHand Controlled By All Ports", 801508B8 (ftMh_MS_341_80150894) and 80156AFC
 * (ftCh_Init_80156AD8): the buttons a hand's player commands come from. `pad` is what the game
 * reads (the third controller for Master Hand, the fourth for Crazy Hand); with the pack's rules
 * in effect the fighter's own held buttons come back instead. */
unsigned int mu_hp_boss_buttons(const struct Fighter* fp, unsigned int pad);
/* "CrazyHand - Disable D-Pad Up+B Attack", 80156CE0, and "Crazy Hand Tag Team Fix", 8015C2F8
 * (ftBossLib_8015C2E0): 1 while the pack's match rules are in effect (mu_hp_stage_on). */
int mu_hp_boss_rules(void);

#endif
