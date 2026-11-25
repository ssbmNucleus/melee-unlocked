/* Every command id of the game -> host command channel, in one place. Both sides include this file.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The channel: the game calls mu_online_abi_command / mu_slippi_command (sourceport/game/shim/
 * mu_online_abi.c, mu_entry.c) -> MuHostApi.slippi_command -> h_slippi_command (port/app/source_host.cpp)
 * -> slippi::online::native_command (port/runtime/hle/native_slippi_bridge.h; the MELEE_NO_SLIPPI build:
 * host::netplay::dispatch in port/runtime/host/netplay_state.h) -> the lambda in h_slippi_command (the
 * private ids) -> slippi::online::handle (Slippi's ids). The game asks, the host answers; the host never
 * pushes. Results: 0 ok, -1 nobody answers the id, -2 bounds, -3 needs native rollback.
 *
 * Rules (each one has bitten):
 * - The reply buffer must hold MU_SLIPPI_RESPONSE_CAPACITY (4096, mu_host.h) bytes, or every call fails
 *   with -2 before any handler runs: `static unsigned char reply[4096]`.
 * - command_payload_valid (native_slippi_bridge.h) runs first and gives Slippi's ids fixed payload sizes.
 *   A private command on a Slippi number inherits that number's size rule (0xFA: 0 or 5 bytes only).
 * - Use these names on both sides. No local #define, enum or literal for a command id anywhere else.
 *   This file includes nothing (game files must not see the C library's types through it).
 * - A new id: one line in MU_COMMAND_LIST with a number of its own. Two entries with the same number do
 *   not compile (mu_command_id_listed below: a duplicate case label).
 * - A second command on a taken id (told apart by payload length or first byte) goes in
 *   MuCommandSharedId below as `= <the owner's name>`, and its branch in the host lambda must come
 *   before any branch that would take the same id and length.
 *
 * Free ids: 0xC7-0xC8, 0xCA-0xCD, 0xCF, 0xD9, 0xDB-0xDD, 0xE0, 0xE6, 0xEA, 0xEF, 0xFC, 0xFE, 0xFF.
 *
 * Netplay message ids are a different id space and are not here: the first byte of a packet between
 * two players (0xE0 MU_BUILD, 0xE4 pick preview, 0xE6 setup hash, 0xE7 reserved, 0xEA skins, ...). They
 * live in port/runtime/hle/slippi_net.h (enum NetMsg) and the wire headers beside this file
 * (mu_pick_preview.h, mu_setup_hash.h, port/runtime/hle/mu_skin_wire.h). A command and a message that
 * share a number do not clash. */
#ifndef MU_COMMANDS_H
#define MU_COMMANDS_H

