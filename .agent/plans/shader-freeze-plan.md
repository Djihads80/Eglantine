# Plan: eliminate shader-compile freezes (Eglantine, Vulkan, Ocarina of Time)

Status 2026-11-07. Author: Claude (with a read-only research pass over the code). Target executor: a
coding agent working in `E:\Projects\Eglantine` on branch `local-combined`. Read `AGENTS.md` first;
its rules apply (commit small on `local-combined`, trailer `Assisted-by: <model> (<what>)`, never
push, build with cmake, the user tests in `build\bin\eglantine.exe`). Line numbers below drift:
find code by the function name given; the line is only a hint.

## 1. What is measured (facts, not theories)

Title: Ocarina of Time `0400486024DEC000`, RTX 5090, NVIDIA driver, async shaders ON.

There are three separate costs, and only one of them is a multi-second freeze:

| # | Cost | Where | Size | Status |
|---|---|---|---|---|
| A | Shader translation (TranslateProgram ~70%, SPIR-V ~12%) on the GPU back thread | `PipelineCache::CreateGraphicsPipeline()` / `CreateComputePipeline()` in `vk_pipeline_cache.cpp` | 2-40 ms per pipeline. An area load with an empty cache: ~1,000 pipelines, ~3 s in a few frames, frames of 1.7-2.3 s | Mitigated by `shader_build_budget` (24 ms per frame, `c9b8bb5e52`, `e330d49488`): worst frame 0.6 s in the lab |
| B | Driver compile (`vkCreate*Pipelines` on the workers) that the **submission thread waits for** | the `scheduler.Record` wait lambda in `GraphicsPipeline::ConfigureDraw` (`vk_graphics_pipeline.cpp`) and `ComputePipeline::Configure` (`vk_compute_pipeline.cpp`) | compute 1.3-5.8 s each; graphics 0.1-1.9 s each. **Worker queue time is 0.0 ms in every case: this is pure driver time** | **Open. These are the freezes the user sees now.** |
| C | White silhouettes while builds are deferred | stock async skip in `PipelineCache::BuiltPipeline` plus the budget deferral | about 5-6 s at an 8 ms budget, shorter at 24 ms | Open |

Why B got so bad: NVIDIA's own disk cache (`%LOCALAPPDATA%\NVIDIA\GLCache\...`, keyed per
application) misses whenever the SPIR-V changes. Eglantine does not use a `VkPipelineCache` on
NVIDIA (`DriverCachesPipelines()` returns true). The SPIR-V changed twice in short order:
- `CACHE_VERSION` (`vk_pipeline_cache.cpp` ~line 79) was bumped **11 times from 2026-10-31 to
  2026-11-06**, and 9 of those bumps were not needed (translation-only changes). On a version
  mismatch, `LoadPipelines` (`shader_environment.cpp` ~1115) **deletes vulkan.bin**. Every pipeline
  is then translated on the GPU thread and compiled by the driver in play, not at boot.
- Six translation commits (2026-11-07 12:16-13:13) changed the SPIR-V without a bump. Boot then
  rebuilt the cached pipelines (the driver compiles there), but every new pipeline in play was cold
  in the driver.

The specific multi-second compute freezes:
- `92c214fe86` (StaleNullPointerSlot): when a compute program's pointer slot was null at
  translation and later becomes valid, `CurrentComputePipeline` re-translates it and **replaces the
  pipeline at once, before the new VkPipeline exists**. The next dispatch blocks for the full
  driver compile. Measured: "Rebuilding compute program 0x1149b7d000", then "Submission thread
  waited 5757.7 ms for Compute pipeline d39c428491e9cef7"; again 4995 ms for 619ac8a173eecd61.
  The counter resets each boot, and only the first candidate variant per key is built at boot, so
  this can recur every session.
- New compute programs: 1.3-1.7 s each (efa6e861..., 594ba195..., c332d879..., 50e0f817...,
  54177e3f...).

Graphics waits: `BuiltPipeline` returns an unbuilt pipeline (that is, it waits) for mesh pipelines
and for draws with `zeta_enable == 0 && draw_element_count <= 6`. That rule also catches instanced
4- or 6-vertex sprite, particle and HUD draws, and **every indirect draw** (`draw_element_count` is
0 for indirect, `draw_manager.cpp` ~333).

## 2. Ground rules for every change

