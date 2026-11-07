# Round 13 integration (2026-11-07 ~11:20, overnight loop, user asleep)

Base `ab2a428068` (round 13 base notes on `6286d0e3a2`). **Accepted: none. Defaults unchanged.**

## Merges (all four candidates claimed bit 87; renumbered 87/88/89/90)
| bit | switch | kind | result |
|---|---|---|---|
| 87 | run_ahead_own_write_probe | measurement probe (needs frames_ahead + run_ahead_ledger + run_ahead_cow_probe + run_ahead_snapshot_probe + run_ahead_overlay_budget) | stays off |
| 88 | bank_dirty_skip | native bank pages compared only when marked (trap / tracker / write hook) | REJECTED (-0.8) |
| 89 | back_replay_lean | split back runs plain 3D entries through Maxwell3D::BackRun | REJECTED (-0.2) |
| 90 | front_macro_call | split front calls a whole in-entry 3D macro call straight to FrontRunMacro | REJECTED (-0.1) |

Merges `c1108d5d5d`, `e4c60ac620`, `a4a2fce0b6`, `09c66d24d8`. Conflicts: the switch lists of nx2_perf.h and the test's
expected-off list (all kept in order, static_asserts renumbered), plus two real ones between the probe and
bank_dirty_skip: `Nx2Cow::HostWrite` (kept the probe's `src` argument and added bank_dirty_skip's
`Nx2BankDirty::HostWrite` call) and `MemoryManager::Write<T>` (calls `Nx2BankDirty::HostWrite`, then
`HostWriteSlow(..., &data, NX2_RETURN_ADDRESS())` only when the cow probe is on, so the probe keeps its who2 site), and
the test includes (memory_manager.h + nx2_bank_dirty.h). Built yuzu-cmd yuzu tests: `[nx2_perf]` 63 cases,
`[video_core]` 140, `[split_writes]` 19, `[native_macro]` 2, all pass. Worktrees a42-102..105 and branches removed.

## Verify runs
Split front register checks ~19-20k, 0 differences in every -Quick run. No DEVICE_LOST anywhere this round. Only the
known verify noise lanes logged errors (front same-first control 3: 64, guest_landed_syncpoints ~160, preflush_aging
~33), identical across runs.
- **r13-v-bank_dirty_skip**: 0 'page changed with no mark'. Per second at the spawn: compares avoided ~180-280k, done
  ~50-420k, traps ~1.1-1.3k, tracker marks 5-29k, hook marks 0, re-arms 5-62k, failed arms 0.
- **r13-v-back_replay_lean**: 0 'Nx2Perf method:', past target 0, verify diffs 0. Under verify ~3.3M entries/s go
  through BackRun in runs of ~6 entries (non-verify speed runs: ~4.6-4.8M entries/s, ~12 entries/run; fallbacks
  engine ~710-740k/s, kind ~240k/s, flags ~35-40k/s).
- **r13-v-front_macro_call**: 'front_macro_call differs' 0 in total (~420-500k calls checked/s), dma_forward
  1.84M checked 0 differed. Speed runs: ~460-700k calls/s taken, ~600-835k/s fall back for 'mode' (Increasing/Inline
  headers to macro methods), ~1.1-1.7k split across entries.
- **r13-v-run_ahead_own_write_probe** (full run with the walk, ON=frames_ahead,run_ahead_ledger,run_ahead_cow_probe,
  run_ahead_snapshot_probe,run_ahead_overlay_budget,run_ahead_own_write_probe, PERF_VERIFY=1): no device loss,
  numbers below.

## Speed (interleaved, fast boot, -GameplayWait 100, fps_windows.py)
| runs | per-run median (min) | pooled median / min |
|---|---|---|
| r13-ctl-a / -b (defaults, HEAD 09c66d24d8) | 40.4 (39.9) / 40.0 (39.7) | **40.3 / 39.7** |
| r13-s-bank_dirty_skip-a / -b | 38.8 (38.1) / 39.7 (38.9) | 39.5 / 38.1 |
| r13-s-back_replay_lean-a / -b | 40.1 (39.8) / 40.1 (39.7) | 40.1 / 39.7 |
| r13-s-front_macro_call-a / -b | 39.7 (39.3) / 40.3 (39.8) | 40.2 / 39.3 |
vi2 fence wait at the spawn: controls 14.6-15.1 ms, bank_dirty_skip 15.5-15.7, back_replay_lean 15.0-15.2,
front_macro_call 15.1-15.3. Fog 0.492-0.505 in all eight. No flash runs for the rejected switches (rejected on speed).
bank_dirty_skip without verify at the spawn: avoided ~0-6k/s, done ~86-95k/s, tracker marks ~36-42k/s, re-arms
~72-86k/s: the tracker marks nearly every bank page every frame, so it only adds trap/re-arm cost.

