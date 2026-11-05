# The Legend of Zelda: Ocarina of Time (Switch 2, NUP)

- Game: `/run/media/kraibse/SOINC/emulation/Switch 2/games/The Legend of Zelda Ocarina of TIme [04004860224DEC000].nup`
  (a standalone update NUP; boots the update program directly). Shader cache key `0400486024dec000`.
- Firmware: `/run/media/kraibse/SOINC/emulation/Switch 2/firmware/firmware` (user-provided).
- Shader dumps: `~/Documents/hermes/eglantine/oot-shader-dump` (all 1937 lowered as of 6b934346d7).

## Reaching a scene

- Title screen with 3D background after ~90 s (first boot of a build is longer: pipelines compile).
- `press a` (hold 200) at the title -> load screen; `a` again picks the newest save, `a` confirms;
  gameplay ~40-75 s later (Kokiri Forest in the user's save).
- Script: `scripts/to_gameplay.sh` in this skill.

## Findings so far (2026-11-05)

- 3D image goes through an HMMA network (DLSS-like). Title: 11 compute programs, final pass
  `6093c9143ba0904d` (TEX.F16 input `0x28`, dither TLD `0x2e`, SUST.P RGBA to `0x32`).
  Gameplay: `6f9367bd41dc0b58` (TLD4.F16 R/G/B gathers with PTP offsets).
- `EDEN_NX2_TENSOR` unset: programs rejected, `UpscaleInsteadOfNativeProgram` blits. Title is
  correct; in gameplay it guesses wrong pairs (4x4 -> 1920x1080, 120x67 -> 2560x1440): black scene.
- `EDEN_NX2_TENSOR=1` and `=stub`: identical yellow cast (mean blue ~0.055 vs ~0.45). So the
  cast isn't the HMMA math. The network's input image has blue. Every instruction on the blue path of
  `6093` lowers correctly in isolation (TEX.F16 split pack, HADD2 A-swizzle, TLDS second dest,
  FFMA, SUST.P). Next: dump each network program's output image per frame and find where blue
  goes missing.
- Output image dump at `0x811344a000` in tensor mode came back all zero (not the network's real
  output address, or not flushed).
- Load screen save thumbnails are corrupted (noise / black) — separate bug.
- Earlier crashes: SIGSEGV in `BufferCacheRuntime::CopyBuffer` (GPU thread, semaphore release path)
  and SIGBUS in `MemoryManager::IsContinuousRange` after a garbage command stream.
