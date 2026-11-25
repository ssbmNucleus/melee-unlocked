// Stage pools: the six stages' random stage skins, see cosmetic_mods.h (shared pieces: cosmetic_mods_internal.h).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The six legal stages each have a pool: the standard stage (unless switched off) and every stage skin
// the player installed for it. plan_stage_skin hands a stage whose pool has skins to plan_pool_locked,
// which picks one member at random for that match with the host's own random source and publishes it
// (one log line per pick). The checks are the random pick's, which keep the game from stopping: the skin's
// tables read, and its collision keeps what the stage's own code finds by number. Online a member is
// served as installed, as offline (under kStrictOnlineStages only a skin with an online form is picked
// there, and read() serves that form). A pool with no skins never gets here: its stage is served as
// before, byte for byte. Pokemon Stadium: a skin with a recorded set of four transformations
// (Profile::stadium_sets) brings them, published with it to GrPs1-4; otherwise those are the standard ones.
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

const PoolStage* pool_stage_of(const std::string& key) {
  for (const auto& stage : kPoolStages) if (lower(key) == lower(stage.target)) return &stage;
  return nullptr;
}
int stadium_form_index(const std::string& key) {
  for (int i = 0; i < 4; ++i) if (lower(key) == lower(kStadiumForms[i])) return i;
  return -1;
}
// A skin in a pool: an available stage skin of one of the six that the player installed (a mod disc's
// stage skins keep their own rules: its pack's random switch, the session's alternates).
bool pool_member(const AssetRecord& asset) {
  return asset.info.available && asset.info.kind == "stage_visual" && asset.source_kind != kDiscSource &&
         pool_stage_of(selection_key(asset)) != nullptr;
}
bool pool_has_skins_locked(const std::string& target) {
  if (!pool_stage_of(target)) return false;
  return std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
    return pool_member(asset) && lower(selection_key(asset)) == lower(target);
  });
}
bool any_pool_locked() {
  return std::any_of(std::begin(kPoolStages), std::end(kPoolStages),
                     [](const PoolStage& stage) { return pool_has_skins_locked(stage.target); });
}
// A saved choice the pools take the place of: a pool stage that has skins, and Stadium's transformations
// while its pool has skins. A pool with no skins leaves everything as it was.
bool pool_owned_key_locked(const std::string& key) {
  if (pool_stage_of(key)) return pool_has_skins_locked(key);
  return stadium_form_index(key) >= 0 && pool_has_skins_locked("GrPs.dat");
}
// The exact look in force serves this stage file itself (publish_locked): no pool then.
bool look_owns_locked(const std::string& target) {
  return !g_look_pack.empty() && std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
    return asset.source_kind == kDiscSource && asset.source_name == g_look_pack && asset.info.available &&
           asset.info.variant.empty() && lower(selection_key(asset)) == lower(target);
  });
}

}  // namespace cosmetic_internal

bool random_stage_skins() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_profile.random_stage_skins;
}

bool set_random_stage_skins(bool enabled, std::string* error) {
  std::string local;
  if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  if (g_profile.random_stage_skins == enabled) return true;
  const bool previous = g_profile.random_stage_skins;
  g_profile.random_stage_skins = enabled;
  if (!save_profile_locked(error)) { g_profile.random_stage_skins = previous; return false; }
  g_message = enabled ? "Random installed stage skins enabled for the next match." :
                        "Fixed stage skins restored at the next match.";
  return true;
}

std::vector<StagePackInfo> stage_packs() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<StagePackInfo> out;
  std::map<std::string, size_t> index;
  std::map<std::string, std::map<std::string, bool>> stages;   // pack key -> stages it has a skin for
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "stage_visual" || asset.source_kind != kDiscSource) continue;
    auto at = index.find(asset.source_id);
    if (at == index.end()) {
      StagePackInfo pack;
      pack.key = asset.source_id; pack.name = asset.source_name;
      pack.random = !g_profile.stage_packs_off.count(pack.key);
      out.push_back(std::move(pack));
      at = index.emplace(asset.source_id, out.size() - 1).first;
    }
    ++out[at->second].skins;
    stages[asset.source_id][asset.info.target_path] = true;
  }
  for (auto& pack : out) pack.stages = (uint32_t)stages[pack.key].size();
  std::stable_sort(out.begin(), out.end(), [](const StagePackInfo& a, const StagePackInfo& b) {
    return lower(a.name) < lower(b.name);
  });
  return out;
}

