# Settings reference for Mario Kart World 1.8.0

What every user-facing setting of this emulator really does, read from the code of branch
`mkw-eglantine` at `71c606cdda` (Eglantine `nx2` de83c3140e merged on 2026-10-31), and what to set
for Mario Kart World (program id `0400C3F00006E000`, a Switch 2 title). Each entry names the
function that reads the setting, so it can be checked again after an update. Measurements quoted
come from [`NX2_NOTES.md`](../../NX2_NOTES.md), [`NX2_VRAM.md`](../../NX2_VRAM.md) and
[experiments](experiments.md); everything else is read from the code and marked untested where it
matters.

Tags used below:

- **[SPEED]**: changes how fast the game's own clock runs, or how often it may present. Wrong
  values make the game run too fast or too slow. See [settings that change game speed](#settings-that-change-game-speed-or-timing).
- **[VISUAL]**: a hack that can cause visual bugs.
- **[PERF]**: a value that costs performance in this game (often the default).
- **[NO EFFECT]**: nothing reads it for Mario Kart World (Switch 1 only, other platform, or unused).

(a) is the reference desktop of this fork: Ryzen 7 5800X + Radeon RX 6900 XT 16 GB, 4K 120 Hz VRR
TV. (b) is any 8 GB graphics card.

## Safe defaults for Mario Kart World

| Setting (where) | `[group] key` in qt-config.ini | (a) 16 GB AMD, 4K 120 Hz VRR | (b) 8 GB GPU | Why |
| --- | --- | --- | --- | --- |
| CPU Clocks (System) | `[System] fast_cpu_time` | `0` Normal | `0` Normal | Boost/Overclock multiply the game's clock: the game runs 1.7x / 2x too fast |
| Synchronize Core Speed (System) | `[Core] sync_core_speed` | `false` | `false` | Divides the game's clock by the speed percentage |
| Limit Speed Percent (System) | `[Core] use_speed_limit`, `speed_limit` | `true`, `100` | `true`, `100` | Paces presents to the console's 60 Hz |
| Multicore CPU Emulation (System) | `[Core] use_multi_core` | `true` | `true` | Off ties the game's clock to emulated instructions |
| Memory Layout (System) | `[Core] memory_layout_mode` | `4` 12GB | `4` 12GB | Forced for Switch 2 titles anyway |
| GPU Clocks (System) | `[System] fast_gpu_time` | `1` Boost (default) | `1` Boost | Only GPU timestamps; every test ran with it |
| Docked / Handheld (Controls) | `[System] use_docked_mode` | `1` Docked | `1` Docked | Docked renders 2560x1440, handheld 1920x1080 |
| CPU Accuracy (CPU) | `[Cpu] cpu_accuracy` | `0` Auto | `0` Auto | Fastest safe JIT settings |
| API (Graphics) | `[Renderer] backend` | `1` Vulkan | `1` Vulkan | The Switch 2 work and the GPU thread split need Vulkan |
| Resolution (Graphics) | `[Renderer] resolution_setup` | `3` 1X | `3` 1X | The game renders 1440p itself; the emulator is CPU-bound |
| VSync Mode (Graphics) | `[Renderer] use_vsync` | `3` FIFO Relaxed | `3` FIFO Relaxed | Plain FIFO waits a whole refresh when a frame is late |
| Window Adapting Filter (Graphics) | `[Renderer] scaling_filter` | `6` AMD FidelityFX Super Resolution | `1` Bilinear (`7` Area if the window is under 2560x1440) | The default, Nearest Neighbor, scales 1440p to 4K unevenly |
| Enable Frame Generation (Graphics) | `[Renderer] frame_gen` | `false` | `false` | New and untested here; see [frame generation](#frame-generation-new-on-2026-10-31) |
| GPU Mode (Advanced) | `[Renderer] gpu_accuracy` | `0` Fast | `0` Fast | **The default, Accurate, turns the split GPU thread off** |
| DMA Accuracy (Advanced) | `[Renderer] dma_accuracy` | `0` Default | `0` Default | Safe also turns the split off |
| GPU Fence Behavior (Advanced) | `[Renderer] gpu_fence_behavior` | `0` Default | `0` Default | Balanced/Accurate make the GPU thread wait for the host GPU |
| ASTC Decoding Method (Advanced) | `[Renderer] accelerate_astc` | `1` GPU | `1` GPU | Decodes and re-compresses on the GPU |
| ASTC Recompression Method (Advanced) | `[Renderer] astc_recompression` | `2` BC3 | `2` BC3 | Uncompressed fills even a 16 GB card: garbage textures, then a crash |
| VRAM Usage Mode (Advanced) | `[Renderer] vram_usage_mode` | `1` Aggressive | `0` Conservative | Next to no effect for Switch 2 titles (the collector follows the driver budget) |
| Frame Pacing Mode (Advanced) | `[Renderer] frame_pacing_mode` | `0` Auto | `0` Auto | The game is already paced to 60 Hz |
| Enable Reactive Flushing (Advanced) | `[Renderer] use_reactive_flushing` | `false` | `false` | On (the default) makes CPU reads of GPU-tracked pages fault |
| Anisotropic Filtering (Advanced) | `[Renderer] max_anisotropy` | `0` Automatic (`5` 16x optional) | `0` Automatic | Visual only |
| Enable asynchronous shader compilation (Extras) | `[Renderer] use_asynchronous_shaders` | `true` | `true` | Off freezes the GPU thread 40-100 ms per new pipeline |
| Enable asynchronous presentation (Extras) | `[Renderer] async_presentation` | `false` (worth trying `true`) | `false` | Tested value; see the entry |
| Extended Dynamic State (Extras) | `[Renderer] dyna_state` | `2` EDS2 | `2` EDS2 | Changing it discards the cached pipelines |
| Everything else | | default | default | See the tables below for the few exceptions |

**Editing qt-config.ini by hand** (`%APPDATA%\eden\config\qt-config.ini`, or `user\config\` next to
the executable): every key has a companion `key\default=true|false`. While `\default` is `true` the
value written after `key=` is ignored and the built-in default is used
(`src/frontend_common/config.cpp` `Config::ReadSettingGeneric`), so set it to `false`. Enums are
stored as the numbers given here.

**Per-game settings**: right-click the game > *Properties* writes
`%APPDATA%\eden\config\custom\0400C3F00006E000.ini`; a key there with `\use_global=false` wins over
the global value. When something behaves oddly, look in both files: a forgotten per-game
`fast_cpu_time=1` makes the game run 1.7x too fast whatever the global setting says.

**Changes during play**: settings marked as changeable while a game runs take effect without a
restart (VSync at the next swapchain creation) and are saved with the configuration, including
those changed by hotkeys (see [hotkeys](#hotkeys-and-status-bar-buttons-that-change-settings)).
The others are greyed out until the game is stopped.

## What the 2026-10-31 merge added

- **Frame generation on desktop** (Djihads80, 8a72dc011e): the eight `frame_gen*` settings are no
  longer hidden on Windows and a new type, FSR 3, needs no external file. See
  [frame generation](#frame-generation-new-on-2026-10-31).
- **FSR 3 window adapting filter**: `scaling_filter=15`.
- **Docked/handheld switch during play** (d2ac227524): Switch 2 titles now get told when the
  console mode changes (`AppletManager::OperationModeChanged`, and Switch 2 programs get a real
  `GetOperationModeRefreshEvent` in `src/core/hle/service/am/service/application_functions.cpp`),
  so F10 / the status bar button switch Mario Kart World between 2560x1440 and 1920x1080 live.

## System tab

*Emulation > Configure > System > System.* The *Core* group holds the `[Core]` settings, the
*System* group the `[System]` ones.

### Multicore CPU Emulation **[SPEED]**

- `[Core] use_multi_core`: `true` / `false`. Default `true`. Not changeable while a game runs.
- What it does: with multicore, the guest counter (CNTPCT, the game's clock) follows the host
  clock. Without it, all guest cores run on one host thread and the counter is derived from the
  number of emulated instructions (`src/core/core_timing.cpp` `CoreTiming::GetClockTicks` uses
  `CPUTickToGuestCNTPCT(cpu_ticks)`; `src/core/hle/kernel/k_process.cpp`
  `KProcess::InitializeInterfaces` passes the mode to the JIT, which then counts cycles).
- Mario Kart World: off makes the game's time depend on how fast the emulator runs, and puts all
  guest cores' work on one host thread. Unplayable.
- Recommended: (a) and (b) `true`. Off is a debugging aid.

### Memory Layout

- `[Core] memory_layout_mode`: `0` 4GB DRAM (Default), `1` 6GB (Unsafe), `2` 8GB, `3` 10GB
  (Unsafe), `4` 12GB (Unsafe). Default `0`.
- What it does: the size of emulated DRAM (`src/core/hle/kernel/board/nintendo/nx/k_system_control.cpp`
  `GetIntendedMemorySize`). For a Switch 2 program `src/core/core.cpp` `System::Impl::Load` sets
  it to 12 GB as a per-game value whatever is configured (log: "Switch 2 program: using the 12 GiB
  memory layout"), and the configured value comes back after the game.
- Mario Kart World: always 12 GB. The 12 GB layout is 12 GiB plus 8 GiB for the Switch 2 graphics
  server (`KSystemControl::Nx2GraphicsServerMemorySize`), committed at once at boot
  (`src/common/host_memory.cpp`, `CreateFileMapping2` with `SEC_COMMIT`): Windows needs about
  **20 GiB of free commit** (RAM + page file). If the boot fails with "Failed to allocate 20480 MiB
  of backing memory", enlarge the page file (NX2_NOTES row 128).
- Recommended: (a) and (b) `4`, so the dialog shows what is used.

### Limit Speed Percent (`use_speed_limit` + `speed_limit`) **[SPEED]**

- `[Core] use_speed_limit`: the row's checkbox, `true` / `false`, default `true`.
  `[Core] speed_limit`: the spin box, 0-9999 %, default `100`. Both changeable during play.
- What it does:
  - Switch 2 presentation: `src/core/hle/service/nx2_gsv.h` `INx2Vi2Session::PaceToVblank` holds
    each present until the next vblank of a 60 Hz x speed/100 timer (16.7 ms at 100 %). Unchecked
    (or 0 %), presents are not paced at all.
  - `Settings::SpeedLimit()` (`src/common/settings.cpp`) returns `speed_limit`, or the Turbo/Slow
    value while those modes are on; it is what Synchronize Core Speed divides the clock by.
  - The Frame Pacing Mode caps apply only while the limit is checked
    (`src/video_core/renderer_vulkan/vk_scheduler.h` `Scheduler::Wait`).
  - Unchecked (or Turbo), VSync FIFO / FIFO Relaxed are replaced by Mailbox or Immediate at the
    next swapchain creation (`src/video_core/renderer_vulkan/vk_swapchain.cpp` `ChooseSwapPresentMode`).
  - Also: the Switch 1 vi vsync rate (`src/core/hle/service/vi/conductor.cpp`) and, with
    multicore off only, `SpeedLimiter::DoSpeedLimiting` (`src/core/perf_stats.cpp`, called at each
    present).
- Mario Kart World: the game's own vsync event stays at 60 Hz (a CoreTiming event in
  `INx2Vi2Session::GetVsyncEvent`), and its game time follows the guest counter, so this setting
  mostly changes how often it may present. Unpaced, another Switch 2 title ran at up to 160 fps,
  "its timing and audio with it" (comment above `PaceToVblank`). Ctrl+U (or Home+Y on a
  controller) toggles the checkbox and the change is saved.
- Recommended: (a) and (b) checked, `100`.

### Turbo Speed / Slow Speed **[SPEED]**

- `[Core] turbo_speed_limit` (default `200`) and `[Core] slow_speed_limit` (default `50`), 0-9999 %.
- What it does: the speed used while the Toggle Turbo Speed (Ctrl+Z) or Toggle Slow Speed (Ctrl+X)
  hotkey is active (`Settings::ToggleTurboMode` / `ToggleSlowMode`); the mode itself
  (`current_speed_mode`) is not saved. Turning a mode on also checks Limit Speed.
- Mario Kart World: paces presents at 120 / 30 Hz, and with Synchronize Core Speed on it
  rescales the game's clock (see below).
- Recommended: defaults; do not use the hotkeys while racing.

### Synchronize Core Speed **[SPEED]**

- `[Core] sync_core_speed`: `true` / `false`. Default `false`. Not changeable while a game runs.
- What it does: `CoreTiming::GetClockTicks` divides the guest counter by `SpeedLimit()/100`. At
  100 % it does nothing.
- Mario Kart World: with any other speed, the game's clock runs at 100/speed of real time. The
  division applies to the whole counter (ticks since the emulator started), not to its rate:
  pressing Turbo during a race makes the game's clock jump back by half the time since the
  emulator started, and forward again when Turbo ends. Speed limit 0 % with this on divides by
  zero.
- Recommended: (a) and (b) `false`.

### CPU Clocks **[SPEED]**

- `[System] fast_cpu_time` (note: in `[System]`, not `[Cpu]`): `0` Normal, `1` Boost, `2`
  Overclock. Default `0`. Changeable while a game runs.
- What it does: despite the tooltip ("raises the clock the emulated CPU reports"), it multiplies
  the guest counter: `CoreTiming::GetClockTicks` returns `ticks * target / 1020` with target 1734
  (Boost, x1.70) or 2040 (Overclock, x2.00). Every reader of the game's clock goes through it:
  `MRS CNTPCT_EL0` (`DynarmicCallbacks64::GetCNTPCT`), `svcGetSystemTick`
  (`src/core/hle/kernel/svc/svc_tick.cpp`), thread CPU time, the steady clock of the time services.
- Mario Kart World: its game time follows the counter, so Boost runs the whole game 1.7x too fast,
  Overclock 2x. Before Eglantine had the correct 31.25 MHz Switch 2 counter, the old 19.2 MHz one
  made the game 1.63x too slow and Boost (x1.70) hid it (0.614 x 1.70 = 1.04). Changing the value
  during a race makes the clock jump by 0.7 times the time since the emulator started (forward when
  Boost is turned on, backward when it is turned off), since the factor applies to the whole count.
- Recommended: (a) and (b) `0` Normal. Check the per-game file too.

### GPU Clocks

- `[System] fast_gpu_time`: `0` Normal, `1` Boost, `2` Overclock. Default `1` Boost. Changeable
  while a game runs.
- What it does: divides the GPU timestamps written into guest memory by 256 (Boost) or 512
  (Overclock) (`src/video_core/gpu.cpp` `GPU::Impl::GetTicks`, used by timestamped query reports
  in `src/video_core/query_cache/query_cache.h` and `Maxwell3D`). Games measuring GPU time see
  their GPU work as nearly free, so dynamic-resolution logic stays at full detail. It does not
  touch the CPU-side clock.
- Mario Kart World: no effect on game speed. Normal would show the game the emulator's real GPU
  timing (frames that take longer than on the console), which could make it lower its detail;
  not measured. Changing it during play makes the timestamps jump.
- Recommended: (a) and (b) `1` Boost, the default every test of this fork used.

### Other System settings

| Label | `[group] key` | Values (default) | What it does | Recommended |
| --- | --- | --- | --- | --- |
| Language: | `[System] language_index` | 0 Japanese, 1 American English, 2 French, 3 German, 4 Italian, 5 Spanish, ... 12 British English, 13 Canadian French, 14 Latin American Spanish ... (1) | The system language the game reads (`set` service) | Your language (e.g. `2`) |
| Region: | `[System] region_index` | 0 Japan, 1 USA, 2 Europe, 3 Australia, 4 China, 5 Korea, 6 Taiwan (1) | System region code | Match the language (e.g. `2`) |
| Time Zone: | `[System] time_zone_index` | 0 Auto, 1 Default, then zone names (0) | Guest time zone | `0` |
| Custom RTC Date: | `[System] custom_rtc_enabled`, `custom_rtc_offset` | off, 0 s | Offsets the wall-clock date/time (`System::Impl::RefreshTime`, steady clock resource). Not the guest counter: no speed effect | off |
| RNG Seed | `[System] rng_seed_enabled`, `rng_seed` (hex) | off, 0 | Fixed seed for the kernel RNG, `spl` and the code load offset (ASLR) | off |
| Device Name | `[System] device_name` | "Eden" | Console name returned by `set` | any |
| Console Mode: | `[System] use_docked_mode` | 0 Handheld, 1 Docked (1) | Shown here only in a game's Properties; globally it is on the Controls tab. See [Controls](#controls-tab) | `1` |

## CPU tab

*Emulation > Configure > CPU.*

### Accuracy: (CPU accuracy)

- `[Cpu] cpu_accuracy`: `0` Auto, `1` Accurate, `2` Unsafe, `3` Paranoid (disables most
  optimizations), `4` Debugging. Default `0`. Not changeable while a game runs.
- What it does (`src/core/arm/dynarmic/arm_dynarmic_64.cpp` `ArmDynarmic64::MakeJit`):
  - Auto: unsafe FMA unfusing, 64-bit fastmem address space (no bounds check before memory
    accesses), global monitor ignored for exclusives.
  - Accurate: none of these. Slower.
  - Unsafe: applies the seven *Unsafe CPU Optimization Settings* checkboxes (shown only in this
    mode); with their defaults it adds faster FRSQRTE/FRECPE and inaccurate NaN handling to Auto.
  - Paranoid: no JIT optimizations at all. Very slow.
  - Debugging: applies the *Debug > CPU* toggles (below).
  - Fastmem (host MMU) stays on except in Unsafe with "Enable Host MMU Emulation" off, or in
    Debugging with the fastmem toggle off (`Settings::IsFastmemEnabled`).
- Mario Kart World: the guest CPU is not the bottleneck (the main core mostly polls the clock while
  it waits for the GPU thread). Auto is right; Accurate/Paranoid only cost speed.
- Recommended: (a) and (b) `0` Auto.

### Backend: **[NO EFFECT]**

- `[Cpu] cpu_backend`: `0` Dynarmic, `1` NCE. Default `0` on Windows. NCE (native execution) is
  built only for ARM64 Android/Linux (`HAS_NCE`, top-level `CMakeLists.txt`); on Windows the
  *CPU Backend* group is hidden and the value is ignored (`Settings::SetNceEnabled`).

### Custom CPU Ticks **[SPEED]** (single core only)

- `[Cpu] use_custom_cpu_ticks` (checkbox, default `false`) and `[Cpu] cpu_ticks` (77-65535,
  default `16000`).
- What it does: replaces the number of ticks the JIT adds per executed block
  (`CoreTiming::GetNextTickCount` / `AddTicks`). The JIT counts cycles only with multicore off
  (`config.enable_cycle_counting = !m_uses_wall_clock` in `ArmDynarmic64::MakeJit`), and there
  those ticks are the game's clock.
- Mario Kart World: no effect with multicore on. With multicore off it directly speeds up or slows
  down the game's clock.
- Recommended: (a) and (b) unchecked.

### Unsafe CPU Optimization Settings (shown only with Accuracy: Unsafe)

All `[Cpu]`, all default `true`, applied only when `cpu_accuracy=2` (`ArmDynarmic64::MakeJit`).

| Label | Key | What it does |
| --- | --- | --- |
| Enable Host MMU Emulation (fastmem) | `cpuopt_unsafe_host_mmu` | Guest loads/stores go straight to host memory; off = software page walk on every access (much slower; reactive flushing's page protection relies on fastmem) |
| Unfuse FMA (improve performance on CPUs without FMA) | `cpuopt_unsafe_unfuse_fma` | Multiply then add instead of fused (tiny rounding differences) |
| Faster FRSQRTE and FRECPE | `cpuopt_unsafe_reduce_fp_error` | Host approximations for reciprocal estimates |
| Faster ASIMD instructions (32 bits only) | `cpuopt_unsafe_ignore_standard_fpcr` | 32-bit code only: no effect on this 64-bit game |
| Inaccurate NaN handling | `cpuopt_unsafe_inaccurate_nan` | Skips NaN fix-ups |
| Disable address space checks | `cpuopt_unsafe_fastmem_check` | 64-bit fastmem space (as Auto does) |
| Ignore global monitor | `cpuopt_unsafe_ignore_global_monitor` | Exclusives rely on cmpxchg only (as Auto does) |

Recommended: stay on Auto; these are not needed.

### Debug > CPU sub-tab (Toggle CPU Optimizations)

*Emulation > Configure > General > Debug > CPU.* `[Cpu]` keys `cpuopt_page_tables`,
`cpuopt_block_linking`, `cpuopt_return_stack_buffer`, `cpuopt_fast_dispatcher`,
`cpuopt_context_elimination`, `cpuopt_const_prop`, `cpuopt_misc_ir`,
`cpuopt_reduce_misalign_checks`, `cpuopt_fastmem`, `cpuopt_fastmem_exclusives`,
`cpuopt_recompile_exclusives`, `cpuopt_ignore_memory_aborts`, all default `true`: each disables one
JIT optimization, and only with `cpu_accuracy=4` Debugging (`ArmDynarmic64::MakeJit`). For JIT
debugging only; keep all checked.

## Graphics tab

*Emulation > Configure > Graphics > Graphics.* All keys are in `[Renderer]`.

### API: / Device:

- `backend`: `1` Vulkan, `0` OpenGL GLSL, `3` OpenGL GLASM, `4` OpenGL SPIR-V, `2` Null. Default
  `1`. `vulkan_device`: index of the Vulkan device (default `0`).
- Mario Kart World: the Switch 2 work targets Vulkan, and the split GPU thread only starts with
  Vulkan (`src/video_core/gpu_split.cpp` `GpuSplit::MaybeStart`). OpenGL is untested and slower.
  Null renders nothing.
- Recommended: (a) and (b) `1` Vulkan, the discrete GPU as device.

### Resolution:

- `resolution_setup`: `0` 0.25X, `1` 0.5X, `2` 0.75X, `3` 1X, `4` 1.25X, `5` 1.5X, `6` 2X, `7` 3X,
  `8` 4X, `9` 5X, `10` 6X, `11` 7X, `12` 8X. Default `3`. Not changeable while a game runs.
- What it does: scales render targets in the texture cache (`Settings::TranslateResolutionInfo`,
  applied when the GPU is created), and raises anisotropy in Automatic mode.
- Mario Kart World: the game already renders 2560x1440 docked and upscales on its own; the
  emulator is limited by one CPU thread, not by the host GPU. Above 1X multiplies render target
  memory (and the Conservative VRAM cap grows by 1 GiB per unit of scale); below 1X causes
  artifacts. Untested on this title.
- Recommended: (a) and (b) `3` 1X.

### VSync Mode:

- `use_vsync`: `0` Immediate (VSync Off), `1` Mailbox, `2` FIFO (VSync On), `3` FIFO Relaxed.
  Default `2`. The list shows only the modes the driver offers.
- What it does: the Vulkan present mode (`ChooseSwapPresentMode`), with two overrides: with frame
  generation on it becomes FIFO unless FIFO Relaxed was chosen; with the speed limit off or Turbo
  active, FIFO/FIFO Relaxed become Mailbox (or Immediate).
- Mario Kart World: the game is paced to 60 Hz by `PaceToVblank` whatever this is. Without
  asynchronous presentation the present (and its wait) runs on the GPU thread, the bottleneck: with
  FIFO, a frame that misses a refresh waits for the next one (on a 60 Hz screen, 60 drops to 30).
  FIFO Relaxed shows a late frame at once (it may tear instead). On a VRR screen inside its range
  both behave the same. Mailbox (marked "Recommended" in the list) never waits for a refresh
  either; not tested with this game.
- Recommended: (a) and (b) `3` FIFO Relaxed (as tested); on (a) `2` FIFO is equivalent while VRR
  is active.

### Window Adapting Filter: / FSR Sharpness:

- `scaling_filter`: `0` Nearest Neighbor, `1` Bilinear, `2` Bicubic, `3` Gaussian, `4` Lanczos,
  `5` ScaleForce, `6` AMD FidelityFX Super Resolution, `7` Area, `8` Zero-Tangent, `9` B-Spline,
  `10` Mitchell, `11` Spline-1, `12` MMPX, `13` Snapdragon GSR, `14` Snapdragon GSR EdgeDir,
  `15` AMD FidelityFX Super Resolution 3 (new). Default `0`. F8 / Home+L cycles it.
  `fsr_sharpening_slider`: 0-200, default `25` (FSR, FSR 3 and SGSR only).
- What it does: the filter that scales the final picture to the window
  (`src/video_core/renderer_vulkan/vk_blit_screen.cpp` `BlitScreen::SetWindowAdaptPass`,
  `src/video_core/renderer_vulkan/present/layer.cpp`). FSR is EASU + RCAS (spatial). The new FSR 3
  filter (`present/fsr3.cpp`) computes its own motion vectors with an optical-flow pass between
  frames and accumulates detail over time, then sharpens.
- Mario Kart World: the game's picture is 2560x1440 docked (cropped from 3840x2160 images by
  `INx2Vi2Session::Composite`), so a 4K screen scales it by 1.5: Nearest Neighbor gives uneven
  pixels. FSR 3 here is temporal without the game's motion vectors: ghosting is possible; untested.
- Recommended: (a) `6` FSR, sharpness `25`; (b) `1` Bilinear, or `7` Area when the window is smaller
  than the game's picture. **[VISUAL]** for `15` FSR 3 until tested.

### Anti-Aliasing Method:

- `anti_aliasing`: `0` None, `1` FXAA, `2` SMAA. Default `0`. A post-process pass on the final
  picture (`present/layer.cpp`), on top of the game's own anti-aliasing.
- Recommended: (a) and (b) `0` None.

### Fullscreen Mode: / Aspect Ratio:

- `fullscreen_mode`: `0` Borderless Windowed (default on Windows), `1` Exclusive Fullscreen.
- `aspect_ratio`: `0` Default (16:9), `1` 4:3, `2` 21:9, `3` 16:10, `4` Stretch to Window. Default
  `0` (`src/core/frontend/framebuffer_layout.cpp`).
- Recommended: (a) Borderless; if VRR does not engage, enable *Variable refresh rate* in Windows'
  graphics settings or use Exclusive. (b) Borderless. Aspect `0`.

### Use asynchronous GPU emulation **[PERF]**

- `use_asynchronous_gpu_emulation`: default `true`. Not changeable while a game runs.
- What it does: off, every GPU command the guest submits blocks until the GPU thread has executed
  it (`src/video_core/gpu_thread.cpp` `ThreadManager::PushCommand`).
- Recommended: (a) and (b) `true`.

### Enable post-processing effects

- `post_shader_enabled` (default `true`), `post_shader_chain`, `post_shader_preset` (set by
  *Configure Effects...*): ReShade FX effects applied to the final picture
  (`src/video_core/post_processing/fx_chain.cpp`). With no effect configured nothing runs.
- Recommended: leave as is; effects cost host GPU time only.

### Frame generation (new on 2026-10-31)

| Label | Key | Values (default) |
| --- | --- | --- |
| Enable Frame Generation | `frame_gen` | `false` |
| Frame Generation Type | `frame_gen_type` | `0` Lossless Scaling (LSFG), `1` AMD FSR 3 (`1`) |
| Frame Generation Multiplier | `frame_gen_multiplier` | 2-4 (`2`) |
| Target Frame Rate | `frame_gen_target_rate` | 0-240, 0 = fixed multiplier (`0`) |
| Auto Flow Scale / Flow Scale | `frame_gen_flow_scale_auto`, `frame_gen_flow_scale` | `true`, 25-100 % (`75`) |
| Queue Target | `frame_gen_queue_target` | 0-2 (`0`) |
| Dump Optical Flow | `frame_gen_dump_flow` | `false` (debug) |

- What it does: presentation only. After each real frame, `RendererVulkan::Composite`
  (`src/video_core/renderer_vulkan/renderer_vulkan.cpp`) asks `FrameGen::Process` /
  `GenerateInto` (`present/frame_gen.cpp`) for 1-3 interpolated frames and presents each one;
  `FrameGenPacer::Plan` adds frames up to the target rate when one is set. LSFG needs your own
  `Lossless.dll` in `%APPDATA%\eden\lossless\`; FSR 3 is built in (`present/fsr3_frame_gen.cpp`).
  VSync becomes FIFO (unless FIFO Relaxed); the present queue grows to (generations + 1) x
  (queue target + 1) frames, at most 7 (`PresentManager::SetImageCount`), so latency grows with the
  multiplier and the queue target.
- Mario Kart World: does not change game speed (the game still presents at 60 Hz). Interpolated
  frames can warp the HUD and fast objects **[VISUAL]**. The recording and, without asynchronous
  presentation, every extra present (with its vsync wait) run on the GPU thread, which is this
  game's bottleneck: real frames may get slower. Untested in this fork; the alternative already
  used is the driver's AMD Fluid Motion Frames (known-issues.md).
- Recommended: (a) off; to try it on the 120 Hz TV: FSR 3, multiplier 2, target 0, asynchronous
  presentation on, VSync FIFO Relaxed. (b) off.

### Background Color

`bg_red`, `bg_green`, `bg_blue` (0-255, default 0): the colour around the picture.

## Graphics > Advanced tab

*Emulation > Configure > Graphics > Advanced.* All keys in `[Renderer]`.

### GPU Mode: **[PERF]**

- `gpu_accuracy`: `0` Fast, `1` Accurate. **Default `1` Accurate on Windows** (`0` on Android).
  Changeable while a game runs: F9 / Home+R or the status bar button toggle it, and the change is
  saved.
- What it does (`Settings::IsGPULevelHigh`, refreshed at the end of each command list):
  - **Turns the split GPU thread off**: `GpuSplit::MaybeStart` only runs the second GPU thread
    with Fast (and stops it, mid-game, when Accurate is selected). The split is the main speed-up
    of the Switch 2 work: 1.46x GPU-thread throughput, frames 17 % shorter (NX2_NOTES row 145).
  - Command lists read with cache-flushing safe reads (DMA Accuracy Default follows this).
  - Macro parameters re-read from memory before each call, so the stack copy fast path is skipped
    (`Maxwell3D::ProcessMacro` / `RefreshParametersImpl`).
  - CPU-side flushes also flush the texture cache, invalidations flush first
    (`RasterizerVulkan::MustFlushRegion`, `FlushAndInvalidateRegion`,
    `ThreadManager::FlushAndInvalidateRegion`).
  - Precise occlusion queries (`vk_query_cache.cpp`); empty fences no longer skipped and query
    reports kept tracked (GPU Fence Behavior Default follows this).
- Mario Kart World: Eden's defaults (Accurate among them) gave 8 fps where Fast and the other
  recommendations gave ~20 on a weak PC (NX2_NOTES row 128); no rendering problem was found with
  Fast.
- Recommended: (a) and (b) `0` Fast.

### DMA Accuracy: **[PERF]**

- `dma_accuracy`: `0` Default, `1` Unsafe (fast), `2` Safe (stable). Default `0`.
- What it does: how GPU command lists are read (`src/video_core/dma_pusher.cpp` `DmaPusher::Step`):
  Safe flushes GPU-written data before reading, Unsafe reads memory as is, Default does what GPU
  Mode says (Accurate = Safe). Safe also keeps the split GPU thread off (`GpuSplit::MaybeStart`).
  Nothing else reads it.
- Recommended: (a) and (b) `0` Default (with GPU Mode Fast this is Unsafe).

### GPU Fence Behavior: **[PERF]**

- `gpu_fence_behavior`: `0` Default, `1` Immediate, `2` Balanced, `3` Accurate. Default `0`.
- What it does: Default follows GPU Mode (Fast = Immediate). Balanced and Accurate never skip empty
  fences and keep fence-signalled query reports tracked so that CPU reads of them flush
  (`src/video_core/fence_manager.h` `FenceManager::SignalFence`, `query_cache.h`), and make the
  GPU thread wait for the host GPU after every buffer copy that will be downloaded
  (`runtime.Finish()` in `buffer_cache.h`); Accurate also synchronizes buffer writes on every
  buffer lookup (`BufferCache::FindBuffer`).
- Recommended: (a) and (b) `0` Default.

### VRAM Usage Mode:

- `vram_usage_mode`: `0` Conservative, `1` Aggressive. Default `0`.
- What it does: sets the "Available VRAM" figure (`Device::CollectPhysicalMemoryInfo` in
  `src/video_core/vulkan_common/vulkan_device.cpp`): on a discrete card, the driver budget of the
  device-local heaps minus min(budget/8, 1 GiB), capped in Conservative at 6 GiB + 1 GiB x the
  resolution scale (7 GiB at 1X).
- Mario Kart World: almost nothing follows that figure for a Switch 2 title in this fork. The
  texture collector's thresholds follow the driver's own budget, re-read every 30 frames
  (`TextureCache::TickFrame`); the buffer collector runs on its own buffer size (1 / 2 GiB,
  `BufferCache::TickFrame`); automatic BC3 reads the budget too. What is left: the per-layer cap of
  large terrain arrays (384 MiB under 7 GiB of Available VRAM, else 768 MiB: same result with both
  modes on 16 GB and on 8 GB cards) and the "Available VRAM" line of the log. On an 8 GB card both
  modes give the same figure.
- Recommended: (a) `1` Aggressive, (b) `0` Conservative; either is harmless.

### NVDEC emulation:

- `nvdec_emulation`: `0` No Video Output, `1` CPU Video Decoding, `2` GPU Video Decoding. Default
  `1` CPU (the list labels GPU "(Default)", which is wrong).
- What it does: Switch 2 movies are decoded by FFmpeg in `src/video_core/host1x/nx2_video_decoder.cpp`;
  `2` adds FFmpeg hardware decoding. That decoder ignores `0`, which only affects Switch 1 NVDEC
  (`nvdec.cpp`, `vic.cpp`).
- Mario Kart World: the cup-opening VP9 movies (2560x1440) decode in real time on the CPU (NX2_NOTES
  row 148). GPU decoding is untested.
- Recommended: (a) and (b) `1` CPU.

### Anisotropic Filtering:

- `max_anisotropy`: `0` Automatic, `1` Default, `2` 2x, `3` 4x, `4` 8x, `5` 16x. Default `0` on
  Windows.
- What it does: `TSCEntry::MaxAnisotropy` (`src/video_core/textures/texture.cpp`) doubles the
  game's anisotropy once per step, on samplers that already use it or are mipmapped, linearly
  filtered and without depth compare: Default adds nothing, 2x-16x add 1-4 steps, Automatic adds
  one step per unit of resolution scale above 1X (nothing at 1X).
- Mario Kart World: sharper roads at grazing angles; the host GPU has spare time. Not tested for
  artifacts.
- Recommended: (a) `0` (or `5` 16x for quality), (b) `0`.

### ASTC Decoding Method:

- `accelerate_astc`: `0` CPU, `1` GPU, `2` CPU Asynchronous. Default `1`.
- What it does (`src/video_core/renderer_vulkan/vk_texture_cache.cpp`): GPU decodes ASTC with a
  compute shader and, with recompression on, re-encodes to BC1/BC3 on the GPU in the same pass
  (`WillUseGpuBcnTranscode`). CPU decodes (and re-compresses) on the ImageTranscode threads, which
  compete with the emulator's own threads. CPU Asynchronous does it in the background and draws
  with the image unfinished meanwhile **[VISUAL]**. Huge ASTC arrays (terrain) always use the GPU
  decoder (`IsHugeAstcArray`).
- Recommended: (a) and (b) `1` GPU.

### ASTC Recompression Method:

- `astc_recompression`: `0` Uncompressed (Best quality), `1` BC1 (Low quality), `2` BC3 (Medium
  quality). Default `0`.
- What it does: PC GPUs cannot read ASTC. Uncompressed stores the decoded RGBA8 (4-8x the size),
  BC3 re-compresses to 1 byte per texel, BC1 to half a byte without real alpha
  (`src/video_core/texture_cache/util.cpp` `ConvertImage`). With BC3, ASTC whose blocks carry no
  alpha goes to BC1 anyway (`TextureCacheRuntime::UseOpaqueAstcFormat`, `EDEN_NX2_ASTC_BC1=0` to
  keep BC3). For a Switch 2 title on a card whose budget is under 12 GiB at boot, Uncompressed is
  replaced by BC3 for the session (`UseBc3ForSwitch2OnSmallCards`, warning in the log;
  `EDEN_NX2_KEEP_ASTC=1` opts out); the configuration file is not changed.
- Mario Kart World: with Uncompressed the 16 GB RX 6900 XT reaches its 13.5 GB budget mid-race,
  then garbage textures and a crash (NX2_VRAM.md); its budget is above 12 GiB, so the automatic
  BC3 does **not** apply there. BC3 keeps it near 11 GB. BC1 loses transparency on textures that
  have some **[VISUAL]**.
- Recommended: (a) `2` BC3 (must be set by hand), (b) `2` BC3 (automatic anyway).

### Frame Pacing Mode (Vulkan only)

- `frame_pacing_mode`: `0` Auto, `1` 30 FPS, `2` 60 FPS, `3` 90 FPS, `4` 120 FPS. Default `0`.
- What it does: when acquiring a swapchain image (`Swapchain::AcquireNextImage`), Auto only waits
  for the GPU to be done with that image; the others also sleep to cap presents at that rate
  (`Scheduler::Wait`, only while Limit Speed is checked). It runs on the presenting thread (the
  GPU thread unless asynchronous presentation is on).
- Mario Kart World: 60 duplicates the 60 Hz pacing the game already has; 30 halves the frame rate
  (game speed is unchanged, it follows the clock); 90/120 do nothing at 60 fps.
- Recommended: (a) and (b) `0` Auto.

### Other Advanced settings

| Label | Key | Default | What it does | Recommended |
| --- | --- | --- | --- | --- |
| Sync Memory Operations | `sync_memory_operations` | `false` | After a command list flagged "sync", drains the split and makes the next one wait for a GPU fence (`DmaPusher::Step`) **[PERF]** | `false` |
| Force maximum clocks (Vulkan only) | `force_max_clock` | `false` | A thread keeps submitting dummy compute work while the game submits (`vk_turbo_mode.cpp`, AMD/NVIDIA/Intel and mobile drivers): keeps clocks up, costs power and a CPU thread; not measured | `false` |
| Use persistent pipeline cache | `use_disk_shader_cache` | `true` | Saves shaders to `%APPDATA%\eden\cache\shader\0400c3f00006e000\` and rebuilds them at boot (`EmuThread::run` > `LoadDiskResources`) | `true` |
| Use Vulkan pipeline cache | `use_vulkan_driver_pipeline_cache` | `true` | Keeps the driver's compiled pipelines (`vulkan_pipelines.bin`, written atomically) | `true` |
| Enable Compute Pipelines (Intel Vulkan Only) | `enable_compute_pipelines` | `false` | Shown and used only for Intel's proprietary driver (`Device`, `has_broken_compute`) **[NO EFFECT]** on AMD/NVIDIA | `false` |
| Sync to framerate of video playback | `use_video_framerate` | `false` | Switch 1 vi only (`Conductor::GetNextTicks`) **[NO EFFECT]** | `false` |
| Enable Reactive Flushing | `use_reactive_flushing` | `true` on Windows | On: pages the GPU caches are protected against CPU reads too, and a read of GPU-written memory waits for the GPU thread to flush it (`Memory::Impl::RasterizerMarkRegionCached`, `GPU::Impl::OnCPURead`). Off: pitch-linear images are downloaded ahead instead (`ImageInfo`). In this game on cost ~12 % of the main core in slow reads, off showed no visual difference (NX2_NOTES rows 115, 128) **[PERF]** | `false` |
| Barrier feedback loops | `barrier_feedback_loops` | `true` | Inserts a barrier when a draw samples its own render target (`TextureCache::CheckFeedbackLoop`); correctness, cheap | `true` |
| Enable buffer history | `enable_buffer_history` | `false` | Switch 1 buffer queue only (`BufferQueueProducer`) **[NO EFFECT]** | `false` |

## Graphics > Extras tab

*Emulation > Configure > Graphics > Extras*: a *Hacks* group and a *Vulkan Extensions* group. All
keys in `[Renderer]`.

### Enable asynchronous shader compilation **[PERF]**

- `use_asynchronous_shaders`: default `false`. Not changeable while a game runs.
- What it does: when a graphics pipeline is not built yet, its draws are skipped (if depth is on,
  or more than 6 vertices/indices) instead of waiting (`PipelineCache::BuiltPipeline`); the driver
  compiles it on worker threads. Shader translation itself stays on the GPU thread.
- Mario Kart World: the driver compile is 40-100 ms per new pipeline (experiments.md): off, each one
  freezes the GPU thread that long. On, objects can be missing for a few frames the first time a
  scene is seen **[VISUAL]** (harmless, gone once cached).
- Recommended: (a) and (b) `true`.

### Enable asynchronous presentation (Vulkan only)

- `async_presentation`: default `false` on Windows (`true` on Android). Not changeable while a game
  runs.
- What it does: a present thread acquires swapchain images and presents
  (`src/video_core/renderer_vulkan/vk_present_manager.cpp` `PresentManager::PresentThread`).
  Without it, the GPU thread waits for the Vulkan worker, acquires the image (with the frame
  pacing sleep) and presents itself (`PresentManager::Present`).
- Mario Kart World: the GPU thread is the bottleneck, so taking presents off it should help,
  mostly with FIFO or frame generation. A race between the present thread and the split was fixed
  (NX2_NOTES row 143); the fork's tests ran with it off.
- Recommended: (a) `false`, worth an A/B test with `true` (needed if you try frame generation);
  (b) `false`.

### Other hacks

| Label | Key | Default | What it does | Recommended |
| --- | --- | --- | --- | --- |
| Skip CPU Inner Invalidation | `skip_cpu_inner_invalidation` | `false` | Checked only by the default `RasterizerInterface::InnerInvalidation`, which the Vulkan rasterizer overrides without looking at it **[NO EFFECT]** on Vulkan; on OpenGL it skips cache invalidations **[VISUAL]** | `false` |
| Fix bloom effects | `fix_bloom_effects` | `false` | Desktop: depth-to-colour copies go through a colour blit (`TextureCacheRuntime::ConvertImage`); the shader part is Android only. A Burnout fix **[VISUAL]** | `false` |
| GPU Unswizzle | `gpu_unswizzle_enabled` | `false` | Unswizzles BCn 3D textures with a compute pass | `false` |
| GPU Unswizzle Max Texture Size | `gpu_unswizzle_texture_size` | `3` Large (256 MB); 0-4 = 16/32/128/256/512 MB | Largest texture sent to that pass | default |
| GPU Unswizzle Stream Size | `gpu_unswizzle_stream_size` | `3` Medium (32 MB); 0-4 = 4/8/16/32/64 MB | Data per frame | default |
| GPU Unswizzle Chunk Size | `gpu_unswizzle_chunk_size` | `3` Medium (256); 0-4 = 32/64/128/256/512 slices | Slices per dispatch | default |

The unswizzle sizes are read only when GPU Unswizzle is on (`TextureCache` constructor).

### Vulkan Extensions

| Label | Key | Values (default) | What it does | Recommended |
| --- | --- | --- | --- | --- |
| Extended Dynamic State | `dyna_state` | `0` Disabled, `1` EDS1, `2` EDS2, `3` EDS3 (`2` on Windows) | Removes the extended dynamic state extensions above the level (`Device` constructor): fewer distinct pipelines with more. **Cached pipelines built under another level are skipped at boot and rebuilt during play** (`PipelineCache::LoadDiskResources`) | `2` |
| Vertex Input Dynamic State | `vertex_input_dynamic_state` | `true` on Windows | Uses `VK_EXT_vertex_input_dynamic_state`; also part of the pipeline cache key, so changing it skips the cache too | `true` |
| Sample Shading | `sample_shading_fraction` | 0-100 % (`0`) | Per-sample shading of multisampled draws, `minSampleShading` = the fraction (`GraphicsPipeline::MakePipeline`): host GPU cost | `0` |

## Audio tab

*Emulation > Configure > Audio.*

| Label | `[group] key` | Values (default) | Notes | Recommended |
| --- | --- | --- | --- | --- |
| Output Engine: | `[Audio] output_engine` | `0` auto, `1` cubeb, `2` sdl3, `3` null (`0`) | Host audio backend | `0` |
| Output Device: / Input Device: | `[Audio] output_device`, `input_device` | names (`auto`) | | `auto` |
| Sound Output Mode: | `[System] sound_index` | `0` Mono, `1` Stereo, `2` Surround (`1`) | Stored but read by nothing in this build **[NO EFFECT]** | default |
| Volume: | `[Audio] volume` | 0-200 % (`100`) | Volume- / Volume+ hotkeys too | any |
| Mute audio | `[Audio] audio_muted` | `false` | Ctrl+M | any |
| Mute audio when in background | `[UiAudio] muteWhenInBackground` | `false` | | any |

## Controls tab

*Emulation > Configure > Controls.* Apart from Docked/Handheld, none of these affect speed or
rendering.

- **Docked / Handheld** (`[System] use_docked_mode`, `0` Handheld, `1` Docked, default `1` on
  Windows; also F10 / Home+X and the status bar button, saved). Mario Kart World draws 2560x1440
  docked and 1920x1080 handheld, and `INx2Vi2Session::Composite` crops its present images to that.
  Since the merge, switching during play reaches the game. If player 1 is a "Handheld" controller,
  the per-game dialog forces handheld (`ConfigurePerGame::ApplyConfiguration`). Handheld lowers
  host GPU load and render target memory a little, not the CPU-bound GPU thread. Recommended:
  Docked.
- Vibration (`vibration_enabled`, `true`), accurate vibration (`enable_accurate_vibrations`), Motion
  (`motion_enabled`, `true`), player mappings, and the *Advanced* page: Mouse (`mouse_enabled`),
  Keyboard (`keyboard_enabled`), Touchscreen, Debug Controller (`debug_pad_enabled`), Ring
  Controller (`enable_ring_controller`, `true`), Infrared Camera (`enable_ir_sensor`), Emulate
  Analog with Keyboard Input (`emulate_analog_keyboard`), Enable XInput 8 player support (disables
  web applet) (`enable_raw_input`), Enable UDP controllers (`enable_udp_controller`,
  `udp_input_servers`), Controller navigation (`controller_navigation`, `true`), Enable direct
  JoyCon driver (`enable_joycon_driver`, `true`), Enable direct Pro Controller driver
  (`enable_procon_driver`), Use random Amiibo ID (`random_amiibo_id`), Disable SDL WGI/XInput
  (`disable_wgi_xinput`), mouse panning (`mouse_panning*`; `mouse_panning_sensitivity` is read by
  nothing), touch (`touch_device`, `touch_from_button_map`), TAS (`tas_enable`, `tas_loop`,
  `pause_tas_on_load`, `tas_show_recording_dialog`). All in `[Controls]`; set them for your
  controller, they have no performance effect.

## Applets tab

*Emulation > Configure > System > Applets.* `[LibraryApplet]` keys `<name>_applet_mode`: `0` Custom
frontend (HLE), `1` Real applet (LLE, needs firmware in the NAND), `2` Disabled (offered for the
controller applet only: no controller dialog is shown). A real applet that is not installed falls
back to the custom frontend (`ILibraryAppletCreator::CreateLibraryApplet`).

| Label | Key prefix | Default |
| --- | --- | --- |
| Amiibo editor | `cabinet` | `1` |
| Controller configuration | `controller` | `0` |
| Error | `error` | `1` |
| Player select | `player_select` | `1` |
| Software keyboard | `swkbd` | `0` |
| Mii Edit | `mii_edit` | `1` |
| Online web | `web` | `0` |
| Photo viewer | `photo_viewer` | `1` |
| Offline web | `offline_web` | `1` |
| Hidden ("untested"): Data erase, Net connect, Shop, Login share, Wifi web auth, My page | `data_erase`, `net_connect`, `shop`, `login_share`, `wifi_web_auth`, `my_page` | `0`, `1`, `0`, `0`, `0`, `1` |

*Enable Overlay Applet* (`enable_overlay`, `false`): starts Horizon's overlay applet (hold Home for
1 s). Recommended: all defaults.

## General, Game List, Hotkeys, Web, Network, Filesystem

| Tab | Settings | Notes for this game |
| --- | --- | --- |
| General | Confirm before stopping (`[UI] confirmStop`, `0` always / `1` if the game asks / `2` never), Pause when not in focus (`pauseWhenInBackground`, `false`), Hide mouse on inactivity (`hideInactiveMouse`, `true`), Prompt for user profile on boot (`select_user_on_boot`), Check for updates (`check_for_updates`) | Pausing stops the emulated CPUs, not the guest counter (it follows the host clock): after a pause the game sees the whole pause as elapsed time |
| Game List | Columns, icon sizes, row texts, list mode, metadata cache (`[UIGameList]`), screenshots path (`[Screenshots]`) | None |
| Hotkeys | Key and controller bindings | See [hotkeys](#hotkeys-and-status-bar-buttons-that-change-settings) |
| Web | Username, token (`[WebService] eden_username`, `eden_token`), Discord status | None |
| Network | Network Interface (`[Network] network_interface`), Enable Airplane Mode (`airplane_mode`, `true` = no guest network) | Keep `true`: online play is not emulated |
| Filesystem | NAND / SD / save / load / dump paths, game card (`[Data%20Storage] gamecard_inserted`, `gamecard_current_game`, `gamecard_path`), Dump ExeFS, Dump Decompressed NSOs (`dump_exefs`, `dump_nso`) | Dumps write files at boot only |

## Debug tab

*Emulation > Configure > General > Debug.* All `[Debugging]` unless noted. Labels marked "**" in the
dialog are reset when the emulator closes. None should be on while playing.

| Label | Key | Default | Effect |
| --- | --- | --- | --- |
| Global Log Filter | `[Miscellaneous] log_filter` | `*:Info` | What goes to `%APPDATA%\eden\log\eden_log.txt`; Debug levels cost time |
| Show Log in Console / Flush log output on each line | `[UI] showConsole`, `[Miscellaneous] flush_line` | `false` | Flushing every line is slow |
| Enable Extended Logging** | `extended_logging` | `false` | Log size limit 1 GB instead of 100 MB |
| Censor username in logs | `[Miscellaneous] censor_username` | `true` | |
| GPU Logging / Level, Dump SPIR-V Shaders | `gpu_log_level` (`0` Off ... `4` All), `gpu_log_shader_dumps`; also `gpu_log_vulkan_calls`, `gpu_log_memory_tracking`, `gpu_log_driver_debug`, `gpu_log_ring_buffer_size` (ini only) | Off | GPU logging; the four ini-only switches act only when the level is not Off **[PERF]** |
| Enable GDB Stub / Port | `use_gdbstub`, `gdbstub_port` | `false`, 6543 | Debugger |
| Homebrew Args | `program_args` | empty | Homebrew only |
| Enable Graphics Debugging | `[Renderer] debug` | `false` | Vulkan validation and debug names **[PERF]** |
| Enable Shader Feedback | `[Renderer] shader_feedback` | `false` | Pipeline statistics |
| Enable Nsight Aftermath | `[Renderer] nsight_aftermath` | `false` | NVIDIA crash dumps |
| Disable Loop safety checks | `[Renderer] disable_shader_loop_safety_checks` | `false` | Removes the iteration guard from shader loops: a bad loop can hang the GPU |
| Enable Renderdoc Hotkey | `[Renderer] renderdoc_hotkey` | `false` | RenderDoc capture hotkey |
| Disable Buffer Reorder | `[Renderer] disable_buffer_reorder` | `false` | Uploads kept in draw order (debugging) |
| Dump Guest (Maxwell) Shaders | `dump_guest_shaders` | `false` | Dumps shaders (Switch 2 programs have `EDEN_NX2_SHADER_DUMP`) |
| Dump Maxwell Macros, Disable Macro JIT, Disable Macro HLE | `dump_macros`, `disable_macro_jit`, `disable_macro_hle` | `false` | Switch 1 (Maxwell) macros only: Switch 2 macros run in `MacroEngine::Execute`'s native path before these are looked at **[NO EFFECT]** (use `EDEN_NX2_MME_JIT=0`) |
| Enable Auto-Stub | `use_auto_stub` | `false` | Switch 2 programs are always auto-stubbed (`ServiceFrameworkBase::ReportUnimplementedFunction`) **[NO EFFECT]** |
| Enable FS Access Log, Enable Verbose Reporting Services**, Dump Audio Commands To Console** | `enable_fs_access_log`, `reporting_services`, `dump_audio_commands` | `false` | Logs |
| Kiosk (Quest) Mode, Use dev.keys, Enable Debug Asserts, Enable All Controller Types, Disable Web Applet, Perform Startup Vulkan Check | `quest_flag`, `use_dev_keys`, `use_debug_asserts`, `enable_all_controllers`, `disable_web_applet` (`true`), `perform_vulkan_check` (`true`) | | Keep defaults |
| Battery Serial / Unit Serial | `serial_battery`, `serial_unit` | random | Reported by `set:sys` |
| Debug Knobs | `debug_knobs` | 0 | Read only by the Android frontend **[NO EFFECT]** |

## Hotkeys and status bar buttons that change settings

These act during play and the new value is saved with the configuration (default bindings from
`src/qt_common/config/uisettings.cpp`).

| Hotkey (controller) | Action | Setting |
| --- | --- | --- |
| Ctrl+U (Home+Y) | Toggle Framerate Limit | `use_speed_limit` **[SPEED]** |
| Ctrl+Z | Toggle Turbo Speed | speed = `turbo_speed_limit` **[SPEED]**, not saved |
| Ctrl+X | Toggle Slow Speed | speed = `slow_speed_limit` **[SPEED]**, not saved |
| F9 (Home+R), status bar GPU button | Change GPU Mode | `gpu_accuracy`: Accurate turns the split off **[PERF]** |
| F10 (Home+X), status bar button | Change Docked Mode | `use_docked_mode` |
| F8 (Home+L), status bar button | Change Adapting Filter | `scaling_filter` |
| Ctrl+M, -, = (Home+Dpad) | Mute, volume | `audio_muted`, `volume` |

The status bar also has buttons for anti-aliasing and the graphics API.

## Environment variables

Set them before starting the emulator (e.g. `set EDEN_NX2_SPLIT=1` in a command prompt, then
`eglantine.exe`). They are read once.

### Behaviour and performance switches

| Variable | Default | Effect (where) |
| --- | --- | --- |
| `EDEN_NX2_SPLIT` | `2` | `0` one GPU thread, `1` engine methods on a second thread, `2` also 3D/compute macros on the first (`GpuSplit::MaybeStart`). Still off with GPU Mode Accurate, DMA Safe or OpenGL. `1` is the workaround for "Native MME ... parameter FIFO underflow" freezes |
| `EDEN_NX2_CNTFRQ=<Hz>` | 31250000 for Switch 2 | Guest counter frequency (`System::Impl::Load`). **[SPEED]**: `19200000` makes this game 1.63x too slow |
| `EDEN_NX2_VSYNC_HZ=<n>` | 60 | Rate of the game's vsync event (`INx2Vi2Session::GetVsyncEvent`); the game starts frames on it **[SPEED]** |
| `EDEN_NX2_FRAMES_AHEAD=0` | on | Wait for the current frame's fence at present instead of the previous one (slower) |
| `EDEN_NX2_PB_SNAPSHOT=0` | on | Execute command lists from memory instead of the words copied at submit: the picture steps back again (`nvhost_gpu.cpp`) |
| `EDEN_NX2_SPARSE_LAZY=0/1` | lazy, except on AMD's proprietary driver | `0` backs sparse regions whole, `1` forces memory only where the GPU writes (`INx2GpuSession::LazySparseRegions`) |
| `EDEN_NX2_KEEP_ASTC=1` | off | No automatic BC3 under 12 GiB |
| `EDEN_NX2_ASTC_BC1=0` | on | Opaque ASTC stays BC3 instead of BC1 (more video memory) |
| `EDEN_NX2_VRAM_LIMIT_MIB=<n>` | none | Every device-local heap treated as n MiB: allocations fail past it, budget and Available VRAM follow (testing small cards) |
| `EDEN_NX2_TC_EXPECTED_MIB`, `EDEN_NX2_TC_CRITICAL_MIB` | from the budget | Texture collector thresholds |
| `EDEN_NX2_TC_MIN_AGE=<frames>` | 0 | Images used more recently are never evicted |
| `EDEN_NX2_TRANSCODE_THREADS=<n>` | a quarter of the hardware threads, at least 2 (4 on a 5800X) | ImageTranscode threads (CPU texture decoding) |
| `EDEN_NX2_BUFFER_DOWNLOADS=0/1` | off for Switch 2 | Copy GPU-written buffer data back at every fence |
| `EDEN_NX2_ASTC_FLUSH=1` | off | GPU ASTC decodes submit without waiting for the GPU to idle |
| `EDEN_NX2_IDLE_SPIN_US=<n>` | 0 | Idle cores poll that long before blocking (no gain measured) |
| `EDEN_NX2_TENSOR=<any>` / `=stub` | off | Emulate tensor-core (HMMA) shader code / make it a no-op; rejected otherwise |
| `EDEN_NX2_AUTO_STUB=1` | Switch 2 always | Auto-stub unknown service commands for Switch 1 titles too |
| `EDEN_NX2_CROP_WIDTH`, `EDEN_NX2_CROP_HEIGHT` | game-specific | Override the crop of presented images |

### Optimizations that can be turned off (for comparisons)

`EDEN_NX2_FENCE_BATCH=0` (fences one at a time), `EDEN_NX2_MACRO_COPY=1` (macro parameters in a
vector), `EDEN_NX2_CNTPCT_PAUSE=0` (no PAUSE in counter polling loops), `EDEN_NX2_INLINE_DEFER=0`
(invalidate every cache at each inline write), `EDEN_NX2_SKIP_SAME=0` / `EDEN_NX2_SPLIT_SKIP=0`
(keep same-value register writes), `EDEN_NX2_MME_JIT=0` (interpret macros), `EDEN_NX2_MME_MEMO=0`
(no macro call memo), `EDEN_NX2_PARK_SPLIT_METHODS=1` (old handling of methods split across
entries). Each one makes the game slower or restores an old bug; NX2_NOTES gives the measurements.

### Diagnostics (logs, dumps; slow, not for playing)

| Variable | Output |
| --- | --- |
| `EDEN_NX2_SVC_TRACE=1` or `=<first>:<last>` | Every SVC (or for those presents) |
| `EDEN_NX2_IPC_TRACE=1`, `EDEN_NX2_IPC_SUMMARY=1` | Every service command / counts every 5 s |
| `EDEN_NX2_MEM_TRACE=1` | Memory layout and heap queries |
| `EDEN_NX2_GPU_PROFILE=1` | GPU-thread zone timers every 10 s |
| `EDEN_NX2_PB_CHECK=1`, `EDEN_NX2_PB_DUMP=<file>` | Command list entries changed after submit / whole list to a file |
| `EDEN_NX2_PULLER_TRACE`, `EDEN_NX2_METHOD_TRACE`, `EDEN_NX2_3D_METHODS`, `EDEN_NX2_QUERY_TRACE`, `EDEN_NX2_QMD_TRACE` | GPU method, report and compute queue traces |
| `EDEN_NX2_SEM_WATCH=<gpu address>`, `EDEN_NX2_SEM_WATCH_EMITS` | Everything touching a semaphore |
| `EDEN_NX2_RT_TRACE`, `EDEN_NX2_TRACE_DST=<hex>`, `EDEN_NX2_TRACE_PS`, `EDEN_NX2_TRACE_VS=<address>`, `EDEN_NX2_TRACE_AFTER_UPSCALE`, `EDEN_NX2_WINDOW_TRACE`, `EDEN_NX2_WINDOW_TRACE_SKIP`, `EDEN_NX2_CB24_TRACE` | Draw and copy traces |
| `EDEN_NX2_TEXTURE_DUMP=<file>`, `EDEN_NX2_FORCE_LOD0` | Texture memory dump / sample mip 0 only **[VISUAL]** |
| `EDEN_NX2_SKIP_VS=<addr,...>`, `EDEN_NX2_REJECT=<addr,...>` | Drop draws using these vertex shaders / treat these programs as rejected **[VISUAL]** |
| `EDEN_NX2_PIPELINE_DIFF=1` | What differs between pipelines sharing shaders |
| `EDEN_NX2_MME_LOG`, `EDEN_NX2_MME_DUMP`, `EDEN_NX2_MME_RECORD=<file>`, `EDEN_NX2_MME_RECORD_SKIP=<n>`, `EDEN_NX2_MME_MEMO_STATS` | Macro calls and code |
| `EDEN_NX2_SPLIT_VERIFY=1`, `EDEN_NX2_SPLIT_CHECK=1` | Split register comparison / thread-ownership check |
| `EDEN_NX2_SHADER_DUMP=<dir>`, `EDEN_NATIVE_SHADER_OVERRIDE_DIR=<dir>` | Native programs to files / override rejected ones |
| `EDEN_NX2_FRAME_CAPTURE=<dir>`, `EDEN_NX2_DUMP=<present>:<hex addr>:<hex size>[,...]`, `EDEN_NX2_VDEC_DUMP=<dir>` | Every 120th frame as TGA / GPU ranges at a present / decoded movie frames |
| `EDEN_NX2_UPSCALE_SOURCE`, `EDEN_NX2_UPSCALE_SOURCE_FORMAT`, `EDEN_NX2_SAMPLE_AT` | Upscale pass debugging |
| `EDEN_NX2_AUDIO_LEVEL=1` | Audio output peak every ~2 s |
| `EDEN_NATIVE_DRAW_DIAGNOSTICS`, `EDEN_NATIVE_DRAW_CAPTURE`, `EDEN_NATIVE_FRAME_CAPTURE` | Native draw/QMD diagnostics and captures |

### eden-cli only

`EDEN_NX2_CRASH_DUMP=<dir>` (crash stack and minidump), `EDEN_NX2_HIDE_WINDOW=1`,
`EDEN_NX2_KEY_SCRIPT=...`, `EDEN_NX2_KEY_RECORD=<file>`, `EDEN_NX2_SAMPLE=<threads>` with
`EDEN_NX2_SAMPLE_START`, `EDEN_NX2_SAMPLE_SECONDS`, `EDEN_NX2_SAMPLE_OUT` (sampling profiler).

## Settings that change game speed or timing

Mario Kart World's game time follows the guest counter (CNTPCT at 31.25 MHz): CPU Clocks Boost made
it run ~1.7x too fast, the old 19.2 MHz counter 1.63x too slow. Anything that scales the counter
changes the game's speed. Its presents are paced to 60 Hz by `PaceToVblank` and it starts frames on
a 60 Hz vsync event.

| Setting | Effect | Safe value |
| --- | --- | --- |
| CPU Clocks `fast_cpu_time` | Counter x1.70 (Boost) / x2.00 (Overclock): the game runs that much too fast. A change during play makes the counter jump | `0` |
| Synchronize Core Speed `sync_core_speed` | Counter / (speed/100): with Turbo/Slow or a speed other than 100 % the game's clock runs at the wrong rate and jumps when the speed changes | `false` |
| Multicore CPU Emulation `use_multi_core` | Off: the counter follows emulated instructions | `true` |
| Custom CPU Ticks `use_custom_cpu_ticks`, `cpu_ticks` | Single core only: ticks per block, i.e. the clock rate | `false` |
| Limit Speed `use_speed_limit`, `speed_limit`, Turbo/Slow and their hotkeys | Present pacing at 60 Hz x speed/100; off = unpaced | `true`, `100` |
| GPU Clocks `fast_gpu_time` | GPU timestamps / 256 or / 512: GPU time measurements only, not game speed; a change during play makes them jump | `1` |
| Frame Pacing Mode `frame_pacing_mode` | 30 halves the frame rate (not the speed) | `0` |
| VSync FIFO `use_vsync=2` | Late frames wait a refresh (frame rate, not speed) | `3` |
| Frame generation `frame_gen*` | Extra presented frames and latency; not the speed | off |
| `EDEN_NX2_CNTFRQ`, `EDEN_NX2_VSYNC_HZ` | Counter frequency / vsync event rate | unset |

Not speed-related, although the names suggest it: Custom RTC (date and time only), Sync to framerate
of video playback (Switch 1 only), Turbo/Slow speed values while their mode is off.

## Settings that cost performance for nothing

In this game, from the code and the measurements cited above:

| Setting | Cost | Use instead |
| --- | --- | --- |
| GPU Mode Accurate (**the default**) | Split GPU thread off (1.46x throughput lost), safe reads, macro parameter re-reads, extra flushes | `gpu_accuracy=0` |
| DMA Accuracy Safe | Split GPU thread off, flushes before every command list read | `dma_accuracy=0` |
| GPU Fence Behavior Balanced / Accurate | GPU thread waits for the host GPU after buffer copies, synchronizes on buffer lookups | `gpu_fence_behavior=0` |
| Enable Reactive Flushing (**the default**) | CPU reads of GPU-tracked pages fault (~12 % of the main core) | `use_reactive_flushing=false` |
| Asynchronous shader compilation off (**the default**) | 40-100 ms freeze per new pipeline | `use_asynchronous_shaders=true` |
| ASTC Recompression Uncompressed (**the default**) | Video memory overflow on 16 GB: garbage, then a crash | `astc_recompression=2` |
| ASTC Decoding CPU / CPU Asynchronous | CPU decoding and BC compression next to the emulator's threads | `accelerate_astc=1` |
| Sync Memory Operations | A drain and fence wait per flagged command list | `false` |
| Use asynchronous GPU emulation off | The guest waits for every GPU command | `true` |
| Multicore off, CPU Accuracy Accurate / Paranoid / Debugging | Slower JIT or one host thread | defaults |
| Resolution above 1X, Sample Shading, FXAA/SMAA | Host GPU time and memory for a CPU-bound game | defaults |
| Graphics debugging, shader feedback, GPU logging, dumps, Debug log levels | Validation and logging | off |
| Force maximum clocks | A busy thread and power; no measured gain | `false` |
| Changing Extended Dynamic State or Vertex Input Dynamic State | Every cached pipeline is rebuilt during play (stutter until the cache refills) | keep `2`, `true` |

And settings that do nothing for this game (no need to spend time on them): Skip CPU Inner
Invalidation (Vulkan), Enable buffer history, Sync to framerate of video playback, Sound Output
Mode, Enable Compute Pipelines (non-Intel), Backend NCE (Windows), Custom CPU Ticks (multicore),
Disable Macro JIT / HLE and Dump Maxwell Macros, Enable Auto-Stub, NVDEC "No Video Output", Debug
Knobs, Faster ASIMD instructions (32-bit only), and VRAM Usage Mode in practice.
