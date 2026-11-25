// Cosmetic sounds: voice banks (checks, import, attach, which bank a match hears) and fighter
// move sound archives (install, merge, publish).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {
namespace cosmetic_internal {

// ---- voice mods: fighter sound banks (docs/voice-mods.md) ----
// A fighter's voice and move sounds are one bank file on the disc, audio/us/<bank>.ssm for English
// and audio/<bank>.ssm for Japanese. The file is four big-endian words (size of the sound table,
// size of the sample data, number of sounds, id of the first sound), the sound table (per sound:
// channel count, sample rate, then 0x40 bytes per channel: loop flag, format, the loop, end and
// start addresses in nibbles from the start of the sample data, and the ADPCM state), padding to
// 32 bytes, and the samples. The game plays a sound by id, so a replacement has to keep the count
// and the first id; the loader reads the file in three parts and budgets the samples from a fixed
// table, so it also has to keep the table's shape and stay inside that budget.
//
// The rows below: the disc file, the game's bank number (its place in the loader's file list), the
// costume file codes of the fighters that use the bank, the sample bytes the loader budgets for it,
// and for the English and the Japanese file the number of sounds, the id of the first one, and
// which sounds have two channels. NTSC 1.02. The disc's own files are compared again at launch.
constexpr VoiceBank voice_banks[] = {
    {"captain.ssm", 6, "Ca", 443328, {{28, 336, ""}, {28, 336, "0"}}},
    {"clink.ssm", 7, "Cl", 298400, {{40, 364, ""}, {40, 364, ""}}},
    {"dk.ssm", 8, "Dk", 206368, {{23, 404, ""}, {23, 404, ""}}},
    {"drmario.ssm", 9, "Dr", 430560, {{36, 427, ""}, {37, 427, "0"}}},
    {"falco.ssm", 10, "Fc", 599712, {{53, 463, "2 33 34"}, {55, 464, "0 2 35 36"}}},
    {"fox.ssm", 11, "Fx", 573216, {{50, 516, "30 31"}, {54, 519, "0 34 35"}}},
    {"ice.ssm", 13, "Pp Nn", 477600, {{44, 589, ""}, {44, 596, "0"}}},
    {"kirby.ssm", 14, "Kb", 586208, {{52, 633, ""}, {52, 640, "0"}}},
    {"koopa.ssm", 15, "Kp", 526752, {{26, 685, "4 5 6 7 8 9 10 11 12 13 14 15 16"}, {26, 692, "0 4 5 6 7 8 9 10 11 12 13 14 15 16"}}},
    {"link.ssm", 16, "Lk", 328672, {{37, 711, ""}, {37, 718, "0"}}},
    {"luigi.ssm", 17, "Lg", 372992, {{35, 748, ""}, {35, 755, ""}}},
    {"mario.ssm", 18, "Mr", 372736, {{32, 783, ""}, {32, 790, ""}}},
    {"mars.ssm", 19, "Ms", 513088, {{46, 815, ""}, {46, 822, "0"}}},
    {"mewtwo.ssm", 20, "Mt", 562912, {{32, 861, "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20"}, {35, 868, "0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23"}}},
    {"ness.ssm", 21, "Ns", 509024, {{35, 893, "26"}, {35, 903, "26"}}},
    {"peach.ssm", 22, "Pe", 416736, {{30, 928, ""}, {30, 938, "0"}}},
    {"pichu.ssm", 23, "Pc", 580128, {{32, 958, "25 30"}, {32, 968, "0 25 30"}}},
    {"pikachu.ssm", 24, "Pk", 613088, {{36, 990, "30 35"}, {36, 1000, "0 30 35"}}},
    {"purin.ssm", 25, "Pr", 334528, {{23, 1026, ""}, {26, 1036, "0"}}},
    {"samus.ssm", 26, "Ss", 319040, {{25, 1049, "14"}, {25, 1062, "0 14"}}},
    {"zs.ssm", 27, "Zd Sk", 590176, {{62, 1074, ""}, {62, 1087, "0 1"}}},
    {"yoshi.ssm", 28, "Ys", 321472, {{33, 1136, ""}, {33, 1149, "0"}}},
    {"gw.ssm", 29, "Gw", 165344, {{21, 1169, ""}, {21, 1182, "0"}}},
    {"ganon.ssm", 30, "Gn", 427872, {{28, 1190, ""}, {28, 1203, "0"}}},
    {"emblem.ssm", 31, "Fe", 495520, {{45, 1218, ""}, {45, 1231, "0"}}},
};

const VoiceBank* family_bank(const FighterFamily& family) {
  for (const auto& bank : voice_banks)
    if ((std::string(" ") + bank.fighters + " ").find(std::string(" ") + family.file_code + " ") != std::string::npos)
      return &bank;
  return nullptr;
}
const VoiceBank* bank_by_file(const std::string& name) {
  const std::string wanted = lower(name);
  for (const auto& bank : voice_banks) if (wanted == bank.file) return &bank;
  return nullptr;
}
// "Fox", "Zelda and Sheik": whose bank it is, for a message.
std::string bank_fighters(const VoiceBank& bank) {
  std::string out;
  for (const auto& family : families)
    if (family_bank(family) == &bank) out += (out.empty() ? "" : " and ") + std::string(family.display_name);
  return out;
}

// Reads a bank's header and sound table the way the game's loader does, and refuses anything the
// loader would misread: a table that does not end where the header says, a sound with a channel
// count the game does not play, a format other than ADPCM, an address outside the sample data.
bool parse_bank(const std::vector<uint8_t>& bytes, BankLayout* out, std::string* error) {
  *out = BankLayout{};
  if (bytes.size() < 0x20) { *error = "The sound bank is too short to have a header."; return false; }
  out->table_size = be32(bytes.data()); out->data_size = be32(bytes.data() + 4);
  out->count = be32(bytes.data() + 8); out->base = be32(bytes.data() + 12);
  if (!out->count || out->count > 4096 || out->table_size < 0x10 || out->table_size > bytes.size() - 0x10) {
    *error = "The file does not start with a sound bank header."; return false;
  }
  const uint64_t table_end = 0x10ull + out->table_size;
  out->data_offset = (uint32_t)((table_end + 31) & ~31ull);
  if (!out->data_size || (uint64_t)out->data_offset + out->data_size > bytes.size()) {
    *error = "The sound bank's sample data runs past the end of the file."; return false;
  }
  uint64_t at = 0x10;
  for (uint32_t sound = 0; sound < out->count; ++sound) {
    if (at + 8 > table_end) { *error = "The sound table ends before sound " + std::to_string(sound) + "."; return false; }
    const uint32_t channels = be32(bytes.data() + at);
    if (channels != 1 && channels != 2) {
      *error = "Sound " + std::to_string(sound) + " has " + std::to_string(channels) + " channels; the game plays 1 or 2.";
      return false;
    }
    if (at + 8 + (uint64_t)channels * 0x40 > table_end) {
      *error = "The sound table ends inside sound " + std::to_string(sound) + "."; return false;
    }
    // The rate sets the pitch the mixer plays the sound at; the game's own banks use 8 to 48 kHz.
    const uint32_t rate = be32(bytes.data() + at + 4);
    if (rate < 4000 || rate > 48000) {
      *error = "Sound " + std::to_string(sound) + " plays at " + std::to_string(rate) + " Hz; the game plays 4 to 48 kHz.";
      return false;
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const uint8_t* voice = bytes.data() + at + 8 + (size_t)channel * 0x40;
      const uint32_t format = be16(voice + 2), loop = be32(voice + 4), end = be32(voice + 8), start = be32(voice + 12);
      if (format != 0) { *error = "Sound " + std::to_string(sound) + " is not ADPCM; the game's banks are."; return false; }
      // Nibble addresses from the start of the sample data. The loader moves all three by the
      // bank's place in audio memory, the loop address also for a sound that does not loop.
      if (start > end || loop > end || (uint64_t)end >= (uint64_t)out->data_size * 2) {
        *error = "Sound " + std::to_string(sound) + " points outside the bank's sample data."; return false;
      }
    }
    out->channels.push_back((uint8_t)channels);
    at += 8 + (uint64_t)channels * 0x40;
  }
  if (at != table_end) {
    *error = "The sound table is " + std::to_string(out->table_size) + " bytes; its " + std::to_string(out->count) +
             " sounds take " + std::to_string(at - 0x10) + "."; return false;
  }
  return true;
}

std::vector<uint8_t> identity_channels(const BankIdentity& identity) {
  std::vector<uint8_t> channels(identity.count, 1);
  std::istringstream list(identity.stereo);
  for (uint32_t sound; list >> sound;) if (sound < channels.size()) channels[sound] = 2;
  return channels;
}

// The first sound whose channel count differs, or -1 when the two tables have the same shape.
int first_channel_difference(const std::vector<uint8_t>& game, const std::vector<uint8_t>& candidate) {
  if (game.size() != candidate.size()) return 0;
  for (size_t i = 0; i < game.size(); ++i) if (game[i] != candidate[i]) return (int)i;
  return -1;
}

// The room the loader takes for a bank's samples: a whole number of 32 byte blocks.
uint32_t bank_sample_room(const BankLayout& layout) { return (layout.data_size + 31) & ~31u; }

// Which of the game's fighter banks a parsed bank is, and which language's (0 English, 1 Japanese),
// from its own header: the same number of sounds, the same first id, the same channels per sound,
// and samples that fit the room the game keeps for the bank. `name_hint` (a file name) only makes
// the refusal say more. Null with the reason otherwise.
const VoiceBank* match_bank(const BankLayout& layout, const std::string& name_hint, int* language, std::string* error) {
  const VoiceBank* near_bank = nullptr; int near_language = 0, near_sound = 0;
  for (const auto& bank : voice_banks)
    for (int which = 0; which < 2; ++which) {
      const BankIdentity& identity = bank.language[which];
      if (identity.count != layout.count || identity.base != layout.base) continue;
      const int differs = first_channel_difference(identity_channels(identity), layout.channels);
      if (differs >= 0) { if (!near_bank) { near_bank = &bank; near_language = which; near_sound = differs; } continue; }
      if (bank_sample_room(layout) > bank.budget) {
        *error = std::string(bank.file) + " holds " + std::to_string(bank_sample_room(layout) / 1024) +
                 " KB of samples; the game keeps room for " + std::to_string(bank.budget / 1024) +
                 " KB for this bank. Shorten or resample the longest sounds.";
        return nullptr;
      }
      *language = which;
      return &bank;
    }
  if (near_bank) {
    const uint32_t have = (size_t)near_sound < layout.channels.size() ? layout.channels[(size_t)near_sound] : 0;
    *error = std::string(near_bank->file) + ": sound " + std::to_string(near_sound) + " has " + std::to_string(have) +
             (have == 1 ? " channel" : " channels") + "; the game's (" + kBankLanguages[near_language] + ") has " +
             std::to_string(have == 1 ? 2 : 1) + ". A voice bank keeps the channel count of every sound.";
    return nullptr;
  }
  const size_t slash = name_hint.find_last_of("/\\");
  if (const VoiceBank* named = bank_by_file(slash == std::string::npos ? name_hint : name_hint.substr(slash + 1))) {
    *error = std::string(named->file) + " here has " + std::to_string(layout.count) + " sounds from id " +
             std::to_string(layout.base) + "; the game's has " + std::to_string(named->language[0].count) + " from id " +
             std::to_string(named->language[0].base) + " (English) or " + std::to_string(named->language[1].count) +
             " from id " + std::to_string(named->language[1].base) +
             " (Japanese). A voice bank keeps every sound of the bank it replaces, so each sound id means the same sound.";
    return nullptr;
  }
  *error = "This is not one of the game's fighter sound banks (" + std::to_string(layout.count) + " sounds from id " +
           std::to_string(layout.base) + ").";
  return nullptr;
}

// The bank as it is served: the file up to the end of its samples, the samples filled to a whole
// 32 byte block (the header says the filled size), then zeros up to `length`, the one length the
// file table carries for this disc file.
std::vector<uint8_t> served_bank(const std::vector<uint8_t>& bytes, const BankLayout& layout, uint32_t length) {
  std::vector<uint8_t> out(bytes.begin(), bytes.begin() + layout.data_offset + layout.data_size);
  out.resize((size_t)layout.data_offset + bank_sample_room(layout), 0);
  put_be32(out.data() + 4, bank_sample_room(layout));
  if (out.size() < length) out.resize(length, 0);
  return out;
}

ImportResult install_fighter_sound_locked(const fs::path& source, const std::string& member,
                                          const std::vector<uint8_t>& mod_bytes) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  size_t kind = 0; const char* character = nullptr;
  const std::string target = path_filename_utf8(source);
  uint32_t offset = 0, size = 0;
  if (!fighter_sound_identity(member.empty() ? target : member, &kind, &character) ||
      !host::disc_find_file(target, &offset, &size) || !size || size > kMaxAssetBytes) {
    result.message = "Choose a fighter animation archive such as PlCa.dat while the matching clean game disc is open.";
    return result;
  }
  std::vector<uint8_t> clean(size), safe;
  if (!host::disc_read(offset, clean.data(), size) || !fighter_sound_overlay(clean, mod_bytes, kind, &safe, &error)) {
    result.message = error.empty() ? "The fighter sound edits could not be validated against the active disc." : error;
    return result;
  }
  const std::string digest = sha256(safe);
  if (digest.empty()) { result.message = "The fighter sound edits could not be hashed."; return result; }
  const std::string id = "fighter-sound-" + digest;
  const std::string profile_key = target + "#sounds";
  auto existing = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& asset) {
    return asset.info.id == id && asset.info.kind == "fighter_sound";
  });
  const bool is_new = existing == g_assets.end();
  AssetRecord previous;
  fs::path stored = fs::path(L"assets") / fs::u8path(id) / fs::u8path(target);
  AssetRecord asset;
  if (!is_new) { previous = *existing; asset = *existing; }
  else {
    asset.info.id = id;
    asset.info.name = std::string(character) + " move sounds: " + path_filename_utf8(source);
    asset.info.kind = "fighter_sound";
    asset.info.target_path = target;
    asset.info.character = character;
    asset.info.costume = "Move sounds";
    asset.info.available = true;
    asset.info.sha256 = digest;
    asset.stored_path = stored.generic_u8string();
    asset.source_kind = "fighter_sound";
    asset.source_name = path_filename_utf8(source);
    asset.source_member = member;
    asset.source_id = id;
  }
  if (!write_atomic(g_root / fs::u8path(asset.stored_path), safe.data(), safe.size(), &error)) {
    result.message = error; return result;
  }
  if (is_new) g_assets.push_back(asset);
  else *existing = asset;
  if (!save_catalog_locked(&error)) {
    if (is_new) g_assets.pop_back(); else *existing = previous;
    result.message = error; return result;
  }
  const auto selected = g_profile.selections.find(profile_key);
  const bool auto_selected = selected == g_profile.selections.end();
  if (auto_selected) {
    Profile old = g_profile;
    g_profile.enabled = true; g_profile.selections[profile_key] = id; ++g_profile.generation;
    if (!save_profile_locked(&error)) {
      g_profile = std::move(old);
      result.message = "Sound edits were imported, but the selection could not be saved: " + error;
      return result;
    }
  }
  g_message = asset.info.name + (auto_selected ? " imported and selected for " : " imported for ") + target +
              "; only move sound fields were kept.";
  result.ok = true; result.asset_id = id; result.message = g_message;
  return result;
}

