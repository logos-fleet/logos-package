# lgx for a phone: the C ABI of src/lgx.h as ONE STATIC ARCHIVE for
# aarch64-android, the same shape ./mobile-ios.nix produces for the two iOS
# targets.
#
# WHY STATIC, WHEN ANDROID LOADS SHARED OBJECTS PERFECTLY WELL. A consumer here
# is a Bare module inside an APK, and logos-nix's DT_NEEDED gate lets a shipped
# .so name only what Android guarantees at the app's API level or what the app
# packages beside it. liblgx.so is neither: it would be a second, unbundled
# soname in every APK that carries package_manager or package_downloader, and
# who puts it there is a question this repo cannot answer for its consumers.
# One archive makes the question not arise -- exactly the reason iOS is static,
# arrived at from the other direction.
#
# (liblogos builds the SAME sources shared for the same phone, and that is not a
# contradiction: liblogos_core is the app's own library, so its liblgx.so is a
# file the app packages. A Downloaded or Bundled module is not.)
#
# WHAT IS FOLDED IN, and why each one has no other home:
#
#   libsodium    Ed25519. A desktop consumer links liblgx.dylib and the loader
#                resolves it; an archive resolves nothing, so without this a
#                consumer would have to know that lgx signs with libsodium.
#   utf8proc     NFC normalization and lowercase (see LGX_UNICODE_UTF8PROC in
#                CMakeLists.txt). The NDK ships no ICU headers at all.
#   zlib         libz.so IS in the NDK stub set, but nixpkgs' zlib records the
#                soname `libz.so.1`, which is NOT -- so linking the nixpkgs
#                shared one would fail the gate naming a library Android does
#                have. The static archive sidesteps the whole question.
#
# Nothing else is left to fold: everything remaining (nlohmann_json, cpp-semver)
# is header-only.
{ pkgs, src, version, cppSemver }:

let
  inherit (pkgs) lib;
  buildPkgs = pkgs.pkgsBuildBuild;

  # nixpkgs builds both of these shared-only for this target, and appending is
  # enough: autoconf and CMake both let the LAST setting of an option win, so
  # neither upstream flag list has to be matched by hand (and a nixpkgs bump
  # that respells one does not silently un-static the build).
  sodium = pkgs.libsodium.overrideAttrs (old: {
    configureFlags = (old.configureFlags or [ ]) ++ [ "--enable-static" "--disable-shared" ];
  });
  utf8proc = pkgs.utf8proc.overrideAttrs (old: {
    cmakeFlags = (old.cmakeFlags or [ ]) ++ [ "-DBUILD_SHARED_LIBS:BOOL=FALSE" ];
  });

  buildInputs = [
    sodium
    utf8proc
    pkgs.zlib
    pkgs.nlohmann_json
    cppSemver
  ];

  # The archives folded into each installed library, in link order.
  vendored = [
    "${sodium}/lib/libsodium.a"
    "${utf8proc}/lib/libutf8proc.a"
    "${pkgs.zlib.static}/lib/libz.a"
  ];
in
pkgs.stdenv.mkDerivation {
  pname = "lgx-android";
  inherit src version buildInputs;

  # NO pkg-config, deliberately. lgx's CMakeLists tries
  # `pkg_check_modules(libsodium)` first, and under cross that yields a bare
  # `-lsodium` with no -L ("ld.lld: error: unable to find library -lsodium").
  # Without it on PATH the find_library fallback takes over and links the
  # absolute store path. (The same workaround, for the same reason, as
  # logos-liblogos' nix/mobile/android.nix.)
  nativeBuildInputs = [ buildPkgs.cmake buildPkgs.ninja ];

  cmakeFlags = [
    "-GNinja"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DLGX_STATIC_CABI=ON"
    "-DLGX_UNICODE_UTF8PROC=ON"
    "-DLGX_BUILD_TESTS=OFF"
    # find_package(ZLIB) would otherwise pick the shared image out of the same
    # prefix; what leaves this derivation has to be the archive we fold in.
    "-DZLIB_INCLUDE_DIR=${lib.getDev pkgs.zlib}/include"
    "-DZLIB_LIBRARY=${pkgs.zlib.static}/lib/libz.a"
  ];

  # The same shape ./lib.nix produces on a desktop: lgx.h beside the archive,
  # and the cpp-semver headers that include/logos/semver.hpp includes.
  postInstall = ''
    # This output is the LIBRARY. The CLI is built (its `if(NOT iOS)` guard
    # does not exclude Android) and is of no use inside an APK.
    rm -rf $out/bin

    cp ../src/lgx.h $out/include/
    cp -r ${cppSemver}/include/semver $out/include/

    # Both archives this package installs, each folded with the three above in
    # turn. An MRI script rather than `ar x`-and-re-archive: it keeps every
    # member, including two that happen to share a name, which is what the
    # ELF linker expects to resolve against.
    for _archive in liblgx liblgx_core; do
      "$AR" -M <<EOF
create $out/lib/$_archive-merged.a
addlib $out/lib/$_archive.a
${lib.concatMapStringsSep "\n" (a: "addlib ${a}") vendored}
save
end
EOF
      mv "$out/lib/$_archive-merged.a" "$out/lib/$_archive.a"
    done
  '';

  meta = {
    description = "lgx C ABI as a static archive for aarch64-android";
    platforms = lib.platforms.aarch64;
  };
}
