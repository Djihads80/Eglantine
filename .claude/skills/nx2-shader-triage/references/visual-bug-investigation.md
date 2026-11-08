# Visual bugs on NX2 titles (tint, black scene, missing effect)

Use when every program lowers and builds but the image is wrong. Goal: find the first program
or stage where the image goes bad, with runs the agent drives itself.

## Tooling

- Shader lab (browser) + diagnostics: `tools/nx2/shader_lab.py`, `src/video_core/nx2_diag.h`,
  from `fix/missing-mkw-effects` (commits a659d78bb1..ec65ee9c6e). Cherry-pick onto a scratch
  branch; the two conflicts (kepler_compute.cpp, vk_rasterizer.cpp) are additive, keep both, with
  the diag zero-block `}` and ablation check before the DLSS-chain code.
- `tools/nx2/lab_ctl.py` (scratch/oot-shader-lab): CLI over the same diag folder. Subcommands:
  `launch`, `press`, `capture`, `stats`, `drawlist`, `state`, `ablate`, `bisect`, `stop`.
- `eden-cli` reads `<EDEN_NX2_DIAG_DIR>/keys.txt` live (`<code> <ms>` lines; SDL scancodes,
  A=4 B=22 +=16, arrows 79-82). `EDEN_NX2_HIDE_WINDOW=1` hides the window, `EDEN_NX2_KEY_SCRIPT`
  is the timed version.

## Procedure

1. **Config.** `eden-cli` reads `sdl2-config.ini`, whose graphics settings differ from the Qt
   app's (async shaders, accuracy, docked). Copy `qt-config.ini` to scratch and merge the SDL
   file's `player_0_button_*`/`lstick`/`rstick` lines into it. The Qt bindings are usually a
   gamepad, so key presses do nothing. Pass it with `launch --config`.
2. **Launch and reach the scene.** `lab_ctl -d $D launch --hidden --config ... --game <nup>
   [--tensor] [--env NAME=VALUE] --max-seconds 300`. Title appears after ~90 s; `press a`
   (hold >= 200 ms) through title, load screen and save. Gameplay programs differ from title ones;
   test where the user saw the bug.
3. **Measure.** `capture --settle N --out x.png` prints mean r/g/b (whole and central half). A
   tint is a channel ratio (`b < 0.3*g`). Look at the PNG with vision too.
4. **A/B ladder before any bisect.** Compare the same scene across: stand-in (TENSOR unset),
   TENSOR=1, TENSOR=stub, and the user's reference screenshot. Each removes a suspect class.
5. **Dump images, don't only ablate.** `--env EDEN_NX2_FRAME_CAPTURE=<dir>
   --env EDEN_NX2_DUMP=<present>:<addr>:<size>,...` writes raw guest bytes at that present.
   Addresses come from `drawlist` (`rt` column) and the `Upscale fallback ... ->` log lines.
   Decode per format (A8B8G8R8, A2B10G10R10, B10G11R11) and compare channel means stage by stage.
   An all-zero dump of a host-written image means it wasn't flushed back or isn't at that address.
   It doesn't mean the image is empty.
6. **Narrow to instructions.** Disassemble with `tools/nx2/nvd_cubin.py <dump>`. Trace the bad
   channel's register chain back from the final store. Then run `scripts/lowered_for.cpp` on
   those offsets and decode each Maxwell word against `frontend/maxwell/translate/impl/*.cpp`
   bitfields.

## Pitfalls

- **A full-screen white wash with black plant silhouettes is missing geometry, not a bloom
  bug.** Ocarina of Time's vegetation is culled on the GPU through pointers loaded from memory
  (pointer tables, a pointer behind a cbuf pointer). If the storage buffer pass can't follow them
  the loads read 0, every instance is culled and the plants draw unlit. Auto-exposure then
  overexposes the rest. Check that storage buffers behind loaded pointers bind
  (`indirect_offset`, table pointers) before touching tone mapping. Luma that holds steady while
  walking means exposure works; luma swinging 0.84 to 0.43 means it doesn't.
- **`EDEN_NX2_SPLIT_CHECK` needs the Linux reporter** (`1789b05d94`). Before that commit it only
  worked on Windows and printed nothing on Linux. A GPU-thread SEGV in
  `CopyBuffer`/`InlineMemoryImplementation` under `SignalFence` or `ReleaseFences` means the
  split's front used back-owned Vulkan state after a drain: route it through `GpuSplit::Run`.


- **Ablation can't prove a producer.** An ablated program stops writing, and its output image keeps
  the last (bad) frame, so `bisect` reports "still bad with everything ablated". Bisect with
  ablation only for effects drawn fresh each frame. For persistent images, dump instead.
- **The upscale stand-in guesses image pairs** from cbuf0 0x80..0x11c when the known slots don't
  match. A wrong guess blits a tiny image over the frame and the scene goes black. Check the
  `Upscale fallback <prog>: <in> WxH -> <out> WxH` lines for implausible sizes.
- **Lowered instruction lists have no Maxwell control words.** Don't skip every 4th entry when
  scanning `LoweredProgram::instructions`. Map back with `source_instruction_offsets[ampere_index]`.
- **`capture` matches files by mtime.** Capture numbers restart each run, so stale
  `capture-N.tga` from a previous run must not count. `launch` clears them.

- **One material pure white for 1-2 frames, camera still (OoT tunic/leaves):** stale per-draw data in a native program's 64 KiB uniform bank, not the GPU split or fences. Bursts (`nx2-lab-automation` scripts/oot_burst.sh + flash_scan.py) A/B it: re-uploading the bank on every bind (`EDEN_NX2_BANK_REUPLOAD`, default 2) took it from ~1/600 presents to 0/1800. Sync GPU makes it more frequent, which is the quickest repro.
- **Distant/LOD vegetation flashing white for a frame:** check the `diag 1s` line's `cs:failed` rate, not just the logged rejections (only the first per program is logged). OoT had 5 compute programs rejected on every dispatch (ULDC.U8/U16, ULEA carry to UP3, two-source F2IP, I2F.U8 of a UR byte); lowering them stopped the LOD flashes. With `EDEN_NX2_RESNAPSHOT=0` the same flashes became ~25x more frequent, a quick way to make them reproducible. Re-list the dump dir before a hash-regression run: lab runs keep adding programs.
- **Catching a 1-2 frame artifact objectively:** `EDEN_NX2_FRAME_CAPTURE=<dir>` plus `EDEN_NX2_FLASH_WATCH=1` examines every present and logs `flash watch: present N has K near-white pixels (median M)`, saving `flash-N.tga` with `before-N.tga`. Calibrate the threshold from a normal frame first: OoT's misty Kokiri Forest has 0 pixels with min(R,G,B)>245 normally and 7000+ when vegetation flashes. Bright mist views in the lab pulse in brightness and trip any absolute count, so the watcher is a user-session tool, not a lab one; the burst scanner (`flash_scan.py`) is the lab equivalent.
