#!/usr/bin/env python3
"""Prepare a fighter's MexTK C for the native game (sourceport/game/mexft/).

An m-ex fighter is C compiled with MexTK into PowerPC, and the Source Port runs no PowerPC. When
the C is at hand, it compiles natively the way Training Mode CE's does (tmce_module): MexTK's
headers have native layouts, and its function names become the decomp's at link time
(mextk_redefine.txt). This script does the rest, with no edit of the fighter's own C:

  * the port's `#include ".../mex.h"` points at mexft/mex.h (native MexTK and its aliases);
  * each raw console address of a function, 0x8xxxxxxx cast to a function pointer, becomes the
    decomp function at that address (the decomp's own symbols.txt);
  * the m-ex exports are found by name, in any case, as MexTK matches them: ftFunction.txt for
    the fighter, itFunction.txt for an article (item_<n>.c, article n of the fighter's m-ex item
    lookup). Several lists may name the same slots: m-ex's current names in tools/mexft/, an older
    naming in MU's tmce/MexTK. An export marked static loses it so the module can export it;
  * each m-ex code file becomes a module of its own, as on the console, where the fighter and each
    article are linked apart: the fighter's C is module mexft_<fighter>, article n's
    mexft_<fighter>_a<n>. It writes each module's sources and export list, modules.txt naming them,
    and the fighter's descriptor (akaneia/mu_ak_fighter.h), whose two data exports go through
    mexft/mexft_adapter.h. A slot the C does not export stays NULL, and the registry gives it the
    m-ex default MxDt names (the donor's own function).

A raw address that is not the start of a function (game data at a console address, or code in the
middle of one) has no native meaning: the script names each one and fails. So does an export with
no native slot yet.

Usage: mexft_prepare.py --manifest mexft.txt --name shulk --out DIR --symbols symbols.txt
                        --functions ftFunction.txt [...] --item-functions itFunction.txt [...]
                        --shim mexft/mex.h --adapter mexft/mexft_adapter.h SOURCE.c [...]
The manifest has two lines, `name <display name>` and `file <fighter file>` (the file MxDt names
for the fighter, which is how the registry finds its slot).
"""
import argparse
import os
import re
import sys

# m-ex ftFunction index -> MuAkFighter field (sourceport/game/akaneia/mu_ak_fighter.h, which keeps
# ftFunction's order for 0..37 under the decomp's names).
FIELDS = [
    'onload', 'ondeath', 'onunknown', None,
    'specialn', 'specialairn', 'specials', 'specialairs',
    'specialhi', 'specialairhi', 'speciallw', 'specialairlw',
    'onabsorb', 'onitempickup', 'onmakeiteminvisible', 'onmakeitemvisible',
    'onitemdrop', 'onitemcatch', 'onunknownitemrelated',
    'onunknowncharactermodelflags1', 'onunknowncharactermodelflags2',
    'onhit', 'onunknowneyetexturerelated', 'onframe', 'onactionstatechange', 'onrespawn',
    'onmodelrender', 'onshadowrender', 'onunknownmultijump',
    'onactionstatechangewhileeyetextureischanged', 'ontwoentrytable',
    'enterfloat', 'enterdoublejump', 'entertether', 'onlanding',
    'onsmashf', 'onsmashhi', 'onsmashlw',
]
MOVE_LOGIC = 3
TRAIL = 45
# m-ex itFunction index -> ItemLogicTable field (it/kinds/types.h: the same order).
ITEM_FIELDS = [
    'states', 'spawned', 'destroyed', 'picked_up', 'dropped', 'thrown', 'dmg_dealt',
    'dmg_received', 'entered_air', 'reflected', 'clanked', 'absorbed', 'shield_bounced',
    'hit_shield', 'evt_unk',
]

RAW = re.compile(r'\b0x(8[01][0-9A-Fa-f]{6})\b')
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]*\bmex\.h)"', re.M)
DECL_NAME = re.compile(r'\b([A-Za-z_]\w*)\s*[\(\[]')
SKIP_WORDS = {'__attribute__', 'if', 'for', 'while', 'switch', 'return', 'sizeof'}
ARTICLE_FILE = re.compile(r'^item_(\d+)\.c$', re.I)

