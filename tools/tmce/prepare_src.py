"""Training Mode CE's C as the native build compiles it, made from the upstream source at the pin.

The pin (sourceport/game/tmce/upstream.txt) names the repository and the commit. From that commit's
src/ folder the files the native build compiles are taken with upstream's own LF line endings, then
tools/tmce/native_edits.py makes its edits, then tools/tmce/native_fixes.patch adds the fixes made
by hand (each one explained by a comment in the code it adds). Nothing else changes them.

The upstream source is a git repository with the pinned commit in it: its files are read from the
commit's objects, so a checkout's line-ending settings and local changes do not matter, and --fetch
makes one holding only that commit. Before anything is written the pin's two recorded hashes are
checked: the src/ files the build takes, and upstream's whole ASM/ folder. The native hooks port
those assembly patches by hand, so an ASM/ change at a new pin stops the build until someone has
looked at it.

The game's data files (TM/ beside the game: the event menu's, the Lab's and its character select's,
the wavedash drill's) are the commit's dats/ files as they are, written by --dats once their recorded
hash (dats-sha256) matches. The native game has TM-CE's code compiled in, so it needs only their data:
no TM-CE disc and none of upstream's disc-building tools. They are build outputs, never committed.

usage (the game's CMakeLists.txt runs the first three):
    python tools/tmce/prepare_src.py --upstream <dir> --fetch --verify
    python tools/tmce/prepare_src.py --upstream <dir> --out <dir> [--check <dir> [--eol-insensitive]]
    python tools/tmce/prepare_src.py --upstream <dir> --dats <dir>
    python tools/tmce/prepare_src.py --upstream <dir> --hashes          (the values to record at a new pin)
    python tools/tmce/prepare_src.py --upstream <dir> --make-patch <dir with the hand-fixed files>

--check compares the result with another copy: byte for byte, or with --eol-insensitive after CRLF
becomes LF (a Windows checkout with core.autocrlf). --make-patch writes native_fixes.patch from the
difference between the edited upstream files and the given ones.
"""
import argparse
import difflib
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tmce_upstream as U  # noqa: E402

PATCH = os.path.join(HERE, 'native_fixes.patch')

# The files of upstream's src/ the native build uses (sourceport/game/CMakeLists.txt compiles the .c
# files; the headers are theirs).
FILES = [
    'dthrowknee.c', 'edgeguard.c', 'edgeguard.h', 'eggs.c', 'eggs.h', 'events.c', 'events.h', 'fc.c', 'lab.c',
    'lab.h', 'lab_common.h', 'lab_css.c', 'laserland.c', 'lcancel.c', 'lcancel.h', 'ledgedash.c', 'ledgedash.h',
    'menu.c', 'menu.h', 'osds.c', 'osds.h', 'powershield.c', 'recovery.c', 'savestate.h', 'savestate_v1.c',
    'slalom.c', 'sweetspot.c', 'techchase.c', 'wavedash.c', 'wavedash.h',
]


# The data files of upstream's dats/ the native game loads from TM/ (tmce/native/tmce_runtime.c: the
# event menu, then each event's own file when it has one; the others' code needs no file).
DATS = ['eventMenu.dat', 'lab.dat', 'labCSS.dat', 'wavedash.dat']


class PrepareError(Exception):
    pass


def lf(data):
    return data.replace(b'\r\n', b'\n')


def upstream_files(upstream, commit, prefix):
    try:
        return U.files(upstream, commit, prefix)
    except U.UpstreamError as e:
        raise PrepareError(str(e))


def src_files(upstream, commit):
    """{name: bytes as stored upstream} of the listed src/ files at the commit."""
    have = dict(upstream_files(upstream, commit, 'src'))
    out = {}
    for name in FILES:
        if 'src/' + name not in have:
            raise PrepareError('%s at %s has no src/%s' % (upstream, commit[:12], name))
        out[name] = have['src/' + name]
    return out


