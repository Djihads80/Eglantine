---
name: nx2-lab-automation
description: "Use when driving an Eglantine NX2 game run yourself."
version: 1.0.0
author: kraibse
license: MIT
metadata:
  hermes:
    tags: [eglantine, eden, switch2, automation, headless, eden-cli, playtest]
    related_skills: [nx2-shader-triage, switch-emulator-diagnostics]
---

# Driving Eglantine NX2 runs unattended

Boot a Switch 2 title hidden, press buttons, capture and measure frames, and read the diagnostics,
all without the user. Built on `eden-cli` (SDL frontend) and the shader lab's diagnostics folder.

## When to Use

- Reproducing a visual bug, crash or hang instead of asking the user to "launch it again".
- Checking a fix in game, A/B-ing environment switches (`EDEN_NX2_TENSOR`, `EDEN_NX2_DUMP`, ...).
- Any "test and control it yourself" request on an NX2 title.
- Not for lowering rejections alone: `nx2-shader-triage` handles those offline from dumps.

## Rules

- **Always bound the run.** `launch` takes `--max-seconds` (default 900; use ~200-420), and every
  command chain ends with `lab_ctl stop`. A forgotten hidden emulator holds the user's GPU and
  they'll have to force-kill it.
- **Keep runs short.** The user's own checks take 1-2 minutes; boot-to-save (~130 s with
  `scripts/to_gameplay.sh`) plus about 1 minute of gameplay is enough for a fix check. Don't run
  4-5 minute walks or sleep through long fixed waits. Ask before a long series of runs if the user
  may be at the PC.
- **Use the user's graphics settings, your key bindings.** `eden-cli` reads `sdl2-config.ini`, whose
  graphics settings differ from the Qt app's (async shaders, accuracy, docked). Make a scratch copy
  of `~/.config/eden/qt-config.ini`, merge in the SDL file's `player_0_button_*`, `lstick`, `rstick`
  and `motion` lines (the Qt ones are usually a gamepad, so keys do nothing), and pass
  `launch --config`.
- **Never `cd` into dumps or run scripts from them.** Pass paths as arguments, and run Python
  with `-I`.
- Check `pgrep -af 'eden-cli|eglantine'` before launching: only one instance should own the GPU.
  When the user says they're testing manually, `lab_ctl stop` the lab instance at once.
- **Lab-run every port before the user gets the build**, at least boot-to-save plus a walk.
  Sync and fence-timing changes from forks can deadlock in this tree's GPU split even when they
  work in the fork; the user should never be the first to boot an untested change.
- **A "capped at 30 fps" report is often `gpu_fence_behavior=Balanced` (value 2) in the user's
  `qt-config.ini`, not a code regression.** Balanced costs ~11 ms of front-thread time here:
  same 50.4 ms GPU-bound lab frame, front 16.7 ms with Default (fence 33.8 ms) against 28.2 ms with
  Balanced (fence 20–24 ms), which is exactly what turns a ~17 ms frame into two-vblank 33 ms
  frames. Check the setting before bisecting commits; the lab is GPU-bound, so measure the front as
  `frame - fence - pacing` rather than by frame time.

## Tooling (repo `tools/nx2/lab_ctl.py`, branch `scratch/oot-shader-lab`)

```bash
L="python3 -I tools/nx2/lab_ctl.py -d $D"       # $D: a scratch diag dir, run from the repo root
$L launch --hidden --config $D/qt-copy.ini --game <nup> [--tensor] [--env NAME=VALUE] --max-seconds 300
$L press a --hold 200        # button names a b x y l r zl zr plus minus dup/ddown/dleft/dright up/down/left/right; name*N repeats
$L capture --settle 5 --out $D/x.png   # waits for screenshot + draw list; prints mean r,g,b (+ central half)
$L drawlist                  # newest draw list, consecutive identical draws folded; rt address/size/format
$L state --kind c|p|v        # programs seen recently, busiest first, with outcomes (ok/failed/drawn)
$L ablate add|remove|set|clear <key...>   # live skip list (ablate.txt), keys = shader dump keys
$L bisect --kind p --bad "b < 0.3 * g"    # halve active programs until the expression turns false
$L stop
```

