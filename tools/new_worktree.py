#!/usr/bin/env python3
"""A new worktree that builds in minutes, with its own test install and port range: one command.

    python tools/new_worktree.py my-branch                   # ..\\mu-my-branch, branch my-branch from netlee-main
    python tools/new_worktree.py my-branch --path ..\\mu-x --seed ..\\mu-clean
    python tools/new_worktree.py --deploy                    # in a worktree: copy its build into its test install
    python tools/new_worktree.py --list                      # worktrees made here, their installs and ports
    python tools/new_worktree.py --remove ..\\mu-x            # remove one made here (worktree, build, install, ports)

In order, each step timed:
  1. git worktree add PATH -b BRANCH BASE (BASE: netlee-main).
  2. The decomp submodule: tools/decomp_fork.py setup --reference SEED/sourceport/extern/melee, so the
     private fork is cloned borrowing the seed worktree's objects.
  3. Host build folder build-host (MSVC): the seed worktree's own MSVC build folder is copied with its
     timestamps, its absolute paths rewritten to the new worktree, and every checked-out file whose
     content equals the seed's committed file gets the seed file's timestamp. MSBuild then recompiles
     only the files that differ from the seed. (A CMake build folder is tied to its source path; these
     are the places that path is kept: CMakeCache.txt, the project files, MSBuild's .tlog files.)
  4. Game build folder build-gcc (MinGW, Ninja): configured fresh, compiling through ccache
     (C:/toolchains/mingw64/bin/ccache.exe, base_dir = the folder holding the worktrees, so paths
     inside any worktree hash the same, and hash_dir=false, so the build folder's own path does not).
     Every object some worktree compiled with the same settings is a cache hit.
     A Ninja folder cannot be copied like the host's: its object names and log hashes hold the path.
  5. Builds melee_game + mexft_plugins (ninja -j4) and melee_source (/m:3) side by side, puts
     melee_game.dll beside melee_source.exe and checks the pair with --print-info.
  6. Its private test install: a copy of pm-test/ui-install2 as pm-test/<worktree name>-install, with
     this build in it (--deploy repeats only this part after later builds).
  7. A range of 100 ports of its own, from 43000 up, kept in the main repository's git folder
     (netlee-worktrees.json) and printed; give its first port to tools/p2p_pair.py --base-port.

--cold skips the seeding (no host copy, ccache disabled for the game) to measure a cold build. The
seed is any built worktree (default: the one this script runs from); its MSVC folder is the
build* folder configured for it that holds the newest melee_source.exe. Seeds whose game folder
did not use ccache still help the host; the game cache fills from the first worktree that builds.
A seed with no build folders at all (mu-guide, say) still works: the host is then configured from
cold.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = Path(r'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions'
             r'\Microsoft\CMake\CMake\bin\cmake.exe')
MINGW = Path(r'C:\toolchains\mingw64')
NINJA = MINGW / 'bin' / 'ninja.exe'
CCACHE = MINGW / 'bin' / 'ccache.exe'
ROBOCOPY = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'robocopy.exe'
GAME_DIR = 'build-gcc'
HOST_DIR = 'build-host'
PORT_FIRST, PORT_BLOCK, PORT_LAST = 43000, 100, 49900
# Files of a build folder that never hold its source path in a way a build checks (and are large).
BINARY_SUFFIXES = {'.obj', '.o', '.pdb', '.lib', '.a', '.exe', '.dll', '.ilk', '.exp', '.pch', '.ipch', '.idb',
                   '.res', '.iobj', '.ipdb', '.dxil', '.map', '.dbg', '.png', '.bin', '.zip', '.gci', '.dat'}
GAME_OUTPUTS = ('melee_game.dll', 'melee_game.snapexcl', 'melee_game.dbg', 'melee_game.syms', 'melee_game.map')
# Training Mode CE's data files, which the game's build writes beside the library (TM/*.dat); the
# host loads them from beside the library it runs.
TRAINING_DIR = 'TM'


def copy_training_files(game_dir: Path, dest: Path) -> int:
    files = sorted((game_dir / TRAINING_DIR).glob('*.dat'))
    if files:
        (dest / TRAINING_DIR).mkdir(exist_ok=True)
        for src in files:
            shutil.copy2(src, dest / TRAINING_DIR / src.name)
    return len(files)


class Failure(Exception):
    pass


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd, cwd=None, env=None, quiet=False, logfile: Path | None = None) -> None:
    cmd = [str(c) for c in cmd]
    if logfile:
        with open(logfile, 'w', encoding='utf-8', errors='replace') as out:
            result = subprocess.run(cmd, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    else:
        result = subprocess.run(cmd, cwd=cwd, env=env, capture_output=quiet, text=True)
    if result.returncode:
        detail = ''
        if logfile:
            detail = '\n'.join(logfile.read_text(encoding='utf-8', errors='replace').splitlines()[-25:])
        elif quiet:
            detail = (result.stdout or '') + (result.stderr or '')
        raise Failure('%s failed (exit %d)%s' % (' '.join(cmd[:4]), result.returncode,
                                                 (':\n' + detail.strip()) if detail.strip() else ''))


def git(*args, cwd=ROOT) -> str:
    result = subprocess.run(['git', '-C', str(cwd), *args], capture_output=True)
    if result.returncode:
        raise Failure('git %s failed: %s' % (' '.join(args[:3]), result.stderr.decode(errors='replace').strip()))
    return result.stdout.decode('utf-8', errors='replace')


def same_path(a, b) -> bool:
    return os.path.normcase(os.path.abspath(str(a))) == os.path.normcase(os.path.abspath(str(b)))


class Timer:
    def __init__(self):
        self.start = time.monotonic()
        self.steps: list[tuple[str, float]] = []

    def step(self, name: str, since: float) -> float:
        now = time.monotonic()
        self.steps.append((name, now - since))
        log('  %s: %.1f s' % (name, now - since))
        return now


# ---- registry: worktrees made here, their installs and ports ----

def registry_path() -> Path:
    common = git('rev-parse', '--git-common-dir').strip()
    path = Path(common)
    if not path.is_absolute():
        path = (ROOT / path).resolve()
    return path / 'netlee-worktrees.json'


def load_registry() -> dict:
    try:
        return json.loads(registry_path().read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_registry(reg: dict) -> None:
    registry_path().write_text(json.dumps(reg, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def allocate_ports(reg: dict) -> int:
    taken = {entry['ports'][0] for path, entry in reg.items() if Path(path).exists()}
    for base in range(PORT_FIRST, PORT_LAST + 1, PORT_BLOCK):
        if base not in taken:
            return base
    raise Failure('no free port block between %d and %d' % (PORT_FIRST, PORT_LAST + PORT_BLOCK - 1))


# ---- the seed worktree's build folders ----

def cache_values(build: Path) -> dict:
    values = {}
    try:
        text = (build / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
    except OSError:
        return values
    for line in text.splitlines():
        m = re.match(r'^([A-Za-z0-9_.+-]+):([A-Z_]+)=(.*)$', line)
        if m:
            values[m.group(1)] = m.group(3)
    return values


def find_seed_builds(seed: Path) -> tuple[Path | None, Path | None]:
    """(MSVC host folder, Ninja game folder) of the seed worktree: the build* folder configured for it
    holding the newest melee_source.exe / melee_game.dll."""
    host, game = None, None
    host_time, game_time = 0.0, 0.0
    for build in sorted(seed.glob('build*')):
        if not build.is_dir():
            continue
        values = cache_values(build)
        home = values.get('CMAKE_HOME_DIRECTORY')
        if not home:
            continue
        # port/ is configured by default with MSVC x64 (MELEE_BUILD_EXPERIMENTAL_PORT, the old name).
        if same_path(home, seed) and values.get('CMAKE_GENERATOR', '').startswith('Visual Studio') \
                and values.get('MELEE_BUILD_EXPERIMENTAL_PORT', 'ON') != 'OFF':
            exe = build / 'port' / 'Release' / 'melee_source.exe'
            if exe.exists() and exe.stat().st_mtime > host_time:
                host, host_time = build, exe.stat().st_mtime
        elif same_path(home, seed / 'sourceport' / 'game') and values.get('CMAKE_GENERATOR') == 'Ninja':
            dll = build / 'melee_game.dll'
            if dll.exists() and dll.stat().st_mtime > game_time:
                game, game_time = build, dll.stat().st_mtime
    return host, game


# ---- copying a build folder to another worktree ----

def path_pattern(old: Path) -> re.Pattern:
    """Every spelling of the worktree path a build folder keeps: / or \\ (or \\\\ in escaped text), any case."""
    parts = [re.escape(p) for p in str(old).replace('\\', '/').split('/') if p]
    sep = r'(?:/|\\\\|\\)'
    return re.compile(sep.join(parts) + r'(?=[/\\"\';:\s|<>,)]|$)', re.IGNORECASE | re.MULTILINE)


def rewrite_paths(build: Path, old: Path, new: Path) -> int:
    pattern = path_pattern(old)
    new_fwd = str(new).replace('\\', '/')
    new_back = str(new).replace('/', '\\')

    def replace(m: re.Match) -> str:
        text = m.group(0)
        if '\\\\' in text:
            value = new_back.replace('\\', '\\\\')
        elif '\\' in text:
            value = new_back
        else:
            value = new_fwd
        return value.upper() if text.isupper() else value

    changed = 0
    for dirpath, _dirs, files in os.walk(build):
        for name in files:
            path = Path(dirpath) / name
            if path.suffix.lower() in BINARY_SUFFIXES:
                continue
            try:
                raw = path.read_bytes()
            except OSError:
                continue
            if len(raw) > 64 * 1024 * 1024:
                continue
            if raw.startswith(b'\xff\xfe'):
                encoding = 'utf-16-le'
                body = raw[2:]
                prefix = b'\xff\xfe'
            else:
                encoding = 'utf-8'
                body = raw
                prefix = b''
                if b'\0' in raw[:4096]:
                    continue    # some other binary file
            text = body.decode(encoding, errors='surrogateescape')
            new_text, count = pattern.subn(replace, text)
            if count:
                stat = path.stat()
                path.write_bytes(prefix + new_text.encode(encoding, errors='surrogateescape'))
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))   # the folder's own timestamps stand
                changed += 1
    return changed


def tracked_blobs(repo: Path) -> dict[str, str]:
    out = git('ls-files', '-s', '-z', cwd=repo)
    blobs = {}
    for entry in out.split('\0'):
        if not entry:
            continue
        meta, path = entry.split('\t', 1)
        mode, blob, stage = meta.split()
        if stage == '0' and mode != '160000':
            blobs[path] = blob
    return blobs


def modified_paths(repo: Path) -> set[str]:
    out = git('status', '--porcelain=v1', '-z', '--untracked-files=no', '--ignore-submodules=all', cwd=repo)
    paths = set()
    items = out.split('\0')
    i = 0
    while i < len(items):
        item = items[i]
        if len(item) > 3:
            paths.add(item[3:])
            if item[0] in 'RC':
                i += 1   # the rename's source follows
        i += 1
    return paths


def borrow_timestamps(seed: Path, new: Path) -> int:
    """Give each checked-out file of NEW the timestamp of the same file in SEED when both hold the same
    committed content (and the seed's copy is unmodified), so the copied build folder finds it built."""
    done = 0
    for sub in ('', 'sourceport/extern/melee'):
        a, b = seed / sub if sub else seed, new / sub if sub else new
        if not (a / '.git').exists() or not (b / '.git').exists():
            continue
        seed_blobs, new_blobs = tracked_blobs(a), tracked_blobs(b)
        dirty = modified_paths(a)
        for path, blob in new_blobs.items():
            if seed_blobs.get(path) != blob or path in dirty:
                continue
            try:
                st = os.stat(a / path)
                os.utime(b / path, ns=(st.st_atime_ns, st.st_mtime_ns))
                done += 1
            except OSError:
                pass
    return done


def robocopy(src: Path, dst: Path, excludes=(), exclude_files=()) -> None:
    cmd = [ROBOCOPY, src, dst, '/E', '/COPY:DAT', '/DCOPY:T', '/MT:8', '/R:1', '/W:1', '/NFL', '/NDL', '/NJH',
           '/NJS', '/NP']
    if excludes:
        cmd += ['/XD', *excludes]
    if exclude_files:
        cmd += ['/XF', *exclude_files]
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if result.returncode >= 8:   # robocopy: 0-7 success, 8+ failure
        raise Failure('robocopy %s -> %s failed (%d): %s' % (src, dst, result.returncode, result.stdout[-2000:]))


# ---- configure and build ----

def ccache_launcher(projects: Path) -> str:
    """The game's compiler launcher. base_dir makes the paths inside any worktree hash the same.
    hash_dir=false leaves the build folder's own path out of the hash: the game is built with debug
    information, and ccache would otherwise hash that path, because the build's -ffile-prefix-map
    (written with /) never matches the folder as ccache sees it (with \\) on Windows."""
    return '%s;base_dir=%s;hash_dir=false' % (CCACHE.as_posix(), projects.as_posix())


def configure_game(new: Path, seed_game: Path | None, cold: bool) -> None:
    build = new / GAME_DIR
    build.mkdir(exist_ok=True)
    # Training Mode CE's pinned upstream commit is fetched at configure time unless the folder holds it.
    if not cold and seed_game is not None and (seed_game / 'tmce-upstream' / '.git').exists() \
            and not (build / 'tmce-upstream').exists():
        robocopy(seed_game / 'tmce-upstream', build / 'tmce-upstream')
    args = [CMAKE, '-S', new / 'sourceport' / 'game', '-B', build, '-G', 'Ninja',
            '-DCMAKE_TOOLCHAIN_FILE=%s' % (new / 'sourceport' / 'cmake' / 'mingw-w64-x86_64.cmake').as_posix(),
            '-DMELEE_MINGW_ROOT=%s' % MINGW.as_posix(), '-DMEXFT_PLUGINS=ALL']
    if not cold and CCACHE.exists():
        args.append('-DCMAKE_C_COMPILER_LAUNCHER=%s' % ccache_launcher(new.parent))
    run(args, logfile=new / (GAME_DIR + '.configure.log'))


def seed_host(new: Path, seed: Path, seed_host_dir: Path) -> None:
    dst = new / HOST_DIR
    robocopy(seed_host_dir, dst, excludes=['Testing'], exclude_files=['*.log'])
    # The build folder first: once the seed's root is rewritten, its build folder's path is gone.
    rewrite_paths(dst, seed_host_dir, dst)   # a seed folder not named build-host
    rewrite_paths(dst, seed, new)


def configure_host_cold(new: Path, seed_host_dir: Path | None) -> None:
    """The host build folder (MELEE_BUILD_EXPERIMENTAL_PORT is on by default with MSVC; the name is
    kept for old build folders)."""
    args = [CMAKE, '-S', new, '-B', new / HOST_DIR, '-G', 'Visual Studio 17 2022', '-A', 'x64',
            '-DMELEE_BUILD_EXPERIMENTAL_PORT=ON']
    if seed_host_dir is not None:
        values = cache_values(seed_host_dir)
        for key in ('MELEE_SQUAD_STRIKE', 'MELEE_STREAMLINE_ROOT',
                    'MELEE_CPU_BASELINE', 'MELEE_NO_SLIPPI'):
            value = values.get(key)
            if value and not value.lower().startswith(str(seed_host_dir.parent).replace('\\', '/').lower()):
                args.append('-D%s=%s' % (key, value))
    run(args, logfile=new / (HOST_DIR + '.configure.log'))


def build_game(new: Path, jobs: int, cold: bool) -> None:
    env = dict(os.environ)
    if cold:
        env['CCACHE_DISABLE'] = '1'
    run([NINJA, '-C', new / GAME_DIR, '-j%d' % jobs, 'melee_game', 'mexft_plugins'], env=env,
        logfile=new / (GAME_DIR + '.build.log'))


def build_host(new: Path, jobs: int) -> None:
    run([CMAKE, '--build', new / HOST_DIR, '--config', 'Release', '--target', 'melee_source', '--',
         '/m:%d' % jobs, '/nologo', '/v:m'], logfile=new / (HOST_DIR + '.build.log'))


def parallel(jobs: list) -> None:
    errors = []

    def wrap(fn):
        try:
            fn()
        except Exception as exc:   # noqa: BLE001 - reported below
            errors.append(exc)

    threads = [threading.Thread(target=wrap, args=(fn,)) for fn in jobs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise errors[0]


def pair_outputs(new: Path) -> dict:
    exe_dir = new / HOST_DIR / 'port' / 'Release'
    for name in GAME_OUTPUTS:
        src = new / GAME_DIR / name
        if src.exists():
            shutil.copy2(src, exe_dir / name)
    copy_training_files(new / GAME_DIR, exe_dir)
    result = subprocess.run([str(exe_dir / 'melee_source.exe'), '--print-info'], capture_output=True, text=True,
                            timeout=60, cwd=exe_dir)
    if result.returncode:
        raise Failure('melee_source.exe --print-info exited %d: %s' % (result.returncode, result.stdout + result.stderr))
    return json.loads(result.stdout)


# ---- the test install ----

def install_path(new: Path) -> Path:
    return ROOT.parent / 'pm-test' / (new.name + '-install')


def make_install(new: Path, template: Path) -> Path:
    install = install_path(new)
    if install.exists():
        log('  %s already exists: kept' % install)
        return install
    robocopy(template, install, excludes=['runs', 'streamline-logs'],
             exclude_files=['*.log', 'melee_port_crash.txt', 'plugin-approvals.json'])
    return install


def running_from(folder: Path) -> list[str]:
    """Programs running from FOLDER (by executable path); a copy must not replace their files."""
    script = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and "
              "$_.ExecutablePath.StartsWith('%s', 'OrdinalIgnoreCase') } | "
              "ForEach-Object { \"$($_.ProcessId) $($_.ExecutablePath)\" }" % (str(folder).rstrip('\\') + '\\'))
    pwsh = shutil.which('pwsh') or shutil.which('powershell')
    result = subprocess.run([pwsh, '-NoProfile', '-NonInteractive', '-Command', script], capture_output=True, text=True)
    return [line for line in result.stdout.splitlines() if line.strip()]


def deploy(new: Path, install: Path, host_dir: Path | None = None, game_dir: Path | None = None) -> list[str]:
    busy = running_from(install)
    if busy:
        raise Failure('not deployed: running from %s: %s' % (install, '; '.join(busy)))
    host_dir = host_dir or new / HOST_DIR
    game_dir = game_dir or new / GAME_DIR
    exe_dir = host_dir / 'port' / 'Release'
    copied = []
    for src in [exe_dir / 'melee_source.exe', *sorted(exe_dir.glob('*.dxil'))]:
        if src.exists():
            shutil.copy2(src, install / src.name)
            copied.append(src.name)
    for name in GAME_OUTPUTS:
        src = game_dir / name
        if src.exists():
            shutil.copy2(src, install / name)
            copied.append(name)
    training = copy_training_files(game_dir, install)
    if training:
        copied.append('TM/*.dat (%d)' % training)
    plugins = sorted((game_dir / 'fighters').glob('*.mufighter'))
    if plugins:
        (install / 'fighters').mkdir(exist_ok=True)
        for src in plugins:
            shutil.copy2(src, install / 'fighters' / src.name)
        copied.append('fighters/*.mufighter (%d)' % len(plugins))
    return copied


# ---- commands ----

def cmd_new(args) -> int:
    timer = Timer()
    seed = Path(args.seed).resolve() if args.seed else ROOT
    new = Path(args.path).resolve() if args.path else (ROOT.parent / ('mu-' + args.branch.replace('/', '-')))
    if new.exists():
        raise Failure('%s already exists' % new)
    reg = load_registry()
    base_port = allocate_ports(reg)
    seed_host_dir, seed_game_dir = find_seed_builds(seed)
    log('new worktree %s, branch %s from %s; seed %s (host %s, game %s)%s'
        % (new, args.branch, args.base, seed, seed_host_dir.name if seed_host_dir else '-',
           seed_game_dir.name if seed_game_dir else '-', ' COLD' if args.cold else ''))
    t = time.monotonic()
    exists = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--verify', '--quiet', 'refs/heads/' + args.branch],
                            capture_output=True).returncode == 0
    if exists:
        run(['git', '-C', ROOT, 'worktree', 'add', new, args.branch], quiet=True)
    else:
        run(['git', '-C', ROOT, 'worktree', 'add', new, '-b', args.branch, args.base], quiet=True)
    reg[str(new)] = {'branch': args.branch, 'ports': [base_port, base_port + PORT_BLOCK - 1],
                     'install': str(install_path(new)), 'created': time.strftime('%Y-%m-%d %H:%M')}
    save_registry(reg)
    t = timer.step('worktree', t)

    reference = seed / 'sourceport' / 'extern' / 'melee'
    # This tree's decomp_fork.py, pointed at the new worktree (its own copy may predate fixes here).
    setup = [sys.executable, '-B', ROOT / 'tools' / 'decomp_fork.py', 'setup', '--worktree', new]
    if (reference / '.git').exists():
        setup += ['--reference', reference]
    run(setup, cwd=new, quiet=True)
    t = timer.step('submodule', t)

    if not args.cold and seed_host_dir is not None:
        seed_host(new, seed, seed_host_dir)
        t = timer.step('host folder copied, paths rewritten', t)
        n = borrow_timestamps(seed, new)
        t = timer.step('timestamps of %d unchanged files' % n, t)
    else:
        configure_host_cold(new, seed_host_dir)
        t = timer.step('host configured', t)
    configure_game(new, seed_game_dir, args.cold)
    t = timer.step('game configured', t)

    if args.no_build:
        info = None
    else:
        if args.serial:
            build_game(new, args.game_jobs, args.cold)
            t = timer.step('game built', t)
            build_host(new, args.host_jobs)
            t = timer.step('host built', t)
        else:
            parallel([lambda: build_game(new, args.game_jobs, args.cold), lambda: build_host(new, args.host_jobs)])
            t = timer.step('game and host built', t)
        info = pair_outputs(new)
        t = timer.step('melee_source.exe --print-info', t)
    total_build = time.monotonic() - timer.start

    install = None
    if not args.no_install:
        install = make_install(new, Path(args.install_from))
        if info is not None:
            deploy(new, install)
        t = timer.step('test install', t)

    log('')
    log('worktree   %s (branch %s)' % (new, args.branch))
    if info is not None:
        log('runnable   %s  (game library %s, plugin ABI %s)'
            % (new / HOST_DIR / 'port' / 'Release' / 'melee_source.exe', info.get('game_library'), info.get('plugin_abi')))
    log('builds     game: ninja -C %s -j4 melee_game mexft_plugins' % (new / GAME_DIR))
    log('           host: cmake --build %s --config Release --target melee_source -- /m:3' % (new / HOST_DIR))
    if install:
        log('install    %s   (redeploy after a build: python tools/new_worktree.py --deploy)' % install)
    log('ports      %d-%d   (e.g. tools/p2p_pair.py --base-port %d)' % (base_port, base_port + PORT_BLOCK - 1, base_port))
    log('time       %.0f s to a runnable build, %.0f s in all' % (total_build, time.monotonic() - timer.start))
    return 0