/* X(name, id): every id that owns its number. The comment: who sends it, what it does. */
#define MU_COMMAND_LIST(X)                                                                                     \
    /* ---- Slippi's online commands: Slippi's numbers and payload sizes, answered by                    */  \
    /* slippi::online::handle (port/runtime/hle/slippi_online.cpp) or by a --p2p-* session's handler.   */  \
    X(MU_CMD_ONLINE_INPUTS, 0xB0)          /* mu_online.c, each online frame: the local pad; reply the remote pads */ \
    X(MU_CMD_CAPTURE_SAVESTATE, 0xB1)      /* mu_online.c: keep a rollback snapshot of this frame */                  \
    X(MU_CMD_LOAD_SAVESTATE, 0xB2)         /* mu_online.c: roll back to a kept snapshot */                            \
    X(MU_CMD_GET_MATCH_STATE, 0xB3)        /* mu_online.c, mu_slippi_menu.c: the match state block (MSRB) */          \
    X(MU_CMD_FIND_OPPONENT, 0xB4)          /* mu_slippi_menu.c: start a search (mode, connect code) */                \
    X(MU_CMD_SET_MATCH_SELECTIONS, 0xB5)   /* mu_slippi_menu.c: the online character and stage picks */              \
    X(MU_CMD_OPEN_LOGIN, 0xB6)             /* mu_slippi_menu.c: the Log in row */                                     \
    X(MU_CMD_LOGOUT, 0xB7)                 /* Slippi's; the Source Port game does not send it */                      \
    X(MU_CMD_UPDATE_APP, 0xB8)             /* mu_slippi_menu.c: the Update row */                                     \
    X(MU_CMD_GET_ONLINE_STATUS, 0xB9)      /* mu_slippi_menu.c: login state, name and connect code */                 \
    X(MU_CMD_CLEANUP_CONNECTION, 0xBA)     /* mu_slippi_menu.c, mu_slippi_css.c: leave the session (reason byte) */   \
    X(MU_CMD_SEND_CHAT_MESSAGE, 0xBB)      /* mu_slippi_menu.c, maker/platform/online_sync.c: a chat message */       \
    X(MU_CMD_GET_NEW_SEED, 0xBC)           /* mu_slippi_menu.c: the match's random seed */                            \
    X(MU_CMD_REPORT_GAME, 0xBD)            /* mu_online.c: the finished game, for Slippi's report */                  \
    X(MU_CMD_FETCH_CODE_SUGGESTION, 0xBE)  /* mu_slippi_menu.c name entry: a connect code suggestion */               \
    X(MU_CMD_OVERWRITE_SELECTIONS, 0xBF)   /* mu_ranked_setup_online.c: a Ranked set's agreed picks */                \
    X(MU_CMD_GP_COMPLETE_STEP, 0xC0)       /* mu_ranked_setup_online.c: a game-prep step done (5 bytes) */            \
    X(MU_CMD_GP_FETCH_STEP, 0xC1)          /* mu_ranked_setup_online.c: the other side's game-prep step */            \
    X(MU_CMD_REPORT_SET_COMPLETE, 0xC2)    /* mu_ranked_setup_online.c, mu_slippi_menu.c: a set ended (mode) */       \
    X(MU_CMD_GET_PLAYER_SETTINGS, 0xC3)    /* mu_slippi_chat.c: the players' chat message lists */                    \
    X(MU_CMD_REPORT_MATCH_STATUS, 0xC4)    /* mu_online.c, mu_ranked_setup_online.c: a match status update */         \
    X(MU_CMD_GET_DELAY, 0xD5)              /* Slippi's; the Source Port game does not send it */                      \
    X(MU_CMD_MUSIC_PLAY, 0xD6)             /* mu_audio.c: start a song (disc offset, length) on the host jukebox */   \
    X(MU_CMD_MUSIC_STOP, 0xD7)             /* mu_audio.c: stop the jukebox */                                         \
    X(MU_CMD_MUSIC_VOLUME, 0xD8)           /* mu_audio.c: the game's music volume (1 byte) */                         \
    X(MU_CMD_GET_RANK, 0xE3)               /* mu_ranked_rank.c, mu_ranked_set.c: the local player's rank */           \
    X(MU_CMD_FETCH_RANK, 0xE4)             /* mu_ranked_rank.c: fetch the rank after a set */                         \
    X(MU_CMD_GET_RANK_VISIBILITY, 0xE5)    /* Slippi's; the Source Port game does not send it */                      \
    X(MU_CMD_NATIVE_MENU_FLAGS, 0xFA)      /* mu_slippi_menu.c, no payload: which online rows the menu offers */      \
    /* ---- Slippi's EXI device only (the Static Recomp, port/runtime/hle/exi_slippi.cpp): kept free. */             \
    X(MU_CMD_EXI_LOG_MESSAGE, 0xD0)        /* reserved */                                                             \
    X(MU_CMD_EXI_FILE_LENGTH, 0xD1)        /* reserved */                                                             \
    X(MU_CMD_EXI_FILE_LOAD, 0xD2)          /* reserved */                                                             \
    X(MU_CMD_EXI_GCT_LENGTH, 0xD3)         /* reserved */                                                             \
    X(MU_CMD_EXI_GCT_LOAD, 0xD4)           /* reserved */                                                             \
    X(MU_CMD_EXI_PREMADE_TEXT_LENGTH, 0xE1) /* reserved */                                                            \
    X(MU_CMD_EXI_PREMADE_TEXT_LOAD, 0xE2)  /* reserved */                                                             \
    /* ---- This port's own commands, answered in the h_slippi_command lambda (source_host.cpp). ---- */             \
    X(MU_CMD_ONLINE_CONTENT, 0xC6)         /* mu_slippi_css.c, op byte first: custom fighters online (online_content_command) */ \
    X(MU_CMD_MOD_BROWSER, 0xC5)            /* sub-command byte first: Mods menu, gallery, roster, plugins, port costume, CSS skins (mod_browser_command) */ \
    X(MU_CMD_LOBBY, 0xC9)                  /* mu_lobby_list.c, op byte first: doubles lobbies (lobby_command) */      \
    X(MU_CMD_NEW_STAGES, 0xCE)             /* mu_new_stages.c, op byte first: the stage select's new stages (new_stages_command) */ \
    X(MU_CMD_CSS_FRAME, 0xDA)              /* mu_slippi_css.c, every CSS frame (0 or 52 bytes: camera) -> sound id; 5 bytes: a hand moved */ \
    X(MU_CMD_ZELDA_AS_SHEIK, 0xDE)         /* mu_slippi_css.c: a human's Zelda starts as Sheik (setting, offline) */  \
    X(MU_CMD_BUTTON_LABEL, 0xDF)           /* mu_slippi_css.c: the key bound to a pad button (port, PAD bit BE16) */  \
    X(MU_CMD_CPU_AI, 0xE7)                 /* mu_cpu_ai.c, op byte first: the CPU AI picker's choices, offline */     \
    X(MU_CMD_FRAME_EVENT, 0xE8)            /* mu_framedata.c: a replay frame event for the coach */                   \
    X(MU_CMD_REPLAY_DEFENSE, 0xE9)         /* mu_replay_defend.c: a takeover's opponent settings (MuPcConfig) */      \
    X(MU_CMD_STAGE_SONGS, 0xEB)            /* gr/ground.c (decomp): a stage's main and alternate songs (16 bytes) */  \
    X(MU_SQUAD_COMMAND, 0xEC)              /* mu_squad*.c, op byte first: Squad Strike (mu_squad_wire.h) */           \
    X(MU_PICK_PREVIEW_COMMAND, 0xED)       /* mu_slippi_menu.c: live pick previews, send or read (mu_pick_preview.h) */ \
    X(MU_MAKER_COMMAND, 0xEE)              /* maker/, op byte first: Melee Maker's data (mu_host.h MU_MAKER_OP_*) */  \
    X(MU_CMD_SELFTEST_KEEP, 0xF0)          /* mu_online.c, native self-test only: keep the state as a reference */    \
    X(MU_CMD_SELFTEST_COMPARE, 0xF1)       /* mu_online.c, native self-test only: compare with the reference */       \
    X(MU_CMD_LAB_SAVE, 0xF2)               /* mu_lab.c: Training Lab save state */                                    \
    X(MU_CMD_LAB_LOAD, 0xF3)               /* mu_lab.c: Training Lab load state */                                    \
    X(MU_CMD_LAB_ADVANTAGE, 0xF4)          /* mu_lab.c: frame advantage for the overlay (s32 BE, kind) */             \
    X(MU_CMD_CONTENT_MODE, 0xF5)           /* mu_content.c: the online mode about to play -> 1 retail view */         \
    X(MU_CMD_PRACTICE_CPU, 0xF6)           /* mu_practice_cpu.c: no payload -> MuPcConfig; 2 or 3 bytes: its status */ \
    X(MU_CMD_TE_MENU_MUSIC, 0xF7)          /* mu_te.c: 20XX TE's menu song (0x80 asks, else sets) */                  \
    X(MU_CMD_TE_SETTINGS, 0xF8)            /* mu_te_debugmenu.c: the TE feature words (0 asks, 8 sets) */              \
    X(MU_CMD_SKIN_CYCLE, 0xF9)             /* mu_content.c: CSS L / R steps a costume through its installed skins */  \
    X(MU_CMD_STAGE_SKIN_CYCLE, 0xFB)       /* mu_content.c: stage select steps a stage through its skins */           \
    X(MU_CMD_STAGE_SKIN_MATCH, 0xFD)       /* mu_content.c: a match's random stage skin, before the stage loads */

