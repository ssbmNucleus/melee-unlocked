// Cosmetic stage skins: a stage file's target, the drawable and structure checks, import, the
// live stage cycle and republish, the online stage form.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

bool stage_target(const std::string& id, std::string* path, std::string* display) {
  static constexpr struct { const char* id; const char* path; const char* display; } stages[] = {
      {"battlefield", "GrNBa.dat", "Battlefield"},
      {"dreamland", "GrOp.dat", "Dream Land"},
      {"final_destination", "GrNLa.dat", "Final Destination"},
      {"fountain_of_dreams", "GrIz.dat", "Fountain of Dreams"},
      {"poke_floats", "GrPu.dat", "Poké Floats"},
      {"pokemon_stadium", "GrPs.dat", "Pokémon Stadium"},
      {"yoshis_story", "GrSt.dat", "Yoshi's Story"},
  };
  for (const auto& stage : stages) if (lower(id) == stage.id) {
    *path = stage.path; *display = stage.display; return true;
  }
  return false;
}

// A stage archive's metadata names the arena, but the DAT names the exact disc resource.
// Stadium's base and four transformations must stay separate, even when a ZIP contains all five.
// The transformation names follow the game's own table (grpstadium.c datfiles: fire, grass, water,
// rock). They were Water, Rock and Grass for 2 to 4, so a player who picked "Grass" for Grass's DAT
// put it on GrPs4, where its meshes match nothing and the whole form was drawn invisible.
static constexpr struct { const char* token; const char* path; const char* label; } kStageResources[] = {
      {"grps1", "GrPs1.dat", "Pokémon Stadium: Fire"},
      {"grps2", "GrPs2.dat", "Pokémon Stadium: Grass"},
      {"grps3", "GrPs3.dat", "Pokémon Stadium: Water"},
      {"grps4", "GrPs4.dat", "Pokémon Stadium: Rock"},
      {"grps", "GrPs.dat", "Pokémon Stadium: Base"},
      {"grop", "GrOp.dat", "Dream Land"},
      {"grnba", "GrNBa.dat", "Battlefield"},
      {"grnla", "GrNLa.dat", "Final Destination"},
      {"griz", "GrIz.dat", "Fountain of Dreams"},
      {"grst", "GrSt.dat", "Yoshi's Story"},
      {"grpu", "GrPu.dat", "Poké Floats"},
      {"grbb", "GrBb.dat", "Big Blue"},
      {"grcn", "GrCn.dat", "Corneria"},
      {"grcs", "GrCs.dat", "Princess Peach's Castle"},
      {"grfs", "GrFs.dat", "Fourside"},
      {"grfz", "GrFz.dat", "Flat Zone"},
      {"grgb", "GrGb.dat", "Great Bay"},
      {"grgd", "GrGd.dat", "Jungle Japes"},
      {"grgr", "GrGr.dat", "Green Greens"},
      {"gri1", "GrI1.dat", "Mushroom Kingdom"},
      {"gri2", "GrI2.dat", "Mushroom Kingdom II"},
      {"grim", "GrIm.dat", "Icicle Mountain"},
      {"grkg", "GrKg.dat", "Kongo Jungle"},
      {"grkr", "GrKr.dat", "Brinstar Depths"},
      {"grmc", "GrMc.dat", "Mute City"},
      {"grok", "GrOk.dat", "Kongo Jungle (64)"},
      {"grot", "GrOt.dat", "Onett"},
      {"groy", "GrOy.dat", "Yoshi's Island (64)"},
      {"grrc", "GrRc.dat", "Rainbow Cruise"},
      {"grsh", "GrSh.dat", "Temple"},
      {"grve", "GrVe.dat", "Venom"},
      {"gryt", "GrYt.dat", "Yoshi's Island"},
      {"grze", "GrZe.dat", "Brinstar"},
  };
bool stage_dat_target(const std::string& filename, std::string* path, std::string* display) {
  const std::string name = lower(filename.substr(filename.find_last_of("/\\") == std::string::npos ?
                                              0 : filename.find_last_of("/\\") + 1));
  if (name.size() < 7 || !dat_extension(name)) return false;
  const std::string stem = name.substr(0, name.size() - 4);
  size_t match_index = std::size(kStageResources);
  for (size_t i = 0; i < std::size(kStageResources); ++i) {
    const auto& resource = kStageResources[i];
    const std::string token = resource.token;
    size_t pos = stem.find(token);
    if (pos == std::string::npos) continue;
    const size_t end = pos + token.size();
    if ((pos && std::isalnum((unsigned char)stem[pos - 1])) ||
        (end < stem.size() && std::isalnum((unsigned char)stem[end]))) continue;
    if (match_index != std::size(kStageResources)) return false; // never guess an ambiguous disc file
    match_index = i;
  }
  if (match_index == std::size(kStageResources)) return false;
  *path = kStageResources[match_index].path; *display = kStageResources[match_index].label;
  return true;
}

