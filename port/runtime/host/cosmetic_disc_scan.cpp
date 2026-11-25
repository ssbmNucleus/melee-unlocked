// Cosmetic disc scans: a mod disc's (or folder's) skins found and installed as a pack, the
// disc's FST, packs and apply_pack_set().
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {
bool parse_fst(uint8_t* fst, uint32_t fst_size, std::vector<FstFile>* files, std::string* error) {
  files->clear();
  if (!fst || fst_size < 12) { *error = "ISO FST is missing or truncated."; return false; }
  uint32_t entries = be32(fst + 8);
  uint64_t strings = (uint64_t)entries * 12;
  if (!entries || strings > fst_size) { *error = "ISO FST entry table is invalid."; return false; }
  struct Dir { std::string path; uint32_t end; };
  std::vector<Dir> stack{{"", entries}};
  for (uint32_t i = 1; i < entries; ++i) {
    while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
    uint8_t* record = fst + (size_t)i * 12;
    uint32_t type_name = be32(record), name_offset = type_name & 0x00ffffffu;
    if (strings + name_offset >= fst_size) { *error = "ISO FST filename points outside the string table."; return false; }
    const char* text = (const char*)fst + strings + name_offset;
    size_t available = fst_size - (size_t)(strings + name_offset), length = 0;
    while (length < available && text[length]) ++length;
    if (length == available) { *error = "ISO FST filename is not terminated."; return false; }
    std::string name(text, length);
    std::string path = stack.back().path.empty() ? name : stack.back().path + "/" + name;
    if (type_name >> 24) {
      uint32_t end = be32(record + 8);
      if (end <= i || end > entries) { *error = "ISO FST directory range is invalid."; return false; }
      stack.push_back({path, end});
    } else files->push_back({i, be32(record + 4), be32(record + 8), path});
  }
  return true;
}

// ---- skins inside a mod disc or a files pack ----

// A file name that holds a costume of a slot of the game: "PlFxGr.dat", the English "PlCaRe.usd", or
// a pack's alternate costume for the slot under another extension ending in "at" ("PlFxGr.lat" and
// "PlFxGr.rat" are the 20XX style L and R alternates). *variant names the set: "" for the costume
// itself, "alt L", "alt R", or "alt <EXT>" for any other such extension. The name decides here; the
// content is checked by the caller (it must be a costume of that slot).
bool disc_skin_set(const std::string& extension, std::string* variant) {
  if (extension == "dat" || extension == "usd") { variant->clear(); return true; }
  if (extension.size() != 3 || extension.compare(1, 2, "at") != 0) return false;
  if (extension == "lat") *variant = "alt L";
  else if (extension == "rat") *variant = "alt R";
  else *variant = std::string("alt ") + (char)std::toupper((unsigned char)extension[0]) + "AT";
  return true;
}

bool disc_skin_member(const std::string& raw, std::string* variant) {
  const std::string name = lower(raw);
  SlotName slot;
  if (name.size() != 10 || name[6] != '.' || !find_slot(name.substr(0, 6) + ".dat", &slot)) return false;
  return disc_skin_set(name.substr(7), variant);
}

bool disc_fighter_sound_member(const std::string& raw) {
  size_t kind = 0; const char* character = nullptr;
  return fighter_sound_identity(raw, &kind, &character);
}

// m-ex's exported portrait symbol file when present, otherwise the English character select file
// and then the other supported menu filenames. Only data is read from these archives.
void note_select_screen(const std::string& name, const fs::path& file, uint64_t offset, uint64_t length,
                        DiscCandidate* screen) {
  const std::string wanted = lower(name);
  // Modern m-ex loads this exported symbol file directly; when present it is authoritative.
  if (wanted == "mexselectchr.dat") { *screen = {name, file, offset, length}; return; }
  if (lower(screen->member) == "mexselectchr.dat") return;
  if (wanted == "mnslchr.usd" ||
      (wanted == "mnslchr.dat" && lower(screen->member) != "mnslchr.usd") ||
      (wanted == "mnslchr.0sd" && screen->member.empty()))
    *screen = {name, file, offset, length};
}

// m-ex gives additional costumes arbitrary DAT filenames. Only its validated costume table can
// identify them; collect potential files here and let the table and model roots decide below.
bool possible_mex_costume(const std::string& name) {
  const std::string extension = lower(fs::u8path(name).extension().u8string());
  return extension == ".dat" || extension == ".usd";
}

// Every file in a disc image whose name is a costume, fighter sound archive, or stage skin.
// *screen gets the disc's character select file when it has one.
bool disc_costume_files(const fs::path& iso, std::vector<DiscCandidate>* out, DiscCandidate* screen, DiscCandidate* metadata,
                        DiscCandidate* hud, std::string* error) {
  std::vector<uint8_t> header;
  if (!read_range(iso, 0, 0x440, &header)) { *error = "The disc image could not be read."; return false; }
  const uint64_t fst_offset = be32(header.data() + 0x424), fst_size = be32(header.data() + 0x428);
  std::vector<uint8_t> fst;
  if (fst_size < 12 || fst_size > 2u * 1024u * 1024u || !read_range(iso, fst_offset, fst_size, &fst)) {
    *error = "The disc image has no readable file table."; return false;
  }
  std::vector<FstFile> files;
  if (!parse_fst(fst.data(), (uint32_t)fst.size(), &files, error)) return false;
  for (const auto& file : files) {
    const size_t slash = file.path.find_last_of('/');
    const std::string name = slash == std::string::npos ? file.path : file.path.substr(slash + 1);
    std::string variant, stage, label;
    note_select_screen(name, iso, file.start, file.size, screen);
    if (lower(name) == "mxdt.dat") *metadata = {name, iso, file.start, file.size};
    if (lower(name) == "ifall.usd") *hud = {name, iso, file.start, file.size};
    if (!disc_skin_member(name, &variant) && !disc_stage_member(name, &stage, &label, &variant) &&
        !disc_fighter_sound_member(name) && !possible_mex_costume(name)) continue;
    out->push_back({name, iso, file.start, file.size});
  }
  return true;
}

void folder_costume_files(const fs::path& root, std::vector<DiscCandidate>* out, DiscCandidate* screen, DiscCandidate* metadata,
                          DiscCandidate* hud) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = path_filename_utf8(it->path());
    std::string variant, stage, label;
    const uint64_t length = it->file_size(ec);
    if (ec) continue;
    note_select_screen(name, it->path(), 0, length, screen);
    if (lower(name) == "mxdt.dat") *metadata = {name, it->path(), 0, length};
    if (lower(name) == "ifall.usd") *hud = {name, it->path(), 0, length};
    if (!disc_skin_member(name, &variant) && !disc_stage_member(name, &stage, &label, &variant) &&
        !disc_fighter_sound_member(name) && !possible_mex_costume(name)) continue;
    out->push_back({name, it->path(), 0, length});
  }
}

