// Source Port host: menu mods from the Mods menu's Menus (host/menu_mods.h), served over the disc.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- menu mods from the Mods menu's Menus (host/menu_mods.h) ----
// A menu screen's file (the character select's MnSlChr.usd) installed from SSBM Nucleus, served in place
// of the disc's copy: its own range between the Slippi system files and the new stages, a new
// entry number at each change (so nothing the game preloaded under the old number is used again) and the
// path opens that entry in every view (a menu file is never read by a match; online, only this player
// sees it). The disc's copy again when the mod is removed or fails its checks (one log line), and always
// in a Vanilla run or a replay. A file the loaded mod pack replaced stays the pack's.
static_assert(kMenuModBase > kSystemFileBase && kMenuModBase < kNewStageBase, "menu mods sit above the system files");

bool read_menu_mod(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kMenuModBase || offset >= kNewStageBase || g_menu_files.empty()) return false;
  *ok = false;
  for (auto& f : g_menu_files) {
    if (offset < f.start || offset >= f.start + (uint32_t)f.data.size()) continue;
    if (!f.read) {   // once a file: the evidence the screen loaded the mod's copy
      f.read = true;
      host::log("menu mods: the game reads %s (%08X+%X)", f.name.c_str(), offset, size);
    }
    const uint32_t at = offset - f.start;
    const uint32_t slot = ((uint32_t)f.data.size() + 0xFFFFu) & ~0xFFFFu;
    *ok = size <= slot - at;
    if (*ok) {
      const uint32_t have = std::min<uint32_t>(size, (uint32_t)f.data.size() - at);
      std::memcpy(dst, f.data.data() + at, have);
      std::memset((uint8_t*)dst + have, 0, size - have);
    }
    return true;
  }
  return true;
}

// Nothing while this release has no Menus (release_scope.h): the installed ones stay on disk, unpublished.
void publish_menu_mods(bool off) {
  if (off) g_menu_mods_off = true;
  if (g_menu_mods_off || host::gameplay::vanilla_only() || host::options.vanilla_game) return;
  if (!host::release_scope::gate(host::release_scope::kMenus, "installed menu mods are not published")) return;
  for (const auto& target : host::menu_mods::targets()) {
    std::string path = "/" + target;
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    host::menu_mods::Installed mod;
    const bool installed = host::menu_mods::installed_for(target, &mod);
    const std::string key = installed ? mod.id + "/" + mod.sha256 : std::string();
    const auto was = g_menu_served.find(path);
    if (was != g_menu_served.end() ? was->second == key : key.empty()) continue;   // nothing changed
    g_menu_served[path] = key;
    const auto it = g_paths.find(path);
    const uint32_t i = it != g_paths.end() && it->second > 0 ? (uint32_t)it->second : 0;
    // The disc's own copy (the raw table's offset): a pack's replacement stays the pack's.
    const bool disc_own = i && (size_t)i * 12 + 12 <= g_fst_raw.size() && i < g_fst.size() &&
                          g_fst[i].offset == be32(g_fst_raw.data() + (size_t)i * 12 + 4);
    std::vector<uint8_t> retail, bytes;
    uint32_t offset = 0, length = 0;
    if (installed && disc_own && host::disc_find_file(target, &offset, &length) && length && length <= host::menu_mods::kMaxBytes) {
      retail.resize(length);
      if (!host::disc_read(offset, retail.data(), length)) retail.clear();
    }
    std::string name;
    if (!installed || !disc_own || !host::menu_mods::served(target, retail, &bytes, &name)) {
      if (installed && !disc_own) host::log("menu mods: %s is the loaded pack's own; %s waits", target.c_str(), mod.name.c_str());
      if (g_menu_paths.erase(path)) host::log("menu mods: %s is the disc's again", target.c_str());
      continue;
    }
    const uint32_t span = ((uint32_t)bytes.size() + 0xFFFFu) & ~0xFFFFu;
    if ((uint64_t)g_menu_next + span > kNewStageBase) {
      host::log("menu mods: %s not served: this run's menu mod range is full (restart to serve it)", target.c_str());
      g_menu_paths.erase(path);
      continue;
    }
    const int32_t entry = (int32_t)g_fst.size();
    g_fst.push_back({g_menu_next, (uint32_t)bytes.size(), false});
    g_menu_files.push_back({g_menu_next, std::move(bytes), target + " of " + name});
    g_menu_next += span;
    g_menu_paths[path] = entry;
    host::log("menu mods: %s opens entry %d (%s, %u bytes at %08X)", target.c_str(), entry, name.c_str(),
              g_fst[(size_t)entry].length, g_fst[(size_t)entry].offset);
  }
}

}  // namespace host_internal
}  // namespace source_port