1. Each change goes behind a new `Nx2Perf::Opt` switch in `src/video_core/nx2_perf.h`: add it to
   `Opt`, `opt_names`, the static_asserts and the unit test `src/tests/video_core/nx2_perf.cpp`.
   It starts default **off**; turn it on only after the before/after numbers in section 5.
2. Off must mean byte-identical behavior.
3. No locks and no heap allocation on the per-draw hot path (AGENTS.md). Slow paths (cache miss,
   rebuild) may allocate.
4. Never call `MemoryManager::ReadBlock` from a worker: it flushes through the rasterizer. On a
   worker use `Read<T>` / `ReadBlockUnsafe` only. Never read `maxwell3d` / `kepler_compute`
   registers or `replace_table` from a worker.
5. `graphics_cache`, `compute_cache`, `graphics_fast_cache`, `used_this_session`,
   `current_pipeline`, transitions, `disk_pipelines` and `evicted_keys` belong to the GPU thread.
   Workers hand results back through a mutex-guarded completion queue, which the GPU thread drains
   (with try_lock, so a contended drain never blocks a draw).
6. Anything that changes what is drawn or when (skips, swaps) needs the lab flash check
   (`ab_run.ps1 -Stay -Flash 40`, 0 outliers vs baseline) and a user play test. The user saw
   flashes from earlier timing shortcuts.
7. Never skip a compute dispatch (see section 4, rejected ideas).

## 3. Work items, in order

### P0. Measurement first (about 0.5 day, no behavior change)

Commit `a99a5ed662` already logs "Graphics/Compute pipeline X: Q ms in the worker queue, B ms to
build" (builds over 50 ms) and "Submission thread waited W ms for ... pipeline X" (over 2 ms) in
`pipeline_helper.h`. The 10 s "Runtime pipelines in the last 10 s" line splits GPU-thread build
time by phase. Add:
- **P0.1 Wait attribution.** In `GraphicsPipeline::ConfigureDraw`, when `!is_built`, capture at
  record time and print in `ReportPipelineBuildWait`: rt0 address and size, `draw_element_count`,
  instance count, indexed/indirect, `zeta_enable`, number of color targets, mesh or not. This tells
  which category the 0.1-1.9 s graphics waits belong to (post pass, HUD/particles, indirect).
- **P0.2 Skip reason.** In `vk_rasterizer.cpp` `report_failure`, log `GraphicsMissReason()`
  (async-build, build-deferred, ...) instead of the fixed "no pipeline" text.
- **P0.3 Miss classification.** Count runtime misses whose key hash is in `disk_pipelines`
  separately from true misses, and log both with the 10 s line.
- **P0.4 Driver cache hit or miss (optional).** With `pipelineCreationCacheControl` (Vulkan 1.3
  core, not enabled today: add it to `FOR_EACH_VK_FEATURE_1_3` in `vulkan_device.h`), try
  `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` first and log `COMPILE_REQUIRED`
  hashes. This is diagnostics only.

### P1. Remove the known multi-second freezes (low risk, do these first)

- **P1.1 Keep the old compute pipeline until a stale-slot rebuild is compiled** (switch e.g.
  `compute_rebuild_async`; about 40 lines). In `CurrentComputePipeline`, on a StaleNullPointerSlot
  trigger, put the rebuilt pipeline in a new `pending` field of `ComputePipelineCacheEntry`
  (`vk_pipeline_cache.h`) instead of replacing `entry.pipeline`. While pending, keep returning the
  old, already-built pipeline and skip the stale check. When `pending->IsBuilt()` (add an acquire
  load to `ComputePipeline`), retire the old one exactly as today (`retired_compute_pipelines`)
  and swap. If the old pipeline is null or failed, keep today's blocking path. Keep the fast-cache
  exclusion while pending.
  *Effect:* removes the 5-6 s freezes after "Rebuilding compute program".
  *Cost:* for the 1-6 s until the swap, accesses through the newly valid pointer are still dropped,
  as they were before the rebuild (Ocarina: foliage culling lists, so brief foliage pop).
  *Verify:* no "Submission thread waited" over 100 ms for a hash right after its "Rebuilding" line.
- **P1.2 Build every persisted compute variant at boot** (about 80-120 lines). `queue_compute`
  (`LoadDiskResources`) stops at the first candidate that translates. Instead, build each candidate
  with a distinct `MappedStoragePointerKeys` and keep the extras as alternates on the entry. On a
  stale trigger, swap to a built alternate that maps the slot (instant) before compiling anything.
  *Effect:* stale rebuilds stop costing compiles from the second session on.
