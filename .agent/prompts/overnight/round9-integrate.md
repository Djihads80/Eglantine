# Round 9 integration (2026-11-07, overnight, user asleep)

Base `9ce18ffd48` (round 9 base notes on `0ef52d85b4`). **Accepted: pending_wake (now on by default).**

## Merges (default off when merged; bits renumbered from the tentative 72s)
| bit | switch | kind | result |
|---|---|---|---|
| 72 | run_ahead_cow_probe | measurement probe (needs run_ahead_ledger + a forced one-ahead reply) | stays off |
| 73 | sync_free_fetch | early-met acquires make no list-fetch sync point | REJECTED (-0.5) |
| 74 | pending_wake | the suspended channel's 1 ms front sleep ends on a submit or latch | **ACCEPTED, default on** |
| 75 | bank_hash_shadow | per-page 64-bit hashes replace the uniform bank's byte shadow memcmp | REJECTED (-0.3) |

Merges `3239d97248`, `bc4ad35805`, `c588dc660c`, `558d86ba78`; default change `6a8a624359`. Conflicts were only the
switch lists in nx2_perf.h / the tests, nx2_profile.h counters (both sets kept, Count 67 < 80) and nx2_gsv.h (CowClose and
the guest_wait_entered_ns clear both kept). Built yuzu-cmd yuzu tests: `[nx2_perf]` 35 cases, `[video_core]` 137,
`[split_writes]` 16, `[native_macro]` 2, all pass (also after the default change). Worktrees a42-40..43 and branches removed.

## Verify runs (-Quick, PERF_VERIFY=1, SPLIT_VERIFY=1)
Every run shows the same pre-existing verify noise as round 8 (guest_landed_syncpoints ~95-98, same-first 0x3ed 64,
preflush_aging ~35): not from these switches.
- **r9-v-sync_free_fetch**: clean. `sync_free_fetch: 0 verify differences in ~4.8k checks` (per second);
  list_fetch_after_sync 0 raced / 0 trips / 0 mismatches (~154k fetches/s, ~560 sync waits, ~6.1k skips);
  acquire_trigger_early 0 met-early errors (~146k met early per report).
- **r9-v-pending_wake**: clean. 0 "resumed unmet". ~71-82 pending sleeps/s, all ended by a push (latch 0, timeout 0).
  The "in guest fence waits" count needs fence_timeline and was 0 here.
- **r9-v-bank_hash_shadow**: clean, 0 "page decision differs".
- **r9-v-run_ahead_cow_probe** (ON=frames_ahead,present_one_ahead,run_ahead_ledger,run_ahead_cow_probe): probe only.
  Numbers below.

## Speed (interleaved, fast boot, -GameplayWait 100, fps_windows.py)
| runs | per-run median (min) | pooled median / min |
|---|---|---|
| r9-ctl-a / -b (old defaults) | 38.0 (37.0) / 37.2 (36.8) | 37.8 / 36.8 |
| r9-s-sync_free_fetch-a / -b | 37.1 (36.7) / 37.3 (36.9) | 37.3 / 36.7 |
| r9-s-pending_wake-a / -b | 39.5 (39.2) / 39.2 (37.9) | **39.3 / 37.9** |
| r9-s-bank_hash_shadow-a / -b | 37.7 (37.3) / 37.3 (37.1) | 37.5 / 37.1 |
pending_wake spawn windows 39.1-39.6 in both runs against 36.8-38.0 for the controls. vi2 at the spawn: frame
25.4-25.6 ms, fence wait 15.5 ms against 26.4-26.6 / 16.1 for ctl-a (~1 ms/frame less).
- **r9-f-pending_wake** (flash): `luma n=40 median=0.4328 min=0.4242 max=0.4440 stdev=0.0049 outliers=0`,
  `cg n=40 median=0.4294 min=0.4185 max=0.4449 stdev=0.0070 outliers=0` (base luma sd 0.0058, cg 0.0057; smooth drift, no
  steps). Fog 0.504 / 0.492 / 0.488; game.png correct. No DEVICE_LOST.
- Flash checks for sync_free_fetch and bank_hash_shadow skipped: rejected on speed.

## Confirmation (new defaults, `6a8a624359`)
- r9-confirm-a 39.2 (37.6), r9-confirm-b 39.4 (38.4): **pooled 39.3 median / 37.6 slowest** (round base 37.7 / 36.8:
  +1.6 / +0.8). No DEVICE_LOST (emulator.log and eden_log.txt). game.png correct (Kokiri spawn, fog, light shafts).
