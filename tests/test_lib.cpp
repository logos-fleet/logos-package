#include <gtest/gtest.h>
#include "lgx.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

class LibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test files
        test_dir_ = std::filesystem::temp_directory_path() / "lgx_test_lib";
        std::filesystem::create_directories(test_dir_);
    }
    
    void TearDown() override {
        // Clean up test files
        std::filesystem::remove_all(test_dir_);
    }
    
    std::filesystem::path test_dir_;
};

TEST_F(LibraryTest, VersionTest) {
    const char* version = lgx_version();
    ASSERT_NE(version, nullptr);
    EXPECT_STREQ(version, "0.1.0");
}

TEST_F(LibraryTest, CreatePackage) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    lgx_result_t result = lgx_create(output_path.c_str(), "testpkg");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.error, nullptr);
    
    // Verify file was created
    EXPECT_TRUE(std::filesystem::exists(output_path));
}

TEST_F(LibraryTest, CreatePackageInvalidArgs) {
    lgx_result_t result = lgx_create(nullptr, "testpkg");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, nullptr);
    
    result = lgx_create("test.lgx", nullptr);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, nullptr);
}

TEST_F(LibraryTest, LoadPackage) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    // Create package first
    lgx_result_t result = lgx_create(output_path.c_str(), "testpkg");
    ASSERT_TRUE(result.success);
    
    // Load it
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Cleanup
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, LoadPackageInvalidPath) {
    lgx_package_t pkg = lgx_load("/nonexistent/path.lgx");
    EXPECT_EQ(pkg, nullptr);
    
    const char* error = lgx_get_last_error();
    EXPECT_NE(error, nullptr);
    EXPECT_NE(strlen(error), 0);
}

TEST_F(LibraryTest, LoadPackageNullArg) {
    lgx_package_t pkg = lgx_load(nullptr);
    EXPECT_EQ(pkg, nullptr);
    
    const char* error = lgx_get_last_error();
    EXPECT_NE(error, nullptr);
}

TEST_F(LibraryTest, GetPackageMetadata) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    // Create and load package
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Get name
    const char* name = lgx_get_name(pkg);
    ASSERT_NE(name, nullptr);
    EXPECT_STREQ(name, "testpkg");
    
    // Get version
    const char* version = lgx_get_version(pkg);
    ASSERT_NE(version, nullptr);
    EXPECT_STREQ(version, "0.0.1");
    
    // Get description (should be empty initially)
    const char* desc = lgx_get_description(pkg);
    ASSERT_NE(desc, nullptr);

    // Get icon (should be empty initially)
    const char* icon = lgx_get_icon(pkg);
    ASSERT_NE(icon, nullptr);
    EXPECT_STREQ(icon, "");

    lgx_free_package(pkg);
}

TEST_F(LibraryTest, SetPackageMetadata) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    // Create and load package
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Set version
    lgx_result_t result = lgx_set_version(pkg, "1.2.3");
    EXPECT_TRUE(result.success);
    
    const char* version = lgx_get_version(pkg);
    EXPECT_STREQ(version, "1.2.3");
    
    // Set description
    lgx_set_description(pkg, "Test package");
    const char* desc = lgx_get_description(pkg);
    EXPECT_STREQ(desc, "Test package");

    // Set icon
    lgx_set_icon(pkg, "icon.png");
    const char* icon = lgx_get_icon(pkg);
    EXPECT_STREQ(icon, "icon.png");

    lgx_free_package(pkg);
}

TEST_F(LibraryTest, SavePackage) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto output_path2 = (test_dir_ / "test2.lgx").string();
    
    // Create and load package
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Modify it
    lgx_set_version(pkg, "1.0.0");
    lgx_set_description(pkg, "Modified package");
    
    // Save to new location
    lgx_result_t result = lgx_save(pkg, output_path2.c_str());
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(std::filesystem::exists(output_path2));
    
    lgx_free_package(pkg);
    
    // Verify changes persisted
    lgx_package_t pkg2 = lgx_load(output_path2.c_str());
    ASSERT_NE(pkg2, nullptr);
    
    EXPECT_STREQ(lgx_get_version(pkg2), "1.0.0");
    EXPECT_STREQ(lgx_get_description(pkg2), "Modified package");
    
    lgx_free_package(pkg2);
}

TEST_F(LibraryTest, VerifyPackage) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    // Create package
    lgx_create(output_path.c_str(), "testpkg");
    
    // Verify it
    lgx_verify_result_t result = lgx_verify(output_path.c_str());
    EXPECT_TRUE(result.valid);
    
    // Should have no errors
    if (result.errors) {
        for (int i = 0; result.errors[i]; i++) {
            // Print any errors for debugging
            printf("Error: %s\n", result.errors[i]);
        }
    }
    
    lgx_free_verify_result(result);
}

TEST_F(LibraryTest, VerifyInvalidPackage) {
    lgx_verify_result_t result = lgx_verify("/nonexistent/path.lgx");
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.errors, nullptr);
    
    // Should have at least one error
    EXPECT_NE(result.errors[0], nullptr);
    
    lgx_free_verify_result(result);
}

