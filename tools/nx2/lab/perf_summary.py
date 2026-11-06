#!/usr/bin/env python3
"""Frame rate, stall and crash summary of an Eglantine log, split at the start of gameplay.

    python -I tools/nx2/lab/perf_summary.py [<run dir> | <log file>] [--marker TEXT] [--after S]
    python -I tools/nx2/lab/perf_summary.py --self-test

With a run dir (tools/nx2/lab/runs/<name>) it reads emulator.log, also checks eden_log.txt for
DEVICE_LOST (emulator.log has missed one), and reads phases.txt from run_oot.ps1 to tell the save's
spawn spot from the walk (without it, all of gameplay is one phase). With no argument it reads
%APPDATA%/eglantine/log/eden_log.txt.

Gameplay starts at the first log line holding --marker (default 64x36x96: Kokiri Forest's fog
volumes, created about 11 s after the save loads). The title screen and menus run at 60 FPS, so a
log without the marker has no gameplay numbers: the run never left the title screen, or the save is
somewhere else (then pass --after with the second gameplay started).

Numbers: the vi2 "last 600 presents" windows (what OOT_STABLE.md quotes), and the per-second
Nx2Frame lines (mean FPS = presents / seconds; fw = time blocked on the GPU fence).
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import statistics
import sys
import tempfile
from pathlib import Path

DEFAULT_LOG = Path(os.environ.get("APPDATA", "")) / "eglantine" / "log" / "eden_log.txt"
DEFAULT_MARKER = "64x36x96"
LIVE_SETTLE = 3.0  # seconds dropped after a live switch flip

RE_TS = re.compile(r"^\[\s*([\d.]+)\]")
RE_KV = re.compile(r"(\w+)=(\S*)")
RE_VERSION = re.compile(r"Eglantine Version:\s*(.+)")
RE_CPU = re.compile(r"Host CPU:\s*(.+)")
RE_SETTING = re.compile(r"LogSettings:\s*(?:M-|--)\s*(\S+):\s*(.+)$")
RE_PRESENT_NUM = re.compile(r"PresentClientBuffer:.*present\s+#(\d+)")
RE_VI2 = re.compile(r"last (\d+) presents: frame (\d+) us.*?blocked on the GPU fence (\d+) us")
RE_CPU_READ = re.compile(r"OnCPURead:.*\[(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)\s*\((\d+)\s+so far\)")
RE_SHADER_REJECT = re.compile(r"Native (?:graphics|compute) shader rejected:\s*(.+)")
SETTING_KEYS = ("use_asynchronous_shaders", "async_presentation", "gpu_accuracy", "use_vsync",
                "prefer_3d_vcache", "use_reactive_flushing", "use_fast_gpu_time")


def _num(value: str):
    try:
        return float(value.rstrip("%"))
    except ValueError:
        return None


def parse_log(path: Path, marker: str) -> dict:
    out = {"path": str(path), "lines": 0, "first_ts": None, "last_ts": 0.0, "version": "unknown",
           "cpu": "unknown", "gpu_split": "disabled/single-thread", "x3d": [], "settings": {},
           "perf_off": None, "device_lost": False, "marker_ts": None, "presents": [], "vi2": [],
           "frames": [], "live": [], "cpu_reads": 0, "cpu_read_regions": collections.Counter(),
           "gfx_pipes": 0, "comp_pipes": 0, "draw_skips": 0, "rejects": collections.Counter()}
    prev_frame_ts = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            out["lines"] += 1
            m = RE_TS.match(line)
            ts = float(m.group(1)) if m else out["last_ts"]
            if m:
                if out["first_ts"] is None:
                    out["first_ts"] = ts
                out["last_ts"] = ts
            if "DEVICE_LOST" in line:
                out["device_lost"] = True
            if out["marker_ts"] is None and marker and marker in line:
                out["marker_ts"] = ts
            if "Nx2Frame: " in line:
                kv = {k: _num(v) for k, v in RE_KV.findall(line[line.find("Nx2Frame: ") + 10:])}
                if kv.get("iv_p50") is not None and kv.get("n"):
                    # Both lines are taken at a present, so the gap to the previous one spans n presents.
                    kv["ts"] = ts
                    kv["dur"] = ts - prev_frame_ts if prev_frame_ts is not None else None
                    prev_frame_ts = ts
                    out["frames"].append(kv)
                continue
            if "presents: frame" in line:
                v = RE_VI2.search(line)
                if v:
                    n, frame_us = int(v.group(1)), int(v.group(2))
                    out["vi2"].append({"ts": ts, "start": ts - n * frame_us / 1e6, "n": n,
                                       "frame_us": frame_us, "fence_us": int(v.group(3))})
                continue
            if "PresentClientBuffer:" in line:
                p = RE_PRESENT_NUM.search(line)
                if p:
                    out["presents"].append((ts, int(p.group(1))))
                continue
            if "Nx2PerfLive: set=" in line:
                kv = dict(RE_KV.findall(line[line.find("Nx2PerfLive: ") + 13:]))
                out["live"].append({"ts": ts, "label": kv.get("set", "-"), "mask": kv.get("mask", ""),
                                    "on": kv.get("on", "-"), "off": kv.get("off", "-"),
                                    "ignored": kv.get("ignored", "-")})
                continue
            if out["perf_off"] is None and "Nx2Perf: " in line and "=on" in line:
                states = dict(RE_KV.findall(line[line.find("Nx2Perf: ") + 9:]))
                out["perf_off"] = sorted(k for k, v in states.items() if v == "off")
                continue
            if "Eglantine Version:" in line:
                v = RE_VERSION.search(line)
                if v:
                    out["version"] = v.group(1).strip()
            elif "Host CPU:" in line:
                v = RE_CPU.search(line)
                if v:
                    out["cpu"] = v.group(1).strip()
            elif "GPU thread split:" in line:
                out["gpu_split"] = line.split("MaybeStart:", 1)[-1].strip()
            elif "asymmetric L3 cache" in line or "X3D Thread Affinity:" in line:
                out["x3d"].append(line.strip()[:200])
            elif "LogSettings:" in line:
                s = RE_SETTING.search(line)
                if s and any(k in s.group(1) for k in SETTING_KEYS):
                    out["settings"][s.group(1)] = s.group(2).strip()
            if "OnCPURead:" in line:
                r = RE_CPU_READ.search(line)
                if r:
                    out["cpu_reads"] = int(r.group(3))
                    out["cpu_read_regions"][f"0x{int(r.group(1), 16) & ~0xFFFFFF:09x}"] += 1
            if "CreateGraphicsPipeline" in line:
                out["gfx_pipes"] += 1
            elif "CreateComputePipeline" in line:
                out["comp_pipes"] += 1
            if "Draw skipped (no pipeline)" in line:
                out["draw_skips"] += 1
            if "shader rejected:" in line:
                r = RE_SHADER_REJECT.search(line)
                if r:
                    reason = re.sub(r"Native shader at 0x[0-9a-fA-F]+, instruction 0x[0-9a-fA-F]+:\s*", "",
                                    r.group(1))
                    out["rejects"][re.sub(r"\[op ([0-9a-fA-F]+) [^\]]+\]", r"[op \1]", reason).strip()] += 1
    return out


def read_phases(run_dir: Path | None) -> dict[str, float]:
    """phases.txt from run_oot.ps1: "<name> <seconds since launch>" per line."""
    phases: dict[str, float] = {}
    path = run_dir / "phases.txt" if run_dir else None
    if path and path.is_file():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) == 2 and _num(parts[1]) is not None:
                phases.setdefault(parts[0], float(parts[1]))
    return phases


def phase_of(start: float, end: float, gameplay: float | None, walk: float | None, known: bool) -> str:
    """known: the run's phases.txt says whether and when it walked; without it gameplay is one phase."""
    if gameplay is None or start < gameplay:
        return "title/menu" if gameplay is None or end <= gameplay else "loading"
    if not known:
        return "gameplay"
    if walk is None or end <= walk:
        return "spawn"
    return "walk" if start >= walk else "spawn+walk"


