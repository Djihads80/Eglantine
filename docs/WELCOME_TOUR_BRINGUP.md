# Welcome Tour native bring-up (upstream Eglantine, nx2)

Pinned revision `50c9fd3471d5f0170a7495ee10fd8b0b2f0298e1`. All work is local and
uncommitted; the original dump is read-only and hash-checked before/after every run.

State (build437): **authentic Welcome Tour intro video frames are visible** through
the game's own GPU drawing/presentation path. This is not interactive gameplay
or proof of working 3D scenes. Both 0.5x and 1x renderer runs expose a composition
scaling defect; native-scale verification is tracked below. All work remains
local, uncommitted and unpushed.

## Verified fixes

1. `NormalUnmapGpuMemory4K` reply (`src/core/hle/service/nx2_gsv.h`, raw nx2:gpu 213)
   returned one word of "success" but the SDK subtracts a u64 byte count from its
   address space's mapped-byte total and then asserts (`DestroyGpuAddressSpace` +
   `0x1c` -> `nn::diag::detail::Abort`). The handler now returns the byte count that
   `Nx2GpuMemoryRegistry::Unmap` actually removed (`TrimMappings` returns it), and
   `TrimMappings` counts only removed backing bytes. Tests:
   `src/tests/core/nx2_gpu_memory.cpp` (4 cases, 13 assertions).
2. `nvhost-as-gpu` mapping modify (ioctl `0xC` / `0xc020410c`) was absent, so
   `NvRmGpuMappingModify` returned error 2 and NVN textures kept GPU address 0.
   Implemented as `nvhost_as_gpu::ModifyMappingKind` -> `MemoryManager::ModifyKind`
   (validates range, flushes and invalidates, updates the page-kind map on the GPU
   thread, waits). Unsupported kinds still return `NotSupported`, so nothing is
   forced. Characteristics bit 34 (`SUPPORT_MAPPING_MODIFY`) is advertised next to
   bits 41/42 in `ApplyNx2Characteristics`.
3. Presentation address: `Layer::ConfigureDraw` calls
   `rasterizer.AccelerateDisplay(framebuffer, framebuffer.address + offset, stride)`,
   i.e. the framebuffer address is a **DAddr** (SMMU/device), not a guest CPU VA.
   Passing the CPU alias made the raw fallback read unrelated memory (white noise).
   `Composite` now passes `*device`.

## Confirmed runtime evidence

Latest runs under the test workspace's `evidence/` directory:

- `welcome-tour-eglantine-jsnksc3b` (30 s, `--capture-frames --capture-draws`):
  13 presents, 65 render-target captures, 8 TGA frames. Offscreen targets
  (256x256, 351232 B) contain data. The two window images are 3840x2160,
  pitch 0x3c00, mapped both as guest VA `0x814a450000`/`0x814c430000` and as
  CPU VA `0x1fa8018000`/`0x1fa9ff8000` (gsv 211 handle 2202, gsv 423 bind).
- `welcome-tour-eglantine-4cplkwjk`, `...-7n8k974c`, `...-_qnmmhxp`,
  `...-lt8x69md`: scanout dumps at present 3 and 150 via
  `EDEN_NX2_DUMP_PRESENT` are 0x1fe0000 bytes and **all zero**, even after
  `FlushRegion(device)`.
- `welcome-tour-eglantine-eeav40x9` / `...-lt8x69md`: `EDEN_NX2_RT_TRACE` shows
  the guest does draw into both window images (draw 12 -> `0x814a450000`,
  draw 15 -> `0x814c430000`), plus small offscreen targets.
- `welcome-tour-eglantine-eeav40x9` (`EDEN_NATIVE_FRAME_CAPTURE`):
  `Native display source: address=0x1697c1000, dimensions=3840x2160,
  stride=3840, accelerated=false`. `TryFindFramebufferImageView` finds no cached
  image for the scanout DAddr, so the layer falls back to `UpdateRawImage`, which
  reads zeroed guest memory -> black frame.
- Earlier white/noise frames were an artefact of `--capture-draws`, which
  downloads render targets into guest memory in the raw (unszwizzled) layout; the
  fallback then samples that noise.

## Earlier blocker (superseded by October 30 checkpoint below)

Guest pixels for the window images live only in the Vulkan/texture-cache image
for DAddr `0x1697c1000` / `0x16b7a1000`; the compositor never finds it
(`accelerated=false`) and never downloads it, so the presented frame is the raw,
zero-filled backing. Next steps, in order:

1. Make the scanout lookup succeed: compare the cached render-target image
   (address, width/height, pitch, PTE kind from mapping modify) against the
   `Tegra::FramebufferConfig` built in `NX2GpuSession::Composite`
   (`native_tiling`, stride 3840, format Rgba8888). A mismatch in pitch, kind or
   size is the likely cause; neither side should be patched to fake a match.