TEST_F(LibraryTest, AddVariantSingleFile) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto file_path = (test_dir_ / "test.txt").string();
    
    // Create a test file
    std::ofstream(file_path) << "test content";
    
    // Create and load package
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Add variant with single file
    lgx_result_t result = lgx_add_variant(pkg, "test-variant", file_path.c_str(), "test.txt");
    EXPECT_TRUE(result.success) << (result.error ? result.error : "");
    
    // Check variant exists
    EXPECT_TRUE(lgx_has_variant(pkg, "test-variant"));
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, HasVariant) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Should not have any variants initially
    EXPECT_FALSE(lgx_has_variant(pkg, "nonexistent"));
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, GetVariants) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto file_path = (test_dir_ / "test.txt").string();
    
    // Create a test file
    std::ofstream(file_path) << "test content";
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Initially no variants
    const char** variants = lgx_get_variants(pkg);
    ASSERT_NE(variants, nullptr);
    EXPECT_EQ(variants[0], nullptr); // Empty array
    lgx_free_string_array(variants);
    
    // Add a variant
    lgx_add_variant(pkg, "test-variant", file_path.c_str(), "test.txt");
    
    // Now should have one variant
    variants = lgx_get_variants(pkg);
    ASSERT_NE(variants, nullptr);
    ASSERT_NE(variants[0], nullptr);
    EXPECT_STREQ(variants[0], "test-variant");
    EXPECT_EQ(variants[1], nullptr); // NULL-terminated
    lgx_free_string_array(variants);
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, RemoveVariant) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto file_path = (test_dir_ / "test.txt").string();
    
    // Create a test file
    std::ofstream(file_path) << "test content";
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Add variant
    lgx_add_variant(pkg, "test-variant", file_path.c_str(), "test.txt");
    EXPECT_TRUE(lgx_has_variant(pkg, "test-variant"));
    
    // Remove variant
    lgx_result_t result = lgx_remove_variant(pkg, "test-variant");
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(lgx_has_variant(pkg, "test-variant"));
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, RemoveNonexistentVariant) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    // Try to remove non-existent variant
    lgx_result_t result = lgx_remove_variant(pkg, "nonexistent");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, nullptr);
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, NullPackageHandles) {
    // All functions should handle NULL package gracefully
    EXPECT_FALSE(lgx_save(nullptr, "test.lgx").success);
    EXPECT_FALSE(lgx_add_variant(nullptr, "variant", "path", nullptr).success);
    EXPECT_FALSE(lgx_remove_variant(nullptr, "variant").success);
    EXPECT_FALSE(lgx_has_variant(nullptr, "variant"));
    EXPECT_EQ(lgx_get_variants(nullptr), nullptr);
    EXPECT_EQ(lgx_get_name(nullptr), nullptr);
    EXPECT_EQ(lgx_get_version(nullptr), nullptr);
    EXPECT_EQ(lgx_get_description(nullptr), nullptr);
    EXPECT_EQ(lgx_get_icon(nullptr), nullptr);

    // These should not crash with NULL
    lgx_free_package(nullptr);
}

TEST_F(LibraryTest, FreeStringArray) {
    // Test that freeing NULL is safe
    lgx_free_string_array(nullptr);
    
    // Test freeing an empty array
    const char** empty = static_cast<const char**>(malloc(sizeof(char*)));
    empty[0] = nullptr;
    lgx_free_string_array(empty);
}

TEST_F(LibraryTest, FreeVerifyResult) {
    // Test that freeing empty result is safe
    lgx_verify_result_t result = {true, nullptr, nullptr};
    lgx_free_verify_result(result);
}

// =============================================================================
// Extract Tests
// =============================================================================

TEST_F(LibraryTest, ExtractVariant) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto file_path = (test_dir_ / "test.txt").string();
    auto extract_dir = (test_dir_ / "extracted").string();
    
    std::ofstream(file_path) << "test content";
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    lgx_result_t result = lgx_add_variant(pkg, "test-variant", file_path.c_str(), "test.txt");
    ASSERT_TRUE(result.success);
    
    lgx_save(pkg, output_path.c_str());
    
    result = lgx_extract(pkg, "test-variant", extract_dir.c_str());
    EXPECT_TRUE(result.success) << (result.error ? result.error : "");
    
    auto extracted_file = std::filesystem::path(extract_dir) / "test-variant" / "test.txt";
    EXPECT_TRUE(std::filesystem::exists(extracted_file)) << "Expected: " << extracted_file.string();
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, ExtractAllVariants) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto file_path = (test_dir_ / "test.txt").string();
    auto extract_dir = (test_dir_ / "extracted").string();
    
    std::ofstream(file_path) << "test content";
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    lgx_add_variant(pkg, "variant1", file_path.c_str(), "test.txt");
    lgx_add_variant(pkg, "variant2", file_path.c_str(), "test.txt");
    lgx_save(pkg, output_path.c_str());
    
    lgx_result_t result = lgx_extract(pkg, nullptr, extract_dir.c_str());
    EXPECT_TRUE(result.success) << (result.error ? result.error : "");
    
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(extract_dir) / "variant1" / "test.txt"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(extract_dir) / "variant2" / "test.txt"));
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, ExtractNonexistentVariant) {
    auto output_path = (test_dir_ / "test.lgx").string();
    auto extract_dir = (test_dir_ / "extracted").string();
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    lgx_result_t result = lgx_extract(pkg, "nonexistent", extract_dir.c_str());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error, nullptr);
    
    lgx_free_package(pkg);
}

