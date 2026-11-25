// The cosmetic catalog and profile on disk: the state file read and written, the runtime
// copies of installed files, configure().
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

void load_companions(const json& item, AssetRecord* asset) {
  auto found = item.find("companions");
  if (found == item.end()) return;
  if (!found->is_array()) throw std::runtime_error("companions");
  for (const auto& value : *found) {
    AssetRecord::Companion companion;
    companion.kind = value.at("kind").get<std::string>();
    companion.stored_path = value.at("stored_path").get<std::string>();
    companion.sha256 = value.at("sha256").get<std::string>();
    companion.source_member = value.value("source_member", std::string());
    if ((companion.kind != "csp" && companion.kind != "stock" && companion.kind != "preview") ||
        !safe_relative_path(companion.stored_path) || companion.sha256.size() != 64)
      throw std::runtime_error("companion");
    asset->companions.push_back(std::move(companion));
  }
  // Separate optional field: older builds ignore it instead of rejecting every catalog entry
  // because their known companion kinds predate pause-screen imports.
  const auto label = item.find("pause_text");
  if (label != item.end() && label->is_object() && asset->info.kind == "pause_screen") {
    AssetRecord::Companion companion;
    companion.kind = "pause_text";
    companion.stored_path = label->value("stored_path", std::string());
    companion.sha256 = label->value("sha256", std::string());
    companion.source_member = label->value("source_member", std::string());
    if (!safe_relative_path(companion.stored_path) || companion.sha256.size() != 64)
      throw std::runtime_error("pause text");
    asset->companions.push_back(std::move(companion));
  }
}

// A skin's sound bank is its own "voice" object, not one of "companions": a build that predates
// voice mods refuses a catalog with a companion kind it does not know, and ignores a key it does not
// know. A malformed entry is dropped (the skin stays); it never invalidates the catalog.
void load_voice(const json& item, AssetRecord* asset) {
  const auto found = item.find("voice");
  if (found == item.end() || !found->is_object()) return;
  AssetRecord::Voice voice;
  voice.stored_path = found->value("stored_path", std::string());
  voice.sha256 = found->value("sha256", std::string());
  voice.source_member = found->value("source_member", std::string());
  voice.bank = found->value("bank", std::string());
  if (!safe_relative_path(voice.stored_path) || voice.sha256.size() != 64 || voice.bank.empty() ||
      voice.bank.find('/') != std::string::npos || voice.bank.find('\\') != std::string::npos) return;
  asset->voice = std::move(voice);
  asset->info.voice = asset->voice.bank;
  asset->info.voice_source = asset->voice.source_member;
}

// The public listing says where a skin comes from; a disc-backed record also says where its bytes are.
void load_disc_fields(const json& item, AssetRecord* asset) {
  asset->info.source = asset->source_kind == kDiscSource ? "disc" : "import";
  if (asset->source_kind != kDiscSource) return;
  const json& disc = item.at("disc");
  asset->disc_path = disc.at("path").get<std::string>();
  asset->disc_offset = disc.at("offset").get<uint64_t>();
  asset->disc_length = disc.at("length").get<uint64_t>();
  asset->disc_size = disc.value("size", (uint64_t)0);
  asset->disc_mtime = disc.value("mtime", (int64_t)0);
  asset->info.source_name = asset->source_name;
  asset->info.disc_path = asset->disc_path;
  if (asset->disc_path.empty() || !asset->disc_length || asset->disc_length > kMaxAssetBytes)
    throw std::runtime_error("disc");
  asset->info.variant = item.value("variant", std::string());
  const auto aliases = item.find("costume_aliases");
  if (aliases != item.end()) {
    asset->costume_aliases = aliases->get<std::map<std::string, std::string>>();
    if (asset->costume_aliases.size() > 2) throw std::runtime_error("costume aliases");
    for (const auto& alias : asset->costume_aliases)
      if (alias.first.empty() || alias.second.empty() || alias.first.size() > 255 || alias.second.size() > 255)
        throw std::runtime_error("costume alias");
  }
  const auto online = item.find("online");
  // A verdict saved under an older online rule is worked out again (kOnlineRule).
  if (online != item.end() && online->is_object() && online->value("rule", 0) == kOnlineRule) {
    asset->online_known = true;
    asset->online_ok = online->value("ok", false);
    asset->online_note = online->value("note", std::string());
  }
}

