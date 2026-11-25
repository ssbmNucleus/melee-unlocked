#!/usr/bin/env python3
# Checks the online rumble path from a tools/p2p_pair.py run made with MELEE_TRACE_ONLINE_SFX=1.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads p1/log.txt and p2/log.txt of a p2p_pair.py output folder and checks what the game asked the
motor to do online (sourceport/game/shim/mu_online_audio.c, the "[online-motor]" trace) against
Melee's Options > Rumble for the local player and the Rumble switch in Controls.

    set MELEE_TRACE_ONLINE_SFX=1
    python tools/p2p_pair.py --chars 26:27 --out pm-test/p2p/motor
    python tools/online_motor_check.py pm-test/p2p/motor --option 1 --controls off

    set MELEE_TEST_GAME_RUMBLE=0       (answers as a card with Options > Rumble off would)
    python tools/p2p_pair.py --chars 26:27 --out pm-test/p2p/motor-off
    python tools/online_motor_check.py pm-test/p2p/motor-off --option 0 --controls off

PASS needs, on each side: at least one trace line where the game asked for the motor; every line's
option matching --option; want = asked and option on every line, so the motor is only wanted when
the player's own option allows it; and for the host: with --option 0 no motor start reached it at
all, with --option 1 and --controls off its first start was refused ("Rumble is off in Controls").
"""
import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[online-motor\] frame=(-?\d+) want=(\d) on=(\d) resim=(\d) asked=(\d) option=(\d)")
REFUSED = "rumble: local player0 -> nowhere (Rumble is off in Controls)"
REACHED = re.compile(r"rumble: local player0 -> (.*)")


def check_side(log, option, controls):
    text = log.read_text(encoding="utf-8", errors="replace")
    lines = [tuple(int(v) for v in m.groups()) for m in LINE.finditer(text)]
    problems = []
    if not lines:
        return 0, 0, ["no [online-motor] lines (was MELEE_TRACE_ONLINE_SFX=1 set?)"]
    asked = [l for l in lines if l[4] == 1]
    if not asked:
        problems.append("the game never asked for the motor (no asked=1 line)")
    for frame, want, _on, _resim, ask, opt in lines:
        if opt != option:
            problems.append(f"frame {frame}: option={opt}, expected {option}")
            break
        if want != (ask and opt):
            problems.append(f"frame {frame}: want={want} with asked={ask} option={opt}")
            break
    wanted = [l for l in lines if l[1] == 1 and l[3] == 0]
    reached = REACHED.findall(text)
    if option == 0:
        if wanted:
            problems.append(f"{len(wanted)} motor starts wanted with the option off")
        if reached:
            problems.append(f"a motor start reached the host with the option off: {reached[0]}")
    elif wanted:
        if not reached:
            problems.append("the motor was wanted but no start reached the host")
        elif controls == "off" and REFUSED not in text:
            problems.append(f"Rumble is off in Controls but the host sent the first start to {reached[0]}")
    return len(lines), len(asked), problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path, help="the p2p_pair.py --out folder")
    ap.add_argument("--option", type=int, choices=(0, 1), required=True,
                    help="Melee's Options > Rumble for the local player in that run (0 with MELEE_TEST_GAME_RUMBLE=0)")
    ap.add_argument("--controls", choices=("off", "on"), default="off",
                    help="the Rumble switch in Controls for that run (off unless the run loaded settings with rumble2 1)")
    args = ap.parse_args(argv)
    ok = True
    for side in ("p1", "p2"):
        log = args.out / side / "log.txt"
        if not log.is_file():
            print(f"{side}: no {log}")
            ok = False
            continue
        count, asked, problems = check_side(log, args.option, args.controls)
        print(f"{side}: {count} trace lines, {asked} asked for the motor{'' if problems else ', ok'}")
        for problem in problems:
            print(f"  {problem}")
        ok = ok and not problems
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