// A Stadium transformation DAT whose own name says which form it is ("GrPs2.dat", "grps4"): that
// form's disc file, else empty. Only the four forms: each one's meshes fit its own file alone.
std::string stadium_form_file(const std::string& filename) {
  std::string name = lower(filename.substr(filename.find_last_of("/\\") == std::string::npos ?
                                           0 : filename.find_last_of("/\\") + 1));
  if (name.size() > 4 && dat_extension(name)) name.resize(name.size() - 4);
  if (name.size() != 5 || name.compare(0, 4, "grps") != 0 || name[4] < '1' || name[4] > '4') return {};
  return std::string("GrPs") + name[4] + ".dat";
}
bool stage_structure_safe(const std::vector<uint8_t>& bytes, std::string* defect) {
  data_safety::Archive archive(bytes);
  if (!archive.parse()) { *defect = "its archive tables do not hold together"; return false; }
  return data_safety::stage_structure_valid(archive, defect);
}

// A file name that holds a stage of the game: "GrNBa.dat", the English "GrPs.usd", or a pack's
// alternate for that stage under another extension ending in "at" ("GrNBa.0at" and "GrNBa.1at" are
// the 20XX style alternate stages). *target is the stage's file on the game disc and *label its
// name; *variant names the set as for a costume. The content is checked by the caller.
bool disc_stage_member(const std::string& raw, std::string* target, std::string* label, std::string* variant) {
  const std::string name = lower(raw);
  // A dynamic alternate stages disc keeps a folder per stage: "GrSt/GrSt-surfshop(L).dat". The text
  // after the dash names the alternate, and a button in brackets is the one that picks it there.
  const size_t dash = name.find('-'), last_dot = name.rfind('.');
  if (dash != std::string::npos && last_dot != std::string::npos && last_dot > dash + 1 && name.size() == last_dot + 4 &&
      (name.compare(last_dot, 4, ".dat") == 0 || name.compare(last_dot, 4, ".usd") == 0)) {
    for (const auto& resource : kStageResources) {
      if (name.compare(0, dash, resource.token) != 0) continue;
      *target = resource.path; *label = resource.label;
      *variant = raw.substr(dash + 1, last_dot - dash - 1);
      return true;
    }
    return false;
  }
  const size_t dot = name.find('.');
  if (dot == std::string::npos || name.size() != dot + 4) return false;
  for (const auto& resource : kStageResources) {
    if (name.compare(0, dot, resource.token) != 0) continue;
    *target = resource.path; *label = resource.label;
    return disc_skin_set(name.substr(dot + 1), variant);
  }
  return false;
}

// The button a dynamic alternate's name carries ("surfshop(L)"), as the game's pad bit, or 0. The
// stage code of such a disc looks for these six, in this order.
uint32_t stage_alternate_button(const std::string& variant) {
  static constexpr struct { const char* tag; uint32_t bit; } kButtons[] = {
      {"(l)", 0x40}, {"(r)", 0x20}, {"(z)", 0x10}, {"(b)", 0x200}, {"(x)", 0x400}, {"(y)", 0x800}};
  const std::string name = lower(variant);
  for (const auto& button : kButtons) if (name.find(button.tag) != std::string::npos) return button.bit;
  return 0;
}

}  // namespace cosmetic_internal

bool make_standalone_stage(const fs::path& source, const std::string& member,
                           std::vector<uint8_t> bytes, VaultResourcePlan* resource,
                           std::string* error, const std::string& target_override) {
  // A Stadium form's DAT named for its form goes to that form, whichever slot was picked.
  const std::string named_form = stadium_form_file(member.empty() ? path_filename_utf8(source) : member);
  const bool form_slot = !target_override.empty() && !stadium_form_file(target_override).empty();
  const std::string filename = form_slot && !named_form.empty() ? named_form :
                              !target_override.empty() ? target_override :
                              member.empty() ? path_filename_utf8(source) : member;
  if (!stage_dat_target(filename, &resource->target_path, &resource->slot)) {
    *error = "The stage DAT name does not identify a supported disc resource."; return false;
  }
  VisualLayout layout;
  if (!parse_visual_layout(bytes, &layout, error)) return false;
  if (!target_override.empty() && std::find(layout.roots.begin(), layout.roots.end(), "map_head") == layout.roots.end()) {
    *error = "This DAT has no stage map; choose a stage DAT rather than a costume or image file."; return false;
  }
  const std::string digest = sha256(bytes);
  if (digest.empty()) { *error = "The stage DAT could not be hashed."; return false; }
  resource->source_id = "standalone-stage:" + resource->target_path + ":" + digest;
  resource->kind = "stage_visual";
  resource->display_name = path_filename_utf8(source.stem());
  // Nucleus downloads use files.zip. Name its stage after the DAT so the
  // selection shows the imported variant instead of an indistinguishable "files".
  if (!member.empty() && (lower(resource->display_name) == "files" || lower(resource->display_name) == "download"))
    resource->display_name = path_filename_utf8(fs::u8path(member).stem());
  if (resource->display_name.empty() || lower(resource->display_name) == lower(resource->target_path))
    resource->display_name = resource->slot + " import";
  resource->group = "Stages";
  resource->source_member = member;
  resource->bytes = std::move(bytes);
  return true;
}

