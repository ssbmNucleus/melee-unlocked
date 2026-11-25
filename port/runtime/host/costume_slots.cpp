// Installed skins as costume slots of their own (costume_slots.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "costume_slots.h"
#include "cosmetic_mods.h"
#include "costume_served_form.h"
#include "host.h"
#include "opponent_skins.h"
#include "roster.h"
#include "test_env.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>

namespace host::costume_slots {
namespace {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
constexpr int kPorts = 4;
constexpr int kCharacters = 26;

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// "plmrnr.dat" -> (character, costume), every retail costume slot of the select screen's fighters.
struct Place { int character = -1, costume = -1; };
const std::map<std::string, Place>& places() {
  static const std::map<std::string, Place> table = [] {
    std::map<std::string, Place> t;
    for (int c = 0; c < kCharacters; ++c)
      for (int k = 0; k < 8; ++k) {
        const std::string file = host::cosmetics::costume_slot_file(c, k);
        if (!file.empty()) t.emplace(lower(file), Place{c, k});
      }
    return t;
  }();
  return table;
}

struct Pick {
  int character = -1, costume = -1;
  std::string asset_id, target, partner_asset, partner_target;
  bool strict = false;   // its served form passed the peer rule (what an opponent's machine builds)
};

// A served form, built once per "<asset>|<slot>|<online or offline>"; null bytes: refused (the reason
// kept for the log). Online: the skin on the slot's retail skeleton (costume_served_form by the peer
// rule, what an opponent's machine builds, so what is announced and the only form this screen plays
// online; host::skins::wearer_form). Offline: costume_offline_form (the strict form, else the skin as
// installed).
struct Built { Bytes bytes; bool strict = false; std::string reason; };

std::mutex g_mutex;
Pick g_picks[kPorts];
Pick g_online;                                   // the online select screen's own door
std::string g_worn[kPorts];                      // the skin last served to each port's fighter
std::map<std::string, Built> g_built;
std::map<std::string, std::vector<uint8_t>> g_pictures;   // "<asset>|<what>"; empty: none

uint32_t fnv(uint32_t h, const std::string& s) {
  for (const unsigned char c : s) h = (h ^ c) * 16777619u;
  return (h ^ 0xFF) * 16777619u;
}

// The installed costumes that are slots, every fighter: (character, slot) in catalog order.
std::vector<std::pair<int, Slot>> all_slots(uint32_t* generation) {
  std::vector<std::pair<int, Slot>> out;
  uint32_t h = 2166136261u;
  if (!host::cosmetics::imports_as_costume_slots()) {
    if (generation) *generation = 0;
    return out;
  }
  int count[kCharacters] = {};
  for (const auto& a : host::cosmetics::assets()) {
    if (a.kind != "character_costume" || a.source == "disc" || !a.available) continue;
    const auto it = places().find(lower(a.target_path));
    if (it == places().end()) continue;   // Nana's files go with Popo's slot; not a slot of their own
    const int c = it->second.character;
    if (count[c] >= kMaxPerFighter) continue;
    ++count[c];
    Slot s;
    s.asset_id = a.id;
    s.name = a.name;
    s.target = a.target_path;
    s.costume = it->second.costume;
    s.has_csp = !a.csp_path.empty();
    s.has_stock = !a.stock_path.empty();
    h = fnv(fnv(fnv(h, a.id), a.csp_path), a.stock_path);
    out.emplace_back(c, std::move(s));
  }
  if (generation) *generation = out.empty() ? 0 : (h ? h : 1);
  return out;
}

const host::cosmetics::AssetInfo* find_asset(const std::vector<host::cosmetics::AssetInfo>& all, const std::string& id) {
  for (const auto& a : all)
    if (a.id == id) return &a;
  return nullptr;
}

// The served form of `asset` for `slot` (locked by the caller, which also holds no other lock).
const Built& build_locked(const std::string& asset, const std::string& slot, bool online) {
  const std::string key = asset + "|" + lower(slot) + (online ? "|online" : "|offline");
  const auto have = g_built.find(key);
  if (have != g_built.end()) return have->second;
  Built b;
  std::vector<uint8_t> retail, dat;
  std::string thumbnail, why;
  if (!host::skins::retail_reference(slot, &retail, &why)) {
    b.reason = "no retail reference: " + why;
  } else if (!host::cosmetics::costume_file(asset, &dat, &thumbnail) || dat.empty()) {
    b.reason = "its file cannot be read";
  } else {
    // Online the peer rule itself (own_skin_policy is peer_skin_policy): what an opponent's machine
    // builds from the same file, so this machine never plays a skin the opponent's would refuse; one it
    // refuses is the retail costume here too. Offline costume_offline_form.
    host::skins::ServedForm form = host::skins::wearer_form(retail, dat, slot, online);
    b.strict = form.ok && (online || form.reason.rfind("as installed", 0) != 0);   // the retail bones
    if (form.ok) b.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(form.bytes));
    b.reason = form.reason;
  }
  if (online && !b.bytes)
    host::log("costume slots: %s for %s: offline only; online the slot plays the retail costume for both players (%s)",
              asset.c_str(), slot.c_str(), b.reason.c_str());
  else
    host::log("costume slots: %s for %s (%s): %s%s", asset.c_str(), slot.c_str(), online ? "online" : "offline",
              b.bytes ? "ready, " : "refused: ", b.reason.c_str());
  return g_built.emplace(key, std::move(b)).first->second;
}

// The other file of a two-file pick and the installed skin for it: Nana's with Popo's, Sheik's with
// Zelda's and the other way round. The catalog's own pairing (cosmetic_mods partner_skin), else the
// one skin for that slot installed from the same SSBM Nucleus post.
void partner_of(const Slot& s, int character, Pick* p) {
  p->partner_target = host::skins::partner_slot((uint8_t)character, (uint8_t)s.costume);
  if (p->partner_target.empty()) return;
  p->partner_asset = host::cosmetics::partner_skin(s.asset_id, p->partner_target);
  if (!p->partner_asset.empty()) return;
  slippi::skin_wire::FileRef own;
  if (!host::skins::origin_of(s.asset_id, &own)) return;
  std::string only;
  int found = 0;
  for (const auto& a : host::cosmetics::assets()) {
    slippi::skin_wire::FileRef ref;
    if (a.kind != "character_costume" || lower(a.target_path) != lower(p->partner_target) || !host::skins::origin_of(a.id, &ref) ||
        ref.mod_id != own.mod_id)
      continue;
    only = a.id;
    ++found;
  }
  if (found == 1) p->partner_asset = only;
}

bool test_retail() {
  const char* hook = host::test_env::automated_getenv("MELEE_TEST_SLOTS_RETAIL");
  return hook && *hook && *hook != '0';
}

}  // namespace