// ---- voice mods: import ----

bool check_voice(const std::vector<uint8_t>& ssm, const std::string& source_name, const std::string& slot_target,
                 VoiceCheck* out, std::string* error) {
  if (!parse_bank(ssm, &out->layout, error)) return false;
  out->bank = match_bank(out->layout, source_name, &out->language, error);
  if (!out->bank) return false;
  if (slot_target.empty()) return true;
  SlotName slot;
  if (!find_slot(slot_target, &slot) || family_bank(*slot.family) != out->bank) {
    *error = std::string(out->bank->file) + " is the sound bank of " + bank_fighters(*out->bank) + "; this skin is " +
             (find_slot(slot_target, &slot) ? std::string(slot.family->display_name) + "'s." : std::string("another fighter's."));
    return false;
  }
  return true;
}

// Gives an installed skin a sound bank, or replaces the one it has. The file is stored beside the
// skin's pictures; the record's "voice" entry names it.
ImportResult attach_voice_locked(const std::string& asset_id, const std::vector<uint8_t>& ssm,
                                 const std::string& source_name) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id && item.info.kind == "character_costume";
  });
  if (asset == g_assets.end()) { result.message = "A sound bank belongs to an installed skin; that skin is not in the list."; return result; }
  if (ssm.empty() || ssm.size() > kMaxAssetBytes) { result.message = "The sound bank is empty or larger than the 64 MB safety limit."; return result; }
  VoiceCheck check;
  if (!check_voice(ssm, source_name, asset->info.target_path, &check, &error)) {
    result.message = "Sound bank refused: " + error; return result;
  }
  const std::string digest = sha256(ssm);
  if (digest.empty()) { result.message = "The sound bank could not be hashed."; return result; }
  AssetRecord::Voice voice;
  voice.stored_path = (fs::path(L"assets") / fs::u8path(asset->info.id) / L"companions" / L"voice.ssm").generic_u8string();
  voice.sha256 = digest;
  voice.source_member = source_name;
  voice.bank = check.bank->file;
  const bool same = asset->voice.sha256 == digest;
  if (!write_atomic(g_root / fs::u8path(voice.stored_path), ssm.data(), ssm.size(), &error)) { result.message = error; return result; }
  const AssetRecord previous = *asset;
  const Profile previous_profile = g_profile;
  asset->voice = voice;
  asset->info.voice = voice.bank;
  asset->info.voice_source = voice.source_member;
  if (!same) ++g_profile.generation;   // the same skin, another voice
  if (!save_state_locked(&error)) { *asset = previous; g_profile = previous_profile; result.message = error; return result; }
  g_message = std::string(check.bank->file) + " (" + std::to_string(check.layout.count) + " sounds, fits the " +
              kBankLanguages[check.language] + " bank) is the voice of " + asset->info.name + ".";
  result.ok = true; result.already_present = same; result.asset_id = asset->info.id; result.message = g_message;
  return result;
}

