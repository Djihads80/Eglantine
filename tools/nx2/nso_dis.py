#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Disassemble a range of an NSO's .text using clang + llvm-objdump.
# Usage: nso_dis.py <llvm bin dir> <nso> <text_offset_hex> <count> <work dir>
# The NSO is only read; scratch files go to <work dir>.
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from a64_coverage import nso_text  # noqa: E402

llvm, nso, off, count, work = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4]), Path(sys.argv[5])
work.mkdir(parents=True, exist_ok=True)
text = nso_text(nso)
words = [int.from_bytes(text[i:i + 4], "little") for i in range(off, off + count * 4, 4)]
# .inst (not .incbin) so the bytes get a code mapping symbol and objdump decodes them.
(work / "range.s").write_text(".text\n" + "".join(f".inst 0x{w:08x}\n" for w in words))
subprocess.run([str(Path(llvm) / "clang.exe"), "--target=aarch64-none-elf", "-c", "range.s", "-o", "range.o"],
               cwd=work, check=True)
out = subprocess.run([str(Path(llvm) / "llvm-objdump.exe"), "-d", "-j", ".text", "--no-show-raw-insn",
                      "--mattr=+v8.5a,+mte,+lse,+rcpc,+pauth,+crc,+sha2,+dotprod,+fullfp16", "range.o"],
                     cwd=work, capture_output=True, text=True, check=True).stdout
for line in out.splitlines():
    parts = line.split(":", 1)
    if len(parts) == 2 and parts[0].strip() and all(c in "0123456789abcdef" for c in parts[0].strip()):
        print(f"{off + int(parts[0], 16):08x}:{parts[1].strip()}")
