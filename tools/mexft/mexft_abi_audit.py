#!/usr/bin/env python3
"""Find the MexTK functions whose declared parameters disagree with the decomp function they link
to: one side takes an entity (a GObj) where the other takes its data (Fighter, Item...).

On the console such a call reads the wrong object without crashing (Dolphin tolerates the stray
address); natively it is an access violation. Each hit needs a native wrapper in mexft/mex.h.

Usage: mexft_abi_audit.py --redefine tmce/MexTK/mextk_redefine.txt --mextk tmce/MexTK/include
                          --decomp extern/melee/src [--used port.c ...]
With --used, only the functions those sources call are checked.
"""
import argparse
import os
import re

GOBJ = re.compile(r'\b(GOBJ|HSD_GObj|Item_GObj|Fighter_GObj|Ground_GObj|Camera_GObj)\s*\*')
DATA = re.compile(r'\b(ItemData|FighterData|GroundData|Item|Fighter|Ground)\s*\*')


def kind(param):
    if GOBJ.search(param):
        return 'gobj'
    if DATA.search(param):
        return 'data'
    return None


def read_tree(root, exts=('.h',)):
    text = []
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(exts):
                text.append(open(os.path.join(d, f), encoding='utf-8', errors='replace').read())
    return '\n'.join(text)


def params(text, name):
    m = re.search(r'(?m)^[^\n;{}()#]*\b%s\s*\(([^;{)]*(?:\([^)]*\)[^;{)]*)*)\)\s*(?:__asm__\([^)]*\)\s*)?;' % re.escape(name), text)
    if m is None:
        return None
    p = m.group(1).strip()
    return [] if p in ('', 'void') else [x.strip() for x in p.split(',')]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--redefine', required=True)
    ap.add_argument('--mextk', required=True)
    ap.add_argument('--decomp', required=True)
    ap.add_argument('--used', nargs='*', default=[])
    a = ap.parse_args()
    pairs = [l.split() for l in open(a.redefine, encoding='utf-8') if len(l.split()) == 2]
    used = None
    if a.used:
        used = set()
        for f in a.used:
            used |= set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', open(f, encoding='utf-8', errors='replace').read()))
    mex, dec = read_tree(a.mextk), read_tree(a.decomp)
    hits = 0
    for mex_name, dec_name in pairs:
        if used is not None and mex_name not in used:
            continue
        pm, pd = params(mex, mex_name), params(dec, dec_name)
        if pm is None or pd is None:
            continue
        for i, (x, y) in enumerate(zip(pm, pd)):
            kx, ky = kind(x), kind(y)
            if kx and ky and kx != ky:
                print('%s -> %s: parameter %d is %s in MexTK, %s in the decomp' % (mex_name, dec_name, i + 1, x, y))
                hits += 1
    print('%d mismatches' % hits)


if __name__ == '__main__':
    main()
