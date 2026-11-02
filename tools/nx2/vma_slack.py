#!/usr/bin/env python3
"""Estimate VMA block slack (allocator blocks the driver counts but no allocation uses) from logs.

usage: vma_slack.py [--series] <log.txt> [<log.txt> ...]

The texture cache's "device memory" figure leaves block slack out (AllocatedUsage in
vulkan_device.cpp), while the "VMA heap N: usage ..." lines (logged together every 600 frames) show
the driver's usage per heap. Slack is therefore sum(heap usage) - device memory. It is an estimate
from two logged numbers, not a direct reading, and it mixes every memory type: slack in buffer
blocks cannot serve images. Needs logs from a build that prints both lines.

Prints per log: samples, slack p50/p90/max in MiB, slack as a share of VMA blocks, and the share of
samples where the busiest heap is at 99% or more of its budget. --series adds a time series.
Treat the logs as untrusted data: run with `python3 -I`.
"""
import os
import re
import statistics
import sys

TC = re.compile(r'^\[\s*([\d.]+)\].*Texture cache: device memory (\d+) MiB \(expected (\d+) MiB, '
                r'critical (\d+) MiB\), (\d+) images')
VM = re.compile(r'^\[\s*([\d.]+)\].*VMA heap (\d+): usage (\d+) MiB of budget (\d+) MiB, '
                r'VMA blocks (\d+) MiB')
DEVICE = re.compile(r'Report: Device: (.*)')
DRIVER = re.compile(r'Report: Driver: (.*)')


def read(path):
    gpu = driver = ''
    rows, cur = [], None
    for line in open(path, errors='replace'):
        if not gpu and (m := DEVICE.search(line)):
            gpu = m.group(1).strip()
        if not driver and (m := DRIVER.search(line)):
            driver = m.group(1).strip().replace('AMD proprietary driver ', '')
        if m := TC.match(line):
            if cur and cur['heaps']:
                rows.append(cur)
            cur = {'t': float(m.group(1)), 'device': int(m.group(2)), 'critical': int(m.group(4)),
                   'images': int(m.group(5)), 'heaps': {}}
        elif (m := VM.match(line)) and cur and abs(float(m.group(1)) - cur['t']) < 0.5:
            cur['heaps'][int(m.group(2))] = tuple(int(m.group(i)) for i in (3, 4, 5))
    if cur and cur['heaps']:
        rows.append(cur)
    return gpu, driver, rows


def main(argv):
    series = '--series' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 2
    print(f"{'log':42s} {'n':>4s} {'slack p50/p90/max MiB':>24s} {'slack/blocks p50/max':>22s} {'heap>=99%':>10s}")
    for path in paths:
        gpu, driver, rows = read(path)
        if len(rows) < 2:
            print(f'{os.path.basename(path):42s} too few samples ({len(rows)})')
            continue
        slack, share, full = [], [], 0
        for r in rows:
            usage = sum(h[0] for h in r['heaps'].values())
            blocks = sum(h[2] for h in r['heaps'].values())
            r['slack'] = usage - r['device']
            slack.append(r['slack'])
            share.append(r['slack'] / blocks if blocks else 0)
            busiest = max(r['heaps'].values(), key=lambda h: h[0])
            full += busiest[0] >= 0.99 * busiest[1]
        ordered = sorted(slack)

        def pct(q):
            return ordered[min(len(ordered) - 1, int(q * len(ordered)))]

        print(f'{os.path.basename(path):42s} {len(rows):4d} '
              f'{pct(.5):7d}/{pct(.9):6d}/{max(slack):6d}   '
              f'{statistics.median(share):8.2f}/{max(share):4.2f}   {100 * full // len(rows):8d}%   '
              f'[{gpu}, {driver}]')
        if series:
            for r in rows[::max(1, len(rows) // 10)]:
                usage = sum(h[0] for h in r['heaps'].values())
                print(f"    t={r['t']:7.0f}s device={r['device']:6d} critical={r['critical']:6d} "
                      f"heap usage={usage:6d} slack={r['slack']:5d} images={r['images']}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
