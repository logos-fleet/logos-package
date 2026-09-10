/**
 * @file lib.cpp
 * @brief LGX Package Format C API Implementation
 * 
 * Thin C wrapper around the C++ lgx::Package class.
 */

#include "lgx.h"
#include "core/package.h"
#include "core/manifest.h"
#include "core/installed_package.h"
#include "core/main_resolution.h"
#include "core/platform_variant.h"
#include "crypto/signing.h"
#include "crypto/keyring.h"
#include "logos/semver.hpp"

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstring>
#include <filesystem>
#include <algorithm>

/* Thread-local error storage */
thread_local std::string g_last_error;

/* Helper to set error and return false */
static void set_error(const std::string& error) {
    g_last_error = error;
}

/* Helper to clear error */
static void clear_error() {
    g_last_error.clear();
}

/* Helper to copy std::string to C string (caller must free) */
static char* strdup_cpp(const std::string& str) {
    char* result = static_cast<char*>(malloc(str.length() + 1));
    if (result) {
        std::strcpy(result, str.c_str());
    }
    return result;
}

/* Helper to convert vector<string> to NULL-terminated C string array */
static const char** vector_to_array(const std::vector<std::string>& vec) {
    const char** result = static_cast<const char**>(malloc((vec.size() + 1) * sizeof(char*)));
    if (!result) return nullptr;
    
    for (size_t i = 0; i < vec.size(); ++i) {
        result[i] = strdup_cpp(vec[i]);
        if (!result[i]) {
            // Cleanup on allocation failure
            for (size_t j = 0; j < i; ++j) {
                free(const_cast<char*>(result[j]));
            }
            free(result);
            return nullptr;
        }
    }
    result[vec.size()] = nullptr;
    return result;
}

/* Helper to convert set<string> to NULL-terminated C string array */
static const char** set_to_array(const std::set<std::string>& s) {
    std::vector<std::string> vec(s.begin(), s.end());
    return vector_to_array(vec);
}

/* Package wrapper struct */
struct lgx_package_opaque {
    std::unique_ptr<lgx::Package> pkg;
    std::string name_cache;
    std::string version_cache;
    std::string description_cache;
    std::string icon_cache;
    std::string manifest_json_cache;
    std::string manifest_sig_json_cache;
};

/* Package creation and loading */

