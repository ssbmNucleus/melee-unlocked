// Menu mods (host/menu_mods.h): the checks a menu file passes before it is installed and served, the
// DAT Texture Wizard layout made readable, and the store's install, replace, serve and remove. Offline,
// no disc: the "retail" file is built here.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "menu_mods.h"

namespace fs = std::filesystem;
namespace mm = host::menu_mods;

static int g_failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = (uint8_t)(v >> 24); b[at + 1] = (uint8_t)(v >> 16); b[at + 2] = (uint8_t)(v >> 8); b[at + 3] = (uint8_t)v;
}

// An HSD archive: `data_words` words of data, a pointer at data offset 0 to `target`, one root symbol
// `root` at 0, then `tail` bytes after the tables (where DAT Texture Wizard puts a grown texture).
static std::vector<uint8_t> archive(const char* root, uint32_t data_words, uint32_t target, uint32_t tail = 0,
                                    uint8_t fill = 0x11) {
  const uint32_t data = data_words * 4, relocs = 1, roots = 1;
  const std::string names = std::string(root) + '\0';
  const uint32_t size = 0x20 + data + relocs * 4 + roots * 8 + (uint32_t)names.size() + tail;
  std::vector<uint8_t> b(size, 0);
  put32(b, 0, size);
  put32(b, 4, data);
  put32(b, 8, relocs);
  put32(b, 12, roots);
  put32(b, 16, 0);
  for (uint32_t i = 4; i < data; ++i) b[0x20 + i] = fill;
  put32(b, 0x20, target);                // the data's first word: a pointer
  put32(b, 0x20 + data, 0);              // relocation: data offset 0
  put32(b, 0x20 + data + 4, 0);          // root at 0
  put32(b, 0x20 + data + 8, 0);          // its name at 0 of the strings
  std::memcpy(&b[0x20 + data + 12], names.data(), names.size());
  for (uint32_t i = 0; i < tail; ++i) b[0x20 + data + 12 + names.size() + i] = 0xAB;
  return b;
}

