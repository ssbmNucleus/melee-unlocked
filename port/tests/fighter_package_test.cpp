// host/fighter_package: a classic fighter.yml read as the roster's fighter.json, the retail fighters a
// package can be made from, and what keeps a fighter with code of its own from being offered.
// With arguments (<fighters folder> <package.zip>[@<root>] ...) it installs each package there and says
// how it went: the coverage survey's "installs" column (docs/guide/mods-and-files.md).

#include "fighter_package.h"
#include "fighter_sound.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace fp = host::fighter_package;
using json = nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

// ---- host/fighter_sound: what a package's bank, scripts and name call must be ----

namespace fsnd = host::fighter_sound;

static void put32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
}
static uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
  return at + 4 <= b.size() ? (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3] : 0xFFFFFFFFu;
}

// An .ssm of `count` one-channel sounds of 14 samples each (one 8-byte frame).
static std::vector<uint8_t> tiny_ssm(uint32_t count, uint32_t base) {
  const uint32_t table = count * (8 + 0x40), samples = count * 8;
  std::vector<uint8_t> b;
  put32(b, table); put32(b, (samples + 31) & ~31u); put32(b, count); put32(b, base);
  for (uint32_t i = 0; i < count; ++i) {
    put32(b, 1); put32(b, 32000);
    std::vector<uint8_t> v(0x40, 0);
    const uint32_t start = i * 16 + 2, last = start + 13;
    for (int k = 0; k < 4; ++k) { v[4 + k] = (uint8_t)(start >> (24 - 8 * k)); v[8 + k] = (uint8_t)(last >> (24 - 8 * k)); v[12 + k] = (uint8_t)(start >> (24 - 8 * k)); }
    b.insert(b.end(), v.begin(), v.end());
  }
  b.resize((b.size() + 31) & ~(size_t)31);
  b.resize(b.size() + ((samples + 31) & ~31u), 0x11);
  return b;
}

// A smash2.sem of 56 banks: bank 0 one script, bank 51 two calls of samples 100 and 101.
static std::vector<uint8_t> tiny_sem() {
  std::vector<std::vector<uint32_t>> bank0 = {{0x01000005u, 0x0E000000u}};
  std::vector<std::vector<uint32_t>> bank51 = {{0x01000064u, 0x10000030u, 0x0E000000u}, {0x01000065u, 0x0E000000u}};
  std::vector<uint8_t> b;
  put32(b, 0); put32(b, 0); put32(b, 56);
  for (uint32_t i = 0; i < 56; ++i) put32(b, i == 0 ? 0 : i <= 51 ? 1 : 3);
  put32(b, 3);
  const size_t offsets = b.size();
  b.resize(b.size() + 12);
  put32(b, 0);
  const auto add = [&](const std::vector<uint32_t>& s, int n) {
    const uint32_t at = (uint32_t)b.size();
    for (int k = 0; k < 4; ++k) b[offsets + 4 * n + k] = (uint8_t)(at >> (24 - 8 * k));
    for (uint32_t w : s) put32(b, w);
  };
  add(bank0[0], 0); add(bank51[0], 1); add(bank51[1], 2);
  return b;
}