def read_upstream(upstream, commit):
    """{name: bytes} of the listed src/ files at the commit, with LF line endings."""
    return {name: lf(data) for name, data in src_files(upstream, commit).items()}


def hashes(upstream, commit):
    """{'src-sha256': ..., 'asm-sha256': ...} of the upstream at the commit (tmce_upstream.digest)."""
    src = src_files(upstream, commit)
    asm = upstream_files(upstream, commit, 'ASM')
    if not asm:
        raise PrepareError('%s at %s has no ASM/ folder' % (upstream, commit[:12]))
    return {'src-sha256': U.digest([('src/' + n, src[n]) for n in sorted(src)])[0],
            'asm-sha256': U.digest(asm)[0], 'asm-files': len(asm),
            'dats-sha256': U.digest(sorted(dat_files(upstream, commit).items()))[0]}


def dat_files(upstream, commit):
    """{'dats/<name>': bytes as stored upstream} of the data files the native game loads."""
    have = dict(upstream_files(upstream, commit, 'dats'))
    out = {}
    for name in DATS:
        if 'dats/' + name not in have:
            raise PrepareError('%s at %s has no dats/%s' % (upstream, commit[:12], name))
        out['dats/' + name] = have['dats/' + name]
    return out


def write_dats(upstream, commit, pin, out):
    """The data files into `out`, once their hash matches the pin's dats-sha256."""
    files = dat_files(upstream, commit)
    got = U.digest(sorted(files.items()))[0]
    if got != pin.get('dats-sha256'):
        raise PrepareError(
            'the dats/ files in %s at %s hash to %s, not the recorded dats-sha256 %s '
            '(sourceport/game/tmce/upstream.txt). Point TMCE_UPSTREAM_DIR at the pinned commit, or delete '
            'the folder so the build fetches it again.' % (upstream, commit[:12], got, pin.get('dats-sha256')))
    os.makedirs(out, exist_ok=True)
    for path, data in files.items():
        open(os.path.join(out, path[len('dats/'):]), 'wb').write(data)   # always: the build's outputs
    return len(files)


def verify(upstream, commit, pin):
    """The recorded hashes against the upstream: raises PrepareError, saying what to do, on a mismatch."""
    got = hashes(upstream, commit)
    if got['src-sha256'] != pin.get('src-sha256'):
        raise PrepareError(
            'the src/ files in %s are not the pinned commit %s: their hash is %s, the pin records %s '
            '(sourceport/game/tmce/upstream.txt). Point TMCE_UPSTREAM_DIR at that commit, or delete the '
            'folder so the build fetches it again.' % (upstream, commit, got['src-sha256'], pin.get('src-sha256')))
    if got['asm-sha256'] != pin.get('asm-sha256'):
        raise PrepareError(
            "upstream's ASM/ folder at %s (%d files) hashes to %s, not the recorded %s. Training Mode CE's "
            'assembly patches changed, and the native hooks (sourceport/game/shim/mu_tmce*.c, '
            'sourceport/game/tmce/native) port them by hand: review the ASM/ changes since the last pin, '
            'port them, then record the new hashes in sourceport/game/tmce/upstream.txt '
            '(python tools/tmce/prepare_src.py --upstream <dir> --hashes).'
            % (commit, got['asm-files'], got['asm-sha256'], pin.get('asm-sha256')))
    return got


def run_native_edits(folder):
    spec = importlib.util.spec_from_file_location('tmce_native_edits', os.path.join(HERE, 'native_edits.py'))
    ne = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ne)
    ne.SRC = folder
    if ne.main() != 0:
        raise PrepareError('native_edits.py: an edit no longer finds its original text (see NOT FOUND above)')


# ---- the hand fixes: a unified diff, applied strictly (every context and removed line must match) ----

def split_lines(text):
    """Lines with their endings; a last line without one stays as it is."""
    return text.splitlines(keepends=True)


