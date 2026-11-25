/* Native ranked tier marker. GPL-3.0-or-later. */
#ifndef MU_RANK_BADGE_H
#define MU_RANK_BADGE_H
#include <mu_ranked.h>
struct HSD_GObj;
typedef struct MuRankBadge MuRankBadge;
void* mu_rank_badge_load_css(void); /* Load the scene's Slippi CSS archive and return its public slpCSS symbol. */
/* Coordinates/scale are in SIS world units. parent may be NULL. The text
 * fallback owns its text separately; callers free badges before scene teardown.
 */
MuRankBadge* mu_rank_badge_create(struct HSD_GObj* parent, float x, float y, float scale, const MuPlayerRank* rank);
void mu_rank_badge_set(MuRankBadge* badge, const MuPlayerRank* rank);
void mu_rank_badge_free(MuRankBadge* badge);
#endif
