// The 20XX Hack Pack on the Source Port: what the host does for the pack's disc when it is loaded as
// a file overlay (--mod-iso or a profile).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Source Port never runs the pack's code. What is left of its features are native rewrites in
// the game (sourceport/game/shim/mu_hp.c, mu_hp_css.c; plan and ledgers in run-source/rel09-hackpack).
// The host's part: which of the pack's files are served. The pack's own menu files stay out (retail
// menus), and the files that carry PowerPC code are never published at all (keeps_retail); and the
// game is told the pack is loaded (MU_MOD_HACKPACK).
//
// The pack's data tables for the game (stage swap table, music names, playlists, default stage file
// names), the player's choices for them (key hp_settings) and the pack's debug menu were served
// through command 0xFA, which the Slippi build refused; they were removed in 2026-11. An hp_settings
// line in an older settings file is now an unknown key and is skipped.
//
// Header only, used by port/app/source_host*.cpp.
#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

namespace source_port::hackpack {

// Disc paths (lower case, as the overlay stores them) that are not served from the pack.
//   Code carriers, never published: the pack's character select files hold PowerPC blocks the pack's
//   main.dol jumps into at fixed addresses, and AI_Engine.bin is PowerPC. The native game keeps
//   loading the retail MnSlChr.usd, which the pack does not have, so the retail copy is served.
//   Retail menus: the pack's main menu, extra menu, results text and event table describe features
//   only its code has (its debug menu entry, its results counters, its custom event matches).
// The pack renamed or deleted MnSlChr.usd, MnSlMap.usd, SdMenu.usd, SdSlChr.usd, IfAll.usd and
// GmTitle.usd: the native game asks for those names and gets the retail disc's copies without any
// rule here. Its MnSlMap.1sd..4sd, SdMenu.1sd..4sd, SdSlChr.1sd..4sd, IfAl0/IfAl1.usd and
// IfCom*.dat are data under names the native game never asks for.
inline bool keeps_retail(const std::string& path) {
  static const char* const kept[] = {
      "/mnslchr.0sd", "/mnslchr.1sd", "/mnslchr.2sd", "/mnslchr.3sd", "/ai_engine.bin",
      "/mnmaall.usd", "/mnextall.usd", "/sdrst.usd", "/gmevent.dat", "/opening.bnr",
  };
  for (const char* name : kept) if (path == name) return true;
  return false;
}

// The pack is recognized by its own files among a layer's: the stage swap table and the code
// carrying character select file. (The Mods page recognizes the official builds by whole-disc MD5;
// this also covers a build the player changed with the pack's own tools.)
inline bool is_pack_marker(const std::string& path) { return path == "/stageswaptable.bin"; }
inline bool is_pack_second_marker(const std::string& path) { return path == "/mnslchr.0sd"; }

struct State {
  std::mutex mutex;
  std::atomic<bool> loaded{false};
  std::filesystem::path iso;
};
inline State& state() { static State s; return s; }

// The loaded layer is the pack: remember its image.
inline void attach(const std::filesystem::path& iso) {
  State& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.iso = iso;
  s.loaded.store(!iso.empty());
}
inline bool loaded() { return state().loaded.load(); }

}  // namespace source_port::hackpack