def make_patch(edited, fixed):
    """The unified diff (3 lines of context) from edited to fixed, {name: str} each."""
    out = []
    for name in FILES:
        a, b = split_lines(edited[name]), split_lines(fixed[name])
        if a == b:
            continue
        for line in difflib.unified_diff(a, b, 'a/' + name, 'b/' + name, n=3):
            if not line.endswith('\n'):
                line += '\n\\ No newline at end of file\n'
            out.append(line)
    return ''.join(out)


HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def parse_patch(text):
    """[(name, [(old_start, [(op, line)])])] with op ' ', '-' or '+'."""
    files = []
    lines = split_lines(text)
    i = 0
    while i < len(lines):
        if not lines[i].startswith('--- '):
            i += 1
            continue
        if i + 1 >= len(lines) or not lines[i + 1].startswith('+++ b/'):
            raise PrepareError('native_fixes.patch: a "---" line without its "+++ b/" line')
        name = lines[i + 1][6:].rstrip('\n')
        hunks = []
        files.append((name, hunks))
        i += 2
        while i < len(lines) and lines[i].startswith('@@'):
            m = HUNK.match(lines[i])
            if not m:
                raise PrepareError('native_fixes.patch: bad hunk header %r' % lines[i])
            old_start = int(m.group(1))
            old_n = int(m.group(2)) if m.group(2) is not None else 1
            new_n = int(m.group(4)) if m.group(4) is not None else 1
            body = []
            i += 1
            seen_old = seen_new = 0
            while i < len(lines) and (seen_old < old_n or seen_new < new_n or lines[i].startswith('\\')):
                line = lines[i]
                if line.startswith('\\'):
                    if body:   # "\ No newline at end of file": the line before has no ending
                        op, prev = body[-1]
                        body[-1] = (op, prev[:-1] if prev.endswith('\n') else prev)
                    i += 1
                    continue
                op, content = line[0], line[1:]
                if op not in ' -+':
                    raise PrepareError('native_fixes.patch: bad line in a hunk of %s: %r' % (name, line))
                body.append((op, content))
                seen_old += op in ' -'
                seen_new += op in ' +'
                i += 1
            hunks.append((old_start, body))
    return files


def apply_patch(files, patch_text):
    """files: {name: str}, changed in place. Raises PrepareError on any mismatch."""
    for name, hunks in parse_patch(patch_text):
        if name not in files:
            raise PrepareError('native_fixes.patch changes %s, which is not one of the files' % name)
        old = split_lines(files[name])
        new = []
        pos = 0
        for old_start, body in hunks:
            start = old_start - 1 if old_start > 0 else 0
            if start < pos:
                raise PrepareError('native_fixes.patch: hunks of %s overlap' % name)
            new += old[pos:start]
            pos = start
            for op, content in body:
                if op in ' -':
                    if pos >= len(old) or old[pos] != content:
                        got = old[pos] if pos < len(old) else '<end of file>'
                        raise PrepareError('native_fixes.patch does not apply to %s at line %d:\n  expected %r\n  found    %r'
                                           % (name, pos + 1, content, got))
                    pos += 1
                if op in ' +':
                    new.append(content)
        new += old[pos:]
        files[name] = ''.join(new)


def prepare(upstream, out, commit):
    os.makedirs(out, exist_ok=True)
    for name, data in read_upstream(upstream, commit).items():
        open(os.path.join(out, name), 'wb').write(data)
    run_native_edits(out)
    files = {n: open(os.path.join(out, n), encoding='utf-8', newline='').read() for n in FILES}
    edited = dict(files)
    apply_patch(files, open(PATCH, encoding='utf-8', newline='').read().replace('\r\n', '\n'))
    for name in FILES:
        if files[name] != edited[name]:
            open(os.path.join(out, name), 'w', encoding='utf-8', newline='').write(files[name])
    return edited


