#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Print the META header and kernel capabilities of a main.npdm (read only).
import struct
import sys

d = open(sys.argv[1], "rb").read()
assert d[:4] == b"META"
flags = d[0xC]
print(f"META flags=0x{flags:02x} is64={flags & 1} address_space_type={(flags >> 1) & 7} "
      f"prio={d[0xE]} core={d[0xF]} sys_res=0x{struct.unpack_from('<I', d, 0x10)[0]:x} "
      f"version=0x{struct.unpack_from('<I', d, 0x14)[0]:x} stack=0x{struct.unpack_from('<I', d, 0x1C)[0]:x} "
      f"name={d[0x20:0x30].rstrip(bytes(1)).decode(errors='replace')}")
aci_off, aci_sz, acid_off, acid_sz = struct.unpack_from("<IIII", d, 0x70)
print(f"ACI0 @0x{aci_off:x}+0x{aci_sz:x}  ACID @0x{acid_off:x}+0x{acid_sz:x}")
a = d[aci_off:aci_off + aci_sz]
assert a[:4] == b"ACI0", a[:4]
pid = struct.unpack_from("<Q", a, 0x10)[0]
fs_off, fs_sz, sac_off, sac_sz, kc_off, kc_sz = struct.unpack_from("<IIIIII", a, 0x20)
print(f"program_id={pid:016x}  fs=0x{fs_sz:x} sac=0x{sac_sz:x} kc=0x{kc_sz:x}")
sac = a[sac_off:sac_off + sac_sz]
i, names = 0, []
while i < len(sac):
    ctl = sac[i]
    n = (ctl & 7) + 1
    names.append(("S:" if ctl & 0x80 else "") + sac[i + 1:i + 1 + n].decode(errors="replace"))
    i += 1 + n
print("services:", " ".join(names))
kc = a[kc_off:kc_off + kc_sz]
svcs = set()
for (cap,) in struct.iter_unpack("<I", kc):
    if cap == 0xFFFFFFFF:
        continue
    t = (~cap & (cap + 1)).bit_length() - 1  # number of trailing ones
    if t == 3:
        lo, hi = cap >> 4 & 0x3F, cap >> 10 & 0x3F
        cl, ch = cap >> 16 & 0xFF, cap >> 24
        print(f"  ThreadInfo prio {lo}..{hi} cores {cl}..{ch}")
    elif t == 4:
        idx, mask = cap >> 29, cap >> 5 & 0xFFFFFF
        for b in range(24):
            if mask >> b & 1:
                svcs.add(idx * 24 + b)
    elif t == 6:
        print(f"  MapRange 0x{cap:08x}")
    elif t == 7:
        print(f"  MapIoPage 0x{cap:08x}")
    elif t == 10:
        print(f"  MapRegion 0x{cap:08x}")
    elif t == 11:
        print(f"  EnableInterrupts 0x{cap:08x}")
    elif t == 13:
        print(f"  ProgramType {cap >> 14 & 7}")
    elif t == 14:
        print(f"  KernelVersion {cap >> 19 & 0x1FFF}.{cap >> 15 & 0xF}")
    elif t == 15:
        print(f"  HandleTableSize {cap >> 16 & 0x3FF}")
    elif t == 16:
        print(f"  DebugFlags 0x{cap:08x}")
    else:
        print(f"  unknown cap type {t}: 0x{cap:08x}")
print("SVCs:", " ".join(f"{s:02x}" for s in sorted(svcs)))
