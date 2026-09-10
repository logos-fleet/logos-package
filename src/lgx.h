/**
 * @file lgx.h
 * @brief LGX Package Format C API
 * 
 * This header provides a C API for working with LGX packages.
 * It wraps the C++ implementation with a stable C interface for
 * cross-language interoperability.
 */

#ifndef LGX_H
#define LGX_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-specific export macros */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LGX_BUILD_SHARED
    #ifdef __GNUC__
      #define LGX_EXPORT __attribute__((dllexport))
    #else
      #define LGX_EXPORT __declspec(dllexport)
    #endif
  #else
    #ifdef __GNUC__
      #define LGX_EXPORT __attribute__((dllimport))
    #else
      #define LGX_EXPORT __declspec(dllimport)
    #endif
  #endif
#else
  #if __GNUC__ >= 4
    #define LGX_EXPORT __attribute__((visibility("default")))
  #else
    #define LGX_EXPORT
  #endif
#endif

/* Opaque handle types */
typedef struct lgx_package_opaque* lgx_package_t;

/* Result structures */
typedef struct {
    bool success;
    const char* error;  /* NULL if success, owned by library */
} lgx_result_t;

typedef struct {
    bool valid;
    const char** errors;   /* NULL-terminated array, owned by library */
    const char** warnings; /* NULL-terminated array, owned by library */
} lgx_verify_result_t;

/* Package creation and loading */

/**
 * Create a new skeleton LGX package.
 * 
 * @param output_path Path where the .lgx file will be created
 * @param name Package name (will be lowercased)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_create(const char* output_path, const char* name);

/**
 * Load an existing LGX package from file.
 * 
 * @param path Path to the .lgx file
 * @return Package handle, or NULL on error (check lgx_get_last_error())
 */
LGX_EXPORT lgx_package_t lgx_load(const char* path);

/**
 * Save a package to file.
 * 
 * @param pkg Package handle
 * @param path Path where the .lgx file will be written
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_save(lgx_package_t pkg, const char* path);

/**
 * Verify a package file.
 * 
 * @param path Path to the .lgx file to verify
 * @return Verification result with errors and warnings
 */
LGX_EXPORT lgx_verify_result_t lgx_verify(const char* path);

/* Installed-package checks
 *
 * An installed package is one variant of a .lgx extracted into a directory,
 * with manifest.json written beside it. Most of lgx_verify()'s work describes
 * ARCHIVE SHAPE -- the variants/ layout, the root-entry whitelist, per-entry
 * tar path rules -- and none of that survives extraction. What survives is
 * what the manifest says about the package, plus whether the files are still
 * the ones it covers. These expose it, so a caller holding an installed
 * directory runs the same code lgx_verify does.
 */

/**
 * Validate a manifest's own rules, over the exact bytes: every field/type gate
 * the parser applies plus every rule Manifest::validate() enforces.
 *
 * No directory is involved, which is the point -- a catalog validating a
 * fetched manifest is the second caller.
 *
 * @param manifest_bytes Manifest JSON. Not NUL-dependent.
 * @param manifest_len   Length of manifest_bytes in bytes
 * @return Verification result. Free with lgx_free_verify_result().
 */
LGX_EXPORT lgx_verify_result_t lgx_manifest_validate(
    const char* manifest_bytes, size_t manifest_len);

/* Whether an installed directory still holds the bytes the manifest covers. */
typedef enum {
    /* Every file in the tree answers to a hash the manifest declares: the
       variant leaf, plus hashes["assets"] where the package has root assets. */
    LGX_INTEGRITY_OK = 0,
    /* It does not. Definitive: content was added, removed or altered. */
    LGX_INTEGRITY_MISMATCH = 1,
    /* The manifest declares no hash for this variant. Nothing was proved and
       nothing was disproved -- a pre-hashes package, or the wrong variant. */
    LGX_INTEGRITY_NO_HASH = 2,
    /* The check could not run: the directory or a file in it was unreadable,
       or the crypto library failed to initialise. Also not a verdict. */
    LGX_INTEGRITY_UNREADABLE = 3,
    /* The CALLER's input is unusable -- no directory, no variant, or a
       manifest that does not parse. Distinct from NO_HASH so a typo'd variant
       cannot read as "this package simply has no hash" and be waved through,
       the same fail-open LGX_SIG_CHECK_BAD_DID exists to avoid. */
    LGX_INTEGRITY_BAD_INPUT = 4
} lgx_integrity_t;

