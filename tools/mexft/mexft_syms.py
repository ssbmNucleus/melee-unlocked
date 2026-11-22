#!/usr/bin/env python3
# Writes the game library's global symbols as "name address" lines, for tools/mexft/mexft_plugin.py.
# SPDX-License-Identifier: GPL-2.0-or-later
"""usage: mexft_syms.py --nm NM --out FILE melee_game.dbg

One line per global the library defines (code, data, read-only data, zeroed data), its address in
hex as the library is loaded. A fighter plugin is linked against these."""
import argparse
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nm', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('image')
    a = ap.parse_args()
    rows = {}
    for line in subprocess.run([a.nm, a.image], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'TDBRC':
            rows[parts[2]] = parts[0]
    with open(a.out, 'w', newline='\n') as f:
        for name in sorted(rows):
            f.write('%s %s\n' % (name, rows[name]))
    print('mexft: %d game symbols -> %s' % (len(rows), a.out))


if __name__ == '__main__':
    main()
