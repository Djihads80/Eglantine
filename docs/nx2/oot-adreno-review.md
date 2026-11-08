# OoT / Adreno 840 experimental review branch

Status: 2026-11-08. This is a work-in-progress review branch, **not a production
merge recommendation**. No game assets, keys, shader binaries, frame captures,
driver binaries or APKs accompany this branch. Measurements are author-local.

## Scope

- Software matrix B gather: replace variable-lane shuffles with constant-lane
  broadcasts on supported Turnip compute pipelines explicitly requiring 32 lanes.
  Float arithmetic, FMA ordering and rounding are unchanged.
- Optional shared-B path: `EDEN_NX2_HMMA_SHARED_B=1`, otherwise broadcast fallback.
  Requires SPIR-V 1.6, explicit workgroup layout, full 32-lane subgroups, a
  conservative reconverged top-level call-flow check and a 16 KiB shared budget.
  Separate aliased explicit-layout scratch follows guest shared memory. Subgroup
  barriers protect publication and reuse. Broader divergent callers are refused.
- Native uniform-bank reupload: upstream backport based on Eglantine commits
  `031e07436c77de3fac223eb7be31b19e4f39e757` and
  `8b3aff59e49e7c71c6e3bfade2c4ca08702b84f9`. Mode 2 defaults to every 64 KiB
  native-bank bind; modes 0/1/3 permit investigation. This did **not** eliminate
  the owner's Fast-mode flashes.
- Optional empty-query WFI skip: `EDEN_NX2_WFI_QUERY_SKIP=1`, off by default.
  Uses prepared-write state after PresyncWrites, not only pending query state:
  desktop samples can have prepared copies even after their pending list drains.
- Optional Accurate command-list copy: `EDEN_NX2_ACCURATE_LIST_COPY=1`, off by
  default. Reuses the existing GPU-written-list copy validation before SafeRead;
  misses retain SafeRead. Submitted entries and CPU-write preservation unchanged.
- Video decoder pacing: backport from Eglantine `1f52ac7e2`; late decoded frames
  can be dropped, with pause/discontinuity guards. `EDEN_NX2_VDEC_PACE=0` disables.
- Optional narrow bilinear stand-in: `EDEN_NX2_OOT_UPSCALE_STANDIN=1`, off by
  default. Adapted from PR #614 / `b536a971a5`, not the removed broad fallback.
  Exact OoT final gameplay hash only; current descriptors/mappings/formats must
  validate and the copy must succeed before skipping that final dispatch.
  Preparatory and exposure passes remain, movie interception is unchanged.
  **Current device scene was refused by the descriptor contract. It did not
  activate and has no demonstrated performance or quality benefit.**

## Evidence and limits

Android 16, Adreno 840, Accurate GPU, 0.5x, frame generation off, plain Turnip.
Repeated samples are four 5-second windows in the same treehouse scene.

| Comparison | Mean FPS | Interpretation |
|---|---:|---|
| Same APK, broadcast / shared-B | 6.54 / 8.08 | Supports a shared-B gain in this run |
| Shared-B plus WFI skip | 8.03 | No clear gain; left disabled |
| Shared-B plus Accurate list-copy, early clean run | 8.63 | Not interleaved; modest apparent gain |
| DD-Turnip v0.5.0 | 2.99 | Slower in this test; reverted |
| Return to plain Turnip, existing Accurate-copy APK | 14.52, repeat 14.50 | Unexplained runtime change; no new code installed |
| New stand-in-capable APK, flag unset | 14.35 | Faster control survives update |
| Flag enabled, but contract refused / native fallback | 14.70 | **Not** a stand-in speedup |

The owner independently observed approximately 15 FPS. This is not 30 FPS and
the unexplained 8.63 -> 14.5 change must not be attributed to a new source fix.
Fast mode still flashes. Accurate was reported to have only one barely noticeable
flash in one run; this is not a flash-free correctness proof.

Matrix checks: 865,962 assertions / 14 host cases; 90 emitted modules passed
SPIR-V validation. 56 paired GPU fixture comparisons and seven full-grid cases
were byte-identical, including guest shared-memory sentinels. Fixture median
40.77 -> 15.03 ms is **not** game frame time. Fresh game-generated SPIR-V admitted
shared-B, validated, and retained its arithmetic/rounding opcode stream.

Local extracted-code harnesses covered prepared/pending/discarded WFI states
and 64 command-fetch routing cases, including SafeRead fallback. These harnesses
are not full Vulkan coherency tests. Full Android compile/link succeeded.
No complete desktop/backend or all-game regression suite was run.

Profiling correction: Accurate-copy reduced buffer Finish waits, but roughly
563–575 ms/s reappeared in WaitSubmittedTick. Much of the wait moved, rather than
vanished. Genuine GPU dependencies and CPU-write preservation remain necessary.

## Before production consideration

1. Split this branch into focused upstream contributions; check current nx2 and
   existing PRs first. Credit the upstream backports rather than treating them
   as new fixes. Do not merge experimental selection flags to production nx2.
2. Reproduce interleaved controls and investigate driver/runtime/clock variance.
   GPU frequency was not readable through the tested sysfs path; no clock floor
   was changed by this emulator patch.
3. Validate multiple scenes, camera motion, videos, transitions, cold/warm caches,
   other games/drivers/subgroup sizes and desktop backends.
4. Diagnose stand-in descriptor refusal from actual view/resource data; do not
   blindly loosen guards or skip preparatory inputs using a prior-frame latch.
5. Obtain longer visual/race/coherency evidence for Accurate list-copy. Keep WFI
   disabled unless a benefit is reproduced. Preserve correctness fallback paths.