// The installed skin a bank that came without a costume file belongs to: the selected skin of the
// costume `slot_hint` names, or with no hint the only selected skin among the bank's fighters, or
// the only installed one. Null with advice when that does not single one out.
const AssetRecord* voice_target_locked(const VoiceBank& bank, const std::string& slot_hint, std::string* error) {
  const auto selected_skin = [&](const std::string& slot) -> const AssetRecord* {
    const auto pick = g_profile.selections.find(slot);
    if (pick == g_profile.selections.end() || pick->second == kVanillaSelection) return nullptr;
    for (const auto& item : g_assets)
      if (item.info.id == pick->second && item.info.kind == "character_costume" && selection_key(item) == slot) return &item;
    return nullptr;
  };
  std::vector<const AssetRecord*> selected, installed;
  SlotName hinted;
  const bool has_hint = !slot_hint.empty() && find_slot(slot_hint, &hinted);
  if (has_hint && family_bank(*hinted.family) != &bank) {
    *error = std::string(bank.file) + " is the sound bank of " + bank_fighters(bank) + ", not of " +
             hinted.family->display_name + ".";
    return nullptr;
  }
  for (const auto& item : g_assets) {
    SlotName slot;
    if (item.info.kind != "character_costume" || !find_slot(item.info.target_path, &slot) ||
        family_bank(*slot.family) != &bank) continue;
    if (has_hint && lower(item.info.target_path) != lower(slot_file(hinted))) continue;
    installed.push_back(&item);
    if (selected_skin(item.info.target_path) == &item) selected.push_back(&item);
  }
  if (selected.size() == 1) return selected[0];
  if (selected.empty() && installed.size() == 1) return installed[0];
  const std::string who = has_hint ? std::string(hinted.family->display_name) + ", " + hinted.color->label : bank_fighters(bank);
  *error = installed.empty() ?
      std::string(bank.file) + " is a voice for " + who + ", and no skin is installed for it. A voice belongs to a skin: "
          "put the costume file in the same zip, or import the skin first." :
      std::string(bank.file) + " is a voice for " + who + ", and " + std::to_string(installed.size()) +
          " skins could take it. Name the file after the costume (for example \"Fox Green.ssm\"), or select the one skin it belongs to first.";
  return nullptr;
}

