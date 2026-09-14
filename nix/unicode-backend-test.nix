# The utf8proc Unicode backend, run against lgx's own PathNormalizer suite.
#
# WHY A SECOND CHECK OVER THE SAME TESTS. `nix/all.nix` builds and runs them
# against ICU, which is the desktop backend and the only one those tests have
# ever seen. Android gets neither ICU nor CoreFoundation (see
# nix/mobile-android.nix), so PathNormalizer has a THIRD implementation there --
# and an implementation that nothing runs is a guess. This builds the suite with
# `-DLGX_UNICODE_UTF8PROC=ON` on the BUILD platform, where a test binary can
# actually execute, so the backend a phone links is the backend a test ran.
#
# It also asserts WHICH backend got linked. Without that the check is worthless:
# an unknown `-D` is just a cache variable to CMake, so a build that quietly
# ignored the option would run the ICU implementation and report a pass.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-utf8proc-backend-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs cmakeFlags;
  # ICU is deliberately NOT in this list: if it were reachable, "the ICU path
  # was not taken" would be a claim about the linker's mood rather than a fact.
  buildInputs = [
    pkgs.zlib
    pkgs.nlohmann_json
    pkgs.libsodium
    pkgs.utf8proc
    pkgs.gtest
    common.cpp-semver
  ];

  configurePhase = ''
    runHook preConfigure
    cmake -S . -B build \
      -GNinja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
      -DLGX_BUILD_TESTS=ON \
      -DLGX_UNICODE_UTF8PROC=ON \
      $cmakeFlags "''${cmakeFlagsArray[@]}"
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    cmake --build build
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck

    # The option did what it says. lgx_tests is the binary that runs the
    # PathNormalizer cases, so this asks the question of exactly those bytes --
    # `grep -a` rather than `strings`, which is not one tool on both platforms.
    tests=$(find build -name lgx_tests -type f | head -1)
    [ -n "$tests" ] || { echo "ERROR: no lgx_tests binary under build/" >&2; find build -maxdepth 2 >&2; exit 1; }
    if ! grep -aq utf8proc "$tests"; then
      echo "ERROR: $tests names no utf8proc library -- the utf8proc backend was not compiled in." >&2
      exit 1
    fi
    if grep -aqE 'libicu(uc|i18n)' "$tests"; then
      echo "ERROR: $tests still links ICU; LGX_UNICODE_UTF8PROC did not take the branch." >&2
      exit 1
    fi

    cd build
    export LGX_BINARY="$(pwd)/lgx"
    ctest --output-on-failure
    cd ..

    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    echo "lgx PathNormalizer suite passed against the utf8proc backend" > $out/result
    runHook postInstall
  '';

  meta = common.meta // {
    description = "lgx tests against the utf8proc Unicode backend (the Android one)";
  };
}
