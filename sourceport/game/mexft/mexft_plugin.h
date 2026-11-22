/* A fighter plugin: one m-ex fighter's code, compiled from its MexTK C (mexft/), as a file of its
 * own that the game loads at startup instead of having it linked in.
 *
 * A plugin is linked for ONE build of the game library: every game function and object its code
 * uses is the address that symbol has in that build (tools/mexft/mexft_plugin.py), so it imports
 * nothing and the game library exports nothing for it. It is linked at a fixed address in the low
 * 4 GB, next to the game image, for the same reasons the game is (32-bit slots that hold pointers).
 * The game loads a plugin only when the build id it was linked for is the running game's.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_MEXFT_PLUGIN_H
#define MU_MEXFT_PLUGIN_H

#define MU_FIGHTER_PLUGIN_MAGIC 0x5446554Du /* "MUFT" */
#define MU_FIGHTER_PLUGIN_VERSION 1u
#define MU_FIGHTER_PLUGIN_SYMBOL "mu_fighter_plugin"
#define MU_FIGHTER_PLUGIN_EXTENSION ".mufighter"

typedef struct MuFighterPlugin {
    unsigned int magic;
    unsigned int version;
    /* The build id of the game library the plugin was linked for (the 16 bytes of the library's
     * CodeView record, written by the linker from the image's contents). */
    unsigned char game_id[16];
    /* The address the plugin was linked at. It works nowhere else. */
    unsigned long long base;
    /* Runs the plugin's constructors (the FtState to MotionState conversion). Called once. */
    void (*init)(void);
    /* The fighters' descriptors (MuAkFighter). */
    const void* const* fighters;
    int fighter_count;
    /* The plugin's own variables: a rollback snapshot holds them with the game's. */
    char* data_lo;
    char* data_hi;
    char* bss_lo;
    char* bss_hi;
} MuFighterPlugin;

/* ---- the game's side (mexft_plugins.c) ---- */
/* The descriptor of the fighter file `file` among the loaded plugins, NULL when none has it. The
 * first call loads the plugins. */
const void* mexft_plugin_fighter(const char* file);
/* The loaded plugins' variables as snapshot ranges ({address, size} pairs, MuStateRegion's
 * layout); returns how many there are, and writes at most `capacity`. */
unsigned int mexft_plugin_ranges(void* out, unsigned int capacity);

#endif
