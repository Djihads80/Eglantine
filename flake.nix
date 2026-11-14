# SPDX-FileCopyrightText: Copyright 2026 Eglantine Community
# SPDX-License-Identifier: GPL-3.0-or-later
{
  description = "Eglantine";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # officially supported systems
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;

      # The build identity CI hands the derivation (composed by tools/ci/prepare.sh
      # and written by build-nix.yml). A checkout without one is a local build and
      # keeps the release version from VERSION.
      version = nixpkgs.lib.removeSuffix "\n" (
        builtins.readFile (
          if builtins.pathExists ./GIT-DISPLAY-STORE then ./GIT-DISPLAY-STORE else ./VERSION
        )
      );

      # The manifest readers and buildEdenEmulator for each system.
      emulatorTools = forAllSystems (
        system:
        import ./nix/build-eden-emulator.nix {
          lib = nixpkgs.lib;
          pkgs = nixpkgs.legacyPackages.${system};
        }
      );

      mkEglantine =
        system: extra:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        pkgs.callPackage ./nix/package.nix (
          {
            inherit version;
            src = ./.;
            buildEdenEmulator = emulatorTools.${system}.buildEdenEmulator;
          }
          // extra
        );
    in
    {
      packages = forAllSystems (system: {
        eglantine = mkEglantine system { };
        eglantine-pgo = mkEglantine system { pgo = true; };
        default = self.packages.${system}.eglantine;
      });

      # buildEdenEmulator, a reusable function, imported here
      lib = forAllSystems (system: emulatorTools.${system});

      overlays.default = final: _prev: {
        eglantine = mkEglantine final.stdenv.hostPlatform.system { };
      };

      # devshell adopted into flake
      # FIXME: Darwin not accounted for, don't believe any current devs use it
      devShells = forAllSystems (
        system:
        nixpkgs.lib.optionalAttrs (!(nixpkgs.lib.hasSuffix "darwin" system)) {
          default = import ./shell.nix { pkgs = nixpkgs.legacyPackages.${system}; };
        }
      );

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);

      # dependencies fetched from the CPM manifest so it stays the source of truth
      checks = forAllSystems (system: {
        cpm-deps = nixpkgs.legacyPackages.${system}.linkFarm "eglantine-cpm-deps" (
          nixpkgs.lib.mapAttrsToList
            (name: path: {
              inherit name path;
            })
            (emulatorTools.${system}.cpmDeps ./.)
        );
      });

      apps = forAllSystems (system: {
        default = self.apps.${system}.eglantine;
        eglantine = {
          type = "app";
          program = "${self.packages.${system}.eglantine}/bin/eglantine";
        };
        eglantine-pgo = {
          type = "app";
          program = "${self.packages.${system}.eglantine-pgo}/bin/eglantine";
        };
      });
    };
}
