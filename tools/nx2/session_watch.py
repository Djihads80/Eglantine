#!/usr/bin/env python3
"""Unattended crash recorder for Eglantine's built-in MCP server (start the emulator with --mcp).

Streams the timeline to disk with repeats collapsed, samples status, snapshots the emulator when
a trigger fires (crash event, burst of unmapped guest accesses, host fault, device loss), and
writes a post-mortem when the server goes away. Significant events print one line each, so the
output can be followed by a person or an agent.

    python3 -I tools/nx2/session_watch.py --out DIR [--binary build/bin/eglantine] [--no-pause]

Read-only except for two opt-outs: pausing on guest corruption (--no-pause) and IPC tracing
(--ipc-prefix, off by default). Nothing is written to the emulator otherwise.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

DATA = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "eglantine"
TOKEN = os.environ.get("EGLANTINE_MCP_TOKEN", "")
NUMBER = re.compile(r"0x[0-9a-fA-F]+|\d+")
UNMAPPED = re.compile(r"Unmapped (?:Read|Write)\d+ @")
DEVICE_LOSS = re.compile(r"DEVICE_LOST|device (?:was )?lost", re.I)
COOLDOWN = {"guest-corruption": 120, "crash": 10, "host-fault": 10, "device-loss": 60}
UNMAPPED_BURST = 20  # accesses within one second


class Watcher:
    def __init__(self, args):
        self.a = args
        self.url = args.url.rstrip("/")
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.started = time.time()
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.since = 0
        self.up = False
        self.sid = None
        self.recent = collections.deque(maxlen=300)
        self.ipc_ring = collections.deque(maxlen=4000)
        self.ipc_counts = collections.Counter()
        self.fired = []
        self.last_fire = {}
        self.unmapped = collections.deque()
        self.last_status = {}
        self.run = None  # [key, first_event, count, last_seq, written_count, last_time]
        self.timeline = open(self.out / "timeline.jsonl", "a", buffering=1)
        self.status_log = open(self.out / "status.jsonl", "a", buffering=1)
        self.symbols = Path(args.symbols) if args.symbols else self.out.parent / "symbols"

    def say(self, text):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    # ------------------------------------------------------------------ http / mcp
    def request(self, path, timeout, body=None, sid=None):
        req = urllib.request.Request(self.url + path, data=body, method="POST" if body else "GET")
        if TOKEN:
            req.add_header("Authorization", "Bearer " + TOKEN)
        if body is not None:
            req.add_header("Content-Type", "application/json")
            req.add_header("Accept", "application/json, text/event-stream")
        if sid:
            req.add_header("Mcp-Session-Id", sid)
        return urllib.request.urlopen(req, timeout=timeout)

    def rpc(self, message, sid=None):
        with self.request("/mcp", 30, json.dumps(message).encode(), sid) as r:
            return r.headers.get("Mcp-Session-Id"), r.read().decode() or "{}"

    def tool(self, name, args=None):
        for attempt in range(2):
            if not self.sid:
                self.sid, _ = self.rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                        "params": {"protocolVersion": "2025-06-18",
                                                   "clientInfo": {"name": "session_watch"}}})
                self.rpc({"jsonrpc": "2.0", "method": "notifications/initialized"}, self.sid)
            try:
                _, text = self.rpc({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                    "params": {"name": name, "arguments": args or {}}}, self.sid)
                return json.loads(text)
            except urllib.error.HTTPError as e:
                if e.code not in (400, 404) or attempt:
                    raise
                self.sid = None

    # ------------------------------------------------------------------ timeline
    def flush_run(self):
        run = self.run
        if run and run[2] != run[4]:
            self.timeline.write(json.dumps({"run_of": run[1]["seq"], "seq_last": run[3],
                                            "n": run[2], "t": round(run[5], 3)}) + "\n")
            run[4] = run[2]

    def record(self, e):
        now = time.time()
        if e["kind"] == "ipc":
            # Hundreds per second: count per command on disk, keep the last few thousand in full.
            with self.lock:
                self.ipc_ring.append(e)
                self.ipc_counts[e["msg"].split(" req [")[0]] += 1
            return
        key = (e["kind"], e["source"], NUMBER.sub("#", e["msg"]))
        with self.lock:
            self.recent.append(e)
            if self.run and self.run[0] == key:
                self.run[2] += 1
                self.run[3] = e["seq"]
                self.run[5] = now
                return
            self.flush_run()
            self.run = [key, e, 1, e["seq"], 1, now]
            self.timeline.write(json.dumps({"t": round(now, 3), **e}) + "\n")

    def stream(self):
        while not self.stop.is_set():
            try:
                with self.request(f"/stream/timeline?since_seq={self.since}", 40) as r:
                    for raw in r:
                        if raw.startswith(b"data: "):
                            e = json.loads(raw[6:])
                            self.since = e["seq"]
                            self.record(e)
                            self.inspect(e)
            except (OSError, ValueError):
                pass
            self.stop.wait(2)

    def flush_ipc(self):
        if self.ipc_counts:
            self.timeline.write(json.dumps({"t": round(time.time(), 3),
                                            "ipc_per_2s": dict(self.ipc_counts)}) + "\n")
            self.ipc_counts.clear()

    def ticker(self):
        while not self.stop.wait(2):
            with self.lock:
                self.flush_run()
                self.flush_ipc()

    # ------------------------------------------------------------------ triggers
    def inspect(self, e):
        msg, now = e["msg"], time.time()
        if e["kind"] == "ipc":
            return
        if e["kind"] == "crash":
            self.fire("crash", msg)
        elif UNMAPPED.search(msg):
            q = self.unmapped
            q.append(now)
            while q and now - q[0] > 1.0:
                q.popleft()
            if len(q) >= UNMAPPED_BURST:
                self.fire("guest-corruption", f"{len(q)} unmapped accesses in 1 s, latest: {msg}")
        elif "Unhandled SIGSEGV" in msg or "Unhandled SIGBUS" in msg:
            self.fire("host-fault", msg)
        elif DEVICE_LOSS.search(msg):
            self.fire("device-loss", msg)

    def fire(self, reason, detail):
        now = time.time()
        with self.lock:
            if now - self.last_fire.get(reason, 0) < COOLDOWN[reason]:
                return
            self.last_fire[reason] = now
            self.fired.append((time.strftime("%H:%M:%S"), reason, detail))
            n = len(self.fired)
        self.say(f"TRIGGER {reason}: {detail[:180]}")
        threading.Thread(target=self.snapshot, args=(n, reason), daemon=True).start()

    def snapshot(self, n, reason):
        d = self.out / f"snap-{n:02d}-{reason}"
        d.mkdir(exist_ok=True)
        if reason == "guest-corruption" and not self.a.no_pause:
            self.pause()
        self.save(d / "status.json", "/status.json", 5)
        self.save(d / "screen.png", "/screen.png?w=960", 10)
        for name in ("process_info", "list_threads", "backtrace", "ipc_summary"):
            try:
                (d / f"{name}.json").write_text(json.dumps(self.tool(name), indent=1))
            except (OSError, ValueError) as e:
                (d / f"{name}.error").write_text(str(e))
        self.say(f"snapshot saved: {d}")

    def save(self, path, endpoint, timeout):
        try:
            with self.request(endpoint, timeout) as r:
                path.write_bytes(r.read())
        except OSError as e:
            path.with_suffix(".error").write_text(str(e))

    def pause(self):
        try:
            self.tool("acquire_control", {"label": "session_watch", "ttl_s": 60})
            self.tool("pause")
            self.tool("release_control")
            self.say("emulator paused for inspection (resume with the MCP 'resume' tool)")
        except (OSError, ValueError) as e:
            self.say(f"could not pause: {e}")

    # ------------------------------------------------------------------ health
    def health(self):
        misses, last_sample = 0, 0.0
        while not self.stop.is_set():
            try:
                with self.request("/status.json", 3) as r:
                    status = json.loads(r.read())
                misses = 0
                self.last_status = status
                if not self.up:
                    self.up = True
                    self.say("emulator up")
                    self.on_up()
                if time.time() - last_sample >= 5:
                    last_sample = time.time()
                    self.status_log.write(json.dumps({"t": round(last_sample, 3), **status}) + "\n")
            except (OSError, ValueError):
                misses += 1
                if self.up and misses >= 3:
                    self.up, self.sid, self.since = False, None, 0
                    self.post_mortem()
            self.stop.wait(2)

    def on_up(self):
        for call in (("mark", {"label": "session_watch attached"}),
                     ("ipc_trace", {"enable": True, "service_prefix": self.a.ipc_prefix})
                     if self.a.ipc_prefix else None):
            if call:
                try:
                    self.tool(*call)
                except (OSError, ValueError) as e:
                    self.say(f"{call[0]} failed: {e}")

    # ------------------------------------------------------------------ post-mortem
    def new_dumps(self):
        return sorted(p for p in Path(self.a.crash_dir).glob("*.dmp")
                      if p.stat().st_mtime >= self.started)

    def stackwalk(self, dump):
        exe = shutil.which("minidump_stackwalk")
        if not exe:
            return "minidump_stackwalk not installed"
        r = subprocess.run([exe, str(dump), str(self.symbols)], capture_output=True, text=True,
                           timeout=180)
        (self.out / "stackwalk.txt").write_text(r.stdout + "\n--- stderr (tail) ---\n" + r.stderr[-2000:])
        lines, frames, crashed = r.stdout.splitlines(), [], False
        head = [x for x in lines if x.startswith(("Crash reason", "Crash address"))]
        for x in lines:
            if "(crashed)" in x:
                crashed = True
            elif crashed and re.match(r"\s+\d+\s+\S", x):
                frames.append(x.rstrip())
            elif crashed and not x.strip():
                break
        return "\n".join(head + frames[:25]) or "no crashed thread found in the stackwalk"

    def symbolize(self, trace):
        """Resolve `#NN module+0xoffset symbol [addr]` frames from crashtrace.so with addr2line."""
        frame = re.compile(r"^#\d+ (\S+)\+(0x[0-9a-f]+) \S* ?\[0x[0-9a-f]+\]$")
        lines = trace.splitlines()
        by_module = {}
        for i, line in enumerate(lines):
            m = frame.match(line.strip())
            if m:
                by_module.setdefault(m.group(1), []).append((i, int(m.group(2), 16)))
        addr2line = shutil.which("addr2line")
        for module, items in by_module.items():
            if not (addr2line and Path(module).is_file()):
                continue
            # Return addresses point past the call: look one byte back to land on it.
            r = subprocess.run([addr2line, "-f", "-C", "-p", "-e", module,
                                *(hex(off - 1) for _, off in items)],
                               capture_output=True, text=True, timeout=180)
            for (i, _), resolved in zip(items, r.stdout.splitlines()):
                if not resolved.startswith("??"):
                    lines[i] = f"{lines[i].strip()}  =>  {resolved}"
        return "\n".join(lines)

    def post_mortem(self):
        self.say("EMULATOR GONE: collecting evidence")
        with self.lock:
            self.flush_run()
            self.flush_ipc()
            recent = list(self.recent)[-40:]
            ipc_tail = list(self.ipc_ring)
        with open(self.out / "ipc-tail.jsonl", "w") as f:
            f.writelines(json.dumps(e) + "\n" for e in ipc_tail)
        dumps = []
        for _ in range(10):
            dumps = self.new_dumps()
            if dumps:
                break
            time.sleep(1)
        report = [f"# Post-mortem {time.strftime('%Y-%m-%d %H:%M:%S')}", "",
                  f"Last status: `{json.dumps(self.last_status)[:400]}`", "", "## Triggers"]
        report += [f"- {t} {r}: {d[:200]}" for t, r, d in self.fired] or ["- none"]
        report += ["", "## Minidump"]
        if dumps:
            dump = dumps[-1]
            shutil.copy2(dump, self.out / dump.name)
            report += [f"`{dump}` ({dump.stat().st_size // 1024} KiB)", "```", self.stackwalk(dump), "```"]
        else:
            report.append("none written (crash handler off, or the process exited normally)")
        trace = self.out / "crashtrace.txt"
        if trace.is_file():
            report += ["", "## Crash trace (faulting thread)", "```",
                       self.symbolize(trace.read_text(errors="replace")), "```"]
        err = self.out / "stderr.txt"
        if err.is_file():
            report += ["", "## stderr (tail)", "```",
                       *err.read_text(errors="replace").splitlines()[-25:], "```"]
        try:
            shutil.copy2(self.a.log, self.out / "eden_log.txt")
        except OSError:
            pass
        j = subprocess.run(["journalctl", "--since", f"@{int(self.started)}", "--no-pager"],
                           capture_output=True, text=True).stdout
        hits = [x for x in j.splitlines()
                if re.search(r"eglantine.*(segfault|trap)|\(eglantine\).*terminated abnormally", x)]
        report += ["", "## Kernel / systemd", "```",
                   *(hits[-5:] or ["no crash line in the journal"]), "```",
                   "", "## Last log events", "```"]
        report += [f"{e['seq']} [{e['kind']}] {e['source']}: {e['msg'][:160]}" for e in recent]
        report += ["```", "", "## Last IPC calls (full list in ipc-tail.jsonl)", "```"]
        report += [f"{e['seq']} {e['msg'].split(' req [')[0][:150]}" for e in ipc_tail[-15:]]
        report.append("```")
        path = self.out / "POSTMORTEM.md"
        path.write_text("\n".join(report) + "\n")
        self.say(f"post-mortem: {path}")

    # ------------------------------------------------------------------ symbols
    def ensure_symbols(self):
        exe, dump_syms = self.a.binary, shutil.which("dump_syms")
        if not (exe and dump_syms and Path(exe).is_file()):
            return
        r = subprocess.run([dump_syms, exe], capture_output=True)
        head = r.stdout[:200].split(b"\n")[0].split()
        if r.returncode or len(head) < 5 or head[0] != b"MODULE":
            self.say("symbol dump failed, backtraces will show offsets only")
            return
        d = self.symbols / head[4].decode() / head[3].decode()
        d.mkdir(parents=True, exist_ok=True)
        (d / (head[4].decode() + ".sym")).write_bytes(r.stdout)
        self.say(f"symbols ready for build {head[3].decode()}")

    def run_forever(self):
        self.say(f"watching {self.url}, evidence in {self.out}")
        for fn in (self.stream, self.ticker, self.health, self.ensure_symbols):
            threading.Thread(target=fn, daemon=True).start()
        try:
            while not self.stop.wait(1):
                pass
        except KeyboardInterrupt:
            pass
        with self.lock:
            self.flush_run()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True, help="session directory for all evidence")
    ap.add_argument("--url", default="http://127.0.0.1:6544")
    ap.add_argument("--binary", help="emulator binary, to generate symbols for minidumps")
    ap.add_argument("--symbols", help="symbol store (default: next to --out)")
    ap.add_argument("--crash-dir", default=str(DATA / "crash_dumps"))
    ap.add_argument("--log", default=str(DATA / "log" / "eden_log.txt"))
    ap.add_argument("--no-pause", action="store_true",
                    help="do not pause the emulator when guest memory corruption is detected")
    ap.add_argument("--ipc-prefix", help="enable IPC tracing for services with this prefix")
    Watcher(ap.parse_args()).run_forever()


if __name__ == "__main__":
    sys.exit(main())
