// A direct launch finds its existing profile and a verified, previously selected disc, in its own data
// folder, after copying a returning player's profile forward once: this program's earlier (Netlee)
// folder first, else Melee Unlocked's. A no-argument start's command line (its folders, no
// --vanilla-only), the saves of the earlier vanilla-only plain starts carried into its card, and its
// replay folder (the Slippi Launcher's, else Documents\Slippi, else its own).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../app/disc_memory.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void write(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

int main(int argc, char** argv) {
  CHECK(disc_memory::iso_line("") == "");
  CHECK(disc_memory::iso_line("iso=C:\\Games\\melee.iso\r\n") == "C:\\Games\\melee.iso");
  CHECK(disc_memory::iso_line("language=en\niso=a.iso\nnotesseen=1\niso=b.iso") == "b.iso");
  CHECK(disc_memory::iso_line("lobbyiso=x.iso\n") == "");

  const fs::path root = fs::absolute(argc > 1 ? fs::path(argv[1]) : fs::path("disc_memory_test_root"));
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path local = root / "Local";
  const auto decode = [](const std::string& raw, disc_memory::Encoding) { return fs::u8path(raw); };
  // "Verifies" when the file exists and starts with GALE01, as is_melee_102 checks the header.
  const auto verify = [](const fs::path& disc) {
    std::ifstream f(disc, std::ios::binary);
    char head[6] = {};
    return f.read(head, 6) && std::string(head, 6) == "GALE01";
  };
  // "Unmodified" stands in for mod_scan's whole-disc check: here, a file whose bytes end in VANILLA.
  int full_checks = 0;
  const auto unmodified = [&full_checks](const fs::path& disc) {
    ++full_checks;
    std::ifstream f(disc, std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return all.size() >= 7 && all.compare(all.size() - 7, 7, "VANILLA") == 0;
  };
  const fs::path good = root / "discs" / "melee.iso", other = root / "discs" / "other.iso", bad = root / "discs" / "notes.txt";
  const fs::path modded = root / "discs" / "20XX.iso";   // the 1.02 header, other contents
  write(good, "GALE01..VANILLA");
  write(other, "GALE01..VANILLA");
  write(bad, "hello");
  write(modded, "GALE01..20XXTE");

  // Nothing remembered anywhere: nothing found (the caller goes on to melee.iso, then the picker).
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified).empty());
  CHECK(disc_memory::places({}).empty());

  const auto chosen = local / "MeleeUnlocked" / "standalone.ini";
  const auto launcher = local / "MeleeUnlocked" / "launcher.ini";
  const auto prior = local / "MeleeUnlocked-YouGame" / "standalone.ini";
  const auto own = local / "Nucleus Client" / "standalone.ini";
  // This program's own folder, whatever Melee Unlocked left; its earlier one is read next.
  CHECK(disc_memory::standalone_dir({}).empty());
  CHECK(disc_memory::standalone_dir(local) == own.parent_path());
  CHECK(disc_memory::places(local).front().ini == own);
  CHECK(disc_memory::places(local)[1].ini == local / "Netlee" / "standalone.ini");
  // Melee Unlocked's profile (read only): a launcher.ini alone must not hide an earlier standalone
  // player's saves. A fork profile wins when the player has already used both. Empty card folders on
  // first launch are not saved data.
  CHECK(disc_memory::upstream_dir({}).empty());
  CHECK(disc_memory::upstream_dir(local) == chosen.parent_path());
  write(launcher, "language=en\n");
  fs::create_directories(chosen.parent_path() / "User/GC/CardA/VanillaOnly");
  write(prior.parent_path() / "User/GC/CardA/VanillaOnly/GALE01.gci", "old save");
  CHECK(disc_memory::upstream_dir(local) == prior.parent_path());
  CHECK(disc_memory::places(local)[2].ini == prior);
  write(chosen.parent_path() / "port-settings.ini", "volume 58\n");
  CHECK(disc_memory::upstream_dir(local) == chosen.parent_path());
  write(prior, "iso=" + good.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  write(launcher, "iso=" + modded.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  write(launcher, "iso=" + other.u8string() + "\n");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == other);
  write(chosen, "iso=" + good.u8string() + "\n");
  full_checks = 0;
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  CHECK(full_checks == 0);

  // remember() writes the line read back.
  disc_memory::remember(chosen, good);
  CHECK(disc_memory::read_iso(chosen) == good.u8string());

  // Copy forward, once: the profile's files (not saves, replays or logs) into this program's folder,
  // Melee Unlocked's folder left exactly as it was.
  const fs::path up = chosen.parent_path(), mine = own.parent_path();
  write(up / "User/Slippi/user.json", "{\"uid\":\"x\"}");
  write(up / "CosmeticMods/catalog.json", "{}");
  write(up / "plugin-approvals.json", "{}");
  write(up / "Replays/Game.slp", "replay");
  write(up / "User/GC/CardA/GALE01.gci", "save");
  write(up / "melee.log", "log");
  const auto read = [](const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string up_ini = read(up / "standalone.ini"), up_settings = read(up / "port-settings.ini");
  CHECK(disc_memory::copy_forward(local) == up);
  CHECK(read(mine / "standalone.ini") == up_ini);
  CHECK(read(mine / "port-settings.ini") == up_settings);
  CHECK(read(mine / "User/Slippi/user.json") == "{\"uid\":\"x\"}");
  CHECK(fs::is_regular_file(mine / "CosmeticMods/catalog.json"));
  CHECK(fs::is_regular_file(mine / "plugin-approvals.json"));
  CHECK(fs::is_regular_file(mine / disc_memory::kForwardMarker));
  CHECK(!fs::exists(mine / "Replays") && !fs::exists(mine / "User/GC") && !fs::exists(mine / "melee.log"));
  CHECK(read(up / "standalone.ini") == up_ini && read(up / "User/GC/CardA/GALE01.gci") == "save" &&
        read(up / "Replays/Game.slp") == "replay" && read(up / "melee.log") == "log");
  CHECK(disc_memory::first_verified(disc_memory::places(local), decode, verify, unmodified) == good);
  // Never again: not after the player empties the folder.
  fs::remove(mine / "standalone.ini", ec);
  fs::remove(mine / "port-settings.ini", ec);
  fs::remove_all(mine / "User", ec);
  CHECK(disc_memory::copy_forward(local).empty());
  CHECK(!fs::exists(mine / "standalone.ini"));
  // A folder that already holds a profile is never copied into.
  const fs::path other_local = root / "Other";
  write(other_local / "MeleeUnlocked/port-settings.ini", "volume 10\n");
  write(other_local / "Nucleus Client/port-settings.ini", "volume 90\n");
  CHECK(disc_memory::copy_forward(other_local).empty());
  CHECK(read(other_local / "Nucleus Client/port-settings.ini") == "volume 90\n");
  // A player who used this program as Netlee: that profile, not Melee Unlocked's older one, comes
  // forward, and the Netlee folder is left exactly as it was.
  const fs::path renamed = root / "Renamed", netlee = renamed / "Netlee", nucleus = renamed / "Nucleus Client";
  write(renamed / "MeleeUnlocked/port-settings.ini", "volume 10\n");
  write(renamed / "MeleeUnlocked/standalone.ini", "iso=" + other.u8string() + "\n");
  write(netlee / "port-settings.ini", "volume 70\n");
  write(netlee / "standalone.ini", "iso=" + good.u8string() + "\n");
  write(netlee / "plugin-approvals.json", "{\"wolf\":1}");
  write(netlee / "User/Slippi/user.json", "{\"uid\":\"n\"}");
  write(netlee / "User/GC/CardA/GALE01.gci", "netlee save");
  write(netlee / disc_memory::kForwardMarker, "profile copied from MeleeUnlocked (3 items); that folder was left as it was\n");
  CHECK(disc_memory::forward_source(renamed) == netlee);
  CHECK(disc_memory::copy_forward(renamed) == netlee);
  CHECK(read(nucleus / "port-settings.ini") == "volume 70\n");
  CHECK(read(nucleus / "plugin-approvals.json") == "{\"wolf\":1}");
  CHECK(read(nucleus / "User/Slippi/user.json") == "{\"uid\":\"n\"}");
  CHECK(read(nucleus / disc_memory::kForwardMarker).find(netlee.u8string()) != std::string::npos);
  CHECK(!fs::exists(nucleus / "User/GC"));
  CHECK(read(netlee / "port-settings.ini") == "volume 70\n" && read(netlee / "User/GC/CardA/GALE01.gci") == "netlee save" &&
        read(netlee / disc_memory::kForwardMarker).find("MeleeUnlocked") != std::string::npos);
  CHECK(disc_memory::first_verified(disc_memory::places(renamed), decode, verify, unmodified) == good);
  CHECK(disc_memory::copy_forward(renamed).empty());   // once
  // No Netlee profile (an empty folder from a start that saved nothing): Melee Unlocked's, as before.
  const fs::path fresh = root / "Fresh";
  fs::create_directories(fresh / "Netlee", ec);
  write(fresh / "MeleeUnlocked/port-settings.ini", "volume 10\n");
  CHECK(disc_memory::copy_forward(fresh) == fresh / "MeleeUnlocked");
  CHECK(read(fresh / "Nucleus Client/port-settings.ini") == "volume 10\n");
  // Nothing to copy from: nothing happens.
  CHECK(disc_memory::copy_forward(root / "Empty").empty());
  CHECK(!fs::exists(root / "Empty"));

  // A no-argument start's command line (standalone_launch_win.h run): the .bat's, in this program's
  // data folder, and never --vanilla-only (a plain start is the Nucleus game since 2026-11-25).
  {
    const fs::path exe = root / "Game", data = disc_memory::standalone_dir(local);
    const auto args = disc_memory::plain_start_args(exe / "melee_source.exe", exe, data, good, {data / "Replays", false});
    const auto after = [&](const wchar_t* option) {
      for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == option) return fs::path(args[i + 1]);
      return fs::path();
    };
    CHECK(args.size() == 18 && args.front() == (exe / "melee_source.exe").wstring());
    CHECK(std::find(args.begin(), args.end(), L"--replay-month-folders") == args.end());
    CHECK(std::find(args.begin(), args.end(), L"--vanilla-only") == args.end());
    CHECK(std::find(args.begin(), args.end(), L"--threaded-renderer") != args.end());
    CHECK(after(L"--iso") == good);
    CHECK(after(L"--settings-path") == data / "port-settings.ini");
    CHECK(after(L"--sys-dir") == exe / "Sys");
    CHECK(after(L"--user-dir") == data / "User" / "Slippi");
    CHECK(after(L"--replay-dir") == data / "Replays");
    CHECK(after(L"--card-dir") == data / "User" / "GC" / "CardA");
    CHECK(after(L"--log-file") == data / "melee.log");
    CHECK(after(L"--shader-cache") == data / "shadercache");
    // The Slippi folder (replay_folder below), with stock Slippi's monthly subfolders when they are on.
    const fs::path slippi = root / "Docs" / "Slippi";
    const auto monthly = disc_memory::plain_start_args(exe / "melee_source.exe", exe, data, good, {slippi, true});
    CHECK(monthly.size() == 19 && monthly.back() == L"--replay-month-folders");
    for (size_t i = 0; i + 1 < monthly.size(); ++i)
      if (monthly[i] == L"--replay-dir") CHECK(fs::path(monthly[i + 1]) == slippi);
    const auto flat = disc_memory::plain_start_args(exe / "melee_source.exe", exe, data, good, {slippi, false});
    CHECK(flat.size() == 18 && std::find(flat.begin(), flat.end(), slippi.wstring()) != flat.end());
  }
  // A plain start's replays go where the player's replay tracker looks, as the Nucleus app's card does:
  // the Slippi Launcher's folder (settings.rootSlpPath) when it exists, else Documents\Slippi when it
  // exists, else this program's own Replays. The Launcher's file is only read, a missing or malformed
  // one is no setting, and no Slippi folder is ever created.
  {
    const fs::path base = root / "ReplayFolder", data = base / "Local" / "Nucleus Client";
    const fs::path docs = base / "Documents", settings = base / "Roaming" / "Slippi Launcher" / "Settings";
    const fs::path chosen_dir = base / "Replays Elsewhere" / "Slippi";
    const auto json_path = [](const fs::path& p) {   // a path as a JSON string's contents
      std::string out;
      for (const char c : p.u8string()) { if (c == '\\' || c == '"') out += '\\'; out += c; }
      return out;
    };
    // Missing file, no Documents\Slippi: this program's own folder, flat; nothing created.
    auto found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == data / "Replays" && !found.month_folders);
    CHECK(!fs::exists(docs / "Slippi") && !fs::exists(settings.parent_path()));
    CHECK(disc_memory::launcher_replays({}).root.empty());
    // Present with a path that exists, monthly subfolders on: that folder, monthly; the file unchanged.
    fs::create_directories(chosen_dir);
    const std::string with_path = "{\"settings\":{\"isoPath\":\"C:\\\\Games\\\\melee.iso\",\"rootSlpPath\":\"" +
                                  json_path(chosen_dir) + "\",\"useMonthlySubfolders\":true,\"spectateSlpPath\":\"x\"},"
                                  "\"accounts\":{\"list\":[]}}";
    write(settings, with_path);
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == chosen_dir && found.month_folders);
    CHECK(read(settings) == with_path);
    CHECK(disc_memory::launcher_replays(settings).root == chosen_dir && disc_memory::launcher_replays(settings).monthly);
    // The same with monthly subfolders off (or not a boolean): flat.
    write(settings, "{\"settings\":{\"rootSlpPath\":\"" + json_path(chosen_dir) + "\",\"useMonthlySubfolders\":false}}");
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == chosen_dir && !found.month_folders);
    write(settings, "{\"settings\":{\"rootSlpPath\":\"" + json_path(chosen_dir) + "\",\"useMonthlySubfolders\":\"yes\"}}");
    CHECK(!disc_memory::replay_folder(settings, docs, data).month_folders);
    // Its folder gone: Documents\Slippi when that exists (with the Launcher's monthly setting), else
    // ours; the Launcher's folder is not made again.
    write(settings, "{\"settings\":{\"rootSlpPath\":\"" + json_path(base / "Gone" / "Slippi") + "\",\"useMonthlySubfolders\":true}}");
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == data / "Replays" && !found.month_folders && !fs::exists(base / "Gone"));
    fs::create_directories(docs / "Slippi");
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == docs / "Slippi" && found.month_folders);
    // Present without a path (an older Launcher, or the setting left at its default): Documents\Slippi.
    write(settings, "{\"settings\":{\"isoPath\":null,\"launchMeleeOnPlay\":true},\"appVersion\":\"2.11.0\"}");
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == docs / "Slippi" && !found.month_folders);
    write(settings, "{\"settings\":{\"rootSlpPath\":\"\"}}");
    CHECK(disc_memory::replay_folder(settings, docs, data).dir == docs / "Slippi");
    write(settings, "{\"settings\":{\"rootSlpPath\":\"relative\\\\Slippi\"}}");   // not absolute: no setting
    CHECK(disc_memory::launcher_replays(settings).root.empty());
    // Malformed: cut short, not JSON, the wrong shapes, empty. No setting, never a failure.
    for (const std::string& bad : {std::string("{\"settings\":{\"rootSlpPath\":\"") + json_path(chosen_dir),
                                   std::string("not json at all"), std::string("[1,2,3]"),
                                   std::string("{\"settings\":\"") + json_path(chosen_dir) + "\"}",
                                   std::string("{\"settings\":{\"rootSlpPath\":42,\"useMonthlySubfolders\":1}}"),
                                   std::string("{\"settings\":{\"rootSlpPath\":\"\\ud800\"}}"), std::string()}) {
      write(settings, bad);
      found = disc_memory::replay_folder(settings, docs, data);
      CHECK(found.dir == docs / "Slippi" && !found.month_folders);
      CHECK(read(settings) == bad);
    }
    // Neither the Launcher's folder nor Documents\Slippi: ours again.
    fs::remove_all(docs / "Slippi", ec);
    found = disc_memory::replay_folder(settings, docs, data);
    CHECK(found.dir == data / "Replays" && !found.month_folders && !fs::exists(docs / "Slippi"));
    CHECK(disc_memory::replay_folder(settings, {}, data).dir == data / "Replays");
  }
  // The saves of the vanilla-only plain starts before (the card's VanillaOnly folder) come into the
  // card a plain start uses now while it has none: copied, never overwritten, and not again.
  {
    const fs::path card = root / "Carry" / "User" / "GC" / "CardA";
    CHECK(disc_memory::carry_vanilla_saves(card) == 0 && !fs::exists(card));
    write(card / "VanillaOnly" / "GALE01-ab.gci", "vanilla save");
    write(card / "VanillaOnly" / "notes.txt", "x");
    CHECK(disc_memory::carry_vanilla_saves(card) == 1);
    CHECK(read(card / "GALE01-ab.gci") == "vanilla save" && read(card / "VanillaOnly" / "GALE01-ab.gci") == "vanilla save" &&
          !fs::exists(card / "notes.txt"));
    write(card / "VanillaOnly" / "GALE01-cd.gci", "later");
    write(card / "GALE01-ab.gci", "played on");
    CHECK(disc_memory::carry_vanilla_saves(card) == 0 && read(card / "GALE01-ab.gci") == "played on" &&
          !fs::exists(card / "GALE01-cd.gci"));
  }

  fs::remove_all(root, ec);
  if (failures) return 1;
  std::printf("disc_memory: ok\n");
  return 0;
}
