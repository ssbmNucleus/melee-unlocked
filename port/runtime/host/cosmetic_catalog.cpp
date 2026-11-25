// The cosmetic catalog's entries: the list, costume slots, online status, rename, remove,
// select, restore, the costume files and their pictures.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {

std::vector<CostumeSlot> costume_slots() {
  std::vector<CostumeSlot> out;
  for (const auto& family : families) {
    // the family's own color order, which is the game's
    std::string colors = family.colors;
    for (size_t at = 0; at < colors.size(); at += 3) {
      const std::string code = colors.substr(at, 2);
      for (const auto& color : color_names)
        if (code == color.code) out.push_back({slot_file({&family, &color}), family.display_name, color.label});
    }
  }
  return out;
}

// One verdict for startup, live picks, and status, including the drawing safety check: whether the
// player's own skin is used online (with the standard rest pose copied in, and by everything else the
// peer rule compares: costume_online_fix).
static bool costume_online_matches(const std::vector<uint8_t>& clean,
                                   const std::vector<uint8_t>& candidate, std::string* detail) {
  return costume_online_fix(clean, candidate, nullptr, detail);
}

bool costume_online_with_open_disc(const AssetRecord& asset,
                                          const std::vector<uint8_t>& candidate, std::string* detail) {
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(asset.info.target_path, &offset, &size) || !size || size > kMaxAssetBytes) {
    detail->clear(); return false;
  }
  std::vector<uint8_t> clean(size);
  if (!host::disc_read(offset, clean.data(), size)) { detail->clear(); return false; }
  return costume_online_matches(clean, candidate, detail);
}

std::string online_status_column(const std::string& asset_id, bool compare_with_open_disc) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id;
  });
  if (asset == g_assets.end() || asset->info.kind != "character_costume") return {};
  bool known = asset->online_known, allowed = asset->online_ok;
  std::string detail = asset->online_note;
  if (!known && compare_with_open_disc) {
    known = true;
    const auto candidate = load_runtime_asset_locked(*asset, &detail);
    allowed = !candidate.empty() && costume_online_with_open_disc(*asset, candidate, &detail);
    detail = online_reason_short(detail);
  }
  return cli::online_column(asset->info.kind, known, allowed, detail);
}

std::vector<AssetInfo> assets() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<AssetInfo> out;
  out.reserve(g_assets.size());
  // The online verdict of each applied choice, from the snapshot this launch runs with. A costume
  // with an English twin has two disc files: it is off online when either one is.
  const auto runtime = std::atomic_load(&g_runtime);
  std::map<std::string, const RuntimeAsset*> verdicts;
  for (const auto& entry : runtime->by_start) {
    if (entry.second.kind == kVoiceKind) continue;   // a bank file carries the skin's id and no verdict
    const RuntimeAsset*& have = verdicts[entry.second.id];
    if (!have || (have->online_allowed && !entry.second.online_allowed)) have = &entry.second;
  }
  for (const auto& record : g_assets) {
    AssetInfo info = record.info;
    info.pack = pack_key(record);
    if (info.source_name.empty()) info.source_name = record.source_name;   // an import: the file it came from
    const auto verdict = verdicts.find(info.id);
    if (verdict != verdicts.end()) {
      info.online_allowed = verdict->second->online_allowed;
      info.online_message = verdict->second->online_reason;
    } else if (record.online_known) {
      // A disc skin was judged when it was scanned, against the same standard file.
      info.online_allowed = record.online_ok;
      info.online_message = record.online_note;
    }
    bool preview_final = false;   // the first portrait or preview; else the last stock icon
    for (const auto& companion : record.companions) {
      if (companion.kind != "csp" && companion.kind != "preview" && companion.kind != "stock") continue;
      const fs::path preview = g_root / fs::u8path(companion.stored_path);
      std::error_code ec;
      if (fs::is_regular_file(preview, ec)) {
        if (companion.kind == "csp" && info.csp_path.empty()) info.csp_path = preview.u8string();
        if (companion.kind == "stock" && info.stock_path.empty()) info.stock_path = preview.u8string();
        if (!preview_final) {
          info.preview_path = preview.string();
          preview_final = companion.kind == "csp" || companion.kind == "preview";
        }
      }
    }
    if (info.kind == "effect_visual") {
      info.scope = effect_scope(record);
      info.costume = effect_label(info.scope);
    }
    auto selected = g_profile.selections.find(selection_key(record));
    info.selected = selected != g_profile.selections.end() && selected->second == info.id;
    // An effect of a file served slot by slot: the source of one of its slots.
    for (const auto& [file, sources] : g_profile.effect_slots)
      if (info.kind == "effect_visual" && lower(file) == lower(info.target_path))
        for (const auto& source : sources) info.selected = info.selected || source.second == info.id;
    out.push_back(std::move(info));
  }
  return out;
}