// Which set of a pack a record belongs to ("" for the plain costume, "alt L" and "alt R" for the
// 20XX style alternates), and which pack: imports are one pack, each scanned disc or folder another.
std::string pack_key(const AssetRecord& asset) { return asset.source_kind == kDiscSource ? asset.source_id : kImportPack; }

json catalog_json_locked() {
  json root;
  root["schema_version"] = kCatalogSchema;
  root["assets"] = json::array();
  for (const auto& asset : g_assets) {
    json item;
    item["id"] = asset.info.id;
    item["name"] = asset.info.name;
    item["kind"] = asset.info.kind;
    item["target_path"] = asset.info.target_path;
    item["character"] = asset.info.character;
    item["costume"] = asset.info.costume;
    item["sha256"] = asset.info.sha256;
    item["stored_path"] = asset.stored_path;
    item["roots"] = string_array(asset.info.roots);
    item["dependencies"] = string_array(asset.info.dependencies);
    item["unsupported_companions"] = string_array(asset.info.unsupported_companions);
    item["source"] = {{"kind", asset.source_kind}, {"name", asset.source_name},
                       {"member", asset.source_member}, {"id", asset.source_id}};
    if (asset.source_kind == kDiscSource)
      item["disc"] = {{"path", asset.disc_path}, {"offset", asset.disc_offset}, {"length", asset.disc_length},
                      {"size", asset.disc_size}, {"mtime", asset.disc_mtime}};
    if (!asset.info.variant.empty()) item["variant"] = asset.info.variant;
    if (!asset.costume_aliases.empty()) item["costume_aliases"] = asset.costume_aliases;
    if (asset.online_known) item["online"] = {{"ok", asset.online_ok}, {"note", asset.online_note}, {"rule", kOnlineRule}};
    item["companions"] = json::array();
    for (const auto& companion : asset.companions) {
      if (companion.kind == "pause_text") {
        item["pause_text"] = {{"stored_path", companion.stored_path}, {"sha256", companion.sha256},
                              {"source_member", companion.source_member}};
        continue;
      }
      item["companions"].push_back({{"kind", companion.kind},
                                      {"stored_path", companion.stored_path},
                                      {"sha256", companion.sha256},
                                      {"source_member", companion.source_member},
                                      {"status", "native_texture_override"}});
    }
    if (!asset.voice.stored_path.empty())
      item["voice"] = {{"stored_path", asset.voice.stored_path}, {"sha256", asset.voice.sha256},
                       {"source_member", asset.voice.source_member}, {"bank", asset.voice.bank}};
    root["assets"].push_back(std::move(item));
  }
  return root;
}

// Optional in the file: a profile written before the switch existed has every pack in the pick.
void load_stage_packs_off(const json& profile, Profile* out) {
  const auto off = profile.find("stage_packs_off");
  if (off == profile.end() || !off->is_array()) return;
  for (const auto& pack : *off) if (pack.is_string()) out->stage_packs_off[pack.get<std::string>()] = true;
}

// Optional in the file too: the stage pools' switched-off standard stages and Stadium skins' sets of
// transformations (cosmetic_stage_pools.cpp). A malformed entry is left out, never the profile.
void load_stage_pools(const json& profile, Profile* out) {
  const auto off = profile.find("stage_pool_vanilla_off");
  if (off != profile.end() && off->is_array())
    for (const auto& stage : *off)
      if (stage.is_string() && stage.get<std::string>().size() < 64) out->pool_vanilla_off[stage.get<std::string>()] = true;
  const auto sets = profile.find("stadium_sets");
  if (sets == profile.end() || !sets->is_object()) return;
  for (auto it = sets->begin(); it != sets->end(); ++it) {
    if (!it.value().is_array() || it.value().size() != 4) continue;
    std::vector<std::string> forms;
    for (const auto& form : it.value()) if (form.is_string()) forms.push_back(form.get<std::string>());
    if (forms.size() == 4) out->stadium_sets[it.key()] = std::move(forms);
  }
}

