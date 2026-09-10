# LGX Package

## Overall Description

LGX is a deterministic package format for distributing multi-platform artifacts. It provides a standardized way to bundle platform-specific binaries, libraries, or other files into a single distributable archive with strong guarantees about reproducibility and integrity.

The format is designed to:
- Support multiple platform variants (e.g., `linux-amd64`, `darwin-arm64`) in a single package
- Produce byte-identical archives given identical inputs (deterministic)
- Ensure cross-platform path compatibility through Unicode NFC normalization (macOS often uses decomposed forms; Linux commonly uses composed - NFC avoids "same name, different bytes" breaking lookups, hashing, and determinism)
- Enforce a strict, predictable internal structure
- Provide tooling for creating, modifying, and validating packages

## Definitions & Acronyms

| Term | Definition |
|------|------------|
| **LGX** | Logos Package Format - the package format specified in this document |
| **Variant** | A platform-specific or configuration-specific build of the package contents (e.g., `linux-amd64`) |
| **NFC** | Unicode Normalization Form C - a Unicode normalization form that uses canonical decomposition followed by canonical composition |
| **USTAR** | Unix Standard TAR - a standardized tar archive format |
| **Manifest** | The `manifest.json` file containing package metadata |
| **Main** | The entry point file for each variant, specified in the manifest |
| **Ed25519** | An elliptic-curve digital signature algorithm used for package signing |
| **Merkle Tree** | A hierarchical hash structure used to verify package content integrity |

## Domain Model

### Package Structure

An LGX package (`.lgx` file) is a gzip-compressed tar archive with the following structure. Gzip is used because it's the most universally supported compression with stable tooling on every OS, providing the simplest default for "any platform can unpack".

```
package.lgx (tar.gz)
├── manifest.json          # Required - package metadata
├── manifest.sig           # Optional - Ed25519 signature with DID identity
├── assets/                # Optional - variant-independent package assets
│   └── icon.png           #   Package icon: PNG, exactly 256x256
├── variants/              # Required - contains variant directories
│   ├── <variant-1>/       # Variant directory (lowercase name)
│   │   └── ...            # Variant contents
│   └── <variant-2>/
│       └── ...
├── docs/                  # Optional - documentation
│   └── ...
└── licenses/              # Optional - license files
    └── ...
```

**Root Entry Constraints:**
- Only `manifest.json`, `manifest.sig`, `assets/`, `variants/`, `docs/`, and `licenses/` are permitted at root
- Any other root entries cause validation failure
- Files directly under `variants/` are forbidden (only directories allowed)
- This strict structure keeps packages easy to validate and reduces ambiguity

### Manifest Schema

The current manifest schema is `0.6.0`. It is a UTF-8 encoded JSON file with the following required fields:

```json
{
  "manifestVersion": "0.6.0",
  "name": "package-name",
  "version": "1.2.3",
  "description": "Package description",
  "author": "Author Name",
  "type": "library",
  "category": "crypto",
  "icon": "assets/icon.png",
  "dependencies": [
    "simple-dep",
    {"name": "ranged-dep", "version": "^1.2.0"},
    {"name": "pinned-dep", "version": ">=0.5.0", "signer": "did:jwk:..."}
  ],
  "main": {
    "linux-amd64": "path/to/main.so",
    "darwin-arm64": "path/to/main.dylib"
  },
  "provides": [
    {"intent": "chat.group.open"}
  ]
}
```

**Field Constraints:**

| Field | Type | Constraints | Purpose |
|-------|------|-------------|---------|
| `manifestVersion` | string | Semver format; tooling rejects unsupported major versions | Version compatibility |
| `name` | string | Canonical lowercase; auto-normalized by tooling | Package identity |
| `version` | string | Package version (semver recommended) | Package identity |
| `description` | string | Human-readable description | Human metadata |
| `author` | string | Author/maintainer name | Human metadata |
| `type` | string | Package type classification | Classification |
| `category` | string | Package category | Classification |
| `icon` | string | Relative path to the icon bundled in the package. At `0.4.0`+ this is `assets/icon.png` — see *Icon contract* below | Display/branding |
| `dependencies` | array | List of dependency entries — see *Dependency entries* below | Runtime needs |
| `optional_dependencies` | array | *Optional, 0.6.0+.* Dependency entries (same two forms) the package can call but does **not** require. An absent one is not a broken install | Runtime needs |
| `interface_dependencies` | array of strings | *Optional, 0.6.0+.* Interface **names** the module binds to a provider at runtime. Not resolvable to a package | Display/discovery |
| `main` | object | Map of variant name → relative path to entry point (e.g ) `"linux-amd64": "path/to/main.so"` means `linux-amd64/path/to/main.so` | Entry point resolution |
| `display_name` | string | *Optional.* Human-readable label shown by UI consumers (Package Manager, App Manager) and CLI tools (`lm metadata`, `lgx manifest`). Falls back to `name` when absent. | Display/branding |
| `provides` | array | *Optional.* Intents this package can service — see *Provided intents* below. Absent ⇒ the package services none. | Capability discovery |

