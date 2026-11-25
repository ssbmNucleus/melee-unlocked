// Source Port host: mod layers. Known packs and their fingerprints, the Mods folder's choices, a pack's
// costume verdicts and online stage copies, the loose-file overlay, --mod-*, the mod flags, Melee
// Maker's cache.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {
// A pack costume with the standard skeleton (a look-only pack such as a one-click mod look) also takes
// the player's own Mods-tab skin: that skin was checked against the standard costume, which this file
// matches. A pack that changes the skeleton keeps its costume, as before.
bool pack_costume_takes_skins(uint32_t entry) { return g_pack_skin_served.count((int32_t)entry) != 0; }
bool read_pack_online(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kPackOnlineBase || offset >= 0x70000000u || g_pack_online_files.empty()) return false;
  for (const auto& f : g_pack_online_files) {
    if (offset < f.start || offset >= f.start + (uint32_t)f.data.size()) continue;
    // Reads come in 32-byte rounded chunks: the tail past the file reads as zeros, within its slot.
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
  return false;
}

// True for a stage file that is no stage on its own: it has a stage map and no collision data. An
// m-ex stage stub is one (its code, in the file, loads the real stage from a folder of alternates).
// The Source Port runs no code from a file, so serving it gave a stage nothing stands on.
bool stage_file_is_stub(const std::vector<uint8_t>& b) {
  auto word = [&](size_t at) { return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3]; };
  if (b.size() < 0x20 || word(0) != b.size()) return false;
  const uint64_t data = word(4), relocations = word(8), roots = word(12), references = word(16);
  const uint64_t table = 0x20 + data + relocations * 4, strings = table + (roots + references) * 8;
  if (!roots || roots > 1024 || references > 1024 || strings > b.size()) return false;
  bool map = false, collision = false;
  for (uint64_t i = 0; i < roots; ++i) {
    const uint64_t name = strings + word((size_t)(table + i * 8 + 4));
    if (name >= b.size()) return false;
    const char* text = (const char*)b.data() + name;
    const size_t length = strnlen(text, b.size() - (size_t)name);
    const std::string symbol(text, length);
    map |= symbol == "map_head";
    collision |= symbol == "coll_data";
  }
  return map && !collision;
}
// The map parts of a stage file (map_head's table of 0x34-byte entries): for each, whether its model
// joint is there (a pointer is there when the archive relocates it). False when the file has no
// readable map_head. Upstream 9e0aa649.
bool stage_map_parts(const std::vector<uint8_t>& b, std::vector<bool>* parts) {
  auto word = [&](uint64_t at) { return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3]; };
  parts->clear();
  if (b.size() < 0x20 || word(0) != b.size()) return false;
  const uint64_t data = word(4), relocations = word(8), roots = word(12), references = word(16);
  const uint64_t table = 0x20 + data + relocations * 4, strings = table + (roots + references) * 8;
  if (!roots || roots > 1024 || references > 1024 || strings > b.size()) return false;
  uint64_t head = UINT64_MAX;
  for (uint64_t i = 0; i < roots; ++i) {
    const uint64_t name = strings + word(table + i * 8 + 4);
    if (name >= b.size()) return false;
    const char* text = (const char*)b.data() + name;
    if (std::string(text, strnlen(text, b.size() - (size_t)name)) == "map_head") head = word(table + i * 8);
  }
  if (head == UINT64_MAX || 0x20 + head + 0x10 > 0x20 + data) return false;
  const uint64_t entries = word(0x20 + head + 8), count = word(0x20 + head + 0x0C);
  if (count > 256 || 0x20 + entries + count * 0x34 > 0x20 + data) return false;
  std::unordered_set<uint32_t> relocated;
  relocated.reserve((size_t)relocations);
  for (uint64_t i = 0; i < relocations; ++i) relocated.insert(word(0x20 + data + i * 4));
  for (uint64_t i = 0; i < count; ++i) parts->push_back(relocated.count((uint32_t)(entries + i * 0x34)) != 0);
  return true;
}

// Known packs, by the content fingerprint of their layer (see iso_layer_fingerprint). The 20XX TE line
// only matches the untouched official file; any 20XX TE v2d r4 save, including one Melee has saved
// records into, is recognized by its code payload (mods::check_te_file, used below and by the scan).
struct KnownPack { const char* fingerprint; const char* name; };
const KnownPack kKnownPacks[] = {
  {"9a1a48b999a2f8f4deaee2554e5f177cb265666b95676a508406db80b181246b", "Akaneia 1.0.1"},
  {"61834b65d921668ff11702c458a1ae6946172b0a4c8321a47e2c2ddff9aef51d", "20XX TE 2d r4"},
  // An m-ex build with its own camera, hitbox and HUD codes (run-source/arcade-0980/STUDY.md): its
  // files load as they are; its codes are native (shim/mu_arcade.c and the Arcade look switches).
  {"23859e58d0f6153bbc9a813f4eac2b3ae1b69e90d5e5a0ef1f57829b1ed588ff", "2D-style build 2.0"},
  // Its later demo (the camera code and stage reskins published with it): the same main.dol and
  // code list, except three draw-only additions (ledge snap boxes and a few move-specific hitbox
  // shapes) in place of the five always-on hitbox switches, which the Arcade look's switches cover.
  {"59933964cb939eb7e32c05f4b9cdfb332dd12043a3c82ccb9d1f204c7a314891", "2D-style build demo"},
};
constexpr const char kArcadePackName[] = "2D-style build";
bool arcade_pack_name(const char* name) { return std::strncmp(name, kArcadePackName, sizeof kArcadePackName - 1) == 0; }