- Live keys: `eden-cli` polls `<EDEN_NX2_DIAG_DIR>/keys.txt` (`<scancode> [ms]` lines; it renames
  and deletes the file). `EDEN_NX2_KEY_SCRIPT` is the timed, at-launch alternative;
  `EDEN_NX2_KEY_RECORD=<file>` records a session in the same format. A `gpu low` / `gpu high`
  line sets the GPU accuracy mid-game and applies it, like the Qt app's Change GPU Mode hotkey.
  The camera (right stick) is on I/J/K/L = scancodes 12 up, 13 left, 14 down, 15 right; `press`
  has no names for them, so write `<code> <ms>` lines to `keys.txt` directly.
- `EDEN_NX2_HIDE_WINDOW=1` (set by `--hidden`) creates the window hidden and unfocusable.
- Screenshots are 1280x720 TGA, the presented frame. `capture` converts to PNG without Pillow; look
  at it with vision, not just the stats.
- Image dumps: `--env EDEN_NX2_FRAME_CAPTURE=<dir> --env EDEN_NX2_DUMP=<present>:<addr>:<size>,...`
  write raw guest bytes at that present (flushed from the host first). Addresses come from
  `drawlist` and the log.
- Viewing a dumped program: `python3 -I tools/nx2/nvd_cubin.py <dump.bin>` disassembles Ampere SASS.

## Procedure

1. Build `eden-cli` (`ninja -C build eden-cli`). The Qt `yuzu` target doesn't rebuild it.
2. Prepare `$D/qt-copy.ini` as above, once per session.
3. Launch, wait for the title (title-specific timing in references), and press through to the
   scene. Write the sequence as a small script in `$D` so later runs repeat it exactly.
4. `capture` for numbers and a PNG. Compare against the user's reference screenshot.
5. Change one variable per run (env switch, ablation, build) and capture again.
6. `stop`, then check `$D/emulator.log` (full log of that run) and `coredumpctl list` if it
   ended early.
7. **Fix checks are short walks, not one capture.** Loop `press <dir> --hold 2000` over a few
   directions for about a minute, then count `signal 11/SEGV` in `journalctl`, `owner check` lines
   and stalls in the log. To provoke a reported freeze, replay what the user did (mode switch,
   camera pan) rather than walking longer.
8. **One-frame artifacts (flashing): pan and burst-capture.** Write `15 900` (camera right) to
   `keys.txt`, `capture --settle 1` right after, repeat ~12 times; paste the PNGs into one contact
   sheet (PIL) and compare neighbours with vision, then zoom (`region`) into any patch that is
   only in one frame. Single captures while walking miss them.
8b. **Flashes with a still camera: burst-capture every present.** Launch with
   `--env EDEN_NX2_FRAME_CAPTURE=<dir>` and write N to `<dir>/burst`: the next N presents are saved
   as `nx2-frame-<present>.tga` (every 2nd in practice, the readback is async). `scripts/oot_burst.sh
   <tag> <N> [launch args]` boots, bursts and stops; `scripts/flash_scan.py <dir> <sheet.png>` finds
   pixels >0.3 brighter than the median of the 8 frames around them and makes a sheet of the worst
   event. 400-600 frames (20-30 s in the lab) caught ~1 flash per run in OoT.
9. **A/B a fix against the build without it** when the symptom is intermittent: save the fix
   (`git diff <files> > fix.patch`), `git checkout -- <files>`, build `eden-cli`, copy it to scratch,
   `git apply fix.patch`, rebuild, then run both with `launch --binary <path>` and the same script.
   Compare a counter (verifier differences, luma spread, stall lines), not just "didn't crash".

### A crash the user reports from the Qt app

