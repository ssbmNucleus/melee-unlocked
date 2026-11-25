// Source Port host: cosmetic packs served over the disc (host/cosmetic_mods.h), and the character
// select's and stage select's skin cycling (MU_CMD_SKIN_CYCLE, MU_CMD_STAGE_SKIN_CYCLE,
// MU_CMD_STAGE_SKIN_MATCH).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- cosmetic packs (costumes, stage and effect visuals) ----
// The same catalog and rules as the static recomp (cosmetic_*.cpp): the player's selections,
// resolved against this disc's table. Each overridden file moves to its own range past the other
// layers, so a replacement longer than the disc's copy never reads into the next file. Online, and
// in replays, a costume skin is served with the standard rest pose (the desktop app's fix), and one
// whose bones cannot be made the standard costume's as the disc's copy. Never mod assets:
// cosmetics are local only and do not change what the game or Slippi sees.
// A stage file the mod disc replaced still takes the player's stage skins (the random installed stage skin
// each match, or a chosen one): they are checked against the game's own stage, and are the player's own
// screen only. Animelee CE's Battlefield refused every pick, so the same stage came every match
// (an 11-10 report). Costumes keep the stricter rule above.
bool pack_stage_takes_skins(uint32_t entry) {
  return entry < g_raw_paths.size() && !g_raw_paths[entry].empty() && skins::is_stage_file(g_raw_paths[entry]);
}

// The alias entry of `entry` (disc entry i's skin) for online play: the disc's copy for a skin not
// used online, its own range serving the online form for a costume that has one.
void add_cosmetic_alias(int32_t entry, uint32_t i, uint32_t vanilla_start, uint32_t length, bool online_allowed,
                        bool online_form) {
  if (!online_allowed) {
    const int32_t alias = (int32_t)g_fst.size();
    g_fst.push_back({vanilla_start, be32(g_fst_raw.data() + (size_t)i * 12 + 8), false});
    g_cosmetic_alias[entry] = alias;
    return;
  }
  if (!online_form) return;
  const uint64_t span = ((uint64_t)length + 0xFFFFu) & ~0xFFFFull;
  if (g_cosmetic_next + span > 0xFFFF0000ull) {
    host::log("cosmetics: no room for the online form of entry %u; online it reads the skin's own entry", i);
    return;
  }
  const int32_t alias = (int32_t)g_fst.size();
  g_fst.push_back({(uint32_t)g_cosmetic_next, length, false});
  g_cosmetic_files.push_back({(uint32_t)g_cosmetic_next, vanilla_start, length, false, true});
  g_cosmetic_next += span;
  g_cosmetic_alias[entry] = alias;
}

// A new entry for disc entry i's skin, in its own range past kCosmeticBase, and its alias entry for
// online play (add_cosmetic_alias). -1 when the range is full.
int32_t add_cosmetic_entry(uint32_t i, uint32_t vanilla_start, uint32_t length, bool online_allowed,
                           bool online_form = false) {
  const uint64_t span = ((uint64_t)length + 0xFFFFu) & ~0xFFFFull;
  if (g_cosmetic_next + span > 0xFFFF0000ull) return -1;
  const int32_t entry = (int32_t)g_fst.size();
  g_fst.push_back({(uint32_t)g_cosmetic_next, length, false});
  g_cosmetic_files.push_back({(uint32_t)g_cosmetic_next, vanilla_start, length, false});
  g_cosmetic_next += span;
  add_cosmetic_alias(entry, i, vanilla_start, length, online_allowed, online_form);
  return entry;
}

