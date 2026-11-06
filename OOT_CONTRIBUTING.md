# Contributing to the Ocarina of Time harvest fork

This fork is **result first**: changes land once they work in game, before review, so the main
Eglantine project can see what helps and pull it through its own review. That makes measuring
the part that matters. This page is everything you need to build, test and send a change.
For the project's general rules, see [CONTRIBUTING.md](CONTRIBUTING.md) and the
[AI policy](docs/policies/AI.md).

## Not a coder? You can still help

- **Test.** Download the [latest release](../../releases), play, and open an issue with the
  **Ocarina of Time test report** form. Other areas than Kokiri Forest and AMD or Intel GPUs are
  the most useful.
- **Review.** Most code here is AI-assisted and not peer-reviewed. Reading a commit and pointing
  out a mistake in an issue is a real contribution.

## Build (Windows)

Needs Visual Studio 2022 or newer Build Tools (C++ workload), CMake, Ninja and Git. Full details
and other platforms: [docs/Build.md](docs/Build.md).

```
git clone https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
cd Eglantine
git checkout stable/oot-playable-no-crash-2026-11-05
. .\tools\windows\load-msvc-env.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=ON -DENABLE_WERROR=OFF
cmake --build build --target yuzu yuzu-cmd tests
```

That gives `build\bin\eglantine.exe` (the app), `build\bin\eden-cli.exe` (what the lab runs) and
`build\bin\tests.exe`. The first build takes a while; later ones rebuild only what changed.

The stable branch still runs `frames_ahead`, the cause of the brightness flashes. Performance work
should start from `local-combined` (2026-11-06 and later: flash fix, `Nx2Perf` switches,
`Nx2Frame` lines); see [OOT_STABLE.md](OOT_STABLE.md).

## Measure before and after

Set up the lab once and run it before and after your change: [tools/nx2/lab/README.md](tools/nx2/lab/README.md).
In short:

```
python -I tools\nx2\lab\make_lab_config.py
$env:EGLANTINE_OOT_GAME = "D:\Games\Ocarina of Time.nup"
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name before
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name after
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\before
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\after
```

Unit tests, which take seconds (`[nx2_perf]` and `[nx2_frame]` exist from 2026-11-06 on):

```
.\build\bin\tests.exe "[video_core]"
.\build\bin\tests.exe "[nx2_perf],[nx2_frame]"
```

## What counts as done

- **A crash fix:** the place that crashed before now runs through, in a lab run or in play.
- **A speed change:** the median FPS of the spawn windows (and of the walk windows) moved in two
  or three runs, and the screenshot still renders correctly. One good 10-second window proves
  nothing, and neither does a run marked `INVALID` (it never left the title screen).
- **A change that touches frame timing** (presents, fences, running ahead): also a `-Flash 30`
  run with `outliers=0`. Two such changes saved ~5 and ~3.5 ms per frame and made the screen
  flash.
- **A shortcut that skips a wait:** a `-Quick` run with `-Env EDEN_NX2_PERF_VERIFY=1` shows zero
  `Nx2Perf split:` lines first. Stale reads lose the GPU device.
- **New optimizations** go behind an `Nx2Perf` switch (`src/video_core/nx2_perf.h`), so anyone
  can turn them off with `EDEN_NX2_PERF_OFF=<name>`.
- **Always:** the unit tests pass, and no `DEVICE_LOST` in either log.

A green build alone is not done. [OOT_WORKFLOW.md](OOT_WORKFLOW.md) explains why.

## Sending it

1. Fork this repo, branch off `stable/oot-playable-no-crash-2026-11-05`, one topic per commit.
2. If AI helped, add a trailer such as `Assisted-by: <model> (<what it did>)` to the commit
   message, and no `Co-Authored-By` or `Signed-off-by` from the AI.
3. Open a pull request against the stable branch. Put the before/after medians, your GPU, and
   what you did not test in the description.

Not sure where to start? [OOT_HELP_WANTED.md](OOT_HELP_WANTED.md) lists open tasks, easiest first.
