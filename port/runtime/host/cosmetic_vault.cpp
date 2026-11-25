// Cosmetic Nucleus vaults: a vault zip's metadata read into a plan and installed, and the
// zip helpers they use.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

std::string companion_label(const ZipEntry& entry) {
  std::string name = lower(entry.name);
  if (lower(fs::path(name).extension().string()) != ".png") return {};
  if (stock_picture_name(name)) return entry.name + " (stock icon: native texture override)";
  if (name.find("csp") != std::string::npos || name.find("portrait") != std::string::npos)
    return entry.name + " (portrait: native texture override)";
  return {};
}

std::string display_name(const fs::path& source, const std::string& member) {
  fs::path candidate = member.empty() ? source : fs::u8path(member);
  std::string name = path_filename_utf8(candidate.stem());
  std::string member_stem = lower(name);
  if (!member.empty() && member_stem.size() >= 7 && member_stem.rfind("pl", 0) == 0 &&
      member_stem.substr(member_stem.size() - 3) == "mod")
    name = path_filename_utf8(source.stem());
  if (name.empty()) name = path_filename_utf8(source.stem());
  return name.empty() ? "Imported costume" : name;
}

bool metadata_text(const json& object, const char* key, size_t limit, std::string* value,
                   std::string* error) {
  auto found = object.find(key);
  if (found == object.end() || !found->is_string()) {
    *error = std::string("Nucleus metadata is missing string field ") + key + "."; return false;
  }
  *value = found->get<std::string>();
  if (value->empty() || value->size() > limit ||
      std::any_of(value->begin(), value->end(), [](char c) { return (unsigned char)c < 0x20; })) {
    *error = std::string("Nucleus metadata field ") + key + " is empty or outside supported bounds.";
    return false;
  }
  return true;
}

bool png_companion(const std::vector<uint8_t>& bytes, std::string* error) {
  static constexpr uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  if (bytes.size() < 24 || bytes.size() > 16ull * 1024 * 1024 ||
      std::memcmp(bytes.data(), signature, sizeof signature) ||
      std::memcmp(bytes.data() + 12, "IHDR", 4)) {
    *error = "A promised companion is not a bounded identifiable PNG."; return false;
  }
  uint32_t width = be32(bytes.data() + 16), height = be32(bytes.data() + 20);
  if (!width || !height || width > 8192 || height > 8192) {
    *error = "A companion PNG has unsupported dimensions."; return false;
  }
  return true;
}

// A picture the player gave a skin afterwards (set_skin_portrait) is stored under a name with its
// hash in it ("csp-0123456789abcdef.png"); the ones an import or a scan brings are csp.png and
// stock.png. So a scan that writes its csp.png again never touches the player's picture.
std::string player_picture_file(const std::string& kind, const std::string& digest) {
  return kind + "-" + digest.substr(0, 16) + ".png";
}
bool player_picture(const AssetRecord::Companion& companion) {
  if (companion.kind != "csp" && companion.kind != "stock") return false;
  const size_t slash = companion.stored_path.find_last_of('/');
  return companion.stored_path.substr(slash == std::string::npos ? 0 : slash + 1) != companion.kind + ".png";
}

const ZipEntry* unique_basename(const std::vector<ZipEntry>& entries, const std::string& name,
                                std::string* error) {
  const ZipEntry* match = nullptr;
  std::string wanted = lower(name);
  for (const auto& entry : entries) {
    std::string base = entry.name.substr(entry.name.find_last_of('/') == std::string::npos ? 0 :
                                         entry.name.find_last_of('/') + 1);
    if (lower(base) != wanted) continue;
    if (match) { *error = "A Nucleus filename is ambiguous in the vault: " + name; return nullptr; }
    match = &entry;
  }
  if (!match) *error = "A Nucleus metadata file is missing from the vault: " + name;
  return match;
}

// A zip inside a Nucleus vault, read in memory by the same rules as open_zip.
bool open_zip_bytes(const std::vector<uint8_t>& bytes, Zip* zip, std::string* error) {
  if (bytes.empty() || bytes.size() > kMaxArchiveBytes) {
    *error = "The ZIP archive is empty or larger than the 512 MB safety limit."; return false;
  }
  return zip->open_memory(bytes, host::archive::kModLimits, error);
}

