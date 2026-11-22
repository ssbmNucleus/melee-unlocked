#!/usr/bin/env python3
# Builds a fighter plugin from an m-ex fighter port's MexTK C, with a kit and a compiler only.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The driver of a fighter plugin kit (tools/mexft/mexft_kit.py makes the kit and puts this file
in its tools folder). It does what the game's own build does for a fighter in sourceport/game/mexft:

  1. prepares the port's C (mexft_prepare.py: raw console addresses named, exports found, the
     descriptor generated);
  2. compiles each module (the fighter, each article) and links it on its own (link_module.py:
     private globals, MexTK names renamed to the game's);
  3. compiles the descriptor;
  4. joins it all into the plugin (mexft_plugin.py).

usage: python tools/mexft_build.py --gcc-bin <mingw64>/bin --fighter <folder> --out <name>.mufighter
                                   [--kit <kit folder>] [--work <folder>]

<folder> holds the port's C and mexft.txt: "name <display name>", "file <PlXx.dat>", and
optionally "mex-name <the fighter's name in MxDt>" when that differs from the display name.
"""
import argparse
import contextlib
import glob
import io
import json
import os
import runpy
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('%s\nfailed: %s' % ((r.stdout + r.stderr).strip(), ' '.join(cmd[:3])))
    return r.stdout


def run_tool(name, args):
    """Runs a tool of the kit (a Python file beside this one) in this process and returns what it
    printed. Not as `python <file>`: a packaged application that uses the kit has no interpreter
    to start, only the one it is running in."""
    path = os.path.join(HERE, name)
    out = io.StringIO()
    argv = sys.argv
    sys.argv = [path] + [str(a) for a in args]
    try:
        with contextlib.redirect_stdout(out):
            runpy.run_path(path, run_name='__main__')
    except SystemExit as e:
        if e.code not in (None, 0):
            sys.exit('%s\n%s\nfailed: %s' % (out.getvalue().strip(), e.code, name))
    finally:
        sys.argv = argv
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gcc-bin', required=True, help="the compiler's bin folder (gcc, ld, nm, objcopy)")
    ap.add_argument('--fighter', required=True, help='the folder with the port\'s C and mexft.txt')
    ap.add_argument('--out', required=True)
    ap.add_argument('--kit', default=os.path.dirname(HERE))
    ap.add_argument('--work', help='where the intermediate files go (default: <out>.build)')
    a = ap.parse_args()

    kit = os.path.abspath(a.kit).replace('\\', '/')
    fighter = os.path.abspath(a.fighter)
    name = os.path.basename(os.path.normpath(fighter))
    work = os.path.abspath(a.work or a.out + '.build')
    exe = '.exe' if os.name == 'nt' else ''
    tool = {t: os.path.join(a.gcc_bin, t + exe) for t in ('gcc', 'ld', 'nm', 'objcopy')}
    for t, path in tool.items():
        if not os.path.isfile(path):
            sys.exit('no %s in %s' % (t, a.gcc_bin))
    manifest = os.path.join(fighter, 'mexft.txt')
    if not os.path.isfile(manifest):
        sys.exit('no mexft.txt in %s' % fighter)
    sources = sorted(glob.glob(os.path.join(fighter, '**', '*.c'), recursive=True))
    if not sources:
        sys.exit('no C source in %s' % fighter)
    flags = json.load(open(os.path.join(kit, 'flags.json')))

    def command(kind):
        f = flags[kind]
        return ([tool['gcc']] + f['defines'] + [w.replace('{kit}', kit) for w in f['flags']] +
                ['-I' + w.replace('{kit}', kit) for w in f['includes']])

    os.makedirs(work, exist_ok=True)
    data = os.path.join(kit, 'data')
    print(run_tool('mexft_prepare.py', ['--manifest', manifest, '--name', name,
               '--out', work, '--symbols', os.path.join(data, 'symbols.txt'),
               '--functions', os.path.join(data, 'ftFunction.txt'),
               '--functions', os.path.join(data, 'tmce', 'ftFunction.txt'),
               '--item-functions', os.path.join(data, 'itFunction.txt'),
               '--item-functions', os.path.join(data, 'tmce', 'itFunction.txt'),
               '--shim', os.path.join(kit, 'include', 'mexft', 'mex.h'),
               '--adapter', os.path.join(kit, 'include', 'mexft', 'mexft_adapter.h'),
               '--mextk', os.path.join(kit, 'include', 'MexTK_native'),
               '--mex-link', os.path.join(data, 'm-ex', 'melee.link'),
               '--mex-include', os.path.join(data, 'm-ex', 'include')] + sources).strip())

    objects = []
    for module in open(os.path.join(work, 'modules.txt')).read().split():
        compiled = []
        for i, src in enumerate(sorted(glob.glob(os.path.join(work, module, 'src', '*.c')))):
            obj = os.path.join(work, '%s_%d.o' % (module, i))
            run(command('module') + ['-c', src, '-o', obj])
            compiled.append(obj)
        linked = os.path.join(work, 'tmce_%s.o' % module)
        run_tool('link_module.py', ['--ld', tool['ld'], '--nm', tool['nm'],
             '--objcopy', tool['objcopy'], '--name', module, '--exports', os.path.join(work, module, 'exports.txt'),
             '--redefine', os.path.join(data, 'mextk_redefine.txt'), '--out', linked] + compiled)
        objects.append(linked)
    descriptor = os.path.join(work, 'mexft_%s.o' % name)
    run(command('descriptor') + ['-c', os.path.join(work, 'mexft_%s.c' % name), '-o', descriptor])

    mex_name = None
    for key in ('mex-name ', 'name '):
        for line in open(manifest, encoding='utf-8'):
            if line.startswith(key) and mex_name is None:
                mex_name = line[len(key):].strip()
    print(run_tool('mexft_plugin.py', ['--gcc', tool['gcc'], '--ld', tool['ld'],
               '--objcopy', tool['objcopy'], '--nm', tool['nm'],
               '--header-dir', os.path.join(kit, 'include', 'mexft'), '--abi-dir', os.path.join(kit, 'include', 'mexft'),
               '--name', name, '--fighter', 'mu_ak_mexft_%s=%s' % (name, mex_name or ''),
               '--out', os.path.abspath(a.out)] + objects + [descriptor]).strip())
    # mexft_plugin.py keeps its own intermediate files beside the plugin: they belong with the rest.
    shutil.rmtree(os.path.abspath(a.out) + '.work', ignore_errors=True)


if __name__ == '__main__':
    main()
