// Cosmetic live picks: a costume slot's skin changed during play, Ice Climbers pairs, session
// packs, imports as costume slots, the character select cycles.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"

namespace host::cosmetics {

// The character select's fighter numbers, in its own order, as the families' file codes.
std::string costume_slot_file(int css_character, int costume) {
  static constexpr const char* codes[] = {"Ca", "Dk", "Fx", "Gw", "Kb", "Kp", "Lk", "Lg", "Mr", "Ms", "Mt", "Ns", "Pe",
                                          "Pk", "Pp", "Pr", "Ss", "Ys", "Zd", "Sk", "Fc", "Cl", "Dr", "Fe", "Pc", "Gn"};
  if (css_character < 0 || css_character >= (int)std::size(codes) || costume < 0) return {};
  for (const auto& family : families) {
    if (std::string(family.file_code) != codes[css_character]) continue;
    const std::string colors = family.colors;   // "Nr Or La Gr": the game's costume order
    const size_t at = (size_t)costume * 3;
    if (at + 2 > colors.size()) return {};
    return std::string("Pl") + family.file_code + colors.substr(at, 2) + ".dat";
  }
  return {};
}

// The online verdict of a skin that is about to be picked live. A disc skin was judged when it was
// scanned; an import is compared with the standard costume here, the way startup does it.
static bool live_online_ok_locked(const AssetRecord& asset, const std::vector<uint8_t>& candidate) {
  if (asset.online_known) return asset.online_ok;
  std::string detail;
  return costume_online_with_open_disc(asset, candidate, &detail);
}

static bool select_live_locked(const std::string& slot, const std::string& asset_id, std::string* error) {
  if (!ready_locked(error)) return false;
  // Between the games of a set the player may pick another skin, as they may pick another color
  // (the select screen itself refuses any change once they are locked in).
  if (kFreezeSkinPicksOnline && g_online_freezes.load(std::memory_order_relaxed)) {
    *error = "Skins cannot change while an online match is queued or running."; return false;
  }
  SlotName known;
  if (!find_slot(slot, &known)) { *error = "That is not a costume slot."; return false; }
  const std::string target = slot_file(known);
  const bool standard = asset_id.empty() || asset_id == kVanillaSelection;
  std::string name = "the standard costume";
  if (!standard) {
    auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == asset_id && item.info.kind == "character_costume" && selection_key(item) == target;
    });
    if (asset == g_assets.end()) { *error = "The selected variant does not belong to that slot."; return false; }
    const bool online = online_active();
    // A scanned skin known to be off online is refused before its file is read.
    if (online && asset->online_known && !asset->online_ok) {
      *error = "That skin is off online (" + asset->online_note + ")."; return false;
    }
    std::string validation_error;
    const std::vector<uint8_t> candidate = load_runtime_asset_locked(*asset, &validation_error);
    asset->info.available = !candidate.empty();
    if (!asset->info.available) {
      asset->info.availability_message = validation_error;
      *error = "The selected variant is unavailable or invalid; the costume is unchanged."; return false;
    }
    if (online && !live_online_ok_locked(*asset, candidate)) {
      *error = "That skin cannot be made safe to play online (its bones do not line up with the standard costume's)."; return false;
    }
    name = asset->info.name;
  }
  const auto current = g_profile.selections.find(target);
  const std::string now = current == g_profile.selections.end() ? std::string(kVanillaSelection) : current->second;
  if (standard ? now == kVanillaSelection : (now == asset_id && g_profile.enabled)) return true;
  if (!g_live_unpublished) {
    const auto runtime = std::atomic_load(&g_runtime);
    g_live_in_step = runtime->initialized && runtime->fingerprint == desired_fingerprint_locked();
  }
  Profile previous = g_profile;
  if (standard) g_profile.selections[target] = kVanillaSelection;
  else { g_profile.enabled = true; g_profile.selections[target] = asset_id; }
  ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = std::move(previous); return false; }
  g_live_unpublished = true;
  g_message = name + " selected for " + target + ".";
  return true;
}

bool select_variant_live(const std::string& slot, const std::string& asset_id, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  return select_live_locked(slot, asset_id, error);
}

// Nana's costume slot for one of Popo's (the same place in each one's costume order), or empty.
std::string climber_partner(const std::string& target) {
  static constexpr const char* popo[] = {"PlPpNr.dat", "PlPpGr.dat", "PlPpOr.dat", "PlPpRe.dat"};
  static constexpr const char* nana[] = {"PlNnNr.dat", "PlNnYe.dat", "PlNnAq.dat", "PlNnWh.dat"};
  for (size_t i = 0; i < std::size(popo); ++i) if (lower(target) == lower(popo[i])) return nana[i];
  return {};
}

