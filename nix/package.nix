# SPDX-FileCopyrightText: Copyright 2026 Eglantine Community
# SPDX-License-Identifier: GPL-3.0-or-later

{ lib
, pkgs
, buildEdenEmulator
, llvmPackages_23
, src
, version
, pgo ? false
, nightly ? false
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

  onOff = b: if b then "ON" else "OFF";

  isDarwin = pkgs.stdenv.hostPlatform.isDarwin;

  pgoBintools = lib.optional (pgo && !isDarwin) llvmPackages_23.bintools;
  pgoLinkerFlags = lib.optionals (pgo && !isDarwin) [
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld"
  ];

  linuxOnlyInputs = [
    "qtwayland"
    "gamemode"
    "sirit"
  ];
in
buildEdenEmulator (
  {
    inherit src version;

    pname = "eglantine";

    stdenv = if pgo then llvmPackages_23.stdenv else pkgs.stdenv;

    extraNativeBuildInputs = pgoBintools;

    cmakeFlags = [
      "-DCMAKE_BUILD_TYPE=Release"
      "-DBUILD_TESTING=OFF"
      "-DYUZU_ROOM=OFF"
      "-DVulkanHeaders_FORCE_BUNDLED=ON"
      "-DVulkanUtilityLibraries_FORCE_BUNDLED=ON"
      "-DNIGHTLY_BUILD=${onOff nightly}"

      # Nix deviations:
      # 1. QT6 is found through the setup hook,
      #    so no prefix path is needed
      # 2. NIGHTLY_BUILD is a function argument,
      #    not a workflow input (checking env vars is impure)
      # 3. SDL3 is built from source for Nix's reproducibility reasons
      #    (not downloaded, but Cachix is integrated in CI)
      # 4. Nixpkgs currently ships GCC 16, which is stricter than
      #    Ubuntu's GCC 13, so -Werror is disabled to avoid build failures

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
      homepage = "https://github.com/Djihads80/Eglantine";
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

    # filter out linux-only inputs
    buildInputs = lib.filter
      (
        p: !(builtins.elem (p.pname or "") linuxOnlyInputs)
      )
      pkgs.eden.buildInputs;

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
