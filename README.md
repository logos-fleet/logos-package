# LGX

## Usage

### Create a Package

Create a new skeleton package:

```bash
lgx create mymodule
# Creates mymodule.lgx
```

### Add Variants

Add a single file to a variant:

```bash
lgx add mymodule.lgx --variant linux-amd64 --files ./build/libfoo.so
```

Add a directory to a variant (usually requires `--main`):

```bash
lgx add mymodule.lgx --variant web --files ./dist --main index.js
```

For `type == "ui_qml"` packages, `view` is the required QML entry point and
`main` is optional backend metadata, so directory variants can be added without
`--main` when there is no backend plugin. Use `--view` to set the QML entry
point in the manifest:

```bash
lgx add mymodule.lgx --variant darwin-arm64 --files ./dist --view qml/Main.qml
```

**Note:** If a variant already exists, it is **completely replaced** (no merging). Use `-y` to skip confirmation:

```bash
lgx add mymodule.lgx -v linux-amd64 -f ./new-build/libfoo.so -y
```

#### Variant names

One vocabulary, shared by `lgx`, `lgpm` and `lgpd`:

| | |
|---|---|
| Desktop | `linux-x86_64` `linux-arm64` `darwin-x86_64` `darwin-arm64` `windows-x86_64` `windows-arm64` |
| Mobile | `android-arm64` `android-x86_64` `ios-arm64` `ios-sim-arm64` |
| Web container | `web` |

`amd64` / `aarch64` are accepted everywhere `x86_64` / `arm64` are, and a
non-portable consumer build looks for the `-dev` flavour of each. A host tries
its own name first, then the other spelling of its architecture, and stops —
`ios-sim-arm64` is not `ios-arm64`, `android-arm64` is not `linux-arm64`, and
no native host falls back to `web`.

A misspelling of one of these is refused with the name that was meant, because
the package it would produce installs nowhere:

```bash
$ lgx add mymodule.lgx -v ios_arm64 -f ./MyModule.framework -m MyModule
Error: Unknown variant 'ios_arm64': did you mean 'ios-arm64'?
```

See `docs/spec.md` § *Platform Variant Vocabulary* for the full table.

### Remove a Variant

```bash
lgx remove mymodule.lgx --variant linux-amd64
```

### Verify a Package

Validate a package against the LGX specification:

```bash
lgx verify mymodule.lgx

# Use a custom keyring for trust lookup
lgx verify mymodule.lgx --keyring-dir /etc/logos/trusted-keys
```

Always verifies content hashes (Merkle tree) match the actual package contents.
If the package is signed, also checks the Ed25519 signature and reports whether
the signer's DID is in the trusted keyring.

Exit code is `0` on success and non-zero on validation failure, so it's safe to
use in scripts and CI pipelines (`set -e`, `gh workflow` jobs, etc.).

### Inspect a Package's Manifest

Read the manifest of a package without unpacking it:

```bash
# Human-readable summary: name, version, type, category, author,
# root hash, variants (keys of manifest.main), dependencies, and the
# signer DID when the package is signed.
lgx manifest mymodule.lgx

# Raw manifest JSON bytes — byte-identical to the manifest.json
# file inside the .lgx. Intended for tooling (e.g. the release CI
# pipeline) that needs to capture the exact manifest verbatim.
lgx manifest mymodule.lgx --json > manifest.json
```

The `--json` output is the source of truth consumed by
`logos-modules-release-action` when it extracts the `name` / `version`
to use for the GitHub release tag — feeding it back into other tools is
expected.

Exit code is non-zero if the package or its manifest is missing or
malformed; the error message goes to stderr.

### Inspect a Package's Signature

For tooling that needs the raw `manifest.sig` JSON (e.g. an out-of-CI
index builder that reproduces what `logos-modules-release-action` records
under `sidecar.json#signature`):

```bash
# Raw manifest.sig bytes — byte-identical to the file inside the .lgx.
lgx signature mymodule.lgx > manifest.sig

# Or pipe straight to jq for the signer DID (signed packages only —
# unsigned ones print nothing, and the empty stream makes jq error
# out with a non-zero exit; gate the pipeline accordingly in scripts).
lgx signature mymodule.lgx | jq -r .did
```

