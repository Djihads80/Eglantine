# Kirby Air Riders: Global Test Ride — local Eglantine bring-up


**Verified current result (build39, 2026-10-30): the Global Test Ride title screen
renders, with readable logo/text/UI.** Screenshot:
`evidence/kirby-title39/title-screen.png`; working binary/source/launcher snapshot
in the same directory. Run: `evidence/kirby-air-riders-eglantine-xlcyz0i2/`.
The image occupies the upper-left portion of the presented frame. Other shaders
still reject. No interactive gameplay or3D-world compatibility is claimed.


Target: the owner's extracted **Global Test Ride**, not the retail game.
Pinned upstream revision: `50c9fd3471d5f0170a7495ee10fd8b0b2f0298e1`, with the
existing local Welcome Tour fixes. No commits, pushes or PRs.

## Test scope

- Original files remain read-only at
  a local extracted Global Test Ride dump (not distributed).
- Start with `base/exefs/main`; update65536 exists but is not combined or repacked.
- Each run copies an isolated profile and hashes every ExeFS file before/after.
- `--offline` runs the emulator in an unprivileged Linux network namespace with
  no external network. Do not contact Nintendo services or bypass the test's
  server/date restrictions. An offline limitation is not an emulator success.
- Use bounded runs, one guest core and the existing memory/log guards.

## Verified static facts

- Program ID `04005b902517c000`; META flags `0x37`, AArch64/39-bit, priority44,
  initial core0, stack1MiB, system resource128MiB.
- NPDM requests cores0..5 and priorities59..28; the current isolated emulator
  configuration selects one guest core.
- Five NSO0 executables: rtld, main, subsdk0, subsdk1, sdk; all flags `0x3f`.
  Base main text is `0x58524f0` bytes. Do not reuse Welcome Tour PCs blindly.
- Update65536 changes main text to `0x5855610`; the other executable file sizes
  match base (matching sizes alone are not proof of matching contents).

## Reproduction

From the test workspace that holds the launcher and the isolated profiles:

```bash
systemd-run --user --scope --quiet -p MemoryMax=10G -p MemorySwapMax=4G \
  env SDL_VIDEODRIVER=x11 python3 launch-eglantine-mkw.py \
  --title kirby-air-riders --offline --seconds 40 --stop-on-panic \
  --max-memory-mib 10000 --capture-frames --scanout-diag
```

## Runtime evidence

- Run1 (`evidence/kirby-air-riders-eglantine-veeoa1sx`): base executables run
  through native graphics initialization, real GPU imports/maps and command212
  mapping-kind changes. The emulator aborts (exit `-6`) around7.06seconds, before
  any captured/presented frame. The asynchronous log ends during another GPU
  mapping, which does **not** establish that mapping as the cause. ExeFS hashes
  remain unchanged. The emulator ran inside the isolated network namespace.
- Runs2/3 reproduce SIGABRT in `INx2GpuSession::SubModifyKind`, not in shader
  compilation. GDB exits0 after reporting a guest-emulator abort; the wrapper's
  return code must not be read as successful emulation. Both hash checks pass.
- Run3 (`evidence/kirby-air-riders-eglantine-294so1rp`) recovers the exact
  request from the paused host frame: AS1, GPU range
  `0x8104b20000..0x81075cbfff`, compressed kind`0xb`, fallback kind`1`, slot1.
  The handler rejected fallback1 before any mapping commit. This range had just
  been backed by a real import. NVIDIA's published TU102 kind definitions
  identify `0xb` as compressed Z16 and `1` as uncompressed Z16; GA100 retains
  these kinds. The old whitelist allowed only0/6/8.
- Local fix: admit Z16 fallback1 in both native212 and the existing ioctl
  kind-modification path. Keep mapping bounds/alignment checks and actual
  `MemoryManager::ModifyKind` commit requirement; signal only after commit.
  No forced completion, guest-memory changes or compression implementation.
- Build4 succeeds; `evidence/kirby-tests4.log` passes41 assertions/6 focused
  GPU-memory cases, including the exact Z16 request and failed-commit handling.

