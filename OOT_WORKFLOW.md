# How this state was reached

Ocarina of Time went from "crashes 2 s after loading a save" to "plays at 40-60 FPS without
crashing" on 2026-11-05. Most of the gain came from the way the work was done, not from any
single fix. This page is that method, so the next person or agent can repeat it.

## The loop

1. **Find out what's actually wrong before writing code.** Every run's log was read with a small
   summary script: frame rate per 10 s window after the save loads, stalls, crashes. The early
   "60 FPS" readings were boot and menu frames; gameplay had never been measured because every
   build crashed first.
2. **Bisect against a known-good build.** Yesterday's build didn't crash, today's did, so the
   cause was in today's commits. Vulkan validation layers then pointed at descriptor offsets far
   past their buffers, which led to the real bug (buffer ids left stale in other GPU channels).
3. **Profile, then fix the biggest cost.** The GPU-thread profiler (`EDEN_NX2_GPU_PROFILE=1`)
   showed where each millisecond went. Two cases: CPU reads of GPU memory stalled the emulator
   55 times a second (fixed by preflushing, 37 -> 52 FPS), and the front GPU thread waited for
   the back on every macro memory read.
4. **One change per lab run.** An automated run boots the game hidden, loads the save, walks in
   Kokiri Forest, and reports the median FPS of the gameplay windows, a screenshot, and any
   device loss. A change stays only if it wins and renders correctly. Run-to-run noise is about
   +-3 FPS, so a single good window means nothing.
5. **Verify every shortcut against the safe path.** Before skipping a wait, run with
   `EDEN_NX2_PERF_VERIFY=1`: the emulator takes the shortcut, then the safe path, and logs every
   difference. The first version of the early macro read returned 7 stale values in 3.5 million;
   the fixed version returned 0. Only then was it turned on.
6. **Measure what correctness costs, and pay it.** Marking occlusion-report pages so reads wait
   for real results cost 4 FPS. An A/B run proved that those 4 FPS had come from reading stale
   data, so the correct version stayed.
7. **"Done" means the game survived.** A green build or passing tests isn't done. A crash fix is
   done when the scene that used to crash runs through. A speed change is done when the median
   moved and the screenshot is right.
8. **Write everything down.** One status file holds the goal, each run's numbers, what was tried
   and dropped, and why. Every new session reads it first, so no one re-measures what's known or
   restarts the wrong work.

## What didn't work

- Several agents fanned out on every task: many builds and spawned agents, little progress.
  One agent, one variable per run, worked.
- Optimizing what looked slow instead of what the profile showed. Most of the day's perf commits
  didn't move the gameplay frame rate, because the crash hid it.
- Trusting "fixed" commits without a game run: one was labelled a device-lost fix and still
  crashed 30 s in.

## Tools used

- Log summary: frame rate windows from `PresentClientBuffer` lines in `eden_log.txt`.
- Headless lab: `eden-cli` with a hidden window, driven by `tools/nx2/lab_ctl.py`
  (launch, button presses, screenshot, stop). The run script and its stall watchdog are local
  helpers, not in git.
- Switches: `EDEN_NX2_GPU_PROFILE=1` (profiler), `EDEN_NX2_PERF_VERIFY=1` (shortcut check),
  `EDEN_NX2_PERF_OFF=<name>` (turn one optimization off), `EDEN_NX2_PREFLUSH=0`.
