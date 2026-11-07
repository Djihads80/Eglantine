# Round 9 base measurement (2026-11-07, overnight, user asleep)

HEAD `0ef52d85b4` (local-combined, clean, = round 8 integration commit). Built yuzu-cmd yuzu tests (relinked only);
`tests.exe "[nx2_perf]"`: all pass (27 cases, 24470 assertions). Save: the user's Kokiri save (same as rounds 7-8).

## Decision: speed and flash base reused from round 8
`git log --stat 0ef52d85b4..HEAD` is empty, so the binary's source equals what round 8 confirmed. There are no
r8-confirm-a/-b folders: round8-integrate.md says r8-ctl-a/-b (defaults, final binary, only a comment changed after them)
serve as the confirmation pair, so I used those. Flash base: r8-confirm-flash. Nothing rerun.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker)
| run | median | min | windows |
|---|---|---|---|
| r8-ctl-a | 37.8 | 37.6 | 37.6 37.9 37.9 37.7 37.8 37.6 46.0 46.3 |
| r8-ctl-b | 37.2 | 36.8 | 36.9 37.1 37.2 36.8 37.1 37.6 45.3 44.2 |
| **pooled** | **37.7** | **36.8** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r8-ctl-a, r8-ctl-b, r8-confirm-flash, r9-base-prof).
- Fog score (capture-0.tga): ctl-a 0.502, ctl-b 0.504, flash 0.495, r9-base-prof 0.495 (fog present; threshold 0.35).
- Spawn spot vi2 (non-profiled, 50-130 s): frame 26.4-27.2 ms, **blocked on the GPU fence 16.2-17.0 ms (mean ~16.5)**,
  composite 6-8 us, vblank pacing 0-1 us, 0 frames ahead. After the walk: frame 21.6-22.7 ms, fence 11.6-12.4 ms.

## Flash baseline (r8-confirm-flash, -Stay -GameplayWait 60 -Flash 40)
```
flash luma: n=40 median=0.4347 min=0.4214 max=0.4447 stdev=0.0058 outliers=0
flash cg: n=40 median=0.4320 min=0.4185 max=0.4435 stdev=0.0057 outliers=0
```

## Profile (r9-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn 25.7-26.7 FPS = ~38.65 ms/frame with profiler)
Gameplay marker at 22.1 s. Spawn spot reports at 101/111/121 s; ms/frame = ms/s / 25.9. Front zones sum to ~1000 ms/s
and the waits by site are nested in them (mostly push); back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 22-24%):
- push 378-384 ms/s = **14.6 ms/frame**; idle 227-228 = 8.8; split drain 195-199 = 7.6; macro 130-133 = 5.1 (~1.0M calls/s;
  0x01 222k, 0x0e 146k, 0x1d 139k, 0x1b 118k, 0x50 52k, 0x16 28.5k at 79 ring words/call); pending sleep 40 = 1.5;
  list copy wait 22 = 0.85.
- Waits for the back by site (111 s): **list writers 253 ms/s = 9.8 ms/frame (~5150/s)**, acquire trigger 168.5 = 6.5
  (~3120/s), acquire poll 26 = 1.0 (~1040/s), map 2.5 = 0.10, fence release 1.3 = 0.05. Total ~452 ms/s = 17.4 ms/frame.

Back (cpu 45-71%): split wait (idle) 299-307 = 11.7; back replay 165-168 = 6.5 (3.6M entries/s); method 144 = 5.6
(3.10M/s); other 82 = 3.2; draw 60 = 2.3 (66k draws/s, ~2550/frame); **bind uniform bank 36-38 = 1.45** (r8-base-prof had
19.5-27; profiled only, the non-profiled speed is unchanged, so likely profiler/placement noise; watch it); read handles 27 =
1.04; bind uniform stream 22 = 0.85; bind stage buffers 19.4 = 0.75; update buffers 18.3 = 0.71; configure 16 = 0.62;
compute 12 = 0.47; bind uniform cached 11.7 = 0.45; bind storage 11.3 = 0.44; flush work 11.2 = 0.43; pipeline lookup
10.9 = 0.42; fill image views 9.3 = 0.36; fence 9.3 = 0.36; push descriptors 6.1 = 0.24.

Counters (per second, 111 s): 3D register writes 7.27M front (4.04M same value), 2.62M back (0.76M same); front direct 2.11M,
from macros 3.06M; draw.begin coalesced 264k; same-value executable writes sent 1.01M (top 0x586 376k, 0x8e5 112k, 0x585 75k);
acquires 3145 (191 waited), acquire polls 6210 (382 met); list copy 82/s, 306 us submit->done; MME DMA reads 0; dma_forward
all forwarded, 0 not; back buffer downloads 29.1 MiB in 1876 copies (<60f 26.6 MiB); fencing thread 27.4 MiB written back;
bind memo 26.2k hits / 162k misses; submits by origin: idle hook 975, waited 559, list copy 364, gpu idle 106, other 52.
spin_handoff (default off): front_sleeps ~1120/s, back_sleeps ~4450/s.

fence_timeline p50/p99 (ms), spawn spot (61-129 s): entered->front_parse **27.9-28.4** / 31.0-31.5; front_parse->back_incr
0.007 / 0.010-0.015; back_incr->waited 0.004 / 0.012-0.013; last_submit_pop->entered 0.81-0.86 / 1.74-1.85;
back_idle_in_wait 2.07-2.10 / 2.88-2.92. After the walk (146-162 s): entered->front_parse 16.4-16.8.
vi2 (profiled): frame 38.6-38.9 ms, blocked on the GPU fence 28.2-28.4 ms. Nx2Frame: iv_p50 ~38.5, fw_p50 ~28.2.

## Reading
Unchanged from rounds 7-8. The guest waits ~16.5 ms (non-profiled) for a fence the back increments ~7 us after the front
parses it; the back idles only ~2 ms of that wait. The front is the critical path: ~17.4 ms/frame (profiled) of its time
is waiting on the back, list writers (9.8) and acquire triggers (6.5) dominating. Round 8 lesson: removing waits that are
not on the critical path (list_fetch_after_sync, spin_handoff) gives +0.0; only waits that block the frame's fence parse count.
