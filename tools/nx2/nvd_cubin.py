#!/usr/bin/env python3
"""Disassemble raw Ampere (SM86) SASS with nvdisasm by patching the code into a cubin.

usage: nvd_cubin.py <raw dump> [skip_bytes]

The dump is 16 bytes per instruction: EDEN_NX2_SHADER_DUMP files (<key>-gfx-<addr>.bin, no header),
or a GPU range dump (graphics programs start with a 0x50-byte header: pass 0x50 as skip_bytes).
A template kernel is compiled once with nvcc into the cache directory; its .text section is
replaced by the dump. One illegal word makes nvdisasm stop, so dis() falls back to one
instruction at a time (slow on zero padding: trim the dump to the last non-zero instruction).
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

NVCC = '/opt/cuda/bin/nvcc'
NVDISASM = '/opt/cuda/bin/nvdisasm'
CACHE = os.path.join(tempfile.gettempdir(), 'nvd_cubin_cache')
TEMPLATE_SRC = '__global__ void k(float* a) { a[threadIdx.x] = a[0] * 2.f + a[1]; }\n'


def template():
    cubin = os.path.join(CACHE, 'k.cubin')
    if not os.path.exists(cubin):
        os.makedirs(CACHE, exist_ok=True)
        src = os.path.join(CACHE, 'k.cu')
        with open(src, 'w') as f:
            f.write(TEMPLATE_SRC)
        subprocess.run([NVCC, '-cubin', '-arch=sm_86', src, '-o', cubin], check=True)
    with open(cubin, 'rb') as f:
        return f.read()


def build(code):
    """Return a cubin whose .text section is `code` (appended at the end of the file)."""
    t = bytearray(template())
    shoff, = struct.unpack_from('<Q', t, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', t, 0x3a)
    strtab_off, = struct.unpack_from('<Q', t, shoff + shstrndx * shentsize + 24)
    text = None
    for i in range(shnum):
        base = shoff + i * shentsize
        name_off, = struct.unpack_from('<I', t, base)
        end = t.index(b'\0', strtab_off + name_off)
        if t[strtab_off + name_off:end].startswith(b'.text.'):
            text = base
    assert text is not None, 'no .text section in template'
    old_size, = struct.unpack_from('<Q', t, text + 32)
    out = t + code
    struct.pack_into('<Q', out, text + 24, len(t))      # sh_offset
    struct.pack_into('<Q', out, text + 32, len(code))   # sh_size
    # The function symbol's st_size must follow (symtab entries are 24 bytes, st_size at +16).
    for i in range(shnum):
        base = shoff + i * shentsize
        sh_type, = struct.unpack_from('<I', t, base + 4)
        if sh_type == 2:  # SHT_SYMTAB
            off, size = struct.unpack_from('<QQ', t, base + 24)
            for e in range(off, off + size, 24):
                if struct.unpack_from('<Q', out, e + 16)[0] == old_size:
                    struct.pack_into('<Q', out, e + 16, len(code))
    return bytes(out)


def run(code):
    with tempfile.NamedTemporaryFile(suffix='.cubin', delete=False) as f:
        f.write(build(code))
        path = f.name
    try:
        return subprocess.run([NVDISASM, '-c', path], capture_output=True, text=True)
    finally:
        os.unlink(path)


LINE = re.compile(r'^\s*/\*([0-9a-f]{4})\*/\s*(.*;)')


def listing(result):
    return [m.group(2).strip() for m in map(LINE.match, result.stdout.splitlines()) if m]


def dis(data):
    """Return 'offset text' lines for data (16 bytes per instruction)."""
    out = []
    for i in range(0, len(data) // 16, 64):
        chunk = data[i * 16:(i + 64) * 16]
        count = len(chunk) // 16
        r = run(chunk)
        lines = listing(r)
        if r.returncode == 0 and len(lines) >= count:
            out += ['%04x %s' % ((i + j) * 16, l) for j, l in enumerate(lines[:count])]
        else:
            for j in range(count):
                lines = listing(run(chunk[j * 16:(j + 1) * 16]))
                out.append('%04x %s' % ((i + j) * 16, lines[0] if lines else
                                        '??? ' + chunk[j * 16:(j + 1) * 16].hex()))
    return out


if __name__ == '__main__':
    skip = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    with open(sys.argv[1], 'rb') as f:
        print('\n'.join(dis(f.read()[skip:])))
