# Round 11 integration (2026-11-07, overnight, user asleep)

Base `e3fb7b9089` (round 11 base notes on `2ac97bc933`). **Accepted: diag_draw_lite (lab-only gain, see below).**

## Merges (all four candidates claimed bit 79, renumbered 79/80/81/82)
| bit | switch | kind | result |
|---|---|---|---|
| 79 | run_ahead_snapshot_probe | measurement probe (needs frames_ahead + run_ahead_ledger + run_ahead_cow_probe) | stays off |
| 80 | diag_draw_lite | back counts lab diag draws (EDEN_NX2_DIAG) in a thread-local table, folds under one lock | **ACCEPTED, on by default** (`c805603330`) |
| 81 | report_submit_early | early fence for 0x16 report waits (timing) | REJECTED (nothing to act on) |
| 82 | dma_forward_tape | back evaluates a recorded tape instead of re-running macro 0x16 for a forwarded call | REJECTED (+0.35 spawn) |

Merges `10a2336d55`, `c9a51f75fb`, `05177320b9`, `2aaf3c6b5b`. Conflicts only in the switch lists of nx2_perf.h and the
expected-off list of tests/video_core/nx2_perf.cpp; each candidate's own test/static_assert renumbered. Built yuzu-cmd
yuzu tests: `[nx2_perf]` 51 cases, `[video_core]` 137, `[split_writes]` 16, `[native_macro]` 2, `[macro_dma_forward]` 5,
all pass. Worktrees a42-70..73 and their branches removed.

## Verify runs (-Quick, PERF_VERIFY=1, SPLIT_VERIFY=1)
Pre-existing noise only (guest_landed_syncpoints, same-first, preflush_aging, unmapped ReadBlock at boot). Split front
register checks ~20k done, 0 differences in every run. No DEVICE_LOST.
- **r11-v-dma_forward_tape**: 1.97M calls verified, **0 differed**; 0 overflow and 0 unsupported-op fallbacks (every 0x16
  call ran from a tape). EvalTape 28-29 ns vs Recompute 159-166 ns mean: saves ~135 ns x ~30k calls/s = ~4 ms/s on the back.
- **r11-v-report_submit_early**: 0 'words differ at' lines. But **pending 0/s and blocked 0.00 ms/s in 84 of 85 seconds**
  (one boot second: 2 pending, 3.3 ms), waits on early fence 0/s, 12-17k FlushQueryResults calls/s on the back. The report
  waits it targets never block at the spawn, so it cannot gain. Decision: rejected without speed or flash runs.
- **r11-v-diag_draw_lite**: 'Nx2Perf diag_draw_lite: 0 differences' every second (96 lines), no error lines.
- **r11-v-run_ahead_snapshot_probe** (ON=frames_ahead,run_ahead_ledger,run_ahead_cow_probe,run_ahead_snapshot_probe):
  the first run **died at 26.6 s** (4 s into gameplay) after the front parsed garbage: 'Draw skipped: 2432696322 vertices',
  KeplerMemory invalid-register asserts, 'Unimplemented engine E000600', puller methods 1..3F; the log stops mid-line.
  The rerun (r11-v-run_ahead_snapshot_probe-2) completed with no device loss. Not attributed: frames_ahead (always off,
  lets the guest run ahead) was on too. Numbers below are from the rerun.

## Speed (interleaved, fast boot, -GameplayWait 100, fps_windows.py)
| runs | per-run median (min) | pooled median / min | spawn windows only, pooled median |
|---|---|---|---|
| r11-ctl-a / -b (defaults) | 39.5 (38.5) / 38.8 (37.5) | **39.2 / 37.5** | 38.9 |
| r11-s-dma_forward_tape-a / -b | 39.9 (39.7) / 38.8 (37.7) | 39.8 / 37.7 | 39.25 |
| r11-s-diag_draw_lite-a / -b | 40.3 (39.9) / 39.9 (39.3) | **40.2 / 39.3** | 40.0 |
Fog 0.494-0.508 in all six. vi2 fence wait at the spawn ~15-15.6 ms everywhere.

Decisions:
- dma_forward_tape: +0.6 pooled / +0.35 spawn-only, below +1.0. Rejected (verify exact; stays off at bit 82).
- diag_draw_lite: +1.0 pooled median and +1.8 slowest against the fresh interleaved controls (both runs ~40, the controls
  38.8-39.5); +0.8 / +1.1 against the round baseline 39.4 / 38.2. **Accepted, judged against the fresh controls as in
  rounds 7+.** Caveat: the lab always runs with EDEN_NX2_DIAG=1 and EDEN_NX2_DIAG_DIR (tools/nx2/lab_ctl.py), so this
  removes lab harness overhead (the per-draw diag lock on the back). The user's eglantine.exe has no EDEN_NX2_DIAG, so
  **the user's game does not get faster**; the lab simply measures closer to it. Without EDEN_NX2_DIAG the switch does
  nothing. No timing change, verify exact, so no flash run of its own (the confirmation flash run covers it). Kept in
  DefaultOffMask, so SAFE turns it off, like pending_wake.