/**
 * Recompute hashes["variants/<variant>"] over an installed directory.
 *
 * The primitive that exists nowhere else: lgx_verify() checks the archive's
 * "root" hash, which covers every variant and the whole tar, and an install
 * has neither. Content paths were hashed relative to variants/<v>/, which is
 * exactly the flattened installed layout; manifest.json, manifest.sig and the
 * installer's `variant` file are skipped.
 *
 * Root-level assets extract into that same directory but answer to
 * hashes["assets"], so where a package has them BOTH hashes must come out
 * right -- otherwise deleting them would look like a package that had none.
 *
 * @param dir_path       The installed package directory
 * @param manifest_bytes manifest.json bytes, exactly as read from that directory
 * @param manifest_len   Length of manifest_bytes in bytes
 * @param variant        The installed variant, e.g. from the `variant` file.
 *                       Required: only this variant's files are on disk.
 * @return an lgx_integrity_t; only LGX_INTEGRITY_OK means verified.
 *         lgx_get_last_error() carries a human-readable reason otherwise.
 */
LGX_EXPORT lgx_integrity_t lgx_verify_installed_tree(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* variant);

/**
 * Every check that survives installation, in one call: the manifest rules of
 * lgx_manifest_validate(), the integrity of lgx_verify_installed_tree(),
 * whether main[variant] and (for ui_qml) `view` resolve, and the icon
 * contract. Errors read the same as the ones `lgx verify` prints.
 *
 * Signature checking is deliberately NOT here: it needs manifest.sig and a DID
 * the caller pins, which is lgx_check_manifest_signature().
 *
 * @param dir_path       The installed package directory
 * @param manifest_bytes manifest.json bytes, exactly as read from that directory
 * @param manifest_len   Length of manifest_bytes in bytes
 * @param variant        The installed variant
 * @return Verification result. Free with lgx_free_verify_result().
 */
LGX_EXPORT lgx_verify_result_t lgx_verify_installed(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* variant);

/* Variant names
 *
 * A variant name, "<os>-<architecture>", is a key inside the SIGNED hash tree
 * (hashes["variants/<name>"]), so a published package can never be renamed on
 * disk and disagreeing producer spellings have to be reconciled on read. This
 * library owns that vocabulary; consumers ask rather than tabulate.
 */

/**
 * The variant this build of the library targets, e.g. "darwin-arm64".
 *
 * Compile-time and reads nothing. "unknown" on a target with no rule.
 *
 * @return Static storage owned by the library. Do NOT free.
 */
LGX_EXPORT const char* lgx_host_variant(void);

/**
 * `variant` first, then every other live spelling of its ARCHITECTURE half:
 * the list to walk when looking for one platform's variant in a package.
 *
 * Only the architecture is aliased. Matching the OS half verbatim is what
 * keeps a Windows package from installing as a macOS one. An architecture with
 * no known alias yields the input alone.
 *
 * @param variant A variant name, e.g. lgx_host_variant()
 * @return NULL-terminated array, canonical caller spelling first.
 *         Free with lgx_free_string_array(). Empty (a bare NULL terminator)
 *         for NULL or an empty `variant`.
 */
LGX_EXPORT const char** lgx_variant_spellings(const char* variant);

/**
 * The canonical name of every target this ecosystem ships -- desktop, mobile
 * and web -- in no particular order. These are the names a producer should
 * write; the architecture aliases lgx_variant_spellings() yields are legacy.
 *
 * @return NULL-terminated array. Free with lgx_free_string_array().
 */
LGX_EXPORT const char** lgx_known_variants(void);

/**
 * Whether this vocabulary vouches for `variant` as written: a canonical name,
 * one of its architecture aliases, or either carrying the "-dev" flavour
 * suffix a non-portable build appends.
 *
 * False is not "invalid" -- a private target this library has never heard of
 * is also false. Only lgx_variant_suggestion() distinguishes the two.
 */
