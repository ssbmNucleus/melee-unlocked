// Cosmetic effects: effect files checked and made, their targets, composition of several
// effects into one file, import, previews, project effects.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"
#include "effect_slots.h"

namespace host::cosmetics {
namespace cosmetic_internal {

constexpr std::array<std::array<uint16_t, 23>, 3> kParticleSignatures{{
    {{0x0323, 0x0521, 0x0523, 0x0525, 0x0523, 0x0526, 0x0522, 0x0520,
      0x0522, 0x051f, 0x0521, 0x0524, 0x0525, 0x0527, 0x0526, 0x0527,
      0x0520, 0x051e, 0x051f, 0x051e, 0x0524, 0x0527, 0x0500}},
    {{0x1516, 0x151a, 0x1515, 0x151a, 0x151b, 0x151a, 0x151d, 0x1514,
      0x1517, 0x1516, 0x1518, 0x1515, 0x1519, 0x151b, 0x1519, 0x151c,
      0x1518, 0x151c, 0x1517, 0x151c, 0x151d, 0x151b, 0x1500}},
    {{0x2f0b, 0x2f10, 0x2f0d, 0x2f10, 0x2f11, 0x2f10, 0x2f13, 0x2f0a,
      0x2f0c, 0x2f0b, 0x2f0e, 0x2f0d, 0x2f0f, 0x2f11, 0x2f0f, 0x2f12,
      0x2f0e, 0x2f12, 0x2f0c, 0x2f12, 0x2f13, 0x2f11, 0x2f00}},
}};
constexpr std::array<uint8_t, 6> kFoxSideColor{{0x00, 0x99, 0xff, 0xff, 0xcc, 0xe6}};

bool exact_root(const VisualLayout& layout, const char* expected, const char* label,
                std::string* error) {
  if (layout.roots.size() == 1 && layout.roots[0] == expected) return true;
  *error = std::string(label) + " DAT root is not exactly " + expected + ".";
  return false;
}

bool find_particle_stream(const std::vector<uint8_t>& bytes, const VisualLayout& layout,
                          const std::array<uint16_t, 23>& signature, const char* label,
                          uint32_t* result, std::string* error) {
  size_t matches = 0;
  constexpr size_t stream_size = 23 * 4;
  for (uint32_t offset = 0; (uint64_t)offset + stream_size <= layout.data_size; offset += 4) {
    bool match = true;
    for (size_t index = 0; index < signature.size(); ++index) {
      if (be16(bytes.data() + 0x20ull + offset + index * 4 + 2) != signature[index]) {
        match = false; break;
      }
    }
    if (match) { *result = offset; ++matches; }
  }
  if (matches == 1) return true;
  *error = std::string(label) + " particle-color stream must occur exactly once; found " +
           std::to_string(matches) + ".";
  return false;
}

bool uniform_stream_color(const std::vector<uint8_t>& bytes, uint32_t offset,
                          const char* label, uint16_t* color, std::string* error) {
  *color = be16(bytes.data() + 0x20ull + offset);
  for (size_t index = 1; index < 23; ++index) {
    if (be16(bytes.data() + 0x20ull + offset + index * 4) != *color) {
      *error = std::string(label) + " particle-color stream is not uniform."; return false;
    }
  }
  return true;
}

// Effect targets: a fighter's effect archive (EfMrData.dat), the common effect archive
// (EfCoData.dat), or a fighter's base archive (PlMr.dat; Fox and Falco also take particle-colour
// edits). The code is any fighter family code, matched case-insensitively.
bool fighter_code_known(const std::string& code) {
  for (const auto& family : families) if (lower(family.file_code) == code) return true;
  return false;
}
bool effect_archive_target(const std::string& lowered) {
  // Ice Climbers and a few others use effect codes of their own (EfIcData.dat), so any two
  // letters pass here; the disc lookup before validation decides whether the file exists.
  return lowered.size() == 12 && lowered.compare(0, 2, "ef") == 0 &&
         lowered.compare(4, 8, "data.dat") == 0;
}
bool fighter_base_target(const std::string& lowered) {
  return lowered.size() == 8 && lowered.compare(0, 2, "pl") == 0 &&
         lowered.compare(4, 4, ".dat") == 0 && fighter_code_known(lowered.substr(2, 2));
}
// Every effect target: EfCoData.dat is loaded again for each match (gm/gmvs.c and the other match
// scenes: efAsync_LoadSync(0) right after efLib_Init, which forgets every effect file at each scene),
// a fighter's EfXxData.dat and PlXx.dat with the fighter for each match (ft/fighter.c Fighter_800679B0).
bool effect_file_live(const std::string& lowered) {
  return effect_archive_target(lowered) || fighter_base_target(lowered);
}

bool materialize_effect_dat_impl(const std::string& target_path,
                                 const std::vector<uint8_t>& clean,
                                 const std::vector<uint8_t>& candidate,
                                 std::vector<uint8_t>* runtime,
                                 std::string* classification,
                                 std::string* error) {
  runtime->clear(); classification->clear();
  const std::string target = lower(target_path);
  if (!effect_archive_target(target) && !fighter_base_target(target)) {
    *error = "Unsupported effect target: " + target_path + ".";
    return false;
  }
  std::string texture_error;
  if (visual_dat_only(clean, candidate, &texture_error)) {
    *runtime = candidate;
    *classification = "Texture-only effect validated against the exact clean ISO.";
    return true;
  }

  VisualLayout base, mod;
  if (!parse_visual_layout(clean, &base, error) || !parse_visual_layout(candidate, &mod, error))
    return false;
  if (target == "efcodata.dat") {
    *error = "Common effect DAT changes bytes outside validated texture payloads.";
    return false;
  }
  // A fighter's own effect archive holds only particle and effect model data, so a full
  // replacement with the same roots is taken as is. It is offline only: the online rule serves the
  // disc's file unless the change is textures alone (particles draw from the game's random seed).
  if (effect_archive_target(target)) {
    if (base.roots.empty() || base.roots != mod.roots) {
      *error = "Effect DAT roots do not match the clean resource."; return false;
    }
    if (clean == candidate) { *error = "Effect DAT is identical to the clean resource."; return false; }
    // Its particle programs and texture bank are read as the game's loader reads them (particle.c).
    data_safety::Archive archive(candidate);
    std::string defect;
    if (!archive.parse() || !data_safety::effect_banks_valid(archive, &defect)) {
      *error = "Effect DAT: " + (defect.empty() ? std::string("its archive tables do not hold together") : defect) + ".";
      return false;
    }
    // The game takes a model effect by its id with no bound (effect_slots::model_effects): one with fewer
    // than the disc's file is read past its table when the move asks for one of the missing ones.
    const int models = effect_slots::model_effects(candidate), retail = effect_slots::model_effects(clean);
    if (models < retail) {
      *error = models < 0 ? std::string("Effect DAT: its table of model effects cannot be read.")
                          : "Effect DAT holds " + std::to_string(models) + " model effects; the clean resource has " +
                                std::to_string(retail) + " (the game would read past its table).";
      return false;
    }
    *runtime = candidate;
    *classification = "Dedicated effect archive validated against the exact clean ISO.";
    return true;
  }

  const char* fighter_root = target == "plfx.dat" ? "ftDataFox" :
                             target == "plfc.dat" ? "ftDataFalco" : nullptr;
  if (!fighter_root) {
    *error = "Fighter DAT effect changes more than textures (" + texture_error + ").";
    return false;
  }
  if (!exact_root(base, fighter_root, "Clean", error) ||
      !exact_root(mod, fighter_root, "Candidate", error)) return false;

  *runtime = clean;
  std::array<uint32_t, 3> clean_offsets{}, candidate_offsets{};
  std::array<uint16_t, 3> candidate_colors{};
  for (size_t stream = 0; stream < kParticleSignatures.size(); ++stream) {
    std::string clean_label = "Clean #" + std::to_string(stream + 1);
    std::string candidate_label = "Candidate #" + std::to_string(stream + 1);
    if (!find_particle_stream(clean, base, kParticleSignatures[stream], clean_label.c_str(),
                              &clean_offsets[stream], error) ||
        !find_particle_stream(candidate, mod, kParticleSignatures[stream], candidate_label.c_str(),
                              &candidate_offsets[stream], error)) return false;
    uint16_t clean_color = 0;
    if (!uniform_stream_color(clean, clean_offsets[stream], clean_label.c_str(),
                              &clean_color, error) ||
        !uniform_stream_color(candidate, candidate_offsets[stream], candidate_label.c_str(),
                              &candidate_colors[stream], error)) return false;
    if (clean_color != 0xfc00) {
      *error = "Clean fighter DAT does not match the exact NTSC 1.02 color stream."; return false;
    }
    for (size_t record = 0; record < 23; ++record) {
      size_t source = 0x20ull + candidate_offsets[stream] + record * 4;
      size_t destination = 0x20ull + clean_offsets[stream] + record * 4;
      (*runtime)[destination] = candidate[source];
      (*runtime)[destination + 1] = candidate[source + 1];
    }
  }
  if (candidate_colors[0] == 0xfc00 || candidate_colors[0] != candidate_colors[1] ||
      candidate_colors[0] != candidate_colors[2]) {
    *error = "Candidate particle-color streams do not contain one coherent new color."; return false;
  }

  if (target == "plfx.dat") {
    if (clean.size() != candidate.size() || base.data_size != mod.data_size ||
        base.relocations != mod.relocations || base.roots != mod.roots ||
        base.relocation_start != mod.relocation_start ||
        std::memcmp(clean.data() + base.relocation_start,
                    candidate.data() + mod.relocation_start,
                    clean.size() - base.relocation_start)) {
      *error = "Fox fighter effect DAT changes layout, relocation, roots, or strings."; return false;
    }
    auto begin = clean.begin() + 0x20;
    auto end = begin + base.data_size;
    auto side = std::search(begin, end, kFoxSideColor.begin(), kFoxSideColor.end());
    if (side == end || std::search(side + 1, end, kFoxSideColor.begin(), kFoxSideColor.end()) != end) {
      *error = "Clean Fox side-B color field is not unique."; return false;
    }
    size_t offset = (size_t)std::distance(clean.begin(), side);
    std::copy(candidate.begin() + offset, candidate.begin() + offset + kFoxSideColor.size(),
              runtime->begin() + offset);
    if (*runtime != candidate) {
      *error = "Fox fighter DAT changes bytes outside verified particle-color fields."; return false;
    }
  }
  if (*runtime == clean) { *error = "Effect DAT does not change a verified particle color."; return false; }
  *classification = "Effect colors merged into the exact clean fighter archive.";
  return true;
}

std::string effect_scope(const AssetRecord& asset) {
  if (asset.info.kind != "effect_visual") return {};
  const std::string& id = asset.source_id;
  if (id.rfind("effect:", 0) != 0) return lower(asset.info.costume);
  const size_t begin = id.find(':', 7);
  if (begin == std::string::npos) return lower(asset.info.costume);
  const size_t end = id.find(':', begin + 1);
  return lower(id.substr(begin + 1, end == std::string::npos ? end : end - begin - 1));
}

std::string effect_label(const std::string& scope) {
  if (scope == "upb") return "Fire Fox";
  if (scope == "shine") return "Reflector (Shine)";
  if (scope == "sideb") return "Fox Illusion";
  if (scope == "laser") return "Laser";
  if (scope == "common_shield") return "Shields";
  return scope;
}

// The disc archive an effect DAT replaces, from its root: "effMarioDataTable" is EfMrData.dat and
// "ftDataMario" is PlMr.dat. Nucleus names its files freely, so the root is the reliable name.
bool effect_target_from_roots(const std::vector<std::string>& roots, std::string* path) {
  if (roots.size() != 1) return false;
  const std::string& root = roots[0];
  for (const auto& family : families) {
    if (root == std::string("eff") + family.root_name + "DataTable") {
      *path = std::string("Ef") + family.file_code + "Data.dat"; return true;
    }
    if (root == std::string("ftData") + family.root_name) {
      *path = std::string("Pl") + family.file_code + ".dat"; return true;
    }
  }
  return false;
}

bool effect_target(const std::string& character, const std::string& scope,
                   const std::string& model_file,
                   std::string* path, std::string* group, std::string* slot) {
  const std::string who = lower(character), what = lower(scope);
  *group = character;
  // Any other scope: the model file names the disc archive it replaces (EfMrData.dat, PlFc.dat).
  const std::string base = fs::path(model_file).filename().string();
  const bool named = effect_archive_target(lower(base)) || fighter_base_target(lower(base));
  if (who == "fox" && (what == "upb" || what == "shine")) {
    *path = "EfFxData.dat"; *slot = effect_label(what); return true;
  }
  if (who == "fox" && (what == "laser" || what == "sideb")) {
    *path = "PlFx.dat"; *slot = effect_label(what); return true;
  }
  if (who == "falco" && what == "laser") {
    *path = "PlFc.dat"; *slot = "Laser effects"; return true;
  }
  if (what == "common_shield") {
    *path = "EfCoData.dat"; *group = "Global"; *slot = "Shields"; return true;
  }
  if (named) {
    *path = base; *slot = effect_label(what); return true;
  }
  return false;
}

bool materialize_effect_with_open_disc(const std::string& target_path,
                                       const std::vector<uint8_t>& candidate,
                                       std::vector<uint8_t>* runtime,
                                       std::string* classification,
                                       std::string* error) {
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(target_path, &offset, &size)) {
    *error = "The exact clean ISO resource is unavailable; validation is deferred until launch.";
    return false;
  }
  if (!size || size > kMaxAssetBytes) {
    *error = "The clean ISO resource is outside supported bounds."; return false;
  }
  std::vector<uint8_t> clean(size);
  if (!host::disc_read(offset, clean.data(), size)) {
    *error = "The clean ISO resource could not be read for visual-only validation."; return false;
  }
  return materialize_effect_dat_impl(target_path, clean, candidate, runtime, classification, error);
}

std::vector<const AssetRecord*> selected_effects_locked(const Profile& profile,
                                                         const std::string& target) {
  std::vector<const AssetRecord*> selected;
  for (const auto& pick : profile.selections) {
    if (pick.second == kVanillaSelection) continue;
    auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == pick.second && item.info.kind == "effect_visual" &&
             item.info.target_path == target && selection_key(item) == pick.first;
    });
    if (asset != g_assets.end()) selected.push_back(&*asset);
  }
  return selected;
}