static void sound_checks() {
  using Scripts = std::vector<std::vector<uint32_t>>;
  // Scripts: known commands, samples inside the bank, loops inside their script with a wait, an end.
  CHECK(fsnd::check_scripts(Scripts{{0x01000000u, 0x0E000000u}}, 1).empty());
  CHECK(fsnd::check_scripts(Scripts{{0x01000000u, 0x00000010u, 0x03000002u, 0x0E000000u}}, 1).empty());
  CHECK(!fsnd::check_scripts(Scripts{{0x01000001u, 0x0E000000u}}, 1).empty());                 // sample 1 of a bank of 1
  CHECK(!fsnd::check_scripts(Scripts{{0x01000000u, 0x03000005u, 0x0E000000u}}, 1).empty());    // loops before its start
  CHECK(!fsnd::check_scripts(Scripts{{0x01000000u, 0x10000030u, 0x03000002u, 0x0E000000u}}, 1).empty());   // no wait
  CHECK(!fsnd::check_scripts(Scripts{{0x01000000u}}, 1).empty());                               // no end
  CHECK(!fsnd::check_scripts(Scripts{{0x0E000000u, 0x01000000u, 0x0E000000u}}, 1).empty());    // goes on after its end
  CHECK(!fsnd::check_scripts(Scripts{{0x40000000u, 0x0E000000u}}, 1).empty());                 // unknown command
  CHECK(!fsnd::check_scripts(Scripts{}, 1).empty());

  // Banks: whole, every sample inside the sample data.
  uint32_t sounds = 0, bytes = 0;
  std::vector<uint8_t> ssm = tiny_ssm(3, 0);
  CHECK(fsnd::check_ssm(ssm, &sounds, &bytes).empty() && sounds == 3 && bytes == 32);
  std::vector<uint8_t> cut(ssm.begin(), ssm.end() - 16);
  CHECK(!fsnd::check_ssm(cut, &sounds, &bytes).empty());
  std::vector<uint8_t> far = ssm;
  far[0x10 + 8 + 8] = 0x10;   // the first sound's end nibble far past the samples
  CHECK(!fsnd::check_ssm(far, &sounds, &bytes).empty());
  std::vector<uint8_t> none = ssm;
  none[11] = 0;               // no sounds
  CHECK(!fsnd::check_ssm(none, &sounds, &bytes).empty());
  std::vector<uint8_t> stereo = ssm;
  stereo[0x13] = 3;           // three channels
  CHECK(!fsnd::check_ssm(stereo, &sounds, &bytes).empty());

  // A scripts.sem of one bank (MexManager's).
  std::vector<uint8_t> group;
  put32(group, 0); put32(group, 0); put32(group, 1); put32(group, 0); put32(group, 2);
  put32(group, 32); put32(group, 40); put32(group, 0);
  put32(group, 0x01000000u); put32(group, 0x0E000000u); put32(group, 0x01000001u); put32(group, 0x0E000000u);
  Scripts read;
  CHECK(fsnd::read_group_sem(group, &read).empty() && read.size() == 2 && read[1][0] == 0x01000001u);
  std::vector<uint8_t> short_group(group.begin(), group.end() - 4);
  CHECK(!fsnd::read_group_sem(short_group, &read).empty());   // the last script never ends

  // The name call: a WAV read as mono, and DSP ADPCM close to it.
  std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                              0xE0, 0x2E, 0, 0, 0xC0, 0x5D, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a'};
  const uint32_t frames = 1200;
  for (int k = 0; k < 4; ++k) wav.push_back((uint8_t)((frames * 2) >> (8 * k)));
  std::vector<int16_t> sine(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    sine[i] = (int16_t)(8000.0 * std::sin(i * 2.0 * 3.14159265358979 * 440.0 / 12000.0) * (i < 600 ? 1.0 : 0.5));
    wav.push_back((uint8_t)sine[i]);
    wav.push_back((uint8_t)((uint16_t)sine[i] >> 8));
  }
  std::vector<int16_t> pcm;
  uint32_t rate = 0;
  CHECK(fsnd::read_wav(wav, &pcm, &rate).empty() && rate == 12000 && pcm.size() == frames && pcm[100] == sine[100]);
  const fsnd::Adpcm a = fsnd::encode_adpcm(pcm);
  CHECK(a.samples == frames && a.data.size() == (frames + 13) / 14 * 8);
  const std::vector<int16_t> back = fsnd::decode_adpcm(a);
  double err = 0;
  for (uint32_t i = 0; i < frames && i < back.size(); ++i) err += (double)(back[i] - pcm[i]) * (back[i] - pcm[i]);
  CHECK(back.size() == frames && std::sqrt(err / frames) < 200.0);   // under 2.5% of the wave
  std::vector<uint8_t> not_wav = wav;
  not_wav[20] = 3;   // floating point samples
  CHECK(!fsnd::read_wav(not_wav, &pcm, &rate).empty());

  // smash2.sem: a bank added past the retail ones, bank 51's samples moved and one more call.
  std::string why;
  std::map<int, Scripts> added = {{56, {{0x01000900u, 0x0E000000u}}}};
  const std::vector<uint8_t> sem = fsnd::extend_sem(tiny_sem(), added, Scripts{{0x01000802u, 0x0E000000u}}, 100, 2, 2048, &why);
  CHECK(why.empty() && get32(sem, 8) == 57);
  const size_t starts = 12, total_at = starts + 4 * 57, offsets = total_at + 4;
  CHECK(get32(sem, starts + 4 * 51) == 1 && get32(sem, starts + 4 * 52) == 4 && get32(sem, starts + 4 * 56) == 4 &&
        get32(sem, total_at) == 5);
  CHECK(get32(sem, get32(sem, offsets + 4 * 1)) == 0x01000800u && get32(sem, get32(sem, offsets + 4 * 2)) == 0x01000801u);
  CHECK(get32(sem, get32(sem, offsets + 4 * 3)) == 0x01000802u && get32(sem, get32(sem, offsets + 4 * 4)) == 0x01000900u);
  CHECK(get32(sem, get32(sem, offsets + 4 * 0)) == 0x01000005u);   // the other banks as they were
  std::vector<uint8_t> pointers = tiny_sem();
  pointers[7] = 1;   // a pointer list this does not move
  CHECK(fsnd::extend_sem(pointers, added, {}, 0, 0, 0, &why).empty() && !why.empty());
}