2. If the image is present but not downloadable, download the overlapping image
   range before compositing (a real flush, not `FlushRegion` alone if that only
   touches buffers).
3. Then verify real 3D content. Note the Switch 2 video decoder
   (`INx2VideoSession` in `nx2_gsv.h`) is still a stub that queues buffers and
   returns outputs without decoding pixels, so any intro/video-driven content
   cannot appear until a real NVDEC path exists.

## October 30 checkpoint

- Cross-address-space identity: drawing and presentation previously resolved the
  same CPU backing to different SMMU addresses. `Nx2Map` now reuses a covering
  registry device range. Scanout lookup succeeds (`accelerated=true`). Borrowed
  entries retain `shared` on remaps and must not free a containing allocation.
  This remains experimental, not a full process-scoped/refcounted alias model.
- Native gather fix: NVIDIA `nvdisasm -b SM86` identifies the rejected instruction
  as `TLD4.SCR.R.PTP.NODEP`. Bits 76..77 encode None/AOFFI/PTP/invalid. Lowering now
  packs coordinates/offsets separately for the Maxwell gather. Depth comparison,
  invalid modes and scratch-register aliases still reject. Synthetic tests are
  in `src/tests/video_core/ampere_gather.cpp`; the independent disassembly probe
  is `evidence/welcome360/probe-gather.py` outside this source tree.
- `evidence/welcome360/run415.log` points to `welcome-tour-eglantine-pqbcry5b`:
  25-second bounded run, hashes unchanged; 3,600 submitted draws and present 600
  reached. No shader rejection/skipped pipeline after the gather fix. Scanout
  dumps at presents 3/200/600 contain **zero RGB bytes**; their 921,600 nonzero
  bytes are exclusively alpha=255. Eight captured frames are black.
- Correction to the previous session's status: "14 draws" counted distinct
  target addresses, not draw calls; "13 presents" counted bounded log messages,
  not presentations. Neither proves the guest stopped rendering. Nonzero bytes
  alone did not prove colored pixels. Offscreen procedural noise is not, by
  itself, evidence of a shader bug.
- `welcome-tour-eglantine-nfszv6xs` (`run416.log`): window source is
  `0x8147c70000`, B10G11R11_FLOAT. Initial no-color-write passes are buffer-clearing
  shaders (confirmed by SASS), not missing scene draws.
- `welcome-tour-eglantine-xil3omfi` (`run417.log`): 13 native shaders captured and
  locally disassembled in its private `shaders/` directory. No shader/game bytes
  added to the source tree.
- `welcome-tour-eglantine-06h0obbk` (`run418.log`): indexed scene draws appear
  around 9.88s, using a 6,704-byte vertex shader and three indices. One pixel
  shader writes white to `0x812c2d0000`; another writes c[11][0x320..328] to
  `0x8147c70000`. Those constants are 0.5 RGB, alpha=1 from c[7][0x20c].
  Viewport/scissor are 3840x2160, color mask 0x1111; vertex c[6] matrix is nonzero.
  These draws warrant investigation before blaming video decoding. Hashes
  unchanged in all four runs.
- `welcome-tour-eglantine-spgicmza` (`run419.log`): bounded later-draw capture
  after 600 submitted draws confirms the white-shader target `0x812c2d0000`
  and the scene target `0x8147c70000` are already zero immediately after drawing,
  before scanout. Scene position data is valid half-float vertex data (stride 8),
  not corrupt float32: (-0.5,0,-0.5,1), (0.5,0,-0.5,1), (-0.5,0,0.5,1).
  Scene draws have depth testing enabled and back-face culling; this is a
  diagnostic lead, not proof either state is wrong. Private runtime files hold
  register snapshots and bound constant buffers. Hashes unchanged.
- Build419 succeeds for `eden-cli` and `tests`; the focused
  `[ampere-gather],[nx2-gpu-memory]` suite passes 6 cases / 50 assertions.
  `git diff --check` passes. No commits or pushes.

Next: capture later scene draws, not just the first 16 initialization draws.
Check vertex/index inputs, vertex lowering/output, clipping/culling/depth and
immediate render-target contents. Do not force flags or insert pixels. The video
decoder remains a stub; its relevance to the current black scene is unproven.
`--capture-draws-skip` supports later captures; downloading images can affect
guest backing, so compare against runs without draw capture.

## October 30 video-path checkpoint (build436)

- Run420 (`welcome-tour-eglantine-wlatq6pp`) captures the actual indices
  `0,1,2`. The unskinned shader path and captured matrices project all three
  scene vertices to NDC y=0: these particular triangles are edge-on, not evidence
  that the rasterizer incorrectly discarded visible geometry. Read-only analysis:
  `evidence/welcome360/check-scene-vertices.py` (outside the repository).