json profile_json_locked() {
  json root;
  root["schema_version"] = kProfileSchema;
  root["name"] = "Default";
  root["enabled"] = g_profile.enabled;
  root["random_stage_skins"] = g_profile.random_stage_skins;
  if (!g_profile.stage_packs_off.empty()) {
    root["stage_packs_off"] = json::array();
    for (const auto& pack : g_profile.stage_packs_off) root["stage_packs_off"].push_back(pack.first);
  }
  if (!g_profile.pool_vanilla_off.empty()) {
    root["stage_pool_vanilla_off"] = json::array();
    for (const auto& stage : g_profile.pool_vanilla_off) root["stage_pool_vanilla_off"].push_back(stage.first);
  }
  if (!g_profile.stadium_sets.empty()) {
    root["stadium_sets"] = json::object();
    for (const auto& set : g_profile.stadium_sets) root["stadium_sets"][set.first] = set.second;
  }
  root["generation"] = g_profile.generation;
  root["selections"] = json::object();
  for (const auto& pick : g_profile.selections) root["selections"][pick.first] = pick.second;
  if (!g_profile.effect_slots.empty()) root["effect_slots"] = effect_slots_json(g_profile);
  return root;
}

bool save_state_locked(std::string* error) {
  json state;
  state["schema_version"] = kStateSchema;
  state["catalog"] = catalog_json_locked();
  state["profile"] = profile_json_locked();
  state["disc_scans"] = json::array();
  for (const auto& scan : g_disc_scans)
    state["disc_scans"].push_back({{"path", scan.path}, {"size", scan.size}, {"mtime", scan.mtime}, {"rules", scan.rules}});
  if (!write_atomic(g_root / L"state.json", state.dump(2) + "\n", error)) return false;
  // These readable mirrors preserve compatibility with older builds and diagnostics. state.json is
  // authoritative because it publishes catalog and profile together in one atomic replacement.
  std::string ignored;
  write_atomic(g_root / L"catalog.json", catalog_json_locked().dump(2) + "\n", &ignored);
  write_atomic(g_root / L"profile.json", profile_json_locked().dump(2) + "\n", &ignored);
  return true;
}
bool save_catalog_locked(std::string* error) { return save_state_locked(error); }
bool save_profile_locked(std::string* error) { return save_state_locked(error); }

std::string desired_fingerprint_locked() {
  std::string source = g_profile.enabled ? "enabled\n" : "disabled\n";
  for (const auto& pick : g_profile.selections) {
    if (pick.second == kVanillaSelection) {
      source += pick.first + "=" + kVanillaSelection + "\n";
      continue;
    }
    auto found = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) {
      return a.info.id == pick.second && selection_key(a) == pick.first;
    });
    if (found != g_assets.end()) {
      source += pick.first + "=" + pick.second + "=" + found->info.sha256;
      for (const auto& companion : found->companions) source += "+" + companion.kind + ":" + companion.sha256;
      if (!found->voice.stored_path.empty()) source += "+voice:" + found->voice.sha256;
      source += "\n";
    }
  }
  return sha256_text(source);
}

bool load_json_file(const fs::path& path, json* out, bool* exists, std::string* error) {
  std::error_code ec;
  *exists = fs::exists(path, ec);
  if (ec || !*exists) return !ec;
  uint64_t size = fs::file_size(path, ec);
  if (ec || size > 16ull * 1024 * 1024) {
    *error = path_filename_utf8(path) + " exceeds the 16 MB metadata safety limit."; return false;
  }
  std::ifstream file(path);
  if (!file) { *error = "Cannot open " + path_filename_utf8(path) + "."; return false; }
  try { file >> *out; }
  catch (...) { *error = path_filename_utf8(path) + " is not valid JSON."; return false; }
  return true;
}