LGX_EXPORT bool lgx_variant_is_known(const char* variant);

/**
 * The canonical name `variant` was probably meant to be.
 *
 * NULL means "nothing to say": either the name is already accepted, or it
 * resembles nothing in the vocabulary and so belongs to someone else. A
 * non-NULL answer is a correction for a name that will resolve on NO host.
 *
 * @return Thread-local storage owned by the library, valid until this thread's
 *         next call. Do NOT free.
 */
LGX_EXPORT const char* lgx_variant_suggestion(const char* variant);

/* Resolving a manifest's `main` against an installed directory */

typedef enum {
    /* The manifest named a main and that file is in the directory. */
    LGX_MAIN_RESOLVED = 0,
    /* The manifest declares no `main` at all. Normal for a QML-only ui_qml
       package, and broken for everything else -- which is the CALLER's rule
       to apply, not this one's. */
    LGX_MAIN_NOT_DECLARED = 1,
    /* `main` is neither a variant map nor a string, or the entry that matched
       names nothing usable: an empty or non-string value, a path that escapes
       the directory, or a path naming a directory rather than a file. */
    LGX_MAIN_MALFORMED_ENTRY = 2,
    /* `main` is a variant map and no candidate variant is a key of it. The
       package is for other platforms than the ones asked about. */
    LGX_MAIN_NO_VARIANT_MATCH = 3,
    /* `main` named a file that is not in the directory. Kept apart from
       NO_VARIANT_MATCH: this install is for another variant, which is a
       different repair from "this package has no main". */
    LGX_MAIN_FILE_MISSING = 4,
    /* The CALLER's input is unusable: no dir_path, or manifest bytes that are
       not a JSON object. Nothing was said about the package. */
    LGX_MAIN_BAD_INPUT = 5
} lgx_main_resolution_t;

/* Where the manifest's `main` resolved to, and why not when it did not.
   Every pointer is NULL when it does not apply, so an unresolved main can
   never be read as a usable path. */
typedef struct {
    lgx_main_resolution_t state;
    const char* path;          /* absolute, native separators; NULL unless RESOLVED */
    const char* declared_path; /* the relative path the manifest named, or NULL */
    const char* variant;       /* the `main` key that selected it, or NULL */
    const char* error;         /* why, or NULL when RESOLVED */
} lgx_main_file_t;

/**
 * Resolve `main` for the first of `variants` the manifest names.
 *
 * THIS TOUCHES THE DISK, and has to: whether the declared path stays inside
 * the package and whether it is a file rather than a directory cannot be
 * answered from the manifest text. A directory that is not there simply
 * contains no main.
 *
 * The first candidate variant that is a key of `main` wins OUTRIGHT and does
 * NOT fall through to a later one when its named file is missing. A key whose
 * value is empty or not a string DOES fall through, and is reported as
 * MALFORMED_ENTRY only if nothing later matches.
 *
 * Variant keys are matched verbatim -- a directory is read as it was written.
 *
 * @param dir_path       The installed package directory
 * @param manifest_bytes manifest.json bytes, exactly as read. Not
 *                       NUL-dependent. Parsed here rather than taken as a
 *                       validated manifest because `main` also has a
 *                       plain-string form the manifest parser rejects.
 * @param manifest_len   Length of manifest_bytes in bytes
 * @param variants       NULL-terminated array of candidate variant spellings,
 *                       most preferred first, e.g. from
 *                       lgx_variant_spellings(). NULL or empty means no
 *                       candidate, so a variant-map `main` yields
 *                       NO_VARIANT_MATCH.
 * @return the resolution. Free with lgx_free_main_file().
 */
LGX_EXPORT lgx_main_file_t lgx_resolve_main(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* const* variants);

/**
 * Free a main-file resolution.
 *
 * @param result Resolution to free
 */
LGX_EXPORT void lgx_free_main_file(lgx_main_file_t result);

/* Package manipulation */