- Run421 (`welcome-tour-eglantine-4_nayk51`, 100 seconds) reaches the intro movie
  after approximately 30 seconds. Shorter runs missed it. Run422
  (`welcome-tour-eglantine-49xh1evt`) captures the submitted configuration and
  compressed packets: Annex B H.264 SPS/PPS and access units, **not VP9**.
  Both runs preserve ExeFS hashes. Captured game bytes stay outside this repo.
- `host1x/native_video.{h,cpp}` now decodes those packets through the existing
  FFmpeg dependency, preserves presentation timestamps and converts to RGBA.
  The service writes completed frames into registered code-5 native images,
  using Switch 2 tiling and the descriptor's 128-row blocks. Other output layouts
  reject explicitly. This is experimental, not general video-service support.
- Run424 (`welcome-tour-eglantine-1_nculsv`) writes three actual 3840x2160
  decoded images, but the previous all-zero output synchronization handles do
  not get the movie consumer through to GetOutputInfo. A bounded queue now stops
  explicitly instead of accumulating unlimited decoded frames. The original run
  hit its memory cgroup limit; `exefs-after-recovered.json` confirms unchanged
  hashes after the launcher was terminated.
- Read-only guest trace427 (`welcome-tour-eglantine-fmlzirkd`) observes
  RegisterH264DecoderBuffer and AcquireH264DecoderOutputBuffer consuming the
  initially registered output slots with zero native sync handles. SDK
  TryWaitSyncPoint (`sdk+0x54f054`) confirms the native handle layout is
  `{u32 counter_id, u32 mode, u64 threshold}`. Counter IDs are complemented
  host1x IDs; zero is an already-signalled handle, not a pending image.
- Build428 allocates real output syncpoints through the NVDRV allocator and
  increments them only **after** the corresponding decoded image write. It
  gates input-ready notifications on available output images. Run428
  (`welcome-tour-eglantine-3dst53ef`) now reaches three real GetOutputInfo calls
  at 32.486/32.531/32.560 seconds, after their image writes. This is confirmed
  video-consumer progress, **not a visible-frame result**. All 12 captured
  screenshots remain black. The run then hits its memory cgroup limit; recovered
  ExeFS hashes are unchanged. No draw downloads were enabled.
- Build429 adds bounded image-allocation diagnostics to locate that memory
  growth. The launcher handles disappearing `/proc` processes and SIGTERM so
  bounded/OOM runs can retain hash-check evidence. Existing lifecycle stubs and
  video cancellation/flush semantics are not claimed complete.
- Focused tests in `evidence/welcome360/test426.log` pass: native-video guard,
  Switch 2 tiling round trip, local H.264 packets/timestamps, gather and GPU
  memory tests (9 cases / 106 assertions). No game fixture is distributed.
- Run429 (`welcome-tour-eglantine-bt9mye7i`) stops at the lower memory guard
  before the movie, with normal-sized image allocations and unchanged hashes.
  Run430 (`welcome-tour-eglantine-3xoh_80b`) again reaches three decoded outputs
  before its scope terminates; the launcher's final hash check survives SIGTERM.
- The transition also invokes previously unsupported raw GPU command212.
  `NormalModifyMemoryKind4K` (`sdk+0x3e9040`, request construction at
  `sdk+0x3e90e0`) lays out AS/first/last at words2..6, u8 kinds at words7/8,
  flags at word9 and job slot at word10. It returns the selected u8 kind.
  Build431 connects this to the existing actual `MemoryManager::ModifyKind`
  path across registered GPU address spaces, uses the requested uncompressed
  fallback and signals the job event only after commit. Unmapped, unsupported or
  failed changes stop explicitly. This still inherits the registry's existing
  cross-AS limitations; it is not a new complete address-space ownership model.
- Build431 succeeds for `eden-cli` and `tests`. Focused tests now pass
  10 cases / 122 assertions (`evidence/welcome360/test431.log`), including
  mapping-kind callback success/failure, range/kind rejection and holes.

- Run431 (`welcome-tour-eglantine-0yllcqql`) commits actual command212 changes,
  including the movie target at `0x8157b90000`; it still terminates at the movie
  transition without a visible frame. Hashes unchanged. Read-only trace432
  (`welcome-tour-eglantine-cwac5g5a`) observes the game retrieving native image
  handles after output completion; the decoder's safety bound stops the run.