LGX_EXPORT lgx_result_t lgx_create(const char* output_path, const char* name) {
    if (!output_path || !name) {
        set_error("Invalid arguments: output_path and name cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    auto result = lgx::Package::create(output_path, name);
    
    if (!result.success) {
        set_error(result.error);
        return {false, g_last_error.c_str()};
    }
    return {true, nullptr};
}

LGX_EXPORT lgx_package_t lgx_load(const char* path) {
    if (!path) {
        set_error("Invalid argument: path cannot be NULL");
        return nullptr;
    }
    
    clear_error();
    auto pkg_opt = lgx::Package::load(path);
    
    if (!pkg_opt) {
        set_error("Failed to load package: " + std::string(path));
        return nullptr;
    }
    
    auto wrapper = new lgx_package_opaque();
    wrapper->pkg = std::make_unique<lgx::Package>(std::move(*pkg_opt));
    return wrapper;
}

LGX_EXPORT lgx_result_t lgx_save(lgx_package_t pkg, const char* path) {
    if (!pkg || !path) {
        set_error("Invalid arguments: pkg and path cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    auto result = pkg->pkg->save(path);
    
    if (!result.success) {
        set_error(result.error);
        return {false, g_last_error.c_str()};
    }
    return {true, nullptr};
}

LGX_EXPORT lgx_verify_result_t lgx_verify(const char* path) {
    if (!path) {
        set_error("Invalid argument: path cannot be NULL");
        return {false, nullptr, nullptr};
    }
    
    clear_error();
    auto result = lgx::Package::verify(path);
    
    lgx_verify_result_t c_result;
    c_result.valid = result.valid;
    c_result.errors = result.errors.empty() ? nullptr : vector_to_array(result.errors);
    c_result.warnings = result.warnings.empty() ? nullptr : vector_to_array(result.warnings);
    
    return c_result;
}

/* Installed-package checks */

namespace {

/* Turn a VerifyResult into the C shape. Shared by every verify entry point so
   ownership stays uniform: lgx_free_verify_result() frees both arrays. */
lgx_verify_result_t to_c_verify_result(const lgx::Package::VerifyResult& r) {
    lgx_verify_result_t out;
    out.valid = r.valid;
    out.errors = r.errors.empty() ? nullptr : vector_to_array(r.errors);
    out.warnings = r.warnings.empty() ? nullptr : vector_to_array(r.warnings);
    return out;
}

lgx_verify_result_t verify_failure(const std::string& message) {
    lgx::Package::VerifyResult r;
    r.valid = false;
    r.errors.push_back(message);
    return to_c_verify_result(r);
}

/* Parse the caller's bytes the way Package::load() does, so a malformed
   manifest produces the message `lgx verify` already prints for one. */
std::optional<lgx::Manifest> parse_manifest(
    const char* bytes, size_t len, std::string& error) {
    if (!bytes) {
        error = "Missing manifest.json";
        return std::nullopt;
    }
    auto m = lgx::Manifest::fromJson(std::string(bytes, len));
    if (!m) {
        error = "Failed to parse manifest: " + lgx::Manifest::getLastError();
        return std::nullopt;
    }
    return m;
}

} // namespace

LGX_EXPORT lgx_verify_result_t lgx_manifest_validate(
    const char* manifest_bytes, size_t manifest_len) {

    clear_error();
    std::string error;
    auto manifest = parse_manifest(manifest_bytes, manifest_len, error);
    if (!manifest) {
        set_error(error);
        return verify_failure(error);
    }

    lgx::Package::VerifyResult result = lgx::Package::VerifyResult::ok();
    auto validation = manifest->validate();
    if (!validation.valid) {
        result.valid = false;
        for (const auto& err : validation.errors) {
            result.errors.push_back("Manifest: " + err);
        }
    }
    return to_c_verify_result(result);
}

LGX_EXPORT lgx_integrity_t lgx_verify_installed_tree(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* variant) {

    clear_error();
    if (!dir_path) {
        set_error("Invalid argument: dir_path cannot be NULL");
        return LGX_INTEGRITY_BAD_INPUT;
    }
    std::string error;
    auto manifest = parse_manifest(manifest_bytes, manifest_len, error);
    if (!manifest) {
        set_error(error);
        return LGX_INTEGRITY_BAD_INPUT;
    }

    std::string detail;
    auto status = lgx::verifyInstalledTree(dir_path, *manifest,
                                           variant ? variant : "", &detail);
    if (!detail.empty()) set_error(detail);

    switch (status) {
        case lgx::InstalledIntegrity::Ok:         return LGX_INTEGRITY_OK;
        case lgx::InstalledIntegrity::Mismatch:   return LGX_INTEGRITY_MISMATCH;
        case lgx::InstalledIntegrity::NoHash:     return LGX_INTEGRITY_NO_HASH;
        case lgx::InstalledIntegrity::Unreadable: return LGX_INTEGRITY_UNREADABLE;
        case lgx::InstalledIntegrity::BadInput:   return LGX_INTEGRITY_BAD_INPUT;
    }
    return LGX_INTEGRITY_UNREADABLE;
}

LGX_EXPORT lgx_verify_result_t lgx_verify_installed(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* variant) {

    clear_error();
    if (!dir_path) {
        set_error("Invalid argument: dir_path cannot be NULL");
        return verify_failure(g_last_error);
    }
    std::string error;
    auto manifest = parse_manifest(manifest_bytes, manifest_len, error);
    if (!manifest) {
        set_error(error);
        return verify_failure(error);
    }

    return to_c_verify_result(
        lgx::verifyInstalled(dir_path, *manifest, variant ? variant : ""));
}

/* Variant names */

LGX_EXPORT const char* lgx_host_variant(void) {
    /* Static, so the caller never frees it: the value is compile-time. */
    static const std::string host = lgx::hostVariant();
    return host.c_str();
}

LGX_EXPORT const char** lgx_variant_spellings(const char* variant) {
    clear_error();
    return vector_to_array(lgx::variantSpellings(variant ? variant : ""));
}

LGX_EXPORT const char** lgx_known_variants(void) {
    clear_error();
    return vector_to_array(lgx::knownVariants());
}

LGX_EXPORT bool lgx_variant_is_known(const char* variant) {
    clear_error();
    return lgx::isKnownVariant(variant ? variant : "");
}

LGX_EXPORT const char* lgx_variant_suggestion(const char* variant) {
    clear_error();
    /* Thread-local, so the caller never frees it and two threads asking about
       two names never race -- the same contract as lgx_get_last_error(). */
    static thread_local std::string suggestion;
    suggestion = lgx::suggestVariantName(variant ? variant : "");
    return suggestion.empty() ? nullptr : suggestion.c_str();
}

/* Resolving a manifest's `main` */

namespace {

lgx_main_resolution_t to_c_main_resolution(lgx::MainResolution state) {
    switch (state) {
        case lgx::MainResolution::Resolved:       return LGX_MAIN_RESOLVED;
        case lgx::MainResolution::NotDeclared:    return LGX_MAIN_NOT_DECLARED;
        case lgx::MainResolution::MalformedEntry: return LGX_MAIN_MALFORMED_ENTRY;
        case lgx::MainResolution::NoVariantMatch: return LGX_MAIN_NO_VARIANT_MATCH;
        case lgx::MainResolution::FileMissing:    return LGX_MAIN_FILE_MISSING;
        case lgx::MainResolution::BadInput:       return LGX_MAIN_BAD_INPUT;
    }
    return LGX_MAIN_BAD_INPUT;
}

/* NULL rather than "" for every field that does not apply, so a caller cannot
   read an unresolved main as a usable empty path. */
const char* own_or_null(const std::string& value) {
    return value.empty() ? nullptr : strdup_cpp(value);
}

} // namespace

LGX_EXPORT lgx_main_file_t lgx_resolve_main(
    const char* dir_path,
    const char* manifest_bytes, size_t manifest_len,
    const char* const* variants) {

    clear_error();
    if (!manifest_bytes) {
        /* Same wording as the other installed-package entry points, so a
           caller comparing errors sees one vocabulary. */
        set_error("Missing manifest.json");
        return { LGX_MAIN_BAD_INPUT, nullptr, nullptr, nullptr,
                 strdup_cpp("Missing manifest.json") };
    }

    std::vector<std::string> candidates;
    if (variants) {
        for (const char* const* v = variants; *v != nullptr; ++v) {
            candidates.emplace_back(*v);
        }
    }

    const lgx::MainFile resolved = lgx::resolveMain(
        dir_path ? std::filesystem::path(dir_path) : std::filesystem::path(),
        std::string(manifest_bytes, manifest_len),
        candidates);

    if (!resolved.error.empty()) set_error(resolved.error);

    lgx_main_file_t out;
    out.state = to_c_main_resolution(resolved.state);
    out.path = own_or_null(resolved.path);
    out.declared_path = own_or_null(resolved.declaredPath);
    out.variant = own_or_null(resolved.variant);
    out.error = own_or_null(resolved.error);
    return out;
}

LGX_EXPORT void lgx_free_main_file(lgx_main_file_t result) {
    free(const_cast<char*>(result.path));
    free(const_cast<char*>(result.declared_path));
    free(const_cast<char*>(result.variant));
    free(const_cast<char*>(result.error));
}

/* Package manipulation */

LGX_EXPORT lgx_result_t lgx_add_variant(
    lgx_package_t pkg,
    const char* variant,
    const char* files_path,
    const char* main_path
) {
    if (!pkg || !variant || !files_path) {
        set_error("Invalid arguments: pkg, variant, and files_path cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    std::optional<std::string> main_opt = main_path ? std::optional<std::string>(main_path) : std::nullopt;
    auto result = pkg->pkg->addVariant(variant, files_path, main_opt);
    
    if (!result.success) {
        set_error(result.error);
        return {false, g_last_error.c_str()};
    }
    return {true, nullptr};
}

LGX_EXPORT lgx_result_t lgx_remove_variant(lgx_package_t pkg, const char* variant) {
    if (!pkg || !variant) {
        set_error("Invalid arguments: pkg and variant cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    auto result = pkg->pkg->removeVariant(variant);
    
    if (!result.success) {
        set_error(result.error);
        return {false, g_last_error.c_str()};
    }
    return {true, nullptr};
}

LGX_EXPORT lgx_result_t lgx_extract(lgx_package_t pkg, const char* variant, const char* output_dir) {
    if (!pkg || !output_dir) {
        set_error("Invalid arguments: pkg and output_dir cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    lgx::Package::Result result;
    
    if (variant) {
        result = pkg->pkg->extractVariant(variant, output_dir);
    } else {
        result = pkg->pkg->extractAll(output_dir);
    }
    
    if (!result.success) {
        set_error(result.error);
        return {false, g_last_error.c_str()};
    }
    return {true, nullptr};
}

LGX_EXPORT bool lgx_has_variant(lgx_package_t pkg, const char* variant) {
    if (!pkg || !variant) {
        set_error("Invalid arguments: pkg and variant cannot be NULL");
        return false;
    }
    
    clear_error();
    return pkg->pkg->hasVariant(variant);
}

LGX_EXPORT const char** lgx_get_variants(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }
    
    clear_error();
    auto variants = pkg->pkg->getVariants();
    return set_to_array(variants);
}

/* Manifest access */

LGX_EXPORT const char* lgx_get_name(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }
    
    clear_error();
    pkg->name_cache = pkg->pkg->getManifest().name;
    return pkg->name_cache.c_str();
}

LGX_EXPORT const char* lgx_get_version(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }
    
    clear_error();
    pkg->version_cache = pkg->pkg->getManifest().version;
    return pkg->version_cache.c_str();
}

LGX_EXPORT lgx_result_t lgx_set_version(lgx_package_t pkg, const char* version) {
    if (!pkg || !version) {
        set_error("Invalid arguments: pkg and version cannot be NULL");
        return {false, g_last_error.c_str()};
    }
    
    clear_error();
    pkg->pkg->getManifest().version = version;
    return {true, nullptr};
}

LGX_EXPORT const char* lgx_get_description(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }
    
    clear_error();
    pkg->description_cache = pkg->pkg->getManifest().description;
    return pkg->description_cache.c_str();
}

LGX_EXPORT void lgx_set_description(lgx_package_t pkg, const char* description) {
    if (!pkg || !description) {
        set_error("Invalid arguments: pkg and description cannot be NULL");
        return;
    }
    
    clear_error();
    pkg->pkg->getManifest().description = description;
}

LGX_EXPORT const char* lgx_get_icon(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }

    clear_error();
    pkg->icon_cache = pkg->pkg->getManifest().icon;
    return pkg->icon_cache.c_str();
}

LGX_EXPORT void lgx_set_icon(lgx_package_t pkg, const char* icon) {
    if (!pkg || !icon) {
        set_error("Invalid arguments: pkg and icon cannot be NULL");
        return;
    }

    clear_error();
    pkg->pkg->getManifest().icon = icon;
}

LGX_EXPORT const char* lgx_get_manifest_json(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }

    clear_error();
    pkg->manifest_json_cache = pkg->pkg->getManifest().toJson();
    return pkg->manifest_json_cache.c_str();
}

LGX_EXPORT const char* lgx_get_manifest_sig_json(lgx_package_t pkg) {
    if (!pkg) {
        set_error("Invalid argument: pkg cannot be NULL");
        return nullptr;
    }

    clear_error();
    const auto& sig = pkg->pkg->getManifestSig();
    if (!sig.has_value()) {
        set_error("Package is unsigned");
        return nullptr;
    }
    pkg->manifest_sig_json_cache = sig->toJson();
    return pkg->manifest_sig_json_cache.c_str();
}

LGX_EXPORT lgx_sig_check_t lgx_check_manifest_signature(
    const char* manifest_bytes, size_t manifest_len,
    const char* sig_json, const char* expected_did) {

    clear_error();

    // The caller's DID is resolved FIRST and on its own. A pin that is not a
    // did:jwk carrying an Ed25519 key is a caller-side error, and it has to be
    // reported as one rather than swallowed into "could not check" -- a
    // typo'd pin that reads as UNUSABLE would be waved through by any policy
    // that tolerates missing evidence, which is the fail-open this whole
    // mechanism exists to avoid.
    if (!expected_did) {
        set_error("Invalid argument: expected_did cannot be NULL");
        return LGX_SIG_CHECK_BAD_DID;
    }
    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        return LGX_SIG_CHECK_UNUSABLE;
    }
    auto pkOpt = lgx::crypto::didToPublicKey(expected_did);
    if (!pkOpt) {
        set_error(std::string("Not a did:jwk Ed25519 DID: ") + expected_did);
        return LGX_SIG_CHECK_BAD_DID;
    }

    if (!sig_json) {
        set_error("No signature document");
        return LGX_SIG_CHECK_UNUSABLE;
    }
    auto sigOpt = lgx::crypto::ManifestSig::fromJson(sig_json);
    if (!sigOpt) {
        set_error("Failed to parse manifest.sig: " + lgx::crypto::ManifestSig::getLastError());
        return LGX_SIG_CHECK_UNUSABLE;
    }
    auto sigBytes = lgx::crypto::base64Decode(sigOpt->signature);
    if (!sigBytes || sigBytes->size() != lgx::crypto::SIGNATURE_SIZE) {
        set_error("Signature in manifest.sig is not a 64-byte Ed25519 signature");
        return LGX_SIG_CHECK_UNUSABLE;
    }
    lgx::crypto::Signature sig;
    std::copy(sigBytes->begin(), sigBytes->end(), sig.begin());

    // sigOpt->did is deliberately not read. See the header.
    if (!manifest_bytes) manifest_len = 0;
    std::vector<uint8_t> message(manifest_bytes, manifest_bytes + manifest_len);

    if (!lgx::crypto::verify(message, *pkOpt, sig)) {
        set_error("Signature does not verify under the supplied DID");
        return LGX_SIG_CHECK_MISMATCH;
    }
    return LGX_SIG_CHECK_OK;
}

/* Signature functions */

LGX_EXPORT lgx_signature_info_t lgx_verify_signature(
    const char* lgx_path, const char* keyring_dir) {
    lgx_signature_info_t info = {};

    if (!lgx_path) {
        set_error("Invalid argument: lgx_path cannot be NULL");
        info.error = strdup_cpp(g_last_error);
        return info;
    }

    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        info.error = strdup_cpp(g_last_error);
        return info;
    }

    auto pkg_opt = lgx::Package::load(lgx_path);
    if (!pkg_opt) {
        set_error("Failed to load package: " + std::string(lgx_path));
        info.error = strdup_cpp(g_last_error);
        return info;
    }

    auto sigInfo = pkg_opt->verifySignature();
    info.is_signed = sigInfo.is_signed;
    info.signature_valid = sigInfo.signature_valid;
    info.package_valid = sigInfo.package_valid;
    info.signer_did = sigInfo.signer_did.empty() ? nullptr : strdup_cpp(sigInfo.signer_did);
    info.signer_name = sigInfo.signer_name.empty() ? nullptr : strdup_cpp(sigInfo.signer_name);
    info.signer_url = sigInfo.signer_url.empty() ? nullptr : strdup_cpp(sigInfo.signer_url);
    info.error = sigInfo.error.empty() ? nullptr : strdup_cpp(sigInfo.error);

    // Check keyring for trust status
    if (info.is_signed && info.signature_valid && !sigInfo.signer_did.empty()) {
        std::filesystem::path krDir;
        if (keyring_dir) {
            krDir = keyring_dir;
        } else {
            krDir = lgx::crypto::Keyring::defaultDirectory();
        }

        if (!krDir.empty() && std::filesystem::exists(krDir)) {
            lgx::crypto::Keyring keyring(krDir);
            auto trusted = keyring.findByDid(sigInfo.signer_did);
            if (trusted) {
                info.trusted_as = strdup_cpp(trusted->name);
            }
        }
    }

    return info;
}

LGX_EXPORT void lgx_free_signature_info(lgx_signature_info_t info) {
    if (info.signer_did) free(const_cast<char*>(info.signer_did));
    if (info.signer_name) free(const_cast<char*>(info.signer_name));
    if (info.signer_url) free(const_cast<char*>(info.signer_url));
    if (info.trusted_as) free(const_cast<char*>(info.trusted_as));
    if (info.error) free(const_cast<char*>(info.error));
}

LGX_EXPORT lgx_result_t lgx_sign(
    const char* lgx_path, const char* secret_key_path,
    const char* signer_name, const char* signer_url) {
    if (!lgx_path || !secret_key_path) {
        set_error("Invalid arguments: lgx_path and secret_key_path cannot be NULL");
        return {false, g_last_error.c_str()};
    }

    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        return {false, g_last_error.c_str()};
    }

    // Load secret key from file
    std::filesystem::path skPath(secret_key_path);
    auto keyName = skPath.stem().string();
    auto keysDir = skPath.parent_path();

    auto sk = lgx::crypto::Keyring::loadSecretKey(keysDir, keyName);
    if (!sk) {
        set_error("Failed to load secret key: " + lgx::crypto::Keyring::getLastError());
        return {false, g_last_error.c_str()};
    }

    auto pkg_opt = lgx::Package::load(lgx_path);
    if (!pkg_opt) {
        set_error("Failed to load package: " + std::string(lgx_path));
        return {false, g_last_error.c_str()};
    }

    std::string name = signer_name ? signer_name : "";
    std::string url = signer_url ? signer_url : "";

    auto signResult = pkg_opt->signPackage(*sk, name, url);
    if (!signResult.success) {
        set_error("Failed to sign package: " + signResult.error);
        return {false, g_last_error.c_str()};
    }

    auto saveResult = pkg_opt->save(lgx_path);
    if (!saveResult.success) {
        set_error(saveResult.error);
        return {false, g_last_error.c_str()};
    }

    return {true, nullptr};
}

LGX_EXPORT lgx_result_t lgx_keygen(
    const char* name, const char* output_dir) {
    if (!name) {
        set_error("Invalid argument: name cannot be NULL");
        return {false, g_last_error.c_str()};
    }

    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        return {false, g_last_error.c_str()};
    }

    std::filesystem::path keysDir;
    if (output_dir) {
        keysDir = output_dir;
    } else {
        keysDir = lgx::crypto::Keyring::defaultKeysDirectory();
    }

    if (keysDir.empty()) {
        set_error("Cannot determine keys directory");
        return {false, g_last_error.c_str()};
    }

    auto kp = lgx::crypto::generateKeypair();
    if (!lgx::crypto::Keyring::saveKeypair(keysDir, name, kp)) {
        set_error("Failed to save keypair: " + lgx::crypto::Keyring::getLastError());
        return {false, g_last_error.c_str()};
    }

    return {true, nullptr};
}

