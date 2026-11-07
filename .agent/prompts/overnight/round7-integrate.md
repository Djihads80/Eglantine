# Round 7 integration (2026-11-07 ~01:00-02:30, overnight, user asleep)

HEAD before: `79e92ef9fc` (agent notes on top of `2e4dd741f8`). After: merges `44749604ad` (run_ahead_ledger),
`0497284ce8` (compute_writer_ranges), `3554c483d3` (acquire_queued_release), `b754001126` (nonwriter_proof_memo), then
`cbda44d624` (always_off comment with the results). Built yuzu-cmd yuzu tests; `[nx2_perf]` 24 cases, `[video_core]` 137,
`[split_writes]` 16, `[native_macro]` 2: all pass. Worktrees wf_c2b11e26-a42-8..11 and their branches removed.

## Merge notes
- All four candidates carried their own "widen the off-mask to 128 bits" commit, each putting its switch at bit 64. Kept
  run_ahead_ledger's widening (0848bbb3d1, merged first), took "ours" for nx2_perf.h/.cpp/tests on the other three and
  re-added each switch by hand: run_ahead_ledger 64, compute_writer_ranges 65, acquire_queued_release 66,
  nonwriter_proof_memo 67 (Opt, opt_names, DefaultOffMask, always_off, static_assert, test expected-off list).
  acquire_queued_release's own unit test was ported to index 66 (`Bit == OffMask{0, 4}`). nx2_profile.h merged cleanly.

## Baseline problem (decision)
The round baseline (r7-base, 34.6 / 33.8) was stale: two interleaved default runs tonight gave **37.6 and 37.8 (pooled
37.8 / 37.3)**, fence wait p50 ~16.3 ms vs ~17-19 in r7-base. Cause not found (no shader compiles in either; the base runs
were the first runs after the user placed the save and closed the GUI). **I judged every switch against the fresh controls
(r7-ctl-a/-b), not 34.6**, otherwise nonwriter_proof_memo (+3.4 vs the stale base, +0.2 vs the controls) would have been
accepted on noise. Next round: measure its own base with interleaved controls.

## Results
| switch | verify | flash | speed (pooled median / slowest) | decision |
|---|---|---|---|---|
| run_ahead_ledger (probe) | 0 differences, 0 error lines | - | quick verify run 33.7 (probe+verify cost) | stays off (measurement) |
| compute_writer_ranges | 0 mismatches, 0 uncovered marks | 0 outliers, sd 0.0045 | a 35.0, b 35.0, c 35.4 (controls 37.8) | **REJECTED (-2.5 FPS)** |
| acquire_queued_release | 0 mismatches | not run (no gain) | 37.5 / 36.9 | REJECTED (no gain; 46 forwarded/s of ~18k attempts) |
| nonwriter_proof_memo | 0 mismatches (62% hits: 23.4k hits / 14.5k misses per s) | not needed (back-local) | 38.0 / 37.1 | REJECTED (+0.2 vs controls) |

Controls: r7-ctl-a 37.6/37.3, r7-ctl-b 37.8/37.4. Confirmation (defaults, merged code): **r7-confirm-a 37.9/37.6, r7-confirm-b
37.5/37.1, pooled 37.8 / 37.1**; fog 0.497 / 0.505; game.png correct (Kokiri spawn, fog and light shafts); no DEVICE_LOST
(emulator.log and eden_log.txt). r7-confirm-flash: `flash luma: n=40 median=0.4320 min=0.4213 max=0.4385 stdev=0.0047
outliers=0`, `flash cg: n=40 median=0.4290 min=0.4126 max=0.4413 stdev=0.0080 outliers=0`.
Pre-existing verify noise in every verify run (not from these switches): front same-first 0x3ed (control 3, 64 lines),
guest_landed_syncpoints observation counts, preflush_aging dropped-block stall.

## compute_writer_ranges: why it is slower (profiled pair r7-prof-cwr vs r7-prof-ctl, GPU_PROFILE + fence_timeline)
| front, ms/s | ctl | cwr |
|---|---|---|
| push | 295-305 | 372-399 |
| split drain | 184-189 | 100-102 |
| list writers wait | 196 (6483/s) | 257 (15114/s) |
| acquire trigger wait | 141 | 95 |
| acquire poll wait | 40 | 2.8 |
Its per-second line (profiled): 0 full waits, 13447 narrowed waits/s, of which **10765 target an unknown write** (I2M /
inline-to-memory, LastUnknown term) and 2682 an unlocalized writer; 4227 waits avoided. The narrowed wait stops at an older
position instead of draining, so the next fetch waits again: 2.3x more front->back handoffs, each with wake latency, and
more total waiting. The old full wait drained once and covered the following fetches. Also launch fallbacks qmd ~4000/s
and dependent ~6900/s (localizable only after the back caught up). Lesson: on this front, **fewer waits beats shorter
waits**; and unknown-effect writes (I2M) are what list fetches really depend on.

## run_ahead_ledger measurement (r7-v-run_ahead_ledger, gameplay 94-105 s, per frame)
- Back reads 2200-2600 device pages per frame (23-36 MB): bank 1490-1770 pages (19-30 MB), ufast 310-350 (2.3-3.5 MB),
  other 350-430, storage 160-240, ucached 30-180, desc 44-49, vertex/index/texture ~0 at the spawn.
- **Pages the back read for frame N that the guest rewrites before the next present: 87-124 per frame**; exact changed
  bytes (first 1024 pages saved in full) ~40-60 KiB per frame: ufast 14-22 KiB, bank 11-13, ucached 6-8, storage 6-10,
  other 4-7, desc ~1.3. gpu_written 57-141 pages. ptr_only (size unknown, GetPointer only) 78-98 pages.
- unmatched=0, capped=0, overflow=0, verify_differences=0; probe cost hash ~0.65-0.75 ms + compare ~0.6 ms per frame.
- RAW (guest reads of GPU-written blocks): ~50-80/s; writer ran 3.3-5.1 ms (p50, p90 4-6 ms) before the guest's read
  inside the frame; raw_pre_reply=0, raw_unfenced=0.
- Reading: round 6 saw ~20k changed bank pages before the fence, but the back actually **reads** only ~1.6k bank pages and
  only ~100 of all read pages (~50 KiB) change before the next present. A per-frame snapshot of exactly those bytes (copy
  the read set's rewritten pages, or copy-on-write via write traps on the ~2.4k read pages) is cheap enough to make
  frames_ahead flash-free. That is the best-supported next step for the run-ahead lever (+~9 FPS historically).

## For the next round
- Critical path unchanged: front (push + waits on the back). Splitting the list-writer wait into narrower waits made it
  worse; target the cause instead: list fetches that depend on I2M (unknown-effect) writes, and acquire triggers.
- Safe run-ahead now has numbers: ~100 pages / ~50 KiB per frame to snapshot (ufast, bank, storage, ucached). Design:
  at the frame's present, trap guest writes to the back's pending read set (from the ledger) and copy the old page into a
  per-frame shadow the back reads, or snapshot those pages at submit. Must pass 0 flash outliers.
- Re-measure the round baseline with interleaved controls; 37.8 / 37.1 is tonight's default.