bool set_stage_pack_random(const std::string& pack_key, bool enabled, std::string* error) {
  std::string local;
  if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  if (enabled == !g_profile.stage_packs_off.count(pack_key)) return true;
  const auto previous = g_profile.stage_packs_off;
  if (enabled) g_profile.stage_packs_off.erase(pack_key);
  else g_profile.stage_packs_off[pack_key] = true;
  if (!save_profile_locked(error)) { g_profile.stage_packs_off = previous; return false; }
  g_message = enabled ? "That pack's stage skins are in the random pick from the next match." :
                        "That pack's stage skins stay out of the random pick from the next match.";
  return true;
}


// The four transformations served with a Stadium skin: its recorded set when all four are installed skins
// of their own form's file, else none (the skin then comes with the standard transformations).
static std::vector<const AssetRecord*> stadium_set_locked(const AssetRecord& base) {
  std::vector<const AssetRecord*> out;
  const auto set = g_profile.stadium_sets.find(base.info.id);
  if (set == g_profile.stadium_sets.end() || set->second.size() != 4) return out;
  for (size_t i = 0; i < 4; ++i) {
    const auto form = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
      return asset.info.id == set->second[i] && asset.info.available && asset.info.kind == "stage_visual" &&
             lower(selection_key(asset)) == lower(kStadiumForms[i]);
    });
    if (form == g_assets.end()) return {};
    out.push_back(&*form);
  }
  return out;
}

// The disc's own file(s) of a stage slot ("GrPs.dat" and its English "GrPs.usd"), at the disc's lengths.
static bool pool_originals_locked(const std::vector<FstFile>& files, const RuntimeState& current, const std::string& slot,
                                  std::vector<std::vector<uint8_t>>* out) {
  const std::string wanted = lower(slot);
  if (wanted.size() < 5) return false;
  const std::string english = wanted.substr(0, wanted.size() - 4) + ".usd";
  for (const auto& file : files) {
    const std::string basename = lower(fs::path(file.path).filename().string());
    if (basename != wanted && basename != english) continue;
    const auto applied = current.by_start.find(file.start);
    const uint32_t size = applied == current.by_start.end() ? file.size : applied->second.vanilla_size;
    if (size > kMaxAssetBytes) return false;
    std::vector<uint8_t> bytes(size);
    if (!host::disc_read(file.start, bytes.data(), size)) return false;
    out->push_back(std::move(bytes));
  }
  return !out->empty() && out->size() <= 2;
}

