#!/usr/bin/nix-shell
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

let
  nixpkgs = fetchTarball "https://github.com/NixOS/nixpkgs/tarball/nixos-25.05";
  pkgs = import nixpkgs { config = {}; overlays = []; };
in
pkgs.mkShellNoCC {
  packages = with pkgs; [
    # essential programs
    git cmake clang gnumake ninja patch jq pkg-config
    # libraries
    openssl boost fmt_11 nlohmann_json lz4 zlib zstd
    enet libopus vulkan-headers vulkan-utility-libraries
    spirv-tools spirv-headers vulkan-loader mesa unzip
    glslang python3 httplib cpp-jwt ffmpeg-headless
    libusb1 cubeb
    # eden
    qt6.qtbase qt6.qtmultimedia qt6.qtwayland qt6.qttools qt6.qtcharts
    qt6.qtwebengine qt6.qt5compat
    # eden-cli
    sdl3
    # optional components
    discord-rpc gamemode
  ];
  # Host /usr Vulkan drivers can't load inside the Nix shell (their libdrm/libz deps
  # aren't visible to the Nix dynamic linker), so point the loader at Nix's RADV.
  VK_DRIVER_FILES = "${pkgs.mesa}/share/vulkan/icd.d/radeon_icd.x86_64.json";
  shellHook = ''
    export CMAKE_PREFIX_PATH="$(echo $nativeBuildInputs $buildInputs | tr ' ' ':')"
  '';
}
