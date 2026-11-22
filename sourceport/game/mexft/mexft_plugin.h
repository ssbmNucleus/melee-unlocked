/* A fighter plugin: one m-ex fighter's code, compiled from its MexTK C (mexft/), as a file of its
 * own that the game loads at startup instead of having it linked in.
 *
 * The file is one relocatable object (COFF, x86-64: tools/mexft/mexft_plugin.py makes it from the
 * fighter's modules and its descriptor). The game links it when it loads it (mexft_plugins.c):
 * it places the sections in the low 4 GB next to the game image, where the game's own code and
 * statics are (32-bit slots hold pointers, and the code reaches the game with 32-bit offsets), and
 * gives each game symbol the object uses the address it has in the running library, by name, from
 * melee_game.syms beside the library. So a plugin has no address of its own and is not built for
 * one build of the library. What it IS built for is the layouts it was compiled with: the ABI id
 * below, which the game compares with its own.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MEXFT_PLUGIN_H
#define MU_MEXFT_PLUGIN_H

#define MU_FIGHTER_PLUGIN_MAGIC 0x5446554Du /* "MUFT" */
#define MU_FIGHTER_PLUGIN_VERSION 2u
#define MU_FIGHTER_PLUGIN_SYMBOL "mu_fighter_plugin"
#define MU_FIGHTER_PLUGIN_EXTENSION ".mufighter"

/* One fighter of a plugin. */
typedef struct MuFighterPluginEntry {
    /* The fighter's descriptor (MuAkFighter). */
    const void* descriptor;
    /* What the fighter is on a disc, so the plugin follows the fighter and not the slot a build
     * happened to give it: the name MxDt gives the fighter (its "Name" in MexManager), compared
     * without case. NULL: only the descriptor's file name finds it. */
    const char* mex_name;
} MuFighterPluginEntry;

typedef struct MuFighterPlugin {
    unsigned int magic;
    unsigned int version;
    /* The layouts the plugin's code was compiled against (the MexTK headers, mexft/mex.h, the
     * adapter and the descriptor's header: tools/mexft/mexft_abi.py). */
    unsigned char abi[16];
    const MuFighterPluginEntry* fighters;
    int fighter_count;
} MuFighterPlugin;

/* ---- the game's side (mexft_plugins.c) ---- */
/* The descriptor of a fighter among the loaded plugins: the one whose m-ex name is `mex_name`
 * (may be NULL), else the one whose file is `file`; NULL when none. The first call loads the
 * plugins. */
const void* mexft_plugin_fighter(const char* file, const char* mex_name);
/* Looks in the folder again and loads the plugins put there since the last look (a download, a
 * file copied in while the game runs); returns how many it loaded. Called between major modes
 * (shim/mu_mex.c mu_mex_boot), where no fighter exists: the registry is then built again. A plugin
 * already loaded stays as it is. */
int mexft_plugins_rescan(void);
/* The loaded plugins' variables as snapshot ranges ({address, size} pairs, MuStateRegion's
 * layout); returns how many there are, and writes at most `capacity`. */
unsigned int mexft_plugin_ranges(void* out, unsigned int capacity);
/* A digest of the plugin file a descriptor came from (its name and contents), 0 when the
 * descriptor is no plugin's: two games that play each other with that fighter must have the same
 * file. It goes into the online state checksum (shim/mu_online.c compute_checksum). */
unsigned int mexft_plugin_digest(const void* descriptor);

#endif
