// Cosmetic import from files and zips: import_file(), a file installed as a catalog entry, a
// mod zip's manifest.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

ImportResult install_asset_locked(const fs::path& source, const std::string& source_kind,
                                  const std::string& member, std::vector<uint8_t> bytes,
                                  const testing::DatInspection& dat,
                                  std::vector<VaultCompanionPlan> companions) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  std::string digest = sha256(bytes);
  if (digest.empty()) { result.message = "SHA-256 validation could not be initialized."; return result; }
  // Use the full digest in the storage identity. Truncating it would make a rare prefix collision
  // overwrite a different immutable asset even though duplicate detection compares the full hash.
  std::string id = "costume-" + digest;
  auto install_companions = [&](AssetRecord* asset) -> bool {
    if (companions.empty()) return true;
    std::vector<AssetRecord::Companion> records;
    std::vector<std::string> notices;
    for (const auto& companion : companions) {
      std::string companion_digest = sha256(companion.bytes);
      if (companion_digest.empty()) { error = "A companion PNG could not be hashed."; return false; }
      const std::string filename = companion.kind == "csp" ? "csp.png" : "stock.png";
      AssetRecord::Companion record;
      record.kind = companion.kind;
      record.stored_path = (fs::path(L"assets") / fs::u8path(asset->info.id) / L"companions" /
                            fs::u8path(filename)).generic_u8string();
      record.sha256 = companion_digest;
      record.source_member = companion.source_member;
      if (!write_atomic(g_root / fs::u8path(record.stored_path), companion.bytes.data(),
                        companion.bytes.size(), &error)) return false;
      notices.push_back((companion.kind == "csp" ? "CSP" : "Stock icon") +
                        std::string(" mapped to the native selector texture with vanilla fallback: ") +
                        companion.source_member);
      records.push_back(std::move(record));
    }
    asset->companions = std::move(records);
    asset->info.unsupported_companions = std::move(notices);
    return true;
  };
  auto duplicate = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) {
    // A disc-backed record has no stored copy to refresh: an import of the same file is its own entry.
    return a.info.sha256 == digest && a.info.target_path == dat.target_path && a.source_kind != kDiscSource;
  });
  if (duplicate != g_assets.end()) {
    std::string validation_error;
    if (load_runtime_asset_locked(*duplicate, &validation_error).empty()) {
      fs::path destination = g_root / fs::u8path(duplicate->stored_path);
      if (!write_atomic(destination, bytes.data(), bytes.size(), &error)) {
        result.message = error; return result;
      }
    }
    duplicate->info.available = true;
    duplicate->info.availability_message.clear();
    AssetRecord previous = *duplicate;
    if (!install_companions(&*duplicate) || !save_catalog_locked(&error)) {
      *duplicate = std::move(previous); result.message = error; return result;
    }
    size_t variants = (size_t)std::count_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) {
      return a.info.target_path == dat.target_path;
    });
    auto current = g_profile.selections.find(dat.target_path);
    bool has_explicit_choice = current != g_profile.selections.end();
    bool selected = has_explicit_choice && current->second == duplicate->info.id;
    if (!has_explicit_choice && variants == 1) {
      Profile previous = g_profile;
      g_profile.selections[dat.target_path] = duplicate->info.id;
      ++g_profile.generation;
      if (!save_profile_locked(&error)) {
        g_profile = std::move(previous); result.message = error; return result;
      }
      selected = true; has_explicit_choice = true;
    }
    g_message = duplicate->info.name + " refreshed for " + dat.target_path +
                (selected ? "." : "; the current selection was preserved.");
    result.ok = true; result.already_present = true; result.asset_id = duplicate->info.id;
    result.message = g_message; return result;
  }
  fs::path relative = fs::path(L"assets") / fs::u8path(id) / fs::u8path(dat.target_path);
  fs::path destination = g_root / relative;
  if (!write_atomic(destination, bytes.data(), bytes.size(), &error)) { result.message = error; return result; }

  AssetRecord asset;
  asset.info.id = id;
  asset.info.name = display_name(source, member);
  asset.info.kind = "character_costume";
  asset.info.target_path = dat.target_path;
  asset.info.character = dat.character;
  asset.info.costume = dat.costume;
  asset.info.sha256 = digest;
  asset.info.roots = dat.roots;
  asset.info.available = true;
  asset.stored_path = relative.generic_u8string();
  asset.source_kind = source_kind;
  asset.source_name = path_filename_utf8(source);
  asset.source_member = member;
  if (!install_companions(&asset)) { DeleteFileW(destination.c_str()); result.message = error; return result; }
  size_t existing_variants = (size_t)std::count_if(
      g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) {
        return a.info.target_path == dat.target_path;
      });
  g_assets.push_back(asset);
  if (!save_catalog_locked(&error)) {
    g_assets.pop_back(); DeleteFileW(destination.c_str()); result.message = error; return result;
  }
  bool auto_selected = existing_variants == 0 &&
                       g_profile.selections.find(dat.target_path) == g_profile.selections.end();
  if (auto_selected) {
    Profile previous = g_profile;
    g_profile.selections[dat.target_path] = id;
    ++g_profile.generation;
    if (!save_profile_locked(&error)) {
      g_profile = std::move(previous);
      result.message = "The asset was cataloged, but selection could not be saved: " + error; return result;
    }
  }
  g_message = asset.info.name + " imported as " + dat.character + ": " + dat.costume +
              " (" + dat.target_path + ")" +
              (auto_selected ? " and automatically selected." :
                               "; the existing selection was preserved.");
  result.ok = true; result.asset_id = id; result.message = g_message;
  return result;
}