TEST_F(LibraryTest, ExtractNullArgs) {
    auto output_path = (test_dir_ / "test.lgx").string();
    
    lgx_create(output_path.c_str(), "testpkg");
    lgx_package_t pkg = lgx_load(output_path.c_str());
    ASSERT_NE(pkg, nullptr);
    
    lgx_result_t result = lgx_extract(nullptr, "variant", "/tmp");
    EXPECT_FALSE(result.success);
    
    result = lgx_extract(pkg, "variant", nullptr);
    EXPECT_FALSE(result.success);
    
    lgx_free_package(pkg);
}

// ============================================================================
// Carrying a signature out of a package, and checking it against a caller's DID
//
// lgx_extract() writes variants/<v>/ and assets/ only, so a consumer that
// installs a package holds the signed bytes without the signature over them.
// These two functions are what let the signature travel and be checked later,
// offline, against a key the ASKER chooses.
// ============================================================================

class ManifestSignatureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "lgx_test_manifest_sig";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "content");
        std::ofstream(dir_ / "content" / "payload.txt") << "hello";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    // A real, structurally valid package.
    std::string makePackage(const std::string& name, const std::string& version) {
        auto path = (dir_ / (name + ".lgx")).string();
        if (!lgx_create(path.c_str(), name.c_str()).success) return {};
        lgx_package_t pkg = lgx_load(path.c_str());
        if (!pkg) return {};
        lgx_set_version(pkg, version.c_str());
        auto res = lgx_add_variant(pkg, "linux-x86_64",
                                   (dir_ / "content").string().c_str(), "payload.txt");
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, path.c_str());
        lgx_free_package(pkg);
        return res.success ? path : std::string{};
    }
    std::string makeKey(const std::string& name) {
        if (!lgx_keygen(name.c_str(), dir_.string().c_str()).success) return {};
        return (dir_ / (name + ".jwk")).string();
    }
    std::string didOf(const std::string& keyName) {
        std::ifstream f(dir_ / (keyName + ".did"));
        std::string did; std::getline(f, did); return did;
    }
    // The two documents a consumer would carry into an install tree.
    bool carry(const std::string& lgxPath, std::string& manifest, std::string& sig) {
        lgx_package_t pkg = lgx_load(lgxPath.c_str());
        if (!pkg) return false;
        const char* m = lgx_get_manifest_json(pkg);
        const char* s = lgx_get_manifest_sig_json(pkg);
        manifest = m ? m : "";
        sig = s ? s : "";
        lgx_free_package(pkg);
        return !manifest.empty();
    }
    std::filesystem::path dir_;
};

TEST_F(ManifestSignatureTest, UnsignedPackageHasNoSignatureDocument) {
    auto path = makePackage("plain", "1.0.0");
    ASSERT_FALSE(path.empty());
    lgx_package_t pkg = lgx_load(path.c_str());
    ASSERT_NE(pkg, nullptr);
    EXPECT_EQ(lgx_get_manifest_sig_json(pkg), nullptr);
    lgx_free_package(pkg);
}

TEST_F(ManifestSignatureTest, NullPackageIsRejected) {
    EXPECT_EQ(lgx_get_manifest_sig_json(nullptr), nullptr);
}

// THE PREMISE. lgx_get_manifest_json() returns getManifest().toJson(), the
// same expression signPackage() signs, so the manifest a consumer carries away
// is byte-for-byte the message the signature covers.
TEST_F(ManifestSignatureTest, TheCarriedManifestIsTheSignedMessage) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto key = makeKey("pub");
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), key.c_str(), nullptr, nullptr).success);

    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));
    ASSERT_FALSE(sig.empty());

    EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                           sig.c_str(), didOf("pub").c_str()),
              LGX_SIG_CHECK_OK);
}

// THE PROPERTY THE PARAMETER EXISTS FOR. The document is entirely
// self-consistent -- a genuine signature by a real key, naming that key's DID
// -- and it is still refused when the caller asks about a DIFFERENT key.
TEST_F(ManifestSignatureTest, AnotherKeysGenuineSignatureIsAMismatch) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto attackerKey = makeKey("attacker");
    ASSERT_FALSE(attackerKey.empty());
    ASSERT_FALSE(makeKey("publisher").empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), attackerKey.c_str(), nullptr, nullptr).success);

    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));

    // Self-consistent: it verifies under its own DID.
    EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                           sig.c_str(), didOf("attacker").c_str()),
              LGX_SIG_CHECK_OK);
    // ...and that buys it nothing when the caller names the publisher.
    EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                           sig.c_str(), didOf("publisher").c_str()),
              LGX_SIG_CHECK_MISMATCH);
}

