# Arch Linux development package

This directory contains a draft `eglantine-git` PKGBUILD for the open Linux
packaging request in [issue #167](https://github.com/Djihads80/Eglantine/issues/167).
It builds the `nx2` branch from source against Arch's system Qt, SDL3 and FFmpeg.
That avoids shipping Ubuntu-linked binaries to a rolling-release distribution
and gives the Qt frontend access to the host's platform plugins, including
Wayland when `qt6-wayland` is installed.

## Local build

On an up-to-date Arch Linux system, install the dependencies listed in
`PKGBUILD`, then run:

```sh
makepkg --syncdeps --cleanbuild
```

For package review, also build it in a clean chroot with `extra-x86_64-build`
and inspect the result with `namcap`. The source branch moves over time, so
review the generated `.SRCINFO` after each `pkgver` update before publishing
this recipe to the AUR.

This recipe is a starting point for review, not an official AUR package. It
does not modify the project's release binaries or CI workflow. The updater is
disabled because package updates should be handled by pacman/AUR helpers.