bool inspect_nested_costume(const std::vector<uint8_t>& archive_bytes,
                            testing::DatInspection* dat, std::vector<uint8_t>* dat_bytes,
                            std::string* member, std::string* error) {
  Zip zip;
  if (!open_zip_bytes(archive_bytes, &zip, error)) return false;
  size_t recognized = 0;
  for (const auto& entry : zip.entries()) {
    if (!dat_extension(entry.name)) continue;
    std::vector<uint8_t> candidate;
    if (!zip.read(entry, &candidate, error)) return false;
    auto inspected = inspect_dat_impl(candidate);
    if (!inspected.ok) continue;
    ++recognized; *dat = std::move(inspected); *dat_bytes = std::move(candidate); *member = entry.name;
  }
  if (recognized != 1) {
    *error = "Each Nucleus character archive must contain exactly one supported existing costume DAT; found " +
             std::to_string(recognized) + ".";
    return false;
  }
  return true;
}

bool inspect_nested_visual(const std::vector<uint8_t>& archive_bytes,
                           std::vector<uint8_t>* dat_bytes, std::string* member,
                           std::string* error) {
  Zip zip;
  if (!open_zip_bytes(archive_bytes, &zip, error)) return false;
  size_t recognized = 0;
  for (const auto& entry : zip.entries()) {
    if (!dat_extension(entry.name)) continue;
    std::vector<uint8_t> candidate;
    if (!zip.read(entry, &candidate, error)) return false;
    VisualLayout layout;
    if (!parse_visual_layout(candidate, &layout, error)) continue;
    ++recognized; *dat_bytes = std::move(candidate); *member = entry.name;
  }
  if (recognized != 1) {
    *error = "Each Nucleus stage archive must contain exactly one structurally valid visual DAT; found " +
             std::to_string(recognized) + ".";
    return false;
  }
  return true;
}

