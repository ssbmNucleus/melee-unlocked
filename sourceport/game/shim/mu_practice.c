/* The practice bridge (game API version 6): the host's Training tools and its way back to Training
 * after an online session (port/runtime/hle/native_practice.cpp) read the scene, pause and step
 * frames, move the CPU, read the card's characters and leave a scene for another through these
 * operations. Ops 1-5, 7, 9, 12 and 13 served the practice search (removed) and are not answered. */
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/types.h>
#include <melee/gm/gmtrainingmode.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#if MU_SQUAD
/* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
#include "mu_squad.h"
#endif
#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/types.h>


/* Same values as MU_PRACTICE_* in mu_host.h (checked in mu_entry.c). */
enum { MU_PR_SCENE, MU_PR_GET_PLAYER, MU_PR_SET_PLAYER, MU_PR_GET_STAGE, MU_PR_SET_STAGE,
       MU_PR_SET_EVENT_BACKUP, MU_PR_REQUEST_MAJOR, MU_PR_SET_ONLINE_MODE, MU_PR_GET_ONLINE_MODE,
       MU_PR_DIRECT_FIRST_MATCH, MU_PR_FRAME_PAUSE, MU_PR_CPU_MOVE, MU_PR_TRAINING_PICK,
       MU_PR_TRAINING_STAGE, MU_PR_UNLOCKED, MU_PR_PAGE_PAUSE, MU_PR_CSS_PICK, MU_PR_FRIENDS_RETURN };

int mu_boot_match_boot(void);
void mu_boot_match_enter_mode(int mode);
enum { MU_PR_BOOT_MATCH = 35 };   /* MU_PRACTICE_BOOT_MATCH, checked in mu_entry.c */
struct gm_80479D58_t* mu_gm_engine_state(void);

/* An explicit scene request (MU_PR_REQUEST_MAJOR: back to Training after an online session) owns
 * one pending destination while the origin scene runs its ordinary decide/unload callbacks. TM-CE's
 * event decide normally selects the menu; that must not replace the requested destination. Clear
 * when the destination is entered.
 * Re-entering the origin completes the handoff too.
 * These are host handoff bookkeeping, never synchronized match state. */
static int practice_pending_mode = -1;
static int practice_origin_mode = -1;

int mu_boot_match_resolve(int requested);
int mu_practice_resolve_pending_mode(int requested)
{
    if (practice_pending_mode >= 0 && gm_GetCurrentGameMode() == practice_origin_mode) {
        return practice_pending_mode;
    }
    return mu_boot_match_resolve(requested);   /* agent-test bootMatch's VS, else the caller's own */
}

void mu_practice_enter_mode(int mode)
{
    mu_boot_match_enter_mode(mode);
    if (practice_pending_mode >= 0) {
        practice_pending_mode = practice_origin_mode = -1;
    }
}

int mu_practice_bridge(int op, int* a, int n);

int mu_practice_bridge(int op, int* a, int n)
{
    MuSlippiMenuState* slp = mu_slippi_state();
    switch (op) {
#if MU_SQUAD
    /* Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026) */
    case MU_SQUAD_TEST_SETUP:
        return gmSquad_OnlineTest(a, n);
#endif
    case MU_PR_BOOT_MATCH:
        return mu_boot_match_boot();
    case MU_PR_SCENE:
        if (n < 2) return -1;
        a[0] = gm_GetCurrentGameMode();
        a[1] = gm_GetCurrentSceneIndex();
        return 0;
    case MU_PR_REQUEST_MAJOR:
        /* Leave the current scene for `major`: the pending mode plus the engine's leave flag, so a
         * Training minor that never ends on its own runs its decide path on the next frame. */
        if (n < 1) return -1;
        practice_origin_mode = gm_GetCurrentGameMode();
        practice_pending_mode = a[0];
        gm_ChangeGameModeAfterCurrentScene(a[0]);
        mu_gm_engine_state()->unk_C = 1;
        return 0;
    case MU_PR_GET_ONLINE_MODE:
        if (n < 1) return -1;
        a[0] = slp->mode;
        return 0;
    case MU_PR_UNLOCKED: {
        /* The characters the memory card has. Training's select cannot pick a locked one. */
        int ck, mask = 0;
        if (n < 1) return -1;
        for (ck = 0; ck < 26; ++ck)
            if (gm_IsCKindUnlocked((u8) ck)) mask |= 1 << ck;
        a[0] = mask;
        return 0;
    }
    case MU_PR_CPU_MOVE: {
        /* Frame advance: the D-pad moves the CPU while the game is frozen (Practice > Tools). */
        int i;
        if (n < 2) return -1;
        for (i = 0; i < 4; ++i) {
            HSD_GObj* gobj;
            Fighter* fp;
            if (Player_GetPlayerSlotType(i) != Gm_PKind_Cpu) continue;
            gobj = Player_GetEntity(i);
            if (gobj == NULL) continue;
            fp = GET_FIGHTER(gobj);
            fp->cur_pos.x += a[0] / 100.0f;
            fp->cur_pos.y += a[1] / 100.0f;
            fp->prev_pos = fp->cur_pos;
            OSReport("practice: CPU moved to x %.1f\n", fp->cur_pos.x);
            return 0;
        }
        return -1;
    }
    case MU_PR_FRAME_PAUSE: {
        /* Frame advance (Practice > Tools): the game's own debug pause and frame step. */
        struct gm_80479D58_t* e = mu_gm_engine_state();
        if (n < 2) return -1;
        if (a[0] > 0) e->unk_10.x0 |= 1;
        else if (a[0] == 0) e->unk_10.x0 &= ~1;
        if (a[1]) e->unk_10.x2 |= 1;
        a[0] = e->unk_10.x0 & 1;
        return 0;
    }
    }
    return -1;
}

MU_EXCLUSIONS(practice, MU_EXCLUDE(practice_pending_mode),
              MU_EXCLUDE(practice_origin_mode))
