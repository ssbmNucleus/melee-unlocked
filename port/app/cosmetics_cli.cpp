// The offline cosmetic command line: MELEE_BUILD_LOOKS and --import-cosmetic, --scan-disc-skins,
// --select-cosmetic, --deselect-cosmetic, --enable-project-effects, --restore-vanilla-cosmetics,
// --cosmetic-status. The bodies are the blocks that stood in melee_main, unchanged; the parameters
// keep the names they had there. Which option was given is still decided in main.cpp. And the effect
// checker, --check-effect and --check-effects (host::cosmetics::check_effect).
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "cosmetics_cli.h"
#include <windows.h>
#include "host.h"
#include "cosmetic_mods.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace cosmetics_cli {
// MELEE_BUILD_LOOKS=<patch folder> builds the exact looks' packs from --iso and exits (0 ready, 1 not).
int build_looks(const char* looks, const host::Options& o) {
    std::string message;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string tool = (std::filesystem::path(exe).parent_path() / L"tools" / L"xdelta3.exe").u8string();
    bool ok = host::cosmetics::build_look_packs(looks, o.iso, tool, &message);
    if (ok && host::disc_open(o.iso)) host::cosmetics::scan_look_packs();   // the test checks the packs too
    std::printf("looks: %s\n", message.c_str());
    return ok ? 0 : 1;
}

// The offline cosmetic options: 0 when every requested step worked, 1 when one did not.
int run(const std::string& cosmetic_import, const std::string& cosmetic_scan_disc, const std::string& cosmetic_select,
        const std::string& cosmetic_deselect, bool cosmetic_deselect_requested, bool cosmetic_enable_effects,
        bool cosmetic_restore, bool cosmetic_status, const host::Options& o) {
    bool ok = true;
    if (!cosmetic_import.empty()) {
      auto result = host::cosmetics::import_file(cosmetic_import);
      std::printf("%s\n", result.message.c_str()); ok &= result.ok;
    }
    if (!cosmetic_scan_disc.empty()) {
      // A costume is a skin only where it differs from the game disc's, so that disc must be open.
      if (o.iso.empty()) {
        std::fprintf(stderr, "--scan-disc-skins requires --iso (your Melee disc) to compare the mod disc against\n");
        ok = false;
      } else if (!host::disc_open(o.iso)) {
        std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str());
        ok = false;
      } else {
        const std::string name = std::filesystem::u8path(cosmetic_scan_disc).stem().u8string() + " disc";
        auto result = host::cosmetics::scan_disc_skins(cosmetic_scan_disc, name);
        std::printf("%s\n", result.message.c_str()); ok &= result.ok;
      }
    }
    if (!cosmetic_select.empty()) {
      std::string error = "no skin has that id";
      bool selected = false;
      for (const auto& asset : host::cosmetics::assets())
        if (asset.id == cosmetic_select) {
          const std::string slot = host::cosmetics::cli::slot_key(asset);
          selected = host::cosmetics::select_variant(slot, asset.id, &error);
          break;
        }
      if (selected) std::printf("%s\n", host::cosmetics::last_message().c_str());
      else { std::fprintf(stderr, "%s\n", error.c_str()); ok = false; }
    }
    if (cosmetic_deselect_requested) {
      std::string error;
      if (!host::cosmetics::cli::known_slot(host::cosmetics::assets(), cosmetic_deselect)) {
        std::fprintf(stderr, "no cosmetic slot matches %s\n", cosmetic_deselect.c_str()); ok = false;
      } else if (!host::cosmetics::disable_target(cosmetic_deselect, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str()); ok = false;
      } else std::printf("%s\n", host::cosmetics::last_message().c_str());
    }
    if (cosmetic_enable_effects) {
      std::string error;
      if (o.iso.empty()) {
        std::fprintf(stderr, "--enable-project-effects requires --iso so every target can be validated against the clean disc\n");
        ok = false;
      } else if (!host::disc_open(o.iso)) {
        std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str());
        ok = false;
      } else if (!host::cosmetics::enable_project_effects(&error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        ok = false;
      } else {
        std::printf("%s\n", host::cosmetics::last_message().c_str());
      }
    }
    if (cosmetic_restore) {
      std::string error;
      if (!host::cosmetics::restore_vanilla(&error)) { std::fprintf(stderr, "%s\n", error.c_str()); ok = false; }
      else std::printf("%s\n", host::cosmetics::last_message().c_str());
    }
    if (cosmetic_status) {
      const bool compare_online = !o.iso.empty() && host::disc_open(o.iso);
      if (!o.iso.empty() && !compare_online) {
        std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str()); ok = false;
      }
      std::printf("profile %s\n", host::cosmetics::profile_enabled() ? "enabled" : "disabled");
      for (const auto& asset : host::cosmetics::assets()) {
        const std::string online = host::cosmetics::online_status_column(asset.id, compare_online);
        std::printf("%c %s | %s | %s | %s%s%s%s\n", asset.selected ? '*' : '-', asset.target_path.c_str(),
                    asset.name.c_str(), asset.id.c_str(), asset.sha256.c_str(),
                    asset.voice.empty() ? "" : " | voice ", asset.voice.c_str(), online.c_str());
      }
    }
    return ok ? 0 : 1;
}

