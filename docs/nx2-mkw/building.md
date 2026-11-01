# Building on Windows

The release is built with **clang-cl** (Clang with the MSVC ABI), like Eden's own Windows CI.
On the test machine it ran clearly faster than the same code built with MSVC.

## Toolchain

| Tool | Version used | How to get it |
| --- | --- | --- |
| Visual Studio 2022 Build Tools | MSVC 14.44 | `winget install Microsoft.VisualStudio.2022.BuildTools`, workload *Desktop development with C++* plus *C++ CMake tools* |
| Clang for Windows | 19.1.5 | Visual Studio Installer, components `Microsoft.VisualStudio.Component.VC.Llvm.Clang` and `...Llvm.ClangToolset` |
| CMake, Ninja | 3.31 (ships with the Build Tools) | Included |
| Python | 3.12 | `winget install Python.Python.3.12` (needed to fetch dependencies) |
| Vulkan SDK | 1.4.341.1 | `tools/windows/install-vulkan-sdk.ps1` (run as administrator) |
| Qt | 6.8.3, `win64_msvc2022_64`, module `qtcharts` | `pip install aqtinstall`, then `python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtcharts -O C:\Qt` |

Why not the bundled Qt (`YUZU_USE_BUNDLED_QT=ON`, the default with MSVC): the bundled Qt 6.11 is
built with a newer MSVC (VS 2026) and fails to link with MSVC 14.44 (`__std_rotate`,
`__std_unique_4` unresolved). Qt 6.8.3 for MSVC 2022 links fine.

## Build

From a *x64 Native Tools* prompt (or after `vcvars64.bat`), with the Vulkan SDK, Qt and Clang on
the `PATH`:

```bat
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;C:\VulkanSDK\1.4.341.1\Bin;C:\Qt\6.8.3\msvc2022_64\bin;%PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.341.1"

cmake -S . -B build/clang -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=ON -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF ^
  -DYUZU_USE_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 ^
  -DYUZU_USE_BUNDLED_SDL3=ON -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF -DENABLE_WERROR=OFF

cmake --build build/clang --target yuzu yuzu-cmd --parallel 16

windeployqt --release --no-translations --no-compiler-runtime build\clang\bin\eglantine.exe
```

Output: `build\clang\bin\eglantine.exe` (Qt frontend, target `yuzu`) and `eden-cli.exe` (target
`yuzu-cmd`). Eglantine renamed only the Qt frontend: earlier releases of this fork had `eden.exe`.

Notes:

- `--parallel`: one job per hardware thread. 16 jobs used about 2 GB of RAM with clang-cl.
- A first build takes 40-60 minutes; later builds only recompile what changed. Headers included
  everywhere (for example `src/core/hle/service/nx2_gsv.h`) trigger long rebuilds.
- Do not run the emulator while building: both reserve a lot of memory and the build can fail
  with "the paging file is too small".
- `eglantine.exe` cannot be replaced while it is running: the link step then fails with
  "permission denied".
- In a `git worktree` checkout, `.git` is a file and the configure step stops with "file COPY
  cannot copy file .../hooks/pre-commit": rename `hooks\pre-commit` for the duration of the
  `cmake -S` step, then rename it back.

## Building with MSVC instead

Same commands without the two `CMAKE_*_COMPILER` options (build directory `build/release` for
example). MSVC treats some warnings as errors in this code base; Eglantine already has the fixes
(`static_cast<u32>` on boolean comparisons in
`src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`, `static_cast<u64>` instead of a braced
narrowing conversion in `src/video_core/texture_cache/util.cpp`). Releases use clang-cl.

## Packaging a release

The release zip contains `build\clang\bin` after `windeployqt` (exe files, Qt DLLs and plugin
folders), `LICENSE.txt` and a README. It must never contain keys, firmware, game files, a user's
config (`qt-config.ini`) or save data.

## Linux build on GitHub

Eglantine's workflow `.github/workflows/release-binaries.yml` builds a Linux AppImage (Ubuntu 24.04,
Qt 6.8.3; runs on glibc 2.39 or newer) and a Windows zip on GitHub's machines. On the fork, start it
by hand from the Actions tab or with
`gh workflow run release-binaries.yml -R Wael3rd/Eglantine-MKW-merge --ref mkw-eglantine -f publish_release=false`;
the files are then artifacts of the run (kept 7 days), and `publish_release=true` would publish them
as a pre-release instead. The Linux build is not tested here: it is built, not run.

## Updating from upstream

Branch `mkw-eglantine` is Eglantine's `nx2` with the commits of [changes.md](changes.md) on top.
To move it to a newer Eglantine:

```bat
git remote add eglantine https://github.com/Djihads80/Eglantine.git
git fetch eglantine nx2
git switch mkw-eglantine
git rebase eglantine/nx2
```

(`git remote add` only the first time.)

- A commit Eglantine has merged meanwhile (for example its PR #28, "nx2: execute command lists as
  submitted") is dropped by the rebase, or stops it as an empty commit: `git rebase --skip`.
- On a conflict, keep Eglantine's version of its own code and put only our change back on top.
  Then `git log --oneline eglantine/nx2..mkw-eglantine` must list the commits of changes.md:
  update that page when the list changes.
- Rebuild (see above), then retest before a release: Mario Kart World 1.8.0 boots past
  "Launching"; a race with an empty shader cache has slowdowns, not freezes; a few minutes of play
  without garbage textures (on an 8 GB card, or with `EDEN_NX2_VRAM_LIMIT_MIB=7168`); stopping and
  closing do not hang.