// The DID inside the document is never consulted for the key, so relabelling
// it changes nothing. This is the case a "compare the DID to the expected one,
// then verify with it" implementation would accept.
TEST_F(ManifestSignatureTest, RelabellingTheDidDoesNotChangeTheAnswer) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto attackerKey = makeKey("attacker");
    ASSERT_FALSE(attackerKey.empty());
    ASSERT_FALSE(makeKey("publisher").empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), attackerKey.c_str(), nullptr, nullptr).success);

    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));

    // Rewrite ONLY the did field to name the publisher, leaving the attacker's
    // genuine signature in place. Textual, so the test does not depend on the
    // JSON library the caller happens to use.
    const std::string publisherDid = didOf("publisher");
    const std::string attackerDid  = didOf("attacker");
    auto at = sig.find(attackerDid);
    ASSERT_NE(at, std::string::npos);
    std::string relabelled = sig.substr(0, at) + publisherDid
                           + sig.substr(at + attackerDid.size());
    ASSERT_NE(relabelled.find(publisherDid), std::string::npos);

    EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                           relabelled.c_str(), publisherDid.c_str()),
              LGX_SIG_CHECK_MISMATCH)
        << "a signature wearing the expected DID's name was accepted";
}

// A signature covers a MESSAGE, not a package name. The manifest carries the
// Merkle root over the payload, so a signature lifted from another package
// cannot describe these bytes.
TEST_F(ManifestSignatureTest, AGenuineSignatureOverOtherBytesIsAMismatch) {
    auto key = makeKey("pub");
    ASSERT_FALSE(key.empty());

    auto a = makePackage("pkg_a", "1.0.0");
    auto b = makePackage("pkg_b", "2.0.0");
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    ASSERT_TRUE(lgx_sign(a.c_str(), key.c_str(), nullptr, nullptr).success);
    ASSERT_TRUE(lgx_sign(b.c_str(), key.c_str(), nullptr, nullptr).success);

    std::string manifestA, sigA, manifestB, sigB;
    ASSERT_TRUE(carry(a, manifestA, sigA));
    ASSERT_TRUE(carry(b, manifestB, sigB));

    const std::string did = didOf("pub");
    EXPECT_EQ(lgx_check_manifest_signature(manifestA.data(), manifestA.size(),
                                           sigA.c_str(), did.c_str()),
              LGX_SIG_CHECK_OK);
    EXPECT_EQ(lgx_check_manifest_signature(manifestA.data(), manifestA.size(),
                                           sigB.c_str(), did.c_str()),
              LGX_SIG_CHECK_MISMATCH);
}

// A caller's DID that is not a did:jwk Ed25519 key is reported as the CALLER's
// error, distinctly from unusable evidence. Callers rank the two differently:
// missing evidence is commonly tolerated, an unsatisfiable expectation is not,
// and collapsing them would let a typo'd DID be waved through.
TEST_F(ManifestSignatureTest, ABadExpectedDidIsItsOwnAnswer) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto key = makeKey("pub");
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), key.c_str(), nullptr, nullptr).success);
    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));

    for (const char* bad : {"", "did:jwk:!!!!", "did:web:example.com", "nonsense"}) {
        EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                               sig.c_str(), bad),
                  LGX_SIG_CHECK_BAD_DID) << bad;
    }
    EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                           sig.c_str(), nullptr),
              LGX_SIG_CHECK_BAD_DID);
}

// Evidence that is absent or unreadable refutes nothing, and is reported
// separately from evidence that refutes.
TEST_F(ManifestSignatureTest, UnusableEvidenceIsNotAMismatch) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto key = makeKey("pub");
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), key.c_str(), nullptr, nullptr).success);
    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));
    const std::string did = didOf("pub");

    const char* unusable[] = {
        nullptr,                                             // no document
        "",                                                  // empty
        "not json at all",                                   // unparseable
        R"({"version":2,"algorithm":"ed25519","did":"x","signature":"AA"})",  // version
        R"({"version":1,"algorithm":"rsa","did":"x","signature":"AA"})",      // algorithm
        R"({"version":1,"algorithm":"ed25519","did":"x","signature":"AA"})",  // short sig
    };
    for (const char* u : unusable) {
        EXPECT_EQ(lgx_check_manifest_signature(manifest.data(), manifest.size(),
                                               u, did.c_str()),
                  LGX_SIG_CHECK_UNUSABLE) << (u ? u : "(null)");
    }
}

// An empty message is a message. It must not be mistaken for "nothing to
// check": a caller handing over a truncated manifest gets a mismatch, not a
// pass.
TEST_F(ManifestSignatureTest, AnEmptyMessageIsAMismatchNotAPass) {
    auto path = makePackage("signed", "1.0.0");
    ASSERT_FALSE(path.empty());
    auto key = makeKey("pub");
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(lgx_sign(path.c_str(), key.c_str(), nullptr, nullptr).success);
    std::string manifest, sig;
    ASSERT_TRUE(carry(path, manifest, sig));

    EXPECT_EQ(lgx_check_manifest_signature("", 0, sig.c_str(), didOf("pub").c_str()),
              LGX_SIG_CHECK_MISMATCH);
    EXPECT_EQ(lgx_check_manifest_signature(nullptr, 99, sig.c_str(), didOf("pub").c_str()),
              LGX_SIG_CHECK_MISMATCH);
}