std::vector<Slot> list(int character, uint32_t* generation) {
  std::vector<Slot> out;
  for (auto& [c, s] : all_slots(generation))
    if (c == character) out.push_back(std::move(s));
  return out;
}

uint32_t generation() {
  uint32_t g = 0;
  all_slots(&g);
  return g;
}

bool pick(int port, int character, int index, bool local_online, int* costume, std::string* why) {
  *costume = -1;
  why->clear();
  if ((!local_online && (port < 0 || port >= kPorts)) || character < 0 || character >= kCharacters) {
    *why = "no such port or fighter";
    return false;
  }
  Pick next;
  if (index > 0) {
    const auto slots = list(character);
    if (index > (int)slots.size()) {
      *why = "no such slot";
    } else {
      const Slot& s = slots[(size_t)index - 1];
      next.character = character;
      next.costume = s.costume;
      next.asset_id = s.asset_id;
      next.target = s.target;
      partner_of(s, character, &next);
      std::lock_guard<std::mutex> lock(g_mutex);
      // Offline the skin as the game can load it (costume_offline_form). The online door keeps a skin
      // that has no online form too, when it loads offline: the slot then plays the retail costume it
      // was made for, on both machines (served_for answers null, online_announce names nothing).
      const Built& offline = build_locked(s.asset_id, s.target, false);
      const Built* b = local_online ? &build_locked(s.asset_id, s.target, true) : &offline;
      if (!offline.bytes) {
        *why = offline.reason;
        next = Pick{};
      } else {
        next.strict = b->bytes && b->strict;
        if (!next.partner_asset.empty() && !build_locked(next.partner_asset, next.partner_target, local_online).bytes)
          next.partner_asset.clear();
      }
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Pick& slot = local_online ? g_online : g_picks[port];
  if (slot.asset_id != next.asset_id || slot.character != next.character) {
    if (next.asset_id.empty())
      host::log("costume slots: %s: the retail colour%s", local_online ? "online pick" : ("port " + std::to_string(port + 1)).c_str(),
                why->empty() ? "" : (" (" + *why + ")").c_str());
    else
      host::log("costume slots: %s: %s for %s (costume %d)%s%s", local_online ? "online pick" : ("port " + std::to_string(port + 1)).c_str(),
                next.asset_id.c_str(), next.target.c_str(), next.costume, next.partner_asset.empty() ? "" : ", with ",
                next.partner_asset.c_str());
  }
  slot = next;
  *costume = next.costume;
  return index == 0 || !next.asset_id.empty();
}

void clear() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& p : g_picks) p = Pick{};
  g_online = Pick{};
  // A picture that was missing may have come since (the Mods menu fetches a skin's companions).
  for (auto it = g_pictures.begin(); it != g_pictures.end();) it = it->second.empty() ? g_pictures.erase(it) : std::next(it);
}