LGX_EXPORT lgx_result_t lgx_keyring_add(
    const char* keyring_dir, const char* name, const char* did,
    const char* display_name, const char* url) {
    if (!name || !did) {
        set_error("Invalid arguments: name and did cannot be NULL");
        return {false, g_last_error.c_str()};
    }

    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        return {false, g_last_error.c_str()};
    }

    std::filesystem::path krDir;
    if (keyring_dir) {
        krDir = keyring_dir;
    } else {
        krDir = lgx::crypto::Keyring::defaultDirectory();
    }

    if (krDir.empty()) {
        set_error("Cannot determine keyring directory");
        return {false, g_last_error.c_str()};
    }

    std::string dispName = display_name ? display_name : "";
    std::string urlStr = url ? url : "";

    lgx::crypto::Keyring keyring(krDir);
    if (!keyring.addKey(name, did, dispName, urlStr)) {
        set_error("Failed to add key: " + lgx::crypto::Keyring::getLastError());
        return {false, g_last_error.c_str()};
    }

    return {true, nullptr};
}

LGX_EXPORT lgx_result_t lgx_keyring_remove(
    const char* keyring_dir, const char* name) {
    if (!name) {
        set_error("Invalid argument: name cannot be NULL");
        return {false, g_last_error.c_str()};
    }

    std::filesystem::path krDir;
    if (keyring_dir) {
        krDir = keyring_dir;
    } else {
        krDir = lgx::crypto::Keyring::defaultDirectory();
    }

    if (krDir.empty()) {
        set_error("Cannot determine keyring directory");
        return {false, g_last_error.c_str()};
    }

    lgx::crypto::Keyring keyring(krDir);
    if (!keyring.removeKey(name)) {
        set_error("Failed to remove key: " + lgx::crypto::Keyring::getLastError());
        return {false, g_last_error.c_str()};
    }

    return {true, nullptr};
}