All fields except `display_name`, `provides`, `optional_dependencies` and `interface_dependencies` are required to ensure consistent metadata for hosts/registries and applications. Each of those is omitted entirely when empty, so a package that uses none of them serialises exactly as earlier tooling produced it.

#### Optional and interface dependencies

These are the two things a module can name that are **not** part of a complete install.

`optional_dependencies` entries take the same two forms as `dependencies` and are validated the same way. What differs is what an installer must do with them: it **may** offer to install one, and it **must not** report the package as broken when one is missing. The runtime never auto-loads them and never fails a load over their absence.

`interface_dependencies` entries are bare **names**. An interface is bound to a concrete provider at runtime by the consuming module, so there is nothing here for an installer to resolve — the names are carried so a catalog or UI can show what a module expects to find. The author's `metadata.json` form is an object carrying `file` / `input` / `impl_class`; those are paths into flake inputs and a source tree and are dropped at bundle time, exactly as intent `params` are (only intent names reach the manifest). A manifest that carries the object form is rejected rather than silently truncated.

#### Dependency entries

Each element of the `dependencies` array is one of:

- **Plain string** (legacy 0.2.x form, still supported): equivalent to `{"name": <string>}`. Means "any version, any signer".
- **Object** with:
  - `name` (string, required) — canonical lowercase package name.
  - `version` (string, optional) — npm/Cargo-style semver range (`^1.2.0`, `~1.2.3`, `>=1.2 <2.0`, `1.2.x`, `*`, `||` for alternatives, ...). Absent ⇒ any version.
  - `signer` (string, optional) — `did:jwk:...` DID identifying the trusted publisher. Absent ⇒ any signer. When set, only packages whose `manifest.sig` was produced by that DID match. Used to disambiguate same-named packages from different publishers.

`lgx verify` syntactically validates each entry and fails the package on any of:

| Rule | Error |
|------|-------|
| `name` is non-empty | `Dependency with empty name` |
| `name` is canonical lowercase | `Dependency name '<n>' is not lowercase` |
| `version`, when present, parses as a range | `Dependency '<n>' has invalid semver range: '<r>'` |
| `signer`, when present, matches `^did:jwk:[A-Za-z0-9_-]+$` | `Dependency '<n>' has invalid signer DID: '<d>'` |

An empty `name` short-circuits the remaining checks for that entry, so one broken entry yields one error.

Ranges are the npm dialect **minus hyphen ranges**: `1.2.3 - 2.3.4` is rejected outright rather than
misread as a version with a `-2.3.4` pre-release suffix. Use `>=1.2.3 <=2.3.4` instead. Note the
converse is *not* rejected: `1.2.3-2.3.4` (no spaces) is a legal SemVer pre-release version and is
accepted as an **exact** match on that pre-release — it does not denote a range.

Semantic matching (does the constraint resolve to a real candidate?) is the responsibility of the
resolver in `logos-package-downloader`. That resolver uses `signer` to *select* among same-named
candidates; it is not an authorization check. Whether a package may be installed at all is decided
separately by the installer's signature policy (see *Install-Time Verification (lgpm)*).

#### Provided intents

At `manifestVersion` `0.5.0`+, `provides` lists the **app-to-app intents** the package
can service, e.g. `chat.group.open`. Each element is one of:

- **Object** with `intent` (string, required) — the intent name.
- **Plain string**, accepted on read and equivalent to `{"intent": <string>}`.

Entries are **normalized to the object form on write**, so a reader never sees two
shapes. Entries that are neither a string nor an object carrying a string `intent`,
and entries whose `intent` is empty, are skipped rather than failing the parse. The
array is copied from the reference package by `lgx merge`, so a multi-variant package
keeps the capability claim of the variants it was built from.

**Names only.** The author's `metadata.json` may also describe each intent's payload
shape (`provides[].params`); none of that is carried here. The manifest copy exists to
answer one question, asked *before* a package is installed: "which installable package
provides X?" — which the name alone answers. The shell enforces the payload shape
against the installed `metadata.json`, which stays the source of truth for it.

**Why the signed manifest rather than only `metadata.json`.** `metadata.json` is
unsigned and only readable once the package is on disk. Putting the claim in
`manifest.json` covers it by `manifest.sig`, so a package cannot claim a capability it
was not published with, and makes it legible to a catalog or registry that has the
manifest but has never unpacked — let alone installed — the package.

