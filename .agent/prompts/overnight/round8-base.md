# Round 8 base measurement (2026-11-07, overnight, user asleep)

HEAD `3ea9227276` (local-combined, clean, = round 7 integration commit). Built yuzu-cmd yuzu tests (relinked only);
`tests.exe "[nx2_perf]"`: all pass (24 cases, 24449 assertions). Save: the user's Kokiri save (same as round 7).

## Decision: speed and flash base reused from round 7's confirmation runs
No commit since the round 7 integration commit `3ea9227276` (`git log --stat 3ea9227276..HEAD` is empty), so the binary's
source equals what r7-confirm-a/-b and r7-confirm-flash measured. I did not rerun them.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker)
| run | median | min | windows |
|---|---|---|---|
| r7-confirm-a | 37.9 | 37.6 | 37.6 37.8 37.7 37.9 37.9 37.8 48.9 49.8 49.1 |
| r7-confirm-b | 37.5 | 37.1 | 37.1 37.5 37.2 37.1 37.4 37.6 45.2 44.9 |
| **pooled** | **37.8** | **37.1** | |

- No DEVICE_LOST (emulator.log and eden_log.txt, both runs, and the flash run).
- Fog score (capture-0.tga): a 0.497, b 0.505, flash 0.488, r8-base-prof 0.498 (fog present; threshold 0.35).
- Spawn spot vi2 (non-profiled, 50-130 s): frame 26.4-27.0 ms, **blocked on the GPU fence 16.3-16.9 ms (mean ~16.5)**,
  composite 6-8 us, vblank pacing ~1 us, 0 frames ahead. After the walk: frame 20-22 ms, fence 10.4-12.3 ms.

## Flash baseline (r7-confirm-flash, -Stay -GameplayWait 60 -Flash 40)
```
flash luma: n=40 median=0.4320 min=0.4213 max=0.4385 stdev=0.0047 outliers=0
flash cg: n=40 median=0.4290 min=0.4126 max=0.4413 stdev=0.0080 outliers=0
```

## Profile (r8-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; 26.1-26.4 FPS = ~38.3 ms/frame with profiler)
Spawn spot reports at 101/111/121 s; ms/frame = ms/s / 26.1. Front zones sum to ~1000 ms/s and the waits by site are
nested in them (mostly push); back zones are flat; per-call zones are inflated by the profiler. No DEVICE_LOST.

Front (cpu 29-34%):
- push 375-382 ms/s = **14.4 ms/frame**; idle 220-229 = 8.6; split drain 204-212 = 8.0; macro 124-128 = 4.8 (~1.0M calls/s;
  0x01 222k, 0x0e 147k, 0x1d 138k, 0x1b 119k, 0x50 52k); pending sleep 40 = 1.5; list copy wait 22.5 = 0.86.
- Waits for the back by site: **list writers 259-267 ms/s = 10.0 ms/frame (~5190/s)**, acquire trigger 177-179 = 6.8
  (~3130/s), acquire poll 23-27 = 0.96 (~1030/s), map 2.6-4.7 = 0.15, fence release 1.3 = 0.05. Total ~470 ms/s = 18.0 ms/frame.

Back (cpu 55-71%): split wait (idle) 297-304 = 11.5; back replay 178 = 6.8 (3.62M entries/s); method 149-150 = 5.7
(3.12M/s); other 77 = 3.0; draw 61 = 2.35 (67k draws/s, ~2580/frame); read handles 28 = 1.07; bind uniform stream 23 = 0.89;
bind uniform bank 19.5-24 = 0.83; bind stage buffers 20.5 = 0.79; update buffers 19.5 = 0.75; configure 16.4 = 0.63;
compute 13.3 = 0.51; bind storage 11.8 = 0.45; pipeline lookup 11.2 = 0.43; flush work 11 = 0.42; fill image views 9.8 = 0.38;
fence 9.7 = 0.37; bind uniform cached 8 = 0.31; push descriptors 6.4 = 0.25.

Counters (per second): 3D register writes 7.28M front (4.04M same value), 2.63M back (0.76M same); front direct 2.11M, from
macros 3.06M; draw.begin coalesced 263k; same-value executable writes sent 1.02M (top 0x586 377k, 0x8e5 112k, 0x585 75k);
acquires ~3160 (~184 waited), acquire polls ~6200 (~366 met); list copy ~70/s, ~355 us submit->done; MME DMA reads 0;
dma_forward ~32.7k macro calls/s forwarded, 0 not; back buffer downloads 29.5 MiB in ~1830 copies (preflush age <60f 26.7 MiB);
fencing thread 27.8 MiB written back; bind memo 29.5k hits / 166k misses; submits by origin: idle hook ~950, waited ~560,
list copy ~365, gpu idle ~120, other ~52.

fence_timeline p50/p99 (ms), spawn spot (61-129 s): entered->front_parse **27.7-28.3** / 30.8-31.6; front_parse->back_incr
0.007 / 0.011-0.015; back_incr->waited 0.005 / 0.012-0.021; last_submit_pop->entered 0.81-0.83 / 1.76-1.84;
back_idle_in_wait 1.93-2.02 / 2.76-2.80. After the walk (146-162 s): entered->front_parse 16.4-17.3.
vi2 (profiled): frame 38.3-38.4 ms, blocked on the GPU fence 28.2-28.3 ms. Nx2Frame: iv_p50 ~38.5, fw_p50 ~28.2.

## Reading
Same structure as round 7. The guest waits ~16.5 ms (non-profiled) for a fence the back increments ~7 us after the front
parses it; the back idles only ~2 ms of that wait. The front is the critical path: ~18 ms/frame (profiled) of its push time
is waiting on the back, list writers (10.0) and acquire triggers (6.8) dominating. Round 7 lesson stands: fewer waits beat
narrower waits. Run-ahead lever: ~100 pages / ~50 KiB per frame to snapshot (round 7 run_ahead_ledger).