# Newer-MexTK constructs with no same-name native field, rewritten to what does the same natively.
# (pattern, replacement, what the build log says)
REWRITES = [
    # ftHit's bytes +40/+41 stored whole: on the console a bitfield counted from the top bit.
    (r'\b(\w+)->(x40|x41)\s*=(?!=)\s*([^;]+);', r'mexft_hit_\2(\1, \3);',
     'ftHit +40/+41 byte store through the decomp bits'),
    # m-ex's ItemData extension (+FCC, the owner before a reflect): MU's items have no such field.
    (r'->MEX\.fighter_orig\b', '->fighter_gobj',
     'ItemData MEX.fighter_orig read as the owner (approximation: no original owner natively)'),
    # A 4-byte field of the fighter or item file (ftData, itData) cast to a pointer: a console
    # pointer slot the native header types as a plain int; MEX_DP reads it as an address.
    (r'\(\s*([A-Za-z_]\w*\s*\*+)\s*\)\s*\(\s*(\w+(?:->\w+)*->(?:ftData|itData)->\w+)\s*\)',
     r'((\1) MEX_DP(\2))', 'file data field cast to a pointer, read as a console pointer slot'),
    (r'\(\s*([A-Za-z_]\w*\s*\*+)\s*\)\s*(\w+(?:->\w+)*->(?:ftData|itData)->\w+)\b',
     r'((\1) MEX_DP(\2))', 'file data field cast to a pointer, read as a console pointer slot'),
    # A pointer the port keeps in a 4-byte variable (state_var, item_var...) and casts back. Native
    # game memory and the game image sit below 4 GB, so the low word is the whole address, but a
    # signed int would widen with its sign: the cast back goes through unsigned.
    (r'\(\s*([A-Za-z_]\w*\s*\*+)\s*\)\s*\(\s*(\w+(?:->|\.)\w*_var\.\w+)\s*\)',
     r'((\1) (uintptr_t) (unsigned int) (\2))', 'pointer kept in an int variable, read back without sign extension'),
    (r'\(\s*([A-Za-z_]\w*\s*\*+)\s*\)\s*(\w+(?:->|\.)\w*_var\.\w+)\b',
     r'((\1) (uintptr_t) (unsigned int) (\2))', 'pointer kept in an int variable, read back without sign extension'),
    # MatchHUD.element_data (console 0x804A0FD8 + 0xF0) is a decomp table of its own natively.
    (r'\bstc_matchhud->element_data\[([^\]]+)\]', r'(*MEX_MATCHHUD_ELEMENT(\1))',
     'MatchHUD element_data read through MEX_MATCHHUD_ELEMENT'),
    # Newer MexTK types these as ints (console pointers are 4 bytes); natively they are pointers.
    (r'->destructor_function\s*=\s*\(\s*int\s*\)\s*', '->destructor_function = (void (*)(void *)) ',
     'GObj destructor stored as a pointer, not an int'),
    (r'\bText_CreateCanvas\(([^,()]+),\s*\(\s*int\s*\)\s*', r'Text_CreateCanvas(\1, ',
     'Text_CreateCanvas camera GObj passed as a pointer, not an int'),
]


def load_symbols(path):
    """console address -> (link name, kind) for the first symbol that starts there.

    A function the decomp keeps static (scope:local) has no symbol to link to; its link name is
    mexft_local_<name>, a one-line wrapper beside it in the decomp's file (under MU_NATIVE and
    MU_AKANEIA_FIGHTERS) that a port's first call to it needs added."""
    out = {}
    pat = re.compile(r'^(\S+)\s*=\s*\.(\w+):0x([0-9A-Fa-f]{8});\s*//\s*type:(\w+)(.*)')
    for line in open(path, encoding='utf-8'):
        m = pat.match(line)
        if m:
            name = m.group(1)
            if m.group(4) == 'function' and re.search(r'\bscope:local\b', m.group(5)):
                name = 'mexft_local_' + name
            out.setdefault(int(m.group(3), 16), (name, m.group(4)))
    return out


def load_slots(paths):
    """name (lower case) -> slot index, from one or more MexTK function lists."""
    slot_of = {}
    for path in paths:
        for idx, l in enumerate(x for x in open(path, encoding='utf-8') if x.strip()):
            slot_of.setdefault(l.strip().lower(), idx)
    return slot_of


