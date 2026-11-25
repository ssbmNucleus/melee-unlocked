/* Compile with the native game's mu_online.c include paths and preprocessor flags.
 * Include the implementation to exercise the same private prediction buffers as the game.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../sourceport/game/shim/mu_online.c"

unsigned long long mu_ticks_since_start(void) { return 0; }
unsigned int mu_online_abi_retrace_count(void) { return 0; }
void mu_online_abi_log(const char* text) { (void) text; }
int printf(const char* format, ...);

int main(void)
{
    const int predicted_frame = 100;
    u8* early = mu_online.rxb + RXB_EARLY;
    int triggered;

    mu_online.savestate_predicting = 1;
    mu_online.player_predicting[0] = 1;
    mu_online.player_savestate_frame[0] = predicted_frame;
    mu_online.predicted_write[0] = 1;
    put32(mu_online.rxb + RXB_OPNT_FRAME_NUMS, predicted_frame - 1);
    mu_online.rxb_size = RXB_EARLY + REMOTES + EARLY_ENTRY;
    early[0] = 1;
    put32(early + 1, predicted_frame);
    early[5] = 1; /* Remote A press contradicts the predicted neutral pad. */
    triggered = check_predictions(predicted_frame);
    if (!triggered) {
        /* The next tick carries the confirmed pad for that same frame. */
        put32(mu_online.rxb + RXB_OPNT_FRAME_NUMS, predicted_frame);
        mu_online.rxb[RXB_OPNT_INPUTS] = 1;
        triggered = check_predictions(predicted_frame + 1);
    }
    printf("Early rollback enabled %d, first rollback on tick %d, rewind to %d.\n",
           early_rollback_on(), mu_online.rollback_end_frame, mu_online.savestate_frame);
    return !(triggered && mu_online.rollback_should_load &&
             mu_online.savestate_frame == predicted_frame &&
             mu_online.rollback_end_frame == predicted_frame + !early_rollback_on());
}