// A bank that came without a costume file. `slot_hint` is a costume slot ("PlFxGr.dat") when the
// manifest named one; otherwise the first of `names` (file names) that names a costume is the hint.
ImportResult attach_loose_voice_locked(const std::vector<uint8_t>& ssm, const std::string& source_name,
                                       std::string slot_hint, const std::vector<std::string>& names) {
  ImportResult result;
  std::string error;
  if (!mutable_profile_locked(&error)) { result.message = error; return result; }
  VoiceCheck check;
  if (!check_voice(ssm, source_name, {}, &check, &error)) { result.message = "Sound bank refused: " + error; return result; }
  for (size_t i = 0; slot_hint.empty() && i < names.size(); ++i) {
    std::string slot, kind;
    if (portrait_slot_from_name_impl(names[i], &slot, &kind)) slot_hint = slot;
  }
  const AssetRecord* target = voice_target_locked(*check.bank, slot_hint, &error);
  if (!target) { result.message = error; return result; }
  return attach_voice_locked(target->info.id, ssm, source_name);
}

}  // namespace cosmetic_internal

ImportResult import_voice(const std::string& ssm_path, const std::string& asset_id) {
  const fs::path path = fs::u8path(ssm_path);
  std::vector<uint8_t> bytes;
  std::string error;
  if (!read_bounded(path, kMaxAssetBytes, &bytes, &error)) return {false, false, {}, error};
  std::lock_guard<std::mutex> lock(g_mutex);
  ImportResult result = attach_voice_locked(asset_id, bytes, path_filename_utf8(path));
  if (result.ok && std::atomic_load(&g_runtime)->initialized) result.message += " Restart to apply it.";
  g_message = result.message;
  return result;
}

