# Eden on Switch 2 titles (branch `nx2`)

Local experiment: get Eden (Switch 1 emulator) to load a Switch 2 title's loose ExeFS and push
boot as far as it goes. Nothing here is upstreamed or pushed. Graphics (Ampere / NVN2) is out of
scope for this pass.

## Setup

- Source + build live in `C:\dev\eden` (a worktree of `eden-nx2/`, branch `nx2`). The original
  working folder path is ~180 chars, and `cl.exe` can't open its PDB from a build folder there
  (C1041), so the build moved to a short path.
- Toolchain: VS 2026 (MSVC 14.51) via `vcvars64.bat`, Ninja from VS, CMake 4.4.
- Configure:
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT -DENABLE_QT=OFF -DYUZU_CMD=ON -DENABLE_WEB_SERVICE=OFF -DYUZU_ROOM=OFF -DYUZU_TESTS=OFF -DENABLE_WERROR=OFF -DENABLE_UPDATE_CHECKER=OFF`
- Build: `cmake --build build --target yuzu-cmd` -> `build/bin/eden-cli.exe`.
- Run (portable mode, `build/bin/user/` holds config/logs; no keys or firmware installed):
  `eden-cli.exe -g "<title>\exefs\main"`
- Debug helpers:
  - `EDEN_NX2_SVC_TRACE=1` logs every SVC with arguments and results.
  - `EDEN_NX2_IPC_TRACE=1` logs every dispatched CMIF command and non-zero result words.
  - `EDEN_NX2_AUTO_STUB=1` answers every unknown command with an empty success (the
    `use_auto_stub` setting gets reset by eden-cli's config rewrite). Used for all runs below.
  - `eden-cli --filter "<log filter>"` sets the log filter for one run (e.g. `*:Info Loader:Debug`
    to get module load addresses).
  - `tools/nx2/nso_syms.py <nso> addr|got|find ...` maps module offsets to exported symbols, GOT
    slots to imports, or searches symbol names. Eden's backtrace shows "unknown" modules for some
    titles; with the `loaded module X @ addr` lines this turns frames into SDK names.
  - `tools/nx2/a64_coverage.py <a64.inc> <nso>...` lists encodings in an NSO's .text that
    dynarmic's decoder table doesn't match.
  - `tools/nx2/npdm_info.py <main.npdm>` dumps the META/ACI0 header, services and kernel caps.
  - `tools/nx2/nso_dis.py <llvm bin> <nso> <text_off> <count> <workdir>` disassembles part of an
    NSO's .text (clang + llvm-objdump from VS 2022's LLVM).
  - Under cdb, stop only on second-chance AVs (`sxd -c2 "..." av`): dynarmic fastmem uses
    first-chance access violations.

## Titles

All titles share the same `main.npdm` layout: 64-bit, 39-bit address space, ThreadInfo cores
0..5 (core mask 0x3f), priorities 28..59, KernelVersion cap 9.1, SVCs 0x01-0x36 plus 0x80,
Switch 2 ACID layout.

All four are **Switch 2 builds**: none is a plain Switch 1 sanity check. Instruction counts that
stock dynarmic did not decode (`a64_coverage.py` against the unpatched `a64.inc`):

| Title (ExeFS) | Program id | RETAA/B | LDAPR | LSE atomics | CAS | SWP | other |
|---|---|---|---|---|---|---|---|
| first `exefs` | 0400C3F00006E000 | 114,767 | 37,560 | 24,718 | 6,640 | 2,470 | CASP 6, XPAC 7, PAC 2, STG 1 |
| second `exefs` and `updates/exefs` | 0400A3A01EE4C000 | 12 (rtld only) | - | - | - | - | main/sdk/subsdk0 don't decompress |
| third `exefs` (update `updates/65536` ~same) | 04005B902517C000 | 134,287 | 37,052 | 160,741 | 19,987 | 1,623 | XPAC 3, PAC 2, STG 1 |
| fourth `exefs` (update `updates/65536` ~same) | 0400380020056000 | 134,911 | 100,520 | 32,110 | 8,667 | 3,733 | PACGA 58, PAC 7, CASP 6, XPAC 3, STG 1 |

With the patched decoder, games 1/3/4 only leave ~60-110 rare SIMD encodings in `sdk` undecoded
(e.g. `5f119f9c`, `4ea05c42`, `0f413044`, FP16/v8.2 forms).

## Blockers hit, in order

| # | Where it stopped | Cause | Fix (commit) |
|---|---|---|---|
| 1 | CMake try-compile | C1041, PDB path ~290 chars | build in `C:\dev\eden` |
| 2 | Compile | MSVC 14.51: `reinterpret_cast<u64>(ULONG_PTR)`, C4505 on amalgamated zstd.c, C4800 int->bool | Fix build with MSVC 14.51 |
| 3 | Decoder | dynarmic had no LSE atomics, CAS/CASP, LDAPR, RETAA/RETAB, BRA*/BLRA*, LDRA*, PAC*/AUT*/XPAC*, PACGA, MTE ops. Title 1 has ~115k RETAB, ~37k LDAPR, ~25k LSE, ~6.6k CAS, ~2.5k SWP | dynarmic: decode ARMv8.1-8.5 instructions (atomics are exclusive load/op/store with retry; PAC is identity; MTE tags are 0) |
| 4 | Loader | `main` identified as a bare NSO, so main.npdm/rtld/sdk were skipped, then a crash writing into an uninitialised process | loader: load a loose ExeFS when given its main |
| 5 | NPDM parse (ErrorBadFileAccessControl) | Switch 2 ACID has a 0x300-byte signature/key area (Switch 1: 0x200); FAC/FAH offsets were also read without the block base | npdm: parse the Switch 2 ACID layout |
| 6 | eden-cli crash on start/exit | `*appstate` never set, SDL3 callbacks returned SUCCESS (quit), InputSubsystem was a local of SDL_AppInit, mouse timer before mouse driver | eden-cli: fix SDL3 main-callback lifetime bugs |
| 7 | `nnosInitialize` abort | `svcGetInfo` 29/30 unknown (region between alias and ASLR) | kernel: stub ids 29/30 (empty region after alias) |
| 8 | `VammManager::InitializeIfEnabled` abort | `svcGetInfo` 33 unknown; it's a page granule (module ranges are checked against `value-1`; VAMM maps one granule first) | return 0x1000 |
| 9 | `nn::am::InitializeApplicationInterfaces` | `appletOE` cmd 10 (Switch 2 OpenApplicationProxy) | alias to cmd 0 |
| 10 | same | `IApplicationProxy` cmds 60 and 40: new getters | return a generic `nx2:` stub interface (`nx2_stub_interface.h`); calls on it are auto-stubbed and logged |
| 11 | `nn::oe::SetupGpuErrorHandler` -> CreateThread | thread on virtual core 5; Eden emulates 4 cores | fold virtual cores 4/5 onto 0/1 in CreateThread/SetThreadCoreMask |
| 12 | (title 3) `nn::oe` init | `IApplicationFunctions` 320 has no handler | empty-success stub |
| 13 | (titles 3/4) `nn::fs::QueryMountRomCacheSize` -> `GetFileSystemProxyServiceObject` abort 2002-1002 | fsp-srv `SetCurrentProcess` returned ResultTargetNotFound: the loose-ExeFS loader never registered the process with the fs controller, and never set its RomFS | loader: give loose ExeFS boots a RomFS (sibling `romfs/`) and an fs registration |
| 14 | (title 4) `IApplicationFunctions` 321 | unknown | empty-success stub |
| 15 | (title 4) `nn::oe::GetCurrentGraphicsScale` UnexpectedDefault | `IApplicationProxy` 60 is a graphics-scale interface; its cmd 10 must return 10/20/30 | `IGraphicsScaleController` returning 10 |
| 16 | (title 4) `nn::gs::sdk::BootGraphicsSystem` -> `nn::os::GetRemoteMemoryUsage` abort | `svcGetInfo` 31/32 (remote memory for gsv/nvsrv) | return 0 for both (logged) |
| 17 | (title 4) `nn::gsv::BootWithRemoteMemory` abort | it sends TIPC on a session handle from `ISelfController` 2000, which had no handler | `ISelfController` 2000 returns a raw session to `INx2GraphicsSystemSession` (`nx2_gsv.h`), which accepts every command |
| 18 | (title 3) `nn::oe::GetPseudoDeviceId` abort | needs control.nacp, which loose ExeFS lacks | seed from program id |
| 19 | (title 4) `nn::syncpt::sdk::InitializeEx` | gsv 202 must return the syncpt session; gsv 204 takes two transfer memories and must leave a header (version 1, count, offsets) in the read-only one and return two events; syncpt 1 returns a u32 < 0x400 | `INx2SyncptSession`, gsv 202/204 |
| 20 | (title 4) `nn::gpu::sdk::BootWithRemoteMemory` | gsv 497 returns the GPU session; gpu 11 (id range), 12(id) event, 21(id) start + wait on that event, 22 another session | `INx2GpuSession` |
| 21 | (title 4) `nn::vi2::sdk::Initialize` / `nn::vi::CreateLayer` | gsv 305 returns the vi2 session; vi2 1/3/5 return small u32s (5 = timestamp recorder count, 7 needed by LayerHistory), 11/31/32 return handles | `INx2Vi2Session` |
| 22 | all TIPC replies with data | Eden bug: `ResponseBuilder::Push(Result)` writes the 2-word CMIF result, so TIPC data lands one word late and past the copied size | `TipcReplyWords` |
| 23 | (title 4) NVN -> NvRm device open | nvsrv is the nvdrv INvDrvServices protocol; serve it with Eden's nvdrv, initialised without `Initialize` | nvdrv-backed `nx2:nvsrv` |
| 24 | NvRm chip check | Switch 2 NvRm asks for a 0xD0-byte characteristics block and accepts chips 0x17E/0x12B/0x12E, but NVN needs the Ampere one; s16 interface versions at 0x70..0x80 must be >= 6 etc. | report chip 0x17E, Ampere classes, versions 0x40; unknown ctrl-gpu ioctls zero-stubbed for such callers |
| 25 | NvRm address space | AllocAsEx(flags 5, no big pages, [0x8000000, 0xC000000000), split 0); NVN then reserves 4 KiB VA up to 512 GiB; Eden's split/limit handling made the reported regions unusable | small pages up to the end, absolute big-page limit |
| 26 | (title 4) NvRm channel setup | nvhost-gpu ioctls 0x1F/0x20, `/dev/nvhost-tsg-gpu`, GetEngineInfo, object class 0 | logged echo for unknown ioctls once an nx2 driver is seen; GR + async-copy engines; Ampere/class-0 objects on Maxwell slots (70f1651, 82dbd88) |
| 27 | GPU memory | svc 0x80 (make memory GPU-accessible), gpu sub-session 101 (register range) and 211 (job) | stubs (608f0d6) |
| 28 | `syncpt::sdk::InitializeEx` -> `InterProcessEvent::Signal` abort E401 | gsv 204's first handle is attached *writable* (client -> server request), the second readable; the shared page has u64 values (RO+0xc), armed bits (RW+0x18, RO+0x1c) and summary words | gsv 204 returns a writable event, lays out the page, a 500 us poller mirrors host1x values and acknowledges armed waits (88367ea) |
| 29 | `NvOsEventPoolWait` E401 | syncpt 22(slot) must return an event; 201(slot, ~syncpoint, value, mode) / 202(slot) request/cancel a signal | slot events via host1x guest actions (88367ea) |
| 30 | GPU never executed submissions | Switch 2 never maps through nvhost-as-gpu: memory registered with gpu sub 101 is used at GPU VA == CPU VA | `Nx2GpuMemoryRegistry` identity-maps every range into each GMMU; SMMU physical range 33 -> 34 bits for 10/12 GB layouts (88367ea) |
| 31 | Puller | binds Ampere classes C797/C7C0/C7B5 and class 0; NVN2 uploads Turing+ (MME64) macros, which Eden's Fermi macro JIT runs as garbage until it crashes | Ampere classes run on the Maxwell engines; macros skipped while `Tegra::g_nx2_ampere` (88367ea) |
| 32 | NVN device init waits on syncpoint 1 >= 1 | submits use `increment_value` (bit 8) and signal completion only with semaphore releases | Eden appends the syncpoint increments for such submits (88367ea) |
| 33 | `wsi::ounce::NativeWindow::Acquire` on a null window | gsv 1510 is `gsv::shim::CalculateRequirements`; size 0 made window init fail | block-linear size/alignment, 4 bytes/pixel (5fb37b5) |
| 34 | `NvOsEventPoolWait` E401 (second channel) | nvhost-gpu QueryEvent 7 | per-channel event (5fb37b5) |
| 35 | CPU spins in NVN for `[0x2106028010] >= 3` | queues release progress to page P+0x1000 and wait (GPU acquire + CPU poll) on the same offset in page P | puller mirrors releases into the first two-page gpu region (5fb37b5; guess, works) |
| 36 | font header check on a wild pointer | `pl:u` 2010 `GetBuiltInFontData` (bool at offset 0, then {flags, u32 offset, u32 size}) and 2000 (font shared memory + size) | plain TTF copies of Eden's shared fonts after the BFTTF data (5fb37b5) |
| 37 | null "SoundResource" heap (300 MB) | `nninitStartup` sizes the heap as total - used - 0x195000000; Eden capped total at the 8 GiB heap region, leaving 1.55 GiB | Switch 2 processes count the alias region in max memory (5fb37b5) |
| 38 | game waits for service `aud:u` | Switch 2 audio entry (`nn::audio::CreateUserClientSessionByHipc`); cmd 2 returns an IAudioRendererManager | `INx2AudioUserClient` backed by Eden's audren (2e6c39c) |
| 39 | every audio `RequestUpdate` fails | updates tagged `REv?` (lowercase v), renderer opened with `REV?` | compare revision numbers (2e6c39c) |
| 40 | main thread parked in `ExecuteVirtualMachineMapBufferEx` | gpu sub 211's job slot is request word 17, not word 2 | signal word 17's event (2e6c39c) |
| 41 | "too many files open" reading RomFS | eden-cli never raised the CRT stdio limit | `_setmaxstdio(8192)` (2e6c39c) |
| 42 | GPU memory model | gpu sub 101 = `CreateGpuAccessibleMemory` (reply word 3 = GpuMemoryHandle); 211 = `NormalMapGpuMemory4K` (words 3-4 first VA, 5-6 last, 7 handle, 8-9 offset, 17 slot), also into sparse regions at 0x81_xxxx_xxxx; 102/213 (`NormalUnmapGpuMemory4K`)/214 (`CreateSparseRegion`) complete through slot events (words 3/8/11) | real handles, `Nx2GpuMemoryRegistry::MapAt` with SMMU range reuse, slot signals |
| 43 | (title 1) `nn::syncpt::sdk::Initialize` abort | newer SDK opens syncpt with TIPC 0(u64 token) and aborts unless it is echoed | handshake echo |
| 44 | (title 1) NVN closes the GPU device after GetVARegions | newer SDK asks gpu 201/202(id) for the user/gsv partitions (two u64s each, stored as min/max) instead of using 203 | 201 = [0x8000000, 0xC000000000), 202 empty |
| 45 | NVN command buffer with no command memory (~12 s, titles 1 and 4) | `NvRmGpuTSGSubcontextCreate` failed (0x11): characteristics reported one VEID | flags bit 8/22 (TSG, TSG_SUBCONTEXTS) and 64 subcontexts at characteristics +0xb4 (12125bc) |
| 46 | NVN asserts in `GetSyncpointShimRoAddress` | Switch 2 reads syncpoints through a read-only GPU page (flag bit 26, nvhost-as ioctl 0xB) | shim at the top of the VA range; GPU reads of it return the syncpoint value (12125bc, from changes.patch) |
| 47 | window/image creation | gsv 111/112/114/423/429/430/435/1510 native memory and images; svc 0x80 is SetGpuMemoryAttribute | native object registry + layout calc (from changes.patch), real svc 0x80 (12125bc) |
| 48 | presents go nowhere | vi2 229 (word 5 image id, ~word 22/word 24 fence) | composite the block-linear image with native GOB tiling (f63ff90, from changes.patch) |
| 49 | crash in `DmaPusher::CallMethod` (null engine) | NVN uses subchannel 2 (inline-to-memory) without binding a class | bind KeplerMemory there by default; drop methods on unbound subchannels (9f30683) |
| 50 | GPU parses garbage, then stalls (~14-19 s) | NVN2 gates each command buffer with a SEMAPHORED acquire the CPU releases after writing it; Eden read past unmet acquires and ran stale commands (`EDEN_NX2_PB_CHECK` confirmed entries changed after submit) | suspend the channel on an unmet acquire and resume it from the same word (9f30683, approach from changes.patch) |
| 51 | desync after a called command buffer | NVN2 ends a GPFIFO entry inside a method's data, calls another buffer, and continues at the next address | set the unfinished method aside and resume it when the stream continues (9f30683) |
| 52 | semaphore words next to releases get clobbered | 4-byte releases (SEMAPHORED bit 24) were written as 16 bytes | 4-byte releases write 4 bytes (9f30683) |
| 53 | no draws: 3D macros skipped | NVN2 drives 3D/compute state and draws through Turing MME64 macros; compute dispatches through QMD 3.0 | changes.patch's MME interpreter (one macro context shared by the process's channels, also on the compute class), native compute and QMD scheduler (d7826ce) |
| 54 | draws hit Eden's Maxwell decoder ("Invalid insn") | shaders are SM86 SASS | changes.patch's Ampere-to-Maxwell lowering, widened until all 32 of title 4's programs lower; `EDEN_NX2_SHADER_DUMP` + `C:\dev\nx2work\lowertest` test it offline (d7826ce) |
| 55 | GPU desyncs again after ~15-30 s | the game ran a full ring of command memory ahead of Eden's GPU thread: the vi2 229 reply reports all buffers free, so nothing paced it | vi2 229 waits (bounded) for the frame's GPU fence (325cac7) |
| 56 | macro arguments lost | NVN interleaves several command buffers, so more than one split method can be pending | several parked methods keyed by resume address, dropped at the end of the submission (325cac7) |
| 57 | frames all black although draws run | gpu 214 creates GPU-only memory (six 300 MB regions at 0x81_0000_0000) that the graphics server backs itself; the render targets live there and were unmapped | back each region with application-pool pages mapped into the alias code region (325cac7). Title 4 now shows its loading screen and a menu screen |
| 58 | (title 4) hang after confirming difficulty: macro 0x142 "parameter FIFO underflow" | NVN2's conditional-render macro reads two query counters with MME_DMA_READ_FIFOED (3D methods 0x154/0x155 address, 0x158 count) and loads them after a load barrier | MME DMA reads into the load FIFO / data RAM (8cbe854) |
| 59 | equal-value gate semaphore missed (~24 s) | the CPU side ran ~96 command buffers ahead when the 250 ms present wait timed out during shader compiles | present waits up to 4 s for the frame fence (8cbe854) |
| 60 | 3D queue waits forever on a word the compute queue should release | NVN2 patches a third QMD release slot at word 52 for queue-to-queue syncs | fire release slots 24, 28 and 52 (8cbe854) |
| 61 | 3D queue waits for N compute completions, 4 missing | compute jobs whose shader Eden rejected never completed, so their +1 releases never fired | rejected QMDs still complete and release (a84ec80); IMAD -UR addend, LDSM lowered |
| 62 | queue waits for ≥ a value that keeps moving backwards | QMD release slot 0 (flag bit 19) is a max release into a word a 3D macro also advances | bit-19 QMD releases write max(old, payload) (a84ec80) |
| 63 | in-game draws refused (~20k per run), black screen behind the dialogue | strong LDG (bit 77), 2D TLD4, RED MAX, AST of RZ unsupported | lowered (a84ec80); 210/214 post-menu shaders lower |
| 64 | intro scene black (DLSS-style pass) | title 4 renders at 2560x1440 and upscales to 3840x2160 with a tensor-core (HMMA/MOVM/LDSM) compute network; emulating it takes the host driver minutes to compile and would cost seconds per frame | bilinear blit from its input (cbuf0+0x90) to its output (cbuf0+0xa0) when the program is rejected (22333bd); full emulation opt-in (EDEN_NX2_TENSOR) |
| 65 | fullscreen blit passes read constant bank 24 | console-provided blit parameter block, never bound by the game | bank 24 -> 17, synthesized from the render area by BlitConstantsPass in vertex and fragment shaders (22333bd, d75e57d) |
| 66 | synthesized blit sizes were 0 | the game never programs surface_clip, which feeds Eden's render area | fall back to colour target 0's size (d75e57d) |
| 67 | tonemap output black | the 3D colour-grading LUT is drawn once per scene by a pixel shader using CALL.REL.NOINC; capture treated it as a real call, never found the end and rejected the shader | NOINC calls count as forward branches in capture (4d58a74). **Intro cutscene renders correctly** |
| 68 | crash ~5 s after the save that follows character customization (garbage pointers in two threads) | nx2:gpu 213 (unmap) and 102 (destroy GPU memory) were no-ops; the game reuses the freed memory as heap and stale GPU write-backs overwrote it | unmap from every address space, drop freed semaphore regions (bed817c); not yet confirmed past customization: frame-locked replays desync across loading screens |
| 69 | hatched edges on UI portraits, buttons, squid cursor | UI textures are ASTC; the GPU ASTC decoder ignored the Switch 2 GOB swizzle | apply it in astc_decoder.comp (7fa2b46) |
| 70 | freeze ~15 s after the post-customization save; channels 2/4 wait on 0x2115c98000 (equal 0x5293, now 0x5310) | a 3D queue ticks a counter two queues acquire value by value; Eden wrote several ticks between their re-checks | releases latch the waiters they meet (8e37f58) |
| 71 | audio broken, game at 90-160 fps | vi2 presents were never paced | 60 Hz vblank pacing |
| 72 | stuck loading the next scene ("too many files open" for route_wallClimb.pack.zs, save.dat) | VFS open-file cache cap equal to the CRT limit (8192) | cap 512 below _getmaxstdio() |
| 73 | loading screen hangs, host GPU TDR | macro parameters split across GP entries were parked; method data continues across entries | continuous split methods (e8d060b) |
| 74 | rejected ATOMG/STL/LDL with small immediate offsets | lowering only took a zero offset | s20 ATOM offset, 24-bit STL/LDL offset (6302be8) |
| 75 | same crash 3 runs (415 s, 429 s, 9960 s): read 0x29 at pc 0x85096d10 (recursive mutex lock), lr 0x82ada310 | worker job runs main+0x2aba2b0 on object C = [A+0x18] whose manager [C+0x720] is null; C's destructor (main+0x2ab8e20, vtable main+0x4b660f8) nulls it before A's teardown (main+0x2b37b50) clears A+0x18; teardown and per-frame update are both jobs (via 0x8365c634); NFC Finalize comes 14-22 s before each crash; LSE atomics (exclusive pairs) and DMB (mfence) checked fine | open: catch all threads at the abort with the gdbstub (`nx2work/dbg_crash_threads.rsp`) |
| 76 | debugger run, crash at 406 s in the same scene load: writes through a null result at pc 0x82b471fc (main+0x2b271fc) | the scene's frame heap "PaintSceneProcHeap.26" (fixed 90 MiB + 0xfc8 parent, head 0x12c9525d18, tail 0x12c9800208) had 2.98 MB left for a 0x302088 allocation (A init main+0x2b37de0 -> main+0x2b27020); about 160 KB short, so some allocation of that scene is bigger than on the console (only 11.7 MB of the heap is nx2:gpu memory); the main thread was waiting in svc 0x34 for job workers | open: log that heap's allocations to find the oversized one |
| 77 | why the scene heap overflowed (run 6) | the heap size is a constant (mov w1, #0x5a00000 at main+0x2b32070); its fixed contents are 71.3 MiB (main+0x2ab9218) + 1.05 MiB + 1.25 MiB + 3 MiB; in runs 3, 4, 5 and the save test the scene's two 0x514000 GPU pools come from the root heap (next to the 0x2ee000 pool), in run 6 from the scene heap (current heap of the loading thread), 10 MiB it has no room for; which heap a job allocates from depends on the thread running it. The game's npdm asks for cores 0..5 (mask 0x3f, reported as is by GetInfo CoreMask) and Eden folds 4, 5 onto 0, 1, so two cores run two workers each | candidate: report or emulate the real core layout; ruled out: memory layout (12 GB), controllers (one), GPU pool alignment |
| 78 | six emulated cores (4fbfd5b), run 7 with the gdbstub: same scene-heap overflow at 292 s, the 0x514000 pools again in the scene heap | both overflow runs (6, 7) had the gdbstub on (memory accesses checked, slower JIT); the three runs without it (3, 4, 5) put those pools in the root heap and crashed later on the torn-down object instead, so the debugger itself shifts this timing | open: a six-core run without the gdbstub |
| 79 | run 8 (six cores, no gdbstub): original crash at 338.6 s, 11 s after the paint scene set up (327 s, pools in the root heap) | C's init (main+0x2ab9120) creates "ColPaintBuild.%u" (main+0x147d3c0 -> main+0x34f3850) sized by [C+0x738], 100 MiB by default (ctor main+0x2ab874c); when that returns null, init returns before setting [C+0x720], and the next state request (main+0x2aba230) crashes the update. So every crash at this load is a memory shortage: 100 MiB (no gdbstub) or 3 MiB in the scene heap (gdbstub). Root heap is 0x19506f000 at 0x1145e10020 | open: why memory is short (reported total memory for Switch 2, frees delayed behind the GPU) |
| 80 | the memory shortage behind rows 75-79 | title 4 is told 9.55 GiB (GetInfo 21) and plans all of it: SetHeapSize 3.125 GiB plus a 6.33 GiB VAMM-backed sead root heap in the alias region; Eden backed the six 300 MB gpu 214 regions from the application pool, 1.8 GB the kernel still reported as free | 214 backing from the non-secure system pool, +8 GiB DRAM on the 12 GB layout, device memory tracker 35 physical bits (5cff620); run 9 passed the crash spot (NFC Finalize 272.8 s, ran to 388 s), no allocation failures |
| 81 | title 1 hangs before its first frame (~17 s): main logic stops, two cores spin, only audio and hid keep running, almost no GPU submits | it asks gpu 214 for ~20 GiB (two 0x16ad80000 regions, these two still fail with the new pool); gets further than before (save data, friend list) | open: thread snapshot at the hang; 214 regions this large must be sparse on the console |
| 82 | title 1 hang (row 81) | `monitor handles` (a4051e4) showed the main thread waiting forever on the nx2:vi2 32 event (loop at main+0x25ec5e4 until [obj+0x88] >= 2); the stub never signalled it | 31 and 32 events signalled at 60 Hz (6196a11); title 1 now presents (241 frames by 95 s) |
| 83 | title 1 after the hang: ~3 fps with "fence not reached in 4 s" stalls | unimplemented vi2 104/105 (every frame) and gpu 215, gpu 214 still short for the two 5.7 GiB regions, one Nx2Map "out of SMMU space", rejected shaders (formatted surface store, IPA mode, LEA carry) | open |
| 84 | title 1 GPU stalls (row 83): channels wait on semaphores 0x21f6093140/3040 until the 4 s timeout | NVN2 queue syncs run through a ring of QMDs (dependency 2 each: the previous entry's launch and macro 0xda, which patches release slot 52 and schedules/decrements via PCAS 3/7); macro 0xda reads the ring base from MME data RAM word 0x27, which each channel sets (0x21098d39, 0x213e8ff8, 0x213ea168) but Eden shared one data RAM for all channels, so every channel drove the same ring | per-channel MME data RAM, one QMD table for all channels (6c20e37); no stalls, title 1 reaches its title screen (world map with logo) |
| 85 | title 1 address space | 214 = CreateSparseRegion, 215 = DestroySparseRegion (SDK symbols); SMMU space (16 GiB) ran out | 64 GiB device space (71b38ab), 215 handled (cc0c400) |
| 86 | title 1 after its title screen: black frames | tens of thousands of rejected shaders: IPA interpolation modes, integer carry/third source, uniform comparison, formatted surface load/store, uniform constant load, unsupported opcodes | open |
| 87 | title 1 loading crash: Vulkan out of memory | an 8192x8192x68-layer virtual texture (~23 GB) | images over 2^28 texels are created sparse with no memory (`Unbacked`: no uploads/downloads) (96a9b81); the game reaches its real title screen ("Press L + R"), ground black (that texture has no data) |
| 88 | title 1 shader coverage | checked against nvdisasm (`nx2work/nvd.py` disassembles one instruction at a time, since a single illegal word stops nvdisasm); calls: RET may restore the link through IMAD.MOV with code between, library functions may EXIT, `@Pg EXIT Pc`; PLOP3/UPLOP3 any table, UP2UR, ULOP3 predicate output; UIMAD.WIDE, uniform USHF/FMNMX/I2F/F2I, LDS `[Ra.Xn+URb]`, ATOMG/RED/STG forms, PRMT, FSWZADD, REDUX.MAX | 1351c73, 52cda01, 49396b7: 1699 of 1712 dumped programs lower; left: 12 task/mesh programs (ISBERD/ISBEWR, row 89) and one SURED |
| 89 | task/mesh shaders | title 1 draws some geometry with NV-style task + mesh programs: the task program culls a batch and writes its payload with `ISBEWR.O.ATTR [Rn+0x10]` and the mesh count with `ISBEWR [0x4]`; mesh programs read it back with `ISBERD.ATTR.SKEW`. The most-drawn rejected program (0x114c459500) is a task program. Eden has no mesh pipeline | open: needs a task/mesh pipeline (VK_EXT_mesh_shader) or a compute + indirect-draw emulation |
| 90 | each rejected program was lowered and logged again on every draw (1.7M log lines in 13 min) | no negative cache | the shader cache keeps the first exception per address until that memory is written; logs deduplicated (bcb4ff0) |
| 91 | a compute program with a jump table: "Failed to track indirect branch" | Eden's tracker wants IMNMX imm -> SHL imm -> LDC -> BRX; IMAD.SHL was lowered to an XMAD multiply | IMAD by a power of two lowers to SHL (49396b7+) |
| 92 | with rows 88's compute forms, title 1 stays on its loading map (file reads stop ~46 s in) | two compute programs failing in Eden's compiler: a jump table whose targets lie past the first EXIT, and a 128-bit store of RZ from unaligned scratch registers | programs with BRX end at the compiler's closing branch-to-self; R248-R251 for 128-bit zero stores (3df6a81) |
| 93 | title 1 colour cast (magenta/cyan, red/green, changing per course) | the sky/atmosphere 32^3 volume shader starts with an unguarded R2UR; R2UR/MATCH/HFMA2.RELU saved P6 with the uniform-guard mask `1 << (6 - 7)` (undefined), which shifted into the opcode bits | always save/restore P6 (2ed0c48). Found with `EDEN_NX2_DUMP` (flush GPU ranges at a present to files) and `nx2work/vol3d.py` / `volslices.py` (untile Switch 2 3D textures) |
| 94 | near terrain black | 8192x8192x68 ASTC 8x8 virtual textures: 1.5 GB in guest memory, 23 GB each as Eden's RGBA8 decode | huge GPU-decoded ASTC arrays keep levels from the first that fits 512 MiB (here 3) on the host (60c4791) |
| 95 | wind/foliage shards | SM70+ MUFU.SIN/COS take turns (sin(2pi x)); Eden's Maxwell MUFU takes radians | scale by 2pi (245e6ad); jungle canopy not re-checked yet |
| 96 | title 1 aborts after ~1.5-4 min at the title or when a race starts (nn::diag::Abort from main+0x19e5df0) | the movie player (main+0x19e5860) lays out its decoder frames with nn::codec::CalculateDecoderImagePoolBuffer, which sends nx2:gsv 991; auto-stubbed, the reply kept the request words, so the frame size read as 0x167027f'00000001 and the budget check at main+0x19e5b58 failed. (Not the "video-frame-count" < 9 check: the game sets that from a two-slot registry, at most 2.) | gsv 991 answers a block-linear NV12 size (640x360 -> 0x64000) |
| 97 | pop-in, objects flicker in and out, low-quality meshes | high-detail LODs drawn through task/mesh programs (bound in stage slot 1, header type VS, ISBERD/ISBEWR), which are rejected and skipped | open: task/mesh support |
| 98 | ~12 fps at the title | host GPU ~20% busy, emulated cores ~0.9 host cores, GPU thread 45-70% busy, much of it waiting on the host GPU: semaphore acquires wait for host fences under GPU accuracy High; ASTC uploads end in Finish() (a Flush() there crashed at boot); a 1 ms poll sleep while channels are suspended | gpu_accuracy=Low: ~16 fps; SDL main loop capped at 240 Hz (88f04ad). Open: ASTC upload without a full stall (and per-layer re-upload of the trimmed virtual textures), native MME interpreter cost, draw path |
| 99 | VRAM over the card (8.4-9.8 GB used on an 8 GB RTX 4070 Laptop), texture cache always collecting | ~700 ASTC 8x8 textures decoded to RGBA8 (16x their size, ~4.5 GB) | astc_recompression=Bc3 + accelerate_astc=CpuAsynchronous in the config; huge ASTC arrays stay on the GPU decoder as trimmed RGBA8 (ed0a3c9): 4.6-5.6 GB, ~10 -> 16 fps |
| 100 | 8 ImageTranscode threads at ~44% each (3.5 cores) | Eden's default pool is half the hardware threads | an eighth, at least 2 (`EDEN_NX2_TRANSCODE_THREADS` overrides): ~19 fps on the attract loop |
| 101 | Medal's capture hook inside Eden (`medal_hook64!OBS_Negotiate` in GPU-thread stacks) | Medal registers a per-user Vulkan implicit layer that loads into every Vulkan app | launches set `DISABLE_VULKAN_MEDAL_OBS_CAPTURE=1` (the layer's own off switch) |
| 102 | host crash (log stops, segfault) at 113 s in one run; under cdb "Free Heap block modified after it was freed" at 243 s, twice | Eden's TickAsyncDecode erased a finished decode while still holding its mutex (unlocked freed memory); only reached with accelerate_astc=CpuAsynchronous (the BC3 setup) | release the lock first, cancel decodes of deleted images (8766a72); clean past 330 s. Also made nx2 service callbacks (vsync timer, syncpt actions, capture/dump) independent of their sessions' lifetime (190747a) |
| 103 | (title 1) texture churn: the collector evicted ~1,250 decoded ASTC textures per 600 frames and they were decoded again (lag and pop-in turning the camera) | device memory sat above the collector's critical line (5.4 GB of a 7.0 GB budget): ASTC 8x8 is 4x larger as BC3, and a bloom chain at unmapped GPU 0xf0000-0x15b600 was re-created on every use (each lookup got a fresh stand-in CPU address), ~5,400 images per 600 frames on the map | unmapped GPU addresses get a stand-in address fixed by the GPU address, so images are found again; ASTC whose blocks carry no alpha (exact endpoint-mode scan, ~70% of them) goes to BC1 (`EDEN_NX2_ASTC_BC1=0` turns it off). Attract loop: 4.8-5.1 GB, no costly evictions, ~17 -> 22-23 fps |
| 104 | garbage textures (title logo, road patches) and a host crash 90-120 s in that skipped the crash handler (fail-fast) | an image re-decoded while a decode was in flight has two; the first to finish cleared IsDecoding, so deleting the image did not cancel the second, which was then uploaded into the next image to take the slot (another texture's data; a staging overrun when that image was smaller) | every pending decode of a deleted image is cancelled; a decode larger than its image is dropped and logged. Frames clean; crash: verifying with long runs |
| 105 | (title 1) movie abort moved on: `nn::codec::InitializeVp9Decoder` failed with 2001-0114 (InvalidHandle) | gsv 901/902/1001/1002 (`nn::codec::video::detail::ipc::Initialize`) were auto-stubbed; 902/1002 must return sessions (handle word 3, result word 4) whose TIPC 1 (CreateVideoDecoderService) returns another | sessions returned (2970100); the movie plays with no picture. Open: decode VP9 (vdecsvc 200/101/3/311 are auto-stubbed) |
| 106 | GPU thread ~50% in the Turing macro interpreter | every instruction re-decoded on every run, register file copied per instruction | decode once per PC, operands read up front (a97d26d): GPU thread ~68% -> ~45-50% busy |
| 107 | streamed textures shown as garbage/black for seconds; 2 transcode threads pegged | ASTC was decoded and re-compressed to BC on the CPU, ~40 textures/s on the attract loop | accelerate_astc=Gpu (config value 1) + BC recompression: GPU ASTC decode into a temporary RGBA8 image and `bc_encoder.comp` (69cc66b); textures ready the frame they are used |
| 108 | frame time is CPU work + GPU-thread work in series | nx2:vi2 229 blocks the game until Eden's GPU thread has processed the frame (WaitForFrame), and RequestComposite waits for the GPU thread to drain its queue too; the present timeline log shows 8-56 ms of each 34-105 ms frame blocked | `EDEN_NX2_FRAMES_AHEAD=1` (wait for the previous frame) deadlocks at the first scene change: the main thread waits on syncpoint 1 (present fences) for a value no queued work reaches. Open |
| 109 | ~1,400 SVCs per frame | the job system's WaitForAddress (0x34) / SignalToAddress (0x35) pairs and yields, all through Eden's global scheduler lock (3 of 6 cores seen blocked on it) | idle-core spinning (`EDEN_NX2_IDLE_SPIN_US`) gave no gain. Open |
| 110 | (title 1) GPU thread saturated, 3D attract scenes ~22 fps | profiled with the new GPU zone timers (`EDEN_NX2_GPU_PROFILE`) and the in-process sampler (`EDEN_NX2_SAMPLE`): per second, ~355 ms command parsing outside macros, ~210 ms in 745k macro calls, ~325 ms draw setup (33k draws), ~89 ms compute (4.4k dispatches) | see 111-115; now ~27-33 ms per frame in the same scenes |
| 111 | the guest clock (CNTPCT) cost ~10% of the main core | AMD CPUs have no CPUID leaf 0x15, so the TSC frequency stayed 0 and every read went through QueryPerformanceCounter | measure the TSC when the leaf is missing (948a391) |
| 112 | C++ exceptions on the GPU thread (~1-2%) | a rejected native shader (mesh/task programs) was rethrown twice every time it was bound | return false for remembered rejections (948a391) |
| 113 | debug aids on the hot path (~4% of the GPU thread) | semaphore watch per command word, FNV hash of every pushbuffer entry, first-write trace, two debug logs per macro call | flag checks unless their variable is set (948a391) |
| 114 | texture churn: 1,400-2,600 textures deleted and recreated per 600 frames, one purge of 4,300 | ~38% of device memory was empty space in 256 MiB VMA blocks; usage came from the driver, so evictions did not lower it | 64 MiB blocks and usage minus VMA block slack (7b0ff0b): 0-224 per 600 frames, 6.24 of 6.97 GB used |
| 115 | main core: ~12% slow-path reads of GPU-tracked pages, GPU thread and CPU fighting over the caches' mutex | `use_reactive_flushing` (on by default on desktop) protects tracked pages against reads too | off in the test config: the slow reads are gone, no visual differences seen in the attract loop. Kernel lock spins before sleeping (5c99031): ~20-25% less time blocked |
| 116 | macro interpreter is ~11-13% of the GPU thread | operand fetch is not the cost: a slot-based rewrite matched the old interpreter on 200,000 recorded calls (`EDEN_NX2_MME_RECORD`, `C:\dev\nx2work\mme_test`) but was no faster | open: needs a JIT or pre-built handlers |
| 117 | macro calls: ~1,000,000 a second, 7 instructions each, 96% exact repeats | the per-call interpreter cost was the time, not the instructions | memo of calls that depend only on their parameters and the data RAM words they read (1cd4a3c, b75fdf8, c28ea9a for compute macros): 97% replayed, verified on 200,000 recorded calls |
| 118 | crashes ~26 s in (fencing thread walking a corrupt operation list; query cache Barriers; Vulkan worker SubmitExecution) | `async_presentation=true` (config): 4 crashes in 8 runs with it, 0 in 6 without; a race between the present thread and the Vulkan scheduler | keep it off (test build 3b). The fence manager's SyncOperation also appended to a list SignalFence moved from another thread: own lock now |
| 119 | (tester, RTX 4070 SUPER) 50 MB log; textures evicted with 3 GB VRAM unused | nx2:vi2 104/105 called ~75 times a second through the logging auto stub; collector thresholds from Eden's ~7 GB device memory cap | quiet stubs (649fb56); thresholds from the driver budget (49225dd) |
| 120 | GPU thread asleep ~8% on the texture/buffer cache mutexes | holders: the fencing thread (it took both caches for every fence, one per compute dispatch, and holds the buffer cache while it copies GPU-written buffers back) and CPU write traps; std::recursive_mutex slept at once | spinning recursive mutex, per-cache fence flush flags, separate locks (f10767f): asleep 3.7%. Buffer downloads at every fence stay: the native MME DMA reads of indirect arguments depend on them (no synchronous flush logged with them on) |
| 121 | first-run freezes from shader translation (one 28,102-instruction compute shader: 3.4 s on the GPU thread) | BreadthFirstSearch's visited list was a linear scan (quadratic), and GlobalMemoryToStorageBufferPass searched again for every access | hash set past 64, per-start memo (ba173b6): 0.85 s; runtime builds now ~2-4 ms each. Translating on workers was not done: environments read live GPU state (texture types, constant buffer values) while translating |
| 122 | final clean run (attract loop): 21-25 ms per frame in most 3D stretches, 26-38 ms in compute-heavy courses (industrial, highway), host GPU ~38% busy | the GPU thread is still the limit: draw setup ~25%, command parsing and register writes ~15%, compute dispatches/inline uploads (heavy courses) | open: per-draw binding cache, uniform buffer copies (staging slices are recycled by tick, so reuse needs care), buffer download interval sets |
| 123 | (title 1, 1.8.0 update ExeFS `updates/917504`) loading screen forever, ~116 s in: `Waiting for service chat:u` | the newer SDK runs `nn::chat::Initialize` (GameChat) at boot; nothing registers `chat:u`, so sm defers the request for good | `chat:u` registered from the friends process (friend.cpp): cmd 0 returns a session, whose cmd 1 (u32 2) returns an event (left empty: 2010-0212, then 2001-0114 InvalidHandle) |
| 124 | 2162-0001 (GameChat's own module) from `nn::chat::gsv::Initialize` | it sends gsv 1201 then 1202, the same pair as 901/902 and 1001/1002: 1202 must return a session. On it, TIPC 1() returns a u32 id (abort if > 0x3ff, sdk+0x6c1b40) and TIPC 11(u32 id) an event (copy handle word 3, result word 4; sdk+0x5d8710) | 1201 stub, 1202 returns `INx2ChatGsvSession` (nx2_gsv.h) |
| 125 | `nvnNativeImageInitialize` returns false, abort at main+0xd32438 | right after GameChat init the game builds its local camera video storage (`nn::chat::InitializeLocalCameraVideoStorage`, three 384x216 images, format 0x38). `CalculateRequiredBufferMemory` sends gsv 1291 (1510's request plus a u32 count; reply u64 size, u64 alignment), auto-stubbed to 0x1000 bytes; `CreateImagePool` sends gsv 1211 (429's request); image i of a pool has handle pool id + i (`nn::gs::sdk::CalculateNativeImageHandle`), so a 429 pool of 3 left handles +1/+2 unknown | 1291 = layout size * count; 1211 = 429; 429/1211 reserve `count` ids |
| 126 | same abort (row 125), then inside `ExecuteVirtualMachineMapBufferEx` | NVN describes and maps images another server made with gsv 428 `nn::gpu::sdk::QueryNativeImage` and 426 `MapNativeImage`(handle, u32, address space, u8 permission). Reply: words 3-4 GPU address (426), 5-6 offset in the memory, 7-38 a `GpuOunceUserImageDescriptor`, 39 flags. The SDK requires descriptor byte 0x00 == 1, byte 0x20 == 1, u32 size at 0x2c != 0 (offset at 0x28); NVN reads (w-1)|(h-1)<<16 at 0x08 and the layout kind at 0x24 (1: formats 0x25/0x37/0x38, 2: 0x99-0x9c; sdk+0x6bb960) | both answered from the pool's preference and memory; block height/pitch at 0x10/0x14 are a guess (only blank camera frames use them) |
| 127 | abort 2034-0000 at main+0xcded94 (~125 s, after the normal graphics boot) | `nn::camera::sdk::CreateCameraContext` sends gsv 601() then 602(u64 1), which must return the camera context session (handle word 3, result word 4); without it the SDK sends 603 (destroy) and fails | 601 stub, 602 returns a `nx2:camera` stub session. Title 1 (1.8.0) then plays |
| 128 | (title 1, 1.8.0, Ryzen 5 2600 + RTX 3050 6 GB) 8 fps | Eden defaults: gpu_accuracy High, uncompressed ASTC, reactive flushing on | gpu_accuracy Low, astc_recompression Bc3, use_reactive_flushing off, async shaders on: ~20 fps, uneven. Bushes/cacti flicker (row 97). Boot also froze once at "Failed to allocate 20480 MiB of backing memory": the 12 GB layout needs 20 GB of free commit up front |

Run settings used (in `build/bin/user/config/sdl2-config.ini`): `memory_layout_mode=4`
(12 GB). Debug knobs: `EDEN_NX2_TC_EXPECTED_MIB` / `EDEN_NX2_TC_CRITICAL_MIB` (collector thresholds),
`EDEN_NX2_ASTC_FLUSH=1` (GPU ASTC decode without the full wait), `EDEN_NX2_CRASH_DUMP=<dir>` (prints the
faulting stack as module+offset, then a minidump), `EDEN_NX2_AUTO_STUB`, `EDEN_NX2_IPC_TRACE`, `EDEN_NX2_SVC_TRACE`,
`EDEN_NX2_PULLER_TRACE`, `EDEN_NX2_METHOD_TRACE`, `EDEN_NX2_HIDE_WINDOW`,
`EDEN_NX2_GPU_PROFILE` (GPU-thread zone timers every 10 s), `EDEN_NX2_SAMPLE=<threads>` with
`_START`/`_SECONDS`/`_OUT` (sampling profiler; `C:\dev\nx2work\folded.py` reads its .folded stacks),
`EDEN_NX2_MME_RECORD=<file>` (macro calls for `C:\dev\nx2work\mme_test`),
`EDEN_NX2_FRAME_CAPTURE=<dir>` (every 120th presented frame as TGA), `EDEN_NX2_PB_CHECK`
(pushbuffer entries changed between submit and execution), `EDEN_NX2_QUERY_TRACE` (every GPU
report/semaphore write). `C:\dev\crashrun.sh` runs eden-cli under cdb and prints the crash stack;
`C:\dev\stackdump.sh <main> <seconds>` dumps host thread stacks of a running test.

## Where it stops now

All runs: `EDEN_NX2_AUTO_STUB=1`, 12 GB memory layout, loose ExeFS with the sibling `romfs/`.

**Title 4** (`fourth switch game\exefs`, 0400380020056000) **plays its intro cutscene correctly
rendered**: boot, loading screen, menu (difficulty select, keyboard input), confirm, then the intro
(plane cockpit, newspaper/map) lit, colour-graded and subtitled, matching console footage. The
DLSS-style upscale is a bilinear stand-in. In a played session the game went on to a later loading
screen that did not finish; open there: a compute shader with a 64-bit add (UIADD3 carry-out to
UP0), TMML (0xb69) in two graphics shaders, "reserved attribute 23" (a[0x5c]) reads. Scripted runs
(`C:\dev\run.sh`) move any title-4 save to `C:\dev\save_backups` first so they start a new game.
Scripted input: `EDEN_NX2_KEY_SCRIPT="50:4,50.5:-4,55:79,55.4:-79,57:4,57.5:-4"`.

**Title 3**: last checked before the graphics work; should now track title 4.

**Title 1, 1.8.0 update** (`updates/917504/exefs`, same program id): needs the GameChat and
camera stubs of rows 123-127, then plays (Qt frontend `eden.exe`, ~20 fps on row 128's hardware) with
the same open problems as below.

**Title 1** (`first switch game\exefs`, 0400C3F00006E000, `Drive.nss`): boots, shows the world-map
loading screen and reaches its title screen ("Press L + R to start") with sky, buildings, props,
kart and logo drawn. Wrong: magenta/cyan colour cast, black ground (unbacked virtual texture, row
87), task/mesh geometry missing (row 89), ~10-17 fps. Launch with frame capture:
`EDEN_NX2_FRAME_CAPTURE=C:/dev/frames_t1x EDEN_NX2_AUTO_STUB=1 EDEN_NX2_SHADER_DUMP=C:/dev/shaders_title1 ./eden-cli.exe -g "C:/switch/first switch game/exefs/main"`.
Offline shader check: `nx2work/lowertest/lowertest.exe C:/dev/shaders_title1 [-all [op]]`.

**Title 2**: main/sdk/subsdk0 fail LZ4 and SHA-256 checks; can't be run from these files.

## Prioritized next steps

1. **Title 4 past the intro**: lower the 64-bit add with carry (UIADD3/IADD3 carry-out and .X
   carry-in), TMML, and attribute 0x5c; then whatever the next area needs (dump + lowertest).
   Squid/paint-splat hatch on the menu. A faster tensor-core emulation (shared-memory HMMA) would
   replace the bilinear upscale stand-in.
2. **Title 1 / title 3** retests with the current tree; dump their shaders and widen the lowering.
3. Replace guesses with real protocol knowledge: syncpt page layout, the semaphore mirror page,
   gsv 214 (GPU-only memory) and vi2 present buffer availability (real buffer release instead of
   the fence wait), gsv 1510 formats, svcGetInfo 29-33.
4. **Six cores** instead of folding 4/5 onto 0/1; rare SIMD encodings still undecoded.
5. Upstream-worthy pieces independent of Switch 2: MSVC 14.51 build fixes, eden-cli SDL3
   lifetime fixes and `_setmaxstdio`, ExeFS loader fixes, FAC/FAH offset fix, GetPseudoDeviceId
   without NACP.
