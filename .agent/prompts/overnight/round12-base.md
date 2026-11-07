# Round 12 base measurement (2026-11-07, overnight, user asleep)

HEAD `4caf96e5df` (local-combined, clean, = round 11 integration notes on top of `c805603330`, diag_draw_lite on by
default). Built yuzu-cmd yuzu tests (ninja: no work to do); `tests.exe "[nx2_perf]"`: all pass (51 cases, 253481
assertions). Save: the user's Kokiri save (same as rounds 7-11). eglantine.exe was not running; no eden-cli left over.

## Decision: speed and flash base reused from round 11
`git log --stat 4caf96e5df..HEAD` is empty, and 4caf96e5df only touched `.agent/` notes after `c805603330`, the binary
the round 11 confirmation runs used. Speed base: r11-confirm-a/-b; flash base: r11-confirm-flash. No speed or flash run
was repeated. Only the profiled run (r12-base-prof) is new.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker, fps_windows.py)
| run | median | min | windows |
|---|---|---|---|
| r11-confirm-a | 40.2 | 39.7 | 39.7 40.0 40.1 40.2 40.0 40.3 43.3 51.0 51.8 |
| r11-confirm-b | 41.0 | 40.5 | 40.5 41.0 41.0 40.8 40.6 40.7 44.3 50.5 52.8 |
| **pooled** | **40.8** | **39.7** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r11-confirm-a, -b, r11-confirm-flash, r12-base-prof).
- Fog score (capture-0.tga): confirm-a 0.496, confirm-b 0.500, flash 0.497, r12-base-prof 0.498 (fog present; threshold 0.35).
  r12-base-prof game.png correct: Kokiri spawn, fog, light shafts.
- Spawn spot vi2 (non-profiled, 48-124 s): frame 24.4-25.3 ms, **blocked on the GPU fence 14.6-15.1 ms (a ~15.07,
  b ~14.64, mean ~14.9)**. After the walk (147-161 s): frame 19.0-19.8 ms, fence 9.2-10.1 ms.
- Reminder from round 11: the +1.4 over round 10 is lab diag overhead removed (diag_draw_lite), not a user-visible gain.

## Flash baseline (r11-confirm-flash, -Stay -GameplayWait 60 -Flash 40; lines from round11-integrate.md)
```
flash luma: n=40 median=0.4317 min=0.4207 max=0.4402 stdev=0.0050 outliers=0
flash cg: n=40 median=0.4293 min=0.4152 max=0.4474 stdev=0.0065 outliers=0
```

## Profile (r12-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn ~27.1 FPS = ~36.9 ms/frame with profiler)
Gameplay marker at 21.5 s; `result: no device loss`. Spawn reports at 101.2/111.2 s (front) and 102.9/112.9 s (back);
ms/frame = ms/s / 27.1. Front zones sum to ~1000 ms/s and the waits by site nest inside them (mostly push, split drain);
back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 23-27%):
- push 399.0-399.3 ms/s = **14.7 ms/frame**; idle 221-224 = 8.2; split drain 196-200 = 7.3; macro 139-141 = 5.2 (~1.05M
  calls/s; 0x01 233k, 0x0e 153k, 0x1d 145k, 0x1b 124k, 0x50 55k, 0x6b 38k, 0x04 37k, 0x4b/0x4d 35k, 0x16 30.0k at 79 ring
  words/call, 0x15 30k, 0x14 18k); list copy wait 23.7 = 0.87; pending sleep 14.2 = 0.52.
- Waits for the back by site (101/111 s): **list writers 266-268 ms/s = 9.9 ms/frame (~5400/s)**, acquire trigger 163.2 =
  6.0 (~3270/s), acquire poll 25.6-26.6 = 0.96 (~1115/s), map 4.4-9.3 = 0.25, fence release 1.4 = 0.05. Total ~465 ms/s
  = 17.2 ms/frame (r11 base: 473 = 17.6).

