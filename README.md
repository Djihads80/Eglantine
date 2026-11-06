<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200"></a>
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center"><b>Eglantine</b> is a free and opensource (FOSS) Switch 2 emulator, derived from Eden, Yuzu and Sudachi - started by developer Camille LaVey.
It's written in C++ with portability in mind, with builds for Windows, Linux and macOS
</h4>

<p align="center">
    </a>
    <a href="https://discord.gg/jCCZ58VExk">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

<p align="center">
  <a href="#about-this-fork">About this fork</a> |
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## About this fork

This repository is for personal experiments, fixes and other things that may or may not end up merged into [Eglantine](https://github.com/Djihads80/Eglantine). Nothing here is an official build, and anything not listed as merged can change or disappear.

### Changes to the engine

| Change | What it does | Status |
|--------|--------------|--------|
| **Keep Switch 2 pipelines built** (`vk_pipeline_cache`) | Switch 2 titles build every cached pipeline at boot and never evict them, instead of rebuilding evicted ones on the GPU thread mid-game. Switch 1 titles are unchanged. Only applies with the disk shader cache on. | Not merged. Branch [`fix/nx2-keep-switch2-pipelines`](https://github.com/Djihads80/Eglantine/compare/fix/nx2-keep-switch2-pipelines) exists upstream. |
| **Dynamic frame rate for Mario Kart World** (`nx2_gsv`, `game_presets`) | The game picks a 1- or 2-vblank frame mode from a frame time that already includes the interval it asked for, so holding a frame for 2 keeps it reading as too slow and it stays at 30 FPS for good. The recommended settings now turn its dynamic mode on, and `PaceToVblank` recognises the title through the application process: a present that arrives from a service context has no current game process, so the setting used to do nothing. | Not merged. Guarded on the loader patch: a title whose instruction sequence does not match is left unchanged, and every other title is unaffected. |
| **Cheaper ASTC decode and upload** (`astc_decoder.comp`, `texture_cache`, `vk_texture_cache`) | Only affects GPUs that cannot sample ASTC, which decode it on the host and re-compress it to BC. The decoder computes the weight scale and the partition hash once per block instead of once per texel and finds the colour range by bisection; its output is bit-identical to the old decoder on millions of random blocks of every footprint, compared on a GPU with a small harness. The decode and the BC encode are recorded in one submit instead of two, which removes the idle gap the GPU had between them (a few percent of GPU time on a low-end GPU). The temporary decode images go back to the pool as soon as the GPU is done with them, the oldest free one is evicted when the pool is full instead of dropping the newest, and a larger image can serve a smaller texture, so reuse rose from about 40% to about 70% of the uploads and the time spent recording an upload fell by around three quarters. The alpha scan reads the guest data in place instead of copying it first. Taken together, on a low-end GPU the CPU time spent on ASTC uploads roughly halved and long frames became less frequent; a capable GPU spends little time on ASTC to begin with, so the gain there is small. Each part can be turned off in `nx2_switches.txt` (`EDEN_NX2_ASTC_ONE_SUBMIT`, `EDEN_NX2_ASTC_SCRATCH_BUCKET`, `EDEN_NX2_ASTC_POOL_V2`, `EDEN_NX2_ASTC_POOL_FIT`). | Not merged. Not proposed upstream. Branch `perf/astc-decode-opts` on this fork. |
| **Photo mode in docked mode for Mario Kart World** (`kepler_compute`) | Docked, the photo mode renders at 2560x1440 and upscales to 3840x2160 with a network of tensor-core passes that cannot run, so the 4K image stayed empty and only the UI drawn over it showed. After the first rejected pass, the next dispatches are searched for a pass that has a block-linear colour image and one 1.5 times its size in its constant buffers, and its result is replaced by a bilinear blit of one to the other. The pass and the places of the two handles are remembered, so later frames read only those. Handheld mode does not use the network and is unchanged. `EDEN_NX2_PHOTO_UPSCALE=0` in `nx2_switches.txt` turns it off. | Not merged. Proposed upstream: [PR #676](https://github.com/Djihads80/Eglantine/pull/676) (open). Branch `fix/mkw-photo-mode-docked` on this fork. |

### Changes already in upstream

These are now in upstream. Unless a row says otherwise, this fork carries no extra code for them.

| Change | What it did | Status |
|--------|-------------|--------|
| **No task/mesh pipelines without `VK_EXT_mesh_shader`** (`vk_pipeline_cache`) | Returned no pipeline instead of building one the driver cannot use, so the draw is skipped as before. Aimed at `VkPipelineBuild` crashes on AMD (RADV) and on GPUs without the extension. | Merged upstream as [`b00481fcc6`](https://github.com/Djihads80/Eglantine/commit/b00481fcc6) (kraibse), the same 8 lines, cherry-picked from [PR #426](https://github.com/Djihads80/Eglantine/pull/426), which is still open for further AMD fixes. Testers on that PR reported the game starting on an iGPU and, on an RX 5700, the `VK_ERROR_INITIALIZATION_FAILED` pipeline failures in a race going from 24 to none. This fork's base does not have it yet and keeps its own identical commit until it is rebased. |
| **Release guest RAM when a Switch 2 game stops** (`core`) | Destroys the 14 GiB guest memory section when a Switch 2 game stops and creates it again for the next one, instead of keeping it (and clearing it again) until the app closes. In my tests, RAM went back to the base after each of three emulate/stop cycles; before it stayed at about half of the peak. Switch 1 titles are unchanged. Tested on one machine and one title. On Windows the first emulation after launching the app still uses more RAM than later ones; this was traced to Microsoft Defender scanning the new process (see Known issues). | Merged upstream: [PR #634](https://github.com/Djihads80/Eglantine/pull/634) (merge commit [`1c1835e8d1`](https://github.com/Djihads80/Eglantine/commit/1c1835e8d1)). This fork's base does not have the merge yet and keeps its own commit until it is rebased. |
| **Texture cache descriptor ids** (`texture_cache`) | Stored the view and sampler ids next to the descriptors, so a lookup per binding on each draw is an array read instead of a hash map lookup. Effect on performance not measured. | Merged upstream as [`863730d82e`](https://github.com/Djihads80/Eglantine/commit/863730d82e) (Yuri Olive), a separate implementation of the same idea. My own commit was dropped and this fork uses the upstream one. |
| **Revert of upstream `b547bda625`** (`texture_cache`) | Removed the `runtime.Finish()` that `UploadInChunks` runs between the chunks of a large image upload. On an AMD RX 9070 XT (driver 2.0.416, Mario Kart World) a build with it hit `VK_ERROR_DEVICE_LOST` about 20 s after the game started loading, and the same build without it did not. Bisected on upstream with one tester, one run per build; why it fails is not known. Without the wait, the staging memory of a large image builds up again, which is what the commit wanted to avoid. Other GPUs not tested. | Merged upstream as [`179b5afb50`](https://github.com/Djihads80/Eglantine/commit/179b5afb50) (Jonas), a revert of the same commit, in `build-203`. Its message cites upstream issues #496 and #499 and its own bisect. This fork's base is still on `build-194` and keeps its own identical revert until it is rebased. |
| **Black textures on AMD: revert of upstream `46d1554186`** (`vk_texture_cache`) | On AMD (tested on an RX 9070, Mario Kart World) the black textures from the menu on went away with "back a sparse tiled image whole when its tiles can't be set up" reverted. Found by bisecting upstream one build at a time, same settings each time, with a single tester. Why that commit does it is not known: its new branch never ran in any of the logs. This fork used to carry its own revert; it now uses the upstream one. Not retested on AMD since the move to build-187. | Merged upstream: [revert `62f2984b89`](https://github.com/Djihads80/Eglantine/commit/62f2984b89) (PR #458, in build-187). Original commit: [`46d1554186`](https://github.com/Djihads80/Eglantine/commit/46d1554186cdfd2255cc3745e3e2cfe9130c5a84). |

### Known issues

- **More RAM in the first emulation after opening the app (Windows).** The first Switch 2 game you start after launching Eglantine can use several GB more RAM than the ones started after it. The extra memory is guest RAM that is resident but almost entirely zeros. It is not written by the emulator or the game: it stays when the emulated CPU threads are suspended, and it fills the guest memory views in address order. In the tests it coincided with Microsoft Defender's real-time protection (`MsMpEng.exe`) using a full core while it scanned the new process, reading the guest memory views and making Windows back every page of them. A later emulation in the same process is not scanned, so it only grows as the game touches memory. Stopping the emulation frees it, and the next one starts with the lower usage. It is not a leak, and it is not caused by the guest RAM release change. Adding a Defender process exclusion for `eglantine.exe` removed the gap in testing. This lowers the protection for that program, so it is optional and on your own responsibility: from an elevated PowerShell, `Add-MpPreference -ExclusionProcess eglantine.exe`, and `Remove-MpPreference -ExclusionProcess eglantine.exe` to undo it. Other antivirus software was not tested. What is still not known is why Defender scans only the first emulation of a process, and whether delaying the creation of the guest memory until after the scan avoids it.

### Ideas

Future changes and optimisations, none of them implemented here yet.

- **Align the pacer to the vsync clock.** The present sleep and the vi2 vsync event are two free-running clocks at the same nominal rate; snapping the sleep to the tick removes up to one vblank of phase error per frame without changing the rate the game asked for, so no title's speed changes.
- **Model the console's present queue.** The compositor flips one frame per vblank and queues the rest, which is not the same as a blocking sleep per present. A bounded queue would let a game run at its real depth instead of stalling on every flip; it needs care, because a title that reuses its command memory once it runs ahead starts reading stale commands.
- **Make the emulated refresh rate a setting.** An emulated display refresh that both the vsync tick and the pacing follow, so frame-rate mods can target it. `notsapinho/Eglantine` ships it as `nx2_refresh_rate`.
- **Two frames ahead for 30 FPS titles.** Letting the game run two frames ahead of the GPU thread while it presents at two or more vblanks keeps long frames out of the present path, and backing off when a wait runs long keeps it from stalling. `notsapinho/Eglantine` ships it as `mkw_smooth`.
- **Raise the texture cache cap.** `MaxCriticalMemory` is a constant applied on every GPU and is re-applied after the budget computation, so a card with more memory than the cap evicts and re-decodes textures that would fit in it. The cap should follow the device budget instead; `mollie2999/Eglantine`'s `exp` branch raises it and collects against the expected threshold.
- **Batch fence and command-list submits.** Fences, command-list copies and `FlushCommands` each submit on their own today; deferring and batching them removes most of the queue submissions on fence-heavy titles.
- **Upload only the written rows of a texture.** A CPU write to a plain 2D image marks the whole image dirty and `RefreshContents` re-uploads every mip level; `PlanUnswizzleImageRows` already exists and is used only for full-range chunks.
- **Count uploads per layer of large layered textures.** A temporary per-image upload tally (not kept) showed no large texture being rewritten every frame: the only images uploaded on every frame were the small planes the video decoder writes, which is expected. Large layered textures are uploaded in bursts of several layers at once, and a per-image tally cannot say whether the same layers are uploaded again; a per-layer count would.
- **Capture a frame on the host GPU.** Nothing in the emulator says how the GPU time splits between draws, copies and decode work, and no CPU counter can; a RenderDoc or Nsight capture would.
- **Cut draw setup.** It is the largest single term on the split back thread, and the split document already names it as the next step.

## Compatibility

The emulator is capable of running Mario Kart World at decent/full speed depending on specs and configurations, and Splatoon Raiders at variable performances

A list of supported games will be available in future. Please be patient.

## Development

For development discussions, please join us on [Discord](https://discord.gg/jCCZ58VExk)

If you would like to contribute, we are open to new developers and pull requests. Please ensure that your work is of a high standard and properly documented. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

You can download the latest releases from [here](https://github.com/Djihads80/Eglantine/releases).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/jCCZ58VExk)

## Star History

<a href="https://www.star-history.com/?repos=djihads80%2Feglantine&type=date&legend=bottom-right">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&theme=dark&legend=bottom-right" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=djihads80/eglantine&type=date&legend=bottom-right" />
 </picture>
</a>

## License

Eglantine is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://github.com/Djihads80/Eglantine/blob/nx2/LICENSE.txt) file.