bool compose_effects_locked(const std::string& target,
                            const std::vector<const AssetRecord*>& choices,
                            const std::vector<uint8_t>& clean,
                            std::vector<uint8_t>* combined, std::string* error) {
  *combined = clean;
  std::vector<const AssetRecord*> owners(clean.size(), nullptr);
  for (const AssetRecord* choice : choices) {
    std::vector<uint8_t> candidate = load_runtime_asset_locked(*choice, error);
    if (candidate.empty()) return false;
    std::vector<uint8_t> materialized;
    std::string classification;
    if (!materialize_effect_dat_impl(target, clean, candidate, &materialized,
                                     &classification, error)) return false;
    if (materialized.size() != clean.size()) {
      // A full archive replacement of another length cannot be merged with other choices.
      if (choices.size() != 1) {
        *error = "Effect materialization changed the clean resource size."; return false;
      }
      *combined = std::move(materialized);
      return true;
    }
    for (size_t offset = 0; offset < clean.size(); ++offset) {
      if (materialized[offset] == clean[offset]) continue;
      if (owners[offset] && (*combined)[offset] != materialized[offset]) {
        std::ostringstream message;
        message << "Conflict: " << owners[offset]->info.name << " and " << choice->info.name
                << " both alter " << target << " at 0x" << std::hex << offset << ".";
        *error = message.str(); return false;
      }
      (*combined)[offset] = materialized[offset];
      owners[offset] = choice;
    }
  }
  return true;
}

