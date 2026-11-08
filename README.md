<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200">
  <br>
  <b>Eglantine</b>, the A-Man-With-A-Plan fork
</h1>

<p align="center">
  <a href="https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases"><b>Download</b></a> |
  <a href="#why-switch">Why switch</a> |
  <a href="#switch-in-three-steps">Switch in three steps</a> |
  <a href="#for-developers">For developers</a> |
  <a href="https://github.com/A-Man-With-A-Plan-dev/Eglantine/issues">Report a problem</a>
</p>

Eglantine is a free and open-source Switch 2 emulator, derived from Eden, yuzu and Sudachi and
started by Camille LaVey. This fork is **upstream Eglantine plus several hundred fixes and speed
improvements**, and it keeps merging upstream. We think it's the better place to play and to
develop, and we suggest using it as your main Eglantine.

## Why switch

**1. You lose nothing.** This fork contains all of upstream Eglantine's `nx2-experimental`,
merged through `797461a8b6` (2026-11-08). New upstream work keeps getting merged in. On top of
that there are 411 more commits.

**2. Games run better.** In the maintainer's testing, every game tried ran better than on upstream
`nx2-experimental`. Some of the fixes here that upstream doesn't have:

| Fix | Effect | Author |
|---|---|---|
| Spread shader building over frames | Worst freeze entering a new area with an empty shader cache: 2.3 s → 0.6 s | this fork |
| Real-time game speed | Ocarina no longer runs in slow motion below 60 FPS | gunvalk |
| Trap-and-flush native CPU reads | Ocarina, Kokiri Forest: 25.97 → 30.75 FPS | nyakuoff |
| Wake the GPU thread on submit | Ocarina: about +1.5 FPS | this fork |
| VRAM budget on large cards | Mario Kart World: 53.0 → 58.7 FPS | win |
| vi2 returns the release fence | Removes Wonder's 30 FPS lock | Alleexxi |
| Turing macro RAM and `ClearCode` | Fixes Ocarina's "Native MME failed" errors and flicker (upstream PR #663, still open) | Brandon Bunnelle |
| Deferred clears before image writes | Fixes Ocarina's terrain blotches | Gavin Schiller |
| Table-pointer pools, unwritten render targets | Fixes Ocarina's haze | jpkto |

Ocarina of Time (`0400486024DEC000`) gets the most attention. It plays without crashes or flashes
in our runs, with correct fog, at about 39-41 FPS at the Kokiri Forest spawn point on an RTX 5090.
Details: [OOT_STATE.md](OOT_STATE.md). Many more ports are listed in
[UPSTREAM_CANDIDATES.md](UPSTREAM_CANDIDATES.md).

**3. Every speed change has an off switch.** Each one is a named switch in
`src/video_core/nx2_perf.h`. If a game misbehaves, you can turn changes off one at a time, or all of
them at once with `EDEN_NX2_PERF_OPT=0`, without going back to another build. Changes only become
default-on after:
- a verify run against the old code path,
- a screen-flash check,
- repeated speed measurements.

**4. Measured, not guessed.**
- The log shows frame times every second, and says which shader caused a stutter and how long it
  took.
- Automated lab runs (`tools/nx2/lab/`) repeat the same measurement on every change.
- Every result and decision is written down, in [OOT_STATE.md](OOT_STATE.md) and
  [OOT_WORKFLOW.md](OOT_WORKFLOW.md).

**5. Work from many forks, in one place.** Fixes that were scattered across other forks and pull
requests are ported, conflict-resolved and tested together here, with every author credited in
[CREDITS_OOT.md](CREDITS_OOT.md).

**6. It gives back.** This is not a hostile split. Changes worth having upstream are listed in
[UPSTREAM_CANDIDATES.md](UPSTREAM_CANDIDATES.md) and sent there as small pull requests, credited
to their original authors.

**7. Switching takes a minute and is reversible.** The fork uses the same user folder as upstream,
so your keys, firmware, saves and settings carry over.

## Before you switch

Please be aware of the following:

- **Changes land here first.** They go in once they work in game and are measured, often before
  anyone else has reviewed the code. Much of it was written with AI tools (Claude, Gemini) and is
  marked in each commit's `Assisted-by:` line. Expect rough edges.
- **Tested mostly on one PC** (Ryzen 9 9950X3D, RTX 5090, Windows 11). Slower machines will see
  lower numbers. Reports from other hardware are very welcome.
- **Windows is what we build and test.** Linux and macOS builds come from the same CI as upstream's,
  but get less testing.
- **Ocarina of Time is not at 60 FPS yet.** It's the goal; [OOT_STATE.md](OOT_STATE.md) explains
  what limits it.

## Switch in three steps

1. Download the latest build from
   **[Releases](https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases)** and unzip it into
   its own folder. Don't unzip it over your upstream Eglantine folder.
2. Back up your saves (`%APPDATA%\eglantine\nand\user\save` on Windows).
3. Run `eglantine.exe`. It uses the same `%APPDATA%\eglantine` folder as upstream, so your keys,
   firmware, games list, saves, mods and settings are already there.

The first time you play each game, its shader cache is rebuilt once, so expect some stutter on the
first visit to each area.

**Going back:** run upstream's `eglantine.exe`. Each switch between the two builds rebuilds the
shader cache of the game you play, because they store it in different formats.

**Keeping both side by side:** make one of them portable.
1. Create an empty folder named `user` next to its `eglantine.exe`.
2. Copy `keys`, `nand`, `load` and `sdmc` into it from `%APPDATA%\eglantine`.
3. Set up the settings again.

That build then keeps its own settings, saves and cache in `user`. Don't copy `config`: it can
still point at the folders in `%APPDATA%\eglantine`.

## For developers

**Get the code:**

```
git clone --recursive https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
```

Or add the fork to an existing upstream clone:

```
git remote add fork https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
git fetch fork
git switch -c fork-nx2 --track fork/nx2
git submodule update --init --recursive
```

Build steps are the same as upstream's: [General Build Guide](docs/Build.md).

**Contribute:** open pull requests against `nx2` here. Your work then reaches everyone who uses
this fork right away. Where it fits upstream, it can still be sent there as a separate PR.
- [OOT_CONTRIBUTING.md](OOT_CONTRIBUTING.md): build, measure, send a change.
- [OOT_HELP_WANTED.md](OOT_HELP_WANTED.md): open tasks, easiest first.
- [SHADER_STUTTER_NOTES.md](SHADER_STUTTER_NOTES.md): what we know about shader and pipeline
  stutter.
- [NX2_NOTES.md](NX2_NOTES.md): notes on the Switch 2 GPU emulation.
- [AI policy](docs/policies/AI.md) and [coding policy](docs/policies/Coding.md): both apply here as
  upstream.

## Reporting problems

Open an [issue on this fork](https://github.com/A-Man-With-A-Plan-dev/Eglantine/issues), not on
upstream. Please include:

- your CPU and GPU;
- the game, and where in the game;
- what you saw;
- `%APPDATA%\eglantine\log\eden_log.txt` from that session.

If a game runs worse here than on upstream, also try launching it with `EDEN_NX2_PERF_OPT=0`. If
that fixes it, one of the switches is the cause, and the report tells us which game to check.

## Credits and license

This fork stands on the work of everyone behind Eglantine, Eden, yuzu and Sudachi, and of the
contributors listed in [CREDITS_OOT.md](CREDITS_OOT.md). For community discussion, see the
[Eglantine Discord](https://discord.gg/XkS5NVPTUY).

Eglantine is licensed under the GPLv3 (or any later version). See [LICENSE.txt](LICENSE.txt).
