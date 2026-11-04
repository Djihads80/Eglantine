# Mario Kart World integration test

Base: upstream `nx2` at `0da830e94` (updated November 2, 2026).

This branch keeps upstream's current command-list copies, shader support, ASTC
transcode fix, fiber allocation changes, and game-loading and amiibo fixes.
It applies the remaining changes independently so they can be reviewed separately.

From the snowstormxd performance fork:

- Execute native macro parameters without a second FIFO copy.
- Keep register-write history optional in release builds.
- Reuse unchanged small uniform allocations within one live scheduler tick.
- Batch shader invalidations, preserving every supplied write range.
- Clear only the descriptor bitmap words exposed by the current table limit.

Adapted from [Selevet performance build 4](https://github.com/sylvieistaken/selevet-eglantine/releases/tag/selevet-eglantine-performance-4):

- Submit native graphics work every 128 draws to overlap GPU execution with CPU
  recording. Other titles retain their existing submission threshold.
- Use the current draw's actual element count when deciding whether asynchronous
  shader compilation may skip it. Indirect counts stay unknown rather than using
  an earlier draw's count.
- Traverse cache and scheduler bitmaps by skipping empty words. Wider priority
  bitsets retain the generic traversal. The slot iterator also handles an empty
  first bitmap word correctly.

Handheld mode, ScaleForce, and FXAA are the defaults. Explicit saved preferences
are retained. Accepting the startup recommendations saves the requested settings
and reapplies the MKW preset after per-game configuration is loaded. Declining or
closing the prompt preserves the user's settings.

The kernel TLS implementation is compiled without LTO on GCC and Clang LTO builds
so its lookups remain outside callers that can switch between migrating fibers.
This restores MKW startup in the tested Windows build. Non-LTO builds keep their
existing flags. The changes use portable timing and synchronization paths.

The Windows hybrid timer, Windows RAM trimming and process-priority changes are
not included. Unsafe preemptive reads, compute-acquire bypasses, and guest SDK VRR
patching are also excluded. Current upstream command-list copying is retained
instead of Selevet's older command-readback implementation.

## Validation

The regression tests exercise the real macro JIT with FIFO reallocations, uniform
data mutation and session resets, descriptor-table growth and invalidation,
batched shader removal against sequential invalidation, sparse slot iteration,
and priority ordering against an independent queue model. Priority tests include
both 64- and 128-priority queues.

The integration also retains the calibration-worker lifetime fix, repairs stale
memory/timer test fixtures, and defines kernel TLS out of line for ThinLTO.
Loose RomFS directory lookup accepts mixed-case names on case-sensitive hosts;
NUP tests check that the base version metadata and archive checksum agree across
Windows and Linux.

The Windows test package uses Clang 23.1.4, the checksum-pinned Eden v020525 PGO
profile, and ThinLTO with backend optimization disabled (`--lto-O0`), matching the
working startup test. Selevet's MSVC-ABI profile is not substituted for the MinGW
build's profile. Link-time optimization is a build option; other platforms can
continue using their usual build configuration.

Compare the same course and route against the prior build, using a copy of the
same shader cache, game version, and settings. Check item icons, geometry,
collision, audio, hitches, and stopping/closing. Asynchronous shaders can delay
the appearance of new objects while their pipelines compile. A lower CPU counter
alone does not establish an FPS gain. No locked-60-FPS result is claimed.

`EDEN_NX2_FLUSH_DRAWS=4096` restores the previous native submission threshold for
a controlled comparison. `EDEN_NX2_UBOREUSE=0` disables uniform allocation reuse.

Assisted by Codex.
