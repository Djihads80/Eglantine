# Round 8 integration (2026-11-07, overnight, user asleep)

Base `3ea9227276` + `06f3e65b46` (round 8 base notes). Result HEAD: see `git log` (merges `75445aa5f2`..`55e1b6836c`,
test fix `f097e9f806`, comment `d01ca32784`). **Nothing accepted; defaults unchanged.**

## Merges (all default off, in always_off and DefaultOffMask, static_asserts renumbered)
| bit | switch | kind |
|---|---|---|
| 68 | inflight_wait_v2 | run-ahead (frames_ahead=1 for OoT + progress-based escapes) |
| 69 | list_fetch_after_sync | front list fetch skips the full writer wait when no GPU work wrote the list |
| 70 | spin_handoff | front/back spin instead of sleeping around handoffs |
| 71 | run_ahead_cover | measurement probe (with run_ahead_ledger) |

Conflicts: nx2_perf.h lists, the default-mask test list, gpu_split.cpp (includes; MaybeStart: both new init lines kept),
nx2_gsv.h (LedgerCoverClassify + the v2 InflightLog members both kept). Fix: `nx2_perf_switches.cpp` hard-coded
SpinHandoff == 68 -> 70. Built yuzu-cmd yuzu tests; `[nx2_perf]` 27 cases, `[video_core]` 137, `[split_writes]` 16,
`[native_macro]` 2: all pass. Worktrees a42-24..27 and their branches removed.

## Verify runs (-Quick, PERF_VERIFY=1, SPLIT_VERIFY=1)
- **r8-v-inflight_wait_v2** (+war_guard_probe so bank violations count): REJECT. Gameplay (30-104 s): 2835 epochs, every
  frame ran ahead, escapes only 2 (no_progress; round 6 escaped every epoch), but **bank pages changed before the fence
  1377** and **16 reads differ** (inflight_read_exact: a settled block changed when its fence landed). First-write waits
  ~200/s at ~9 ms avg, read waits ~27/s at ~8 ms. Frame interval at the spawn under verify ~33.8 ms vs ~35 for the other
  verify runs: little speed even when it runs ahead, because the guest's first writes wait ~9 ms for the fence.
- **r8-v-list_fetch_after_sync**: clean. 0 verify mismatches, 0 detector trips, 0 raced. Spawn: ~155k fetches/s,
  ~490 sync waits/s, ~6.9k skips/s.
- **r8-v-spin_handoff**: clean, verify_diffs 0, no boot/shutdown hang. Spawn: back_hits ~23k/s, back_sleeps ~6k/s,
  front_sleeps ~55/s, front_expired ~30/s, front_back_idle ~70/s, back_expired ~80/s.
- **r8-v-run_ahead_cover** (full run with walk, + run_ahead_ledger): probe only, stays off. Gameplay: verify_differences 0
  (ledger and cover), unmatched/capped/overflow 0 (nonzero only during boot loading < 26 s). Numbers below.

## Speed (defaults vs switch, interleaved, fast boot, fps_windows.py)
| runs | spawn windows median | pooled (incl. walk) median / min |
|---|---|---|
| r8-ctl-a / -b (defaults) | 37.75 / 37.1 (pooled spawn 37.6) | 37.7 / 36.8 |
| r8-s-list_fetch_after_sync-a / -b | 38.15 / 36.9 (pooled spawn 37.65) | 38.1 / 36.4 |
| r8-s-spin_handoff-a / -b | 37.3 / 38.3 (pooled spawn 37.6) | 38.2 / 37.0 |
Both are within noise of the controls (+0.0 at the spawn, +0.4/+0.5 pooled with the walk windows), below the +1.0 bar.
REJECT both on speed; flash checks skipped because neither could be accepted.

## Confirmation (defaults, same binary as the controls)
r8-ctl-a/-b are default runs on the final binary (only a comment changed after them), so they serve as r8-confirm-a/-b:
**pooled 37.7 median / 36.8 slowest** (spawn-only 37.6), no DEVICE_LOST, game.png correct (Kokiri spawn, fog and light
shafts). Fog: ctl-a 0.502, ctl-b 0.504, flash 0.495, cover 0.494. Same as the round baseline (37.8/37.1), not better.
r8-confirm-flash:
```
flash luma: n=40 median=0.4347 min=0.4214 max=0.4447 stdev=0.0058 outliers=0
flash cg: n=40 median=0.4320 min=0.4185 max=0.4435 stdev=0.0057 outliers=0
```

## run_ahead_cover numbers (per frame, spawn 60-63 s; after the walk 150-152 s in brackets)
- K-frame union misses (pages the back reads that were not in the union of the last K frames' read sets):
  K=1/2/4/8/16 = 265/86/73/14/6 [237/87/69/28/8]. Union size 3370 -> 4150 pages [2530 -> 3330].
- **changed_misses** (read pages the guest rewrote that the K-union did not cover): 3.5/2.4/2.0/2.0/1.9 [6.5/4.9/3.6/3.6/1.8].
  Never reaches 0 at K <= 16.
- changed pages ~120-129 [~93], gpu_changed ~75-123 [~71]. changed_class tracked/armset/untracked/nocpu:
  all = 55-63 / 2.5 / **62** / 0 [40 / 3 / **50** / 0]; untracked: bank 39 [26], ufast 15 [12.5], other 6-7 [9.5], desc 1.
  storage (29-36) and ucached are all tracked.
- RAW stall simulation: **land_stall p50 15.5-15.9 ms (p90 18-21)** vs **fenceN_stall p50 31.7-33.2 ms (p90 36-39)**
  [12.2-13.1 vs 22.1-23.1]. preflush commits ~41/frame [~51]. cover_ms ~0.18, ledger hash+compare ~1.7 ms/frame.
- Decision rules from the commit: CoW run-ahead **fails** (changed_misses never 0, untracked 50-62 pages/frame);
  raw_landing_wait **passes** (land p50 is ~17 ms [~10 ms] below fenceN p50).

## What the round taught (critical path)
1. Removing waits that are not on the critical path does nothing: list_fetch_after_sync skipped ~6.9k list-writer
   waits/s exactly (0 mismatches) and spin_handoff turned ~23k back sleeps/s into spin hits, yet both are +0.0 at the
   spawn. The handoff latency (sleep/wake) is not the cost, and the skipped list fetches were not the ones that block.
   The front's real stall is waiting for the back to *finish work* (acquire triggers / GPU-written lists / I2M), not
   the wake-up or the conservative wait itself.
2. inflight_wait_v2 almost removes the round-6 escapes (2 in 2835 epochs) but the write wait is not exact: the guest
   changed 1377 bank pages the back still read before the fence, and 16 settled reads differed. And even running ahead,
   each guest first write waits ~9 ms, so the gain is small. Trap-and-wait run-ahead is not the right shape.
3. run_ahead_cover: the bank/ufast/other pages the guest rewrites are mostly **untracked** by the CPU-side trackers
   (~50-62 pages/frame), so a copy-on-write snapshot would need its own write protection on those pages; a K-frame
   read-set union never covers all changed pages. The RAW model says waiting only for the writer's landing instead of
   the frame's fence would halve the guest's read stall (15.5 vs 32 ms p50 at the spawn). Next run-ahead design:
   snapshot (copy) the ~100 read-and-rewritten pages per frame at present time, using write protection on the
   back's read set for untracked pages, plus raw_landing_wait for GPU-written reads.