void load_cosmetics(bool always_vanilla_unsafe) {
  if (g_fst_raw.size() < 12) return;
  if (always_vanilla_unsafe) host::cosmetics::freeze_for_online_session();
  std::vector<uint8_t>& fst = g_cosmetic_fst;
  fst = g_fst_raw;
  host::cosmetics::apply_to_fst(fst.data(), (uint32_t)fst.size());
  const uint32_t count = be32(fst.data() + 8);
  uint64_t& next = g_cosmetic_next;
  g_raw_paths.assign(count, std::string());
  for (const auto& path : g_paths)
    if (path.second > 0 && (uint32_t)path.second < count) g_raw_paths[(size_t)path.second] = path.first;
  for (uint32_t i = 1; i < count && i < g_fst.size(); ++i) {
    const uint8_t* e = fst.data() + i * 12;
    if (e[0]) continue;
    const uint32_t vanilla_start = be32(e + 4), length = be32(e + 8);
    if (host::cosmetics::read(vanilla_start, 0, nullptr, 0) == host::cosmetics::OverrideRead::NotOverridden) continue;
    if (g_fst[i].offset != vanilla_start) {
      // A costume file the loaded pack replaced. Only that pack's own sets may serve it (the 20XX
      // Hack Pack's L / R costumes); they get an entry of their own and entry i stays the pack's
      // file, which is the standard costume of this session.
      const std::string applied = host::cosmetics::applied_asset(vanilla_start);
      if ((!host::cosmetics::session_pack_skin(applied) && !pack_costume_takes_skins(i) && !pack_stage_takes_skins(i)) || i >= g_raw_paths.size() ||
          g_raw_paths[i].empty()) {
        host::log("cosmetics: entry %u is replaced by a mod pack; cosmetic selection skipped", i);
        continue;
      }
      const int32_t entry = add_cosmetic_entry(i, vanilla_start, length, host::cosmetics::online_allowed(vanilla_start),
                                               host::cosmetics::has_online_form(vanilla_start));
      if (entry < 0) { host::log("cosmetics: selections exceed the cosmetic range; rest skipped"); break; }
      g_cosmetic_live[{i, applied}] = entry;
      g_paths[g_raw_paths[i]] = entry;
      continue;
    }
    const uint64_t span = ((uint64_t)length + 0xFFFFu) & ~0xFFFFull;
    if (next + span > 0xFFFF0000ull) { host::log("cosmetics: selections exceed the cosmetic range; rest skipped"); break; }
    g_cosmetic_files.push_back({(uint32_t)next, vanilla_start, length, false});
    g_fst[i] = {(uint32_t)next, length, false};
    next += span;
    g_cosmetic_live[{i, host::cosmetics::applied_asset(vanilla_start)}] = (int32_t)i;
    add_cosmetic_alias((int32_t)i, i, vanilla_start, length, host::cosmetics::online_allowed(vanilla_start),
                       host::cosmetics::has_online_form(vanilla_start));
  }
  host::cosmetics::set_online_probe(native_online_mode);
  host::cosmetics::set_stage_button_probe(held_buttons);
  const auto profile = host::cosmetics::session_profile();
  host::log("cosmetics: %zu disc files overridden (profile %s%s)", g_cosmetic_files.size(),
            profile.fingerprint.empty() ? "vanilla" : profile.fingerprint.c_str(),
            always_vanilla_unsafe ? ", visual-only rule forced" : "");
}

// Serves a read inside the cosmetic range; false when the offset is outside it.
bool read_cosmetic(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kCosmeticBase || g_cosmetic_files.empty()) return false;
  const auto upper = std::upper_bound(g_cosmetic_files.begin(), g_cosmetic_files.end(), offset,
      [](uint32_t address, const CosmeticFile& f) { return address < f.start; });
  if (upper == g_cosmetic_files.begin()) { *ok = false; return true; }
  CosmeticFile& f = *std::prev(upper);
  *ok = (f.online_form ? host::cosmetics::read_online_form(f.vanilla_start, offset - f.start, dst, size)
                       : host::cosmetics::read(f.vanilla_start, offset - f.start, dst, size)) ==
        host::cosmetics::OverrideRead::Success;
  if (!f.logged) {
    f.logged = true;
    host::log("cosmetics: first read of the override at %08X (disc copy %08X)%s%s", f.start, f.vanilla_start,
              f.online_form ? ", its online form" : "", host::cosmetics::session_profile().frozen ? ", online rule active" : "");
  }
  return true;
}

// ---- character select L / R skin cycling (mn/mncharsel.c, shim/mu_content.c) ----
// payload: port, fighter, costume, direction; optional fifth byte is the allowed offline colors.
// reply: changed, optional new costume for a five-byte request, then the choice's name (MU_CMD_SKIN_CYCLE).