bool compose_effects_with_open_disc_locked(const std::string& target,
                                          const Profile& profile, std::string* error) {
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(target, &offset, &size) || !size || size > kMaxAssetBytes) {
    *error = "The exact clean ISO effect resource is unavailable."; return false;
  }
  std::vector<uint8_t> clean(size), combined;
  if (!host::disc_read(offset, clean.data(), size)) {
    *error = "The clean ISO effect resource could not be read."; return false;
  }
  return compose_effect_file_locked(profile, target, clean, &combined, nullptr, error);
}

}  // namespace cosmetic_internal

// ---- an effect file on its own (SSBM Nucleus's effects: docs/public-api.md, "the file under its own
// name") ----
// The game's effect archives and whose effects they are (ef/efasync.c efAsync_DatEntries): the ones
// the fighters' own roots (effect_target_from_roots) do not name, the shared one and the Ice Climbers'.
struct EffectArchive { const char* code; const char* root; const char* who; };
constexpr EffectArchive kOtherEffectArchives[] = {
    {"Co", "effCommonDataTable", "Common"},
    {"Ic", "effIceclimberDataTable", "Ice Climbers"},
};

// The disc file an effect file replaces and whose effects they are ("Fox"; "Common" for the shared
// archive): from its root ("effFoxDataTable" is EfFxData.dat, "ftDataFox" is PlFx.dat), which is the
// reliable name (Nucleus names its files freely), else from its own name when that is a disc file's
// ("EfFxData.dat", "PlFc.dat"). False when it is neither.
bool standalone_effect_target(const std::vector<std::string>& roots, const std::string& filename,
                              std::string* target, std::string* who) {
  const auto fighter_of = [&](const std::string& path) {
    const std::string code = lower(path.substr(2, 2));
    for (const auto& family : families)
      if (lower(family.file_code) == code) { *who = family.display_name; return true; }
    for (const auto& other : kOtherEffectArchives)
      if (lower(other.code) == code) { *who = other.who; return true; }
    return false;
  };
  for (const auto& other : kOtherEffectArchives)
    if (roots.size() == 1 && roots[0] == other.root) {
      *target = std::string("Ef") + other.code + "Data.dat"; *who = other.who; return true;
    }
  if (effect_target_from_roots(roots, target) && fighter_of(*target)) return true;
  const std::string name = lower(path_filename_utf8(fs::u8path(filename)));
  if (!effect_archive_target(name) && !fighter_base_target(name)) return false;
  // The disc's spelling: "Ef" or "Pl", the code's capital and small letter, the rest.
  std::string disc = name;
  disc[0] = (char)std::toupper((unsigned char)disc[0]);
  disc[2] = (char)std::toupper((unsigned char)disc[2]);
  if (disc.size() == 12) disc[4] = 'D';
  if (!fighter_of(disc)) return false;
  *target = disc;
  return true;
}