// Why a pool's skin cannot be served for this match, "" when it can. `longest` grows to the longest
// form it would be served in (the skin, or its online form).
static std::string pool_refusal_locked(const AssetRecord& asset, const std::vector<std::vector<uint8_t>>& originals,
                                       const std::string& slot, bool online, size_t* longest) {
  std::string error;
  const std::vector<uint8_t> bytes = load_runtime_asset_locked(asset, &error);
  if (bytes.empty()) return error.empty() ? "its file cannot be read" : error;
  // Only judged against an original this reader knows (every retail stage file is one).
  bool original_reads = true;
  for (const auto& original : originals) {
    stage_safety::Archive archive{original};
    if (!archive.parse()) original_reads = false;
  }
  if (original_reads) {
    stage_safety::Archive readable{bytes};
    if (!readable.parse()) return "its stage tables are malformed";
    // Fewer collision lines or groups than the original: the stage's own code finds them by number and
    // stops the game when one is not there (publish_locked refuses it the same way).
    stage_safety::Archive o{originals.front()}, k{bytes};
    if (o.parse() && k.parse() && o.roots.count("coll_data") && k.roots.count("coll_data")) {
      const uint32_t co = o.roots.at("coll_data"), ck = k.roots.at("coll_data");
      if (o.bounds(co, 0x30) && k.bounds(ck, 0x30) &&
          (k.get(ck + 0x0C) < o.get(co + 0x0C) || k.get(ck + 0x28) < o.get(co + 0x28)))
        return "its collision is missing parts the stage needs";
    }
  }
  size_t served = bytes.size();
  // Online a member is served as installed, as offline (the desktop app's way). Under
  // kStrictOnlineStages it needs a form whose gameplay matches the original, and every member is
  // served at the length of the longest form.
  for (const auto& original : originals) {
    if (!kStrictOnlineStages) break;
    std::vector<uint8_t> form;
    if (!stage_online_form(original, bytes, &form, &error, slot)) {
      if (online) return "no online form (" + online_reason_short(error) + ")";
      continue;
    }
    served = std::max(served, form.size());
  }
  *longest = std::max(*longest, served);
  return {};
}

// A Stadium transformation file is served by a skin now (in the running snapshot).
static bool form_served_locked(const std::string& form) {
  const auto runtime = std::atomic_load(&g_runtime);
  return std::any_of(runtime->by_start.begin(), runtime->by_start.end(), [&](const auto& entry) {
    return lower(entry.second.target_path) == lower(form);
  });
}

