// Source Port host: new stages from the Mods menu's New Stages (host/new_stages.h, MU_CMD_NEW_STAGES).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- new stages from the Mods menu's New Stages (host/new_stages.h, shim/mu_new_stages.c) ----
// The stage select lists the installed new stages (MU_CMD_NEW_STAGES 0) and, when one is chosen, picks
// it (2). At the next match's boundary (MU_CMD_STAGE_SKIN_MATCH, plan_new_stage) on its base stage, the
// base's path opens a new entry serving the new stage's file, checked again, from one of two slots used
// in turn (so the slot the last match read is not written while anything may still read it). The pick
// holds for that match and its Sudden Death: the stage select opening again, any scene that is no match
// (op 3) or a match on another stage lets it go. Offline only: never online, in a peer-to-peer session, a
// replay (it would play on the base) or a Vanilla run, and never while this release has no New Stages
// (release_scope.h: the installed ones stay on disk, unlisted).
static_assert(kNewStageBase > kMenuModBase && kNewStageBase + 2 * kNewStageSlot <= kTrainingFileBase,
              "new stages sit between the menu mods and Training Mode's files");
static_assert(host::new_stages::kMaxBytes <= kNewStageSlot, "a new stage fits its slot");

bool new_stages_offline() {
  return !g_replaying && slippi::online::session_mode() < 0 && !source_p2p::requested() &&
         !host::cosmetics::online_active() && !host::gameplay::vanilla_only() && !host::options.vanilla_game &&
         host::release_scope::gate(host::release_scope::kNewStages, "added stages are not on the stage select");
}

bool read_new_stage(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kNewStageBase || offset >= kNewStageBase + 2 * kNewStageSlot) return false;
  NewStageSlot& s = g_new_stage_slots[offset >= kNewStageBase + kNewStageSlot ? 1 : 0];
  *ok = false;
  if (s.data.empty() || offset < s.start) return true;
  if (!s.read) {   // once a serve: the evidence the match loaded the new stage
    s.read = true;
    host::log("new stages: the game reads %s (%08X+%X)", s.name.c_str(), offset, size);
  }
  const uint32_t at = offset - s.start;
  *ok = (uint64_t)at + size <= kNewStageSlot;
  if (*ok) {
    const uint32_t have = at < s.data.size() ? std::min<uint32_t>(size, (uint32_t)s.data.size() - at) : 0;
    if (have) std::memcpy(dst, s.data.data() + at, have);
    std::memset((uint8_t*)dst + have, 0, size - have);
  }
  return true;
}

// MU_CMD_STAGE_SKIN_MATCH, before the stage loads: the picked new stage served when this match is on its
// base (`file`), else the base's own path again. True when what a stage path opens changed (the game
// then forgets the entry numbers it holds).
bool plan_new_stage(const std::string& stage_path) {
  // The game's stage table names its files with a slash first ("/GrNLa.dat").
  const std::string file = !stage_path.empty() && stage_path[0] == '/' ? stage_path.substr(1) : stage_path;
  std::string path = "/" + file;
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
  host::new_stages::Installed info;
  bool want = false;
  if (!g_new_stage_pick.empty()) {
    if (!new_stages_offline()) {
      g_new_stage_pick.clear();
    } else if (host::new_stages::find(g_new_stage_pick, &info) && _stricmp(info.base.c_str(), file.c_str()) == 0) {
      want = true;
    } else {
      host::log("new stages: %s was picked, but this match is on %s: the pick is let go", g_new_stage_pick.c_str(), file.c_str());
      g_new_stage_pick.clear();
    }
  }
  const bool serving = g_new_stage_entry > 0;
  if (want && serving && g_new_stage_path == path && g_new_stage_last_slot >= 0 &&
      g_new_stage_slots[g_new_stage_last_slot].id == g_new_stage_pick)
    return false;   // Sudden Death, or the next match on it: served already
  if (want) {
    std::vector<uint8_t> retail, bytes;
    uint32_t offset = 0, length = 0;
    if (host::disc_find_file(info.base, &offset, &length) && length && length <= 64u * 1024 * 1024) {
      retail.resize(length);
      if (!host::disc_read(offset, retail.data(), length)) retail.clear();
    }
    if (host::new_stages::served(g_new_stage_pick, retail, &bytes, &info)) {
      const int slot = g_new_stage_last_slot == 0 ? 1 : 0;
      NewStageSlot& s = g_new_stage_slots[slot];
      s.start = kNewStageBase + (uint32_t)slot * kNewStageSlot;
      s.data = std::move(bytes);
      s.id = info.id;
      s.name = info.name + " (" + info.id + ")";
      s.read = false;
      const int32_t entry = (int32_t)g_fst.size();
      g_fst.push_back({s.start, (uint32_t)s.data.size(), false});
      g_new_stage_last_slot = slot;
      g_new_stage_entry = entry;
      g_new_stage_path = path;
      host::log("new stages: %s plays as %s: entry %d, %u bytes at %08X", path.c_str(), s.name.c_str(), entry,
                (uint32_t)s.data.size(), s.start);
      return true;
    }
    g_new_stage_pick.clear();   // (served() said why, once)
  }
  if (!serving) return false;
  host::log("new stages: %s is the stage's own again", g_new_stage_path.c_str());
  g_new_stage_entry = -1;
  g_new_stage_path.clear();
  return true;
}

