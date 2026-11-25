// The effect checker (host::cosmetics::check_effect, cosmetic_effect_check.cpp): each verdict from small
// files made here against a stand-in disc, what an effect would take from one installed, and the import
// refusing what the checker refuses.
//   effect_check_test          the files made here (always)
//   effect_check_test --real   the effects study's real files (MU_FX_SLOTS_DIR: pm-test\mods-ui\fx-particles,
//                              its retail\ copies standing in for the disc): the Honduran Falcon Punch works,
//                              the Jigglypuff Turnips is a model swap; skips (77) without the folder
// No file is "works_after_restart": every effect file the game has is read again for each match
// (effect_file_live), so none is made here.
// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include "cosmetic_mods.h"
#include "effect_slots_fixture.h"
#include "nlohmann/json.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using effect_slots_fixture::put32;

// The stand-in disc: file name -> bytes, each at its own offset.
static std::map<std::string, std::vector<uint8_t>> g_disc;
static bool g_quiet = true;
namespace host {
void log(const char* format, ...) {
  if (g_quiet) return;
  va_list args; va_start(args, format); std::vfprintf(stdout, format, args); va_end(args);
  std::fputc('\n', stdout);
}
bool disc_find_file(const std::string& path, uint32_t* offset, uint32_t* size) {
  uint32_t at = 0;
  for (const auto& [name, bytes] : g_disc) {
    if (name == path) { *offset = at << 24; *size = (uint32_t)bytes.size(); return true; }
    ++at;
  }
  return false;
}
bool disc_read(uint32_t offset, void* out, uint32_t size) {
  uint32_t at = 0;
  for (const auto& [name, bytes] : g_disc)
    if (at++ == offset >> 24) {
      const uint32_t from = offset & 0xFFFFFF;
      if ((uint64_t)from + size > bytes.size()) return false;
      std::memcpy(out, bytes.data() + from, size);
      return true;
    }
  return false;
}
}  // namespace host

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}
bool read_file(const fs::path& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
uint32_t get32(const std::vector<uint8_t>& b, size_t at) { return effect_slots_fixture::get32(b, at); }

// An archive: its data block, its pointer fields (data offsets: each holds the offset it points to) and
// its roots.
std::vector<uint8_t> archive(const std::vector<uint8_t>& data, const std::vector<uint32_t>& pointers,
                             const std::vector<std::pair<std::string, uint32_t>>& roots) {
  size_t names = 0;
  for (const auto& r : roots) names += r.first.size() + 1;
  std::vector<uint8_t> b(0x20 + data.size() + 4 * pointers.size() + 8 * roots.size() + names);
  put32(b, 4, (uint32_t)data.size());
  put32(b, 8, (uint32_t)pointers.size());
  put32(b, 12, (uint32_t)roots.size());
  std::memcpy(b.data() + 0x20, data.data(), data.size());
  size_t at = 0x20 + data.size();
  for (uint32_t p : pointers) { put32(b, at, p); at += 4; }
  size_t name = 0;
  for (const auto& r : roots) {
    put32(b, at, r.second); put32(b, at + 4, (uint32_t)name); at += 8;
    name += r.first.size() + 1;
  }
  for (const auto& r : roots) { std::memcpy(b.data() + at, r.first.c_str(), r.first.size() + 1); at += r.first.size() + 1; }
  put32(b, 0, (uint32_t)b.size());
  return b;
}

// A fighter's own file (PlPe.dat): ftData (0x60 bytes of fields) with x0 pointing at one block and x48
// (its items) at another. `items`, `attributes`: the byte the block's pattern starts at; `pad` zero bytes
// after the x0 block (another writer's alignment).
std::vector<uint8_t> fighter_file(uint8_t attributes, uint8_t items, uint32_t pad = 0) {
  const uint32_t at = 0x80 + pad;
  std::vector<uint8_t> data(at + 0x20, 0);
  put32(data, 0x00, 0x60);
  put32(data, 0x48, at);
  for (uint32_t i = 0; i < 0x20; ++i) { data[0x60 + i] = uint8_t(attributes + i); data[at + i] = uint8_t(items + i); }
  return archive(data, {0x00, 0x48}, {{"ftDataPeach", 0}});
}

// One more pointer field in an archive (its relocation table grows by one entry).
std::vector<uint8_t> with_pointer(std::vector<uint8_t> b, uint32_t field, uint32_t target) {
  const uint32_t data_size = get32(b, 4), count = get32(b, 8);
  const size_t at = 0x20 + data_size + 4ull * count;
  b.insert(b.begin() + (std::ptrdiff_t)at, 4, 0);
  put32(b, at, field);
  put32(b, 0x20 + field, target);
  put32(b, 8, count + 1);
  put32(b, 0, (uint32_t)b.size());
  return b;
}

host::cosmetics::EffectVerdict judge(const fs::path& folder, const char* name, const std::vector<uint8_t>& bytes) {
  const fs::path file = folder / name;
  write_file(file, bytes);
  return host::cosmetics::check_effect(file.u8string());
}

int made_here(const fs::path& folder) {
  using host::cosmetics::check_effect;
  const auto clean = effect_slots_fixture::make(0, nullptr, true);
  g_disc.clear();
  g_disc["EfCaData.dat"] = clean.bytes;
  g_disc["EfCoData.dat"] = clean.bytes;   // (only looked up: the disc is there)
  g_disc["PlPe.dat"] = fighter_file(0x10, 0x40);

  // works: a Falcon Punch generator changed, the disc file's layout kept.
  auto punch = clean.bytes, all = clean.bytes;
  punch[clean.gen4001] ^= 0x55;
  all[clean.gen4001] ^= 0x11; all[clean.kick] ^= 0x01;
  auto v = judge(folder, "Honduran Punch.dat", punch);
  check(v.verdict == "works" && v.reason.empty(), "a changed punch works");
  check(v.target == "EfCaData.dat" && v.character == "Captain Falcon", "its file and fighter");
  check(v.slots == std::vector<std::string>{"Falcon Punch"} && !v.whole, "it changes the Falcon Punch slot alone");
  check(v.overrides.empty(), "nothing installed: it overrides nothing");
  const auto line = host::cosmetics::effect_verdict_json(v);
  const auto parsed = nlohmann::json::parse(line, nullptr, false);
  check(line.find('\n') == std::string::npos && parsed.is_object() && parsed.value("verdict", "") == "works" &&
            parsed.value("target", "") == "EfCaData.dat" && parsed["slots"].size() == 1 && parsed.value("checks", 0) == 1,
        "its JSON line");

  // overrides: one installed that changes every slot loses its Falcon Punch to this one.
  host::cosmetics::configure((folder / L"port-settings.ini").string());
  write_file(folder / "American Falcon.dat", all);
  const auto installed = host::cosmetics::import_effect((folder / "American Falcon.dat").u8string());
  check(installed.ok, "an effect of every slot installs");
  v = check_effect((folder / "Honduran Punch.dat").u8string());
  check(v.verdict == "works" && v.overrides.size() == 1 && v.overrides[0].asset_id == installed.asset_id &&
            v.overrides[0].slots == std::vector<std::string>{"Falcon Punch"},
        "it would take the Falcon Punch from the installed effect");
  v = check_effect((folder / "American Falcon.dat").u8string());
  check(v.verdict == "works" && v.overrides.empty(), "an installed effect takes nothing from itself");

  // broken: cut short, the disc's own file, fewer model effects than the disc's, an added fighter's.
  auto cut = punch;
  cut.resize(cut.size() / 2);
  v = judge(folder, "cut.dat", cut);
  check(v.verdict == "broken" && v.reason == "It is damaged: not a whole game file.", "a file cut short is broken");
  v = judge(folder, "same.dat", clean.bytes);
  check(v.verdict == "broken" && v.reason == "It is the game's own file.", "the disc's own file is broken");
  // (a pointer at the table's third model effect ends the table there: two model effects, the disc's three)
  v = judge(folder, "short.dat", with_pointer(punch, 0x44, 0x30));
  check(v.verdict == "broken" && v.reason == "It has fewer effects than the game's file.",
        "fewer model effects than the disc's is broken");
  auto added = punch;
  const size_t root = added.size() - std::strlen("effCaptainDataTable") - 1;
  std::memcpy(added.data() + root, "effCaptaiXDataTable", 19);
  v = judge(folder, "added.dat", added);
  check(v.verdict == "broken" && v.reason == "Its fighter is not in this game.", "an added fighter's effects are broken");
  v = judge(folder, "junk.dat", std::vector<uint8_t>(64, 0xAB));
  check(v.verdict == "broken", "junk is broken");

  // not_an_effect: a fighter file that swaps only its items' models (Jigglypuff Turnips), one that changes
  // more, a costume, and a file of no effect at all.
  // (laid out anew with its blocks padded, as the real one is: padding is no change)
  v = judge(folder, "Jigglypuff Turnips.dat", fighter_file(0x10, 0x77, 0x20));
  check(v.verdict == "not_an_effect" && v.really == "a model swap of Peach's items" &&
            v.reason == "Not an effect: it is a model swap of Peach's items.",
        "items alone changed: a model swap");
  v = judge(folder, "Peach mod.dat", fighter_file(0x22, 0x77));
  check(v.verdict == "not_an_effect" && v.really == "a change to Peach's fighter file", "more changed: a fighter file mod");
  std::vector<uint8_t> joint(0x40, 0);
  const auto skin = archive(joint, {}, {{"PlyPeach5K_Share_joint", 0}});
  v = judge(folder, "skin.dat", skin);
  check(v.verdict == "not_an_effect" && v.really.rfind("a character skin (Peach", 0) == 0, "a costume is a skin");
  v = judge(folder, "readme.txt", {'h', 'i'});
  check(v.verdict == "not_an_effect", "a text file is no effect");

  // The import refuses what the checker refuses, with its verdict.
  const auto refused = host::cosmetics::import_effect((folder / "Jigglypuff Turnips.dat").u8string());
  check(!refused.ok && refused.verdict == "not_an_effect" && refused.message == "Not an effect: it is a model swap of Peach's items.",
        "the import refuses a model swap");
  const auto broken = host::cosmetics::import_effect((folder / "short.dat").u8string());
  check(!broken.ok && broken.verdict == "broken", "the import refuses fewer model effects");

  // A folder is one mod: the worst of its files.
  fs::create_directories(folder / "pack");
  write_file(folder / "pack" / "EfCaData.dat", punch);
  write_file(folder / "pack" / "skin.dat", skin);
  v = check_effect((folder / "pack").u8string());
  check(v.verdict == "not_an_effect" && v.parts.size() == 2, "a folder with a skin in it is refused");

  // Without the disc it is not checked.
  g_disc.clear();
  v = check_effect((folder / "Honduran Punch.dat").u8string());
  check(v.verdict.empty() && v.reason == "The game disc is needed to check it.", "no disc: not checked");
  return failures;
}

int real(const fs::path& folder) {
  const char* dir = std::getenv("MU_FX_SLOTS_DIR");
  if (!dir || !*dir) { std::puts("MU_FX_SLOTS_DIR unset; skipping the real files"); return 77; }
  const fs::path root = fs::u8path(dir);
  g_disc.clear();
  for (const char* name : {"EfCaData.dat", "EfFxData.dat", "PlPe.dat"})
    if (!read_file(root / "retail" / name, &g_disc[name])) { std::fprintf(stderr, "no retail\\%s in %s\n", name, dir); return 1; }
  g_disc["EfCoData.dat"] = g_disc["EfCaData.dat"];   // (only looked up: the disc is there)
  host::cosmetics::configure((folder / L"real-settings.ini").string());
  const auto honduran = host::cosmetics::check_effect((root / "standin-data2" / "files" / "EfCaData.dat").u8string());
  std::printf("%s\n", host::cosmetics::effect_verdict_json(honduran).c_str());
  check(honduran.verdict == "works" && honduran.target == "EfCaData.dat" && !honduran.slots.empty(),
        "the Honduran Falcon Punch works");
  const auto turnips = host::cosmetics::check_effect((root / "real-files" / "10854.dat").u8string());
  std::printf("%s\n", host::cosmetics::effect_verdict_json(turnips).c_str());
  check(turnips.verdict == "not_an_effect" && turnips.really == "a model swap of Peach's items",
        "the Jigglypuff Turnips is a model swap");
  return failures;
}
}  // namespace

int main(int argc, char** argv) {
  const bool real_files = argc > 1 && std::strcmp(argv[1], "--real") == 0;
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const fs::path folder = fs::path(temp) / (L"effect_check_test_" + std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  const int result = real_files ? real(folder) : made_here(folder);
  fs::remove_all(folder, ec);
  if (result == 77) return 77;
  if (failures) { std::fprintf(stderr, "%d failed\n", failures); return 1; }
  std::puts(real_files ? "effect checks: the real files as the study found them" : "effect checks: every verdict as expected");
  return 0;
}