def top_level_names(text):
    """(name, line index) of each definition at file scope: a line at column 0 that is not a
    directive, comment or prototype, and names something followed by ( or [."""
    found = []
    for i, line in enumerate(text.split('\n')):
        if not line or line[0] in ' \t#/}{*' or line.rstrip().endswith(';'):
            continue
        for m in DECL_NAME.finditer(line):
            if m.group(1) not in SKIP_WORDS:
                found.append((m.group(1), i))
                break
    return found


def count_rows(text, name):
    """Rows of a table initializer `name[] = { {...}, {...}, }`."""
    m = re.search(r'\b%s\s*\[\s*\]\s*=\s*\{' % re.escape(name), text)
    if not m:
        return None
    depth, rows, i = 1, 0, m.end()
    in_comment = None
    while i < len(text) and depth:
        c = text[i]
        if in_comment == '//':
            if c == '\n':
                in_comment = None
        elif in_comment == '/*':
            if text.startswith('*/', i):
                in_comment, i = None, i + 1
        elif text.startswith('//', i) or text.startswith('/*', i):
            in_comment, i = text[i:i + 2], i + 1
        elif c == '{':
            depth += 1
            if depth == 2:
                rows += 1
        elif c == '}':
            depth -= 1
        i += 1
    return rows


def load_edits(path):
    """A fighter's native.edits: the few hand edits no rule above can make (code that leans on a
    console layout the game reads too). Each hunk names a file of the fighter's folder and replaces
    lines that must occur there exactly once:

        # why (shown in the build log)
        @@ code/fighter.c
        -old line, exactly
        +new line

    Returns [[file, old, new, why, applied]]."""
    hunks = []
    if not os.path.isfile(path):
        return hunks
    cur, why, last = None, '', None
    for raw in open(path, encoding='utf-8'):
        line = raw.rstrip('\r\n')
        if line.startswith('@@'):
            cur, last = line[2:].strip(), None
        elif line.startswith('#'):
            why = (why + ' ' if last == '#' else '') + line[1:].strip()
            last = '#'
        elif line.startswith('-') or line.startswith('+'):
            if cur is None:
                sys.exit('%s: a hunk line before any "@@ file"' % path)
            if line[0] == '-' and last != '-':
                hunks.append([cur, [], [], why, False])
            if not hunks or hunks[-1][0] != cur:
                sys.exit('%s: a "+" line with no "-" line before it' % path)
            (hunks[-1][1] if line[0] == '-' else hunks[-1][2]).append(line[1:])
            last = line[0]
        elif line.strip():
            sys.exit('%s: unexpected line: %s' % (path, line))
        else:
            last = None
    return [[f, '\n'.join(o), '\n'.join(n), w, False] for f, o, n, w, _ in hunks]


def read_dir(path):
    text = []
    for root, _, files in os.walk(path):
        for f in files:
            if f.endswith('.h'):
                text.append(open(os.path.join(root, f), encoding='utf-8', errors='replace').read())
    return '\n'.join(text)


def disc_pointer_fields(mextk):
    """{struct: fields} of the fighter and item file structs (ftData, itData) that are console
    pointer slots natively (unsigned int)."""
    out = {}
    for header, struct in (('fighter.h', 'ftData'), ('item.h', 'itData')):
        text = open(os.path.join(mextk, header), encoding='utf-8').read()
        m = re.search(r'struct\s+(?:__attribute__\(\([^)]*\)\)\)\s+)?%s\b[^{;]*\{([^}]*)\}' % struct, text)
        out[struct] = re.findall(r'unsigned int (\w+);', m.group(1)) if m else []
    return out


