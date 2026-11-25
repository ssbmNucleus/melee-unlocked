#!/usr/bin/env python3
# Writes port/runtime/host/retail_costume_hashes.h from a retail NTSC 1.02 disc.
# SPDX-License-Identifier: GPL-2.0-or-later
"""The size and SHA-256 of each of the 124 retail costume files (Pl<fighter><colour>.dat), so the host
can tell the retail file from a mod disc's when it needs the retail one as a reference (opponents'
skins online, host/opponent_skins.cpp). Only sizes and digests are written; no game data.

    python -I -B tools/retail_costume_hashes.py "<retail disc>.iso" > port/runtime/host/retail_costume_hashes.h
"""
import hashlib
import struct
import sys

# cosmetic_mods_internal.h families[]: file code and colours in the game's costume order.
FAMILIES = [("Ca", "Nr Gy Re Wh Gr Bu"), ("Cl", "Nr Re Bu Wh Bk"), ("Dk", "Nr Bk Re Bu Gr"), ("Dr", "Nr Re Bu Gr Bk"),
            ("Fc", "Nr Re Bu Gr"), ("Fe", "Nr Re Bu Gr Ye"), ("Fx", "Nr Or La Gr"), ("Gn", "Nr Re Bu Gr La"), ("Gw", "Nr"),
            ("Kb", "Nr Ye Bu Re Gr Wh"), ("Kp", "Nr Re Bu Bk"), ("Lg", "Nr Wh Aq Pi"), ("Lk", "Nr Re Bu Bk Wh"),
            ("Mr", "Nr Ye Bk Bu Gr"), ("Ms", "Nr Re Gr Bk Wh"), ("Mt", "Nr Re Bu Gr"), ("Nn", "Nr Ye Aq Wh"),
            ("Ns", "Nr Ye Bu Gr"), ("Pc", "Nr Re Bu Gr"), ("Pe", "Nr Ye Wh Bu Gr"), ("Pk", "Nr Re Bu Gr"),
            ("Pp", "Nr Gr Or Re"), ("Pr", "Nr Re Bu Gr Ye"), ("Sk", "Nr Re Bu Gr Wh"), ("Ss", "Nr Pi Bk Gr La"),
            ("Ys", "Nr Re Bu Ye Pi Aq"), ("Zd", "Nr Re Bu Gr Wh")]


def main():
    f = open(sys.argv[1], "rb")
    f.seek(0)
    head = f.read(0x440)
    if head[:6] != b"GALE01" or head[7] != 2:
        sys.exit("not an NTSC 1.02 disc (GALE01 revision 2)")
    fst_off, fst_size = struct.unpack_from(">II", head, 0x424)
    f.seek(fst_off)
    fst = f.read(fst_size)
    entries = struct.unpack_from(">I", fst, 8)[0]
    files = {}
    for i in range(1, entries):
        a, off, size = struct.unpack_from(">III", fst, i * 12)
        if a >> 24:
            continue
        so = entries * 12 + (a & 0xFFFFFF)
        files[fst[so:fst.index(b"\0", so)].decode("latin1")] = (off, size)
    rows = []
    for code, colours in FAMILIES:
        for colour in colours.split():
            name = f"Pl{code}{colour}.dat"
            off, size = files[name]
            f.seek(off)
            rows.append((name, size, hashlib.sha256(f.read(size)).hexdigest()))
    print("// The retail NTSC 1.02 costume files: name, size, SHA-256. Written by tools/retail_costume_hashes.py")
    print("// from a retail disc; sizes and digests only. host/opponent_skins.cpp takes a costume file from the")
    print("// open disc as the reference for an opponent's skin only when it is one of these.")
    print("// SPDX-License-Identifier: GPL-2.0-or-later")
    print("#pragma once")
    print()
    print("namespace host::skins {")
    print("struct RetailCostume { const char* file; unsigned size; const char* sha256; };")
    print("constexpr RetailCostume kRetailCostumes[] = {")
    for name, size, digest in rows:
        print(f'    {{"{name}", {size}u, "{digest}"}},')
    print("};")
    print(f"constexpr unsigned kRetailCostumeCount = {len(rows)};")
    print("}  // namespace host::skins")


if __name__ == "__main__":
    main()
