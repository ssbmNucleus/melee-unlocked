// Effect files served slot by slot (effect_slots.h, cosmetic_mods.h "an effect file's slots"): each
// slot of a fighter's effect file (Falcon Punch, Falcon Kick, ...) has one source, an installed
// effect that changes it or the disc's own. Effects that keep the disc file's layout combine byte by
// byte along the slots' bytes; one laid out anew is served whole ("All effects"). The profile keeps the
// sources (Profile::effect_slots); the file's selection ("EfCaData.dat#effects") names one of them so
// the publish finds the file, and is vanilla when the disc's own file is served.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmetic_mods_internal.h"
#include "effect_slots.h"

#include <list>

namespace host::cosmetics {
namespace cosmetic_internal {
namespace {

using effect_slots::kAllSlots;
using Sources = std::map<std::string, std::string>;   // slot -> effect id

// The disc's own file, read from the open disc.
bool clean_effect_file(const std::string& target, std::vector<uint8_t>* clean) {
  uint32_t offset = 0, size = 0;
  if (!host::disc_find_file(target, &offset, &size) || !size || size > kMaxAssetBytes) return false;
  clean->resize(size);
  return host::disc_read(offset, clean->data(), size);
}

// The slot maps of the disc's files, made once per file (and disc).
struct CachedLayout { std::string target; uint32_t crc = 0; size_t size = 0; bool ok = false; effect_slots::Layout layout; };
std::list<CachedLayout> g_layouts;
const effect_slots::Layout* layout_of(const std::string& target, const std::vector<uint8_t>& clean) {
  if (effect_slots::table_slots(target).empty()) return nullptr;
  const uint32_t crc = crc32(clean.data(), clean.size());
  for (const auto& c : g_layouts)
    if (lower(c.target) == lower(target) && c.crc == crc && c.size == clean.size()) return c.ok ? &c.layout : nullptr;
  CachedLayout made;
  made.target = target; made.crc = crc; made.size = clean.size();
  std::string error;
  made.ok = effect_slots::map_slots(target, clean, &made.layout, &error);
  if (!made.ok) host::log("cosmetics: the slots of %s could not be mapped (%s)", target.c_str(), error.c_str());
  g_layouts.push_back(std::move(made));
  return g_layouts.back().ok ? &g_layouts.back().layout : nullptr;
}

// What an installed effect changes against the disc's file, worked out once per file and disc.
std::map<std::string, std::pair<bool, effect_slots::Diff>> g_diffs;
bool diff_of(const AssetRecord& asset, const effect_slots::Layout& layout, const std::vector<uint8_t>& clean,
             effect_slots::Diff* out) {
  const std::string key = asset.info.sha256 + "|" + lower(layout.target) + "|" +
                          std::to_string(crc32(clean.data(), clean.size()));
  const auto hit = g_diffs.find(key);
  if (hit != g_diffs.end()) { *out = hit->second.second; return hit->second.first; }
  std::string error;
  const std::vector<uint8_t> bytes = load_runtime_asset_locked(asset, &error);
  if (bytes.empty()) {   // (not kept: the file may be readable again later)
    host::log("cosmetics: effect %s could not be read for its slots (%s)", asset.info.name.c_str(), error.c_str());
    return false;
  }
  const bool ok = effect_slots::diff(layout, clean, bytes, out, &error);
  if (!ok) host::log("cosmetics: effect %s: its slots could not be found (%s)", asset.info.name.c_str(), error.c_str());
  else
    host::log("cosmetics: effect %s changes %zu slot(s) of %s%s%s", asset.info.name.c_str(), out->changed.size(),
              layout.target.c_str(), out->keeps_layout ? " (the disc file's layout)" : " (laid out anew: ",
              out->keeps_layout ? "" : (out->relaid + ")").c_str());
  g_diffs[key] = {ok, *out};
  return ok;
}

const AssetRecord* asset_by_id(const std::string& id) {
  const auto it = std::find_if(g_assets.begin(), g_assets.end(), [&](const AssetRecord& a) { return a.info.id == id; });
  return it == g_assets.end() ? nullptr : &*it;
}

// The installed effects of a file, newest first.
std::vector<const AssetRecord*> effects_of(const std::string& target) {
  std::vector<const AssetRecord*> out;
  for (auto it = g_assets.rbegin(); it != g_assets.rend(); ++it)
    if (standalone_effect(*it) && lower(it->info.target_path) == lower(target)) out.push_back(&*it);
  return out;
}

std::string selection_of(const std::string& target) { return target + "#effects"; }

const Sources* stored_sources(const Profile& profile, const std::string& target) {
  for (const auto& [file, sources] : profile.effect_slots)
    if (lower(file) == lower(target)) return &sources;
  return nullptr;
}

// The file's sources now: the profile's, else (a profile from before slots) the selected effect's.
Sources sources_of(const Profile& profile, const effect_slots::Layout& layout, const std::vector<uint8_t>& clean) {
  if (const Sources* stored = stored_sources(profile, layout.target); stored && !stored->empty()) return *stored;
  const auto pick = profile.selections.find(selection_of(layout.target));
  if (pick == profile.selections.end() || pick->second == kVanillaSelection) return {};
  const AssetRecord* asset = asset_by_id(pick->second);
  effect_slots::Diff d;
  if (!asset || !standalone_effect(*asset) || !diff_of(*asset, layout, clean, &d)) return {};
  Sources out;
  if (!d.keeps_layout) out[kAllSlots] = asset->info.id;
  else for (const auto& slot : d.changed) out[slot] = asset->info.id;
  return out;
}

// `id` becomes the source of the slots `d` changes (every slot, laid out anew). Per effect that loses
// slots, the slots it loses.
std::map<std::string, std::vector<std::string>> take(Sources* s, const std::string& id, const effect_slots::Diff& d) {
  std::map<std::string, std::vector<std::string>> lost;
  if (!d.keeps_layout) {
    for (const auto& [slot, source] : *s)
      if (source != id) lost[source].push_back(slot);
    s->clear();
    (*s)[kAllSlots] = id;
    return lost;
  }
  const auto whole = s->find(kAllSlots);
  if (whole != s->end()) {
    if (whole->second != id) lost[whole->second].push_back(kAllSlots);
    s->erase(whole);
  }
  for (const auto& slot : d.changed) {
    const auto had = s->find(slot);
    if (had != s->end() && had->second != id) lost[had->second].push_back(slot);
    (*s)[slot] = id;
  }
  return lost;
}

// The file's selection follows its slots: one of their effects (`prefer` when it is one), else vanilla.
void store(Profile* profile, const std::string& target, Sources sources, const std::string& prefer = {}) {
  for (auto it = profile->effect_slots.begin(); it != profile->effect_slots.end();)
    it = lower(it->first) == lower(target) ? profile->effect_slots.erase(it) : std::next(it);
  const std::string key = selection_of(target);
  if (sources.empty()) {
    const auto pick = profile->selections.find(key);
    if (pick != profile->selections.end()) pick->second = kVanillaSelection;
    return;
  }
  std::string marker = sources.begin()->second;
  for (const auto& source : sources)
    if (source.second == prefer) marker = prefer;
  profile->selections[key] = marker;
  profile->effect_slots[target] = std::move(sources);
}

void order_by_layout(std::vector<std::string>* slots, const effect_slots::Layout& layout) {
  std::sort(slots->begin(), slots->end(), [&](const std::string& a, const std::string& b) {
    return layout.slot_index(a) < layout.slot_index(b);   // ("All effects" is -1: first)
  });
}

}  // namespace

bool standalone_effect(const AssetRecord& asset) {
  return asset.info.kind == "effect_visual" && asset.source_kind == "standalone_effect";
}

bool compose_effect_file_locked(const Profile& profile, const std::string& target, const std::vector<uint8_t>& clean,
                                std::vector<uint8_t>* out, size_t* effects, std::string* error) {
  const std::vector<const AssetRecord*> choices = selected_effects_locked(profile, target);
  const effect_slots::Layout* layout = layout_of(target, clean);
  const Sources sources = layout ? sources_of(profile, *layout, clean) : Sources{};
  if (sources.empty()) {
    if (effects) *effects = choices.size();
    return compose_effects_locked(target, choices, clean, out, error);
  }
  // Each source once: checked as every effect is (materialize_effect_dat_impl), and laid out as the
  // disc's file unless it is served whole.
  std::map<std::string, std::vector<uint8_t>> files;
  const auto file_of = [&](const std::string& id, bool whole) -> const std::vector<uint8_t>* {
    const auto have = files.find(id);
    if (have != files.end()) return &have->second;
    const AssetRecord* asset = asset_by_id(id);
    if (!asset) { *error = "An effect chosen for " + target + " is no longer installed."; return nullptr; }
    effect_slots::Diff d;
    if (!whole && (!diff_of(*asset, *layout, clean, &d) || !d.keeps_layout)) {
      *error = asset->info.name + " is not laid out as the game's " + target + "."; return nullptr;
    }
    std::vector<uint8_t> candidate = load_runtime_asset_locked(*asset, error), served;
    std::string classification;
    if (candidate.empty() || !materialize_effect_dat_impl(target, clean, candidate, &served, &classification, error))
      return nullptr;
    return &(files[id] = std::move(served));
  };
  std::vector<uint8_t> base;
  const auto whole = sources.find(kAllSlots);
  if (whole != sources.end()) {
    const std::vector<uint8_t>* file = file_of(whole->second, true);
    if (!file) return false;
    base = *file;
  } else {
    std::vector<const std::vector<uint8_t>*> per(layout->slots.size(), nullptr);
    for (const auto& [slot, id] : sources) {
      const int at = layout->slot_index(slot);
      if (at < 0) continue;   // (a slot of an older table)
      if (!(per[size_t(at)] = file_of(id, false))) return false;
    }
    if (!effect_slots::compose(*layout, clean, per, &base, error)) return false;
    // The slots of different effects together: their banks still hold together (they are the
    // disc's tables with each effect's own bytes, so they do).
    data_safety::Archive archive(base);
    std::string defect;
    if (base != clean && (!archive.parse() || !data_safety::effect_banks_valid(archive, &defect))) {
      *error = target + " made from its slots does not hold together (" + defect + ")."; return false;
    }
  }
  // An older effect of one move (a project's "EfFxData.dat#upb") is merged on top, as before.
  std::vector<const AssetRecord*> rest;
  for (const AssetRecord* choice : choices)
    if (!standalone_effect(*choice)) rest.push_back(choice);
  if (effects) *effects = files.size() + rest.size();
  if (rest.empty()) { *out = std::move(base); return true; }
  std::vector<uint8_t> over;
  if (!compose_effects_locked(target, rest, clean, &over, error)) return false;
  if (over.size() != clean.size() || base.size() != clean.size()) {
    *error = "The effects chosen for " + target + " cannot be combined."; return false;
  }
  for (size_t i = 0; i < clean.size(); ++i) {
    if (over[i] == clean[i]) continue;
    if (base[i] != clean[i] && base[i] != over[i]) {
      *error = "Conflict: two effects chosen for " + target + " change the same bytes."; return false;
    }
    base[i] = over[i];
  }
  *out = std::move(base);
  return true;
}

bool assign_effect_locked(Profile* profile, const AssetRecord& asset, std::string* error) {
  if (!standalone_effect(asset)) return false;
  std::vector<uint8_t> clean;
  const effect_slots::Layout* layout = nullptr;
  effect_slots::Diff d;
  if (!clean_effect_file(asset.info.target_path, &clean) || !(layout = layout_of(asset.info.target_path, clean)) ||
      !diff_of(asset, *layout, clean, &d)) {
    if (error) *error = "no slots for " + asset.info.target_path;
    return false;
  }
  Sources sources = sources_of(*profile, *layout, clean);
  const auto lost = take(&sources, asset.info.id, d);
  for (const auto& [id, slots] : lost) {
    const AssetRecord* other = asset_by_id(id);
    host::log("cosmetics: %s takes %zu slot(s) of %s from %s", asset.info.name.c_str(), slots.size(),
              layout->target.c_str(), other ? other->info.name.c_str() : id.c_str());
  }
  store(profile, layout->target, std::move(sources), asset.info.id);
  // An older effect of one move of the file goes back to the disc's own (it would not merge).
  for (auto& pick : profile->selections)
    if (pick.second != kVanillaSelection && pick.first != selection_of(layout->target) &&
        lower(pick.first).rfind(lower(layout->target) + "#", 0) == 0)
      pick.second = kVanillaSelection;
  return true;
}

bool release_effect_locked(Profile* profile, const AssetRecord& asset) {
  if (!standalone_effect(asset)) return false;
  std::vector<uint8_t> clean;
  const effect_slots::Layout* layout = nullptr;
  if (!clean_effect_file(asset.info.target_path, &clean) || !(layout = layout_of(asset.info.target_path, clean)))
    return false;
  Sources sources = sources_of(*profile, *layout, clean);
  std::vector<std::string> freed;
  bool whole = false;
  for (auto it = sources.begin(); it != sources.end();) {
    if (it->second != asset.info.id) { ++it; continue; }
    if (it->first == kAllSlots) whole = true;
    else freed.push_back(it->first);
    it = sources.erase(it);
  }
  if (freed.empty() && !whole) return false;
  if (whole) freed = layout->slots;
  // Each freed slot: the newest other installed effect (of the file's layout) that changes it.
  const auto others = effects_of(layout->target);
  for (const auto& slot : freed)
    for (const AssetRecord* other : others) {
      effect_slots::Diff d;
      if (other->info.id == asset.info.id || !other->info.available || !diff_of(*other, *layout, clean, &d) ||
          !d.keeps_layout || std::find(d.changed.begin(), d.changed.end(), slot) == d.changed.end())
        continue;
      sources[slot] = other->info.id;
      host::log("cosmetics: %s of %s goes to %s", slot.c_str(), layout->target.c_str(), other->info.name.c_str());
      break;
    }
  // None: the newest one laid out anew, whole.
  if (sources.empty())
    for (const AssetRecord* other : others) {
      effect_slots::Diff d;
      if (other->info.id == asset.info.id || !other->info.available || !diff_of(*other, *layout, clean, &d)) continue;
      if (!d.keeps_layout) sources[kAllSlots] = other->info.id;
      break;
    }
  store(profile, layout->target, std::move(sources));
  return true;
}

void prune_effect_slots_locked(Profile* profile) {
  std::vector<std::string> files;
  for (const auto& entry : profile->effect_slots) files.push_back(entry.first);
  for (const auto& file : files) {
    Sources sources = profile->effect_slots[file];
    for (auto it = sources.begin(); it != sources.end();) {
      const AssetRecord* asset = asset_by_id(it->second);
      const bool ok = asset && standalone_effect(*asset) && asset->info.available &&
                      lower(asset->info.target_path) == lower(file);
      it = ok ? std::next(it) : sources.erase(it);
    }
    const auto pick = profile->selections.find(selection_of(file));
    store(profile, file, std::move(sources), pick == profile->selections.end() ? std::string() : pick->second);
  }
}

void forget_effect_slots_locked(Profile* profile, const std::string& target) {
  for (auto it = profile->effect_slots.begin(); it != profile->effect_slots.end();)
    it = lower(it->first) == lower(target) ? profile->effect_slots.erase(it) : std::next(it);
}

bool effect_would_take_locked(const std::string& target, const std::vector<uint8_t>& bytes, const std::string& digest,
                              std::vector<std::string>* slots, bool* whole, std::vector<EffectTaken>* taken) {
  slots->clear(); taken->clear(); *whole = true;
  std::vector<uint8_t> clean;
  if (!clean_effect_file(target, &clean)) return false;
  std::string error;
  const bool catalog = ready_locked(&error);
  // Its id when these very bytes are installed for this file already (it takes nothing from itself).
  std::string id = "(this file)";
  if (catalog)
    for (const auto& a : g_assets)
      if (standalone_effect(a) && a.info.sha256 == digest && lower(a.info.target_path) == lower(target)) id = a.info.id;
  const effect_slots::Layout* layout = layout_of(target, clean);
  effect_slots::Diff d;
  if (!layout || !effect_slots::diff(*layout, clean, bytes, &d, &error)) {
    // No slot table: one choice per file, which it takes from the effect chosen now.
    *slots = {effect_slots::kAllSlots};
    const auto pick = catalog ? g_profile.selections.find(selection_of(target)) : g_profile.selections.end();
    if (pick != g_profile.selections.end() && pick->second != kVanillaSelection && pick->second != id)
      if (const AssetRecord* other = asset_by_id(pick->second))
        taken->push_back({other->info.id, other->info.name, {effect_slots::kAllSlots}});
    return true;
  }
  *whole = !d.keeps_layout;
  *slots = d.changed;
  if (!catalog) return true;
  Sources sources = sources_of(g_profile, *layout, clean);
  for (auto& [other, lost] : take(&sources, id, d)) {
    const AssetRecord* asset = asset_by_id(other);
    order_by_layout(&lost, *layout);
    taken->push_back({other, asset ? asset->info.name : other, lost});
  }
  return true;
}

void load_effect_slots(const json& profile, Profile* out) {
  const auto all = profile.find("effect_slots");
  if (all == profile.end() || !all->is_object()) return;
  for (auto file = all->begin(); file != all->end(); ++file) {
    if (!file.value().is_object() || file.key().size() > 64) continue;
    Sources sources;
    for (auto slot = file.value().begin(); slot != file.value().end(); ++slot)
      if (slot.value().is_string() && slot.key().size() <= 64 && slot.value().get<std::string>().size() <= 160)
        sources[slot.key()] = slot.value().get<std::string>();
    if (!sources.empty()) out->effect_slots[file.key()] = std::move(sources);
  }
}

json effect_slots_json(const Profile& profile) {
  json out = json::object();
  for (const auto& [file, sources] : profile.effect_slots) {
    json one = json::object();
    for (const auto& [slot, id] : sources) one[slot] = id;
    out[file] = std::move(one);
  }
  return out;
}

}  // namespace cosmetic_internal

bool effect_slots_of(const std::string& target, EffectSlots* out, std::string* error) {
  std::string local; if (!error) error = &local;
  *out = EffectSlots{};
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ready_locked(error)) return false;
  std::vector<uint8_t> clean;
  if (!clean_effect_file(target, &clean)) { *error = "The game disc is needed to read " + target + "."; return false; }
  const effect_slots::Layout* layout = layout_of(target, clean);
  if (!layout) { *error = "No slots for " + target + "."; return false; }
  const Sources sources = sources_of(g_profile, *layout, clean);
  out->target = layout->target;
  const auto others = effects_of(layout->target);
  std::map<std::string, effect_slots::Diff> diffs;
  for (const AssetRecord* effect : others) {
    out->effects.push_back(effect->info.id);
    effect_slots::Diff d;
    if (effect->info.available && diff_of(*effect, *layout, clean, &d)) diffs[effect->info.id] = d;
  }
  const auto whole = sources.find(effect_slots::kAllSlots);
  if (whole != sources.end()) {
    out->whole_id = whole->second;
    if (const AssetRecord* a = asset_by_id(whole->second)) out->whole_name = a->info.name;
  }
  for (size_t i = 0; i < layout->slots.size(); ++i) {
    EffectSlot slot;
    slot.name = layout->slots[i];
    const auto source = sources.find(slot.name);
    if (source != sources.end()) {
      slot.source_id = source->second;
      if (const AssetRecord* a = asset_by_id(source->second)) slot.source_name = a->info.name;
    }
    for (const AssetRecord* effect : others) {
      const auto d = diffs.find(effect->info.id);
      if (d != diffs.end() && d->second.keeps_layout &&
          std::find(d->second.changed.begin(), d->second.changed.end(), slot.name) != d->second.changed.end())
        slot.choices.push_back(effect->info.id);
    }
    // Shown: a slot of a player-facing part, or one an installed effect changes.
    if (layout->listed[i] || !slot.choices.empty() || !slot.source_id.empty()) out->slots.push_back(std::move(slot));
  }
  return true;
}