// The effect checks' reason (materialize_effect_dat_impl) in a few plain words for the player (the
// in-game viewer has one short line; the log keeps the checks' own words).
std::string plain_effect_refusal(const std::string& why) {
  const auto says = [&](const char* words) { return why.find(words) != std::string::npos; };
  if (says("unavailable")) return "The game disc is needed to check it.";
  if (says("identical")) return "It is the game's own file.";
  if (says("model effects")) return "It has fewer effects than the game's file.";
  if (says("roots do not match") || says("Unsupported effect target")) return "Not laid out as the game's file.";
  if (says("outside validated texture") || says("more than textures") || says("outside verified")) return "It changes more than textures.";
  if (says("olor")) return "Its color change does not fit.";
  if (why.rfind("Effect DAT: ", 0) == 0) return "Its effect data is damaged.";
  return "The game cannot use this file.";
}

// An effect file as a catalog resource, checked now against the open disc by the checks every effect
// passes when it is served (judge_effect_dat: materialize_effect_dat_impl and the checker's own). When it
// is not taken, `verdict` says why (its reason a short plain sentence).
bool make_standalone_effect(const fs::path& source, const std::string& member, std::vector<uint8_t> bytes,
                            VaultResourcePlan* resource, EffectVerdict* verdict) {
  const std::string filename = path_filename_utf8(member.empty() ? source : fs::u8path(member));
  judge_effect_dat(filename, bytes, verdict);
  if (verdict->verdict != "works" && verdict->verdict != "works_after_restart") {
    host::log("cosmetics: effect file %s refused (%s): %s", filename.c_str(),
              verdict->verdict.empty() ? "not checked" : verdict->verdict.c_str(), verdict->detail.c_str());
    return false;
  }
  const std::string& target = verdict->target;
  const std::string& who = verdict->character;
  const std::string digest = verdict->sha256;
  if (digest.empty()) { verdict->verdict.clear(); verdict->reason = "The file could not be checked."; return false; }
  host::log("cosmetics: effect %s for %s (%s): %s", filename.c_str(), target.c_str(), who.c_str(), verdict->detail.c_str());
  resource->source_id = "standalone-effect:" + target + ":" + digest;
  resource->kind = "effect_visual";
  resource->target_path = target;
  resource->group = who;
  // Its move key (selection_key: "EfFxData.dat#effects"): one effect file per disc file at a time.
  resource->slot = "Effects";
  resource->display_name = path_filename_utf8(fs::u8path(filename).stem());
  if (resource->display_name.empty() || lower(resource->display_name + ".dat") == lower(target))
    resource->display_name = who + " effects";
  resource->source_member = member;
  resource->bytes = std::move(bytes);
  return true;
}

