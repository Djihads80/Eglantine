# Eglantine NixOS Flake - Builds and Installs latest release on any x86_64 NixOS system
{pkgs, ...}: let
  version = "build-204"; # change this periodically to targetted release tag from the github

  zbic = pkgs.fetchFromGitHub {
    owner = "kinnay";
    repo = "zbic";
    rev = "11b08f2712264bbed731545085cbd9702096ceb7";
    hash = "sha256-pHvxa1gkUvQo6mUgmLWtj1kcu01BLdGmttZyb8ZAj/o=";
  };

  tzdb_to_nx = pkgs.fetchFromGitHub {
        "https://git.eden-emu.dev/eden-emu/tzdb_to_nx/releases/"
    };

  reshade = pkgs.fetchFromGitHub {
    owner = "crosire";
    repo = "reshade";
    rev = "v6.8.0";
    hash = "sha256-K6QC7CLqy8M318n9WbuqGvcRF9g9abvDD/xbr7T+nXo=";
  };

  eglantine = pkgs.stdenv.mkDerivation {
    pname = "eglantine";
    inherit version;

    src = pkgs.fetchFromGitHub {
      owner = "Djihads80";
      repo = "Eglantine";
      rev = version;
      hash = "sha256-qsooLhvdXQ0JuDf+KqsDtjUlawNjGigM4XIb06g4OIQ="; # this will also need updating for every new release
    };

    nativeBuildInputs = with pkgs; [
      cmake
      ninja
      clang
      pkg-config
      python3
      git
      makeWrapper
      qt6.wrapQtAppsHook
    ];

    buildInputs = with pkgs; [
      # taken directly from shell.nix
      openssl
      boost
      fmt_11
      nlohmann_json
      lz4
      zlib
      zstd
      enet
      libopus
      vulkan-headers
      vulkan-utility-libraries
      spirv-tools
      spirv-headers
      vulkan-loader
      mesa
      unzip
      glslang
      python3
      httplib
      cpp-jwt
      ffmpeg-headless
      libusb1
      cubeb

      # had to add because it is downloaded via CPM in the shell.nix
      xbyak
      frozen-containers
      simpleini
      sirit
      vulkan-memory-allocator

      # so many qt6 libraries lol
      qt6.qtbase
      qt6.qtmultimedia
      qt6.qtwayland
      qt6.qttools
      qt6.qtcharts
      qt6.qtwebengine
      qt6.qt5compat

      sdl3
      discord-rpc
      gamemode
    ];

    # I put the cmake configuration in configure phase
    # and the actual compilation in build phase
    # have to force system bc cmake package manager can't download in nix sandbox

    configurePhase = ''
      runHook preConfigure

      cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF -Dzstd_FORCE_SYSTEM=ON -DOpus_FORCE_SYSTEM=ON -Dxbyak_FORCE_SYSTEM=ON -Dfrozen_FORCE_SYSTEM=ON -Dsimpleini_FORCE_SYSTEM=ON -Dsirit_FORCE_SYSTEM=ON -DVulkanMemoryAllocator_FORCE_SYSTEM=ON -Dzbic_CUSTOM_DIR=${zbic} -Dreshade_CUSTOM_DIR=${reshade}

      runHook postConfigure
    '';

    buildPhase = ''
      runHook preBuild

      cmake --build build --parallel $NIX_BUILD_CORES

      runHook postBuild
    '';

    # installation phase is literally just a copy paste of the executable
    installPhase = ''
      runHook preInstall

      mkdir -p $out/bin
      cp build/bin/eglantine $out/bin/eglantine

      runHook postInstall
    '';

    meta = {
      description = "Experimental Nintendo Switch 2 Emulator";
      homepage = "https://github.com/Djihads80/Eglantine";
      license = pkgs.lib.licenses.gpl3Plus; #GPL3 license
      platforms = ["x86_64-linux"]; # unless this changes
      mainProgram = "eglantine";
    };
  };
in {
  environment.systemPackages = [
    eglantine
  ];
}
