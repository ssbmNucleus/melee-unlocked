// Fighters installed beside the game, for a disc that has no m-ex table of its own.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// A fighter in the m-ex fighter format is a folder: fighter.json (what MexTool exports and the SSBM
// Nucleus Vault keeps) and the files it names. On an m-ex disc such a fighter is a row MexTool wrote
// into MxDt.dat. For a disc without that file the host serves the installed fighters itself: their
// files join the disc, and the game is handed one record a fighter, from which it builds the table
// in memory (sourceport/game/shim/mu_mex_roster.c).
namespace host::roster {

struct File {
  std::string name;              // as the game opens it: "PlPc_001.dat"
  std::filesystem::path path;    // where it is on this computer
  uint64_t size = 0;
};

struct Fighter {
  std::string name;
  std::string folder;
  std::filesystem::path path;    // the fighter's folder on this computer
  std::vector<uint8_t> record;   // the game's record: the order mu_mex_roster.c reads it in
  std::vector<File> files;
};

// Reads every <folder>\<name>\fighter.json again. Returns true when the set changed since the last
// scan (a fighter added, removed or edited).
bool scan(const std::filesystem::path& folder);

const std::vector<Fighter>& fighters();

// Changes whenever the scanned set does; 0 while there is none.
uint32_t generation();

// A fighter's picture for the character select, as a console RGBA8 texture of a fixed size:
// what 0 is its icon (icon.png, 64x56), what 1 the portrait of costume `index` (csp_<index>.png,
// 136x188), what 2 that costume's stock icon (stc_<index>.png, 24x24). A fighter without an icon shows
// the top of its first portrait there; one with neither picture gets a plain placeholder, so it can
// still be picked; one without a stock icon gets an
// empty picture, and the game keeps a retail stock icon.
// nullptr when there is no such fighter. The pictures of a scanned set are kept until it changes.
struct Image {
  uint16_t width = 0, height = 0;
  std::vector<uint8_t> data;
};
const Image* image(size_t fighter, int what, int index);
// Any PNG file (UTF-8 path) made into a console RGBA8 texture of exactly `width` x `height` (multiples
// of 4) by the same reader and resampling as the pictures above: at most 16 MB and 4096 pixels a side,
// the size read from the header before any pixel is decoded. Empty when it cannot be read.
std::vector<uint8_t> console_picture(const std::string& png_utf8, int width, int height);

// Installs an unpacked m-ex fighter package (what MexTool exports and the SSBM Nucleus Vault keeps as
// fighter.zip) as <fighters>\<name>, the folder scan() reads. The package is fighter.json, the files
// it names, icon.png, and one zip a costume holding the costume's file with its portrait (csp.png)
// and stock icon (stc.png); those become csp_<costume>.png and stc_<costume>.png here. `unzip`
// unpacks one of the costume zips into a folder. Only files a disc carries and pictures are taken,
// by their own names: nothing in a package can land outside the fighter's folder. The folder
// replaces an installed fighter of the same name whole, and only once the new one is complete.
struct Installed {
  std::string name, folder;
  int files = 0, portraits = 0;
  bool has_code = false;          // the package has the fighter's C (a code folder)
  std::filesystem::path base;     // where fighter.json was found in the package
};
bool install_package(const std::filesystem::path& unpacked, const std::filesystem::path& fighters_folder,
                     const std::function<bool(const std::filesystem::path& zip, const std::filesystem::path& dest)>& unzip,
                     Installed* out, std::string* problem);

}   // namespace host::roster
