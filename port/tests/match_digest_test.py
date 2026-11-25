"""One scripted offline match, played twice, must give the same state digest, and the one recorded here.

    python port/tests/match_digest_test.py <path to melee_source.exe> [--record]

Both runs: --fast --time-base 0 --rng-seed 0x12345678 --state-digest, the script
port/tests/fixtures/digest_vs.txt (the menus to a VS match; after the entry both players close in and
trade jabs and forward smashes, so hits, damage and knockback run) and --match 31:11:13 (Battlefield).
The two runs go side by side.
What is compared is every field of the first FRAMES match frames, as tools/digest_compare.py reads
them; RECORDED is the SHA-256 of those rows. A change that alters gameplay changes it: check the change
was meant to, then run with --record and paste the printed value (AGENT-RULES: CPU/AI or gameplay work
must replay byte-identical).

Needs the player's own disc (MELEE_TEST_ISO; tools/run_tests.py finds it) and the game library:
melee_game.dll beside the program, or MELEE_GAME_DLL. Without either it says so and exits 77
(skipped). Everything a run writes goes to a scratch folder.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools"))
from print_info_test import CRASH_FILES  # noqa: E402
from digest_compare import compare, read_digest_csv  # noqa: E402

SKIP = 77
LIMIT = 300.0          # seconds per run
BOOT_FRAMES = 2500     # the match starts near retrace 1527; this leaves about 970 match frames
FRAMES = 900           # match frames compared and hashed
RECORDED = "c5b511eedc0a738c3bf46f428dd11bd6766a30b399d340d738e7cabc5a6fd992"   # netlee-main 7e3e436a


def rows_digest(frames):
    h = hashlib.sha256()
    for frame in range(FRAMES):
        row = frames[frame]
        h.update((f"{frame}:" + ",".join(f"{k}={row[k]}" for k in sorted(row)) + "\n").encode())
    return h.hexdigest()


def start(exe, iso, scratch, env):
    scratch.mkdir(parents=True)
    (scratch / "settings.ini").write_text("discordon 0\n", encoding="ascii")
    args = [str(exe), "--hidden", "--no-dialogs", "--volume", "0", "--no-music", "--iso", iso,
            "--settings-path", str(scratch / "settings.ini"), "--sys-dir", str(ROOT / "port" / "slippi_sys"),
            "--sysgen-data-dir", str(scratch / "sysgen"), "--user-dir", str(scratch / "User" / "Slippi"),
            "--card-dir", str(scratch / "Card"), "--replay-dir", str(scratch / "Replays"),
            "--fighters-dir", "none", "--log-file", str(scratch / "port.log"),
            "--fast", "--time-base", "0", "--rng-seed", "0x12345678",
            "--state-digest", str(scratch / "digest.csv"), "--frames", str(BOOT_FRAMES),
            "--script", str(HERE / "fixtures" / "digest_vs.txt"), "--match", "31:11:13"]
    return subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            cwd=str(exe.parent), env=env)


def check(exe, record=False):
    exe = Path(exe).resolve()
    iso = os.environ.get("MELEE_TEST_ISO", "")
    dll = Path(os.environ.get("MELEE_GAME_DLL", str(exe.parent / ("melee_game.dll" if os.name == "nt" else "melee_game.so"))))
    if not iso or not Path(iso).is_file():
        print("skipped: MELEE_TEST_ISO does not name the disc")
        return SKIP
    if not dll.is_file():
        print(f"skipped: no game library at {dll} (copy it beside the program or set MELEE_GAME_DLL)")
        return SKIP
    env = {k: v for k, v in os.environ.items() if not k.startswith("MELEE_TEST_")}
    env["MELEE_GAME_DLL"] = str(dll.resolve())
    env["MELEE_NO_GC_ADAPTER"] = "1"   # never take the adapter from a game the player is running
    crash_before = {n: (exe.parent / n).stat().st_mtime_ns if (exe.parent / n).is_file() else None for n in CRASH_FILES}
    with tempfile.TemporaryDirectory(prefix="mu_match_digest_") as scratch:
        runs = [Path(scratch) / name for name in ("a", "b")]
        began = time.monotonic()
        procs = [start(exe, iso, run, env) for run in runs]
        codes = []
        for proc in procs:
            try:
                codes.append(proc.wait(timeout=max(1.0, LIMIT - (time.monotonic() - began))))
            except subprocess.TimeoutExpired:
                proc.kill()   # this script's own child, by its handle
                proc.wait()
                codes.append(None)
        took = time.monotonic() - began
        crash_after = {n: (exe.parent / n).stat().st_mtime_ns if (exe.parent / n).is_file() else None for n in CRASH_FILES}
        problems = []
        if crash_after != crash_before:
            problems.append("a run wrote a crash report")
        frames = []
        for run, code in zip(runs, codes):
            if code != 0:
                problems.append(f"run {run.name}: " + ("still running after %.0f s (ended it)" % LIMIT if code is None
                                                         else f"exit {code if code < 256 else hex(code & 0xFFFFFFFF)}"))
                continue
            try:
                data = read_digest_csv(run / "digest.csv", (run / "digest.csv").read_text(encoding="utf-8-sig"))
            except (OSError, ValueError) as exc:
                problems.append(f"run {run.name}: {exc}")
                continue
            if len(data) < FRAMES:
                problems.append(f"run {run.name}: only {len(data)} match frames, {FRAMES} needed")
                continue
            frames.append(data)
        if problems:
            for run in runs:
                log = run / "port.log"
                tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-6:] if log.is_file() else []
                print(f"{run.name} log tail:\n    " + "\n    ".join(tail))
            raise SystemExit("FAIL: " + "; ".join(problems))
        same, message = compare([("a", {f: frames[0][f] for f in range(FRAMES)}),
                                 ("b", {f: frames[1][f] for f in range(FRAMES)})])
        print(f"two runs in {took:.1f} s: {message}")
        if not same:
            raise SystemExit("FAIL: the same match played twice differs")
        digest = rows_digest(frames[0])
        if record:
            print(f'RECORDED = "{digest}"')
            return 0
        if digest != RECORDED:
            raise SystemExit(f"FAIL: digest {digest}, recorded {RECORDED}: this build plays the match differently. "
                             f"If the change was meant to alter gameplay, run with --record and update RECORDED.")
        print(f"ok   {FRAMES} match frames, digest {digest[:16]} as recorded")
    return 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--record"):
        sys.exit(__doc__)
    sys.exit(check(sys.argv[1], record=len(sys.argv) == 3))
