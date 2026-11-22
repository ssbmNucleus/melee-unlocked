#!/usr/bin/env python3
"""The MexTK headers mexft modules build against: MU's native MexTK (tmce/MexTK, with tmce/native's
headers beside it so its relative includes hold), plus the newer m-ex member names of
compat_fields.txt.

MU's native headers lay out each struct as a union of padded members, one line each:
    struct { char _p1223[144]; float hitlag_frames; };
so a newer name for the same field is one more line with the same padding. TM-CE's own build keeps
the headers as they are.

Usage: mexft_overlay.py --tmce sourceport/game/tmce --compat compat_fields.txt --out DIR
Writes DIR/MexTK/... and DIR/native/*.h; DIR/MexTK/mex.h is what mexft/mex.h includes.
"""
import argparse
import os
import re
import shutil
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tmce', required=True)
    ap.add_argument('--compat', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    rows = []
    for line in open(a.compat, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            header, member, decl = line.split(None, 2)
            rows.append((header, member, decl.strip()))

    src, out = os.path.join(a.tmce, 'MexTK'), os.path.join(a.out, 'MexTK')
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(src, out, ignore=shutil.ignore_patterns('*.txt'))
    os.makedirs(os.path.join(a.out, 'native'), exist_ok=True)
    for f in os.listdir(os.path.join(a.tmce, 'native')):
        if f.endswith('.h'):
            shutil.copy2(os.path.join(a.tmce, 'native', f), os.path.join(a.out, 'native', f))

    errors = []
    for header, member, decl in rows:
        # "member+N": a field N bytes after that member (one the native headers do not name yet).
        member, _, delta = member.partition('+')
        delta = int(delta or '0', 0)
        path = os.path.join(out, 'include', header)
        text = open(path, encoding='utf-8').read()
        pat = re.compile(r'^([ \t]*)struct \{ char (_p\w+)\[(\d+)\]; [^;{}]*\b%s(?:\s*:\s*\d+)?; \};[ \t]*$'
                         % re.escape(member), re.M)
        count = [0]

        def add(m):
            count[0] += 1
            return '%s\n%sstruct { char %s_mx%d[%d]; %s; };' % (
                m.group(0), m.group(1), m.group(2), delta, int(m.group(3)) + delta, decl)
        text = pat.sub(add, text)
        if count[0] == 0:
            errors.append('%s: no padded member %s' % (header, member))
        elif delta and count[0] > 1:
            errors.append('%s: %s+%d is ambiguous (%d structs have %s)' % (header, member, delta, count[0], member))
        open(path, 'w', encoding='utf-8', newline='\n').write(text)
    if errors:
        sys.exit('mexft overlay:\n  ' + '\n  '.join(errors))
    print('mexft overlay: %d newer member names' % len(rows))
    return 0


if __name__ == '__main__':
    sys.exit(main())