ImportResult import_effect(const std::string& path_text, bool select) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string readiness;
    if (!mutable_profile_locked(&readiness)) return {false, false, {}, readiness};
  }
  const fs::path path = fs::u8path(path_text);
  const std::string extension = lower(path.extension().string());
  VaultPlan plan;
  std::string error;
  EffectVerdict verdict;
  if (extension == ".dat" || extension == ".usd") {
    std::vector<uint8_t> bytes;
    VaultResourcePlan effect;
    if (!read_bounded(path, kMaxAssetBytes, &bytes, &error)) return {false, false, {}, error};
    if (!make_standalone_effect(path, {}, std::move(bytes), &effect, &verdict))
      return {false, false, {}, verdict.reason, verdict.verdict};
    plan.resources.push_back(std::move(effect));
  } else if (extension == ".zip") {
    // The site's export of an effects card: the file under its own name and "preview.<ext>", the mod's
    // first screenshot (kept as the effect's picture when it is a PNG). Every DAT in it must be an
    // effect the checks take: a zip goes in whole or not at all.
    Zip zip;
    if (!open_zip(path, &zip, &error)) return {false, false, {}, error};
    const std::vector<ZipEntry>& entries = zip.entries();
    const ZipEntry* picture = nullptr;
    uint32_t candidates = 0;
    for (const auto& entry : entries) {
      const std::string name = lower(path_filename_utf8(fs::u8path(entry.name)));
      if (lower(fs::u8path(entry.name).extension().string()) == ".png" &&
          (!picture || (name.rfind("preview.", 0) == 0 &&
                        lower(path_filename_utf8(fs::u8path(picture->name))).rfind("preview.", 0) != 0)))
        picture = &entry;
      if (!dat_extension(entry.name)) continue;
      if (++candidates > kMaxDatCandidates) return {false, false, {}, "Too many files in it."};
      std::vector<uint8_t> bytes;
      VaultResourcePlan effect;
      if (!zip.read(entry, &bytes, &error)) return {false, false, {}, error};
      if (!make_standalone_effect(path, entry.name, std::move(bytes), &effect, &verdict))
        return {false, false, {}, verdict.reason, verdict.verdict};
      plan.resources.push_back(std::move(effect));
    }
    if (plan.resources.empty()) return {false, false, {}, "No effect file in it.", "not_an_effect"};
    std::vector<uint8_t> preview;
    std::string preview_error;
    if (picture && zip.read(*picture, &preview, &preview_error) && png_companion(preview, &preview_error))
      for (auto& effect : plan.resources) effect.companions.push_back({"preview", picture->name, preview});
  } else {
    return {false, false, {}, "Not an effect file of this game.", "not_an_effect"};
  }
  plan.effect_records = plan.resources.size();
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = install_vault_locked(path, std::move(plan), "standalone_effect", select);
  g_message = result.message;
  return result;
}