static RepublishResult plan_pool_locked(uint8_t* fst, uint32_t fst_size, const std::string& target, uint64_t match_token,
                                        const RuntimeState& current) {
  RepublishResult result;
  const auto started = std::chrono::steady_clock::now();
  const PoolStage* stage = pool_stage_of(target);
  std::vector<FstFile> files;
  if (!stage || !parse_fst(fst, fst_size, &files, &result.message)) return result;
  std::vector<std::vector<uint8_t>> originals;
  if (!pool_originals_locked(files, current, target, &originals)) {
    host::log("cosmetics: stage pool %s: this disc's own file cannot be read; the standard stage stays", stage->name);
    return result;
  }
  const bool online = online_active();
  const bool stadium = lower(target) == "grps.dat";
  std::vector<std::vector<std::vector<uint8_t>>> form_originals(stadium ? 4 : 0);
  bool forms_read = stadium;
  for (size_t i = 0; i < form_originals.size(); ++i)
    if (!pool_originals_locked(files, current, kStadiumForms[i], &form_originals[i])) forms_read = false;

  struct Candidate { const AssetRecord* asset; std::vector<const AssetRecord*> forms; };
  std::vector<Candidate> eligible;
  size_t extent = 0, skins = 0;
  for (const auto& original : originals) extent = std::max(extent, original.size());
  size_t form_extent[4] = {0, 0, 0, 0};
  static std::set<std::string> told;   // each refusal is logged once
  for (const auto& asset : g_assets) {
    if (!pool_member(asset) || lower(selection_key(asset)) != lower(target)) continue;
    ++skins;
    size_t longest = 0, form_longest[4] = {0, 0, 0, 0};
    std::string why = pool_refusal_locked(asset, originals, target, online, &longest);
    std::vector<const AssetRecord*> forms = why.empty() && stadium ? stadium_set_locked(asset) : std::vector<const AssetRecord*>{};
    for (size_t i = 0; i < forms.size() && why.empty(); ++i) {
      const std::string form_why = forms_read ? pool_refusal_locked(*forms[i], form_originals[i], kStadiumForms[i], online,
                                                                    &form_longest[i])
                                              : "this disc's transformations cannot be read";
      if (!form_why.empty()) why = std::string("its ") + kStadiumForms[i] + ": " + form_why;
    }
    if (!why.empty()) {
      if (told.insert(asset.info.id + (online ? "/online/" : "/") + why).second)
        host::log("cosmetics: stage pool %s: %s stays out of the pick%s: %s", stage->name, asset.info.name.c_str(),
                  online ? " online" : "", why.c_str());
      continue;
    }
    extent = std::max(extent, longest);
    for (size_t i = 0; i < 4; ++i) form_extent[i] = std::max(form_extent[i], form_longest[i]);
    eligible.push_back({&asset, std::move(forms)});
  }
  const bool vanilla_on = !g_profile.pool_vanilla_off.count(stage->target);
  const uint32_t count = (uint32_t)eligible.size() + (vanilla_on ? 1u : 0u);
  int picked = -1;   // an eligible skin, or -1 the standard stage
  if (count > 1) {
    // Independent host entropy, unbiased; never the game's RNG or its seed.
    uint32_t random = 0;
    const uint32_t threshold = (0u - count) % count;
    do {
      if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        result.message = "Stage skin random generator unavailable; the standard stage stays.";
        host::log("cosmetics: stage pool %s: %s", stage->name, result.message.c_str());
        return result;
      }
    } while (random < threshold);
    const uint32_t at = random % count;
    picked = at < eligible.size() ? (int)at : -1;
  } else if (count == 1 && !eligible.empty()) {
    picked = 0;
  }
  const std::string chosen = picked >= 0 ? eligible[(size_t)picked].asset->info.id : std::string();
  if (extent) g_stage_extent[target] = std::max(g_stage_extent[target], extent);
  for (size_t i = 0; i < 4; ++i)
    if (form_extent[i]) g_stage_extent[kStadiumForms[i]] = std::max(g_stage_extent[kStadiumForms[i]], form_extent[i]);
  publish_locked(fst, fst_size, &target, &result, &chosen);
  if (!result.ok) {
    host::log("cosmetics: stage pool %s: the pick was not published (%s); the stage stays as it was", stage->name,
              result.message.c_str());
    return result;
  }
  size_t forms_served = 0;
  if (stadium) {
    // Its transformations with it, else the standard ones (published only when a skin's were served).
    const auto* set = picked >= 0 ? &eligible[(size_t)picked].forms : nullptr;
    for (size_t i = 0; i < 4; ++i) {
      const std::string form = kStadiumForms[i];
      const std::string form_pick = set && set->size() == 4 ? (*set)[i]->info.id : std::string();
      if (form_pick.empty() && !form_served_locked(form)) continue;
      RepublishResult form_result;
      publish_locked(fst, fst_size, &form, &form_result, &form_pick);
      if (!form_result.ok) {
        host::log("cosmetics: stage pool %s: %s was not published (%s)", stage->name, form.c_str(), form_result.message.c_str());
        continue;
      }
      forms_served += form_pick.empty() ? 0 : 1;
      result.files.insert(result.files.end(), form_result.files.begin(), form_result.files.end());
    }
  }
  auto next = std::make_shared<RuntimeState>(*std::atomic_load(&g_runtime));
  next->stage_matches[target] = {match_token, chosen};
  std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
  host::log("cosmetics: match %llu stage pool %s: %s%s (pool: %s%zu of %zu skins%s, %lld ms)",
            (unsigned long long)match_token, stage->name,
            picked >= 0 ? eligible[(size_t)picked].asset->info.name.c_str() : "Vanilla",
            forms_served == 4 ? " with its transformations" : "", vanilla_on ? "Vanilla and " : "",
            eligible.size(), skins, !online ? "" : kStrictOnlineStages ? ", online forms" : ", online as installed",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
  return result;
}

