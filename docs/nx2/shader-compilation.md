# Shader compilation: how it works, what was measured, what is left

How a new Switch 2 pipeline gets built, which costs were measured (Ocarina of Time, November
2026), what changed, and the plan for the rest. Measured on Linux with a Ryzen 7 9800X3D (8 cores,
16 threads), an RTX 4070 SUPER on NVIDIA 620.70, and the 9800X3D's integrated GPU on RADV
(Mesa 26.2.6). Windows drivers weren't measured; see "Reading tester logs".

## The path of a new pipeline

1. The GPU thread (`GPUBack`) meets a draw whose pipeline key isn't in the cache. The native
   Ampere program was lowered to Maxwell code when the shader cache first met it (about 25 us a
   program, not a cost worth chasing).
2. With asynchronous shaders (on by default), the environments are detached from the 3D engine
   (`GraphicsEnvironment::Detach`: constant buffers copied, texture pool located) and queued on the
   `VkPipelineTranslator` threads. The GPU thread waits up to 2 ms for the pipeline, 4 ms a frame
   in total, then skips the draw until a later draw adopts the pipeline.
3. A translator looks up each stage in the stage cache (`VideoCommon::StageCache`), translates the
   stages that miss (in parallel when there are several), emits SPIR-V and queues the driver
   compile on `VkPipelineBuilder`.
4. The driver builds the pipeline: about 1 ms from its own disk cache, tens of ms when it compiles.

Draws that can't be skipped (no depth, 6 elements or fewer), compute pipelines and Switch 1 programs
still translate on the GPU thread.

## Measurements

Offline bench over 1,483 OoT and 1,952 Mario Kart World native programs
(`EGLANTINE_SHADER_BENCH=<dump dir> ./tests "[.shader_bench]"`): translation is ~80% of the CPU
cost, SPIR-V emission ~11%, control flow analysis 3%, lowering 2%.

| Change | Effect |
|---|---|
| SSA and SPIR-V speedups ported from jojo's fork, pruned storage buffer search | -19% (OoT) / -23% (MKW) translation and emission time, identical SPIR-V |
| Translation on worker threads | OoT cold boot: GPU-thread time on pipelines ~3.4 s -> ~0.9 s; ~5,000 draws skipped in the 10 s loading burst |
| Stage cache | 64% of the stages a cold boot translated repeated SPIR-V already made (48% of the time); 1,276 stages reused, 0 differences in verify mode |
| 15 translators below normal priority (was 7) | loading burst latency 4.0-5.0 ms mean / 52-57 ms max -> 3.4 ms / 38 ms |
| Stages of a pipeline in parallel | pipelines outside the burst: 1.0-1.1 ms -> 0.8 ms mean latency |
| NVIDIA driver cache in a folder of its own (Linux) | cold boot 74.6 core-s of driver compiles, warm 2.4 |

Per-thread CPU over a cold OoT boot and walk: the shader work is a small part of the process
(translators ~3.5 core-s, the process uses 5-6.5 of 16 threads), so it isn't CPU-bound; the cost
users see is the loading burst (~1,100 pipelines at once) and the driver's compiles on a cache miss.

## Driver caches

| Platform | Driver cache | Limit | Our own copy |
|---|---|---|---|
| Linux NVIDIA | `~/.cache/nvidia/GLCache`, shared by every application; now `<shader cache>/nvidia` for us | 1 GB on many systems (the README doesn't name the default); ours 10 GiB | none |
| Linux AMD/Intel (Mesa) | `~/.cache/mesa_shader_cache`, shared | 1 GB (Mesa `disk_cache.c`) | `vulkan_pipelines.bin` (whole file in RAM, rewritten at each save) |
| Windows NVIDIA | NVIDIA's folder in `%LOCALAPPDATA%` | Control Panel setting; 10-16 GB default reported, unverified | none |
| Windows AMD (proprietary) | AMD's own | unknown | none |
| Windows Intel | Intel's own | unknown | `vulkan_pipelines.bin` |

- NVIDIA stores ~26 KB a pipeline; a full compile costs ~48 ms of CPU, a cache hit 0.6-1.5 ms.
  120k pipelines need ~3.1 GB, three times a 1 GB limit.
- NVIDIA README 11K: the default cache has one size limit for all applications, and an application
  that sets `__GL_SHADER_DISK_CACHE_SIZE` locally makes the next one without it wipe the cache. A
  global setting (`~/.config/environment.d/`) or a private path avoids that.
- Verified: a private `__GL_SHADER_DISK_CACHE_PATH` is honoured and `__GL_SHADER_DISK_CACHE_SIZE`
  enforced there (loosely: a 10 MB cap reached 20 MB on a later run). A warm boot from a full
  capped cache still compiled three quarters of OoT's pipelines (56.7 core-s against 0.14).
- Mesa honours `MESA_SHADER_CACHE_DIR`; RADV and ANV read it when the physical device is set up.
  Whether a private Mesa cache then hits wasn't measurable here (the iGPU builds about one pipeline
  in 70 s and times out in the OoT load).