ImportResult set_effect_preview(const std::string& asset_id, const std::string& png_path) {
  ImportResult result;
  std::vector<uint8_t> png;
  std::string error;
  if (!read_bounded(fs::u8path(png_path), 16ull * 1024 * 1024, &png, &error)) { result.message = error; return result; }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id && item.info.kind == "effect_visual";
  });
  if (asset == g_assets.end()) { result.message = "That effect is not installed."; return result; }
  if (!png_companion(png, &error)) { result.message = error; return result; }
  const std::string digest = sha256(png);
  if (digest.empty()) { result.message = "The picture could not be hashed."; return result; }
  AssetRecord::Companion picture;
  picture.kind = "preview";
  picture.stored_path = (fs::path(L"assets") / fs::u8path(asset_id) / L"companions" /
                         fs::u8path(player_picture_file("preview", digest))).generic_u8string();
  picture.sha256 = digest;
  picture.source_member = path_filename_utf8(fs::u8path(png_path));
  const auto had = std::find_if(asset->companions.begin(), asset->companions.end(),
                                [](const AssetRecord::Companion& c) { return c.kind == "preview"; });
  if (had != asset->companions.end() && had->sha256 == digest) {
    result.ok = result.already_present = true; result.asset_id = asset_id; return result;
  }
  const std::string old_path = had != asset->companions.end() ? had->stored_path : std::string();
  if (!write_atomic(g_root / fs::u8path(picture.stored_path), png.data(), png.size(), &error)) { result.message = error; return result; }
  const AssetRecord previous = *asset;
  asset->companions.erase(std::remove_if(asset->companions.begin(), asset->companions.end(),
      [](const AssetRecord::Companion& c) { return c.kind == "preview"; }), asset->companions.end());
  asset->companions.push_back(picture);
  // (a picture only the lists show: the served file and the profile are the same as before)
  if (!save_state_locked(&error)) {
    *asset = previous;
    DeleteFileW((g_root / fs::u8path(picture.stored_path)).c_str());
    result.message = error; return result;
  }
  if (!old_path.empty() && old_path != picture.stored_path) DeleteFileW((g_root / fs::u8path(old_path)).c_str());
  result.ok = true; result.asset_id = asset_id; result.message = "Picture of " + asset->info.name + " set.";
  return result;
}

