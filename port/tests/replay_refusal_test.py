"""A replay this game cannot play is refused before its match, with one line in the log and exit code 5.

    python port/tests/replay_refusal_test.py <path to melee_source.exe>

Needs the player's own disc (MELEE_TEST_ISO) and the game library: melee_game.dll beside the program,
or MELEE_GAME_DLL naming one (build-sourceport-gcc/melee_game.dll). Without either it says so and exits
77, which ctest counts as skipped. The Sys folder is the source tree's (port/slippi_sys); the three
files generated from the disc go to a scratch folder, as does everything else a run writes.

Each case is a small .slp written here (a Game Start and a few frames, Slippi 3.20.0): a character
that is in no game (external id 40), a stage that is not one (400), a costume past the character's
count, and a replay recorded with two installed fighters (Shulk and Wolf take the external ids 26 and
27, recorded as fighters 27 and 28): without them those ids are Master Hand and a Wireframe, and the
Wireframe is fighter 29 here. Playing such a replay once crashed the game (melee_game.dll+0x275301):
loading the Wireframe's costume passed an int 0 where a pointer ends the list (ftdata.c). Each must
exit 5 within the limit, without a window or a crash report, with the expected line in the log and no
recording written. A Fox and Marth replay made the same way must start its match and exit 0."""
import os
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from print_info_test import CRASH_FILES, visible_windows  # noqa: E402

LIMIT = 90.0
SKIP = 77
REFUSED = 5
VERSION = (3, 20, 0, 0)
# Event payload sizes of Slippi 3.20.0 (Game Start, pre-frame, post-frame, Game End).
SIZES = {0x36: 828, 0x37: 66, 0x38: 92, 0x39: 6}
FIRST_FRAME = -123

# A retail VS match's rules (0x60 bytes: 4 stocks, 8 minutes, items off), the stage at 0xE.
RULES = bytes.fromhex(
    "32018e4cc3000000000000ffff6e0020" "000001e0000000000000000000000000"
    "fffffff80000000f000000003f800000" "3f8000003f8000000000000000000000"
    "00000000000000000000000000000000" "00000000000000000000000000000000")
# A human player (character, slot type 0, 4 stocks, costume 0, ...) and an empty slot (type 3).
PLAYER = bytes.fromhex("020004000000000009007800c0000401" "00000000000000003f8000003f800000" "3f800000")
EMPTY = bytes.fromhex("150304000000ff0009007800c0000401" "00000000000000003f8000003f800000" "3f800000")


def game_info(stage, players):
    """The 0x138-byte block: rules, then six slots. players: [(external id, costume, recorded fighter)]."""
    block = bytearray(RULES)
    struct.pack_into(">H", block, 0xE, stage)
    for i in range(6):
        if i < len(players):
            slot = bytearray(PLAYER)
            slot[0], slot[3], slot[4] = players[i][0], players[i][1], i
        else:
            slot = bytearray(EMPTY)
            slot[4] = i
        block += slot
    assert len(block) == 0x138
    return bytes(block)


def write_slp(path, stage, players, frames=30):
    """A replay of `frames` frames from the first one: each player stands still, recorded as its fighter."""
    def event(command, body):
        payload = bytearray(SIZES[command])
        payload[:len(body)] = body
        return bytes([command]) + bytes(payload)

    start = bytes(VERSION) + game_info(stage, players) + struct.pack(">I", 0x5EED1234)
    out = [event(0x36, start)]
    for frame in range(FIRST_FRAME, FIRST_FRAME + frames):
        for port, (_, _, fighter) in enumerate(players):
            x = -30.0 + 60.0 * port
            pre = struct.pack(">iBBIHfff", frame, port, 0, 0x5EED1234, 14, x, 0.0, 1.0)
            out.append(event(0x37, pre))
            post = struct.pack(">iBBBHfffffBBBBf", frame, port, 0, fighter, 14, x, 0.0, 1.0, 0.0, 60.0,
                               0, 0, 6, 4, 0.0)
            out.append(event(0x38, post))
    out.append(event(0x39, bytes([7, 0xFF])))   # no contest
    sizes = b"".join(bytes([c]) + struct.pack(">H", s) for c, s in SIZES.items())
    raw = bytes([0x35, 1 + len(sizes)]) + sizes + b"".join(out)
    path.write_bytes(b"{U\x03raw[$U#l" + struct.pack(">I", len(raw)) + raw)
    return path