bool remove_voice(const std::string& asset_id, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) { return item.info.id == asset_id; });
  if (asset == g_assets.end() || asset->voice.stored_path.empty()) { *error = "That skin has no voice bank."; return false; }
  const AssetRecord previous = *asset;
  const Profile previous_profile = g_profile;
  const fs::path stored = g_root / fs::u8path(asset->voice.stored_path);
  asset->voice = AssetRecord::Voice{};
  asset->info.voice.clear(); asset->info.voice_source.clear();
  ++g_profile.generation;
  if (!save_state_locked(error)) { *asset = previous; g_profile = previous_profile; return false; }
  DeleteFileW(stored.c_str());
  g_message = asset->info.name + " uses the game's own voice again" +
              (std::atomic_load(&g_runtime)->initialized ? "; restart to apply it." : ".");
  return true;
}

// Voice banks, at startup only (docs/voice-mods.md). Every disc bank file that an installed skin has
// a fitting bank for becomes a served file: it starts as the disc's own bank, and plan_match_voices
// switches it between the disc's and the skins' at each match load. Every installed skin counts,
// selected or not, so a skin picked later on the character select screen brings its voice. The
// file table gets one length per bank file for the whole session, the longest of the disc's and
// the candidates', and everything served is padded to it: the entry never changes again, and a
// read that started on one bank and ends on another never runs past the file.
void publish_voices_locked(uint8_t* fst, const std::vector<FstFile>& files, RuntimeState* next) {
  struct Loaded {
    const AssetRecord* asset = nullptr; const VoiceBank* bank = nullptr;
    std::vector<uint8_t> bytes; BankLayout layout; bool used = false;
  };
  std::vector<Loaded> voices;
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "character_costume" || !asset.info.available || asset.voice.stored_path.empty()) continue;
    Loaded item;
    item.asset = &asset; item.bank = bank_by_file(asset.voice.bank);
    std::string error;
    SlotName slot;
    if (!item.bank || !find_slot(asset.info.target_path, &slot) || family_bank(*slot.family) != item.bank ||
        !safe_relative_path(asset.voice.stored_path) ||
        !read_bounded(g_root / fs::u8path(asset.voice.stored_path), kMaxAssetBytes, &item.bytes, &error) ||
        sha256(item.bytes) != asset.voice.sha256 || !parse_bank(item.bytes, &item.layout, &error)) {
      host::log("cosmetics: the voice bank of %s is missing or changed; the game's own sounds are used",
                asset.info.name.c_str());
      continue;
    }
    voices.push_back(std::move(item));
  }
  if (voices.empty()) return;
  for (const auto& file : files) {
    const std::string path = lower(file.path);
    const size_t slash = path.find_last_of('/');
    const VoiceBank* bank = bank_by_file(slash == std::string::npos ? path : path.substr(slash + 1));
    if (!bank || path.rfind("audio/", 0) != 0 || next->by_start.count(file.start)) continue;
    if (std::none_of(voices.begin(), voices.end(), [&](const Loaded& voice) { return voice.bank == bank; })) continue;
    if (file.size < 0x20 || file.size > kMaxAssetBytes) continue;
    std::vector<uint8_t> disc(file.size);
    BankLayout disc_layout;
    std::string error;
    if (!host::disc_read(file.start, disc.data(), file.size) || !parse_bank(disc, &disc_layout, &error)) {
      host::log("cosmetics: %s on this disc is not a bank the voice mods can replace (%s)", file.path.c_str(), error.c_str());
      continue;
    }
    VoiceFile served;
    served.start = file.start; served.disc_size = file.size; served.length = file.size;
    served.bank = bank->file; served.path = file.path; served.bank_index = bank->index;
    std::vector<Loaded*> fitting;
    for (auto& voice : voices) {
      // The same sounds as this disc file: count, first id and channels per sound, and samples
      // inside the room the game's loader budgets for the bank.
      if (voice.bank != bank || voice.layout.count != disc_layout.count || voice.layout.base != disc_layout.base ||
          first_channel_difference(disc_layout.channels, voice.layout.channels) >= 0 ||
          bank_sample_room(voice.layout) > bank->budget) continue;
      fitting.push_back(&voice);
      served.length = std::max<uint32_t>(served.length, voice.layout.data_offset + bank_sample_room(voice.layout));
    }
    if (fitting.empty()) continue;
    for (Loaded* voice : fitting) {
      served.candidates[voice->asset->info.id] =
          std::make_shared<const std::vector<uint8_t>>(served_bank(voice->bytes, voice->layout, served.length));
      voice->used = true;
      host::log("cosmetics: voice bank of %s fits %s (%u sounds from id %u)", voice->asset->info.name.c_str(),
                file.path.c_str(), disc_layout.count, disc_layout.base);
    }
    if (served.length != file.size) put_be32(fst + (size_t)file.index * 12 + 8, served.length);
    next->by_start[file.start] = RuntimeAsset{std::string(), bank->file, nullptr, file.size, true, kVoiceKind,
                                              std::string(), served.length};
    next->voice_files.push_back(std::move(served));
  }
  for (const auto& voice : voices)
    if (!voice.used)
      host::log("cosmetics: the voice bank of %s fits no %s on this disc (%u sounds from id %u); the game's own sounds are used",
                voice.asset->info.name.c_str(), voice.bank->file, voice.layout.count, voice.layout.base);
  if (!next->voice_files.empty())
    host::log("cosmetics: %zu sound bank files can be served by a skin's voice", next->voice_files.size());
}