bool refresh_catalog(std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  uint64_t previous_generation = g_profile.generation;
  if (!refresh_assets_locked(true, error)) return false;
  if (g_profile.generation == previous_generation)
    g_message = "Cosmetic catalog refreshed; selections are valid.";
  return true;
}

bool rename_asset(const std::string& asset_id, const std::string& display_name,
                  std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  size_t begin = 0, end = display_name.size();
  while (begin < end && std::isspace((unsigned char)display_name[begin])) ++begin;
  while (end > begin && std::isspace((unsigned char)display_name[end - 1])) --end;
  std::string name = display_name.substr(begin, end - begin);
  if (name.empty() || name.size() > 96 || std::any_of(name.begin(), name.end(), [](char c) {
        return (unsigned char)c < 0x20 || c == 0x7f;
      })) {
    *error = "Display names must contain 1-96 printable characters."; return false;
  }
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id;
  });
  if (asset == g_assets.end()) { *error = "The asset is no longer in the catalog."; return false; }
  std::string previous = asset->info.name;
  asset->info.name = name;
  if (!save_catalog_locked(error)) { asset->info.name = std::move(previous); return false; }
  g_message = "Renamed cosmetic variant to " + name + ".";
  return true;
}

bool remove_asset(const std::string& asset_id, std::string* error, std::string* freed_target) {
  std::string local; if (!error) error = &local;
  if (freed_target) freed_target->clear();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id;
  });
  if (asset == g_assets.end()) { *error = "The skin is no longer installed."; return false; }
  if (asset->source_kind == kDiscSource) { *error = "That skin is part of a mod disc, not an install."; return false; }
  // Its folder is assets/<id>: an id that could name anything else is refused before any file goes.
  if (asset_id.empty() || asset_id.size() > 128 ||
      !std::all_of(asset_id.begin(), asset_id.end(), [](char c) {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
      }) || asset_id.find("..") != std::string::npos) {
    *error = "That skin's folder cannot be named safely."; return false;
  }
  const std::string key = selection_key(*asset);
  // The catalog and the profile are one saved state: the skin leaves both at once, and a slot that
  // wore it wears the standard costume (or stage) again.
  Profile previous_profile = g_profile;
  // An effect's slots go to the next installed effect that changes them, else the disc's own
  // (cosmetic_effect_slots.cpp); its file is published again.
  const bool slots_moved = release_effect_locked(&g_profile, *asset);
  if (slots_moved) ++g_profile.generation;
  const auto picked = g_profile.selections.find(key);
  const bool was_selected = slots_moved || (picked != g_profile.selections.end() && picked->second == asset_id);
  if (picked != g_profile.selections.end() && picked->second == asset_id) {
    picked->second = kVanillaSelection;
    ++g_profile.generation;
  }
  const size_t at = (size_t)(asset - g_assets.begin());
  AssetRecord removed = std::move(*asset);
  g_assets.erase(asset);
  if (!save_catalog_locked(error)) {
    g_assets.insert(g_assets.begin() + (std::ptrdiff_t)at, std::move(removed));
    g_profile = std::move(previous_profile);
    return false;
  }
  std::error_code ec;
  const fs::path folder = g_root / L"assets" / fs::u8path(asset_id);
  if (fs::is_directory(folder, ec)) fs::remove_all(folder, ec);
  if (ec) host::log("cosmetics: %s left behind (%s)", folder.u8string().c_str(), ec.message().c_str());
  if (was_selected && freed_target) *freed_target = removed.info.target_path;
  g_message = "Removed " + removed.info.name + ".";
  host::log("cosmetics: removed %s (%s, %s)%s", removed.info.name.c_str(), asset_id.c_str(), key.c_str(),
            slots_moved    ? "; its moves go to the next effect that changes them, else the standard ones"
            : was_selected ? "; its slot is the standard one again"
                           : "");
  return true;
}

