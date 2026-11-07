# Round 11 base measurement (2026-11-07, overnight, user asleep)

HEAD `2ac97bc933` (local-combined, clean, = round 10 integration notes on top of `b3e91cb3c6`; no switch accepted in
round 10, defaults as round 9: pending_wake on). Built yuzu-cmd yuzu tests (ninja: no work to do);
`tests.exe "[nx2_perf]"`: all pass (40 cases, 27422 assertions). Save: the user's Kokiri save (same as rounds 7-10).
eglantine.exe was not running; no eden-cli left over.

## Decision: speed and flash base reused from round 10
`git log --stat 2ac97bc933..HEAD` is empty, and 2ac97bc933 only touched `.agent/` notes after `b3e91cb3c6`, the binary the
round 10 runs used. The task named r10-confirm-a/-b, but those do not exist: round 10 used its interleaved default
controls **r10-ctl-a/-b** as the speed confirmation (same binary, defaults; see round10-integrate.md), so those are the
speed base. Flash base: r10-confirm-flash. No speed or flash run was repeated.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker, fps_windows.py)
| run | median | min | windows |
|---|---|---|---|
| r10-ctl-a | 39.4 | 38.2 | 38.9 39.4 39.4 39.4 39.3 38.2 43.6 48.0 |
| r10-ctl-b | 39.6 | 39.1 | 39.2 39.5 39.8 39.4 39.6 39.1 44.1 50.5 49.4 |
| **pooled** | **39.4** | **38.2** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r10-ctl-a, -b, r10-confirm-flash, r11-base-prof).
- Fog score (capture-0.tga): ctl-a 0.495, ctl-b 0.507, flash 0.499, r11-base-prof 0.499 (fog present; threshold 0.35).
- Spawn spot vi2 (non-profiled, 49-126 s): frame 25.1-26.2 ms, **blocked on the GPU fence 15.1-15.5 ms (mean ~15.3)**.
  After the walk (138-162 s): frame 19.8-23.0 ms, fence 10.0-12.4 ms.

## Flash baseline (r10-confirm-flash, -Stay -GameplayWait 60 -Flash 40; lines from round10-integrate.md)
```
flash luma: n=40 median=0.4330 min=0.4234 max=0.4410 stdev=0.0054 outliers=0
flash cg: n=40 median=0.4313 min=0.4122 max=0.4449 stdev=0.0077 outliers=0
```

## Profile (r11-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn ~26.9 FPS = ~37.1 ms/frame with profiler)
Gameplay marker at 22.0 s; `result: no device loss`. Spawn reports at 101/111 s (front) and 102.8/112.8 s (back);
ms/frame = ms/s / 26.9. Front zones sum to ~1000 ms/s and the waits by site nest inside them (mostly push, split drain);
back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 20-25%):
- push 392.5-393.0 ms/s = **14.6 ms/frame**; idle 216-221 = 8.1; split drain 205-211 = 7.7; macro 141 = 5.2 (~1.04M calls/s;
  0x01 234k, 0x0e 155k, 0x1d 144k, 0x1b 124k, 0x50 55k, 0x6b 38k, 0x04 37k, 0x4b/0x4d 36k, 0x16 29.8k at 79 ring words/call);
  list copy wait 23 = 0.86; pending sleep 14.3 = 0.53.
- Waits for the back by site (111 s): **list writers 262.8 ms/s = 9.8 ms/frame (~5370/s)**, acquire trigger 173.0 = 6.4
  (~3250/s), acquire poll 31.5 = 1.17 (~1090/s), map 4.6 = 0.17, fence release 1.4 = 0.05. Total ~473 ms/s = 17.6 ms/frame
  (r10 base: 493 = 18.9).

Back (cpu 56-62%): split wait (idle) 266-272 = 10.0; back replay 173-175 = 6.5 (3.77M entries/s); method 150-152 = 5.6
(3.25M/s); other 86 = 3.2; draw 62.9 = 2.3 (69k draws/s, ~2570/frame); bind uniform bank 40.8 = 1.52; read handles 27.4 = 1.02;
bind uniform stream 23.3 = 0.87; bind stage buffers 20.4 = 0.76; update buffers 19.6 = 0.73; configure 16.7 = 0.62; compute
12.9 = 0.48; flush work 12.4 = 0.46; bind uniform cached 12.3 = 0.46; bind storage 12.1 = 0.45; pipeline lookup 11.8 = 0.44;
fill image views 9.9 = 0.37; fence 9.5 = 0.35; push descriptors 6.5 = 0.24.

Counters (per second, 111-112.8 s): 3D register writes 7.61M front (4.23M same value), 2.74M back (0.79M same); front direct
2.21M, from macros 3.20M; draw.begin coalesced 277k; same-value executable writes sent 1.06M (top 0x586 392k, 0x8e5 118k,
0x585 79k, 0x5f8 77k, 0x5f7 76k); acquires 3278 (190 waited), acquire polls 15.7k (379 met); list copy 86/s, 312 us
submit->done; MME DMA reads 0; back buffer downloads 30.5 MiB in 1919 copies (<60f 27.9 MiB); fencing thread 28.7 MiB
written back; bind memo 26.6k hits / 170k misses; submits by origin: idle hook 981, waited 596, list copy 379, gpu idle 106,
other 54. pending_wake 52-65 sleeps/s, all ended by a push. dma_forward: every macro call forwarded, 0 not.
spin_handoff (off): front_sleeps ~1150-1180/s, back_sleeps ~4500-4700/s.

fence_timeline p50/p99 (ms), spawn spot (59-126 s): entered->front_parse **26.9-27.3** / 29.7-30.6; front_parse->back_incr
0.007 / 0.013-0.015; back_incr->waited 0.005 / 0.013-0.015; last_submit_pop->entered 0.47-0.53 / 1.63-1.79;
back_idle_in_wait **1.44-1.46** / 1.73-1.78. After the walk (142-158 s): entered->front_parse 15.7-16.3.
vi2 (profiled, 59-126 s): frame 37.1-37.6 ms, blocked on the GPU fence 26.9-27.1 ms. Nx2Frame: iv_p50 ~36.8-38.3,
fw_p50 ~27.0-27.9.

## Reading
Same structure as rounds 7-10 (round 10 changed no defaults, so this profile repeats round 10's within noise: profiled
frame 37.1 vs 38.3 ms, list writers 9.8 vs 10.5 ms/frame, acquire trigger 6.4 vs 6.9). The guest waits ~15.3 ms
(non-profiled) for a fence the back increments ~7 us after the front parses it; the back idles only ~1.45 ms of that wait.
The front is the critical path: its waits on the back total ~17.6 ms/frame profiled (list writers 9.8, acquire trigger 6.4),
nested in push and split drain; its own CPU work is macro ~5.2 and the rest of push. Rounds 7-10 showed that changing what
happens around those waits does not move the front; the next gain must make the back reach the list-writer / acquire
producers sooner (ring priority or reordering), cut back replay + method (~12.1 ms/frame) directly, or remove the front's
need to wait (a full per-frame snapshot of what the back reads, for run-ahead).
