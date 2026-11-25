// A custom fighter from SSBM Nucleus (a MexTool fighter.yml package or a MexManager fighter.json one),
// installed for this port from the Mods menu (Browse -> Fighters, host/mods_menu_fighters.cpp).
//
// This port runs the game natively, so a fighter's own compiled PowerPC code can never run here. A
// fighter made only of data (its model, animations, stats, effects and sounds) on a retail fighter's
// moves plays through the m-ex data support as it is (akaneia/mu_ak_fighters.c clone_fighter: every
// function the game's own, no ftFunction in its file). One with code of its own plays only with a
// native plugin built from its author's C (sourceport/game/mexft, a .mufighter): such a package is
// never offered, and install() refuses it again from its files.
//
// install() unpacks a downloaded package, finds its fighter (at the top, in a folder, or in a zip
// inside it), turns a fighter.yml into the fighter.json the roster reads, checks the files above, fills
// what a classic package leaves to the disc (its move table and sound bank: the fighter it is made
// from's), and installs it through host::roster::install_package into the fighters folder, with a
// nucleus.json naming the post it came from. The game picks it up at its next change of mode.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace host::fighter_package {

// The words for a fighter that needs a plugin (the Mods menu, the install log).
inline constexpr char kNeedsPlugin[] = "Needs a plugin that is not available yet.";

// What decides whether a fighter plays here without a plugin. The site's check lists these for each
// post (GET /fighters, its "fighter" object); install() reads them again from the files.
struct Facts {
  bool own_code_file = false;     // its fighter file exports ftFunction or itFunction (code m-ex loads)
  int functions_outside = 0;      // functions it gives the game that are not the game's own
  int moves_outside = 0;          // move table callbacks that are not the game's own
  bool own_move_table = false;    // a move table other than the one of the fighter it is made from
  int items = 0;                  // articles of its own
  bool code_folder = false;       // its C source or a plugin comes with it
  bool donor_unknown = false;     // made from no fighter this game has
};
// Why the fighter cannot play here as it is (for the log), "" when it can.
std::string needs_plugin(const Facts& facts);

struct Result {
  bool ok = false;
  bool needs_plugin = false;      // refused: its files hold code of their own (message: kNeedsPlugin)
  std::string message;            // the player's words when it failed
  std::string folder, name;       // the fighter's folder in the fighters folder, and its name
  std::string donor;              // the retail fighter it is made from ("Fox")
};
// `zip`: the downloaded package. `root_hint`: where the site's check found the fighter in it ("" the top,
// "Folder/", "Inner.zip|" a zip inside it), tried first. `zip_sha256` is kept in nucleus.json.
// Worker threads only.
Result install(const std::filesystem::path& zip, const std::filesystem::path& fighters_folder, const std::string& root_hint,
               int mod_id, const std::string& title, const std::string& zip_sha256);

// nucleus.json of an installed fighter's folder: the post it came from. False for a fighter put there
// another way (Wolf and Shulk come with the game), which the Mods menu does not remove.
struct Origin {
  int mod_id = 0;
  std::string title, donor, sha256;
};
bool read_origin(const std::filesystem::path& fighter_folder, Origin* out);
// Removes a fighter the Mods menu installed (its folder holds nucleus.json). False with the reason.
bool remove(const std::filesystem::path& fighters_folder, const std::string& folder, std::string* error);

// Exposed for tests.
bool yml_to_json(const std::string& yml, std::string* json_text, std::string* why);
int donor_of(uint32_t on_load);            // the retail internal kind whose onLoad it is, -1 none
const char* donor_name(int kind);
// The facts fighter.json alone gives (functions, move table, articles) and the fighter it is made from.
Facts manifest_facts(const std::string& json_text, int* donor);

}  // namespace host::fighter_package