def summarize(log: dict, gameplay: float | None, walk: float | None, known: bool = True) -> dict:
    vi2 = [dict(v, phase=phase_of(v["start"], v["ts"], gameplay, walk, known)) for v in log["vi2"]]
    frames = [dict(f, phase=phase_of(f["ts"] - f["dur"], f["ts"], gameplay, walk, known))
              for f in log["frames"] if f["dur"]]
    phases = {}
    for name in ("title/menu", "gameplay", "spawn", "walk"):
        ws = [v for v in vi2 if v["phase"] == name]
        fs = [f for f in frames if f["phase"] == name]
        s: dict = {"windows": len(ws), "seconds": sum(f["dur"] for f in fs)}
        if ws:
            fps = [1e6 / v["frame_us"] for v in ws]
            s.update(median_fps=statistics.median(fps), min_fps=min(fps),
                     fence_ms=statistics.median(v["fence_us"] / 1000 for v in ws))
        presents = sum(f["n"] for f in fs)
        if presents and s["seconds"] > 0:
            s.update(mean_fps=presents / s["seconds"],
                     fw_p50=statistics.median(f["fw_p50"] for f in fs if f.get("fw_p50") is not None),
                     over_20ms=int(sum(f.get("iv_n_over_20ms") or 0 for f in fs)))
        phases[name] = s
    live = []
    markers = log["live"]
    for i, m in enumerate(markers):
        end = markers[i + 1]["ts"] if i + 1 < len(markers) else float("inf")
        flipped = i == 0 or m["mask"] != markers[i - 1]["mask"]
        start = m["ts"] + (LIVE_SETTLE if flipped else 0.0)
        fs = [f for f in frames if f["ts"] - f["dur"] >= start - 0.01 and f["ts"] < end]
        arm = re.sub(r"\.\d+$", "", m["label"])
        if arm in ("boot", "end"):
            continue
        live.append({"label": m["label"], "arm": arm, "mask": m["mask"], "on": m["on"], "off": m["off"],
                     "ignored": m["ignored"], "presents": sum(f["n"] for f in fs),
                     "seconds": sum(f["dur"] for f in fs)})
    arms: dict[str, dict] = {}
    for w in live:
        a = arms.setdefault(w["arm"], {"windows": 0, "presents": 0, "seconds": 0.0, "on": w["on"],
                                       "off": w["off"], "ignored": w["ignored"]})
        if w["seconds"] > 0:
            a["windows"] += 1
            a["presents"] += w["presents"]
            a["seconds"] += w["seconds"]
    return {"vi2": vi2, "phases": phases, "arms": arms}


