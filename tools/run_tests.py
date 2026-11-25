#!/usr/bin/env python3
"""Run one label of the tests in a worktree's host and game build folders, with one summary.

    python tools/run_tests.py quick     # no game run, no disc: the host's and the game's quick tests (< 1 min)
    python tools/run_tests.py game      # start melee_source.exe or read the disc: a match digest twice,
                                        # a p2p pair with 0 mismatched, replay refusal, print-info ...
    python tools/run_tests.py slow      # long programs, and the port_slippi_* programs built only on request
    python tools/run_tests.py all       # everything both build folders register
    python tools/run_tests.py quick --build   # first build every program the label's tests run

The labels are set in port/tests/test_labels.cmake (host) and sourceport/game/CMakeLists.txt (game:
all quick). Build folders default to build-host and build-gcc (tools/new_worktree.py makes those);
--host and --game name others. The game tests need the disc: MELEE_TEST_ISO, else the one
tools/melee_iso.py finds on this machine. Their UDP ports (MU_TEST_BASE_PORT) are this worktree's own
range from tools/new_worktree.py. A test that cannot run here (no compiler for a fixture, a program
built only on request, no disc) reports itself skipped with the reason, not failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VS_CMAKE_BIN = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions"
                    r"\Microsoft\CMake\CMake\bin")
NINJA = Path(r"C:\toolchains\mingw64\bin\ninja.exe")


def tool(name):
    exe = VS_CMAKE_BIN / f"{name}.exe"
    if exe.is_file():
        return str(exe)
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} not found")
    return found


def label_args(label):
    return [] if label == "all" else ["-L", f"^{label}$"]


def unbuilt_commands(build, config):
    """{test: the .exe paths of its command} from the build folder's CTestTestfile.cmake files. ctest's
    own listing leaves the command out when the program does not exist yet, as in a fresh folder."""
    found = {}
    marker = f"/{config}/".lower()
    for testfile in build.rglob("CTestTestfile.cmake"):
        for line in testfile.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r'\s*add_test\((?:\[=\[(.+?)\]=\]|([^\s"\[]+))\s+(.*)$', line)
            if m:
                exes = re.findall(r'"([^"]+?\.exe)"', m.group(3))
                found.setdefault(m.group(1) or m.group(2), []).extend(
                    p for p in exes if marker in p.replace("\\", "/").lower())
    return found


def build_programs(name, build, label, config):
    """Build what the label's tests run: the game folder's mu_game_tests, or each host test program."""
    if name == "game":
        cmd = [str(NINJA) if NINJA.is_file() else "ninja", "-C", str(build), "-j3", "mu_game_tests"]
    else:
        listing = subprocess.run([tool("ctest"), "--test-dir", str(build), "-C", config, "--show-only=json-v1",
                                  *label_args(label)], capture_output=True, text=True, errors="replace")
        targets = set()
        fallback = None
        for test in json.loads(listing.stdout).get("tests", []):
            command = test.get("command")
            if not command:   # its program is not built yet
                fallback = unbuilt_commands(build, config) if fallback is None else fallback
                command = fallback.get(test.get("name"), [])
            for part in command:
                path = Path(part)
                # The test's program, or the program run_if_built.py / a Python test is handed.
                if path.suffix.lower() == ".exe" and build in path.parents and not path.stem.startswith("port_slippi_"):
                    targets.add(path.stem)
        if not targets:
            return 0
        cmd = [tool("cmake"), "--build", str(build), "--config", config, "--target", *sorted(targets),
               "--", "/m:2", "/nologo", "/v:m"]
    print(f"{name}: building {'mu_game_tests' if name == 'game' else '%d test programs' % len(cmd[cmd.index('--target') + 1:cmd.index('--')])}")
    started = time.monotonic()
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if proc.returncode:
        print((proc.stdout + proc.stderr)[-4000:])
        print(f"{name}: build failed ({time.monotonic() - started:.0f} s)")
    else:
        print(f"{name}: built in {time.monotonic() - started:.0f} s")
    return proc.returncode


def skip_reasons(build):
    """{test: its first line that says why it skipped}, from ctest's log of the last run."""
    log = build / "Testing" / "Temporary" / "LastTest.log"
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    reasons = {}
    for match in re.finditer(r'^\d+/\d+ Test: (\S+)\n(.*?)<end of output>', text, re.M | re.S):
        line = next((l.strip() for l in match.group(2).splitlines() if re.search(r"skip", l, re.I)), "")
        reasons[match.group(1)] = line
    return reasons


