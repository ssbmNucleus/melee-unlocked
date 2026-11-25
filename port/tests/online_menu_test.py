"""Execute the production menu input/entry functions with offline scene stubs."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest

import fixture_compiler

ROOT = Path(__file__).resolve().parents[2]
# Every test here, and every test that imports this module, compiles a C++ fixture: without a
# compiler the whole test is skipped (CTest SKIP, exit 77) instead of failing.
_CXX = fixture_compiler.require_cxx()


def compiler_command():
    # On Windows the command force-includes posix_fixture_compat.h (setenv/unsetenv).
    return list(_CXX)
spec = importlib.util.spec_from_file_location('fixture', ROOT / 'tools/native_major_fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def menu_functions(*names):
    """Extract function bodies lazily so unrelated fixtures can reuse the compiler helper."""
    source = (ROOT / "sourceport/game/shim/mu_slippi_menu.c").read_text()
    functions = "\n".join(fixture.extract(source, name) for name in names)
    if "mu_slippi_online_menu_think" in names:
        functions = navigation_functions("mu_mn_move_selection") + functions
    return functions


def navigation_functions(*names):
    source = (ROOT / "sourceport/extern/melee/src/melee/mn/mnmain.c").read_text()
    return "\n".join(fixture.extract(source, name) for name in names)


STUBS = r"""
#include <cassert>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
using u8 = unsigned char;
using u16 = unsigned short;
using s8 = signed char;
using s32 = int;
using u32 = unsigned;
using MenuKind = int;
enum { MU_SLP_MODE_RANKED, MU_SLP_MODE_UNRANKED, MU_SLP_MODE_DIRECT,
       MU_SLP_MODE_TEAMS, MU_SLP_MODE_PARTY };
enum { MU_SLP_OPT_LOGIN=5, MU_SLP_OPT_LOGOUT, MU_SLP_OPT_UPDATE };
enum { MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_LOGGED_IN, MU_SLP_APP_UPDATE };
enum { MENU_KIND_8 = 8, MENU_KIND_1P = 1, SEL_1P_2 = 2, GM_HANYU_CSS = 8,
       MU_SLP_WINNER_NULL = -1, MenuInput_Confirm = 1, MenuInput_Back = 2,
       MenuInput_Up = 4, MenuInput_Down = 8, MU_CMD_OPEN_LOGIN = 0xb6, MU_CMD_UPDATE_APP = 0xb8 };
struct HSD_GObj {};
struct MenuExitData { int pending_mode; } exit_data;
struct { int app_state, mode, is_winner, chose_stage; } slp;
struct { int cooldown; } mn_804D6BC8;
struct { int cur_menu, hovered_selection, entering_menu, buttons; } mn_804A04F0;
// The menu table, declared once here in the shape of mn/types.h so every fixture that imports
// these stubs (slippi_title_test too) shares it instead of declaring a second, clashing copy.
struct AnimLoopSettings { int row; };
struct MenuKindData { AnimLoopSettings* anim_loop; float start_frame; u16* description_indices;
                      u8 selection_count; void (*think)(HSD_GObj*); } mn_803EB6B0[9];
