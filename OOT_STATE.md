# Ocarina of Time on Eglantine: state of this branch

This branch is a work in progress toward running **The Legend of Zelda: Ocarina of Time**
(`0400486024DEC000`) at a locked 60 FPS. It is shared so others can test it and read the code.
It is not a pull request: it is ahead of upstream `nx2` by a few hundred commits, including work
picked from other forks.

## Where it stands

Measured on a Ryzen 9 9950X3D and an RTX 5090, Windows 11:

| Scene | FPS |
|---|---|
| Kokiri Forest, at the spawn point | about 37 to 39 |
| Kokiri Forest, after walking around | 43 to 45 |

- **No screen flashes.** They came from letting the game run a frame ahead of the GPU emulation:
  the game rewrote buffers that hadn't been drawn yet. That mode is off by default.
- **Volumetric fog and light shafts** render as on the console. A ported "opt-in" workaround was
  on by default and scaled the fog down to a quarter; it is now really opt-in
  (`EDEN_NX2_OOT_VISUAL_WORKAROUND=1`).
- **Renders correctly** in the scenes we test, with no GPU device loss in our automated runs.
- **Normal game speed below 60 FPS.** A built-in patch makes the game use the real frame time, so
  it doesn't run in slow motion.
- **Full resolution.** A built-in patch turns off the game's dynamic resolution. You can disable
  either patch in the game's Properties, Add-ons.

## What this branch promises

- **Every speed change can be turned off.** Each one is a switch in
  `src/video_core/nx2_perf.h`. A change becomes default on only after it passes:
  - a verify run that checks it against the old code path,
  - a screen-flash check,
  - repeated speed measurements.
- **Switches you can set at launch (environment variables):**
  - `EDEN_NX2_PERF_OFF=name1,name2` turns single switches off. `EDEN_NX2_PERF_ON=...` turns
    experimental ones on.
  - `EDEN_NX2_PERF_SAFE=1` turns off the most recent default-on switches.
  - `EDEN_NX2_PERF_OPT=0` turns every switch off.
  - `EDEN_NX2_PRESENT_QUEUE=0` restores the old blocking frame presentation.
  - `EDEN_NX2_PERF_VERIFY=1` runs the old and new paths side by side and logs any difference.
- **The log shows frame times every second:** the `Nx2Frame:` lines in
  `%APPDATA%\eglantine\log\eden_log.txt`.

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

Each frame takes about 27 ms:

- the game's own CPU work takes about 10 ms;
- the game then waits about 12 ms for our GPU emulation to finish that same frame.

The game's work and the GPU emulation take turns instead of overlapping. Letting them overlap is
worth about 9 FPS, but it caused the flashes, because the game rewrites data the GPU side still
needs. The next work:

- make that overlap safe, by holding back the game's write until the GPU side is done with the data;
- make the GPU emulation thread cheaper: it is close to the 16.7 ms budget a 60 FPS frame allows.

## Reporting problems

Please include:

- your CPU and GPU;
- the game and where in the game;
- what you saw;
- `%APPDATA%\eglantine\log\eden_log.txt` from that session.
