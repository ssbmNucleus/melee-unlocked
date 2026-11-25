"""X / Y on the online Teams select reach the fighter's installed skins (mn/mncharsel.c).

The online select is the one-player layout (mnCharSel_804D6CF5 1). mnCharSel_CursorThink's branch for
that layout handles the door's presses itself, every frame, once the token is down; it used to call
mnCharSel_CostumeChange(0, trigger), which refuses every change in online Teams, so no skin could be
picked there. It now calls mu_css_one_player_costume_press(trigger).

Two tests, neither of which runs mnCharSel_CursorThink (the whole select screen; the proof in the game is
a scripted run, docs/guide/areas/character-skins.md):
- a unit test: the production mu_css_one_player_costume_press, the team's press, the Teams test and the
  door's skin check, compiled against stubs and run for each kind of select;
- a routing test: the source of mnCharSel_CursorThink's one-player branch calls that function where it
  called mnCharSel_CostumeChange, under MU_NATIVE, behind the token-down check, and nothing else does."""
from pathlib import Path
import importlib.util
import re
import unittest

import online_menu_test as menu

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sourceport/extern/melee/src/melee/mn/mncharsel.c"
spec = importlib.util.spec_from_file_location("fixture", ROOT / "tools/native_major_fixture.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

STUBS = r"""
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
using u8 = unsigned char;
using s8 = signed char;
using s32 = int;
using u32 = unsigned;
enum { HSD_PAD_R = 0x20, HSD_PAD_L = 0x40, HSD_PAD_A = 0x100, HSD_PAD_X = 0x400, HSD_PAD_Y = 0x800, CSS_NONE = 26 };
struct CSSDoor { u8 p_kind, sel_icon, costume; };
struct { CSSDoor doors[4]; } mnCharSel_803F0DFC;
struct { int char_kind; } icons[CSS_NONE];
struct CSSData { struct { struct { struct { u8 is_teams; } rules; struct { u8 color; } players[4]; } start; } vs; };
CSSData css_data;
CSSData* mnCharSel_804D6CB0 = &css_data;
u8 mnCharSel_804D6CF5;   // 1: the one-player layout (the online select), 4: the VS screen
struct { s8 ckind; u8 skin, costume; } mu_door_slot[4];
int online, teams_mode, local_ready, skinnable = 1, skin_count = 2;
int skin_made_for[3] = { -1, 0, 4 };   // skin 1 for costume 0, skin 2 for costume 4
int ring_calls, ring_teams, change_calls, change_door;
u32 change_input;
int mu_slippi_css_online(void) { return online; }
int mu_slippi_css_teams(void) { return online && teams_mode; }
int mu_slippi_css_local_ready(void) { return local_ready; }
static int mu_css_door_skinnable(int) { return skinnable; }
int mu_css_skins_count(int) { return skin_count; }
int mu_css_skins_costume(int, int k) { return k >= 1 && k <= 2 ? skin_made_for[k] : -1; }
static int mu_css_skins_teams(void);
static int mu_css_skins_ring(int, u32) { ++ring_calls; ring_teams = mu_css_skins_teams(); return 1; }
// The retail change, which refuses everything in online Teams (mu_slippi_css_block_costume_change).
void mnCharSel_CostumeChange(int door, u32 input) { ++change_calls; change_door = door; change_input = input; }
"""

MAIN = r"""
static void screen(int layout, int is_online, int teams, int is_teams)
{
    mnCharSel_804D6CF5 = (u8) layout;
    online = is_online;
    teams_mode = teams;
    css_data.vs.start.rules.is_teams = (u8) is_teams;
    local_ready = 0;
    skin_count = 2;
    ring_calls = ring_teams = change_calls = 0;
    change_door = -1;
    change_input = 0;
    for (int i = 0; i < 4; i++) {
        mnCharSel_803F0DFC.doors[i] = CSSDoor{ 0, 2, 0 };
    }
    icons[2].char_kind = 2;
}

int main()
{
    // Online Teams select: Y and X step the skin ring as Teams; the retail change is never called.
    screen(1, 1, 1, 0);
    mu_css_one_player_costume_press(HSD_PAD_Y);
    CHECK(ring_calls == 1 && ring_teams == 1 && change_calls == 0);
    screen(1, 1, 1, 0);
    mu_css_one_player_costume_press(HSD_PAD_X);
    CHECK(ring_calls == 1 && ring_teams == 1 && change_calls == 0);
    // ... A, L, R or nothing pressed: nothing; locked in: nothing; a fighter without skins: nothing.
    const u32 others[] = { 0u, (u32) HSD_PAD_A, (u32) HSD_PAD_L, (u32) HSD_PAD_R };
    for (u32 other : others) {
        screen(1, 1, 1, 0);
        mu_css_one_player_costume_press(other);
        CHECK(ring_calls == 0 && change_calls == 0);
    }
    screen(1, 1, 1, 0);
    local_ready = 1;
    mu_css_one_player_costume_press(HSD_PAD_Y);
    CHECK(ring_calls == 0 && change_calls == 0);
    screen(1, 1, 1, 0);
    skin_count = 0;
    mu_css_one_player_costume_press(HSD_PAD_Y);
    CHECK(ring_calls == 0 && change_calls == 0);
    // A later game's select, rules.is_teams left at 1 by the stage pick: still the Teams ring.
    screen(1, 1, 1, 1);
    mu_css_one_player_costume_press(HSD_PAD_Y);
    CHECK(ring_calls == 1 && ring_teams == 1 && change_calls == 0);
    // Online Direct: the retail change of door 0 with the press as it came (it reaches the ring itself).
    screen(1, 1, 0, 0);
    mu_css_one_player_costume_press(HSD_PAD_Y);
    CHECK(change_calls == 1 && change_door == 0 && change_input == HSD_PAD_Y && ring_calls == 0);
    CHECK(mu_css_skins_teams() == 0);
    // An offline one-player select (Classic, Training ...): the retail change, as before.
    screen(1, 0, 0, 0);
    mu_css_one_player_costume_press(HSD_PAD_X);
    CHECK(change_calls == 1 && change_door == 0 && change_input == HSD_PAD_X && ring_calls == 0);
    CHECK(mu_css_skins_teams() == 0);

    // The VS screen's Teams press is unchanged: a skin made for the team's color goes through the
    // costume change, none: nothing.
    screen(4, 0, 0, 1);
    css_data.vs.start.players[1].color = 4;
    mu_css_skins_team_press(1, HSD_PAD_Y);
    CHECK(change_calls == 1 && ring_calls == 0 && mu_css_skins_teams() == 1);
    screen(4, 0, 0, 1);
    css_data.vs.start.players[1].color = 2;
    mu_css_skins_team_press(1, HSD_PAD_Y);
    CHECK(change_calls == 0 && ring_calls == 0);

    // The door keeps a skin made for another color on the online Teams select (worn in the team's
    // color), but not on the online Direct select, and never on another fighter.
    screen(1, 1, 1, 0);
    mnCharSel_803F0DFC.doors[0].costume = 4;   // the team's costume
    mu_door_slot[0].ckind = 2;
    mu_door_slot[0].skin = 1;
    mu_door_slot[0].costume = 0;               // skin 1 was made for costume 0
    CHECK(mu_css_door_skin(0) == 1);
    screen(1, 1, 0, 0);
    mnCharSel_803F0DFC.doors[0].costume = 4;
    CHECK(mu_css_door_skin(0) == 0);
    mnCharSel_803F0DFC.doors[0].costume = 0;
    CHECK(mu_css_door_skin(0) == 1);
    screen(1, 1, 1, 0);
    icons[2].char_kind = 5;
    CHECK(mu_css_door_skin(0) == 0);
    std::puts("css teams skins: ok");
    return 0;
}
"""


def function_body(source, name):
    """The text of function `name`'s definition in `source`, through its closing brace."""
    start = re.search(r"^void %s\(" % re.escape(name), source, re.M)
    assert start, name
    end = re.search(r"^}", source[start.start():], re.M)
    return source[start.start():start.start() + end.end()]


class CssTeamsSkinsTest(unittest.TestCase):
    def test_unit_one_player_press_and_teams_ring(self):
        """Unit test of the press routing and the ring's Teams test (stubs; not the select screen)."""
        source = SOURCE.read_text(encoding="utf-8")
        production = "".join(fixture.extract(source, name) for name in (
            "mu_css_skins_teams", "mu_css_door_skin", "mu_css_skins_team_press",
            "mu_css_one_player_costume_press"))
        menu.OnlineMenuTest().compile_fixture("css-teams-skins", STUBS + production + MAIN)

    def test_routing_cursor_think_one_player_branch(self):
        """The select screen's one-player branch is where the online select's X / Y arrive."""
        source = SOURCE.read_text(encoding="utf-8")
        think = function_body(source, "mnCharSel_CursorThink")
        self.assertRegex(
            think,
            r"if \(mnCharSel_804D6CF5 == 1\) \{\s+"
            r"if \(mnCharSel_804A0BD0\[cursor->x4\]->x5 == 0\) \{\s+"
            r"#ifdef MU_NATIVE\s+mu_css_one_player_costume_press\(trigger\);\s+"
            r"#else\s+mnCharSel_CostumeChange\(0, trigger\);\s+#endif\s+\}")
        # Under MU_NATIVE no other path of the select screen sends door 0's press to the retail change.
        native = re.sub(r"#else\s+mnCharSel_CostumeChange\(0, trigger\);\s+#endif", "#endif", think)
        self.assertNotIn("mnCharSel_CostumeChange(0, trigger)", native)
        # Called from there only, and every Teams test of the skin ring goes through the one helper.
        self.assertEqual(source.count("mu_css_one_player_costume_press(trigger);"), 1)
        self.assertEqual(len(re.findall(r"teams = mu_css_skins_teams\(\);", source)), 2)


if __name__ == "__main__":
    unittest.main()