static unsigned network_policy, pressed, command;
static int p2p_auto_direct, entered, content, back;
static int row_count = 8, unlocked_mask = -1, lock_checks;
int mu_slippi_option_unlocked(int);
int mn_80229624(int) { lock_checks = 0; return pressed; }
int mn_80229938(int, int row) {
    assert(row >= 0 && row < row_count);
    assert(++lock_checks <= row_count);
    return unlocked_mask < 0 ? mu_slippi_option_unlocked(row) : (unlocked_mask >> row) & 1;
}
void mn_80229894(int, int, int) { ++back; }
int mu_online_p2p_menus() { return 0; }
int mu_lobby_menu_enter() { return 0; }   // the lobbies submenu off: Teams enters as retail (shim/mu_lobby_list.c)
int mu_online_p2p_port() { return 0; }
int confirm_port() { return 0; }
void gm_801677E8(s8) {}
void gm_801A4B60() { ++entered; }
void mu_content_mode(unsigned mode) { content = mode; }
MenuExitData* gm_GetCurrentSceneExitData() { return &exit_data; }
void mu_slippi_cmd(int cmd, void*, int, void*, int, void*) { command = cmd; }
void sfxForward() {} void sfxBack() {} void sfxMove() {}
#define OSReport(...) ((void)0)
"""


class OnlineMenuTest(unittest.TestCase):
    def compile_fixture(self, name, body, *defines):
        with tempfile.TemporaryDirectory(prefix=name + "-") as directory:
            cpp, exe = Path(directory) / "fixture.cpp", Path(directory) / "fixture"
            cpp.write_text(body)
            subprocess.run([*compiler_command(), "-std=c++17", *defines,
                            "-I", str(ROOT / "port/runtime/hle"), "-I", str(ROOT / "port/third_party"),
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_locked_entry_and_repeated_navigation(self):
        production = menu_functions(
            "mu_slippi_menu_count", "mu_slippi_option_unlocked",
            "mu_slippi_first_unlocked", "mu_slippi_online_menu_think")
        self.compile_fixture("locked-online-menu", STUBS + production + r"""
int main() {
  mn_803EB6B0[MENU_KIND_8].selection_count = 8;
  for (int policy : {0, 8, 24}) {
    network_policy = policy;
    for (int state : {MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_LOGGED_IN, MU_SLP_APP_UPDATE}) {
      slp.app_state = state;
      lock_checks = 0;
      const int first = mu_slippi_first_unlocked();
      // An agent copy (8) has no open row, except Direct with local peering (16) when it is not signed in.
      const bool empty = policy & 8 && !(policy & 16);
      if (policy == 24 && state != MU_SLP_APP_LOGGED_IN)
        for (int row = 0; row < 8; ++row) assert(bool(mu_slippi_option_unlocked(row)) == (row == MU_SLP_MODE_DIRECT));
      assert(first >= 0 && first < 8);
      assert(empty ? first == MU_SLP_MODE_RANKED : mu_slippi_option_unlocked(first));
      for (int start = 0; start < 8; ++start) {
        mn_804A04F0.hovered_selection = start;
        for (int press = 0; press < 100; ++press) {
          pressed = press % 2 ? MenuInput_Up : MenuInput_Down;
          mu_slippi_online_menu_think(nullptr);
          const int row = mn_804A04F0.hovered_selection;
          assert(empty ? row == start : mu_slippi_option_unlocked(row));
        }
      }
      if (empty) {
        command = entered = 0;
        pressed = MenuInput_Confirm;
        mu_slippi_online_menu_think(nullptr);
        assert(command == 0 && entered == 0);
        back = 0;
        pressed = MenuInput_Back;
        mu_slippi_online_menu_think(nullptr);
        assert(back == 1);
      }
    }
  }
}
""")

    def test_public_build_has_no_direct_exception(self):
        # NUCLEUS_PUBLIC_BUILD (docs/client-release.md): the agent copy's Direct row without a sign-in is
        # left out, so local peering opens nothing the sign-in would not; signed in, the modes still open.
        production = menu_functions("mu_slippi_menu_count", "mu_slippi_option_unlocked")
        self.compile_fixture("public-online-menu", STUBS + production + r"""
int main() {
  mn_803EB6B0[MENU_KIND_8].selection_count = 8;
  network_policy = 24;
  for (int state : {MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_UPDATE}) {
    slp.app_state = state;
    for (int row = 0; row < 8; ++row) assert(!mu_slippi_option_unlocked(row));
  }
  slp.app_state = MU_SLP_APP_LOGGED_IN;
  for (int row = 0; row < 8; ++row)
    assert(bool(mu_slippi_option_unlocked(row)) == (row >= MU_SLP_MODE_RANKED && row <= MU_SLP_MODE_PARTY));
}
""", "-DNUCLEUS_PUBLIC_BUILD=1")

    def test_shared_navigation_preserves_selection_and_wraps(self):
        production = menu_functions("mu_slippi_menu_count", "mu_slippi_option_unlocked")
        production += navigation_functions("mu_mn_move_selection", "x2_dec", "x2_inc",
                                           "decrement_selection", "increment_selection")
        self.compile_fixture("menu-navigation", STUBS + production + r"""
