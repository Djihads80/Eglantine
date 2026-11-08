<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

> [!IMPORTANT]
> **This fork: The Legend of Zelda: Ocarina of Time (Switch 2) is playable here.**
> It includes upstream Eglantine's `nx2-experimental` and adds fixes on top: no crashes in
> Kokiri Forest, no brightness flashes, correct fog, fixed save thumbnails, much shorter shader
> stutter and normal game speed when the frame rate dips.
> Not 60 FPS yet: about 39-41 FPS at the Kokiri Forest spawn point, on an RTX 5090.
> The goal is a locked 60 FPS, and help is welcome.
>
> - **[Switching from upstream Eglantine](#switching-to-this-fork)**: download, your data, going back
> - [Current state: numbers, shader stutter, known issues](OOT_STATE.md)
> - [How we got here: the method in one page](OOT_WORKFLOW.md)
> - [Help wanted: open tasks, easiest first](OOT_HELP_WANTED.md)
> - [Contribute: build, measure, send a change](OOT_CONTRIBUTING.md)
> - [Credits](CREDITS_OOT.md)

> [!WARNING]
> **This is a RESULT FIRST fork.** Changes land here once they work in game, measured and tested, before anyone else has reviewed the code. Much of it is AI-assisted and has not been peer-reviewed. Expect rough edges, and don't take a change here as accepted by upstream Eglantine. Code meant for upstream goes through its normal review there.


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
    <a href="https://discord.gg/XkS5NVPTUY">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

The emulator is capable of running Mario Kart World at decent/full speed depending on specs and configurations, and Splatoon Raiders at variable performances

A list of supported games will be available in future. Please be patient.

## Development

For development discussions, please join us on [Discord](https://discord.gg/XkS5NVPTUY)

If you would like to contribute, we are open to new developers and pull requests. Please ensure that your work is of a high standard and properly documented. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

Builds of this fork: [Releases](https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases).
Upstream Eglantine's builds: [here](https://github.com/Djihads80/Eglantine/releases).

## Switching to this fork

**To play:**

1. Download the latest build from [Releases](https://github.com/A-Man-With-A-Plan-dev/Eglantine/releases)
   and unzip it into its own folder. Don't unzip it over your upstream Eglantine folder.
2. Run `eglantine.exe`. It uses the same user folder as upstream Eglantine
   (`%APPDATA%\eglantine` on Windows), so your keys, firmware, games list, saves, mods and settings
   are already there. Nothing needs to be copied. Back up your saves (`nand\user\save`) first anyway.
3. The first time you play each game, its shader cache is rebuilt once. Expect some stutter on
   that first visit to each area.

**Going back** is the same: run upstream's `eglantine.exe`. Each time you switch builds the shader
cache of the game you play is rebuilt, because the two builds store it in different formats. If you
want to keep both builds side by side, make one of them portable: create an empty folder named
`user` next to its `eglantine.exe` and copy `keys`, `nand`, `load` and `sdmc` into it from
`%APPDATA%\eglantine`. That build then keeps its own settings, saves and cache in the `user`
folder. Don't copy `config`: it can still point at the folders in `%APPDATA%\eglantine`, so set
the settings up again instead.

**To build from source:**

```
git clone --recursive https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
```

Or add the fork to an existing upstream clone:

```
git remote add fork https://github.com/A-Man-With-A-Plan-dev/Eglantine.git
git fetch fork
git switch -c fork-nx2 --track fork/nx2
git submodule update --init --recursive
```

The build steps are the same as upstream's: see the [General Build Guide](docs/Build.md).

**Reporting problems:** open an [issue here](https://github.com/A-Man-With-A-Plan-dev/Eglantine/issues),
not on upstream, and say that you use this fork. Include your CPU, GPU, the game, where in the game,
and `%APPDATA%\eglantine\log\eden_log.txt` from that session. If a game runs worse here than on
upstream, also try launching it with `EDEN_NX2_PERF_OPT=0` (see [OOT_STATE.md](OOT_STATE.md)).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/XkS5NVPTUY)

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
