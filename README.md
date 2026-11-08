<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine on Crack" width="180">
  <br>
  <b>Eglantine on Crack</b>
</h1>

<p align="center">
  <i>An ultra-bleeding-edge Switch 2 emulator fork — hundreds of commits ahead of upstream Eglantine.</i>
</p>

<p align="center">
  <a href="https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases"><b>⬇ Download Latest Build</b></a> ·
  <a href="#what-makes-this-different">What's Inside</a> ·
  <a href="#quick-start">Quick Start</a> ·
  <a href="#if-something-breaks">Troubleshooting</a> ·
  <a href="https://github.com/A-Man-With-A-Plan-dev/Eglantine/issues">Report a Bug</a>
</p>

---

> [!WARNING]
> **Bleeding-Edge & Not Fully Tested**
> This build sits **~500 commits ahead** of upstream `nx2-experimental`. Fixes, GPU rewrites, and speed hacks land here the moment they work in local testing—long before full multi-game or multi-hardware QA.
> - **Tested primarily on a single rig** (Windows 11 · Ryzen 9 9950X3D · RTX 5090).
> - **AI-assisted development** (Claude / Gemini, disclosed via `Assisted-by:` commit trailers) is used heavily to move fast.
> - **Back up your saves** before jumping in. Expect rough edges!

---

## What Makes This Different?

**Eglantine on Crack** includes **100% of upstream Eglantine** plus hundreds of aggressive performance, rendering, and stability patches gathered from across the community and built in-house:

| Feature / Fix | What It Does |
|---|---|
| **NVIDIA DLSS & DLAA** | Native Streamline DLSS Super Resolution (`Performance` / `Balanced` / `Quality`) and `DLAA` with automatic 3D pass detection and zero-blur HUD compositing |
| **2x Frame Generation** | Built-in FSR3 Frame Generation + high-refresh VRR frame pacing to push ~40 FPS gameplay to 80+ displayed FPS |
| **Ocarina of Time Overhaul** | Fixed terrain blotches, haze/fog, MME macro crashes, thumbnail capture, and real-time game speed below 60 FPS ([OOT_STATE.md](OOT_STATE.md)) |
| **Shader Stutter Eliminator** | Spreads background pipeline compilation across frames (cuts worst new-area freezes from **2.3 s → 0.6 s**) |
| **CPU & GPU Hot-Path Speedups** | Trap-and-flush CPU reads, fast 4 KiB page-run lookups, SPSC GPU thread wakeups, and AMD X3D V-Cache CCD pinning |
| **Mario Kart World & Wonder Fixes** | Large-VRAM budget fix (**53 → 58.7 FPS** in MKW) and `vi2` release fence fix (unlocks Wonder past 30 FPS) |

---

## Quick Start

Your existing Eglantine setup carries over automatically:

1. **Back up your saves** (`%APPDATA%\eglantine\nand\user\save` on Windows).
2. **Download & extract** the latest release from **[Releases](https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases)** into its **own new folder** (do not overwrite your upstream folder).
3. **Run `eglantine.exe`** — it shares `%APPDATA%\eglantine`, so your keys, firmware, games, mods, and saves work out of the box.

> [!TIP]
> **Using NVIDIA DLSS:** Go to `Emulation -> Configure -> Graphics`, tick **Enable NVIDIA DLSS**, and pick your **NVIDIA DLSS Quality** (`Performance`, `Balanced`, `Quality`, or `DLAA`). Window Adapting Filter (`Bilinear`) and Anti-Aliasing (`None`) lock automatically so post-processing never blurs the DLSS output.

---

## If Something Breaks

Every performance optimization in this fork is guarded by a runtime switch in `src/video_core/nx2_perf.h`:

- **Disable all experimental speedups at once:** Launch with environment variable `EDEN_NX2_PERF_OPT=0`.
- **Switch back anytime:** Just launch your original upstream `eglantine.exe` (only the per-game shader cache will rebuild).
- **Report issues:** Open an issue **[on this fork](https://github.com/A-Man-With-A-Plan-dev/Eglantine/issues)** (never on upstream!) with your hardware specs and `%APPDATA%\eglantine\log\eden_log.txt`.

---

## Build From Source & Docs

```bash
git clone --recursive https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
```

- **Build Guide:** [docs/Build.md](docs/Build.md)
- **Ocarina of Time Status & Benchmarks:** [OOT_STATE.md](OOT_STATE.md) · [OOT_CONTRIBUTING.md](OOT_CONTRIBUTING.md)
- **Upstream PR Candidates & Credits:** [UPSTREAM_CANDIDATES.md](UPSTREAM_CANDIDATES.md) · [CREDITS_OOT.md](CREDITS_OOT.md)
- **Policies:** [AI Policy](docs/policies/AI.md) · [Coding Policy](docs/policies/Coding.md) · [GPLv3 License](LICENSE.txt)