Kind reference (MIT-licensed published headers; facts only, no copied code):
<https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/common/inc/swref/published/turing/tu102/dev_mmu.h>
and `ampere/ga100/dev_mmu.h` in the same directory tree. External evidence copies
are under `evidence/kirby-{tu102,ga100}-dev-mmu.txt`, outside the source repo.

- Run4 (`evidence/kirby-air-riders-eglantine-dauztf9c`) passes the Z16 request,
  lowers native graphics shaders and reaches VI2 presentation. The only captured
  1280x720 frame is entirely black (all RGB extrema0); **not visible game content**.
  The40-second budget expires; exit-6 during termination is not attributed to
  the earlier Z16 rejection. Hashes remain unchanged.
- First explicit compute failure in run4 at9.424seconds: shader `0x11caa0f300`,
  instruction offset`0x360`, words `05ff73a1 00000000 00000000 000e2400`, rejected
  as `native warp match mode`. NVIDIA nvdisasm SM86 identifies this instruction
  as `MATCH.ALL P0, RZ, R5`, a32-bit match. The existing translator only accepted
  `MATCH.ALL.U64` and required even source registers. Evidence:
  `evidence/kirby-match5-disassembly.txt` (synthetic instruction variants).
- Extend the existing active-lane first-value/shuffle/equality/all-vote lowering
  to32-bit sources: one shuffle and one comparison instead of two. Keep64-bit
  pair validation and reject returned masks/unknown modes. This is real lowering,
  not skipping the instruction or returning a constant predicate.
- Build5 succeeds; `evidence/kirby-tests5.log` passes90 assertions/9 focused
  shader/GPU-memory cases, including32-bit R5, existing64-bit lowering size,
  emitted shuffle/compare/all-vote operands and unsupported-mode rejection.
- Run5 (`evidence/kirby-air-riders-eglantine-dfgmjmhc`) advances beyond MATCH
  to the next instruction, offset`0x370`: `05ff838c 00000003 03000000 0011e200`.
  nvdisasm confirms `@!P0 ATOMS.OR RZ, [R5], R3`; the translator previously
  implemented only ADD for this opcode. Extend this exact32-bit OR mode to the
  existing real shared-memory atomic OR, preserving predicate/address/value.
  Evidence: `evidence/kirby-atoms6-disassembly.txt`. Frame remains black;
  exit-6 with no budget stop requires a new native stack, independent of the
  compute failure. ExeFS hashes unchanged.
- Build6 passes101 assertions/10 focused cases (`evidence/kirby-tests6.log`).
  Run6 (`evidence/kirby-air-riders-eglantine-xvs_vqfs`) passes ATOMS.OR and
  rejects `REDUX.OR UR6, R3` at offset`0x3a0`, words
  `030673c4 00000000 00004000 000ea200`. nvdisasm evidence is in
  `evidence/kirby-redux7-disassembly.txt`. Extend the existing active-lane
  reduction to OR (zero identity, real per-lane bitwise OR), not a constant.
- Run6 also proves the later SIGABRT is a second kind whitelist rejection:
  AS1, GPU `0x8115e00000..0x81166fffff`, compressed`0xc`, fallback`3`, slot1.
  Published TU102 definitions identify this as compressed S8Z24 and uncompressed
  S8Z24, respectively. Admit fallback3, retaining commit checks. The GPU page
  kind is stored verbatim; its current consumer distinguishes pitch from
  blocklinear, not legacy Maxwell depth/compression names. Hashes unchanged.
- Build7 succeeds and passes271 assertions/11 focused cases
  (`evidence/kirby-tests7.log`). Read-only offline lowering of all four captured
  run6 compute programs now succeeds using the current header; helper source
  `evidence/kirby-lower-check.cpp` stays outside the repository. Lowering success
  alone is not evidence of correct GPU execution or visible output.