Unsigned packages produce **no output** and exit 0 — callers tell "no
signature" apart from "error" by checking the exit status, not the stream
length. Bad/missing packages exit non-zero with the error on stderr.

### Generate a Signing Key

```bash
lgx keygen --name my-key
# Creates ~/.config/logos/keys/my-key.jwk (secret, JWK format)
#         ~/.config/logos/keys/my-key.pub (SSH format)
#         ~/.config/logos/keys/my-key.did (DID string)
# Prints the did:jwk:... DID to stdout

# Use a custom output directory
lgx keygen --name ci-key --output-dir /etc/logos/keys
```

### Sign a Package

```bash
lgx sign mymodule.lgx --key my-key
lgx sign mymodule.lgx --key my-key --name "My Organization" --url "https://example.com"

# Use keys from a custom directory
lgx sign mymodule.lgx --key ci-key --keys-dir /etc/logos/keys
```

Signing validates the package, then creates `manifest.sig` with the signer's DID
(`did:jwk:...`), an Ed25519 signature over the manifest bytes, and optional signer metadata.

### Manage Trusted Keys

```bash
# Add a trusted key by DID
lgx keyring add publisher-name did:jwk:eyJjcnYi... --display-name "Publisher" --url "https://..."

# List trusted keys
lgx keyring list

# Remove a trusted key
lgx keyring remove publisher-name

# Use a custom keyring directory
lgx keyring list --dir /etc/logos/trusted-keys
lgx keyring add ci-signer did:jwk:eyJj... --dir /etc/logos/trusted-keys
```

Trusted keys are stored as `.json` files in the keyring directory (default: `~/.config/logos/trusted-keys/`).

### Merge Packages

Merge multiple single-variant `.lgx` packages into one multi-variant package:

```bash
lgx merge linux.lgx darwin.lgx -o mymodule.lgx
```

All input packages must have identical manifests (except for the variant-specific `main` field). Fails on duplicate variants unless `--skip-duplicates` is used:

```bash
lgx merge pkg1.lgx pkg2.lgx pkg3.lgx --skip-duplicates -o mymodule.lgx -y
```

### Inspect Package Contents

Since `.lgx` files are just `tar.gz` archives:

```bash
tar -tzf mymodule.lgx
```

## Command Reference

| Command | Description |
|---------|-------------|
| `lgx create <name>` | Create a new skeleton package |
| `lgx add <pkg> --variant <v> --files <path> [--main <relpath>] [--view <relpath>] [-y]` | Add files to a variant |
| `lgx remove <pkg> --variant <v> [-y]` | Remove a variant |
| `lgx extract <pkg> [--variant <v>] [--output <dir>]` | Extract variant contents |
| `lgx merge <pkg1> <pkg2> ... [-o <output>] [--skip-duplicates] [-y]` | Merge packages into one |
| `lgx verify <pkg> [--keyring-dir <dir>]` | Validate package structure and signature |
| `lgx manifest <pkg> [--json]` | Print the embedded `manifest.json` (human-readable or raw bytes) |
| `lgx signature <pkg>` | Print the raw `manifest.sig` bytes (unsigned → empty + exit 0) |
| `lgx sign <pkg> --key <name> [--keys-dir <dir>] [--name "..."] [--url "..."]` | Sign package with Ed25519 key and DID identity |
| `lgx keygen --name <name> [--output-dir <dir>]` | Generate an Ed25519 signing keypair (outputs DID) |
| `lgx keyring add\|remove\|list [--dir <dir>]` | Manage trusted keys (by DID) |
| `lgx semver compare\|sort\|satisfies\|valid\|valid-range` | Compare, sort and range-match versions (see below) |
| `lgx publish <pkg>` | Publish package (TODO) |

## Semver

