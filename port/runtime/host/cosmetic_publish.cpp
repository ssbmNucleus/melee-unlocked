// Cosmetic publishing: the selection written over the game's FST (publish_locked), republish
// of one slot or effect, which asset a disc file serves.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"
#include "release_scope.h"   // no installed effects while this release has no Particles

namespace host::cosmetics {

// True when the running snapshot matched the saved profile before the live picks not published yet
// changed it, so publishing them leaves nothing waiting for a restart.
bool g_live_in_step = false, g_live_unpublished = false;

// An alternate stage of the disc whose alternates are this session's stages (set_session_stage_pack):
// one of its named sets ("surfshop(L)"). The disc's plain stage files are not alternates: a mod
// session already serves them, and a look serves them as its own files.
bool session_stage_record(const AssetRecord& asset) {
  return !g_session_stage_pack.empty() && asset.source_kind == kDiscSource && asset.source_id == g_session_stage_pack &&
         asset.info.kind == "stage_visual" && asset.info.available && !asset.info.variant.empty();
}

// The length every form of a stage is served at while random stage skins are on (g_stage_extent):
// the longest of the disc's own file(s) and every installed skin of that stage, offline and in its
// online form. `files` carries the disc's own lengths. 0 when the stage is not on this disc.
static size_t stage_extent_locked(const std::string& target, const std::vector<FstFile>& files) {
  const std::string wanted = lower(target);
  if (wanted.size() < 5) return 0;
  const std::string english = wanted.substr(0, wanted.size() - 4) + ".usd";
  std::vector<std::vector<uint8_t>> originals;
  for (const auto& file : files) {
    const std::string base = lower(fs::path(file.path).filename().string());
    if (base != wanted && base != english) continue;
    if (file.size > kMaxAssetBytes) return 0;
    std::vector<uint8_t> bytes(file.size);
    if (!host::disc_read(file.start, bytes.data(), file.size)) return 0;
    originals.push_back(std::move(bytes));
  }
  if (originals.empty() || originals.size() > 2) return 0;
  size_t extent = 0;
  for (const auto& original : originals) extent = std::max(extent, original.size());
  for (const auto& asset : g_assets) {
    if (!asset.info.available || asset.info.kind != "stage_visual" || selection_key(asset) != target) continue;
    if (asset.source_kind == kDiscSource && g_profile.stage_packs_off.count(asset.source_id)) continue;
    std::string error;
    const auto bytes = load_runtime_asset_locked(asset, &error);
    if (bytes.empty()) continue;
    extent = std::max(extent, bytes.size());
    for (const auto& original : originals) {
      std::vector<uint8_t> form;
      if (stage_online_form(original, bytes, &form, &error, target)) extent = std::max(extent, form.size());
    }
  }
  return extent;
}

// The whole profile at startup (only_slot null), or one costume slot again while the game runs
// (republish_slot): the snapshot is then a copy of the running one with that slot's files replaced.
void publish_locked(uint8_t* fst, uint32_t fst_size, const std::string* only_slot, RepublishResult* republished,
                           const std::string* stage_pick) {
  const auto current = std::atomic_load(&g_runtime);
  auto next = only_slot ? std::make_shared<RuntimeState>(*current) : std::make_shared<RuntimeState>();
  next->initialized = true; next->generation = g_profile.generation;
  if (!only_slot || g_live_in_step) next->fingerprint = desired_fingerprint_locked();
  if (only_slot && (!g_configured || !g_catalog_valid || !g_profile_valid || !current->initialized)) {
    republished->message = "The cosmetic catalog is not running."; return;
  }
  if (!only_slot && (!g_configured || !g_catalog_valid || !g_profile_valid || !g_profile.enabled)) {
    std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
    host::log("cosmetics: vanilla profile (%s)", !g_profile.enabled ? "disabled" : "catalog unavailable");
    return;
  }
  std::vector<FstFile> files; std::string error;
  if (!parse_fst(fst, fst_size, &files, &error)) {
    g_message = error; host::log("cosmetics: %s", error.c_str());
    if (only_slot) { republished->message = error; return; }
    std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next)); return;
  }
  std::vector<FstFile> slot_files;   // the slot's disc files, with the disc's own lengths
  if (only_slot) {
    // The table carries the lengths published so far: every overridden file gets the disc's own
    // length back in the list, which is what the checks below compare against.
    for (auto& file : files) {
      const auto served = current->by_start.find(file.start);
      if (served != current->by_start.end()) file.size = served->second.vanilla_size;
    }
    const std::string wanted = lower(*only_slot);
    const std::string english = wanted.size() > 4 && wanted.compare(wanted.size() - 4, 4, ".dat") == 0 ?
        wanted.substr(0, wanted.size() - 4) + ".usd" : std::string();
    for (const auto& file : files) {
      const std::string path = lower(file.path);
      const size_t slash = path.find_last_of('/');
      const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
      if (base == wanted || (!english.empty() && base == english)) slot_files.push_back(file);
    }
    if (slot_files.empty()) { republished->message = "This disc has no " + *only_slot + "."; return; }
    // Back to the disc's own files first; the slot's pick, if it has one, is applied by the loop.
    bool was_served = false;
    for (const auto& file : slot_files) {
      was_served |= next->by_start.erase(file.start) != 0;
      put_be32(fst + (size_t)file.index * 12 + 8, file.size);
    }
    if (was_served && next->assets) --next->assets;
    next->companions.erase(std::remove_if(next->companions.begin(), next->companions.end(),
        [&](const CompanionOverride& have) { return lower(have.target_path) == wanted; }), next->companions.end());
  }
  // The whole set at once (startup, a changed profile) with random stage skins on: every stage with
  // installed skins gets its shared length now, before the game can read any of them ahead of a
  // match, and before the skins below are padded to it.
  if (!only_slot && g_profile.enabled && (g_profile.random_stage_skins || !g_session_stage_pack.empty())) {
    std::map<std::string, bool> stages;
    for (const auto& asset : g_assets)
      if (asset.info.available && asset.info.kind == "stage_visual" &&
          (g_profile.random_stage_skins ? !(asset.source_kind == kDiscSource && g_profile.stage_packs_off.count(asset.source_id))
                                        : session_stage_record(asset)))
        stages[selection_key(asset)] = true;
    for (const auto& stage : stages) {
      const size_t extent = stage_extent_locked(stage.first, files);
      if (extent) g_stage_extent[stage.first] = std::max(g_stage_extent[stage.first], extent);
    }
  }
  // The same for each stage pool that has skins (cosmetic_stage_pools.cpp), Stadium's transformations
  // with its base: every member and the standard stage are served at one length from the start. A pool
  // with no skins is left as it is.
  if (!only_slot && g_profile.enabled)
    for (const auto& stage : kPoolStages) {
      if (!pool_has_skins_locked(stage.target) || look_owns_locked(stage.target)) continue;
      std::vector<std::string> slots{stage.target};
      if (lower(stage.target) == "grps.dat") slots.insert(slots.end(), std::begin(kStadiumForms), std::end(kStadiumForms));
      for (const auto& slot : slots) {
        const size_t extent = stage_extent_locked(slot, files);
        if (extent) g_stage_extent[slot] = std::max(g_stage_extent[slot], extent);
      }
    }
  std::map<std::string, bool> effect_done;
  std::vector<std::string> runtime_errors;
  std::vector<const AssetRecord*> portraits;
  auto selections = g_profile.selections;
  // A stage pool with skins picks for every match (plan_stage_skin): a saved choice of that stage stays unused.
  for (auto it = selections.begin(); it != selections.end();)
    it = pool_owned_key_locked(it->first) ? selections.erase(it) : std::next(it);
  // An exact look selects every file of its pack (plain sets only), on top of the player's own picks for
  // those slots; the profile itself is untouched, so turning the look off gives the player's picks back.
  if (!g_look_pack.empty())
    for (const auto& asset : g_assets)
      if (asset.source_kind == kDiscSource && asset.source_name == g_look_pack && asset.info.available &&
          asset.info.variant.empty())
        selections[selection_key(asset)] = asset.info.id;
  if (stage_pick && only_slot) selections[*only_slot] = stage_pick->empty() ? kVanillaSelection : *stage_pick;
  for (const auto& pick : selections) {
    if (pick.second == kVanillaSelection) continue;
    // One slot: its skin and its own portrait entry, and nothing while the profile is switched off. An
    // effect file (republish_effect) takes every effect picked for it ("EfCaData.dat#effects").
    const bool effect_pick = only_slot && lower(pick.first) != lower(*only_slot + kPortraitSuffix) &&
                             lower(pick.first).rfind(lower(*only_slot) + "#", 0) == 0;
    if (only_slot && (!g_profile.enabled || (lower(pick.first) != lower(*only_slot) &&
                                              lower(pick.first) != lower(*only_slot + kPortraitSuffix) && !effect_pick))) continue;
    auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == pick.second && selection_key(item) == pick.first;
    });
    if (asset == g_assets.end()) { host::log("cosmetics: missing catalog asset %s", pick.second.c_str()); continue; }
    if (effect_pick && asset->info.kind != "effect_visual") continue;
    if (asset->info.kind == kPortraitKind) { portraits.push_back(&*asset); continue; }
    // Particles off in this release (release_scope.h): an installed effect stays installed, the disc's file plays.
    if (asset->info.kind == "effect_visual" &&
        !host::release_scope::gate(host::release_scope::kParticles, "installed effects are not applied in matches"))
      continue;
    if (asset->info.kind == "effect_visual" && effect_done[asset->info.target_path]) continue;
    if (asset->info.kind == "effect_visual") effect_done[asset->info.target_path] = true;
    std::vector<uint8_t> bytes = load_runtime_asset_locked(*asset, &error);
    if (bytes.empty()) { host::log("cosmetics: %s", error.c_str()); continue; }
    std::string wanted = lower(asset->info.target_path);
    // Stadium forms installed before their slot names were right: a DAT named for one form sitting
    // in another form's slot is served to the form it names, where its meshes match.
    if (asset->info.kind == "stage_visual" && !stadium_form_file(wanted).empty()) {
      const std::string named = stadium_form_file(asset->source_name.empty() ? asset->info.name : asset->source_name);
      if (!named.empty() && lower(named) != wanted) {
        host::log("cosmetics: %s is %s's DAT but was installed for %s; served to %s", asset->info.name.c_str(),
                  named.c_str(), asset->info.target_path.c_str(), named.c_str());
        wanted = lower(named);
      }
    }
    // The English twin of a per-language file. The disc holds PlCaRe.usd and GrPs.usd beside the
    // .dat, and the game loads the .usd when its language is English, so a costume or stage for
    // the .dat slot replaces both: replacing only the .dat changed nothing for English players.
    std::string english;
    if (asset->info.kind != "effect_visual" && wanted.size() > 4 &&
        wanted.compare(wanted.size() - 4, 4, ".dat") == 0)
      english = wanted.substr(0, wanted.size() - 4) + ".usd";
    std::vector<FstFile*> matches, english_matches;
    for (auto& file : files) {
      std::string path = lower(file.path);
      size_t slash = path.find_last_of('/');
      std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
      const bool by_name = wanted.find('/') == std::string::npos;
      if (path == wanted || (by_name && base == wanted)) matches.push_back(&file);
      else if (!english.empty() && (path == english || (by_name && base == english))) english_matches.push_back(&file);
    }
    if (matches.size() != 1 || english_matches.size() > 1) {
      host::log("cosmetics: %s resolved to %zu ISO files (%zu English); override not applied", pick.first.c_str(),
                matches.size(), english_matches.size());
      continue;
    }
    std::vector<FstFile*> targets{matches[0]};
    if (!english_matches.empty()) targets.push_back(english_matches[0]);
    FstFile& file = *matches[0];
    if (asset->info.kind == "effect_visual") {
      std::vector<uint8_t> clean(file.size);
      std::vector<uint8_t> materialized;
      std::string visual_error;
      size_t composed = 0;
      // Slot by slot for a file the slot table covers (cosmetic_effect_slots.cpp), else the chosen moves.
      if (!host::disc_read(file.start, clean.data(), file.size) ||
          !compose_effect_file_locked(g_profile, asset->info.target_path, clean, &materialized, &composed,
                                      &visual_error)) {
        host::log("cosmetics: effect selection rejected for %s; vanilla fallback (%s)",
                  file.path.c_str(), visual_error.c_str());
        runtime_errors.push_back(visual_error);
        continue;
      }
      if (materialized == clean) {
        host::log("cosmetics: effect %s: every slot is the disc's own", file.path.c_str());
        continue;
      }
      bytes = std::move(materialized);
      host::log("cosmetics: composed %zu effect(s) for %s", composed, file.path.c_str());
    }
    bool applied = false;
    for (FstFile* target : targets) {
      FstFile& disc_file = *target;
      std::vector<uint8_t> served = bytes;   // per disc file: the online rule and the length below are per file
      std::vector<uint8_t> online_form;      // a stage skin's online form, when it differs from `served`
      bool online_allowed = true;
      std::string online_reason;   // kept for the Mods tab: the verdict is never only in the log
      if (asset->info.kind == "stage_visual") {
        // A selected skin needs readable gameplay tables offline too. An old explicit selection
        // bypassed the random picker check and reached Ground's missing-lightset assertion.
        std::vector<uint8_t> clean(disc_file.size);
        const bool clean_read = host::disc_read(disc_file.start, clean.data(), disc_file.size);
        stage_safety::Archive original{clean}, readable{served};
        if (clean_read && original.parse() && !readable.parse()) {
          host::log("cosmetics: stage %s rejected: malformed stage tables; using the standard stage",
                    disc_file.path.c_str());
          runtime_errors.push_back(asset->info.name + ": malformed stage tables; using the standard stage.");
          continue;
        }
        // Fewer collision lines or groups than the original: the stage's own code finds its lines and
        // groups by number (Pokemon Stadium's transformations, moving platforms), and one that is not
        // there stops the game ("mplib.c: not found lineID=140", a Stadium skin with its own collision
        // on the Melee 3D disc). Offline too, that skin gives way to the standard stage.
        if (clean_read) {
          stage_safety::Archive o{clean}, k{served};
          if (o.parse() && k.parse() && o.roots.count("coll_data") && k.roots.count("coll_data")) {
            const uint32_t co = o.roots.at("coll_data"), ck = k.roots.at("coll_data");
            if (o.bounds(co, 0x30) && k.bounds(ck, 0x30)) {
              const uint32_t lines_o = o.get(co + 0x0C), lines_k = k.get(ck + 0x0C);
              const uint32_t groups_o = o.get(co + 0x28), groups_k = k.get(ck + 0x28);
              if (lines_k < lines_o || groups_k < groups_o) {
                host::log("cosmetics: stage %s rejected: the skin's collision has %u lines and %u groups, the stage's "
                          "own code needs the original's %u and %u; using the standard stage",
                          disc_file.path.c_str(), lines_k, groups_k, lines_o, groups_o);
                runtime_errors.push_back(asset->info.name + ": its collision is missing parts the stage needs; using the standard stage.");
                continue;
              }
            }
          }
        }
        std::string validation_error = "as installed";
        // The skin is served online as installed, as the desktop app builds it (kStrictOnlineStages).
        // Under the stricter rule, a skin that does not match the original as it is: offline it is
        // served as it is, and online (and in replays) its meshes drawn over the original stage
        // (stage_online_form). The two are given one length, the longer one's, since the game is told
        // one length per file.
        if (kStrictOnlineStages)
          online_allowed = clean_read &&
                           stage_online_form(clean, served, &online_form, &validation_error, disc_file.path);
        // The English twin of a skin made from the .dat: the skin carries the Japanese text (the two
        // files differ only in their SIS text, the Stadium jumbotron's). When the skin drawn over the
        // English file keeps every one of its meshes, that is the skin with English text, so it is
        // served offline too. Otherwise the skin as it is, Japanese text and all.
        const std::string served_path = lower(disc_file.path);
        if (online_allowed && !online_form.empty() && served_path.size() > 4 &&
            served_path.compare(served_path.size() - 4, 4, ".usd") == 0 &&
            lower(asset->info.target_path).find(".usd") == std::string::npos) {
          unsigned drawn = 0, total = 0;
          const size_t at = validation_error.rfind(" of the skin's ");
          if (at != std::string::npos) {
            size_t start = at;
            while (start > 0 && std::isdigit((unsigned char)validation_error[start - 1])) --start;
            drawn = (unsigned)std::strtoul(validation_error.c_str() + start, nullptr, 10);
            total = (unsigned)std::strtoul(validation_error.c_str() + at + 15, nullptr, 10);
          }
          if (total && drawn == total) {
            served = online_form;
            host::log("cosmetics: %s keeps the English text (all %u of the skin's meshes drawn over it)",
                      disc_file.path.c_str(), total);
          }
        }
        if (online_allowed && !online_form.empty()) {
          const size_t extent = std::max(served.size(), online_form.size());
          if (extent > std::numeric_limits<uint32_t>::max() || served.size() < 4 || online_form.size() < 4) {
            online_allowed = false; online_form.clear();
          } else {
            served.resize(extent, 0); put_be32(served.data(), (uint32_t)extent);
            online_form.resize(extent, 0); put_be32(online_form.data(), (uint32_t)extent);
          }
        }
        const auto shared = g_stage_extent.find(asset->info.target_path);
        if (shared != g_stage_extent.end() && shared->second > served.size() && served.size() >= 4 &&
            shared->second <= std::numeric_limits<uint32_t>::max()) {
          served.resize(shared->second, 0); put_be32(served.data(), (uint32_t)shared->second);
          if (online_form.size() >= 4) { online_form.resize(shared->second, 0); put_be32(online_form.data(), (uint32_t)shared->second); }
        }
        host::log("cosmetics: stage %s %s online (%s)", disc_file.path.c_str(),
                  online_allowed ? "allowed" : "uses vanilla", validation_error.c_str());
        online_reason = online_allowed ? validation_error : online_reason_short(validation_error);
      }
      // An effect file is used online as installed, as the desktop app builds it. Under
      // kStrictOnlineEffects: effect files hold the particle generators, and particles draw from the
      // game's own random number seed (generator.c, particle.c call HSD_Randf), so an effect file that
      // emits a different number of particles changes everything random after it, a desync online.
      // There an effect stays only when it changes textures alone, or is the composed Fox/Falco
      // particle-colour change (colour values only, the same commands); otherwise the disc's file.
      if (asset->info.kind == "effect_visual" && !kStrictOnlineEffects) {
        online_reason = "as installed";
        host::log("cosmetics: effect %s allowed online (as installed)", disc_file.path.c_str());
      } else if (asset->info.kind == "effect_visual") {
        std::vector<uint8_t> clean(disc_file.size);
        std::string validation_error;
        const std::string name = lower(disc_file.path);
        const bool colour_only = name.find("plfx.dat") != std::string::npos || name.find("plfc.dat") != std::string::npos;
        online_allowed = host::disc_read(disc_file.start, clean.data(), disc_file.size) &&
                         (colour_only || visual_dat_only(clean, served, &validation_error));
        if (online_allowed && colour_only) validation_error = "particle colours only";
        else if (online_allowed) validation_error = "textures only";
        host::log("cosmetics: effect %s %s online (%s)", disc_file.path.c_str(),
                  online_allowed ? "allowed" : "uses vanilla", validation_error.c_str());
        online_reason = online_allowed ? validation_error : "changes more than textures";
      }
      if (asset->info.kind == "character_costume") {
        std::vector<uint8_t> clean(disc_file.size), fixed;
        std::string validation_error, close_error;
        const bool clean_read = host::disc_read(disc_file.start, clean.data(), disc_file.size);
        // The desktop app's fix: the skin is drawn, and the bones the game simulates with are the
        // standard costume's (costume_fix_rest_pose). Online, only by everything else an opponent's
        // machine compares too (costume_online_fix: flags, constraints, class names, joint roots), so
        // this machine never simulates with a skin the peer rule refuses; the player then sees the
        // standard costume online, and the skin offline as before.
        std::string offline_detail;
        const bool fixable = clean_read && costume_fix_rest_pose(clean, served, &fixed, &offline_detail);
        online_allowed = fixable && costume_online_fix(clean, served, nullptr, &validation_error);
        if (!clean_read) validation_error.clear();
        else if (!fixable) validation_error = offline_detail;
        host::log("cosmetics: costume %s %s online (%s)", disc_file.path.c_str(),
                  online_allowed ? "allowed" : "uses vanilla", validation_error.c_str());
        online_reason = online_reason_short(validation_error);
        if (fixable && fixed != served && costume_skeleton_matches(clean, served, &close_error)) {
          // A skin within the old check's tolerance keeps the standard bones offline too, as it
          // always did (also when online it plays as the standard costume).
          served = std::move(fixed);
          host::log("cosmetics: costume %s takes the standard costume's rest pose everywhere (the skin's was within the old tolerance)",
                    disc_file.path.c_str());
        } else if (online_allowed && fixed != served) {
          // A reshaped one is the skin as installed offline and the fixed copy online.
          online_form = std::move(fixed);
          host::log("cosmetics: costume %s takes the standard costume's rest pose online (offline the skin keeps its own)",
                    disc_file.path.c_str());
        }
      }
      if (!online_allowed) {
        // This file is the override offline and the disc's own file online, and the game sees one
        // length for it (the FST is patched once). The game also checks that an archive's own
        // length field equals the file's length (HSD_ArchiveParse) and stops when they differ.
        // So the file's length is the longer of the two, the shorter one is padded with zeros,
        // and the length field of whichever is served says that length (the disc's: see read()).
        const size_t extent = std::max<size_t>(served.size(), disc_file.size);
        if (extent > std::numeric_limits<uint32_t>::max() || served.size() < 4) continue;
        served.resize(extent, 0);
        put_be32(served.data(), (uint32_t)extent);
      }
      if (!online_form.empty() && online_form.size() != served.size()) {
        // The same one length for the two forms of an online-allowed file: the FST carries one, so a
        // shorter online form failed the game's reads past its end ("disc read failed") and a longer
        // one was cut short. Both are padded and their length fields say the shared length.
        const size_t extent = std::max(served.size(), online_form.size());
        if (extent > std::numeric_limits<uint32_t>::max() || served.size() < 4 || online_form.size() < 4) continue;
        served.resize(extent, 0);
        online_form.resize(extent, 0);
        put_be32(served.data(), (uint32_t)extent);
        put_be32(online_form.data(), (uint32_t)extent);
      }
      if (served.size() > std::numeric_limits<uint32_t>::max()) continue;
      put_be32(fst + (size_t)disc_file.index * 12 + 8, (uint32_t)served.size());
      RuntimeAsset active{asset->info.id, asset->info.target_path,
                          std::make_shared<const std::vector<uint8_t>>(std::move(served)),
                          disc_file.size, online_allowed, asset->info.kind, std::move(online_reason)};
      if (!online_form.empty()) active.online_bytes = std::make_shared<const std::vector<uint8_t>>(std::move(online_form));
      next->by_start[disc_file.start] = std::move(active);
      host::log("cosmetics: %s -> %s at %08X (%u vanilla bytes, %zu override bytes)",
                asset->info.name.c_str(), disc_file.path.c_str(), disc_file.start, disc_file.size,
                next->by_start[disc_file.start].bytes->size());
      applied = true;
    }
    if (!applied) continue;
    ++next->assets;
    for (const auto& companion : asset->companions) {
      if (companion.kind == "preview") continue;
      std::vector<uint8_t> companion_bytes;
      const fs::path path = g_root / fs::u8path(companion.stored_path);
      std::string companion_error;
      if (!read_bounded(path, kMaxAssetBytes, &companion_bytes, &companion_error) ||
          sha256(companion_bytes) != companion.sha256) {
        host::log("cosmetics: %s companion for %s is missing or changed; using vanilla",
                  companion.kind.c_str(), asset->info.target_path.c_str());
        continue;
      }
      next->companions.push_back({companion.kind, asset->info.target_path, path.string()});
    }
  }
  // With random stage skins on, a stage whose pick is the standard one is served at the same length
  // as its skins (g_stage_extent): the disc's own file padded with zeros, its length field saying so.
  // The game can keep a stage file's length from a read ahead made before the pick was published, and
  // that length must hold whichever form comes next. A standard file at its own, shorter length let
  // a skin be cut short (its symbols are at the end: "Cannot find symbol map_head"), and a skin's
  // length kept for the standard file made the read run past its buffer into the fighter file loaded
  // after it ("Cannot find symbol ftDataNess", lbarchive.c:112).
  if (g_profile.enabled && (g_profile.random_stage_skins || !g_session_stage_pack.empty() || any_pool_locked())) {
    for (const auto& shared : g_stage_extent) {
      if (only_slot && lower(shared.first) != lower(*only_slot)) continue;
      // (with only the stage pools: their stages; cosmetic_stage_pools.cpp)
      if (!g_profile.random_stage_skins && g_session_stage_pack.empty() &&
          (!pool_owned_key_locked(shared.first) || look_owns_locked(shared.first))) continue;
      const std::string wanted = lower(shared.first);
      const std::string english = wanted.size() > 4 && wanted.compare(wanted.size() - 4, 4, ".dat") == 0
                                      ? wanted.substr(0, wanted.size() - 4) + ".usd" : std::string();
      for (auto& file : files) {
        const std::string path = lower(file.path);
        const size_t slash = path.find_last_of('/');
        const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        if (base != wanted && (english.empty() || base != english)) continue;
        if (next->by_start.count(file.start) || shared.second <= file.size || file.size < 4 ||
            shared.second > std::numeric_limits<uint32_t>::max()) continue;
        std::vector<uint8_t> padded(file.size);
        if (!host::disc_read(file.start, padded.data(), file.size)) continue;
        padded.resize(shared.second, 0);
        put_be32(padded.data(), (uint32_t)shared.second);
        put_be32(fst + (size_t)file.index * 12 + 8, (uint32_t)shared.second);
        next->by_start[file.start] = RuntimeAsset{std::string(), shared.first,
            std::make_shared<const std::vector<uint8_t>>(std::move(padded)), file.size, true, kStageLengthKind, std::string()};
        host::log("cosmetics: %s is the standard stage, served at the %zu bytes its skins share", file.path.c_str(),
                  (size_t)shared.second);
      }
    }
  }
  // An exact look: the fighter data files its costumes need (look_fighter_tables). Display tables only, so
  // they are served online and in replays too, with the costumes (whose skeletons match the game's).
  if (!only_slot && !g_look_pack.empty()) {
    std::string pack_disc;
    for (const auto& asset : g_assets)
      if (asset.source_kind == kDiscSource && asset.source_name == g_look_pack) { pack_disc = asset.source_id; break; }
    std::vector<uint8_t> header, pack_fst;
    std::vector<FstFile> pack_files;
    std::string pack_error;
    if (!pack_disc.empty() && read_range(fs::u8path(pack_disc), 0, 0x440, &header) &&
        be32(header.data() + 0x428) >= 12 && be32(header.data() + 0x428) <= 2u * 1024u * 1024u &&
        read_range(fs::u8path(pack_disc), be32(header.data() + 0x424), be32(header.data() + 0x428), &pack_fst) &&
        parse_fst(pack_fst.data(), (uint32_t)pack_fst.size(), &pack_files, &pack_error)) {
      size_t served_tables = 0;
      for (const auto& file : files) {
        const std::string name = file.path.substr(file.path.find_last_of('/') + 1);
        if (name.size() != 8 || name.compare(0, 2, "Pl") != 0 || lower(name.substr(4)) != ".dat" ||
            next->by_start.count(file.start)) continue;
        const auto theirs = std::find_if(pack_files.begin(), pack_files.end(),
                                         [&](const FstFile& f) { return f.path == file.path; });
        if (theirs == pack_files.end() || theirs->size != file.size) continue;
        std::vector<uint8_t> clean(file.size), pack;
        if (!host::disc_read(file.start, clean.data(), file.size) ||
            !read_range(fs::u8path(pack_disc), theirs->start, theirs->size, &pack)) continue;
        std::vector<uint8_t> merged = look_fighter_tables(clean, pack);
        if (merged.empty()) continue;
        next->by_start[file.start] = RuntimeAsset{std::string(), name, std::make_shared<const std::vector<uint8_t>>(std::move(merged)),
                                                  file.size, true, kLookFighterKind, "the look's parts tables"};
        ++served_tables;
      }
      host::log("looks: %s parts tables in %zu fighter files", g_look_pack.c_str(), served_tables);
      // The rest of the look that is only seen or heard: the HUD (IfAll), the pause screen (GmPause), the
      // announcer's names (nr_name.ssm) and the effect files whose particle programs are the game's
      // (effect_particles_same: their textures and effect models change, the match does not). An effect
      // file with other programs would start other particles and draw other random numbers, so it stays the
      // disc's. The select screens stay too (Slippi's menus and the skins' portraits use them). Online and in
      // replays as well: none of it reaches the match.
      size_t served_files = 0, effects_kept = 0, refused = 0;
      for (const auto& file : files) {
        const std::string name = file.path.substr(file.path.find_last_of('/') + 1);
        const std::string low = lower(name);
        const bool effect = low.size() > 6 && low.compare(0, 2, "ef") == 0 && low.compare(low.size() - 4, 4, ".dat") == 0;
        const bool sound = lower(file.path) == "audio/us/nr_name.ssm";
        if (!(effect || sound || low == "ifall.usd" || low == "gmpause.usd") || next->by_start.count(file.start)) continue;
        const auto theirs = std::find_if(pack_files.begin(), pack_files.end(),
                                         [&](const FstFile& f) { return f.path == file.path; });
        if (theirs == pack_files.end() || theirs->size < 4 || (sound && theirs->size != file.size)) continue;
        std::vector<uint8_t> clean(file.size), pack;
        if (!host::disc_read(file.start, clean.data(), file.size) ||
            !read_range(fs::u8path(pack_disc), theirs->start, theirs->size, &pack) || pack == clean) continue;
        if (effect && !effect_particles_same(clean, pack)) { ++effects_kept; continue; }
        std::string why;
        if (!look_file_safe(file.path, clean, pack, &why)) {
          host::log("looks: %s's %s is not served (%s); the disc's file stays", g_look_pack.c_str(), file.path.c_str(),
                    why.c_str());
          ++refused;
          continue;
        }
        const size_t extent = std::max<size_t>(pack.size(), file.size);
        if (extent > std::numeric_limits<uint32_t>::max()) continue;
        if (!sound) { pack.resize(extent, 0); put_be32(pack.data(), (uint32_t)extent); }
        put_be32(fst + (size_t)file.index * 12 + 8, (uint32_t)extent);
        next->by_start[file.start] = RuntimeAsset{std::string(), name, std::make_shared<const std::vector<uint8_t>>(std::move(pack)),
                                                  file.size, true, kLookFileKind, "the look's own file"};
        ++served_files;
      }
      host::log("looks: %s HUD, pause, announcer and effect files: %zu served, %zu effects kept (their particle programs "
                "differ), %zu refused", g_look_pack.c_str(), served_files, effects_kept, refused);
    }
  }
  // A slot's own portrait and stock icon, after the skins: they show where the selected skin brought
  // no picture of that kind.
  for (const AssetRecord* asset : portraits) {
    const std::string slot = portrait_slot(asset->info.target_path);
    bool any = false;
    for (const auto& companion : asset->companions) {
      if (companion.kind != "csp" && companion.kind != "stock") continue;
      std::vector<uint8_t> picture;
      const fs::path path = g_root / fs::u8path(companion.stored_path);
      std::string picture_error;
      if (!read_bounded(path, kMaxAssetBytes, &picture, &picture_error) || sha256(picture) != companion.sha256) {
        host::log("cosmetics: %s for %s is missing or changed; using vanilla",
                  companion.kind == "csp" ? "portrait" : "stock icon", slot.c_str());
        continue;
      }
      // One rule for the player: a skin that brings its own picture shows it; a picture added to
      // the slot on its own fills in wherever there is none (the standard costume, or a skin
      // without one). So stepping through skins on the character select always changes the picture
      // with the skin, and taking the skin off brings the added picture back.
      const bool skin_has_own = std::any_of(next->companions.begin(), next->companions.end(),
          [&](const CompanionOverride& have) { return have.kind == companion.kind && have.target_path == slot; });
      if (skin_has_own) continue;
      next->companions.push_back({companion.kind, slot, path.string()});
      host::log("cosmetics: %s for %s from %s", companion.kind == "csp" ? "portrait" : "stock icon", slot.c_str(),
                companion.source_member.c_str());
      any = true;
    }
    if (any && !only_slot) ++next->assets;   // one slot again: its portrait entry was counted at startup
  }
  // One slot again keeps the bank files as they are (they came with the copy of the snapshot): which
  // bank a file serves is decided at match load, from the skins the slots serve then.
  if (!only_slot) publish_voices_locked(fst, files, next.get());
  std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
  if (only_slot) {
    bool served = false;
    for (const auto& file : slot_files) {
      RepublishedFile out;
      out.fst_index = file.index; out.vanilla_start = file.start; out.length = file.size;
      const auto now = next->by_start.find(file.start);
      if (now != next->by_start.end()) {
        out.overridden = true;
        served |= now->second.kind != kStageLengthKind;   // a padded standard stage is still the standard one
        out.length = (uint32_t)now->second.bytes->size();
        out.online_allowed = now->second.online_allowed;
        out.online_form = now->second.online_bytes != nullptr && now->second.kind == "character_costume";
        out.asset_id = now->second.id;
        if (now->second.kind == "effect_visual" && now->second.bytes) {
          uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a of what is served
          for (const uint8_t b : *now->second.bytes) h = (h ^ b) * 0x100000001b3ull;
          char text[17];
          std::snprintf(text, sizeof text, "%016llx", (unsigned long long)h);
          out.content = text;
        }
      }
      republished->files.push_back(std::move(out));
    }
    if (!served)
      host::log("cosmetics: %s is the %s again", only_slot->c_str(),
                effect_archive_target(lower(*only_slot)) || fighter_base_target(lower(*only_slot)) ? "game's own file"
                                                                                                  : "standard costume");
    g_live_unpublished = false;
    republished->ok = true;
    return;
  }
  g_message = next->assets == 0 ? "No selected cosmetic matched this ISO." :
              std::to_string(next->assets) + " cosmetic override(s) active for this launch.";
  for (const auto& issue : runtime_errors) g_message += " " + issue;
  // The game loads every stage and fighter file whole into heaps of a fixed size, laid out for the
  // disc's own files (lbheap.c: 5.0 MB and 6.3 MB of main memory), and stops with
  // `assertion "memp_kouho"` in lbmemory.c when a file finds no room there. That includes the
  // title screen, which preloads a random demo stage and four fighters. Nothing is changed here:
  // a file that grew by more than this is named, so the stop has a cause the player can act on.
  constexpr size_t kLargeGrowth = 1u << 20;
  size_t large = 0, growth = 0;
  std::string largest; size_t largest_growth = 0;
  for (const auto& served : next->by_start) {
    const RuntimeAsset& item = served.second;
    if (!item.bytes || item.bytes->size() <= (size_t)item.vanilla_size + kLargeGrowth) continue;
    const size_t grew = item.bytes->size() - item.vanilla_size;
    ++large; growth += grew;
    if (grew > largest_growth) { largest_growth = grew; largest = item.target_path; }
    host::log("cosmetics: %s is %.1f MB larger than the game's file; the game's memory is laid out for the original",
              item.target_path.c_str(), grew / 1048576.0);
  }
  if (large) {
    char text[256];
    std::snprintf(text, sizeof text, " %zu mod file(s) are much larger than the game's own (%.1f MB more in all, the most in %s). "
                  "The game can stop with a memory error when it loads them; if it does, turn the largest ones off.",
                  large, growth / 1048576.0, largest.c_str());
    g_message += text;
    host::log("cosmetics: %zu oversized override(s), %.1f MB over the originals; a lbmemory.c stop on load means one did not fit",
              large, growth / 1048576.0);
  }
}