- Run7 (`evidence/kirby-air-riders-eglantine-txjn5_yw`) survives the second
  depth-kind rejection and reaches later programs. No nonblack frame among the
  captures. It stops at the50-second budget, exit-9, with hashes unchanged.
  Remaining explicit failures:
  - At13.932seconds, compute `0x11656cc600+0x14d0` requires HMMA matrix
    multiplication. The captured program contains288 HMMA instructions.
    Upstream's optional software expansion is not runtime-validated here.
    Never use its `EDEN_NX2_TENSOR=stub` mode.
  - Graphics `0x11b221ba00+0x10` reads `SR_WSCALEFACTOR_XY` using S2UR UR4
    (`000479c3 00000000 00001e00 000e7000`). nvdisasm confirms the identity;
    existing backend support is a fixed-value stub, so **do not just whitelist
    this special register**. Implement and verify its real state contract first.
  - At20.828seconds, guest execution reaches unmapped PC0. Its cause is not
    established. The tail-only launcher panic check missed the exception amid
    heavy HID logging; change it to scan all new bytes with marker overlap.
- Audit discovered upstream's rejected-matrix path substitutes an upscale blit.
  Remove that fallback in build8: rejected native compute now remains rejected,
  never replaced by a visually plausible pass. No content from that fallback
  is accepted as evidence of authentic rendering.
- Final build8 succeeds. Run8
  (`evidence/kirby-air-riders-eglantine-0nammjrh`) reproduces both later shader
  rejections with the synthetic-upscale fallback removed, then PC0 at20.829602s.
  The corrected launcher detects the exception and terminates the run; exit0
  is orderly shutdown **after failure**, not successful game execution. One
  all-black capture, no visible content; original ExeFS hashes unchanged.
- Focused regression `evidence/kirby-regression7.log`:329 assertions/14 cases
  pass, including external native-video fixture, gather, match, atomic OR,
  warp OR and mapping lifetime/kind checks. Launcher scan separately passes
  busy-log and split-marker checks. `git diff --check` passes.

## Next verified work

Implement the actual native W-scale state contract rather than exposing the
backend stub; validate tensor arithmetic (not HMMA no-ops or replacement blits)
before enabling it; inspect the independent guest PC0 failure. No retail,
interactive, offline-menu or gameplay compatibility claim is supported.

Important limitation: inherited upstream automatic service stubs remain active
and are logged (including VI2 and HID commands). Reaching these calls or a black
presentation is not evidence that those services are correctly implemented.

No visible rendering, offline menu or gameplay has been verified.

## Kirby10..14 — audio crash fixed; graphics blocker narrowed

- The fault9 debugger probe read a stale saved CPU context. Its main+0xe66688
  spin loop is not the faulting instruction. The synchronized emulator backtrace
  instead has SDK LR+0x1cd1f4 in `nn::audio::OpenAudioOut`: SDK+0x1cd1f0 calls
  an absent manager vtable entry after aud:u1 command1 was auto-stubbed.
- Implement aud:u1 command1 by returning the existing real `IAudioOutManager`
  service, preserving command2. Run10 (`evidence/kirby-air-riders-eglantine-pitvu7a9`)
  opens the real SDL audio stream at20.834333s and survives its45s budget without
  the previous PC-zero exception. This verifies stream creation, not audible fidelity.
- W-scale candidate: lower S2UR special registers30/31 through existing IR and
  obtain Vulkan constants from viewport clip-control XY/Z guardband modes.
  The published register fields specify factors256/1 and Z-same-as-XY; linking
  these to SR_WSCALEFACTOR is an inference supported by the captured shader's
  w-times-factor clip comparisons, not an independent hardware measurement.
  Include both modes in pipeline keys/runtime info; increment Vulkan cache19.
  Reject reserved Z mode3. GL backends explicitly reject instead of leaving
  previously empty emitter stubs. State/key handling is tested; final shader
  execution remains unverified because of the subsequent CSMTEST rejection.
- Captured `LDC.64.IS R6,c[0][R0+8]` at graphics+0x40 uses mode2. Preserve that
  mode into Maxwell's existing segmented address implementation (upper16 bits
  select buffer, lower16 select offset), retaining rejection of IL/ISL. Added
  encoding tests cover destination, source, byte offset, bank, width and mode.
