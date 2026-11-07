# Round 7 base measurement (2026-11-07, after the user put the Kokiri save in place)

HEAD `2e4dd741f8` (local-combined, clean). Built yuzu-cmd yuzu tests (no work to do); `tests.exe "[nx2_perf]"`: all pass (23 cases).
Save: the user's new Kokiri save (game.png shows Link at the Kokiri spawn with fog and light shafts). **These numbers replace
rounds 5-6 (36-39 FPS at the old save); do not compare with them.**

## Speed (defaults, fast boot, 600-present windows from the gameplay marker)
| run | median | min | windows |
|---|---|---|---|
| r7-base-a | 35.4 | 34.3 | 34.5 34.7 35.7 35.4 34.3 36.6 43.3 |
| r7-base-b | 34.4 | 33.8 | 34.4 34.4 34.4 33.8 33.8 40.8 44.3 |
| **pooled** | **34.6** | **33.8** | |

- No DEVICE_LOST (emulator.log and eden_log.txt, both runs).
- Fog score (capture-0.tga): a 0.495, b 0.495 (fog present; threshold 0.35).
- Spawn spot vi2 (non-profiled, 54-124 s): frame 28.1-29.7 ms, **blocked on the GPU fence 17.6-18.9 ms (mean ~18.4)**,
  composite 8 us, vblank pacing ~1 us, 0 frames ahead. Last windows (walk) 40.8-44.3 FPS.

## Flash baseline (r7-base-flash, -Stay -GameplayWait 60 -Flash 40)
```
flash luma: n=40 median=0.4382 min=0.4292 max=0.4439 stdev=0.0041 outliers=0
flash cg: n=40 median=0.4322 min=0.4241 max=0.4516 stdev=0.0058 outliers=0
```
Slow drift only, no steps. No device loss. Note the stdev (0.0041) is higher than the old save's 0.0016-0.0031: use
this run as the reference for round 7+ flash gates.

## Profile (r7-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; 24.4 FPS = ~41 ms/frame with profiler overhead)
Per-second zones at the spawn spot (100-115 s), as ms/frame (/24.4). Front zones sum to 1000 ms/s at top level and the
"waits by site" are nested inside them (mostly push); back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 17% of a core):
- push 380 ms/s = 15.6 ms/frame; split drain 204 = 8.3; macro 126 = 5.2 (950k calls/s; 0x01 213k, 0x0e 142k, 0x1d 130k, 0x1b 112k);
  idle 226 = 9.3; pending sleep 37 = 1.5; list copy wait 26 = 1.0.
- Waits for the back by site: **list writers 257 ms/s = 10.5 ms/frame (4824/s)**, acquire trigger 165 = 6.8 (2927/s),
  acquire poll 31 = 1.25, map 6 = 0.25, fence release 1.3 = 0.05. Total 461 ms/s = 18.9 ms/frame.
- Change vs round 6 (other save): macro 335 -> 126 ms/s, push 200 -> 380, list writers ~100 -> 257. Waiting on list
  writers is now the largest single front wait.

Back (cpu 52%): split wait (idle) 304 = 12.5; back replay 179 = 7.3 (3.43M entries/s); method 137 = 5.6 (2.95M/s);
other 81 = 3.3; draw 59 = 2.4 (64k draws/s, ~2600/frame); bind uniform bank 38 = 1.5; read handles 26 = 1.1; bind uniform
stream 22 = 0.9; bind stage buffers 19 = 0.8; update buffers 18 = 0.75; configure 16 = 0.65; compute 11.5 = 0.47; bind
storage 11.4 = 0.47; flush work 11.3 = 0.46; pipeline lookup 10.9 = 0.45; fence 8.5 = 0.35; push descriptors 6.2 = 0.25.

Counters (per second): 3D register writes 6.91M front (3.84M same value), 2.50M back (0.72M same); front direct 2.00M,
from macros 2.90M; draw.begin coalesced 251k; same-value executable writes sent 959k (top 0x586 356k, 0x8e5 107k);
acquires 2952 (174 waited), acquire polls 5886 (349 met); list copy 84/s, 360 us submit->done; MME DMA reads 0; back buffer
downloads 29 MiB in 1735 copies (preflush age <60f 27 MiB); bind memo 27k hits / 160k misses; submits by origin: idle hook
881, waited 532, list copy 343, gpu idle 120, other 49; dma_forward 0 not forwarded.

fence_timeline p50/p99 (ms), spawn spot (65-114 s): entered->front_parse **29.6-29.9** / 33.9-34.9; front_parse->back_incr
0.007 / 0.013; back_incr->waited 0.004 / 0.02; last_submit_pop->entered 0.90 / 1.9; back_idle_in_wait 2.3-2.4 / 3.5.
vi2 (profiled): frame 41.0 ms, blocked on the GPU fence 30.0 ms.

**Reading:** same as round 6, stronger. The guest waits ~18 ms (non-profiled) for a fence the back increments 7 us after
the front parses it; the back idles only ~2.4 ms of that wait. The front is the critical path, and ~60% of its busy time
is waiting on the back (list writers, acquire triggers) inside push. Levers: (1) cut the front's waits on the back for
list writers and acquire triggers (let the front parse ahead of the back safely), (2) push parsing cost itself, (3) back
replay + method (~13 ms/frame profiled) since the front waits for the back to reach those points.

## Decisions
- The previous r7-base-a at the wrong save stays renamed INVALID-wrong-save-r7-base-a; these runs reused the names.
