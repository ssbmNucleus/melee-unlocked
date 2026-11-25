"""Two instances play one peer-to-peer rollback match on loopback and agree on every checksum.

    python port/tests/p2p_pair_test.py <path to melee_source.exe>

A wrapper around tools/p2p_pair.py (Fox against Marth on Final Destination, its default bot script):
PASS there needs the handshake, the match started, checksums compared with 0 mismatched, no desync or
crash, and the same descriptor and input transcript on both sides. This also reads its table and
fails on any mismatched count above 0. UDP ports: MU_TEST_BASE_PORT and the next one
(tools/run_tests.py sets it from this worktree's own range; default 41300).

Needs the player's own disc (MELEE_TEST_ISO; tools/run_tests.py finds it) and the game library beside
the program (or MELEE_GAME_DLL). Without either it says so and exits 77 (skipped).
"""
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = 77
FRAMES = 3600          # retraces each instance runs: the match and its end, at 60 a second
LIMIT = FRAMES / 40 + 120


def check(exe):
    exe = Path(exe).resolve()
    iso = os.environ.get("MELEE_TEST_ISO", "")
    dll = Path(os.environ.get("MELEE_GAME_DLL", str(exe.parent / ("melee_game.dll" if os.name == "nt" else "melee_game.so"))))
    if not iso or not Path(iso).is_file():
        print("skipped: MELEE_TEST_ISO does not name the disc")
        return SKIP
    if not dll.is_file():
        print(f"skipped: no game library at {dll} (copy it beside the program or set MELEE_GAME_DLL)")
        return SKIP
    port = int(os.environ.get("MU_TEST_BASE_PORT", "41300"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("MELEE_TEST_")}
    env["MELEE_GAME_DLL"] = str(dll.resolve())
    with tempfile.TemporaryDirectory(prefix="mu_p2p_pair_") as out:
        # Each instance writes only into this scratch folder (user data, generated Sys files, replays),
        # and reads the source tree's Sys folder: a build folder has no Sys or deltas of its own.
        # --extra-p1/--extra-p2 split on spaces, so their paths must have none.
        if " " in out:
            print(f"skipped: the scratch folder {out} has a space in it (set TEMP to a folder without one)")
            return SKIP
        slots = [" ".join(["--user-dir", f"{out}/p{i}/User", "--sysgen-data-dir", f"{out}/p{i}/sysgen",
                           "--replay-dir", f"{out}/p{i}/Replays", "--fighters-dir", "none"]) for i in (1, 2)]
        cmd = [sys.executable, "-B", str(ROOT / "tools" / "p2p_pair.py"), "--exe", str(exe), "--iso", iso,
               "--base-port", str(port), "--frames", str(FRAMES), "--out", out,
               "--extra-p1", slots[0], "--extra-p2", slots[1],
               "--extra", "--sys-dir", str(ROOT / "port" / "slippi_sys")]
        began = time.monotonic()
        try:
            run = subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True, errors="replace",
                                 timeout=LIMIT)
        except subprocess.TimeoutExpired as exc:
            raise SystemExit(f"FAIL: p2p_pair.py still running after {LIMIT:.0f} s\n{exc.stdout or ''}")
        report = run.stdout + run.stderr
        print(report)
        rows = re.findall(r"^P([12])\s+-?\d+\s+(\w+)\s+(\w+)\s+(\d+)\s+(\d+)", report, re.M)
        problems = []
        if len(rows) != 2:
            problems.append("no result table for both instances")
        for slot, handshake, started, compared, mismatched in rows:
            if int(mismatched):
                problems.append(f"P{slot} {mismatched} checksums mismatched")
            if not int(compared):
                problems.append(f"P{slot} compared no checksums")
        if run.returncode != 0 or "\nPASS" not in report:
            problems.append(f"p2p_pair.py exit {run.returncode}, no PASS")
        if problems:
            raise SystemExit("FAIL: " + "; ".join(problems))
        print(f"ok   0 mismatched over {', '.join(r[3] for r in rows)} checksums ({time.monotonic() - began:.0f} s)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(check(sys.argv[1]))
