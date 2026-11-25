"""The native Slippi menu uses the game's built-in row labels and help text."""
import unittest
import online_menu_test as menu


class TitleTest(unittest.TestCase):
    def test_original_rows_and_help_text(self):
        # Exercise the actual menu-table installation in the game's mnmain.c.
        mnmain = (menu.ROOT / "sourceport/extern/melee/src/melee/mn/mnmain.c").read_text(
            encoding="utf-8", errors="surrogateescape")
        install = menu.fixture.extract(mnmain, "mu_mnmain_install_online_menu")
        # The menu table mn_803EB6B0 and AnimLoopSettings come from menu.STUBS (one shared shape).
        menu.OnlineMenuTest().compile_fixture("slippi-row-labels", menu.STUBS + r"""
AnimLoopSettings mn_803EB57C[] = {{0},{1},{2}}, mn_803EB5A0[] = {{3},{4},{5}},
                 mn_803EB5C4[] = {{6},{7}}, mn_803EB444[3]{};
u16 mn_803EB66C[3];
void mu_slippi_log_online_rows() {}
void mu_slippi_online_menu_think(HSD_GObj*) {}
int mu_slippi_option_unlocked(int row) { return row >= 0 && row < 8; }
""" + install + r"""
int main() {
  const u16 help[] = {0x645,0x646,0x647,0x64b,0x64c,0x648,0x649,0x64a};
  mu_mnmain_install_online_menu();
  auto& online = mn_803EB6B0[MENU_KIND_8];
  assert(online.selection_count == 8 && online.think == mu_slippi_online_menu_think);
  for (int i = 0; i < online.selection_count; ++i) {
    assert(online.anim_loop[i].row == i);
    assert(online.description_indices[i] == help[i]);
  }
}
""")


if __name__ == "__main__":
    unittest.main()
