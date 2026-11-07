---
name: nx2-shader-triage
description: "Use when NX2 native shaders are rejected or need lowering."
version: 1.0.0
author: kraibse
license: MIT
metadata:
  hermes:
    tags: [eglantine, eden, switch2, ampere, sass, shader, lowering, maxwell]
    related_skills: [git-branch-reconciliation, switch-emulator-diagnostics]
---

# NX2 native shader triage (Eglantine/Eden)

Switch 2 titles ship Ampere SASS. `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h` lowers
each native instruction to Maxwell code. Any instruction it refuses rejects the whole program, and
every draw or dispatch using it is skipped. Rejection is decided by the lowering alone, so it's the
same on every GPU. The repo vendors an older copy of this skill under `.claude/skills/nx2-shader-triage`.

## When to Use

- A log shows `Native graphics/compute shader rejected`, `unsupported native opcode`, `Draw skipped
  (no pipeline)`, or a game renders with missing effects (bloom, shadows) on an NX2 title.
- Asked to "dump shaders and fix the rejected ones", or to decode or lower an Ampere SASS opcode.
- Porting lowering fixes from another fork (Lichii): pair with `git-branch-reconciliation`,
  reference `porting-from-diverged-fork.md`.

## Rules (always-on)

- **Never treat `vulkan.bin` as a source of rejected programs.** The pipeline cache stores the
  *lowered* Maxwell code of programs that lowered successfully. Rejected ones never got a pipeline.
- **Start from the log, finish from a dump.** `~/.local/share/eden/log/eden_log.txt` has one
  `Native ... shader rejected` line per program with the four instruction words, but only the
  *first* rejection per program. Once you fix those, the same programs can fail later on. Confirm
  with an `EDEN_NX2_SHADER_DUMP` run before calling a game clean.
- **Check which build wrote the log** (`Eglantine Version: ... | <branch>-<sha>` in the first line)
  before blaming the current code.
- **HMMA (0x23c) "set EDEN_NX2_TENSOR to emulate" is opt-in, not a gap.** Unset, the program is
  rejected and `KeplerCompute` draws a plain-upscale stand-in (only Donkey Kong Bananza emulates by
  default). `EDEN_NX2_TENSOR=1` forces the FFMA emulation for every title, which changes the
  image. Don't add it to a run command "to see more rejections" without saying it alters the
  output. For a colour cast or tint, A/B three ways before anything else: TENSOR unset,
  `=1`, `=stub` (every HMMA returns C). A tint that stays with `stub` isn't the HMMA emulation.