// ---------------------------------------------------------------------------
// Installed-package checks
//
// The archive is gone by the time a module is installed: one variant, flattened
// into a directory, with manifest.json and `variant` written beside it. These
// exercise the C ABI a host holding such a directory calls.
// ---------------------------------------------------------------------------

namespace {

// A NULL-terminated array the library owns; joined for substring assertions.
std::string joinErrors(const lgx_verify_result_t& r) {
    std::string out;
    if (r.errors) {
        for (size_t i = 0; r.errors[i]; ++i) out += std::string("\n  ") + r.errors[i];
    }
    return out;
}

bool hasError(const lgx_verify_result_t& r, const std::string& needle) {
    if (!r.errors) return false;
    for (size_t i = 0; r.errors[i]; ++i) {
        if (std::string(r.errors[i]).find(needle) != std::string::npos) return true;
    }
    return false;
}

// A parseable manifest with `field` replaced, for the field-rule table.
std::string manifestWith(const std::string& extra) {
    return std::string(R"({"manifestVersion":"0.5.0","name":"pkg","version":"1.0.0",)"
                       R"("description":"d","author":"a","type":"core",)"
                       R"("category":"c","icon":"","dependencies":[])") +
           extra + "}";
}

} // namespace

class InstalledAbiTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "lgx_test_installed_abi";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_ / "content" / "lib");
        std::ofstream(dir_ / "content" / "payload.txt") << "hello";
        std::ofstream(dir_ / "content" / "lib" / "dep.txt") << "dependency";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::string makePackage(const std::string& name) {
        auto path = (dir_ / (name + ".lgx")).string();
        if (!lgx_create(path.c_str(), name.c_str()).success) return {};
        lgx_package_t pkg = lgx_load(path.c_str());
        if (!pkg) return {};
        auto res = lgx_add_variant(pkg, "linux-x86_64",
                                   (dir_ / "content").string().c_str(), "payload.txt");
        if (res.success) res = lgx_save(pkg, path.c_str());
        lgx_free_package(pkg);
        return res.success ? path : std::string{};
    }

    // Install the way lgpm does: extract the variant, then write the root
    // manifest.json and the `variant` file into the extracted directory.
    std::filesystem::path install(const std::string& lgxPath,
                                  const std::string& variant,
                                  std::string& manifestOut) {
        lgx_package_t pkg = lgx_load(lgxPath.c_str());
        EXPECT_NE(pkg, nullptr);
        if (!pkg) return {};
        std::filesystem::path out = dir_ / "install";
        EXPECT_TRUE(lgx_extract(pkg, variant.c_str(), out.string().c_str()).success);
        const char* m = lgx_get_manifest_json(pkg);
        manifestOut = m ? m : "";
        lgx_free_package(pkg);

        std::filesystem::path installed = out / variant;
        std::ofstream(installed / "manifest.json", std::ios::binary) << manifestOut;
        std::ofstream(installed / "variant") << variant;
        return installed;
    }

    std::filesystem::path dir_;
};

TEST_F(InstalledAbiTest, ManifestValidateAcceptsAWellFormedManifest) {
    const std::string m = manifestWith("");
    lgx_verify_result_t r = lgx_manifest_validate(m.data(), m.size());
    EXPECT_TRUE(r.valid) << joinErrors(r);
    lgx_free_verify_result(r);
}

// Every rule Manifest::validate() carries, reported through the ABI with the
// same wording `lgx verify` prints.
TEST_F(InstalledAbiTest, ManifestValidateReportsEveryRule) {
    struct Case { std::string manifest; std::string expected; };
    const std::vector<Case> cases = {
        {R"({"manifestVersion":"1.0.0","name":"p","version":"1.0.0","description":"d",)"
         R"("author":"a","type":"core","category":"c","icon":"","dependencies":[]})",
         "Unsupported manifest version: 1.0.0"},
        {R"({"manifestVersion":"0.5.0","name":"","version":"1.0.0","description":"d",)"
         R"("author":"a","type":"core","category":"c","icon":"","dependencies":[]})",
         "'name' field is empty"},
        {R"({"manifestVersion":"0.5.0","name":"p","version":"","description":"d",)"
         R"("author":"a","type":"core","category":"c","icon":"","dependencies":[]})",
         "'version' field is empty"},
        {R"({"manifestVersion":"0.5.0","name":"p","version":"1.0","description":"d",)"
         R"("author":"a","type":"core","category":"c","icon":"","dependencies":[]})",
         "'version' is not a valid SemVer 2.0.0 version: '1.0'"},
        {manifestWith(R"(,"main":{"linux-amd64":"../../etc/passwd"})"),
         "Invalid main path for 'linux-amd64': Path contains '..' segment"},
        {manifestWith(R"(,"view":"/abs/Main.qml")"),
         "Invalid view path: Path is absolute"},
        {R"({"manifestVersion":"0.5.0","name":"p","version":"1.0.0","description":"d",)"
         R"("author":"a","type":"ui_qml","category":"c","icon":"","dependencies":[]})",
         "'view' field is required for ui_qml packages"},
        {manifestWith(R"(,"dependencies":[{"name":""}])"),
         "Dependency with empty name"},
        {manifestWith(R"(,"dependencies":[{"name":"Waku"}])"),
         "Dependency name 'Waku' is not lowercase"},
        {manifestWith(R"(,"dependencies":[{"name":"waku","version":"not-a-range"}])"),
         "Dependency 'waku' has invalid semver range: 'not-a-range'"},
        {manifestWith(R"(,"dependencies":[{"name":"waku","signer":"did:web:x"}])"),
         "Dependency 'waku' has invalid signer DID: 'did:web:x'"},
    };

    for (const auto& c : cases) {
        lgx_verify_result_t r = lgx_manifest_validate(c.manifest.data(), c.manifest.size());
        EXPECT_FALSE(r.valid) << c.expected;
        EXPECT_TRUE(hasError(r, "Manifest: " + c.expected))
            << c.expected << " ->" << joinErrors(r);
        lgx_free_verify_result(r);
    }
}