bool profile_enabled() { std::lock_guard<std::mutex> lock(g_mutex); return g_profile.enabled; }

bool select_variant(const std::string& target_path, const std::string& asset_id, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id && selection_key(item) == target_path;
  });
  if (asset == g_assets.end()) { *error = "The selected variant does not belong to that slot."; return false; }
  std::string validation_error;
  std::vector<uint8_t> candidate = load_runtime_asset_locked(*asset, &validation_error);
  asset->info.available = !candidate.empty();
  if (asset->info.available && asset->info.kind == "effect_visual") {
    std::vector<uint8_t> runtime;
    std::string classification;
    asset->info.available = materialize_effect_with_open_disc(
        asset->info.target_path, candidate, &runtime, &classification, &validation_error);
    if (asset->info.available) asset->info.availability_message = classification;
  }
  if (asset->info.available && asset->info.kind == "stage_visual")
    asset->info.availability_message =
        "Stage replacement is offline unless exact-ISO validation proves texture-only changes.";
  else if (!asset->info.available)
    asset->info.availability_message = validation_error;
  if (!asset->info.available) {
    *error = "The selected variant is unavailable or invalid; Vanilla remains selected."; return false;
  }
  Profile previous = g_profile;
  g_profile.enabled = true; ++g_profile.generation;
  // A standalone effect of a file with slots: the source of the slots it changes.
  std::string why;
  if (!assign_effect_locked(&g_profile, *asset, &why)) g_profile.selections[target_path] = asset_id;
  if (asset->info.kind == "effect_visual" &&
      !compose_effects_with_open_disc_locked(asset->info.target_path, g_profile, error)) {
    g_profile = std::move(previous);
    return false;
  }
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_message = asset->info.name + " selected for " + target_path + "; restart to apply it.";
  return true;
}

bool disable_target(const std::string& target_path, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  Profile previous = g_profile;
  auto current = g_profile.selections.find(target_path);
  const size_t hash = target_path.find('#');
  if (hash != std::string::npos) forget_effect_slots_locked(&g_profile, target_path.substr(0, hash));
  if (current != g_profile.selections.end() && current->second == kVanillaSelection &&
      previous.effect_slots.size() == g_profile.effect_slots.size()) return true;
  g_profile.selections[target_path] = kVanillaSelection;
  ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_message = target_path + " disabled; restart to restore its vanilla asset.";
  return true;
}

bool restore_vanilla(std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  Profile previous = g_profile;
  g_profile.enabled = false; g_profile.selections.clear(); g_profile.effect_slots.clear(); ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_message = "Vanilla profile staged. Restart clears guest/render caches and uses the clean ISO.";
  return true;
}

std::string last_message() { std::lock_guard<std::mutex> lock(g_mutex); return g_message; }

bool pending_restart() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto runtime = std::atomic_load(&g_runtime);
  return runtime->initialized && runtime->fingerprint != desired_fingerprint_locked();
}
bool runtime_initialized() { return std::atomic_load(&g_runtime)->initialized; }

std::vector<CompanionOverride> active_companions() {
  return std::atomic_load(&g_runtime)->companions;
}

// The picture's file is named by the skin's id, which is "costume-" and a SHA-256 for an import;
// anything else in an id (a disc skin's) is reduced to what is safe in a file name.
std::string costume_thumbnail_path(const std::string& asset_id) {
  if (asset_id.empty() || asset_id.size() > 200) return {};
  std::string name;
  for (unsigned char c : asset_id) name += std::isalnum(c) || c == '-' || c == '_' ? (char)c : '_';
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_root.empty()) return {};
  return (g_root / L"thumbnails" / fs::u8path(name + ".png")).u8string();
}

bool costume_file(const std::string& asset_id, std::vector<uint8_t>* bytes, std::string* thumbnail_path) {
  const std::string path = costume_thumbnail_path(asset_id);
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id && item.info.kind == "character_costume";
  });
  if (asset == g_assets.end() || path.empty()) return false;
  std::string error;
  *bytes = load_runtime_asset_locked(*asset, &error);
  *thumbnail_path = path;
  return !bytes->empty();
}

}  // namespace host::cosmetics
