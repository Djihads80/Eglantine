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
| **Texture cache descriptor ids** (`texture_cache`) | Stores the view and sampler ids next to the descriptors, so a lookup per binding on each draw is an array read instead of a hash map lookup. Effect on performance not measured. | Not merged. Not planned: the devs have not confirmed it. |
| **Keep Switch 2 pipelines built** (`vk_pipeline_cache`) | Switch 2 titles build every cached pipeline at boot and never evict them, instead of rebuilding evicted ones on the GPU thread mid-game. Switch 1 titles are unchanged. Only applies with the disk shader cache on. | Not merged. Branch [`fix/nx2-keep-switch2-pipelines`](https://github.com/Djihads80/Eglantine/compare/fix/nx2-keep-switch2-pipelines) exists upstream. |
| **No task/mesh pipelines without `VK_EXT_mesh_shader`** (`vk_pipeline_cache`) | Returns no pipeline instead of building one the driver cannot use, so the draw is skipped as before. Aimed at a `VkPipelineBuild` crash seen on RADV (Vega). Not confirmed by a tester yet. | Not merged. A dev said it is planned, but nothing is on the main repo yet. |
| **Release guest RAM when a Switch 2 game stops** (`core`) | Destroys the 14 GiB guest memory section when a Switch 2 game stops and creates it again for the next one, instead of keeping it (and clearing it again) until the app closes. In my tests, RAM went back to the base after each of three emulate/stop cycles; before it stayed at about half of the peak. Switch 1 titles are unchanged. Tested on one machine and one title. The first emulation after launching the app still uses 3 to 6 GB more than later ones, and the cause is not known. | Not merged. Not proposed upstream yet. Branch [`feat/release-guest-memory`](https://github.com/SmileyTatsu/Eglantine/tree/feat/release-guest-memory) on this fork. |

### Known issues

- **More RAM in the first emulation after opening the app.** The first Switch 2 game you start after launching Eglantine can use 3 to 6 GB more RAM than the ones started after it (about 22 to 28 GB against 19 to 22 GB of total PC RAM in my tests). The extra memory is guest memory that the emulated CPU touches, and it stayed flat during the minutes I measured. Stopping the emulation frees it, and the next emulation starts with the lower usage, so stopping the game and starting it again brings the RAM down. The cause is not known yet. It is not a leak, and it is not caused by the RAM release change above: the build without that change used as much on its first run.

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
