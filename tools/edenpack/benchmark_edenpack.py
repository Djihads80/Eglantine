#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare loose ExeFS/RomFS access with EdenPack indexed random reads.

This is a storage/format microbenchmark, not an emulator launch benchmark.
Run it on the same SD card and avoid moving either input between runs.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import sys
import time
from pathlib import Path

from edenpack import EdenPackError, read_index

MiB = 1024 * 1024


def timed_ms(fn):
    start = time.perf_counter()
    value = fn()
    return (time.perf_counter() - start) * 1000.0, value


def scan_loose(root: Path):
    files = []
    directories = 0
    for top in ("ExeFS", "RomFS"):
        base = root / top
        if not base.is_dir():
            raise EdenPackError(f"source is missing {top}/")
        for current, dirs, names in os.walk(base):
            directories += len(dirs) + (1 if current == str(base) else 0)
            for name in names:
                path = Path(current) / name
                if path.is_file():
                    files.append((path.relative_to(root).as_posix(), path, path.stat().st_size))
    files.sort(key=lambda item: item[0])
    return files, directories


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def random_workload(files, operations, chunk_size, seed):
    candidates = [item for item in files if item[2] > 0]
    if not candidates:
        raise EdenPackError("no non-empty files to benchmark")
    rng = random.Random(seed)
    workload = []
    for _ in range(operations):
        path, size = rng.choice(candidates)
        length = min(chunk_size, size)
        offset = rng.randrange(0, size - length + 1)
        workload.append((path, offset, length))
    return workload


def benchmark_loose(root, workload):
    latencies = []
    total = 0
    start = time.perf_counter()
    for relative, offset, length in workload:
        t0 = time.perf_counter()
        with (root / Path(*relative.split("/"))).open("rb") as stream:
            stream.seek(offset)
            data = stream.read(length)
        latencies.append((time.perf_counter() - t0) * 1000.0)
        if len(data) != length:
            raise EdenPackError(f"short read from loose file: {relative}")
        total += len(data)
    elapsed = time.perf_counter() - start
    return summarize(latencies, total, elapsed)


def benchmark_pack(archive, entries, workload):
    by_path = {entry["path"]: entry for entry in entries if entry["type"] == "file"}
    latencies = []
    total = 0
    start = time.perf_counter()
    with archive.open("rb", buffering=0) as stream:
        for relative, offset, length in workload:
            entry = by_path[relative]
            t0 = time.perf_counter()
            stream.seek(entry["offset"] + offset)
            data = stream.read(length)
            latencies.append((time.perf_counter() - t0) * 1000.0)
            if len(data) != length:
                raise EdenPackError(f"short read from EdenPack: {relative}")
            total += len(data)
    elapsed = time.perf_counter() - start
    return summarize(latencies, total, elapsed)


def summarize(latencies, total_bytes, elapsed):
    return {
        "operations": len(latencies),
        "bytes_read": total_bytes,
        "elapsed_ms": round(elapsed * 1000, 2),
        "throughput_mib_s": round(total_bytes / MiB / elapsed, 2) if elapsed else 0,
        "latency_ms_median": round(statistics.median(latencies), 3),
        "latency_ms_p95": round(percentile(latencies, 0.95), 3),
        "latency_ms_max": round(max(latencies), 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="loose game folder with ExeFS/ and RomFS/")
    parser.add_argument("archive", type=Path, help="matching .edenpack file")
    parser.add_argument("--operations", type=int, default=500, help="random reads (default: 500)")
    parser.add_argument("--chunk-kib", type=int, default=64, help="bytes per random read in KiB (default: 64)")
    parser.add_argument("--seed", type=int, default=20261031, help="fixed workload seed")
    args = parser.parse_args()

    if args.operations < 1 or args.chunk_kib < 1:
        parser.error("--operations and --chunk-kib must be positive")
    source, archive = args.source.resolve(), args.archive.resolve()

    try:
        scan_ms, loose = timed_ms(lambda: scan_loose(source))
        index_ms, index_data = timed_ms(lambda: read_index(archive))
        index, archive_size = index_data
        packed_files = [(e["path"], e["size"]) for e in index["entries"] if e["type"] == "file"]
        loose_files = [(rel, size) for rel, _, size in loose[0]]
        if loose_files != packed_files:
            raise EdenPackError("loose folder and archive file lists/sizes differ; run verify --source first")

        workload = random_workload(packed_files, args.operations, args.chunk_kib * 1024, args.seed)
        # Alternate order on repeated runs manually if comparing cold-cache results.
        loose_result = benchmark_loose(source, workload)
        pack_result = benchmark_pack(archive, index["entries"], workload)

        print(json.dumps({
            "warning": "Microbenchmark only: this does not measure Android SAF, VFS mounting, or game boot.",
            "source": str(source),
            "archive": str(archive),
            "files": len(loose[0]),
            "directories": loose[1],
            "archive_bytes": archive_size,
            "workload": {"operations": args.operations, "chunk_bytes": args.chunk_kib * 1024, "seed": args.seed},
            "loose_tree_scan_ms": round(scan_ms, 2),
            "edenpack_index_load_ms": round(index_ms, 2),
            "loose_random_reads": loose_result,
            "edenpack_random_reads": pack_result,
            "random_read_speedup": round(
                loose_result["elapsed_ms"] / pack_result["elapsed_ms"], 2
            ) if pack_result["elapsed_ms"] else None,
        }, indent=2))
    except (OSError, EdenPackError) as exc:
        print(f"edenpack-bench: error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
