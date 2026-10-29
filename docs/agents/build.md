# Building Eglantine: Linux and Windows

`master` is feature-safe only if both platforms build it. This page is the
authoritative list of how each one is built here, what the gate runs, and which
flags are real.

## The three tiers

| Tier | Presets | Who runs it | What it proves |
|---|---|---|---|
| Dev | `linux-gcc-dev`, `linux-clang-dev`, `windows-msvc-dev`, `windows-clang-cl-dev` | humans and agents on their own machine | your change compiles, links, and the suite passes, with the real Qt frontend |
| Gate (`ci-core`) | `linux-ci-core`, `linux-clang-ci`, `windows-ci-core` | `gate/linux-gcc`, `gate/linux-clang`, `gate/windows-msvc` | the tree compiles on both platforms and the suite passes, dependency-light (no Qt) |
| Release parity | `linux-release-parity`, `windows-release-parity` | nightly only | the flags in `.ci/{linux,windows}/build.sh` still produce a build |

The gate deliberately skips Qt: installing Qt on three runners costs more than
it catches, and Qt-frontend breakage is caught by the nightly parity build. Core,
`video_core`, `shader_recompiler`, `audio_core` and the CLI are all in the gate,
which is where nearly all breakage happens.

## Commands

```sh
tools/agents/setup.sh
cmake --preset linux-gcc-dev
cmake --build --preset linux-gcc-dev --target yuzu yuzu-cmd tests
ctest --preset linux-gcc-dev
```

`cmake --build --preset <p> -- -k 0` collects every failure in one pass instead of
dying on the first file — worth it on a tree this size.

## Platform notes

### Linux

- System Qt 6, system SDL2 (`YUZU_USE_EXTERNAL_SDL2=ON`), OpenGL enabled.
- Dependencies: see `docs/Deps.md`.
- `linux-release-parity` pins `-march=x86-64-v3 -O3` because that is what
  `.ci/linux/build.sh` ships as its `amd64` target. Do not put arch flags in a
  dev preset; portability bugs then only appear on someone else's CPU.

### Windows

> **Nothing in this section has been verified in this fork.** It documents how the presets
> and helper scripts are wired, read from the tree itself. No Windows machine was available,
> so `windows-msvc-dev`, `windows-clang-cl-dev` and `windows-ci-core` are untested here:
> treat any Windows result as unproven until someone runs one.

- MSVC is the default and the gated one; `clang-cl` is supported as a second
  opinion (`windows-clang-cl-dev`) and can be selected in CI later.
- Set up the environment first: `tools/windows/load-msvc-env.ps1` (PowerShell) or
  `tools/windows/vcvarsall.sh` (Git Bash); in Actions that is `ilammy/msvc-dev-cmd`.
- Install the Vulkan SDK once: `tools/windows/install-vulkan-sdk.ps1`.
- The `windows-*` presets build under `C:/b/<preset>` on purpose: build paths
  under a deep `C:\Users\...\clone` plus a long preset name blow past MAX_PATH.
- MSVC builds download bundled Qt and SDL2 (`YUZU_USE_BUNDLED_QT=ON`,
  `YUZU_USE_BUNDLED_SDL2=ON`). That is also why the Windows gate needs no Qt
  install.
- `windeployqt` is needed only for packaging (`.ci/windows/build.sh`), never for a
  dev or gate build.
- MinGW/MSYS2 works but is the community path, not the gated one.

## Flags that do not exist

`.ci/linux/build.sh` and `.ci/windows/build.sh` pass `-DENABLE_LTO=ON`,
`-DYUZU_USE_FASTER_LD=ON` and `-DYUZU_USE_PRECOMPILED_HEADERS=OFF`. **None of
those options exist anywhere in this CMake tree** (`CMakeLists.txt`,
`CMakeModules/`, `src/`): grep finds them only in the scripts themselves, so
CMake silently ignores them and prints nothing. The release-parity presets
therefore mirror the *effective* flags, not the written ones. Either implement
those options properly in a real PR or delete the dead flags from the scripts —
do not copy them into anything new.

## Warnings are asymmetric

- GCC and Clang: `-Werror=all`, `-Werror=extra`, `-Werror=shadow`,
  `-Werror=unused`, `-Werror=missing-declarations`, `-Werror=conversion` (see
  `src/CMakeLists.txt` and the per-library `CMakeLists.txt`). Always on; there is
  no `ENABLE_WERROR` switch in this tree.
- MSVC: warnings-as-errors is explicitly disabled (`if (MSVC AND NOT CXX_CLANG)`).

Consequence: a Linux-green change can still fail to compile on Windows
(POSIX-only calls, missing includes, `ssize_t`), and a Windows-green change can
be rejected by Linux for a warning. Both legs are load-bearing.

## Verification beyond compiling

- `ctest --preset <p>` runs Catch2: `common`, `core`, `input_common`, and
  `video_core/memory_tracker` only. Rendering, HLE services and the frontends have
  **no** automated coverage.
- **Measured state of master (2026-10-29, ci-core preset, GCC):** 71 test cases,
  62 pass, 9 fail —
  `MemoryTracker: {Cached write downloads, FlushCachedWrites batching, Out of bound
  ranges 1-3, Rasterizer counting, Small region, Sparse regions 2}` and
  `CoreTiming[BasicOrder]`. The memory-tracker cases assert on
  `rasterizer.Count()` and get `0`, i.e. the tracker is never populated in this
  configuration; nobody has diagnosed it. The suite additionally **hangs
  intermittently** (one hang in two consecutive runs; a clean run takes about 7 s).
- That is why `tools/agents/known-failures.txt` exists: the gate compares failing
  case names against it and fails only on *new* ones, and it runs the binary under
  `timeout` so a hang is reported instead of stalling a job. Passing a flaky entry
  is expected and marked `[flaky]` in that file.
- Do not "fix" the gate by adding lines to that baseline. It follows the same rule
  as a `TEMP()` marker: an issue, a plan, and a removal.
- For a renderer or HLE change, capture a run log
  (`./build/<preset>/bin/eden-cli -g <title>/exefs/main > run.log 2>&1`) and read
  it. That log is the evidence a reviewer will ask for.
- `tools/agents/verify.sh` runs build + tests + license headers + spelling +
  TEMP audit. Nothing else counts as "it works".

## Troubleshooting

| Symptom | Cause |
|---|---|
| Configure fails after switching presets | stale build dir; presets use `build/<preset>` so this is rare — delete that one dir |
| Link errors mentioning Qt | dev preset with no system Qt installed; use `linux-ci-core` to check whether your change is the problem |
| Two builds, odd errors, 100% CPU | another agent is compiling; `verify.sh` serialises with `flock`, hand-rolled `ninja` does not |
| `--target eden` fails | `eden` is the output name; the target is `yuzu` |
| Configure warns "Cannot generate a safe runtime search path … hidden by files in /opt/miniconda3/lib", and the link later fails with `libglib-2.0.so.0: undefined reference to pcre2_*@PCRE2_10.47` | conda was on `PATH` **when CMake configured**: it resolves `fmt`, `zstd` and `ICU` from `/opt/miniconda3`, emits `-Wl,-rpath,/opt/miniconda3/lib`, and the linker then takes conda's `libpcre2` (0.14.0) for system glib 2.0, which needs `PCRE2_10.47`. Configure still succeeds, so the failure only shows up at link time. Fix: deactivate conda, `rm build/<preset>/CMakeCache.txt`, reconfigure |
| Windows configure slow / path errors | build dir outside `C:/b`, or MAKEPATH length; check the preset is the one in use |