## Confirmation (defaults, binary `c805603330`, diag_draw_lite=on in the log)
| run | median | min | windows |
|---|---|---|---|
| r11-confirm-a | 40.2 | 39.7 | 39.7 40.0 40.1 40.2 40.0 40.3 43.3 51.0 51.8 |
| r11-confirm-b | 41.0 | 40.5 | 40.5 41.0 41.0 40.8 40.6 40.7 44.3 50.5 52.8 |
| **pooled** | **40.8** | **39.7** | |
+1.4 median / +1.5 slowest over the round baseline 39.4 / 38.2; that gain is the lab's diag overhead, not a speed-up of
the user's game. Fog 0.496 / 0.500 / flash 0.497. game.png (r11-confirm-b) correct: Kokiri spawn, fog, light shafts.
No DEVICE_LOST.
```
flash luma: n=40 median=0.4317 min=0.4207 max=0.4402 stdev=0.0050 outliers=0
flash cg: n=40 median=0.4293 min=0.4152 max=0.4474 stdev=0.0065 outliers=0
```

## run_ahead_snapshot_probe numbers (rerun, spawn, 600-epoch windows at 67.8 / 82.2 / 95.6 s)
- Snapshot set per epoch: pages p50/p99 21.1-22.2k / 22.2-24.0k = union of the last 16 frames' read pages 4.0-5.1k +
  **arm set 17.1-17.3k** (dirty unprotected tracker pages). The 32 MiB pool (8192 pages) overflows every epoch
  (overflow ~7.8M pages per window), so the real copy would be ~85 MiB per frame, not the 32 MiB measured.
- copy_ms p50/p99 1.50-1.52 / 1.9-2.1 (for the capped 32 MiB), hash_ms 1.07-1.11, build_ms 0.26-0.28.
- Cow hazard bytes per epoch, a/b/c p50/p99: a (covered by the copy) 8.8-10.0k / 26-30k, b (pre-imaged through
  HandleRasterizerWrite) 0.5-1.0k / 2.6-4.4k, **c (residual) 24-36 / 54-1032 B**.
- Residual c by category: at 82 and 95 s **only front_macro** (23-33 B/epoch; unregistered, reason stale; site RVA
  0xa5a505); at 67.8 s also storage 82 B (reason pool_full) and ufast/ucached ~8 B.
- Self-check: mismatch 0 (a+b+c equals the cow hazard bytes exactly), differs 0, verify_differences 0, table/ring overflow 0.
- RAW: untrapped_gpu_reads ~3.4M per window, all re-armable at the reply, 0 in areas kept before the reply;
  trapped 43-57k; t_first_ms p10/p50/p90 4.1-4.3 / 4.5-4.8 / 5.5-5.9.
- Decision lines: **union_snapshot rule FAIL** (residual c p99 54 B, needs 0; copy_ms p50 1.50-1.52 vs < 1.5, and that
  with the pool capped). **raw_hold rule PASS** (t_first p50 4.5-4.8 ms >= 3).

## What the round taught (critical path)
1. **The lab's own diagnostics cost ~1 FPS.** diag_draw_lite took the per-draw diag lock off the back and gained +1.0
   pooled in the lab (both runs ~40). Since the lab always runs with EDEN_NX2_DIAG, every earlier lab number
   understated the user's build by about that much, and future rounds' baselines move up by ~1 FPS (round 12 base
   ~40.8 / 39.7) without the game changing. Next round: before trusting a lab gain, check whether the lab's env
   (EDEN_NX2_DIAG, DIAG_DIR, DIAG_SAMPLES) is involved.
2. **Back-side costs off the critical path still do not move the frame.** dma_forward_tape is exact and makes each
   forwarded 0x16 recompute ~5x cheaper (~4 ms/s of back time), and gained +0.35 at the spawn. Same lesson as
   bank_hash_shadow and nonwriter_proof_memo: the back is ~60% busy and the front waits on specific producers, not on
   the back's total time.
3. **0x16 report waits do not block.** report_submit_early found 0 pending reports at 12-17k waits/s: by the time the
   back reaches a report's FlushQueryResults the fence has already landed. The report/occlusion path is not on the
   critical path any more (dma_forward removed its front drain in round 5).
4. **Run-ahead by full snapshot: close but not exact, and too big.** A reply-time copy of the union of the last 16
   frames' read pages plus the dirty unprotected tracker pages leaves only 24-36 B per frame uncovered, all from the
   **front's macro DMA reads** (site 0xa5a505, unregistered pages read before the frame's writes land). But the set is
   ~21k pages (~85 MiB) per frame, dominated by the 17k-page arm set, so copying it costs well over the 1.5 ms budget.
   Next design: shrink the arm set (most of those dirty unprotected pages are never read by the back; intersect it with
   the read union, or protect them), and make the front's macro DMA reads come from the snapshot (or wait for their
   writers) so residual c reaches 0. Only then is a flash-tested run-ahead switch worth building.
5. The front's waits on the back (list writers ~9.8 ms/frame, acquire triggers ~6.4 ms/frame, round 11 base profile)
   are unchanged and still the critical path. Next levers stay: make the back reach the list-writer / acquire producers
   sooner, cut back replay + method directly, or cut the front's push cost.