// Session identity must be derived from this launch's loaded layer bytes.
std::string iso_layer_fingerprint(const std::filesystem::path& iso, const std::string& profile) {
  const auto begin = std::chrono::steady_clock::now();
  const auto fingerprint = layer_fingerprint(g_mod_overlay, profile, [] { return mods::Sha256{}; });
  host::log("mods: fingerprinted %s in %.1f s", iso.u8string().c_str(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
  return fingerprint;
}

std::string dir_layer_fingerprint(const std::string& profile) {
  mods::Sha256 total;
  for (const auto& file : g_mod_overlay.files()) {
    if (file.profile != profile) continue;
    total.update(file.path + "\t" + std::to_string(file.length) + "\n");
    total.update(file.bytes.data(), file.bytes.size());
  }
  return total.hex();
}

// ---- the Mods folder (mod_scan.h) ----
// Normal launches only (the settings file was read): the memory card folder check, the scan of the
// Mods folder and the player's on/off choices. The packs found apply when neither a saved profile
// nor --mod-* flags chose this session's mods; the Mods page lists them either way. 20XX TE is not a
// layer here (its save stays out of the card): it switches on TE's native features. Card mods and
// code mods need the Static Recomp and are not loaded.
void apply_detected_mods() {
   // startup() scans/protects the ordinary card
  if (!gx::RenderOptions::mod_features_available() || !mods::auto_detect()) return;
  mods::StartupOptions options;
  options.card_dir = std::filesystem::u8path(host::options.card_dir);
  options.base_iso = std::filesystem::u8path(host::options.iso);
  options.source_port = true;
  const mods::Startup found = mods::startup(options);
  for (const auto& line : found.log) host::log("mods: %s", line.c_str());
  // Every mod disc found, loaded here or not, offers its costumes in the skin list (the same list
  // on both engines). A skin picked there for a file a loaded pack replaces is skipped below, so a
  // loaded pack's own costume wins.
  mods::scan_detected_disc_skins(options.base_iso);
  if (!g_mod_layers.empty()) {
    const std::string chooser = g_mod_profile_name.empty() ? std::string("the command line") : "profile " + g_mod_profile_name;
    host::log("mods: %s chose this session's mods; the Mods folder's packs are only listed", chooser.c_str());
    return;
  }
  mods::status().detected = true;
  mods::status().te_owned = found.te;
  g_mod_layers.insert(g_mod_layers.end(), found.layers.begin(), found.layers.end());
}

// Validate local cosmetics against the original disc before keeping them in the retail view.
// Gameplay files and unverified replacements continue to use their original copies online.
void compute_pack_skin_verdicts(const std::unordered_map<std::string, int32_t>& base_paths,
                                const std::vector<FstFile>& base_fst) {
  g_pack_skins.clear();
  g_pack_skin_served.clear();
  g_pack_online_entry.clear();
  g_pack_online_files.clear();
  g_pack_online_next = kPackOnlineBase;
  auto& notes = mods::status().pack_skins;
  notes.clear();
  for (const auto& file : g_mod_overlay.files()) {
    const bool costume = skins::is_costume_file(file.path);
    const bool stage = skins::is_stage_file(file.path);
    const bool music = skins::is_music_file(file.path);
    if (!costume && !stage && !music) continue;
    const auto now = g_paths.find(file.path);
    const auto base = base_paths.find(file.path);
    if (now == g_paths.end() || base == base_paths.end() || base_fst[base->second].dir) continue;
    const auto alias = g_view_alias.find(now->second);
    if (alias == g_view_alias.end() || alias->second < 0) continue;   // a costume only the pack has
    skins::Verdict verdict;
    verdict.path = skins::display_name(file.path);
    verdict.entry = now->second;
    // The layer label is "layer N <path>": the panel shows the file or folder name.
    const size_t name_at = file.profile.find(' ', file.profile.find(' ') + 1);
    verdict.layer = name_at == std::string::npos ? file.profile
                    : std::filesystem::u8path(file.profile.substr(name_at + 1)).filename().u8string();
    const FstFile& standard = base_fst[base->second];
    constexpr uint32_t limit = 64u * 1024 * 1024;
    if (file.length > limit || standard.length > limit) {
      verdict.reason = "replacement exceeds 64 MB";
      notes.push_back({verdict.path, verdict.layer, verdict.reason, false, false});
      g_pack_skins.push_back(std::move(verdict));
      continue;
    }
    std::vector<uint8_t> pack(file.length), clean(standard.length);
    if (pack.empty() || clean.empty() ||
        g_mod_overlay.read(file.start, pack.data(), file.length) != ModOverlay::Read::Success ||
        !host::disc_read(standard.offset, clean.data(), standard.length)) {
      verdict.reason = "the file could not be read";
    } else if (pack == clean) {
      verdict.identical = verdict.served = true;
      verdict.reason = "same as the original file";
    } else {
      std::string detail;
      verdict.served = costume ? host::cosmetics::costume_skeleton_matches(clean, pack, &detail)
                     : stage ? host::cosmetics::stage_safety::matches(clean, pack, &detail)
                             : skins::music_is_valid(pack, &detail);
      verdict.reason = host::cosmetics::online_reason_short(detail);
      // A stage that is not the same file may still be the same stage: the pack's look drawn over
      // the original's gameplay, as an installed stage skin gets. Served from memory in that view.
      std::vector<uint8_t> online;
      std::string graft_detail;
      if (stage && !verdict.served &&
          host::cosmetics::stage_online_form(clean, pack, &online, &graft_detail, file.path) && !online.empty()) {
        const uint32_t span = ((uint32_t)online.size() + 0xFFFFu) & ~0xFFFFu;
        if (online.size() < 64u * 1024 * 1024 && (uint64_t)g_pack_online_next + span <= 0x70000000ull) {
          const int32_t grafted = (int32_t)g_fst.size();
          g_fst.push_back({g_pack_online_next, (uint32_t)online.size(), false});
          g_pack_online_files.push_back({g_pack_online_next, std::move(online)});
          g_pack_online_next += span;
          g_pack_online_entry[verdict.entry] = grafted;
          verdict.served = true;
          verdict.reason = "its look drawn over the standard stage's gameplay";
          host::log("mods: cosmetic %s online as %s", verdict.path.c_str(), graft_detail.c_str());
        }
      }
    }
    if (verdict.served && !g_pack_online_entry.count(verdict.entry)) g_pack_skin_served.insert(verdict.entry);
    host::log("mods: cosmetic %s %s online (%s)", verdict.path.c_str(), verdict.served ? "stays on" : "swapped",
              verdict.reason.c_str());
    notes.push_back({verdict.path, verdict.layer, verdict.reason, verdict.served, verdict.identical});
    g_pack_skins.push_back(std::move(verdict));
  }
}

void load_mod_overlay() {
  if (host::gameplay::vanilla_only()) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  apply_detected_mods();
  if (!host::gameplay::mod_autodetect_disabled && !g_mod_root_explicit) {
    if (fs::exists(g_mod_root, ec) && !ec)
      g_mod_layers.push_back({mods::LayerKind::Dir, g_mod_root});
  }
  if (g_mod_layers.empty()) return;
  std::string error;
  g_mod_overlay.set_layered(true);
  // Compare each imported ISO against the original disc, never against a previously
  // overlaid pack, so what a pack changes does not depend on the packs before it.
  const auto base_paths = g_paths;
  const auto base_fst = g_fst;
  auto lookup = [&](const std::string& path, ModOverlay::DiscFile* out) {
    const auto it = base_paths.find(path);
    if (it == base_paths.end() || base_fst[it->second].dir) return false;
    *out = {base_fst[it->second].offset, base_fst[it->second].length};
    return true;
  };
  auto read_base = [](uint32_t offset, void* dst, uint32_t size) {
    return host::disc_read(offset, dst, size);
  };
  {
    ModOverlay::BaseDisc base;
    for (const auto& kv : base_paths) if (!base_fst[kv.second].dir) base.files.push_back(kv.first);
    std::sort(base.files.begin(), base.files.end());
    uint8_t header[0x440];
    uint8_t dol[0x100];
    if (host::disc_read(0, header, sizeof header) && host::disc_read(be32(header + 0x420), dol, sizeof dol)) {
      base.dol_offset = be32(header + 0x420);
      uint32_t extent = sizeof dol;
      for (int i = 0; i < 18; ++i) extent = std::max(extent, be32(dol + i * 4) + be32(dol + 0x90 + i * 4));
      base.dol_size = extent;
    }
    g_mod_overlay.set_base_disc(std::move(base), read_base);
  }
  mods::Sha256 identity;
  std::string summary;
  uint32_t layer_index = 0;
  for (const auto& layer : g_mod_layers) {
    const std::string label = "layer " + std::to_string(++layer_index) + " " + layer.path.u8string();
    const bool explicit_layer = std::any_of(g_explicit_mod_layers.begin(), g_explicit_mod_layers.end(),
        [&](const mods::Layer& selected) { return selected.kind == layer.kind && selected.path == layer.path; });
    mods::Detected detected;
    const auto overlay_failed = [&] {
      host::die("mods: %s", error.c_str());
    };
    std::string fingerprint;
    switch (layer.kind) {
    case mods::LayerKind::Iso:
      if (!g_mod_overlay.add_iso(layer.path, label, lookup, read_base, error)) overlay_failed();
      fingerprint = iso_layer_fingerprint(layer.path, label);
      break;
    case mods::LayerKind::Dir:
      if (!g_mod_overlay.add_directory(layer.path, label, error)) overlay_failed();
      fingerprint = dir_layer_fingerprint(label);
      break;
    case mods::LayerKind::Gci: {
      fingerprint = mods::sha256_file(layer.path);
      std::string gci_id;
      if (fingerprint.empty() || !mods::gci_identity(layer.path, &gci_id, &error))
        host::die("mods: cannot read card file %s", layer.path.u8string().c_str());
      g_mod_gcis.push_back(layer.path);
      g_card_imports.push_back({layer.path, gci_id, fingerprint});
      break;
    }
    }
    const char* kind = layer.kind == mods::LayerKind::Iso ? "iso" : layer.kind == mods::LayerKind::Dir ? "dir" : "gci";
    identity.update(std::string(kind) + "\t" + fingerprint + "\n");
    const KnownPack* known = nullptr;
    for (const auto& pack : kKnownPacks) if (*pack.fingerprint && fingerprint == pack.fingerprint) known = &pack;
    if (known && std::strncmp(known->name, "20XX TE", 7) == 0) mods::status().te_owned = true;
    if (!known && layer.kind == mods::LayerKind::Gci) {
      const mods::TeCheck te = mods::check_te_file(layer.path);
      if (te.supported()) {
        mods::status().te_owned = true;
        host::log("mods: %s is 20XX TE %s (%u of %u payload bytes intact)", layer.path.u8string().c_str(),
                  te.version.c_str(), te.matching_bytes, te.payload_bytes);
      }
    }
    if (known && std::strncmp(known->name, "Akaneia", 7) == 0) mods::status().akaneia = true;
    if (known && arcade_pack_name(known->name)) mods::status().arcade = true;
    host::log("mods: %s %s, content %s%s%s", kind, layer.path.u8string().c_str(), fingerprint.substr(0, 16).c_str(),
              known ? ", recognized as " : "", known ? known->name : "");
    summary += (summary.empty() ? "" : ", ") + (known ? std::string(known->name) : layer.path.filename().u8string());
  }
  g_mod_fingerprint = identity.hex();
  for (const auto& report : g_mod_overlay.iso_reports()) {
    uint64_t dol_bytes = 0;
    for (const auto& run : report.dol_runs) dol_bytes += run.second;
    host::log("mods: %s: %u files changed or added, %zu retail files missing, main.dol %u bytes (retail %u), "
              "%zu code patch runs over %llu bytes", report.profile.c_str(), report.changed_files, report.deleted.size(),
              report.dol_size, report.base_dol_size, report.dol_runs.size(), (unsigned long long)dol_bytes);
    if (!report.dol_runs.empty() || report.dol_size != report.base_dol_size)
      host::log("mods: %s patches the game's code; the Source Port runs its own source code, so only the pack's files apply",
                report.profile.c_str());
  }
  for (const auto& conflict : g_mod_overlay.conflicts())
    host::log("mods: %s is in both %s and %s; the later one is used", conflict.path.c_str(),
              conflict.earlier.c_str(), conflict.later.c_str());

  // Online policy: a single ISO carrying the m-ex tables and Akaneia's added fighters is treated
  // as Akaneia (Direct only); anything else is another mod.
  auto has = [](const char* path) {
    return std::any_of(g_mod_overlay.files().begin(), g_mod_overlay.files().end(),
        [path](const ModOverlay::File& file) { return file.path == path; });
  };
  // Training mods (Training Mode CE's disc) only feed their own training events, never a match, so
  // they are left out of what decides how this session plays online (owner 11-17): Animelee with
  // Training Mode CE on is still a look, and Training Mode CE alone plays anyone.
  auto training_disc = [](const mods::Layer& layer) {
    if (layer.training) return true;   // marked by the Mods folder scan
    if (layer.kind != mods::LayerKind::Iso) return false;
    return std::any_of(g_mod_overlay.files().begin(), g_mod_overlay.files().end(), [&](const ModOverlay::File& f) {
      if (f.path != "/tm/eventmenu.dat") return false;
      std::error_code ec;
      return f.iso == layer.path || std::filesystem::equivalent(f.iso, layer.path, ec);
    });
  };
  std::vector<const mods::Layer*> play_layers;
  for (const auto& layer : g_mod_layers) if (!training_disc(layer)) play_layers.push_back(&layer);
  g_training_only = !g_mod_layers.empty() && play_layers.empty();
  if (play_layers.size() != g_mod_layers.size())
    host::log("mods: %s; online matches leave training mods out", g_training_only ? "only training mods are on" : "training mods are on with other mods");
  const bool one_iso = play_layers.size() == 1 && play_layers[0]->kind == mods::LayerKind::Iso;
  g_cosmetic_pack = one_iso && host::cosmetics::official_cosmetic_disc(play_layers[0]->path.u8string());
  if (g_cosmetic_pack)
    host::log("mods: verified Animelee look; online uses standard gameplay with verified costumes and music");
  const bool akaneia = one_iso && has("/mxdt.dat") && has("/plsn.dat") && has("/plts.dat");
  if (akaneia) host::log("mods: Akaneia profile detected; its content online is Direct-only");
  // The 2D-style build: its hitbox code is the Arcade look's hitbox switches, on for this profile.
  // Its camera code runs in the game (MU_GAME_OPTION2_ARCADE_PACK, offline); online, where the game
  // leaves it off, the renderer's 2D camera gives the same flat look.
  if (mods::status().arcade) {
    // Not our grid: the build's own stages carry theirs, and on other stages ours covered the scenery.
    gx::set_arcade_display(gx::ArcadeSource::Pack, (gx::kArcadeGameBits & ~gx::kArcadeGrid) | gx::kArcadeCamera2d);
    host::log("mods: %s found; its codes run natively", kArcadePackName);
  }
  // Training Mode CE: its disc's TM/ files carry the event menus and lab models; its code is
  // compiled into the native game (sourceport/game/tmce). Like the original, vanilla base only.
  if (has("/tm/eventmenu.dat")) {
    if (akaneia || mods::status().akaneia) {
      host::log("mods: Training Mode CE files found, but Training Mode CE needs the vanilla game; left off");
    } else {
      mods::status().tmce = true;
      host::log("mods: Training Mode CE files found; its events run natively");
    }
  }
  g_direct_fingerprint.clear();
  // rogueMelee: its disc is the 1.02 game under the id GALERM. Its menu files load like any pack's;
  // its code is compiled into the native game (sourceport/game/rogue). Single player, offline.
  if (one_iso) {
    char id[6] = {};
    std::ifstream disc(play_layers[0]->path, std::ios::binary);
    if (disc.read(id, sizeof id) && std::memcmp(id, "GALERM", sizeof id) == 0) {
      mods::status().rogue = true;
      host::log("mods: rogueMelee disc found; Rogue Mode runs natively (1-P Mode > Regular Match)");
    }
    // Melee Maker: the 1.02 game's id; its main.dol carries its build id and the bone tables the
    // native engine (sourceport/game/maker) reads through command 0xEE (maker_source.h).
    if (!mods::status().rogue && maker::attach(play_layers[0]->path)) {
      mods::status().maker = true;
      host::log("mods: Melee Maker %s disc found; its builds run natively", maker::kBuild);
      // Direct: the fingerprint the Static Recomp sends for this disc too (host.cpp), so players on
      // either engine can play each other.
      const std::string shared = maker::fingerprint(mods::sha256_file(play_layers[0]->path));
      g_direct_fingerprint = shared;
    }
  }
  // The 20XX Hack Pack (hackpack_source.h): its stages, music and costumes load like any other
  // pack's files; its menu files and the files that carry its code do not (pack_keeps below).
  std::filesystem::path pack_iso;
  for (const auto& file : g_mod_overlay.files()) {
    if (!hackpack::is_pack_marker(file.path) || file.iso.empty() || !has("/mnslchr.0sd")) continue;
    pack_iso = file.iso;
    hackpack::attach(file.iso);
    mods::status().hackpack = true;
    host::log("mods: 20XX Hack Pack files found in %s; its content loads with the retail menus, none of its code runs",
              file.iso.filename().u8string().c_str());
  }
  const auto pack_keeps = [&](const ModOverlay::File& file) {
    return !pack_iso.empty() && file.iso == pack_iso && hackpack::keeps_retail(file.path);
  };
  // The pack's extra costumes (PlFxNr.lat, PlFxNr.rat and the other sets) become skins of their
  // costume slot, the same scan that lists a Mods folder disc's costumes (a disc scanned before and
  // unchanged is not read again), so L / R on the character select steps through them. The pack is
  // this session's disc: its plain costumes are already the standard ones, and its sets may serve
  // a costume file it replaced (load_cosmetics, republish_cosmetic_slot).
  if (!pack_iso.empty()) {
    const auto scan = host::cosmetics::scan_disc_skins(pack_iso.u8string(), pack_iso.stem().u8string() + " disc");
    if (!scan.ok) host::log("mods: 20XX Hack Pack costumes not listed as skins: %s", scan.message.c_str());
    host::cosmetics::set_session_pack(pack_iso.u8string());
  }

  // The profile's own memory card, named after the profile, or after the content for layers given
  // on the command line. The ordinary card is never written by a modded session.
  {
    const fs::path ordinary = fs::u8path(host::options.card_dir);
    const std::string folder = !g_mod_profile_name.empty() ? mods::safe_name(g_mod_profile_name)
                                                          : "mods-" + g_mod_fingerprint.substr(0, 12);
    g_profile_card = ordinary.parent_path() / "Profiles" / fs::u8path(folder);
    std::vector<std::string> notes;
    if (!mods::prepare_profile_card(ordinary, g_profile_card, g_card_imports, &notes, &error))
      host::die("mods: %s", error.c_str());
    for (const auto& note : notes) host::log("card: %s", note.c_str());
  }
  {
    auto& status = mods::status();
    status.profile = g_mod_profile_name;
    status.identity = g_mod_fingerprint;
    status.layers.clear();
    for (const auto& layer : g_mod_layers) {
      const char* kind = layer.kind == mods::LayerKind::Iso ? "Disc" : layer.kind == mods::LayerKind::Dir ? "Files" : "Save";
      status.layers.push_back(std::string(kind) + ": " + layer.path.filename().u8string());
    }
    status.notes.clear();
    for (const auto& report : g_mod_overlay.iso_reports())
      if (status.arcade)   // every run and code of this build has a native equivalent (run-source/arcade-0980/STUDY.md)
        status.notes.push_back(std::string(kArcadePackName) + ": its code patches run natively, and its hitbox, camera "
                               "and HUD looks are on for this profile.");
      else if (!report.dol_runs.empty() || report.dol_size != report.base_dol_size)
        status.notes.push_back(std::to_string(report.dol_runs.size()) +
                               " code patches in a modded disc are not applied: the Source Port runs its own game code.");
    for (const auto& conflict : g_mod_overlay.conflicts())
      status.notes.push_back(conflict.path.substr(1) + " is in two layers; the later one is used.");
    status.notes.push_back("Memory card: " + g_profile_card.u8string());
  }
  host::log("mods: profile %s, identity %s, %zu layers: %s",
            g_mod_profile_name.empty() ? "(command line)" : g_mod_profile_name.c_str(),
            g_mod_fingerprint.substr(0, 16).c_str(), g_mod_layers.size(), summary.c_str());

  // A stage stub of a dynamic alternate stages disc is not served: the game's own stage file stays,
  // and the disc's alternates of that stage ("GrSt/GrSt-surfshop(L).dat") are picked each match the
  // way the stub's code picks them (held L, R, Z, B, X or Y names one, otherwise any of them).
  std::unordered_set<std::string> stage_stubs;
  std::filesystem::path stub_disc;
  for (const auto& file : g_mod_overlay.files()) {
    if (!skins::is_stage_file(file.path) || !file.length || file.length > 1024u * 1024u) continue;
    // Only a file in place of one of the game's own stages is a stub: a stage file the disc adds has
    // no game stage to keep, and leaving it out lost it (ACE's Smashville sunset map GrAcSs.dat: every
    // evening match there stopped the game, 0.9.201 report).
    if (g_paths.find(file.path) == g_paths.end()) continue;
    std::vector<uint8_t> bytes(file.length);
    if (g_mod_overlay.read(file.start, bytes.data(), file.length) != ModOverlay::Read::Success ||
        !stage_file_is_stub(bytes)) continue;
    stage_stubs.insert(file.path);
    if (!file.iso.empty()) stub_disc = file.iso;
    host::log("mods: %s is a stage stub (its stage code is not run here); the game's stage stays, with the disc's alternates",
              skins::display_name(file.path).c_str());
  }
  if (!stub_disc.empty()) {
    std::string name = host::cosmetics::disc_pack_name(stub_disc.u8string());
    if (name.empty()) name = stub_disc.stem().u8string() + " disc";
    const auto scan = host::cosmetics::scan_disc_skins(stub_disc.u8string(), name);
    if (!scan.ok) host::log("mods: alternate stages of %s not listed: %s", stub_disc.filename().u8string().c_str(), scan.message.c_str());
    host::cosmetics::set_session_stage_pack(stub_disc.u8string());
  }
  // A disc file in place of one of the game's archives (.dat/.usd) whose own header names another size
  // can never be loaded: HSD_ArchiveParse stops the game (a rebuilt disc's zeroed NtMemAc.usd ended every
  // boot that showed the memory card notice, 1.0.0 reports). The game's own file stays for it.
  std::unordered_set<std::string> broken_archives;
  for (const auto& file : g_mod_overlay.files()) {
    const auto found = g_paths.find(file.path);
    if (found == g_paths.end() || g_fst[found->second].dir || stage_stubs.count(file.path)) continue;
    const std::string& path = file.path;
    if (path.size() < 4 || (path.compare(path.size() - 4, 4, ".dat") != 0 && path.compare(path.size() - 4, 4, ".usd") != 0)) continue;
    const FstFile& retail = g_fst[found->second];
    uint8_t own[4] = {}, game[4] = {};
    if (retail.length < 0x20 || !host::disc_read(retail.offset, game, 4) || be32(game) != retail.length) continue;
    if (file.length >= 4 && g_mod_overlay.read(file.start, own, 4) == ModOverlay::Read::Success && be32(own) == file.length) continue;
    broken_archives.insert(path);
    host::log("mods: %s in the disc is not a whole archive (header %08X, file %u bytes): the game's own file stays",
              path.c_str(), be32(own), file.length);
  }
  // A disc stage file in place of one of the game's stages that lacks a map part the game's own file
  // has: the stage's code makes an object for each of its parts by number, and one that is not there
  // was a read through a null archive (a custom disc's Pokemon Stadium ended the match as it started).
  // The game's own stage stays for it. The .dat and its English twin (.usd), which English games load
  // in its place. Upstream 9e0aa649 and 22efe234.
  std::unordered_set<std::string> stages_missing_parts;
  for (const auto& file : g_mod_overlay.files()) {
    const auto found = g_paths.find(file.path);
    const std::string& path = file.path;
    const bool stage_file = path.rfind("/gr", 0) == 0 && path.find('/', 1) == std::string::npos && path.size() > 7 &&
                            (path.compare(path.size() - 4, 4, ".dat") == 0 || path.compare(path.size() - 4, 4, ".usd") == 0);
    if (found == g_paths.end() || g_fst[found->second].dir || stage_stubs.count(file.path) ||
        broken_archives.count(file.path) || !stage_file)
      continue;
    const FstFile& retail = g_fst[found->second];
    if (retail.length < 0x20 || retail.length > 64u * 1024u * 1024u || file.length < 0x20 ||
        file.length > 64u * 1024u * 1024u)
      continue;
    std::vector<uint8_t> game(retail.length), own(file.length);
    if (!host::disc_read(retail.offset, game.data(), retail.length) ||
        g_mod_overlay.read(file.start, own.data(), file.length) != ModOverlay::Read::Success)
      continue;
    std::vector<bool> game_parts, own_parts;
    if (!stage_map_parts(game, &game_parts)) continue;
    const bool own_readable = stage_map_parts(own, &own_parts);
    size_t missing = 0;
    for (size_t i = 0; i < game_parts.size(); ++i)
      if (game_parts[i] && (!own_readable || i >= own_parts.size() || !own_parts[i])) ++missing;
    if (!missing) continue;
    stages_missing_parts.insert(file.path);
    host::log("mods: %s in the disc lacks %zu of the %zu map parts the game's own stage has: the game's own stage stays",
              file.path.c_str(), missing, game_parts.size());
  }
  uint32_t replaced = 0, added = 0, pack_kept = 0;
  for (const auto& file : g_mod_overlay.files()) {
    if (stage_stubs.count(file.path) || broken_archives.count(file.path) || stages_missing_parts.count(file.path)) continue;
    if (pack_keeps(file)) { ++pack_kept; continue; }   // retail menus, and no file that carries code
    auto found = g_paths.find(file.path);
    int32_t entry;
    if (found != g_paths.end()) {
      if (g_fst[found->second].dir) host::die("mods: file conflicts with disc directory %s", file.path.c_str());
      entry = found->second; ++replaced;
    } else {
      entry = (int32_t)g_fst.size(); g_fst.push_back({0, 0, false}); g_paths[file.path] = entry; ++added;
    }
    g_fst[entry] = {file.start, file.length, false};
  }
  if (replaced || added) host::log("mods: %u disc files replaced, %u added", replaced, added);
  if (pack_kept)
    host::log("mods: 20XX Hack Pack: %u of its files are not served (its menu files and the files that carry its code)", pack_kept);

  // The retail view of every file the layers changed.
  g_view_alias.clear();
  for (const auto& file : g_mod_overlay.files()) {
    if (pack_keeps(file) || stage_stubs.count(file.path) || broken_archives.count(file.path) ||
        stages_missing_parts.count(file.path))
      continue;   // not served: the disc's own entry, the same in both views
    const auto now = g_paths.find(file.path);
    if (now == g_paths.end()) continue;
    const auto base = base_paths.find(file.path);
    if (base == base_paths.end() || base_fst[base->second].dir) { g_view_alias[now->second] = -1; continue; }
    const int32_t alias = (int32_t)g_fst.size();
    g_fst.push_back(base_fst[base->second]);
    g_view_alias[now->second] = alias;
  }
  g_mod_gameplay_profile = g_view_alias.empty() ? slippi::online::NativeGameplayProfile::Vanilla
                           : akaneia ? slippi::online::NativeGameplayProfile::Akaneia
                                     : slippi::online::NativeGameplayProfile::OtherMod;
  slippi::online::set_native_gameplay_profile(g_mod_gameplay_profile);
  g_mod_display_name = summary.empty() ? std::string("this mod") : summary;
#ifdef MELEE_NO_SLIPPI   // the same fact without the other build's mode names
  if (!g_view_alias.empty())
    host::log("mods: %zu files have a retail view: online matches play the retail game", g_view_alias.size());
#else
  if (!g_view_alias.empty())
    host::log("mods: %zu files have a retail view: Unranked, Teams and Party play the retail game", g_view_alias.size());
#endif
  // After the aliases: the verdicts only say which entries keep their own number in that view. The
  // alias table itself is left whole, because its being empty or not is what decides the gameplay
  // profile above and how replays are tagged.
  if (!g_view_alias.empty()) compute_pack_skin_verdicts(base_paths, base_fst);
}

// Serves a read inside the overlay range; false when the offset is ordinary disc.
bool read_overlay(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  const auto result = g_mod_overlay.read(offset, dst, size);
  *ok = result == ModOverlay::Read::Success;
  return result != ModOverlay::Read::NotOverridden;
}

uint32_t h_mod_flags() {

  return (g_mod_overlay.files().empty() ? 0 : MU_MOD_ASSETS_PRESENT) | (hackpack::loaded() ? MU_MOD_HACKPACK : 0u) |
         (mods::status().rogue ? MU_MOD_ROGUE : 0u) | (mods::status().maker ? MU_MOD_MAKER : 0u);
}

// MU_MAKER_COMMAND's cache op (MU_MAKER_OP_CACHE), from h_slippi_command (source_host.cpp).
bool maker_cache_command(std::vector<uint8_t>& reply) {
  // Melee Maker's loading cache: 32 MB right after the 64 MB GX can address, which then grows
  // to cover it (host::try_ptr), so donor models in it draw. Mapped once, only for its disc.
  static uint32_t cache_size = 0;
  static bool tried = false;
  if (!tried && maker::loaded()) {
    tried = true;
    constexpr uintptr_t base = MEM1_BASE + (64u << 20);
    if (VirtualAlloc((void*)base, 32u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == (void*)base) {
      cache_size = 32u << 20;
      host::ram_size = (64u << 20) + cache_size;
      host::log("mods: Melee Maker cache, %u MB at %08llX", cache_size >> 20, (unsigned long long)base);
    } else {
      host::log("mods: Melee Maker cache could not be mapped; its moves load into the match heap");
    }
  }
  if (cache_size) { append_be32(reply, uint32_t(MEM1_BASE + (64u << 20))); append_be32(reply, cache_size); }
  return true;
}

}  // namespace host_internal


void set_mod_directory(const char* path) {
  g_mod_layers.push_back({mods::LayerKind::Dir, std::filesystem::u8path(path)});
  g_explicit_mod_layers.push_back(g_mod_layers.back());
  g_mod_root_explicit = true;
}

void set_mod_iso(const char* path) {
  g_mod_layers.push_back({mods::LayerKind::Iso, std::filesystem::u8path(path)});
  g_explicit_mod_layers.push_back(g_mod_layers.back());
}

void set_mod_gci(const char* path) {
  g_mod_layers.push_back({mods::LayerKind::Gci, std::filesystem::u8path(path)});
  g_explicit_mod_layers.push_back(g_mod_layers.back());
}

bool set_mod_profile(const char* name, bool explicit_layer) {
  mods::Profile profile;
  std::string error;
  if (!name || !*name || !mods::parse_profile(mods::profile_path(name), &profile, &error)) {
    std::fprintf(stderr, "%s\n", error.empty() ? "--mod-profile needs a name" : error.c_str());
    return false;
  }
  if (!g_mod_profile_name.empty()) { std::fprintf(stderr, "only one --mod-profile per run\n"); return false; }
  g_mod_profile_name = profile.name;
  mods::status().requested = name;
  // The profile's layers come first; command-line layers, wherever they were given, load after it.
  g_mod_layers.insert(g_mod_layers.begin(), profile.layers.begin(), profile.layers.end());
  if (explicit_layer)
    g_explicit_mod_layers.insert(g_explicit_mod_layers.end(), profile.layers.begin(), profile.layers.end());
  return true;
}

const std::string& mod_fingerprint() { return g_mod_fingerprint; }
bool has_mod_layers() { return !g_mod_layers.empty(); }

}  // namespace source_port