// The skin for `partner_target` that belongs with `skin`: the same pack and the same set of it, or
// for a vault import the skin the vault itself names as the pair. Null when there is none.
static const AssetRecord* partner_skin_locked(const std::string& skin_id, const std::string& partner_target) {
  if (skin_id.empty()) return nullptr;
  const auto skin = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == skin_id && item.info.kind == "character_costume";
  });
  if (skin == g_assets.end()) return nullptr;
  for (const auto& other : g_assets) {
    if (other.info.kind != "character_costume" || !other.info.available || selection_key(other) != partner_target ||
        other.source_kind != skin->source_kind) continue;
    if (skin->source_kind == kDiscSource) {
      if (other.source_id == skin->source_id && other.info.variant == skin->info.variant) return &other;
      continue;
    }
    if (other.source_name != skin->source_name || other.source_id.empty() || skin->source_id.empty()) continue;
    const auto names = [](const AssetRecord& a, const AssetRecord& b) {
      return std::find(a.info.dependencies.begin(), a.info.dependencies.end(), b.source_id) != a.info.dependencies.end();
    };
    if (names(*skin, other) || names(other, *skin)) return &other;
  }
  return nullptr;
}

// The other costume a fighter can use after a transformation or with Popo.
static std::string live_costume_partner(const std::string& target) {
  const std::string name = lower(target);
  if (name.size() == 10 && name.compare(0, 4, "plzd") == 0) return "PlSk" + target.substr(4);
  if (name.size() == 10 && name.compare(0, 4, "plsk") == 0) return "PlZd" + target.substr(4);
  return climber_partner(target);
}

static void pair_costumes_locked(const std::string& target, LiveCycle* result) {
  const std::string partner = live_costume_partner(target);
  if (partner.empty()) return;
  const auto picked = g_profile.selections.find(partner);
  const std::string wears = picked == g_profile.selections.end() || picked->second == kVanillaSelection ?
      std::string() : picked->second;
  const AssetRecord* before = partner_skin_locked(result->previous_id, partner);
  const AssetRecord* after = partner_skin_locked(result->asset_id, partner);
  // Update only a partner still wearing the previous automatic pick or its standard costume.
  if (wears != (before ? before->info.id : std::string())) return;
  const std::string message = g_message;
  std::string wanted = after ? after->info.id : std::string(), error;
  bool changed = wanted != wears && select_live_locked(partner, wanted, &error);
  // Her partner skin cannot be picked (online, or its file is gone): not left in the old pair's skin.
  if (!changed && !wanted.empty() && !wears.empty()) { wanted.clear(); changed = select_live_locked(partner, wanted, &error); }
  g_message = message;
  if (!changed) return;
  result->partner_slot = partner;
  result->partner_previous_id = wears;
}

// A skin scanned from the disc this session serves as its own files.
static bool session_pack_record(const AssetRecord& asset) {
  return !g_session_pack.empty() && asset.source_kind == kDiscSource && asset.source_id == g_session_pack;
}

// ---- installed costumes as costume slots (cosmetic_mods.h, host/costume_slots.h) ----
static std::atomic<bool> g_import_slots{false};
void set_imports_as_costume_slots(bool on) { g_import_slots.store(on); }
bool imports_as_costume_slots() { return g_import_slots.load(); }
// An installed costume (an import) while those are costume slots: L / R leave it to the slots.
static bool costume_slot_record(const AssetRecord& asset) {
  return g_import_slots.load() && asset.info.kind == "character_costume" && asset.source_kind != kDiscSource;
}

uint32_t release_imported_selections(std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return 0;
  const Profile previous = g_profile;
  uint32_t released = 0;
  for (auto& [target, id] : g_profile.selections) {
    if (id == kVanillaSelection) continue;
    const auto asset = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
      return item.info.id == id && item.info.kind == "character_costume" && item.source_kind != kDiscSource;
    });
    if (asset == g_assets.end()) continue;
    host::log("cosmetics: %s (%s) is a costume slot of its own now; %s shows its standard costume again",
              asset->info.name.c_str(), id.c_str(), target.c_str());
    id = kVanillaSelection;
    ++released;
  }
  if (!released) return 0;
  ++g_profile.generation;
  if (!save_profile_locked(error)) { g_profile = previous; return 0; }
  return released;
}

std::string partner_skin(const std::string& skin_id, const std::string& partner_target) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const AssetRecord* partner = partner_skin_locked(skin_id, partner_target);
  return partner ? partner->info.id : std::string();
}

void set_session_pack(const std::string& iso_path) {
  std::error_code ec;
  const std::string key = iso_path.empty() ? std::string() : fs::absolute(fs::u8path(iso_path), ec).u8string();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_session_pack = key;
}

void set_session_stage_pack(const std::string& iso_path) {
  std::error_code ec;
  const std::string key = iso_path.empty() ? std::string() : fs::absolute(fs::u8path(iso_path), ec).u8string();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_session_stage_pack = key;
}
void set_stage_button_probe(StageButtonProbe probe) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stage_buttons = probe;
}
void set_lockstep_probe(LockstepProbe probe) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_lockstep = probe;
}
std::string disc_pack_name(const std::string& iso_path) {
  std::error_code ec;
  const std::string key = fs::absolute(fs::u8path(iso_path), ec).u8string();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& asset : g_assets)
    if (asset.source_kind == kDiscSource && asset.source_id == key) return asset.source_name;
  return {};
}