// The optional manifest of a mod zip (mod.json): the mod's name, the costume it is for when the zip
// has no costume file, and which bank is the voice when the zip has several.
struct ModManifest { std::string name, slot, voice; };
bool parse_mod_manifest(const std::vector<uint8_t>& bytes, ModManifest* out, std::string* error) {
  json root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (!root.is_object()) { *error = "mod.json is not a JSON object."; return false; }
  const auto text = [&](const char* key, std::string* value) {
    const auto found = root.find(key);
    if (found == root.end()) return true;
    if (!found->is_string()) return false;
    *value = found->get<std::string>();
    return value->size() <= 256 && std::none_of(value->begin(), value->end(), [](char c) { return (unsigned char)c < 0x20; });
  };
  if (!text("name", &out->name) || !text("slot", &out->slot) || !text("voice", &out->voice)) {
    *error = "mod.json: \"name\", \"slot\" and \"voice\" are short texts."; return false;
  }
  if (out->name.size() > 96) out->name.resize(96);
  if (!out->slot.empty()) {
    // A costume by file code ("PlFxGr") or by fighter and color ("Fox Green"). The suffix keeps a
    // dot inside a name ("Dr. Mario Red") from being read as a file extension.
    std::string slot, kind;
    if (!portrait_slot_from_name_impl(out->slot + ".slot", &slot, &kind)) {
      *error = "mod.json: \"slot\" does not name one costume (write it like \"Fox Green\" or \"PlFxGr\")."; return false;
    }
    out->slot = slot;
  }
  return true;
}

// The manifest and the voice bank of a mod zip, when it has them. The bank is the one the manifest
// names, or the only .ssm in the zip.
bool read_mod_extras(const Zip& zip, const std::vector<ZipEntry>& entries, ModManifest* manifest,
                     std::vector<uint8_t>* voice, std::string* voice_member, std::string* error) {
  const auto base_name = [](const std::string& name) {
    const size_t slash = name.find_last_of('/');
    return lower(slash == std::string::npos ? name : name.substr(slash + 1));
  };
  std::vector<const ZipEntry*> banks;
  const ZipEntry* manifest_entry = nullptr;
  for (const auto& entry : entries) {
    if (entry.name.empty() || entry.name.back() == '/') continue;
    if (lower(fs::path(entry.name).extension().string()) == ".ssm") banks.push_back(&entry);
    else if (base_name(entry.name) == "mod.json" && !manifest_entry) manifest_entry = &entry;
  }
  if (manifest_entry) {
    std::vector<uint8_t> text;
    if (manifest_entry->size > 64 * 1024) { *error = "mod.json is larger than 64 KB."; return false; }
    if (!zip.read(*manifest_entry, &text, error) || !parse_mod_manifest(text, manifest, error)) return false;
  }
  if (banks.empty()) {
    if (!manifest->voice.empty()) { *error = "mod.json names a voice bank the zip does not have: " + manifest->voice; return false; }
    return true;
  }
  const ZipEntry* chosen = nullptr;
  if (!manifest->voice.empty()) {
    for (const ZipEntry* bank : banks)
      if (lower(bank->name) == lower(manifest->voice) || base_name(bank->name) == base_name(manifest->voice)) { chosen = bank; break; }
    if (!chosen) { *error = "mod.json names a voice bank the zip does not have: " + manifest->voice; return false; }
  } else if (banks.size() == 1) chosen = banks[0];
  else { *error = "The zip has " + std::to_string(banks.size()) + " sound banks. A mod has one voice: keep one, or name it in mod.json (\"voice\")."; return false; }
  if (chosen->size > kMaxAssetBytes) { *error = "The sound bank in the zip exceeds the 64 MB safety limit."; return false; }
  if (!zip.read(*chosen, voice, error)) return false;
  *voice_member = chosen->name;
  return true;
}

