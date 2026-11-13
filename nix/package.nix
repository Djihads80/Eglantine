# SPDX-FileCopyrightText: Copyright 2026 Eglantine Community
# SPDX-License-Identifier: GPL-3.0-or-later

{ lib
, pkgs
, buildEdenEmulator
, llvmPackages_23
, src
, version
, pgo ? false
, # CPU tuning, off by default, only here for tinkerers
  march ? null
, # mark the binary as a Nix-specific build
  # off by default as the output should be practically identical
  # to the non-Nix builds
  nixBuildIdentity ? false
,
}:

let
  # ccache keys on the compiler command line, so the profile path inside it has
  # to be the same string on every commit. ${src} is a hash of the whole tree
  # and changes with every commit, which is what put the PGO variants at 0 hits
  # out of 3678 while the plain variant beside them hit 45%. builtins.path
  # hashes the file it is handed, so this path moves only when the profile
  # itself moves -- the property upstream gets for free by passing
  # -fprofile-use=$PWD/eglantine.profdata.
  #
  # Both are lazy and only forced under pgo, so a tree without a profile still
  # evaluates; a fork that has none is unaffected unless it asks for PGO.
  srcPath = if builtins.isPath src then src else /. + src;
  pgoProfile = builtins.path {
    path = srcPath + "/eglantine.profdata";
    name = "eglantine.profdata";
  };

  profile = lib.optionalString pgo (toString [
    "-fprofile-use=${pgoProfile}"
    "-Wno-backend-plugin"
    "-Wno-profile-instr-unprofiled"
    "-Wno-profile-instr-out-of-date"
  ]);

  archFlags = lib.optionalString (march != null) "-march=${march} -mtune=${march}";

  isDarwin = pkgs.stdenv.hostPlatform.isDarwin;

  pgoBintools = lib.optional (pgo && !isDarwin) llvmPackages_23.bintools;
  pgoLinkerFlags = lib.optionals (pgo && !isDarwin) [
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld"
  ];

  # Eglantine's dependency list. Written out rather than inherited from
  # pkgs.eden: that made this package's dependencies another emulator's
  # decision (Eden links SDL2 where this tree is SDL3-only, fmt_11 where
  # cpmfile.json asks for 12.1.0), made it fail outright if nixpkgs ever
  # renamed `eden`, and cannot be submitted to nixpkgs, where a derivation
  # cannot inherit from a sibling one.
  #
  # What is left is only what CPM cannot supply. The builder passes
  # -DCPMUTIL_FORCE_SYSTEM=OFF and points every cpmfile.json entry at a
  # pre-fetched store path, so a nixpkgs copy of a manifest library is shadowed
  # at best and the wrong version at worst.
  #
  #   ffmpeg-headless  deliberately the system build, see the FFmpeg note below
  #   openssl          upstream's Linux build links the system OpenSSL too
  #   gamemode         Linux only, and loaded by the running process rather
  #                    than linked; upstream installs the distro's dev package
  #   qt6.qtbase       Core, Gui, Widgets and Concurrent all live here
  #   qt6.qtcharts     listed in YUZU_QT_COMPONENTS, so required unconditionally
  #   qt6.qtwayland    the wayland platform plugin the wrapper hook needs
  #
  # Dropped from the inherited list because cpmfile.json supplies them:
  #   boost cpp-jwt cubeb enet fmt_11 frozen-containers httplib quazip libopus
  #   libusb1 lz4 nlohmann_json simpleini sirit spirv-headers vulkan-headers
  #   vulkan-memory-allocator vulkan-utility-libraries zlib zstd xbyak oaknut
  # Dropped because nothing here uses them:
  #   SDL2 (SDL3 comes from the manifest), stb (vendored in externals/stb,
  #   which defines the stb target), unordered_dense (no reference anywhere in
  #   the tree), qt6.qtmultimedia and qt6.qtwebengine (YUZU_USE_QT_MULTIMEDIA
  #   and YUZU_USE_QT_WEB_ENGINE both default OFF and are never set here)
  nativeDeps = with pkgs; [
    cmake
    ninja
    glslang
    pkg-config
    python3
    qt6.qttools
    qt6.wrapQtAppsHook
    wrapGAppsHook3
  ];

  # SDL3 is compiled from source inside this derivation: the manifest pins
  # 3.4.8 and the builder hands CPM the source tree, so SDL3's own configure
  # runs here and its dependencies have to be ours. They used to arrive by
  # accident, transitively through SDL2 and qt6.qtwebengine -- dropping those
  # is what broke the first trimmed build, with SDL3 failing on XSCRNSAVER.
  # This is nixpkgs' SDL3 build dependency set for Linux, minus the optional
  # tray/ibus/jack/pipewire bits, which SDL3 autodetects rather than requires.
  sdl3Deps = with pkgs; [
    alsa-lib
    dbus
    libGL
    libdecor
    libdrm
    libgbm
    libpulseaudio
    libusb1
    libx11
    libxcb
    libxcursor
    libxext
    libxfixes
    libxi
    libxkbcommon
    libxrandr
    libxscrnsaver
    libxtst
    systemdLibs
    vulkan-headers
    vulkan-loader
    wayland
  ];

  deps =
    with pkgs;
    [
      ffmpeg-headless
      openssl
      qt6.qtbase
      qt6.qtcharts
    ]
    ++ lib.optionals (!isDarwin) (
      [
        gamemode
        qt6.qtwayland
      ]
      ++ sdl3Deps
    );