- **P1.3 Cache-version policy** (a comment plus about 10 lines). Write the rule next to
  `CACHE_VERSION`: bump **only** when `GenericEnvironment::Serialize` / `FileEnvironment::Deserialize`
  change, when a key layout changes (`FixedPipelineState`, `GraphicsPipelineCacheKey`,
  `ComputePipelineCacheKey`), or when what the environment records changes. Do not bump for IR
  passes, SPIR-V emission or Ampere lowering: lowering already changes the native `unique_hash`
  (`shader_environment.cpp` ~266-270), boot re-translates every loaded entry anyway, and a missing
  recorded key drops only that entry (LogicError path). In `LoadPipelines`, on a mismatch **rename**
  the file to `vulkan.bin.v<old>` instead of deleting it.
  *Effect:* after a translation-only change, the driver's recompiles land on the loading screen
  (the boot workers build every used-list pipeline) instead of in play. This is the biggest lever
  for anyone who updates often.
  *Risk:* a recording change shipped without a bump reuses stale classifications. Reviewers check
  `shader_environment.*` and the env queries made by passes on every shader commit.
- **P1.4 Stop evicting boot-built pipelines that never drew.** Turn `pipeline_keep_preloaded` on
  by default (remove it from `DefaultOffMask` and `always_off`, fix the static_assert and the test),
  or never evict `LastUse()==0` in `EvictUnusedPipelines`. Fix that function's comment as well:
  after eviction a rebuild is a full GPU-thread translation (~8.5 ms each), not a 2-3 ms driver
  cache hit. Memory stays at the after-boot level (~0.45 MB per pipeline, a measurement from
  another title).
- **P1.5 Keep the disk cache across driver updates (optional, small).** `FixedPipelineState`
  stores `driver_id` and `driver_version` (`fixed_pipeline_state.cpp` ~110), so after a driver
  update no disk key matches and everything is translated live. In `load_graphics`, check the
  used list against the hash as stored, then rewrite the driver fields to the current values
  before building and inserting, and write the used list with the new hashes.

### P2. Graphics draws that wait for a driver compile (medium risk)

Do P0.1 first and decide from its data.
- **P2.1 Skip a small no-depth draw instead of waiting, when its target already holds an earlier
  frame's output.** Add `last_written_frame` to `ImageBase` (`texture_cache/image_base.h`), set
  where render targets are prepared (`texture_cache.h`, `UpdateRenderTargets` /
  `PrepareImageView`), plus a "fully cleared this frame" mark. In `BuiltPipeline`, wait only if the
  color target was never written before this frame (a one-time texture build), was fully cleared
  this frame (skipping it would flash the clear color), or the draw is a mesh draw. Otherwise skip
  (`graphics_miss = "async-build-stale"`). Treat indirect draws as not small. Apply the same rule
  in `DeferRuntimeBuild`.
  *Effect:* the 0.1-1.9 s waits for per-frame post passes and HUD become 1-2 s of last frame's
  bloom, fog or tonemap.
  *Risk:* ping-pong blur passes mixing frames; flashes if the clear test is wrong. Flash gate
  required.
- **P2.2 Mesh pipelines:** if P0.1 shows mesh waits, apply the same stale-target rule to them.
  NX2_NOTES.md row 97 explains why mesh draws are never skipped today (high-detail LODs pop).

### P3. White silhouettes (medium risk)

The mechanism, consistent with the code but not yet confirmed in a frame capture: shared
depth-only and shadow pipelines are already built and still draw, while the per-material G-buffer
pipelines are deferred or skipped. The full-screen lighting quads (which always wait) then light
depth that has no material behind it.
- **P3.0** Capture one frame with `EDEN_NX2_DIAG` during the effect to confirm the draw order:
  depth prepass, G-buffer, lighting.
- **P3.1 Geometry consistency.** When a depth-tested color draw is skipped, insert its geometry
  identity into a fixed-size open-addressing table, tagged with the frame: index buffer address +
  first + count + base vertex, or vertex buffer 0 + first + count. Skip depth-only draws (all color
  masks 0) whose identity is in the table for this frame or the previous one. Entries expire after
  2 frames, or as soon as the material pipeline is built. One hash probe per draw, and only while
  the table is non-empty.
  *Effect:* objects pop in whole instead of as white silhouettes.
  *Risk:* depends on the depth pass reusing the same index ranges; confirm with P3.0.
