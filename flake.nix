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

      # The three mobile targets, each keyed by the ONE build platform this
      # flake publishes it under. iOS needs Xcode, so aarch64-darwin is not a
      # choice; Android builds from either member of logos-nix's
      # androidBuildSystems, and a cross derivation's `system` is its BUILD
      # platform -- so it is published under every one of them rather than
      # under a target key nothing can realise.
      #
      # ALL THREE ARE STATIC ARCHIVES, for two different reasons that meet in
      # the same place: iOS loads no dynamic library of its own, and an Android
      # APK may not carry an unbundled liblgx.so past logos-nix's DT_NEEDED
      # gate. See nix/mobile-ios.nix and nix/mobile-android.nix.
      iosBuildSystem = "aarch64-darwin";
      iosTargets = [ "aarch64-ios" "aarch64-ios-simulator" ];
      # `version` is the only thing read out of the desktop common config, and
      # it is a string: nothing in it is instantiated for a phone. `cppSemver`
      # is the BUILD platform's header-only semver package, taken as it is --
      # it installs headers and an INTERFACE-only CMake config, so there is
      # nothing in it to cross-compile.
      mobileLib = { buildSystem, file, pkgs }: {
        lib = import file {
          inherit pkgs;
          src = ./.;
          inherit (import ./nix/default.nix { inherit pkgs; }) version;
          cppSemver = self.packages.${buildSystem}.cpp-semver;
        };
      };
      mobileLibs = nixpkgs.lib.genAttrs iosTargets (target:
        mobileLib {
          buildSystem = iosBuildSystem;
          file = ./nix/mobile-ios.nix;
          pkgs = logos-nix.lib.mkIosPkgs { inherit target; buildSystem = iosBuildSystem; };
        });
      # The build platforms that can produce the Android archive. logos-nix owns
      # this list, and the workspace's `follows` supplies a pin that publishes it
      # -- but the ATTRIBUTE NAMES of `legacyPackages` have to be computable from
      # THIS repo's own lock as well (`ws test` evaluates the sub-repo flake with
      # no overrides at all), and that pin predates `lib.androidBuildSystems`. So
      # the list is read when it is there and spelled out when it is not; the
      # derivations under those names still come from logos-nix and still fail
      # loudly on a pin too old to build them, exactly as the iOS ones do.
      androidBuildSystems =
        logos-nix.lib.androidBuildSystems or [ "x86_64-linux" "aarch64-darwin" ];
      androidLibs = buildSystem: {
        aarch64-android = mobileLib {
          inherit buildSystem;
          file = ./nix/mobile-android.nix;
          pkgs = logos-nix.lib.mkAndroidPkgs { inherit buildSystem; };
        };
      };
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

      # `legacyPackages`, not `packages`: a cross derivation's `system` is its
      # BUILD platform, so these would collide with the native aarch64-darwin
      # set, and `nix flake check` would try to realise an iOS archive as if it
      # were a Mac one. The shape is the one logos-module-builder's
      # `mobilePackages` seam reads: mobile.<target>.lib, laid out lib/ +
      # include/ exactly like packages.<system>.lib.
      legacyPackages =
        nixpkgs.lib.genAttrs androidBuildSystems
          (buildSystem: { mobile = androidLibs buildSystem; })
        // {
          # Merged rather than assigned: aarch64-darwin builds both Android and
          # iOS, and writing it twice would drop one of the two.
          ${iosBuildSystem}.mobile =
            mobileLibs // (androidLibs iosBuildSystem);
        };

      checks = forAllSystems ({ pkgs }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
        in {
          tests = import ./nix/all.nix { inherit pkgs common src; };
          # The Android Unicode backend, run where a test binary can execute.
          utf8proc-backend-tests =
            import ./nix/unicode-backend-test.nix { inherit pkgs common src; };
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