// The shape MexTool writes (a Luigi clone, two of its move rows, one costume).
static const char kYml[] =
    "functions:\n"
    "  version: 1\n"
    "  onLoad: 2148803364\n"
    "  onRespawn: 2148802852\n"
    "  moveLogic:\n"
    "  - animationID: 295\n"
    "    stateFlags: 3408145\n"
    "    attackID: 18\n"
    "    bitFlags: 0\n"
    "    animationCallBack: 2148426404\n"
    "    iASACallBack: 2148428468\n"
    "    physicsCallback: 2148428636\n"
    "    collisionCallback: 2148428828\n"
    "    cameraCallback: 2147967432\n"
    "  - animationID: 296\n"
    "    stateFlags: 3932433\n"
    "    attackID: 18\n"
    "    bitFlags: 0\n"
    "    animationCallBack: 2148426600\n"
    "    iASACallBack: 2148428508\n"
    "    physicsCallback: 2148428668\n"
    "    collisionCallback: 2148428860\n"
    "    cameraCallback: 2147967432\n"
    "  moveLogicPointer: 0\n"
    "  onItemRelease: 2148000000\n"
    "unkTable: \n"
    "boneTable:\n"
    "  boneCount: 73\n"
    "  top: 0\n"
    "  waist: 255\n"
    "items: []\n"
    "costumes:\n"
    "- fileName: PlMlNr.dat\n"
    "  modelSymbol: PlyLuigi5K_Share_joint\n"
    "  materialSymbol: PlyLuigi5K_Share_matanim_joint\n"
    "  visibilityIndex: 0\n"
    "kirbyCostumes: []\n"
    "nameText: Mr L\r\n"
    "fighterDataPath: PlMl.dat\n"
    "fighterDataSymbol: ftDataLuigi\n"
    "insigniaID: 0\n"
    "animFile: PlLgAJ.dat\n"
    "animCount: 290\n"
    "victoryTheme: &o0\n"
    "  fileName: ff_poke.hps\n"
    "  label: Pokemon Fanfare\n"
    "fighterSongID2: *o0\n"
    "resultScreenScale: 1\n"
    "endingScreenScale: 0.75\n"
    "canWallJump: false\n"
    "subCharacterBehavior: SpawnSimultaneously\n"
    "redCostumeIndex: 0\n";

