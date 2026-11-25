/* Training Mode CE, native build: whether it may run (shim/mu_tmce.c loads it and gates its hooks).
 * Apart from the game headers, for the host's option bits (mu_host.h's stdint.h and the decomp's
 * platform.h disagree on ssize_t). A build without TM-CE (-DMU_TMCE=OFF) has shim/mu_tmce_off.c
 * instead, where it is always 0.
 *
 * TM-CE: Training Mode - Community Edition, by UnclePunch and the TM-CE contributors. */
#include "mu_host.h"
#include "mu_native.h"

/* Its disc files are in the mod profile on the vanilla game, and this is not an online session. */
int mu_tmce_enabled(void)
{
    const unsigned int options = mu_game_options();
    if (!(options & MU_GAME_OPTION_TMCE) || (options & MU_OPTION_VANILLA)) {
        return 0;
    }
    return !(mu_online_active() || mu_online_pending() || mu_slippi_in_online_mode());
}
