# Shader stutter: what the Ocarina fork found and changed

A short handoff for anyone working on shader or pipeline compilation in Eglantine (Vulkan).
Everything below is on the branch [`oot-60fps-wip`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/tree/oot-60fps-wip).
It was measured on a Ryzen 9 9950X3D and an RTX 5090 (NVIDIA driver, Windows 11), in Ocarina of
Time, with async shaders on.

## Where the stutter actually comes from

| Cost | Where | Size | Status |
|---|---|---|---|
| **Translation** (guest code → IR → SPIR-V) on the GPU thread | `PipelineCache::CreateGraphicsPipeline()` / `CreateComputePipeline()` | 2-40 ms per pipeline. One area load with an empty cache: ~1,000 pipelines, ~3 s, freezes up to 2.3 s. About 70% of it is `TranslateProgram`, 12% SPIR-V emission | Spread over frames (below) |
| **Driver compile that the submission thread waits for** | the `scheduler.Record` wait lambda in `GraphicsPipeline::ConfigureDraw` and `ComputePipeline::Configure` | **compute: 1.3-5.8 s each; graphics: 0.1-1.9 s.** Time in the worker queue was 0 ms every time: this is pure driver time | **Open: the biggest remaining freezes** |
| **White silhouettes** while pipelines are pending | the async skip in `BuiltPipeline` | a few seconds after a cold area load | Mitigation on by default, untested (below) |

Facts that matter for a pipeline-level fix:

- **Async shaders only make the driver compile async.** Translation stays synchronous. And these
  draws still *wait* for their pipeline rather than skip:
  - compute dispatches (always);
  - mesh pipelines;
  - draws with no depth and 6 or fewer elements. That rule also catches indirect draws (count 0)
    and instanced quads.
- **Compute is the worst case.** Ocarina's native compute programs take seconds in the NVIDIA
  compiler. Graphics pipeline libraries don't cover compute.
- **NVIDIA keeps its own disk cache, not ours.** `DriverCachesPipelines()` is true for NVIDIA, so
  no `VkPipelineCache` / `vulkan_pipelines.bin` is used there. NVIDIA's own cache misses whenever
  the SPIR-V changes, so every translator change means a cold driver.
- **The pipeline cache is wiped too often.**
  - `CACHE_VERSION` was bumped 11 times in a week, and 9 of those bumps were for translation-only
    changes that didn't need it.
  - Each bump deletes `vulkan.bin`, which moves every compile from boot into gameplay.
  - `FixedPipelineState` also stores the driver ID and version, so every driver update orphans the
    whole disk cache.
- **Most pipelines reuse shaders the driver has already compiled, in new state combinations.**
  That is exactly the case pipeline libraries make cheap.
- **Vulkan device-feature plumbing:** `vulkan_device.cpp` merges only extension *names* from
  outside requirements. Feature structs have to go through the `FOR_EACH_VK_FEATURE_*` lists in
  `vulkan_device.h`, where `pipelineCreationCacheControl` is also missing.

## What the fork changed

On by default. Each one is a switch in `src/video_core/nx2_perf.h` (`EDEN_NX2_PERF_OFF=<name>` turns
it off):

- **`shader_build_budget`** (`c9b8bb5e52`, `e330d49488`): at most 24 ms of runtime pipeline building
  per frame (`EDEN_NX2_SHADER_BUDGET_MS`). Past that, skippable draws are skipped without building.
  Mesh draws are deferred too. Worst frame with an empty cache: 1.69/2.27 s → 0.60 s; a warm cache
  gives 0.51 s, which is the scene load itself.
- **Not lab-verified yet:**
  - `compute_rebuild_async` (`3eb64cb1ed`): keep the old compute pipeline while a stale-pointer
    rebuild compiles;
  - `compute_preload_variants` (`9cf1bc6308`);
  - `stale_target_skip` (`c41d23692f`);
  - `geometry_consistency` and `build_order_priority` (`357a488b77`);
  - `replay_disk_recordings` (`233b404586`);
  - `pipeline_keep_preloaded` on (`86a7dc9e58`);
  - rename an old `vulkan.bin` instead of deleting it (`f2ce6c3f0f`);
  - re-key the cache on driver updates (`ee560f97ab`).
- **Fix:** a data race on `clip_distance_written` in SPIR-V emission (`0dfaf624ba`).

## What could help you

- **Diagnostics** (`a99a5ed662` plus the phase split in `vk_pipeline_cache.cpp`). Each log line
  gives the pipeline hash, so you can see which pipeline caused a freeze:
  - `"<kind> pipeline <hash>: Q ms in the worker queue, B ms to build"`
  - `"Submission thread waited W ms for <kind> pipeline <hash>"`
  - every 10 s, `"Runtime pipelines in the last 10 s: ... (graphics: environments, translate, SPIR-V,
    modules, pipeline object)"`
  - per-second `Nx2Frame:` frame-time percentiles
  - wait attribution and skip reasons (`ca091a90f8`)
- **A repeatable cold-cache test** (`tools/nx2/lab/`, `OOT_CONTRIBUTING.md`):
  - Boot the save with an empty `%APPDATA%\eglantine\cache\shader\<title>\vulkan*.bin`, so the
    emulator cache is cold.
  - For a cold *driver*, also move NVIDIA's app file in `%LOCALAPPDATA%\NVIDIA\GLCache\...` aside.
    Move it, never delete it.
  - Compare worst frames (`Nx2Frame` `iv_max`) and the wait lines, before and after your change.
- **The full plan, with file:line references:** `.agent/plans/shader-freeze-plan.md`. It covers
  each freeze source, the designs considered and rejected, and the open questions.
- **Where your extension fits:** graphics pipeline libraries (fast-link, then optimize in the
  background) would remove most of the *graphics* driver-compile waits above, plus the need to skip
  draws. Two things they can't fix:
  - **compute pipelines:** keep-old-until-new and boot preloading help there;
  - **the first compile of a never-seen shader.**

  We're happy to run your branch through the same cold-cache measurements.