// A pool whose last skin was removed while that skin was served: the standard stage again from this match
// (with no skin of the stage left, plan_stage_skin has no target and would leave the removed one served).
static RepublishResult plan_emptied_pool_locked(uint8_t* fst, uint32_t fst_size, const std::string& stage_file,
                                                uint64_t match_token, const RuntimeState& current) {
  RepublishResult result;
  std::string name = lower(stage_file);
  if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
  if (name.find('.') == std::string::npos) name += ".dat";
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".usd") == 0) name = name.substr(0, name.size() - 4) + ".dat";
  const PoolStage* stage = pool_stage_of(name);
  if (!stage || pool_has_skins_locked(stage->target)) return result;
  const std::string target = stage->target;
  const auto served = current.stage_matches.find(target);
  if (served == current.stage_matches.end() || served->second.second.empty()) return result;
  const std::string none;
  publish_locked(fst, fst_size, &target, &result, &none);
  if (!result.ok) {
    host::log("cosmetics: stage pool %s: the standard stage was not published (%s)", stage->name, result.message.c_str());
    return result;
  }
  if (lower(target) == "grps.dat")
    for (const char* form_name : kStadiumForms) {
      const std::string form = form_name;
      if (!form_served_locked(form)) continue;
      RepublishResult form_result;
      publish_locked(fst, fst_size, &form, &form_result, &none);
      if (form_result.ok) result.files.insert(result.files.end(), form_result.files.begin(), form_result.files.end());
    }
  auto next = std::make_shared<RuntimeState>(*std::atomic_load(&g_runtime));
  next->stage_matches[target] = {match_token, std::string()};
  std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
  host::log("cosmetics: match %llu stage pool %s: Vanilla (its last skin was removed)", (unsigned long long)match_token,
            stage->name);
  return result;
}

std::vector<StagePool> stage_pools() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<StagePool> out;
  for (const auto& stage : kPoolStages) {
    StagePool pool;
    pool.target_path = stage.target;
    pool.name = stage.name;
    pool.vanilla_on = !g_profile.pool_vanilla_off.count(stage.target);
    for (const auto& asset : g_assets) {
      if (!pool_member(asset) || lower(selection_key(asset)) != lower(stage.target)) continue;
      StagePoolMember member;
      member.asset_id = asset.info.id;
      member.name = asset.info.name;
      for (const AssetRecord* form : stadium_set_locked(asset)) member.forms.push_back(form->info.id);
      pool.skins.push_back(std::move(member));
    }
    out.push_back(std::move(pool));
  }
  return out;
}

bool is_pool_stage(const std::string& stage_file) {
  std::string name = stage_file.substr(stage_file.find_last_of("/\\") == std::string::npos ? 0 : stage_file.find_last_of("/\\") + 1);
  if (name.find('.') == std::string::npos) name += ".dat";
  return pool_stage_of(name) != nullptr;
}

bool set_pool_vanilla(const std::string& target_path, bool on, std::string* error) {
  std::string local;
  if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  const PoolStage* stage = pool_stage_of(target_path);
  if (!stage) { *error = "Not one of the six stages."; return false; }
  if (!mutable_profile_locked(error)) return false;
  if (on == !g_profile.pool_vanilla_off.count(stage->target)) return true;
  const auto previous = g_profile.pool_vanilla_off;
  if (on) g_profile.pool_vanilla_off.erase(stage->target);
  else g_profile.pool_vanilla_off[stage->target] = true;
  if (!save_profile_locked(error)) { g_profile.pool_vanilla_off = previous; return false; }
  g_message = std::string(stage->name) + (on ? ": the standard stage is in the pool again." : ": the standard stage is out of the pool.");
  host::log("cosmetics: stage pool %s: Vanilla %s", stage->name, on ? "on" : "off");
  return true;
}

