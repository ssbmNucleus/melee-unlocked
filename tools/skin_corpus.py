#!/usr/bin/env python3
# Corpus run of the opponent-skin rule (host/costume_served_form.h) over a folder of skin zips.
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extracts the costume DAT of every skin zip under a Vault-like folder (<storage>/<Character>/*.zip,
read only) into --work, then runs port_skin_corpus on them against the retail disc and prints its
verdict table. One process, below-normal priority, one skin at a time.

    python -I -B tools/skin_corpus.py --exe build/port/Release/port_skin_corpus.exe \
        --iso "D:/.../Super Smash Bros. Melee (USA) (En,Ja) (v1.02).iso" \
        --storage "%LOCALAPPDATA%/SSBM Nucleus/storage" --work <scratch folder>

The first .dat or .usd member whose name starts with "Pl" (as the validator study took it) is the
skin's costume file. Writes <work>/list.tsv and <work>/corpus.csv.
"""
import argparse
import os
import subprocess
import sys
import zipfile
from pathlib import Path

SKIP = {"bundles", "custom_characters", "custom_stages", "das", "menus", "xdelta"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--storage", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    storage, work = Path(args.storage), Path(args.work)
    dats = work / "dats"
    dats.mkdir(parents=True, exist_ok=True)
    rows, skipped = [], 0
    for character in sorted(p for p in storage.iterdir() if p.is_dir() and not p.name.startswith("_") and p.name not in SKIP):
        for archive in sorted(character.glob("*.zip")):
            try:
                with zipfile.ZipFile(archive) as z:
                    names = [n for n in z.namelist() if Path(n).suffix.lower() in (".dat", ".usd")
                             and Path(n).name.lower().startswith("pl") and not n.startswith("__MACOSX")]
                    if not names:
                        skipped += 1
                        continue
                    data = z.read(names[0])
            except (OSError, zipfile.BadZipFile, KeyError):
                skipped += 1
                continue
            safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in f"{character.name}__{archive.stem}")
            target = dats / (safe + ".dat")
            target.write_bytes(data)
            rows.append((f"{character.name}/{archive.stem}", target))
    with open(work / "list.tsv", "w", encoding="utf-8", newline="\n") as f:
        for label, path in rows:
            f.write(f"{label}\t{path}\n")
    print(f"{len(rows)} skins extracted ({skipped} zips without a costume file)", flush=True)
    flags = 0x00004000 if os.name == "nt" else 0   # BELOW_NORMAL_PRIORITY_CLASS
    result = subprocess.run([str(Path(args.exe).resolve()), args.iso, str(work / "list.tsv"), str(work / "corpus.csv")], creationflags=flags)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