bool enable_project_effects(std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;

  // Select one validated variant for each move, then ensure the selected moves
  // compose without writing different values to the same clean-resource byte.
  std::map<std::string, bool> targets;
  std::map<std::string, bool> disc_targets;
  std::map<std::string, std::string> selections;
  std::map<std::string, std::string> failures;
  for (auto& asset : g_assets) {
    if (asset.info.kind != "effect_visual") continue;
    const std::string& target = asset.info.target_path;
    const std::string key = selection_key(asset);
    targets[key] = true;
    disc_targets[target] = true;
    if (selections.find(key) != selections.end()) continue;

    std::string validation_error;
    std::vector<uint8_t> candidate = load_runtime_asset_locked(asset, &validation_error);
    asset.info.available = !candidate.empty();
    if (asset.info.available) {
      std::vector<uint8_t> runtime;
      std::string classification;
      asset.info.available = materialize_effect_with_open_disc(
          target, candidate, &runtime, &classification, &validation_error);
      if (asset.info.available) {
        asset.info.availability_message = classification;
        selections[key] = asset.info.id;
        failures.erase(key);
        continue;
      }
    }
    asset.info.availability_message = validation_error;
    failures[key] = validation_error;
  }
  if (targets.empty()) {
    *error = "No project effects are installed.";
    return false;
  }
  if (selections.size() != targets.size()) {
    std::ostringstream message;
    message << "Project effects were not changed because no safe variant validated for ";
    bool first = true;
    for (const auto& target : targets) {
      if (selections.find(target.first) != selections.end()) continue;
      if (!first) message << ", ";
      first = false;
      message << target.first;
      auto reason = failures.find(target.first);
      if (reason != failures.end() && !reason->second.empty()) message << " (" << reason->second << ")";
    }
    *error = message.str();
    return false;
  }

  Profile previous = g_profile;
  for (auto it = g_profile.selections.begin(); it != g_profile.selections.end();) {
    if (it->first.find('#') != std::string::npos) it = g_profile.selections.erase(it);
    else ++it;
  }
  g_profile.effect_slots.clear();
  for (const auto& selection : selections)
    g_profile.selections[selection.first] = selection.second;
  for (const auto& target : disc_targets) {
    if (!compose_effects_with_open_disc_locked(target.first, g_profile, error)) {
      g_profile = std::move(previous);
      return false;
    }
  }
  g_profile.enabled = true;
  ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_message = std::to_string(selections.size()) +
              " project effect move slots selected; restart to apply them.";
  return true;
}