- Fog: 0.488 / 0.493 / flash 0.495.
- r9-confirm-flash:
```
flash luma: n=40 median=0.4329 min=0.4258 max=0.4389 stdev=0.0036 outliers=0
flash cg: n=40 median=0.4303 min=0.4143 max=0.4425 stdev=0.0066 outliers=0
```

## run_ahead_cow_probe numbers (spawn, steady reports at 70/84/100 s; per frame, p50/p99)
- Traps (first guest write of an armed page): tracked 313-315/338-369, untracked 260-306/296-448, bank 46-52/55-68;
  ns/trap 1.23-1.26 us; **guest_trap_ms 0.7-0.8 / 1.4-3.5** (rule <= ~8 ms: PASS).
- preimaged 618-686 / 676-749 pages, **pool_overflow 0** (4096 budget: PASS; 78k only in the loading window at 45 s).
- Arm 0.4-0.5 / 0.6-0.8 ms; first full arm ~1.0M pages (once at boot).
- **unarmed_changed** p50 1-3 / p99 9-30 per frame (1.6k-3.3k per 600 frames; host_changed 5-7k, preimage_stale 3-6k per
  600): rule needs 0: **FAIL**. Writes that bypass the hooks (raw GetPointer, WriteBlockUnsafe) still change read pages.
- **Unsized sites: 11-13** return addresses with hazard hits (rule <= ~5: **FAIL**); top 0x...0fd2 ~87-161k,
  0x...3b8d ~65-87k, 0x...eb6e ~66-70k per 600 frames.
- **hazard_bytes p50 12.2-18.6 KB / p99 27.7-36.0 KB per frame, by category: storage 7.8-17.7 KB, ucached 2.9-4.3 KB,
  other 2.1-2.7 KB, macro_list ~55 B, ufast 0-5 B, desc 0-1.4 B, bank 0.0.** So the WAR hazard is not in the bank
  (the guest rewrites bank pages but the back reads none of the changed bytes after the reply); it is in storage,
  ucached and "other" reads.
- Back reads ~25-41 MB/frame (bank 25-41 MB, ufast 2.8-4.1 MB, texture small); host_write_through 3-17k per 600 frames.
- verify_differences 4480: the chunked hazard count differs from the byte loop by a few bytes per read (e.g. 8 vs 1,
  147 vs 146): the hazard byte totals are approximate (the guest writes concurrently and the chunk count rounds), not a
  behaviour problem (the probe changes nothing).
- Under verify + probe + one-ahead the frame was ~27.1 ms with fence wait ~0.08 ms (598/600 ran ahead): no speed reading
  possible here (verify cost).
- Decision rules: CoW run-ahead **not** to be built as specified (unarmed_changed and unsized sites fail); hazard is not
  ~0 outside bank, so the flash is not purely RAW either.

## What the round taught (critical path)
1. **The front's own fixed sleeps are on the critical path; back handoff wakes are not.** pending_wake turns the
   suspended channel's 1 ms sleep_for into an event wait ended by the next submit (all ~75 sleeps/s ended by a push).
   That gave ~1 ms/frame and +1.5 FPS, where round 8's spin_handoff (23k back sleeps/s -> spins) gave +0.0. The front
   waits for the guest's next submit, so any fixed-quantum sleep there adds directly to the frame. Next: hunt other
   fixed sleeps or polling quanta on the front/guest path (acquire poll ~1k/s, list copy wait 22 ms/s, vi2/nvdrv waits,
   syncpoint wait polling in the guest's fence wait).
2. Removing list-fetch sync points (sync_free_fetch) and making the bank compare cheaper (bank_hash_shadow) do not help:
   the bank compare is not a measurable back cost at the spawn and the skipped syncs do not block the fence parse.
3. Run-ahead CoW: trap cost (<1 ms/frame) and pre-image volume (~650 pages/frame) are affordable, but the hazard bytes
   that matter are in storage (SSBO), ucached and other reads (12-18 KB/frame), not the bank, and some writes escape the
   hooks (unarmed_changed 1-3 pages/frame) through raw pointers. A run-ahead design must snapshot or protect storage and
   ucached reads at the reply (they are CPU-tracked, so the buffer cache can copy them at present time) and close the
   raw-pointer write paths (11-13 GetPointer call sites) before any flash test.
