// New stages: a stage file from SSBM Nucleus (a post of type custom_stage) installed from the Mods menu's
// New Stages (host/mods_menu.cpp) and played as a stage of its own: an icon on the stage select's next
// pages (shim/mu_new_stages.c, mn/mnstagesel.c), the stage it was made over (its base) underneath.
//
// What a new stage is, and how it plays:
//   - one data file made to replace a retail stage file (its name says which: "Smashville_GrTCa.dat"
//     replaces GrTCa.dat), with no code of its own. Link-only, zip, xdelta and m-ex files (a stage id
//     the retail game does not have, "GrHw.dat") are not new stages here.
//   - its base is a retail versus stage, or one of the Target Test stages tested to host a four-player
//     versus match (kBases in new_stages.cpp): the stage's code is the base's, the file is the new one.
//   - picking it on the stage select starts the base stage; at that match's boundary the host serves the
//     new file in place of the base's (port/app/source_host_new_stages.cpp, MU_CMD_STAGE_SKIN_MATCH). The next
//     match without the pick loads the base's own file (or its stage skin) again.
//   - offline only: never online, in a peer-to-peer session, a replay or a Vanilla run. The random
//     stage pick leaves new stages out unless random_included().
// Files live in <settings folder>/NewStages: installed.json and one <id>.dat (and <id>.png, the post's
// picture its icon is made from) per stage.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace host::new_stages {

// The largest file taken. The largest tested to play with four fighters is 2.8 MB (Inverted Pyramid
// over GrTDr); the game's stage heap holds the stage and the fighters' files together.
constexpr uint64_t kMaxBytes = 3584u * 1024u;
// Two stage select pages of 22 icons.
constexpr size_t kMaxInstalled = 44;
// The icon: the post's picture, cut to the stage select icon's shape (8:7), as a console RGBA8 texture.
constexpr int kIconWidth = 64, kIconHeight = 56;

struct Installed {
  std::string id;            // "s<file id>"
  std::string base;          // the retail file it plays over ("GrTCa.dat")
  std::string name, creator, author, description, file_name;
  int mod_id = 0, file_id = 0, downloads = 0;
  std::string picture;       // its PNG (the post's first screenshot), absolute; empty when none
  uint64_t size = 0;
  std::string sha256;
};

// The retail stage file a site file replaces, from its name ("Castle DeDeDe(GrTYs).dat" -> GrTYs.dat):
// a word of the name that is a base's name, or (a retail versus stage only) a word that begins with
// one when the post says it is that stage ("GrNLaPs5.dat" on a Final Destination post). Empty when the
// name names no base, or two.
std::string base_of(const std::string& file_name, const std::string& post_stage);
// What the base is called ("Battlefield", "C. Falcon's Target Test"): drawable text, at most 23 letters.
std::string base_name(const std::string& base);
bool target_test_base(const std::string& base);
// Why a file is not offered as a new stage, one plain line (empty when it is).
std::string unsupported_reason(const std::string& file_name, const std::string& post_stage);

// The checks every new stage passes before it is installed and again before it is served:
//   - a stage archive the game reads (map_head, coll_data, grGroundParam; every pointer inside it)
//     and whose collision, particle banks and model joint trees hold together (mod_data_safety.h);
//   - not the base's own file;
//   - no fewer collision groups than the base (the base's code walks them by number); for a retail
//     versus stage no fewer collision lines either (its code finds lines by number, as for a skin);
//   - the points a versus match needs: four starting points (0 to 3), the camera's limits (148, 149)
//     and the blast zones (150, 151). A Target Test file made for one player has one start: refused.
// `bytes` as prepare() left it. False with one plain line in `why`.
bool check(const std::string& base, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
           std::string* why);
// A file DAT Texture Wizard grew past its data block made into one the game reads (menu_mods::normalize).
bool prepare(std::vector<uint8_t>* bytes);

void configure(const std::string& settings_path);
// Installed stages, by name.
std::vector<Installed> list();
bool find(const std::string& id, Installed* out);
// Installs `bytes` (as downloaded) over `info.base`, checked against `retail` (the disc's base file).
bool install(const Installed& info, const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& retail,
             const std::string& picture_png, std::string* error);
bool remove(const std::string& id, std::string* error);
// The bytes to serve for `id`: its file, checked again. False (the base plays as it is) when it is not
// installed or fails a check, said once in the log.
bool served(const std::string& id, const std::vector<uint8_t>& retail, std::vector<uint8_t>* bytes, Installed* info);
// Its icon: kIconWidth x kIconHeight console RGBA8 (a plain tile when it has no picture).
std::vector<uint8_t> icon(const std::string& id);
// Whether the random stage pick may draw new stages (off by default; kept in installed.json).
bool random_included();
bool set_random_included(bool on, std::string* error = nullptr);

}  // namespace host::new_stages