## Confirmation (defaults; nothing accepted, so the controls ran the confirmation binary)
Speed: r13-ctl-a 40.4 / r13-ctl-b 40.0, **pooled 40.3 median / 39.7 slowest**. Same as the round baseline 41.0 / 39.5
within noise (-0.7 median, +0.2 slowest), not better. Fog 0.500 / 0.504; game.png (r13-ctl-b) correct: Kokiri spawn,
fog, light shafts. Flash r13-confirm-flash (fog 0.489):
```
flash luma: n=40 median=0.4340 min=0.4204 max=0.4434 stdev=0.0060 outliers=0
flash cg: n=40 median=0.4266 min=0.4145 max=0.4416 stdev=0.0065 outliers=0
```

## run_ahead_own_write_probe numbers (per 600-epoch window; spawn windows 73 / 104 / 131 s, after the walk 146 / 162 s)
- **Decision PASS in every window.** resid_guest **0 B**, unarmed any_guest **0**, unarmed_guest_outside_copy **0**,
  who2_nonown writes 0, front_nonown_bytes 0, own_verify_differences **0** (own_checked 4.0-5.1M per window; unchecked
  0 at the spawn, 382k in the boot window).
- front_macro residual 11.0-11.8k B per window: **resid_own 11.0-11.8k, resid_unknown 0-616** (unknown from range
  overflow), sum mismatch 0.
- unarmed_changed 5.2-6.6k pages per window: **all_own 100%**, any_guest 0, unknown 0, sum mismatch 0.
- GPU-own writes per window ~11-18M (616-757 MB), pointer writes 0.9-2.8M; range_overflow grows (~3-6M per window: pages
  with more than 8 disjoint own ranges), table_overflow 416 (boot only).
- The overlay budget rules in the same run: copy_set PASS (unprot union p99 2.0-3.0k pages, copy_ms p50 0.28-0.33),
  arm_trap FAIL and front_macro_residual FAIL only because they count these GPU-own changes as hazards; span PASS.

## What the round taught (critical path)
1. **The two run-ahead holes from round 12 are the emulated GPU's own writes, not the guest.** Every front_macro
   residual byte and every unarmed_changed page in gameplay is inside a byte range the fencing thread, the split back,
   query stores or downloads wrote; no guest write escapes the traps plus the reply-time copy set. So a reply-time
   overlay (copy set ~2-3k pages, 0.3 ms) plus the cow traps covers every guest write the back reads. The remaining
   design rule: the overlay must not hide GPU-own writes. Own writes after the reply must go through to the overlay
   (write-through, or drop the overlay page) so the back and the front's macro DMA reads see them in GPU order. With
   that, the next step is a real run-ahead switch (frames_ahead + overlay + own-write forwarding), which must pass the
   flash check with 0 outliers. That is the ~+9 FPS lever.
2. **Back replay cost is not on the critical path.** back_replay_lean moved ~4.7M entries/s into a tight loop and
   gained nothing (-0.2); dma_forward_tape (round 11) said the same. The back is not what the front waits on in
   aggregate; it is specific producers (list writers, acquire releases).
3. **The front's per-call macro dispatch is not the cost either.** front_macro_call took ~0.5-0.7M macro calls/s
   straight to FrontRunMacro and gained nothing (-0.1). With round 10's macro memo and round 12's same-value/dead-store
   results, the front's own CPU work is not reducible by small shortcuts.
4. **The bank compare is cheap and the bank pages change every frame.** bank_dirty_skip's tracker marks hit nearly
   every bank page each frame, so it avoided almost no compares and paid ~80k re-arms/s (-0.8). Same verdict as
   bank_hash_shadow (round 9).
5. Unchanged: the spawn fence wait is ~14.6-15.1 ms, and the front's waits on the back (list writers ~9.5, acquire
   triggers ~6.0 ms/frame in the round 13 base profile) remain the critical path. With small front/back shortcuts
   exhausted, run-ahead (point 1) is the lever to pursue.
