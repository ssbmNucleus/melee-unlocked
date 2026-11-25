#!/usr/bin/env python3
# Reads an m-ex disc's MxDt.dat: the fighter tables, and a fighter as an m-ex fighter package.
# SPDX-License-Identifier: GPL-2.0-or-later
"""usage:
  mxdt.py list <disc.iso>
  mxdt.py retail --out mu_mex_retail.inc <disc.iso> [<another disc.iso> ...]
  mxdt.py export <disc.iso> <fighter name or external id> <out folder>

list    prints the disc's fighters: external id, internal id, name, fighter file.
retail  writes the retail rows the game needs to build an m-ex table of its own on a disc that has
        none (sourceport/game/shim/mu_mex_roster.c): where each external id's fighter is, and the
        console address of each retail fighter's default function in every ftFunction table. They
        are the same on every m-ex disc; with more than one disc given, the rows must agree.
export  writes one fighter of the disc as the folder an m-ex fighter package unpacks to:
        fighter.json (the m-ex fighter format, the fields the game reads), the files it names, and
        its character select pictures from the disc's MnSlChr file: icon.png and a csp_<n>.png
        for each costume.
"""
import argparse
import json
import os
import struct
import sys
import zlib

RETAIL_INTERNAL = 33   # Ft_Kind_Max
RETAIL_EXTERNAL = 33
RETAIL_PLAYABLE_INTERNAL = 27   # Ft_Kind_MasterH: added fighters sit from here
RETAIL_PLAYABLE_EXTERNAL = 26   # CKind_Playable_Count
FUNCTION_TABLES = 46
TWO_ENTRY_TABLE = 30

# fighter.json "functions" name of each ftFunction table (m-ex mexLib MexFighterFunctions.ToMxDt).
FUNCTION_NAMES = [
    'onLoad', 'onRespawn', 'onDestroy', 'moveLogicPointer', 'specialN', 'specialNAir', 'specialS', 'specialSAir',
    'specialHi', 'specialHiAir', 'specialLw', 'specialLwAir', 'onAbsorb', 'onItemPickup', 'onMakeItemInvisible',
    'onMakeItemVisible', 'onItemDrop', 'onItemCatch', 'onUnknownItemRelated', 'onApplyHeadItem', 'onRemoveHeadItem',
    'eyeTextureDamaged', 'eyeTextureNormal', 'onFrame', 'onActionStateChange', 'resetAttribute', 'onModelRender',
    'onShadowRender', 'onUnknownMultijump', 'onActionStateChangeWhileEyeTextureIsChanged1', 'onTwoEntryTable1',
    'enterFloat', 'enterDoubleJump', 'enterTether', 'onLanding', 'onSmashF', 'onSmashHi', 'onSmashLw', 'onExtRstAnim',
    'onIndexExtRstAnim', 'demoMoveLogicPointer', 'onIntroL', 'onIntroR', 'onAppeal', 'onCatch', 'getSwordTrail',
]


# fighter.json "boneDefinitions.lookup": the parts of a skeleton table, in the table's order.
PARTS = [
    'top', 'trans', 'xRot', 'yRot', 'hip', 'waist', 'lLegA', 'lLeg', 'lKnee', 'lFootA', 'lFoot', 'rLegA', 'rLeg', 'rKnee',
    'rFootA', 'rFoot', 'waistB', 'bust', 'lClavicle', 'lShoulderA', 'lShoulder', 'lArm', 'lHand', 'lIndex1', 'lIndex2',
    'lMiddle1', 'lMiddle2', 'lRing1', 'lRing2', 'lPinky1', 'lPinky2', 'lHave', 'lThumb1', 'lThumb2', 'neckN', 'headN',
    'rClavicle', 'rShoulderA', 'rShoulder', 'rArm', 'rHand', 'rIndex1', 'rIndex2', 'rMiddle1', 'rMiddle2', 'rRing1',
    'rRing2', 'rPinky1', 'rPinky2', 'rHave', 'rThumb1', 'rThumb2', 'throw', 'extra',
]


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0x424)
        fst_at, fst_size = struct.unpack('>II', self.f.read(8))
        self.f.seek(fst_at)
        fst = self.f.read(fst_size)
        count = struct.unpack_from('>I', fst, 8)[0]
        names = count * 12
        self.files = {}
        for i in range(1, count):
            word, start, length = struct.unpack_from('>III', fst, i * 12)
            if word >> 24:
                continue
            at = names + (word & 0xFFFFFF)
            name = fst[at:fst.index(b'\0', at)].decode('latin-1')
            self.files[name.lower()] = (name, start, length)

    def has(self, name):
        return name.lower() in self.files

    def read(self, name):
        _, start, length = self.files[name.lower()]
        self.f.seek(start)
        return self.f.read(length)

    def real_name(self, name):
        return self.files[name.lower()][0]


