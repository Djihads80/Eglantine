# Help wanted: Ocarina of Time to a locked 60 FPS

This fork plays Ocarina of Time (Switch 2) without crashing, and since 2026-11-06 without the
brightness flashes, at ~36 FPS at our lab save's Kokiri Forest spawn spot and 43-45 after a short
walk (RTX 5090). These are the open problems, easiest first. Read
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
objects, slow motion). Attach `%APPDATA%\eglantine\log\eden_log.txt` from that session. AMD and Intel
results are especially wanted: almost all testing so far was on an RTX 5090. If you see flashes,
say whether they stop with the environment variable `EDEN_NX2_PERF_SAFE=1`.

**2. Catch the rare freeze.** One automated run in fourteen froze the whole emulator for 60 s and
then carried on. If it happens to you, note the time, and while it is frozen save a dump from
Task Manager (Details, right-click `eglantine.exe`, Create dump file). That dump shows where
every thread waits.

**3. Measure 3D V-Cache pinning (Ryzen X3D owners).** The emulation threads are pinned to the
V-Cache CCD by default, and nobody has measured whether that helps. Run the lab
([tools/nx2/lab](tools/nx2/lab/README.md)) with `-Env EDEN_NX2_VCACHE=1` and `=0`, alternating, two
runs each, and post the spawn and walk medians. Unless pinning wins, it goes off by default.

## Some C++ needed

**4. Clean up Vulkan validation errors.** Run with validation layers on and fix what they report:
`vkCmdEndQuery` on queries started outside a render pass, an image view format
(A8B8G8R8_UNORM_PACK32) not in the image's format list, and `SPV_EXT_shader_stencil_export`
declared without the extension.

**5. Missing hedge leaves.** The garden hedges' mesh pipelines now build, but their draws get a
zero count from vegetation culling compute programs. Find which program and why
(`src/shader_recompiler/frontend/ampere/`).

## Deep: the remaining ~11 ms per frame

The spawn spot runs at ~27.5 ms per frame; 60 FPS is 16.7. Profile with `EDEN_NX2_GPU_PROFILE=1`
and look at `Nx2Frame:` lines. A change that skips a wait must first pass a run with
`EDEN_NX2_PERF_VERIFY=1` and zero `Nx2Perf split:` mismatches, because a stale read gives the
guest wrong draw arguments and can lose the GPU device. A change that touches frame timing must
also pass the lab's flash check (`-Flash 30`, zero outliers). Put each one behind an `Nx2Perf`
switch so it can be A/B tested and turned off. The wait counts below come from a profile taken
before the flash fix; re-profile before you start.

**6. Get the ~5 ms of `frames_ahead` back, without flashes.** Running the guest a frame ahead of
the GPU thread was worth ~5 ms per frame, and `present_one_ahead` another ~3.5 ms on top. Both
flash: the guest's syncpoint goes up when the back thread *records* a frame, and the back reads
guest memory late (uniform buffers re-read at bind, dirty pages uploaded at draw), so a guest
running ahead rewrites constants and exposure data before they are read. Only the pushbuffer is
copied. Waiting for the previous frame to land on the GPU is not enough (`landed_prev_always` and
`one_ahead_landed` still flashed: 7-8 outliers in 30 captures). What is left: copy, or
write-protect until the back has passed the frame, the pages the back reads late. See
`nx2_gsv.h` (present path), `fence_manager.h` and `buffer_cache.h`.

**7. One macro's DMA reads block the front.** A single macro (start `0x16`, method `0xe2c`) does
every front MME DMA read (6 words of an occlusion report), ~39,000 times a second, and each waits
~7 us for the back: ~255 ms per second. Running that read on the back in ring order would remove the sync
point. `acquire_trigger_early`, `exact_noted_writes` and `writer_set_exec` each removed many waits
but gained nothing alone, because the wait moved to this macro's next read; measure them again on
top of it.

**8. Command-list writer waits.** Every DMA copy and 2D blit counts as an unknown GPU writer, so
the front waits for them before each command list (~6,300 waits, ~145 ms per second). Noting
their destination ranges would let most go through. See `dma_pusher.cpp` and
`split_write_tracker.h`.

**9. Back per-draw setup.** Each fence with pending downloads ends the render pass to record its
copies (record them once per submission instead, ~0.6 ms per frame), and every native 64 KiB
uniform bank is compared with its shadow once per frame (~0.35 ms). See `buffer_cache.h`.

**10. Early command-list fetches (`split_overlap`).** Worth about 2 FPS, but quarantined: it once
lost the device, and it has no verify mode yet. Adding one is the first step.

## Reporting results

Give the commit hash (shown in the window title and the first log line), the place in game, and
before/after FPS from the same spot of the same save (`perf_summary.py` separates the spawn spot
from the walk). One good 10-second window proves nothing: frame rate swings about 3 FPS run to
run.
