# Round 10 base measurement (2026-11-07, overnight, user asleep)

HEAD `ac0157ffcc` (local-combined, clean, = round 9 integration notes on top of `6a8a624359`, pending_wake default on).
Built yuzu-cmd yuzu tests (ninja: no work to do); `tests.exe "[nx2_perf]"`: all pass (35 cases, 27384 assertions).
Save: the user's Kokiri save (same as rounds 7-9). eglantine.exe was not running; lab lock free.

## Decision: speed and flash base reused from round 9
`git log --stat ac0157ffcc..HEAD` is empty and ac0157ffcc only added agent notes after `6a8a624359`, the binary
r9-confirm-a/-b and r9-confirm-flash ran on. So those are the base; no speed or flash run was repeated.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker, fps_windows.py)
| run | median | min | windows |
|---|---|---|---|
| r9-confirm-a | 39.2 | 37.6 | 39.3 39.6 39.2 39.2 39.2 37.6 46.6 49.5 |
| r9-confirm-b | 39.4 | 38.4 | 39.4 39.4 39.4 39.3 39.2 38.4 43.3 49.4 |
| **pooled** | **39.3** | **37.6** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r9-confirm-a, -b, r9-confirm-flash, r10-base-prof).
- Fog score (capture-0.tga): confirm-a 0.488, confirm-b 0.493, flash 0.495, r10-base-prof 0.495 (fog present; threshold 0.35).
- Spawn spot vi2 (non-profiled, 49-126 s): frame 25.3-26.6 ms, **blocked on the GPU fence 15.2-15.7 ms (mean ~15.5)**,
  composite 7-9 us, vblank pacing 0-1 us, 0 frames ahead. After the walk (139-151 s): frame 20.2-23.1 ms, fence 10.4-12.7 ms.

## Flash baseline (r9-confirm-flash, -Stay -GameplayWait 60 -Flash 40; lines from round9-integrate.md)
```
flash luma: n=40 median=0.4329 min=0.4258 max=0.4389 stdev=0.0036 outliers=0
flash cg: n=40 median=0.4303 min=0.4143 max=0.4425 stdev=0.0066 outliers=0
```

## Profile (r10-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn ~26.1 FPS = ~38.3 ms/frame with profiler)
Gameplay marker at 21.8 s; `result: no device loss`. Spawn reports at 101/111/121 s; ms/frame = ms/s / 26.1. Front zones
sum to ~1000 ms/s and the waits by site nest inside them (mostly push); back zones are flat; per-call zones are inflated.

Front (cpu 24-26%):
- push 395-402 ms/s = **15.2 ms/frame**; idle 218-219 = 8.4; split drain 215-220 = 8.4; macro 126-128 = 4.9 (~1.01M calls/s;
  0x01 225k, 0x0e 150k, 0x1d 140k, 0x1b 120k, 0x50 53k, 0x6b 37k, 0x04 36k, 0x16 28.8k at 79 ring words/call);
  list copy wait 23 = 0.88; pending sleep 13.1-13.4 = 0.51 (r9 base: 40 = 1.5, pending_wake's effect).
- Waits for the back by site (111 s): **list writers 272.9 ms/s = 10.5 ms/frame (~5190/s)**, acquire trigger 179.5 = 6.9
  (~3130/s), acquire poll 34.6 = 1.33 (~1060/s), map 4.4 = 0.17, fence release 1.3 = 0.05. Total ~493 ms/s = 18.9 ms/frame
  (r9: 452 = 17.4). The front now waits more on the back (list writers +20-27, acquire trigger +11-13, split drain +20 ms/s):
  pending_wake moved the front's freed time into waits on the back.

Back (cpu 55-67%): split wait (idle) 266 = 10.2 (r9: 299-307); back replay 173 = 6.6 (3.64M entries/s); method 155 = 5.9
(3.13M/s); other 86 = 3.3; draw 63.6 = 2.4 (68k draws/s, ~2600/frame); bind uniform bank 37 = 1.42; read handles 27 = 1.03;
bind uniform stream 24.2 = 0.93; bind stage buffers 21 = 0.80; update buffers 19.9 = 0.76; configure 16.8 = 0.64; compute
13.6 = 0.52; bind storage 12.2 = 0.47; bind uniform cached 12.1 = 0.46; flush work 11.8 = 0.45; pipeline lookup 11.4 = 0.44;
fence 10.1 = 0.39; fill image views 10.0 = 0.38; push descriptors 6.6 = 0.25.

Counters (per second, 111 s): 3D register writes 7.36M front (4.09M same value), 2.66M back (0.76M same); front direct 2.14M,
from macros 3.09M; draw.begin coalesced 269k; same-value executable writes sent 1.02M (top 0x586 380k, 0x8e5 113k, 0x585 76k);
acquires 3160 (192 waited), acquire polls 15.4k (385 met); list copy 71/s, 361 us submit->done; MME DMA reads 0; back buffer
downloads 31.7 MiB in 1856 copies (<60f 29.3 MiB); fencing thread 30.0 MiB written back; bind memo 29.6k hits / 169k misses;
submits by origin: idle hook 993, waited 559, list copy 367, gpu idle 129, other 52. pending_wake 54-58 sleeps/s, all ended
by a push (latch 0, timeout 0, 0 resumed unmet). spin_handoff (off): front_sleeps ~1150-1200/s, back_sleeps ~4370-4590/s.

fence_timeline p50/p99 (ms), spawn spot (60-129 s): entered->front_parse **27.9-28.4** / 30.9-31.4; front_parse->back_incr
0.007 / 0.015-0.016; back_incr->waited 0.005 / 0.012-0.014; last_submit_pop->entered 0.42-0.45 / 1.63-1.69;
back_idle_in_wait **1.39-1.42** / 1.64-1.71 (r9: 2.07-2.10). After the walk (145-160 s): entered->front_parse 15.5-16.2.
vi2 (profiled, 61-106 s): frame 38.2-38.6 ms, blocked on the GPU fence 28.1-28.3 ms. Nx2Frame: iv_p50 ~37.4-38.6, fw_p50 ~27.6-28.4.

## Reading
Same structure as rounds 7-9, ~1 ms/frame shorter. The guest waits ~15.5 ms (non-profiled) for a fence the back increments
~7 us after the front parses it; the back idles only ~1.4 ms of that wait now. The front is the critical path: its waits on the back total
~18.9 ms/frame (profiled, nested in push and split drain), list writers 10.5 and acquire trigger 6.9 dominating.
After pending_wake removed the front's fixed 1 ms sleep, the time it saved went into waiting on the back, so the next gain must
shorten the back work that list-writer and acquire-trigger waits block on, or let those waits resolve earlier without a drain.