int main(int argc, char** argv) {
  if (argc >= 3) {
    for (int i = 2; i < argc; ++i) {
      std::string arg = argv[i], root;
      if (const size_t at = arg.find('@'); at != std::string::npos) { root = arg.substr(at + 1); arg.resize(at); }
      const auto r = fp::install(std::filesystem::u8path(arg), std::filesystem::u8path(argv[1]), root, 900000 + i, arg, "");
      std::printf("%s: %s%s%s%s%s\n", arg.c_str(), r.ok ? "installed as " : "not installed: ", r.ok ? r.folder.c_str() : r.message.c_str(),
                  r.ok ? " (made from " : "", r.ok ? r.donor.c_str() : "", r.ok ? ")" : "");
    }
    return 0;
  }
  // fighter.yml -> fighter.json
  std::string text, why;
  CHECK(fp::yml_to_json(kYml, &text, &why));
  const json j = json::parse(text, nullptr, false);
  CHECK(!j.is_discarded());
  CHECK(j.value("name", "") == "Mr L");
  CHECK(j["files"].value("fighterDataPath", "") == "PlMl.dat");
  CHECK(j["files"].value("animCount", 0) == 290);
  CHECK(j["costumes"].size() == 1);
  CHECK(j["costumes"][0]["file"].value("jointSymbol", "") == "PlyLuigi5K_Share_joint");
  CHECK(j["boneDefinitions"]["lookup"].value("boneCount", 0) == 73);
  CHECK(j["boneDefinitions"]["lookup"].value("waist", 0) == 255);
  CHECK(j["functions"].value("onLoad", 0u) == 2148803364u);
  CHECK(j["functions"].value("onItemDrop", 0u) == 2148000000u);   // onItemRelease, as mexLib names it
  CHECK(j["moveLogic"].size() == 2);
  CHECK(j["moveLogic"][1].value("animationID", 0) == 296);
  CHECK(j["items"].is_array() && j["items"].empty());
  CHECK(!fp::yml_to_json("nameText: x\n", &text, &why));   // no fighter file
  CHECK(!fp::yml_to_json("", &text, &why));

  // The fighters a package is made from, by onLoad.
  CHECK(fp::donor_of(0x80142324u) == 17 && std::strcmp(fp::donor_name(17), "Luigi") == 0);
  CHECK(fp::donor_of(0x800E57ACu) == 1 && std::strcmp(fp::donor_name(1), "Fox") == 0);
  CHECK(fp::donor_of(0x80123456u) == -1);

  // Luigi's onLoad; two rows are not Luigi's 18: a move table of its own.
  int donor = -1;
  fp::Facts f = fp::manifest_facts(text, &donor);
  CHECK(donor == 17);
  CHECK(f.functions_outside == 0 && f.moves_outside == 0 && f.items == 0);
  CHECK(f.own_move_table);
  CHECK(!fp::needs_plugin(f).empty());
  // A MexManager fighter.json: a retail move table by its address, no rows.
  json m = j;
  m["functions"]["moveLogicPointer"] = 0x803D0628u;
  m.erase("moveLogic");
  f = fp::manifest_facts(m.dump(), &donor);
  CHECK(!f.own_move_table && !f.donor_unknown);
  CHECK(fp::needs_plugin(f).empty());
  // Code of its own: a function past the game's, a move callback past it, an article.
  m["functions"]["specialN"] = 0x80500000u;
  f = fp::manifest_facts(m.dump(), &donor);
  CHECK(f.functions_outside == 1 && !fp::needs_plugin(f).empty());
  m["functions"]["specialN"] = 0x8012345Cu;
  m["moveLogic"] = json::array({json{{"animationCallBack", 0x81000000u}}});
  f = fp::manifest_facts(m.dump(), &donor);
  CHECK(f.moves_outside == 1);
  m.erase("moveLogic");
  m["items"] = json::array({json::object()});
  f = fp::manifest_facts(m.dump(), &donor);
  CHECK(f.items == 1 && !fp::needs_plugin(f).empty());
  m["items"] = json::array();
  m["functions"]["onLoad"] = 0x80123456u;
  f = fp::manifest_facts(m.dump(), &donor);
  CHECK(f.donor_unknown && donor == -1 && !fp::needs_plugin(f).empty());
  fp::Facts code;
  code.own_code_file = true;
  CHECK(fp::needs_plugin(code).find("ftFunction") != std::string::npos);
  CHECK(std::strlen(fp::kNeedsPlugin) < 48);

  sound_checks();

  std::printf("%s\n", failures ? "fighter_package: FAILED" : "fighter_package: ok");
  return failures ? 1 : 0;
}