// The slot's disc files after the catalog published it again: each one gets the entry that serves
// it now, and the path opens that entry. A skin is a new entry number in its own range past
// kCosmeticBase, so nothing the game preloaded under the old number is reused; a skin not proven to
// change looks alone also gets the alias entry that names the disc's copy for online play, as at
// startup. False when a file could not be given an entry (the caller puts the old pick back).
bool publish_cosmetic_files(const std::string& slot, const host::cosmetics::RepublishResult& result) {
  if (!result.ok) return false;
  bool ok = true;
  for (const auto& file : result.files) {
    const uint32_t i = file.fst_index;
    if (i >= g_raw_paths.size() || g_raw_paths[i].empty() || (size_t)i * 12 + 12 > g_fst_raw.size()) continue;
    // A file the loaded pack replaced: entry i is the pack's file, the standard costume of this
    // session, and only the pack's own sets may serve it instead (load_cosmetics).
    const bool pack_replaced = g_fst[i].offset != file.vanilla_start && g_fst[i].offset < kCosmeticBase;
    if (pack_replaced && (!file.overridden || (!host::cosmetics::session_pack_skin(file.asset_id) && !pack_costume_takes_skins(i) && !pack_stage_takes_skins(i)))) {
      if (file.overridden) host::log("cosmetics: entry %u is replaced by a mod pack; cosmetic selection skipped", i);
      g_paths[g_raw_paths[i]] = (int32_t)i;
      continue;
    }
    const uint32_t vanilla_length = be32(g_fst_raw.data() + (size_t)i * 12 + 8);
    // (an effect file's key names its bytes too: one asset id can head several compositions)
    const std::pair<uint32_t, std::string> key{
        i, file.overridden ? file.asset_id + (file.content.empty() ? "" : "#" + file.content) : std::string()};
    int32_t entry;
    const auto known = g_cosmetic_live.find(key);
    // An entry made at another length is not used again: a stage's shared length (its pool's or the random
    // pick's) can grow after an install, and the game stops on a file whose length is not its archive's own
    // (HSD_ArchiveParse). The pick gets a new entry at its length instead.
    const uint32_t wanted_length = file.overridden ? file.length : vanilla_length;
    if (known != g_cosmetic_live.end() && known->second >= 0 && (size_t)known->second < g_fst.size() &&
        g_fst[(size_t)known->second].length == wanted_length) entry = known->second;
    else if (!file.overridden && g_fst[i].offset == file.vanilla_start) entry = (int32_t)i;
    else if (!file.overridden) {
      entry = (int32_t)g_fst.size();
      g_fst.push_back({file.vanilla_start, vanilla_length, false});
    } else {
      entry = add_cosmetic_entry(i, file.vanilla_start, file.length, file.online_allowed, file.online_form);
      if (entry < 0) {
        host::log("cosmetics: selections exceed the cosmetic range; %s keeps its previous skin", slot.c_str());
        ok = false;
        continue;
      }
    }
    g_cosmetic_live[key] = entry;
    g_paths[g_raw_paths[i]] = entry;
  }
  return ok;
}

bool republish_cosmetic_slot(const std::string& slot, bool stage = false) {
  return publish_cosmetic_files(slot, stage
      ? host::cosmetics::republish_stage(g_cosmetic_fst.data(), (uint32_t)g_cosmetic_fst.size(), slot)
      : host::cosmetics::republish_slot(g_cosmetic_fst.data(), (uint32_t)g_cosmetic_fst.size(), slot));
}

// What the Mods menu's installs and removals changed (host/mods_menu.h take_republish), published again
// on the game thread: an effect file under a new entry number, so the next match loads the installed
// effect (or the disc's own again) with no restart. Never in a replay or a Vanilla run (no Mods menu).
void republish_from_mods_menu() {
  for (const auto& [file, kind] : host::mods_menu::take_republish()) {
    if (kind == host::mods_menu::FileKind::Menu) {   // a menu mod installed or removed: its screen's next load
      publish_menu_mods(false);
      continue;
    }
    if (g_cosmetic_fst.empty()) continue;
    if (kind == host::mods_menu::FileKind::Effect)
      publish_cosmetic_files(file, host::cosmetics::republish_effect(g_cosmetic_fst.data(), (uint32_t)g_cosmetic_fst.size(), file));
    else
      republish_cosmetic_slot(file, kind == host::mods_menu::FileKind::Stage);
  }
}

