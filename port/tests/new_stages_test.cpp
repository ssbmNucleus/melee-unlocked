// New stages (host/new_stages.h): which retail stage a site file plays over (from its name), the checks a
// stage file passes before it is installed and served (a stage the game reads, the collision groups and
// lines its base's code needs, four starting points, the camera's limits, the blast zones), and the
// store's install, serve, icon, random setting and remove. Offline, no disc: the stage files are built here.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "new_stages.h"

namespace fs = std::filesystem;
namespace ns = host::new_stages;

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

// A stage archive as the game reads one: coll_data with `lines` lines (two vertices) and `groups`
// collision groups, map_head with one point list holding `ids`, an empty model list, grGroundParam.
// `fill` marks the ground parameters so two files with the same shape differ.
static std::vector<uint8_t> stage(uint32_t lines, uint32_t groups, std::initializer_list<int> ids, uint8_t fill = 0) {
  const uint32_t coll = 0, verts = 0x30, line_at = verts + 16, joint_at = line_at + lines * 16;
  const uint32_t head = joint_at + groups * 0x28, list = head + 48, pairs = list + 12;
  const uint32_t param = pairs + (uint32_t)ids.size() * 4, data = param + 0x40;
  std::vector<uint32_t> relocs = {coll + 0, coll + 8, coll + 0x24, head + 0, head + 8, list + 4};
  const char* const names[] = {"coll_data", "grGroundParam", "map_head"};
  const uint32_t root_at[] = {coll, param, head};
  std::string strings;
  std::vector<uint32_t> name_at;
  for (const char* n : names) {
    name_at.push_back((uint32_t)strings.size());
    strings += n;
    strings += '\0';
  }
  const uint32_t size = 0x20 + data + (uint32_t)relocs.size() * 4 + 3 * 8 + (uint32_t)strings.size();
  std::vector<uint8_t> b(size, 0);
  put32(b, 0, size);
  put32(b, 4, data);
  put32(b, 8, (uint32_t)relocs.size());
  put32(b, 12, 3);
  auto d = [&](uint32_t at, uint32_t v) { put32(b, 0x20 + at, v); };
  d(coll + 0, verts);
  d(coll + 4, 2);
  d(coll + 8, line_at);
  d(coll + 0x0C, lines);
  d(coll + 0x24, joint_at);
  d(coll + 0x28, groups);
  for (uint32_t i = 0; i < lines; ++i) {
    d(line_at + i * 16, 0x00000001);   // vertex 0 to vertex 1
    d(line_at + i * 16 + 4, 0xFFFFFFFF);
    d(line_at + i * 16 + 8, 0xFFFFFFFF);
  }
  for (uint32_t g = 0; g < groups; ++g) d(joint_at + g * 0x28 + 0x24, 0x00000002);   // vertices 0..1
  d(head + 0, list);
  d(head + 4, 1);
  d(head + 8, list);   // the model list: none (count 0)
  d(list + 4, pairs);
  d(list + 8, (uint32_t)ids.size());
  uint32_t k = 0;
  for (const int id : ids) d(pairs + 4 * k++, (uint32_t)(uint16_t)id);
  for (uint32_t i = 0; i < 0x40; ++i) b[0x20 + param + i] = fill;
  const uint32_t table = 0x20 + data;
  for (size_t i = 0; i < relocs.size(); ++i) put32(b, table + i * 4, relocs[i]);
  const uint32_t roots = table + (uint32_t)relocs.size() * 4;
  for (int i = 0; i < 3; ++i) {
    put32(b, roots + i * 8, root_at[i]);
    put32(b, roots + i * 8 + 4, name_at[i]);
  }
  std::memcpy(&b[roots + 24], strings.data(), strings.size());
  return b;
}

static const std::initializer_list<int> kVersus = {0, 1, 2, 3, 148, 149, 150, 151};

// An 8x7 red PNG (the icon's picture).
static const uint8_t kRedPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x07, 0x08, 0x02, 0x00, 0x00, 0x00, 0xBA, 0x3B, 0x9B, 0x09, 0x00, 0x00, 0x00, 0x12, 0x49,
    0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xFC, 0xCF, 0x80, 0x1D, 0x30, 0xE1, 0x10, 0x1F, 0x58, 0x09, 0x00, 0xB2, 0xCA,
    0x01, 0x0D, 0x12, 0xBC, 0x65, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

