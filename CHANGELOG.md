# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

## [0.2.0] - 2026-11-08

> [!WARNING]
> **Experimental & Not Fully Tested:** This build sits **~500 commits ahead** of upstream `nx2-experimental`. Features, GPU rewrites, and speed optimizations land immediately after local testing. **Back up your saves** (`%APPDATA%\eglantine\nand\user\save` on Windows) before launching!

### ✨ Highlights in This Build
- **NVIDIA DLSS Super Resolution & DLAA (Windows):**
  - Native Streamline DLSS (`Performance` / `Balanced` / `Quality`) and `DLAA` selector in `Emulation -> Configure -> Graphics`.
  - Ticking **Enable NVIDIA DLSS** automatically locks **Window Adapting Filter** to `Bilinear` and **Anti-Aliasing** to `None` so post-processing never blurs the DLSS output.
  - Automatic 3D pass upscaling before HUD compositing in *Ocarina of Time* (sharp 1440p 3D + native crisp HUD).
- **2x Frame Generation + High-Refresh VRR Pacing:**
  - Built-in FSR3 Frame Generation with half-period presentation pacing (`~40 FPS` guest -> `~80 FPS` displayed on VRR/G-Sync displays) and duplicate-frame skip when guest frames pause.
- **Ocarina of Time Overhaul (`0400486024DEC000`):**
  - Fixed terrain blotches, haze/fog rendering, Turing MME macro crashes, and inventory/save-screen thumbnail capture.
  - Real-time game speed below 60 FPS (no more slow-motion when below 60 FPS).
- **Major Stutter & FPS Improvements (All Platforms):**
  - **Shader Stutter Eliminator:** Spreads background pipeline compilation across frames (cuts worst new-area freezes from **2.3 s → 0.6 s**).
  - **CPU/GPU Hot Paths:** Trap-and-flush CPU reads, fast 4 KiB page-run lookups, SPSC GPU thread wakeups, and AMD X3D V-Cache CCD thread affinity.
  - **Mario Kart World & Super Mario Bros. Wonder:** Large-VRAM budget fix (**53 → 58.7 FPS** in MKW) and `vi2` release fence fix (unlocks Wonder past 30 FPS).

## [0.1.0]

- Initial release candidate
