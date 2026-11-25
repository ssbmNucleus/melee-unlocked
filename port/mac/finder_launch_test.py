"""Reproduce Finder's no-argument launch in a disposable .app and HOME, without opening a window."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        contents = Path(temp, "Test.app/Contents")
        exe = contents / "MacOS/test"
        Path(temp, "disposable-finder-test").touch()
        exe.parent.mkdir(parents=True)
        (contents / "Resources/Sys").mkdir(parents=True)
        (contents / "Resources/melee_game.so").touch()
        subprocess.run(["/usr/bin/clang++", "-std=c++17", "-Iport/third_party",
                        "port/mac/finder_launch_test.cpp", "-o", exe], cwd=ROOT, check=True)
        subprocess.run([exe], env={"HOME": temp, "PATH": os.defpath}, check=True)
        print("Finder launch, cancel, remembered disc, bundle resources, profile and account discovery passed.")