std::vector<StageSlot> stage_slots() {
  std::vector<StageSlot> result;
  for (const auto& resource : kStageResources) result.push_back({resource.path, resource.label});
  std::stable_sort(result.begin(), result.end(), [](const StageSlot& a, const StageSlot& b) { return a.name < b.name; });
  return result;
}

ImportResult import_stage_dat(const std::string& path_text, const std::string& target_path) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string readiness;
    if (!mutable_profile_locked(&readiness)) return {false, false, {}, readiness};
  }
  const fs::path path = fs::u8path(path_text);
  if (lower(path.extension().string()) != ".dat" && lower(path.extension().string()) != ".usd")   // (GrPs.usd: Stadium's English file)
    return {false, false, {}, "Choose a raw stage DAT file. ZIP archives use Import / Refresh."};
  const auto slots = stage_slots();
  if (std::none_of(slots.begin(), slots.end(), [&](const StageSlot& slot) { return slot.target_path == target_path; }))
    return {false, false, {}, "Choose a supported stage to replace."};
  VaultResourcePlan stage;
  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_bounded(path, kMaxAssetBytes, &bytes, &error) ||
      !make_standalone_stage(path, {}, std::move(bytes), &stage, &error, target_path))
    return {false, false, {}, error};
  VaultPlan plan;
  plan.resources.push_back(std::move(stage)); plan.stage_records = 1;
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = install_vault_locked(path, std::move(plan), "standalone_stage");
  // (a stage pool's skin is in its pool from the next match: plan_stage_skin)
  if (result.ok && std::atomic_load(&g_runtime)->initialized && !pool_stage_of(target_path))
    result.message += " Restart to apply the staged profile safely.";
  g_message = result.message;
  return result;
}

// ---- stage select skin cycling ----
// The profile's key for a stage file: the target path of the stage skins installed for it, as the
// catalog spells it. Empty when no stage skin is installed for that file. The game names a stage
// that has an English file without an extension ("/GrPs", "/GrOt") and adds it when it loads.
std::string stage_target_locked(const std::string& stage_file) {
  std::string wanted = lower(stage_file);
  const size_t slash = wanted.find_last_of('/');
  if (slash != std::string::npos) wanted = wanted.substr(slash + 1);
  if (wanted.find('.') == std::string::npos) wanted += ".dat";
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "stage_visual" || !asset.info.available) continue;
    std::string have = lower(asset.info.target_path);
    const size_t at = have.find_last_of('/');
    if (at != std::string::npos) have = have.substr(at + 1);
    if (have == wanted) return selection_key(asset);
  }
  return {};
}

