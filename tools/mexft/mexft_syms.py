#!/usr/bin/env python3
# Writes the game library's global symbols as "name address" lines, for the fighter plugin loader.
# SPDX-License-Identifier: GPL-2.0-or-later
"""usage: mexft_syms.py --nm NM --library melee_game.dll --out melee_game.syms melee_game.dbg

The file the game reads beside its library when it has fighter plugins to link
(sourceport/game/mexft/mexft_plugins.c): "# build <id>" (the library's build id, so a file of
another build is refused), then one line per global the library defines (code, data, read-only
data, zeroed data): its name and its address in hex as the library is loaded, sorted by name as
strcmp orders them."""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mexft_plugin import build_id


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nm', required=True)
    ap.add_argument('--library', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('image')
    a = ap.parse_args()
    rows = {}
    for line in subprocess.run([a.nm, a.image], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TDBRC':
            rows[parts[2]] = parts[0]
    with open(a.out, 'w', newline='\n') as f:
        f.write('# build %s\n' % build_id(a.library).hex())
        for name in sorted(rows, key=lambda n: n.encode()):
            f.write('%s %s\n' % (name, rows[name].lstrip('0') or '0'))
    print('mexft: %d game symbols -> %s' % (len(rows), a.out))


if __name__ == '__main__':
    main()