class Dat:
    """An HSD archive: its data block, with pointers as offsets into it."""

    def __init__(self, raw):
        _, data_size, relocs, roots, refs = struct.unpack_from('>5I', raw, 0)
        self.d = raw[0x20:0x20 + data_size]
        table = 0x20 + data_size + relocs * 4
        strings = table + (roots + refs) * 8
        self.roots = {}
        for i in range(roots):
            at, name = struct.unpack_from('>II', raw, table + i * 8)
            end = raw.index(b'\0', strings + name)
            self.roots[raw[strings + name:end].decode('latin-1')] = at
        self.relocated = set(struct.unpack_from('>%dI' % relocs, raw, 0x20 + data_size))

    def u8(self, at):
        return self.d[at]

    def u16(self, at):
        return struct.unpack_from('>H', self.d, at)[0]

    def u32(self, at):
        return struct.unpack_from('>I', self.d, at)[0]

    def f32(self, at):
        return struct.unpack_from('>f', self.d, at)[0]

    def ptr(self, at):
        """The offset a pointer slot holds, None when the slot is empty or not a pointer."""
        return self.u32(at) if at in self.relocated else None

    def text(self, at):
        p = self.ptr(at)
        if p is None:
            return None
        return self.d[p:self.d.index(b'\0', p)].decode('latin-1')


class Mex:
    def __init__(self, disc):
        self.disc = disc
        self.dat = d = Dat(disc.read('MxDt.dat'))
        root = d.roots['mexData']
        self.meta = d.ptr(root)
        self.fighter = d.ptr(root + 8)
        self.functions = d.ptr(root + 0xC)
        self.effect = d.ptr(root + 0x18)
        self.kirby = d.ptr(root + 0x20)
        self.internal = d.u32(self.meta + 4)
        self.external = d.u32(self.meta + 8)
        self.effects = d.u32(self.meta + 0x24)

    def table(self, index):
        return self.dat.ptr(self.fighter + 4 * index)

    def ext_internal(self, ext):
        return self.dat.u8(self.table(3) + 3 * ext)

    def ext_of(self, internal):
        for ext in range(self.external):
            if self.ext_internal(ext) == internal:
                return ext
        return -1

    def name(self, ext):
        return self.dat.text(self.table(0) + 4 * ext)

    def pl_file(self, internal):
        return self.dat.text(self.table(1) + 8 * internal)

    def function(self, table, internal):
        words = self.dat.ptr(self.functions + 4 * table)
        if words is None:
            return 0, False
        # Table 30 (onTwoEntryTable) is two words a fighter, numbers not addresses: the first.
        at = words + (8 if table == TWO_ENTRY_TABLE else 4) * internal
        return self.dat.u32(at), at in self.dat.relocated


def cmd_list(a):
    mex = Mex(Disc(a.disc))
    print('%d internal, %d external fighters' % (mex.internal, mex.external))
    for ext in range(mex.external):
        internal = mex.ext_internal(ext)
        print('ext %3d  internal %3d  %-24s %s' % (ext, internal, mex.name(ext), mex.pl_file(internal)))


def retail_rows(mex):
    """The retail rows of a disc: ext -> retail internal (its specials shifted back), and
    [table][retail internal] function words."""
    shift = mex.internal - RETAIL_INTERNAL
    ext_shift = mex.external - RETAIL_EXTERNAL

    def retail_internal(i):   # the disc's internal id of a retail one
        return i if i < RETAIL_PLAYABLE_INTERNAL else i + shift

    ext_map = []
    for ext in range(RETAIL_EXTERNAL):
        disc_ext = ext if ext < RETAIL_PLAYABLE_EXTERNAL else ext + ext_shift
        row = mex.table(3) + 3 * disc_ext
        internal = mex.dat.u8(row)
        if internal >= RETAIL_PLAYABLE_INTERNAL:
            internal -= shift
        sub = mex.dat.u8(row + 1)
        if sub != 0xFF and sub >= RETAIL_PLAYABLE_INTERNAL:
            sub -= shift
        ext_map.append((internal, sub, mex.dat.u8(row + 2)))
    functions = []
    for table in range(FUNCTION_TABLES):
        row = []
        for i in range(RETAIL_INTERNAL):
            word, relocated = mex.function(table, retail_internal(i))
            if relocated:
                raise SystemExit('ftFunction %d of retail fighter %d points into MxDt.dat: not a retail row' % (table, i))
            row.append(word)
        functions.append(row)
    insignia = []
    for ext in range(RETAIL_EXTERNAL):
        insignia.append(mex.dat.u8(mex.table(2) + (ext if ext < RETAIL_PLAYABLE_EXTERNAL else ext + ext_shift)))
    return ext_map, functions, insignia