`lgx` owns the one semver implementation for the whole packaging stack. `lgpm`,
`lgpd` and the package-manager UI all include it directly
(`include/logos/semver.hpp`); everything else reaches it through `lgx semver`.
That matters most for the catalog builder
([logos-modules-release-tool](https://github.com/logos-co/logos-modules-release-tool)'s
`index.py`), which orders each package's `versions[]` — it shells out here
rather than reimplementing semver in Python, so the catalog can never disagree
with the clients about which version is newest.

Precedence is [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html) §10–§11 exactly:
a pre-release ranks below its own release, numeric pre-release identifiers compare
**numerically** (`1.0.0-rc.2` < `1.0.0-rc.11`), and build metadata is ignored.

```bash
lgx semver compare 1.0.0-rc.2 1.0.0-rc.11   # -> -1
lgx semver sort --desc 1.0.0 2.0.0-alpha 1.9.0
lgx semver satisfies 1.5.0 '^1.0.0'          # exit 0
lgx semver satisfies 2.0.0-alpha '^1.0.0'    # exit 1 — see the pre-release rule below
lgx semver valid 1.0.0                       # exit 0
lgx semver valid-range '>=1.0 <2.0'          # exit 0
```

**Ranges are npm's dialect, not the spec's** — SemVer 2.0.0 defines precedence
but says nothing about `^ ~ x * ||`. Supported: `^`, `~`, `=`, `>`, `>=`, `<`,
`<=`, `x`/`X`/`*` wildcards, hyphen ranges (`1.2.3 - 2.3.4`), whitespace
conjunction, and `||` alternation. A bare `1` or `1.5` is a partial version and
widens accordingly.

```bash
lgx semver satisfies 1.5.0 '1.2.3 - 2.3.4'   # exit 0 — inclusive at both ends
lgx semver satisfies 2.3.5 '1.2.3 - 2.3.4'   # exit 1
```

Note the spaces: `1.2.3 - 2.3.4` is a range, while `1.2.3-2.3.4` is a single
version whose pre-release is `2.3.4`.

Also following npm: **a range never matches a pre-release unless the range
itself names one at the same `major.minor.patch`.** So `^1.0.0` does *not* match
`2.0.0-alpha` — without that rule an unreleased alpha of the next major
satisfies a caret range on 1.x — while `^1.0.0-rc.1` still matches `1.0.0-rc.2`.

## Package Structure

```
mymodule.lgx (tar.gz)
├── manifest.json          # Package metadata
├── manifest.sig           # Optional - Ed25519 signature with DID identity
├── variants/
│   ├── linux-amd64/
│   │   └── libfoo.so
│   ├── darwin-arm64/
│   │   └── libfoo.dylib
│   └── web/
│       └── index.js
├── docs/                  # Optional
└── licenses/              # Optional
```

## Manifest schema

Current `manifestVersion` is **`0.3.0`** (introduced support for richer
dependency entries — see below). Tooling reads both `0.2.x` and `0.3.x`
manifests; new packages produced by `lgx create` use `0.3.0`.

```jsonc
{
  "manifestVersion": "0.3.0",
  "name": "mymodule",
  "version": "1.2.3",
  "description": "...",
  "author": "...",
  "type": "core",                     // "core" | "ui" | "ui_qml" | "library"
  "category": "...",
  "icon": "icon.png",
  "view": "qml/Main.qml",             // required for type == "ui_qml"
  "main": {                           // variant -> entry path
    "linux-amd64": "libfoo.so",
    "darwin-arm64": "libfoo.dylib"
  },
  "dependencies": [                   // see "Dependency entries" below
    "waku_module",
    {"name": "core_lib", "version": "^1.2.0"},
    {"name": "secure_store", "version": ">=0.5.0", "signer": "did:jwk:..."}
  ],
  "hashes": {                         // Merkle tree, recomputed on save
    "root": "...",
    "variants/linux-amd64": "...",
    "...": "..."
  }
}
```

The full schema (field constraints, completeness rules, `ui_qml`
contract) lives in [`docs/spec.md`](docs/spec.md).

### Dependency entries

Each `dependencies[]` element is one of:

- **Plain string** (legacy 0.2.x form, still supported):
  `"waku_module"` is equivalent to `{"name": "waku_module"}` —
  "any version, any signer".
- **Object** with:
  - `name` (string, required) — canonical lowercase package name.
  - `version` (string, optional) — npm/Cargo-style semver range:
    `^1.2.0`, `~1.2.3`, `>=1.2 <2.0`, `1.2.x`, `*`, `||` for
    alternatives. Absent ⇒ any version.
  - `signer` (string, optional) — `did:jwk:...` identifying the
    trusted publisher. Absent ⇒ any signer. When set, only packages
    whose `manifest.sig` was produced by that DID match —
    useful for disambiguating same-named packages from different
    publishers.

`lgx verify` syntactically validates that `version` parses as a
semver range and that `signer` matches the `did:jwk:` shape; the
semantic resolve (does the constraint actually find a candidate?)
happens client-side in `logos-package-downloader`. Both use the same
implementation (see [Semver](#semver)) — validation used to be a
separate regex here, which accepted ranges the resolver could not
actually evaluate.

Note the pre-release rule: `"version": "^1.2.0"` will **not** resolve to
`2.0.0-alpha` or `1.5.0-beta.1`. To depend on a pre-release, name one:
`"^1.2.0-rc.1"`.

## Example Workflow

```bash
# Create package
lgx create mylib

# Add variants
lgx add mylib.lgx -v linux-amd64 -f ./out/linux/libmylib.so -y
lgx add mylib.lgx -v darwin-arm64 -f ./out/macos/libmylib.dylib -y

# Verify
lgx verify mylib.lgx

# Inspect the manifest (name, version, variants, dependencies, signer)
lgx manifest mylib.lgx

# Optional: sign the package
lgx keygen --name my-key
lgx sign mylib.lgx --key my-key --name "My Org" --url "https://example.com"

# Inspect the archive contents
tar -tzf mylib.lgx

# Extract for use
lgx extract mylib.lgx --variant linux-amd64 --output ./extracted
```

## Building

### Nix (Recommended)

The recommended way to build `lgx` is using Nix, which automatically handles all dependencies:

#### Binary

```bash
nix build '.#lgx'
```

The binary will be available at `./result/bin/lgx`.

#### Library

To build the library (.so/.dylib/.dll):

```bash
nix build '.#lib'
```

The library will be available at:
- `./result/lib/liblgx.dylib` (macOS)
- `./result/lib/liblgx.so` (Linux)
- `./result/lib/lgx.dll` (Windows)

The C API header will be at `./result/include/lgx.h`.

#### Running Tests with Nix

Tests run automatically during `nix build`. The build will fail if any tests do not pass.

**Note:** If you haven't enabled flakes, you'll need to add the experimental features flag:

```bash
nix --extra-experimental-features "nix-command flakes" build '.#lgx'
```

Or enable flakes permanently in your Nix configuration by adding to `~/.config/nix/nix.conf`:

```
experimental-features = nix-command flakes
```

### CMake

If you prefer not to use Nix, you can build with CMake directly.

#### Prerequisites

- CMake 3.16+
- C++17 compiler (GCC 8+, Clang 7+, MSVC 2019+)
- zlib
- ICU (or, on Apple platforms, `-DLGX_UNICODE_COREFOUNDATION=ON` to use
  CoreFoundation for Unicode normalization instead; iOS has no ICU)

##### macOS (Homebrew)

```bash
brew install cmake icu4c
```

##### Ubuntu/Debian

```bash
sudo apt install cmake libicu-dev zlib1g-dev
```

#### Building

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

The `lgx` executable will be created in the `build/` directory.

#### Building the Library

To build the library:

```bash
mkdir build
cd build
cmake .. -DLGX_BUILD_SHARED=ON
make -j$(nproc)
```

This will create:
- `build/liblgx.dylib` (macOS) or `build/liblgx.so` (Linux) or `build/lgx.dll` (Windows)
- The C API header is at `src/lgx.h`

`-DLGX_STATIC_CABI=ON` builds the same C ABI as a static archive
(`build/liblgx.a`) instead, for hosts that cannot load a shared library
(iOS). The `lgx` CLI is not built when `CMAKE_SYSTEM_NAME` is `iOS`.

#### Building with Tests

```bash
mkdir build
cd build
cmake .. -DLGX_BUILD_TESTS=ON -DLGX_BUILD_SHARED=ON
make -j$(nproc)
```

### Running Tests

```bash
cd build
ctest --output-on-failure
```