// Fighter animation archives mix gameplay commands with audio commands. Copy only audio fields
// when importing a modded file, so changed timing, hitboxes, damage, and other gameplay data stay
// on the clean disc.
bool fighter_sound_identity(const std::string& raw, size_t* kind, const char** character) {
  static constexpr const char* codes[] = {
      "Mr", "Fx", "Ca", "Dk", "Kb", "Kp", "Lk", "Sk", "Ns", "Pe", "Pp", "Nn", "Pk", "Ss", "Ys", "Pr",
      "Mt", "Lg", "Ms", "Zd", "Cl", "Dr", "Fc", "Pc", "Gw", "Gn", "Fe", "Mh", "Ch", "Bo", "Gl", "Gk", "Sb"};
  static constexpr const char* names[] = {
      "Mario", "Fox", "Captain Falcon", "Donkey Kong", "Kirby", "Bowser", "Link", "Sheik", "Ness", "Peach",
      "Popo", "Nana", "Pikachu", "Samus", "Yoshi", "Jigglypuff", "Mewtwo", "Luigi", "Marth", "Zelda",
      "Young Link", "Dr. Mario", "Falco", "Pichu", "Mr. Game & Watch", "Ganondorf", "Roy", "Master Hand",
      "Crazy Hand", "Wireframe Male", "Wireframe Female", "Giga Bowser", "Sandbag"};
  const std::string name = lower(fs::path(raw).filename().string());
  if (name.size() != 8 || name.compare(0, 2, "pl") != 0 || name.compare(4, 4, ".dat") != 0) return false;
  for (size_t i = 0; i < std::size(codes); ++i) {
    if (name.compare(2, 2, lower(codes[i])) == 0) { *kind = i; *character = names[i]; return true; }
  }
  return false;
}

