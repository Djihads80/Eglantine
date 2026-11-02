# Getting started

## What you need

- Windows 10 or 11, 64-bit.
- A Vulkan GPU. 8 GB of video memory is the minimum; see [video memory](#video-memory).
- 16 GB of RAM.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64), if `eglantine.exe` does not start.
- **Your own** dump of Mario Kart World and **your own** `prod.keys`. Nothing of the kind is
  included in this repository or its releases.

## Dump layout

The emulator loads an extracted ("loose") ExeFS, not an NSP/XCI. The 1.8.0 update is a folder
named `917504` that contains everything needed:

```
mkw 1.8.0 update\
  updates\
    917504\
      exefs\
        main            <- the file you open in the emulator
        main.npdm, rtld, sdk, subsdk0, subsdk1
      romfs\            <- found automatically next to exefs
```

Optional files that make the game list and the in-game version correct (the dump has no
`control.nacp`, so without them the list shows the program id and the game shows "1.0.0"):

| File | Where | Effect |
| --- | --- | --- |
| `control.nacp` | `917504\exefs\` | Game name in the list, and the version the game displays |
| `icon.jpg` (any png/jpg/bmp) | `917504\exefs\` | Icon in the game list |
| `display_version.txt` containing `1.8.0` | `917504\` or `917504\exefs\` | Version the game displays, if there is no `control.nacp` |

## First launch

1. Put `prod.keys` in `%APPDATA%\eden\keys\`.
2. Start `eglantine.exe`.
3. *File > Load File*, pick `...\updates\917504\exefs\main`.

The first minutes of a new course compile shaders: short slowdowns are expected. Compiled shaders
are kept in `%APPDATA%\eden\cache\shader\0400c3f00006e000\`, so the next runs are smoother.

### Eglantine and Eden names

Since this fork is based on Eglantine, the program is `eglantine.exe` (it was `eden.exe` in the
earlier releases) and its window title starts with "Eglantine". The command-line build is still
`eden-cli.exe`, and the user folder is still Eden's: `%APPDATA%\eden` (keys, `config\qt-config.ini`,
saves, shader cache, `log\eden_log.txt`), or a `user` folder next to the executable if there is
one (portable install). Settings, saves and shader caches of the earlier releases are used as they
are.

## Recommended settings

*Emulation > Configure*:

| Setting | Value | Why |
| --- | --- | --- |
| Graphics API | Vulkan | The Switch 2 work targets Vulkan only |
| Memory layout | 12 GB | Switch 2 games plan ~9.5 GB (set automatically for Switch 2 titles) |
| Resolution | 1x | The game already renders and upscales on its own |
| GPU accuracy | **Low** | At High, the split GPU thread turns itself off: much slower |
| Asynchronous shaders | On | Fewer freezes when new shaders appear |
| ASTC recompression | **BC3** | See [video memory](#video-memory) |
| VRAM usage | Aggressive (16 GB cards) / Conservative (8 GB cards) | |
| Reactive flushing | Off | Costly in this game |
| VSync | FIFO Relaxed | Plain FIFO drops to 30 fps as soon as a frame misses 60 |

## Video memory

Switch 2 textures are ASTC-compressed. PC graphics cards cannot read ASTC, so the emulator converts
them. Without recompression they become RGBA8, 4 to 8 times larger, and Mario Kart World fills a
16 GB card within a couple of minutes. Textures that no longer fit are created empty (garbage or
black textures), then the emulator crashes.

Use **ASTC recompression: BC3**. The emulator also switches to BC3 on its own for Switch 2 games
on cards with less than 12 GB, and keeps its texture cache under the budget the driver really
gives it; Eglantine gives the large terrain textures memory only where the game writes them.
Details: [`NX2_VRAM.md`](../../NX2_VRAM.md).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Game list shows `0x400c3f00006e000` and no icon | No `control.nacp` in the dump | Add `control.nacp` and an icon to `exefs\` (see above) |
| Game shows version 1.0.0 although it is 1.8.0 | Same | Add `control.nacp` or `display_version.txt` |
| Stuck on "Launching" | An emulator without the 1.8.0 fixes (e.g. Takoroka's own release) | Use this fork's release (or a current Eglantine) |
| Garbage or black textures after a few minutes, then a crash | Video memory full | ASTC recompression BC3, then clear the shader cache |
| ~30 fps cap | VSync FIFO | VSync FIFO Relaxed or Off |
| Slow overall | GPU accuracy High | GPU accuracy Low |
| Emulator freezes on a still image, log shows `Native MME ... parameter FIFO underflow` | GPU macro desync in the split GPU thread | Start with the environment variable `EDEN_NX2_SPLIT=1` (safer, slightly slower) |
| Crash while the map loads (AMD) | The driver reports no sparse block size and older builds divided by it | Fixed in this release |
| Closing or stopping hangs | Older builds | Fixed in this release; otherwise end `eglantine.exe` in the Task Manager |

Clearing the shader cache: close the emulator and delete
`%APPDATA%\eden\cache\shader\0400c3f00006e000`.

When reporting a problem, attach `%APPDATA%\eden\log\eden_log.txt` (it is overwritten at each
launch: copy it before starting the emulator again) and a screenshot.