def cmd_retail(a):
    rows = None
    for path in a.discs:
        mine = retail_rows(Mex(Disc(path)))
        if rows is not None and mine != rows:
            for t in range(FUNCTION_TABLES):
                for i in range(RETAIL_INTERNAL):
                    if mine[1][t][i] != rows[1][t][i]:
                        print('  ftFunction %d fighter %d: %08X here, %08X before' % (t, i, mine[1][t][i], rows[1][t][i]))
            if mine[0] != rows[0]:
                print('  the external map differs')
            if mine[2] != rows[2]:
                print('  the emblems differ')
            raise SystemExit('%s: its retail rows are not the first disc\'s' % path)
        rows = mine
    ext_map, functions, insignia = rows
    with open(a.out, 'w', newline='\n') as f:
        f.write('/* Generated by tools/mexft/mxdt.py retail: the retail rows of an m-ex table. Do not edit.\n'
                ' * The same on every m-ex disc (checked on %d). */\n' % len(a.discs))
        f.write('static const unsigned char mu_mex_retail_ext_map[%d][3] = {\n' % RETAIL_EXTERNAL)
        for row in ext_map:
            f.write('    { %d, %d, %d },\n' % row)
        f.write('};\n')
        f.write('/* The series emblem of each external id. */\n')
        f.write('static const unsigned char mu_mex_retail_insignia[%d] = {\n   ' % RETAIL_EXTERNAL)
        for value in insignia:
            f.write(' %d,' % value)
        f.write('\n};\n')
        f.write('static const unsigned int mu_mex_retail_functions[%d][%d] = {\n' % (FUNCTION_TABLES, RETAIL_INTERNAL))
        for t, row in enumerate(functions):
            f.write('    /* %d %s */\n    {' % (t, FUNCTION_NAMES[t]))
            for i, word in enumerate(row):
                f.write('%s0x%08X,' % ('\n      ' if i % 8 == 0 else ' ', word))
            f.write('\n    },\n')
        f.write('};\n')
    print('mxdt: retail rows of %d discs agree -> %s' % (len(a.discs), a.out))


# ---- textures: the character select pictures of an m-ex disc ----

def write_png(path, width, height, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    rows = b''.join(b'\0' + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def rgb565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)


def rgb5a3(v):
    if v & 0x8000:
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255)
    a, r, g, b = (v >> 12) & 7, (v >> 8) & 15, (v >> 4) & 15, v & 15
    return (r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1)


def palette_colors(d, at, fmt, count):
    out = []
    for i in range(count):
        v = struct.unpack_from('>H', d, at + 2 * i)[0]
        out.append((v & 255, v & 255, v & 255, v >> 8) if fmt == 0 else rgb565(v) if fmt == 1 else rgb5a3(v))
    return out