bool fighter_sound_overlay(const std::vector<uint8_t>& clean, const std::vector<uint8_t>& mod,
                                  size_t fighter_kind, std::vector<uint8_t>* merged, std::string* error) {
  static constexpr uint16_t motion_counts[] = {
      303,327,318,337,479,316,314,317,326,318,321,321,320,313,314,327,314,312,327,311,314,303,327,320,
      323,318,327,345,344,295,295,316,296};
  static const auto event_words = [] {
    std::array<uint8_t, 0x31> words{};
    words.fill(1);
    words[0] = 5; words[1] = 5; words[7] = 3; words[0x22 - 0x0A] = 3;
    words[0x26 - 0x0A] = 7; words[0x27 - 0x0A] = 4;
    words[0x36 - 0x0A] = 3; words[0x37 - 0x0A] = 3;
    words[0x38 - 0x0A] = 2; words[0x3A - 0x0A] = 4;
    return words;
  }();
  if (fighter_kind >= std::size(motion_counts) || clean.size() != mod.size() || clean.size() < 0x20 || !merged) {
    *error = "Fighter sound files do not match a supported archive."; return false;
  }
  const uint32_t data_size = be32(clean.data() + 4), relocs = be32(clean.data() + 8), roots = be32(clean.data() + 12),
                 refs = be32(clean.data() + 16);
  const uint64_t reloc_start = 0x20ull + data_size, root_start = reloc_start + 4ull * relocs,
                 strings = root_start + 8ull * (roots + (uint64_t)refs);
  if (strings > clean.size() || strings > mod.size() || be32(mod.data() + 4) != data_size ||
      be32(mod.data() + 8) != relocs || be32(mod.data() + 12) != roots || be32(mod.data() + 16) != refs ||
      !std::equal(clean.begin(), clean.begin() + 0x20, mod.begin()) ||
      !std::equal(clean.begin() + (ptrdiff_t)reloc_start, clean.end(), mod.begin() + (ptrdiff_t)reloc_start)) {
    *error = "Fighter sound archive layout changed; only same-layout sound edits can be imported."; return false;
  }
  const uint8_t* data = clean.data() + 0x20;
  const uint8_t* mod_data = mod.data() + 0x20;
  std::unordered_map<uint32_t, uint32_t> pointers;
  pointers.reserve(relocs);
  for (uint32_t i = 0; i < relocs; ++i) {
    const uint32_t at = be32(clean.data() + reloc_start + 4ull * i);
    if ((at & 3) || (uint64_t)at + 4 > data_size) { *error = "Fighter sound archive has an invalid relocation."; return false; }
    pointers.emplace(at, be32(data + at));
  }
  uint32_t ftdata = UINT32_MAX;
  for (uint32_t i = 0; i < roots; ++i) {
    const uint64_t entry = root_start + 8ull * i;
    const uint32_t object = be32(clean.data() + entry), name = be32(clean.data() + entry + 4);
    if ((uint64_t)strings + name + 6 >= clean.size() || object >= data_size) continue;
    if (std::memcmp(clean.data() + strings + name, "ftData", 6) == 0 &&
        clean[(size_t)strings + name + 6] == 0) ftdata = object;
  }
  auto ptr = [&](uint32_t at, uint32_t* out) {
    auto found = pointers.find(at);
    if (found == pointers.end() || found->second >= data_size) return false;
    *out = found->second; return true;
  };
  uint32_t motions = 0;
  if (ftdata == UINT32_MAX || (uint64_t)ftdata + 0x10 > data_size || !ptr(ftdata + 0x0C, &motions) || !motions ||
      (uint64_t)motions + (uint64_t)motion_counts[fighter_kind] * 0x18 > data_size) {
    *error = "Fighter sound archive has no bounded move-script table."; return false;
  }
  *merged = clean;
  bool changed = false;
  std::vector<uint32_t> queue;
  std::unordered_map<uint32_t, bool> visited;
  for (uint32_t i = 0; i < motion_counts[fighter_kind]; ++i) {
    uint32_t script = 0;
    if (ptr(motions + i * 0x18 + 0x0C, &script) && script) queue.push_back(script);
  }
  auto copy_word = [&](uint32_t at) {
    if (std::memcmp(merged->data() + 0x20 + at, mod_data + at, 4) == 0) return;
    std::memcpy(merged->data() + 0x20 + at, mod_data + at, 4); changed = true;
  };
  size_t visited_words = 0;
  while (!queue.empty()) {
    uint32_t at = queue.back(); queue.pop_back();
    while ((uint64_t)at + 4 <= data_size && visited.emplace(at, true).second) {
      if (++visited_words > 250000) { *error = "Fighter move scripts exceed the safe command limit."; return false; }
      const uint32_t word = be32(data + at), opcode = word >> 26;
      if (opcode == 0) break;
      uint32_t words = 1;
      if (opcode >= 10 && opcode <= 0x3A) words = event_words[opcode - 10];
      else if (opcode > 9) { *error = "Fighter move script contains an unsupported command."; return false; }
      else if (opcode == 5 || opcode == 7) words = 2;
      if ((uint64_t)at + 4ull * words > data_size) { *error = "Fighter move command exceeds its archive."; return false; }
      if (opcode == 0x0B) {             // hitbox creation: sound kind only
        const uint32_t clean_word = be32(data + at + 16), mod_word = be32(mod_data + at + 16);
        const uint32_t value = (clean_word & ~0x1Fu) | (mod_word & 0x1Fu);
        if (value != clean_word) { put_be32(merged->data() + 0x20 + at + 16, value); changed = true; }
      } else if (opcode == 0x22) {      // throw hitbox: sound kind only
        const uint32_t clean_word = be32(data + at + 8), mod_word = be32(mod_data + at + 8);
        const uint32_t value = (clean_word & ~0xFu) | (mod_word & 0xFu);
        if (value != clean_word) { put_be32(merged->data() + 0x20 + at + 8, value); changed = true; }
      } else if (opcode == 0x11) {      // sound ID, volume and pan
        copy_word(at + 4); copy_word(at + 8);
      } else if (opcode == 0x26) {      // six random sound IDs; leave RNG range and behavior alone
        for (uint32_t n = 1; n <= 6; ++n) copy_word(at + 4 * n);
      }
      if (opcode == 5 || opcode == 7) {
        const uint32_t target = be32(data + at + 4);
        if (pointers.find(at + 4) == pointers.end() || target >= data_size) {
          *error = "Fighter move script has an invalid branch target."; return false;
        }
        queue.push_back(target);
      }
      at += words * 4;
    }
  }
  if (!changed) { *error = "Fighter file has no supported move-sound changes."; return false; }
  return true;
}