def report(log: dict, other_lost: list[str], gameplay: float | None, gameplay_from: str,
           walk: float | None, s: dict) -> None:
    duration = log["last_ts"] - (log["first_ts"] or 0.0)
    print(f"=== NX2 performance summary ({log['path']}) ===")
    print(f"Build:     {log['version']}")
    print(f"Host CPU:  {log['cpu']}")
    print(f"GPU split: {log['gpu_split']}")
    print(f"Duration:  {duration:.1f} s ({log['lines']:,} log lines)")
    if log["settings"]:
        print("Settings:  " + ", ".join(f"{k}={v}" for k, v in sorted(log["settings"].items())))
    for line in log["x3d"][-2:]:
        print(f"X3D:       {line}")
    if log["perf_off"] is not None:
        print(f"Switches off: {', '.join(log['perf_off']) or 'none'}")

    lost = (["emulator log"] if log["device_lost"] else []) + other_lost
    print("\n--- Result ---")
    print(f"DEVICE_LOST: {'YES in ' + ', '.join(lost) if lost else 'no'}")
    if gameplay is None:
        print("Gameplay:    NOT REACHED: no gameplay marker. Every number below is title screen or menu"
              " (about 60 FPS) and says nothing about the game. Pass --after if the save is elsewhere.")
    else:
        print(f"Gameplay:    from {gameplay:.1f} s ({gameplay_from})"
              + (f"; walk from {walk:.1f} s" if walk is not None else ""))

    print("\n--- 600-present windows (nx2:vi2) ---")
    if s["vi2"]:
        for v in s["vi2"]:
            print(f"  {v['start']:7.1f}..{v['ts']:7.1f} s  {1e6 / v['frame_us']:5.1f} FPS "
                  f"({v['frame_us'] / 1000:5.2f} ms), fence {v['fence_us'] / 1000:5.2f} ms  {v['phase']}")
    else:
        print("  none logged")
    print("\n--- By phase (windows: median/min FPS; Nx2Frame: presents / seconds) ---")
    for name, p in s["phases"].items():
        if not p["windows"] and not p["seconds"]:
            continue
        parts = []
        if p["windows"]:
            parts.append(f"{p['windows']} windows, median {p['median_fps']:.1f} FPS, min {p['min_fps']:.1f}, "
                         f"fence {p['fence_ms']:.2f} ms")
        if p.get("mean_fps"):
            parts.append(f"Nx2Frame {p['seconds']:.0f} s, {p['mean_fps']:.1f} FPS, fw_p50 {p['fw_p50']:.2f} ms, "
                         f"{p['over_20ms']} intervals over 20 ms")
        print(f"  {name:<10} " + "; ".join(parts))
    if s["arms"]:
        print(f"\n--- Live switch sets (Nx2PerfLive, {LIVE_SETTLE:g} s dropped after each flip) ---")
        for arm, a in s["arms"].items():
            fps = f"{a['presents'] / a['seconds']:.1f} FPS" if a["seconds"] else "no Nx2Frame data"
            ignored = f"  IGNORED (not changed): {a['ignored']}" if a["ignored"] not in ("-", "") else ""
            print(f"  {arm:<10} {a['windows']} windows, {a['seconds']:.0f} s, {fps}  on={a['on']} off={a['off']}"
                  + ignored)

    print("\n--- CPU<->GPU drains and pipelines ---")
    total_presents = log["presents"][-1][1] if log["presents"] else 0
    per_sec = log["cpu_reads"] / duration if duration > 0 else 0.0
    per_frame = log["cpu_reads"] / total_presents if total_presents else 0.0
    print(f"OnCPURead sync drains: {log['cpu_reads']:,} total ({per_sec:.1f}/s, {per_frame:.2f}/frame)")
    if log["cpu_read_regions"]:
        print("  Hot guest regions:   " + ", ".join(
            f"{r}+16M ({c * 64:,} reads)" for r, c in log["cpu_read_regions"].most_common(4)))
    print(f"Pipeline builds:       gfx={log['gfx_pipes']:,}, compute={log['comp_pipes']:,}")
    print(f"Draws skipped:         {log['draw_skips']:,} (no pipeline)")
    print(f"Rejected shaders:      {sum(log['rejects'].values()):,}")
    for reason, count in log["rejects"].most_common(6):
        print(f"  {count:4d}x  {reason}")