def decode_texture(d, at, width, height, fmt, palette):
    """A console texture as RGBA bytes. fmt: 0 I4, 1 I8, 2 IA4, 3 IA8, 4 RGB565, 5 RGB5A3, 6 RGBA8,
    8 CI4, 9 CI8, 14 CMPR."""
    block_w, block_h = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 8: (8, 8),
                        9: (8, 4), 14: (8, 8)}[fmt]
    out = bytearray(width * height * 4)

    def put(x, y, color):
        if x < width and y < height:
            out[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(color)

    def pal(i):
        return palette[i] if palette and i < len(palette) else (0, 0, 0, 0)

    for by in range(0, height, block_h):
        for bx in range(0, width, block_w):
            if fmt in (0, 8):
                for i in range(64):
                    v = (d[at + i // 2] >> (0 if i & 1 else 4)) & 15
                    put(bx + i % 8, by + i // 8, (v * 17, v * 17, v * 17, v * 17) if fmt == 0 else pal(v))
                at += 32
            elif fmt in (1, 2, 9):
                for i in range(32):
                    v = d[at + i]
                    color = (v, v, v, v) if fmt == 1 else ((v & 15) * 17, (v & 15) * 17, (v & 15) * 17, (v >> 4) * 17) \
                        if fmt == 2 else pal(v)
                    put(bx + i % 8, by + i // 8, color)
                at += 32
            elif fmt in (3, 4, 5):
                for i in range(16):
                    v = struct.unpack_from('>H', d, at + 2 * i)[0]
                    color = (v & 255, v & 255, v & 255, v >> 8) if fmt == 3 else rgb565(v) if fmt == 4 else rgb5a3(v)
                    put(bx + i % 4, by + i // 4, color)
                at += 32
            elif fmt == 6:
                for i in range(16):
                    put(bx + i % 4, by + i // 4, (d[at + 2 * i + 1], d[at + 32 + 2 * i], d[at + 33 + 2 * i], d[at + 2 * i]))
                at += 64
            else:
                for sub in range(4):
                    c0, c1, bits = struct.unpack_from('>HHI', d, at)
                    at += 8
                    a, b = rgb565(c0), rgb565(c1)
                    if c0 > c1:
                        colors = [a, b, tuple((2 * a[k] + b[k]) // 3 for k in range(3)) + (255,),
                                  tuple((a[k] + 2 * b[k]) // 3 for k in range(3)) + (255,)]
                    else:
                        colors = [a, b, tuple((a[k] + b[k]) // 2 for k in range(3)) + (255,), (0, 0, 0, 0)]
                    for i in range(16):
                        put(bx + (sub & 1) * 4 + i % 4, by + (sub >> 1) * 4 + i // 4, colors[(bits >> (30 - 2 * i)) & 3])
    return out


def texture_of(dat, image, tlut):
    """RGBA of an image description and its palette description (or None): width, height, bytes."""
    width, height, fmt = dat.u16(image + 4), dat.u16(image + 6), dat.u32(image + 8)
    palette = None
    if tlut is not None and dat.ptr(tlut) is not None:
        palette = palette_colors(dat.d, dat.ptr(tlut), dat.u32(tlut + 4), dat.u16(tlut + 12))
    return width, height, decode_texture(dat.d, dat.ptr(image), width, height, fmt, palette)


def joint_at(dat, joint, want, index=None):
    """The joint at a joint index of a model, counted as the game counts (a joint, its children, the next)."""
    index = index if index is not None else [0]
    while joint is not None:
        if index[0] == want:
            return joint
        index[0] += 1
        found = joint_at(dat, dat.ptr(joint + 8), want, index)
        if found is not None:
            return found
        joint = dat.ptr(joint + 0xC)
    return None


def track_value(dat, fobj, frame):
    """The value a key track holds at a frame (the keys of a picture track are steps)."""
    data, length = dat.ptr(fobj + 0x10), dat.u32(fobj + 4)
    frac = dat.u8(fobj + 0xD)
    kind, scale = frac >> 5, float(1 << (frac & 31))
    slope = dat.u8(fobj + 0xE)
    slope_kind = slope >> 5
    at, end = data, data + length
    d = dat.d

    def packed():
        nonlocal at
        value, shift = 0, 0
        while True:
            b = d[at]
            at += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value

    def number(k):
        nonlocal at
        if k == 0:
            v = struct.unpack_from('<f', d, at)[0]
            at += 4
            return v
        size, code = {1: (2, '<h'), 2: (2, '<H'), 3: (1, '<b'), 4: (1, '<B')}[k]
        v = struct.unpack_from(code, d, at)[0]
        at += size
        return v / scale

    time, value = dat.f32(fobj + 8), None
    while at < end:
        head = packed()
        op, count = head & 15, (head >> 4) + 1
        if op == 0:
            break
        for _ in range(count):
            v = None
            if op in (1, 2, 3, 4, 6):
                v = number(kind)
            if op in (4, 5):
                number(slope_kind)
            wait = packed() if op in (1, 2, 3, 4) else 0
            if v is not None:
                if time > frame:
                    return value
                value = v
            time += wait
    return value


def export_pictures(disc, mex, ext, costumes, out):
    """icon.png and csp_<n>.png of external fighter id ext, from the disc's character select file."""
    name = next((n for n in ('MnSlChr.usd', 'MnSlChr.dat') if disc.has(n)), None)
    if name is None:
        return []
    dat = Dat(disc.read(name))
    sel = dat.roots.get('mexSelectChr')
    if sel is None:
        return []
    written = []
    d = mex.dat
    menu = d.ptr(d.roots['mexData'] + 4)
    css = d.ptr(menu + 4) if menu is not None else None
    for i in range(d.u32(mex.meta + 0xC) if css is not None else 0):
        if d.u8(css + 0xDC + 0x1C * i + 1) != ext:
            continue
        joint = joint_at(dat, dat.ptr(sel), i + 1)
        dobj = dat.ptr(joint + 0x10) if joint is not None else None
        while dobj is not None:
            mobj = dat.ptr(dobj + 8)
            tobj = dat.ptr(mobj + 8) if mobj is not None else None
            # The picture, not the shade drawn under it (an intensity texture of the same size).
            if tobj is not None and dat.ptr(tobj + 0x4C) is not None and dat.u32(dat.ptr(tobj + 0x4C) + 8) >= 4:
                width, height, rgba = texture_of(dat, dat.ptr(tobj + 0x4C), dat.ptr(tobj + 0x50))
                write_png(os.path.join(out, 'icon.png'), width, height, rgba)
                written.append('icon.png (%dx%d)' % (width, height))
                break
            dobj = dat.ptr(dobj + 4)
        break
    matanim, stride = dat.ptr(sel + 0xC), dat.u32(sel + 0x10)
    texanim = dat.ptr(matanim + 8) if matanim is not None else None
    if texanim is not None and stride:
        images, tluts = dat.ptr(texanim + 0xC), dat.ptr(texanim + 0x10)
        image_count, tlut_count = dat.u16(texanim + 0x14), dat.u16(texanim + 0x16)
        aobj = dat.ptr(texanim + 8)
        tracks = {}
        fobj = dat.ptr(aobj + 8) if aobj is not None else None
        while fobj is not None:
            tracks[dat.u8(fobj + 0xC)] = fobj
            fobj = dat.ptr(fobj)
        for c in range(costumes):
            frame = ext + c * stride
            image = track_value(dat, tracks[1], frame) if 1 in tracks else None
            tlut = track_value(dat, tracks[10], frame) if 10 in tracks else None
            if image is None or not 0 <= int(image) < image_count:
                continue
            tlut_at = dat.ptr(tluts + 4 * int(tlut)) if tluts is not None and tlut is not None and \
                0 <= int(tlut) < tlut_count else None
            width, height, rgba = texture_of(dat, dat.ptr(images + 4 * int(image)), tlut_at)
            write_png(os.path.join(out, 'csp_%d.png' % c), width, height, rgba)
            written.append('csp_%d.png (%dx%d)' % (c, width, height))
    return written


def cmd_export(a):
    disc = Disc(a.disc)
    mex = Mex(disc)
    d = mex.dat
    ext = -1
    if a.fighter.isdigit():
        ext = int(a.fighter)
    else:
        for e in range(mex.external):
            if (mex.name(e) or '').lower() == a.fighter.lower():
                ext = e
    if ext < 0 or ext >= mex.external:
        raise SystemExit('no fighter %s on this disc (mxdt.py list)' % a.fighter)
    internal = mex.ext_internal(ext)
    os.makedirs(a.out, exist_ok=True)
    wanted = []

    def want(name):
        if name and disc.has(name) and name not in wanted:
            wanted.append(name)
        return name

    info = mex.table(4) + 4 * ext
    costumes = []
    lst = d.ptr(mex.table(5) + 4 * internal)
    for c in range(d.u8(info)):
        at = lst + 16 * c
        costumes.append({'name': 'Costume %d' % c, 'file': {
            'visibilityIndex': d.u32(at + 12), 'fileName': want(d.text(at)), 'jointSymbol': d.text(at + 4),
            'materialSymbol': d.text(at + 8)}})
    demo = d.ptr(mex.table(6) + 4 * internal)
    effect_index = d.u8(mex.table(9) + internal)
    effect = d.ptr(mex.effect) + 12 * effect_index if effect_index < mex.effects else None
    kirby_caps = d.ptr(mex.kirby)
    kirby_effects = d.ptr(mex.kirby + 0x10)
    kirby_effect = d.u8(kirby_effects + internal) if kirby_effects is not None else 0xFF
    kirby_effect = d.ptr(mex.effect) + 12 * kirby_effect if kirby_effect < mex.effects else None
    items = []
    lookup = mex.table(19) + 8 * internal
    ids = d.ptr(lookup + 4)
    for i in range(d.u32(lookup) if ids is not None else 0):
        items.append(d.u16(ids + 2 * i))
    functions = {}
    for t, name in enumerate(FUNCTION_NAMES):
        word, relocated = mex.function(t, internal)
        functions[name] = 0 if relocated else word   # a table inside MxDt.dat is not carried here
    two = d.ptr(mex.functions + 4 * TWO_ENTRY_TABLE)
    functions['onTwoEntryTable2'] = d.u32(two + 8 * internal + 4) if two is not None else 0
    # The skeleton tables are in PlCo.dat (ftLoadCommonData words 4 and 5), by internal id.
    plco = Dat(disc.read('PlCo.dat'))
    common = plco.roots['ftLoadCommonData']
    table = plco.ptr(plco.ptr(common + 16) + 4 * internal)
    lookup = {'boneCount': plco.u32(table + 8)}
    part_to_joint = plco.ptr(table + 4)
    for i, part in enumerate(PARTS):
        lookup[part] = plco.u8(part_to_joint + i)
    bone_ext = []
    ext_table = plco.ptr(plco.ptr(common + 20) + 4 * internal)
    if ext_table is not None and plco.ptr(ext_table) is not None:
        for i in range(plco.u32(ext_table + 4)):
            at = plco.ptr(ext_table) + 4 * i
            bone_ext.append({'x00': plco.u8(at), 'x01': plco.u8(at + 1), 'x02': plco.u8(at + 2), 'x03': plco.u8(at + 3)})
    target = mex.table(20)
    fighter = {
        'name': mex.name(ext),
        'seriesID': d.u8(mex.table(2) + ext),
        'canWallJump': bool(d.u8(mex.table(17) + internal)),
        'soundBank': d.u8(mex.table(14) + 16 * ext),
        'announcerCall': d.u32(mex.table(13) + 4 * ext),
        'victoryTheme': d.u32(mex.table(12) + 4 * ext),
        'targetTestStage': d.u16(target + 2 * ext) if target is not None else 0,
        'resultScreenScale': d.f32(mex.table(11) + 4 * ext),
        'items': items,
        'redCostumeIndex': d.u8(info + 1), 'blueCostumeIndex': d.u8(info + 2), 'greenCostumeIndex': d.u8(info + 3),
        'costumes': costumes,
        'files': {
            'fighterDataPath': want(d.text(mex.table(1) + 8 * internal)),
            'fighterDataSymbol': d.text(mex.table(1) + 8 * internal + 4),
            'animFile': want(d.text(mex.table(7) + 4 * internal)),
            'animCount': d.u32(mex.table(8) + 8 * internal + 4),
            'demoFile': None,
            'demoResult': d.text(demo) if demo is not None else None,
            'demoIntro': d.text(demo + 4) if demo is not None else None,
            'demoEnding': d.text(demo + 8) if demo is not None else None,
            'demoWait': d.text(demo + 12) if demo is not None else None,
            'rstAnimFile': want(d.text(mex.table(10) + 4 * ext)),
            'effectFile': want(d.text(effect)) if effect is not None else None,
            'effectSymbol': d.text(effect + 4) if effect is not None else None,
            'kirbyCapFileName': want(d.text(kirby_caps + 8 * internal)) if kirby_caps is not None else None,
            'kirbyCapSymbol': d.text(kirby_caps + 8 * internal + 4) if kirby_caps is not None else None,
            'kirbyEffectFile': want(d.text(kirby_effect)) if kirby_effect is not None else None,
            'kirbyEffectSymbol': d.text(kirby_effect + 4) if kirby_effect is not None else None,
        },
        'functions': functions,
        'boneDefinitions': {'lookup': lookup, 'ext': bone_ext},
    }
    with open(os.path.join(a.out, 'fighter.json'), 'w', newline='\n') as f:
        json.dump(fighter, f, indent=2)
    for name in wanted:
        with open(os.path.join(a.out, disc.real_name(name)), 'wb') as f:
            f.write(disc.read(name))
    print('mxdt: %s (external %d, internal %d): fighter.json and %d files -> %s' % (
        fighter['name'], ext, internal, len(wanted), a.out))
    for name in wanted:
        print('  %s' % disc.real_name(name))
    for line in export_pictures(disc, mex, ext, len(costumes), a.out):
        print('  %s' % line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list')
    p.add_argument('disc')
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser('retail')
    p.add_argument('--out', required=True)
    p.add_argument('discs', nargs='+')
    p.set_defaults(fn=cmd_retail)
    p = sub.add_parser('export')
    p.add_argument('disc')
    p.add_argument('fighter')
    p.add_argument('out')
    p.set_defaults(fn=cmd_export)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
