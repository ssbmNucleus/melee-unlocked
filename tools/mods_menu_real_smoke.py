"""Real-site smoke run of the in-game Mods menu (SSBM Nucleus, read only).

Starts the game hidden from an install folder, drives the menus with a scripted pad, and checks the log:
Browse -> Characters (all fighters), opens the first row (a pack's list or a card's viewer) and backs
out; steps to Bowser and opens its second row; then Browse -> Stages (the owner's crash path, a row
picked in Characters first), opens its first two rows and backs out to the Mods menu.

Nothing is installed: A is pressed only on a list of posts (which opens a row) and never in a pack's
list or a viewer, where it would install. About eight GETs: three list pages and the pictures of the
rows opened. Frames are captured to <out>/cap for evidence.

    python tools/mods_menu_real_smoke.py --install <folder with melee_source.exe> --iso <1.02 disc> --out <dir>
"""

import argparse
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

# (frame, buttons): pressed for six frames each. From the title to the main menu, down to Mods, Browse,
# Characters; the rest as the docstring says.
PAD = [(150, "B"), (260, "B"), (410, "DD"), (470, "DD"), (560, "A"), (680, "A"), (800, "A"),
       (1100, "A"), (1400, "B"), (1600, "R"), (1900, "DD"), (1950, "A"), (2250, "B"), (2450, "B"),
       (2600, "DD"), (2700, "A"), (3000, "A"), (3300, "B"), (3450, "DD"), (3500, "A"), (3800, "B"),
       (4000, "B"), (4150, "B")]
FRAMES = 4300


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    install, iso, out = args.install.resolve(), args.iso.resolve(), args.out.resolve()
    exe = install / "melee_source.exe"
    if not exe.is_file() or not iso.is_file():
        print("melee_source.exe or the disc is missing")
        return 2
    (out / "cap").mkdir(parents=True, exist_ok=True)
    script = out / "pad.txt"
    script.write_text("\n".join(f"{f} {b}\n{f + 6}" for f, b in PAD) + "\n", encoding="ascii")
    env = os.environ.copy()
    env.pop("MELEE_TEST_MOD_BROWSER_API", None)   # the real site
    crash = install / "melee_port_crash.txt"
    if crash.exists():
        crash.unlink()
    command = [str(exe), "--hidden", "--no-dialogs", "--iso", str(iso),
               "--settings-path", str(out / "port-settings.ini"), "--sys-dir", str(install / "Sys"),
               "--user-dir", str(out / "User" / "Slippi"), "--replay-dir", str(out / "Replays"),
               "--card-dir", str(out / "Card"), "--sysgen-data-dir", str(out / "sysgen"),
               "--window", "1280x1056", "--capture", str(out / "cap" / "f.ppm"), "--capture-every", "100",
               "--frames", str(FRAMES), "--script", str(script)]
    log_path = out / "stdout.txt"
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        try:
            result = subprocess.run(command, cwd=install, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout, check=False)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    try:
        from PIL import Image
        for f in glob.glob(str(out / "cap" / "*.ppm")):
            Image.open(f).save(f[:-4] + ".png")
            os.remove(f)
    except ImportError:
        pass
    text = log_path.read_text(encoding="utf-8", errors="replace")
    pages = re.findall(r"mods menu: Browse (Characters|Stages) \((\w[^)]*)\) page 1: (\d+) posts", text)
    failed = re.findall(r"mods menu: .* not read \(.*", text)
    gets = re.findall(r"^.*net: GET .*$", text, re.M)
    installs = re.findall(r"mods menu: install card", text)
    problems = []
    if code != 0:
        problems.append(f"exit {code}")
    if crash.exists():
        problems.append("crash file: " + crash.read_text(errors="replace")[:200])
    for want in (("Characters", "all"), ("Characters", "Bowser"), ("Stages", "all")):
        if not any(p[0] == want[0] and p[1] == want[1] and int(p[2]) > 0 for p in pages):
            problems.append(f"no posts read for {want[0]} ({want[1]})")
    if installs:
        problems.append("an install was started")
    print(f"pages read: {len(pages)}, not read: {len(failed)}, GETs: {len(gets)}, captures in {out / 'cap'}")
    for line in gets:
        print("  " + line.split("net: ", 1)[1][:160])
    for line in failed:
        print("  " + line[:160])
    if problems:
        print("FAILED: " + "; ".join(problems))
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