#define MU_COMMAND_ENUM_(name, id) name = id,
enum MuCommandId { MU_COMMAND_LIST(MU_COMMAND_ENUM_) MU_COMMAND_LIST_END_ };
#undef MU_COMMAND_ENUM_

/* Commands that share a number with an entry of the list, told apart by payload length or first byte.
 * Each one names the entry whose number it takes, never a number of its own. */
enum MuCommandSharedId {
    /* 0xC0 with no payload: mu_p2p_entry.c asks a --p2p-* session (source_p2p.cpp) whether another game
     * of the series follows; one reply byte. */
    MU_CMD_SERIES_STATE = MU_CMD_GP_COMPLETE_STEP,
    /* 0xDA with 2 bytes: mu_online_options.c, an online select option {option, value or 0xFF}. */
    MU_CMD_ONLINE_OPTION = MU_CMD_CSS_FRAME,
    /* 0xFA with 5 bytes: mu_replay.c, the replay viewer's gate (s32 BE frame, 0 ask / 1 restore). */
    MU_CMD_REPLAY_GATE = MU_CMD_NATIVE_MENU_FLAGS
    /* (0xFA with an op byte first was the 20XX Hack Pack's data and settings, MU_HP_COMMAND: removed
     * 2026-11 with the pack's debug menu. Its op bytes are not reused.) */
};

/* The collision check: every number in MU_COMMAND_LIST is a case label here, so two entries with the
 * same number stop the build on both sides ("duplicate case value"). */
#define MU_COMMAND_CASE_(name, id) case id:
static inline int mu_command_id_listed(unsigned int id)
{
    switch (id) {
    MU_COMMAND_LIST(MU_COMMAND_CASE_)
        return 1;
    default:
        return 0;
    }
}
#undef MU_COMMAND_CASE_

#endif