in
buildEdenEmulator (
  {
    inherit src version;

    pname = "eglantine";

    nativeBuildInputs = nativeDeps;
    buildInputs = deps;

    stdenv = if pgo then llvmPackages_23.stdenv else pkgs.stdenv;

    extraNativeBuildInputs = pgoBintools;

    cmakeFlags = [
      "-DCMAKE_BUILD_TYPE=Release"
      "-DBUILD_TESTING=OFF"
      "-DYUZU_ROOM=OFF"
      "-DVulkanHeaders_FORCE_BUNDLED=ON"
      "-DVulkanUtilityLibraries_FORCE_BUNDLED=ON"

      # NIGHTLY_BUILD is not a flag here: CMakeLists.txt turns it on when a
      # GIT-NIGHTLY file sits in the source root, the same way GIT-RELEASE
      # already decides release-ness. CI drops that file in before building.

      # Matching upstream's build-linux.yml, which has always passed this for
      # the Linux release. discord-rpc comes from cpmfile.json like everything
      # else, so this costs a link step, not a new input.
      "-DUSE_DISCORD_PRESENCE=ON"

      # Nix deviations:
      # 1. QT6 is found through the setup hook,
      #    so no prefix path is needed
      # 2. NIGHTLY_BUILD is a function argument,
      #    not a workflow input (checking env vars is impure)
      # 3. SDL3 is built from source for Nix's reproducibility reasons
      #    (not downloaded, but Cachix is integrated in CI)
      # 4. Nixpkgs currently ships GCC 16, which is stricter than
      #    Ubuntu's GCC 13, so -Werror is disabled to avoid build failures
      # 5. FFmpeg is the nixpkgs build, not upstream's bundled static one.
      #    Upstream bundles 9.0.2 because its tar.gz linked Ubuntu's
      #    libavcodec.so.60 (absent on users' machines), needed libva 2.20
      #    instead of 2.21+, and wanted dav1d on aarch64. None of that
      #    transfers: nixpkgs' ffmpeg-headless IS ffmpeg_9-headless, 9.0.2
      #    with dav1d, and the store path is baked into the binary. Do not set
      #    YUZU_USE_BUNDLED_FFMPEG=ON: it selects ffmpeg-ci-av1-linux, a `ci`
      #    entry with no hash that this builder cannot fetch, so configure
      #    would try to download in the sandbox.
      #    (Same trap for -DYUZU_USE_BUNDLED_SDL3=ON -> sdl3-ci.)
      #    Watch it if nixpkgs moves its unversioned ffmpeg default.

      "-DENABLE_WERROR=OFF"
    ]
    ++ pgoLinkerFlags
    ++ lib.optionals pgo [
      "-DCMAKE_C_FLAGS=${profile} ${archFlags}"
      "-DCMAKE_CXX_FLAGS=${profile} ${archFlags}"
    ]
    ++ lib.optionals (!pgo && march != null) [
      "-DCMAKE_C_FLAGS=${archFlags}"
      "-DCMAKE_CXX_FLAGS=${archFlags}"
    ];

    # opt-in build-identity, see above
    env = lib.optionalAttrs nixBuildIdentity {
      TITLEBARFORMATIDLE = "eglantine | ${version} (nix) {}";
      TITLEBARFORMATRUNNING = "eglantine | ${version} (nix) | {}";
    };

    meta = {
      description = "Experimental Nintendo Switch 2 emulator";
      homepage = "https://github.com/Eglantine-Community/Eglantine";
      mainProgram = "eglantine";
      license = lib.licenses.gpl3Plus;
      platforms = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
    };
  }
    // lib.optionalAttrs isDarwin {

    # dynarmic dependencies on darwin
    extraNativeBuildInputs = pgoBintools ++ [
      pkgs.darwin.bootstrap_cmds # mig
      pkgs.darwin.DarwinTools # sw_ver
    ];

    # depending on llvmPackages.libcxx would put its libc++.dylib
    # on the link path as well, and -lc++ would then resolve to that
    # copy instead of the SDK's -- a second C++ runtime, kept apart on purpose
    # -> we only take what we need
    extraBuildInputs = [
      (pkgs.runCommandLocal "libcxx-experimental" { } ''
        mkdir -p $out/lib
        cp ${pkgs.llvmPackages.libcxx}/lib/libc++experimental.a $out/lib/
      '')
    ];

    # simple Nix-specific install rules
    postInstall = lib.optionalString isDarwin ''
      mkdir -p $out/Applications $out/bin
      cp -R "$(find . -maxdepth 3 -name 'eglantine.app' -type d | head -1)" $out/Applications/
      install -m755 "$(find . -maxdepth 3 -name 'eden-cli' -type f | head -1)" $out/bin/

      # The same path as on every other platform, so meta.mainProgram and the
      # flake's apps output need no macOS special case.
      ln -s ../Applications/eglantine.app/Contents/MacOS/eglantine $out/bin/eglantine
    '';
  }
)