- Build13 PASS. Offline, cgroup-protected run13:
  `evidence/kirby-air-riders-eglantine-qe9kl8tc/console.log`:
  at14.035951s compute0x11656cc600+0x14d0 still rejects HMMA;
  at14.036027s graphics0x11cac49500+0xf0 now rejects opcode0x80d:
  `CSMTEST.RA.F.AND PT,PT,0x4321111,PT`. Later graphics shaders reject the
  same opcode at+0x410 and+0x570. Audio opens at21.028472s. No guest PC-zero
  exception; result.json records45s budget termination, exit-9 and unchanged
  ExeFS hashes. Exit-9 reflects shutdown requiring kill, not successful exit.
- Both captured1280x720 frames have exactly zero nonblack RGB pixels:
  `evidence/kirby-frame-check13.json`. No visible Kirby rendering or gameplay.
- Regression13 with the existing local native-video fixture passes356 assertions
  in17 cases (`evidence/kirby-regression13-fixture.log`). Without that fixture,
  one case skips; do not report it as passed.
- Candidate14 names opcode0x80d as an explicit clip-state-machine failure and
  adds regression checks for both captured CSMTEST forms. Do not map this to
  the inherited constant-false FCSM_TR stub, discard the updates, or force EXIT.

Next blockers: actual primitive clip-state-machine semantics and HMMA numeric
validation. NVIDIA's public Binary Utilities names CSMTEST “Clip State Machine
Test and Update” but does not specify the state transitions needed here:
https://docs.nvidia.com/cuda/archive/10.1/cuda-binary-utilities/index.html .
No external implementation copied. HMMA software expansion remains disabled;
`EDEN_NX2_TENSOR=stub` is forbidden. Global Test Ride server/date restrictions
have not been reached or bypassed. Welcome Tour work is preserved.

Build14 PASS; focused regression14:360 assertions/18 cases PASS. `git diff --check` PASS. Binary and tracked/untracked source snapshot: `evidence/kirby-preserve14/`. No emulator remains running.

## Kirby15..23 — real tensor path exercised; compute addressing/local memory extended

- `--tensor-emulation` now explicitly opts the isolated launcher into real
  HMMA software arithmetic and records the setting in each run manifest.
  Ambient EDEN_NX2 variables are intentionally sanitized: run15 did NOT enable
  tensors, was stopped, and is not tensor-validation evidence. Runs16+ use the
  explicit option. No production profile or original save was changed.
- Removed the inherited `EDEN_NX2_TENSOR=stub` D=C substitution entirely.
  Only the exact `emulate` mode enables matrix lowering; other values reject.
  Default remains disabled because software expansion is expensive.
- New `[ampere-tensor]` test executes the actual emitted Maxwell words in a
  small wave interpreter and compares against independent matrix multiplication:
  12,288 exact integer outputs, zero/nonzero C, D aliasing A and D aliasing C.
  This checks fragment/lane mapping and arithmetic, not bit-exact hardware
  rounding for general inputs. Nvidia PTX permits at least-half accumulation
  for f16 C/D and leaves accumulation order unspecified:
  https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#warp-level-matrix-fragment-mma-16816-float
- Run16 (`evidence/kirby-air-riders-eglantine-i6splugv`) passes the initial
  HMMA rejection, then exposes LEA R58,R61,UR4,1 at later compute+0x3ca0
  (69.680758s). nvdisasm SM86 confirms the opcode. Add the bounded uniform
  addend form using existing ISCADD; include it in uniform-bank reservations.
- Later captured compute programs expose ULEA carry into UP0, STL/LDL.128
  and indexed LDL. Add real scaled-add carry tracking with the existing
  uniform-predicate namespace, preserving guest GPR predicates and aliases.
  Lower private local vectors into individual word accesses with actual
  register/address/offset values; RZ stores zero all words. Keep size,
  alignment, cache-mode and scratch-register checks. Indexed scalar LDL
  reads its address before overwriting an aliased destination.
- Shader-only check23 against run17's28 captured programs:
  `evidence/kirby-lowering23.log`:23 accepted,5 rejected; every remaining
  rejection is graphics CSMTEST. This does not alone verify GPU output.