bool set_stadium_set(const std::string& base_id, const std::vector<std::string>& form_ids, std::string* error) {
  std::string local;
  if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  const auto is_skin = [&](const std::string& id, const std::string& slot) {
    return std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
      return asset.info.id == id && asset.info.kind == "stage_visual" && lower(selection_key(asset)) == lower(slot);
    });
  };
  if (!is_skin(base_id, "GrPs.dat")) { *error = "Not a Pokemon Stadium skin."; return false; }
  if (form_ids.size() != 4) { *error = "A Stadium skin brings all four transformations or none."; return false; }
  for (size_t i = 0; i < 4; ++i)
    if (!is_skin(form_ids[i], kStadiumForms[i])) {
      *error = std::string("Not a skin of ") + kStadiumForms[i] + ".";
      return false;
    }
  const Profile previous = g_profile;
  g_profile.stadium_sets[base_id] = form_ids;
  // The set's transformations come only with their Stadium: none stays a saved choice of its own (an import
  // of a transformation on its own is one), served when the pool has no skin.
  for (size_t i = 0; i < 4; ++i) {
    const auto picked = g_profile.selections.find(kStadiumForms[i]);
    if (picked != g_profile.selections.end() && picked->second == form_ids[i]) {
      picked->second = kVanillaSelection; ++g_profile.generation;
    }
  }
  if (!save_profile_locked(error)) { g_profile = previous; return false; }
  host::log("cosmetics: stage pool Pokemon Stadium: %s comes with its four transformations", base_id.c_str());
  return true;
}