bool session_pack_skin(const std::string& asset_id) {
  if (asset_id.empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return std::any_of(g_assets.begin(), g_assets.end(), [&](const AssetRecord& item) {
    return item.info.id == asset_id && item.info.kind == "character_costume" && session_pack_record(item);
  });
}

LiveCycle cycle_slot_live(const std::string& slot, int direction) {
  LiveCycle result;
  std::lock_guard<std::mutex> lock(g_mutex);
  SlotName known;
  if (!ready_locked(&result.message)) return result;
  if (!find_slot(slot, &known)) { result.message = "That is not a costume slot."; return result; }
  const std::string target = slot_file(known);
  // The standard costume, then the slot's skins in catalog order. The session pack's plain costume
  // is left out: this session already serves it as the standard costume (set_session_pack).
  std::vector<std::string> ids{std::string()}, names{"Standard"};
  for (const auto& asset : g_assets) {
    if (asset.info.kind != "character_costume" || !asset.info.available || selection_key(asset) != target) continue;
    if (asset.info.variant.empty() && session_pack_record(asset)) continue;
    if (costume_slot_record(asset)) continue;   // a costume slot of its own on the character select
    ids.push_back(asset.info.id);
    // A pack's skin is labelled by the pack and its set; an import by the name the player gave it.
    names.push_back(asset.source_kind == kDiscSource ?
        asset.source_name + (asset.info.variant.empty() ? "" : " (" + asset.info.variant + ")") : asset.info.name);
  }
  size_t at = 0;
  const auto picked = g_profile.selections.find(target);
  if (g_profile.enabled && picked != g_profile.selections.end())
    for (size_t i = 1; i < ids.size(); ++i) if (ids[i] == picked->second) at = i;
  result.previous_id = ids[at];
  result.asset_id = ids[at]; result.name = names[at];
  result.ok = true;
  const size_t count = ids.size();
  for (size_t step = 1; step < count; ++step) {
    const size_t next = direction > 0 ? (at + step) % count : (at + count - step) % count;
    std::string error;
    if (!select_live_locked(target, ids[next], &error)) { result.message = error; continue; }
    result.changed = true; result.asset_id = ids[next]; result.name = names[next]; result.message.clear();
    break;
  }
  if (!result.changed && result.message.empty()) result.message = "No other skin is installed for this costume.";
  if (result.changed) pair_costumes_locked(target, &result);
  return result;
}

CharacterCycle cycle_character_live(int character, int costume, int direction, uint32_t allowed_costumes) {
  CharacterCycle result;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ready_locked(&result.pick.message)) return result;
  if (online_active()) {
    result.pick.message = "Online skins cycle within the selected color.";
    return result;
  }
  if (costume < 0 || costume_slot_file(character, 0).empty()) return result;
  struct Choice { int costume; std::string target, id, name; };
  std::vector<Choice> choices;
  size_t current = 0;
  bool any_skin = false;
  for (int color = 0; color < 32; ++color) {
    const std::string slot = costume_slot_file(character, color);
    if (slot.empty()) break;
    if (!(allowed_costumes & (1u << color))) continue;
    SlotName known;
    find_slot(slot, &known);
    const auto selected = g_profile.selections.find(slot);
    const std::string selected_id = g_profile.enabled && selected != g_profile.selections.end() &&
        selected->second != kVanillaSelection ? selected->second : std::string();
    if (color == costume) current = choices.size();
    choices.push_back({color, slot, {}, std::string("Standard ") + known.color->label});
    for (const auto& asset : g_assets) {
      if (asset.info.kind != "character_costume" || !asset.info.available || selection_key(asset) != slot ||
          (asset.info.variant.empty() && session_pack_record(asset)) || costume_slot_record(asset)) continue;
      if (color == costume && selected_id == asset.info.id) current = choices.size();
      choices.push_back({color, slot, asset.info.id, asset.source_kind == kDiscSource ?
          asset.source_name + (asset.info.variant.empty() ? "" : " (" + asset.info.variant + ")") : asset.info.name});
      any_skin = true;
    }
  }
  result.pick.ok = true;
  if (!any_skin || choices.empty()) {
    result.pick.message = "No other skin is installed for this fighter.";
    return result;
  }
  for (size_t step = 1; step < choices.size(); ++step) {
    const auto& next = choices[direction > 0 ? (current + step) % choices.size() :
        (current + choices.size() - step) % choices.size()];
    std::string error;
    const auto before = g_profile.selections.find(next.target);
    const std::string previous = g_profile.enabled && before != g_profile.selections.end() &&
        before->second != kVanillaSelection ? before->second : std::string();
    if (!select_live_locked(next.target, next.id, &error)) { result.pick.message = error; continue; }
    result.target = next.target;
    result.costume = next.costume;
    result.pick.changed = true;
    result.pick.previous_id = previous;
    result.pick.asset_id = next.id;
    result.pick.name = next.name;
    result.pick.message.clear();
    pair_costumes_locked(next.target, &result.pick);
    break;
  }
  return result;
}

}  // namespace host::cosmetics
