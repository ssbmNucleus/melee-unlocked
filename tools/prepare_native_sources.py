"""Prepare the game sources in sourceport/extern/melee before building.

A public tree pins the public decompilation at BASE and carries Netlee's changes to it as
sourceport/patches/melee-native.patch: this applies that patch, as it always has.

A development tree's submodule is the private working fork, whose commits already hold those changes
(docs/decomp-fork-workflow.md). There is nothing to apply, and a patch is never applied on top of fork
sources: this checks the checkout, says so, and refreshes the generated, untracked copy of the patch
that tests read (tools/decomp_fork.py export-patch --worktree). --check only checks and writes nothing.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = '8e65295479b697e86021012707178df1fc51e7bd'
PATCH = 'sourceport/patches/melee-native.patch'


def fork_sources(decomp, head, git, check_only):
    """A checkout of the working fork: real sources, never patched."""
    if git('cat-file', '-e', BASE).returncode == 0 and git('merge-base', '--is-ancestor', BASE, head).returncode:
        raise SystemExit('%s is at %s, which is neither the public pin %s nor a fork commit built on it'
                         % (decomp, head[:12], BASE[:12]))
    tracked = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '--error-unmatch', PATCH], capture_output=True)
    if tracked.returncode == 0:
        # An old-workflow merge brought the tracked patch back: its changes are not in the fork, and
        # applying it on top of fork sources would apply most of it twice.
        raise SystemExit('%s is tracked in this development tree again (an old-workflow merge?). Its changes are '
                         'not applied to fork sources: bring them over with python tools/decomp_fork.py '
                         'import-branch, then git rm --cached %s (docs/decomp-fork-workflow.md).' % (PATCH, PATCH))
    dirty = [line for line in git('status', '--porcelain').stdout.splitlines() if line.strip()]
    print('Native Melee sources are already prepared.')
    print('Development tree: %s is the working fork at %s%s; nothing to apply.'
          % (decomp.relative_to(ROOT).as_posix() if decomp.is_relative_to(ROOT) else decomp, head[:12],
             ', with %d uncommitted files' % len(dirty) if dirty else ''))
    if check_only or decomp.resolve() != (ROOT / 'sourceport/extern/melee').resolve():
        return
    if git('cat-file', '-e', BASE).returncode:
        print('(%s not refreshed: the public pin is not in this clone of the fork)' % PATCH)
        return
    refresh = subprocess.run([sys.executable, str(ROOT / 'tools/decomp_fork.py'), 'export-patch', '--worktree'],
                             capture_output=True, text=True)
    if refresh.returncode:
        print('(%s not refreshed: %s)' % (PATCH, (refresh.stderr or refresh.stdout).strip()))
    else:
        print('(local copy of %s refreshed from the working tree, for the tests that read it)' % PATCH)


def prepare(decomp, archive=None, check_only=False):
    patch = ROOT / PATCH
    def git(*args):
        return subprocess.run(['git', '-C', str(decomp), *args], capture_output=True, text=True)
    if not archive and (decomp / '.git').exists():
        head = git('rev-parse', 'HEAD').stdout.strip()
        if head and head != BASE:
            return fork_sources(decomp, head, git, check_only)
    if check_only:
        if not (decomp / '.git').exists():
            print('Melee sources at %s are not a git checkout: not checked.' % decomp)
            return
        if not patch.is_file():
            raise SystemExit('%s is the public pin and there is no %s to apply' % (decomp, PATCH))
        if git('apply', '--reverse', '--check', str(patch)).returncode == 0:
            print('Native Melee sources are already prepared.')
            return
        raise SystemExit('%s holds the public decompilation without the native patch: run python '
                         'tools/prepare_native_sources.py first' % decomp)
    if archive and git('rev-parse', '--show-toplevel').stdout.strip() != str(decomp.resolve()):
        subprocess.run(['git', '-C', str(decomp), 'init', '-q'], check=True)
    if git('apply', '--reverse', '--check', str(patch)).returncode == 0:
        print('Native Melee sources are already prepared.')
        return
    if archive:
        # git archives lack .git. Verify both their embedded pinned commit and
        # every extracted file before applying; never accept an arbitrary tree.
        with archive.open('rb') as stream:
            commit = subprocess.run(['git', 'get-tar-commit-id'], stdin=stream,
                                    capture_output=True, text=True)
        if commit.returncode or commit.stdout.strip() != BASE:
            raise SystemExit('Archive is not the pinned Melee commit')
        with tarfile.open(archive) as tree:
            for member in tree.getmembers():
                if member.isdir():
                    continue
                if not member.isfile() or Path(member.name).is_absolute() or '..' in Path(member.name).parts:
                    raise SystemExit('Unsupported archive member')
                path = decomp / member.name
                if not path.is_file() or path.is_symlink() or path.read_bytes() != tree.extractfile(member).read():
                    raise SystemExit('Extracted Melee sources differ from the pinned archive')
    else:
        head = git('rev-parse', 'HEAD')
        if head.returncode or head.stdout.strip() != BASE:
            raise SystemExit('Initialize the pinned sources first: git submodule update --init sourceport/extern/melee')
        status = git('status', '--porcelain', '--untracked-files=no')
        if status.returncode or status.stdout.strip():
            raise SystemExit('Melee sources have local changes. Use a clean checkout before applying the native patch.')
    if not patch.is_file():
        raise SystemExit('No %s to apply to the public pin' % PATCH)
    check = git('apply', '--check', str(patch))
    if check.returncode:
        raise SystemExit(check.stderr)
    result = git('apply', str(patch))
    if result.returncode:
        raise SystemExit(result.stderr)
    print('Prepared the released native Melee sources.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--decomp', type=Path, default=ROOT / 'sourceport/extern/melee')
    parser.add_argument('--archive', type=Path, help='pinned git archive used to populate --decomp')
    parser.add_argument('--check', action='store_true', help='check that the sources are prepared; change nothing')
    args = parser.parse_args()
    prepare(args.decomp.resolve(), args.archive, args.check)
