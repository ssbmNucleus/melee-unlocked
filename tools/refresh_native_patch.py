"""Refreshes the sections of sourceport/patches/melee-native.patch for the decomp files whose
working copy differs from what the patch gives them. usage: repatch.py <repo root> [--check]

In a development tree (the submodule is the private working fork, docs/decomp-fork-workflow.md) the
patch is not edited by hand any more: commit the change in the submodule instead. There this script
only says so and regenerates the untracked local copy from the working tree (decomp_fork.py
export-patch --worktree; with --check it compares instead), so the old habit still leaves the tests
that read the patch up to date."""
import re
import subprocess
import sys

root = sys.argv[1]
check = '--check' in sys.argv
sub = root + '/sourceport/extern/melee'
path = root + '/sourceport/patches/melee-native.patch'
BASE = re.search(r"^BASE = '([0-9a-f]{40})'", open(root + '/tools/prepare_native_sources.py').read(), re.M).group(1)

sub_head = subprocess.run(['git', '-C', sub, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
if sub_head and sub_head != BASE:
    print('This tree keeps the game-source changes as commits in sourceport/extern/melee (the working fork).')
    print('Commit there instead of refreshing the patch, on the fork branch named like this branch:')
    print('  git -C sourceport/extern/melee add -A && git -C sourceport/extern/melee commit -m "..."')
    print('  python tools/decomp_fork.py push   then   git add sourceport/extern/melee  (docs/decomp-fork-workflow.md)')
    command = [sys.executable, root + '/tools/decomp_fork.py', 'export-patch', '--worktree']
    if check:
        command += ['--check', path]
    print('Regenerating the local copy from the working tree: ' + ' '.join(command[1:]), flush=True)
    sys.exit(subprocess.call(command))

raw = open(path, 'rb').read()
parts = re.split(rb'(?m)^(?=diff --git )', raw)
head, sections = parts[0], parts[1:]
names = [re.match(rb'diff --git a/(\S+) b/', s).group(1).decode() for s in sections]


def fresh(name):
    return subprocess.run(['git', '-C', sub, '-c', 'core.autocrlf=false', '-c', 'core.abbrev=9', 'diff', '--', name], capture_output=True).stdout


# The generator's settings: an untouched file's section must come out byte for byte.
targets = [a for a in sys.argv[2:] if not a.startswith('--')]
probe = next(n for n in ('src/melee/mn/mnmain.c', 'src/melee/mn/mnstagesel.c', 'src/melee/lb/lbaudio_ax.c') if n not in targets)
def body(section):   # without the index line: its hash length follows the clone it was made in
    return re.sub(rb'(?m)^index [0-9a-f]+\.\.[0-9a-f]+', b'index', section, count=1)


same = body(fresh(probe)) == body(sections[names.index(probe)])
print('probe %s: %s' % (probe, 'identical' if same else 'DIFFERENT'))
if not same:
    a, b = fresh(probe), sections[names.index(probe)]
    print(len(a), len(b), a[:300], b[:300])
    sys.exit(1)
changed = 0
for name in sys.argv[2:]:
    if name.startswith('--'):
        continue
    new = fresh(name)
    i = names.index(name)
    if new != sections[i]:
        changed += 1
        print('  %s: %d -> %d bytes' % (name, len(sections[i]), len(new)))
        sections[i] = new
if not check and changed:
    open(path, 'wb').write(head + b''.join(sections))
print('%d sections refreshed%s' % (changed, ' (check only)' if check else ''))
