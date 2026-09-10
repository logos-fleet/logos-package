cmake_minimum_required(VERSION 3.16)
project(lgx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(ZLIB REQUIRED)

# libsodium - try pkg-config first, then manual search
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SODIUM QUIET libsodium)
endif()
if(NOT SODIUM_FOUND)
    find_library(SODIUM_LIBRARIES NAMES sodium)
    find_path(SODIUM_INCLUDE_DIRS NAMES sodium.h)
    if(SODIUM_LIBRARIES AND SODIUM_INCLUDE_DIRS)
        set(SODIUM_FOUND TRUE)
    else()
        message(FATAL_ERROR "libsodium not found")
    endif()
endif()

# ICU configuration - check homebrew paths on macOS
if(APPLE)
    # Try to find ICU in homebrew keg-only location
    execute_process(
        COMMAND brew --prefix icu4c
        OUTPUT_VARIABLE ICU_BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE ICU_BREW_RESULT
    )
    
    if(ICU_BREW_RESULT EQUAL 0 AND EXISTS "${ICU_BREW_PREFIX}")
        message(STATUS "Found ICU via Homebrew: ${ICU_BREW_PREFIX}")
        set(ICU_ROOT "${ICU_BREW_PREFIX}")
        set(ENV{PKG_CONFIG_PATH} "${ICU_BREW_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# iOS has no ICU; CoreFoundation does the same NFC normalization and
# case mapping (see src/core/path_normalizer.cpp).
option(LGX_UNICODE_COREFOUNDATION "Use CoreFoundation instead of ICU for Unicode normalization" OFF)
if(LGX_UNICODE_COREFOUNDATION)
    set(LGX_UNICODE_LIBS "-framework CoreFoundation")
    add_compile_definitions(LGX_UNICODE_COREFOUNDATION=1)
else()
    find_package(ICU REQUIRED COMPONENTS uc i18n)
    set(LGX_UNICODE_LIBS ICU::uc ICU::i18n)
endif()

# Try to find nlohmann_json first (for Nix and system installations)
# Fall back to FetchContent if not found
find_package(nlohmann_json 3.11.0 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)
endif()

# cpp-semver — the SemVer 2.0.0 precedence engine behind include/logos/semver.hpp.
# Same find_package-then-FetchContent shape as nlohmann_json above; under Nix it
# is supplied by nix/cpp-semver.nix (the sandbox has no network for FetchContent).
find_package(semver 0.4.0 QUIET)
if(NOT semver_FOUND)
    message(STATUS "cpp-semver not found, fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        cpp_semver
        GIT_REPOSITORY https://github.com/z4kn4fein/cpp-semver.git
        GIT_TAG v0.4.0
    )
    set(SEMVER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SEMVER_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cpp_semver)
    # We install include/logos/semver.hpp as a public header, and it includes
    # <semver/semver.hpp>. With SEMVER_INSTALL off, the FetchContent path
    # wouldn't install that dependency header, so a downstream `cmake --install`
    # consumer would fail to compile. Install it alongside ours. (The
    # find_package path needn't: Nix supplies and vendors the header itself.)
    install(DIRECTORY ${cpp_semver_SOURCE_DIR}/include/semver DESTINATION include)
endif()

# Core library
add_library(lgx_core STATIC
    src/core/path_normalizer.cpp
    src/core/gzip_handler.cpp
    src/core/tar_writer.cpp
    src/core/tar_reader.cpp
    src/core/manifest.cpp
    src/core/package.cpp
    src/core/installed_package.cpp
    src/core/platform_variant.cpp
    src/core/main_resolution.cpp
    src/crypto/signing.cpp
    src/crypto/manifest_sig.cpp
    src/crypto/keyring.cpp
)

target_include_directories(lgx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(lgx_core PUBLIC
    ${SODIUM_INCLUDE_DIRS}
)

target_link_libraries(lgx_core PUBLIC
    ZLIB::ZLIB
    ${LGX_UNICODE_LIBS}
    nlohmann_json::nlohmann_json
    semver::semver
    ${SODIUM_LIBRARIES}
)

# Shared library (optional)
option(LGX_BUILD_SHARED "Build shared library" OFF)
# The same C ABI as a static archive, for hosts that cannot load a dylib.
option(LGX_STATIC_CABI "Build the lgx C ABI (lgx.h) as a static archive named liblgx.a" OFF)
set(LGX_CABI_LIBRARY_TYPE SHARED)
if(LGX_STATIC_CABI)
    set(LGX_CABI_LIBRARY_TYPE STATIC)
endif()
if(LGX_BUILD_SHARED OR LGX_STATIC_CABI)
    add_library(lgx_shared ${LGX_CABI_LIBRARY_TYPE}
        src/lib.cpp
        src/core/path_normalizer.cpp
        src/core/gzip_handler.cpp
        src/core/tar_writer.cpp
        src/core/tar_reader.cpp
        src/core/manifest.cpp
        src/core/package.cpp
    src/core/installed_package.cpp
        src/core/platform_variant.cpp
        src/core/main_resolution.cpp
        src/crypto/signing.cpp
        src/crypto/manifest_sig.cpp
        src/crypto/keyring.cpp
    )
    
    target_include_directories(lgx_shared PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${SODIUM_INCLUDE_DIRS}
    )

    target_link_libraries(lgx_shared PUBLIC
        ZLIB::ZLIB
        ${LGX_UNICODE_LIBS}
        nlohmann_json::nlohmann_json
        semver::semver
        ${SODIUM_LIBRARIES}
    )
    
    set_target_properties(lgx_shared PROPERTIES
        OUTPUT_NAME "lgx"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    
    target_compile_definitions(lgx_shared PRIVATE LGX_BUILD_SHARED)
    
    install(TARGETS lgx_shared
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
    install(FILES src/lgx.h DESTINATION include)
endif()

# CLI executable
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
add_executable(lgx
    src/main.cpp
    src/commands/command.cpp
    src/commands/create_command.cpp
    src/commands/add_command.cpp
    src/commands/remove_command.cpp
    src/commands/extract_command.cpp
    src/commands/verify_command.cpp
    src/commands/sign_command.cpp
    src/commands/publish_command.cpp
    src/commands/merge_command.cpp
    src/commands/keygen_command.cpp
    src/commands/keyring_command.cpp
    src/commands/manifest_command.cpp
    src/commands/signature_command.cpp
    src/commands/semver_command.cpp
)

target_link_libraries(lgx PRIVATE lgx_core)

# Installation
# On Windows, BOTH the lgx executable and lgx_shared (which carries
# OUTPUT_NAME "lgx") generate an import library called liblgx.dll.a, and both
# install it to lib/. Whichever install rule runs last wins, so consumers got
# either the real 22 KB DLL import library or the 892-byte one belonging to
# lgx.exe -- at random. When the executable's won, every consumer failed to
# link with "undefined reference to `__imp_lgx_*'" while lgx itself built and
# ran perfectly. Renaming the executable's archive removes the collision.
# Harmless elsewhere: no other platform generates an import library at all.
if(WIN32)
    set_target_properties(lgx PROPERTIES ARCHIVE_OUTPUT_NAME "lgx_exe")
endif()

install(TARGETS lgx RUNTIME DESTINATION bin)
endif() # not iOS
install(TARGETS lgx_core ARCHIVE DESTINATION lib)

# Public headers. logos/semver.hpp is the shared semver implementation that
# lgpm, lgpd and the package-manager UI all include, so it has to ship with the
# library rather than stay internal to this repo.
install(DIRECTORY include/logos DESTINATION include)

# Testing (optional)
option(LGX_BUILD_TESTS "Build tests" OFF)
if(LGX_BUILD_TESTS)
    # Try to find Google Test first (for Nix and system installations)
    # Fall back to FetchContent if not found
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GTest not found, fetching from GitHub")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        # Prevent overriding the parent project's compiler/linker settings on Windows
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()