/**
 * Add files to a variant in the package.
 * If the variant exists, it will be completely replaced.
 * 
 * @param pkg Package handle
 * @param variant Variant name (will be lowercased)
 * @param files_path Path to file or directory to add
 * @param main_path Relative path to main file (required for directories, can be NULL for single files)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_add_variant(
    lgx_package_t pkg,
    const char* variant,
    const char* files_path,
    const char* main_path
);

/**
 * Remove a variant from the package.
 * 
 * @param pkg Package handle
 * @param variant Variant name (case-insensitive)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_remove_variant(lgx_package_t pkg, const char* variant);

/**
 * Extract variant contents from a package to a directory.
 * 
 * @param pkg Package handle
 * @param variant Variant name (NULL to extract all variants)
 * @param output_dir Output directory path (variant contents go to output_dir/variant/)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_extract(lgx_package_t pkg, const char* variant, const char* output_dir);

/**
 * Check if a variant exists in the package.
 * 
 * @param pkg Package handle
 * @param variant Variant name
 * @return true if variant exists
 */
LGX_EXPORT bool lgx_has_variant(lgx_package_t pkg, const char* variant);

/**
 * Get list of variants in the package.
 * 
 * @param pkg Package handle
 * @return NULL-terminated array of variant names, owned by library.
 *         Free with lgx_free_string_array().
 */
LGX_EXPORT const char** lgx_get_variants(lgx_package_t pkg);

/* Manifest access */

/**
 * Get the package name from manifest.
 * 
 * @param pkg Package handle
 * @return Package name, owned by library (valid until package is freed)
 */
LGX_EXPORT const char* lgx_get_name(lgx_package_t pkg);

/**
 * Get the package version from manifest.
 * 
 * @param pkg Package handle
 * @return Package version, owned by library (valid until package is freed)
 */
LGX_EXPORT const char* lgx_get_version(lgx_package_t pkg);

/**
 * Set the package version in manifest.
 * 
 * @param pkg Package handle
 * @param version New version string
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_set_version(lgx_package_t pkg, const char* version);

/**
 * Get the package description from manifest.
 * 
 * @param pkg Package handle
 * @return Package description, owned by library (valid until package is freed)
 */
LGX_EXPORT const char* lgx_get_description(lgx_package_t pkg);

/**
 * Set the package description in manifest.
 *
 * @param pkg Package handle
 * @param description New description string
 */
LGX_EXPORT void lgx_set_description(lgx_package_t pkg, const char* description);

/**
 * Get the package icon path from manifest.
 *
 * @param pkg Package handle
 * @return Package icon path, owned by library (valid until package is freed)
 */
LGX_EXPORT const char* lgx_get_icon(lgx_package_t pkg);

/**
 * Set the package icon path in manifest.
 *
 * @param pkg Package handle
 * @param icon New icon path string
 */
LGX_EXPORT void lgx_set_icon(lgx_package_t pkg, const char* icon);

/**
 * Get the full manifest as a JSON string.
 *
 * @param pkg Package handle
 * @return JSON string of the manifest, owned by library (valid until package is freed)
 */
LGX_EXPORT const char* lgx_get_manifest_json(lgx_package_t pkg);

/**
 * Get the package's manifest.sig document as JSON.
 *
 * The counterpart to lgx_get_manifest_json(), and it exists for the same
 * reason: lgx_extract() writes `variants/<v>/` and `assets/` only, so a
 * consumer that extracts a package ends up holding the SIGNED BYTES
 * (manifest.json) without the signature over them. Both together are what
 * make an installed package's publisher checkable after the .lgx is gone.
 *
 * The returned string is owned by `pkg` and is invalidated by the next call
 * on the same package. Do not free it.
 *
 * @param pkg Package handle
 * @return manifest.sig JSON, or NULL if the package is unsigned
 */
LGX_EXPORT const char* lgx_get_manifest_sig_json(lgx_package_t pkg);

/* Signature types and functions */

