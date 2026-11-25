#!/usr/bin/env python3
# Packs what a fighter plugin is built from into one folder, for a tool outside this repository.
# SPDX-License-Identifier: GPL-2.0-or-later
"""A fighter plugin (sourceport/game/mexft/mexft_plugin.h) is compiled against this game's
layouts, not against a build of it. This writes everything that compile needs into a kit: a mod
tool (SSBM Nucleus) with the kit and a MinGW-w64 GCC makes a plugin from a port's MexTK C with
tools/mexft_build.py of the kit, with no checkout and no CMake.

    python tools/mexft/mexft_kit.py --build-dir build-sourceport-gcc --out mexft-kit

The build directory must be configured with MEXFT_PLUGINS naming at least one fighter: the
compile flags are read from its build.ninja (one fighter module, one descriptor), so the kit
compiles as this build does. A plugin made with the kit carries the kit's layout id
(mexft_abi.h), and a game loads it when its own id is the same.

kit/
  README.txt, abi.txt        what this is; the layout id
  flags.json                 the two compile commands' flags, paths relative to the kit
  tools/                     mexft_build.py (the driver), mexft_prepare.py, mexft_plugin.py, link_module.py
  data/                      symbols.txt, the ftFunction / itFunction lists, MexTK's redefine list, m-ex's melee.link and headers
  include/                   the headers: MexTK as overlaid, mexft/, and the game's
"""
import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HEADER = ('.h', '.inc', '.def', '.hpp')


def ninja_block(text, pattern):
    """DEFINES, FLAGS and INCLUDES of the first build statement whose output matches."""
    m = re.search(r'^build (\S*%s\S*\.obj):.*\n((?:  .*\n)+)' % pattern, text, re.M)
    if not m:
        sys.exit('no compile of %s in build.ninja (configure with -DMEXFT_PLUGINS=ALL)' % pattern)
    out = {}
    for line in m.group(2).splitlines():
        key, _, value = line.strip().partition(' = ')
        out[key] = value
    return out


def split(flags):
    """Ninja's shell words: quotes hold spaces, \\" is a quote inside a word."""
    words, cur, quoted, i = [], '', False, 0
    while i < len(flags):
        c = flags[i]
        if c == '\\' and i + 1 < len(flags) and flags[i + 1] == '"':
            cur += '"'
            i += 1
        elif c == '"':
            quoted = not quoted
        elif c == ' ' and not quoted:
            if cur:
                words.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    if cur:
        words.append(cur)
    return words


