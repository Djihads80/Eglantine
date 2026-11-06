# Round 6 integration (2026-11-06, resumed after the user's pause)

HEAD before this summary: `553b6c2fbe` (merges fb31567b63 and earlier, plus docs). Build from 15:22
(after the last merge; later commits are docs/agent files only). Unit tests [nx2_perf] [video_core]
[split_writes] [native_macro] pass.

Round baseline (defaults, r6-base-a/b): pooled median 36.8 FPS, slowest 35.4. Flash baseline
(r6-base-flash): luma sd 0.0020, 0 outliers; cg sd 0.0025, 0 outliers.
Pooled = the 7 600-present windows from ~145 s of each run (spawn spot, then the first after-walk window).

## Merged (all default off, all stay off)
inflight_write_wait + inflight_read_exact, front_sync_trio, fence_timeline, dma_forward_memo.

## Results
| switch | verify | flash | speed a / b | pooled median / slowest | decision |
|---|---|---|---|---|---|
| inflight_write_wait + inflight_read_exact (with frames_ahead, war_guard_probe) | **fails**: at gameplay every epoch escapes (61/61 per s), bank pages changed before the fence ~20k, reads differ 711 of ~1.8k compared, 82 write give-ups | not run | not run | - | REJECTED (real mismatches) |
| front_sync_trio | clean (acquire_trigger_early 0 met-early errors, exact_noted_writes 0 differed, macro_read_writer_set 0 differed) | luma sd 0.0021, 0 outliers | ~36.4 / ~36.3 | 36.4 / 34.3 | REJECTED (-0.4) |
| dma_forward_memo | clean (0 plan differs; dma_forward 1.64M calls, 0 differed) | not needed (no timing change) | 37.6 / 35.6 | 36.6 / 32.7 | REJECTED (-0.2; hit rate only 3-4%) |
| fence_timeline | clean (0 unmatched / misordered stamps) | - | - | - | probe only, stays off |

dma_forward (pending in the brief) was already accepted in round 5; nothing left to evaluate.

## fence_timeline reading (r6-v-fence_timeline, r6-maxres-on-b, spawn spot)
Per 600 presents, ms p50: entered->front_parse 16-22, front_parse->back_incr 0.007, back_incr->waited
0.006, last_submit_pop->entered 0.6-0.8, back_idle_in_wait 3.4-4.8.
So the guest's fence wait is almost entirely the **front** not yet having parsed this frame's fence
increment; once the front parses it the back increments within ~7 us and the guest wakes within ~6 us.
The back idles only ~4 ms of the ~16-22 ms wait. The front is the critical path (it waits on the back
at list-writer and acquire sites, per r6-base-prof), not the host GPU and not the back's replay.

## Max Resolution patch measurement (brief "Current state")
The lab config (`.agent/lab/qt-copy.ini`) already DISABLES "Max Resolution (built-in)" for OoT, so all
lab numbers so far are without it. One run with it enabled (r6-maxres-on-b, fence_timeline on):
37.2 37.7 37.5 37.3 37.4 34.6 41.4 (median 37.4, min 34.6) vs base 36.8 / 35.4: no measurable cost at
the spawn. fence_timeline shows the same front-bound shape. The first attempt (r6-maxres-on-a) hung at
boot (~3.9 s, log stops while channels are created; stall dumps in its run dir): a second sighting of
the intermittent boot hang seen in round 3 (r3-v-writer_set_exec), here with only fence_timeline on.

## Confirmation (defaults unchanged)
r6-confirm-a: 37.0 37.6 37.8 37.7 36.8 36.5 44.2; r6-confirm-b: 35.7 36.6 36.7 36.6 36.8 34.6 41.4.
Pooled median 36.8, slowest 34.6. No DEVICE_LOST (emulator.log and eden_log.txt). game.png correct
(Kokiri village). r6-confirm-flash: luma sd 0.0016, 0 outliers; cg sd 0.0031, 0 outliers.

## Next
- The inflight epoch approach needs its gaps closed before another measurement: the write-side escape
  fires every frame at gameplay (channels waiting on each other, as the implementer feared), so the
  guest writes bank pages before the back passed the fence. It needs a no-progress escape condition
  and coverage of pages registered later in frame N.
- Front critical path: entered->front_parse is the fence wait. Work that makes the front reach the
  frame's fence increment earlier (fewer list-writer and acquire drains, cheaper push) is what moves FPS.
- Intermittent boot hang at ~3.6-3.9 s (compute channel creation): worth a stack read of
  r6-maxres-on-a/stall-1.dmp.