typedef struct {
    bool is_signed;          /* manifest.sig present */
    bool signature_valid;    /* Ed25519 signature verifies */
    bool package_valid;      /* package structure and content hashes are valid */
    const char* signer_did;  /* did:jwk:... or NULL */
    const char* signer_name; /* self-asserted display name, or NULL */
    const char* signer_url;  /* self-asserted URL, or NULL */
    const char* trusted_as;  /* keyring name if trusted, or NULL */
    const char* error;       /* error message, or NULL */
} lgx_signature_info_t;

/**
 * Verify the cryptographic signature of a package.
 *
 * @param lgx_path Path to the .lgx package file
 * @param keyring_dir Path to trusted keys directory (NULL for default)
 * @return Signature info. Free with lgx_free_signature_info().
 */
LGX_EXPORT lgx_signature_info_t lgx_verify_signature(
    const char* lgx_path, const char* keyring_dir);

/**
 * Free a signature info structure.
 *
 * @param info Signature info to free
 */
LGX_EXPORT void lgx_free_signature_info(lgx_signature_info_t info);

/* Outcome of checking a detached manifest signature against a caller's DID. */
typedef enum {
    /* `expected_did`'s Ed25519 key produced this signature over these bytes. */
    LGX_SIG_CHECK_OK = 0,
    /* A usable signature document that `expected_did` did NOT produce.
       Definitive: somebody else signed these bytes, or these are not the
       bytes that were signed. */
    LGX_SIG_CHECK_MISMATCH = 1,
    /* No usable signature document: NULL, unparseable, unsupported version
       or algorithm, or a signature that is not 64 bytes. Nothing was proved
       and nothing was disproved. */
    LGX_SIG_CHECK_UNUSABLE = 2,
    /* `expected_did` is not a did:jwk carrying an Ed25519 key. The CALLER's
       input is malformed, not the package's. Distinct from UNUSABLE because
       the remedy is the opposite one, and distinct from OK because a pin
       nobody can parse must never be treated as satisfied. */
    LGX_SIG_CHECK_BAD_DID = 3
} lgx_sig_check_t;

/**
 * Check a detached manifest signature against a DID the CALLER supplies.
 *
 * Answers exactly one question: did `expected_did`'s key sign these bytes?
 *
 * THE DID IN `sig_json` IS NEVER CONSULTED FOR THE KEY, and that is the whole
 * point of the parameter. Reading the DID out of the signature document,
 * comparing it to a pin, and then verifying with that same document's key
 * proves only that the document is INTERNALLY CONSISTENT — an attacker who
 * replaces the signature also replaces the DID beside it, signs the bytes with
 * a key they own, and every check agrees. Taking the key from the caller's DID
 * instead means an attacker must produce an Ed25519 signature under a key they
 * do not have. `sig_json` contributes the signature bytes and nothing else.
 *
 * Verifies over the caller's bytes verbatim. The bytes to pass are the ones
 * lgx_get_manifest_json() returns — Package::signPackage() signs
 * getManifest().toJson(), the same expression, so a manifest.json written from
 * that getter is byte-identical to what was signed by construction.
 *
 * @param manifest_bytes The signed message (manifest JSON). Not NUL-dependent.
 * @param manifest_len   Length of manifest_bytes in bytes
 * @param sig_json       manifest.sig document, as from lgx_get_manifest_sig_json()
 * @param expected_did   did:jwk whose key must have signed. Never taken from sig_json.
 * @return an lgx_sig_check_t; only LGX_SIG_CHECK_OK means verified
 */
LGX_EXPORT lgx_sig_check_t lgx_check_manifest_signature(
    const char* manifest_bytes, size_t manifest_len,
    const char* sig_json, const char* expected_did);

/**
 * Sign a package with a secret key.
 *
 * @param lgx_path Path to the .lgx package file
 * @param secret_key_path Path to the secret key file (.jwk)
 * @param signer_name Optional display name for signer metadata (can be NULL)
 * @param signer_url Optional URL for signer metadata (can be NULL)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_sign(
    const char* lgx_path, const char* secret_key_path,
    const char* signer_name, const char* signer_url);

/**
 * Generate an Ed25519 signing keypair.
 *
 * @param name Name for the keypair
 * @param output_dir Directory to write key files (NULL for default)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_keygen(
    const char* name, const char* output_dir);

/**
 * Add a trusted key to the keyring by DID.
 *
 * @param keyring_dir Path to keyring directory (NULL for default)
 * @param name Local name for the key
 * @param did DID string (did:jwk:...)
 * @param display_name Optional display name (can be NULL)
 * @param url Optional URL (can be NULL)
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_keyring_add(
    const char* keyring_dir, const char* name, const char* did,
    const char* display_name, const char* url);

/**
 * Remove a trusted public key from the keyring.
 *
 * @param keyring_dir Path to keyring directory (NULL for default)
 * @param name Name of the key to remove
 * @return Result indicating success or failure
 */
