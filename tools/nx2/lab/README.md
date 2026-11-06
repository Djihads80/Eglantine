# Ocarina of Time lab

Measures a change in game, the same way every time: boot hidden, load the newest save, play in
Kokiri Forest, keep the log. Every number in `OOT_STABLE.md` came from these runs. Windows only
for now (PowerShell); `tools/nx2/lab_ctl.py` underneath also runs on Linux.

## One-time setup

1. Build `eden-cli` (target `yuzu-cmd`): see [OOT_CONTRIBUTING.md](../../../OOT_CONTRIBUTING.md).
2. Boot Ocarina once in the Qt app and make a save in Kokiri Forest. The run loads the newest save.
3. Run `eden-cli` once so `sdl2-config.ini` exists, then make the lab config (your graphics
   settings, player 1 on the keyboard so the script can press buttons):

   ```
   python -I tools\nx2\lab\make_lab_config.py
   ```

4. Tell the script where the game is, once per terminal:

   ```
   $env:EGLANTINE_OOT_GAME = "D:\Games\Ocarina of Time.nup"
   ```

## A before/after measurement

```
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name before
# ... make your change, rebuild yuzu-cmd ...
powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name after
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\before\emulator.log
python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\after\emulator.log
```

A run takes about 5 minutes and needs the GPU to itself: close the emulator and don't build
while it runs. Results land in `tools\nx2\lab\runs\<name>\`:

| File | What it is |
|---|---|
| `emulator.log` | The full log. The script's last lines in the terminal say `result: no device loss` or `DEVICE_LOST`. |
| `game.png` | The frame about 100 s into gameplay. Look at it: a faster frame that renders wrong is a failure. |
| `stall-N.dmp` | Saved if the log stops for 15 s: every thread's stack. Open it in Visual Studio. |

Compare the **median of the 600-frame windows after the save loads** (about 150 s on), not the
overall average, which includes boot and menus. Frame rate swings about 3 FPS between runs, so
claim a gain only if it shows in two or three runs each.

## Switches worth knowing

| Option | Use |
|---|---|
| `-Env EDEN_NX2_GPU_PROFILE=1` | Per-second breakdown of where the GPU threads spend their time (`per second` log lines). Costs a few FPS. |
| `-Env EDEN_NX2_PERF_VERIFY=1` | Takes each shortcut and the safe path, and logs every difference as `Nx2Perf split:`. Use with `-Quick`. Must show zero before a shortcut ships. |
| `-Env EDEN_NX2_PERF_OFF=<name>` | Turns one optimization off (names are in the `Nx2Perf:` log line). |
| `-Quick` | 40 s of play, no screenshot. |

Several switches at once: `-Env A=1,B=2`.
