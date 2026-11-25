// Source Port host: practice. Training Mode CE's data files, Training Lab savestates, the practice
// bridge, the Practice CPU (MU_CMD_PRACTICE_CPU) and the CPU AI picker (MU_CMD_CPU_AI).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_host_internal.h"

namespace source_port {
namespace host_internal {

// ---- Training Mode CE's data files (TM/ beside the game, publish_training_files) ----

// Serves a read inside the training files' range; false when the offset is outside it.
bool read_training_file(uint32_t offset, void* dst, uint32_t size, bool* ok) {
  if (offset < kTrainingFileBase || offset >= kRosterBase || g_training_files.empty()) return false;
  *ok = false;
  for (const auto& f : g_training_files) {
    if (offset < f.start || offset >= f.start + (uint32_t)f.data.size()) continue;
    // As the system files: the last 32-byte chunk runs past the end, into zeros up to the 64 KB slot.
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

// Training Lab savestates (shim/mu_lab.c): one kept state on the rollback snapshot engine, under a
// frame number no online match uses. The engine frees its slots on a load (rollback semantics), so
// a lab load keeps the state again at once and it can be returned to as often as wanted.
constexpr int32_t kLabFrame = -1000000;
void lab_save() {
  if (!g_savestates.active()) begin_native_savestates();
  g_savestates.capture(kLabFrame);
  host::log("lab: state saved (%.2f ms)", g_savestates.stats().capture_ms_max);
}
void lab_load() {
  if (!g_savestates.active() || !g_savestates.load(kLabFrame)) {
    host::log("lab: no saved state");
    return;
  }
  g_savestates.capture(kLabFrame);
  gx::mark_discontinuity();
  host::log("lab: state loaded");
}

// MU_CMD_PRACTICE_CPU, from h_slippi_command (source_host.cpp).
bool practice_cpu_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  if (n == 2 || n == 3) { practice_cpu::note_status(p[0] != 0, p[1], 0, n == 3 ? p[2] : -1); return true; }
  const MuPcConfig cfg = practice_cpu::config();
  const auto* bytes = reinterpret_cast<const uint8_t*>(&cfg);
  reply.assign(bytes, bytes + sizeof cfg);
  return true;
}

// MU_CMD_CPU_AI, from h_slippi_command (source_host.cpp).
// MU_CMD_CPU_AI, the CPU AI picker (shim/mu_cpu_ai.c): op 0 the remembered choices; op 1 port, style: a
// choice made on the character select; op 2 word (BE32): a match begins, reply the word it plays with.
bool cpu_ai_command(const uint8_t* p, uint32_t n, std::vector<uint8_t>& reply) {
  // Offline only: never in a network session, a Vanilla launch or the vanilla game.
  const bool offline = !host::gameplay::vanilla_only() && !host::options.vanilla_game &&
                       slippi::online::session_mode() < 0 && !source_p2p::requested();
  if (p[0] == 0 && n == 1) {   // the character select: each port's last choice
    append_be32(reply, offline ? gx::RenderOptions::live_cpu_ai().load() & 0xFFFFu : 0u);
  } else if (p[0] == 1 && n == 3 && p[1] < 4 && p[2] < 16) {   // a port's choice, kept in the settings file
    if (offline && !g_replaying) {
      const uint32_t shift = 4u * p[1];
      const uint32_t old = gx::RenderOptions::live_cpu_ai().load();
      const uint32_t now = (old & ~(0xFu << shift)) | ((uint32_t)p[2] << shift);
      if (now != old) {
        gx::RenderOptions::live_cpu_ai().store(now);
        gx::RenderOptions::live_cpu_ai_changes().fetch_add(1);
      }
    }
  } else if (p[0] == 2 && n == 5) {   // a match begins: the word it plays with (the replay's in playback)
    static const char* test = host::test_env::automated_getenv("MELEE_TEST_CPU_AI");   // test runs: this word instead
    uint32_t word = read_be32(p + 1) & 0xFFFFu;
    if (g_replaying) word = g_replay_cpu_ai;
    else if (!offline) word = 0;
    else if (test && *test) word = (uint32_t)std::strtoul(test, nullptr, 16) & 0xFFFFu;
    g_cpu_ai_match = word;
    if (word) host::log("cpu ai: match with choices %04X%s", word, g_replaying ? " (the replay's)" : "");
    append_be32(reply, word);
  }
  return true;
}

}  // namespace host_internal






int32_t console_practice_bridge(int32_t op, int32_t* args, int32_t count) {

  return g_game.practice ? g_game.practice(op, args, count) : -1;
}

// Training Mode CE is part of the game: the game's build writes its data files into TM/ beside the
// game library (upstream's dats/ at the pinned commit, tools/tmce/prepare_src.py --dats) and its code
// is compiled in. Here they join the disc as /tm/<name>, which turns TM-CE on (MU_GAME_OPTION_TMCE):
// Event Match then opens its event menu (shim/mu_tmce.c, shim/mu_tmce_menu.c). Not in a Vanilla run,
// in a replay's playback, or on an m-ex disc or rogueMelee (TM-CE needs the vanilla game). A mod
// profile's own TM/ files (a TM-CE disc) give way to these, which match the code built in.
void publish_training_files(const std::filesystem::path& game_folder) {
  namespace fs = std::filesystem;
  if (host::gameplay::vanilla_only() || host::options.vanilla_game || g_replay) return;
  if (mods::status().akaneia || mods::status().rogue || g_paths.count("/mxdt.dat")) {
    host::log("training mode: off (TM-CE needs the vanilla game; this disc is an m-ex build or rogueMelee)");
    return;
  }
  std::error_code ec;
  const fs::path folder = game_folder / "TM";
  std::vector<fs::path> files;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    if (ext == ".dat" && it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  uint32_t next = kTrainingFileBase, replaced = 0;
  for (const auto& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) continue;
    const uint64_t span = ((uint64_t)data.size() + 0xFFFFu) & ~0xFFFFull;
    if (next + span > kRosterBase) {
      host::log("training mode: %s does not fit its range; left out", file.u8string().c_str());
      continue;
    }
    std::string path = "/tm/" + file.filename().u8string();
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    const auto found = g_paths.find(path);
    int32_t entry;
    if (found != g_paths.end() && !g_fst[found->second].dir) {
      entry = found->second;
      g_view_alias.erase(entry);   // the same file in both views
      ++replaced;
    } else {
      entry = (int32_t)g_fst.size();
      g_fst.push_back({0, 0, false});
      g_paths[path] = entry;
    }
    g_fst[entry] = {next, (uint32_t)data.size(), false};
    host::log("training mode: %s %u bytes fnv1a %08X", path.c_str(), (uint32_t)data.size(), fnv1a(data.data(), data.size()));
    g_training_files.push_back({next, std::move(data)});
    next += (uint32_t)span;
  }
  if (g_training_files.empty() || !g_paths.count("/tm/eventmenu.dat")) {
    host::log("training mode: no eventMenu.dat in %s; Event Match as the game has it", folder.u8string().c_str());
    return;
  }
  mods::status().tmce = true;
  host::log("training mode: on, %zu files from %s%s", g_training_files.size(), folder.u8string().c_str(),
            replaced ? " (in place of the mod profile's)" : "");
}

}  // namespace source_port