LGX_EXPORT lgx_result_t lgx_keyring_remove(
    const char* keyring_dir, const char* name);

/**
 * A single trusted key entry from the keyring.
 */
typedef struct {
    const char* name;         /* local keyring name */
    const char* did;          /* did:jwk:... string */
    const char* display_name; /* display name, or NULL */
    const char* url;          /* URL, or NULL */
    const char* added_at;     /* ISO 8601 timestamp, or NULL */
} lgx_trusted_key_t;

/**
 * List of trusted keys from the keyring.
 */
typedef struct {
    lgx_trusted_key_t* keys;  /* array of key entries */
    size_t count;             /* number of entries */
} lgx_keyring_list_t;

/**
 * List all trusted keys in the keyring.
 *
 * @param keyring_dir Path to keyring directory (NULL for default)
 * @return List of trusted keys. Free with lgx_free_keyring_list().
 */
LGX_EXPORT lgx_keyring_list_t lgx_keyring_list(const char* keyring_dir);

/**
 * Free a keyring list structure.
 *
 * @param list Keyring list to free
 */
LGX_EXPORT void lgx_free_keyring_list(lgx_keyring_list_t list);

/* Memory management */

/**
 * Free a package handle.
 * 
 * @param pkg Package handle to free
 */
LGX_EXPORT void lgx_free_package(lgx_package_t pkg);

/**
 * Free a string array returned by library functions.
 * 
 * @param array NULL-terminated string array
 */
LGX_EXPORT void lgx_free_string_array(const char** array);

/**
 * Free a verify result structure.
 * 
 * @param result Verify result to free
 */
LGX_EXPORT void lgx_free_verify_result(lgx_verify_result_t result);

/* Error handling */

/**
 * Get the last error message.
 * 
 * @return Error message string, owned by library (thread-local storage)
 */
LGX_EXPORT const char* lgx_get_last_error(void);

/* Semver
 *
 * The one semver implementation for the whole packaging stack, exposed on the C
 * ABI so the non-C++ consumers reach the same code the C++ ones do. Precedence
 * is SemVer 2.0.0 (pre-releases order per §11, build metadata ignored); ranges
 * are the npm dialect the manifests already use. See include/logos/semver.hpp.
 */

/**
 * Compare two versions by SemVer 2.0.0 precedence.
 *
 * @param a First version (e.g. "1.0.0-rc.2")
 * @param b Second version (e.g. "1.0.0-rc.11")
 * @return -1 if a < b, 0 if equal, 1 if a > b. An unparseable version sorts
 *         below every valid one; NULL is treated as an empty string.
 */
LGX_EXPORT int lgx_semver_compare(const char* a, const char* b);

/**
 * Is this a valid SemVer 2.0.0 version string?
 */
LGX_EXPORT bool lgx_semver_valid(const char* version);

/**
 * Does `version` satisfy `range` (npm-style: ^ ~ x * || >= <= > < =)?
 *
 * A range never matches a pre-release unless the range itself names one at the
 * same major.minor.patch — so "^1.0.0" does NOT match "2.0.0-alpha".
 */
LGX_EXPORT bool lgx_semver_satisfies(const char* version, const char* range);

/**
 * Is this a well-formed npm-style range? (Syntax only.)
 */
LGX_EXPORT bool lgx_semver_valid_range(const char* range);

/* Version info */

/**
 * Get the library version string.
 *
 * @return Version string (e.g., "0.1.0")
 */
LGX_EXPORT const char* lgx_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LGX_H */
