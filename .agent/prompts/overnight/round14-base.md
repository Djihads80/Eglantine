# Round 14 base measurement (2026-11-07, overnight, user asleep)

HEAD `29c50533cd` (local-combined, clean, = round 13 integration notes + switch comments on top of `09c66d24d8`).
Built yuzu-cmd yuzu tests; `tests.exe "[nx2_perf]"`: all pass (63 cases, 11.7M assertions). Save: the user's Kokiri
save (same as rounds 7-13). eglantine.exe was not running; no eden-cli left over.

## Decision: speed and flash base reused from round 13
`git log --stat 29c50533cd..HEAD` is empty. 29c50533cd changed `.agent/` notes and one doc comment in
`src/video_core/nx2_perf.h` (switch result notes; no code), so the binary behaves the same as `09c66d24d8`, the one the
round 13 confirmation used. The brief named r13-confirm-a/-b, which do not exist: round 13 accepted nothing, so its
confirmation speed pair is the interleaved controls **r13-ctl-a/-b** (round13-integrate.md, "Confirmation"). Speed base:
r13-ctl-a/-b; flash base: r13-confirm-flash. Only the profiled run (r14-base-prof) is new.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker, fps_windows.py)
| run | median | min | windows |
|---|---|---|---|
| r13-ctl-a | 40.4 | 39.9 | 40.0 40.9 40.4 40.0 39.9 40.3 43.3 51.1 49.2 |
| r13-ctl-b | 40.0 | 39.7 | 39.9 40.3 40.0 39.8 39.7 39.8 43.6 49.9 49.3 |
| **pooled** | **40.3** | **39.7** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r13-ctl-a, -b, r13-confirm-flash, r14-base-prof).
- Fog score (capture-0.tga): ctl-a 0.500, ctl-b 0.504, flash 0.489, r14-base-prof 0.496 (fog present; threshold 0.35).
  r14-base-prof game.png correct: Kokiri spawn, fog, light shafts.
- Spawn spot vi2 (non-profiled, 48-124 s): frame 24.5-25.2 ms, **blocked on the GPU fence 14.6-15.2 ms (a ~14.91,
  b ~15.02, mean ~14.97)**. After the walk (~137-150 s): frame ~19.6-23.1 ms, fence 9.9-12.1 ms.

## Flash baseline (r13-confirm-flash, -Stay -GameplayWait 60 -Flash 40; lines from round13-integrate.md)
```
flash luma: n=40 median=0.4340 min=0.4204 max=0.4434 stdev=0.0060 outliers=0
flash cg: n=40 median=0.4266 min=0.4145 max=0.4416 stdev=0.0065 outliers=0
```

## Profile (r14-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn ~26.8 FPS = ~37.3 ms/frame with profiler)
Gameplay marker at 21.8 s; `result: no device loss`. Spawn reports at 101.2/111.3 s (front) and 103.2/113.2 s (back);
ms/frame = ms/s / 26.8. Front zones sum to ~1000 ms/s and the waits by site nest inside them (mostly push, split
drain); back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 26-29%):
- push 388.4-388.9 ms/s = **14.5 ms/frame**; idle 222-229 = 8.3-8.5; split drain 198-203 = 7.4-7.6; macro 144.7-146.9 =
  5.4-5.5 (~1.04M calls/s; 0x01 230k, 0x0e 152k, 0x1d 144k, 0x1b 123k, 0x50 54k, 0x6b 38k, 0x04 37k, 0x4b/0x4d 35k,
  0x16 29.7k at 79 ring words/call, 0x15 29.7k, 0x14 18k); list copy wait 23.4-23.7 = 0.88; pending sleep 13.0-13.6 =
  0.49-0.51; resume pending 1.5-1.6 = 0.06.
- Waits for the back by site: **list writers 257-260 ms/s = 9.6-9.7 ms/frame (~5385/s)**, acquire trigger 167-171 =
  6.2-6.4 (~3250/s), acquire poll 25.5-26.7 = 0.95-1.0 (~1090/s), map 4.0-4.5 = 0.15-0.17 (27/s), fence release 1.4
  = 0.05 (196/s). Total ~456-461 ms/s = **17.0-17.2 ms/frame** (r13 base: 454 = 16.6).

