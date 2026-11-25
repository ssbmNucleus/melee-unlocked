"""melee_source --print-info and the other one-shot modes, run the way a launcher or a script runs them.

    python port/tests/print_info_test.py <path to melee_source.exe or melee_port.exe>

Each mode runs with stdin closed and its output on a pipe, and must end by itself within 20 seconds
without showing a window and without writing a crash report: a message box nobody is there to answer
(a crash box over a bad option once waited on the owner's screen) keeps the process alive, so a
process still running at the limit is a failure (this script ends it, by its own handle). On Windows
the visible top-level windows of the process are also checked while it runs.

--print-info must print one JSON object and exit 0. With the game library beside the program (an
install, or a build folder the library was copied to) the plugin fields must have been read from it;
without it they are null. The other modes are --version, --help and -h, a skin catalog command
(--cosmetic-status, against a settings file of its own), three bad command lines with --no-dialogs
(an unknown option, an option missing its value, no disc), which must exit 2, and a disc that is not
there, which must exit 1."""
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

LIMIT = 20.0
BOOLEANS = ("cpu_supported", "fighters_dir", "no_crash_dialog", "no_dialogs")
CRASH_FILES = ("melee_port_crash.txt", "melee_port_crash.dmp")


def visible_windows(pid):
    """Titles of the visible top-level windows that belong to the process (Windows only)."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            title = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, title, 256)
            found.append(title.value or "(untitled)")
        return True

    user32.EnumWindows(proc(each), 0)
    return found


def crash_stamps(folder):
    return {name: (folder / name).stat().st_mtime_ns if (folder / name).is_file() else None for name in CRASH_FILES}


def run(exe, args, env, folder=None):
    """Runs one mode; fails if it is alive after LIMIT seconds, showed a window or wrote a crash report."""
    folder = folder if folder is not None else exe.parent
    before = crash_stamps(folder)
    started = time.monotonic()
    proc = subprocess.Popen([str(exe)] + list(args), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, cwd=str(folder), env=env)
    output = {}

    def drain(name, stream):
        output[name] = stream.read()

    readers = [threading.Thread(target=drain, args=("stdout", proc.stdout), daemon=True),
               threading.Thread(target=drain, args=("stderr", proc.stderr), daemon=True)]
    for reader in readers:
        reader.start()
    windows = set()
    while proc.poll() is None and time.monotonic() - started < LIMIT:
        windows.update(visible_windows(proc.pid))
        time.sleep(0.05)
    alive = proc.poll() is None
    if alive:
        windows.update(visible_windows(proc.pid))
        proc.kill()   # this script's own child, by its handle
        proc.wait()
    for reader in readers:
        reader.join(5)
    took = time.monotonic() - started
    label = " ".join(args)
    assert not alive, (f"{label}: still running after {LIMIT:.0f} s (ended it, pid {proc.pid})", sorted(windows))
    assert not windows, (f"{label}: showed a window", sorted(windows))
    assert crash_stamps(folder) == before, (f"{label}: wrote a crash report", output.get("stderr", b"")[:400])
    return proc.returncode, output.get("stdout", b"").decode("utf-8", "replace"), \
        output.get("stderr", b"").decode("utf-8", "replace"), took


def check_info(exe, text):
    lines = [line for line in text.splitlines() if line.strip()]
    assert len(lines) == 1, ("one line of output expected", text[:400])
    info = json.loads(lines[0])
    assert isinstance(info, dict), info
    assert "error" not in info, info
    for key in ("version", "engine", "cpu_baseline", "game_library", "plugin_format", "plugin_abi",
                "linked_fighters", "fighters_folder") + BOOLEANS:
        assert key in info, ("missing", key, info)
    assert isinstance(info["version"], str) and info["version"], info
    # The name programs read (product_identity.h kToken: it stays "Nucleus" whatever players are shown;
    # the SSBM Nucleus desktop app reads this answer), and the project it is built from.
    assert info.get("product") == "Nucleus" and info.get("upstream") == "Melee Unlocked", info
    assert info["engine"] in ("source", "recomp"), info
    assert info["cpu_baseline"] in ("AVX2", "SSE2"), info
    for key in BOOLEANS:
        assert isinstance(info[key], bool), (key, info)
    assert info["no_crash_dialog"] and info["no_dialogs"], info
    assert "no_remember_iso" not in info, info   # the game writes no launcher.ini any more
    # NUCLEUS_PUBLIC_BUILD (port/runtime/host/public_build.h): the program's, and the library's export
    # (null when no library could be read). windows-build/client_release.py packages only true and true.
    assert isinstance(info.get("public_build"), bool), info
    assert info.get("game_public_build", "missing") in (True, False, None), info
    if info["engine"] == "source":
        assert info["fighters_dir"], info
        assert info["game_library"] in ("melee_game.dll", "melee_game_compat.dll"), info
        assert info["fighters_folder"] == str(exe.parent / "fighters"), (info["fighters_folder"], exe.parent)
        if (exe.parent / info["game_library"]).is_file():
            assert isinstance(info["plugin_format"], int) and info["plugin_format"] > 0, info
            assert re.fullmatch(r"[0-9a-f]{32}", info["plugin_abi"] or ""), info
            assert isinstance(info["linked_fighters"], list), info
            assert all(isinstance(name, str) and name for name in info["linked_fighters"]), info
            assert isinstance(info["game_public_build"], bool), info
        else:
            assert info["plugin_format"] is None and info["plugin_abi"] is None, info
            assert info["linked_fighters"] is None, info
            assert info["game_public_build"] is None, info
    else:
        assert info["game_library"] is None and info["plugin_format"] is None, info
    return info, lines[0]


def check(exe):
    exe = Path(exe).resolve()
    env = {k: v for k, v in os.environ.items() if not k.startswith("MELEE_TEST_")}
    env["MELEE_NO_GC_ADAPTER"] = "1"   # never take the adapter from a game the player is running
    with tempfile.TemporaryDirectory(prefix="mu_print_info_") as scratch:
        scratch = Path(scratch)
        # Settings of its own: Discord off (on by default), and the skin catalog beside it.
        settings = scratch / "settings.ini"
        settings.write_text("discordon 0\n", encoding="ascii")
        own = ["--settings-path", str(settings), "--log-file", str(scratch / "run.log")]

        code, out, err, took = run(exe, ["--print-info"], env)
        assert code == 0, ("--print-info exit code", code, err[:400])
        info, line = check_info(exe, out)
        print(f"--print-info ok in {took:.2f} s: {line}")

        if os.name == "nt" and info["public_build"]:   # a public build reads no test hook: the usual answer
            code, out, err, took = run(exe, ["--print-info"], dict(env, MELEE_TEST_PRINT_INFO_FAULT="1"))
            assert code == 0, ("--print-info, public build, fault hook set", code, out[:400], err[:400])
            check_info(exe, out)
            print(f"--print-info, public build: the fault hook is ignored ({took:.2f} s)")
        elif os.name == "nt":   # a fault while it is put together: {"error": ...}, exit 1, still no window
            code, out, err, took = run(exe, ["--print-info"], dict(env, MELEE_TEST_PRINT_INFO_FAULT="1"))
            lines = [line for line in out.splitlines() if line.strip()]
            assert code == 1 and len(lines) == 1, ("--print-info after a fault", code, out[:400], err[:400])
            error = json.loads(lines[0])
            assert isinstance(error.get("error"), str) and error["error"], error
            print(f"--print-info with a fault: exit 1 in {took:.2f} s: {lines[0]}")

        # Answered before anything else reads the command line: a disc that is not there is never opened.
        code, out, err, took = run(exe, ["--iso", str(scratch / "no-such.iso"), "--print-info"], env)
        assert code == 0, ("--print-info after --iso, exit code", code, err[:400])
        check_info(exe, out)
        print(f"--iso ... --print-info ok in {took:.2f} s")

        cases = [
            (["--version"], 0, lambda out: info["version"] in out.splitlines()),
            (["--help"], 0, lambda out: out.startswith("Nucleus Unlocked ") and "Exit codes" in out and "--no-dialogs" in out),
            (["-h"], 0, lambda out: "Exit codes" in out),
            (["--cosmetic-status"] + own, 0, lambda out: re.search(r"^profile (enabled|disabled)", out, re.M)),
            (["--no-dialogs"] + own + ["--no-such-option"], 2, None),
            (["--no-dialogs"] + own + ["--frames"], 2, None),
            (["--no-dialogs"] + own, 2, None),   # no disc named
            (["--no-dialogs"] + own + ["--iso", str(scratch / "no-such.iso")], 1, None),
        ]
        for args, expected, output_ok in cases:
            code, out, err, took = run(exe, args, env)
            label = " ".join(a for a in args if a not in own)
            assert code == expected, (label, "exit code", code, "expected", expected, err[:400], out[:400])
            assert output_ok is None or output_ok(out), (label, "output", out[:400])
            print(f"{label}: exit {code} in {took:.2f} s, no window")


def check_cli(exe, squad):
    exe = Path(exe).resolve()
    env = {k: v for k, v in os.environ.items() if not k.startswith("MELEE_TEST_")}
    env.update(MELEE_AGENT_COPY="1", MELEE_NO_GC_ADAPTER="1")
    with tempfile.TemporaryDirectory(prefix="cli_options_", dir=exe.parent) as folder:
        scratch = Path(folder)
        settings = scratch / "settings.ini"
        settings.write_text("discordon 0\n", encoding="ascii")
        script = scratch / "input.txt"
        script.write_text("1 A\n", encoding="ascii")
        profile = scratch / "fixture.ini"
        profile.write_text("dir = .\n", encoding="ascii")
        own = ["--settings-path", str(settings), "--log-file", str(scratch / "run.log")]
        missing = str(scratch / "missing")
        checked = set()

        def verify(args, want, *, stdout="", stderr=""):
            code, out, err, _ = run(exe, args, env, scratch)
            assert code == want, (args, code, want, out[:200], err[:400])
            assert stdout in out and stderr in err, (args, out[:200], err[:400])
            checked.update(a.split("=", 1)[0] for a in args if re.match(r"^--?[a-z]", a))
            return out

        help_text = verify(["--help"], 0, stdout="Nucleus Unlocked ")
        assert verify(["-h"], 0) == help_text
        published = re.findall(r"^  (-{1,2}[\w-]+)(?=[ \[])", help_text, re.MULTILINE)
        assert len(published) == len(set(published)), published
        assert "Exit codes" in help_text
        info, _ = check_info(exe, verify(["--print-info"], 0))
        # The test fixtures' entry points (CliOption::test_only in port/app/main.cpp) with their values. A
        # public build (NUCLEUS_PUBLIC_BUILD) leaves them out of --help and refuses each with exit 2.
        public = info["public_build"]
        test_only = [["--allow-matchmaking"], ["--local-peer", "0:44203:127.0.0.1:44204"], ["--test-stage", "31"],
                     ["--online-test", "direct"]] + ([["--squad-test-invalid-lineup"]] if squad else [])
        if public:
            for option in test_only:
                assert option[0] not in published, (option[0], "in a public build's --help")
                verify(own + ["--no-dialogs", "--hidden"] + option + ["--list-audio-devices"], 2,
                       stderr=f"{option[0]} is a test option")
        assert verify(["--version"], 0).strip()
        verify(["--prewarm-only", "--invalid"], 64, stdout="Usage: melee_source")
        assert json.loads(verify(own + ["--list-plugins", "--fighters-dir", "none"], 0))["ok"]
        verify(own + ["--approve-plugin", missing + ".mufighter"], 1, stdout='"action":"approve"')
        verify(own + ["--revoke-plugin", "not-a-hash"], 2, stdout='"action":"revoke"')

        # The final audio-list command is a successful parse sentinel. It stops before opening
        # a disc, window, or online session, even when an earlier option requests one.
        def parse(args):
            verify(own + ["--no-dialogs", "--hidden"] + args + ["--list-audio-devices"], 0)

        parse([
            "--iso", missing, "-e", missing, "--mod-base-iso", missing, "--look-iso", missing,
            "--frames", "30", "--fast", "--headless", "--threaded-renderer", "--fps", "monitor",
            "--frame-mode", "authored-interpolate", "--scale", "auto", "--window", "640x480",
            "--aspect", "73:60", "--fullscreen", "--backend", "vk", "--dlss", "fsr-aa",
            "--dlss-mode", "1", "--frame-generation", "--frame-generation=3x", "--ssao", "0.5",
            "--reflex", "--dlss-jitter-sign", "-1", "--vsync", "--flicker-scan", "--pin-phase", "0.5",
            "--frame-times", missing, "--capture", missing, "--capture-frame", "1",
            "--capture-every", "2", "--capture-burst", "3", "--capture-sim-frame", "4",
            "--dump", missing, "--dump-frame", "1", "--shader-cache", missing, "--widescreen",
            "--true-widescreen", "--true-widescreen=", "--true-widescreen=fit",
            "--custom-textures", "--dump-textures", "--sharpness", "1", "--crt", "1",
            "--border", "sharp:" + missing, "--shader", missing, "--ssaa", "2", "--msaa", "4",
            "--anisotropy", "8", "--effects", "2", "--lightweight", "1", "--look", "1", "--retro",
        ])
        parse([
            "--settings-window", "--settings-tab", "audio", "--settings-standby", "1", "--load-settings",
            "--pc-settings", "--no-crash-dialog", "--volume", "0", "--music", "50", "--no-music",
            "--music-volume", "40", "--audio-dump", missing, "--sys-dir", missing, "--sysgen-data-dir", missing,
            "--replay-dir", missing, "--replay-month-folders", "--compress-replays", "--replay", missing + ".slp",
            "-i", missing, "-b", "--trace", missing, "--as-experienced", "--card-dir", missing,
            "--card-self-test", missing, "--fighters-dir", "none", "--vanilla-game", "--no-mod-autodetect",
            "--scan-mods", "--mod-profile", str(profile), "--mod-dir", missing, "--mod-iso", missing,
            "--mod-gci", missing, "--mod-opponent-unverified", "--import-cosmetic", missing,
            "--scan-disc-skins", missing, "--select-cosmetic", "id", "--deselect-cosmetic", "slot",
            "--enable-project-effects", "--restore-vanilla-cosmetics", "--cosmetic-status",
            "--check-effect", missing, "--check-effects", missing, "--csv", missing + ".csv",
        ])
        # The same order as always in the default build; a public build parses the rest without them.
        allow, peer, stage, online = ([], [], [], []) if public else test_only[:4]
        parse([
            "--pal-stock-icons", "--language", "japanese", "--no-screen-shake", "--gecko-codes", missing,
            "--te-options", "ff", "--te-options2", "1", "--cpu-training", "0", "--auto-lcancel",
            "--lcancel-indicator", "--lcancel-log", missing, *allow, "--online-delay", "2",
            "--resume", "--resume", "online", "--user-dir", missing, "--lobby-direct", "TEST#123",
            "--lobby-character", "4", "--chat", "direct", "--netplay-port", "44203",
            *peer, *stage, "--record-native",
            "--slippi-menus", "off", *online,
        ] + (["--squad-test-invalid-lineup"] if squad and not public else []))
        parse([
            "--p2p-port", "44205", "--p2p-peer", "127.0.0.1:44206", "--p2p-slot", "0", "--p2p-chars", "1:2",
            "--p2p-stage", "31", "--p2p-seed", "1234", "--p2p-delay", "2", "--p2p-identity", missing,
            "--p2p-expect", "1" * 64, "--p2p-result", missing, "--p2p-names", "P1:P2",
            "--p2p-connect-seconds", "2", "--p2p-games", "3", "--script", str(script), "--match", "31:1:2",
            "--time-base", "1", "--rng-seed", "0", "--state-trace", missing, "--state-digest", missing,
            "--input-log", missing, "--trace-func", "80000000:2", "--trace-calls", "--quiet",
            "--hang-watch", "2", "--profile", "--profile-render", "--lag-test",
        ])
        parse(["--vanilla-only"])
        assert set(published) <= checked, ("options without a parser test", sorted(set(published) - checked))
        hidden = {option[0] for option in test_only} if public else set()
        assert checked - set(published) == {"--invalid"} | hidden, ("options missing from help", sorted(checked - set(published)))

        # Check argument boundaries, short aliases, and errors using real startup behavior.
        verify(own + ["--hidden", "-e", missing], 1, stderr="cannot open ISO")
        verify(own + ["--hidden", "--iso", "--fast"], 1, stderr="cannot open ISO --fast")
        for args in (["--frames"], ["--frames=3"], ["--frame-generation", "3x"], ["--hidden", "--help"]):
            verify(own + ["--hidden"] + args, 2, stdout="Usage: melee_source")
        verify(own + ["--hidden", "--frame-generation="], 2, stderr="--frame-generation=2x")
        verify(own + ["--hidden", "--p2p-unknown", "1"], 2, stderr="unknown option --p2p-unknown")
        verify(own + ["--hidden", "--scan-mods", "--vanilla-only"], 2,
               stderr="--vanilla-only cannot be combined with --scan-mods")
        # The effect checker dispatches before the game: 2 (not checked) without a disc it can open, and
        # its stdout carries only its JSON lines (none here), never the log.
        assert verify(own + ["--hidden", "--iso", missing, "--check-effect", missing], 2, stderr="cannot open ISO").strip() == ""
        assert verify(own + ["--hidden", "--check-effects", missing, "--csv", missing + ".csv"], 2, stderr="need --iso").strip() == ""
        print(f"{len(published)} command-line options parsed without a disc or window")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[2] == "--cli":
        check_cli(sys.argv[1], squad=sys.argv[3] == "1")
    elif len(sys.argv) == 2:
        check(sys.argv[1])
    else:
        sys.exit(__doc__)