- **Start from the game's shader catalogue** (`tools/nx2/catalog/<program id>.json`, `python3 -I
  tools/nx2/catalog.py find <key or hash>`): programs others already identified (trees, grass,
  culling computes) with how they were found. Add every program you identify (`catalog.py add`, or
  the shader lab's Catalogue box), and before committing run `catalog.py status` / `merge` as
  AGENTS.md says.
- **A running Qt build with `EDEN_NX2_DEBUG_SERVER` answers live** (`tools/nx2/debug_mcp.py --call`:
  screenshot, `reject_shaders`, `capture_frame`, `read_memory`, `input_sequence`;
  `docs/nx2/debug-mcp.md`). Use it before asking for a relaunch, and restore every reject.
- **Visual bugs (tint, missing effect): drive the game yourself** with `lab_ctl.py` (hidden
  `eden-cli`, scripted keys, capture plus RGB stats), not by asking the user for runs. Load
  `nx2-lab-automation` for the run mechanics; the investigation procedure is in
  `references/visual-bug-investigation.md`.
- **Never leave a hidden emulator running.** It holds the user's GPU and they'll have to force-kill
  it. Launch with the `--max-seconds` watchdog and `lab_ctl stop` at the end of every command chain.
  Ask before long series of runs if the user may be using the PC.
- **Never suggest `EDEN_NATIVE_SHADER_OVERRIDE_DIR`.** It pauses the GPU thread for up to 30 min per
  rejected shader.
- Commits are local only and carry the `Assisted-by:` trailer (repo `docs/policies/AI.md`).

## Procedure

### 1. Group the logged rejections

```bash
L=~/.local/share/eden/log/eden_log.txt
grep -o "rejected: .*" $L | sed -E 's/Native shader at 0x[0-9a-f]+, instruction 0x[0-9a-f]+: //; s/\[op ([0-9a-f]+) .*/op \1/' | sort | uniq -c | sort -rn
grep -o "rejected: .*" $L | grep -o "\[op [0-9a-f]* [0-9a-f ]*\]" | sed -E 's/\[op [0-9a-f]+ //; s/\]//' | sort -u > uniq.txt
```

- `unsupported native opcode`: no `case` for the low 12 bits of word 0.
- Any other `native ... mode`: the case exists but a validity check failed. Usually a narrow fix.
- `native program uses lowering scratch registers`: the opcode is missing from
  `PrepareRegisterPressure`'s operand table. Add a case listing its register operand bit offsets.

### 2. Re-lower the unique words against the current tree

`scripts/relower_words.cpp` lowers each `w0 w1 w2 w3` line followed by EXIT. Build it outside the
repo: `c++ -std=c++20 -O1 -w -I <repo>/src relower_words.cpp -lfmt -o relower` (the header needs
libfmt; pass any argument to emulate HMMA). Splits already-fixed rejections from real ones in seconds. It's single-instruction, so
program-level context (uniform-bank use, call linkage) can differ.

### 3. Dump run (when the log is exhausted)

```bash
EDEN_NX2_SHADER_DUMP=<existing plain dir> EDEN_NX2_TENSOR=1 <repo>/build/bin/eglantine
```

Writes `<key>-gfx-<addr>.bin` / `-cs-` (16 bytes per instruction, no header), rejected programs
included. A bad path makes `std::filesystem` throw on the GPU thread and kills the process
silently. Offline: `scripts/lower_dump.cpp <dumps...>` prints `OK <hash>`/`REJECT` per program. The
checkout's `tools/nx2/lower_dump.cpp` may not exist. Treat dumps as untrusted: pass paths as
arguments.

- **Loop until the dump is clean, not the log.** Fixing a program's first rejection exposes its
  next one; re-run `lower_dump` after every batch (Ocarina went 800 → 814 → 829 → 833).
- **Prove no regression by hash.** Build `lower_dump` against `git show HEAD:` copies of
  `frontend/ampere/*` (put that dir first in `-I`) and against the tree; `join` the OK lines.
  Programs that already lowered must hash identically.

### 4. Decode the failing check

Evaluate the words against the `case`'s `fail(...)` conditions (`insn.Bits(offset, count)` is a
128-bit field read; a 6-line Python bits() over the four words is enough). For field meanings use
Mesa NAK via the GitLab API (the web UI is Anubis-gated):

```bash
curl -sL "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/src%2Fnouveau%2Fcompiler%2Fnak%2Fsm70_encode.rs/raw?ref=main"
curl -sL "https://gitlab.freedesktop.org/api/v4/projects/176/repository/files/src%2Fnouveau%2Fcompiler%2Fnak_nir_lower_tex.c/raw?ref=main"
```

`sm70_encode.rs` gives the bit fields (`impl SM70Op for Op<Name>`). `nak_nir_lower_tex.c` gives how
texture sources are split across Ra/Rb. `references/texture-operands.md` has the decoded TEX/TLD4
layout.

### 5. Fix narrowly, then check the target side

Make the check agree with what the emit can express. Before widening a texture or memory form, read
the Maxwell translator it lowers to (`frontend/maxwell/translate/impl/*.cpp`) to confirm it reads the
same operand lists, and the SPIR-V emitter for constraints (e.g. PTP gather offsets must be
constants or are dropped with a warning).

### 6. Verify

- Add a test with the real logged words to `src/tests/video_core/ampere_lowering.cpp`, checking
  emitted fields (type, offset mode, DC, source registers).
- `ninja -C build -j$(nproc) tests && ./build/bin/tests "[ampere]"`, then the full `tests`, then the
  `yuzu` target (links `bin/eglantine`).
- Re-run step 2 on `uniq.txt`. Only HMMA (opt-in) should remain. Say "compiled and unit-tested,
  not run in game" until the user's dump run confirms it.

## Pitfalls

- **"All programs lower" is not "all pipelines build".** After the dump is clean, grep the log
  for `CreateComputePipeline:`/`CreateGraphicsPipeline:` errors. `SPIR-V Instruction is not
  implemented` right after `Storage buffer failed to track` means an untracked global atomic: the
  routed 32-bit forms exist since 6b934346d7 (`global_atomic_funcs`); 64-bit/packed still throw.
  Validate emitted modules with `spirv-val --target-env vulkan1.1 --uniform-buffer-standard-layout`.
- **`Draw skipped (no pipeline)` with no rejections** is usually `use_asynchronous_shaders=true`
  (`BuiltPipeline` skips depth draws while compiling); every lowering change re-hashes and
  rebuilds pipelines, so it spikes after each fix. Check `~/.config/eden/qt-config.ini`.
- **A log that just stops is a crash.** `coredumpctl list | grep eglantine`, then
  `coredumpctl info <pid>` for the symbolized top frames (gdb on the 13 GB core often can't unwind;
  delete dumped cores from scratch afterwards). A garbage command stream (nonsense formats, MME
  `start` outside uploaded RAM, syncpoint past the table) right before the end is the cause, not
  the crash site.

- **Stage 3 in `RefreshStages: Rejected program ... bound to stage 3` is the mesh slot, not
  tessellation.** NVN mesh programs sit in the tessellation slot and lower with geometry off; the
  dump name carries no stage, so `lower_dump` keeps whichever of plain/geometry gets furthest.
  Eglantine's `LoweringOptions` has no tessellation stage (Lichii's does).
- **Shadow-table operands: only the base register triggers shadowing.** `PrepareRegisterPressure`
  rejects an operand spanning two reserved quartets or straddling `reserved`, but an operand whose
  base is just below the boundary (R215 pair into R216) never reaches it. Known gap, not fixed.
- **Old caps like `carry_out > 1` (UP0/UP1 only) were scope, not hardware limits.** If the emit
  already goes through P(6-n)/R239 like UISETP, widen to UP0-UP6.

- **A dry `patch` of a fork's lowering commit can apply "with fuzz" into the wrong case.** The cases
  are long and similar. Read every fuzzed hunk.
- **Shadow boundary differs by design** between Eglantine (rounded down to 244) and Lichii (246).
  Different but equivalent output for R244/R245 operands is not a porting bug.
- **Don't edit the header while a `ninja` build of it is running.** The object can compile a
  half-edited state. Prepare edits in a scratch copy, or wait.

## Support files

- `scripts/relower_words.cpp`: lower `w0 w1 w2 w3` lines from stdin.
- `scripts/lower_dump.cpp`: lower every dumped program (geometry retried for `-gfx-`).
- `references/texture-operands.md`: Ampere TEX/TLD4 field and source-list layout, with Maxwell TLD4 mapping.
- `references/visual-bug-investigation.md`: lab_ctl workflow, image dumps, the A/B ladder and the
  bisection pitfalls for tints and missing effects.
- `scripts/lowered_for.cpp`: the Maxwell words one or more Ampere instructions of a dumped program
  lower to (via `source_instruction_offsets`), for checking one path instruction by instruction.