// After a mod zip's costume was installed: the manifest's name for a new entry, and the voice.
void finish_mod_locked(ImportResult* result, const ModManifest& manifest, const std::string& name_suffix,
                       const std::vector<uint8_t>& voice, const std::string& voice_member) {
  if (!result->ok) return;
  if (!manifest.name.empty() && !result->already_present) {
    const auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == result->asset_id;
    });
    std::string ignored;
    if (asset != g_assets.end()) { asset->info.name = manifest.name + name_suffix; save_catalog_locked(&ignored); }
  }
  if (voice.empty()) return;
  const std::string costume_message = result->message;
  const ImportResult attached = attach_voice_locked(result->asset_id, voice, voice_member);
  result->message = costume_message + " " + attached.message;
  g_message = result->message;
}

}  // namespace cosmetic_internal

ImportResult import_file(const std::string& path_text) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string readiness;
    if (!mutable_profile_locked(&readiness)) return {false, false, {}, readiness};
  }
  fs::path path = fs::u8path(path_text);
  std::string extension = lower(path.extension().string());
  std::vector<uint8_t> bytes;
  testing::DatInspection dat;
  std::vector<VaultCompanionPlan> companions;
  std::string member, error;
  // A mod zip's own extras (docs/voice-mods.md): its manifest and the fighter's sound bank.
  ModManifest manifest;
  std::vector<uint8_t> voice;
  std::string voice_member;
  if (extension == ".ssm") {
    // A sound bank on its own: it becomes the voice of an installed skin of its fighter.
    if (!read_bounded(path, kMaxAssetBytes, &bytes, &error)) return {false, false, {}, error};
    std::lock_guard<std::mutex> lock(g_mutex);
    ImportResult result = attach_loose_voice_locked(bytes, path_filename_utf8(path), {}, {path_filename_utf8(path)});
    if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
    g_message = result.message;
    return result;
  }
  if (extension == ".png") {
    // A portrait or stock icon on its own: the file name says which costume it is for.
    std::string slot, kind;
    if (!portrait_slot_from_name_impl(path_filename_utf8(path), &slot, &kind))
      return {false, false, {}, "This picture's name does not say which costume it is for. Name it like "
                                "\"Fox Green.png\" or \"PlFxGr stock.png\", or use Add portrait and pick the costume."};
    if (!read_bounded(path, 16ull * 1024 * 1024, &bytes, &error)) return {false, false, {}, error};
    std::lock_guard<std::mutex> lock(g_mutex);
    ImportResult result = install_portrait_locked(slot, kind, bytes, path_filename_utf8(path));
    if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
    g_message = result.message;
    return result;
  }
  if (extension == ".dat" || extension == ".usd") {
    if (!read_bounded(path, kMaxAssetBytes, &bytes, &error)) return {false, false, {}, error};
    if (pause_filename(path_filename_utf8(path))) {
      VaultResourcePlan pause;
      if (!pause_resource(path, {}, std::move(bytes), &pause, &error)) return {false, false, {}, error};
      VaultPlan plan; plan.resources.push_back(std::move(pause));
      std::lock_guard<std::mutex> lock(g_mutex);
      return install_vault_locked(path, std::move(plan), "pause_screen");
    }
    size_t sound_kind = 0; const char* sound_character = nullptr;
    if (fighter_sound_identity(path_filename_utf8(path), &sound_kind, &sound_character)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result = install_fighter_sound_locked(path, path_filename_utf8(path), bytes);
      if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
      g_message = result.message;
      return result;
    }
    dat = inspect_dat_impl(bytes);
    if (!dat.ok) {
      VaultResourcePlan stage;
      if (!make_standalone_stage(path, {}, std::move(bytes), &stage, &error))
        return {false, false, {}, error};
      VaultPlan plan;
      plan.resources.push_back(std::move(stage)); plan.stage_records = 1;
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result = install_vault_locked(path, std::move(plan), "standalone_stage");
      if (result.ok && std::atomic_load(&g_runtime)->initialized)
        result.message += " Restart to apply the staged profile safely.";
      g_message = result.message;
      return result;
    }
  } else if (extension == ".zip") {
    Zip zip;
    if (!open_zip(path, &zip, &error)) return {false, false, {}, error};
    const std::vector<ZipEntry>& entries = zip.entries();
    const auto pause_files = std::count_if(entries.begin(), entries.end(), [](const auto& file) { return pause_filename(file.name); });
    if (pause_files) {
      if (pause_files > 2) return {false, false, {}, "A pause-screen pack can contain at most two styles."};
      VaultPlan plan;
      std::vector<uint8_t> hd_text;
      std::string hd_name;
      for (const auto& entry : entries) {
        const std::string filename = lower(path_filename_utf8(fs::u8path(entry.name)));
        if (filename == "tex1_108x20_c68e0bb8472069de_14.png") {
          if (!hd_text.empty() || !zip.read(entry, &hd_text, &error) || !png_companion(hd_text, &error))
            return {false, false, {}, "The pause text texture is duplicated or invalid: " + error};
          hd_name = entry.name;
        } else if (dat_extension(entry.name) && !pause_filename(entry.name))
          return {false, false, {}, "Import pause screens separately from character, stage or gameplay archives."};
      }
      for (const auto& entry : entries) {
        if (!pause_filename(entry.name)) continue;
        std::vector<uint8_t> data;
        VaultResourcePlan pause;
        if (!zip.read(entry, &data, &error) || !pause_resource(path, entry.name, std::move(data), &pause, &error))
          return {false, false, {}, "Pause screen rejected: " + error};
        if (!hd_text.empty()) pause.companions.push_back({"pause_text", hd_name, hd_text});
        plan.resources.push_back(std::move(pause));
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      return install_vault_locked(path, std::move(plan), "pause_screen");
    }
    auto metadata = std::find_if(entries.begin(), entries.end(), [](const ZipEntry& entry) {
      return lower(entry.name) == "metadata.json";
    });
    if (metadata != entries.end()) {
      VaultPlan plan;
      try {
        if (!parse_nucleus_vault(zip, entries, *metadata, &plan, &error))
          return {false, false, {}, error};
      } catch (const std::exception&) {
        return {false, false, {}, "Nucleus metadata contains an invalid field type or structure."};
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result = install_vault_locked(path, std::move(plan));
      if (result.ok && std::atomic_load(&g_runtime)->initialized)
        result.message += " Restart to apply the staged profile safely.";
      g_message = result.message;
      return result;
    }
    if (!read_mod_extras(zip, entries, &manifest, &voice, &voice_member, &error)) return {false, false, {}, error};
    std::vector<std::pair<ZipEntry, testing::DatInspection>> recognized;
    std::vector<std::vector<uint8_t>> recognized_bytes;
    std::vector<VaultResourcePlan> stage_resources;
    uint32_t candidates = 0;
    for (const auto& entry : entries) {
      if (!dat_extension(entry.name)) continue;
      if (++candidates > kMaxDatCandidates)
        return {false, false, {}, "ZIP contains too many DAT candidates for a bounded import."};
      std::vector<uint8_t> candidate;
      if (!zip.read(entry, &candidate, &error)) return {false, false, {}, error};
      auto inspected = inspect_dat_impl(candidate);
      if (inspected.ok) {
        recognized.push_back({entry, inspected});
        recognized_bytes.push_back(std::move(candidate));
      } else {
        std::string target, slot;
        if (stage_dat_target(entry.name, &target, &slot)) {
          VaultResourcePlan stage;
          if (!make_standalone_stage(path, entry.name, std::move(candidate), &stage, &error))
            return {false, false, {}, "Stage " + entry.name + " rejected: " + error};
          stage_resources.push_back(std::move(stage));
        }
      }
    }
    if (!stage_resources.empty()) {
      if (!recognized.empty())
        return {false, false, {}, "ZIP mixes costumes and stages; import separate archives."};
      VaultPlan plan;
      const ZipEntry* screenshot = nullptr;
      for (const auto& entry : entries) {
        if (lower(fs::path(entry.name).extension().string()) != ".png") continue;
        if (!screenshot || lower(entry.name).find("screenshot_0") != std::string::npos)
          screenshot = &entry;
      }
      if (screenshot) {
        std::vector<uint8_t> preview;
        std::string preview_error;
        if (zip.read(*screenshot, &preview, &preview_error) &&
            png_companion(preview, &preview_error)) {
          for (auto& stage : stage_resources)
            stage.companions.push_back({"preview", screenshot->name, preview});
        }
      }
      plan.stage_records = stage_resources.size();
      plan.resources = std::move(stage_resources);
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result = install_vault_locked(path, std::move(plan), "standalone_stage");
      if (result.ok && std::atomic_load(&g_runtime)->initialized)
        result.message += " Restart to apply the staged profile safely.";
      g_message = result.message;
      return result;
    }
    if (recognized.empty() && candidates == 0 && !voice.empty()) {
      // A voice with no costume file: for the costume the manifest, the bank's name or the zip's
      // name says, else for the one skin of that fighter it can only be meant for.
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result = attach_loose_voice_locked(voice, voice_member, manifest.slot,
                                                      {voice_member, path_filename_utf8(path)});
      if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
      g_message = result.message;
      return result;
    }
    if (recognized.empty() && candidates == 0) {
      // A pack of pictures with no costume file: each PNG whose name identifies a costume.
      size_t installed = 0, pictures = 0;
      std::string skipped, last_id, failure;
      std::lock_guard<std::mutex> lock(g_mutex);
      for (const auto& entry : entries) {
        if (lower(fs::path(entry.name).extension().string()) != ".png") continue;
        ++pictures;
        std::string slot, kind;
        std::vector<uint8_t> picture;
        if (!portrait_slot_from_name_impl(entry.name, &slot, &kind) ||
            !zip.read(entry, &picture, &error)) {
          if (skipped.size() < 160) skipped += (skipped.empty() ? "" : ", ") + path_filename_utf8(fs::u8path(entry.name));
          continue;
        }
        ImportResult one = install_portrait_locked(slot, kind, picture, path_filename_utf8(fs::u8path(entry.name)));
        if (one.ok) { ++installed; last_id = one.asset_id; }
        else failure = one.message;
      }
      if (pictures) {
        ImportResult result;
        result.ok = installed > 0;
        result.asset_id = last_id;
        result.message = installed ? std::to_string(installed) + " of " + std::to_string(pictures) + " pictures set." :
                                     failure.empty() ? "No picture in the ZIP names a costume." : failure;
        if (!skipped.empty()) result.message += " Names that identify no costume: " + skipped + ".";
        if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply them.";
        g_message = result.message;
        return result;
      }
    }
    if (recognized.empty())
      return {false, false, {}, "ZIP contains no valid DAT for a supported costume or stage resource."};
    // The same costume twice (a .dat and its .usd with the same content) is one costume.
    for (size_t i = 0; i < recognized.size(); ++i)
      for (size_t k = recognized.size(); k-- > i + 1;)
        if (lower(recognized[k].second.target_path) == lower(recognized[i].second.target_path) &&
            recognized_bytes[k] == recognized_bytes[i]) {
          recognized.erase(recognized.begin() + (std::ptrdiff_t)k);
          recognized_bytes.erase(recognized_bytes.begin() + (std::ptrdiff_t)k);
        }
    // The bank is checked before anything is written: a mod goes in whole or not at all.
    if (!voice.empty())
      for (const auto& costume : recognized) {
        VoiceCheck check;
        if (!check_voice(voice, voice_member, costume.second.target_path, &check, &error))
          return {false, false, {}, "Sound bank refused: " + error + " Nothing from this zip was imported."};
      }
    if (recognized.size() != 1) {
      // Several costumes in one mod: one entry per costume slot, each with the pictures whose
      // names say that costume, and all of them with the mod's voice.
      std::map<std::string, size_t> slots;
      for (size_t i = 0; i < recognized.size(); ++i)
        if (!slots.emplace(lower(recognized[i].second.target_path), i).second)
          return {false, false, {}, "ZIP contains two different costume files for " + recognized[i].second.target_path +
                                    "; a mod has one file per costume."};
      std::vector<std::vector<VaultCompanionPlan>> pictures(recognized.size());
      for (const auto& entry : entries) {
        if (lower(fs::path(entry.name).extension().string()) != ".png") continue;
        std::string slot, kind;
        if (!portrait_slot_from_name_impl(entry.name, &slot, &kind)) continue;
        const auto owner = slots.find(lower(slot));
        if (owner == slots.end()) continue;
        auto& mine = pictures[owner->second];
        if (std::any_of(mine.begin(), mine.end(), [&](const VaultCompanionPlan& item) { return item.kind == kind; })) continue;
        VaultCompanionPlan picture;
        picture.kind = kind;
        picture.source_member = entry.name;
        if (!zip.read(entry, &picture.bytes, &error) || !png_companion(picture.bytes, &error))
          return {false, false, {}, error};
        mine.push_back(std::move(picture));
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      ImportResult result;
      size_t installed = 0;
      for (size_t i = 0; i < recognized.size(); ++i) {
        ImportResult one = install_asset_locked(path, "zip", recognized[i].first.name, std::move(recognized_bytes[i]),
                                                recognized[i].second, std::move(pictures[i]));
        finish_mod_locked(&one, manifest, ", " + recognized[i].second.costume, voice, voice_member);
        if (!one.ok) { result.message = one.message; break; }
        ++installed;
        result.asset_id = one.asset_id;
        result.already_present = installed == 1 ? one.already_present : result.already_present && one.already_present;
      }
      result.ok = installed == recognized.size();
      if (result.ok) {
        result.message = std::to_string(installed) + " costumes imported from " + path_filename_utf8(path) +
                         (voice.empty() ? std::string(".") : ", each with the voice bank " +
                              path_filename_utf8(fs::u8path(voice_member)) + ".");
        if (std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply the staged profile safely.";
      } else if (installed) {
        result.message = std::to_string(installed) + " of " + std::to_string(recognized.size()) +
                         " costumes imported, then: " + result.message;
      }
      g_message = result.message;
      return result;
    }
    member = recognized[0].first.name;
    dat = std::move(recognized[0].second);
    bytes = std::move(recognized_bytes[0]);
    for (const auto& entry : entries) {
      std::string label = companion_label(entry);
      if (label.empty()) continue;
      VaultCompanionPlan companion;
      companion.kind = stock_picture_name(entry.name) ? "stock" : "csp";
      companion.source_member = entry.name;
      if (std::any_of(companions.begin(), companions.end(), [&](const VaultCompanionPlan& item) {
            return item.kind == companion.kind;
          }))
        return {false, false, {}, "ZIP contains multiple " + companion.kind + " companion PNGs."};
      if (!zip.read(entry, &companion.bytes, &error) ||
          !png_companion(companion.bytes, &error))
        return {false, false, {}, error};
      companions.push_back(std::move(companion));
    }
  } else {
    return {false, false, {}, "Choose a costume or stage .dat, .usd or .zip, a portrait .png, a voice bank .ssm, or a Nucleus vault .zip."};
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = install_asset_locked(path, extension == ".zip" ? "zip" : "dat", member,
                                             std::move(bytes), dat, std::move(companions));
  finish_mod_locked(&result, manifest, {}, voice, voice_member);
  if (result.ok && std::atomic_load(&g_runtime)->initialized)
    result.message += " Restart to apply the staged profile safely.";
  g_message = result.message;
  return result;
}

}  // namespace host::cosmetics