// Gives each plain costume of a scan the portrait the pack's character select file shows for its
// slot, when that picture differs from the game's own. The picture is written beside the catalog as
// the skin's "csp" companion. m-ex names each costume's portrait with its external fighter ID,
// costume index and CSP stride; its extra costumes must not use the retail animation's columns.
// m-ex also renames the exported skeleton/material symbols for new colors. Map those names to
// the native costume whose visibility tables the pack explicitly chose; model data stays intact.
bool prepare_mex_costume(const disc_metadata::MexCostume& row, std::vector<uint8_t>* bytes,
                         testing::DatInspection* inspected, std::map<std::string, std::string>* aliases) {
  *inspected = inspect_dat_impl(*bytes);
  if (inspected->ok) return inspected->target_path.substr(2, 2) == row.family;
  for (const auto& family : families) {
    if (row.family != family.file_code) continue;
    const std::string prefix = std::string("Ply") + family.root_name + "5K";
    const std::string suffix = "_Share_joint";
    if (row.joint.size() < prefix.size() + suffix.size() || row.joint.compare(0, prefix.size(), prefix) ||
        row.joint.compare(row.joint.size() - suffix.size(), suffix.size(), suffix)) return false;
    std::istringstream colors(family.colors);
    std::string color;
    if (row.visibility > 15) return false;
    for (uint32_t i = 0; i <= row.visibility; ++i) if (!(colors >> color)) return false;
    const std::string joint = prefix + (color == "Nr" ? "" : color) + suffix;
    (*aliases)[row.joint] = joint;
    if (!row.matanim.empty()) {
      const std::string expected = row.joint.substr(0, row.joint.size() - 6) + "_matanim_joint";
      if (row.matanim != expected) return false;
      (*aliases)[row.matanim] = joint.substr(0, joint.size() - 6) + "_matanim_joint";
    }
    std::vector<uint8_t> renamed;
    if (!disc_metadata::alias_roots(*bytes, *aliases, &renamed)) return false;
    *inspected = inspect_dat_impl(renamed);
    if (!inspected->ok || inspected->target_path != std::string("Pl") + family.file_code + color + ".dat") return false;
    *bytes = std::move(renamed);
    return true;
  }
  return false;
}

}  // namespace cosmetic_internal