def copy_headers(src, dst):
    count = 0
    for folder, _dirs, names in os.walk(src):
        for name in names:
            if name.endswith(HEADER):
                rel = os.path.relpath(os.path.join(folder, name), src)
                os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
                shutil.copyfile(os.path.join(folder, name), os.path.join(dst, rel))
                count += 1
    return count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    build = os.path.abspath(a.build_dir)
    out = os.path.abspath(a.out)
    game = os.path.join(ROOT, 'sourceport', 'game')
    text = open(os.path.join(build, 'build.ninja'), encoding='utf-8', errors='replace').read().replace('$:', ':')

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'include'))
    known = {}   # an include folder of the build -> its folder in the kit
    total = 0

    def kit_dir(path):
        nonlocal total
        key = os.path.normcase(os.path.normpath(path))
        if key not in known:
            name = 'i%02d_%s' % (len(known), re.sub(r'[^A-Za-z0-9]+', '_', os.path.basename(os.path.normpath(path))))
            known[key] = 'include/' + name
            total += copy_headers(path, os.path.join(out, 'include', name))
        return known[key]

    def command(block):
        flags, includes = [], []
        words = split(block.get('FLAGS', ''))
        i = 0
        while i < len(words):
            w = words[i]
            if w.startswith('-ffile-prefix-map='):
                pass
            elif w == '-include':
                path = words[i + 1]
                flags += ['-include', '{kit}/%s/%s' % (kit_dir(os.path.dirname(path)), os.path.basename(path))]
                i += 1
            elif w == '-g':
                flags.append('-g0')
            else:
                flags.append(w)
            i += 1
        for w in split(block.get('INCLUDES', '')):
            if w.startswith('-I'):
                includes.append('{kit}/' + kit_dir(w[2:]))
            elif w not in ('-isystem',):
                includes.append('{kit}/' + kit_dir(w))
        return {'defines': split(block.get('DEFINES', '')), 'flags': flags, 'includes': includes}

    flags = {'module': command(ninja_block(text, r'tmce_mexft_\w+_objs\.dir')),
             'descriptor': command(ninja_block(text, r'mexft_desc_\w+\.dir'))}

    # The fighter's own headers and the lists the preparation reads.
    mexft_inc = os.path.join(out, 'include', 'mexft')
    os.makedirs(mexft_inc)
    for name in ('mex.h', 'mexft_adapter.h', 'mexft_plugin.h'):
        shutil.copyfile(os.path.join(game, 'mexft', name), os.path.join(mexft_inc, name))
    shutil.copyfile(os.path.join(build, 'mexft', 'abi', 'mexft_abi.h'), os.path.join(mexft_inc, 'mexft_abi.h'))
    # The adapter includes the descriptor's header as "../akaneia/mu_ak_fighter.h".
    os.makedirs(os.path.join(out, 'include', 'akaneia'))
    shutil.copyfile(os.path.join(game, 'akaneia', 'mu_ak_fighter.h'),
                    os.path.join(out, 'include', 'akaneia', 'mu_ak_fighter.h'))
    total += copy_headers(os.path.join(game, 'tmce', 'MexTK', 'include'), os.path.join(out, 'include', 'MexTK_native'))
    data = os.path.join(out, 'data')
    os.makedirs(os.path.join(data, 'tmce'))
    shutil.copyfile(os.path.join(ROOT, 'sourceport', 'extern', 'melee', 'config', 'GALE01', 'symbols.txt'),
                    os.path.join(data, 'symbols.txt'))
    for name in ('ftFunction.txt', 'itFunction.txt'):
        shutil.copyfile(os.path.join(HERE, name), os.path.join(data, name))
        shutil.copyfile(os.path.join(game, 'tmce', 'MexTK', name), os.path.join(data, 'tmce', name))
    shutil.copyfile(os.path.join(game, 'tmce', 'MexTK', 'mextk_redefine.txt'), os.path.join(data, 'mextk_redefine.txt'))
    shutil.copytree(os.path.join(HERE, 'm-ex'), os.path.join(data, 'm-ex'))
    tools = os.path.join(out, 'tools')
    os.makedirs(tools)
    for path in (os.path.join(HERE, 'mexft_build.py'), os.path.join(HERE, 'mexft_prepare.py'),
                 os.path.join(HERE, 'mexft_plugin.py'), os.path.join(ROOT, 'tools', 'tmce', 'link_module.py')):
        shutil.copyfile(path, os.path.join(tools, os.path.basename(path)))

    abi = re.search(r'MEXFT_ABI_TEXT "(\w+)"', open(os.path.join(mexft_inc, 'mexft_abi.h')).read()).group(1)
    json.dump(flags, open(os.path.join(out, 'flags.json'), 'w', newline='\n'), indent=1)
    open(os.path.join(out, 'abi.txt'), 'w', newline='\n').write(abi + '\n')
    open(os.path.join(out, 'README.txt'), 'w', newline='\n').write(
        'Nucleus fighter plugin kit, layout id %s\n\n'
        'Builds a fighter plugin (.mufighter) from an m-ex fighter port\'s MexTK C source:\n\n'
        '    python tools/mexft_build.py --gcc-bin <mingw64>/bin --fighter <folder> --out <name>.mufighter\n\n'
        '<folder> holds the port\'s C and a mexft.txt manifest ("name <display name>", "file <PlXx.dat>",\n'
        'optionally "mex-name <the fighter\'s name in MxDt>"). Needs a MinGW-w64 GCC 14 or newer.\n'
        'The plugin goes in the fighters folder beside melee_game.dll. A game loads it when its own\n'
        'layout id is this kit\'s; a game with another id says so in its log and keeps the fighter locked.\n' % abi)
    print('mexft kit %s: %d headers, %d include folders -> %s' % (abi, total, len(known), out))


if __name__ == '__main__':
    main()
