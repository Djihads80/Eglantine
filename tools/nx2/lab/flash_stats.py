#!/usr/bin/env python3
"""Brightness spread over a burst of captures at one idle spot (run_oot.ps1 -Flash N writes flash.jsonl).

    python -I tools/nx2/lab/flash_stats.py tools/nx2/lab/runs/<name>/flash.jsonl

Standing still, every frame should be about as bright as the next. An outlier is a capture whose
brightness is more than max(0.02, 8% of the median) away from the median: a flash or a dark frame.
A change that touches frame timing should show 0 outliers in 30-40 captures.
"""
import json
import statistics
import sys


def load(path):
    """The capture rows (lab_ctl.py capture JSON) of a flash.jsonl."""
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("{"):
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    return rows


def summary(values):
    median = statistics.median(values)
    outliers = [v for v in values if abs(v - median) > max(0.02, 0.08 * median)]
    return {"n": len(values), "median": median, "min": min(values), "max": max(values),
            "stdev": statistics.pstdev(values), "outliers": len(outliers)}


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    rows = [row for row in load(argv[0]) if "luma" in row and "cg" in row]
    if not rows:
        print("flash: no captures")
        return 1
    for key in ("luma", "cg"):
        values = [row[key] for row in rows]
        s = summary(values)
        print(f"flash {key}: n={s['n']} median={s['median']:.4f} min={s['min']:.4f} max={s['max']:.4f} "
              f"stdev={s['stdev']:.4f} outliers={s['outliers']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
