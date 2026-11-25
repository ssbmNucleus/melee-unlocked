#!/usr/bin/env python3
"""Build the native Linux ELF game and host from the same source tree."""
# Based on Squad Strike by Chris (x9x9x9x9x9), https://github.com/x9x9x9x9x9/squad-strike v1.0.0, used with the author's permission (11-03-2026)
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Those targets are not registered by ctest in a Linux configuration.
LINUX_TEST_EXCLUSION_REASON = "Tests declared after the root Linux return() are unavailable to the Linux build."


def run(*arguments):
    command = [str(value) for value in arguments]
    print('+ ' + ' '.join(command), flush=True)
    subprocess.run(command, check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument('--cpu', choices=('SSE2', 'AVX', 'AVX2'), default='AVX2')
    parser.add_argument('--headless-build', action='store_true')
    parser.add_argument('--print-cmake', action='store_true', help='print configure commands without building (any platform)')
    parser.add_argument('--host-only', action='store_true', help='use the existing game build')
    parser.add_argument('--test', action='store_true')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-linux')
    parser.add_argument('--game-build-dir', type=Path, default=ROOT / 'build-linux-game')
    parser.add_argument('--tmce-dir', type=Path, help="an existing tmce-upstream folder (configure then fetches nothing)")
    args = parser.parse_args()
    if sys.platform != 'linux' and not args.print_cmake:
        parser.error('run this script with Linux Python (including Ubuntu under WSL)')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    full = not args.headless_build
    for name in ('gcc-14', 'g++-14', 'cmake', 'ninja'):
        if not args.print_cmake and not shutil.which(name):
            parser.error('missing build tool: ' + name)
    def configure(*command):
        if args.print_cmake:
            print('+ ' + ' '.join(str(x) for x in command), flush=True)
        else:
            run(*command)
    game_build = args.game_build_dir.resolve()
    host_build = args.build_dir.resolve()
    if not args.host_only:
        # The prepared game sources (tools/prepare_native_sources.py) when this tree has them, as the
        # Windows game build uses; otherwise the submodule.
        decomp = ROOT / 'build-native/decomp'
        extra = ['-DMELEE_DECOMP_DIR=' + str(decomp)] if (decomp / 'src/melee').is_dir() else []
        if args.tmce_dir:
            extra.append('-DTMCE_UPSTREAM_DIR=' + str(args.tmce_dir.resolve()))
        # -DMEXFT_PLUGINS=ALL, as releases and new_worktree.py configure it: no added fighter's code is
        # linked into melee_game.so. Linux builds and loads no fighter plugins (mexft_plugins.c scan()
        # is Windows only), so the library runs only the retail cast. A plain configure links all 15
        # m-ex fighters in, and find_fighter matches those by data file name before any plugin: an
        # installed fighter with the same file name then ran another fighter's code (traps.md).
        configure('cmake', '-S', ROOT / 'sourceport/game', '-B', game_build, '-G', 'Ninja',
            '-DCMAKE_C_COMPILER=gcc-14', '-DCMAKE_CXX_COMPILER=g++-14',
            '-DCMAKE_BUILD_TYPE=Release', '-DMELEE_CPU_BASELINE=' + args.cpu, '-DMEXFT_PLUGINS=ALL',
            *extra)
        if not args.print_cmake:
            run('cmake', '--build', game_build, '--target',
                'mu_game_tests' if args.test else 'melee_game', '-j', args.jobs)
        if args.test and not args.print_cmake:
            run('ctest', '--test-dir', game_build, '--output-on-failure')
    configure('cmake', '-S', ROOT, '-B', host_build, '-G', 'Ninja',
        '-DCMAKE_CXX_COMPILER=g++-14', '-DCMAKE_BUILD_TYPE=Release',
        '-DMELEE_BUILD_NATIVE_LINUX=ON', '-DMELEE_CPU_BASELINE=' + args.cpu,
        '-DMELEE_LINUX_GAME_LIBRARY=' + str(game_build / 'melee_game.so'),
        '-DMELEE_LINUX_GRAPHICS=' + ('OFF' if args.headless_build else 'ON'))
    if args.print_cmake:
        return
    targets = ['melee_linux'] + (['melee_' 'source'] if full else [])
    if full and args.cpu != 'AVX2':
        targets = ['melee_' 'source']   # the compatibility build: only the game, under its own names

    if args.test:
        targets = ['all']
    run('cmake', '--build', host_build, '--target', *targets, '-j', args.jobs)
    if args.test:
        run('ctest', '--test-dir', host_build, '--output-on-failure')
    print('Native executable: ' + str(host_build / 'port/linux/melee_linux'))


if __name__ == '__main__':
    main()
