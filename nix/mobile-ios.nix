# lgx for a phone: the C ABI of src/lgx.h as ONE STATIC ARCHIVE per iOS target.
#
# WHY IT LIVES HERE. Three different repos now cross-compile lgx for iOS --
# liblogos' host chain, logos-package-manager's module library and
# logos-package-downloader's -- and the two options that make an iOS build
# possible at all are properties of THIS repo's CMakeLists, not of any consumer:
#
#   LGX_STATIC_CABI            iOS loads no dynamic library of its own; the C
#                              ABI has to be an archive the consumer links.
#   LGX_UNICODE_COREFOUNDATION ICU has no iOS build on this pin, and
#                              CFStringNormalize does the same NFC normalization
#                              PathNormalizer asks ICU for.
#
# A consumer that restated them would be free to drift from what the source
# actually supports, and the failure mode is a link error forty lines deep in a
# repo that does not own the answer.
#
# NOT nixpkgs' iOS cross stdenv: it fails building compiler-rt at this pin (see
# logos-nix's nix/ios/third-party.nix). `mkIosCmakeStage` is Xcode's clang, and
# it gates the result -- a dynamic image leaving this derivation is an error.
{ pkgs, src, version, cppSemver }:

let
  buildInputs = [
    pkgs.libsodium
    pkgs.nlohmann_json
    cppSemver
  ];
in
pkgs.mkIosCmakeStage {
  pname = "lgx-ios";
  inherit src version buildInputs;
  cmakeFlags = [
    # An iOS toolchain puts find_package in root-only mode, so being on
    # CMAKE_PREFIX_PATH is not enough -- every input has to be named as a ROOT.
    "-DCMAKE_FIND_ROOT_PATH=${pkgs.lib.concatStringsSep ";" (map toString buildInputs)}"
    "-DLGX_STATIC_CABI=ON"
    "-DLGX_UNICODE_COREFOUNDATION=ON"
    "-DLGX_BUILD_TESTS=OFF"
  ];
  # The same shape nix/lib.nix produces on a desktop: lgx.h beside the archive,
  # and the cpp-semver headers that include/logos/semver.hpp includes.
  #
  # ...plus libsodium FOLDED IN. A desktop consumer links liblgx.dylib and the
  # dynamic loader resolves Ed25519 through it; an archive resolves nothing, so
  # without this a consumer would have to know that lgx signs with libsodium and
  # name it on its own link line -- in a repo that has no reason to know. One
  # archive keeps the iOS package honouring the desktop contract: link what
  # lib/ names, and nothing is missing.
  #
  # zlib and CoreFoundation stay OUT, and deliberately: both are the PLATFORM's
  # (libz.tbd and the framework ship in the iOS SDK), there is no archive to
  # fold in, and a consumer names them the way it names any system library.
  postInstall = ''
    cp ../src/lgx.h $out/include/
    cp -r ${cppSemver}/include/semver $out/include/

    "$(xcrun --find libtool)" -static -no_warning_for_no_symbols       -o liblgx-merged.a "$out/lib/liblgx.a" "${pkgs.libsodium}/lib/libsodium.a"
    mv liblgx-merged.a "$out/lib/liblgx.a"
    "$(xcrun --find libtool)" -static -no_warning_for_no_symbols       -o liblgx_core-merged.a "$out/lib/liblgx_core.a" "${pkgs.libsodium}/lib/libsodium.a"
    mv liblgx_core-merged.a "$out/lib/liblgx_core.a"
  '';
}