def check(out, other, eol_insensitive):
    bad = 0
    for name in FILES:
        a = open(os.path.join(out, name), 'rb').read()
        path = os.path.join(other, name)
        b = open(path, 'rb').read() if os.path.isfile(path) else None
        if a == b:
            continue
        if b is not None and eol_insensitive and lf(a) == lf(b):
            continue
        what = 'missing' if b is None else ('line endings only' if lf(a) == lf(b) else 'different')
        print('DIFFERS %s (%s)' % (name, what))
        bad += 1
    print('%d of %d files identical%s' % (len(FILES) - bad, len(FILES), ' (line endings aside)' if eol_insensitive else ''))
    return bad == 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    upstream = U.checkout(argv)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out')
    ap.add_argument('--check')
    ap.add_argument('--eol-insensitive', action='store_true')
    ap.add_argument('--make-patch')
    ap.add_argument('--fetch', action='store_true', help='first fetch the pinned commit into --upstream if it is not there')
    ap.add_argument('--verify', action='store_true', help="check the pin's recorded hashes and stop")
    ap.add_argument('--hashes', action='store_true', help='print the hashes to record for the commit')
    ap.add_argument('--list', action='store_true', help='print the files --out writes (for CMake)')
    ap.add_argument('--dats', help="write the game's TM/ data files (upstream's dats/, hash-checked) here")
    ap.add_argument('--list-dats', action='store_true', help='print the files --dats writes (for CMake)')
    ap.add_argument('--commit', help='instead of the pin (nothing is verified)')
    args = ap.parse_args(argv)
    if args.list:
        print(';'.join(FILES))
        return 0
    if args.list_dats:
        print(';'.join(DATS))
        return 0
    pin = U.pin()
    commit = args.commit or pin['commit']
    try:
        if args.fetch:
            try:
                if U.fetch(upstream, pin['url'], commit):
                    print('Training Mode CE %s fetched into %s' % (commit[:12], upstream))
            except U.UpstreamError as e:
                raise PrepareError('cannot get Training Mode CE %s from %s into %s (%s). Point '
                                   'TMCE_UPSTREAM_DIR at a git repository that has that commit (no network '
                                   'is needed then), or configure with -DMU_TMCE=OFF.'
                                   % (commit, pin['url'], upstream, e))
        if args.hashes:
            got = hashes(upstream, commit)
            print('commit %s\nsrc-sha256 %s\nasm-sha256 %s\ndats-sha256 %s'
                  % (commit, got['src-sha256'], got['asm-sha256'], got['dats-sha256']))
            return 0
        if args.verify:
            got = verify(upstream, commit, pin)
            print('Training Mode CE %s in %s: src/ (%d files) and ASM/ (%d files) match the pin'
                  % (commit[:12], upstream, len(FILES), got['asm-files']))
            return 0
        if args.make_patch:
            import tempfile
            with tempfile.TemporaryDirectory() as tmp:
                for name, data in read_upstream(upstream, commit).items():
                    open(os.path.join(tmp, name), 'wb').write(data)
                run_native_edits(tmp)
                edited = {n: open(os.path.join(tmp, n), encoding='utf-8', newline='').read() for n in FILES}
            fixed = {n: lf(open(os.path.join(args.make_patch, n), 'rb').read()).decode('utf-8') for n in FILES}
            text = make_patch(edited, fixed)
            open(PATCH, 'w', encoding='utf-8', newline='').write(text)
            heads = [line[:3] for line in text.splitlines()]
            print('wrote %s: %d files, %d hunks' % (PATCH, heads.count('+++'), heads.count('@@ ')))
            return 0
        if args.dats:
            n = write_dats(upstream, commit, pin, args.dats)
            print('Training Mode CE %s: %d data files in %s' % (commit[:12], n, args.dats))
            return 0
        if not args.out:
            ap.error('--out, --dats, --verify, --hashes or --make-patch')
        if not args.commit:
            verify(upstream, commit, pin)
        prepare(upstream, args.out, commit)
        print('Training Mode CE %s: %d files in %s' % (commit[:12], len(FILES), args.out))
        if args.check:
            return 0 if check(args.out, args.check, args.eol_insensitive) else 1
        return 0
    except PrepareError as e:
        print('prepare_src: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