namespace {
bool open_disc_for_checks(const host::Options& o) {
  if (o.iso.empty()) { std::fprintf(stderr, "the effect checks need --iso (your Melee disc) to compare against\n"); return false; }
  if (!host::disc_open(o.iso)) { std::fprintf(stderr, "cannot open ISO %s\n", o.iso.c_str()); return false; }
  return true;
}
std::string csv_field(std::string s) {
  for (char& c : s) if (c == '"' || c == '\n' || c == '\r') c = c == '"' ? '\'' : ' ';
  return "\"" + s + "\"";
}
}  // namespace

int check_effect(const std::string& path, const host::Options& o) {
  if (!open_disc_for_checks(o)) return 2;
  const auto verdict = host::cosmetics::check_effect(path);
  std::printf("%s\n", host::cosmetics::effect_verdict_json(verdict).c_str());
  std::fflush(stdout);
  return verdict.verdict.empty() ? 2 : host::cosmetics::effect_verdict_refuses(verdict.verdict) ? 1 : 0;
}

int check_effects(const std::string& folder, const std::string& csv, const host::Options& o) {
  if (!open_disc_for_checks(o)) return 2;
  namespace fs = std::filesystem;
  const fs::path root = fs::u8path(folder);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) { std::fprintf(stderr, "%s is not a folder\n", folder.c_str()); return 2; }
  std::vector<fs::path> mods;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string extension = it->path().extension().u8string();
    for (char& c : extension) c = (char)std::tolower((unsigned char)c);
    if (extension == ".dat" || extension == ".usd" || extension == ".zip") mods.push_back(it->path());
  }
  std::sort(mods.begin(), mods.end());
  FILE* out = nullptr;
  if (!csv.empty() && !(out = _wfopen(fs::u8path(csv).c_str(), L"wb"))) {
    std::fprintf(stderr, "cannot write %s\n", csv.c_str()); return 2;
  }
  if (out) std::fprintf(out, "file,verdict,reason,really,target,character,slots,sha256\n");
  bool all = true;
  for (const auto& path : mods) {
    auto verdict = host::cosmetics::check_effect(path.u8string());
    const std::string name = path.lexically_relative(root).generic_u8string();
    verdict.file = name;
    std::printf("%s\n", host::cosmetics::effect_verdict_json(verdict).c_str());
    all &= !verdict.verdict.empty();
    if (out) {
      std::string slots;
      for (const auto& s : verdict.slots) slots += (slots.empty() ? "" : "; ") + s;
      std::fprintf(out, "%s,%s,%s,%s,%s,%s,%s,%s\n", csv_field(name).c_str(),
                   verdict.verdict.empty() ? "unchecked" : verdict.verdict.c_str(), csv_field(verdict.reason).c_str(),
                   csv_field(verdict.really).c_str(), verdict.target.c_str(), csv_field(verdict.character).c_str(),
                   csv_field(slots).c_str(), verdict.sha256.c_str());
    }
  }
  std::fflush(stdout);
  if (out) std::fclose(out);
  return all ? 0 : 2;
}
}  // namespace cosmetics_cli