class Prep:
    def __init__(self, a):
        self.a = a
        self.symbols = load_symbols(a.symbols)
        self.shim = os.path.abspath(a.shim).replace('\\', '/')
        self.errors = []
        self.raw_count = 0
        self.newer = set()
        self.notes = []
        self.home = os.path.dirname(os.path.abspath(a.manifest))
        self.edits = load_edits(os.path.join(self.home, 'native.edits'))
        # Names MU's MexTK (or the shim) declares: the link renames those (mextk_redefine.txt).
        known = read_dir(a.mextk) + open(a.shim, encoding='utf-8').read()
        self.known = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', known))
        # Newer MexTK names: m-ex's own headers give the prototype, melee.link the address.
        self.link = {}
        for line in open(a.mex_link, encoding='utf-8'):
            p = line.strip().split(':')
            if len(p) == 2 and re.fullmatch(r'[0-9A-Fa-f]{8}', p[0]):
                self.link.setdefault(p[1], int(p[0], 16))
        self.mex_headers = read_dir(a.mex_include)
        self.slots = disc_pointer_fields(os.path.join(a.mextk))

    def newer_names(self, src, body):
        """Declarations, each bound to the decomp function at its address, for the MexTK functions
        the port calls that MU's MexTK does not declare."""
        decls = []
        for name in sorted(set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body))):
            if name in self.known or name not in self.link:
                continue
            m = re.search(r'(?m)^[ \t]*([A-Za-z_][\w \t\*]*?\b%s\s*\([^;{]*\))\s*;' % re.escape(name),
                          self.mex_headers)
            sym = self.symbols.get(self.link[name])
            if m is None or sym is None or sym[1] != 'function':
                self.errors.append('%s: %s (0x%08X): %s' % (
                    src, name, self.link[name], 'no prototype in the m-ex headers' if m is None
                    else "no decomp function starts there (m-ex's own code?)"))
                continue
            decls.append('%s __asm__("%s");' % (' '.join(m.group(1).split()), sym[0]))
            self.newer.add(name)
        return decls

    def prepare(self, src, module, slot_of, what):
        """Write the prepared copy of src into module's folder; return ({slot: export name},
        prepared text)."""
        text = open(src, encoding='utf-8').read()
        rel = os.path.relpath(os.path.abspath(src), self.home).replace('\\', '/')
        for hunk in self.edits:
            if hunk[0] != rel:
                continue
            n = text.count(hunk[1])
            if n != 1:
                self.errors.append('native.edits: %s: the lines to replace occur %d times, not once:\n    %s'
                                   % (rel, n, hunk[1].replace('\n', '\n    ')))
                continue
            text = text.replace(hunk[1], hunk[2])
            hunk[4] = True
            self.notes.append('%s: hand edit (native.edits): %s' % (os.path.basename(src), hunk[3]))
        text = INCLUDE.sub('#include "%s"' % self.shim, text)
        refs = {}

        def resolve(m):
            addr = int(m.group(1), 16)
            sym = self.symbols.get(addr)
            if sym is None or sym[1] != 'function':
                line = text.count('\n', 0, m.start()) + 1
                why = 'no symbol starts there' if sym is None else '%s is %s' % (sym[0], sym[1])
                self.errors.append('%s:%d: 0x%08X: %s' % (src, line, addr, why))
                return m.group(0)
            refs[addr] = sym[0]
            return '((void*) mexft_ref_%08X)' % addr

        body = RAW.sub(resolve, text)
        # The fighter file's pointers, which are 4-byte console slots natively (MEX_DP).
        body = re.sub(r'\b(\w+)->ftData->items\b', r'mexft_ft_items(\1)', body)
        for struct, fields in self.slots.items():
            if fields:
                body, n = re.subn(r'\b(\w+)->%s->(%s)\b(?!\s*=[^=])' % (struct, '|'.join(fields)),
                                  r'((void *) MEX_DP(\1->%s->\2))' % struct, body)
                if n:
                    self.notes.append('%s: %d x %s pointer slot read through MEX_DP' % (os.path.basename(src), n, struct))
        for pat, rep, why in REWRITES:
            body, n = re.subn(pat, rep, body)
            if n:
                self.notes.append('%s: %d x %s' % (os.path.basename(src), n, why))
        exports = {}
        lines = body.split('\n')
        for name, i in top_level_names(body):
            idx = slot_of.get(name.lower())
            if idx is None:
                continue
            if idx in exports and exports[idx] != name:
                self.errors.append('%s: two definitions for %s %d (%s, %s)' % (src, what, idx, exports[idx], name))
            exports[idx] = name
            lines[i] = re.sub(r'\bstatic\s+', '', lines[i], count=1)
        body = '\n'.join(lines)

        out = os.path.join(self.a.out, module, 'src')
        os.makedirs(out, exist_ok=True)
        path = os.path.abspath(src).replace('\\', '/')
        decls = self.newer_names(src, body)
        if decls:
            inc = INCLUDE.search(body)
            if inc is None:
                self.errors.append('%s: no #include of mex.h to declare newer names after' % src)
            else:
                line = body.count('\n', 0, inc.end()) + 1
                body = (body[:inc.end()] + '\n/* newer MexTK names (m-ex melee.link -> decomp) */\n'
                        + '\n'.join(decls) + '\n#line %d "%s"' % (line + 1, path) + body[inc.end():])
        prelude =['/* Prepared by tools/mexft/mexft_prepare.py from %s: do not edit. */' % path]
        for addr in sorted(refs):
            prelude.append('extern char mexft_ref_%08X[] __asm__("%s");' % (addr, refs[addr]))
        prelude.append('#line 1 "%s"' % path)
        with open(os.path.join(out, os.path.basename(src)), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(prelude) + '\n' + body)
        self.raw_count += len(refs)
        return exports, body