int main() {
  std::string why;

  // Names: the survey's real file names (SSBM Nucleus custom stage posts, 2026-11-24).
  check(ns::base_of("Smashville_GrTCa.dat", "") == "GrTCa.dat", "a Target Test word in the name");
  check(ns::base_of("Castle DeDeDe/Castle DeDeDe(GrTYs).dat", "") == "GrTYs.dat", "a base in brackets");
  check(ns::base_of("GrTYs - Shifting Sand Land.dat", "") == "GrTYs.dat", "a base first");
  check(ns::base_of("inverted pyramid/GrTDr(Upside-down Pyramid).dat", "") == "GrTDr.dat", "a folder and a note");
  check(ns::base_of("Icicle Smashville/Replace Stage (DAT Texture Wizard)/GrNBa.dat", "Icicle Mountain") == "GrNBa.dat",
        "the file's name wins over the post's stage");
  check(ns::base_of("PS5 over Final Destination/GrNLaPs5.dat", "Final Destination") == "GrNLa.dat",
        "a word that begins with the stage the post names");
  check(ns::base_of("PS5 over Final Destination/GrNLaPs5.dat", "").empty(), "the same without the post's word");
  check(ns::base_of("Tartarus_Lobby/GrTrs.dat", "Final Destination").empty(), "an m-ex stage id is no base");
  check(ns::base_of("GrTLk.dat", "").empty(), "an untested Target Test");
  check(ns::base_of("GrNBa GrNLa.dat", "").empty(), "two bases");
  check(ns::base_of("GrTCa.zip", "").empty(), "a zip");
  check(ns::base_of("GrPs.dat", "Pokemon Stadium").empty(), "Pokemon Stadium");
  check(ns::base_of("grtca.DAT", "") == "GrTCa.dat", "any case");
  check(ns::unsupported_reason("Smashville_GrTCa.dat", "").empty(), "a taken file has no reason");
  check(ns::unsupported_reason("GrPs.dat", "").find("Pokemon") != std::string::npos, "Pokemon Stadium says so");
  check(ns::unsupported_reason("GrTLk.dat", "").find("Target Test") != std::string::npos, "an untested Target Test says so");
  check(ns::unsupported_reason("GrTCa.zip", "") == "Not a stage file.", "a zip says so");
  check(ns::base_name("GrNLa.dat") == "Final Destination" && ns::base_name("GrXx.dat").empty(), "base names");
  check(ns::target_test_base("GrTCa.dat") && !ns::target_test_base("GrNLa.dat"), "which bases are Target Tests");

  // The checks. Retail counts as the disc has them: Captain Falcon's Target Test 134 lines in 5 groups,
  // Final Destination 16 lines in 1 group.
  const auto tca = stage(134, 5, kVersus), fd = stage(16, 1, kVersus);
  check(ns::check("GrTCa.dat", stage(7, 5, kVersus), tca, &why), "a Target Test stage with few lines (Smashville)");
  check(ns::check("GrNLa.dat", stage(16, 1, kVersus, 0x22), fd, &why), "a Final Destination with its lines (PS5)");
  check(!ns::check("GrTCa.dat", stage(7, 4, kVersus), tca, &why) && why.find("groups") != std::string::npos,
        "fewer collision groups than the base");
  check(!ns::check("GrNLa.dat", stage(9, 1, kVersus), fd, &why) && why.find("lines") != std::string::npos,
        "a versus stage with fewer lines (Icicle Smashville over Battlefield)");
  check(!ns::check("GrTCa.dat", stage(5, 6, {0, 148, 149, 150, 151}), tca, &why) && why.find("1 starting point") != std::string::npos,
        "one start (a Target Test made for one player)");
  check(!ns::check("GrTCa.dat", stage(7, 5, {0, 1, 2, 3, 150, 151}), tca, &why) && why.find("camera") != std::string::npos,
        "no camera limits");
  check(!ns::check("GrTCa.dat", stage(7, 5, {0, 1, 2, 3, 148, 149}), tca, &why) && why.find("blast") != std::string::npos,
        "no blast zones");
  check(!ns::check("GrNLa.dat", fd, fd, &why), "the disc's own file");
  check(!ns::check("GrNLa.dat", stage(16, 1, kVersus, 0x22), {}, &why), "no disc file to compare");
  check(!ns::check("GrPs.dat", stage(16, 1, kVersus), fd, &why), "not a base");
  check(!ns::check("GrNLa.dat", std::vector<uint8_t>(64, 0x55), fd, &why), "junk");
  check(!ns::check("GrNLa.dat", std::vector<uint8_t>(ns::kMaxBytes + 1, 0), fd, &why), "an oversized file");
  {
    auto b = stage(7, 5, kVersus);
    put32(b, 0x20 + 0x0C, 4000);   // more lines than the file holds
    check(!ns::check("GrTCa.dat", b, tca, &why), "a collision that runs past the file");
  }

  // The store.
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / L"new_stages_test";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ns::Installed a;
  a.base = "GrTCa.dat";
  a.name = "Smashville";
  a.file_id = 17089;
  a.mod_id = 14046;
  const auto smashville = stage(7, 5, kVersus);
  check(!ns::install(a, smashville, tca, "", &why), "not set up: refused");
  ns::configure((dir / L"settings.ini").u8string());
  check(ns::list().empty() && !ns::random_included(), "nothing installed at first, random leaves them out");
  std::vector<uint8_t> out;
  check(!ns::served("s17089", tca, &out, nullptr), "nothing served at first");
  check(ns::install(a, smashville, tca, std::string((const char*)kRedPng, sizeof kRedPng), &why), "install");
  check(!ns::install(a, stage(7, 5, {0}), tca, "", &why), "a file that fails a check is refused");
  ns::Installed b = a;
  b.base = "GrNLa.dat";
  b.name = "PS5 over Final Destination";
  b.file_id = 16343;
  const auto ps5 = stage(16, 1, kVersus, 0x33);
  check(ns::install(b, ps5, fd, "", &why), "a second");
  const auto all = ns::list();
  check(all.size() == 2 && all[0].name == "PS5 over Final Destination" && all[1].id == "s17089", "listed by name");
  ns::Installed got;
  check(ns::served("s17089", tca, &out, &got) && out == smashville && got.base == "GrTCa.dat", "served");
  // The icon: the picture, cut and made 64x56, as RGBA8 4x4 blocks (alpha and red, then green and blue).
  const auto icon = ns::icon("s17089");
  check(icon.size() == (size_t)ns::kIconWidth * ns::kIconHeight * 4, "an icon's size");
  check(icon.size() > 33 && icon[0] == 0xFF && icon[1] == 0xFF && icon[32] == 0 && icon[33] == 0, "the picture's red");
  const auto plain = ns::icon("s16343");
  check(plain.size() == icon.size() && plain[1] != 0xFF, "no picture: a plain tile");
  check(ns::set_random_included(true, &why) && ns::random_included(), "random takes them");
  // Read again from disk.
  ns::configure((dir / L"settings.ini").u8string());
  check(ns::list().size() == 2 && ns::random_included(), "kept across runs");
  // A file changed on disk is not served.
  {
    std::fstream f(dir / L"NewStages" / L"s16343.dat", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(0x30);
    f.put((char)0x77);
  }
  check(!ns::served("s16343", fd, &out, nullptr), "a changed file: the base plays as it is");
  check(ns::remove("s16343", &why) && ns::list().size() == 1 && !fs::exists(dir / L"NewStages" / L"s16343.dat"),
        "remove");
  check(ns::remove("s17089", &why) && !fs::exists(dir / L"NewStages" / L"s17089.png"), "removed with its picture");
  check(!ns::remove("s17089", &why), "removing twice");
  // Room for two pages of icons.
  for (int i = 0; i < (int)ns::kMaxInstalled; ++i) {
    ns::Installed m = a;
    m.file_id = 1000 + i;
    m.name = "Stage " + std::to_string(i);
    check(ns::install(m, smashville, tca, "", &why), "filling the pages");
  }
  ns::Installed extra = a;
  extra.file_id = 2000;
  check(!ns::install(extra, smashville, tca, "", &why) && why.find("remove one") != std::string::npos, "no room past them");
  ns::Installed again = a;
  again.file_id = 1000;
  again.name = "Again";
  check(ns::install(again, smashville, tca, "", &why) && ns::list().size() == ns::kMaxInstalled,
        "installing one again takes its place");
  // A store that cannot be read: nothing loaded.
  {
    std::ofstream f(dir / L"NewStages" / L"installed.json", std::ios::binary | std::ios::trunc);
    f << "{ not json";
  }
  ns::configure((dir / L"settings.ini").u8string());
  check(ns::list().empty(), "an unreadable store loads nothing");
  fs::remove_all(dir, ec);

  if (g_failures == 0) std::printf("new_stages_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