void load_catalog_locked() {
  g_assets.clear(); g_catalog_valid = true;
  json root; bool exists = false; std::string error;
  if (!load_json_file(g_root / L"catalog.json", &root, &exists, &error)) {
    g_catalog_valid = false; g_message = error; return;
  }
  if (!exists) return;
  if (!root.is_object() || root.value("schema_version", 0u) != kCatalogSchema ||
      !root["assets"].is_array()) {
    g_catalog_valid = false; g_message = "catalog.json has an unsupported or malformed schema."; return;
  }
  try {
    for (const auto& item : root["assets"]) {
      AssetRecord asset;
      asset.info.id = item.at("id").get<std::string>();
      asset.info.name = item.at("name").get<std::string>();
      asset.info.kind = item.at("kind").get<std::string>();
      asset.info.target_path = item.at("target_path").get<std::string>();
      asset.info.character = item.value("character", std::string());
      asset.info.costume = item.value("costume", std::string());
      asset.info.sha256 = item.at("sha256").get<std::string>();
      asset.stored_path = item.at("stored_path").get<std::string>();
      asset.info.roots = get_string_array(item, "roots");
      asset.info.dependencies = get_string_array(item, "dependencies");
      asset.info.unsupported_companions = get_string_array(item, "unsupported_companions");
      load_companions(item, &asset);
      load_voice(item, &asset);
      if (item.find("source") != item.end() && item["source"].is_object()) {
        asset.source_kind = item["source"].value("kind", std::string());
        asset.source_name = item["source"].value("name", std::string());
        asset.source_member = item["source"].value("member", std::string());
        asset.source_id = item["source"].value("id", std::string());
      }
      load_disc_fields(item, &asset);
      if (asset.info.id.empty() || asset.info.name.empty() || asset.info.sha256.size() != 64 ||
          !safe_relative_path(asset.stored_path) || asset.info.target_path.find('/') != std::string::npos ||
          asset.info.target_path.find('\\') != std::string::npos)
        throw std::runtime_error("invalid asset");
      g_assets.push_back(std::move(asset));
    }
  } catch (...) {
    g_assets.clear(); g_catalog_valid = false;
    g_message = "catalog.json contains an invalid asset record; it was not modified.";
  }
}

void load_profile_locked() {
  g_profile = Profile{}; g_profile_valid = true;
  json root; bool exists = false; std::string error;
  if (!load_json_file(g_root / L"profile.json", &root, &exists, &error)) {
    g_profile_valid = false; g_message = error; return;
  }
  if (!exists) return;
  try {
    if (!root.is_object() || root.value("schema_version", 0u) != kProfileSchema ||
        !root["selections"].is_object()) throw std::runtime_error("schema");
    g_profile.enabled = root.value("enabled", true);
    g_profile.random_stage_skins = root.value("random_stage_skins", false);
    load_stage_packs_off(root, &g_profile);
    load_stage_pools(root, &g_profile);
    load_effect_slots(root, &g_profile);
    g_profile.generation = std::max<uint64_t>(1, root.value("generation", 1ull));
    for (auto it = root["selections"].begin(); it != root["selections"].end(); ++it) {
      if (!it.value().is_string() || it.key().find('/') != std::string::npos ||
          it.key().find('\\') != std::string::npos) throw std::runtime_error("selection");
      g_profile.selections[it.key()] = it.value().get<std::string>();
    }
  } catch (...) {
    g_profile = Profile{}; g_profile_valid = false;
    g_message = "profile.json has an unsupported or malformed schema; it was not modified.";
  }
}

bool ready_locked(std::string* error) {
  if (!g_configured) { *error = "The cosmetic catalog has not been configured."; return false; }
  if (!g_catalog_valid || !g_profile_valid) { *error = g_message; return false; }
  return true;
}

