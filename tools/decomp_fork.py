"""The game's decompiled sources in a development tree: a private working fork, and the public patch made from it.

In a development tree, sourceport/extern/melee is a private working fork of the public decompilation
(the URL is in .gitmodules). Netlee's changes to the game's files are ordinary commits there, on
branches named like the main repository's branches, and the main repository records which fork
commit it builds. Nothing from the fork is ever published: a public tree keeps today's layout, the
public decompilation at the pinned commit (BASE in tools/prepare_native_sources.py) plus
sourceport/patches/melee-native.patch, and this script generates that patch from the fork.
docs/decomp-fork-workflow.md has the day-to-day rules.

    python tools/decomp_fork.py status [--fetch]
        Where the submodule stands: its commit and branch, uncommitted files, whether the commit is
        pushed to the private fork, whether the main repository records it.
    python tools/decomp_fork.py verify [--snapshot DIR] [--rev MAIN-REV] [--patch FILE]
        The same checks as status, failing (exit 1) on any problem. --snapshot checks a public
        snapshot instead: public submodule URL and pin, no trace of the private fork, and a patch
        equal to an export of the fork commit. --rev checks that the fork commit imported for an
        old-workflow main-repository commit gives back that commit's patch.
    python tools/decomp_fork.py export-patch [--rev FORK-REV | --worktree] [--out FILE | --stdout] [--check FILE] [--base B]
        Write sourceport/patches/melee-native.patch (the untracked, generated copy in a development
        tree) from the fork's HEAD, or another fork commit, against the public pin, in the format
        the patch always had. Sections whose content did not change keep their previous bytes.
        --base rebased (or a public commit) makes the patch against the newer public decompilation
        the fork's history was rebuilt on (fork branch netlee-rebased, docs/decomp-fork-history.md)
        instead of the pin; public-snapshot and verify --snapshot take the same option.
    python tools/decomp_fork.py import-branch MAIN-REV [--branch NAME | --onto FORK-BRANCH] [--since MAIN-REV] [--single] [--dry-run]
        Bring a main-repository revision that still carries the patch (an old-workflow branch, or a
        patch-based update from Hero's or 0xburn's repository) into the fork as commits, on a fork
        branch named after the main-repository branch, parented on the fork commits that stand for
        its ancestors, so later merges in the fork are true three-way merges.
    python tools/decomp_fork.py public-snapshot OUT-DIR [--rev MAIN-REV] [--git] [--base B]
        Write the main repository as it is published: the public submodule URL and pin, the
        generated patch, nothing that names the private fork (checked). With --base the snapshot
        pins that public commit instead (its prepare_native_sources.py BASE says so too).
    python tools/decomp_fork.py setup [--no-fetch]
        Point this worktree's submodule at the private fork and check out the commit the main
        repository records (after an old-workflow checkout: only when its applied patch is exactly
        that commit's sources, so nothing is lost).
    python tools/decomp_fork.py push [FORK-BRANCH ...]
        Push fork branches to the private fork, after checking with gh that the repository is
        private. The only place decompiled source is ever pushed.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUB_PATH = 'sourceport/extern/melee'
PATCH_PATH = 'sourceport/patches/melee-native.patch'
PUBLIC_URL = 'https://github.com/doldecomp/melee.git'
# The newer public decompilation the fork's history was rebuilt on (tools/decomp_layers.py, fork branch
# netlee-rebased, docs/decomp-fork-history.md). The pin stays BASE in prepare_native_sources.py until
# the owner switches; until then --base rebased selects this one.
REBASED_BASE = '8e65295479b697e86021012707178df1fc51e7bd'
TRAILER_MAIN = 'Netlee-Main-Commit'
TRAILER_REBASED = 'Netlee-Rebased-From'
TRAILER_BLOB = 'Netlee-Patch-Blob'
TRAILER_BASE = 'Netlee-Public-Base'
# Files and folders only a development tree has: they describe the private fork or this machine, so a
# public snapshot leaves them out.
PRIVATE_ONLY = ('docs/decomp-fork-workflow.md', 'docs/decomp-fork-history.md', 'CLAUDE.md', 'docs/guide')
# Markdown between these markers describes development trees only; a public snapshot drops it.
DEV_BLOCK = re.compile(rb'<!-- development tree -->.*?<!-- end development tree -->[ \t]*\r?\n?', re.S)
# The .gitignore lines a development tree adds for the generated patch (a public tree tracks the patch).
GITIGNORE_LINES = (
    '# Generated in development trees from the decomp fork (tools/decomp_fork.py export-patch); public trees track it',
    '/' + PATCH_PATH,
)
# The settings the patch has always been written with (git's defaults), stated so that a user's own
# git configuration cannot change the output.
DIFF_CONFIG = ('-c', 'core.quotePath=true', '-c', 'diff.noprefix=false', '-c', 'diff.mnemonicPrefix=false',
               '-c', 'diff.suppressBlankEmpty=false', '-c', 'diff.renames=true', '-c', 'diff.relative=false')
DIFF_OPTIONS = ('--binary', '--full-index', '-M', '-l1000', '-U3', '--no-color', '--no-ext-diff', '--no-textconv',
                '--src-prefix=a/', '--dst-prefix=b/', '--diff-algorithm=myers', '--indent-heuristic')
ABBREV = 9   # the index-line abbreviation of the published patch (tools/refresh_native_patch.py)


class Failure(Exception):
    pass


def public_pin(root: Path = ROOT) -> str:
    """The public decompilation commit the patch applies to: BASE in tools/prepare_native_sources.py."""
    text = (root / 'tools/prepare_native_sources.py').read_text(encoding='utf-8')
    match = re.search(r"^BASE = '([0-9a-f]{40})'", text, re.M)
    if not match:
        raise Failure('tools/prepare_native_sources.py has no BASE commit')
    return match.group(1)


def resolve_base(spec: str | None) -> str:
    """The public commit a patch is made against: the pin (default, or `pin`), `rebased` for
    REBASED_BASE, or any public decompilation commit."""
    if not spec or spec == 'pin':
        return public_pin()
    if spec == 'rebased':
        return REBASED_BASE
    if re.fullmatch(r'[0-9a-f]{40}', spec):
        return spec
    return SUB.out('rev-parse', '--verify', spec + '^{commit}')


class Git:
    def __init__(self, path):
        self.path = str(path)

    def run(self, *args, env=None, input=None, check=True, config=()):
        result = subprocess.run(['git', *config, '-C', self.path, *args], capture_output=True, env=env, input=input)
        if check and result.returncode:
            raise Failure('git %s (in %s) failed:\n%s' % (' '.join(args[:3]), self.path,
                                                         result.stderr.decode(errors='replace').strip()))
        return result

    def out(self, *args, **kw) -> str:
        return self.run(*args, **kw).stdout.decode('utf-8', errors='replace').strip()

    def ok(self, *args) -> bool:
        return self.run(*args, check=False).returncode == 0

    def has(self, obj) -> bool:
        return self.ok('cat-file', '-e', obj)

    def is_ancestor(self, a, b) -> bool:
        return self.ok('merge-base', '--is-ancestor', a, b)


MAIN = Git(ROOT)
SUB = Git(ROOT / SUB_PATH)


def submodule_url(root: Path = ROOT) -> str:
    result = subprocess.run(['git', 'config', '-f', str(root / '.gitmodules'), '--get',
                             'submodule.%s.url' % SUB_PATH], capture_output=True, text=True)
    return result.stdout.strip()


def is_public_url(url: str) -> bool:
    return url.rstrip('/').removesuffix('.git').lower() == PUBLIC_URL.removesuffix('.git').lower()


def github_name(url: str) -> str | None:
    match = re.match(r'(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)([^/]+)/([^/]+?)(?:\.git)?/?$', url)
    return match and match.group(1) + '/' + match.group(2)


def private_markers(url: str) -> list[str]:
    """Strings that name the private fork: a public snapshot must contain none of them."""
    name = github_name(url)
    markers = [url.removesuffix('.git')]
    if name:
        markers += [name, name.split('/')[1]]
    return sorted(set(m for m in markers if m), key=len, reverse=True)


def ensure_private(url: str) -> str:
    """The fork must be a private GitHub repository: checked with gh before anything is pushed."""
    name = github_name(url)
    if not name:
        raise Failure('the submodule URL %r is not a GitHub repository; refusing to push' % url)
    if is_public_url(url):
        raise Failure('the submodule URL is the public decompilation; nothing of ours is ever pushed there')
    gh = shutil.which('gh')
    if not gh:
        raise Failure('gh (GitHub CLI) is needed to check that %s is private before pushing' % name)
    result = subprocess.run([gh, 'repo', 'view', name, '--json', 'visibility'], capture_output=True, text=True)
    if result.returncode:
        raise Failure('gh repo view %s failed: %s' % (name, result.stderr.strip()))
    visibility = json.loads(result.stdout).get('visibility')
    if visibility != 'PRIVATE':
        raise Failure('%s is %s, not PRIVATE: refusing to push decompiled source there' % (name, visibility))
    return name


# ---- the patch ----

SECTION_START = re.compile(rb'(?m)^(?=diff --git )')
INDEX_FULL = re.compile(rb'(?m)^index ([0-9a-f]{40})\.\.([0-9a-f]{40})')
INDEX_ANY = re.compile(rb'(?m)^index [0-9a-f]+\.\.[0-9a-f]+')
SIMILARITY = re.compile(rb'(?m)^(?:dis)?similarity index \d+%\n')


def split_sections(raw: bytes):
    parts = SECTION_START.split(raw)
    return parts[0], parts[1:]


def section_key(section: bytes) -> bytes:
    return section.split(b'\n', 1)[0]


def section_body(section: bytes) -> bytes:
    """A section without its index line and rename score: what it changes, not how it was produced
    (the hashes' abbreviation follows the clone it was made in, the score its line endings)."""
    header_end = section.find(b'\n---')
    if header_end < 0:
        header_end = section.find(b'\nGIT binary patch')
    if header_end < 0:
        header_end = len(section)
    header, rest = section[:header_end], section[header_end:]
    header = SIMILARITY.sub(b'', INDEX_ANY.sub(b'index', header + b'\n', count=1), count=1)[:-1]
    return header + rest


def same_patch(a: bytes, b: bytes) -> bool:
    head_a, secs_a = split_sections(a)
    head_b, secs_b = split_sections(b)
    return head_a == head_b and [section_body(s) for s in secs_a] == [section_body(s) for s in secs_b]


def patch_differences(a: bytes, b: bytes) -> list[str]:
    """The files whose sections differ between two patches (ignoring index lines and rename scores)."""
    _, secs_a = split_sections(a)
    _, secs_b = split_sections(b)
    map_a = {section_key(s): section_body(s) for s in secs_a}
    map_b = {section_key(s): section_body(s) for s in secs_b}
    names = sorted(set(map_a) | set(map_b))
    return [n.decode(errors='replace')[len('diff --git '):] for n in names if map_a.get(n) != map_b.get(n)]


def diff_trees(base: str, tree: str, repo: Git = None) -> bytes:
    repo = repo or SUB
    result = repo.run('diff', *DIFF_OPTIONS, base, tree, config=DIFF_CONFIG)
    if b'rename detection was skipped' in result.stderr:
        raise Failure('git skipped rename detection; the patch would not have its usual form:\n'
                      + result.stderr.decode(errors='replace'))
    out = []
    head, sections = split_sections(result.stdout)
    for section in sections:
        if b'\nGIT binary patch\n' not in section:   # binary sections keep the full hashes git writes for them
            section = INDEX_FULL.sub(lambda m: b'index ' + m.group(1)[:ABBREV] + b'..' + m.group(2)[:ABBREV],
                                     section, count=1)
        out.append(section)
    return head + b''.join(out)


def keep_unchanged(fresh: bytes, reference: bytes | None) -> bytes:
    """Sections that change nothing new keep the bytes they had in the reference patch, as
    tools/refresh_native_patch.py always kept untouched sections."""
    if not reference:
        return fresh
    head, sections = split_sections(fresh)
    _, old = split_sections(reference)
    by_key = {section_key(s): s for s in old}
    kept = []
    for section in sections:
        previous = by_key.get(section_key(section))
        kept.append(previous if previous is not None and section_body(previous) == section_body(section) else section)
    return head + b''.join(kept)


def last_tracked_patch(rev: str = 'HEAD') -> bytes | None:
    """The patch as the main repository last tracked it (before the switch to the fork)."""
    if MAIN.has('%s:%s' % (rev, PATCH_PATH)):
        return MAIN.run('show', '%s:%s' % (rev, PATCH_PATH)).stdout
    last = MAIN.out('log', '-1', '--format=%H', rev, '--', PATCH_PATH, check=False)
    if last and MAIN.has('%s^:%s' % (last, PATCH_PATH)):
        return MAIN.run('show', '%s^:%s' % (last, PATCH_PATH)).stdout
    return None


def load_reference(spec: str | None, out: Path | None) -> bytes | None:
    if spec == 'none':
        return None
    if spec:
        if Path(spec).is_file():
            return Path(spec).read_bytes()
        if ':' in spec and MAIN.has(spec):
            return MAIN.run('show', spec).stdout
        raise Failure('no reference patch at %s' % spec)
    if out and out.is_file():
        return out.read_bytes()
    return last_tracked_patch()


def worktree_tree(repo: Git = None) -> str:
    """The submodule's working tree (uncommitted and untracked, not ignored, files too) as a tree, built
    in a temporary index so the real one is untouched."""
    repo = repo or SUB
    with tempfile.TemporaryDirectory(prefix='decomp-fork-') as scratch:
        index = Path(scratch) / 'index'
        real = Path(repo.out('rev-parse', '--path-format=absolute', '--git-path', 'index'))
        if real.is_file():
            shutil.copyfile(real, index)   # its stat data spares re-reading unchanged files
        env = dict(os.environ, GIT_INDEX_FILE=str(index))
        if not real.is_file():
            repo.run('read-tree', 'HEAD', env=env)
        repo.run('add', '-A', env=env)
        return repo.out('write-tree', env=env)


def export_patch(tree: str, reference: bytes | None = None, base: str | None = None) -> bytes:
    base = base or public_pin()
    if not SUB.has(base + '^{tree}'):
        raise Failure('the public pin %s is not in this clone of the fork (a shallow clone?); fetch it: '
                      'git -C %s fetch upstream %s' % (base[:12], SUB_PATH, base))
    return keep_unchanged(diff_trees(base, tree), reference)


def resolve_fork_tree(rev: str | None, worktree: bool) -> tuple[str, str]:
    """(tree, description) for --rev or --worktree."""
    if worktree:
        return worktree_tree(), 'the working tree of %s' % SUB_PATH
    rev = rev or 'HEAD'
    sha = SUB.out('rev-parse', '--verify', rev + '^{commit}')
    return SUB.out('rev-parse', sha + '^{tree}'), 'fork commit %s' % sha[:12]


# ---- importing old-workflow revisions ----

class Importer:
    """Turns main-repository revisions that carry the patch into fork commits. Every fork commit made
    here names its main-repository commit in a trailer; a revision's fork parents are the fork commits
    of its newest already-imported ancestors, so the fork's history has the main repository's shape."""

    def __init__(self, dry_run=False, verbose=True):
        self.dry_run = dry_run
        self.verbose = verbose
        self.imported = self.scan()
        self.ancestor_cache = {}
        self.tree_cache = {}
        self.made = []

    def log(self, text):
        if self.verbose:
            print(text, flush=True)

    @staticmethod
    def scan() -> dict:
        """main-repository commit -> {fork, blob, base} for every imported commit the fork's branches,
        its pushed branches and HEAD reach."""
        pin = public_pin()
        refs = ['--branches', '--remotes=origin', '--tags']
        if SUB.ok('rev-parse', '--verify', '-q', 'HEAD'):
            refs.append('HEAD')
        exclude = ['^' + pin] if SUB.has(pin) else []
        fmt = '%H%x1f%(trailers:key={0},valueonly,separator=%x2c)%x1f%(trailers:key={1},valueonly)' \
              '%x1f%(trailers:key={2},valueonly)%x1e'.format(TRAILER_MAIN, TRAILER_BLOB, TRAILER_BASE)
        result = SUB.run('log', '--format=' + fmt, *refs, *exclude, '--', check=False)
        found, others = {}, {}
        for record in result.stdout.decode(errors='replace').split('\x1e'):
            fields = record.strip().split('\x1f')
            if len(fields) != 4 or not fields[1]:
                continue
            fork, main, blob, base = (f.strip() for f in fields)
            for sha in main.split(','):
                sha = sha.strip()
                if not re.fullmatch(r'[0-9a-f]{40}', sha):
                    continue
                if sha not in found:
                    found[sha] = {'fork': fork, 'blob': blob, 'base': base}
                elif found[sha]['fork'] != fork:
                    others.setdefault(sha, []).append({'fork': fork, 'blob': blob, 'base': base})
        # The rebuilt history (netlee-rebased, tools/decomp_layers.py) carries the same trailers as the
        # commits it replays: of two fork commits for one main commit, take the one this checkout's
        # history holds, so an import parents onto the history being worked on.
        if others and SUB.ok('rev-parse', '--verify', '-q', 'HEAD'):
            for sha, alternatives in others.items():
                for entry in [found[sha]] + alternatives:
                    if SUB.is_ancestor(entry['fork'], 'HEAD'):
                        found[sha] = entry
                        break
        return found

    def ancestors(self, rev) -> set:
        if rev not in self.ancestor_cache:
            self.ancestor_cache[rev] = set(MAIN.out('rev-list', rev).split())
        return self.ancestor_cache[rev]

    def maximal(self, shas) -> list:
        shas = sorted(set(shas))
        if len(shas) <= 1:
            return shas
        return MAIN.out('merge-base', '--independent', *shas).split()

    def bases_of(self, rev) -> list:
        """The newest imported ancestors of a main-repository revision (itself excluded)."""
        anc = self.ancestors(rev)
        return self.maximal([m for m in self.imported if m in anc and m != rev])

    def parents(self, rev) -> list:
        return MAIN.out('rev-list', '--parents', '-n', '1', rev).split()[1:]

    def blob(self, rev) -> str | None:
        result = MAIN.run('rev-parse', '--verify', '-q', '%s:%s' % (rev, PATCH_PATH), check=False)
        return result.stdout.decode().strip() if result.returncode == 0 else None

    def gitlink(self, rev) -> str:
        line = MAIN.out('ls-tree', rev, '--', SUB_PATH)
        match = re.match(r'160000 commit ([0-9a-f]{40})\t', line)
        if not match:
            raise Failure('%s has no %s submodule entry' % (rev[:12], SUB_PATH))
        return match.group(1)

    def order(self, rev, mains) -> list:
        """Fork parents in order: the one the revision's first parent leads to first."""
        if len(mains) <= 1:
            return mains
        parents = self.parents(rev)
        first_line = MAIN.out('rev-list', '--first-parent', parents[0]).split() if parents else []
        position = {sha: i for i, sha in enumerate(first_line)}
        anc_first = self.ancestors(parents[0]) if parents else set()
        return sorted(mains, key=lambda m: (0 if m in position else 1 if m in anc_first else 2,
                                            position.get(m, 0), m))

    def ensure_decomp_commit(self, sha):
        if not SUB.has(sha + '^{commit}'):
            self.log('  fetching the public decompilation commit %s' % sha[:12])
            SUB.run('fetch', '--no-tags', 'upstream', sha, check=False)
            if not SUB.has(sha + '^{commit}'):
                raise Failure('the decompilation commit %s is not in this clone and could not be fetched '
                              'from the remote "upstream" (%s)' % (sha, PUBLIC_URL))

    def patched_tree(self, rev) -> str:
        blob, link = self.blob(rev), self.gitlink(rev)
        if not blob:
            raise Failure('main-repository commit %s has no %s: it is not an old-workflow revision' % (rev[:12], PATCH_PATH))
        key = (blob, link)
        if key not in self.tree_cache:
            self.ensure_decomp_commit(link)
            patch = MAIN.run('cat-file', 'blob', blob).stdout
            with tempfile.TemporaryDirectory(prefix='decomp-import-') as scratch:
                path = Path(scratch) / 'native.patch'
                path.write_bytes(patch)
                env = dict(os.environ, GIT_INDEX_FILE=str(Path(scratch) / 'index'))
                SUB.run('read-tree', link, env=env)
                SUB.run('apply', '--cached', '--binary', '--whitespace=nowarn', str(path), env=env)
                self.tree_cache[key] = SUB.out('write-tree', env=env)
        return self.tree_cache[key]

    def side_needs_import(self, merge, side) -> bool:
        """A merged-in side branch gets fork commits of its own when it changed the game sources and
        none of its imported commits reach the merge any other way."""
        if side in self.imported or not self.blob(side):
            return False
        first = self.parents(merge)[0]
        if any(not MAIN.is_ancestor(s, first) for s in self.bases_of(side)):
            return False   # its imported commits become the fork merge's other parents
        base = MAIN.out('merge-base', first, side, check=False)
        return not base or not self.blob(base) or (self.blob(side), self.gitlink(side)) != (self.blob(base), self.gitlink(base))

    def commit(self, rev, tree, fork_parents, note) -> str:
        fields = MAIN.out('log', '-1', '--format=%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI%x00%s', rev).split('\x00')
        an, ae, ad, cn, ce, cd, subject = fields
        blob, link = self.blob(rev), self.gitlink(rev)
        message = ('%s\n\nGame sources of main-repository commit %s, made from its %s\n'
                   'by tools/decomp_fork.py import-branch.%s\n\n%s: %s\n%s: %s\n%s: %s\n'
                   % (subject, rev, PATCH_PATH, note, TRAILER_MAIN, rev, TRAILER_BLOB, blob, TRAILER_BASE, link))
        # The main commit's own author and committer, names and dates: the same import made in any
        # clone gives the same fork commit.
        env = dict(os.environ, GIT_AUTHOR_NAME=an, GIT_AUTHOR_EMAIL=ae, GIT_AUTHOR_DATE=ad,
                   GIT_COMMITTER_NAME=cn, GIT_COMMITTER_EMAIL=ce, GIT_COMMITTER_DATE=cd)
        args = ['commit-tree', tree]
        for parent in fork_parents:
            args += ['-p', parent]
        sha = SUB.out(*args, env=env, input=message.encode('utf-8'))
        self.imported[rev] = {'fork': sha, 'blob': blob, 'base': link}
        self.made.append((rev, sha))
        self.log('  %s <- main %s  %s%s' % (sha[:12], rev[:12], subject[:70], note and ' (no source change)'))
        return sha

    def import_one(self, rev, tip):
        parents = self.parents(rev)
        for side in parents[1:]:
            if self.side_needs_import(rev, side):
                self.log('  merged-in side branch %s changed the sources: importing it first' % side[:12])
                self.import_rev(side, single=True)
        mains = self.order(rev, self.bases_of(rev))
        link = self.gitlink(rev)
        fork_parents = [self.imported[m]['fork'] for m in mains]
        if not fork_parents:
            self.ensure_decomp_commit(link)
            fork_parents = [link]
        elif not SUB.is_ancestor(link, fork_parents[0]):
            self.ensure_decomp_commit(link)
            fork_parents.append(link)   # this revision moved to a newer public decompilation
        single = len(fork_parents) == 1
        first = self.imported.get(mains[0]) if mains else None
        unchanged = single and first is not None and (first['blob'], first['base']) == (self.blob(rev), link)
        if unchanged and not tip:
            return
        tree = self.patched_tree(rev)
        if single and not tip and tree == SUB.out('rev-parse', fork_parents[0] + '^{tree}'):
            return
        same_tree = single and tree == SUB.out('rev-parse', fork_parents[0] + '^{tree}')
        self.commit(rev, tree, fork_parents, ' It changes nothing in them; it marks this commit.' if same_tree else '')

    def import_rev(self, rev, single=False, since=None) -> str:
        rev = MAIN.out('rev-parse', '--verify', rev + '^{commit}')
        if rev in self.imported:
            return self.imported[rev]['fork']
        bases = self.bases_of(rev)
        if not bases and since:
            self.import_rev(since, single=True)
            bases = self.bases_of(rev)
        if single or not bases:
            walk = [rev]
        else:
            walk = MAIN.out('rev-list', '--first-parent', '--reverse', '--topo-order', rev,
                            *['^' + b for b in bases]).split()
        self.log('main %s: %d first-parent commits to consider since %s'
                 % (rev[:12], len(walk), ', '.join(b[:12] for b in bases) or 'the public pin'))
        for sha in walk:
            if sha not in self.imported:
                self.import_one(sha, tip=(sha == rev))
        return self.imported[rev]['fork']


def default_branch_name(rev: str) -> str:
    full = MAIN.out('rev-parse', '--symbolic-full-name', rev, check=False)
    if full.startswith('refs/heads/'):
        return full[len('refs/heads/'):]
    if full.startswith('refs/remotes/'):
        name = full[len('refs/remotes/'):]
        if name.split('/')[0] in ('origin', 'upstream'):
            raise Failure('%s would clash with the fork\'s own remote of that name: give --branch '
                          '(for Hero\'s repository, say --branch hero/main)' % name)
        return name
    raise Failure('%s is not a branch: give --branch NAME (or --onto FORK-BRANCH)' % rev)


def set_branch(name, sha, force=False):
    ref = 'refs/heads/' + name
    old = SUB.out('rev-parse', '--verify', '-q', ref, check=False)
    if old == sha:
        return 'already at'
    if old and not force and not SUB.is_ancestor(old, sha):
        raise Failure('fork branch %s (%s) is not an ancestor of %s: not a fast-forward (use --force to move it)'
                      % (name, old[:12], sha[:12]))
    if SUB.out('symbolic-ref', '-q', 'HEAD', check=False) == ref:
        raise Failure('fork branch %s is checked out in %s; check out another branch (or detach) first' % (name, SUB_PATH))
    SUB.run('update-ref', ref, sha, *([old] if old else []))
    return 'moved from %s to' % old[:12] if old else 'created at'


# ---- status and verify ----

def submodule_state():
    if not (ROOT / SUB_PATH / '.git').exists():
        raise Failure('%s is not checked out: python tools/decomp_fork.py setup' % SUB_PATH)
    head = SUB.out('rev-parse', 'HEAD', check=False)
    branch = SUB.out('symbolic-ref', '-q', '--short', 'HEAD', check=False)
    dirty = SUB.out('status', '--porcelain', check=False).splitlines()
    return head, branch, dirty


def recorded_gitlink() -> str | None:
    line = MAIN.out('ls-files', '-s', '--', SUB_PATH, check=False)
    match = re.match(r'160000 ([0-9a-f]{40}) ', line)
    return match and match.group(1)


def committed_gitlink() -> str | None:
    line = MAIN.out('ls-tree', 'HEAD', '--', SUB_PATH, check=False)
    match = re.match(r'160000 commit ([0-9a-f]{40})\t', line)
    return match and match.group(1)


def checks(fetch=False) -> list[tuple[bool, str]]:
    """(ok, line) for every check status prints and verify enforces."""
    url = submodule_url()
    pin = public_pin()
    results = []
    if is_public_url(url):
        head, _, dirty = submodule_state()
        results.append((True, 'public tree: %s is the public decompilation, the changes are the tracked patch' % SUB_PATH))
        results.append((head == pin, 'submodule at %s (the pin is %s)' % (head[:12], pin[:12])))
        return results
    head, branch, dirty = submodule_state()
    results.append((True, 'development tree: %s is the private working fork named in .gitmodules' % SUB_PATH))
    subject = SUB.out('log', '-1', '--format=%s', head, check=False)
    results.append((True, 'submodule HEAD %s %s  %s' % (head[:12], '(%s)' % branch if branch else '(detached)', subject[:70])))
    if SUB.has(pin):
        results.append((SUB.is_ancestor(pin, head), 'built on the public pin %s' % pin[:12]))
    else:
        results.append((True, 'public pin %s not in this clone (shallow?): ancestry not checked' % pin[:12]))
    results.append((not dirty, 'working tree clean' if not dirty else
                    'working tree has %d uncommitted or untracked files (commit them in the submodule): %s'
                    % (len(dirty), ', '.join(line[3:] for line in dirty[:6]) + (' ...' if len(dirty) > 6 else ''))))
    if fetch:
        SUB.run('fetch', '--no-tags', 'origin', check=False)
    pushed = SUB.out('for-each-ref', '--contains', head, '--format=%(refname:short)', 'refs/remotes/origin', check=False).split()
    results.append((bool(pushed), 'pushed to the private fork (in %s)' % ', '.join(pushed[:4]) if pushed else
                    'NOT pushed to the private fork%s: python tools/decomp_fork.py push %s'
                    % ('' if fetch else ' (as of the last fetch; --fetch to ask GitHub)', branch or '<branch>')))
    recorded, committed = recorded_gitlink(), committed_gitlink()
    if recorded != head:
        results.append((False, 'the main repository records %s, the submodule is at %s: '
                         'git add %s (and commit), or git submodule update to go back'
                         % ((recorded or 'nothing')[:12], head[:12], SUB_PATH)))
    elif committed != head:
        results.append((False, 'the new submodule pointer is staged but not committed in the main repository'))
    else:
        results.append((True, 'the main repository\'s HEAD records this commit'))
    if MAIN.ok('ls-files', '--error-unmatch', PATCH_PATH):
        results.append((False, '%s is tracked again (an old-workflow merge?): bring its changes over with '
                               'import-branch, then git rm --cached it' % PATCH_PATH))
    return results


def snapshot_checks(snapshot: Path, fork_rev: str | None, base: str | None = None) -> list[tuple[bool, str]]:
    results = []
    url = submodule_url(snapshot)
    results.append((is_public_url(url), '.gitmodules names %s' % (url or 'no URL')))
    pin = public_pin(snapshot)
    base = base or public_pin()
    results.append((pin == base, 'prepare_native_sources.py pins %s' % pin[:12]))
    if (snapshot / '.git').exists():
        line = Git(snapshot).out('ls-tree', 'HEAD', '--', SUB_PATH, check=False)
        results.append((('commit ' + pin) in line, 'the snapshot commit pins the submodule at %s' % (line.split()[2][:12] if line else 'nothing')))
    markers = private_markers(submodule_url())
    hits = scan_for(snapshot, markers)
    results.append((not hits, 'no file names the private fork' if not hits else
                    'files naming the private fork: ' + ', '.join(hits[:10])))
    patch_file = snapshot / PATCH_PATH
    if not patch_file.is_file():
        results.append((False, 'no %s' % PATCH_PATH))
        return results
    tree, what = resolve_fork_tree(fork_rev, False)
    exported = export_patch(tree, base=base)
    committed = patch_file.read_bytes()
    differing = patch_differences(committed, exported)
    results.append((not differing, 'the patch matches an export of %s' % what if not differing else
                    'the patch differs from an export of %s in: %s' % (what, ', '.join(differing[:8]))))
    return results


def scan_for(folder: Path, markers: list[str]) -> list[str]:
    lowered = [m.lower().encode() for m in markers]
    hits = []
    for path in sorted(folder.rglob('*')):
        if '.git' in path.relative_to(folder).parts[:1] or not path.is_file():
            continue
        data = path.read_bytes().lower()
        rel = path.relative_to(folder).as_posix()
        if any(m in data for m in lowered) or any(m in rel.lower().encode() for m in lowered):
            hits.append(rel)
    return hits


# ---- public snapshot ----

def public_snapshot(out: Path, rev: str = 'HEAD', make_git=False, reference_spec=None, base=None) -> dict:
    out = out.resolve()
    if out.exists() and any(out.iterdir()):
        raise Failure('%s is not empty' % out)
    main_sha = MAIN.out('rev-parse', '--verify', rev + '^{commit}')
    private_url = submodule_url()
    pin = base or public_pin()
    line = MAIN.out('ls-tree', main_sha, '--', SUB_PATH)
    fork_commit = line.split()[2]
    old_patch = MAIN.has('%s:%s' % (main_sha, PATCH_PATH))
    if old_patch and pin != public_pin():
        raise Failure('%s carries its own patch against the pin %s; it cannot be published on another base'
                      % (main_sha[:12], public_pin()[:12]))
    if not SUB.has(fork_commit + '^{commit}'):
        raise Failure('fork commit %s (recorded by %s) is not in %s: git -C %s fetch origin'
                      % (fork_commit[:12], main_sha[:12], SUB_PATH, SUB_PATH))
    out.mkdir(parents=True, exist_ok=True)
    archive = MAIN.run('archive', '--format=tar', main_sha, config=('-c', 'core.autocrlf=false', '-c', 'core.eol=lf')).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(out, filter='data')
    for path in PRIVATE_ONLY:
        if (out / path).is_dir():
            shutil.rmtree(out / path)
        elif (out / path).exists():
            (out / path).unlink()
    for doc in out.rglob('*.md'):
        data = doc.read_bytes()
        if b'<!-- development tree -->' in data:
            doc.write_bytes(DEV_BLOCK.sub(b'', data))
    # The public submodule URL; other submodules as they are.
    subprocess.run(['git', 'config', '-f', str(out / '.gitmodules'), 'submodule.%s.url' % SUB_PATH, PUBLIC_URL], check=True)
    text = (out / '.gitmodules').read_text(encoding='utf-8')
    (out / '.gitmodules').write_text(text.replace('\r\n', '\n'), encoding='utf-8', newline='\n')
    prepare = out / 'tools/prepare_native_sources.py'
    if prepare.is_file() and public_pin(out) != pin:   # --base: the published tree pins that commit
        text = prepare.read_text(encoding='utf-8')
        prepare.write_text(re.sub(r"(?m)^BASE = '[0-9a-f]{40}'", "BASE = '%s'" % pin, text, count=1),
                           encoding='utf-8', newline='\n')
    gitignore = out / '.gitignore'
    if gitignore.is_file():
        lines = gitignore.read_text(encoding='utf-8').split('\n')
        gitignore.write_text('\n'.join(l for l in lines if l.rstrip('\r') not in GITIGNORE_LINES), encoding='utf-8', newline='\n')
    if old_patch:
        patch = MAIN.run('show', '%s:%s' % (main_sha, PATCH_PATH)).stdout   # an old-workflow revision: as committed
    else:
        reference = load_reference(reference_spec, None) if reference_spec else last_tracked_patch(main_sha)
        patch = export_patch(SUB.out('rev-parse', fork_commit + '^{tree}'), reference, pin)
    (out / PATCH_PATH).parent.mkdir(parents=True, exist_ok=True)
    (out / PATCH_PATH).write_bytes(patch)
    hits = scan_for(out, private_markers(private_url)) if not is_public_url(private_url) else []
    if hits:
        raise Failure('the snapshot still names the private fork in: ' + ', '.join(hits))
    if make_git:
        snap = Git(out)
        snap.run('init', '-q')
        snap.run('add', '-A', config=('-c', 'core.autocrlf=false'))
        for entry in MAIN.out('ls-tree', '-r', main_sha).splitlines():
            mode, kind, sha, path = entry.split(None, 3)
            if mode == '160000':
                snap.run('update-index', '--add', '--cacheinfo', '160000,%s,%s' % (pin if path == SUB_PATH else sha, path))
        fields = MAIN.out('log', '-1', '--format=%an%x00%ae%x00%aI', main_sha).split('\x00')
        env = dict(os.environ, GIT_AUTHOR_NAME=fields[0], GIT_AUTHOR_EMAIL=fields[1], GIT_AUTHOR_DATE=fields[2],
                   GIT_COMMITTER_NAME=fields[0], GIT_COMMITTER_EMAIL=fields[1], GIT_COMMITTER_DATE=fields[2])
        version = (out / 'VERSION').read_text(encoding='utf-8').strip() if (out / 'VERSION').is_file() else ''
        snap.run('commit', '-q', '--no-verify', '-m', 'Netlee %s source' % version, env=env,
                 config=('-c', 'commit.gpgsign=false', '-c', 'core.autocrlf=false'))
    return {'main_commit': main_sha, 'fork_commit': fork_commit, 'pin': pin, 'patch_bytes': len(patch),
            'from_committed_patch': old_patch, 'out': str(out)}


# ---- setup and push ----

def setup(fetch=True, reference=None):
    url = submodule_url(ROOT)
    if is_public_url(url):
        print('this tree uses the public decompilation and the tracked patch: run tools/prepare_native_sources.py')
        return
    sub = ROOT / SUB_PATH
    target = recorded_gitlink()
    fresh = False
    if not (sub / '.git').exists():
        print('cloning the private fork into %s' % SUB_PATH)
        # --reference: another worktree's fork clone lends its objects, so only what it lacks is downloaded.
        borrow = ['--reference-if-able', str(Path(reference).resolve()), '--dissociate'] if reference else []
        subprocess.run(['git', 'clone', '--no-checkout', *borrow, url, str(sub)], check=True)
        fresh = True   # nothing checked out yet: no local changes to keep (status lists every file as deleted)
    remotes = SUB.out('remote').split()
    if 'origin' in remotes:
        SUB.run('remote', 'set-url', 'origin', url)
    else:
        SUB.run('remote', 'add', 'origin', url)
    if 'upstream' not in remotes:
        SUB.run('remote', 'add', 'upstream', PUBLIC_URL)
    if fetch and not fresh:   # a fresh clone has just fetched everything
        print('fetching the private fork')
        SUB.run('fetch', '--no-tags', 'origin')
    if not SUB.has(target + '^{commit}'):
        raise Failure('the recorded fork commit %s is not in the private fork\'s pushed branches: whoever made it must '
                      'push it (python tools/decomp_fork.py push)' % target[:12])
    head = SUB.out('rev-parse', 'HEAD', check=False)
    if not fresh and head == target and not SUB.out('status', '--porcelain'):
        print('%s is at %s, clean' % (SUB_PATH, target[:12]))
        return
    if not fresh and SUB.out('status', '--porcelain', check=False):
        state = worktree_tree()
        wanted = SUB.out('rev-parse', target + '^{tree}')
        if state != wanted:
            raise Failure('%s has local changes that are not exactly the sources of %s; nothing was touched. '
                          'Commit them on a fork branch (or, for an old-workflow branch, run import-branch) first.'
                          % (SUB_PATH, target[:12]))
        print('the working tree already holds exactly the sources of %s (an applied patch): taking the commit' % target[:12])
    SUB.run('checkout', '-q', '-f', '--detach', target)
    print('%s now at %s' % (SUB_PATH, target[:12]))


def push(branches):
    url = submodule_url()
    name = ensure_private(url)
    origin = SUB.out('remote', 'get-url', '--push', 'origin', check=False)
    if github_name(origin) != name:
        raise Failure('the submodule\'s origin (%s) is not the fork in .gitmodules (%s); run setup first' % (origin, url))
    if not branches:
        branch = SUB.out('symbolic-ref', '-q', '--short', 'HEAD', check=False)
        if not branch:
            raise Failure('the submodule HEAD is detached: name the fork branches to push')
        branches = [branch]
    print('%s is private; pushing %s' % (name, ', '.join(branches)))
    result = SUB.run('push', 'origin', *['refs/heads/%s:refs/heads/%s' % (b, b) for b in branches], check=False)
    sys.stdout.write(result.stderr.decode(errors='replace'))
    if result.returncode:
        raise Failure('push failed')


# ---- command line ----

def cmd_status(args):
    for ok, line in checks(args.fetch):
        print(('  ' if ok else '! ') + line)
    return 0


def cmd_verify(args):
    if args.snapshot:
        results = snapshot_checks(args.snapshot.resolve(), args.fork_rev, resolve_base(args.base))
    elif args.rev:
        rev = MAIN.out('rev-parse', '--verify', args.rev + '^{commit}')
        importer = Importer(verbose=False)
        if rev not in importer.imported:
            raise Failure('main-repository commit %s is not imported in this clone of the fork' % rev[:12])
        fork = importer.imported[rev]['fork']
        exported = export_patch(SUB.out('rev-parse', fork + '^{tree}'))
        committed = MAIN.run('show', '%s:%s' % (rev, PATCH_PATH)).stdout
        differing = patch_differences(committed, exported)
        results = [(not differing, 'fork commit %s gives back the patch of main %s%s'
                    % (fork[:12], rev[:12], '' if not differing else ': differs in ' + ', '.join(differing[:8])))]
        results.append((keep_unchanged(exported, committed) == committed,
                         'byte for byte with the committed patch as reference'))
    else:
        results = checks(args.fetch)
        if args.patch:
            tree, what = resolve_fork_tree(None, False)
            differing = patch_differences(args.patch.read_bytes(), export_patch(tree, base=resolve_base(args.base)))
            results.append((not differing, '%s matches an export of %s' % (args.patch, what) if not differing else
                            '%s differs from an export of %s in: %s' % (args.patch, what, ', '.join(differing[:8]))))
    bad = 0
    for ok, line in results:
        print(('  ok  ' if ok else '  BAD ') + line)
        bad += not ok
    print('verify: %s' % ('all checks passed' if not bad else '%d problem(s)' % bad))
    return 1 if bad else 0


def cmd_export(args):
    tree, what = resolve_fork_tree(args.rev, args.worktree)
    out = None if args.stdout or args.check else (args.out or ROOT / PATCH_PATH)
    reference = load_reference(args.reference, out or args.check)
    base = resolve_base(args.base)
    patch = export_patch(tree, reference, base)
    if args.check:
        differing = patch_differences(args.check.read_bytes(), patch)
        if differing:
            print('%s differs from an export of %s in %d files: %s' % (args.check, what, len(differing), ', '.join(differing[:10])))
            return 1
        exact = args.check.read_bytes() == patch
        print('%s matches an export of %s%s' % (args.check, what, ' byte for byte' if exact else ' (apart from index lines)'))
        return 0
    if args.stdout:
        sys.stdout.buffer.write(patch)
        return 0
    previous = out.read_bytes() if out.is_file() else None
    if previous == patch:
        print('%s is up to date with %s' % (out, what))
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(patch)
    changed = patch_differences(previous, patch) if previous else None
    print('wrote %s from %s against %s: %d files, %d bytes%s'
          % (out, what, 'the public pin' if base == public_pin() else 'public commit %s' % base[:12],
             len(split_sections(patch)[1]), len(patch),
             '' if changed is None else '; %d sections changed' % len(changed)))
    return 0


def cmd_import(args):
    branch = args.onto or args.branch or default_branch_name(args.rev)
    if args.onto and not SUB.ok('rev-parse', '--verify', '-q', 'refs/heads/' + args.onto):
        raise Failure('no fork branch %s to commit onto' % args.onto)
    importer = Importer(dry_run=args.dry_run)
    fork = importer.import_rev(args.rev, single=args.single, since=args.since)
    made = len(importer.made)
    print('main %s is fork commit %s (%d new fork commit%s)' % (args.rev, fork[:12], made, '' if made == 1 else 's'))
    if args.dry_run:
        print('dry run: fork branch %s not moved (the commits above exist only as unreferenced objects)' % branch)
        return 0
    state = set_branch(branch, fork, args.force)
    print('fork branch %s %s %s' % (branch, state, fork[:12]))
    print('next: python tools/decomp_fork.py push %s' % branch)
    return 0


def cmd_snapshot(args):
    info = public_snapshot(args.out, args.rev, args.git, args.reference, resolve_base(args.base))
    print('public snapshot of main %s in %s' % (info['main_commit'][:12], info['out']))
    print('  submodule: %s at %s' % (PUBLIC_URL, info['pin']))
    print('  %s: %d bytes, %s' % (PATCH_PATH, info['patch_bytes'],
                                  'as committed (old-workflow revision)' if info['from_committed_patch']
                                  else 'generated from the fork commit the revision records'))
    print('  no file names the private fork (checked)')
    return 0


def cmd_setup(args):
    if args.worktree:
        global ROOT, MAIN, SUB
        ROOT = args.worktree.resolve()
        MAIN, SUB = Git(ROOT), Git(ROOT / SUB_PATH)
    setup(fetch=not args.no_fetch, reference=args.reference)
    return 0


def cmd_push(args):
    push(args.branches)
    return 0


BASE_HELP = ('public decompilation commit the patch is made against: pin (default, BASE in '
             'prepare_native_sources.py), rebased (%s, the base of fork branch netlee-rebased), or a commit'
             % REBASED_BASE[:9])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('status', help='where the submodule stands')
    p.add_argument('--fetch', action='store_true', help='fetch the private fork first, to know what is pushed')
    p.set_defaults(func=cmd_status)
    p = sub.add_parser('verify', help='status as checks, or check a public snapshot or an import')
    p.add_argument('--fetch', action='store_true')
    p.add_argument('--snapshot', type=Path, help='check a public snapshot folder')
    p.add_argument('--fork-rev', help='with --snapshot: the fork commit its patch must equal (default HEAD)')
    p.add_argument('--rev', help='check that the fork commit imported for this old-workflow main commit gives back its patch')
    p.add_argument('--patch', type=Path, help='also check that this patch file equals an export of HEAD')
    p.add_argument('--base', help=BASE_HELP)
    p.set_defaults(func=cmd_verify)
    p = sub.add_parser('export-patch', help='generate the public patch from the fork')
    which = p.add_mutually_exclusive_group()
    which.add_argument('--rev', help='fork commit to export (default HEAD)')
    which.add_argument('--worktree', action='store_true', help='export the working tree, uncommitted files included')
    p.add_argument('--out', type=Path, help='default %s' % PATCH_PATH)
    p.add_argument('--stdout', action='store_true')
    p.add_argument('--check', type=Path, metavar='FILE', help='compare FILE with the export instead of writing')
    p.add_argument('--reference', help='patch whose unchanged sections are kept byte for byte: a file, MAIN-REV:PATH, '
                                       'or none (default: the file being replaced, else the last tracked patch)')
    p.add_argument('--base', help=BASE_HELP)
    p.set_defaults(func=cmd_export)
    p = sub.add_parser('import-branch', help='bring an old-workflow main-repository revision into the fork')
    p.add_argument('rev')
    name = p.add_mutually_exclusive_group()
    name.add_argument('--branch', help='fork branch to create or move (default: named after REV)')
    name.add_argument('--onto', help='existing fork branch to extend (must fast-forward)')
    p.add_argument('--since', help='with no imported ancestor: import this main commit first, on the public pin')
    p.add_argument('--single', action='store_true', help='one fork commit for REV, not one per commit that changed the patch')
    p.add_argument('--dry-run', action='store_true', help='make the commits but move no branch')
    p.add_argument('--force', action='store_true', help='move the fork branch even when it is not a fast-forward')
    p.set_defaults(func=cmd_import)
    p = sub.add_parser('public-snapshot', help='write the main repository as it is published')
    p.add_argument('out', type=Path)
    p.add_argument('--rev', default='HEAD', help='main-repository revision (default HEAD; committed files only)')
    p.add_argument('--git', action='store_true', help='also make OUT a git repository with one commit and the public submodule pins')
    p.add_argument('--reference', help='as for export-patch (default: the last patch the main repository tracked)')
    p.add_argument('--base', help=BASE_HELP)
    p.set_defaults(func=cmd_snapshot)
    p = sub.add_parser('setup', help='point this worktree\'s submodule at the private fork')
    p.add_argument('--no-fetch', action='store_true')
    p.add_argument('--reference', help="another worktree's sourceport/extern/melee to copy objects from when cloning")
    p.add_argument('--worktree', type=Path, help='set up this worktree instead of the one holding this script '
                                                  '(tools/new_worktree.py, for a tree whose own copy is older)')
    p.set_defaults(func=cmd_setup)
    p = sub.add_parser('push', help='push fork branches to the private fork (checked private first)')
    p.add_argument('branches', nargs='*')
    p.set_defaults(func=cmd_push)
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except Failure as error:
        print('decomp_fork: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
