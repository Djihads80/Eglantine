#!/usr/bin/env python3
"""Fast (<0.1s) NX2 performance, FPS & stall telemetry parser for eden_log.txt."""

import collections
import os
import re
import sys
from pathlib import Path

DEFAULT_LOG = Path(os.environ.get("APPDATA", "")) / "eden" / "log" / "eden_log.txt"

RE_TS = re.compile(r"^\[\s*([\d.]+)\]")
RE_VERSION = re.compile(r"Eglantine Version:\s*(.+)")
RE_CPU = re.compile(r"Host CPU:\s*(.+)")
RE_SETTING = re.compile(r"LogSettings:\s*(?:M-|--)\s*(\S+):\s*(.+)$")
RE_PRESENT_NUM = re.compile(r"PresentClientBuffer:.*present\s+#(\d+)")
RE_CPU_READ = re.compile(
    r"OnCPURead:.*\[(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)\s*\((\d+)\s+so far\)"
)
RE_SHADER_REJECT = re.compile(r"Native (?:graphics|compute) shader rejected:\s*(.+)")


def main() -> int:
    log_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOG
    if not log_path.is_file():
        print(f"[perf_summary] Log file not found: {log_path}", file=sys.stderr)
        return 1

    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    version = "unknown"
    cpu = "unknown"
    gpu_split = "disabled/single-thread"
    x3d_lines: list[str] = []
    settings: dict[str, str] = {}

    presents: list[tuple[float, int]] = []
    cpu_reads: list[tuple[float, int, str]] = []
    cpu_read_regions: collections.Counter[str] = collections.Counter()

    gfx_pipe_times: list[float] = []
    comp_pipe_times: list[float] = []
    draw_skips = 0
    shader_rejects: collections.Counter[str] = collections.Counter()

    first_ts = None
    last_ts = 0.0

    for line in lines:
        m_ts = RE_TS.match(line)
        ts = float(m_ts.group(1)) if m_ts else last_ts
        if m_ts:
            if first_ts is None:
                first_ts = ts
            last_ts = ts

        if "Eglantine Version:" in line:
            m = RE_VERSION.search(line)
            if m:
                version = m.group(1).strip()
        elif "Host CPU:" in line:
            m = RE_CPU.search(line)
            if m:
                cpu = m.group(1).strip()
        elif "GPU thread split:" in line:
            gpu_split = line.split("MaybeStart:", 1)[-1].strip()
        elif "X3D Thread Affinity:" in line:
            x3d_lines.append(line.strip())
        elif "LogSettings:" in line:
            m = RE_SETTING.search(line)
            if m and any(
                k in m.group(1)
                for k in (
                    "use_asynchronous_shaders",
                    "async_presentation",
                    "gpu_accuracy",
                    "use_vsync",
                    "x3d_thread_affinity",
                    "use_reactive_flushing",
                    "use_fast_gpu_time",
                )
            ):
                settings[m.group(1)] = m.group(2).strip()

        if "PresentClientBuffer:" in line:
            m = RE_PRESENT_NUM.search(line)
            if m:
                presents.append((ts, int(m.group(1))))

        if "OnCPURead:" in line:
            m = RE_CPU_READ.search(line)
            if m:
                start_hex, end_hex, count_str = m.group(1), m.group(2), m.group(3)
                cpu_reads.append((ts, int(count_str), start_hex))
                # Group by 16 MiB region prefix
                addr_val = int(start_hex, 16)
                region = f"0x{addr_val & ~0xFFFFFF:09x}"
                cpu_read_regions[region] += 1

        if "CreateGraphicsPipeline" in line:
            gfx_pipe_times.append(ts)
        elif "CreateComputePipeline" in line:
            comp_pipe_times.append(ts)

        if "Draw skipped (no pipeline)" in line:
            draw_skips += 1

        if "shader rejected:" in line:
            m = RE_SHADER_REJECT.search(line)
            if m:
                reason = re.sub(
                    r"Native shader at 0x[0-9a-fA-F]+, instruction 0x[0-9a-fA-F]+:\s*",
                    "",
                    m.group(1),
                )
                reason = re.sub(r"\[op ([0-9a-fA-F]+) [^\]]+\]", r"[op \1]", reason)
                shader_rejects[reason.strip()] += 1

    duration = (last_ts - (first_ts or 0.0)) if last_ts else 0.0
    print(f"=== NX2 Performance & Stall Summary ({log_path.name}) ===")
    print(f"Build:     {version}")
    print(f"Host CPU:  {cpu}")
    print(f"GPU Split: {gpu_split}")
    print(f"Duration:  {duration:.2f}s ({len(lines):,} log lines)")
    if settings:
        print(
            "Settings:  "
            + ", ".join(f"{k}={v}" for k, v in sorted(settings.items()))
        )
    for xl in x3d_lines[-2:]:
        print(f"X3D:       {xl}")

    print("\n--- Frame Pacing & FPS (from nx2:vi2 PresentClientBuffer) ---")
    if len(presents) >= 2:
        t0, p0 = presents[0]
        t_last, p_last = presents[-1]
        total_dt = t_last - t0
        total_dp = p_last - p0
        if total_dt > 0 and total_dp > 0:
            avg_fps = total_dp / total_dt
            avg_ms = 1000.0 / avg_fps
            print(
                f"Overall (#{p0} -> #{p_last} over {total_dt:.1f}s): {avg_fps:.1f} FPS ({avg_ms:.2f} ms/frame)"
            )
        # Show checkpoints (>= 600-frame steps)
        checkpoints = [p for p in presents if p[1] == 1 or p[1] % 600 == 0]
        if len(checkpoints) >= 2:
            print("600-Frame Windows:")
            for (ta, pa), (tb, pb) in zip(checkpoints, checkpoints[1:]):
                dt = tb - ta
                dp = pb - pa
                if dt > 0 and dp > 0:
                    fps = dp / dt
                    ms = 1000.0 / fps
                    print(
                        f"  presents #{pa:5d}..#{pb:5d} ({ta:6.1f}s..{tb:6.1f}s): {fps:5.1f} FPS ({ms:5.2f} ms/frame)"
                    )
    else:
        print("No PresentClientBuffer checkpoints logged.")

    print("\n--- Synchronous CPU<->GPU Drains & Pipeline Stalls ---")
    total_cpu_reads = cpu_reads[-1][1] if cpu_reads else 0
    total_presents = presents[-1][1] if presents else 0
    reads_per_sec = (total_cpu_reads / duration) if duration > 0 else 0.0
    reads_per_frame = (
        (total_cpu_reads / total_presents) if total_presents > 0 else 0.0
    )
    print(
        f"OnCPURead sync drains: {total_cpu_reads:,} total ({reads_per_sec:.1f}/sec, {reads_per_frame:.2f}/frame)"
    )
    if cpu_read_regions:
        top_reg = ", ".join(
            f"{r}+16M ({c * 64:,} reads)"
            for r, c in cpu_read_regions.most_common(4)
        )
        print(f"  Hot guest regions:   {top_reg}")

    print(
        f"Pipeline builds:       gfx={len(gfx_pipe_times):,}, compute={len(comp_pipe_times):,}"
    )
    print(f"Draws skipped:         {draw_skips:,} (no pipeline)")
    if shader_rejects:
        print(f"Rejected shaders:      {sum(shader_rejects.values()):,} total")
        for reason, cnt in shader_rejects.most_common(6):
            print(f"  {cnt:4d}x  {reason}")
    else:
        print("Rejected shaders:      0")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