LGX_EXPORT lgx_keyring_list_t lgx_keyring_list(const char* keyring_dir) {
    lgx_keyring_list_t result = {};

    if (!lgx::crypto::init()) {
        set_error("Failed to initialize crypto library");
        return result;
    }

    std::filesystem::path krDir;
    if (keyring_dir) {
        krDir = keyring_dir;
    } else {
        krDir = lgx::crypto::Keyring::defaultDirectory();
    }

    if (krDir.empty()) {
        set_error("Cannot determine keyring directory");
        return result;
    }

    lgx::crypto::Keyring keyring(krDir);
    auto keys = keyring.listKeys();

    if (keys.empty()) {
        return result;
    }

    result.keys = static_cast<lgx_trusted_key_t*>(
        calloc(keys.size(), sizeof(lgx_trusted_key_t)));
    if (!result.keys) {
        return result;
    }
    result.count = keys.size();

    for (size_t i = 0; i < keys.size(); ++i) {
        result.keys[i].name = strdup_cpp(keys[i].name);
        result.keys[i].did = strdup_cpp(keys[i].did);
        result.keys[i].display_name = keys[i].displayName.empty()
            ? nullptr : strdup_cpp(keys[i].displayName);
        result.keys[i].url = keys[i].url.empty()
            ? nullptr : strdup_cpp(keys[i].url);
        result.keys[i].added_at = keys[i].addedAt.empty()
            ? nullptr : strdup_cpp(keys[i].addedAt);
    }

    return result;
}