- **P3.2 Build-order priority.** Over budget, give the budget to the deferred keys that matter
  most: G-buffer first (zeta on and more than one color target), then by draws per frame. Use
  preallocated slots only.
- *Rejected as the default:* deferring a whole render pass. One rare material would freeze the 3D
  view for the length of a driver compile.

### P4. Move translation (A) off the GPU thread (high effort; lowers stutter, not the B freezes)

- **P4.1 Replay disk recordings on workers** (1.5-2 days). Today, graphics entries skipped at boot
  (not on the used list) have their `FileEnvironment`s dropped. Keep each one's file offset, and
  on first use (skippable draw) queue `CreateGraphicsPipeline(local ShaderPools, key, envs,
  nullptr, false)` on a worker, exactly the boot path, so it needs no live state. Return nullptr
  (skip) until the GPU thread drains the result. Pass `native_turing` in, so the worker never reads
  `maxwell3d`. Use a separate low-priority `Common::ThreadWorker` (1-2 threads) for background
  warm-up; the shared `workers` queue is one FIFO with no priority. Fix the existing boot race
  while there: `disk_pipelines.insert` in `load_graphics` runs without `state.mutex` while workers
  erase under it.
  P0.3 shows how much this covers: in the user's current cache nothing was skipped at boot
  (Total 3262, no skip line), so it matters mainly after a short or lost used list.
- **P4.2 Snapshot environment for true misses** (4-7 days, high risk). Capture on the GPU thread
  what translation reads lazily, then translate on a worker:
  - the enabled const buffers of each stage, eager-copied with no-flush reads, with a size cap and
    a synchronous fallback;
  - the `tex_header` address and limit, `sampler_binding`, `viewport_scale_offset_enabled`,
    `UsesNativeTiling`, `texture_bound`, and `replace_table` when HLE state is used.

  The lazily read items and their call sites are listed in `shader_environment.cpp`:
  `ReadCbufValue`, `ReadTextureType` / `ReadTexturePixelFormat`, `ReadViewportTransformState`,
  `GetReplaceConstBuffer`, `IsCbufStorageBufferPointer` and `IsCbufIndirectStorageBufferPointer`
  (each records into a map that `FileEnvironment` serializes). Validate on adoption only what can
  drift: re-read the TIC entries and compare type and format, recompute the storage-pointer
  classifications, and check that the memory id still matches. **Do not validate `cbuf_values`**:
  they are per-draw data, so they would mismatch constantly, and the capture-time copy is what the
  synchronous path would have read anyway. Any other mismatch discards the result and builds
  synchronously. Needs a Verify mode that also translates synchronously and memcmps the SPIR-V
  per stage, because a wrong result is written to disk permanently.

### P5. Experiments (measure before building)

- **Two-tier compute compile:** build a `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` pipeline
  first, then the optimized one in the background, and swap atomically. Prototype first: time
  `vkCreateComputePipelines` with and without the bit for 3-5 of the hashes that froze (listed in
  section 1). Proceed only if NVIDIA honors it and it is at least 5x faster. Unoptimized
  culling/skinning kernels may cost FPS until the swap. The `pipeline` member then needs atomic
  access.
- **A separate compute build lane:** gains nothing today (queue time 0 ms). Revisit only if P4.1's
  background warm-up shares the pool.

## 4. Rejected ideas (and why)

- **Skipping compute dispatches while they build.** Compute writes GPU-driven command lists
  (`DispatchCompute` arms `Nx2CollectListCopies`) and indirect arguments. A null native compute
  pipeline throws "Native compute pipeline unavailable" and permanently blacklists the program
  (`kepler_compute.cpp` `rejected_compute_programs`). The user has had device losses in Kokiri
  before.
- **Holding the last complete frame on screen during bursts:** looks like a freeze again.
- **A much larger budget (e.g. 256 ms per frame):** turns the freeze into a slideshow of
  partly drawn frames.
- **VK_EXT_graphics_pipeline_library, VK_KHR_pipeline_binary, shader objects:** none of them avoid
  NVIDIA's compile when the SPIR-V changes. GPL is graphics-only.

## 5. How to measure (lab, before and after each item)

- Lab driver: `.agent/lab/ab_run.ps1` (skill `nx2-lab-automation`). One emulator at a time; never
  kill `eglantine.exe`. The lab uses the user's real data folder `%APPDATA%\eglantine`.