int main() {
  for (int count : {0, 1, 3, 8}) {
    row_count = count;
    for (int mask = 0; mask < (1 << count); ++mask) {
      unlocked_mask = mask;
      for (int start = 0; start <= count; ++start) {
        for (int direction : {-1, 1}) {
          int expected = start < count ? start : 0;
          if (mask) {
            do { expected = (expected + count + direction) % count; }
            while (!(mask & (1 << expected)));
          }
          for (int wrapper = 0; wrapper < 3; ++wrapper) {
            mn_804A04F0.hovered_selection = start;
            mn_804A04F0.cur_menu = 8;
            lock_checks = 0;
            if (wrapper == 0) mu_mn_move_selection(8, count, direction);
            else if (wrapper == 1) {
              if (direction < 0) x2_dec(count); else x2_inc(count);
            } else {
              if (direction < 0) decrement_selection(count, 8); else increment_selection(count, 8);
            }
            assert(mn_804A04F0.hovered_selection == expected);
          }
        }
      }
    }
  }
}
""", "-DMU_NATIVE=1")

    def test_slippi_mode_selection_account_states_and_navigation(self):
        production = menu_functions(
            "mu_slippi_menu_count", "mu_slippi_option_unlocked",
            "mu_slippi_ranked_available", "mu_slippi_online_menu_think")
        self.compile_fixture("online-menu", STUBS + production + r"""
int main() {
  assert(mu_slippi_menu_count() == 8);
  assert(!mu_slippi_option_unlocked(-1) && !mu_slippi_option_unlocked(8));
  pressed = MenuInput_Confirm;
  for (int state : {MU_SLP_APP_LOGGED_OUT, MU_SLP_APP_UPDATE}) {
    slp.app_state = state;
    int allowed = state == MU_SLP_APP_UPDATE ? MU_SLP_OPT_UPDATE : MU_SLP_OPT_LOGIN;
    assert(!mu_slippi_ranked_available());
    for (int row = 0; row < 8; ++row) {
      assert(bool(mu_slippi_option_unlocked(row)) == (row == allowed));
      command = 0; mn_804A04F0.hovered_selection = row;
      mu_slippi_online_menu_think(nullptr);
      assert(entered == 0);
      assert(command == (row != allowed ? 0 : state == MU_SLP_APP_UPDATE ? MU_CMD_UPDATE_APP : MU_CMD_OPEN_LOGIN));
    }
  }
  slp.app_state = MU_SLP_APP_LOGGED_IN;
  assert(mu_slippi_ranked_available());
  for (int row = 0; row <= MU_SLP_MODE_PARTY; ++row) {
    assert(mu_slippi_option_unlocked(row));
    mn_804A04F0.hovered_selection = row;
    mu_slippi_online_menu_think(nullptr);
    assert(slp.mode == row && content == row && entered == row + 1);
    assert(exit_data.pending_mode == GM_HANYU_CSS);
  }
  pressed = MenuInput_Down; mu_slippi_online_menu_think(nullptr);
  assert(mn_804A04F0.hovered_selection == MU_SLP_MODE_RANKED);
  pressed = MenuInput_Up; mu_slippi_online_menu_think(nullptr);
  assert(mn_804A04F0.hovered_selection == MU_SLP_MODE_PARTY);
  pressed = MenuInput_Back; mu_slippi_online_menu_think(nullptr);
  assert(back == 1);
}
""")


if __name__ == "__main__":
    unittest.main()