Read the log timeline before any stack: `~/.local/share/eden/log/eden_log.txt` (the previous run
is `eden_log.txt.old.txt`).

```bash
tr -d '\000' < $LOG | grep -a -E "LogSettings: M- Renderer.gpu_accuracy|GPU thread split|Waited [0-9]+ s|Shutdown requested|Userspace PANIC|failed to stop|SIGSEGV"
```

- `Waited N s for the GPU thread`, then `Shutdown requested`, then `Userspace PANIC ... 2010-0212`,
  then `Emulation thread failed to stop` and a SEGV is a **hang**. The user closed it, the forced
  stop made the guest abort, and the SEGV is teardown. Debug the first stall line, not the SEGV.
- A core written at teardown has no emulation threads left and often won't symbolize. Only a SEGV
  without a preceding stall has a useful `coredumpctl info` stack.
- Several `Channel N waits on semaphore` lines at once, or a looping `Waited N s for the GPU
  thread`, is a fence/split deadlock: see `references/gpu-split-hangs.md` for the signatures,
  the split invariants and how to get live stacks.
- `GPU thread split: stopped` / `engine methods run on a second thread` mid-log means the user
  switched GPU accuracy during the run. Reproduce that sequence (`gpu high`/`gpu low` in
  `keys.txt`) before theorising, and ask the user what they did around the event if the log
  doesn't show it.

## Pitfalls

- **No input reaches the game:** the config's player 1 is a gamepad. Merge the keyboard bindings
  (rule above). Hold presses at least 200 ms; the title may ignore a 120 ms tap.
- **`capture` times out:** the game isn't presenting (loading, paused, crashed), or a previous
  screenshot is still pending in the GSV capture hook. Check `state` (does the frame count advance?)
  and capture again.
- **Stale results across runs:** capture numbers restart per process. `launch` clears the old
  `capture-*.tga`/`draw-list-*.tsv`, and `capture` matches by mtime.
- **Hidden runs still use the GPU fully:** keep `--max-seconds`.
- **Don't `pkill -f <script name>` from a shell command that mentions that name:** it matches
  and kills your own shell, cutting the output. Stop runs with `lab_ctl stop` or by pid.
- **Fixed boot sleeps waste most of a run.** Time a title once with a capture every 10 s (luma
  shows black/title/load/gameplay), then press at those points and poll `capture` luma for the
  first gameplay frame instead of sleeping.
- **Reproducing a hang after a mid-game settings switch:** accurate GPU mode stops the GPU split
  and fast mode restarts it. Write `gpu high`, then `gpu low`, to `keys.txt`, and run with
  `--env EDEN_NX2_SPLIT_VERIFY=1`: "Split front register" warnings right after the restart mean the
  front's macro state went stale. The verifier resyncs what it finds, so verified runs don't hang;
  compare the difference counts instead.
- The shader lab browser UI (`EDEN_NX2_LAB=open`) and `lab_ctl` share the same files and can run
  together. The lab shows each program's shader catalogue entry and saves new names to the local
  catalogue (`tools/nx2/catalog.py`); `catalog.py status` / `merge` before a commit (AGENTS.md).
- The debug server (`EDEN_NX2_DEBUG_SERVER`, `docs/nx2/debug-mcp.md`) works in `eden-cli` too:
  `input_sequence` replays exact frame-timed moves, `capture_frame` dumps the frame's passes.

## References

- `references/ocarina-of-time.md`: OOT paths, timings, key sequence and findings (network
  programs, stand-in behaviour, colour-cast status).
- `scripts/to_gameplay.sh`: boot OOT hidden and load the newest save (template for other titles).
  Extra args go to `launch`; a later `--config`/`--binary` overrides the script's.
- `references/gpu-split-hangs.md`: freeze signatures, GPU split invariants, fence-behaviour modes,
  lab configs per fence mode, live stacks under gdb.
- `scripts/eden-gdb.sh`: `eden-cli` under gdb for `launch --binary` (stacks on SIGINT).
