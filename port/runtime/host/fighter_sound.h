// An installed fighter's own sounds and its name call (host/roster.h, host/fighter_package.h).
//
// On an m-ex disc a fighter MexTool adds has its own sound bank: an .ssm file of samples (audio/us/) and
// that bank's scripts in smash2.sem, the one table of every bank's scripts the game loads at boot. Its
// row in MxDt names the bank, and the announcer's call of its name is one more script of bank 51
// (nr_name.ssm). A package from SSBM Nucleus carries the same things: sound.zip (MexManager: the .ssm,
// scripts.sem and group.json) or sound.spk (MexTool: the scripts with the .ssm inside), and the call as
// namecall.wav.
//
// take_package_sound() keeps them, checked, in the fighter's folder (sound\bank.ssm, scripts.sem,
// sound.json, namecall.wav). build() turns the installed set into what the game is served for a disc
// without m-ex, the same layout an m-ex disc has:
//   - each fighter's bank as a bank past the retail ones (56 and up), its samples renumbered past every
//     retail sample and its file served as audio/us/nf_<hash>.ssm (and audio/ for Japanese);
//   - smash2.sem with the retail banks as they are, an empty bank 55, and those banks' scripts after;
//   - the calls encoded (DSP ADPCM, the console's sample format) and added to nr_name.ssm, whose
//     samples move to a range of their own, with one more bank 51 script each (English only);
//   - for each fighter, what its record adds (shim/mu_mex_roster.c): its bank, file, size and row, and
//     its call.
// Every number the game indexes with is checked here first (sizes, offsets, counts, each script's
// commands and the samples they name); a fighter whose sounds fail keeps its base fighter's bank, with
// one log line. With no fighter sounds the disc's own files are served unchanged.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "roster.h"

namespace host::fighter_sound {

// The folder of a fighter's sounds, inside its folder.
inline constexpr char kFolder[] = "sound";

// Takes the package's sounds (sound.zip or sound.spk, and namecall.wav) from the unpacked package at
// `package` into `sound_dir`, each checked. Returns what was taken for the log ("" none); `notes` gets
// why something was left out.
std::string take_package_sound(const std::filesystem::path& package, const std::filesystem::path& sound_dir,
                               std::string* notes);

// A bank and its scripts as they are checked: the .ssm's sound count, the scripts' words.
struct Bank {
  std::vector<uint8_t> ssm;                    // the package's .ssm
  std::vector<std::vector<uint32_t>> scripts;  // each script's words, local sample numbers (0 on)
  uint8_t row[4] = {0, 0, 0, 0};               // group, load and unload priority, voice threshold
  uint32_t sounds = 0, sample_bytes = 0;
};
// The .ssm: sizes, the sound table, every sample address inside the sample data. Empty when good.
std::string check_ssm(const std::vector<uint8_t>& ssm, uint32_t* sounds, uint32_t* sample_bytes);
// Scripts of one bank: known commands, every sample < `sounds`, each loop inside its script with a wait
// in it, each script ending. Empty when good.
std::string check_scripts(const std::vector<std::vector<uint32_t>>& scripts, uint32_t sounds);
// A scripts.sem of one bank (MexManager's), its scripts. Empty when good.
std::string read_group_sem(const std::vector<uint8_t>& sem, std::vector<std::vector<uint32_t>>* scripts);
// The bank of a fighter folder's sound\, checked. Empty when good.
std::string load_bank(const std::filesystem::path& fighter_dir, Bank* out);

// A WAV file (PCM, 16-bit, one or two channels) as mono samples. Empty when good.
std::string read_wav(const std::vector<uint8_t>& wav, std::vector<int16_t>* pcm, uint32_t* rate);
// DSP ADPCM: 8 coefficient pairs, frames of 8 bytes for 14 samples.
struct Adpcm {
  int16_t coefs[16] = {};
  std::vector<uint8_t> data;
  uint32_t samples = 0;
};
Adpcm encode_adpcm(const std::vector<int16_t>& pcm);
std::vector<int16_t> decode_adpcm(const Adpcm& a);   // for the tests

// The disc's own files (empty: not on this disc, or another layer replaced it).
struct DiscSounds {
  std::vector<uint8_t> sem_us, sem_jp, names_us;   // audio/us/smash2.sem, audio/smash2.sem, audio/us/nr_name.ssm
};

struct Served {
  std::string path;                // "/audio/us/smash2.sem"
  std::vector<uint8_t> data;
};
struct Set {
  uint32_t serial = 0;             // 0: the disc's own sounds; otherwise changes with what is served
  std::vector<Served> files;
  std::vector<std::vector<uint8_t>> tails;   // per fighter of the list, appended to its record
  std::vector<std::string> log;
};

// What stays the same for the whole run: a bank's number and sample range, by its contents, so a
// number the game may still have loaded never means another fighter's sounds.
struct Session {
  std::map<uint32_t, std::pair<int, uint32_t>> banks;   // content hash -> bank id, first sample number
  int next_bank = 56;
  uint32_t next_sample = 2304;
};
inline constexpr int kMaxBank = 126;             // lbaudio_ax.c mu_ak_sound_view fills rows below LB_SSM_CAP - 1
inline constexpr uint32_t kNamesBase = 2048;     // nr_name.ssm's samples once calls are added
inline constexpr uint32_t kMaxNameBytes = 64 * 1024;   // room left in the game's resident sound memory

Set build(const std::vector<host::roster::Fighter>& fighters, const DiscSounds& disc, Session* session);

// The smash2.sem of `disc` with banks added (exposed for the tests): `added` by bank id (each id > 55),
// their scripts already numbered; `names` more bank 51 scripts, and bank 51's samples `names_from` on
// (`names_count` of them) renumbered from `names_to` (0: as they are). Empty with `why` when the disc's
// file is not one this can extend.
std::vector<uint8_t> extend_sem(const std::vector<uint8_t>& sem, const std::map<int, std::vector<std::vector<uint32_t>>>& added,
                                const std::vector<std::vector<uint32_t>>& names, uint32_t names_from, uint32_t names_count,
                                uint32_t names_to, std::string* why);

}  // namespace host::fighter_sound