**Declaration, not authorization.** `provides` says what a package *can* service. It
grants nothing: resolution among candidates, user consent and dispatch are the shell's
(see `logos-basecamp`'s `IntentBroker`), and `lgx` neither validates intent names
against a registry nor checks that the payload implements them.

#### Schema version compatibility

Tooling reads `manifestVersion` `0.2.x` through `0.6.x`; packages produced by `lgx create`
use `0.6.0`. Every field added across those versions is **optional**, so compatibility runs
both ways: an older client reading a newer manifest ignores what it does not recognize
rather than failing, which is why the check is on the major version alone. A 0.2.0 manifest
with plain-string dependencies round-trips unchanged through tooling — strings are emitted
as strings, object-form entries are emitted as objects. Two contracts are gated on the minor
version rather than applied retroactively: the *icon contract* (`0.4.0`+) and `provides`
(`0.5.0`+). `0.6.0` adds `optional_dependencies` and `interface_dependencies`; the version
was bumped rather than widening `0.5.0` in place, because two documents claiming one version
with different key sets is a distinction nothing downstream can recover. Bumping the major
version (1.x.x) is reserved for future breaking changes.

### Icon Contract

At `manifestVersion` `0.4.0`+, `icon` points at a **root-level, variant-independent
asset**: `assets/icon.png`.

**Requirements**

| Rule | Value |
|------|-------|
| Format | PNG |
| Dimensions | **exactly** 256x256 — not a minimum |
| Location | `assets/icon.png` (canonical; set by `lgx add --icon`) |
| Required for | `type == "ui_qml"` |
| Optional for | every other type, including `core` |

**Why root-level rather than per-variant.** One copy instead of one per
platform, and a host can read the icon without choosing or unpacking a
platform build — which is what lets a registry extract it at publish time and
serve it to clients that have not downloaded the package. `assets/` is inside
the Merkle tree, so the icon is authenticated by `manifest.sig` for free.

**Why exactly 256x256.** The artifact stays byte-predictable: the icon in
`assets/` is the author's file unmodified, so the content hash is reproducible
from source without depending on an image library's resampling behaviour. It
also keeps the packaging step free of an image-processing dependency —
dimension validation is a fixed-offset PNG `IHDR` read.

**Why `ui_qml` only.** UI packages render a tile in the App Manager, sidebar
and launcher. Core modules appear in package lists but have no such surface,
so requiring artwork from them would be cost without benefit.

**Validation timing.** `lgx create` and `lgx add` are construction steps and do
not enforce the contract — a package may be assembled in any order. Enforcement
happens at `lgx verify`, `lgx sign` and `lgpm install`, i.e. wherever a package
is asserted to be complete.

**Backward compatibility.** Manifests below `0.4.0` are **exempt**. `icon: ""`
was legal at `0.3.0` (it is what `lgx create` defaulted to), so applying the
rule unconditionally would make every already-published package fail
verification and become uninstallable. Tooling reads `0.2.x` through `0.5.x`;
only `0.4.0`+ carries the icon contract.

### `ui_qml` Contract

For most package types, `main` remains the per-variant entry point and is required.

For `type == "ui_qml"`, the contract is:

- `view` (**required**): relative path to the QML entry point. Interpreted from the installed package root and identical across variants.
- `main` (**optional**): per-variant backend Qt plugin library. When present, the host runs it in an isolated `ui-host` process and bridges it to the QML view; when absent, the QML view is loaded directly in-process.

`view` must be set for every `ui_qml` package. `main` is set only when the module ships a backend C++ plugin.

Example `ui_qml` manifest with a backend plugin:

```json
{
  "manifestVersion": "0.3.0",
  "name": "package-manager-ui",
  "version": "1.0.0",
  "description": "Package manager UI",
  "author": "Logos",
  "type": "ui_qml",
  "category": "ui",
  "icon": "modules.png",
  "dependencies": [],
  "view": "qml/PackageManager.qml",
  "main": {
    "linux-amd64": "lib/package_manager_ui_plugin.so",
    "darwin-arm64": "lib/package_manager_ui_plugin.dylib"
  }
}
```

Example `ui_qml` manifest without a backend (QML-only):

```json
{
  "manifestVersion": "0.3.0",
  "name": "calc-ui",
  "version": "1.0.0",
  "description": "Calculator QML UI",
  "author": "Logos",
  "type": "ui_qml",
  "category": "tools",
  "icon": "",
  "dependencies": ["calc_module"],
  "view": "Main.qml",
  "main": {}
}
```

**Main Entry Constraints:**
- Keys must be lowercase (auto-normalized)
- Values must be valid relative paths (no absolute paths, no `..` segments)
- Values must be NFC-normalized
- Each path must resolve to an existing regular file within the variant directory

**View Entry Constraints:**
- `view` is required for `type == "ui_qml"`; ignored for other types
- `view` must be a string
- `view` must be a valid relative archive path
- `view` is interpreted relative to the installed package root

### Variant Structure

- Variant names are drawn from the platform vocabulary below, stored in lowercase (case-insensitive behavior avoids Windows/macOS filesystem quirks and human typos; canonical lowercase makes matching deterministic)
- Variant directories contain platform-specific files
- The directory structure within a variant is preserved from source

**Completeness Constraint:**
- Every `main` entry must have a corresponding variant directory
- For non-`ui_qml` packages: every variant directory must have a corresponding `main` entry
- For `ui_qml` packages: `main` is optional. When present, the same `main`/variant correspondence applies; when absent, every variant simply ships the QML view referenced by `view`
- This ensures installers don't have to guess entrypoints

### Platform Variant Vocabulary

A variant name is a key inside the signed hash tree (`hashes["variants/<name>"]`), so a
published package can never be renamed on disk. That makes the vocabulary this library's
to own: every consumer (`lgpm`, `lgpd`, the module builder) reads it from here rather than
tabulating its own.

**Canonical names** — one per target this ecosystem ships:

| Target | Canonical variant | Also accepted (legacy) |
|---|---|---|
| Linux x86-64 | `linux-x86_64` | `linux-amd64` |
| Linux arm64 | `linux-arm64` | `linux-aarch64` |
| macOS x86-64 | `darwin-x86_64` | `darwin-amd64` |
| macOS arm64 | `darwin-arm64` | `darwin-aarch64` |
| Windows x86-64 | `windows-x86_64` | `windows-amd64` |
| Windows arm64 | `windows-arm64` | `windows-aarch64` |
| Android arm64 | `android-arm64` | `android-aarch64` |
| Android x86-64 | `android-x86_64` | `android-amd64` |
| iOS device | `ios-arm64` | `ios-aarch64` |
| iOS simulator (Apple silicon) | `ios-sim-arm64` | `ios-sim-aarch64` |
| Web container | `web` | — |

`linux-x86`, `windows-x86` and `ios-sim-x86_64` are also part of the vocabulary because
host detection can still compute them; nothing in the ecosystem builds for them today.

**Fallback order.** A host looks for its own spelling first, then the other live spelling
of its *architecture* half, and stops. The OS half is matched verbatim, which is what keeps
a Windows package from installing as a macOS one — and, now that mobile is in the table,
keeps `android` apart from `linux` (shared kernel, different ABI and packaging) and
`ios-sim` apart from `ios` (same chip, different ABI: a device framework does not load on
the simulator). So `ios-arm64` yields `[ios-arm64, ios-aarch64]` and nothing else.

`web` has no architecture half — the Web container runs the same bytes everywhere — so it
resolves to itself alone and **no native host falls back to it**. A web payload needs the
container; a host without one would install JavaScript where it loads a plugin.

A non-portable build of a consumer appends `-dev` to every name it will accept. That
suffix is a property of the consumer's build, not of a target, so it rides on top of the
table rather than doubling it.

**Unknown spellings.** `lgx add` and `lgx verify` reject a variant name that is a
misspelling of one in the table — an Apple SDK or Android ABI name (`iphoneos-arm64`,
`arm64-v8a`), a toolchain name for the web target (`wasm`, `emscripten`), a separator or
case slip (`ios_arm64`, `linux-x8664`) — and name the canonical spelling that was meant.
Such a package resolves on no host at all, so the error belongs at build time rather than
on a user's device. A name that resembles nothing in the table is left alone: private
targets exist and this vocabulary does not own the whole namespace.

## Features & Requirements

### Determinism Requirements

LGX packages must be deterministic - identical inputs must produce byte-identical outputs.

**Tar Determinism:**
- Entries sorted lexicographically by NFC-normalized path bytes
- Fixed metadata: `uid=0`, `gid=0`, `uname=""`, `gname=""` (tar headers include uid/gid/mtime/mode; normalizing them prevents host-specific differences from changing checksums)
- Fixed timestamps: `mtime=0`
- Fixed permissions: directories `0755`, files `0644`
- USTAR format

**Gzip Determinism:**
- Header mtime = 0
- No original filename in header
- Fixed OS byte (0xFF = unknown)
- Gzip headers can embed timestamps/filenames/OS markers; zeroing them prevents two builds from differing despite identical content

### Path Safety Rules

All archive paths must satisfy:
- Not absolute (no leading `/`)
- No `..` segments after normalization
- Not empty
- No backslash characters (`\`)
- Unicode NFC-normalized

These rules are enforced both when verifying a package (`lgx verify`) **and at
extraction time**. Extraction (`lgx extract`, `lgpm install`, and the
`lgx_extract` C API) re-validates every entry path and additionally checks that
the resolved destination stays inside the output directory before any file or
directory is written. A crafted package whose entry escapes the variant root
(e.g. `variants/<variant>/../../etc/...`) is rejected with an error and **no
files are written outside the target directory** — even on the unsigned /
`--allow-unsigned` path, which does not run full package verification. This
prevents zip-slip / path-traversal arbitrary file writes from an untrusted
`.lgx`.

**Forbidden File Types:**
- Symlinks
- Hardlinks
- Device nodes
- FIFOs

These are forbidden for portability and security: links can escape variant roots or behave differently on extract; special files are unsafe/meaningless for plugins.

### Decompression Limits

A `.lgx` is a gzip-compressed tar archive, and DEFLATE can reach compression
ratios on the order of 1000:1. Left unbounded, a small crafted archive could
inflate to gigabytes when loaded and exhaust the host's memory — a
"decompression bomb" that OOM-kills or hangs the process loading it (basecamp
and every in-process module).

To prevent this, decompression enforces a **hard cap on total decompressed
output** (1 GiB by default). The gzip reader tracks a running total as it
inflates and rejects the stream the moment the output would exceed the cap,
before the excess bytes are allocated — so the cost of an oversized archive is
bounded regardless of how small the compressed input is. Loading an untrusted
`.lgx` (`lgx verify`, `lgpm install`, signature inspection, the `lgx_*` C API)
runs through this guard, which also bounds the buffer subsequently handed to the
tar reader. A package whose contents exceed the cap is rejected with an error
and no oversized buffer is ever materialized.

The cap applies to the **whole archive** — the total size of the decompressed
tar (every entry plus tar overhead), not any single file within it. It is
configurable by embedders of the library: globally via
`GzipHandler::setDefaultMaxDecompressedSize(bytes)` (affects every load that
does not specify its own limit) or per call via the `maxOutputSize` argument to
`decompress` / `decompressStream`. There is no "unlimited" setting — a `0`
value is rejected — so the protection cannot be turned off by misconfiguration.

### Package Creation Workflow

```
lgx create <name>
```

1. Normalize `name` to lowercase
2. Check if `<name>.lgx` already exists; if so, exit with error
3. Create skeleton manifest with default values:
   - `name`: normalized lowercase name
   - `version`: `"0.0.1"`
   - `description`: `""`
   - `author`: `""`
   - `type`: `""`
   - `category`: `""`
   - `icon`: `""`
   - `dependencies`: `[]`
   - `main`: `{}`
4. Create empty `variants/` directory
5. Write deterministic tar.gz to `<name>.lgx`

### Variant Addition Workflow

```
lgx add <pkg.lgx> --variant <v> --files <path> [--main <relpath>] [--view <relpath>] [-y]
```

1. Load existing package
2. Verify package file exists; if not, exit with error
3. Verify files path exists; if not, exit with error
4. Normalize variant name to lowercase, and reject a misspelling of a known variant, naming the canonical spelling
5. Determine effective main path:
   - If `--files` is a directory: `--main` is required except for `ui_qml` packages, where `view` is the required entry point and `main` is optional backend metadata
   - If `--files` is a single file: use basename if `--main` not provided
6. Check if confirmation is needed (unless `-y`):
   - If variant exists and will be replaced
   - If `main[variant]` would change (even if variant doesn't exist yet)
   - If both variant exists and `main` would change
7. **Replace** entire variant directory (no merging - a variant is treated as an atomic build output; merge risks stale leftovers and hard-to-debug installs)
8. Copy files/directory to `variants/<variant>/`
   - Single file: `variants/<variant>/<filename>`
   - Directory: `variants/<variant>/...` (contents placed directly)
9. Update `main[variant]` entry only when an effective main path exists
10. Validate and save package

**Confirmation Required When:**
- Replacing existing variant
- Changing existing `main` entry (even if variant is new)
- Both variant replacement and `main` change

**Option Aliases:**
- `-v` for `--variant`
- `-f` for `--files`
- `-m` for `--main`
- `-y` for `--yes`

### Variant Removal Workflow

```
lgx remove <pkg.lgx> --variant <v> [-y]
lgx remove <pkg.lgx> -v <v> [-y]
```

1. Load existing package
2. Verify package file exists; if not, exit with error
3. Verify variant exists; if not, exit with error
4. Prompt for confirmation (unless `-y`)
5. Remove variant directory entries
6. Remove `main[variant]` entry
7. Save package

**Option Aliases:**
- `-v` for `--variant`
- `-y` for `--yes`

### Variant Extraction Workflow

```
lgx extract <pkg.lgx> [--variant <v>] [--output <dir>]
```

1. Verify package file exists; if not, exit with error
2. Load existing package
3. If `--variant` specified:
   - Normalize variant name to lowercase
   - Verify variant exists; if not, exit with error
   - Extract variant contents to `<output>/<variant>/`
4. If `--variant` not specified:
   - Extract all variants to `<output>/<variant>/` for each variant
5. For each entry, enforce the [Path Safety Rules](#path-safety-rules):
   reject any entry with an unsafe path (absolute, `..` segment, backslash,
   non-NFC) and reject any entry whose resolved destination would fall outside
   `<output>/<variant>/`. On rejection, extraction fails with an error and no
   file outside the output directory is written.
6. Create directories as needed
7. Write files preserving internal directory structure

**Output Structure:**
- Each variant is extracted to `<output>/<variant-name>/`
- The internal variant structure (from `variants/<variant>/`) is preserved
- For example, `variants/linux-amd64/lib.so` extracts to `<output>/linux-amd64/lib.so`

**Option Aliases:**
- `-v` for `--variant`
- `-o` for `--output`

### Package Merge Workflow

```
lgx merge <pkg1.lgx> <pkg2.lgx> ... [-o <output.lgx>] [--skip-duplicates] [-y]
```

1. Verify all input package files exist; if any missing, exit with error
2. Load all input packages
3. Compare manifests across all inputs, ignoring the `main` field (which is variant-specific):
   - All non-variant fields must be identical (`manifestVersion`, `name`, `version`, `description`, `author`, `type`, `category`, `icon`, `dependencies`, `optional_dependencies`, `interface_dependencies`, `display_name`). Two builds that disagree about what a package can call are not two variants of one package
   - If any mismatch is found, report all mismatching fields and exit with error
4. Check for duplicate variants across all input packages:
   - By default, exit with error if any variant appears in more than one input
   - With `--skip-duplicates`: warn and keep only the first occurrence
5. Determine output path:
   - Use `--output` if provided
   - Otherwise default to `<name>.lgx` (from the manifest name)
6. If output file exists, prompt for confirmation (unless `-y`)
7. Create a fresh skeleton package with the shared metadata
8. For each input package, for each variant:
   - Extract variant files to a temporary directory
   - Add variant to the output package using the standard `addVariant` flow
   - Preserve the `main` entry from the source package
9. Save merged package
10. Clean up temporary files

**Manifest Comparison:**
The merge command compares all manifest fields except `main`, which is expected to differ across platform-specific builds. This ensures the merged package represents the same logical module across all variants.

**Option Aliases:**
- `-o` for `--output`
- `-y` for `--yes`

### Manifest Inspection Workflow

```
lgx manifest <pkg.lgx> [--json]
```

Reads `manifest.json` from inside the package and prints it.

- Without `--json`: human-readable summary including name, version, type, category, author, root hash, variants (the keys of `main`), dependencies, and signer DID (when the package is signed).
- With `--json`: the raw `manifest.json` bytes are written to stdout verbatim — byte-identical to the file embedded in the `.lgx`. This is intended for tooling (e.g. `logos-modules-release-action`) that needs to capture the manifest exactly as it appears in the package.

Exit code: `0` on success, non-zero if the package or its manifest is missing/malformed.

### Verification Workflow

```
lgx verify <pkg.lgx> [--keyring-dir <dir>]
```

Options:
- `--keyring-dir <dir>` — Keyring directory for trust lookup (default: `~/.config/logos/trusted-keys/`)

1. Verify package file exists; if not, exit with error
2. Load and validate package

**Validation Checks:**
1. Archive is valid tar.gz
2. Root layout restrictions enforced
3. All manifest required fields present
4. Manifest version is supported (major version check)
5. All paths are NFC-normalized
6. Completeness constraint satisfied (variants ↔ main)
7. Each `main` entry points to existing regular file
8. No forbidden file types present
9. Content hashes present and valid (Merkle tree root hash matches recomputed value)

**Output:**
- Errors: Validation failures that prevent package from being valid
- Warnings: Non-fatal issues that should be noted
- Exit code: `0` if valid, `1` if validation failed

## CLI Interface

### Global Options

All commands support the following global options:

- `--help, -h`: Show help information (global or command-specific)
- `--version, -V`: Show version information

### Exit Codes

- `0`: Success
- `1`: Error (validation failure, file not found, etc.)

### Error Handling

Commands perform validation before operations:
- File existence checks for package files and source paths
- Required option validation with clear error messages
- Confirmation prompts for destructive operations (unless `-y` flag is used)

## Package Signing

### Overview

Packages can be cryptographically signed using Ed25519 via libsodium. Signing creates a `manifest.sig` file containing the signer's DID (`did:jwk:...`), the Ed25519 signature over `manifest.json`, and optional signer metadata.

### Content Hashes

Content hashes (Merkle tree) are **always present** in `manifest.json`, regardless of whether the package is signed. They are automatically recomputed whenever package content is modified (adding/removing variants). The `lgx verify` command validates these hashes for all packages.

### Sign Command

```
lgx sign <pkg.lgx> --key <name> [--keys-dir <dir>] [--name "Display Name"] [--url "https://..."]
```

Signs a package by:
1. Validating the package (structure and content hashes)
2. Creating an Ed25519 detached signature over the exact bytes of `manifest.json`
3. Writing `manifest.sig` with the signer's DID, signature, and optional metadata

Options:
- `--key, -k <name>` — Name of the signing key (required)
- `--keys-dir, -d <dir>` — Directory containing key files (default: `~/.config/logos/keys/`)
- `--name <display-name>` — Signer display name (self-asserted metadata)
- `--url <url>` — Signer URL (self-asserted metadata)

The secret key is loaded from `<keys-dir>/<name>.jwk`.

### Keygen Command

```
lgx keygen --name <name> [--output-dir <dir>]
```

Generates an Ed25519 signing keypair.

Options:
- `--name, -n <name>` — Name for the keypair (required)
- `--output-dir, -o <dir>` — Directory to write key files (default: `~/.config/logos/keys/`)

Files created:
- `<dir>/<name>.jwk` — Secret key (JWK format, permissions 0600)
- `<dir>/<name>.pub` — Public key (SSH public key format)
- `<dir>/<name>.did` — DID string (plain text `did:jwk:...`)

Prints the `did:jwk:...` DID to stdout.

#### Secret Key Format (JWK)

```json
{
  "crv": "Ed25519",
  "d": "<base64url-encoded 32-byte private seed>",
  "kty": "OKP",
  "x": "<base64url-encoded 32-byte public key>"
}
```

Keys are sorted alphabetically for determinism. The `d` field contains the 32-byte Ed25519 seed (not libsodium's 64-byte expanded key).

### Keyring Command

```
lgx keyring add <name> <did:jwk:...> [--display-name "..."] [--url "..."] [--dir <dir>]
lgx keyring remove <name> [--dir <dir>]
lgx keyring list [--dir <dir>]
```

Options:
- `--dir, -d <dir>` — Keyring directory (default: `~/.config/logos/trusted-keys/`)

Manages trusted keys stored as `.json` files in the keyring directory:

```json
{
  "did": "did:jwk:eyJjcnYi...",
  "name": "Logos Foundation",
  "url": "https://logos.co",
  "addedAt": "2026-04-06T12:00:00Z"
}
```

### DID Identity

Signers are identified by **DID (Decentralized Identifier)** strings using the `did:jwk` method:

```
did:jwk:<base64url({"crv":"Ed25519","kty":"OKP","x":"<base64url-pubkey>"})>
```

Construction:
1. Take 32-byte Ed25519 public key
2. Base64url-encode it (RFC 4648 §5, no padding) → the `x` value
3. Construct minimal JWK: `{"crv":"Ed25519","kty":"OKP","x":"<x>"}` (keys sorted alphabetically)
4. Base64url-encode the JWK JSON string
5. Prepend `did:jwk:`

The DID is deterministic from the public key — the same key always produces the same DID string.

### manifest.sig Format

```json
{
  "algorithm": "ed25519",
  "did": "did:jwk:eyJjcnYiOiJFZDI1NTE5Iiwia3R5IjoiT0tQIiwieCI6IjExcVlBWUt4Q3JmVlNfN1R5V1FIT2c3aGN2UGFwaU1scndJYWFQY0hVUm8ifQ",
  "linkedDids": [],
  "signature": "<base64-encoded 64-byte Ed25519 signature>",
  "signer": {
    "name": "Logos Foundation",
    "url": "https://logos.co"
  },
  "version": 1
}
```

- `did` — signer identity as a `did:jwk:` string. The public key is encoded in the DID itself.
- `signer` — optional, self-asserted metadata (display name, URL). Not covered by the signature. Used for trust prompts.
- `linkedDids` — reserved for future `did:pkh` entries (blockchain-verified identity). Always `[]` for now. The current type is `string[]` as a placeholder; the final schema will evolve to `{did, proofType, proof}[]` when did:pkh is implemented — each entry will carry a cryptographic proof (e.g., CACAO/EIP-4361) binding the blockchain identity to the signer's `did:jwk`. This is not a breaking change since the field is currently always empty.

### Merkle Tree Hashing

The `hashes` field in `manifest.json` is a map of directory paths to SHA-256 hex digests. Hashes are always present and kept up to date whenever content changes.

- **File hashes**: SHA-256 of each file's content
- **Leaf directory hashes** (e.g., `variants/darwin-arm64`): Sort files by relative path, concatenate `(path + '\0' + file_hash + '\n')`, SHA-256 the result
- **Parent directory hashes** (e.g., `variants`): Sort child directory names, concatenate `(dirname + '\0' + child_hash + '\n')`, SHA-256 the result
- **Root hash**: Same algorithm over all top-level entries

### Signature Invalidation

Any operation that modifies package content:
- Clears the existing signature (`manifest.sig`)
- Recomputes content hashes in `manifest.json`
- `save()` without a valid signature does NOT write `manifest.sig`

### Verification

`lgx verify <pkg.lgx>` performs:
1. Structural validation (existing checks)
2. Content hash validation — recomputes Merkle tree and verifies root hash matches (all packages)
3. If `manifest.sig` is present: verify Ed25519 signature, report signer DID and trust status

### Install-Time Verification (lgpm)

`lgpm install` verifies signatures before extracting packages:
- **Default (warn)**: Unsigned packages accepted with a warning; signed packages from unknown signers are accepted with trust info in the response
- `--allow-unsigned`: No signature checking
- `--require-signatures`: Reject unsigned packages and packages signed by untrusted keys

Keyring management (adding/removing trusted keys) is handled separately via `lgx keyring` or the package-manager module's `addTrustedKey`/`removeTrustedKey`/`listTrustedKeys` API.

**A dependency's `signer` pin is not a second trust anchor.** The two checks are distinct and both
are required:

| Check | Question | Where | Effect |
|-------|----------|-------|--------|
| Signature policy | *May this package be installed at all?* | `lgpm install` | Rejects a package no active trust anchor validates |
| Dependency `signer` pin | *Which of these same-named candidates did the author mean?* | resolver in `logos-package-downloader` | Filters candidates; never grants install rights |

A bare identifier, a manifest field, a catalog entry or a downloaded key does **not** establish a
trust anchor. The pin narrows a choice inside the set the policy has already accepted; it can only
ever reduce that set, never widen it. A package whose signer matches a dependency pin but whom no
keyring entry trusts is still rejected under `--require-signatures`.

## Future Work

### did:pkh — Blockchain-Verified Identity

The current DID implementation uses `did:jwk` exclusively — a self-contained, offline identity where the public key is embedded in the DID string. No DID Documents are produced or consumed; the DID string is used directly as a structured public key encoding. This is sufficient because `did:jwk` DID Documents are deterministically derivable from the string itself.

When `did:pkh` support is added, signers will be able to prove they control a blockchain account in addition to their Ed25519 signing key. This provides stronger identity guarantees (the signer is tied to a public, auditable on-chain identity).

#### linkedDids schema evolution

The `linkedDids` field in `manifest.sig` is currently `string[]` (always empty). When did:pkh is implemented, each entry will become an object carrying both the DID and a cryptographic proof:

```json
"linkedDids": [
  {
    "did": "did:pkh:eip155:1:0xab16a96D359eC26a11e2C2b3d8f8B8942d5Bfcdb",
    "proofType": "cacao",
    "proof": "<CACAO/EIP-4361 signature proving the blockchain account authorized this did:jwk>"
  }
]
```

Without a proof, a `linkedDids` entry is just a claim — anyone could list any blockchain address. The proof cryptographically binds the `did:pkh` to the signer's `did:jwk`.

#### Proof mechanisms

| Mechanism | Description | Offline verification? |
|-----------|-------------|----------------------|
| **CACAO (EIP-4361)** | Blockchain account signs a "Sign-In with Ethereum" message authorizing the `did:jwk` | Yes (proof is self-contained) |
| **Verifiable Credential** | A VC issued by the `did:pkh` subject attesting ownership of the `did:jwk` | Yes (proof is self-contained) |
| **On-chain registry** | Smart contract mapping blockchain addresses to `did:jwk` strings | No (requires chain query) |
| **Bidirectional signatures** | Ed25519 key signs `did:pkh` + blockchain key signs `did:jwk` | Yes (both proofs stored) |

CACAO/EIP-4361 is the most practical option: standardized, self-contained, and widely supported.

#### New dependencies

- `secp256k1` library for Ethereum signature verification
- CACAO/EIP-4361 message format parsing
- Optional: blockchain RPC client for on-chain registry approaches
- DID Document resolution for `did:pkh` (extracting verification methods)

### Publish Command

```
lgx publish <pkg.lgx>
```

**Status:** TBC. No-op in v0.1. Always exits with success code 0.

**Planned Behavior:**
- Publish package to a registry
- Upload package file and metadata
- Registry authentication and authorization
- Version conflict detection and resolution
