# Round 13 base measurement (2026-11-07, overnight, user asleep)

HEAD `6286d0e3a2` (local-combined, clean, = round 12 integration notes + switch comments on top of `47e17231c2`).
Built yuzu-cmd yuzu tests (80 objects recompiled for the comment-only nx2_perf.h change); `tests.exe "[nx2_perf]"`:
all pass (57 cases, 11.7M assertions). Save: the user's Kokiri save (same as rounds 7-12). eglantine.exe was not
running; no eden-cli left over.

## Decision: speed and flash base reused from round 12
`git log --stat 6286d0e3a2..HEAD` is empty. 6286d0e3a2 changed `.agent/` notes and one doc comment in
`src/video_core/nx2_perf.h` (switch result notes; no code), so the binary behaves the same as `47e17231c2`, the one the
round 12 confirmation used. The brief named r12-confirm-a/-b, which do not exist: round 12 accepted nothing, so its
confirmation speed pair is the interleaved controls **r12-ctl-a/-b** (round12-integrate.md, "Confirmation"). Speed base:
r12-ctl-a/-b; flash base: r12-confirm-flash. Only the profiled run (r13-base-prof) is new.

## Speed (defaults, fast boot, 600-present windows from the gameplay marker, fps_windows.py)
| run | median | min | windows |
|---|---|---|---|
| r12-ctl-a | 40.0 | 39.5 | 39.5 40.2 39.8 40.0 40.0 40.1 41.9 50.1 |
| r12-ctl-b | 41.1 | 40.4 | 40.4 41.0 41.0 41.1 40.9 41.2 43.4 50.2 48.6 |
| **pooled** | **41.0** | **39.5** | |

- No DEVICE_LOST (emulator.log and eden_log.txt: r12-ctl-a, -b, r12-confirm-flash, r13-base-prof).
- Fog score (capture-0.tga): ctl-a 0.497, ctl-b 0.506, flash 0.489, r13-base-prof 0.490 (fog present; threshold 0.35).
  r13-base-prof game.png correct: Kokiri spawn, fog, light shafts.
- Spawn spot vi2 (non-profiled, 48-124 s): frame 24.3-25.2 ms, **blocked on the GPU fence 14.2-15.0 ms (a ~14.98,
  b ~14.27, mean ~14.6)**. After the walk (~147-159 s): frame ~20.0-20.6 ms, fence 10.0-10.4 ms.

## Flash baseline (r12-confirm-flash, -Stay -GameplayWait 60 -Flash 40; lines from round12-integrate.md)
```
flash luma: n=40 median=0.4291 min=0.4191 max=0.4400 stdev=0.0056 outliers=0
flash cg: n=40 median=0.4270 min=0.4165 max=0.4486 stdev=0.0069 outliers=0
```

## Profile (r13-base-prof, EDEN_NX2_GPU_PROFILE=1 + fence_timeline; spawn ~27.4 FPS = ~36.5 ms/frame with profiler)
Gameplay marker at 22.5 s; `result: no device loss`. Spawn reports at 101.4/111.4 s (front) and 103.0/113.0 s (back);
ms/frame = ms/s / 27.4. Front zones sum to ~1000 ms/s and the waits by site nest inside them (mostly push, split
drain); back zones are flat; per-call zones are inflated by the profiler.

Front (cpu 13-18%):
- push 393.8-396.7 ms/s = **14.4-14.5 ms/frame**; idle 223-225 = 8.2; split drain 194-197 = 7.1-7.2; macro 143-145 =
  5.2-5.3 (~1.06M calls/s; 0x01 236k, 0x0e 157k, 0x1d 145k, 0x1b 125k, 0x50 55k, 0x6b 38k, 0x04 37k, 0x4b/0x4d 36k,
  0x16 30.0k at 79 ring words/call, 0x15 30k, 0x14 18k); list copy wait 23.5-24.0 = 0.87; pending sleep 15.0 = 0.55;
  resume pending 1.6 = 0.06.
- Waits for the back by site: **list writers 257-262 ms/s = 9.4-9.6 ms/frame (~5416/s)**, acquire trigger 161-165 =
  5.9-6.0 (~3276/s), acquire poll 24.9-26.8 = 0.91-0.98 (~1100/s), map 3.2-6.6 = 0.12-0.24 (27/s), fence release 1.4
  = 0.05 (192/s). Total ~453-455 ms/s = **16.6 ms/frame** (r12 base: 465 = 17.2).

