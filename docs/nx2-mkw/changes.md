# Changes in this fork

Base: the [Eglantine](https://github.com/Djihads80/Eglantine) project's `nx2` branch at
`ea33ad1865` ("vulkan: write the driver pipeline cache atomically; reject truncated ones",
2026-10-30). Eglantine's `nx2` carries Takoroka's Switch 2 work and the Mario Kart World 1.8.0
fixes that the earlier releases of this fork had to add themselves. Branch `mkw-eglantine` is that
base plus the commits below, and nothing else:

```bat
git log --oneline --reverse eglantine/nx2..mkw-eglantine
```

They are grouped by purpose below, under their exact subjects; each commit message gives the
details. "Ours" means written for this fork (with Claude Code, see the
[AI disclosure](README.md#ai-disclosure)); the other rows name their author and origin.

## 1. The game shows its real version

| Commit | Author | What and why |
| --- | --- | --- |
| am: display version from the loose ExeFS when there is no control data | Ours | A loose dump has no `control.nacp`, so `GetDisplayVersion` answered "1.0.0" and the game showed it while running 1.8.0. The loader now reads a `control.nacp`, then a `display_version.txt` ("1.8.0"), in `exefs\` or next to it (`src/core/loader/`, `am/service/application_functions.cpp`) |

## 2. Option C: no freezes on new shaders

All three are explained in [`NX2_SHADER_STUTTER.md`](../../NX2_SHADER_STUTTER.md).

| Commit | Author | What and why |
| --- | --- | --- |
| shader_cache: lower each native program once | Ours | A Switch 2 program was captured and lowered to Maxwell code for the shader cache, then again for every pipeline built from it, on the GPU thread. `ShaderInfo` now keeps the result (`LoweredNativeCode`) and the pipelines' environments reuse it |
| nx2: capture native programs a page at a time | Ours | The capture read 16 bytes per instruction, each with a mapping check and a GPU cache flush. It now reads up to 4 KiB at once (`NativeInstructionReader`); the captured program is the same |
| nx2: fold bank 1 reads of cached native programs as live ones | Ours | Programs rebuilt from the disk cache at boot did not fold constant buffer 1 like live ones (`is_proprietary_driver`): the replayed pipeline was not the one the game had used |

## 3. Video memory never overflows

Explained in [`NX2_VRAM.md`](../../NX2_VRAM.md).

| Commit | Author | What and why |
| --- | --- | --- |
| texture_cache: keep Switch 2 thresholds under the main heap's budget | Ours | The collector's thresholds followed the sum of every heap's budget, which on AMD counts the same VRAM twice: allocations went past the budget, textures came out as garbage, then the game crashed. The thresholds now follow the main device-local heap's budget, re-read every 30 frames. `EDEN_NX2_VRAM_LIMIT_MIB=<n>` makes a big card behave like an n MiB one, for testing |
| vulkan: re-compress ASTC to BC3 for Switch 2 titles under 12 GiB | Ours | Uncompressed ASTC does not fit on smaller cards: there a Switch 2 game uses BC3 for the session (the configuration file is untouched). `EDEN_NX2_KEEP_ASTC=1` opts out |
| texture_cache: count unbacked images by the device memory they have | Ours | Eglantine's sparse terrain images have no memory, or memory for some layers only, but the collector's own estimate between two driver readings counted them at their full size: deleting one looked like freeing gigabytes, or took the total below zero. They now count for what is bound to them |

## 4. The picture no longer steps back

| Commit | Author | What and why |
| --- | --- | --- |
| nx2: execute command lists as submitted | givethesourceplox, Eglantine PR #28 (not merged yet) | Mario Kart World refills command memory it has already submitted while the GPU thread is still a frame behind: about 1 frame in 10 was drawn into the wrong image and the picture stepped back. Each entry's words are copied at submit and the copy is executed. `EDEN_NX2_PB_SNAPSHOT=0` executes from memory as before |

## 5. Fixes to Eglantine's code

These fix bugs that are in Eglantine's `nx2` as well, and are the first candidates to propose
upstream.

| Commit | Author | What and why |
| --- | --- | --- |
| vulkan: don't divide by a zero sparse block size (AMD) | Ours | **Worth proposing to Eglantine.** On an RX 6900 XT, `vkGetImageSparseMemoryRequirements` returns an `imageGranularity` of 0 and `Image::InitSparseLayers` divided by it: the emulator crashed (0xc0000094) while Mario Kart World loaded its map. It now uses Vulkan's standard 2D block shape when the device reports `residencyStandard2DBlockShape` (logged), and otherwise does not bind that image by layer |
| nvdrv: clear the sparse region state with the other Switch 2 globals | Ours | Which sparse blocks were written is global state: a game started after another in the same window could find its images "unwritten" (no memory, black) |
| core: end device page runs where pages are unmapped or mapped again | Takoroka, Eglantine branch `fix-page-runs` (not in `nx2` yet) | Unmapping or remapping device pages left stale run lengths, so a read could copy the host memory of an old mapping, or zeros (Splatoon Raiders lost its dialogue text) |
| shader_recompiler: run the destructors of pooled objects | Ours | `ObjectPool` never ran the pooled objects' destructors: every shader build leaked the memory IR instructions own (phi operand lists, associated instructions) |

## 6. Android

| Commit | Author | What and why |
| --- | --- | --- |
| NCE: add VisitorBase entries for the new A64 decoder rows | hesanaltdoesgithub (his Eglantine fork, branch `nx2-android`) | Android uses NCE's interpreter visitor, which must provide every entry of `a64.inc`, the Switch 2 rows included |
| Android: list loose ExeFS games and install as Eglantine | hesanaltdoesgithub | The game list finds loose ExeFS dumps (`main` + `main.npdm`). Its package id change is undone by the last commit of this group |
| Android: name extracted games after their folder, past update folders | hesanaltdoesgithub | A dump in `updates/917504/exefs` is listed under the game's folder name, marked "(update)" |
| android: per-SoC build presets; keep the Optimized flavor's package id | Ours | `YUZU_BUILD_PRESET` gains `sd8elite`, `sd8gen3` and `sd870` (instruction set shared by all cores of the chip, tuned for its big cores). Release builds keep Eden's package id: the Optimized flavor needs its exact id for phones' game modes |

## 7. Documentation

| Commit | Author | What |
| --- | --- | --- |
| docs: NX2_SHADER_STUTTER.md, NX2_VRAM.md, notes rows 150-152 | Ours | Design notes for option C and video memory; [`NX2_NOTES.md`](../../NX2_NOTES.md) rows for option C, video memory and the sparse state reset |
| docs: add the Mario Kart World fork documentation (docs/nx2-mkw) | Ours | These pages |
| README: point to the Mario Kart World documentation and releases | Ours | Banner at the top of `README.md` |

## What the earlier releases had and Eglantine now provides

`release-c` and `release-d` were built on Takoroka's eden-nx2 of 2026-10-29 and carried these
themselves; they are part of Eglantine's `nx2` now, so they are no longer changes of this fork:

- Mario Kart World 1.8.0 boot: `chat:u`, the GameChat and camera graphics sessions (NX2_NOTES rows
  123-127) and the window size fix.
- Stopping and closing without a hang (timer thread, GPU split ring, fastmem arena, Switch 2 GPU
  globals, audio renderer).
- The driver pipeline cache written atomically (Eglantine PR #29).
- What `release-d` had caught up from Eglantine: service stubs for Switch 2 programs, 2D TXD,
  the MSVC and GCC build fixes, the Mario Kart World loading crash fix, debug-level diagnostic
  logs, the QMD cache fix, the 31.25 MHz guest counter, and the sparse memory work (regions get
  memory only where the GPU writes, images over unwritten blocks get none, terrain arrays are
  bound layer by layer).

## Left out on purpose

- Five commits of Takoroka that `release-d` took from Eglantine's `nx2-experimental` branch, which
  are not in `nx2` yet: "trim huge ASTC arrays harder on small GPUs", "report each auto-stubbed
  command once", "skip clearing guest memory that is still zero", and the two dynarmic commits
  ("keep the x64 JIT's per-block tables compact", "skip untouched locations when an IR instruction
  ends"). They save RAM and log noise; they will come with a later update once Eglantine merges
  them into `nx2`.
- `release-d`'s hand ports of Eglantine's logging and sparse memory commits: Eglantine's own
  versions are used. Only the memory accounting those ports added is kept, as its own commit
  (section 3); unlike `release-d`, it does not count the mip tail a terrain array shares between
  its layers (a few sparse blocks), so that Eglantine's `Image::BindLayers` stays as it is.
- "nx2:gsv: close GetCpuImageDescriptor after the MKW 1.8.0 merge": it only repaired
  `release-c`'s own merge of the 1.8.0 fixes.

## Environment variables added by this fork

| Variable | Effect |
| --- | --- |
| `EDEN_NX2_KEEP_ASTC=1` | Keep uncompressed ASTC even on a card under 12 GB |
| `EDEN_NX2_VRAM_LIMIT_MIB=<n>` | Pretend the card has `<n>` MiB of video memory (testing) |
| `EDEN_NX2_PB_SNAPSHOT=0` | Execute command lists from memory (turns PR #28 off) |

Eglantine's own include `EDEN_NX2_SPARSE_LAZY=0` (sparse regions backed whole, terrain images as
before) and `EDEN_NX2_CNTFRQ=<Hz>` (guest counter rate); Takoroka's (`EDEN_NX2_SPLIT`,
`EDEN_NX2_AUTO_STUB`, `EDEN_NX2_SHADER_DUMP`, ...) are listed at the end of
[`NX2_NOTES.md`](../../NX2_NOTES.md).

## Proposing a change upstream

Each commit stands on its own on top of Eglantine's `nx2`, so it can go to Eglantine as one pull
request (`git cherry-pick` onto a branch from `eglantine/nx2`). Eglantine follows Eden's
[AI policy](../policies/AI.md): replace the `Co-Authored-By: Claude` trailer with an
`Assisted-by:` one, and the human submitting it must have tested it and be able to explain every
line.
