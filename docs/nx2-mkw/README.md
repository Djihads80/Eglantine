# Eglantine for Mario Kart World: documentation

This repository is a fork of the [Eglantine](https://github.com/Djihads80/Eglantine) project's
`nx2` branch (experimental Nintendo Switch 2 emulation on top of the Eden emulator, which now
includes Takoroka's Switch 2 work), aimed at one game: **Mario Kart World 1.8.0** (program id
`0400C3F00006E000`). Our changes sit on top of `nx2` as separate commits, so that a newer
Eglantine is a rebase away and each change can be proposed to Eglantine on its own.

Everything specific to this fork is documented here. The emulator's own documentation is in
[`docs/`](../README.md), and the engineering log of the Switch 2 work (Takoroka's, continued in
Eglantine) is [`NX2_NOTES.md`](../../NX2_NOTES.md) (one row per problem solved).

## Pages

| Page | For whom | What it covers |
| --- | --- | --- |
| [Getting started](getting-started.md) | Players | Dump layout, keys, first launch, recommended settings, troubleshooting |
| [Settings reference](settings-reference.md) | Players, developers | Every setting and environment variable: what the code really does, its effect on Mario Kart World (speed, timing, performance, video memory), recommended values |
| [Building](building.md) | Developers | Toolchain, exact build commands (clang-cl and MSVC), packaging, updating from Eglantine |
| [Changes in this fork](changes.md) | Developers, reviewers | Every commit on top of Eglantine `nx2`: why, what, files, origin |
| [Experiments that were dropped](experiments.md) | Developers | What was tried to remove shader stutter and raise the frame rate, with measurements, and why it is not in the release |
| [Known issues](known-issues.md) | Everyone | What still does not work |

Detailed design notes for the two main topics:

- [`NX2_SHADER_STUTTER.md`](../../NX2_SHADER_STUTTER.md): why new shaders froze the game and the
  "option C" changes that fixed it.
- [`NX2_VRAM.md`](../../NX2_VRAM.md): why video memory filled up (garbage textures, then a crash)
  and how the release prevents it.

## Branches

| Branch | Content |
| --- | --- |
| `mkw-eglantine` | Eglantine `nx2` + the commits of [changes.md](changes.md): what releases are built from |
| `release-c`, `release-d` | Earlier releases, built on Takoroka's eden-nx2 of 2026-10-29 (superseded) |

## AI disclosure

This fork's own changes were written with Claude Code (Claude Opus 5.5) and tested by a human on
Mario Kart World 1.8.0 (Ryzen 7 5800X, Radeon RX 6900 XT). Each commit says so; changes taken from
other authors keep their own disclosure. If you submit any of this code to another project, follow
that project's AI policy (for Eden and Eglantine: [`docs/policies/AI.md`](../policies/AI.md), which
asks for an `Assisted-by:` trailer and a human author).
