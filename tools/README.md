# Tools

Tools for Eden and other subprojects. When adding new scripts please use `#!/bin/sh -e` or `#!/usr/bin/env <interpreter>` (for `.py`, `.rb`, or `.perl`). Keep scripts POSIX compliant (i.e not require hard `bash` to run, just plain old `sh`).

## Third-Party

- [CPMUtil Scripts](./cpm)
    * `cpm/update.sh`: Updates CPMUtil, its docs and related tools from the latest release.

## Binaries

- `maxwell-spirv`: Converts Maxwell shaders (dumped from `.ash` files) into SPIR-V code (emitted into STDOUT).
- `maxwell-disas`: Dumb raw Maxwell disassembler.
- `maxwell-ir`: Dump generated IR of Maxwell shaders.

## Scripts

- `generate_converters.py`: Generates converters for given formats of textures (C++ helper).
- `svc_generator.py`: Generates the files `src/core/hle/kernel/svc.cpp` and `src/core/hle/kernel/svc.h` based off prototypes.
- `VectorDrawable2Svg.py`: Converts Android vector drawables into SVG.
- `shellcheck.sh`: Ensure POSIX compliance (and syntax sanity) for all tools in this directory and subdirectories.
- `llvmpipe-run.sh`: Sets environment variables needed to run any command (or Eden) with llvmpipe.
- `optimize-assets.sh`: Optimizes PNG assets with OptiPng.
- `update-icons.sh`: Rebuild all icons (macOS, Windows, bitmaps) based on the master SVG file (`dist/dev.eden_emu.eden.svg`)
    * Also optimizes the master SVG
    * Requires: `png2icns` (libicns), ImageMagick, [`svgo`](https://github.com/svg/svgo)
- `dtrace-tool.pl`: Runs dtrace sampling over the program (requires Flamegraph).
- `lanczos-gen.pl`: Generates constants for the Lanczos filter.
- `clang-format.sh`: Runs `clang-format` on the entire codebase.
    * Requires: clang
- `cpp-lint.sh`: Homemade dumb C++ linter.
- `fuzzsettings.cpp`: Fuzz settings files.
- `integration/game_discovery_test.cpp`: Standalone test for the game list discovery code in `qt_common`.

## nx2

Helpers for poking at NX2 binaries. All of them only read the files they are given.

- `nx2/a64_coverage.py`: Reports which AArch64 encodings in NSO `.text` segments dynarmic does not decode.
- `nx2/gdbrsp.py`: Minimal GDB remote-serial-protocol client for the gdbstub (AArch64 guests).
- `nx2/npdm_info.py`: Prints the META header and kernel capabilities of a `main.npdm`.
- `nx2/nso_dis.py`: Disassembles a range of an NSO's `.text` using clang and llvm-objdump.
- `nx2/nso_syms.py`: Symbol lookup for an NSO (nearest export, GOT slot, or name search).

## Windows

- `windows/install-vulkan-sdk.sh`, `windows/install-vulkan-sdk.ps1`: Installs the Vulkan SDK.
- `windows/load-msvc-env.sh`, `windows/load-msvc-env.ps1`: Loads the MSVC environment.
- `windows/vcvarsall.sh`: Vendored bash implementation of `vcvarsall.bat`, used by `load-msvc-env.sh`.

## CI

Used by the workflows in `.github/workflows`. See [CI docs](../docs/CI.md).

- `ci/prepare.sh`: Decides what a run is and emits the build/release parameters.
- `ci/release-notes.sh`: Generates the release notes for a tag.
- `ci/changelog-section.sh`: Prints the body of one `CHANGELOG.md` section.
- `ci/bump-version.sh`: Bumps `VERSION` and `CHANGELOG.md` for a new release.
- `ci/slugify.sh`: Returns the short slug shared by branch release and cleanup jobs.

## Android

It's recommended to run these scripts after almost any Android change, as they are relatively fast and important both for APK bloat and CI.

- `unused-strings.sh`: Finds unused strings in `strings.xml` files.
- `stale-translations.sh`: Finds translated strings that aren't present in the source `strings.xml` file.

## Translations

- [Translation Scripts](./translations)
    * `translations/qt-source.sh`: Runs `lupdate` over the Qt frontend to update `dist/languages/en.ts`.
    * `translations/update-translations.sh`: Pulls the translation bundles from Transifex.
