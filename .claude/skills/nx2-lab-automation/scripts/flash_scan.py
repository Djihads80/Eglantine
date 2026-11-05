#!/usr/bin/env python3
"""Finds short flashes in a burst of nx2-frame-*.tga: pixels far brighter than the median of
the surrounding frames. Usage: flash_scan.py <burst dir> [out sheet]"""
import glob, os, re, sys
import numpy as np
from PIL import Image

d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "nx2-frame-*.tga")), key=lambda f: int(re.findall(r"frame-(\d+)", f)[0]))
ids = [int(re.findall(r"frame-(\d+)", f)[0]) for f in files]
L = []
for f in files:
    im = np.asarray(Image.open(f).convert("RGB").resize((320, 180)), dtype=np.float32) / 255
    L.append(im @ np.array([0.299, 0.587, 0.114], dtype=np.float32))
L = np.stack(L)
gaps = np.diff(ids)
print(f"{len(files)} frames, presents {ids[0]}..{ids[-1]}, step median {np.median(gaps):.0f} max {gaps.max()}")
W = 8
hits = []
for i in range(len(L)):
    lo, hi = max(0, i - W), min(len(L), i + W + 1)
    ref = np.median(np.concatenate([L[lo:i], L[i + 1:hi]]), axis=0)
    mask = (L[i] - ref) > 0.30
    hits.append((int(mask.sum()), i))
counts = [h for h, _ in hits]
print("flash pixels per frame (320x180): median %d, p95 %d, max %d" % (np.median(counts), np.percentile(counts, 95), max(counts)))
top = sorted(hits, reverse=True)[:8]
for n, i in top:
    ys, xs = np.nonzero((L[i] - np.median(np.concatenate([L[max(0,i-W):i], L[i+1:i+W+1]]), axis=0)) > 0.30)
    box = (xs.min() * 4, ys.min() * 4, xs.max() * 4 + 4, ys.max() * 4 + 4) if n else None
    print(f"present {ids[i]} (#{i}): {n} px, bbox {box}")
if len(sys.argv) > 2 and top[0][0] > 0:
    i = top[0][1]
    picks = [j for j in range(i - 2, i + 3) if 0 <= j < len(files)]
    sheet = Image.new("RGB", (640 * len(picks), 360))
    for k, j in enumerate(picks):
        sheet.paste(Image.open(files[j]).convert("RGB").resize((640, 360)), (640 * k, 0))
    sheet.save(sys.argv[2])
    print("sheet", sys.argv[2], "presents", [ids[j] for j in picks])
