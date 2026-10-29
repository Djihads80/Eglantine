# Repository context for agents and newcomers

This is the operational half of the agent instructions. The normative half is
[docs/policies/AI.md](../policies/AI.md) (AI usage, disclosure, hard limits) and
[docs/policies/Coding.md](../policies/Coding.md) (style). `AGENTS.md` at the root
points at the policy and is upstream-owned: **do not extend it, extend this file and
the policy instead.**

## What this tree is

Eglantine: a Nintendo Switch 2 emulator derived from Eden, itself derived from yuzu
and Sudachi. C++20, CMake + Ninja, dependencies via CPM (`cpmfile.json`) with local
dependency patches under `.patch/`.

**`master` is the feature-safe trunk.** It carries Eden's history plus local
Eglantine commits. A fresh clone of `master` must configure and build on x86_64 Linux
and Windows; work that cannot hold that line lives on `int/<area>`.

The tree still carries yuzu-era names (`project(yuzu)`, `src/yuzu/`,
`org.yuzu.yuzu_emu`, `yuzu-cmd`). They are being renamed deliberately, not by
drive-by edits.

## Build and test

```sh
tools/agents/setup.sh                  # once per clone: hooks, worktree root
cmake --preset linux-gcc-dev           # or linux-clang-dev / windows-msvc-dev
cmake --build --preset linux-gcc-dev --target yuzu yuzu-cmd tests
ctest --preset linux-gcc-dev
```

- CMake **target** names are `yuzu`, `yuzu-cmd`, `tests`, `yuzu-room`. `eden`,
  `eden-cli` and `eden-room` are output names only: `--target eden` fails.
- Warnings are errors on GCC and Clang, and disabled on MSVC. That asymmetry is why
  the gate has both platforms; it is not redundancy.
- The suite does not cover rendering, is **not green on trunk** (nine known failures)
  and hangs intermittently. `tools/agents/known-failures.txt` records the baseline;
  the gate fails only on failures outside it. For a renderer change the only real
  verification is a run log with a title.
- `tools/agents/verify.sh` is the single accepted definition of "it works".

Details per platform, including the presets, the dead flags in `.ci/*/build.sh` and the
conda-poisoning symptom: [build.md](build.md).

## How work enters this repository

```sh
tools/agents/branch.sh <issue> <slug> --area <area>   # branch + worktree
tools/agents/claim.sh                                 # claim files on the issue
tools/agents/verify.sh                                # the gate
tools/agents/status.sh                                # what is in flight
```

- Branch names come from `branch.sh` only. `verify.sh` refuses to run on a hand-named
  branch, so a hand-named branch cannot produce an accepted result.
- One branch = one owner, and one record: an issue, or the PR itself when the branch
  carries issue 0. Legacy-named branches are frozen: to add
  commits, cut a conforming branch from trunk for the issue.
- One worktree and one build dir per branch (`.worktrees/<branch>`,
  `build/<preset>`). Compiles are serialised machine-wide by `flock`.
- `exp/` branches expire; the issue carries a `decide by` date.

Grammar, lifecycle and the migration rule for the 164 legacy branches:
[branching.md](branching.md).

## Cleanliness rules the gate can see

- Every probe, debug toggle or diagnostic carries `// TEMP(<issue>): <what deletes
  it>`. `verify.sh` lists what a change adds, and the PR asks which issue removes it.
  `HANDOFF.md` is allowed only on `exp/` and `ai/` branches and must be deleted before
  merge.
- Three lanes for any new toggle, decided before code: **permanent setting**
  (documented, bounded, sane default) / **debug knob** (`debug_knobs` bitmask, zero
  frontend churn) / **temporary toggle** (TEMP + cleanup issue). A permanent setting
  touches 6-14 files (see `docs/user/AddingBooleanToggles.md`); most experiments belong
  in the other two lanes.
- Registries are **append-only**: new settings go at the end of their category block in
  `src/common/settings.h`; never reorder or reformat a registry in a feature change.
- Prefer a new slice file plus one registration line over editing a hot monolith.
  [hot-files.md](hot-files.md) is generated from history and live branches.

## Where the work currently stops

- The host's default branch was a feature branch 459 commits behind `master` until
  2026-10-29, when it was flipped to `master` and PR #1 (the experimental NS2 import)
  merged. Remaining owner actions: enable Actions on the host, and add a ruleset
  protecting `master` and `int/*` with the gate's checks once the workflow exists.
- The branch population is still legacy-named nearly end to end (53 of 53 active at the
  last measurement). That is what `branch.sh` and the weekly audit exist to drain.
- The Vulkan work is split across competing branch families (`v2` vs `V3`,
  `dynamic-rendering-2` vs `tiled-gpu-v2`) that share up to 37 files. The answer is an
  `int/vulkan` integration branch and an issue per family, not a rebase.

## Pointers

| File | Owns |
|---|---|
| `AGENTS.md` | upstream-owned policy pointer; leave it alone |
| `docs/policies/AI.md` | AI policy, including the Eglantine agent addendum |
| `docs/policies/Coding.md` | code style |
| `docs/agents/build.md` | per-platform build truth |
| `docs/agents/branching.md` | branch grammar and claims |
| `docs/agents/providers.md` | which harness may do what |
| `docs/agents/disclosure.md` | disclosure forms |
| `docs/agents/hot-files.md` | generated collision map |
| `docs/agents/work-in-flight.md` | generated branch/issue table |
| `.claude/skills/` | vendored project skills |
