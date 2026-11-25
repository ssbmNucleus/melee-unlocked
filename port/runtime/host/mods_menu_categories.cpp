// The Mods menu's categories: what the site calls each, the catalog kind each installs as, its words.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mods_menu_internal.h"

namespace host::mods_menu {
namespace menu_internal {
int category_of(int c) {
  return c == kStages || c == kEffects || c == kFighters || c == kNewStages || c == kMenus ? c : kCharacters;
}
const char* site_type(int c) {
  return c == kStages ? "stage_skin" : c == kEffects ? "effects" : c == kMenus ? "menu" : c == kNewStages ? "custom_stage" : "costume";
}
const char* file_type(int c) { return c == kStages ? "stage_dat" : c == kEffects ? "effects_dat" : "character_dat"; }
const char* asset_kind(int c) {
  return c == kStages ? "stage_visual" : c == kEffects ? "effect_visual" : c == kMenus ? "menu_visual"
       : c == kNewStages ? "new_stage" : "character_costume";
}
const char* category_name(int c) {
  return c == kFighters ? "Fighters" : c == kStages ? "Stages" : c == kEffects ? "Effects" : c == kMenus ? "Menus"
       : c == kNewStages ? "New Stages" : "Characters";
}
// Characters and Effects are a fighter's: their lists start with the fighters.
bool by_fighter(int c) { return c != kStages && c != kFighters && c != kMenus && c != kNewStages; }
// "5 skins", "5 stage skins", "5 effects", "5 menu mods", "5 new stages".
std::string things(int c, int n) {
  return std::to_string(n) + (c == kStages ? " stage skin" : c == kEffects ? " effect" : c == kMenus ? " menu mod"
                              : c == kNewStages ? " new stage" : " skin") +
         (n == 1 ? "" : "s");
}

}  // namespace menu_internal
}  // namespace host::mods_menu
