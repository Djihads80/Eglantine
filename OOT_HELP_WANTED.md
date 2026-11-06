# Help wanted: Ocarina of Time to a locked 60 FPS

This fork's `stable/oot-playable-no-crash-2026-11-05` branch plays Ocarina of Time (Switch 2)
without crashing, at 40-60 FPS. These are the open problems, easiest first. Read
[OOT_WORKFLOW.md](OOT_WORKFLOW.md) before changing emulation code: every change here is measured
in game and checked against the safe path, and a fix that only compiles isn't done.

To claim a task, open an issue on this fork with its title, or comment on an existing one.
Test reports have their own form (New issue, **Ocarina of Time test report**). To build and
measure a change, see [OOT_CONTRIBUTING.md](OOT_CONTRIBUTING.md).

This is a RESULT FIRST fork: code lands once it works in game, before review, and much of it is
AI-assisted. **Reviewing what's already here is a contribution too.** Point out anything wrong
in an issue or PR.

## No emulator knowledge needed

**1. Test other areas and GPUs.** Play Hyrule Field, a dungeon, Kakariko, a cutscene. Report
the place, your GPU and driver, the FPS range, and anything wrong (crash, freeze, flashes, missing
objects, slow motion). Attach `%APPDATA%\eden\log\eden_log.txt` from that session. AMD and Intel results are especially wanted: almost all testing so far was on an
RTX 5090.

**2. Catch the rare freeze.** One automated run in fourteen froze the whole emulator for 60 s and
then carried on. If it happens to you, note the time, and while it is frozen save a dump from
Task Manager (Details, right-click `eglantine.exe`, Create dump file). That dump shows where
every thread waits.

## Some C++ needed

**3. Find out which guest-CPU readbacks matter.** Preflushing GPU writes the CPU reads took
Kokiri Forest from 37 to 52 FPS, but it now copies ~1.4 GB/s back from the GPU in ~150,000
copies a second. Count how many of those ranges the CPU reads before the GPU writes them again
(`src/video_core/buffer_cache/`). If most are never read, aging their marks out is cheap speed.

**4. Clean up Vulkan validation errors.** Run with validation layers on and fix what they report:
`vkCmdEndQuery` on queries started outside a render pass, an image view format not in the
image's format list, and stencil export declared without the extension.

**5. Missing hedge leaves.** The garden hedges' mesh pipelines now build, but their draws get a
zero count from vegetation culling compute programs. Find which program and why
(`src/shader_recompiler/frontend/ampere/`).

## Deep: the remaining ~3 ms per frame

The split GPU thread's front (macros, command parsing) is the bottleneck: it is idle under 1%
of the time, and the back idles ~24% waiting for it. Profile with `EDEN_NX2_GPU_PROFILE=1`.
Each item below must pass a run with `EDEN_NX2_PERF_VERIFY=1` and zero `Nx2Perf split:`
mismatches before it is turned on, because a stale read gives the guest wrong draw arguments
and can lose the GPU device.

**6. Early macro reads wait for every queued GPU job** (~250 ms per second of gameplay). The
1 MiB "GPU-written" marks are set only when the back runs a job, so any queued job might write
first. Fix: an exact set of shaders the back has proven never write memory; unseen shaders,
transform feedback and compute stay writers. See `Maxwell3D::FrontDmaReadEarly` and
`split_write_tracker.h`.

**7. Constant buffer words the front already knows.** Reads that overlap queued constant buffer
writes wait for them, although the front knows the values it queued. Serve them from a small
overlay instead (reports and fences excluded: they land later).

**8. Semaphore acquires drain the back** (~155 ms per second). A safe early check needs a bitmap
of pages the buffer or query cache may still hold newer data for, plus a gate on the fence
settings. See `Puller::ProcessSemaphoreTriggerMethod`.

**9. Early command-list fetches (`split_overlap`).** Worth about 2 FPS, but quarantined: it once
lost the device, and it has no verify mode yet. Adding one is the first step.

## Reporting results

Give the commit hash (shown in the window title and the first log line), the place in game, and
before/after FPS from the same spot. One good 10-second window proves nothing: frame rate swings
about 3 FPS run to run.
