# Shader stutter: lower each native program once

"Option C": three commits of the Mario Kart World fork, on top of Eglantine's `nx2` branch
(written first on Takoroka's eden-nx2, branch `shader-c`). Target: Mario Kart World (title 1),
whose first laps froze for a fraction of a second, sometimes seconds, each time a new effect
appeared.

## Result

One tester (Ryzen 7 5800X, Radeon RX 6900 XT, Mario Kart World 1.8.0, Vulkan, GPU accuracy Low,
asynchronous shaders on), on the earlier base: on a first launch with an empty shader cache, the
freezes are gone. Shaders still cost time, but the game slows down for a moment instead of
stopping. Frame times of such a session are in
[`docs/nx2-mkw/experiments.md`](docs/nx2-mkw/experiments.md); nothing was measured again on
Eglantine `nx2` yet.

## Background: what happens when the game uses a shader Eden has not seen

A Switch 2 shader is SM86 machine code (Ampere). Eden's recompiler only understands Maxwell code,
so a "native" program goes through four steps:

1. **Capture** - read the program from guest GPU memory, instruction by instruction, until its end.
2. **Lowering** - rewrite the Ampere instructions as Maxwell ones (`LowerToMaxwell`,
   `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`).
3. **Translation** - Maxwell code to IR to SPIR-V (`TranslateProgram`, `EmitSPIRV`).
4. **Driver compile** - `vkCreateGraphicsPipelines` / `vkCreateComputePipelines`.

With asynchronous shaders, only step 4 of graphics pipelines runs on worker threads. Steps 1-3
run on the GPU thread, and the game waits for them (NX2_NOTES row 121).

## The three changes

### 1. Lower each native program once (`shader_cache: lower each native program once`)

**Problem.** Steps 1 and 2 ran twice for every new program:

- once when the shader cache met the program (`ShaderCache::RefreshStages` / `ComputeShader`
  build an environment to compute its hash), and
- again when its pipeline was created (`GetGraphicsEnvironments`, `CreateComputePipeline`),
  because building an environment for a native program always captured and lowered it.

**Change.** The result of steps 1 and 2 is a `LoweredNativeCode` (`shader_environment.h`):
the lowered code, the number of source bytes read and the texture buffer the program needs.
`ShaderInfo` keeps it (`shader_cache.h`), and the environments built for pipelines take it as an
optional constructor argument. `GenericEnvironment::InitializeNativeCode` adopts it when given and
only calls `LowerNativeCode` (the old capture + lowering code, unchanged) when not.

The environment ends up with exactly the same `code`, sizes and texture buffer as before, so
hashes, analysis and the disk cache format do not change. Switch 1 shaders do not take this path.

### 2. Read the program a page at a time (`nx2: capture native programs a page at a time`)

**Problem.** The capture read 16 bytes per instruction, each read preceded by a mapping check and
a flush of the GPU caches (`ReadBlock`). Programs are thousands of instructions long.

**Change.** `NativeInstructionReader` (`shader_environment.cpp`) reads up to the next 4 KiB
boundary in one `ReadBlockUnsafe` call when that range is fully mapped, and serves the following
instructions from that copy. An instruction it cannot serve (unmapped range, or one straddling a
page boundary) is checked and read on its own, so the capture still stops at the same place and
throws the same errors.

**Behaviour change to know about.** These reads no longer flush the GPU caches first. If a game
ever wrote shader code with the GPU and that data had not reached guest memory yet, the capture
would read stale code. No such case is known; it is the first thing to suspect if new shader
rejections or corrupted effects show up.

### 3. Replay cached native programs like live ones (`nx2: fold bank 1 reads of cached native programs as live ones`)

**Problem.** When a native program is built live, its environment sets `is_proprietary_driver`,
which lets the recompiler replace reads of constant buffer 1 (driver constants) with the values
seen at build time. When the same program is rebuilt from the disk cache at boot,
`FileEnvironment::Deserialize` only set that flag for `texture_bound == 2`. The replayed pipeline
then read bank 1 at run time: a different shader from the one built live, under the same hash.

**Change.** `Deserialize` also sets the flag when the cached code carries a native address map
(`HasNativeAddressMap`), which every lowered program has. The cache file format is unchanged.
Pipelines already in a cache are rebuilt the new way on the next boot.

## Files

| File | What changed |
| --- | --- |
| `src/video_core/shader_environment.h` | `LoweredNativeCode`; optional lowered code in the environment constructors |
| `src/video_core/shader_environment.cpp` | `LowerNativeCode` split out of `InitializeNativeCode`; `NativeInstructionReader`; `Deserialize` flag |
| `src/video_core/shader_cache.h/.cpp` | `ShaderInfo::lowered_native`, filled in `MakeShaderInfo`, passed in `GetGraphicsEnvironments` |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` | `CreateComputePipeline` passes the shader's lowered code |
| `src/video_core/renderer_opengl/gl_shader_cache.cpp` | same for OpenGL |

## How to test

1. Move the game's shader cache away (`%APPDATA%\eden\cache\shader\<title id>`; Eglantine keeps
   Eden's user folder).
2. First launch: play a course. Expect short slowdowns, no multi-frame freezes.
3. Close, relaunch, play the same course: content already seen should not slow down at all.
4. Watch for new visual corruption or more "Native shader ... rejected" log lines than before.

## What was tried next

Translation (step 3) still runs on the GPU thread. Moving it to worker threads, reusing
translations between pipelines, Vulkan graphics pipeline libraries and faster translation passes
were tried after these changes and dropped: none made the game smoother than option C alone, as
the driver compile (step 4, already on worker threads) dominates. Measurements and branches:
[`docs/nx2-mkw/experiments.md`](docs/nx2-mkw/experiments.md).

---

AI disclosure: written with Claude Code (Claude Opus 5.5), reviewed and tested by a human
on Mario Kart World 1.8.0.