int main() {
  const std::vector<uint8_t> retail = archive("MnSelectChrDataTable", 64, 16);
  std::string why;

  // Names.
  check(mm::target_of("Vaporwave CSS 1.1/MnSlChr.dat") == "MnSlChr.usd", "a CSS file's target");
  check(mm::target_of("MnSlChr.usd - Blue Space CSS (v2 by NyxTheShield).dat") == "MnSlChr.usd", "a decorated CSS name");
  check(mm::target_of("Blue MnSlMap.dat").empty(), "a stage select is not taken yet");
  check(mm::target_of("PlFxNr.dat").empty(), "a costume is no menu");
  check(mm::target_of("MnSlChr.png").empty(), "a picture is no menu");
  check(mm::kind_of("GmPause.dat") == "Pause screen", "pause kind");
  check(mm::kind_of("IfAll.dat") == "HUD font", "HUD kind");
  check(!mm::unsupported_reason("IfAll.dat").empty(), "a HUD font says why");
  check(mm::unsupported_reason("MnSlChr.dat").empty(), "a CSS is offered");

  // The checks.
  check(mm::check("MnSlChr.usd", retail, retail, &why), "the retail file passes");
  check(mm::check("MnSlChr.usd", archive("MnSelectChrDataTable", 80, 64, 0, 0x22), retail, &why),
        "another layout with the same symbols passes");
  check(!mm::check("MnSlMap.usd", retail, retail, &why), "a file this game does not take");
  check(!mm::check("MnSlChr.usd", archive("MnSelectStageDataTable", 64, 16), retail, &why), "another screen's symbols");
  check(!mm::check("MnSlChr.usd", archive("MnSelectChrDataTable", 64, 999), retail, &why), "a pointer past the data");
  {
    auto b = retail;
    put32(b, 0, (uint32_t)b.size() + 4);
    check(!mm::check("MnSlChr.usd", b, retail, &why), "a header larger than the file");
    b = retail;
    put32(b, 8, 0x01000000);
    check(!mm::check("MnSlChr.usd", b, retail, &why), "a relocation table past the end");
    b = retail;
    put32(b, 0x20 + 256, 4096);
    check(!mm::check("MnSlChr.usd", b, retail, &why), "a relocation outside the data");
    b = retail;
    put32(b, 0x20 + 256 + 8, 4096);
    check(!mm::check("MnSlChr.usd", b, retail, &why), "a symbol name outside the strings");
    b.resize(10);
    check(!mm::check("MnSlChr.usd", b, retail, &why), "a short file");
    check(!mm::check("MnSlChr.usd", std::vector<uint8_t>(mm::kMaxBytes + 1, 0), retail, &why), "an oversized file");
    check(!mm::check("MnSlChr.usd", retail, {}, &why), "no retail copy to compare");
  }

  // A texture after the tables (DAT Texture Wizard): refused as it is, read after normalize.
  {
    // The names end at data offset 256 + 4 + 8 + 21 = 289; the picture after them, 32-byte aligned, at 320.
    const uint32_t data = 64 * 4, names_end = data + 4 + 8 + (uint32_t)sizeof("MnSelectChrDataTable");
    const uint32_t tail_at = (names_end + 31) & ~31u;
    auto grown = archive("MnSelectChrDataTable", 64, tail_at, 96);
    check(!mm::check("MnSlChr.usd", grown, retail, &why), "a pointer to after the tables is refused as it is");
    // Loose is still not anywhere: into the tables, unaligned, or past the file.
    for (const uint32_t bad_to : {data + 2u, data + 8u, names_end, tail_at + 4u, names_end + 96u + 64u}) {
      auto bad = archive("MnSelectChrDataTable", 64, bad_to, 96);
      check(!mm::normalize(&bad) && !mm::check("MnSlChr.usd", bad, retail, &why),
            ("a pointer to data offset " + std::to_string(bad_to) + " is refused even loose").c_str());
    }
    auto fixed = grown;
    check(mm::normalize(&fixed), "normalize takes it");
    check(mm::check("MnSlChr.usd", fixed, retail, &why), "normalized, it passes");
    // The same bytes at the same offset of the data.
    check(fixed.size() > grown.size() && std::memcmp(fixed.data() + 0x20, grown.data() + 0x20, grown.size() - 0x20) == 0,
          "the data keeps every offset");
    auto junk = std::vector<uint8_t>(64, 0x55);
    check(!mm::normalize(&junk), "junk is no archive");
    auto plain = retail;
    check(mm::normalize(&plain) && plain == retail, "a readable file is left as it is");
  }

  // The store.
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / L"menu_mods_test";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  mm::configure((dir / L"settings.ini").u8string());
  check(mm::list().empty(), "nothing installed at first");
  std::vector<uint8_t> out;
  check(!mm::served("MnSlChr.usd", retail, &out), "nothing served at first");
  mm::Installed a;
  a.target = "MnSlChr.usd";
  a.name = "Blue CSS";
  a.file_id = 101;
  a.mod_id = 1;
  const auto first = archive("MnSelectChrDataTable", 70, 40, 0, 0x33);
  check(mm::install(a, first, retail, "", &why), "install");
  check(!mm::install(a, archive("MnSelectStageDataTable", 64, 16), retail, "", &why), "a wrong file is refused");
  check(mm::served("MnSlChr.usd", retail, &out) && out == first, "served");
  mm::Installed b = a;
  b.name = "Red CSS";
  b.file_id = 202;
  std::string replaced;
  const auto second = archive("MnSelectChrDataTable", 72, 40, 0, 0x44);
  check(mm::install(b, second, retail, "", &why, &replaced) && replaced == "Blue CSS", "a second replaces the first");
  check(mm::list().size() == 1 && mm::list()[0].file_id == 202, "one a file");
  check(!fs::exists(dir / L"MenuMods" / L"m101.dat"), "the first's file is gone");
  check(mm::served("MnSlChr.usd", retail, &out) && out == second, "the second is served");
  // Read again from disk.
  mm::configure((dir / L"settings.ini").u8string());
  check(mm::list().size() == 1 && mm::list()[0].name == "Red CSS", "kept across runs");
  // A file changed on disk is not served.
  {
    std::fstream f(dir / L"MenuMods" / L"m202.dat", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0x30);
    f.put((char)0x77);
  }
  check(!mm::served("MnSlChr.usd", retail, &out), "a changed file falls back to the disc's");
  std::string target;
  check(mm::remove("m202", &why, &target) && target == "MnSlChr.usd", "remove");
  check(mm::list().empty() && !fs::exists(dir / L"MenuMods" / L"m202.dat"), "removed with its file");
  check(!mm::remove("m202", &why), "removing twice");
  fs::remove_all(dir, ec);

  if (g_failures == 0) std::printf("menu_mods_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
