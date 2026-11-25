"""Prints the retail move tables' fingerprints for port/runtime/host/fighter_package.cpp (kDonors): for
each fighter a classic MexTool package can be made from, its move table's rows (ft*_Init_MotionStateTable,
0x20 bytes each) and an FNV-1a hash of them as fighter.yml writes them (animationID, stateFlags,
attackID, bitFlags, five callbacks: nine big-endian words a row). A package whose embedded table starts
with its donor's rows plays as the donor's table; the host compares by this hash.
Usage: python -I tools/fighter_donor_tables.py <NTSC 1.02 disc> <decomp symbols.txt>"""
import re
import struct
import sys

iso = open(sys.argv[1], "rb")
iso.seek(0x420)
dol_at = struct.unpack(">I", iso.read(4))[0]
iso.seek(dol_at)
hdr = iso.read(0x100)
offs = struct.unpack_from(">18I", hdr, 0)
addrs = struct.unpack_from(">18I", hdr, 0x48)
sizes = struct.unpack_from(">18I", hdr, 0x90)


def read_addr(a, n):
    for o, ad, sz in zip(offs, addrs, sizes):
        if sz and ad <= a < ad + sz:
            iso.seek(dol_at + o + (a - ad))
            return iso.read(n)
    raise SystemExit("0x%08X is not in main.dol" % a)


tables = {}
for line in open(sys.argv[2], encoding="utf-8"):
    m = re.match(r"^ft\w\w_Init_MotionStateTable = \.data:0x([0-9A-F]{8}); // type:object size:0x([0-9A-F]+)", line)
    if m:
        tables[int(m.group(1), 16)] = int(m.group(2), 16)

# Internal kinds 0-26 (Mario .. Roy): the move table address m-ex gives each (moveLogicPointer).
POINTERS = [0x803C7120, 0x803C7788, 0x803C72B8, 0x803CB838, 0x803C8368, 0x803CEDC0, 0x803C7E18, 0x803CC060,
            0x803CC650, 0x803CCCB8, 0x803CD2D0, 0x803CD838, 0x803CDD78, 0x803CE2D0, 0x803CE6D0, 0x803CFEF0,
            0x803D0B00, 0x803D0628, 0x803CF420, 0x803CFA58, 0x803D0FA0, 0x803D1498, 0x803D1848, 0x803D1EA8,
            0x803D23E8, 0x803D29F8, 0x803D2E80]
for kind, pointer in enumerate(POINTERS):
    size = tables[pointer]
    data = read_addr(pointer, size)
    h = 2166136261
    rows = size // 0x20
    for i in range(rows):
        w = struct.unpack_from(">8I", data, i * 0x20)
        for v in (w[0], w[1], w[2] >> 24, (w[2] >> 16) & 0xFF) + w[3:8]:
            for b in struct.pack(">I", v):
                h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    print("kind %2d  table 0x%08X  rows %3d  hash 0x%08Xu" % (kind, pointer, rows, h))