Back (cpu 44-51%): split wait (idle) 274-277 = 10.0-10.1; back replay 180-181 = 6.6 (3.79-3.82M entries/s); method
154-155 = 5.6-5.7 (3.26-3.29M/s); other 87-88 = 3.2; bind uniform bank 40.2-40.3 = 1.47; draw 36.7-36.8 = 1.34 (~70k
draws/s, ~2550/frame); read handles 30.8 = 1.12; bind uniform stream 24.1 = 0.88; bind stage buffers 20.7-20.8 = 0.76;
update buffers 20.4 = 0.74; configure 16.8-16.9 = 0.61; compute 13.3-13.5 = 0.49; bind uniform cached 12.5-12.6 =
0.46; flush work 12.3 = 0.45; bind storage 12.2 = 0.45; pipeline lookup 11.6-11.7 = 0.43; fill image views 10.2 =
0.37; fence 9.7-9.8 = 0.36; push descriptors 6.6 = 0.24; render targets 5.0 = 0.18.

Counters (per second, 101-113 s): 3D register writes 7.69M front (4.27M same value), 2.76-2.78M back (0.79-0.80M
same); front direct 2.23M, from macros 3.23M; draw.begin coalesced 279-280k; same-value executable writes sent 1.07M
(top 0x586 396k, 0x8e5 119k, 0x585 79k, 0x5f8 77k, 0x5f7 77k, 0x6c3 62k, 0x54c 62k, 0x904 54k); acquires 3303-3305
(192 waited), acquire polls 16.2-16.4k (384 met); list copy 87-94/s, 302-312 us submit->done; MME DMA reads 0; back
buffer downloads 30.3-30.5 MiB in 1926-1933 copies (<60f 27.7-27.9 MiB); fencing thread 28.6-28.8 MiB written back;
bind memo 27.4-27.7k hits / 171-172k misses; bank rechecks skipped 55.5-56.0k; submits by origin: idle hook 990-1001,
waited 554-562, list copy 381-383, gpu idle 140, other 54-55. pending_wake 54-65 sleeps/s, all ended by a push.
dma_forward: every macro call forwarded, 0 not. spin_handoff (off): front_sleeps ~1120-1185/s, back_sleeps
~4560-4770/s. report_submit_early (off): pending 0/s at ~29.5-30.8k back calls/s.

fence_timeline p50/p99 (ms), spawn spot (58.8-124.3 s): entered->front_parse **26.00-26.23** / 29.7-30.8;
front_parse->back_incr 0.007-0.008 / 0.011-0.016; back_incr->waited 0.005 / 0.014-0.015; last_submit_pop->entered
0.55-0.71 / 1.78-1.93; back_idle_in_wait **1.438-1.452** / 1.77-1.79. After the walk (140.7-156.1 s):
entered->front_parse 15.4-15.9, back_idle_in_wait 1.17.
vi2 (profiled, 58.8-124.3 s): frame 36.3-36.6 ms, blocked on the GPU fence 26.2-26.4 ms. After the walk: frame
25.8-27.4, fence 15.9-16.6. Nx2Frame (102-112 s): iv_p50 ~35.6-37.3, fw_p50 ~26.0-26.6.

## Reading
Same structure as rounds 7-12, within noise of the round 12 base (profiled frame 36.5 vs 36.9 ms; front waits 16.6 vs
17.2 ms/frame; back replay + method 12.2 vs 12.4). The guest waits ~14.6 ms (non-profiled) for a fence the back
increments ~7 us after the front parses it; the back idles only ~1.45 ms of that wait. The front is the critical path:
its waits on the back (list writers ~9.5, acquire trigger ~6.0 ms/frame profiled) nest in push and split drain, and its
own CPU work is macro ~5.2 plus the rest of push. Round 12 showed the acquire waits are for releases not yet produced
(~80% unmet when the back has caught up), and that neither same-value executable writes nor dead state stores are
reducible. Levers stay: make the back reach the list-writer / acquire producers sooner (back replay + method ~12.2
ms/frame), or remove the front's need to wait via run-ahead with a shrunk snapshot (round 12: copy set fits; holes are
the host-write-path pages, ~11.6/frame, and the 4-page 'other' region at guest VA 0x2113d5c000 read by the front's
macro DMA at site RVA 0xa5e025).
