# Round 12 integration (2026-11-07 ~09:15, overnight, user asleep)

Base `01d41bf3ac` (round 12 base notes on `4caf96e5df`). **Accepted: none. Defaults unchanged.**

## Merges (all four candidates claimed bit 83; renumbered 83/84/85/86)
| bit | switch | kind | result |
|---|---|---|---|
| 83 | run_ahead_overlay_budget | measurement probe (needs frames_ahead + run_ahead_ledger + run_ahead_cow_probe + run_ahead_snapshot_probe) | stays off |
| 84 | acquire_drain_lite | trigger/poll acquires wait only for the back to consume everything sent, not for back_quiet (timing) | REJECTED (-0.2) |
| 85 | exec_same_drop | front leaves out executable writes the back would no-op (store-only same value, repeated CB binds) | REJECTED (nothing to act on) |
| 86 | state_dead_store | front folds a 3D state write into the same register's unpublished entry | REJECTED (nothing to act on) |

Merges `b66e66eee2`, `16522030d0`, `fd6c849ca4`, `47e17231c2`. Conflicts only in the switch lists of nx2_perf.h, the
expected-off list of tests/video_core/nx2_perf.cpp, and (state_dead_store) the FrontState members and Front() init in
maxwell_3d.cpp (both sides kept). Built yuzu-cmd yuzu tests: `[nx2_perf]` 57 cases, `[video_core]` 140,
`[split_writes]` 19, `[native_macro]` 2, all pass. Worktrees a42-86..89 and their branches removed.
exec_same_drop + state_dead_store interaction (both off, never run together): a dropped executable write does not bump
exec_gen, so a fold can span it; that is safe because the dropped write never reaches the back.

## Verify runs (-Quick, PERF_VERIFY=1, SPLIT_VERIFY=1 unless noted)
Split front register checks ~20k done, 0 differences in every run. No DEVICE_LOST anywhere.
- **r12-v-acquire_drain_lite**: 0 'word differs' (98 per-second lines all 0). At the spawn **lite_met ~5.3k/s,
  fallback_unmet ~19-21k/s**, fallback_flush 0. So ~80% of acquire checks are unmet and now pay WaitCaughtUp + the full
  drain.
- **r12-v-exec_same_drop**: vacuous. FrontExecSameDrop never drops when EDEN_NX2_SPLIT_VERIFY is set
  (`!FrontVerifyEnabled()`), so 0 checks. Rerun **r12-v-exec_same_drop-2** with PERF_VERIFY only: ~4.7k binds/s
  would be left out (4096-5120 per ~1.1 s), **0 store-only**, ~39k same-value store-only writes/s kept because they
  mark state dirty (index_buffer.first); back checks ~290k cumulative, **0 differed**, 0 deletion rebinds, 0 serial
  refreshes. Decision: rejected without speed runs. ~4.7k ring entries/s of ~3.8M back entries/s (0.1%) cannot give
  +1 FPS.
- **r12-v-state_dead_store**: back VerifyFold 73.7k folds checked, **0 differ**. But only **~950 folds/s** of ~1.39M
  state writes/s; 1.37M/s not folded because "no entry since an executable one" (3.8k value returns, ~10 published).
  The implementer's bar was ~5k folds per frame; this is ~25 per frame. Rejected without speed runs.
- **r12-v-run_ahead_overlay_budget** (ON=frames_ahead,run_ahead_ledger,run_ahead_cow_probe,run_ahead_snapshot_probe,
  run_ahead_overlay_budget, full run with the walk): no device loss, no garbage-stream crash this time; numbers below.

## Speed (interleaved, fast boot, -GameplayWait 100, fps_windows.py)
| runs | per-run median (min) | pooled median / min |
|---|---|---|
| r12-ctl-a / -b (defaults, HEAD 47e17231c2) | 40.0 (39.5) / 41.1 (40.4) | **41.0 / 39.5** |
| r12-s-acquire_drain_lite-a / -b | 40.8 (40.3) / 40.2 (40.1) | 40.8 / 40.1 |
vi2 fence wait at the spawn: controls 14.2-15.0 ms, acquire_drain_lite 14.5-14.7 ms. Fog 0.496-0.508 in all four.
acquire_drain_lite flash check r12-f-acquire_drain_lite: luma n=40 median 0.4345 stdev 0.0065 **outliers 0**, cg
stdev 0.0070 outliers 0, fog 0.497 (clean; smooth drift, no steps). Decision: -0.2 median / +0.6 slowest against the
fresh controls; below +1.0. Rejected (stays off at bit 84).

## Confirmation (defaults; nothing accepted, so the controls ran the confirmation binary)
Speed: r12-ctl-a 40.0 / r12-ctl-b 41.1, **pooled 41.0 median / 39.5 slowest** (windows 39.5 40.2 39.8 40.0 40.0
40.1 41.9 50.1 | 40.4 41.0 41.0 41.1 40.9 41.2 43.4 50.2 48.6). Same as the round baseline 40.8 / 39.7 within noise,
not better. Fog 0.497 / 0.506; game.png (r12-ctl-b) correct: Kokiri spawn, fog, light shafts.
Flash r12-confirm-flash (fog 0.489):
```
flash luma: n=40 median=0.4291 min=0.4191 max=0.4400 stdev=0.0056 outliers=0
flash cg: n=40 median=0.4270 min=0.4165 max=0.4486 stdev=0.0069 outliers=0
```