bool load_state_locked(bool* exists) {
  g_assets.clear(); g_disc_scans.clear(); g_profile = Profile{}; g_catalog_valid = g_profile_valid = true;
  json state; std::string error;
  if (!load_json_file(g_root / L"state.json", &state, exists, &error)) {
    g_assets.clear(); g_profile = Profile{}; g_catalog_valid = g_profile_valid = false;
    g_message = error; return false;
  }
  if (!*exists) return true;
  try {
    if (!state.is_object() || state.value("schema_version", 0u) != kStateSchema ||
        !state.at("catalog").is_object() || !state.at("profile").is_object())
      throw std::runtime_error("schema");
    const json& catalog = state.at("catalog");
    const json& profile = state.at("profile");
    if (catalog.value("schema_version", 0u) != kCatalogSchema ||
        !catalog.at("assets").is_array() ||
        profile.value("schema_version", 0u) != kProfileSchema ||
        !profile.at("selections").is_object())
      throw std::runtime_error("nested schema");
    std::vector<AssetRecord> assets;
    for (const auto& item : catalog.at("assets")) {
      AssetRecord asset;
      asset.info.id = item.at("id").get<std::string>();
      asset.info.name = item.at("name").get<std::string>();
      asset.info.kind = item.at("kind").get<std::string>();
      asset.info.target_path = item.at("target_path").get<std::string>();
      asset.info.character = item.value("character", std::string());
      asset.info.costume = item.value("costume", std::string());
      asset.info.sha256 = item.at("sha256").get<std::string>();
      asset.stored_path = item.at("stored_path").get<std::string>();
      asset.info.roots = get_string_array(item, "roots");
      asset.info.dependencies = get_string_array(item, "dependencies");
      asset.info.unsupported_companions = get_string_array(item, "unsupported_companions");
      load_companions(item, &asset);
      load_voice(item, &asset);
      if (item.find("source") != item.end() && item["source"].is_object()) {
        asset.source_kind = item["source"].value("kind", std::string());
        asset.source_name = item["source"].value("name", std::string());
        asset.source_member = item["source"].value("member", std::string());
        asset.source_id = item["source"].value("id", std::string());
      }
      load_disc_fields(item, &asset);
      if (asset.info.id.empty() || asset.info.name.empty() || asset.info.sha256.size() != 64 ||
          !safe_relative_path(asset.stored_path) || asset.info.target_path.find('/') != std::string::npos ||
          asset.info.target_path.find('\\') != std::string::npos)
        throw std::runtime_error("asset");
      assets.push_back(std::move(asset));
    }
    Profile next;
    next.enabled = profile.value("enabled", true);
    next.random_stage_skins = profile.value("random_stage_skins", false);
    load_stage_packs_off(profile, &next);
    load_stage_pools(profile, &next);
    load_effect_slots(profile, &next);
    next.generation = std::max<uint64_t>(1, profile.value("generation", 1ull));
    for (auto it = profile.at("selections").begin(); it != profile.at("selections").end(); ++it) {
      if (!it.value().is_string() || it.key().find('/') != std::string::npos ||
          it.key().find('\\') != std::string::npos) throw std::runtime_error("selection");
      next.selections[it.key()] = it.value().get<std::string>();
    }
    std::vector<DiscScan> scans;
    const auto scan_list = state.find("disc_scans");
    if (scan_list != state.end() && scan_list->is_array())
      for (const auto& scan : *scan_list)
        scans.push_back({scan.at("path").get<std::string>(), scan.value("size", (uint64_t)0),
                         scan.value("mtime", (int64_t)0), scan.value("rules", 1u)});
    g_assets = std::move(assets); g_profile = std::move(next); g_disc_scans = std::move(scans);
    g_catalog_valid = g_profile_valid = true;
    return true;
  } catch (...) {
    g_assets.clear(); g_profile = Profile{}; g_catalog_valid = g_profile_valid = false;
    g_message = "state.json has an unsupported or malformed schema; it was not modified.";
    return false;
  }
}

bool mutable_profile_locked(std::string* error) {
  if (!ready_locked(error)) return false;
  if (g_online_freezes.load(std::memory_order_relaxed)) {
    *error = "The cosmetic profile is frozen while an online session is queued or active.";
    return false;
  }
  return true;
}

std::string selection_key(const AssetRecord& asset) {
  return asset.info.target_path + (asset.info.kind == "effect_visual" ? "#" + effect_scope(asset) :
                                   asset.info.kind == "fighter_sound" ? "#sounds" : "");
}