- Standard renderer scaling to 0.5x reduces host GPU memory; guest display and
  decoded images remain 3840x2160. Run433 (`welcome-tour-eglantine-uh0dx70f`)
  reveals a guest panic in `main+0xaafea0` at the checked queue pop
  `main+0xaafffc`, after the first decoded output. This is not successful video
  presentation. Removing the old synthetic periodic-ready timer alone does not
  fix it (run434, `welcome-tour-eglantine-0sajkjus`). Both retain unchanged hashes.
- Build435 clears a reused syncpoint event before arming the next request and
  reads the wire mode as u8, not stale upper bytes (the logged mode3840 was
  actually mode0). Run435 (`welcome-tour-eglantine-6i0yc402`) consumes three
  decoded outputs without that panic before hitting the memory guard/explicit
  decoder queue bound. All 12 screenshots remain black; hashes unchanged.
- Static analysis finds **the exact input retry result**: following
  SubmitH264DecoderInputBuffer, `main+0xab9e7c..88` recognizes `0x1c266f`
  separately from general codec errors. Build436 returns that error before
  reading/decoding a packet or advancing its submission ID when no free output
  image exists. It does not drop a frame or claim completion. Native consumer
  fences are waited before image reuse; decoded timestamps and EOS are retained.
  Flush/cancellation/reconfiguration lifecycle remains incomplete.
- Run436 (`welcome-tour-eglantine-z1waecfd`, 50 seconds, standard 0.5x renderer
  scaling) confirms repeated real output-image release/reuse and increasing
  decoded timestamps. It reaches its time budget without the guest panic or
  decoder queue overflow; ExeFS hashes are unchanged. All 18 screenshots are
  still black. The game now queries/maps native movie images through428/426 and
  creates the movie's RGBA render target at `0x8159b70000`.
- The old426 handler returned CPU backing addresses without creating GPU PTEs.
  Build437 validates the bounded code-5 image backing and uses the existing
  shared SMMU/registry path to map it and commit kind6 before replying. Existing
  live mappings are preserved rather than remapped. This inherits the existing
  cross-AS ownership limitations; complete per-process image alias/refcount
  teardown is not claimed.
- `test435.log`: 10 focused cases / 124 assertions, now including H.264 EOS
  draining. `git diff --check` and launcher Python syntax checks pass.

### First visible game-content frame (run437)

`welcome-tour-eglantine-rm78g3h5`, build437, 48-second bounded run with
`--resolution-scale 0.5x --video-diag --capture-frames --scanout-diag --shader-dump`:

- Real native images3/5/4 are imported into GPU address spaces at
  32.235/32.301/32.337 seconds. Texture-cache allocations now observe those same
  addresses and actual 3840x2160 RGBA backing, rather than an unmapped image.
- Five captured renderer frames have nonzero **RGB**, not just alpha:
  `nx2-frame-01441`, `01561`, `01681`, `01801`, `01921`. Inspection of
  `frames/nx2-frame-01801.png` confirms recognizable Welcome Tour intro imagery:
  Switch 2 console, orange/right Joy-Con, miniature visitors and signage.
- This is the actual title-submitted H.264 stream, decoded into registered guest
  images, sampled by game shaders and captured from the renderer. No pixels,
  flags or guest code were injected; no draw-download diagnostics were enabled.
- The 0.5x run has a visible composition defect: content occupies the upper-left
  quarter of the 1280x720 capture. Do not call it correct full-frame presentation.
- The run reaches its time budget; `result.json` confirms unchanged ExeFS hashes.
  This is an **intro-video frame milestone**, not a playable-game claim.
- Run438 (`welcome-tour-eglantine-ktms9i0b`, 46 seconds, default **1x** renderer)
  independently reproduces visible intro imagery in four captured frames,
  including `frames/nx2-frame-01801.png`. It reaches its time budget and preserves
  hashes. The same upper-left-quarter presentation remains at1x, so it is **not
  established to be caused by reduced resolution**. Diagnose the game blit,
  compositor viewport and screenshot path before attributing it to any one stage.
- Final rebuild/tests439 passes the same 10 focused cases /124 assertions against
  the current source; `git diff --check` is clean. No commits, pushes or PRs.

Remaining: fix quarter-size presentation,
finish native-image alias lifetime/teardown and lifecycle semantics, then verify
interactive game rendering. Do not confuse decoded intro footage with game 3D.

## Build / test

```bash
cmake --build build --target eden-cli tests -j 2
build/bin/tests '[nx2-gpu-memory]'
```

Manual run (no timeout, isolated profile, hash-checked dump):

```bash
python3 launch-eglantine-mkw.py --title welcome-tour
```

Diagnostics used here (all env-gated): `--capture-frames`, `--capture-draws`,
`--dump-present 3,150`, `--native-frame-capture`, `--linear-scanout`,
`--gdb-probe evidence/welcome360/probe.gdb`.