RepublishResult plan_stage_skin(uint8_t* fst, uint32_t fst_size, const std::string& stage_file,
                               uint64_t match_token) {
  RepublishResult result;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto current = std::atomic_load(&g_runtime);
  const std::string target = stage_target_locked(stage_file);
  // One of the six whose last skin was removed while it was served (cosmetic_stage_pools.cpp).
  if (target.empty() && current->initialized && fst && g_profile.enabled && !(g_lockstep && g_lockstep()))
    return plan_emptied_pool_locked(fst, fst_size, stage_file, match_token, *current);
  if (!current->initialized || !fst || target.empty() || !g_profile.enabled) return result;
  const auto previous = current->stage_matches.find(target);
  if (previous != current->stage_matches.end() && previous->second.first == match_token) return result;
  // The session disc's own alternates of this stage: picked for every match, as the disc's stage code
  // does (a held button names one, otherwise any of them), whatever the random setting says.
  const bool session = std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
    return session_stage_record(asset) && selection_key(asset) == target;
  });
  // In a lockstep session (Friends) the two games must load the same stage file, and each game's own
  // pick (a saved stage skin, a random draw, the disc's alternates by held button) can differ: every
  // stage is the game's own there, published even for a first match, as a saved skin may be applied.
  const bool lockstep = g_lockstep && g_lockstep();
  // One of the six legal stages with skins: its pool (cosmetic_stage_pools.cpp).
  if (!lockstep && !session && pool_has_skins_locked(target) && !look_owns_locked(target))
    return plan_pool_locked(fst, fst_size, target, match_token, *current);
  const bool pick_any = !lockstep && (g_profile.random_stage_skins || session);
  if (!lockstep && !pick_any && previous == current->stage_matches.end()) return result;

  std::vector<std::string> eligible;
  std::vector<uint32_t> eligible_buttons;
  size_t extent = 0;
  const auto started = std::chrono::steady_clock::now();
  if (pick_any) {
    std::vector<FstFile> files;
    if (!parse_fst(fst, fst_size, &files, &result.message)) return result;
    const std::string wanted = lower(target);
    const std::string english = wanted.substr(0, wanted.size() - 4) + ".usd";
    std::vector<std::vector<uint8_t>> originals;
    for (const auto& file : files) {
      const std::string basename = lower(fs::path(file.path).filename().string());
      if (basename != wanted && basename != english) continue;
      const auto applied = current->by_start.find(file.start);
      const uint32_t size = applied == current->by_start.end() ? file.size : applied->second.vanilla_size;
      if (size > kMaxAssetBytes) return result;
      std::vector<uint8_t> bytes(size);
      if (!host::disc_read(file.start, bytes.data(), size)) return result;
      originals.push_back(std::move(bytes));
    }
    if (originals.empty() || originals.size() > 2) return result;
    for (const auto& original : originals) extent = std::max(extent, original.size());
    bool original_reads = true;   // every retail stage file does (all 76 of melee 1.02)
    for (const auto& original : originals) {
      stage_safety::Archive archive{original};
      if (!archive.parse()) original_reads = false;
    }
    for (const auto& asset : g_assets) {
      if (!asset.info.available || asset.info.kind != "stage_visual" || selection_key(asset) != target) continue;
      if (session ? !session_stage_record(asset)
                  : asset.source_kind == kDiscSource && g_profile.stage_packs_off.count(asset.source_id) != 0) continue;
      std::string error;
      const auto bytes = load_runtime_asset_locked(asset, &error);
      if (bytes.empty()) continue;
      // A file whose stage tables do not even read (relocations past its data, pointers into
      // nothing) is never a random pick, offline either: one such Yoshi's Story file on a player's
      // disc stopped the game at its Shy Guys and at the stage's load (0.9.97 reports). Online it
      // was already refused below. Still served when the player picks it themselves.
      // Only when the original reads: a file this reader does not know at all is not judged.
      if (original_reads) {
        stage_safety::Archive readable{bytes};
        if (!readable.parse()) {
          static std::set<std::string> told;
          if (told.insert(asset.info.id).second)
            host::log("cosmetics: %s stays out of the random pick for %s: its stage tables are malformed",
                      asset.info.id.c_str(), target.c_str());
          continue;
        }
      }
      bool allowed = true;
      size_t longest = bytes.size();
      // Under kStrictOnlineStages a skin needs a form whose gameplay matches the original to be a
      // pick online, and every pick is served at the length of the longest form. Otherwise each skin
      // is served as installed, online too.
      for (const auto& original : originals) {
        if (!kStrictOnlineStages) break;
        std::vector<uint8_t> form;
        if (!stage_online_form(original, bytes, &form, &error, target)) { if (online_active()) { allowed = false; break; } continue; }
        longest = std::max(longest, form.size());
      }
      if (allowed) {
        eligible.push_back(asset.info.id);
        eligible_buttons.push_back(session ? stage_alternate_button(asset.info.variant) : 0);
        extent = std::max(extent, longest);
      }
    }
  }
  std::string chosen;
  bool by_button = false;
  if (session && g_stage_buttons && !eligible.empty()) {
    // The disc's rule: L, R, Z, B, X, Y in that order, the first one held that an alternate is named for.
    const uint32_t held = g_stage_buttons();
    for (const uint32_t bit : {0x40u, 0x20u, 0x10u, 0x200u, 0x400u, 0x800u}) {
      if (!(held & bit)) continue;
      for (size_t i = 0; i < eligible.size() && !by_button; ++i)
        if (eligible_buttons[i] == bit) { chosen = eligible[i]; by_button = true; }
      if (by_button) break;
    }
  }
  if (by_button) {
  } else if (lockstep) {
  } else if (!pick_any) {
    const auto fixed = g_profile.selections.find(target);
    if (fixed != g_profile.selections.end() && fixed->second != kVanillaSelection) chosen = fixed->second;
  } else if (!eligible.empty()) {
    // Independent host entropy; never calls the game's RNG or changes its seed.
    uint32_t random = 0;
    const uint32_t count = (uint32_t)eligible.size(), threshold = (0u - count) % count;
    do {
      if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof random,
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        result.message = "Stage skin random generator unavailable; fixed choice retained."; return result;
      }
    } while (random < threshold);
    chosen = eligible[random % count];
  }
  if (pick_any && extent) g_stage_extent[target] = std::max(g_stage_extent[target], extent);
  publish_locked(fst, fst_size, &target, &result, &chosen);
  if (!result.ok) return result;
  auto next = std::make_shared<RuntimeState>(*std::atomic_load(&g_runtime));
  next->stage_matches[target] = {match_token, chosen};
  std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
  host::log("cosmetics: match %llu %s skin for %s: %s (%zu eligible, %lld ms)",
            (unsigned long long)match_token, by_button ? "held button" : pick_any ? "random" : "fixed",
            target.c_str(), chosen.empty() ? "Standard" : chosen.c_str(), eligible.size(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
  return result;
}

}  // namespace host::cosmetics