## run_ahead_overlay_budget numbers (r12-v-run_ahead_overlay_budget; per 600-epoch window, spawn windows 77.5 / 110.3 s, after-walk 134-166 s)
- Snapshot self-check: mismatch 0 (a_union+a_arm+a_both+b+c = cow hazard bytes exactly), differs 0,
  budget_verify_differences 0.
- **Copy set without the arm set**: unprotected union pages p50/p99 **2039-2046 / 2306-2341** (8.0 / 9.1 MiB),
  copy_ms p50/p99 **0.33 / 0.54-0.55**. Union pages by class per epoch: tracked ~3135, untracked ~1935, arm_set ~115,
  no_cpu 0. copy_set rule PASS at the spawn (needs p99 <= 3072 pages, p50 <= 0.6 ms); FAIL only in the boot window
  (p99 6576) and one after-walk window (p99 3121).
- **Arm set trap coverage**: arm pages read ~64.7k per window (~108/epoch), changed (arm_needed) 31.1-32.3k
  (~52/epoch), trapped 24.0-25.4k, **unarmed_changed 6.9-7.0k (~11.6/epoch)** of which hostw 6.0-6.2k (written through
  the device-memory host-write hooks), unknown 600-614. arm_trap rule FAIL.
- Hazard split: a_union 3.79-3.89M, **a_arm 0**, a_both 1.59-1.62M, b 0.46-0.50M, c 83-87k bytes per window.
- **span_hazard_bytes 0** at the spawn (189 B in the boot window, all bank at site RVA 0xcddd5a). span rule PASS.
- **front_macro residual 12.7-12.9k B per window (~21 B/epoch, ~2.2k events)**, residual categories
  vi2_delta 0, download 0, pre_copy 0, **all 'other'**. Records: every one at site RVA 0xa5e025, 6 bytes each, on 4
  adjacent physical pages 0x34d15c-0x34d15f (guest VA 0x2113d5c000-0x2113d5f3ff, device 0xd15dxxx-0xd160xxx).
  front_macro_residual rule FAIL (unattributed).

## What the round taught (critical path)
1. **The acquire waits are on unmet words, not on back_quiet.** acquire_drain_lite found ~80% of acquire checks
   (19-21k/s) unmet at the moment the back has consumed everything sent; only ~5.3k/s were met early. Dropping the
   back_quiet tail of the drain for the met ones gained nothing (-0.2). The front waits for a release that has not
   been produced yet (it comes from GPU work further on or from another channel), so making the drain cheaper does
   not shorten the wait. Same lesson as rounds 7-11: change when the producer finishes, not the shape of the wait.
2. **The front's same-value executable writes are almost all needed.** Of ~1.06M same-value executable writes/s, only
   ~4.7k/s are CB binds that repeat the last bind; zero store-only writes qualify, and index_buffer.first (the
   biggest store-only one) must stay because it marks Dirty::IndexBuffer. The ring traffic is not reducible this way.
3. **State writes are not dead between executable entries.** state_dead_store folds only ~950/s of ~1.39M state
   writes/s: almost every register is written once between executable entries (draws/binds), so there is nothing to
   fold. With eager_publish, entries are also published quickly. Front push cost is per-write parse/dispatch cost,
   not redundant writes.
4. **Run-ahead snapshot: the copy is now cheap enough, the traps are not exact.** Without the 17k-page arm set, the
   reply-time copy is ~2.0-2.3k pages (8-9 MiB, 0.33 ms p50 / 0.55 ms p99): it fits the budget. Two holes remain:
   (a) ~52 arm pages per frame are read by the back and changed by the guest; the write-protect traps catch ~40 of
   them, ~11.6 per frame change without a trap, mostly through the host-write hooks (hostw ~90%), i.e. emulator-side
   writes (DMA/download/WriteBlock paths) that bypass the cow arm. (b) The front's macro DMA reads see ~21 B per frame
   changed, all from one site (RVA 0xa5e025) on 4 adjacent physical pages, category 'other' (not vi2 delta, not a
   download, not pre-copy). Next design: have the host-write paths (Nx2Cow::HostWriteSlow callers) take a pre-image
   before writing an armed page, and identify the 4-page 'other' region (guest VA 0x2113d5c000) and either snapshot
   it or make the front's macro DMA reads of it wait for its writer. Only then a flash-tested run-ahead switch.
5. The front's waits on the back stay the critical path (list writers ~9.9 ms/frame, acquire triggers ~6.0 ms/frame
   in the round 12 base profile), and the spawn fence wait is unchanged at ~14.2-15.0 ms.
