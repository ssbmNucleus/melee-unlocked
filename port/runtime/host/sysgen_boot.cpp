// SPDX-License-Identifier: GPL-2.0-or-later
#include "sysgen_boot.h"
#include "sysgen_disc.h"
#include "host.h"
#include "product_identity.h"

namespace host {
namespace { sysgen::Assets assets; }
bool required_sys_asset(const std::string& name) {
  return name == "GameSetup_gui.dat" || name == "slpCSS.dat" || name == "SlippiCSS_gui.dat";
}
std::string prepare_sys_assets(const std::string& user_dir, const std::string& data_dir) {
#ifdef MELEE_NO_SLIPPI
  (void)user_dir; (void)data_dir;
#else
  try {
    sysgen::Config config;
    config.sys_dir = std::filesystem::u8path(options.sys_dir).lexically_normal();
    if (config.sys_dir.filename().empty()) config.sys_dir = config.sys_dir.parent_path();
    config.build_dir = config.sys_dir.parent_path();
    if (config.build_dir.empty()) config.build_dir = ".";
    // A launcher that ships User/ in the game folder still starts:
    // derived folders inside it move to the player's own data folder. An explicit one is never moved.
    std::string note;
    config.data_dir = sysgen::choose_data_dir(config.build_dir, user_dir, data_dir, sysgen::user_data_dir(), note);
    if (!note.empty()) host::log("%s", note.c_str());
    sysgen::Disc disc;
    disc.verify_retail = [] { return disc_has_vanilla_dol(); };
    disc.read = [](const std::string& name, size_t expected, std::vector<uint8_t>& out) {
      uint32_t offset = 0, size = 0;
      if (!disc_find_file(name, &offset, &size) || size != expected) return false;
      out.resize(size);
      return disc_read(offset, out.data(), size);
    };
    DiscImageReader base(options.mod_base_iso);   // opens only when prepare needs disc bytes
    disc = sysgen::boot_disc(options.mod_base_iso, base, disc, kVanillaDolSize, vanilla_dol_image);
    std::string error;
    if (!assets.prepare(config, disc, error)) {
      // The disc's own file is not the game's (a modded MnSlChr.usd), or the disc is not retail 1.02:
      // nothing here can build the online menus from it, and that is the player's disc, not a crash.
      if (error.find("disc source size/SHA-1 mismatch") != std::string::npos ||
          error.find("disc is not verified retail") != std::string::npos) {
        host::log("Sys generation failed: %s", error.c_str());
        const auto colon = error.rfind(": ");
        const std::string file = error.find("mismatch: ") != std::string::npos && colon != std::string::npos ? error.substr(colon + 2) : "";
        return "This Melee disc is modded" + (file.empty() ? std::string() : " (its " + file + " is not the original)") +
               ", so " PRODUCT_NAME " cannot build its online menus from it.\n\nStart the game with an unmodified "
               "Melee NTSC 1.02 ISO. A modded disc can go into Mods instead, where it plays on top of the original.";
      }
      // The game's own files are not all there (no Sys or deltas folder beside the game: a download
      // only partly extracted, or files an antivirus removed; 1.0.1 report). Said plainly, no crash report.
      if (error.find("missing delta manifest entry") != std::string::npos || error.find("cannot read delta") != std::string::npos) {
        host::log("Sys generation failed: %s", error.c_str());
        return "Some of " PRODUCT_NAME "'s files are missing from this folder (its Sys or deltas folder), so it cannot start."
               "\n\nExtract the whole download again into a normal folder, all of its files, then start "
               "the game from there. If your antivirus removed files, allow the " PRODUCT_NAME " folder first.";
      }
      die("Sys generation failed: %s", error.c_str());
    }
    else if (!assets.status_message().empty()) host::log("%s%s", assets.status_message().c_str(), host::startup_mark().c_str());
  } catch (const std::exception&) {
    die("Sys generation failed: cannot prepare boot paths or read disc assets");
  }
#endif
  return {};
}
std::string sys_game_file_path(const std::string& name) {
  // Ordinary Slippi file/diff lookup stays unchanged; only the two managed assets redirect.
  if (!required_sys_asset(name)) return options.sys_dir + "/GameFiles/GALE01/" + name;
  return assets.lookup(std::filesystem::u8path(options.sys_dir), name).u8string();
}
std::vector<std::string> generated_sys_names() {
  return assets.generated_names(std::filesystem::u8path(options.sys_dir));
}
}  // namespace host
