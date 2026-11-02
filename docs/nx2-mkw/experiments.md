# Experiments that were dropped

After option C removed the freezes, five more changes were written to make new shaders even
cheaper and to raise the frame rate. They were all tested on the same course, on the same machine
(Ryzen 7 5800X 8 cores / 16 threads, Radeon RX 6900 XT 16 GB, BC3 textures), and none of them
made the game better than option C alone. Their code is kept on branches for reference; those
branches, and the measurements below, are on the earlier base (Takoroka's eden-nx2), not on
Eglantine `nx2`.

## Summary

| Experiment | Branch | Measured | Verdict |
| --- | --- | --- | --- |
| A: translate new graphics pipelines on worker threads, skip the draw until ready, 3 ms wait budget | `shader-a` | GPU-thread pipeline work 3.8 s -> 0.6 s per session; frames not smoother | Dropped |
| Piste 1: reuse translated shaders between pipelines that differ only in fixed-function state | `shader-reuse` | 0 reuses in 683 runtime pipelines | Dropped |
| Piste 2: Vulkan graphics pipeline libraries (fast linking, optimized pipeline in the background) | `shader-gpl` | 959 background recompiles, 154 s of CPU in 6.5 min; ~40 fps instead of 50-60 | Dropped |
| Piste 3: faster shader translation passes (SSA rewrite, storage buffer tracking, SPIR-V constants, CFG) | `shader-passes` | Offline benchmark -32 % (8.9 s -> 6.1 s for 5,201 pipelines), identical SPIR-V | Correct, but translation was already ~1 ms per shader |
| Pipeline creation timers (`PipelineStats`) | `shader-measure` | Measurement only | Kept on its branch as a tool |

## What the measurements showed

Per new graphics pipeline on this machine, with option C:

| Step | Cost |
| --- | --- |
| Lowering Switch 2 code to Maxwell code | well under 1 ms |
| Translation to IR and SPIR-V emission | ~1 ms |
| **Driver compile (`vkCreateGraphicsPipelines`)** | **~40-100 ms** |

The driver compile dominates and already runs on worker threads with asynchronous shaders. Moving
the rest off the GPU thread (A) or making it faster (piste 3) attacks the small part. Worse,
every extra worker thread competes with the emulator's own threads for the 8 CPU cores: with A
the GPU thread waited less, yet frames were not shorter.

Piste 2 made each new pipeline cost a library compile plus a full optimized recompile in the
background. Because a library could only be shared by pipelines with the same shaders and the
same render pass (one descriptor set layout for all stages, render passes instead of dynamic
rendering), almost nothing was shared, and the background recompiles ate CPU time during races.

Piste 1 found no pipeline to reuse: in this game, pipelines that share shaders almost always
differ in something translation depends on.

## Session comparison (empty shader cache, same course)

| | Option C alone | A + piste 3 + fixes, piste 2 off |
| --- | --- | --- |
| Mean frame time, loading screens included | 19.9 ms | 23.3 ms |
| Mean headroom before the next 60 Hz vblank | 5.4 ms | 3.8 ms |
| Frames over 100 ms | 72 | 83 |
| Pipeline work on the GPU thread | 3.8 s | 0.6 s |

The two sessions did not last exactly the same time; the differences are small but all point the
same way, and the tester felt no improvement either.

## If someone wants to continue

- The driver compile is the cost to attack. Graphics pipeline libraries would need dynamic
  rendering and independent descriptor set layouts to share parts between pipelines; that is a
  large change to Eden's Vulkan backend.
- Piste 3's speedups are correct (SPIR-V identical for 11,883 modules) and could be upstreamed on
  their own; they help shader-heavy loading and the boot-time cache replay more than gameplay.