Back (cpu 48-49%, was 56-62% in round 11 before diag_draw_lite): split wait (idle) 273-277 = 10.1; back replay 177-179 =
6.6 (3.78M entries/s); method 156 = 5.8 (3.25M/s); other 87.4 = 3.2; bind uniform bank 41.7 = 1.54; **draw 37.3 = 1.38
(was 62.9 = 2.3: the diag lock is gone)** (70.4k draws/s, ~2600/frame); read handles 28.8 = 1.06; bind uniform stream
24.3 = 0.90; bind stage buffers 21.2 = 0.78; update buffers 20.7 = 0.76; configure 17.0 = 0.63; compute 13.4 = 0.49;
bind uniform cached 12.9 = 0.47; bind storage 12.6 = 0.46; flush work 12.0 = 0.44; pipeline lookup 11.8 = 0.43; fill
image views 10.1 = 0.37; fence 9.9 = 0.36; push descriptors 6.6 = 0.24.

Counters (per second, 101-112.9 s): 3D register writes 7.62M front (4.23M same value), 2.75M back (0.79M same); front
direct 2.21M, from macros 3.20M; draw.begin coalesced 277k; same-value executable writes sent 1.06M (top 0x586 393k,
0x8e5 118k, 0x585 79k, 0x5f8 77k, 0x5f7 76k, 0x6c3 61k, 0x54c 61k); acquires 3290-3300 (201-202 waited), acquire polls
15.9k (402-405 met); list copy 87-88/s, 311-317 us submit->done; MME DMA reads 0; back buffer downloads 32.8-33.1 MiB in
1920-1945 copies (<60f 29.3 MiB); fencing thread 31.1-31.3 MiB written back; bind memo 29.9k hits / 175k misses; bank
rechecks skipped 56.7k; submits by origin: idle hook 1004-1028, waited 539-546, list copy 379-383, gpu idle 149, other
54-55. pending_wake 52-62 sleeps/s, all ended by a push. dma_forward: every macro call forwarded, 0 not.
spin_handoff (off): front_sleeps ~1095-1187/s, back_sleeps ~4516-4800/s. report_submit_early (off): pending 0/s at
~30k back calls/s. Runtime pipelines: 2 built in 10 s (9.3 ms on the GPU thread).

fence_timeline p50/p99 (ms), spawn spot (59.7-126 s): entered->front_parse **26.58-26.80** / 29.3-30.3; front_parse->back_incr
0.007 / 0.010-0.012; back_incr->waited 0.005 / 0.011-0.013; last_submit_pop->entered 0.46-0.53 / 1.68-1.74;
back_idle_in_wait **1.456-1.468** / 1.75-1.81. After the walk (141.7-157.7 s): entered->front_parse 15.1-16.6,
back_idle_in_wait 1.14-1.15.
vi2 (profiled, 59.7-126 s): frame 36.8-37.1 ms, blocked on the GPU fence 26.5-26.7 ms. After the walk: frame 26.2-26.6,
fence 16.0-16.6. Nx2Frame (99-114 s): iv_p50 ~35.8-37.8, fw_p50 ~26.2-27.4.

## Reading
Same structure as rounds 7-11. The only change is the back's draw zone (2.3 -> 1.38 ms/frame) and back CPU (56-62% ->
48-49%) from diag_draw_lite; the front did not move (push 14.6 -> 14.7, list writers 9.8 -> 9.9, acquire trigger 6.4 ->
6.0 ms/frame), and the profiled frame is 36.9 vs 37.1 ms. The guest waits ~14.9 ms (non-profiled) for a fence the back
increments ~7 us after the front parses it; the back idles only ~1.46 ms of that wait. The front is the critical path:
its waits on the back total ~17.2 ms/frame profiled (list writers 9.9, acquire trigger 6.0), nested in push and split
drain; its own CPU work is macro ~5.2 and the rest of push. Levers stay as in round 11: make the back reach the
list-writer / acquire producers sooner, cut back replay + method (~12.4 ms/frame) directly, or remove the front's need
to wait (run-ahead via a shrunk per-frame snapshot; residual c came only from the front's macro DMA reads).
