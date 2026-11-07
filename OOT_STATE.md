# Ocarina of Time on Eglantine: state of this branch

This branch is a work in progress toward running **The Legend of Zelda: Ocarina of Time**
(`0400486024DEC000`) at a locked 60 FPS. It is shared so others can test it and read the code.
It is not a pull request: it is ahead of upstream `nx2` by a few hundred commits, including work
picked from other forks.

Last updated: 2026-11-07.

## Where it stands

Measured on a Ryzen 9 9950X3D and an RTX 5090, Windows 11:

| Scene | FPS |
|---|---|
| Kokiri Forest, at the spawn point | about 39 to 41 |
| Kokiri Forest, after walking around | mid 40s to low 50s |

- **No screen flashes.** They came from letting the game run a frame ahead of the GPU emulation:
  the game rewrote buffers that hadn't been drawn yet. That mode is off by default.
- **Volumetric fog and light shafts** render as on the console. A ported "opt-in" workaround was
  on by default and scaled the fog down to a quarter; it is now really opt-in
  (`EDEN_NX2_OOT_VISUAL_WORKAROUND=1`).
- **Save thumbnails are fixed.** The garbled previews came from a CPU-emulation bug, not the GPU.
  Slots saved before this fix keep their old broken preview until you save over them.
- **Renders correctly** in the scenes we test, with no GPU device loss in our automated runs.
- **Normal game speed below 60 FPS.** A built-in patch makes the game use the real frame time, so
  it doesn't run in slow motion.
- **Full resolution.** A built-in patch turns off the game's dynamic resolution. You can disable
  either patch in the game's Properties, Add-ons.

## Shader compilation stutter (new, being worked on)

The first time you visit an area, new shaders have to be built. What changed today:

- **Shader building is spread over frames.** Before, entering a new area with an empty shader
  cache built ~1,000 shaders at once on the GPU thread and froze the game for up to 2.3 s. Now each
  frame builds for at most 24 ms. Draws whose shaders aren't ready yet are skipped for a few frames.
  - **Side effect:** while that happens, some objects appear late or as bright white silhouettes
    for a second or two.
  - Change the per-frame budget with `EDEN_NX2_SHADER_BUDGET_MS=<ms>`: lower means smoother frames
    and longer pop-in, higher means the reverse.
  - It only applies with asynchronous shader compilation on, which is the default.
- **Freezes of 1 to 6 seconds can still happen** when the graphics driver compiles a large shader
  for the first time. They are worst right after an update that changes the generated shaders,
  because the driver's own cache misses then. A second visit to the same place is fast.
- **Several further fixes landed today and are ON by default, but have NOT yet been through the
  verify, flash and speed checks below:**
  - `compute_rebuild_async`: keep the old compute shader while its rebuild compiles;
  - `compute_preload_variants`: build every saved compute variant at boot;
  - `stale_target_skip`: skip, instead of wait for, small post-processing draws whose target still
    holds last frame's image;
  - `geometry_consistency`: also skip an object's depth draw while its material draw is skipped;
  - `build_order_priority`: build the most-used pending shaders first;
  - `replay_disk_recordings`: build shaders from the disk cache on background threads.

  If you see something wrong (stale effects, missing objects, flicker, flashes, crashes), check
  whether it goes away with:

  ```
  EDEN_NX2_PERF_OFF=compute_rebuild_async,compute_preload_variants,stale_target_skip,geometry_consistency,build_order_priority,replay_disk_recordings
  ```

  and report what you saw.

## What this branch promises

- **Every speed change can be turned off.** Each one is a switch in
  `src/video_core/nx2_perf.h`. Normally a change becomes default on only after it passes:
  - a verify run that checks it against the old code path,
  - a screen-flash check,
  - repeated speed measurements.

  The shader-stutter switches listed above are the exception: they are on before those checks,
  because the stutter was the most-reported problem.
- **Switches you can set at launch (environment variables):**
  - `EDEN_NX2_PERF_OFF=name1,name2` turns single switches off. `EDEN_NX2_PERF_ON=...` turns
    experimental ones on.
  - `EDEN_NX2_PERF_SAFE=1` turns off the most recent default-on switches.
  - `EDEN_NX2_PERF_OPT=0` turns every switch off.
  - `EDEN_NX2_PRESENT_QUEUE=0` restores the old blocking frame presentation.
  - `EDEN_NX2_PERF_VERIFY=1` runs the old and new paths side by side and logs any difference.
- **The log shows frame times every second:** the `Nx2Frame:` lines in
  `%APPDATA%\eglantine\log\eden_log.txt`. Lines starting "Submission thread waited" and
  "Runtime pipelines in the last 10 s" show what each shader stutter cost.

## What it does not promise

- **Not 60 FPS yet.**
- **Tested on one machine.** Slower CPUs will likely see lower numbers. The CPU pinning for X3D
  and hybrid CPUs is on by default, and its gain has not been measured.
- **Tested on Ocarina only.** The changes are not specific to Ocarina, but other games, Mario Kart
  World included, have not been rechecked. If another game got worse, launch it with
  `EDEN_NX2_PERF_OPT=0` and `EDEN_NX2_PRESENT_QUEUE=0`. If that fixes it, one of the switches
  is the cause: please report which game and what you saw.
- **Not ready for upstream.** It mixes our work with commits picked from other forks. Pieces will
  go upstream as separate, smaller changes.
- **Experimental switches are off by default for a reason.** Turning them on can bring back
  flashes or crashes.

## Why it is hard to reach 60

Each frame takes about 25 ms. About 15 ms of that is the game waiting for its frame's GPU work to
be accepted. Most of that wait is our **GPU command thread** (the "front" thread). It reads the
game's GPU commands and runs its GPU macros, and it hasn't finished reading the frame yet. The
host graphics card is not the limit: rendering at full resolution costs nothing measurable. So:

- **Upscalers such as DLSS or FSR do not raise the frame rate here.** The GPU isn't what's slow.
- **Frame generation can make motion look smoother on screen,** but the game itself still runs at
  ~40 FPS and input latency goes up.
- **The real levers:**
  - make the front thread cheaper: command parsing, macros, and its waits for the second GPU thread;
  - safely let the game prepare the next frame while the GPU side still draws this one. That is
    worth about 9 FPS, but the first attempt caused the flashes.

## For developers

- `UPSTREAM_CANDIDATES.md`: which commits here are worth sending to upstream Eglantine, in what
  order, and which belong to other authors.
- `NX2_NOTES.md`: notes on the Switch 2 GPU emulation.
- `.agent/STATUS.md`: dated log of every measurement and decision, newest first.
- `.agent/prompts/overnight/round*-base.md` and `round*-integrate.md`: per-round profiles, A/B
  results and rejected ideas from the optimization rounds.
- `.agent/plans/shader-freeze-plan.md`: the plan behind today's shader-stutter work, with file and
  line references and the measurements it rests on.
- `.agent/plan-review/`: design reviews between two AI agents. They cover the save-thumbnail fix,
  the speed quick wins, and DLSS / frame generation. The DLSS review is still open.
- `tools/nx2/lab/` (see its `README.md`) and `OOT_CONTRIBUTING.md`: how to run the same automated
  Ocarina measurements yourself (Windows, PowerShell).

AI tools (Claude, Gemini) were used heavily on this branch. Commits say so in an `Assisted-by:`
line, as the repo's AI policy (`docs/policies/AI.md`) requires. Some older commits still carry a
`Co-Authored-By` line instead.

## Reporting problems

Please include:

- your CPU and GPU;
- the game and where in the game;
- what you saw;
- `%APPDATA%\eglantine\log\eden_log.txt` from that session.