bool effect_takes(const std::string& asset_id, std::vector<std::string>* slots, bool* whole,
                  std::vector<EffectTaken>* taken, std::string* error) {
  std::string local; if (!error) error = &local;
  slots->clear(); taken->clear(); *whole = true;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ready_locked(error)) return false;
  const AssetRecord* asset = asset_by_id(asset_id);
  if (!asset || asset->info.kind != "effect_visual") { *error = "That effect is not installed."; return false; }
  std::vector<uint8_t> clean;
  const effect_slots::Layout* layout = nullptr;
  effect_slots::Diff d;
  if (!standalone_effect(*asset) || !clean_effect_file(asset->info.target_path, &clean) ||
      !(layout = layout_of(asset->info.target_path, clean)) || !diff_of(*asset, *layout, clean, &d)) {
    // One choice per file: it takes the file from the effect chosen now.
    const auto pick = g_profile.selections.find(selection_key(*asset));
    if (pick != g_profile.selections.end() && pick->second != kVanillaSelection && pick->second != asset_id)
      if (const AssetRecord* other = asset_by_id(pick->second))
        taken->push_back({other->info.id, other->info.name, {effect_slots::kAllSlots}});
    return true;
  }
  *whole = !d.keeps_layout;
  *slots = d.changed;
  Sources sources = sources_of(g_profile, *layout, clean);
  for (auto& [id, lost] : take(&sources, asset_id, d)) {
    const AssetRecord* other = asset_by_id(id);
    order_by_layout(&lost, *layout);
    taken->push_back({id, other ? other->info.name : id, lost});
  }
  return true;
}