TEST_F(InstalledAbiTest, ManifestValidateReportsParseFailures) {
    const std::string notJson = "not json";
    lgx_verify_result_t r = lgx_manifest_validate(notJson.data(), notJson.size());
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "Failed to parse manifest")) << joinErrors(r);
    lgx_free_verify_result(r);

    const std::string noName =
        R"({"manifestVersion":"0.5.0","version":"1.0.0","description":"d",)"
        R"("author":"a","type":"core","category":"c","icon":"","dependencies":[]})";
    r = lgx_manifest_validate(noName.data(), noName.size());
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "Missing or invalid 'name' field")) << joinErrors(r);
    lgx_free_verify_result(r);

    r = lgx_manifest_validate(nullptr, 0);
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "Missing manifest.json")) << joinErrors(r);
    lgx_free_verify_result(r);
}

// The length governs, not a terminator: the bytes a caller carries come from a
// file read, and the same bytes go to lgx_check_manifest_signature().
TEST_F(InstalledAbiTest, ManifestValidateHonoursTheLength) {
    std::string m = manifestWith("");
    const size_t len = m.size();
    m += "trailing garbage that is not JSON";

    lgx_verify_result_t r = lgx_manifest_validate(m.data(), len);
    EXPECT_TRUE(r.valid) << joinErrors(r);
    lgx_free_verify_result(r);

    r = lgx_manifest_validate(m.data(), m.size());
    EXPECT_FALSE(r.valid);
    lgx_free_verify_result(r);
}

TEST_F(InstalledAbiTest, VerifyInstalledAcceptsAnUntouchedInstall) {
    auto path = makePackage("pkg");
    ASSERT_FALSE(path.empty());
    std::string manifest;
    auto installed = install(path, "linux-x86_64", manifest);
    ASSERT_FALSE(manifest.empty());

    EXPECT_EQ(lgx_verify_installed_tree(installed.string().c_str(),
                                        manifest.data(), manifest.size(),
                                        "linux-x86_64"),
              LGX_INTEGRITY_OK)
        << lgx_get_last_error();

    lgx_verify_result_t r = lgx_verify_installed(installed.string().c_str(),
                                                 manifest.data(), manifest.size(),
                                                 "linux-x86_64");
    EXPECT_TRUE(r.valid) << joinErrors(r);
    lgx_free_verify_result(r);
}

TEST_F(InstalledAbiTest, VerifyInstalledDetectsTampering) {
    auto path = makePackage("pkg");
    ASSERT_FALSE(path.empty());
    std::string manifest;
    auto installed = install(path, "linux-x86_64", manifest);
    std::ofstream(installed / "lib" / "dep.txt", std::ios::binary) << "dependencY";

    EXPECT_EQ(lgx_verify_installed_tree(installed.string().c_str(),
                                        manifest.data(), manifest.size(),
                                        "linux-x86_64"),
              LGX_INTEGRITY_MISMATCH);

    lgx_verify_result_t r = lgx_verify_installed(installed.string().c_str(),
                                                 manifest.data(), manifest.size(),
                                                 "linux-x86_64");
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "Content hash mismatch")) << joinErrors(r);
    lgx_free_verify_result(r);
}

// Four non-Ok answers, four different remedies. Collapsing any of them into
// "not verified" would let a caller treat a typo as a clean package.
TEST_F(InstalledAbiTest, TheIntegrityAnswersAreDistinct) {
    auto path = makePackage("pkg");
    ASSERT_FALSE(path.empty());
    std::string manifest;
    auto installed = install(path, "linux-x86_64", manifest);
    const std::string dir = installed.string();

    EXPECT_EQ(lgx_verify_installed_tree(dir.c_str(), manifest.data(), manifest.size(),
                                        "freebsd-x86"),
              LGX_INTEGRITY_NO_HASH);
    EXPECT_EQ(lgx_verify_installed_tree((dir + "-gone").c_str(),
                                        manifest.data(), manifest.size(),
                                        "linux-x86_64"),
              LGX_INTEGRITY_UNREADABLE);
    EXPECT_EQ(lgx_verify_installed_tree(dir.c_str(), manifest.data(), manifest.size(),
                                        nullptr),
              LGX_INTEGRITY_BAD_INPUT);
    EXPECT_EQ(lgx_verify_installed_tree(nullptr, manifest.data(), manifest.size(),
                                        "linux-x86_64"),
              LGX_INTEGRITY_BAD_INPUT);
    const std::string junk = "not json";
    EXPECT_EQ(lgx_verify_installed_tree(dir.c_str(), junk.data(), junk.size(),
                                        "linux-x86_64"),
              LGX_INTEGRITY_BAD_INPUT);
}

