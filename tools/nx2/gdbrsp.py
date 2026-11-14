#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Minimal GDB remote-serial-protocol client for Eden's gdbstub (AArch64 guests).
# Usage: gdbrsp.py <port> <script>
# Script lines:  break <hexaddr> | unbreak <hexaddr> | continue | regs | x0 | mem <hexaddr|xN> <len> | bt <hexaddr|sp> <count>
#                | quit
import socket
import struct
import sys

REGS = [f"x{i}" for i in range(31)] + ["sp", "pc", "cpsr"]


class Rsp:
    def __init__(self, port):
        import time
        for _ in range(240):  # wait up to ~2 min for the stub to listen
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
                self.s.settimeout(None)  # a stop can be hours of play away
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise SystemExit("gdbstub not reachable")
        self.buf = b""

    def _read_packet(self):
        while True:
            while b"$" not in self.buf or b"#" not in self.buf[self.buf.index(b"$"):]:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise EOFError
                self.buf += chunk
            start = self.buf.index(b"$")
            end = self.buf.index(b"#", start)
            if len(self.buf) < end + 3:
                self.buf += self.s.recv(65536)
                continue
            data = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return data.decode(errors="replace")

    def cmd(self, c):
        pkt = c.encode()
        self.s.sendall(b"$" + pkt + b"#" + f"{sum(pkt) & 0xff:02x}".encode())
        return self._read_packet()

    def regs(self):
        r = self.cmd("g")
        vals = {}
        off = 0
        for name in REGS:
            n = 8 if name != "cpsr" else 4
            if off + n * 2 > len(r):
                break
            vals[name] = int.from_bytes(bytes.fromhex(r[off:off + n * 2]), "little")
            off += n * 2
        return vals

    def mem(self, addr, length):
        r = self.cmd(f"m{addr:x},{length:x}")
        try:
            return bytes.fromhex(r)
        except ValueError:
            return b""