# Fox is external 2 (fighter 1), Marth 9 (fighter 18). Final Destination is stage 32.
CASES = [
    ("unknown character", 32, [(2, 0, 1), (40, 0, 1)], REFUSED, "player 2's character 40 is not in this game"),
    ("unknown stage", 400, [(2, 0, 1), (9, 0, 18)], REFUSED, "stage 400 is not a stage of this game"),
    ("costume past the count", 32, [(2, 9, 1), (9, 0, 18)], REFUSED, "player 1's costume 9 is past the character's"),
    ("installed fighters", 32, [(26, 0, 27), (27, 0, 28)], REFUSED,
     "player 2's character 27 is fighter 29 in this game but fighter 28 in the replay"),
    ("retail match", 32, [(2, 0, 1), (9, 0, 18)], 0, "replay: match starts, stage 32"),
]


def run(exe, args, env, log):
    folder = exe.parent
    before = {name: (folder / name).stat().st_mtime_ns if (folder / name).is_file() else None for name in CRASH_FILES}
    started = time.monotonic()
    proc = subprocess.Popen([str(exe)] + args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, cwd=str(folder), env=env)
    windows = set()
    while proc.poll() is None and time.monotonic() - started < LIMIT:
        windows.update(visible_windows(proc.pid))
        time.sleep(0.1)
    alive = proc.poll() is None
    if alive:
        proc.kill()   # this script's own child, by its handle
        proc.wait()
    after = {name: (folder / name).stat().st_mtime_ns if (folder / name).is_file() else None for name in CRASH_FILES}
    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    return proc.returncode, alive, sorted(windows), after != before, text, time.monotonic() - started


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
    sys_dir = HERE.parent / "slippi_sys"
    env = {k: v for k, v in os.environ.items() if not k.startswith("MELEE_TEST_")}
    env["MELEE_GAME_DLL"] = str(dll.resolve())
    env["MELEE_NO_GC_ADAPTER"] = "1"   # never take the adapter from a game the player is running
    failures = []
    with tempfile.TemporaryDirectory(prefix="mu_replay_refusal_") as scratch:
        scratch = Path(scratch)
        settings = scratch / "settings.ini"
        settings.write_text("discordon 0\n", encoding="ascii")
        for number, (name, stage, players, expected, line) in enumerate(CASES):
            slp = write_slp(scratch / f"case{number}.slp", stage, players)
            log = scratch / f"case{number}.log"
            replays = scratch / f"replays{number}"
            args = ["--hidden", "--no-dialogs", "--volume", "0", "--iso", iso, "--settings-path", str(settings),
                    "--sys-dir", str(sys_dir), "--sysgen-data-dir", str(scratch / "sysgen"),
                    "--user-dir", str(scratch / "User" / "Slippi"), "--card-dir", str(scratch / "Card"),
                    "--replay-dir", str(replays), "--fighters-dir", "none", "--frames", "900",
                    "--log-file", str(log), "--replay", str(slp)]
            code, alive, windows, crashed, text, took = run(exe, args, env, log)
            problems = []
            if alive:
                problems.append(f"still running after {LIMIT:.0f} s (ended it)")
            if windows:
                problems.append(f"showed a window {windows}")
            if crashed:
                problems.append("wrote a crash report")
            if code != expected:
                problems.append(f"exit {code if code is None or code < 256 else hex(code & 0xFFFFFFFF)}, expected {expected}")
            if line not in text:
                problems.append(f"no log line containing {line!r}")
            if expected == REFUSED:
                if "the match is not started" not in text:
                    problems.append("no refusal line")
                if "match starts" in text:
                    problems.append("started the match")
                if replays.is_dir() and any(replays.iterdir()):
                    problems.append("wrote a recording of a match that never started")
            if problems:
                failures.append(f"{name}: " + "; ".join(problems))
                tail = [l for l in text.splitlines() if "replay" in l or "stopped" in l][-8:]
                print(f"FAIL {name} ({took:.1f} s): {'; '.join(problems)}\n    " + "\n    ".join(tail))
            else:
                print(f"ok   {name}: exit {code} in {took:.1f} s: {line}")
    if failures:
        raise SystemExit("\n".join(failures))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(check(sys.argv[1]))