void apply_to_fst(uint8_t* fst, uint32_t fst_size) {
  std::lock_guard<std::mutex> lock(g_mutex);
  publish_locked(fst, fst_size, nullptr, nullptr);
}

RepublishResult republish_slot(uint8_t* fst, uint32_t fst_size, const std::string& slot) {
  RepublishResult result;
  std::lock_guard<std::mutex> lock(g_mutex);
  SlotName known;
  if (!fst || !find_slot(slot, &known)) { result.message = "That is not a costume slot."; return result; }
  const std::string target = slot_file(known);
  publish_locked(fst, fst_size, &target, &result);
  if (!result.ok) host::log("cosmetics: %s was not published again (%s)", target.c_str(), result.message.c_str());
  return result;
}

RepublishResult republish_effect(uint8_t* fst, uint32_t fst_size, const std::string& effect_file) {
  RepublishResult result;
  std::string target = effect_file;
  if (!target.empty() && target[0] == '/') target.erase(0, 1);
  if (const size_t key = target.find('#'); key != std::string::npos) target.erase(key);   // a pick's key
  const std::string low = lower(target);
  std::lock_guard<std::mutex> lock(g_mutex);
  // (only a file the game reads again for every match: the effect checker's "works" is this same test)
  if (!fst || target.find('/') != std::string::npos || !effect_file_live(low)) {
    result.message = "That is not an effect file.";
    return result;
  }
  publish_locked(fst, fst_size, &target, &result);
  if (!result.ok) host::log("cosmetics: %s was not published again (%s)", target.c_str(), result.message.c_str());
  else host::log("cosmetics: effect file %s published again for the next match", target.c_str());
  return result;
}

std::string applied_asset(uint32_t vanilla_file_start) {
  const auto runtime = std::atomic_load(&g_runtime);
  const auto found = runtime->by_start.find(vanilla_file_start);
  return found == runtime->by_start.end() ? std::string() : found->second.id;
}

}  // namespace host::cosmetics