ImportResult scan_disc_skins(const std::string& iso_path, const std::string& pack_name, std::string* error) {
  std::string local; if (!error) error = &local;
  ImportResult result;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) { result.message = *error; return result; }
  std::error_code ec;
  const fs::path root = fs::absolute(fs::u8path(iso_path), ec);
  const std::string key = root.u8string();
  const bool folder = fs::is_directory(root, ec);
  uint64_t root_size = 0; int64_t root_mtime = 0;
  if (!folder && !file_stamp(root, &root_size, &root_mtime)) {
    *error = result.message = "The disc image could not be opened."; return result;
  }
  // A disc scanned before, unchanged since: nothing to read. A folder is small and has no single
  // time to compare, so it is read every time.
  if (!folder)
    for (const auto& scan : g_disc_scans)
      if (scan.path == key && scan.size == root_size && scan.mtime == root_mtime && scan.rules >= kScanRules) {
        result.ok = result.already_present = true;
        result.message = "Skins of " + pack_name + " are already in the skin list.";
        return result;
      }
  std::vector<DiscCandidate> candidates;
  DiscCandidate select_screen;   // the pack's character select file, when it has one
  DiscCandidate hud;             // and its match HUD file (m-ex keeps every stock icon there)
  DiscCandidate metadata;
  if (folder) folder_costume_files(root, &candidates, &select_screen, &metadata, &hud);
  else if (!disc_costume_files(root, &candidates, &select_screen, &metadata, &hud, error)) { result.message = *error; return result; }
  disc_metadata::MexCostumes mex_costumes;
  if (!metadata.member.empty()) {
    std::vector<uint8_t> bytes;
    const bool mapped = read_range(metadata.file, metadata.offset, metadata.length, &bytes) &&
                        disc_metadata::mex_costumes(bytes, &mex_costumes);
    host::log("cosmetics: %s m-ex costume metadata from %s (%zu mapped files)",
              mapped ? "read" : "unsupported", key.c_str(), mex_costumes.size());
  }
  // The plain .dat of a slot first, then its English .usd: when both differ from the game's and from
  // each other, the English one is the one this game loads, so it is the one listed.
  std::stable_sort(candidates.begin(), candidates.end(), [](const DiscCandidate& a, const DiscCandidate& b) {
    return lower(a.member) < lower(b.member);
  });
  std::map<std::string, AssetRecord> fresh;   // by target slot and variant
  size_t same = 0, skipped = 0, stages = 0, fighter_sounds = 0;
  for (const auto& candidate : candidates) {
    uint32_t retail_offset = 0, retail_size = 0;
    std::vector<uint8_t> bytes, retail;
    std::string variant, stage_path, stage_label;
    const bool stage = disc_stage_member(candidate.member, &stage_path, &stage_label, &variant);
    size_t sound_kind = 0; const char* sound_character = nullptr;
    const bool sound_file = fighter_sound_identity(candidate.member, &sound_kind, &sound_character);
    const bool ordinary_costume = disc_skin_member(candidate.member, &variant);
    const auto mex = mex_costumes.find(lower(candidate.member));
    const bool added_costume = !stage && !sound_file && !ordinary_costume && mex != mex_costumes.end();
    if (!stage && !sound_file && !ordinary_costume && !added_costume) continue;
    if (!read_range(candidate.file, candidate.offset, candidate.length, &bytes)) { ++skipped; continue; }
    testing::DatInspection added;
    std::map<std::string, std::string> aliases;
    std::string source_digest;
    if (added_costume) {
      source_digest = sha256(bytes);
      if (!prepare_mex_costume(mex->second, &bytes, &added, &aliases)) { ++skipped; continue; }
      variant = "m-ex costume " + std::to_string(mex->second.costume + 1);
    }
    // An alternate costume is compared with the slot's own costume on the game disc: that is the
    // file it replaces when chosen, and the skeleton it must keep. An alternate stage likewise.
    const std::string retail_name = stage ? stage_path : sound_file ? candidate.member :
                                    added_costume ? added.target_path :
                                    variant.empty() ? candidate.member : candidate.member.substr(0, 6) + ".dat";
    if (!host::disc_find_file(retail_name, &retail_offset, &retail_size) || !retail_size ||
        retail_size > kMaxAssetBytes) {
      ++skipped; continue;
    }
    retail.resize(retail_size);
    if (!host::disc_read(retail_offset, retail.data(), retail_size)) { ++skipped; continue; }
    if (bytes == retail && !added_costume) { ++same; continue; }
    if (sound_file) {
      std::vector<uint8_t> safe;
      std::string detail;
      if (!fighter_sound_overlay(retail, bytes, sound_kind, &safe, &detail)) { ++skipped; continue; }
      const std::string digest = sha256(bytes);
      if (digest.empty()) { ++skipped; continue; }
      const std::string sound_target = candidate.member;
      const std::string fresh_key = sound_target + "\n#sounds";
      AssetRecord asset;
      asset.info.id = "disc-" + sha256_text(key + "\n" + lower(candidate.member));
      asset.info.name = std::string(sound_character) + " move sounds: from " + pack_name;
      asset.info.kind = "fighter_sound";
      asset.info.target_path = sound_target;
      asset.info.character = sound_character;
      asset.info.costume = "Move sounds";
      asset.info.sha256 = digest;
      asset.info.available = true;
      asset.info.source = "disc";
      asset.info.source_name = pack_name;
      asset.info.disc_path = candidate.file.u8string();
      asset.stored_path = "disc/" + asset.info.id + "/" + sound_target;
      asset.source_kind = kDiscSource;
      asset.source_name = pack_name;
      asset.source_member = candidate.member;
      asset.source_id = key;
      asset.disc_path = asset.info.disc_path;
      asset.disc_offset = candidate.offset;
      asset.disc_length = candidate.length;
      if (!file_stamp(candidate.file, &asset.disc_size, &asset.disc_mtime)) { ++skipped; continue; }
      auto have = fresh.find(fresh_key);
      if (have == fresh.end() || have->second.info.sha256 != digest) fresh[fresh_key] = std::move(asset);
      if (have == fresh.end()) ++fighter_sounds;
      continue;
    }
    if (stage) {
      // The checks a stage DAT added by hand gets: it has a stage map, and the game can draw it.
      VisualLayout layout;
      std::string detail;
      if (!parse_visual_layout(bytes, &layout, &detail) ||
          std::find(layout.roots.begin(), layout.roots.end(), "map_head") == layout.roots.end()) { ++skipped; continue; }
      // An m-ex stage stub is not a stage: it keeps its code (grFunction) and tables in the stage's file,
      // has no collision data, and loads the real stage from a folder of alternates. Served as it is,
      // nothing stands on it.
      if (std::find(layout.roots.begin(), layout.roots.end(), "grFunction") != layout.roots.end() &&
          std::find(layout.roots.begin(), layout.roots.end(), "coll_data") == layout.roots.end()) { ++skipped; continue; }
      const std::string digest = sha256(bytes);
      if (digest.empty()) { ++skipped; continue; }
      const std::string fresh_key = stage_path + "\n" + variant;
      const auto have = fresh.find(fresh_key);
      if (have != fresh.end() && have->second.info.sha256 == digest) continue;   // .dat and .usd carry the same stage
      AssetRecord asset;
      asset.info.id = "disc-" + sha256_text(key + "\n" + lower(candidate.member));
      asset.info.name = stage_label + ": from " + pack_name + (variant.empty() ? "" : " (" + variant + ")");
      asset.info.variant = variant;
      asset.online_known = true;
      if (kStrictOnlineStages) {
        asset.online_ok = stage_online_form(retail, bytes, nullptr, &detail, stage_path);
        asset.online_note = asset.online_ok ? detail : online_reason_short(detail);
      } else {
        asset.online_ok = true;
        asset.online_note = "as installed";
      }
      if (!stage_draw_safe(bytes, &detail)) { asset.online_ok = false; asset.online_note = online_reason_short(detail); }
      asset.info.kind = "stage_visual";
      asset.info.target_path = stage_path;
      asset.info.character = "Stages";
      asset.info.costume = stage_label;
      asset.info.sha256 = digest;
      asset.info.available = true;
      asset.info.source = "disc";
      asset.info.source_name = pack_name;
      asset.info.disc_path = candidate.file.u8string();
      asset.stored_path = "disc/" + asset.info.id + "/" + stage_path;
      asset.source_kind = kDiscSource;
      asset.source_name = pack_name;
      asset.source_member = candidate.member;
      asset.source_id = key;
      asset.disc_path = asset.info.disc_path;
      asset.disc_offset = candidate.offset;
      asset.disc_length = candidate.length;
      if (!file_stamp(candidate.file, &asset.disc_size, &asset.disc_mtime)) { ++skipped; continue; }
      if (have == fresh.end()) ++stages;
      fresh[fresh_key] = std::move(asset);
      continue;
    }
    const auto dat = added_costume ? added : inspect_dat_impl(bytes);
    const std::string slot_name = added_costume ? added.target_path : candidate.member.substr(0, 6) + ".dat";
    if (!dat.ok || lower(dat.target_path) != lower(slot_name)) { ++skipped; continue; }
    const std::string digest = source_digest.empty() ? sha256(bytes) : source_digest;
    if (digest.empty()) { ++skipped; continue; }
    const std::string fresh_key = dat.target_path + "\n" + variant;
    const auto have = fresh.find(fresh_key);
    if (have != fresh.end() && have->second.info.sha256 == digest) continue;   // .dat and .usd carry the same skin
    AssetRecord asset;
    // The identity is where the skin is, not what it is: a newer build of the same disc keeps the
    // player's selection.
    asset.info.id = "disc-" + sha256_text(key + "\n" + lower(candidate.member));
    asset.info.name = dat.character + " " + dat.costume + ": from " + pack_name + (variant.empty() ? "" : " (" + variant + ")");
    asset.info.variant = variant;
    asset.costume_aliases = std::move(aliases);
    {
      std::string detail;
      asset.online_known = true;
      asset.online_ok = costume_online_fix(retail, bytes, nullptr, &detail);
      asset.online_note = online_reason_short(detail);
      // A costume the game cannot draw stays listed with its reason, and the load refuses it
      // everywhere (load_runtime_asset_locked); the online verdict carries the reason to the list.
      if (!costume_draw_safe(bytes, &detail)) { asset.online_ok = false; asset.online_note = online_reason_short(detail); }
    }
    asset.info.kind = "character_costume";
    asset.info.target_path = dat.target_path;
    asset.info.character = dat.character;
    asset.info.costume = dat.costume;
    asset.info.sha256 = digest;
    asset.info.roots = dat.roots;
    asset.info.available = true;
    asset.info.source = "disc";
    asset.info.source_name = pack_name;
    asset.info.disc_path = candidate.file.u8string();
    // Never written: a build that does not know disc-backed skins finds no file here and lists the
    // entry as unavailable, instead of refusing the whole catalog.
    asset.stored_path = "disc/" + asset.info.id + "/" + dat.target_path;
    asset.source_kind = kDiscSource;
    asset.source_name = pack_name;
    asset.source_member = candidate.member;
    asset.source_id = key;
    asset.disc_path = asset.info.disc_path;
    asset.disc_offset = candidate.offset;
    asset.disc_length = candidate.length;
    if (!file_stamp(candidate.file, &asset.disc_size, &asset.disc_mtime)) { ++skipped; continue; }
    fresh[fresh_key] = std::move(asset);
  }
  const size_t portraits = pack_portraits_locked(select_screen, mex_costumes, &fresh);
  const size_t stocks = pack_stock_icons_locked(hud, mex_costumes, &fresh);
  if (stocks) host::log("cosmetics: %zu stock icons taken from the %s of %s", stocks, hud.member.c_str(), key.c_str());
  const size_t costumes = std::count_if(fresh.begin(), fresh.end(), [](const auto& item) {
    return item.second.info.kind == "character_costume";
  });  // count before moving the records; portraits are costume companions, not entries to subtract
  // Replace this disc's earlier records: files that no longer differ go away, changed ones are
  // updated, and a name the player gave an entry is kept. Records of a disc that no longer exists
  // go too; they hold no data and a new scan brings them back.
  auto previous_assets = g_assets;
  auto previous_scans = g_disc_scans;
  std::map<std::string, std::string> names;
  std::map<std::string, AssetRecord::Voice> voices;   // a voice bank the player gave a pack's skin stays with it
  std::map<std::string, std::vector<AssetRecord::Companion>> pictures;   // and so does a picture (set_skin_portrait)
  g_assets.erase(std::remove_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    if (item.source_kind != kDiscSource) return false;
    if (item.source_id == key) {
      if (item.info.name.find(": from ") == std::string::npos) names[item.info.id] = item.info.name;
      if (!item.voice.stored_path.empty()) voices[item.info.id] = item.voice;
      for (const auto& companion : item.companions)
        if (player_picture(companion)) pictures[item.info.id].push_back(companion);
      return true;
    }
    std::error_code gone;
    return !fs::exists(fs::u8path(item.disc_path), gone);
  }), g_assets.end());
  for (auto& item : fresh) {
    const auto renamed = names.find(item.second.info.id);
    if (renamed != names.end()) item.second.info.name = renamed->second;
    const auto voiced = voices.find(item.second.info.id);
    if (voiced != voices.end()) {
      item.second.voice = voiced->second;
      item.second.info.voice = voiced->second.bank;
      item.second.info.voice_source = voiced->second.source_member;
    }
    const auto pictured = pictures.find(item.second.info.id);
    if (pictured != pictures.end())
      for (const auto& kept : pictured->second) {
        auto& have = item.second.companions;
        have.erase(std::remove_if(have.begin(), have.end(),
            [&](const AssetRecord::Companion& c) { return c.kind == kept.kind; }), have.end());
        have.push_back(kept);
      }
    g_assets.push_back(std::move(item.second));
  }
  g_disc_scans.erase(std::remove_if(g_disc_scans.begin(), g_disc_scans.end(),
      [&](const DiscScan& scan) { return scan.path == key; }), g_disc_scans.end());
  // A scan that matched no file at all (none listed, none standard) compared against no game disc,
  // so it is not kept: the next scan reads the disc again.
  if (!folder && (same > 0 || !fresh.empty())) g_disc_scans.push_back({key, root_size, root_mtime, kScanRules});
  if (!save_state_locked(error)) {
    g_assets = std::move(previous_assets); g_disc_scans = std::move(previous_scans);
    result.message = *error; return result;
  }
  result.ok = true;
  result.message = std::to_string(costumes) + " skins" +
                   (stages ? " and " + std::to_string(stages) + " stage skins" : std::string()) +
                   (fighter_sounds ? " and " + std::to_string(fighter_sounds) + " fighter sound mods" : std::string()) + " from " + pack_name +
                   " are in the skin list (" + std::to_string(same) + " files are the standard ones, " +
                   std::to_string(skipped) + " not usable).";
  host::log("cosmetics: scanned %s: %zu skins, %zu stage skins and %zu fighter sound mods listed, %zu standard, %zu not usable", key.c_str(),
            costumes, stages, fighter_sounds, same, skipped);
  if (!select_screen.member.empty())
    host::log("cosmetics: %zu portraits taken from the %s of %s", portraits, select_screen.member.c_str(), key.c_str());
  return result;
}