bool use_effect(const std::string& asset_id, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  const AssetRecord* asset = asset_by_id(asset_id);
  if (!asset || asset->info.kind != "effect_visual") { *error = "That effect is not installed."; return false; }
  Profile previous = g_profile;
  std::string why;
  if (!assign_effect_locked(&g_profile, *asset, &why)) {
    // One choice per file, as before slots.
    const std::string key = selection_key(*asset);
    g_profile.selections[key] = asset_id;
    for (auto& pick : g_profile.selections)
      if (pick.first != key && pick.second != kVanillaSelection &&
          pick.first.rfind(asset->info.target_path + "#", 0) == 0)
        pick.second = kVanillaSelection;
  }
  g_profile.enabled = true;
  ++g_profile.generation;
  if (!compose_effects_with_open_disc_locked(asset->info.target_path, g_profile, error) || !save_profile_locked(error)) {
    g_profile = std::move(previous);
    return false;
  }
  host::log("cosmetics: %s used for %s", asset->info.name.c_str(), asset->info.target_path.c_str());
  return true;
}

bool set_effect_slot(const std::string& target, const std::string& slot, const std::string& asset_id, std::string* error) {
  std::string local; if (!error) error = &local;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!mutable_profile_locked(error)) return false;
  std::vector<uint8_t> clean;
  if (!clean_effect_file(target, &clean)) { *error = "The game disc is needed to read " + target + "."; return false; }
  const effect_slots::Layout* layout = layout_of(target, clean);
  if (!layout) { *error = "No slots for " + target + "."; return false; }
  if (slot != effect_slots::kAllSlots && layout->slot_index(slot) < 0) { *error = "No slot " + slot + "."; return false; }
  Sources sources = sources_of(g_profile, *layout, clean);
  // A slot of a file served whole: the whole file goes, its other slots are the disc's own.
  if (slot != effect_slots::kAllSlots && sources.count(effect_slots::kAllSlots)) sources.clear();
  if (asset_id.empty()) {
    if (slot == effect_slots::kAllSlots) sources.clear();
    else sources.erase(slot);
  } else {
    const AssetRecord* asset = asset_by_id(asset_id);
    effect_slots::Diff d;
    if (!asset || !standalone_effect(*asset) || lower(asset->info.target_path) != lower(layout->target) ||
        !diff_of(*asset, *layout, clean, &d)) {
      *error = "That effect is not one of " + target + "'s."; return false;
    }
    if (slot == effect_slots::kAllSlots) take(&sources, asset_id, d);
    else if (!d.keeps_layout || std::find(d.changed.begin(), d.changed.end(), slot) == d.changed.end()) {
      *error = asset->info.name + " does not change " + slot + "."; return false;
    } else sources[slot] = asset_id;
  }
  Profile previous = g_profile;
  store(&g_profile, layout->target, std::move(sources), asset_id);
  g_profile.enabled = true;
  ++g_profile.generation;
  if (!compose_effects_with_open_disc_locked(layout->target, g_profile, error) || !save_profile_locked(error)) {
    g_profile = std::move(previous);
    return false;
  }
  host::log("cosmetics: %s of %s from %s", slot.c_str(), layout->target.c_str(),
            asset_id.empty() ? "the disc" : asset_id.c_str());
  return true;
}

}  // namespace host::cosmetics
