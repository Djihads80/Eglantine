# Upstream candidates from the Ocarina of Time branch

Which commits on this branch (`oot-60fps-wip`) are worth sending to upstream Eglantine
([Djihads80/Eglantine](https://github.com/Djihads80/Eglantine)) as small pull requests, and in
what order.

- **Compared against:** upstream `nx2` at `ae9d3eb8d2` and `nx2-experimental` at `db093d7325`,
  2026-11-07. They differ by only 7 commits.
- **What was checked:**
  - all 399 non-merge commits here that upstream lacks; 303 of them are code;
  - whether upstream already has each change, by its content rather than its hash;
  - whether it applies cleanly (`git merge-tree` onto upstream);
  - how it was tested, using the dated log in `.agent/STATUS.md`.
- **Result:** 23 rated high value, 56 medium, 79 low, 145 none. 27 are already upstream, and 105
  are ports of other people's work, listed for their authors at the end.

Every speed number below was measured on one machine (Ryzen 9 9950X3D + RTX 5090) in Ocarina of
Time only. Each PR should say so.

## Tier A: generic fixes that apply cleanly (send these first)

Small, game-agnostic, and they apply onto upstream as they are. Rebase each onto upstream,
build, and run a game other than Ocarina if you can.

| # | PR | Commits | What it fixes | Evidence | Applies to `nx2` / `nx2-experimental` |
|---|---|---|---|---|---|
| A1 | buffer_cache: clear deleted buffer ids in every channel | `43b6b17c3a` | `DeleteBuffer` only clears the active channel, so other channels keep a stale `BufferId`, leading to `VK_ERROR_DEVICE_LOST` in multi-channel games | Lab A/B on 2026-11-05: builds without it lost the device ~163 s after loading; none with it. The user confirmed in play. A ready branch exists: `pr/oot-channel-buffer-ids` | yes / yes |
| A2 | shader_recompiler: remove the `clip_distance_written` data race | `0dfaf624ba` | A namespace-scope `static std::bitset` in a header: every translation unit gets its own copy (so the copy that is set is never the one tested), and the shader-compile workers write it concurrently | Code inspection; unit tests pass | yes / yes (same file, no conflict). Drop the unrelated comment hunk |
| A3 | video_core: handle unmapped GPU writes; clean up on channel release | `161b0cf219` + `36e627c684` (squash) | Unmapped GPU writes log instead of asserting; state-tracker flags reset on channel release; null-address guard in the compute reduction; null-rasterizer inline upload and query waiters | From crash dumps during the 2026-11-05 device-loss hunt; no isolated repro | yes / **conflict** in `vk_rasterizer.cpp` (small rebase) |
| A4 | video_core: guard out-of-bounds TIC reads and zeroed texture descriptors | `5ed69c878c` | Upstream asserts `index <= tic_limit` and then reads past the table anyway; this returns an empty descriptor instead | Came with the load-crash fix branch; no isolated repro | yes / yes |
| A5 | memory_manager: guard the GPU address range in `MemoryOperation` and `GpuToCpuAddress` | `d0da791130` | Out-of-range GPU addresses read zeros, and device-space overflow returns `nullopt`, instead of indexing past the page table | 2026-11-05 batch; no isolated repro | yes / yes |
| A6 | video_core: publish the split ring after `ResumePending`; guard render-target bounds | `6cd3259195` | `ResumePending` never publishes the split ring, which can hang `PresentClientBuffer` until the fence times out | Diagnosed from a fence timeout; no isolated test. Upstream has part of it already: check the diff after rebasing | yes / yes |
| A7 | renderer_vulkan: don't treat mesh stages as tessellation; don't skip unbuilt mesh pipelines | `0fea015a51` | Upstream sets `has_tess_stages` from slots 1 and 2, so mesh pipelines get a patch topology. The second half is a policy choice: with async shaders, waiting for mesh pipelines (no LOD pop-in) instead of skipping them | Code inspection; consider sending the two halves separately | yes / yes |
| A8 | video_core: no debug readback by default in the upscaler stand-in; re-serialize failed disk pipelines | `29dae80d16` | Upstream defaults `EDEN_NX2_SAMPLE_AT` to 20, so a debug GPU readback runs for every user; failed disk pipelines are never erased or rebuilt | 4 lines, code inspection | yes / yes |
| A9 | settings: asynchronous shader compilation on by default | `f36f16a1c9` | Fresh configs froze on every new shader | Tester reports; policy change that upstream may decline. Best sent with or after B4 | yes / yes |

## Tier B: generic fixes that need a manual rebase or split

| # | PR | Commits | What | Work needed |
|---|---|---|---|---|
| B1 | vk_pipeline_cache: rename an outdated `vulkan.bin` instead of deleting it; guard `disk_pipelines` | `f2ce6c3f0f` | Keeps the old cache across version bumps and fixes a boot race | Conflicts in `vk_pipeline_cache.cpp` (upstream is at `CACHE_VERSION` 31). Add the rule "bump only when the format, key or recorded data change" to the PR text. 9 of the last 11 bumps here were avoidable, and each one deleted every user's cache |
| B2 | usage_tracker: fix `PageMask` UB at `num_bits == 0`; reset the tracker per command buffer | from `c4573fabb1` | Upstream still computes `~u64{0} >> (64 - num_bits)` | Hand-extract the `usage_tracker.h` hunks only; build and test again |
| B3 | texture_cache / shader: two one-line fixes from the lowering commit | from `59c4bde459` | `ImageBase::TryFindBase` truncates a >4 GiB delta to `u32`; ISBERD's immediate bitfield overlaps the skew bit | Hand-extract; add a unit test or a repro first |
| B4 | vk_pipeline_cache: per-frame shader build budget | `c9b8bb5e52` + `e330d49488` | Spreads run-time shader translation over frames: on an empty cache, the worst area-load frame went from 1.69/2.27 s to 0.60 s | Uses the local `Nx2Perf` switch framework, which upstream lacks; turn it into a setting or env var. Side effect to document: objects pop in or show as white silhouettes for a moment |

## Tier C: measured speed-ups that need a rewrite for upstream

These depend on this branch's switch framework (`src/video_core/nx2_perf.h`) and its profiler,
neither of which upstream has. Each would have to be rewritten without them, measured on more than
Ocarina, and pass a screen-flash check, since timing changes caused flashes here before.

| # | Change | Commits | Measured |
|---|---|---|---|
| C1 | Submit while the split front waits or the GPU is idle | `8705321017`, `ead0ff6ade`, `d1f7f92b27` | Kokiri spawn 38.7 → 45.7 FPS (2 runs per side), fence wait 13.7 → 10.3 ms, verify clean. The largest single gain |
| C2 | End the GPU thread's 1 ms sleep on a submit (`pending_wake`) | `94424afdbd`, `ac4d3b2ce4`, `6a8a624359` | +1.5 FPS, ~1 ms per frame less fence wait, flash check clean |
| C3 | Early MME DMA reads outside GPU-written 1 MiB regions | `bc34d467b6`, `5f795179b0`, `59bd152917` | About +1 FPS after the correctness fixes. Mixed authorship (thibaut, Lichii #179): coordinate with them |
| C4 | `dma_forward`: macro DMA reads on the split front without a drain | `a8a3563d00`, `dd76044228`, `4a8a8b5fda` | Verify: 1.57M calls, 0 differences. +1.9 FPS alone, but only noise-level once combined |
| C5 | Skip the dirty-memory gather when nothing was collected | `e073cc0b9e` | Unit tested; FPS not measured |
| C6 | Read a stage's texture handles through one page translation | `f30ce237c8` | Unit tested; FPS not measured |
| C7 | Draw-written command lists and batched indirect resets | `b33ef6014b`, `27a56ad29f` | Not measured; motivated by Donkey Kong Bananza |
| C8 | Per-second frame-time percentiles in the log (`Nx2Frame:`) | `5f21b5f882`, `c1abca904e` | Tooling: how every number here was measured. Probably off by default upstream |

## Not ours to send: ports of other people's work

These are other people's commits, picked into this branch from Lichii, other forks and open
upstream PRs. Upstream should get them from their authors: point the author to this list, or
mention it on their PR. The shas are the local copies on this branch.

**High value**
- Brandon Bunnelle: `47f8345228` + `bf96316653`. Turing macro RAM 0x8000 words plus a `ClearCode`
  fix (his open upstream PR #663; fixes Ocarina's "Native MME failed" and flicker). Applies cleanly.
- Gavin Schiller: `6397a4ac8b` + `671768a0ed`. Land deferred clears before non-pass image writes
  (Lichii #185; fixes Ocarina's terrain blotches).
- jpkto: `b514e5f0f2` (#351, table-pointer pools) and `8e1b50ca4c` (#353, unwritten render-target
  components). The user confirmed in game that the haze is fixed.
- lichii / thibaut: `35fa1ba02f`. Wait for pending query results before macro DMA reads
  (Lichii #215; fixes overexposed flashes). The local `08b5ed82eb` (a 2 s bound on that wait) is a
  possible follow-up.
- nyakuoff: `4027efccce` + `40111ba760`. Trap-and-flush native CPU reads (Kokiri 25.97 → 30.75 FPS).
- Matt Artist: `767f9b36fe`. Unrolled software HMMA K8 groups. Applies cleanly.
- gunvalk: `2c5caba758`. Ocarina at real-time speed below 60 FPS (#216).
- James: `a49bc963d2`. IMAD uniform-multiplier register shadow. Applies cleanly.
- win: `11d59f1e6d`. 5/8 VRAM budget on large cards (Mario Kart World 53.0 → 58.7 FPS).
- PantACRO4life: `238124fe3e`. float16 host shaders so LSFG loads. Applies cleanly.
- Alleexxi: `89a45908a5`. vi2 returns the release fence (fixes Wonder's 30 FPS lock). The local
  `9d3354de75` (multicore-only, plus a backstop) is a possible follow-up.
- Marc Stampfli: `682f0640dc`. RangeSet on a sorted array (Lichii #481).
- Zortos / mstampfli: `0aeaec747c`. HMMA.16816.F16 on cooperative matrices (Lichii #582).

**Medium value**
- **Zortos / zortos293:**
  - `8d186814fa` (#112)
  - `87773ca72f` (#124, Ocarina's Max Resolution patch)
  - `7a5ca62ab0` + `26618817a7`
  - `979991233b` (#398)
  - `0d58dbc92e` (#485)
  - `58f2ecb591` (#436)
  - `ba161f22a2` (#580 / #478)
  - `7353bf81f9` (#414)
  - `6db25ea841` with `f046b65a9d` and `92c214fe86`
- **chuppatricky:** `13e12a6ce9`
- **Gavin Schiller:** `4361022e2f`
- **RsMan-Dev:** `7306941b44`, `573f05c4c1`
- **Frysix:** `d4b73a14fc`, `a0926757d6`
- **Shrxmy:** `4950e91f33` + `9adfd1b28a`
- **Felipe Lima:** `af300c0f32`, `b1788ea326` (#329)
- **Paulius:** `5830dfd16f`
- **resadent:** `da76cb072e`
- **thibaut:** `6f50adf4c8` + `7e443f496a` + `c869a431d2` (partly upstream already), and `2392ace81a`
- **Kajcsi:** `7795aac2a8`, `a35ccb90a9`
- **Ghostwolf13:** `2c399c6a18`
- **PantACRO4life:** `5bf6ac030d`
- **nowhere02:** `aea4a24e57`
- **Frizti:** `94cab67bd8`

## Already upstream, or not needed there

- **Save-thumbnail fix** (`3f77c57997`, Dynarmic AVX-512 CLZ, by kraibse via zortos293/Lichii#8):
  upstream already uses `vpmovdw`.
- **GCC unused-variable fix** (`2b416c3889`): upstream never had that variable.
- **Not proposed:**
  - agent and lab tooling;
  - Ocarina-only experiments that were rejected or are off by default;
  - reverts and work replaced later;
  - noise-level switches;
  - today's untested shader-stutter switches (`compute_rebuild_async`, `stale_target_skip`,
    `geometry_consistency`, `build_order_priority`, `replay_disk_recordings`,
    `compute_preload_variants`), until they pass the lab checks.

## How to send one

1. Branch from upstream: `git fetch upstream && git switch -c pr/<topic> upstream/nx2-experimental`
   (or `upstream/nx2`, whichever the maintainers ask for).
2. `git cherry-pick <sha>`, or squash a group. Keep the original author on other people's work.
3. Build, run at least one game, and write in the PR what was tested and on what hardware.
4. Follow upstream's `CONTRIBUTING.md` and `docs/policies/AI.md`: an `Assisted-by:` line for AI
   help, and no `Co-Authored-By` for AI.