def run(target: Path, marker: str, after: float | None) -> int:
    run_dir = target if target.is_dir() else None
    log_path = run_dir / "emulator.log" if run_dir else target
    if run_dir and not log_path.is_file():
        log_path = run_dir / "eden_log.txt"
    if not log_path.is_file():
        print(f"[perf_summary] log not found: {log_path}", file=sys.stderr)
        return 1
    log = parse_log(log_path, marker)
    other_lost = []
    for other in ([run_dir / "eden_log.txt"] if run_dir else []):
        if other.is_file() and other != log_path:
            with open(other, encoding="utf-8", errors="replace") as f:
                if any("DEVICE_LOST" in line for line in f):
                    other_lost.append(other.name)
    phases = read_phases(run_dir)
    if after is not None:
        gameplay, gameplay_from = after, "--after"
    elif log["marker_ts"] is not None:
        gameplay, gameplay_from = log["marker_ts"], f"first '{marker}' line"
    else:
        gameplay, gameplay_from = None, ""
    walk = phases.get("walk") if gameplay is not None else None
    known = "gameplay" in phases  # written by run_oot.ps1, which also writes "walk" if it walked
    report(log, other_lost, gameplay, gameplay_from, walk, summarize(log, gameplay, walk, known))
    return 0


def self_test() -> int:
    failures = []

    def check(name, cond):
        print(f"  {'ok  ' if cond else 'FAIL'} {name}")
        if not cond:
            failures.append(name)

    def vi2(end, fps):
        return (end, f"[{end:11.6f}] Service <Info> x:1:PresentClientBuffer: nx2:vi2 last 600 presents: frame "
                     f"{int(1e6 / fps)} us (max 1, 0 over 100 ms), blocked on the GPU fence 5000 us (max 1), "
                     "composite 4 us")

    def frame(t, n, over=0):
        return (t, f"[{t:11.6f}] HW.GPU <Info> x:1:PresentClientBuffer: Nx2Frame: n={n} iv_p50=16.67 iv_p90=20.00 "
                   f"iv_p99=30.00 iv_max=30.00 fw_p50=4.00 fw_p99=9.00 fw_max=12.00 iv_n_over_20ms={over}")

    def live(t, label, off, mask):
        return (t, f"[{t:11.6f}] HW.GPU <Info> x:1:Poll: Nx2PerfLive: set={label} on=- off={off} ignored=- "
                   f"mask={mask}")

    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        rows = [(0.01, "[   0.010000] HW.GPU <Info> x:1:F: Nx2Perf: bounds=on frames_ahead=off front_run=on"),
                (103.0, "[ 103.000000] HW.GPU <Info> x:1:ImageInfo: 3D TIC 0: 64x36x96 block_depth 4 levels 1")]
        rows += [vi2(10.0 * i, 60.0) for i in range(1, 11)]  # title screen
        # loading (starts before the marker), spawn x3 (ends by the walk at 170.5), walk x2
        rows += [vi2(120.0, 30.0), vi2(136.0, 36.0), vi2(153.0, 36.5), vi2(170.0, 35.5),
                 vi2(185.0, 44.0), vi2(200.0, 45.0)]
        rows += [frame(t, 36, over=1) for t in range(120, 171)]  # first line has no duration
        rows += [frame(t, 44) for t in range(172, 181)]
        rows += [live(181.0, "A.1", "-", "0x1")] + [frame(t, 40) for t in range(182, 192)]
        rows += [live(191.5, "B.2", "front_run", "0x3")] + [frame(t, 30) for t in range(192, 202)]
        lines = [text for _, text in sorted(rows, key=lambda r: r[0])]
        (d / "emulator.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (d / "eden_log.txt").write_text("[ 1.0] Render.Vulkan <Critical> VK_ERROR_DEVICE_LOST\n", encoding="utf-8")
        (d / "phases.txt").write_text("gameplay 103.0\nwalk 170.5\n", encoding="utf-8")

        log = parse_log(d / "emulator.log", DEFAULT_MARKER)
        check("marker found at 103 s", log["marker_ts"] == 103.0)
        check("switches off parsed", log["perf_off"] == ["frames_ahead"])
        check("emulator.log itself has no DEVICE_LOST", not log["device_lost"])
        s = summarize(log, log["marker_ts"], read_phases(d)["walk"])
        p = s["phases"]
        check("10 title windows at 60 FPS", p["title/menu"]["windows"] == 10
              and abs(p["title/menu"]["median_fps"] - 60.0) < 0.1)
        check("loading window kept out of the spawn", p["spawn"]["windows"] == 3)
        check("spawn median 36.0", abs(p["spawn"]["median_fps"] - 36.0) < 0.1)
        check("walk windows 44 and 45", p["walk"]["windows"] == 2 and abs(p["walk"]["min_fps"] - 44.0) < 0.1)
        check("Nx2Frame mean FPS in the spawn", abs(p["spawn"].get("mean_fps", 0) - 36.0) < 0.1)
        check("live arms A 40 FPS, B 30 FPS (settle dropped)",
              abs(s["arms"]["A"]["presents"] / s["arms"]["A"]["seconds"] - 40) < 0.1
              and abs(s["arms"]["B"]["presents"] / s["arms"]["B"]["seconds"] - 30) < 0.1
              and s["arms"]["B"]["windows"] == 1 and s["arms"]["A"]["seconds"] == 7.0)
        nomarker = parse_log(d / "emulator.log", "no such marker")
        s2 = summarize(nomarker, None, None)
        check("no marker: nothing counts as gameplay", s2["phases"]["spawn"]["windows"] == 0
              and s2["phases"]["walk"]["windows"] == 0 and s2["phases"]["gameplay"]["windows"] == 0)
        s3 = summarize(log, log["marker_ts"], None, known=False)
        check("no phases.txt: spawn and walk stay one 'gameplay' phase",
              s3["phases"]["gameplay"]["windows"] == 5 and s3["phases"]["spawn"]["windows"] == 0)
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = run(d, DEFAULT_MARKER, None)
        text = out.getvalue()
        check("run dir: DEVICE_LOST from eden_log.txt reported", rc == 0 and "YES in eden_log.txt" in text)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            run(d, "no such marker", None)
        check("run dir without marker says NOT REACHED", "NOT REACHED" in out.getvalue())
    print("self-test:", "PASS" if not failures else f"FAIL ({len(failures)})")
    return 0 if not failures else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", nargs="?", type=Path, default=DEFAULT_LOG, help="run dir or log file")
    parser.add_argument("--marker", default=DEFAULT_MARKER, help="text of the first gameplay log line")
    parser.add_argument("--after", type=float, help="gameplay starts at this log second (overrides --marker)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    return run(args.target, args.marker, args.after)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
