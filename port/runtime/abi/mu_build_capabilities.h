// Optional peer features. Preserve their established bit positions on the wire.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define MU_BUILD_CAPS2_GONE_OWN_FRAMES 0x04 /* reads a player who left by their own frames (slippi_gone_port.h) */
#define MU_BUILD_CAPS2_ARENA 0x08          /* takes arena room messages (NP_MSG_MU_ARENA); upstream sends caps 0x10 */
#define MU_BUILD_CAPS2_AUTO_LCANCEL 0x10    /* presses for L-cancels this session; upstream sends caps 0x20 */
#define MU_BUILD_CAPS2_GECKO 0x20          /* takes Gecko code messages (NP_MSG_MU_GECKO); upstream sends caps 0x40 */
#define MU_BUILD_CAPS2_WARMUP 0x40         /* takes and sends warm-up messages (NP_MSG_MU_WARMUP); upstream sends caps 0x80 */

static inline int mu_gone_own_frames_capable(unsigned caps2) { return (caps2 & MU_BUILD_CAPS2_GONE_OWN_FRAMES) != 0; }