- `vulkan_pipelines.bin` is off for the NVIDIA and AMD proprietary drivers (`DriverCachesPipelines`),
  as in Lichii.

## Pre-compiling from the game's files

"Pre-compile Shaders" in a game's context menu reads the shader archives in the game's RomFS
(ported from Lichii #781/#812: zstd with the game's dictionaries, SARC, BFSHA/BNSH, effect bundles
and more), lowers every compute program the way a dispatch does (`LayOutLoweredNativeCode` is
shared with the runtime) and writes `<shader dir>/<title id>/warm-compute.bin`. At the next boot,
after the disk cache, the programs not already cached are built on background threads with the
reads the file lacks guessed (`GuessMissing`) and thrown away: they only fill the driver's cache,
so a dispatch that converts to the same SPIR-V doesn't wait for the driver. A `.done` marker
skips this until the driver, device or file changes. Graphics programs aren't pre-compiled: their
pipelines need draw state that only exists at runtime.

OoT: 1,458 archives, 3,978 compute programs in 1.2 s; 136 of the 141 compute pipelines in its
disk cache are among them (keys match; SPIR-V can still differ where a guess is wrong). The boot
built the other 3,842 in 36.8 s on 4 background threads. Those builds show up in the "Driver
pipeline builds" lines while they run.

`eden-cli --build-warm <game> <title id hex> <output dir>` runs the builder without the GUI.

## VK_KHR_pipeline_binary (shelved)

NVIDIA 620.70, RADV and ANV offer it. Lichii turned its store off for NVIDIA and AMD's proprietary
drivers: it duplicates their cache, NVIDIA 610.88 exported ~7.8 MB a pipeline, and 610.88's
`vkGetPipelineKeyKHR` without a create info overwrites a register the caller keeps.

On 620.70 a pipeline exports 3 binaries, ~2.0 MB, but one is a 2 MiB blob identical in every
pipeline; stored once, ~11 KiB a pipeline remains. A pipeline builds from its binaries in 2.2-2.9 ms
(binaries from bytes 0.11 ms, the shared one can be reused), slower than a hit in NVIDIA's cache
(0.6-1.5 ms); a capturing build costs 6-7.5 ms. Not worth it while the driver's cache works.

## Reading tester logs

```
NVIDIA driver shader cache in <path>, limit <bytes>
Driver pipeline builds at boot: <n> graphics, <n> compute; <n> over 10 ms ...
Driver pipeline builds in the last 10 s: ...; <n> over 10 ms (compiled, not from the driver's cache); <mean> ms mean, <max> ms max
Async translation in the last 10 s: ... latency <mean> ms mean <max> ms max, peak <n> queued ...
Stage cache: <n> stages reused whole, <n> translations reused, <n> translated
```

Many builds over 10 ms on a second boot of the same scene mean the driver's cache is missing:
too small, wiped, or shared with other applications.

## Switches

| Variable | Effect |
|---|---|
| `EDEN_NX2_ASYNC_TRANSLATION=0` | Translate new native pipelines on the GPU thread |
| `EDEN_NX2_ASYNC_TRANSLATION_WAIT_MS`, `_FRAME_MS` | Wait per pipeline (default 2) and per frame (default 4) |
| `EDEN_NX2_TRANSLATION_THREADS` | Translator count (default as many as the driver compile workers) |
| `EDEN_NX2_PARALLEL_STAGES=0` | Translate a queued pipeline's stages one after another |
| `EDEN_NX2_STAGE_CACHE=0` / `verify` | Stage cache off / rebuild every reused stage and log differences |
| `EDEN_NX2_PRIVATE_DRIVER_CACHE=0` | Leave NVIDIA's cache in the shared folder (Linux); the same as turning off Graphics > Advanced > "Separate driver shader cache" (`private_driver_shader_cache`, applies at the next start) |

The same setting's size (`driver_shader_cache_size`, 1-1024 GiB, default 10) is the private folder's
limit; it replaces a `__GL_SHADER_DISK_CACHE_SIZE` from the environment, which is meant for the
shared cache, and drops Steam's `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP`, which would turn
the limit off. A `__GL_SHADER_DISK_CACHE_PATH` from the environment (Steam sets one per game) is
kept. The folder is created after the user data migration, so the migration doesn't find it in the
way.

## Plan for the rest

1. Mesa on Linux: the same private cache folder (`MESA_SHADER_CACHE_DIR`, `MESA_SHADER_CACHE_MAX_SIZE`);
   once AMD tester logs show it hits, retire `vulkan_pipelines.bin` on Mesa.
2. Switch 2 titles build every cached pipeline at boot. A 120k cache needs a cap or a recently-used
   selection, whatever the driver cache does (driver memory, boot time).
3. Windows: leave the driver environment alone; decide from tester logs.
4. Pre-compiling from the game's files: compute is done (see above). Next: measure whether
   the guessed reads give the runtime's SPIR-V, and look at the walker's member errors (8,728
   on OoT).
