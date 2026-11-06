# Ocarina of Time lab

Measures a change in game, the same way every time: boot hidden, load the newest save, play in
Kokiri Forest, keep the log. Every number in `OOT_STABLE.md` came from these runs. Windows only
for now (PowerShell); `tools/nx2/lab_ctl.py` underneath also runs on Linux.

## One-time setup

1. Build `eden-cli` (target `yuzu-cmd`): see [OOT_CONTRIBUTING.md](../../../OOT_CONTRIBUTING.md).
2. Boot Ocarina once in the Qt app and make a save in Kokiri Forest. The run loads the newest save,
   and it knows gameplay has started by a Kokiri Forest log line (see `-GameplayMarker`).
3. Run `eden-cli` once so `sdl2-config.ini` exists, then make the lab config (your graphics
   settings, player 1 on the keyboard so the script can press buttons):

   ```
   python -I tools\nx2\lab\make_lab_config.py
   ```

4. Tell the script where the game is, once per terminal (or pass `-Game` each time):

   ```
   $env:EGLANTINE_OOT_GAME = "D:\Games\Ocarina of Time.nup"
   ```

## A before/after measurement

```
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name before
# ... make your change, rebuild yuzu-cmd ...
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name after
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\before
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\after
```

A run takes about 5 minutes and needs the GPU to itself: close the emulator and don't build
while it runs. It ends with the summary and a `result:` line, and its exit code says the same:

| Result | Exit | Meaning |
|---|---|---|
| `no device loss` | 0 | Gameplay reached and measured. |
| `DEVICE_LOST` / `EMULATOR EXITED` | 1 | A crash. Checked in both `emulator.log` and `%APPDATA%\eden\log\eden_log.txt`: one has missed it before. |
| `INVALID: never reached gameplay` | 3 | The run stayed on the title screen or a menu. Its windows read ~60 FPS and mean nothing; run it again. |
| `lab step failed` | 6 | Python, the launch, a key press or stopping the emulator failed. No result. |

The emulator is stopped at the end, on Ctrl+C, and by `lab_ctl.py`'s watchdog after `-MaxSeconds`
(420) even if the script itself is gone. Results land in `tools\nx2\lab\runs\<name>\`:

| File | What it is |
|---|---|
| `emulator.log` | The full log; `eden_log.txt` is the app log of the same run. |
| `phases.txt` | When gameplay and the walk started, so the summary can tell the spawn spot from the walk. |
| `game.png` | The frame about 100 s into gameplay. Look at it: a faster frame that renders wrong is a failure. |
| `stall-N.dmp` | Saved if the log stops for 45 s: every thread's stack. Open it in Visual Studio. |
| `flash.jsonl` | With `-Flash N`: the brightness of each capture. |

## Reading the numbers

`perf_summary.py` lists the 600-present windows (`nx2:vi2 last 600 presents` lines) and labels
each one `title/menu`, `loading`, `spawn` (standing at the save's spot) or `walk` (`gameplay`
for a log without the run's `phases.txt`). Compare the
**median of the spawn windows** (and of the walk windows, separately), never the overall average:
spawn and walk differ by ~8 FPS at our save, and the title screen runs at 60. It also sums the
per-second `Nx2Frame:` lines (presents per second, time blocked on the GPU fence, intervals over
20 ms). Frame rate swings about 3 FPS between runs, so claim a gain only if it shows in two or
three runs each. Numbers are comparable only at the same save and spot.

## Switches worth knowing

| Option | Use |
|---|---|
| `-Quick` | 40 s of play, no screenshot. |
| `-Stay` | No walk: measure only the save's spawn spot. |
| `-Flash 30` | 30 captures standing still, then `flash_stats.py`. A change that touches frame timing must show `outliers=0`: running the guest ahead of the GPU caused brightness flashes before. |
| `-Env EDEN_NX2_GPU_PROFILE=1` | Per-second breakdown of where the GPU threads spend their time (`per second` log lines). Costs a few FPS. |
| `-Env EDEN_NX2_PERF_VERIFY=1` | Takes each shortcut and the safe path, and logs every difference as `Nx2Perf split:`. Use with `-Quick`. Must show zero before a shortcut ships. |
| `-Env EDEN_NX2_PERF_OFF=a,b` / `EDEN_NX2_PERF_ON=a,b` | Turn optimizations off or on by name. The `Nx2Perf:` log line at start lists every switch and its state. `EDEN_NX2_PERF_SAFE=1` turns all experimental ones off. |
| `-Env EDEN_NX2_PERF_CONTROL=<file>` | Switch live, without a reboot: edit the file (`label B.1`, `off front_run`, `on ...`, `safe`), and each change is logged as `Nx2PerfLive: set=...`. Only live-safe switches change (listed in the `Nx2PerfLive: watching` line); others show as `ignored=`. `perf_summary.py` then prints FPS per label (`B.1`, `B.2` count as arm `B`). Alternate A and B a few times: one pair proves nothing. |
| `-Env EDEN_NX2_VCACHE=0` | Turns 3D V-Cache pinning off (Ryzen X3D); `=1` forces it on. |

Several switches at once: `-Env A=1,B=2`.
