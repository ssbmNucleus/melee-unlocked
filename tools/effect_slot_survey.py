"""Who spawns each part of a fighter's effect file (EfXxData.dat): the survey behind the slot tables
in port/runtime/host/effect_slots.cpp. Read-only; it writes nothing.

    python tools/effect_slot_survey.py <iso> [EfCaData.dat ...]

For each model effect (an EF_EffectDesc record, gfx id = bank * 1000 + index) and each particle
generator (command bank record, the same numbering) it prints what makes it:
  - request ids of the decomp's tables (ef/efsync.c, ef/efalt.c, ef/efasync.c) and the fighter
    code files that ask for them (ft/kinds/ft<Name>/*.c: specialn, specials, ...);
  - the fighter's move scripts (Pl<Xx>.dat actions, command 10 "spawn effect": a request id);
  - particle keys in a model effect's own animation (track 0x28) or joint particle lists;
  - another generator's program (opcodes 0xA5, 0xEF, 0xF0: spawn a child generator).
and which texture group each generator draws. The slot tables are written by hand from this."""
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT / "sourceport" / "extern" / "melee" / "src" / "melee"

BANKS = {0: "Co", 1: "Mr", 2: "Ss", 3: "Fx", 4: "Ca", 5: "Kb", 6: "Lk", 7: "Pk", 8: "Dk", 9: "Ys", 10: "Ns",
         11: "Pr", 12: "Kp", 13: "Mt", 14: "Ic", 15: "Pe", 16: "Ms", 17: "Zd", 18: "Lg", 19: "Gn", 49: "Fe"}
# Fighter code folders (ft/kinds) and their effect file.
FOLDERS = {"ftMario": "Mr", "ftDrMario": "Mr", "ftSamus": "Ss", "ftFox": "Fx", "ftFalco": "Fx", "ftCaptain": "Ca",
           "ftKirby": "Kb", "ftLink": "Lk", "ftCLink": "Lk", "ftPikachu": "Pk", "ftPichu": "Pk", "ftDonkey": "Dk",
           "ftYoshi": "Ys", "ftNess": "Ns", "ftPurin": "Pr", "ftKoopa": "Kp", "ftMewtwo": "Mt", "ftPopo": "Ic",
           "ftNana": "Ic", "ftPeach": "Pe", "ftMars": "Ms", "ftZelda": "Zd", "ftSeak": "Zd", "ftLuigi": "Lg",
           "ftGanon": "Gn", "ftEmblem": "Fe"}
# The fighter files whose move scripts use each effect file.
PL_FILES = {"Mr": ["PlMr.dat", "PlDr.dat"], "Ss": ["PlSs.dat"], "Fx": ["PlFx.dat", "PlFc.dat"], "Ca": ["PlCa.dat"],
            "Kb": ["PlKb.dat"], "Lk": ["PlLk.dat", "PlCl.dat"], "Pk": ["PlPk.dat", "PlPc.dat"], "Dk": ["PlDk.dat"],
            "Ys": ["PlYs.dat"], "Ns": ["PlNs.dat"], "Pr": ["PlPr.dat"], "Kp": ["PlKp.dat"], "Mt": ["PlMt.dat"],
            "Ic": ["PlPp.dat", "PlNn.dat"], "Pe": ["PlPe.dat"], "Ms": ["PlMs.dat"], "Zd": ["PlZd.dat", "PlSk.dat"],
            "Lg": ["PlLg.dat"], "Gn": ["PlGn.dat"], "Fe": ["PlFe.dat"]}