// MU_CMD_NEW_STAGES (shim/mu_new_stages.c, the stage select). payload[0]:
//   0  the list: reply 1, count, generation u32, random (1: Random may pick them), then per stage its
//      base's file name[16], its name[40] and its base's name[24]. {0} when new stages are off (online,
//      Vanilla, a replay).
//   1  a part of an icon: index, offset u24. reply 1, width u16, height u16, size u24, then the bytes
//      from the offset (at most 3840): new_stages::kIconWidth x kIconHeight RGBA8.
//   2  a pick for the next match: index, 0xFF none. reply {1}.
//   3  a scene that is no match begins (gm_1A3F.c preloadState): the pick is let go. reply {1}.
void new_stages_command(const uint8_t* p, size_t n, std::vector<uint8_t>& reply) {
  reply = {0};
  if (p[0] == 3 && n == 1) {
    if (!g_new_stage_pick.empty()) host::log("new stages: %s is let go (no match followed)", g_new_stage_pick.c_str());
    g_new_stage_pick.clear();
    reply = {1};
    return;
  }
  if (!new_stages_offline()) {
    g_new_stage_pick.clear();
    return;
  }
  if (p[0] == 0 && n == 1) {
    const auto all = host::new_stages::list();
    g_new_stage_listed.clear();
    g_new_stage_icons.clear();
    uint32_t generation = 2166136261u;
    reply = {1, 0, 0, 0, 0, 0, (uint8_t)(host::new_stages::random_included() ? 1 : 0)};
    for (const auto& s : all) {
      if (g_new_stage_listed.size() >= host::new_stages::kMaxInstalled) break;
      g_new_stage_listed.push_back(s.id);
      put_text(reply, s.base, 16);
      put_text(reply, host::mods_menu::drawable(s.name, 39), 40);
      put_text(reply, host::new_stages::base_name(s.base), 24);
      const std::string key = s.id + s.sha256;
      generation = (generation ^ fnv1a((const uint8_t*)key.data(), key.size())) * 16777619u;
    }
    reply[1] = (uint8_t)g_new_stage_listed.size();
    reply[2] = (uint8_t)(generation >> 24); reply[3] = (uint8_t)(generation >> 16);
    reply[4] = (uint8_t)(generation >> 8); reply[5] = (uint8_t)generation;
    if (!all.empty()) host::log("new stages: the stage select lists %zu", g_new_stage_listed.size());
    return;
  }
  if (p[0] == 1 && n == 5 && p[1] < g_new_stage_listed.size()) {
    const std::string& id = g_new_stage_listed[p[1]];
    auto it = g_new_stage_icons.find(id);
    if (it == g_new_stage_icons.end()) it = g_new_stage_icons.emplace(id, host::new_stages::icon(id)).first;
    const std::vector<uint8_t>& px = it->second;
    const size_t at = (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4];
    if (px.empty() || at >= px.size()) return;
    const size_t size = px.size(), part = std::min<size_t>(size - at, 3840);
    const int w = host::new_stages::kIconWidth, h = host::new_stages::kIconHeight;
    reply = {1, (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(size >> 16), (uint8_t)(size >> 8),
             (uint8_t)size};
    reply.insert(reply.end(), px.begin() + (std::ptrdiff_t)at, px.begin() + (std::ptrdiff_t)(at + part));
    return;
  }
  if (p[0] == 2 && n == 2) {
    if (p[1] == 0xFF) {
      if (!g_new_stage_pick.empty()) host::log("new stages: no pick (the stage select opened)");
      g_new_stage_pick.clear();
    } else if (p[1] < g_new_stage_listed.size()) {
      g_new_stage_pick = g_new_stage_listed[p[1]];
      host::new_stages::Installed info;
      host::new_stages::find(g_new_stage_pick, &info);
      host::log("new stages: %s \"%s\" picked for the next match on %s", g_new_stage_pick.c_str(), info.name.c_str(),
                info.base.c_str());
    } else {
      return;
    }
    reply = {1};
  }
}

}  // namespace host_internal
}  // namespace source_port
