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

  # nixpkgs' eden pins httplib to 0.30.2 locally (newer releases break it) and
  # the inherited list used that pin. Reproduced verbatim so step one changes
  # nothing; the trim in the next commit drops it, because this tree takes
  # httplib from cpmfile.json.
  httplibPinned = pkgs.httplib.overrideAttrs (finalAttrs: {
    version = "0.30.2";

    src = pkgs.fetchFromGitHub {
      owner = "yhirose";
      repo = "cpp-httplib";
      rev = "v${finalAttrs.version}";
      hash = "sha256-psVyn14QHMXG/x9SOOiR7ZBt8dHqa2A/w92WQQDukKM=";
    };
  });

  # Eglantine's dependency list, written out rather than inherited from
  # pkgs.eden.
  #
  # These were `pkgs.eden.{nativeBuildInputs,buildInputs}` with a throw if the
  # attribute was ever absent. That borrowed another emulator's packaging
  # decisions: Eden still links SDL2 where this tree is SDL3-only through the
  # CPM manifest, fmt_11 where the manifest asks for 12.1.0, and KDE
  # Frameworks' quazip, so parts of the list were versions of libraries that
  # cpmfile.json also supplies. It also made this package fail outright if
  # nixpkgs ever renamed or dropped `eden`, and it cannot be submitted to
  # nixpkgs at all, where you cannot inherit from a derivation in the same tree.
  #
  # Order is kept identical to what was inherited, because buildInputs order
  # decides which copy of a library wins when two are on the prefix path.
  # The three linux-only entries are now conditional instead of being filtered
  # out by pname on darwin.
  #
  # One deliberate difference: pkgs.eden.buildInputs is 35 entries because
  # nixpkgs folds that package's checkInputs (catch2, oaknut on x86_64) into it
  # when doCheck is on. This build sets doCheck = false -- BUILD_TESTING=OFF, no
  # test suite -- so those two are not carried. Verified by diffing the two
  # derivations: apart from the output path, catch2 and oaknut are the only
  # inputs that differ.
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

  deps =
    with pkgs;
    [
      boost
      cpp-jwt
      cubeb
      enet
      ffmpeg-headless
      fmt_11
      frozen-containers
    ]
    ++ lib.optionals (!isDarwin) [ gamemode ]
    ++ [
      httplibPinned
      kdePackages.quazip
      libopus
      libusb1
      lz4
      nlohmann_json
      openssl
      qt6.qtbase
      qt6.qtmultimedia
    ]
    ++ lib.optionals (!isDarwin) [ qt6.qtwayland ]
    ++ [
      qt6.qtwebengine
      qt6.qtcharts
      SDL2
      stb
      simpleini
      spirv-headers
      vulkan-headers
      vulkan-memory-allocator
      vulkan-utility-libraries
    ]
    ++ lib.optionals (!isDarwin) [ sirit ]
    ++ [
      unordered_dense
      zlib
      zstd
    ]
    ++ lib.optionals pkgs.stdenv.hostPlatform.isx86_64 [ xbyak ]
    ++ lib.optionals pkgs.stdenv.hostPlatform.isAarch64 [ oaknut ];
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
