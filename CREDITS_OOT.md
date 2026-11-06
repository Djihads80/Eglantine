# Credits: Ocarina of Time stable state

This branch stands on a lot of people's work. Everyone below has commits on this branch that are
not in `Djihads80/Eglantine` `nx2`, or is credited in one of those commits' messages. Names are
written exactly as git records them; a few may be the same person under two handles. If you're
missing or listed wrong, open an issue or tell the branch owner and it will be fixed.

The emulator itself is Eglantine, built on Eden and yuzu: thanks to everyone who worked on those.

**Every contributor's individual commits, linked:** [jump to the list](#every-contributors-commits).

## Branch owner and integration

- **AManWithAPlan** (68 commits): integration on `local-combined`, the Ocarina crash and
  performance work in this state, porting and conflict resolution of the PRs below, testing.

## Code authors (commits on this branch)

| Contributor | Commits | Notable work |
|---|---|---|
| kraibse | 35 | Ocarina diagnostics (flash watch, present bursts), native 64 KiB uniform bank re-uploads, FP16 texture packing |
| zortos293 / Zortos | 16 / 5 | Lichii perf line: command-list waits, early MME DMA read quarantine, `EDEN_NX2_PERF_OFF`, fragment barriers |
| Duck912 | 5 | Ocarina's end-of-frame network as a compact software HMMA, per-tap gather offsets, F2I fixes, buffer download ordering |
| Jonas | 4 | Native storage buffer pointers through subgroup broadcasts, preflush downloads, MaxContinuousRange cache, core idling |
| thibaut | 3 | |
| nyakuoff | 3 | |
| Zarigis | 3 | |
| Shrxmy | 3 | |
| Gavin Schiller | 3 | Deferred clears before texture copies (lichii #185) |
| Felipe Lima | 3 | |
| jpkto | 2 | |
| SmileyTatsu | 2 | |
| Matt Artist | 2 | |
| Kajcsi / Kaj | 2 / 2 | |
| Brandon Bunnelle | 2 | |
| Frizti | 1 | Mesh programs that output points or lines; the command-writer classification ported from Frizti's work |
| Frysix | 1 | One write per console log line on Windows (lichii #199); found the occlusion-result stale read (#179) |
| gunvalk | 1 | Real-time speed below 60 FPS (lichii #216) |
| chuppatricky | 1 | RED/SURED register shadowing |
| Dr. Sergio | 1 | LayeredFS mods for NUP titles |
| resadent, rdenno, cr1ckt, SinCuri0, Paulo Tumabing, Paulius, Inc0rporate, 999sian | 1 each | |

## Credited in commit messages

- **Franseven**, **Quixomatic** (co-authors): native uniform bank re-uploads and FP16 texture
  packing.
- **Mattscreative** (co-author): the software HMMA network.
- **RomainROCH**: the review that made deferred clears land only when a copy proceeds (#185).

## Pull requests included

Lichii and upstream PR numbers referenced in this branch's commits (some refer to older Eden/yuzu PRs): #59, #70, #84, #89, #100, #101, #112, #120,
#121, #124, #129, #138, #141, #142, #149, #161, #165, #173, #174, #178, #179, #185, #189, #192,
#199, #204, #206, #215, #216, #387, #494, #538, #544, #556, #663.

## Tools

Some commits were written with AI assistance and say so in their trailers: Claude (Fable 5.1,
Opus 5.5, Sonnet 5.5), Google Antigravity, and capy-ai. The people above reviewed, tested and
own those changes.

## Every contributor's commits

Each name lists that person's commits on this branch, oldest first, linked on GitHub.

### AManWithAPlan

<details><summary>72 commits</summary>

- [`dce3f525f4`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/dce3f525f42b1182f6d34bf04380d32aec7121c3) video_core: wait for back thread idle hook in GpuSplit::Drain
- [`59c4bde459`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/59c4bde459ff652fcdcf18c3d53a6a5a5164caf8) video_core, shader_recompiler: fix Ampere/Maxwell lowering and texture cache lookups
- [`c495fc63e5`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/c495fc63e5ae1b0195f4058392ece04e23a75807) docs: add multi-agent git worktree workflow to AGENTS.md
- [`f38fb76e45`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/f38fb76e45ecb3d4edf4e0478b6e749897a6104c) video_core: Support draw-written GPFIFO command lists and batch indirect reset uploads
- [`36779002c5`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/36779002c521d73cc32df094b8fa4d803511dba1) video_core: Fix texture cache aliasing and DMA BPP for tutorial video rendering
- [`9389c71b6d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9389c71b6de2df22d53fe3c485e04be120dc67ee) video_core: Remove tautological tick check in JoinImages and revert DMA BPP override
- [`b33ef6014b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b33ef6014b576dd0a81269a70d62bd06edbbf155) video_core: Support draw-written GPFIFO command lists and batch indirect resets
- [`161b0cf219`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/161b0cf21967ebde6dcfcf8ce31c3e4f45e0ffef) video_core: handle unmapped memory writes and channel release cleanup
- [`36e627c684`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/36e627c684cae64c15245156d9ba52ade214af36) video_core: guard null address in compute reduction and signal query waiters in null rasterizer
- [`27a56ad29f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/27a56ad29f6cccdbfd7f8395484beba6d4709879) video_core: Fix regressions in written command list tracking and indirect reset
- [`2fd5a1f7f8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2fd5a1f7f809ed1a390048e702ec8329714eac35) tools/nx2: support Windows in lab_ctl launch and stop
- [`de69be8b07`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/de69be8b073a5ef9e7c55a28d81ccee847c9e3cb) chore: keep AGENTS.md untouched on feat/nx2-shader-lab
- [`6cd3259195`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6cd3259195bffc84abdf8702f9e367adb5508102) video_core: publish split ring after ResumePending and guard render target bounds
- [`df97d2a252`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/df97d2a25238dec2dd2b523e72f0264e25cb23a4) common, config: pin emulation threads to 3D V-Cache CCD on asymmetric L3 CPUs
- [`69af6e440b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/69af6e440b1fb72201e859cef76842ad9bc487cb) video_core, nx2:vi2: pace OoT presents on current frame and guard invalid formats
- [`b1ba8fa5ed`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b1ba8fa5ed1059e7526443dc7d7abdec5bd226f7) shader_recompiler: fix conflict resolution build issues in lower_to_maxwell and spirv_emit_context
- [`df6e79460b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/df6e79460baebf781f407a039f9d4d412466a696) shader_recompiler: preserve standalone TLD4.PTP lowering in unit tests and fix MSVC signed/unsigned check
- [`a20d457835`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/a20d457835eeb276980d1b6099b2ebb6d1477ecc) video_core: guard out-of-bounds TIC reads and zeroed texture descriptors
- [`ea06975da8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ea06975da88d8a7839531f7fd63ec5fa2a4f778a) memory_manager: 64 KiB page run cache for MaxContinuousRange and RangeMap interval reuse for GetMemoryLayoutSize
- [`62e060fc16`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/62e060fc166913590449f8bafe2b05d0b5d06f1b) video_core: add Maxwell3D::CallStateMethod fast path for macro-emitted state writes on GPUBack
- [`d3e85ff393`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d3e85ff3934609cb23bc668348a928547f96313c) video_core: gate upscaler debug readback on EDEN_NX2_SAMPLE_AT and re-serialize failed disk pipelines
- [`5ed69c878c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5ed69c878cbe2457ef8a46d12da73da389106a41) video_core: guard out-of-bounds TIC reads and zeroed texture descriptors
- [`29dae80d16`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/29dae80d167d7a6a3c98251b854396a827a08d5a) video_core: gate upscaler debug readback on EDEN_NX2_SAMPLE_AT and re-serialize failed disk pipelines
- [`2899dbeeac`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2899dbeeac102bb7915d898be303d7813d0074c3) video_core: promote recurring CPU read traps to preflushable and reduce GPU/compute spin overhead
- [`0112f30e16`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/0112f30e163f3945a46aeb0393d267a226500fca) video_core, memory: auto-promote recurring readbacks, reduce GPU front-thread spin, and fix threshold logging
- [`e0231d3afc`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e0231d3afc91ff3b1ec490da1db6c6f9d1dae9ea) renderer_vulkan: do not skip unbuilt mesh pipelines or treat mesh stages as tessellation
- [`0fea015a51`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/0fea015a51d8e70805d749b43c70b1c414ffa45b) renderer_vulkan: do not skip unbuilt mesh pipelines or treat mesh stages as tessellation
- [`6c75a66599`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6c75a66599e332d7182a3a5a26abc7655f3e2b16) buffer_cache, video_core, common: skip unchanged 64 KiB uniform bank re-uploads, cancel pending downloads before WriteBlock, and retry early UMWAIT wakes
- [`763b3f9b89`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/763b3f9b896efb94012c3286d0e2faceb7f81707) buffer_cache: keep native CPU readbacks on batched trap-and-flush instead of preflushing at fences
- [`c4573fabb1`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/c4573fabb110e091185643f6a81044cefe481242) video_core, memory: cache SSBO address resolution, fix per-cmdbuf UsageTracker reset and PageMask UB, and reuse contiguous page-table block IDs
- [`493ef92531`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/493ef92531c661ab70e056c407091a602328b974) memory_manager, vulkan, shader: key page runs by 4 KiB page, bump CACHE_VERSION to 26, and allow env-tuning OoT fog/exposure
- [`ea9e424dab`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ea9e424dab87be820503efe9e73865e472db2a65) video_core: per-bank 64 KiB uniform shadow & 4 KiB MaxContinuousRange run cache
- [`b8b49a2a94`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b8b49a2a94e3701cc6da1c13edca3ef40858bd23) video_core: cache rejected compute programs and skip redundant QMD reads
- [`85e483b40c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/85e483b40cb806cb3059a937545be171edf3f02a) video_core: eliminate QueryCache heap allocs and narrow split writer/compute methods
- [`be5af6547a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/be5af6547a7962c5ea34c683d91afb2c6964790c) buffer_cache: inline UnbindGraphicsStorageBuffers and UnbindGraphicsTextureBuffers
- [`d0da791130`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d0da79113049d574f53e4eeba4de695f513af21e) memory_manager: guard GPU address range in MemoryOperation and GpuToCpuAddress
- [`b06d1485ac`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b06d1485ac15515f9f3e1d5515039f13a4266cde) video_core: inline SetUniformBuffersState and skip empty per-draw stage/view loops
- [`3007dab0f7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/3007dab0f77dee1f666b47557252b1e6e49beee6) video_core, vulkan: inline uniform/scheduler fast paths and deduplicate consecutive sampler lookups
- [`e052226a76`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e052226a769450b67447520ab7e75ea1fd3cefc7) video_core, vulkan: optimize compute dispatch binds, descriptor acquire, and dynamic vertex input
- [`768f937697`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/768f9376979ad4482e6e3c4eacd7a65e17337cc6) memory_manager: bump map_serial and reload snapshot serial under runs_guard
- [`b4c0a17739`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b4c0a177399f02ca5f63a3c5194ac407eb2f93cb) video_core, vulkan: reuse compute stream uniforms and descriptor buffers
- [`48982ecd3b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/48982ecd3b9f1b94160e7ad4856be21427658456) video_core, vulkan: incremental descriptor buffer writes and per-draw fast paths
- [`8d95efb217`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8d95efb2174ced0d9c533eb78cb0a117a268504c) video_core, common: contiguous LRU cache and lock-free shader/compute pipeline caches
- [`d78b120e35`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d78b120e355a7794a5b244fd047912fd935f4e7a) video_core, vulkan: keep deferred-clear render passes active and fast-path descriptor/dynamic state updates
- [`98d254633f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/98d254633fad015d844004d1ecc94d5139d2961e) video_core: bulk macro DMA reads, bulk compute macro parameters, and skip front macro dirty defer
- [`9b06c4fd38`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9b06c4fd38f10febf9bbefe7f00a2458f853b9e7) video_core, vulkan: batch split state methods, track unresolved UBOs, and deduplicate draw push constants
- [`143d446e79`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/143d446e79132bfdd088938068785df45b128c3d) video_core, vulkan, core: fast-path CB map_serial binds, retain deferred fragment barriers, and coalesce invalidations
- [`25e1b0546d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/25e1b0546d4274248598c5ba82b393a559ec195a) video_core, vulkan: single-lookup buffer writes, vertex/index map_serial fast paths, and bitmask RT feedback checks
- [`51294580a5`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/51294580a5e9de88fb0f55063a502144637546d5) vulkan: update last_rescaling_words to 6 words for HMMA push constants
- [`59fa202572`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/59fa2025722dfe6349b588c899d31bd3530134ef) video_core, vulkan: zero DescriptorUpdateEntry padding, heap-free WordManager batches, and per-draw SynchronizeBuffer deduplication
- [`6ee6d1dfd6`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6ee6d1dfd671df5aff62c6075c456db32bfa7672) video_core, vulkan: cache uniform bank shadows, skip no-op ConfigureDraw, fast-path PrepareImage, and trim QMD/CB overhead
- [`78e6e4c5af`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/78e6e4c5aff3816ef2965737c3b4e508f372b323) video_core: one GPU-written lookup per buffer write, heap-free tracker batches (Nx2Perf)
- [`60395122f8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/60395122f8b21968e49337a4751f795d319e6005) video_core: trim compute release scratch and allocations behind Nx2Perf
- [`678e138f1d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/678e138f1dcf7be041ea02eb2f683f0a5985beb2) vulkan, video_core: invalidate descriptor buffer after BindDescriptorSets and fix per-draw buffer sync tracking
- [`43b6b17c3a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/43b6b17c3a9f5b73f7e19c5f1285d024e36ebb52) buffer_cache: clear deleted buffer ids in every channel
- [`973082a1cc`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/973082a1ccd73c0fd4ef620390d96f66b24a20b2) video_core: word-wide GPU-written page marks, constexpr page size, gate split overlap
- [`b5a01b8e98`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b5a01b8e98fb14386a404c4c17e31de8b18b5509) buffer_cache: preflush GPU writes the CPU reads, native titles included
- [`8f75c9ef76`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8f75c9ef7695b220af3eaf00a1941c993d5a352e) video_core: flush the query cache before macro DMA reads under the GPU split
- [`6e1d1f85f8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6e1d1f85f89cb9af68ff28675685a905652d45ef) video_core: plain native compute state is not a command list writer
- [`33ff21eceb`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/33ff21ecebaed095c17cdb25266295a2b70d911d) fixup: brace and query flush scope from the cherry-picks
- [`51bded1730`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/51bded17306e4077ae8b469352089f904cb75e44) fixup: define the writer helper after MacroRegistersStart
- [`d45dd6ca41`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d45dd6ca4186207478132b1e5feee1467650ffb9) tests: software HMMA checks for the unrolled K8 groups
- [`35fa1ba02f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/35fa1ba02f10696ef6ffc8f0ef03b5c23dcdae8e) video_core: wait for pending query results before macro DMA reads
- [`bc34d467b6`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/bc34d467b615fbe4c9ef6e7999ffe6b7994557b1) video_core: early MME DMA reads outside GPU-written 1 MiB regions
- [`5f795179b0`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5f795179b0e8ce60dcc6287b012bf129d0f1eab5) video_core: reports and fence releases mark their pages GPU-written
- [`08b5ed82eb`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/08b5ed82eb2312795220732eac6cceba049114a1) vulkan: bound the wait for query results to 2 s
- [`1fa3108afb`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/1fa3108afb90b031e50651ce031f412d4bec4496) puller: skip semaphore drains that only fed log lines
- [`b591289d7b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b591289d7b4629bea0bd3ac441fca7c9310bd0f6) docs: Ocarina of Time stable playable state (2026-11-05)
- [`0c612cee55`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/0c612cee558fe6c978bb61458423dc99764d3dbe) docs: credit the contributors to the Ocarina of Time stable state
- [`85763598a6`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/85763598a6b45f03150d3d01eefe77262c5315e8) docs: how the Ocarina of Time stable state was reached
- [`b79ffba039`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b79ffba0397e7ce4e9853bf7ebf615cbba045cd0) docs: point the README at the Ocarina of Time state and list open tasks
- [`b8597ed1b9`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b8597ed1b9a1a3fd44ea8750d792432e7eae5a54) docs: mark the fork as result first, code not necessarily peer-reviewed

</details>

### kraibse

<details><summary>35 commits</summary>

- [`41e7291f79`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/41e7291f794ce5049c1de6f1b1f08db9b07d215c) shader: lower Ocarina ISETP.EX accumulation and F32 global reductions
- [`245d902e98`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/245d902e98649b452650d04b906b213aae225190) shader: lower more Ocarina compute and high-register graphics forms
- [`2d29137f25`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2d29137f253c6a51363033096e615b8b63f12598) shader: lower Ocarina CALL helpers, 64-bit shifts and high uniform forms
- [`502e6e2f24`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/502e6e2f24f9b8cc6ca9c6ff26555b8fb9cdf92e) shader: lower Ocarina guarded uniform compares and uniform SEL
- [`f918505146`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/f9185051468175952d26c05c6fe1ae4a7cfe6749) shader: lower Ocarina 2D array, per-pixel offset and depth gathers
- [`8424eeb253`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8424eeb2531a3bfb4788918405996f21adf17365) shader: lower the rest of Ocarina's dumped native programs
- [`01783ceb29`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/01783ceb29d2d94bda72f273003323a2a446c7ae) shader: route untracked 32-bit global atomics to their storage buffer
- [`dc3e912fb7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/dc3e912fb76d0b318b4a14007d771017a3fb586b) shader: pack FP16 texture results with c0 in the low half
- [`8de2d42a2e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8de2d42a2eefb67b97963de760b0041c20747629) buffer_cache: re-upload a native program's 64 KiB uniform bank once per frame
- [`b536a971a5`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/b536a971a54524ef271be60baef830e368529797) video_core: upscale stand-in takes its images from the native program
- [`2c2dd2197e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2c2dd2197eee93238af8fb8f356453a07fe51f19) buffer_cache: size native graphics storage buffers like compute ones
- [`0e97fc8ca4`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/0e97fc8ca4b43bcd009d58c9d2635a2696b39b37) macro: flush pending query writes before native MME DMA reads
- [`828f158b84`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/828f158b841e305d11c59bd083b068b0b1d5aaeb) tools/nx2: add shader triage and VMA analysis tools
- [`10cc70085b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/10cc70085b0d94dfd347c0049a6a953192b3c800) nx2: add draw and dispatch diagnostics
- [`bd0a8f5099`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/bd0a8f50998887f0b267ed5cf35080063bd6a31b) nx2: add program keys to the diagnostics draw list
- [`4b0b94258d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4b0b94258dfc623fb0259a9401bed511ca787321) nx2: add a live shader lab for the diagnostics
- [`aa992a227c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/aa992a227c3708ee814ba697a38e4fc8a274ea77) nx2: add a per-draw pixels-written probe to the diagnostics
- [`3287f9adf4`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/3287f9adf41d1ca4fe851785ba017f719eb08bc4) nx2: start the shader lab from the emulator, add clear buttons
- [`5f968ab56e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5f968ab56e9307f7d7070531b7bf28dbbfd506e2) nx2: remove a stale lab address before starting the shader lab
- [`0ec76d1599`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/0ec76d159940ad8b697a5aa4c8f9fde2d574cd5c) nx2: start the shader lab when the process starts
- [`99f6fb7a61`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/99f6fb7a61188da4cb54ac286dfb94a9cf062f59) nx2: save a screenshot with each diagnostics capture
- [`fa1d9eb052`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/fa1d9eb05207efd16f9776e2910113029d2718bd) nx2: scriptable shader lab control, live keys for eden-cli
- [`787571969c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/787571969c08ac081408d1231d07d134ca3703ef) agents: vendor the NX2 shader triage and lab automation skills
- [`cb56c7c423`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/cb56c7c42300e621d3030ab811b1d9a2f8993969) video_core: run front-side buffer flushes on the GPU split back thread
- [`451e34eb3a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/451e34eb3a65bd146daaa9afb9cbcc7eb63c2973) video_core: run the front's fence work, macro DMA flushes and maps on the split's back
- [`f1db39100b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/f1db39100b9f115242f81cf9ef0dc1ee1b916d5d) shader, video_core: bind storage buffers behind loaded pointers for OoT's vegetation
- [`918c84d5b3`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/918c84d5b3d1fd040014150c438b824af8555bbc) engines: hand the macros back to the engines when the GPU split stops
- [`7be56014df`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/7be56014df93f6d3677d593bfa1b4bc899b3ef3f) fence_manager: submit deferred work before waiting on our own fence
- [`c445b1490f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/c445b1490f0252ae316be7cf4dc330262e46e692) puller: release fences again while an acquire waits with delayed fence operations
- [`3c6fab65bf`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/3c6fab65bf4c466fecb1d6cfe9e6961c5ad0c47e) buffer_cache: re-upload a native program's 64 KiB uniform bank on every bind
- [`aadca53092`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/aadca530920b50a5c071904088778d1f6a81aeb6) shader: lower Ocarina's byte uniform loads, ULEA carries, F2IP pairs and I2F.U8 of a UR byte
- [`d5465e7ca0`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d5465e7ca00a060db63245901c2064290aeb361e) buffer_cache: let the native bank re-upload cover banks of any size
- [`dfbe893c4c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/dfbe893c4ce5282221ad77f6cab73ca7e68cfcfd) yuzu_cmd: set the GPU accuracy from the live keys file
- [`ffa6d615da`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ffa6d615da243414509dd325da65cf28b2f559f7) nx2_gsv: save a burst of consecutive presents on request
- [`51c57567b6`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/51c57567b69b8b1d1d61e5fb5751ab005bd7f6ea) nx2_gsv: EDEN_NX2_FLASH_WATCH counts near-white pixels and keeps the flashing frames

</details>

### zortos293

<details><summary>16 commits</summary>

- [`a6dea8a75d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/a6dea8a75db8e080de793228d22e964b0cbc3798) shader: drop the U64 clamp from lowered SHF.R funnel immediates
- [`26618817a7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/26618817a7c363d8a7ddf089b43cecfab6cc8c04) video_core: scale Ampere GPU time since the last present under Boost and Overclock
- [`62701342cf`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/62701342cfa3dd9757cf9f2235370616249759af) TEMPORARY: opt-in OoT fog/exposure visual workaround
- [`5dab944282`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5dab9442825bc8c7a8f57bff4b2328daca014238) vulkan: round the alpha test reference to 1/255 in the pipeline key (upstream #544)
- [`3b43ff038c`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/3b43ff038ca9306a3e3b1b1de28d3db391c4f510) video_core: EDEN_NX2_PERF_OPT master switch and EDEN_NX2_PERF_VERIFY for the back-thread optimizations
- [`45c4f09d23`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/45c4f09d23e6d365fcaf6401e04f59b60a59666f) vulkan, nx2_gsv: port #161's submission batching and Ocarina frame-ahead present behind EDEN_NX2_PERF_OPT
- [`de5c30856e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/de5c30856e7192e51c9215599067f67cb7acefd4) vk_pipeline_cache: keep sixteen sessions of used pipelines and save the list every 15 s (#161)
- [`e3ae51f537`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e3ae51f5371add3c15c93db7d109aef47c8c1119) buffer_cache: skip clean tracker words, refilled usage pages and repeated index binds (Nx2Perf)
- [`d0869ed882`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d0869ed882c2c99f07b9a4a2bdfe24f75c87d189) memory_manager: answer MaxContinuousRange and GetMemoryLayoutSize from runs kept at map time
- [`e91293acc7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e91293acc7b25c592863644503a476c4725d085e) video_core: cheaper Maxwell3D method dispatch on the GPU back thread (Nx2Perf)
- [`4cf30d7ced`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4cf30d7ced6a3cda2c44ea73fae245253d342830) video_core: split front waits only for the queued work an MME DMA read depends on
- [`8a5158e9a5`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8a5158e9a51116b2f766ea58e78f746946366faa) video_core: keep batched CB writes within one device page
- [`19e2f41e73`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/19e2f41e73ae782d97dffaeb5ab2f26b22eb6e4a) vulkan: retain deferred fragment barriers across unrelated draws
- [`755974328a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/755974328a43476cdbe41cfac2facca0eb7c5438) nx2_perf: EDEN_NX2_PERF_OFF=<name>,... turns single optimizations off; the state is logged once
- [`71cfd178bb`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/71cfd178bbca86a051633733e2d9c3d599a8d83a) video_core: quarantine unsafe early MME DMA reads
- [`df4eeebb7a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/df4eeebb7a2dbc70b3da2d62b4f3be4a8a91babc) video_core: command list fetches wait only for the work that may write them

</details>

### Duck912

- [`ab3ece2ea1`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ab3ece2ea13bc26be26065463ebd0bf4fcd89e2b) shader: fold multiplies by 0 and 1 so Ocarina's sky ambient binds its buffer
- [`11447ea438`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/11447ea438e663216e8820968c089e7903a6877c) buffer_cache: don't download GPU data over newer CPU writes
- [`4d5b718234`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4d5b718234c614154dd61f72b02bff89a868d7d0) shader: apply F2I constant-buffer abs and neg
- [`adcf05e2f3`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/adcf05e2f3de5e06e1315cf40a1b9b6b4675737b) shader: gather each tap with its own offset when per-tap offsets stay dynamic
- [`9befa0a11b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9befa0a11bb56786bcb647c5dec9f799952f3e60) shader, video_core: run Ocarina's end-of-frame network through a compact software HMMA

### Zortos

- [`8d186814fa`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8d186814fa442a853dc44376f7d307cc6f5fe2ff) shader: lower the green-hat kid adventure's 1080p upscaler gathers (#112)
- [`87773ca72f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/87773ca72f72f0806a72301ec7b3671948157c00) file_sys: built-in Max Resolution patch for Ocarina of Time (#124)
- [`2e5f2716ac`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2e5f2716ac3aee48375aa30e2c329655636cd020) common: retry unsignalled MWAITX wakes until the Event deadline (#120)
- [`7a5ca62ab0`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/7a5ca62ab0241d674353638785c3747f38be7d47) video_core: use native nanosecond timestamps for Ampere (#84)
- [`ef98de39a7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ef98de39a7a8e9435df166fca51c642f5cc955b4) video_core: drain draw-written command pages only after a write, and keep fence work on the split's back (#70)

### Jonas

- [`e48d49e51f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e48d49e51f3f6c5aaba6e78b7b3f7953e3f13083) kernel: spin briefly before idling cores for Switch 2 titles
- [`a61ba59f01`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/a61ba59f01c1937fd2991bfa45bc1b3f08c70a5b) video_core: cache MaxContinuousRange until the GPU page table changes
- [`8d9395490a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8d9395490a9573d2a0c72a8ace68919d47c8625b) buffer_cache: download only the Preflushable parts of a GPU write at fences
- [`ff772eb8fb`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ff772eb8fb658dd567bb3b2deee062b6f84feac2) shader: follow subgroup broadcasts when finding native storage buffer pointers

### Felipe Lima

- [`5869d1d9ca`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5869d1d9ca28d64e57c8a5bc6bdcfa6d8e9a9788) vulkan: drop draws recorded after a graphics pipeline that failed to build
- [`af300c0f32`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/af300c0f32d4beec48f3a5749ac84371ccb00426) kepler_compute: square Ocarina's packed frame in the upscaler stand-in
- [`76aebe547e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/76aebe547e72c0f64567bbb5c5222becde1a8db2) vulkan: drop draws recorded after a graphics pipeline that failed to build (#142)

### Gavin Schiller

- [`6397a4ac8b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6397a4ac8b82e08d89c40eb57a48c82cc8cb0acf) texture_cache: land deferred clears before non-pass image writes
- [`671768a0ed`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/671768a0edb08049f09d0b5a75202e4a7da02fe7) texture_cache: only land deferred clears when the write proceeds
- [`4361022e2f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4361022e2f4a3d50832514f62c561082b6fc6087) shader: shadow the gradient and global atomic registers of TXD and ATOMG

### nyakuoff

- [`4027efccce`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4027efccce4739da5942963d86a545e5ba888808) buffer_cache: default native CPU reads to trap-and-flush
- [`40111ba760`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/40111ba760147e72cf185e6d89308a2e9a73c87a) buffer_cache: download every read-back range at a guest read that must flush
- [`cb4f95cb8d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/cb4f95cb8d8a6d27c89e2051ffe142be63234730) buffer_cache: cache the preflush environment override

### Shrxmy

- [`4950e91f33`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/4950e91f33d2bdb1b8c5e8120397b298808fbf31) shader_recompiler: implement 32-bit global ATOM.CAS
- [`9adfd1b28a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9adfd1b28a405dd528a8e24ec2fc8e7f211a53d3) shader_recompiler: route global ATOM.CAS and handle an R254 comparator
- [`257935fde7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/257935fde7d460f80357a11cac51eedd613f596e) core: serialize GPU dirty memory gathers

### thibaut

- [`6f50adf4c8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6f50adf4c894b8930f42a1b612a8e0bb226a855c) video_core: cheaper split back-thread bookkeeping for storage buffers and instance loops
- [`7e443f496a`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/7e443f496a39daf5192b899661aa54055b92a919) video_core: track the instance loop state on multi-method writes too
- [`c869a431d2`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/c869a431d25700c1535907f0ea5d3dc39e3e30ba) video_core: always send index_buffer.first from the split front

### Zarigis

- [`6dac08d328`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6dac08d328f57b321342a8f4dbd3f24bc59cf43b) video_core: invalidate caches after MemoryManager::WriteBlock copies the data
- [`3c6c24a38f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/3c6c24a38f1527c5a174de762614d756d09a49ca) buffer_cache: upload CPU writes onto GPU-written pages instead of downloading them
- [`efd03df47b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/efd03df47b08755dc811ee43365eaf0813b4feb2) shader: shadow ISBEWR data registers (Ocarina door NaN) (#100)

### Brandon Bunnelle

- [`47f8345228`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/47f8345228291bb01df0918abc2d3f7c94d8cfd7) macro: enlarge Turing macro RAM to 0x8000 words so entries past 0x1000 execute
- [`bf96316653`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/bf963166531034480a5532ab5c2c24d1b5912c15) macro: widen the ClearCode offset check to u64 and document MacroEngine methods

### jpkto

- [`cfd191a55d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/cfd191a55d85e70b5e48f9f773e7851c3738afc0) shader: retry compare-exchange loops with the value the exchange returned
- [`53b291c28e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/53b291c28ec54d8cd79f59eac5bf80b3308b50f9) shader: send f16x2 compare-exchange loops through the u32 word too

### Kaj

- [`565bcc214d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/565bcc214df3f0d4b3d4b408c029d48b1ca7fabc) vk_scheduler: pause, don't reset, the occlusion counter at each submission (Ocarina white flashes) (#121)
- [`86db146a7d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/86db146a7d67b1f9c5eda727ca194571d51fa582) texture_cache: skip the upload of a new render target a clear overwrites whole (#141)

### Kajcsi

- [`a7c8bf8703`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/a7c8bf8703fdf2418fd1f374e4cf08a52a657441) texture_cache: skip the upload of a new render target a clear overwrites whole
- [`f1ef3cb444`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/f1ef3cb4445fac179897bc0343d46656013a427f) texture_cache: let IsFullClear decide before a new clear target skips its upload

### Matt Artist

- [`ae5385f499`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ae5385f499102f3fb3c36d701fb7b65edf06b33d) fix(shader): keep depth-compare images float when the descriptor reads as integer
- [`767f9b36fe`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/767f9b36fef938ed1aa1d483fc85b261d24a2603) perf(shader): unroll each K8 group of the software HMMA

### SmileyTatsu

- [`9e643218a3`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9e643218a3c91ef1c70ef7660d19ba2763122804) core: release the guest RAM when a Switch 2 game stops
- [`22901986a1`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/22901986a1d971c28fccfe93013b9316a8c29b5c) video_core: reuse resolved SSBO bindings and cache last mapping interval

### 999sian

- [`66432b3e68`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/66432b3e684257d4efd870863edf02947707c048) common, dynarmic: calibrate invariant RDTSC, inline CNTPCT_EL0, and use x64 TSO loads/barriers

### chuppatricky

- [`13e12a6ce9`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/13e12a6ce9bc13c93088cfaf340fc10712ce378e) shader: shadow the data registers of RED and SURED

### cr1ckt

- [`1b0445f997`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/1b0445f997b216b387e27093a4978553dd33876e) vulkan: fix invalid depth-stencil buffer copies in ReinterpretImage (#89)

### Dr. Sergio

- [`05a84bf337`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/05a84bf337d37b1e6093082f78f8237c8735774f) fix(file_sys): apply LayeredFS mods to NUP titles

### Frizti

- [`e45373d781`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e45373d781038cc25db6a35a2e8f3fa02ab88fef) shader: mesh programs that output points or lines

### Frysix

- [`d4b73a14fc`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/d4b73a14fc29d12f621471b5351c791c89b6ef1d) common: write each console log line in one call on Windows

### gunvalk

- [`2c5caba758`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/2c5caba7581922bd71d10863f68f1a8c36ad5df1) core: keep Ocarina of Time at real speed below 60 fps

### Inc0rporate

- [`61a05f088b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/61a05f088b3b3fe690e29c6f41666b030adc99f7) shader: count nested returns before moving demotes to the end (AMD black scene)

### Paulius

- [`5830dfd16f`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/5830dfd16f1b282c0df0e422d93d63b75f972f5c) core: detect Switch 2 XCI metadata before selecting the guest clock

### Paulo Tumabing

- [`8a2f253374`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8a2f2533748f20f0d1e31ae530202d22369476fc) Fix VP9 movie crash (dirty memory gather race) and let eden-cli restart a game (#138)

### rdenno

- [`e071d4133d`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/e071d4133dab77e140611c610dbbaa5daa8b0952) nx2: keep other threads out of the Vulkan scheduler while the split's back submits

### resadent

- [`da76cb072e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/da76cb072ef61aa7a8902958dbe22dcee10bda79) cmake: define NOMINMAX for Windows clang-cl builds (#129)

### SinCuri0

- [`ad37ad1cb3`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/ad37ad1cb30c4dbcd8844bcec45b110d7c58f611) video_core: store guarded and mesh pipelines in the Vulkan disk cache

## Credited in commit messages

### Franseven

- [`dc3e912fb7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/dc3e912fb76d0b318b4a14007d771017a3fb586b) shader: pack FP16 texture results with c0 in the low half
- [`8de2d42a2e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8de2d42a2eefb67b97963de760b0041c20747629) buffer_cache: re-upload a native program's 64 KiB uniform bank once per frame

### Quixomatic

- [`dc3e912fb7`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/dc3e912fb76d0b318b4a14007d771017a3fb586b) shader: pack FP16 texture results with c0 in the low half
- [`8de2d42a2e`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/8de2d42a2eefb67b97963de760b0041c20747629) buffer_cache: re-upload a native program's 64 KiB uniform bank once per frame

### Mattscreative

- [`9befa0a11b`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/9befa0a11bb56786bcb647c5dec9f799952f3e60) shader, video_core: run Ocarina's end-of-frame network through a compact software HMMA

### RomainROCH

- [`671768a0ed`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/671768a0edb08049f09d0b5a75202e4a7da02fe7) texture_cache: only land deferred clears when the write proceeds

### Frizti (ported work)

- [`6e1d1f85f8`](https://github.com/A-Man-With-A-Plan-dev/Eglantine/commit/6e1d1f85f89cb9af68ff28675685a905652d45ef) video_core: plain native compute state is not a command list writer