LGX_EXPORT void lgx_free_keyring_list(lgx_keyring_list_t list) {
    if (!list.keys) return;
    for (size_t i = 0; i < list.count; ++i) {
        free(const_cast<char*>(list.keys[i].name));
        free(const_cast<char*>(list.keys[i].did));
        if (list.keys[i].display_name) free(const_cast<char*>(list.keys[i].display_name));
        if (list.keys[i].url) free(const_cast<char*>(list.keys[i].url));
        if (list.keys[i].added_at) free(const_cast<char*>(list.keys[i].added_at));
    }
    free(list.keys);
}

/* Memory management */

LGX_EXPORT void lgx_free_package(lgx_package_t pkg) {
    if (pkg) {
        delete pkg;
    }
}

LGX_EXPORT void lgx_free_string_array(const char** array) {
    if (!array) return;
    
    for (size_t i = 0; array[i] != nullptr; ++i) {
        free(const_cast<char*>(array[i]));
    }
    free(array);
}

LGX_EXPORT void lgx_free_verify_result(lgx_verify_result_t result) {
    if (result.errors) {
        lgx_free_string_array(result.errors);
    }
    if (result.warnings) {
        lgx_free_string_array(result.warnings);
    }
}

/* Error handling */

LGX_EXPORT const char* lgx_get_last_error(void) {
    return g_last_error.c_str();
}

/* Semver */

LGX_EXPORT int lgx_semver_compare(const char* a, const char* b) {
    return logos::semver::compare(a ? a : "", b ? b : "");
}

LGX_EXPORT bool lgx_semver_valid(const char* version) {
    return version && logos::semver::valid(version);
}

LGX_EXPORT bool lgx_semver_satisfies(const char* version, const char* range) {
    if (!version) return false;
    return logos::semver::satisfies(version, range ? range : "");
}

LGX_EXPORT bool lgx_semver_valid_range(const char* range) {
    return range && logos::semver::valid_range(range);
}

/* Version info */

LGX_EXPORT const char* lgx_version(void) {
    return "0.1.0";
}
