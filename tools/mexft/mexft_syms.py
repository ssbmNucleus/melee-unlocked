#!/usr/bin/env python3
# Writes the game library's symbols a fighter plugin may use, as "name address" lines.
# SPDX-License-Identifier: GPL-2.0-or-later
"""usage: mexft_syms.py --nm NM --library melee_game.dll --out melee_game.syms melee_game.dbg

The file the game reads beside its library when it has fighter plugins to link
(sourceport/game/mexft/mexft_plugins.c): "# build <id>" (the library's build id, so a file of
another build is refused), then one line per symbol a plugin may use: its name and its address in
hex as the library is loaded, sorted by name as strcmp orders them.

A plugin is a fighter's code, and a fighter's code calls the game. So the file lists the globals
the game itself defines, and leaves out what would let a plugin reach past the game:
  - everything the library imports from the system (files, processes, libraries, the
    environment, memory), except the pure helpers in SAFE_IMPORTS;
  - the game's own doors to the host program (DENY);
  - the C runtime's and the linker's internals.
A plugin that names anything left out is refused at load: "this game has no symbol ...". This
narrows what downloaded fighter code can do by name. It is not a sandbox: the code still runs in
the game's process.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mexft_plugin import build_id

# Imported functions with no effect outside their arguments: arithmetic and memory copies.
SAFE_IMPORTS = {
    'memcpy', 'memmove', 'memset', 'memcmp', 'memchr', 'strlen', 'strcmp', 'strncmp', 'strchr', 'strrchr',
    'sqrtf', 'sqrt', 'fabs', 'fabsf', 'sinf', 'cosf', 'tanf', 'atan2f', 'atanf', 'asinf', 'acosf', 'powf', 'floorf',
    'ceilf', 'fmodf', 'sin', 'cos', 'tan', 'atan2', 'atan', 'asin', 'acos', 'pow', 'floor', 'ceil', 'fmod',
}
# The game's own ways to the host program and to the plugin loader.
DENY = re.compile(r'^(mu_host|mu_online_abi_command$|mu_slippi_command$|mu_test_getenv$|mexft_plugin|mexft_plugins_)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nm', required=True)
    ap.add_argument('--library', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('image')
    a = ap.parse_args()
    rows, imports = {}, set()
    for line in subprocess.run([a.nm, a.image], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        if parts[2].startswith('__imp_'):
            imports.add(parts[2][len('__imp_'):])
        elif parts[1] in 'TDBRC':
            rows[parts[2]] = parts[0]
    kept, left_out = {}, 0
    for name, address in rows.items():
        system = name in imports and name not in SAFE_IMPORTS
        internal = name.startswith('_') or name.startswith('.')
        if system or internal or DENY.search(name):
            left_out += 1
            continue
        kept[name] = address
    with open(a.out, 'w', newline='\n') as f:
        f.write('# build %s\n' % build_id(a.library).hex())
        for name in sorted(kept, key=lambda n: n.encode()):
            f.write('%s %s\n' % (name, kept[name].lstrip('0') or '0'))
    print('mexft: %d game symbols for plugins (%d left out: system imports, host doors, internals) -> %s' % (
        len(kept), left_out, a.out))


if __name__ == '__main__':
    main()