uint32_t disc_skin_count(const std::string& iso_path) {
  std::error_code ec;
  const std::string key = fs::absolute(fs::u8path(iso_path), ec).u8string();
  std::lock_guard<std::mutex> lock(g_mutex);
  return (uint32_t)std::count_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.source_kind == kDiscSource && item.source_id == key;
  });
}

std::vector<PackInfo> packs() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<PackInfo> out;
  std::map<std::string, size_t> index;
  std::map<std::string, std::map<std::string, bool>> slots;   // pack key -> covered slots
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "character_costume") continue;
    const std::string key = pack_key(asset);
    auto at = index.find(key);
    if (at == index.end()) {
      PackInfo pack;
      pack.key = key;
      pack.disc = asset.source_kind == kDiscSource;
      pack.name = pack.disc ? asset.source_name : "Imported skins";
      out.push_back(std::move(pack));
      at = index.emplace(key, out.size() - 1).first;
    }
    PackInfo& pack = out[at->second];
    ++pack.skins;
    slots[key][asset.info.target_path] = true;
    if (std::find(pack.variants.begin(), pack.variants.end(), asset.info.variant) == pack.variants.end())
      pack.variants.push_back(asset.info.variant);
    if (asset.online_known) pack.online_on += asset.online_ok ? 1 : 0;
    else ++pack.online_unchecked;
    const auto picked = g_profile.selections.find(asset.info.target_path);
    if (picked != g_profile.selections.end() && picked->second == asset.info.id) ++pack.selected;
  }
  for (auto& pack : out) {
    pack.slots = (uint32_t)slots[pack.key].size();
    std::sort(pack.variants.begin(), pack.variants.end(), [](const std::string& a, const std::string& b) {
      // Costume 10 follows 9, rather than preceding 2 in a large m-ex pack.
      constexpr const char* prefix = "m-ex costume ";
      if (a.rfind(prefix, 0) == 0 && b.rfind(prefix, 0) == 0) {
        const auto left = std::strtoul(a.c_str() + 13, nullptr, 10);
        const auto right = std::strtoul(b.c_str() + 13, nullptr, 10);
        if (left != right) return left < right;
      }
      return a < b;
    });
  }
  // Imports first, then the packs by name.
  std::stable_sort(out.begin(), out.end(), [](const PackInfo& a, const PackInfo& b) {
    if (a.disc != b.disc) return !a.disc;
    return lower(a.name) < lower(b.name);
  });
  return out;
}