LiveCycle cycle_stage_live(const std::string& stage_file, int direction) {
  LiveCycle result;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ready_locked(&result.message)) return result;
  if (kFreezeSkinPicksOnline && (g_online_freezes.load(std::memory_order_relaxed) || online_active())) {
    result.message = "Stage skins cannot change while an online session is up."; return result;
  }
  const std::string target = stage_target_locked(stage_file);
  if (target.empty()) { result.message = "No skin is installed for this stage."; return result; }
  // One of the six legal stages with skins: its pool picks at every match (cosmetic_stage_pools.cpp).
  if (pool_has_skins_locked(target) && !look_owns_locked(target)) {
    result.message = "This stage's skins are a pool: one is picked at random for each match."; return result;
  }
  // A stage whose alternates the session disc picks by the button held as it loads (L, R, Z, B, X, Y):
  // those buttons pick the alternate here, as on that disc, and do not step this stage's skin. A step
  // now would also load its pick before the match's own, and the game would keep that one.
  if (std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
        return session_stage_record(asset) && selection_key(asset) == target;
      })) {
    result.message = "This stage's alternates are picked by the button held as it loads."; return result;
  }
  std::vector<std::string> ids{std::string()}, names{"Standard"};
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "stage_visual" || !asset.info.available || selection_key(asset) != target) continue;
    ids.push_back(asset.info.id);
    names.push_back(asset.source_kind == kDiscSource ?
        asset.source_name + (asset.info.variant.empty() ? "" : " (" + asset.info.variant + ")") : asset.info.name);
  }
  size_t at = 0;
  const auto picked = g_profile.selections.find(target);
  if (g_profile.enabled && picked != g_profile.selections.end())
    for (size_t i = 1; i < ids.size(); ++i) if (ids[i] == picked->second) at = i;
  result.previous_id = ids[at]; result.asset_id = ids[at]; result.name = names[at];
  result.ok = true;
  const size_t count = ids.size();
  if (count < 2) { result.message = "No skin is installed for this stage."; return result; }
  const size_t next = direction > 0 ? (at + 1) % count : (at + count - 1) % count;
  const bool standard = ids[next].empty();
  if (!g_live_unpublished) {
    const auto runtime = std::atomic_load(&g_runtime);
    g_live_in_step = runtime->initialized && runtime->fingerprint == desired_fingerprint_locked();
  }
  Profile previous = g_profile;
  if (standard) g_profile.selections[target] = kVanillaSelection;
  else { g_profile.enabled = true; g_profile.selections[target] = ids[next]; }
  ++g_profile.generation;
  if (!save_profile_locked(&result.message)) { g_profile = std::move(previous); return result; }
  g_live_unpublished = true;
  g_message = names[next] + " selected for " + target + ".";
  result.changed = true; result.asset_id = ids[next]; result.name = names[next];
  return result;
}

RepublishResult republish_stage(uint8_t* fst, uint32_t fst_size, const std::string& stage_file) {
  RepublishResult result;
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string target = stage_target_locked(stage_file);
  if (!fst || target.empty()) { result.message = "No skin is installed for this stage."; return result; }
  publish_locked(fst, fst_size, &target, &result);
  if (!result.ok) host::log("cosmetics: %s was not published again (%s)", target.c_str(), result.message.c_str());
  return result;
}

bool stage_gameplay_matches(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& candidate,
                            std::string* detail) {
  std::string local;
  if (!detail) detail = &local;
  if (visual_dat_only(clean, candidate, detail)) { *detail = "textures only"; return true; }
  return stage_safety::matches(clean, candidate, detail);
}

bool stage_online_form(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& skin,
                       std::vector<uint8_t>* online, std::string* detail, const std::string& stage_file) {
  std::string local;
  if (!detail) detail = &local;
  if (stage_gameplay_matches(clean, skin, detail)) { if (online) online->clear(); return true; }
  const std::string as_is = *detail;
  std::string note;
  // The skin's gameplay values put back to the original's (stage_safety::restore_gameplay): a skin
  // that only changed values (20XX's stage files set a music table the original leaves empty) is then
  // the skin itself with the original's gameplay, and one that also changed models is grafted from
  // that repaired copy below.
  std::vector<uint8_t> restored;
  // A repaired copy that cannot be drawn as it is (its models changed too) is still the base of the
  // graft: grafting the raw skin would bring its changed values back and be refused (Dream Land
  // skins that set the music table and rebuild the stage were all served as the original online).
  if (stage_safety::restore_gameplay(clean, skin, &restored, &note)) {
    if (stage_draw_safe(restored, &note)) {
      *detail = "the skin with the original stage's gameplay values";
      if (online) *online = std::move(restored);
      return true;
    }
  }
  // A copy restore_gameplay could only partly repair is still tried first, as 0.9.13 did.
  // Grafted from the repaired copy first, then from the raw skin: some skins only pass one way.
  std::vector<uint8_t> grafted;
  bool drawn = false;
  const std::vector<uint8_t>* bases[] = {&restored, &skin};
  for (const std::vector<uint8_t>* base : bases) {
    if (base->empty()) continue;
    std::string try_note;
    std::vector<uint8_t> out;
    if (!stage_safety::graft_visuals(clean, *base, &out, &try_note, stage_safety::visual_only_models(stage_file))) {
      if (!drawn) note = try_note;
      continue;
    }
    if (!stage_draw_safe(out, &try_note)) { note = "drawn over the original: " + try_note; continue; }
    grafted = std::move(out); note = try_note; drawn = true;
    break;
  }
  if (!drawn) { *detail = as_is + "; " + note; return false; }
  *detail = "drawn over the original stage's gameplay (" + note + ")";
  if (online) *online = std::move(grafted);
  return true;
}

}  // namespace host::cosmetics
