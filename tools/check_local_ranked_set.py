#!/usr/bin/env python3
"""Play a local Ranked set through the fork's online menu using two isolated clients."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def two_ports():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as first, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as second:
        first.bind(("127.0.0.1", 0))
        second.bind(("127.0.0.1", 0))
        return first.getsockname()[1], second.getsockname()[1]


def controller(player, setup_frames=20000, match=None):
    """The input script: the online menu, then Ranked setup presses for setup_frames retraces
    after the first setup screen, then `match` (match-relative lines; default: stand still, or
    hold right for player 2)."""
    lines = []
    lines.extend(["300 A", "310", "840 sx=61 sy=127", "857", "880 A", "890", "940 START", "950"])
    lines.append("@scene 8:5")
    for base in range(0, setup_frames, 130):
        for offset, pad in [(0, "A"), (10, "A"), (20, "DR"), (30, "A"), (40, "A"), (50, "DR"),
                            (60, "A"), (70, "A"), (80, "DL"), (90, "A"), (100, "A"), (110, "DR")]:
            lines.extend([f"{base + offset} {pad}", str(base + offset + 3)])
    lines.extend(match or ["@match", "0 sx=100" if player else "0", "@loop 6"])
    return "\n".join(lines) + "\n"


def start_client(player, work, ports, args, log):
    for directory in ("home", "user", "data", "cache", "config", "appdata", "localappdata"):
        (work / directory).mkdir(parents=True, exist_ok=True)
    (work / "user/user.json").write_text(json.dumps({
        "uid": f"local-test-{player}", "playKey": "offline-fixture",
        "displayName": f"Localtest{player}", "connectCode": f"TEST#{100 + player}",
        "latestVersion": "3.7.0",
    }))
    script = work / "script.txt"
    script.write_text(controller(player))
    env = {k: os.environ[k] for k in ("PATH", "SystemRoot", "WINDIR", "TMP", "TEMP") if k in os.environ}
    env.update(HOME=str(work / "home"), APPDATA=str(work / "appdata"), LOCALAPPDATA=str(work / "localappdata"),
               XDG_DATA_HOME=str(work / "data"), XDG_CACHE_HOME=str(work / "cache"),
               XDG_CONFIG_HOME=str(work / "config"), SDL_AUDIODRIVER="dummy", MELEE_NO_GC_ADAPTER="1",
               ROSETTA_ADVERTISE_AVX="1", MELEE_TEST_MM_HOST="127.0.0.1:1", MELEE_TEST_SCRIPT_PAD_FRAMES="1")
    command = [args.exe, "--iso", args.iso, "--headless", "--volume", "0", "--slippi-menus", "on",
               "--local-peer", f"{player}:{ports[player]}:127.0.0.1:{ports[1 - player]}",
               "--sys-dir", args.sys_dir, "--user-dir", str(work / "user"), "--card-dir", str(work / "cards"),
               "--settings-path", str(work / "settings.ini"), "--replay-dir", str(work / "games"),
               "--rng-seed", "0x12345678", "--time-base", "0", "--script", str(script),
               "--frames", "23000", "--log-file", str(work / "host.log")]
    if args.fast:
        command.append("--fast")
    return subprocess.Popen(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT)


def check(text, player):
    games = re.findall(r"game report: mode 0, (\d+) frames, game (\d+), tiebreak 0, winner (\d+), end 2, lras -1, stage (\d+)", text)
    score = "0-2" if player else "2-0"
    verified = (f"ranked set: over {score}, practice 0" in text and len(games) == 2 and
                [g[1] for g in games] == ["1", "2"] and all(g[2] == "0" for g in games) and
                text.count("slippi: local test session, game report not sent") >= 2 and
                text.count("desyncs 0 result completed") >= 2)
    return games if verified else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--sys-dir", required=True, type=Path)
    parser.add_argument("--fast", action="store_true", help="Run unpaced. Use paced mode when either peer stalls.")
    parser.add_argument("--output", type=Path, help="Create a fresh directory here for logs and both profiles.")
    args = parser.parse_args()
    args.exe, args.iso, args.sys_dir = (path.resolve(strict=True) for path in (args.exe, args.iso, args.sys_dir))
    work = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="local-ranked-"))
    if args.output:
        work.mkdir(parents=True, exist_ok=False)
    print(f"Profiles and logs in {work}", flush=True)
    ports = two_ports()
    processes, files = [], []
    deadline = time.monotonic() + 420
    try:
        for player in range(2):
            peer = work / str(player)
            peer.mkdir()
            log = (peer / "console.log").open("wb")
            files.append(log)
            processes.append(start_client(player, peer, ports, args, log))
        while time.monotonic() < deadline:
            results = []
            for player, proc in enumerate(processes):
                log = work / str(player) / "host.log"
                text = log.read_text(errors="replace") if log.exists() else ""
                result = check(text, player)
                if result is None and (proc.poll() is not None or "ranked set: abandoned" in text):
                    raise RuntimeError(f"Peer {player} failed to finish its set. See {log}")
                results.append(result)
            if all(results):
                if results[0] != results[1]:
                    raise RuntimeError(f"Conflicting game reports: {results}")
                print(f"Both clients completed the same Ranked set, 2-0. Reports: {results[0]}")
                return
            time.sleep(1)
        raise RuntimeError("Timed out waiting for both set-complete messages.")
    finally:
        for proc in processes:
            if proc.poll() is None:
                proc.terminate()
        for proc in processes:
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for f in files:
            f.close()


if __name__ == "__main__":
    main()
