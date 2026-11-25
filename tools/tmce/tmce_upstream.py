"""Training Mode CE's upstream source for the tools here: the pin, the checkout to read, its hashes.

The pin (sourceport/game/tmce/upstream.txt) names the repository and the commit the native build
compiles, with two recorded hashes: of the src/ files the build takes and of the whole ASM/ folder
(the patches the native hooks port by hand). The checkout is a git repository with that commit in
it: `--upstream DIR` on a tool's command line, else the TMCE_UPSTREAM environment variable, else
run-source/tmce-src. fetch() makes one holding that commit alone, fetched by its full hash (the
build's own, without a work tree); the tools that read MexTK/ (gen_mextk.py and its helpers) need a
clone with the commit checked out.
"""
import hashlib
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PIN_FILE = os.path.join(ROOT, 'sourceport', 'game', 'tmce', 'upstream.txt')
DEFAULT_CHECKOUT = os.path.join(ROOT, 'run-source', 'tmce-src')


class UpstreamError(Exception):
    pass


def pin(path=PIN_FILE):
    """The pin file's `key value` lines as a dict (url, commit, src-sha256, asm-sha256)."""
    out = {}
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, value = line.partition(' ')
        out[key] = value.strip()
    return out


def checkout(argv=None):
    """The upstream checkout folder. `--upstream DIR` in argv is taken out of it (and kept in
    TMCE_UPSTREAM, so modules imported later see the same folder)."""
    if argv is not None and '--upstream' in argv:
        i = argv.index('--upstream')
        if i + 1 >= len(argv):
            raise SystemExit('--upstream needs a folder')
        os.environ['TMCE_UPSTREAM'] = argv[i + 1]
        del argv[i:i + 2]
    return os.path.abspath(os.environ.get('TMCE_UPSTREAM') or DEFAULT_CHECKOUT)


def git(upstream, *args, check=True, input=None):
    """Runs git (TMCE_GIT, which the game's build sets to the git CMake found, else git on PATH)."""
    exe = os.environ.get('TMCE_GIT') or shutil.which('git') or 'git'
    try:
        r = subprocess.run([exe, '-C', upstream] + list(args), capture_output=True, input=input)
    except OSError as e:
        raise UpstreamError('cannot run git (%s): %s' % (exe, e))
    if check and r.returncode != 0:
        raise UpstreamError('git %s in %s failed: %s' % (' '.join(args), upstream,
                                                         r.stderr.decode('utf-8', 'replace').strip()))
    return r


def is_git(upstream):
    return os.path.exists(os.path.join(upstream, '.git'))


def has_commit(upstream, commit):
    return is_git(upstream) and git(upstream, 'cat-file', '-e', commit + '^{commit}', check=False).returncode == 0


def fetch(upstream, url, commit):
    """Make the git repository `upstream` hold the pinned commit (True when it had to be fetched):
    a new folder becomes a repository with only that commit, fetched by its full hash. Git checks
    every object it receives against that hash, so what arrives is the pinned commit or nothing."""
    if has_commit(upstream, commit):
        return False
    if os.path.isdir(upstream) and os.listdir(upstream) and not is_git(upstream):
        raise UpstreamError('%s is a folder but not a git repository' % upstream)
    os.makedirs(upstream, exist_ok=True)
    if not is_git(upstream):
        git(upstream, 'init', '-q')
    git(upstream, 'fetch', '-q', '--depth', '1', '--no-tags', url, commit)
    if not has_commit(upstream, commit):
        raise UpstreamError('fetching %s from %s did not give that commit' % (commit, url))
    git(upstream, 'update-ref', 'refs/tmce/pin', commit)   # a ref, so no gc drops it
    return True


def files(upstream, commit, prefix):
    """[(path, bytes)] of the commit's files under prefix (a folder or one file), sorted by path,
    as git stores them: the repository's line-ending settings and work tree do not matter."""
    if not is_git(upstream):
        raise UpstreamError('%s is not a git repository: fetch the pin into it first' % upstream)
    if not has_commit(upstream, commit):
        raise UpstreamError('%s has no commit %s (the pin): fetch it first' % (upstream, commit))
    listing = git(upstream, 'ls-tree', '-r', '-z', commit, '--', prefix).stdout.split(b'\0')
    blobs = []
    for entry in listing:
        if not entry:
            continue
        meta, _, path = entry.partition(b'\t')
        mode, kind, oid = meta.split()
        if kind == b'blob':
            blobs.append((path.decode('utf-8'), oid))
    # One git process for all of them: "<oid> blob <size>\n<content>\n" each, in order.
    r = git(upstream, 'cat-file', '--batch', check=False, input=b''.join(oid + b'\n' for _, oid in blobs))
    out, data, pos = [], r.stdout, 0
    for path, oid in blobs:
        end = data.find(b'\n', pos)
        head = data[pos:end].split() if end >= 0 else []
        if r.returncode != 0 or len(head) != 3 or head[0] != oid or head[1] != b'blob':
            raise UpstreamError('git cat-file in %s could not read %s (%s)' % (upstream, path, oid.decode()))
        size = int(head[2])
        out.append((path, data[end + 1:end + 1 + size]))
        pos = end + 1 + size + 1
    return sorted(out)


def digest(entries):
    """One SHA-256 over (path, content) pairs: the hash of the sha256sum listing of them."""
    listing = ''.join('%s  %s\n' % (hashlib.sha256(data).hexdigest(), path) for path, data in entries)
    return hashlib.sha256(listing.encode('utf-8')).hexdigest(), len(entries)
