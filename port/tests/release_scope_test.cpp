// The Mods categories' release switches (host/release_scope.h), the host's half: the table's
// defaults (Characters and Stages on, the other four coming soon), each settings line turning its
// category on and being written back, an automated run's MELEE_TEST_MODS_SCOPE, the byte the game's
// menu gets (MU_CMD_MOD_BROWSER 8), and Fighters off advertising nothing online (no custom-fighters
// capability bit in SendBuild, no content list). The game's half (the greyed row reading "Coming
// soon."): sourceport/game/tests/mods_release.c.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gx/pc_settings.h"
#include "host/host.h"
#include "host/online_fighters.h"
#include "host/public_build.h"
#include "host/release_scope.h"
#include "host/test_env.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <process.h>
#include <string>

namespace {
int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace rs = host::release_scope;

std::filesystem::path g_path;

// Loads a settings file holding `text`; returns the mask the menu would get.
unsigned load(const std::string& text) {
  { std::ofstream f(g_path, std::ios::binary | std::ios::trunc); f << text; }
  gx::RenderOptions options;
  options.settings_path = g_path.string();
  int volume = 0;
  gx::load_pc_settings(options, volume);
  return rs::mask();
}

std::string saved_text() {
  gx::RenderOptions options;
  options.settings_path = g_path.string();
  int volume = 0;
  gx::load_pc_settings(options, volume);
  CHECK(gx::save_pc_settings(options, volume));
  std::ifstream f(g_path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void set_scope(const char* v) { _putenv_s("MELEE_TEST_MODS_SCOPE", v ? v : ""); }
}  // namespace

int main() {
  host::options.quiet = true;   // gate() logs; no log file from a test
  host::test_env::automated = false;   // a player's run
  set_scope(nullptr);
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("mu_release_scope_test_" + std::to_string(_getpid()));
  std::filesystem::create_directories(dir);
  g_path = dir / "settings.txt";

  // The table: six categories in the game's order, Characters and Stages always on, the other four
  // coming soon unless their settings line says otherwise.
  CHECK(std::string(rs::kTable[rs::kCharacters].name) == "Characters" && rs::kTable[rs::kCharacters].setting == nullptr);
  CHECK(std::string(rs::kTable[rs::kStages].name) == "Stages" && rs::kTable[rs::kStages].setting == nullptr);
  CHECK(std::string(rs::kTable[rs::kParticles].setting) == "modsparticles" && !rs::kTable[rs::kParticles].on_by_default);
  CHECK(std::string(rs::kTable[rs::kFighters].setting) == "modsfighters" && !rs::kTable[rs::kFighters].on_by_default);
  CHECK(std::string(rs::kTable[rs::kNewStages].setting) == "modsnewstages" && !rs::kTable[rs::kNewStages].on_by_default);
  CHECK(std::string(rs::kTable[rs::kMenus].setting) == "modsmenus" && !rs::kTable[rs::kMenus].on_by_default);
  rs::reset_settings();
  CHECK(rs::mask() == 0x03);
  CHECK(rs::on(rs::kCharacters) && rs::on(rs::kStages));
  CHECK(!rs::on(rs::kParticles) && !rs::on(rs::kFighters) && !rs::on(rs::kNewStages) && !rs::on(rs::kMenus));
  CHECK(!rs::gate(rs::kFighters, "test") && rs::gate(rs::kCharacters, "test"));
  CHECK(!rs::on(-1) && !rs::on(rs::kCategories));
  CHECK(rs::describe() == "Characters, Stages on; Particles, Fighters, New Stages, Menus coming soon");

  // A file without the lines: the defaults; one without "1": still off. Each line turns its own on,
  // "key 1" or "key=1".
  CHECK(load("vsync 1\n") == 0x03);
  CHECK(load("modsparticles 0\nmodsfighters\nmodsmenus 2\n") == 0x03);
  const struct { const char* line; unsigned bit; } lines[] = {
      {"modsparticles", 1u << rs::kParticles}, {"modsfighters", 1u << rs::kFighters},
      {"modsnewstages", 1u << rs::kNewStages}, {"modsmenus", 1u << rs::kMenus}};
  for (const auto& l : lines) {
    CHECK(load(std::string("vsync 1\n") + l.line + " 1\n") == (0x03u | l.bit));
    CHECK(load(std::string(l.line) + "=1\r\n") == (0x03u | l.bit));
    // Written back while on, so it stays on at the next launch.
    load(std::string(l.line) + " 1\n");
    const std::string text = saved_text();
    CHECK(text.find(std::string("\n") + l.line + " 1") != std::string::npos);
    CHECK(rs::mask() == (0x03u | l.bit));
  }
  CHECK(load("modsparticles 1\nmodsfighters 1\nmodsnewstages 1\nmodsmenus 1\n") == 0x3F);
  // A file read again without the line goes back to the defaults (nothing sticks from the last file).
  CHECK(load("vsync 1\n") == 0x03);
  {
    // The defaults are not written: the next release's own table applies to a file saved by this one.
    const std::string text = saved_text();
    CHECK(text.find("\nmodsparticles") == std::string::npos && text.find("\nmodsfighters") == std::string::npos);
    CHECK(text.find("\nmodsnewstages") == std::string::npos && text.find("\nmodsmenus") == std::string::npos);
  }

  // Fighters off (the release's default): no custom-fighters capability bit in SendBuild and no
  // content list (both read online_fighters::enabled()), so two release clients play as before.
  rs::reset_settings();
  CHECK(!host::online_fighters::enabled());
  rs::set_setting(rs::kFighters, true);   // "modsfighters 1"
  CHECK(host::online_fighters::enabled());
  rs::reset_settings();
  CHECK(!host::online_fighters::enabled());

  if (host::public_build::kOn) {
    // A public build: an automated run is a player's run (the table plus the file's lines), and
    // MELEE_TEST_MODS_SCOPE does nothing.
    host::test_env::automated = true;
    CHECK(rs::mask() == 0x03 && !host::online_fighters::enabled());
    set_scope("all");
    CHECK(rs::mask() == 0x03);
    set_scope("particles,fighters");
    CHECK(rs::mask() == 0x03 && !host::online_fighters::enabled());
    rs::set_setting(rs::kParticles, true);   // "modsparticles 1" still turns it on
    CHECK(rs::mask() == 0x07);
    rs::reset_settings();
    CHECK(rs::describe() == "Characters, Stages on; Particles, Fighters, New Stages, Menus coming soon");
    set_scope(nullptr);
    host::test_env::automated = false;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (g_failures == 0) std::printf("release scope (public build): all checks passed\n");
    return g_failures == 0 ? 0 : 1;
  }

  // An automated run (--hidden, --headless, --script): every category unless MELEE_TEST_MODS_SCOPE
  // says otherwise ("release": the table and the file's lines; a list adds categories; "all").
  host::test_env::automated = true;
  CHECK(rs::mask() == 0x3F && host::online_fighters::enabled());
  set_scope("release");
  CHECK(rs::mask() == 0x03 && !host::online_fighters::enabled());
  rs::set_setting(rs::kParticles, true);
  CHECK(rs::mask() == 0x07);
  rs::reset_settings();
  set_scope("particles");
  CHECK(rs::mask() == 0x07);
  set_scope("fighters,modsmenus");
  CHECK(rs::mask() == 0x2B && host::online_fighters::enabled());
  set_scope("all");
  CHECK(rs::mask() == 0x3F);
  set_scope(nullptr);
  host::test_env::automated = false;
  CHECK(rs::mask() == 0x03);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  if (g_failures == 0) std::printf("release scope: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
