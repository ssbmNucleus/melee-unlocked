/* Per-player effect files (prototype; shim/mu_player_fx.c). Off unless the host serves a variant
 * effect file for a port (MELEE_TEST_PLAYER_FX, port/runtime/host/player_fx_test.cpp): then nothing
 * here reads or writes any game state, and every hook returns what it was given.
 *
 * A variant is loaded into a free effect bank (22-29, the "alias"), keeping the file's own first
 * generator id, so the ids stay the character's (4xxx for Falcon) and only the bank changes. The
 * bank is swapped where it is used: efLib_Create (model effects) and hsd_8039F05C (particle
 * generators). Which player a spawn belongs to comes from a scope the entry points open
 * (efSync_Spawn, efAsync_Spawn, the async queue, efLib_Create, efLib_CreateGenerator*,
 * efLib_SpawnParticleEffect, the particle-key callbacks): their owner gobj when they have one,
 * else the owner of the joint a particle key fired on, else the gobj whose proc is running. Spawns
 * the particle system makes itself (a generator's program) keep the bank of their parent.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MU_PLAYER_FX_H
#define MU_PLAYER_FX_H

enum {
    MU_FX_SITE_DIRECT = 0, /* a generator made with no entry point open */
    MU_FX_SITE_SYNC,       /* efSync_Spawn */
    MU_FX_SITE_ASYNC,      /* efAsync_Spawn */
    MU_FX_SITE_DEFERRED,   /* efAsync_QueueProcessDeferred */
    MU_FX_SITE_CREATE,     /* efLib_Create */
    MU_FX_SITE_GENERATOR,  /* efLib_CreateGenerator* */
    MU_FX_SITE_PARTICLE,   /* efLib_SpawnParticleEffect */
    MU_FX_SITE_DPTCL,      /* efLib_Cb_DPtcl (a particle key, dynamic) */
    MU_FX_SITE_SPTCL,      /* efLib_Cb_SPtcl (a particle key, static) */
    MU_FX_SITES
};

struct HSD_GObj;
struct HSD_JObj;
struct Fighter;

typedef struct MuFxScope {
    signed char active;
    signed char port;        /* the port the spawn is for; -1 none */
    signed char owner_port;  /* the outermost entry point's owner, and the running proc's gobj: */
    signed char proc_port;   /* logged when both are known and differ ("cross") */
    unsigned char site;
    void* caller;
} MuFxScope;

extern int mu_fx_on;        /* a variant is loaded in this scene */
extern int mu_fx_internal;  /* inside the particle procs: spawns inherit their parent's bank */
extern signed char mu_fx_generation;  /* 1-127, new each scene a variant loads: EF_Effect x2B */

MuFxScope mu_fx_scope_begin(int site, struct HSD_GObj* owner, int port_hint, void* caller);
void mu_fx_scope_end(MuFxScope* saved);
int mu_fx_remap(int bank, int id);
int mu_fx_scope_port(void);
int mu_fx_queue_port(int gfx_id);   /* efAsync_Spawn: the port its queue entry is for */
struct HSD_GObj* mu_fx_jobj_owner(struct HSD_JObj* jobj);
void mu_fx_player_prepare(struct Fighter* fp);
void mu_fx_player_reset(void);

/* Opens a scope for the rest of the enclosing block (GCC cleanup: closed on every return). */
#define MU_FX_SCOPE(site, owner)                                                                   \
    MuFxScope mu_fx_scope_ __attribute__((cleanup(mu_fx_scope_end))) =                             \
        mu_fx_scope_begin((site), (owner), -1, __builtin_return_address(0))
#define MU_FX_SCOPE_PORT(site, owner, port)                                                        \
    MuFxScope mu_fx_scope_ __attribute__((cleanup(mu_fx_scope_end))) =                             \
        mu_fx_scope_begin((site), (owner), (port), __builtin_return_address(0))
#define MU_FX_BANK(bank, id) (mu_fx_on ? mu_fx_remap((bank), (id)) : (bank))
/* The joint's owner, looked up only while a variant is loaded (the scope ignores it otherwise). */
#define MU_FX_JOBJ_OWNER(jobj) (mu_fx_on ? mu_fx_jobj_owner(jobj) : (struct HSD_GObj*) 0)

#endif