Back (cpu 52-65%): split wait (idle) 272-280 = 10.2-10.4; back replay 192-193 = 7.2 (3.75M entries/s); method
158-160 = 5.9-6.0 (3.23M/s); other 89-90 = 3.3-3.4; draw 37.6-38.0 = 1.40-1.42 (~69.6k draws/s, ~2600/frame); read
handles 28.1-28.7 = 1.05-1.07; bind uniform stream 24.5-24.8 = 0.91-0.93; bind uniform bank 22.2-22.8 = 0.83-0.85
(r13 base 40.2: lower here, cause not checked; speed unchanged); bind stage buffers 21.2-21.4 = 0.79-0.80; update
buffers 20.5-20.8 = 0.77; configure 17.0-17.2 = 0.64; compute 13.9-14.6 = 0.52-0.54; bind storage 12.6-12.7 = 0.47;
flush work 11.9-12.0 = 0.45; pipeline lookup 11.7 = 0.44; fill image views 10.3-10.4 = 0.39; fence 10.1-10.3 = 0.38;
bind uniform cached 8.2-8.4 = 0.31; push descriptors 6.6-6.7 = 0.25; render targets 5.0 = 0.19.

Counters (per second, 101-113 s): 3D register writes 7.55M front (4.19-4.20M same value), 2.72M back (0.79M same);
front direct 2.19M, from macros 3.17-3.18M; draw.begin coalesced 276-277k; same-value executable writes sent 1.05M
(top 0x586 391-393k, 0x8e5 116k, 0x585 78k, 0x5f8 75k, 0x5f7 75k, 0x6c3 61k, 0x54c 60k, 0x904 54k); acquires 3276-3278
(195-196 waited), acquire polls 13.8k (389-393 met; r13 base 16.2-16.4k); list copy 73-74/s, 359-360 us submit->done;
MME DMA reads 0; back buffer downloads 31.0-31.8 MiB in 1920-1925 copies (<60f 28.0-28.9 MiB); fencing thread
29.3-30.0 MiB written back; bind memo 30.9-31.3k hits / 171.6k misses; bank rechecks skipped 55.3-55.4k; submits by
origin: idle hook 1003-1014, waited 538-540, list copy 380-381, gpu idle 148-149, other 54. pending_wake 47-59
sleeps/s, all ended by a push. dma_forward: every macro call forwarded, 0 not. spin_handoff (off): front_sleeps
~1090-1150/s, back_sleeps ~4550-4740/s. report_submit_early (off): pending 0/s at ~29.2-30.7k back calls/s.

fence_timeline p50/p99 (ms), spawn spot (60.4-128.0 s): entered->front_parse **26.86-27.67** / 30.4-31.1;
front_parse->back_incr 0.008 / 0.010-0.016; back_incr->waited 0.005 / 0.013-0.018; last_submit_pop->entered
0.47-0.52 / 1.61-1.75; back_idle_in_wait **1.393-1.424** / 1.72-1.75. After the walk (144.7-161.8 s):
entered->front_parse 17.5-18.3, back_idle_in_wait 1.08-1.10.
vi2 (profiled, 60.4-128.0 s): frame 37.3-37.9 ms, blocked on the GPU fence 26.6-27.6 ms. After the walk: frame
28.0-28.5, fence 17.8-18.1. Nx2Frame (100-114 s): iv_p50 ~36.4-37.3, fw_p50 ~26.3-27.1.

## Top costs (profiled ms/frame at the spawn)
front push 14.5 | back split wait (idle) 10.3 | front wait list writers 9.7 (nested) | front idle 8.4 | front split
drain 7.5 | back replay 7.2 | front wait acquire trigger 6.3 (nested) | back method 6.0 | front macro 5.5 | back other 3.4

## Reading
Same structure as rounds 7-13 and within noise of the round 13 base (profiled frame 37.3 vs 36.5 ms; front waits 17.1
vs 16.6 ms/frame; back replay + method 13.2 vs 12.2). Non-profiled speed 40.3 / 39.7 (r13 base 41.0 / 39.5). The guest
waits ~15.0 ms (non-profiled) for a fence the back increments ~8 us after the front parses it; the back idles only
~1.4 ms of that wait. The front is the critical path: its waits on the back (list writers ~9.7, acquire trigger ~6.3
ms/frame profiled) nest in push and split drain. Round 13 showed small front/back shortcuts are exhausted (back replay
lean, front macro call, bank dirty skip all neutral or worse) and that every run-ahead hole is a GPU-own write; the
next lever is the real run-ahead switch (frames_ahead + reply-time overlay + own-write forwarding), which must pass the
flash check with 0 outliers.