# Fighter command sizes in words: opcodes 0-9 (the shared script commands), then ftaction.c's table.
CMD_WORDS = [1, 1, 1, 1, 1, 2, 1, 2, 1, 1] + [5, 5, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                3, 1, 1, 1, 7, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 1, 4]


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def disc_files(iso, names):
    out = {}
    with open(iso, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
        count = be32(fst, 8)
        strings = count * 12
        wanted = {n.lower(): n for n in names}
        for i in range(1, count):
            w, off, size = struct.unpack_from(">III", fst, i * 12)
            if w >> 24:
                continue
            n = fst[strings + (w & 0xFFFFFF):].split(b"\0", 1)[0].decode("latin-1")
            if n.lower() in wanted:
                f.seek(off)
                out[wanted[n.lower()]] = f.read(size)
    return out


class Dat:
    def __init__(self, raw):
        self.raw = raw
        _, dsize, nrel, nroot, nref = struct.unpack_from(">IIIII", raw, 0)
        self.data = raw[0x20:0x20 + dsize]
        rel_at = 0x20 + dsize
        self.relocs = sorted(be32(raw, rel_at + 4 * i) for i in range(nrel))
        self.reloc_set = set(self.relocs)
        root_at = rel_at + 4 * nrel
        strings = root_at + 8 * (nroot + nref)
        self.roots = {}
        for i in range(nroot):
            off, name = struct.unpack_from(">II", raw, root_at + 8 * i)
            self.roots[raw[strings + name:].split(b"\0", 1)[0].decode("latin-1")] = off
        starts = {len(self.data)} | set(self.roots.values())
        for r in self.relocs:
            starts.add(be32(self.data, r))
        self.starts = sorted(starts)

    def word(self, o):
        return be32(self.data, o)

    def ptr(self, o):
        return self.word(o) if o in self.reloc_set else None

    def block_end(self, o):
        import bisect
        i = bisect.bisect_right(self.starts, o)
        return self.starts[i] if i < len(self.starts) else len(self.data)


# ---- request ids -> gfx ids (the decomp's effect tables) ----
NUM = r"(0x[0-9A-Fa-f]+|\d+)U?"


def request_table():
    table = defaultdict(list)   # request id -> [(kind, gfx id)]
    for name in ("efsync.c", "efalt.c", "efasync.c"):
        text = (DECOMP / "ef" / name).read_text(errors="replace")
        text = re.sub(r"([(,])\s*\n\s*", r"\1 ", text)   # a call's arguments on one line
        labels, body_started = [], False
        for line in text.splitlines():
            m = re.match(r"\s*case\s+" + NUM + r"\s*:", line)
            if m:
                if body_started:
                    labels, body_started = [], False
                labels.append(int(m.group(1), 0))
                rest = line[m.end():]
            else:
                rest = line
            if re.match(r"\s*(default\s*:|switch\s*\()", rest):
                labels, body_started = [], False
                continue
            if not labels:
                continue
            for call in re.finditer(r"(efLib_Create\w*|hsd_8039EFAC)\s*\(([^;]*)", rest):
                fn, args = call.group(1), call.group(2)
                if fn == "hsd_8039EFAC":
                    a = re.match(r"\s*\w+\s*,\s*" + NUM + r"\s*,\s*" + NUM, args)
                    if a:
                        for lab in labels:
                            table[lab].append(("gen", int(a.group(2), 0)))
                        body_started = True
                    continue
                a = re.match(r"\s*" + NUM, args)
                if a:
                    kind = "gen" if "Generator" in fn else "model"
                    for lab in labels:
                        table[lab].append((kind, int(a.group(1), 0)))
                    body_started = True
            if re.search(r"\bbreak\s*;|\breturn\b", rest) and labels:
                body_started = True
    return table


def code_requests():
    """Request ids asked for by the fighters' and items' code: {request id: {file, ...}} (a file asks
    for other fighters' effects too; the bank of what a request makes says whose)."""
    asked = defaultdict(set)
    paths = sorted((DECOMP / "ft" / "kinds").glob("*/*.c")) + sorted((DECOMP / "it").glob("**/*.c"))
    for path in paths:
        text = re.sub(r"\(\s*\n\s*", "(", path.read_text(errors="replace"))
        for m in re.finditer(r"efSync_Spawn\(\s*" + NUM, text):
            asked[int(m.group(1), 0)].add(path.stem)
        for m in re.finditer(r"efAsync_Spawn\([^,]*,[^,]*,[^,]*,\s*" + NUM, text):
            asked[int(m.group(1), 0)].add(path.stem)
        # a generator or model made by its id (bank * 1000 + index) in the code itself
        for m in re.finditer(r"(efLib_Create\w*|hsd_8039EFAC)\(\s*(?:\w+\s*,\s*\w+\s*,\s*)?" + NUM, text):
            v = int(m.group(2), 0)
            if v >= 1000:
                asked[("gen" if "Generator" in m.group(1) or m.group(1) == "hsd_8039EFAC" else "model", v)].add(path.stem)
    return asked


def figatree_keys(raw):
    """(bank, generator id) particle keys of one move's animation (a FigaTree DAT of Pl<Xx>AJ.dat:
    lb/lbanim.h; track obj_type 0x28 is the joint's particle track, as in jobj.c)."""
    try:
        d = Dat(raw)
    except struct.error:
        return set()
    found = set()
    for name, t in d.roots.items():
        nodes, tracks = d.ptr(t + 0xC), d.ptr(t + 0x10)
        if nodes is None or tracks is None:
            continue
        count, at = 0, nodes
        while at < len(d.data) and d.data[at] != 0xFF and count < 4096:
            count += d.data[at]
            at += 1
        for i in range(count):
            tr = tracks + 0xC * i
            if tr + 0xC > len(d.data):
                break
            length = struct.unpack_from(">H", d.data, tr)[0]
            typ, fv, fs = d.data[tr + 4], d.data[tr + 5], d.data[tr + 6]
            ad = d.ptr(tr + 8)
            if typ == 0x28 and ad is not None:
                for v in fobj_values(d, ad, length, fv | (fs << 8), raw=True):
                    iv = int(v)
                    found.add((iv & 0x3F, (iv >> 6) & 0xFFFFFF))
    return found


# ---- the fighter's move scripts ----
def script_requests(raw, anims=None):
    """{action name: {request ids}} from a fighter file's move table (records 0x18: name, anim, size,
    script, ...); a record is found by its name pointer reaching an "_ACTION_" string. With the
    fighter's animation file, each move's particle keys too, as ("key", bank, id)."""
    d = Dat(raw)
    out = defaultdict(set)
    if anims is not None:
        for r in d.relocs:
            target = d.word(r)
            if target >= len(d.data) or r + 16 > len(d.data):
                continue
            name = d.data[target:target + 96].split(b"\0", 1)[0]
            if b"_ACTION_" not in name:
                continue
            action = name.decode("latin-1").split("_ACTION_", 1)[1].replace("_figatree", "")
            off, size = d.word(r + 4), d.word(r + 8)
            if 0 < size and off + size <= len(anims):
                for bank, gid in figatree_keys(anims[off:off + size]):
                    out[action].add(("key", bank, gid))
    for r in d.relocs:
        target = d.word(r)
        if target >= len(d.data):
            continue
        name = d.data[target:target + 96].split(b"\0", 1)[0]
        if b"_ACTION_" not in name or r + 16 > len(d.data):
            continue
        script = d.ptr(r + 12)
        if script is None:
            continue
        action = name.decode("latin-1").split("_ACTION_", 1)[1].replace("_figatree", "")
        seen, todo = set(), [script]
        while todo:
            at = todo.pop()
            for _ in range(4096):
                if at in seen or at + 4 > len(d.data):
                    break
                seen.add(at)
                op = d.data[at] >> 2
                if op == 0:
                    break
                if op == 10:
                    out[action].add(be32(d.data, at + 4) >> 16)
                if op in (5, 7):
                    p = d.ptr(at + 4)
                    if p is not None:
                        todo.append(p)
                    if op == 7:
                        break
                if op == 6:
                    break
                words = CMD_WORDS[op] if op < len(CMD_WORDS) else 1
                at += 4 * words
    return out


# ---- the effect file ----
def gen_records(d, bank):
    b = d.data[bank:]
    version = struct.unpack_from(">H", b, 0)[0]
    first, n, at = (0, be32(b, 4), 8) if version == 0 else (be32(b, 4), be32(b, 8), 12)
    offs = [be32(b, at + 4 * i) for i in range(n)]
    end = d.block_end(bank + 1) - bank
    bounds = sorted(set(o for o in offs if o) | {end})
    recs = []
    for o in offs:
        if not o:
            recs.append(None)
            continue
        e = next(x for x in bounds if x > o)
        recs.append(bytes(b[o:e]))
    return first, recs


def fobj_values(d, ad, length, frac, raw=False):
    """The key values of one FObj stream (fobj.c). raw: a float key as its 32 bits (a particle
    track's key is an int: bank | generator id << 6, jobj.c case 0x28)."""
    b, at, out = d.data, ad, []
    end = ad + length

    def value(frac):
        nonlocal at
        if (frac & 0xE0) == 0 and (frac & 0x1F) == 0:
            v = struct.unpack_from("<I" if raw else "<f", b, at)[0]
            at += 4
            return v
        denom = 1 << (frac & 0x1F)
        kind = frac & 0xE0
        if kind == 0x60:
            v = struct.unpack_from("b", b, at)[0]; at += 1
        elif kind == 0x80:
            v = b[at]; at += 1
        elif kind == 0x20:
            v = struct.unpack_from("<h", b, at)[0]; at += 2
        elif kind == 0x40:
            v = struct.unpack_from("<H", b, at)[0]; at += 2
        else:
            v = struct.unpack_from("<f", b, at)[0]; at += 4
            return v
        return v / denom

    def wait():
        nonlocal at
        while at < end and b[at] & 0x80:
            at += 1
        at += 1

    while at < end:
        op = b[at] & 0xF
        n = ((b[at] >> 4) & 7) + 1
        shift = 3
        more = b[at] & 0x80
        at += 1
        while more and at < end:
            n += (b[at] & 0x7F) << shift
            shift += 7
            more = b[at] & 0x80
            at += 1
        for _ in range(n):
            if at >= end:
                break
            if op == 5:   # SLP: a slope, no wait
                value(frac >> 8)
                continue
            out.append(value(frac & 0xFF))
            if op == 4:
                value(frac >> 8)
            wait()
    return out


def particle_keys(d, model):
    """Generator ids a model effect's joints and animations start: [(bank, id)]."""
    found = set()
    seen = set()

    def joint(o):
        while o is not None and o not in seen:
            seen.add(o)
            flags = d.word(o + 4)
            if flags & (1 << 5):
                p = d.ptr(o + 0x10)
                while p is not None and p not in seen:
                    seen.add(p)
                    data = d.word(p + 4)
                    found.add((data & 0x3F, (data >> 6) & 0xFFFFFF))
                    p = d.ptr(p)
            joint(d.ptr(o + 8))
            o = d.ptr(o + 0xC)

    def anim(o):
        while o is not None and ("a", o) not in seen:
            seen.add(("a", o))
            aobj = d.ptr(o + 8)
            if aobj is not None:
                f = d.ptr(aobj + 8)
                while f is not None and ("f", f) not in seen:
                    seen.add(("f", f))
                    length = d.word(f + 4)
                    typ, fv, fs = d.data[f + 0xC], d.data[f + 0xD], d.data[f + 0xE]
                    ad = d.ptr(f + 0x10)
                    if typ == 0x28 and ad is not None:
                        for v in fobj_values(d, ad, length, fv | (fs << 8), raw=True):
                            iv = int(v)
                            found.add((iv & 0x3F, (iv >> 6) & 0xFFFFFF))
                    f = d.ptr(f)
            anim(d.ptr(o))
            o = d.ptr(o + 4)

    joint(model[0])
    anim(model[1])
    return sorted(found)


def survey(name, raw, pl, reqs, code_asked):
    d = Dat(raw)
    tname = next(n for n in d.roots if n.endswith("DataTable"))
    t = d.roots[tname]
    end = d.block_end(t + 1)
    code = name[2:4]
    bank = next(k for k, v in BANKS.items() if v == code)
    first, recs = gen_records(d, d.ptr(t)) if d.ptr(t) is not None else (bank * 1000, [])
    models = [[d.ptr(at + 4 + 4 * k) for k in range(4)] for at in range(t + 8, end - 0x13, 0x14)]
    owners = defaultdict(set)   # ("model"|"gen", index) -> {why}
    # requests: by code file and by move script
    by_request = defaultdict(set)
    for req, files in code_asked.items():
        if isinstance(req, tuple):
            if req[1] // 1000 == bank:
                owners[(req[0], req[1] % 1000)] |= {"code:" + f + " (direct)" for f in files}
            continue
        by_request[req] |= {"code:" + f for f in files}
    for plname, actions in pl.items():
        for action, ids in actions.items():
            for req in ids:
                if isinstance(req, tuple):
                    if req[1] == bank:
                        owners[("gen", req[2] % 1000)].add(plname[:4] + ":" + action + " key")
                    continue
                by_request[req].add(plname[:4] + ":" + action)
    for req, whys in by_request.items():
        targets = list(reqs.get(req, []))
        if not targets and bank * 1000 <= req < bank * 1000 + 1000:
            targets = [("gen", req)]   # a script's gfx id that is a generator id itself
        for kind, gfx in targets:
            if gfx // 1000 != bank:
                continue
            owners[(kind, gfx % 1000)] |= {f"{w} (req {req:#x})" for w in whys}
    for i, m in enumerate(models):
        for b, gid in particle_keys(d, m):
            if b == bank:
                owners[("gen", gid % 1000)].add(f"model {i} key")
    for i, r in enumerate(recs):
        if not r:
            continue
        code_bytes = r[0x3C:]
        for at in range(len(code_bytes) - 2):
            op = code_bytes[at]
            if op in (0xA5, 0xEF, 0xF0):
                gid = (code_bytes[at + 1] << 8) | code_bytes[at + 2]
                if first <= gid < first + len(recs) and gid != first + i:
                    owners[("gen", gid - first)].add(f"gen {first + i} program")
    print(f"== {name} ({tname}, bank {bank}): {len(models)} models, {len(recs)} generators from {first}")
    for i in range(len(models)):
        print(f"  model {i}: {'; '.join(sorted(owners.get(('model', i), []))) or '-'}")
    for i, r in enumerate(recs):
        tex = struct.unpack_from(">H", r, 2)[0] if r else None
        print(f"  gen {first + i} (tex {tex}): {'; '.join(sorted(owners.get(('gen', i), []))) or '-'}")


def main():
    iso = sys.argv[1]
    names = sys.argv[2:] or [f"Ef{c}Data.dat" for c in PL_FILES]
    want = set(names)
    for n in names:
        want |= set(PL_FILES.get(n[2:4], []))
        want |= {p[:-4] + "AJ.dat" for p in PL_FILES.get(n[2:4], [])}
    files = disc_files(iso, want)
    reqs = request_table()
    code_asked = code_requests()
    for n in names:
        if n not in files:
            print(f"== {n}: not on the disc")
            continue
        pl = {p: script_requests(files[p], files.get(p[:-4] + "AJ.dat")) for p in PL_FILES.get(n[2:4], [])
              if p in files}
        survey(n, files[n], pl, reqs, code_asked)


if __name__ == "__main__":
    main()