- Build23 PASS. Regression23 passes26 focused cases, including the local video
  fixture and12,288 tensor outputs. The1,376,728 raw assertions include repeated
  interpreter invariants; do not present that as independent scenario coverage.
  Logs: `evidence/kirby-build23.log`, `evidence/kirby-regression23.log`.

CSM investigation: instruction encoding and Nvidia's own batch-clipping
publication establish shared batch state and guardband factors1/256, rather
than a standalone per-vertex boolean. The current Vulkan vertex path has no
verified mapping of guest clip batches to host invocation groups. Implementing
ordinary subgroup-all or substituting FCSM_TR=false is not verified emulation.
Reference: https://patents.google.com/patent/US8976195B1/en .
The publication is architectural evidence, not a complete Ampere instruction
specification. No external implementation code was imported. Encoding reference
`evidence/kirby-csm-encoding16.txt` is outside the repository; it is not runtime
semantics or imported production code.

The graphics blocker remains genuine and unresolved. No clip-state shortcut,
forced job completion or replacement draw was introduced. Welcome Tour changes
remain preserved. Runtime23 result and final preservation follow below.

## Kirby24..25 — constant clip case and the next geometry-stage boundary

- Add native FSETP with a uniform operand, preserving the existing comparison
  and predicate-combination semantics. The newly reached small vertex shader
  compares zero against1 and positive W-scale factors for all seven clip
  predicates. Every invocation has the same known-inside result.
- ConstantAcceptedClipPrefix recognizes only that unconditional prefix and
  folds the exact reset/accumulate plus FCSM_TR-query pair. A batch of all-inside
  vertices cannot be trivially rejected, independently of host subgroup layout.
  This is a narrowly proven constant fold, NOT general CSMTEST support or use
  of the inherited constant-false flow-test stub. Unknown inputs, missing
  predicates, negative/NaN factors and other CSM modes still reject; tests cover
  those counterexamples. All five dynamic clip shaders remain rejected.
- Run24 (`evidence/kirby-air-riders-eglantine-48ywpx79`) verifies the formerly
  rejected constant shader lowers at71.238508s, then reaches native graphics
  stage4 (geometry), which was categorically unsupported. Its actual SPH first
  words are100c1081,00000540,01000000,07000000,00000008: geometry type4,
  1344B local storage, one invocation, triangle-strip output, eight vertices,
  stream0. These match the existing ProgramHeader field interpretation.
- Candidate25 routes stream0 native geometry headers into the existing
  geometry-stage translator, preserving the real stage and header. This does
  not establish native geometry compatibility. Run25 reaches a precise opcode
  rejection at geometry+0 (`LEPC R0`, opcode0x34e), rather than bypassing it.
  The dumped program then configures `BMOV.64 ATEXIT_PC` and uses
  `SR_INVOCATION_INFO`/`ISBERD`; their native address, lifecycle and primitive
  access contracts remain unimplemented. Never treat them as no-ops.
- A copied test-cache option (`--cache-source`) avoids needless driver recompiles
  while preserving source caches and creating a separate profile. The source
  path is recorded in manifests. Run25 uses a copy of run24's cache.
- Runtime25 evidence: `evidence/kirby-air-riders-eglantine-7djetqic/console.log`.
  Dynamic CSMTEST rejects at14.127696s; geometry LEPC rejects at35.847893s.
  No new compute-shader rejection or PC-zero exception was observed. This is
  not a numeric verification of all GPU compute outputs.
- Run24 stops at120s; run25 at100s. Both require kill after the test budget
  (exit-9), and both report unchanged ExeFS. Eight run24 captures and twenty
  run25 captures are all black: `evidence/kirby-frame-check24.json` and
  `evidence/kirby-frame-check25.json`. No game-rendered frame or3D claimed.
- Build25 and27 focused cases PASS, including12,288 exact tensor outputs and
  the local captured video fixture: `evidence/kirby-regression25.log`.

Current blockers: dynamic batch CSMTEST semantics/guest-to-host primitive
mapping, plus native geometry address/ATEXIT/invocation/index-buffer operations.
These are substantive missing shader execution paths. No further identical run
is useful until one of those paths has an evidence-backed implementation.