def cmd_deploy(args) -> int:
    here = Path(git('rev-parse', '--show-toplevel', cwd=Path.cwd()).strip()).resolve()
    reg = load_registry()
    entry = next((e for p, e in reg.items() if same_path(p, here)), None)
    install = Path(args.install_to) if args.install_to else Path(entry['install']) if entry else install_path(here)
    if not install.exists():
        raise Failure('%s does not exist (make it with this script, or name one with --install-to)' % install)
    # This worktree's own build folders (build-host and build-gcc, or the newest configured for it).
    host_dir, game_dir = find_seed_builds(here)
    if host_dir is None or game_dir is None:
        raise Failure('no built host and game folders found in %s' % here)
    copied = deploy(here, install, host_dir, game_dir)
    log('deployed to %s: %s' % (install, ', '.join(copied)))
    return 0


def cmd_list(args) -> int:
    for path, entry in sorted(load_registry().items(), key=lambda kv: kv[1]['ports'][0]):
        log('%-55s %-24s ports %d-%d  %s%s' % (path, entry['branch'], entry['ports'][0], entry['ports'][1],
                                               entry['install'], '' if Path(path).exists() else '  (gone)'))
    return 0


def cmd_remove(args) -> int:
    target = Path(args.remove).resolve()
    reg = load_registry()
    key = next((p for p in reg if same_path(p, target)), None)
    if key is None:
        raise Failure('%s was not made by this script; remove it by hand' % target)
    install = Path(reg[key]['install'])
    for folder in (target, install):
        busy = running_from(folder) if folder.exists() else []
        if busy:
            raise Failure('not removed: running from %s: %s' % (folder, '; '.join(busy)))
    if target.exists():
        dirty = git('status', '--porcelain', '--untracked-files=no', cwd=target).strip()
        if dirty and not args.force:
            raise Failure('%s has uncommitted changes (--force removes it anyway):\n%s' % (target, dirty))
        run(['git', '-C', ROOT, 'worktree', 'remove', '--force', target], quiet=True)
    if target.exists():
        shutil.rmtree(target, ignore_errors=True)
    if install.exists():
        shutil.rmtree(install, ignore_errors=True)
    branch = reg[key]['branch']
    del reg[key]
    save_registry(reg)
    log('removed %s and %s (branch %s kept: git branch -D %s)' % (target, install, branch, branch))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('branch', nargs='?', help='branch for the new worktree (made from --base unless it exists)')
    ap.add_argument('--path', help='where (default ..\\mu-BRANCH)')
    ap.add_argument('--base', default='netlee-main')
    ap.add_argument('--seed', help='built worktree to seed from (default: this one)')
    ap.add_argument('--install-from', default=str(ROOT.parent / 'pm-test' / 'ui-install2'))
    ap.add_argument('--cold', action='store_true', help='no seeding: measure a cold build')
    ap.add_argument('--serial', action='store_true', help='build the game, then the host (default: side by side)')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--no-install', action='store_true')
    ap.add_argument('--game-jobs', type=int, default=4)
    ap.add_argument('--host-jobs', type=int, default=3)
    ap.add_argument('--deploy', action='store_true', help='copy this worktree\'s build into its test install')
    ap.add_argument('--install-to', help='with --deploy: another install folder')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--remove', metavar='PATH')
    ap.add_argument('--force', action='store_true', help='with --remove: even with uncommitted changes')
    args = ap.parse_args(argv)
    try:
        if args.deploy:
            return cmd_deploy(args)
        if args.list:
            return cmd_list(args)
        if args.remove:
            return cmd_remove(args)
        if not args.branch:
            ap.error('name the branch')
        return cmd_new(args)
    except Failure as exc:
        log('new_worktree: %s' % exc)
        return 1


if __name__ == '__main__':
    sys.exit(main())