def run(build, label, jobs, config, env):
    cmd = [tool("ctest"), "--test-dir", str(build), "-j", str(jobs), "--output-on-failure"]
    if config:
        cmd += ["-C", config]
    cmd += label_args(label)
    start = time.monotonic()
    proc = subprocess.run(cmd, cwd=build, capture_output=True, text=True, errors="replace", env=env)
    seconds = time.monotonic() - start
    out = proc.stdout + proc.stderr
    total = re.search(r"tests passed, (\d+) tests failed out of (\d+)", out)
    if total is None:
        if "No tests were found" in out:
            return {"count": 0, "failed": 0, "skipped": 0, "passed": 0, "seconds": seconds,
                    "failed_names": [], "skipped_names": [], "output": out}
        print(out)
        return None
    skipped_names = re.findall(r"^\s*\d+ - (\S+) \((?:Skipped|Disabled)\)", out, re.M)
    failed_names = re.findall(r"^\s*\d+ - (\S+) \(((?!Skipped|Disabled)[^)]*)\)", out, re.M)
    failed, count = int(total.group(1)), int(total.group(2))
    skipped = len(skipped_names)
    return {"count": count, "failed": failed, "skipped": skipped, "passed": count - failed - skipped,
            "seconds": seconds, "failed_names": failed_names, "skipped_names": skipped_names, "output": out}


def worktree_ports():
    """The first port of this worktree's range in tools/new_worktree.py's registry, or None."""
    common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--git-common-dir"], capture_output=True, text=True)
    if common.returncode:
        return None
    path = Path(common.stdout.strip())
    path = path if path.is_absolute() else (ROOT / path).resolve()
    try:
        registry = json.loads((path / "netlee-worktrees.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    for folder, entry in registry.items():
        if os.path.normcase(os.path.abspath(folder)) == os.path.normcase(str(ROOT)):
            return entry["ports"][0]
    return None


def test_env(label):
    env = dict(os.environ)
    if label in ("game", "all"):
        if not env.get("MELEE_TEST_ISO"):
            sys.path.insert(0, str(ROOT / "tools"))
            from melee_iso import find_iso
            iso = find_iso()
            if iso:
                env["MELEE_TEST_ISO"] = str(iso)
                print(f"disc: {iso} (set MELEE_TEST_ISO to use another)")
            else:
                print("disc: none found (set MELEE_TEST_ISO); the tests that need it skip")
        if not env.get("MU_TEST_BASE_PORT"):
            port = worktree_ports()
            if port:
                env["MU_TEST_BASE_PORT"] = str(port)
    return env


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("label", choices=("quick", "game", "slow", "all"))
    ap.add_argument("--host", default=str(ROOT / "build-host"), help="MSVC build folder (default %(default)s)")
    ap.add_argument("--game", default=str(ROOT / "build-gcc"), help="MinGW game build folder (default %(default)s)")
    ap.add_argument("--build", action="store_true", help="build the programs the label's tests run first")
    ap.add_argument("-j", "--jobs", type=int, default=8)
    args = ap.parse_args()
    # Absolute: ctest runs inside the folder, where a relative --test-dir would name another one.
    folders = [("host", Path(args.host).resolve(), "Release")]
    if args.label in ("quick", "all"):   # the game folder's tests are all quick
        folders.append(("game", Path(args.game).resolve(), None))
    env = test_env(args.label)
    worst = 0
    for name, build, config in folders:
        if not (build / "CTestTestfile.cmake").is_file():
            print(f"{name}: {build} is not a configured build folder (--{name} names another)")
            worst = 1
            continue
        if args.build and build_programs(name, build, args.label, config or "Release"):
            worst = 1
            continue
        r = run(build, args.label, args.jobs, config, env)
        if r is None:
            worst = 1
            continue
        print(f"{name:5} {r['passed']:4} passed {r['skipped']:4} skipped {r['failed']:4} failed "
              f"of {r['count']:4}  {r['seconds']:6.1f} s  ({build})")
        reasons = skip_reasons(build) if r["skipped_names"] else {}
        for test in r["skipped_names"]:
            print(f"      skipped: {test}" + (f": {reasons[test]}" if reasons.get(test) else ""))
        for test, why in r["failed_names"]:
            hint = " (its program is not built: --build)" if why == "Not Run" else ""
            print(f"      failed: {test}{hint}")
        if r["failed"]:
            worst = 1
    return worst


if __name__ == "__main__":
    sys.exit(main())