// MU_CMD_STAGE_SKIN_MATCH: one command at the new match scene boundary, before any stage DVD preload.
void plan_random_stage_skin(const std::string& file, std::vector<uint8_t>& reply) {
  reply.assign(1, 0);
  if (plan_new_stage(file)) reply[0] = 1;   // a new stage picked on the stage select, or its base again
  if (g_replaying || g_cosmetic_fst.empty()) return;
  static uint64_t match_token = 0;
  const auto result = host::cosmetics::plan_stage_skin(g_cosmetic_fst.data(),
      (uint32_t)g_cosmetic_fst.size(), file, ++match_token);
  if (result.ok && publish_cosmetic_files(file, result)) reply[0] = 1;
}

// ---- stage select skin cycling (mn/mnstagesel.c, shim/mu_content.c) ----
// MU_CMD_STAGE_SKIN_CYCLE. payload: direction (1 next, 0 previous), then the stage's file name.
// reply: 1 when it changed.

void cycle_stage_skin(const std::string& file, int direction, std::vector<uint8_t>& reply) {
  reply.assign(1, 0);
  // Online too (Direct's stage select): the pick is this player's own screen, like a costume skin.
  if (g_replaying || g_cosmetic_fst.empty()) return;
  const auto pick = host::cosmetics::cycle_stage_live(file, direction);
  if (!pick.ok || !pick.changed) {
    if (!pick.message.empty()) host::log("cosmetics: %s skin not changed (%s)", file.c_str(), pick.message.c_str());
    return;
  }
  if (!republish_cosmetic_slot(file, true)) {
    // No entry for it: the previous pick goes back, saved and published.
    host::cosmetics::cycle_stage_live(file, -direction);
    republish_cosmetic_slot(file, true);
    return;
  }
  host::log("cosmetics: stage select picked %s for %s", pick.name.c_str(), file.c_str());
  reply[0] = 1;
}

void cycle_costume_skin(int character, int costume, int direction, std::vector<uint8_t>& reply,
                        uint32_t allowed_costumes) {
  reply.assign(1, 0);
  if (g_replaying || g_cosmetic_fst.empty()) return;
  std::string slot = host::cosmetics::costume_slot_file(character, costume);
  // An m-ex screen may currently show an added color. The offline fighter-wide request can
  // return to a native slot; the original per-color command still requires a native slot.
  if (slot.empty() && !allowed_costumes) return;
  host::cosmetics::LiveCycle pick;
  if (allowed_costumes) {
    const auto cycle = host::cosmetics::cycle_character_live(character, costume, direction, allowed_costumes);
    pick = cycle.pick;
    if (pick.changed) { slot = cycle.target; costume = cycle.costume; }
  } else pick = host::cosmetics::cycle_slot_live(slot, direction);
  if (!pick.ok || !pick.changed) {
    if (!pick.message.empty()) host::log("cosmetics: %s skin not changed (%s)", slot.c_str(), pick.message.c_str());
    return;
  }
  // The Ice Climbers: Nana's slot changed with Popo's (pick.partner_slot), and is published the same
  // way. A slot that cannot be given an entry gets its previous pick back; so does Nana's when
  // Popo's failed, so the pair never ends half changed by a failure.
  const auto put_back = [](const std::string& which, const std::string& previous_id) {
    std::string error;
    host::cosmetics::select_variant_live(which, previous_id, &error);
    republish_cosmetic_slot(which);
  };
  if (!republish_cosmetic_slot(slot)) {
    put_back(slot, pick.previous_id);
    if (!pick.partner_slot.empty()) put_back(pick.partner_slot, pick.partner_previous_id);
    return;
  }
  if (!pick.partner_slot.empty()) {
    if (republish_cosmetic_slot(pick.partner_slot))
      host::log("cosmetics: character select pairs %s with %s", pick.partner_slot.c_str(), slot.c_str());
    else put_back(pick.partner_slot, pick.partner_previous_id);
  }
  // The pictures the new choice brings (its portrait and stock icon), for the renderer's next frame.
  std::vector<gx::texpack::CosmeticCompanion> companions;
  for (const auto& item : host::cosmetics::active_companions())
    companions.push_back({item.kind, item.target_path, item.path});
  gx::texpack::refresh_cosmetic_companions(std::move(companions));
  host::log("cosmetics: character select picked %s for %s", pick.name.c_str(), slot.c_str());
  reply[0] = 1;
  if (allowed_costumes) reply.push_back((uint8_t)costume);
  reply.insert(reply.end(), pick.name.begin(), pick.name.begin() + (std::ptrdiff_t)std::min<size_t>(pick.name.size(), 63));
}

}  // namespace host_internal
}  // namespace source_port
