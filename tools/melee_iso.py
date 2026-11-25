"""Finds the Melee NTSC 1.02 disc image without anyone's path being written into the repository.

In order: the MELEE_ISO environment variable, the disc the game remembered the last time it was
started by double-clicking (%LOCALAPPDATA%\\MeleeUnlocked\\standalone.ini), the one older builds
recorded (launcher.ini in the same folder), then melee.iso at the root of the checkout.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def remembered_iso():
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        return None
    for name in ("standalone.ini", "launcher.ini"):
        ini = Path(local) / "MeleeUnlocked" / name
        try:
            for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("iso="):
                    path = Path(line[4:].strip())
                    if path.is_file():
                        return path
        except OSError:
            pass
    return None


def find_iso(explicit=None):
    """Returns a Path, or None when nothing plausible is on this machine."""
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    env = os.environ.get("MELEE_ISO")
    if env and Path(env).is_file():
        return Path(env)
    path = remembered_iso()
    if path:
        return path
    local = ROOT / "melee.iso"
    return local if local.is_file() else None


def require_iso(explicit=None):
    path = find_iso(explicit)
    if not path:
        raise SystemExit(
            "cannot find the Melee NTSC 1.02 ISO. Pass --iso, set MELEE_ISO, put melee.iso at the "
            "root of the checkout, or run the game once so its path is remembered."
        )
    return path
