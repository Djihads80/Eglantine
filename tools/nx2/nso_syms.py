#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Symbol lookup for an NSO (read only).
#   nso_syms.py <nso> addr <hex offset>...   nearest exported symbol for module offsets
#   nso_syms.py <nso> got <hex offset>...    import bound to a GOT slot (e.g. from a PLT stub)
#   nso_syms.py <nso> find <substring>       exported symbols whose name contains substring
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from a64_coverage import lz4_block_decompress  # noqa: E402

DT_NULL, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ = 0, 5, 6, 7, 8, 23, 2


def load_image(path):
    d = open(path, "rb").read()
    assert d[:4] == b"NSO0"
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = [struct.unpack_from("<III", d, 0x10 + i * 0x10) for i in range(3)]
    csizes = struct.unpack_from("<III", d, 0x60)
    bss = struct.unpack_from("<I", d, 0x3C)[0]
    end = segs[2][1] + segs[2][2] + bss
    img = bytearray(end)
    for i, (off, loc, size) in enumerate(segs):
        raw = d[off:off + (csizes[i] if flags >> i & 1 else size)]
        img[loc:loc + size] = lz4_block_decompress(raw, size) if flags >> i & 1 else raw
    return bytes(img)


def parse(img):
    mod0 = struct.unpack_from("<I", img, 4)[0]
    assert img[mod0:mod0 + 4] == b"MOD0"
    dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
    tags = {}
    i = dyn
    while True:
        tag, val = struct.unpack_from("<qQ", img, i)
        if tag == DT_NULL:
            break
        tags.setdefault(tag, val)
        i += 16
    strtab, symtab = tags[DT_STRTAB], tags[DT_SYMTAB]

    def name(idx):
        st_name = struct.unpack_from("<I", img, symtab + idx * 24)[0]
        e = img.index(b"\0", strtab + st_name)
        return img[strtab + st_name:e].decode(errors="replace")

    syms = []
    idx = 1
    # The symbol table directly precedes the string table in these modules.
    while symtab + (idx + 1) * 24 <= strtab:
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", img, symtab + idx * 24)
        if st_value and st_shndx:
            syms.append((st_value, st_size, name(idx)))
        idx += 1
    syms.sort()
    got = {}
    for start, size in ((tags.get(DT_JMPREL), tags.get(DT_PLTRELSZ)), (tags.get(DT_RELA), tags.get(DT_RELASZ))):
        if not start:
            continue
        for j in range(start, start + size, 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", img, j)
            sym = r_info >> 32
            if sym:
                got[r_off] = name(sym)
    return syms, got


def main():
    img = load_image(sys.argv[1])
    syms, got = parse(img)
    mode, args = sys.argv[2], sys.argv[3:]
    if mode == "addr":
        for a in args:
            a = int(a, 16)
            best = None
            for v, s, n in syms:
                if v <= a:
                    best = (v, s, n)
                else:
                    break
            print(f"{a:#x}: " + (f"{best[2]}+{a - best[0]:#x}" if best else "?"))
    elif mode == "got":
        for a in args:
            a = int(a, 16)
            print(f"{a:#x}: {got.get(a, '?')}")
    elif mode == "find":
        for v, s, n in syms:
            if args[0] in n:
                print(f"{v:#x} {s:#x} {n}")


if __name__ == "__main__":
    main()