- **Back up the user's shader cache before any cold run, and restore it after:**
  `%APPDATA%\eglantine\cache\shader\0400486024dec000\` (`vulkan.bin`, `vulkan_used.bin`,
  `vulkan_mesh.bin`). A verified backup from 2026-11-07 is in
  `.agent/backup/shader-0400486024dec000-2026-11-07/`.
- Three cache states:
  1. **Warm:** as is.
  2. **Emulator-cold:** empty `vulkan*.bin`.
  3. **Driver-cold:** also move NVIDIA's GLCache file for the app aside. Inferred to be
     `%LOCALAPPDATA%\NVIDIA\GLCache\...\8554a83c993c59da.bin` (445 MB), but verify that by
     timestamp; move it, never delete it, and put it back afterwards. State 3 reproduces the user's
     B freezes.
- Run: `ab_run.ps1 -Name <n> -GameplayWait 60 -MaxSeconds 420` (walks). For pop-in and white
  frames: `-GameplayWait 0 -Flash 30`, which takes 30 captures right after the area loads and
  prints a luma sequence: 0.97 means white-out, ~0.43 is normal.
- Read from `.agent/lab/runs/<n>/emulator.log`:
  - worst frames: `Nx2Frame:` lines with `iv_max` / `fw_max` (ms) at or above 100;
  - waits: "Submission thread waited" lines;
  - builds: the "ms in the worker queue, ... ms to build" lines;
  - the "Runtime pipelines in the last 10 s" phase split;
  - "Shader build budget" deferral counts.
- Also check fog (`.agent/lab/fogscore.py <run>/capture-0.tga`, about 0.50 at this spot, never
  below 0.35), `game.png`, and DEVICE_LOST in both `emulator.log` and `eden_log.txt`.
- Baselines from the lab, Kokiri area load:
  - emulator-cold before the budget: worst 1.69 / 2.27 s;
  - with the budget: 0.60 s;
  - warm: 0.51 s (this rest is the scene load itself, not shaders).
- B-type freezes come from the user's own play (driver-cold), not from the lab yet: up to 6.0 s.
- **Targets:**
  - no "Submission thread waited" over 250 ms in an emulator-cold run;
  - no `Nx2Frame` `iv_max` over 600 ms in a driver-cold run, apart from the scene-load frames;
  - white-frame luma back to normal within 1 s of the area load;
  - steady-state FPS unchanged.

## 6. Small bugs found along the way (fix separately)

- `clip_distance_written` is a `static std::bitset<8>` at namespace scope in a header
  (`spirv_emit_context.h` ~22). Each translation unit gets its own copy: the one set in
  `emit_spirv_context_get_set.cpp` is never the one tested in `emit_spirv_special.cpp`, so every
  clip distance is zero-filled. It is also written from many workers without synchronization.
- Boot race: `disk_pipelines.insert` (`load_graphics`) runs without `state.mutex` while worker
  tasks erase under it (only when a cached pipeline fails to build).
- The `EvictUnusedPipelines` comment says rebuilds cost 2-3 ms from the driver cache; they are
  full GPU-thread translations.

## 7. Suggested order and size

| Order | Item | Size | Removes |
|---|---|---|---|
| 1 | P0.1-P0.3 diagnostics | 0.5 day | (data for 4-6) |
| 2 | P1.1 async compute rebuild | 0.5 day | 5-6 s compute freezes after rebuilds |
| 3 | P1.3 cache-version policy + rename | 0.5 day | most driver-cold play after updates |
| 4 | P1.4 keep preloaded | 1 hour | late-session re-translation |
| 5 | P1.2 all compute variants at boot | 1 day | repeat rebuild compiles |
| 6 | P2.1 stale-target skip | 1-2 days | 0.1-1.9 s graphics waits |
| 7 | P3.0-P3.2 silhouettes | 2-3 days | white pop-in |
| 8 | P4.1 replay recordings | 1.5-2 days | GPU-thread stutter for recorded keys |
| 9 | P4.2 snapshot translation | 4-7 days | remaining GPU-thread stutter |
| - | P5 experiments | measure first | maybe first-use compute compiles |

New compute programs that the driver has never seen still cost 1.3-1.7 s once each. Only P1.3
(compiles moved to boot), P5 (a faster first compile) or the driver cache filling up over time
reduce them.