TEST_F(InstalledAbiTest, VerifyInstalledReportsAMissingMain) {
    auto path = makePackage("pkg");
    ASSERT_FALSE(path.empty());
    std::string manifest;
    auto installed = install(path, "linux-x86_64", manifest);
    std::filesystem::remove(installed / "payload.txt");

    lgx_verify_result_t r = lgx_verify_installed(installed.string().c_str(),
                                                 manifest.data(), manifest.size(),
                                                 "linux-x86_64");
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "main[linux-x86_64] points to non-existent file: payload.txt"))
        << joinErrors(r);
    lgx_free_verify_result(r);
}

TEST_F(InstalledAbiTest, VerifyInstalledRejectsUnusableInput) {
    lgx_verify_result_t r = lgx_verify_installed(nullptr, "", 0, "linux-x86_64");
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "dir_path cannot be NULL")) << joinErrors(r);
    lgx_free_verify_result(r);

    r = lgx_verify_installed("/tmp", nullptr, 0, "linux-x86_64");
    EXPECT_FALSE(r.valid);
    EXPECT_TRUE(hasError(r, "Missing manifest.json")) << joinErrors(r);
    lgx_free_verify_result(r);
}

// =============================================================================
// Variant names and `main` resolution across the C ABI
//
// The C++ side is covered by test_platform_variant.cpp and
// test_main_resolution.cpp; what is at stake here is the ABI contract itself —
// who owns which pointer, and that "not applicable" is NULL rather than "".
// =============================================================================

class VariantAbiTest : public ::testing::Test {
protected:
    std::vector<std::string> spellings(const char* variant) {
        std::vector<std::string> out;
        const char** array = lgx_variant_spellings(variant);
        if (!array) return out;
        for (const char** v = array; *v != nullptr; ++v) out.emplace_back(*v);
        lgx_free_string_array(array);
        return out;
    }
};

TEST_F(VariantAbiTest, HostVariantIsStableStorage) {
    const char* first = lgx_host_variant();
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(std::string(first).empty());
    // Owned by the library, so a second call hands back the same storage and
    // the caller never frees it.
    EXPECT_EQ(first, lgx_host_variant());
}

TEST_F(VariantAbiTest, SpellingsLeadWithTheInputAndCarryItsAliases) {
    EXPECT_EQ(spellings("darwin-x86_64"),
              (std::vector<std::string>{"darwin-x86_64", "darwin-amd64"}));
    EXPECT_EQ(spellings("linux-arm64"),
              (std::vector<std::string>{"linux-arm64", "linux-aarch64"}));
}

TEST_F(VariantAbiTest, SpellingsOfNothingIsAnEmptyArrayNotNull) {
    const char** array = lgx_variant_spellings(nullptr);
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array[0], nullptr);
    lgx_free_string_array(array);

    array = lgx_variant_spellings("");
    ASSERT_NE(array, nullptr);
    EXPECT_EQ(array[0], nullptr);
    lgx_free_string_array(array);
}

TEST_F(VariantAbiTest, KnownVariantsCrossesTheAbiAndCarriesTheMobileTargets) {
    const char** array = lgx_known_variants();
    ASSERT_NE(array, nullptr);
    std::vector<std::string> known;
    for (const char** v = array; *v != nullptr; ++v) known.emplace_back(*v);
    lgx_free_string_array(array);

    for (const char* v : { "darwin-arm64", "android-arm64", "android-x86_64",
                           "ios-arm64", "ios-sim-arm64", "web" }) {
        EXPECT_NE(std::find(known.begin(), known.end(), v), known.end()) << v;
    }
}

TEST_F(VariantAbiTest, ASuggestionIsNullWhenThereIsNothingToSay) {
    // "not applicable" is NULL, never "": an accepted name and a name from
    // someone else's vocabulary both leave the caller nothing to print.
    EXPECT_EQ(lgx_variant_suggestion("ios-arm64"), nullptr);
    EXPECT_EQ(lgx_variant_suggestion("my-own-target"), nullptr);
    EXPECT_EQ(lgx_variant_suggestion(nullptr), nullptr);
    EXPECT_EQ(lgx_variant_suggestion(""), nullptr);
}

TEST_F(VariantAbiTest, ASuggestionNamesTheTargetThatWasMeant) {
    const char* meant = lgx_variant_suggestion("ios_arm64");
    ASSERT_NE(meant, nullptr);
    EXPECT_EQ(std::string(meant), "ios-arm64");

    meant = lgx_variant_suggestion("wasm");
    ASSERT_NE(meant, nullptr);
    EXPECT_EQ(std::string(meant), "web");
}