// An effect file whose particle programs are the disc's own, byte for byte: the bank the first word of its
// data table ("eff...DataTable") points to. The programs decide how many particles start and every random
// number they draw, from the game's own generator, which the match shares; the texture bank and the effect
// models only change what is drawn. So such a file changes nothing in the match, online or in a replay. A
// file without programs (the table's first word not a pointer) only matches another without.
bool effect_particles_same(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& pack) {
  struct Bank { const uint8_t* at = nullptr; size_t size = 0; bool none = false; };
  auto bank = [](const std::vector<uint8_t>& b, Bank* out) {
    if (b.size() < 0x20) return false;
    const uint32_t data_size = be32(b.data() + 4), relocs = be32(b.data() + 8), roots = be32(b.data() + 12),
                   refs = be32(b.data() + 16);
    const uint64_t tables = 0x20ull + data_size, names = tables + 4ull * relocs + 8ull * (roots + (uint64_t)refs);
    if (names > b.size()) return false;
    const uint8_t* data = b.data() + 0x20;
    uint32_t root = UINT32_MAX;
    for (uint32_t i = 0; i < roots; ++i) {
      const uint8_t* entry = b.data() + tables + 4ull * relocs + 8ull * i;
      const uint64_t name = names + be32(entry + 4);
      if (name >= b.size()) return false;
      const char* text = (const char*)b.data() + name;
      const size_t length = strnlen(text, b.size() - name);
      if (length > 9 && std::memcmp(text + length - 9, "DataTable", 9) == 0) root = be32(entry);
    }
    if (root == UINT32_MAX || root + 8ull > data_size) return false;
    std::vector<uint32_t> starts{data_size, root};
    bool relocated = false;
    for (uint32_t i = 0; i < relocs; ++i) {
      const uint32_t at = be32(b.data() + tables + 4ull * i);
      if (at + 4ull > data_size) return false;
      starts.push_back(be32(data + at));
      relocated |= at == root;
    }
    if (!relocated) { out->none = true; return true; }
    const uint32_t programs = be32(data + root);
    std::sort(starts.begin(), starts.end());
    const auto next = std::upper_bound(starts.begin(), starts.end(), programs);
    const uint32_t end = next == starts.end() ? data_size : *next;
    if (programs >= data_size || end <= programs) return false;
    out->at = data + programs; out->size = end - programs;
    return true;
  };
  Bank a, b;
  if (!bank(clean, &a) || !bank(pack, &b)) return false;
  if (a.none || b.none) return a.none && b.none;
  return a.size == b.size && std::memcmp(a.at, b.at, a.size) == 0;
}

}  // namespace host::cosmetics