std::shared_ptr<const std::vector<uint8_t>> served_for(int port, const std::string& slot, bool online, int local_port,
                                                       std::string* asset_id) {
  asset_id->clear();
  if (port < 0 || port >= kPorts || slot.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Pick* p = online ? (port == local_port ? &g_online : nullptr) : &g_picks[port];
  const std::string low = lower(slot);
  bool main = p && !p->asset_id.empty() && lower(p->target) == low;
  const bool partner = p && !p->partner_asset.empty() && lower(p->partner_target) == low;
  // Online the player's own pick follows them into the color actually played (their team's, say):
  // the skin is built for that slot, its symbols renamed to it and its skeleton made that slot's
  // retail one, as for any color (a Captain Falcon skin made for his default color and played in
  // green ends with the green costume's own skeleton; port_skin_corpus checks every pairing).
  if (!main && !partner && online && p && !p->asset_id.empty()) {
    const auto at = places().find(low);
    if (at != places().end() && at->second.character == p->character) {
      main = true;
      static std::set<std::string> told;
      if (told.insert(p->asset_id + "|" + low).second)
        host::log("costume slots: online pick %s for %s is worn in %s, the color played", p->asset_id.c_str(),
                  p->target.c_str(), slot.c_str());
    }
  }
  if (!main && !partner) {
    // Nana's (or Sheik's) file of a pick leaves what the port wears alone.
    if (!(p && !p->partner_target.empty() && lower(p->partner_target) == low)) g_worn[port].clear();
    return nullptr;
  }
  if (test_retail()) {
    static bool said = false;
    if (!said) { said = true; host::log("costume slots: MELEE_TEST_SLOTS_RETAIL: the picks keep their slots, the retail files are loaded"); }
    if (main) g_worn[port].clear();
    return nullptr;
  }
  const std::string& asset = main ? p->asset_id : p->partner_asset;
  const Built& b = build_locked(asset, slot, online);
  if (main) g_worn[port] = b.bytes ? asset : std::string();
  if (b.bytes) *asset_id = asset;
  return b.bytes;
}

std::string online_announce(int character, int costume, std::string* partner_asset) {
  partner_asset->clear();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_online.asset_id.empty() || g_online.character != character || g_online.costume != costume || !g_online.strict) return {};
  if (!g_online.partner_asset.empty() && build_locked(g_online.partner_asset, g_online.partner_target, true).strict)
    *partner_asset = g_online.partner_asset;
  return g_online.asset_id;
}

bool plays_online(const std::string& asset_id, const std::string& slot, std::string* why) {
  why->clear();
  if (asset_id.empty() || slot.empty()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Built& b = build_locked(asset_id, slot, true);
  if (!b.bytes) *why = b.reason;
  return b.bytes != nullptr;
}

const std::vector<uint8_t>* picture(int character, int index, int what) {
  if ((what != 1 && what != 2) || index < 1) return nullptr;
  const auto slots = list(character);
  if (index > (int)slots.size()) return nullptr;
  const std::string id = slots[(size_t)index - 1].asset_id;
  const std::string key = id + "|" + std::to_string(what);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto have = g_pictures.find(key);
    if (have != g_pictures.end()) return have->second.empty() ? nullptr : &have->second;
  }
  const auto all = host::cosmetics::assets();
  const auto* a = find_asset(all, id);
  const std::string path = !a ? std::string() : what == 1 ? a->csp_path : a->stock_path;
  std::vector<uint8_t> px = host::roster::console_picture(path, what == 1 ? 136 : 24, what == 1 ? 188 : 24);
  if (!path.empty() && px.empty()) host::log("costume slots: %s: its %s could not be read", id.c_str(), what == 1 ? "portrait" : "stock icon");
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& kept = g_pictures[key];
  kept = std::move(px);
  return kept.empty() ? nullptr : &kept;
}

const std::vector<uint8_t>* worn_stock(int port) {
  if (port < 0 || port >= kPorts) return nullptr;
  std::string id;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    id = g_worn[port];
  }
  if (id.empty()) return nullptr;
  const std::string key = id + "|2";
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto have = g_pictures.find(key);
    if (have != g_pictures.end()) return have->second.empty() ? nullptr : &have->second;
  }
  const auto all = host::cosmetics::assets();
  const auto* a = find_asset(all, id);
  std::vector<uint8_t> px = a ? host::roster::console_picture(a->stock_path, 24, 24) : std::vector<uint8_t>();
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& kept = g_pictures[key];
  kept = std::move(px);
  return kept.empty() ? nullptr : &kept;
}

}  // namespace host::costume_slots