TEST_F(VariantAbiTest, IsKnownVouchesForCanonicalNamesAliasesAndTheDevFlavour) {
    EXPECT_TRUE(lgx_variant_is_known("ios-sim-arm64"));
    EXPECT_TRUE(lgx_variant_is_known("android-aarch64"));
    EXPECT_TRUE(lgx_variant_is_known("darwin-arm64-dev"));
    EXPECT_TRUE(lgx_variant_is_known("web"));
    EXPECT_FALSE(lgx_variant_is_known("ios_arm64"));
    EXPECT_FALSE(lgx_variant_is_known(nullptr));
}

class MainAbiTest : public ::testing::Test {
protected:
    std::filesystem::path dir;

    void SetUp() override {
        dir = std::filesystem::temp_directory_path() /
              ("lgx_main_abi_" + std::to_string(::rand()));
        std::filesystem::create_directories(dir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void writeFile(const std::string& name, const std::string& content) {
        std::ofstream f(dir / name, std::ios::binary);
        f << content;
    }
};

TEST_F(MainAbiTest, ResolvedCarriesEveryFieldAndFreesClean) {
    writeFile("p.so", "ELF");
    const std::string manifest = R"({"main":{"linux-amd64":"p.so"}})";
    const char* variants[] = { "linux-amd64", nullptr };

    lgx_main_file_t r = lgx_resolve_main(dir.string().c_str(),
                                         manifest.data(), manifest.size(), variants);
    EXPECT_EQ(r.state, LGX_MAIN_RESOLVED);
    ASSERT_NE(r.path, nullptr);
    EXPECT_EQ(std::string(r.path), (dir / "p.so").string());
    ASSERT_NE(r.declared_path, nullptr);
    EXPECT_STREQ(r.declared_path, "p.so");
    ASSERT_NE(r.variant, nullptr);
    EXPECT_STREQ(r.variant, "linux-amd64");
    EXPECT_EQ(r.error, nullptr);
    lgx_free_main_file(r);
}

TEST_F(MainAbiTest, AnUnresolvedMainCarriesNoPath) {
    const std::string manifest = R"({"main":{"linux-amd64":"p.so"}})";
    const char* variants[] = { "linux-amd64", nullptr };

    lgx_main_file_t r = lgx_resolve_main(dir.string().c_str(),
                                         manifest.data(), manifest.size(), variants);
    EXPECT_EQ(r.state, LGX_MAIN_FILE_MISSING);
    EXPECT_EQ(r.path, nullptr);
    EXPECT_STREQ(r.declared_path, "p.so");
    ASSERT_NE(r.error, nullptr);
    lgx_free_main_file(r);
}

TEST_F(MainAbiTest, ANullVariantListIsNoVariantMatchNotACrash) {
    const std::string manifest = R"({"main":{"linux-amd64":"p.so"}})";

    lgx_main_file_t r = lgx_resolve_main(dir.string().c_str(),
                                         manifest.data(), manifest.size(), nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_NO_VARIANT_MATCH);
    EXPECT_EQ(r.path, nullptr);
    lgx_free_main_file(r);
}

TEST_F(MainAbiTest, TheLengthIsHonouredNotTheNulByte) {
    writeFile("p.so", "ELF");
    // Trailing garbage past manifest_len must not be parsed.
    const std::string manifest = R"({"main":"p.so"} trailing garbage)";
    const size_t realLen = manifest.find('}') + 1;

    lgx_main_file_t r = lgx_resolve_main(dir.string().c_str(),
                                         manifest.data(), realLen, nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_RESOLVED);
    lgx_free_main_file(r);
}

TEST_F(MainAbiTest, UnusableInputIsBadInputAndReachesTheLastError) {
    lgx_main_file_t r = lgx_resolve_main(nullptr, "{}", 2, nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_BAD_INPUT);
    EXPECT_EQ(r.path, nullptr);
    ASSERT_NE(r.error, nullptr);
    EXPECT_STREQ(lgx_get_last_error(), r.error);
    lgx_free_main_file(r);

    r = lgx_resolve_main(dir.string().c_str(), "{ not json", 10, nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_BAD_INPUT);
    lgx_free_main_file(r);

    r = lgx_resolve_main(dir.string().c_str(), nullptr, 0, nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_BAD_INPUT);
    EXPECT_STREQ(r.error, "Missing manifest.json");
    lgx_free_main_file(r);
}

TEST_F(MainAbiTest, TheContainmentGuardHoldsAcrossTheAbi) {
    std::ofstream(dir.parent_path() / "outside.so", std::ios::binary) << "ELF";
    const std::string manifest = R"({"main":"../outside.so"})";

    lgx_main_file_t r = lgx_resolve_main(dir.string().c_str(),
                                         manifest.data(), manifest.size(), nullptr);
    EXPECT_EQ(r.state, LGX_MAIN_MALFORMED_ENTRY);
    EXPECT_EQ(r.path, nullptr);
    lgx_free_main_file(r);

    std::error_code ec;
    std::filesystem::remove(dir.parent_path() / "outside.so", ec);
}
