# Round 10 integration (2026-11-07, overnight, user asleep)

Base `f8dc5c9c30` (round 10 base notes on `ac0157ffcc`). **Accepted: none. Defaults unchanged.**

## Merges (default off; all three candidates claimed bit 76, renumbered 76/77/78)
| bit | switch | kind | result |
|---|---|---|---|
| 76 | run_ahead_raw_probe | measurement probe (needs frames_ahead + run_ahead_ledger + run_ahead_cow_probe) | stays off |
| 77 | list_writer_hook_copies_only | idle hook after a list-writer wait submits only the list copies (timing) | REJECTED (+0.0) |
| 78 | front_macro_memo | replays recorded emits of pure front macro calls | REJECTED (+0.0, 7% hit rate) |

Merges `ad94ad3f9b`, `125a002aab`, `0b44eaf032`, test fix `b3e91cb3c6` (my resolver left the list_writer test case
unclosed). Conflicts were only the switch lists in nx2_perf.h and the two tests. Built yuzu-cmd yuzu tests:
`[nx2_perf]` 40 cases, `[video_core]` 137, `[split_writes]` 16, `[native_macro]` 2, all pass. Worktrees a42-56..58 and
their branches removed.

## Verify runs (-Quick, PERF_VERIFY=1, SPLIT_VERIFY=1)
Same pre-existing noise as rounds 8-9 (guest_landed_syncpoints, same-first 0x3ed, preflush_aging): not from these switches.
Split front register checks 20417 done, 0 differences in each run.
- **r10-v-list_writer_hook_copies_only**: clean. `0 differences in 145959 checks` (list reads during a skipped submit),
  0 "found a skipped deferred submit pending" lines at Drain/FlushQueryResults/WaitSubmitted.
- **r10-v-front_macro_memo**: clean (`verify 5.7M checked, 0 differed`), but **hit rate 7%** (~69k hits / ~980k calls/s;
  ~910k/s not memoizable). Every hot macro is classified "(no)": 0x01 186k, 0x1d 191k, 0x0e 164k, 0x1b 89k, 0x04 48k,
  0x14 24k calls/s. Hits come only from small macros (0x02, 0x05, 0x07, 0x0f, 0x10, 0x15 at 96-100%).
- **r10-v-run_ahead_raw_probe** (ON=frames_ahead,present_one_ahead,run_ahead_ledger,run_ahead_cow_probe,run_ahead_raw_probe):
  probe only, numbers below. Raw verify_differences 0 (the cow probe's own verify_differences 1.7-3.7k is the known
  chunked-vs-byte hazard-count approximation from round 9).

## Speed (interleaved, fast boot, -GameplayWait 100, fps_windows.py)
| runs | per-run median (min) | pooled median / min |
|---|---|---|
| r10-ctl-a / -b (defaults) | 39.4 (38.2) / 39.6 (39.1) | **39.4 / 38.2** |
| r10-s-list_writer_hook_copies_only-a / -b | 39.2 (38.3) / 39.5 (39.0) | 39.4 / 38.3 |
| r10-s-front_macro_memo-a / -b | 39.5 (38.1) / 39.4 (38.4) | 39.4 / 38.1 |
vi2 fence wait at the spawn: controls 15.1-15.4 ms, list_writer 14.9-15.6, memo 15.4-15.8. No DEVICE_LOST anywhere.
Fog 0.495-0.507. Flash checks for the two switches skipped: rejected on speed.

## Confirmation (defaults, binary `b3e91cb3c6`; no accepted switch)
Decision: the controls r10-ctl-a/-b ran the same binary at defaults, so they are the speed confirmation (no separate
r10-confirm-a/-b). **39.4 median / 38.2 slowest** vs round base 39.3 / 37.6: the same within noise, not better.
game.png (r10-ctl-b) correct: Kokiri spawn, fog, light shafts. Fog: 0.495 / 0.507 / flash 0.499.
```
flash luma: n=40 median=0.4330 min=0.4234 max=0.4410 stdev=0.0054 outliers=0
flash cg: n=40 median=0.4313 min=0.4122 max=0.4449 stdev=0.0077 outliers=0
```

## run_ahead_raw_probe numbers (spawn, steady reports at 68/83/99 s; per frame)
- Guest reads of preflushed blocks: 75-91/frame, frame-N writer reads 75-84, resolved 52-69.
- **stale 2.6/frame (10.3-10.6 B/frame), all uncovered**: known.unc.stale 0.5-0.85, **nowriter.unc.stale 1.8-2.05**
  (~70% of stale reads had no frame-N writer yet when the guest read). Covered classes are 0; tl_wait_reads 0 and
  stale_uncovered_any_wait = stale, so no guest syncpoint wait (on any thread) orders these reads after their writer.
- land_after_read p50/p90 0.7-2.8 / 2.5-6.9 ms; read_after_reply p50/p90 5-6 / 7.7-8.3 ms; landed_before_read ~22-29k
  per 600 frames (most reads are already fresh); no_frame_writer ~13k per 600.
- unarmed_changed (cow) p50/p99 0-7 / 14-25 per frame; sampled gap classes: unarmed.mem 15-32/64, unarmed.rcached
  12-27, armed.debug.hostw 6-24, unarmed.rcached.hostw 1-3 = **4 classes** (rule: <= ~3 fixable).
- Unsized sites: 13-15 with hits (top RVAs 0xcc6ed2, 0xcc9a8d, 0xa92b0e, 0xb8066a).
- Decision rules: stale is not ~0, it is mostly no_writer_yet -> **exact RAW needs fence-N waits: the run-ahead RAW lever
  is dead** as designed; guest_landed_syncpoints does not make it exact (covered = 0); gap classes 4 > 3.

## What the round taught (critical path)
1. **Deferring the back's other work at list-writer idle hooks does not move the front.** list_writer_hook_copies_only
   (verify clean) gave +0.0: the list-writer wait is ended by the copy, and the extra submits the old hook made were not
   what the front waited on. Same lesson as rounds 7-8: changing what happens around a wait does not help unless the
   back finishes the waited-for work earlier.
2. **The front macro cost is in macros that cannot be memoized by a static scan.** All hot macros (0x01, 0x1d, 0x0e,
   0x1b, 0x04, 0x14; ~700k of ~980k calls/s) use backward branches or data RAM, so the memo hit 7% of calls, and only
   cheap ones. Profiled macro is ~4.9 ms/frame; a memo would need loop-aware classification (or per-call dynamic
   validation of data RAM reads) to reach the hot macros, and even then the gain is bounded by that 4.9 ms.
3. **Run-ahead via RAW waits is closed.** The stale guest reads of GPU-written data are few (~2.6/frame, ~10 B) but
   ~70% happen before the frame-N writer is even recorded, and no guest wait covers any of them. Only fence-N waits
   would make them exact, which removes the run-ahead. Together with round 9's CoW result (unarmed_changed != 0, 11-15
   unsized sites), the remaining run-ahead route is a full snapshot of what the back reads at submit (storage, ucached,
   "other"), not trapping or waiting.
4. The front critical path is unchanged from the round 10 base profile: list writers (~10.5 ms/frame) and acquire
   triggers (~6.9 ms/frame) waiting on real back work. Next levers: make the back reach the list-writer / acquire
   producers sooner (priority or reordering in the ring), or cut back replay+method (~12.5 ms/frame) directly.