bool parse_nucleus_vault(const Zip& zip, const std::vector<ZipEntry>& entries,
                         const ZipEntry& metadata_entry, VaultPlan* plan, std::string* error) {
  plan->variants.clear(); plan->resources.clear(); plan->rejected_resources.clear();
  plan->stage_records = plan->effect_records = 0;
  if (metadata_entry.size > 16ull * 1024 * 1024) {
    *error = "Nucleus metadata.json exceeds the 16 MB safety limit."; return false;
  }
  std::vector<uint8_t> metadata_bytes;
  if (!zip.read(metadata_entry, &metadata_bytes, error)) return false;
  json metadata;
  try { metadata = json::parse(metadata_bytes.begin(), metadata_bytes.end()); }
  catch (...) { *error = "Nucleus metadata.json is not valid JSON."; return false; }
  auto characters = metadata.find("characters");
  if (!metadata.is_object() || characters == metadata.end() || !characters->is_object()) {
    *error = "Nucleus metadata.json does not contain a character catalog."; return false;
  }
  std::map<std::string, bool> source_ids;
  for (auto group = characters->begin(); group != characters->end(); ++group) {
    if (!group.value().is_object()) { *error = "A Nucleus character group is malformed."; return false; }
    auto skins = group.value().find("skins");
    if (skins != group.value().end() && !skins->is_array()) {
      *error = "A Nucleus character skin list is malformed."; return false;
    }
    if (skins != group.value().end()) for (const auto& skin : *skins) {
      if (!skin.is_object()) { *error = "A Nucleus skin record is malformed."; return false; }
      VaultVariantPlan variant;
      std::string filename, costume_code;
      if (!metadata_text(skin, "id", 256, &variant.source_id, error) ||
          !metadata_text(skin, "color", 256, &variant.display_name, error) ||
          !metadata_text(skin, "filename", 512, &filename, error) ||
          !metadata_text(skin, "costume_code", 16, &costume_code, error)) return false;
      if (!source_ids.emplace(lower(variant.source_id), true).second) {
        *error = "Nucleus metadata contains a duplicate skin id: " + variant.source_id; return false;
      }
      for (const char* dependency_key : {"paired_nana_id", "paired_popo_id"}) {
        auto dependency = skin.find(dependency_key);
        if (dependency == skin.end()) continue;
        std::string dependency_id;
        if (!metadata_text(skin, dependency_key, 256, &dependency_id, error)) return false;
        variant.dependencies.push_back(std::move(dependency_id));
      }
      if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos ||
          lower(fs::path(filename).extension().string()) != ".zip") {
        *error = "A Nucleus skin filename is not a safe ZIP basename: " + filename; return false;
      }
      const ZipEntry* archive = unique_basename(entries, filename, error);
      if (!archive) return false;
      variant.archive_member = archive->name;
      std::vector<uint8_t> archive_bytes;
      if (!zip.read(*archive, &archive_bytes, error) ||
          !inspect_nested_costume(archive_bytes, &variant.dat, &variant.bytes,
                                  &variant.dat_member, error)) return false;
      if (lower(variant.dat.target_path) != lower(costume_code + ".dat")) {
        *error = "Nucleus metadata target " + costume_code +
                 " conflicts with validated DAT target " + variant.dat.target_path + ".";
        return false;
      }
      size_t slash = archive->name.find_last_of('/');
      std::string parent = slash == std::string::npos ? "" : archive->name.substr(0, slash + 1);
      std::string stem = filename.substr(0, filename.size() - 4);
      for (const auto& spec : {std::pair<const char*, const char*>{"csp", "_csp.png"},
                               {"stock", "_stc.png"}}) {
        bool promised = skin.value(std::string("has_") + spec.first, false);
        std::string wanted = lower(parent + stem + spec.second);
        auto companion = std::find_if(entries.begin(), entries.end(), [&](const ZipEntry& item) {
          return lower(item.name) == wanted;
        });
        if (promised && companion == entries.end()) {
          variant.companion_notices.push_back(
              (std::string(spec.first) == "csp" ? "CSP" : "Stock icon") +
              std::string(" missing; vanilla UI fallback will be used."));
          continue;
        }
        if (companion != entries.end()) {
          VaultCompanionPlan planned{spec.first, companion->name, {}};
          if (!zip.read(*companion, &planned.bytes, error) ||
              !png_companion(planned.bytes, error)) return false;
          variant.companions.push_back(std::move(planned));
        }
      }
      plan->variants.push_back(std::move(variant));
    }
    auto extras = group.value().find("extras");
    if (extras != group.value().end()) {
      if (!extras->is_object()) { *error = "A Nucleus effects catalog is malformed."; return false; }
      for (auto scope = extras->begin(); scope != extras->end(); ++scope) {
        if (!scope.value().is_array()) { *error = "A Nucleus effect list is malformed."; return false; }
        plan->effect_records += scope.value().size();
        for (const auto& effect : scope.value()) {
          if (!effect.is_object()) { *error = "A Nucleus effect record is malformed."; return false; }
          VaultResourcePlan resource;
          std::string effect_id, model_file;
          std::string record_error;
          if (!metadata_text(effect, "id", 256, &effect_id, &record_error)) {
            plan->rejected_resources.push_back("effect:" + group.key() + ":" + scope.key() +
                                               " has incomplete metadata.");
            continue;
          }
          resource.source_id = "effect:" + group.key() + ":" + scope.key() + ":" + effect_id;
          resource.kind = "effect_visual";
          if (!metadata_text(effect, "name", 256, &resource.display_name, &record_error) ||
              !metadata_text(effect, "model_file", 512, &model_file, &record_error)) {
            plan->rejected_resources.push_back(resource.source_id + " has incomplete metadata.");
            continue;
          }
          const bool scoped = effect_target(group.key(), scope.key(), model_file,
                                            &resource.target_path, &resource.group, &resource.slot);
          const ZipEntry* model = nullptr;
          const std::string suffix = "/" + lower(model_file);
          for (const auto& entry : entries) {
            std::string candidate = lower(entry.name);
            if (candidate.size() < suffix.size() ||
                candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix)) continue;
            if (model) { model = nullptr; break; }
            model = &entry;
          }
          if (!model) {
            plan->rejected_resources.push_back(resource.source_id + " has a missing or ambiguous model file.");
            continue;
          }
          resource.source_member = model->name;
          if (!zip.read(*model, &resource.bytes, &record_error)) {
            plan->rejected_resources.push_back(resource.source_id + " rejected: " + record_error);
            continue;
          }
          VisualLayout layout;
          std::string visual_error;
          if (!parse_visual_layout(resource.bytes, &layout, &visual_error)) {
            plan->rejected_resources.push_back(resource.source_id + " rejected: " + visual_error);
            continue;
          }
          if (!scoped) {
            if (!effect_target_from_roots(layout.roots, &resource.target_path)) {
              plan->rejected_resources.push_back(resource.source_id + " has an unsupported effect scope.");
              continue;
            }
            resource.group = group.key();
            resource.slot = effect_label(lower(scope.key()));
          }
          plan->resources.push_back(std::move(resource));
        }
      }
    }
  }
  for (const auto& variant : plan->variants) for (const auto& dependency : variant.dependencies) {
    if (source_ids.find(lower(dependency)) == source_ids.end()) {
      *error = "Nucleus skin " + variant.source_id + " references missing dependency " + dependency + ".";
      return false;
    }
  }
  auto stages = metadata.find("stages");
  if (stages != metadata.end()) {
    if (!stages->is_object()) { *error = "The Nucleus stage catalog is malformed."; return false; }
    for (auto stage = stages->begin(); stage != stages->end(); ++stage) {
      if (!stage.value().is_object()) { *error = "A Nucleus stage group is malformed."; return false; }
      auto variants = stage.value().find("variants");
      if (variants != stage.value().end()) {
        if (!variants->is_array()) { *error = "A Nucleus stage variant list is malformed."; return false; }
        plan->stage_records += variants->size();
        for (const auto& variant : *variants) {
          if (!variant.is_object()) { *error = "A Nucleus stage record is malformed."; return false; }
          VaultResourcePlan resource;
          std::string variant_id, filename;
          std::string record_error;
          if (!metadata_text(variant, "id", 256, &variant_id, &record_error)) {
            plan->rejected_resources.push_back("stage:" + stage.key() +
                                               " has incomplete metadata.");
            continue;
          }
          resource.source_id = "stage:" + stage.key() + ":" + variant_id;
          resource.kind = "stage_visual";
          if (!metadata_text(variant, "name", 256, &resource.display_name, &record_error)) {
            plan->rejected_resources.push_back(resource.source_id + " has incomplete metadata.");
            continue;
          }
          if (!stage_target(stage.key(), &resource.target_path, &resource.slot)) {
            plan->rejected_resources.push_back(resource.source_id + " targets an unsupported stage.");
            continue;
          }
          resource.group = "Stages";
          auto filename_value = variant.find("filename");
          if (filename_value == variant.end() || !filename_value->is_string() ||
              filename_value->get<std::string>().empty()) {
            plan->rejected_resources.push_back(resource.source_id + " contains metadata only.");
            continue;
          }
          filename = filename_value->get<std::string>();
          if (filename.size() > 512 || filename.find('/') != std::string::npos ||
              filename.find('\\') != std::string::npos ||
              lower(fs::path(filename).extension().string()) != ".zip") {
            plan->rejected_resources.push_back(resource.source_id + " has an unsafe stage ZIP name.");
            continue;
          }
          const std::string wanted = lower("das/" + stage.key() + "/" + filename);
          auto archive = std::find_if(entries.begin(), entries.end(), [&](const ZipEntry& entry) {
            return lower(entry.name) == wanted;
          });
          if (archive == entries.end()) {
            plan->rejected_resources.push_back(resource.source_id + " is missing its stage ZIP.");
            continue;
          }
          std::vector<uint8_t> archive_bytes;
          if (!zip.read(*archive, &archive_bytes, &record_error)) {
            plan->rejected_resources.push_back(resource.source_id + " rejected: " + record_error);
            continue;
          }
          std::string visual_error, dat_member;
          if (!inspect_nested_visual(archive_bytes, &resource.bytes, &dat_member, &visual_error)) {
            plan->rejected_resources.push_back(resource.source_id + " rejected: " + visual_error);
            continue;
          }
          if (lower(stage.key()) == "pokemon_stadium") {
            std::string actual, actual_slot;
            if (!stage_dat_target(dat_member, &actual, &actual_slot) ||
                (actual != "GrPs.dat" && actual != "GrPs1.dat" && actual != "GrPs2.dat" &&
                 actual != "GrPs3.dat" && actual != "GrPs4.dat")) {
              plan->rejected_resources.push_back(resource.source_id +
                                                 " has no identifiable Pokémon Stadium disc DAT.");
              continue;
            }
            resource.target_path = actual;
            resource.slot = actual_slot;
          }
          resource.source_member = archive->name + "::" + dat_member;
          plan->resources.push_back(std::move(resource));
        }
      }
    }
  }
  if (plan->variants.empty() && plan->resources.empty()) {
    *error = "The Nucleus project contains no supported cosmetic resources."; return false;
  }
  return true;
}