std::vector<uint8_t> load_runtime_asset_locked(const AssetRecord& asset, std::string* error) {
  std::vector<uint8_t> bytes;
  if (asset.source_kind == kDiscSource) {
    // Read from the disc itself, and never trust it unseen: the hash is what the scan recorded.
    if (!read_range(fs::u8path(asset.disc_path), asset.disc_offset, asset.disc_length, &bytes) ||
        sha256(bytes) != asset.info.sha256) {
      *error = kDiscChanged; return {};
    }
  } else {
    if (!safe_relative_path(asset.stored_path) ||
        !read_bounded(g_root / fs::u8path(asset.stored_path), kMaxAssetBytes, &bytes, error)) return {};
    if (sha256(bytes) != asset.info.sha256) {
      *error = asset.info.name + ": stored DAT hash no longer matches the catalog."; return {};
    }
  }
  if (asset.info.kind == "character_costume") {
    if (!asset.costume_aliases.empty()) {
      std::vector<uint8_t> renamed;
      if (!disc_metadata::alias_roots(bytes, asset.costume_aliases, &renamed)) {
        *error = asset.info.name + ": costume export names no longer match the catalog."; return {};
      }
      bytes = std::move(renamed);
    }
    auto dat = inspect_dat_impl(bytes);
    if (!dat.ok || dat.target_path != asset.info.target_path) {
      *error = asset.info.name + ": stored DAT identity no longer matches its catalog target."; return {};
    }
    // Every way a costume reaches the game passes here, so one that the game cannot draw is
    // refused offline too and the slot keeps the standard costume.
    std::string defect;
    if (!costume_draw_safe(bytes, &defect)) { *error = asset.info.name + ": " + defect; return {}; }
    std::vector<uint8_t> standard;
    if (!costume_structure_safe(bytes, standard_costume(asset.info.target_path, &standard), &defect)) {
      *error = asset.info.name + ": " + defect + "."; return {};
    }
  } else if (asset.info.kind == "fighter_sound") {
    size_t kind = 0; const char* character = nullptr;
    uint32_t offset = 0, size = 0;
    std::vector<uint8_t> clean;
    if (!fighter_sound_identity(asset.info.target_path, &kind, &character) ||
        lower(character) != lower(asset.info.character) ||
        !host::disc_find_file(asset.info.target_path, &offset, &size) || !size || size > kMaxAssetBytes) {
      *error = asset.info.name + ": the matching fighter archive is not on the active disc."; return {};
    }
    clean.resize(size);
    std::vector<uint8_t> safe;
    if (!host::disc_read(offset, clean.data(), size) || !fighter_sound_overlay(clean, bytes, kind, &safe, error)) {
      if (error->empty()) *error = asset.info.name + ": sound-only validation failed.";
      return {};
    }
    bytes = std::move(safe);
  } else if (asset.info.kind == "pause_screen") {
    if (asset.info.target_path != "GmPause.usd" || !pause_screen_valid(bytes, error)) return {};
  } else if (asset.info.kind == "stage_visual" || asset.info.kind == "effect_visual") {
    VisualLayout layout;
    if (!parse_visual_layout(bytes, &layout, error)) {
      *error = asset.info.name + ": " + *error; return {};
    }
    // A stage the game cannot draw is refused like a costume is: the stage keeps the disc's own file.
    std::string defect;
    if (asset.info.kind == "stage_visual" && !stage_draw_safe(bytes, &defect)) { *error = asset.info.name + ": " + defect; return {}; }
    if (asset.info.kind == "stage_visual" && !stage_structure_safe(bytes, &defect)) {
      *error = asset.info.name + ": " + defect + "."; return {};
    }
  } else if (asset.info.kind == kPortraitKind) {
    if (!png_companion(bytes, error)) { *error = asset.info.name + ": " + *error; return {}; }
  } else {
    *error = asset.info.name + ": unsupported catalog resource kind."; return {};
  }
  return bytes;
}