Final source/binary/launcher snapshot: `evidence/kirby-preserve25/`. No commits or pushes.

## Builds26..39 — geometry, texture sampling and a genuine title screen

- Read-only geometry mapping dump26 proves the first EXIT was followed by
  `OUT.FINAL`, handler clear and EXIT. Capture now retains this handler. A
  narrowly validated PIC prologue materializes the real64-bit code address;
  exits branch through the recovered handler. Dynamic/unknown registrations or
  incomplete handlers reject. Stream0 geometry output uses real EmitVertex and
  EndPrimitive IR; finalization is not suppressed.
- Limit native geometry to stream0, one invocation. SR_INVOCATION_INFO uses the
  existing topology-count IR. Only a proven invocation-zero input entry is
  normalized to logical vertex0; unknown ISBERD addresses reject. A whitelist
  makes the proof conservative about control flow, aliases and vector writes.
- Add actual uniform-addressed local accesses, U16 stores and indexed vector
  loads that preserve an aliased address before loading. U64 high-word funnel
  shift uses the register-shift form: the Maxwell immediate decoder includes
  width bits in its immediate, so reusing it here would compute the wrong shift.
  Both captured geometry programs lower with complete exit handlers (0x8660
  and0x7aa0 source bytes); run33 reaches their fragment shaders.
- Add FP32 bindless2D TEX.SCR with implicit/zero LOD, real uniform handles,
  component masks and split destination pairs. Coordinates are preserved before
  aliasing outputs. No substitute texture or fixed sample value is introduced.
- Position-only copy shader first passed via a conservative clipping translation:
  preserve all vertex/attribute exports and defer redundant frustum rejection
  to the real host clipper. Build39 extends the proof to three read-only
  matrix-transformed vertex shaders. It verifies all seven comparisons against
  the actual exported positions, positive W-scale definitions and unchanged
  position values; query predicates have no remaining consumers. Unsupported
  control flow, storage writes or changed positions still reject.
- Every draw rechecks the proof's pipeline requirements, including cached
  shader reuse: full frustum clipping, no VertexA/tessellation/geometry stage and
  no transform feedback. A private converted-header marker persists this
  requirement through cache serialization; original guest headers are never
  modified. Vulkan cache version is20. Unit tests cover incompatible pipeline
  transitions and proof-breaking mutations. This is equivalent conservative
  execution for the accepted class, not general native CSMTEST emulation.
- Run37 (`evidence/kirby-air-riders-eglantine-uie3d0nr`) produces changing
  streak-like output;23 captures are nonblack with21 unique RGB hashes.
  Working snapshot: `evidence/kirby-first-frame37/`. A second run
  (`evidence/kirby-air-riders-eglantine-fs69yt4y`) with the exact same binary
  reproduces16 nonblack captures; reproduction manifest is in that snapshot.
- **Run39 renders the genuine Kirby Air Riders: Global Test Ride title screen.**
  `evidence/kirby-air-riders-eglantine-xlcyz0i2/frames/nx2-frame-00196.tga`
  is the preserved screenshot source, losslessly exported to
  `evidence/kirby-title39/title-screen.png`.40 captured frames,27 nonblack.
  Binary SHA256:872f68133676fee11a889431d44c299af2583bb9c6bc2646049f9053a02326bd.
  Result:100s offline budget, shutdown required kill (exit-9), ExeFS unchanged.
  Build39 and41 focused cases PASS (`evidence/kirby-regression39.log`), including
  the original video fixture, tensor arithmetic and new geometry/clip tests.

Remaining: the complex branching vertex shader still rejects CSMTEST at+0x4fa0.
Another shader exposes texture opcode0x361 at+0xa40. The title is partially
sized/positioned within the output frame. No world, input response, performance
or complete rendering-correctness claim. The eShop UI is visible but was not
activated; tests stay offline and do not bypass trial/server restrictions.

Owner launch (copied profile/cache, no manual timeout):