def main():
    import re
    import time
    rsp = Rsp(int(sys.argv[1]))
    # Until the guest process exists the stub answers with a bogus thread id; commands sent then
    # are lost, so wait for a real one.
    for _ in range(120):
        reason = rsp.cmd("?")
        m = re.search(r"thread:([0-9a-f]+);", reason)
        if m and int(m.group(1), 16) < 0x10000:
            break
        time.sleep(0.5)
    print("halt reason:", reason)
    regs = {}
    for line in open(sys.argv[2]):
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        op = parts[0]
        if op == "break":
            print("break", parts[1], "->", rsp.cmd(f"Z0,{int(parts[1], 16):x},4"))
        elif op == "breaklr":
            # Break at the current return address (x30 from the last regs/x0).
            lr = regs.get("x30", 0) & 0xFFFFFFFFFFFF
            print("breaklr", hex(lr), "->", rsp.cmd(f"Z0,{lr:x},4"))
        elif op == "unbreakpc":
            pc = regs.get("pc", 0)
            print("unbreakpc", hex(pc), "->", rsp.cmd(f"z0,{pc:x},4"))
        elif op == "unbreak":
            print("unbreak", parts[1], "->", rsp.cmd(f"z0,{int(parts[1], 16):x},4"))
        elif op == "step":
            print("step:", rsp.cmd("s"))
        elif op == "trace":
            # Single-step N instructions, printing pc and x0 (stop early at an optional pc).
            n = int(parts[1])
            stop_at = int(parts[2], 16) if len(parts) > 2 else None
            lo = int(parts[3], 16) if len(parts) > 4 else 0
            hi = int(parts[4], 16) if len(parts) > 4 else 1 << 64
            for _ in range(n):
                rsp.cmd("s")
                r = rsp.regs()
                if lo <= r["pc"] < hi:
                    print(f"  t pc={r['pc']:#x} x0={r['x0']:#x} x8={r['x8']:#x}")
                if stop_at is not None and r["pc"] == stop_at:
                    break
        elif op == "breakwhen":
            # breakwhen <addr> <reg> <value> [max]: stop at addr only when reg == value.
            addr, reg, want = int(parts[1], 16), parts[2], int(parts[3], 16)
            for _ in range(int(parts[4]) if len(parts) > 4 else 50):
                rsp.cmd(f"Z0,{addr:x},4")
                rsp.cmd("c")
                regs = rsp.regs()
                if regs.get("pc") != addr:
                    print("  breakwhen: stopped elsewhere at", hex(regs.get("pc", 0)))
                    break
                if regs.get(reg) == want:
                    print(f"  breakwhen: hit with {reg}={want:#x}")
                    break
                rsp.cmd(f"z0,{addr:x},4")
                rsp.cmd("s")
            print("  " + " ".join(f"{k}={v:#x}" for k, v in regs.items() if k in ("pc", "x0", "x1", "x2", "x19", "x20", "x21", "x22", "x24", "x30")))
        elif op == "flow":
            # flow N stop_hex: single-step N instructions, print only non-sequential pcs (branches)
            n, stop_at = int(parts[1]), int(parts[2], 16)
            prev = rsp.regs().get("pc", 0)
            for _ in range(n):
                rsp.cmd("s")
                r = rsp.regs()
                pc = r.get("pc", 0)
                if pc != prev + 4:
                    print(f"  f {prev:#x} -> {pc:#x} x0={r.get('x0', 0):#x}")
                prev = pc
                if pc == stop_at:
                    break
        elif op == "hits":
            # hits <max> <addr>...: break at the addresses and, on each hit, print the registers,
            # the frame-pointer chain and the 0x40 bytes at x0, then step past and continue.
            # Stops after <max> hits or when the target stops anywhere else.
            limit, addrs = int(parts[1]), [int(a, 16) for a in parts[2:]]
            for a in addrs:
                rsp.cmd(f"Z0,{a:x},4")
            for hit in range(limit):
                reason = rsp.cmd("c")
                # Several threads can stop at once: read and step the one this reply names.
                tid = re.search(r"thread:([0-9a-f]+);", reason)
                if tid:
                    rsp.cmd(f"Hg{tid.group(1)}")
                r = rsp.regs()
                pc = r.get("pc", 0)
                stamp = time.strftime("%H:%M:%S")
                label = (f"hit {hit}" if pc in addrs else f"stopped elsewhere ({reason})") +                     (f" thread {tid.group(1)}" if tid else "")
                print(f"[{stamp}] {label} pc={pc:#x} " + " ".join(
                    f"{k}={r.get(k, 0):#x}" for k in ("x0", "x1", "x2", "x19", "x20", "x21", "x30", "sp")), flush=True)
                obj = rsp.mem(r.get("x0", 0), 0x40)
                if len(obj) == 0x40:
                    print("   *x0: " + " ".join(f"{w:016x}" for w in struct.unpack("<8Q", obj)), flush=True)
                fp = r.get("x29", 0)
                chain = []
                for _ in range(16):
                    d = rsp.mem(fp, 16)
                    if len(d) < 16:
                        break
                    nfp, ret = struct.unpack("<QQ", d)
                    chain.append(f"{ret & 0xFFFFFFFFFFFF:x}")
                    if nfp <= fp:
                        break
                    fp = nfp
                print("   bt " + " ".join(chain), flush=True)
                if pc not in addrs:
                    break
                rsp.cmd(f"z0,{pc:x},4")
                rsp.cmd("s")
                rsp.cmd(f"Z0,{pc:x},4")
            for a in addrs:
                rsp.cmd(f"z0,{a:x},4")
        elif op == "continue":
            print("stop:", rsp.cmd("c"))
        elif op == "continue_async":
            # Resume without waiting for a stop reply (pair with interrupt).
            pkt = b"c"
            rsp.s.sendall(b"$" + pkt + b"#" + f"{sum(pkt) & 0xff:02x}".encode())
        elif op == "x0":
            r = rsp.regs()
            regs = r
            print(f"  pc={r.get('pc', 0):#x} x0={r.get('x0', 0):#x} x1={r.get('x1', 0):#x} lr={r.get('x30', 0):#x}")
        elif op == "regs":
            regs = rsp.regs()
            print(" ".join(f"{k}={v:#x}" for k, v in regs.items()))
        elif op == "mem":
            base, _, off = parts[1].partition("+")
            a = (regs[base] if base in regs else int(base, 16)) + (int(off, 16) if off else 0)
            data = rsp.mem(a, int(parts[2], 0))
            for i in range(0, len(data), 16):
                words = [struct.unpack_from("<Q", data, i + j)[0] for j in range(0, min(16, len(data) - i) - 7, 8)]
                print(f"  {a + i:#x}: " + " ".join(f"{w:016x}" for w in words))
        elif op == "fp":
            # Walk the frame-pointer chain: print return addresses.
            fp = regs.get("x29", 0)
            for _ in range(int(parts[1]) if len(parts) > 1 else 12):
                d = rsp.mem(fp, 16)
                if len(d) < 16:
                    break
                nfp, ret = struct.unpack("<QQ", d)
                print(f"  frame fp={fp:#x} ret={ret & 0xFFFFFFFFFFFF:#x}")
                if nfp <= fp:
                    break
                fp = nfp
        elif op == "interrupt":
            # Stop a running target (Ctrl-C) and print the stop reply.
            rsp.s.sendall(b"")
            print("interrupt:", rsp._read_packet())
        elif op == "threads":
            # threads [frames]: every thread's pc, lr and frame-pointer chain.
            ids = []
            r = rsp.cmd("qfThreadInfo")
            while r.startswith("m"):
                ids += [t for t in r[1:].split(",") if t]
                r = rsp.cmd("qsThreadInfo")
            depth = int(parts[1]) if len(parts) > 1 else 16
            for t in ids:
                rsp.cmd(f"Hg{t}")
                r = rsp.regs()
                chain = []
                fp = r.get("x29", 0)
                for _ in range(depth):
                    d = rsp.mem(fp, 16)
                    if len(d) < 16:
                        break
                    nfp, ret = struct.unpack("<QQ", d)
                    chain.append(ret & 0xFFFFFFFFFFFF)
                    if nfp <= fp:
                        break
                    fp = nfp
                print(f"thread {t} pc={r.get('pc', 0):#x} lr={r.get('x30', 0) & 0xFFFFFFFFFFFF:#x} "
                      f"x0={r.get('x0', 0):#x} x1={r.get('x1', 0):#x} chain=" + ",".join(f"{c:#x}" for c in chain))
        elif op == "selthread":
            # selthread <lo> <hi>: select the first thread whose pc, lr or frame chain falls in
            # [lo, hi); later regs/mem ops then read that thread.
            lo, hi = int(parts[1], 16), int(parts[2], 16)
            ids = []
            r = rsp.cmd("qfThreadInfo")
            while r.startswith("m"):
                ids += [t for t in r[1:].split(",") if t]
                r = rsp.cmd("qsThreadInfo")
            for t in ids:
                rsp.cmd(f"Hg{t}")
                r = rsp.regs()
                cands = [r.get("pc", 0), r.get("x30", 0) & 0xFFFFFFFFFFFF]
                fp = r.get("x29", 0)
                for _ in range(8):
                    d = rsp.mem(fp, 16)
                    if len(d) < 16:
                        break
                    nfp, ret = struct.unpack("<QQ", d)
                    cands.append(ret & 0xFFFFFFFFFFFF)
                    if nfp <= fp:
                        break
                    fp = nfp
                if any(lo <= c < hi for c in cands):
                    regs = r
                    print("selthread", t)
                    break
            else:
                print("selthread: none")
        elif op == "hits":
            # hits <addr> <n> [memreg]: stop at addr n times, printing x0/x1/lr (and 16 bytes at
            # memreg) each time; steps off the breakpoint before resuming.
            addr, n = int(parts[1], 16), int(parts[2])
            memreg = parts[3] if len(parts) > 3 else None
            for i in range(n):
                rsp.cmd(f"Z0,{addr:x},4")
                rsp.cmd("c")
                r = rsp.regs()
                regs = r
                extra = ""
                if memreg and memreg in r:
                    extra = " mem=" + rsp.mem(r[memreg], 16).hex()
                print(f"  hit {i} pc={r.get('pc', 0):#x} x0={r.get('x0', 0):#x} x1={r.get('x1', 0):#x} "
                      f"x19={r.get('x19', 0):#x} x20={r.get('x20', 0):#x}" + extra)
                if r.get("pc") != addr:
                    break
                rsp.cmd(f"z0,{addr:x},4")
                rsp.cmd("s")
        elif op == "watch":
            # watch <addr> <n> [stopval]: write watchpoint on a u64; print pc, lr and the new value
            # for up to n hits, stopping early once it equals stopval.
            addr, n = int(parts[1], 16), int(parts[2])
            stopval = int(parts[3], 16) if len(parts) > 3 else None
            print("watch ->", rsp.cmd(f"Z2,{addr:x},8"))
            for i in range(n):
                reply = rsp.cmd("c")
                r = rsp.regs()
                regs = r
                d = rsp.mem(addr, 8)
                val = struct.unpack("<Q", d)[0] if len(d) == 8 else None
                chain = []
                fp = r.get("x29", 0)
                for _ in range(6):
                    fd = rsp.mem(fp, 16)
                    if len(fd) < 16:
                        break
                    nfp, ret = struct.unpack("<QQ", fd)
                    chain.append(ret & 0xFFFFFFFFFFFF)
                    if nfp <= fp:
                        break
                    fp = nfp
                print(f"  w{i} {reply[:40]} pc={r.get('pc', 0):#x} lr={r.get('x30', 0) & 0xFFFFFFFFFFFF:#x} value={val if val is None else hex(val)} chain=" + ",".join(f"{c:#x}" for c in chain))
                if stopval is not None and val == stopval:
                    break
                # Step off the access with the watchpoint removed, then re-arm it.
                rsp.cmd(f"z2,{addr:x},8")
                rsp.cmd("s")
                rsp.cmd(f"Z2,{addr:x},8")
        elif op == "monitor":
            # monitor <command...>: run a gdbstub monitor command (qRcmd) and print its text.
            reply = rsp.cmd("qRcmd," + " ".join(parts[1:]).encode().hex())
            try:
                print(bytes.fromhex(reply).decode(errors="replace"))
            except ValueError:
                print("monitor:", reply)
        elif op == "raw":
            # raw <packet>: send one packet and print the reply (e.g. qXfer:threads:read::0,ffff).
            print("raw:", rsp.cmd(parts[1]))
        elif op == "sleep":
            import time
            time.sleep(float(parts[1]))
        elif op == "quit":
            break
    rsp.s.close()


if __name__ == "__main__":
    main()
