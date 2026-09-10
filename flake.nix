{
  description = "lgx - Logos Package Manager CLI";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = { self, nixpkgs, logos-nix }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
      });

      # Adds the "x86_64-windows" pseudo-system on top of the native ones. A
      # cross derivation's `system` attribute is its BUILD platform, so
      # `packages.x86_64-windows.*` evaluates anywhere but realises on Linux.
      #
      # PACKAGES only. `checks` stays native: ctest cannot execute PE binaries
      # on the Linux build host, so a Windows "check" would assert nothing. And
      # a cross devShell would hand you a mingw compiler with no way to run what
      # it produces.
      forAllTargets = logos-nix.lib.forAllTargets;
    in
    {
      packages = forAllTargets ({ pkgs, ... }:
        let
          # Common configuration
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          
          # Binary package
          bin = import ./nix/bin.nix { inherit pkgs common src; };
          
          # Library package
          libPkg = import ./nix/lib.nix { inherit pkgs common src; };

          # Headers-only package (no library — see nix/headers.nix)
          headersPkg = import ./nix/headers.nix { inherit pkgs common src; };

          # Combined package (binary + library + tests)
          allPkg = import ./nix/all.nix { inherit pkgs common src; };
        in
        {
          # lgx binary package
          lgx = bin;

          # lgx shared library
          lib = libPkg;

          # lgx public headers, without the library. For consumers that need the
          # shared semver implementation but must not link liblgx.
          headers = headersPkg;

          # The header-only SemVer engine include/logos/semver.hpp is written
          # against (nix/cpp-semver.nix -- it is not in nixpkgs). Exposed
          # because a cross build of lgx has to stage it explicitly: the
          # header travels with lgx's install tree, but find_package(semver)
          # during the build needs the package itself, and a consumer
          # cross-compiling lgx cannot reach into this flake's nix/ directory.
          cpp-semver = common.cpp-semver;

          # lgx all-in-one package (binary, library, and tests)
          all = allPkg;

          # Default package
          default = allPkg;
        }
      );

      checks = forAllSystems ({ pkgs }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
        in {
          tests = import ./nix/all.nix { inherit pkgs common src; };
        }
      );

      devShells = forAllSystems ({ pkgs }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.zlib
            pkgs.icu
            pkgs.nlohmann_json
            pkgs.libsodium
          ];
          
          shellHook = ''
            echo "lgx development environment"
            echo "Build with: nix build"
            echo "Or use cmake directly: cmake -B build -GNinja && cmake --build build"
          '';
        };
      });
    };
}
