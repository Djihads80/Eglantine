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
- **Ask before long series of runs** if the user may be at the PC. Each run takes the GPU 2-5
  minutes.
- **Use the user's graphics settings, your key bindings.** `eden-cli` reads `sdl2-config.ini`, whose
  graphics settings differ from the Qt app's (async shaders, accuracy, docked). Make a scratch copy
  of `~/.config/eden/qt-config.ini`, merge in the SDL file's `player_0_button_*`, `lstick`, `rstick`
  and `motion` lines (the Qt ones are usually a gamepad, so keys do nothing), and pass
  `launch --config`.
- **Never `cd` into dumps or run scripts from them.** Pass paths as arguments, and run Python
  with `-I`.
- Check `pgrep -af 'eden-cli|eglantine'` before launching: only one instance should own the GPU.

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
  `EDEN_NX2_KEY_RECORD=<file>` records a session in the same format.
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

## Pitfalls

- **No input reaches the game:** the config's player 1 is a gamepad. Merge the keyboard bindings
  (rule above). Hold presses at least 200 ms; the title may ignore a 120 ms tap.
- **`capture` times out:** the game isn't presenting (loading, paused, crashed), or a previous
  screenshot is still pending in the GSV capture hook. Check `state` (does the frame count advance?)
  and capture again.
- **Stale results across runs:** capture numbers restart per process. `launch` clears the old
  `capture-*.tga`/`draw-list-*.tsv`, and `capture` matches by mtime.
- **Hidden runs still use the GPU fully:** keep `--max-seconds`.
- The shader lab browser UI (`EDEN_NX2_LAB=open`) and `lab_ctl` share the same files and can run
  together.

## References

- `references/ocarina-of-time.md`: OOT paths, timings, key sequence and findings (network
  programs, stand-in behaviour, colour-cast status).
- `scripts/to_gameplay.sh`: boot OOT hidden and load the newest save (template for other titles).