def write_exports(out, module, names):
    with open(os.path.join(out, module, 'exports.txt'), 'w', newline='\n') as f:
        for n in names:
            f.write(n + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--symbols', required=True)
    ap.add_argument('--functions', required=True, action='append')
    ap.add_argument('--item-functions', required=True, action='append')
    ap.add_argument('--shim', required=True)
    ap.add_argument('--adapter', required=True)
    ap.add_argument('--mextk', required=True, help="MU's native MexTK headers (tmce/MexTK/include)")
    ap.add_argument('--mex-link', required=True, help="m-ex's melee.link (MexTK name -> address)")
    ap.add_argument('--mex-include', required=True, help="m-ex's current MexTK headers")
    ap.add_argument('sources', nargs='+')
    a = ap.parse_args()

    manifest = {}
    for line in open(a.manifest, encoding='utf-8'):
        p = line.split(None, 1)
        if len(p) == 2:
            manifest[p[0]] = p[1].strip()
    for key in ('name', 'file'):
        if key not in manifest:
            sys.exit('%s: no "%s" line' % (a.manifest, key))

    ft_slots = load_slots(a.functions)
    it_slots = load_slots(a.item_functions)
    prep = Prep(a)
    fighter = 'mexft_' + a.name
    os.makedirs(a.out, exist_ok=True)

    # The fighter's own C (every file that is no article) and its articles.
    exports, tables, articles = {}, {}, {}
    for src in sorted(a.sources):
        m = ARTICLE_FILE.match(os.path.basename(src))
        if m:
            n = int(m.group(1))
            module = '%s_a%d' % (fighter, n)
            ex, _ = prep.prepare(src, module, it_slots, 'itFunction')
            articles[n] = (module, ex)
            continue
        ex, body = prep.prepare(src, fighter, ft_slots, 'ftFunction')
        for idx, name in ex.items():
            if idx in exports and exports[idx] != name:
                prep.errors.append('%s: ftFunction %d defined twice (%s, %s)' % (src, idx, exports[idx], name))
            exports[idx] = name
            if idx == MOVE_LOGIC:
                tables[name] = count_rows(body, name)
    for hunk in prep.edits:
        if not hunk[4] and not any('native.edits: %s:' % hunk[0] in e for e in prep.errors):
            prep.errors.append('native.edits: %s is not one of the fighter\'s code files' % hunk[0])
    if prep.errors:
        sys.exit('mexft %s: a hand edit is needed:\n  %s' % (a.name, '\n  '.join(prep.errors)))

    modules = [fighter] + [articles[n][0] for n in sorted(articles)]
    write_exports(a.out, fighter, [exports[i] for i in sorted(exports)])
    for n in sorted(articles):
        module, ex = articles[n]
        write_exports(a.out, module, [ex[i] for i in sorted(ex)])
    with open(os.path.join(a.out, 'modules.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(modules) + '\n')

    # The descriptor.
    sym = lambda module, name: 'tmce_%s_%s' % (module, name)
    c = ['/* Generated by tools/mexft/mexft_prepare.py from %s: do not edit.' % a.manifest.replace('\\', '/'),
         ' * %s (%s), compiled from its MexTK C: %d fighter exports, %d articles. */'
         % (manifest['name'], manifest['file'], len(exports), len(articles)),
         '#include "%s"' % os.path.abspath(a.adapter).replace('\\', '/'), '']
    fields = []
    unsupported = []
    for idx in sorted(exports):
        name = exports[idx]
        s = sym(fighter, name)
        if idx == MOVE_LOGIC:
            rows = tables.get(name)
            if not rows:
                sys.exit('mexft %s: cannot count the rows of %s' % (a.name, name))
            c.append('extern const MexftState %s[%d];' % (s, rows))
            c.append('static MotionState mexft_states_%s[%d];' % (a.name, rows))
            c.append('__attribute__((constructor)) static void mexft_init_%s(void)' % a.name)
            c.append('{')
            c.append('    mexft_states(mexft_states_%s, %s, %d);' % (a.name, s, rows))
            c.append('}')
            fields.append('.move_logic = mexft_states_%s,' % a.name)
            fields.append('.move_logic_count = %d,' % rows)
        elif idx == TRAIL:
            c.append('extern const MexftTrail* %s(HSD_GObj* gobj);' % s)
            c.append('static struct SwordAttrs* mexft_trail_%s(HSD_GObj* gobj)' % a.name)
            c.append('{')
            c.append('    return mexft_trail(%s(gobj));' % s)
            c.append('}')
            fields.append('.gettraildata = (MuAkEvent) (void (*)(void)) mexft_trail_%s,' % a.name)
        elif FIELDS[idx:idx + 1] == ['onframe']:
            # Through the registry's test hook (MELEE_TEST_AK_MOTION logs each motion change).
            c.append('extern void %s(HSD_GObj* gobj);' % s)
            c.append('static void mexft_onframe_%s(HSD_GObj* gobj)' % a.name)
            c.append('{')
            c.append('    mu_ak_test_motion(GET_FIGHTER(gobj));')
            c.append('    %s(gobj);' % s)
            c.append('}')
            fields.append('.onframe = mexft_onframe_%s,' % a.name)
        elif idx < len(FIELDS) and FIELDS[idx]:
            c.append('extern void %s(HSD_GObj* gobj);' % s)
            fields.append('.%s = %s,' % (FIELDS[idx], s))
        else:
            unsupported.append('%s (ftFunction %d)' % (name, idx))

    # Articles: one ItemLogicTable each, in m-ex item lookup order; a gap is an article with no code.
    if articles:
        count = max(articles) + 1
        for n in sorted(articles):
            module, ex = articles[n]
            c.append('')
            row = []
            for idx in sorted(ex):
                name = ex[idx]
                s = sym(module, name)
                if idx == 0:
                    c.append('extern ItemStateTable %s[];' % s)
                elif idx < len(ITEM_FIELDS):
                    c.append('extern void %s(void);' % s)
                else:
                    unsupported.append('article %d: %s (itFunction %d)' % (n, name, idx))
                    continue
                f = ITEM_FIELDS[idx]
                row.append('    .%s = (__typeof__(((ItemLogicTable*) 0)->%s)) %s,' % (f, f, s))
            c.append('static ItemLogicTable mexft_article_%s_%d = {' % (a.name, n))
            c += row
            c.append('};')
        c.append('static ItemLogicTable* const mexft_articles_%s[%d] = {' % (a.name, count))
        c += ['    %s,' % ('&mexft_article_%s_%d' % (a.name, n) if n in articles else 'NULL')
              for n in range(count)]
        c.append('};')
        fields.append('.article_tables = mexft_articles_%s,' % a.name)
        fields.append('.article_count = %d,' % count)

    c.append('')
    c.append('const MuAkFighter mu_ak_%s = {' % fighter)
    c.append('    .name = "%s",' % manifest['name'])
    c.append('    .file = "%s",' % manifest['file'])
    c += ['    ' + f for f in fields]
    c.append('    .flags = MU_AK_READY,')
    c.append('};')
    with open(os.path.join(a.out, 'mexft_%s.c' % a.name), 'w', newline='\n') as f:
        f.write('\n'.join(c) + '\n')

    if unsupported:
        sys.exit('mexft %s: exports with no native slot yet: %s' % (a.name, ', '.join(unsupported)))
    print('mexft %s: %d raw addresses resolved; %d exports (%s); %d articles%s'
          % (a.name, prep.raw_count, len(exports), ', '.join(exports[i] for i in sorted(exports)),
             len(articles), '; newer MexTK names: ' + ', '.join(sorted(prep.newer)) if prep.newer else ''))
    for note in prep.notes:
        print('mexft %s:   %s' % (a.name, note))
    return 0


if __name__ == '__main__':
    sys.exit(main())
