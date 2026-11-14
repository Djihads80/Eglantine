#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Report which AArch64 encodings in NSO .text segments are not decoded by dynarmic.
# Usage: a64_coverage.py <a64.inc> <nso> [<nso> ...]
# The NSO files are only read.
import struct
import sys
from collections import Counter

import numpy as np


def lz4_block_decompress(src, out_size):
    dst = bytearray(out_size)
    si = di = 0
    n = len(src)
    while si < n:
        tok = src[si]
        si += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[si]
                si += 1
                lit += b
                if b != 255:
                    break
        dst[di:di + lit] = src[si:si + lit]
        si += lit
        di += lit
        if si >= n:
            break
        off = src[si] | (src[si + 1] << 8)
        si += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[si]
                si += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = di - off
        if off >= ml:
            dst[di:di + ml] = dst[start:start + ml]
        else:
            for k in range(ml):
                dst[di + k] = dst[start + k]
        di += ml
    return bytes(dst)


def nso_text(path):
    data = open(path, "rb").read()
    assert data[:4] == b"NSO0", path
    flags = struct.unpack_from("<I", data, 0xC)[0]
    file_off, mem_off, size = struct.unpack_from("<III", data, 0x10)
    csize = struct.unpack_from("<I", data, 0x60)[0]
    raw = data[file_off:file_off + (csize if flags & 1 else size)]
    return lz4_block_decompress(raw, size) if flags & 1 else raw


def load_patterns(inc):
    pats = []
    for line in open(inc, encoding="utf-8"):
        line = line.strip()
        if not line.startswith("INST("):
            continue
        parts = line.split('"')
        name, bits = parts[1], parts[3]
        assert len(bits) == 32, line
        mask = expect = 0
        for c in bits:
            mask <<= 1
            expect <<= 1
            if c in "01":
                mask |= 1
                expect |= int(c)
        pats.append((name, mask, expect))
    return pats


# Named ARMv8.1+ encodings, for labelling what dynarmic rejects.
NAMED = [
    ("LSE atomic (LDADD/LDCLR/LDEOR/LDSET/LD[SU]MAX/MIN)", 0x3F208C00, 0x38200000),
    ("LSE SWP", 0x3F20FC00, 0x38208000),
    ("LDAPR (RCpc)", 0x3FFFFC00, 0x38BFC000),
    ("CAS/CASA/CASL/CASAL", 0x3FA07C00, 0x08A07C00),
    ("CASP", 0xBFA07C00, 0x08207C00),
    ("LDAPUR/STLUR (RCpc2)", 0x3F200C00, 0x19000000),
    ("RETAA/RETAB", 0xFFFFFBFF, 0xD65F0BFF),
    ("BRAA/BRAB/BLRAA/BLRAB(+Z)", 0xFEDFF800, 0xD61F0800),
    ("ERETAA/ERETAB", 0xFFFFFBFF, 0xD69F0BFF),
    ("LDRAA/LDRAB", 0xFF200400, 0xF8200400),
    ("PAC*/AUT* (register)", 0xFFFFC000, 0xDAC10000),
    ("XPACI/XPACD", 0xFFFFFBE0, 0xDAC143E0),
    ("PACGA", 0xFFE0FC00, 0x9AC03000),
    ("MTE STG/STZG/ST2G/STZ2G/LDG", 0xFF200000, 0xD9200000),
    ("MTE STGP", 0xFE400000, 0x68000000),
    ("MTE IRG/GMI", 0xFFE0E800, 0x9AC01000),
    ("MTE ADDG/SUBG", 0xBFC0C000, 0x91800000),
    ("SDOT/UDOT (vector)", 0xBF20FC00, 0x0E009400),
    ("SDOT/UDOT (element)", 0xBF00F400, 0x0F00E000),
    ("FJCVTZS", 0xFFFFFC00, 0x1E7E0000),
    ("FCMLA/FCADD", 0xBF20E400, 0x2E00C400),
    ("SQRDMLAH/SQRDMLSH (vector)", 0xBF20F400, 0x2E008400),
    ("SQRDMLAH/SQRDMLSH (element)", 0xBF00D400, 0x2F00D000),
    ("RMIF/SETF (flagm)", 0xFFE07C10, 0xBA000400),
    ("CFINV/XAFLAG/AXFLAG", 0xFFFFF0FF, 0xD500401F),
    ("DC CVAP/CVADP etc", 0xFFF80000, 0xD5080000),
]


def main():
    pats = load_patterns(sys.argv[1])
    by_mask = {}
    for _, m, e in pats:
        by_mask.setdefault(m, set()).add(e)
    total_unknown = Counter()
    for path in sys.argv[2:]:
        try:
            text = nso_text(path)
        except (IndexError, AssertionError) as e:
            print(f"== {path}: .text does not decompress as a plain LZ4 block ({type(e).__name__})")
            continue
        w = np.frombuffer(text[: len(text) // 4 * 4], dtype="<u4")
        known = np.zeros(w.shape, dtype=bool)
        for m, es in by_mask.items():
            known |= np.isin(w & np.uint32(m), np.array(sorted(es), dtype=np.uint32))
        unk = w[~known]
        named = np.zeros(unk.shape, dtype=bool)
        print(f"== {path}: {len(w)} words, {len(unk)} not decoded by dynarmic")
        for name, m, e in NAMED:
            hit = (unk & np.uint32(m)) == np.uint32(e)
            hit &= ~named
            named |= hit
            c = int(hit.sum())
            if c:
                total_unknown[name] += c
                ex = int(unk[hit][0])
                print(f"  {c:9d}  {name}  (e.g. {ex:08x})")
        rest = unk[~named]
        # Group the remainder by the top 11 bits (rough instruction class).
        groups = Counter((int(x) >> 21) for x in rest)
        examples = {}
        for x in rest:
            examples.setdefault(int(x) >> 21, int(x))
        print(f"  {len(rest):9d}  unnamed (mostly data in .text or zero padding); top groups:")
        for g, c in groups.most_common(15):
            print(f"      {c:8d}  top11={g:03x}  e.g. {examples[g]:08x}")
    print("== total named:")
    for name, c in total_unknown.most_common():
        print(f"  {c:9d}  {name}")


if __name__ == "__main__":
    main()