PackSetResult apply_pack_set(const std::string& pack, const std::string& variant, bool preview) {
  PackSetResult result;
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string error;
  if (!preview && !mutable_profile_locked(&error)) { result.message = error; return result; }
  const bool none = variant == "none";
  // The pack's choice for each slot: the record of that variant, an available one when there is more
  // than one. For "none", every record of the pack counts, since any of them may be selected now.
  std::map<std::string, const AssetRecord*> choice;
  std::map<std::string, const AssetRecord*> by_id;
  for (const auto& asset : g_assets) {
    by_id[asset.info.id] = &asset;
    if (asset.info.kind != "character_costume" || pack_key(asset) != pack) continue;
    if (!none && asset.info.variant != variant) continue;
    const AssetRecord*& have = choice[asset.info.target_path];
    if (!have || (!have->info.available && asset.info.available)) have = &asset;
  }
  if (choice.empty()) { result.message = "That pack has no skins of that set."; return result; }
  Profile next = g_profile;
  std::map<std::string, uint32_t> replaced_from;
  for (const auto& slot : choice) {
    const auto current = next.selections.find(slot.first);
    const std::string current_id = current == next.selections.end() ? std::string() : current->second;
    const auto current_record = by_id.find(current_id);
    const bool current_is_skin = current_record != by_id.end() && current_id != kVanillaSelection;
    if (none) {
      // Only this pack's own picks go back to the standard costume; another pack's choice is kept.
      if (current_is_skin && pack_key(*current_record->second) == pack) {
        next.selections[slot.first] = kVanillaSelection; ++result.cleared;
      }
      continue;
    }
    if (!slot.second->info.available) continue;
    if (current_id == slot.second->info.id) continue;
    if (current_is_skin && pack_key(*current_record->second) != pack) {
      ++result.replaced;
      ++replaced_from[current_record->second->source_kind == kDiscSource ? current_record->second->source_name
                                                                          : std::string("Imported skins")];
    }
    next.selections[slot.first] = slot.second->info.id; ++result.set;
  }
  uint32_t most = 0;
  for (const auto& from : replaced_from) if (from.second > most) { most = from.second; result.replaced_from = from.first; }
  if (replaced_from.size() > 1) result.replaced_from = "other packs";
  result.ok = true;
  if (preview) return result;
  if (result.set || result.cleared) {
    Profile previous = g_profile;
    g_profile = std::move(next);
    if (!none) g_profile.enabled = true;
    ++g_profile.generation;
    if (!save_profile_locked(&error)) { g_profile = std::move(previous); result.ok = false; result.message = error; return result; }
  }
  result.message = none ? std::to_string(result.cleared) + " costumes back to standard; restart to apply."
                        : std::to_string(result.set) + " costumes set" +
                          (result.replaced ? ", " + std::to_string(result.replaced) + " replaced from " + result.replaced_from : "") +
                          "; restart to apply.";
  g_message = result.message;
  return result;
}

}  // namespace host::cosmetics