```sh
cd ~/Nebby/eden-switch2-experimental
systemd-run --user --scope --quiet -p MemoryMax=10G -p MemorySwapMax=4G \
  env SDL_VIDEODRIVER=x11 python3 launch-eglantine-mkw.py \
  --title kirby-air-riders --offline --tensor-emulation \
  --cache-source evidence/kirby-air-riders-eglantine-xlcyz0i2/profile/cache \
  --max-memory-mib 10000 --capture-frames --scanout-diag
```

## Title screen, input, network flow and the launch-table stall

Verified in `evidence/kirby-air-riders-eglantine-eib3gwvh`,
`...-omfi6kfr`, `...-rk2e1z8a` and the earlier `evidence/kirby-net*.png`
captures. All runs are offline (network namespace, no route), copy the profile
per run, and report `exefs_unchanged: true`.

- **Title text renders.** `TEX.SCR` (`0xf60`) keeps its original destination
  order (components0/1 to bits16..23, 2/3 to bits64..71). "nvdisasm prints the
  primary component first" was a wrong reading: swapping it produced a magenta
  title (mean94.59) against97.48 for the original mapping. The later `.B` form
  (`0x361`, `d, u, v, handle` with consecutive RGBA writes) is implemented and
  makes the "PRESS ANY BUTTON" prompt readable instead of speckle.
  `[ampere-sample]` and `[ampere-gather]` cover both bindless forms.
- **Input reaches the guest.** `xdotool` XTEST keys arrive as
  `Keyboard::PressKey(code=4)`, then `EmulatedController::SetButton`
  (`apply A npad_id=0 value=true raw=0x1`) and the npad `dev_btn` word. Short
  taps are missed by the UI's sampling: hold the button for >=3 s.
- **nifm `IRequest` follows console semantics.** Submit moves the request to
  OnHold, `Cancel` terminates it and signals the completion event, and a request
  that ends without a connection reports `ResultNetworkCommunicationDisabled`
  through a separate `request_failed` flag (a failed request and
  `NotSubmitted` share enum value 1). Before this the applet waited on an event
  that never fired.
- **The app is alive.** Frames after the dialog run at 59 fps and keep
  presenting (present #5400, fence (1,10835) reached) with no stall.

### QMD launch table must recycle finished entries

`Tegra::NativeQmdScheduler` keeps one dependency table for every channel and
refused any new QMD address past 4096 entries ("QMD cache limit"), returning
before the QMD's releases ran. Titles bump-allocate a QMD per compute launch
(~57/s here), so the table filled after about70 s of gameplay. The releases are
what other queues wait on, so the dropped release parked a queue:

```
[77.292471] HW.GPU <Warning> puller.cpp:81:CanResumeAcquire: Channel 1 waits on
  semaphore 0x1163431000 (op 4, 0x1378, now 0xffe) for 2 s
[79.314321] Service <Warning> nx2_gsv.h:955:WaitForFrame: nx2:vi2 present #708
  fence (1, 1419) not reached in 4 s (now 1417)
```

The guest then retried every present with a fence the GPU could no longer
reach, freezing on the last drawn frame (the network dialog). `Reserve` now
recycles completed entries with no deferred dependency count (insertion order,
bounded by the same 4096), so a launch always performs its releases; an address
that comes back simply starts unloaded, which leaves later decrements deferred
exactly as before. `[native-qmd]` proves both properties: 2x the table bound of
launches still releases every semaphore, and a launch with outstanding
dependencies is never recycled. Test count: 32776 assertions in 2 cases.

This is a resource-limit fix, not compute emulation: the unsupported compute
programs are still skipped and still log. Their outputs stay missing.

### Offline network path

The Global Test Ride treats "no network" as a hard failure: the title leads to
"Connecting to network...", then "Unable to connect to the network." with an OK
button, and each OK press retries (`nifm GetAppletInfo` repeated) and fails
again. `nifm` reports `ResultNetworkCommunicationDisabled` because the offline
namespace has no non-loopback IPv4 address. `--dummy-network` (new launcher
option) brings up a routeless dummy interface inside the namespace so the guest
sees an up link with an address while nothing is reachable; this distinguishes
"the build needs any link" from "the build needs Nintendo reachability". No
external traffic is possible in either case.