bool refresh_assets_locked(bool prune_invalid_selections, std::string* error) {
  std::vector<std::string> invalid_selected;
  for (auto& asset : g_assets) {
    std::string validation_error;
    if (asset.source_kind == kDiscSource) {
      // Startup only asks whether the disc is still the file that was scanned. Hashing every skin of
      // every disc here would read tens of megabytes on each launch; the hash is checked when a
      // skin is selected and when it is applied.
      uint64_t size = 0; int64_t mtime = 0;
      asset.info.available = file_stamp(fs::u8path(asset.disc_path), &size, &mtime) &&
                             size == asset.disc_size && mtime == asset.disc_mtime;
      validation_error = kDiscChanged;
    } else {
      asset.info.available = !load_runtime_asset_locked(asset, &validation_error).empty();
    }
    asset.info.availability_message = asset.info.available ?
        (asset.info.kind == "character_costume" || asset.info.kind == kPortraitKind || asset.info.kind == "fighter_sound" ||
         asset.info.kind == "pause_screen" ? std::string() :
         asset.info.kind == "stage_visual" ?
             "Stage replacement is offline unless exact-ISO validation proves texture-only changes." :
             "Exact clean-ISO effect validation and safe materialization are required when selected and repeated at every launch.") :
        validation_error;
    auto selected = g_profile.selections.find(selection_key(asset));
    if (!asset.info.available && selected != g_profile.selections.end() &&
        selected->second == asset.info.id)
      invalid_selected.push_back(selection_key(asset));
  }
  for (const auto& selected : g_profile.selections) {
    if (selected.second == kVanillaSelection) continue;
    auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == selected.second && selection_key(item) == selected.first;
    });
    if (asset == g_assets.end()) invalid_selected.push_back(selected.first);
  }
  if (!prune_invalid_selections) return true;
  Profile previous = g_profile;
  std::sort(invalid_selected.begin(), invalid_selected.end());
  invalid_selected.erase(std::unique(invalid_selected.begin(), invalid_selected.end()),
                         invalid_selected.end());
  for (const auto& target : invalid_selected) g_profile.selections[target] = kVanillaSelection;
  // Effect slots of effects gone or unreadable: the disc's own; each file's selection follows.
  prune_effect_slots_locked(&g_profile);
  if (g_profile.selections == previous.selections && g_profile.effect_slots == previous.effect_slots) return true;
  ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_message = std::to_string(invalid_selected.size()) +
              " invalid selection(s) fell back to Vanilla.";
  return true;
}

}  // namespace cosmetic_internal

void configure(const std::string& settings_path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  fs::path settings = fs::u8path(settings_path);
  fs::path parent = settings.parent_path();
  if (parent.empty()) parent = fs::current_path();
  fs::path root = parent / L"CosmeticMods";
  g_root = std::move(root); g_configured = true; g_message.clear();
  bool state_exists = false;
  load_state_locked(&state_exists);
  if (!state_exists && g_catalog_valid && g_profile_valid) {
    load_catalog_locked(); load_profile_locked();
    if (g_catalog_valid && g_profile_valid) {
      std::string migration_error;
      if (!save_state_locked(&migration_error)) {
        g_catalog_valid = g_profile_valid = false; g_message = migration_error;
      }
    }
  }
  if (g_catalog_valid && g_profile_valid) {
    // Older profiles keyed all effects by their DAT. Preserve the selected move
    // when moving to independent scope keys; other moves stay vanilla.
    bool migrated_effects = false;
    for (const auto& asset : g_assets) {
      if (asset.info.kind != "effect_visual") continue;
      auto old = g_profile.selections.find(asset.info.target_path);
      if (old == g_profile.selections.end() || old->second != asset.info.id) continue;
      g_profile.selections[selection_key(asset)] = old->second;
      g_profile.selections.erase(old);
      migrated_effects = true;
    }
    if (migrated_effects) {
      ++g_profile.generation;
      std::string migration_error;
      if (!save_state_locked(&migration_error)) {
        g_catalog_valid = g_profile_valid = false;
        g_message = migration_error;
      }
    }
  }
  if (g_catalog_valid && g_profile_valid) {
    std::string error;
    if (refresh_assets_locked(true, &error)) {
      if (g_message.empty())
        g_message = g_assets.empty() ? "No cosmetic mods imported." : "Cosmetic catalog loaded.";
    } else g_message = error;
  }
}

}  // namespace host::cosmetics