// ---- voice mods: which bank a match hears (docs/voice-mods.md) ----

// The skin a costume slot serves right now, from the running snapshot: what the match will load for
// that costume. Empty for the standard costume, and for a skin that online play shows as the
// standard costume (its voice goes with it).
static std::string served_skin(const RuntimeState& runtime, const std::string& slot) {
  const std::string wanted = lower(slot);
  std::string id;
  for (const auto& entry : runtime.by_start) {
    const RuntimeAsset& file = entry.second;
    if (file.kind != "character_costume" || lower(file.target_path) != wanted) continue;
    if (!file.online_allowed && online_active()) return {};
    id = file.id;
  }
  return id;
}

// The other costume file a port's fighter also wears: Nana's for Popo's, and for Zelda and Sheik
// each other's of the same color (the two share a bank and a port).
static std::string voice_partner(const std::string& slot) {
  const std::string nana = climber_partner(slot);
  if (!nana.empty()) return nana;
  const std::string name = lower(slot);
  if (name.size() == 10 && name.compare(0, 4, "plzd") == 0) return "PlSk" + slot.substr(4);
  if (name.size() == 10 && name.compare(0, 4, "plsk") == 0) return "PlZd" + slot.substr(4);
  return {};
}

VoicePlan plan_match_voices(const MatchFighter ports[4]) {
  VoicePlan plan;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto current = std::atomic_load(&g_runtime);
  if (!ports || !current->initialized || current->voice_files.empty()) return plan;
  auto next = std::make_shared<RuntimeState>(*current);
  const auto skin_name = [&](const std::string& id) {
    if (id.empty()) return std::string("the game's own sounds");
    for (const auto& item : g_assets) if (item.info.id == id) return item.info.name;
    return id;
  };
  bool any = false;
  for (auto& file : next->voice_files) {
    // The voice a port's costume asks for on this bank file: its skin's bank, else its partner
    // costume's, else the game's own (also when the skin's bank fits the other language only).
    const auto wanted_by = [&](const std::string& slot) {
      const std::string own = served_skin(*next, slot);
      if (!own.empty() && file.candidates.count(own)) return own;
      const std::string partner = voice_partner(slot);
      const std::string other = partner.empty() ? std::string() : served_skin(*next, partner);
      return !other.empty() && file.candidates.count(other) ? other : std::string();
    };
    int decider = -1;
    std::string wanted;
    for (int port = 0; port < 4; ++port) {
      const std::string slot = costume_slot_file(ports[port].character, ports[port].costume);
      SlotName known;
      if (slot.empty() || !find_slot(slot, &known)) continue;
      const VoiceBank* bank = family_bank(*known.family);
      if (!bank || file.bank != bank->file) continue;
      const std::string asks = wanted_by(slot);
      if (decider < 0) { decider = port; wanted = asks; continue; }
      if (asks == wanted) continue;
      // One bank per fighter file: the lowest port decided, this one is told.
      plan.notes.push_back(asks.empty() ?
          "voice: port " + std::to_string(port + 1) + "'s costume gets port " + std::to_string(decider + 1) + "'s voice (" +
              skin_name(wanted) + "): the game loads one " + file.bank :
          "voice: port " + std::to_string(port + 1) + "'s " + skin_name(asks) + " is not used: port " +
              std::to_string(decider + 1) + " decides " + file.bank + " (" + skin_name(wanted) + ")");
    }
    if (decider < 0) continue;   // no fighter of this bank in the match: it keeps what it serves
    if (wanted == file.chosen) continue;
    plan.notes.push_back("voice: " + file.path + " is " + skin_name(wanted) + " for this match (port " +
                         std::to_string(decider + 1) + ")");
    file.chosen = wanted;
    RuntimeAsset& served = next->by_start[file.start];
    served.id = wanted;
    served.bytes = wanted.empty() ? nullptr : file.candidates[wanted];
    any = true;
    if (std::none_of(plan.changed.begin(), plan.changed.end(), [&](const VoiceChange& have) { return have.file == file.bank; }))
      plan.changed.push_back({file.bank, file.bank_index});
  }
  if (any) std::atomic_store(&g_runtime, std::shared_ptr<const RuntimeState>(next));
  for (const auto& note : plan.notes) host::log("cosmetics: %s", note.c_str());
  return plan;
}

uint32_t voice_bank_count() { return (uint32_t)std::atomic_load(&g_runtime)->voice_files.size(); }

}  // namespace host::cosmetics