ImportResult install_vault_locked(const fs::path& source, VaultPlan plan,
                                  const std::string& resource_source_kind, bool select_effects) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  std::vector<AssetRecord> next_assets = g_assets;
  Profile next_profile = g_profile;
  std::vector<fs::path> newly_created;
  std::map<std::string, bool> touched_targets;
  size_t added = 0, refreshed = 0;

  struct PendingWrite { fs::path path; const std::vector<uint8_t>* bytes; };
  std::vector<PendingWrite> writes;
  for (auto& variant : plan.variants) {
    std::string digest = sha256(variant.bytes);
    if (digest.empty()) { result.message = "SHA-256 validation could not be initialized."; return result; }
    std::string identity = sha256_text(variant.source_id + "\n" + variant.dat.target_path + "\n" + digest);
    if (identity.empty()) { result.message = "Nucleus identity hashing could not be initialized."; return result; }
    std::string id = "nucleus-" + identity;
    auto existing = std::find_if(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
      return item.source_kind == "nucleus_vault" && item.source_id == variant.source_id;
    });
    if (existing != next_assets.end()) {
      if (existing->info.sha256 != digest || existing->info.target_path != variant.dat.target_path) {
        result.message = "Nucleus skin id conflicts with an existing catalog asset: " + variant.source_id;
        return result;
      }
      writes.push_back({g_root / fs::u8path(existing->stored_path), &variant.bytes});
      ++refreshed; touched_targets[variant.dat.target_path] = true;
      if (result.asset_id.empty()) result.asset_id = existing->info.id;
      continue;
    }
    if (std::any_of(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
          return item.info.id == id;
        })) {
      result.message = "A generated Nucleus asset identity conflicts with the existing catalog.";
      return result;
    }
    AssetRecord asset;
    asset.info.id = id;
    asset.info.name = variant.display_name;
    if (std::any_of(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
          return item.info.target_path == variant.dat.target_path && item.info.name == asset.info.name;
        })) asset.info.name += " (" + variant.source_id + ")";
    asset.info.kind = "character_costume";
    asset.info.target_path = variant.dat.target_path;
    asset.info.character = variant.dat.character;
    asset.info.costume = variant.dat.costume;
    asset.info.sha256 = digest;
    asset.info.roots = variant.dat.roots;
    asset.info.dependencies = variant.dependencies;
    asset.info.unsupported_companions = variant.companion_notices;
    asset.info.available = true;
    asset.stored_path = (fs::path(L"assets") / fs::u8path(id) /
                         fs::u8path(variant.dat.target_path)).generic_u8string();
    asset.source_kind = "nucleus_vault";
    asset.source_name = path_filename_utf8(source);
    asset.source_member = variant.archive_member + "::" + variant.dat_member;
    asset.source_id = variant.source_id;
    writes.push_back({g_root / fs::u8path(asset.stored_path), &variant.bytes});
    for (auto& companion : variant.companions) {
      std::string companion_digest = sha256(companion.bytes);
      if (companion_digest.empty()) {
        result.message = "A Nucleus companion could not be hashed."; return result;
      }
      std::string filename = companion.kind == "csp" ? "csp.png" : "stock.png";
      AssetRecord::Companion record;
      record.kind = companion.kind;
      record.stored_path = (fs::path(L"assets") / fs::u8path(id) / L"companions" /
                            fs::u8path(filename)).generic_u8string();
      record.sha256 = companion_digest;
      record.source_member = companion.source_member;
      asset.info.unsupported_companions.push_back(
          (companion.kind == "csp" ? "CSP" : "Stock icon") +
          std::string(" mapped to the native selector texture with vanilla fallback: ") +
          companion.source_member);
      asset.companions.push_back(record);
      writes.push_back({g_root / fs::u8path(record.stored_path), &companion.bytes});
    }
    next_assets.push_back(std::move(asset));
    touched_targets[variant.dat.target_path] = true;
    if (result.asset_id.empty()) result.asset_id = id;
    ++added;
  }

  size_t resource_added = 0, resource_refreshed = 0;
  for (auto& resource : plan.resources) {
    std::string digest = sha256(resource.bytes);
    if (digest.empty()) { result.message = "A Nucleus resource could not be hashed."; return result; }
    std::string identity = sha256_text(resource.source_id + "\n" + resource.target_path + "\n" + digest);
    if (identity.empty()) { result.message = "A Nucleus resource identity could not be hashed."; return result; }
    std::string id = (resource_source_kind == "nucleus_project" ? "nucleus-resource-" :
                      resource_source_kind == "pause_screen" ? "pause-" :
                      resource_source_kind == "standalone_effect" ? "effect-" : "stage-") + identity;
    auto existing = std::find_if(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
      return item.source_kind == resource_source_kind && item.source_id == resource.source_id;
    });
    if (existing != next_assets.end()) {
      if (existing->info.sha256 != digest || existing->info.target_path != resource.target_path ||
          existing->info.kind != resource.kind) {
        result.message = "Nucleus resource id conflicts with an existing catalog asset: " + resource.source_id;
        return result;
      }
      writes.push_back({g_root / fs::u8path(existing->stored_path), &resource.bytes});
      ++resource_refreshed;
      id = existing->info.id;
    } else {
      AssetRecord asset;
      asset.info.id = id; asset.info.name = resource.display_name; asset.info.kind = resource.kind;
      asset.info.target_path = resource.target_path; asset.info.character = resource.group;
      asset.info.costume = resource.slot; asset.info.sha256 = digest; asset.info.available = true;
      asset.info.availability_message = resource.kind == "stage_visual" ?
          "Exact-ISO visual validation controls online use; full replacements are offline only." :
          "Validated against the exact clean ISO when selected or launched.";
      asset.stored_path = (fs::path(L"assets") / fs::u8path(id) /
                           fs::u8path(resource.target_path)).generic_u8string();
      asset.source_kind = resource_source_kind; asset.source_name = path_filename_utf8(source);
      asset.source_member = resource.source_member; asset.source_id = resource.source_id;
      writes.push_back({g_root / fs::u8path(asset.stored_path), &resource.bytes});
      next_assets.push_back(std::move(asset));
      ++resource_added;
    }
    // An effect file on its own names the asset it made (or found already), as a costume import does.
    if (resource_source_kind == "standalone_effect" && result.asset_id.empty()) result.asset_id = id;
    auto recorded = std::find_if(next_assets.begin(), next_assets.end(),
                                 [&](const AssetRecord& item) { return item.info.id == id; });
    for (auto& companion : resource.companions) {
      AssetRecord::Companion record;
      record.kind = companion.kind;
      record.sha256 = sha256(companion.bytes);
      record.source_member = companion.source_member;
      record.stored_path = (fs::path(L"assets") / fs::u8path(id) / L"companions" /
                            fs::u8path(companion.kind + ".png")).generic_u8string();
      auto previous = std::find_if(recorded->companions.begin(), recorded->companions.end(),
                                   [&](const AssetRecord::Companion& item) {
                                     return item.kind == record.kind;
                                   });
      if (previous == recorded->companions.end()) recorded->companions.push_back(record);
      else *previous = record;
      writes.push_back({g_root / fs::u8path(record.stored_path), &companion.bytes});
    }
    std::string visual_error, classification;
    std::vector<uint8_t> effect_runtime;
    bool validated_now = resource.kind == "stage_visual" || resource.kind == "pause_screen" ||
        materialize_effect_with_open_disc(resource.target_path, resource.bytes, &effect_runtime,
                                          &classification, &visual_error);
    const std::string key = selection_key(*recorded);
    touched_targets[key] = touched_targets[key] || validated_now;
  }

  bool profile_changed = false;
  // A standalone stage import is an explicit choice, including when another
  // variant or Vanilla was already selected. Vault refreshes keep existing choices.
  // So is an effect file on its own: of a file the slot table covers, it becomes the source of the slots
  // it changes (cosmetic_effect_slots.cpp, below, once it is in the catalog); of any other, the only
  // effect of its disc file, the other moves chosen for that file going back to the disc's own (it
  // replaces the whole file, which would not merge).
  if (resource_source_kind == "standalone_stage" || (resource_source_kind == "standalone_effect" && select_effects)) {
    for (const auto& resource : plan.resources) {
      auto chosen = std::find_if(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
        return item.source_id == resource.source_id && item.info.target_path == resource.target_path;
      });
      if (chosen == next_assets.end()) continue;
      if (chosen->info.kind == "effect_visual") {
        if (!next_profile.enabled) { next_profile.enabled = true; profile_changed = true; }
        continue;
      }
      const std::string key = selection_key(*chosen);
      // A skin of one of the six legal stages joins that stage's pool, which picks at every match
      // (cosmetic_stage_pools.cpp): no saved choice, no restart. (A Stadium transformation is chosen as
      // before; one of a Stadium skin's set stops being a choice of its own when the set is recorded.)
      if (chosen->info.kind == "stage_visual" && pool_stage_of(key)) {
        if (!next_profile.enabled) { next_profile.enabled = true; profile_changed = true; }
        continue;
      }
      if (next_profile.selections[key] != chosen->info.id) {
        next_profile.selections[key] = chosen->info.id; profile_changed = true;
      }
      if (!next_profile.enabled) { next_profile.enabled = true; profile_changed = true; }
    }
  }
  for (const auto& touched : touched_targets) {
    if (!touched.second) continue;
    if (resource_source_kind == "standalone_effect") break;   // chosen below, or (select_effects off) not at all
    if (pool_stage_of(touched.first)) continue;   // a stage pool's
    if (next_profile.selections.find(touched.first) != next_profile.selections.end()) continue;
    auto matches = std::count_if(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
      return selection_key(item) == touched.first;
    });
    if (matches == 1) {
      auto only = std::find_if(next_assets.begin(), next_assets.end(), [&](const AssetRecord& item) {
        return selection_key(item) == touched.first;
      });
      next_profile.selections[touched.first] = only->info.id;
      profile_changed = true;
    }
  }
  if (profile_changed) ++next_profile.generation;

  for (const auto& pending : writes) {
    std::error_code ec;
    bool existed = fs::exists(pending.path, ec);
    if (ec || !write_atomic(pending.path, pending.bytes->data(), pending.bytes->size(), &error)) {
      for (const auto& created : newly_created) DeleteFileW(created.c_str());
      result.message = ec ? "A Nucleus asset destination could not be inspected." : error;
      return result;
    }
    if (!existed) newly_created.push_back(pending.path);
  }
  auto previous_assets = std::move(g_assets);
  Profile previous_profile = std::move(g_profile);
  g_assets = std::move(next_assets); g_profile = std::move(next_profile);
  // A standalone effect chosen: its slots (its file is in the catalog now), else its file as one choice.
  if (resource_source_kind == "standalone_effect" && select_effects)
    for (const auto& resource : plan.resources) {
      const auto chosen = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
        return item.source_kind == resource_source_kind && item.source_id == resource.source_id;
      });
      if (chosen == g_assets.end()) continue;
      std::string why;
      if (!assign_effect_locked(&g_profile, *chosen, &why)) {
        const std::string key = selection_key(*chosen);
        g_profile.selections[key] = chosen->info.id;
        for (auto& pick : g_profile.selections)
          if (pick.first != key && pick.second != kVanillaSelection &&
              pick.first.rfind(chosen->info.target_path + "#", 0) == 0) {
            host::log("cosmetics: %s takes %s's place in %s", chosen->info.name.c_str(), pick.first.c_str(),
                      chosen->info.target_path.c_str());
            pick.second = kVanillaSelection;
          }
      }
      ++g_profile.generation;
    }
  if (!save_state_locked(&error)) {
    g_assets = std::move(previous_assets); g_profile = std::move(previous_profile);
    for (const auto& created : newly_created) DeleteFileW(created.c_str());
    result.message = error; return result;
  }
  result.ok = true; result.already_present = added == 0;
  result.already_present = result.already_present && resource_added == 0;
  if (resource_source_kind == "pause_screen") {
    result.message = std::to_string(resource_added) + " new / " + std::to_string(resource_refreshed) +
                     " existing pause screens imported. Choose a style under Mods, Pause screen, then restart.";
    g_message = result.message;
    return result;
  }
  if (resource_source_kind == "standalone_effect") {
    result.message = "Effect for " + (plan.resources.empty() ? std::string("?") : plan.resources[0].target_path) +
                     (result.already_present ? " was installed already." : " installed.") +
                     " It is used from the next start.";
    g_message = result.message;
    return result;
  }
  result.message = (resource_source_kind == "nucleus_project" ?
                    "Nucleus project committed atomically: " : "Stage import committed atomically: ") +
                   std::to_string(added) +
                   " new / " + std::to_string(refreshed) + " existing character variants, " +
                   std::to_string(resource_added) + " new / " + std::to_string(resource_refreshed) +
                   " existing stage/effect resources; " +
                   std::to_string(plan.rejected_resources.size()) +
                   " structurally unsupported records quarantined. Effects require exact-ISO materialization; stage visuals are checked before online use.";
  return result;
}

}  // namespace cosmetic_internal
}  // namespace host::cosmetics
