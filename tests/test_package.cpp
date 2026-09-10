#include <gtest/gtest.h>
#include "core/package.h"
#include "core/tar_writer.h"
#include "core/gzip_handler.h"
#include "core/manifest.h"
#include "crypto/signing.h"
#include "crypto/keyring.h"

#include <filesystem>
#include <fstream>
#include <vector>

#include "test_png.h"

using namespace lgx;
namespace fs = std::filesystem;

// Test fixture with temp directory management
class PackageTest : public ::testing::Test {
protected:
    fs::path tempDir;
    
    void SetUp() override {
        // Create a unique temp directory for each test
        tempDir = fs::temp_directory_path() / ("lgx_test_" + std::to_string(rand()));
        fs::create_directories(tempDir);
    }
    
    void TearDown() override {
        // Clean up temp directory
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }
    
    // Helper to create a test file
    void createTestFile(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path);
        file << content;
    }
    
    // Helper to create a test directory with files
    void createTestDirectory(const fs::path& dir,
                            const std::map<std::string, std::string>& files) {
        fs::create_directories(dir);
        for (const auto& [name, content] : files) {
            createTestFile(dir / name, content);
        }
    }

    // Helper: write a crafted .lgx whose tar contains a single attacker-chosen
    // entry path verbatim. The normal addVariant() API never produces unsafe
    // paths (".." segments, absolute paths, etc.), so to exercise the
    // extraction path-safety checks we have to build the tar by hand. The
    // package always contains manifest.json and the variants/<variant>/
    // directory so that load() and hasVariant("linux-x86_64") succeed.
    void writeCraftedPackage(const fs::path& pkgPath,
                             const std::string& maliciousEntryPath,
                             const std::string& payload) {
        Manifest manifest;
        manifest.name = "evil";
        manifest.version = "0.0.1";

        DeterministicTarWriter writer;
        writer.addFile("manifest.json", manifest.toJson());
        writer.addDirectory("variants");
        writer.addDirectory("variants/linux-x86_64");
        writer.addFile(maliciousEntryPath, payload);

        auto tarData = writer.finalize();
        auto gzipData = GzipHandler::compress(tarData);
        ASSERT_FALSE(gzipData.empty());

        std::ofstream out(pkgPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(gzipData.data()),
                  static_cast<std::streamsize>(gzipData.size()));
    }
};

// =============================================================================
// Package Creation Tests
// =============================================================================

TEST_F(PackageTest, Create_SkeletonPackage) {
    fs::path pkgPath = tempDir / "test.lgx";
    
    auto result = Package::create(pkgPath, "testpkg");
    
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(fs::exists(pkgPath));
}

TEST_F(PackageTest, Create_NormalizesName) {
    fs::path pkgPath = tempDir / "test.lgx";
    
    Package::create(pkgPath, "MyPackage");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->getManifest().name, "mypackage");
}

TEST_F(PackageTest, Create_ValidStructure) {
    fs::path pkgPath = tempDir / "test.lgx";
    
    Package::create(pkgPath, "testpkg");
    
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid) << "Errors: " << 
        (verifyResult.errors.empty() ? "none" : verifyResult.errors[0]);
}

// =============================================================================
// Package Loading Tests
// =============================================================================

TEST_F(PackageTest, Load_ValidPackage) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    auto pkg = Package::load(pkgPath);
    
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->getManifest().name, "testpkg");
}

TEST_F(PackageTest, Load_NonExistent) {
    fs::path pkgPath = tempDir / "nonexistent.lgx";
    
    auto pkg = Package::load(pkgPath);
    
    EXPECT_FALSE(pkg.has_value());
}

TEST_F(PackageTest, Load_InvalidFile) {
    fs::path pkgPath = tempDir / "invalid.lgx";
    std::ofstream file(pkgPath);
    file << "not a valid lgx file";
    file.close();
    
    auto pkg = Package::load(pkgPath);
    
    EXPECT_FALSE(pkg.has_value());
}

// =============================================================================
// Add Single File Variant Tests
// =============================================================================

TEST_F(PackageTest, AddVariant_SingleFile) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Create test file
    fs::path testFile = tempDir / "lib.so";
    createTestFile(testFile, "library content");
    
    // Load and add variant
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    auto result = pkg->addVariant("linux-amd64", testFile);
    EXPECT_TRUE(result.success);
    
    // Save and verify
    pkg->save(pkgPath);
    
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);
}

TEST_F(PackageTest, AddVariant_SingleFile_AutoMain) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path testFile = tempDir / "mylib.so";
    createTestFile(testFile, "content");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    pkg->addVariant("linux-amd64", testFile);
    
    // Main should be set to the filename
    auto mainPath = pkg->getManifest().getMain("linux-amd64");
    ASSERT_TRUE(mainPath.has_value());
    EXPECT_EQ(*mainPath, "mylib.so");
}

// =============================================================================
// Add Directory Variant Tests
// =============================================================================

TEST_F(PackageTest, AddVariant_Directory) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Create test directory
    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {
        {"index.js", "console.log('hello')"},
        {"lib.js", "export {}"}
    });
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    auto result = pkg->addVariant("web", testDir, "index.js");
    EXPECT_TRUE(result.success);
    
    pkg->save(pkgPath);
    
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);
}

TEST_F(PackageTest, AddVariant_Directory_RequiresMain) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"file.txt", "content"}});
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    // Adding directory without --main should fail
    auto result = pkg->addVariant("web", testDir);
    EXPECT_FALSE(result.success);
}

TEST_F(PackageTest, AddVariant_UiQmlDirectory_AllowsMissingMain) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    // 0.4.0 requires ui_qml packages to carry a conforming icon.
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {
        {"qml/Main.qml", "import QtQuick 2.15\nItem {}"},
        {"qml/Helper.qml", "import QtQuick 2.15\nItem {}"}
    });

    auto result = pkg->addVariant("darwin-arm64", testDir);
    EXPECT_TRUE(result.success);

    pkg->save(pkgPath);

    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    EXPECT_FALSE(pkg->getManifest().getMain("darwin-arm64").has_value());
}

TEST_F(PackageTest, AddVariant_UiQmlDirectory_ClearsStaleMain) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    // 0.4.0 requires ui_qml packages to carry a conforming icon.
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path firstDir = tempDir / "first";
    createTestDirectory(firstDir, {
        {"qml/Main.qml", "import QtQuick 2.15\nItem {}"},
        {"backend.dylib", "backend"}
    });
    auto firstResult = pkg->addVariant("darwin-arm64", firstDir, "backend.dylib");
    EXPECT_TRUE(firstResult.success);
    pkg->save(pkgPath);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    // 0.4.0 requires ui_qml packages to carry a conforming icon.
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path secondDir = tempDir / "second";
    createTestDirectory(secondDir, {
        {"qml/Main.qml", "import QtQuick 2.15\nItem { objectName: \"replacement\" }"}
    });
    auto secondResult = pkg->addVariant("darwin-arm64", secondDir);
    EXPECT_TRUE(secondResult.success);
    pkg->save(pkgPath);

    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    EXPECT_FALSE(pkg->getManifest().getMain("darwin-arm64").has_value());
}

// =============================================================================
// Variant Replacement Tests (No Merge)
// =============================================================================

TEST_F(PackageTest, AddVariant_ReplacesExisting) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Add initial variant
    fs::path file1 = tempDir / "old.so";
    createTestFile(file1, "old content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->save(pkgPath);
    
    // Add replacement variant
    fs::path file2 = tempDir / "new.so";
    createTestFile(file2, "new content");
    
    pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file2);
    pkg->save(pkgPath);
    
    // Verify - old content should be gone
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);
    
    // Check main points to new file
    pkg = Package::load(pkgPath);
    auto mainPath = pkg->getManifest().getMain("linux-amd64");
    ASSERT_TRUE(mainPath.has_value());
    EXPECT_EQ(*mainPath, "new.so");
}

TEST_F(PackageTest, AddVariant_NoMerge) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Add first file to variant
    fs::path file1 = tempDir / "file1.so";
    createTestFile(file1, "content1");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->save(pkgPath);
    
    // Add second file (should replace, not merge)
    fs::path file2 = tempDir / "file2.so";
    createTestFile(file2, "content2");
    
    pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file2);
    pkg->save(pkgPath);
    
    // Load and check - only file2.so should exist
    pkg = Package::load(pkgPath);
    EXPECT_TRUE(pkg->hasVariant("linux-amd64"));
    
    // The variant should only have file2.so, not file1.so
    auto mainPath = pkg->getManifest().getMain("linux-amd64");
    EXPECT_EQ(*mainPath, "file2.so");
}

// =============================================================================
// Remove Variant Tests
// =============================================================================

TEST_F(PackageTest, RemoveVariant) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Add two variants
    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.so";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("darwin-arm64", file2);
    pkg->save(pkgPath);
    
    // Remove one variant
    pkg = Package::load(pkgPath);
    auto result = pkg->removeVariant("linux-amd64");
    EXPECT_TRUE(result.success);
    pkg->save(pkgPath);
    
    // Verify
    pkg = Package::load(pkgPath);
    EXPECT_FALSE(pkg->hasVariant("linux-amd64"));
    EXPECT_TRUE(pkg->hasVariant("darwin-arm64"));
    
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);
}

TEST_F(PackageTest, RemoveVariant_NonExistent) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    auto pkg = Package::load(pkgPath);
    auto result = pkg->removeVariant("nonexistent");
    
    EXPECT_FALSE(result.success);
}

// =============================================================================
// HasVariant Tests
// =============================================================================

TEST_F(PackageTest, HasVariant) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    
    EXPECT_FALSE(pkg->hasVariant("linux-amd64"));
    
    pkg->addVariant("linux-amd64", file);
    
    EXPECT_TRUE(pkg->hasVariant("linux-amd64"));
    EXPECT_TRUE(pkg->hasVariant("Linux-AMD64"));  // Case-insensitive
}

// =============================================================================
// GetVariants Tests
// =============================================================================

TEST_F(PackageTest, GetVariants) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    pkg->addVariant("darwin-arm64", file);
    
    auto variants = pkg->getVariants();
    
    EXPECT_EQ(variants.size(), 2);
    EXPECT_TRUE(variants.count("linux-amd64") > 0);
    EXPECT_TRUE(variants.count("darwin-arm64") > 0);
}

// =============================================================================
// Verification Tests
// =============================================================================

TEST_F(PackageTest, Verify_ValidPackage) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    pkg->save(pkgPath);
    
    auto result = Package::verify(pkgPath);
    
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(PackageTest, Verify_InvalidPackage_NonExistent) {
    fs::path pkgPath = tempDir / "nonexistent.lgx";
    
    auto result = Package::verify(pkgPath);
    
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.errors.empty());
}

// =============================================================================
// WouldMainChange Tests
// =============================================================================

TEST_F(PackageTest, WouldMainChange) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    
    // Main is "lib.so", checking with same value
    EXPECT_FALSE(pkg->wouldMainChange("linux-amd64", "lib.so"));
    
    // Main is "lib.so", checking with different value
    EXPECT_TRUE(pkg->wouldMainChange("linux-amd64", "other.so"));
    
    // Non-existent variant: no prior main, so no "change" for prompting purposes
    EXPECT_FALSE(pkg->wouldMainChange("nonexistent", "anything"));
    EXPECT_FALSE(pkg->wouldMainChange("nonexistent", ""));
}

// =============================================================================
// Save/Load Roundtrip Tests
// =============================================================================

TEST_F(PackageTest, SaveLoad_Roundtrip) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Add some content
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "binary content here");
    
    auto pkg = Package::load(pkgPath);
    pkg->getManifest().description = "Test description";
    pkg->getManifest().version = "2.0.0";
    pkg->addVariant("linux-amd64", file);
    pkg->save(pkgPath);
    
    // Load again and verify
    auto pkg2 = Package::load(pkgPath);
    ASSERT_TRUE(pkg2.has_value());
    
    EXPECT_EQ(pkg2->getManifest().description, "Test description");
    EXPECT_EQ(pkg2->getManifest().version, "2.0.0");
    EXPECT_TRUE(pkg2->hasVariant("linux-amd64"));
}

// =============================================================================
// Multiple Operations Tests
// =============================================================================

TEST_F(PackageTest, MultipleOperations) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    // Create test files
    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.so";
    fs::path file3 = tempDir / "lib3.dylib";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");
    createTestFile(file3, "content3");
    
    // Add variants
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("linux-arm64", file2);
    pkg->addVariant("darwin-arm64", file3);
    pkg->save(pkgPath);
    
    // Verify
    auto result1 = Package::verify(pkgPath);
    EXPECT_TRUE(result1.valid);
    
    // Remove one
    pkg = Package::load(pkgPath);
    pkg->removeVariant("linux-arm64");
    pkg->save(pkgPath);
    
    // Verify again
    auto result2 = Package::verify(pkgPath);
    EXPECT_TRUE(result2.valid);
    
    // Check state
    pkg = Package::load(pkgPath);
    EXPECT_TRUE(pkg->hasVariant("linux-amd64"));
    EXPECT_FALSE(pkg->hasVariant("linux-arm64"));
    EXPECT_TRUE(pkg->hasVariant("darwin-arm64"));
    EXPECT_EQ(pkg->getVariants().size(), 2);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(PackageTest, AddVariant_EmptyFile) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path emptyFile = tempDir / "empty.txt";
    createTestFile(emptyFile, "");
    
    auto pkg = Package::load(pkgPath);
    auto result = pkg->addVariant("test", emptyFile);
    EXPECT_TRUE(result.success);
    
    pkg->save(pkgPath);
    
    auto verifyResult = Package::verify(pkgPath);
    EXPECT_TRUE(verifyResult.valid);
}

TEST_F(PackageTest, VariantName_CaseNormalization) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("Linux-AMD64", file);  // Mixed case
    pkg->save(pkgPath);
    
    pkg = Package::load(pkgPath);
    
    // Should be stored as lowercase
    EXPECT_TRUE(pkg->hasVariant("linux-amd64"));
    EXPECT_TRUE(pkg->hasVariant("LINUX-AMD64"));  // Case-insensitive lookup
    
    auto variants = pkg->getVariants();
    EXPECT_TRUE(variants.count("linux-amd64") > 0);
    EXPECT_TRUE(variants.count("Linux-AMD64") == 0);  // Not stored as mixed case
}

// =============================================================================
// Extract Variant Tests
// =============================================================================

TEST_F(PackageTest, ExtractVariant_SingleFile) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path testFile = tempDir / "lib.so";
    createTestFile(testFile, "library content");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("linux-amd64", testFile);
    pkg->save(pkgPath);
    
    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractVariant("linux-amd64", extractDir);
    
    EXPECT_TRUE(result.success) << result.error;
    
    fs::path extractedFile = extractDir / "linux-amd64" / "lib.so";
    EXPECT_TRUE(fs::exists(extractedFile)) << "Expected: " << extractedFile.string();
    
    std::ifstream file(extractedFile);
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "library content");
}

TEST_F(PackageTest, ExtractVariant_Directory) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {
        {"index.js", "console.log('hello')"},
        {"lib.js", "export {}"}
    });
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("web", testDir, "index.js");
    pkg->save(pkgPath);
    
    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractVariant("web", extractDir);
    
    EXPECT_TRUE(result.success) << result.error;
    
    EXPECT_TRUE(fs::exists(extractDir / "web" / "index.js"));
    EXPECT_TRUE(fs::exists(extractDir / "web" / "lib.js"));
}

TEST_F(PackageTest, ExtractVariant_NonExistent) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractVariant("nonexistent", extractDir);
    
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(PackageTest, ExtractAll_MultipleVariants) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.dylib";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("darwin-arm64", file2);
    pkg->save(pkgPath);
    
    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractAll(extractDir);
    
    EXPECT_TRUE(result.success) << result.error;
    
    EXPECT_TRUE(fs::exists(extractDir / "linux-amd64" / "lib1.so"));
    EXPECT_TRUE(fs::exists(extractDir / "darwin-arm64" / "lib2.dylib"));
    
    std::ifstream f1(extractDir / "linux-amd64" / "lib1.so");
    std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    EXPECT_EQ(c1, "content1");
    
    std::ifstream f2(extractDir / "darwin-arm64" / "lib2.dylib");
    std::string c2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    EXPECT_EQ(c2, "content2");
}

TEST_F(PackageTest, ExtractAll_EmptyPackage) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    
    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractAll(extractDir);
    
    EXPECT_TRUE(result.success);
}

// =============================================================================
// Extraction Path-Safety Tests (zip-slip / path traversal)
// =============================================================================

// A crafted package whose tar entry escapes the variant root via ".." segments
// must NOT write outside the output directory. Reachable from `lgx extract`,
// `lgpm --allow-unsigned install`, and the lgx_extract C API, none of which run
// validatePackage() before extracting.
TEST_F(PackageTest, ExtractVariant_RejectsPathTraversal) {
    fs::path pkgPath = tempDir / "evil.lgx";

    // Resolves to <tempDir>/pwned.txt — outside the intended output directory.
    writeCraftedPackage(pkgPath,
                        "variants/linux-x86_64/../../pwned.txt",
                        "owned");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value()) << Package::getLastError();
    ASSERT_TRUE(pkg->hasVariant("linux-x86_64"));

    fs::path extractDir = tempDir / "extracted";
    fs::path escapeTarget = tempDir / "pwned.txt";

    auto result = pkg->extractVariant("linux-x86_64", extractDir);

    // The traversal entry must be rejected...
    EXPECT_FALSE(result.success) << "extractVariant accepted a traversal path";
    // ...and crucially, nothing may be written outside the output directory.
    EXPECT_FALSE(fs::exists(escapeTarget))
        << "zip-slip: file written outside output dir at " << escapeTarget.string();
}

// extractAll() delegates to extractVariant(), so it must inherit the same guard.
TEST_F(PackageTest, ExtractAll_RejectsPathTraversal) {
    fs::path pkgPath = tempDir / "evil.lgx";

    writeCraftedPackage(pkgPath,
                        "variants/linux-x86_64/../../pwned.txt",
                        "owned");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value()) << Package::getLastError();

    fs::path extractDir = tempDir / "extracted";
    fs::path escapeTarget = tempDir / "pwned.txt";

    auto result = pkg->extractAll(extractDir);

    EXPECT_FALSE(result.success) << "extractAll accepted a traversal path";
    EXPECT_FALSE(fs::exists(escapeTarget))
        << "zip-slip: file written outside output dir at " << escapeTarget.string();
}

// An absolute entry path must also be rejected (it would otherwise ignore the
// output directory entirely).
TEST_F(PackageTest, ExtractVariant_RejectsAbsolutePath) {
    fs::path pkgPath = tempDir / "evil.lgx";
    fs::path escapeTarget = tempDir / "abs_pwned.txt";

    // Build an entry whose path, after the "variants/<v>/" prefix, is absolute.
    writeCraftedPackage(pkgPath,
                        "variants/linux-x86_64/" + escapeTarget.string(),
                        "owned");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value()) << Package::getLastError();

    fs::path extractDir = tempDir / "extracted";

    auto result = pkg->extractVariant("linux-x86_64", extractDir);

    EXPECT_FALSE(result.success) << "extractVariant accepted an absolute path";
    EXPECT_FALSE(fs::exists(escapeTarget))
        << "absolute-path escape wrote to " << escapeTarget.string();
}

// Negative control: a legitimate package with a nested subdirectory must still
// extract correctly — the hardening must not over-reject benign paths.
TEST_F(PackageTest, ExtractVariant_AllowsBenignNestedPaths) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {
        {"index.js", "console.log('hello')"},
        {"sub/dir/lib.js", "export {}"}
    });

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("web", testDir, "index.js");
    pkg->save(pkgPath);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());

    fs::path extractDir = tempDir / "extracted";
    auto result = pkg->extractVariant("web", extractDir);

    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(fs::exists(extractDir / "web" / "index.js"));
    EXPECT_TRUE(fs::exists(extractDir / "web" / "sub" / "dir" / "lib.js"));
}

// Restores the process working directory on scope exit, so a test that chdir's
// to exercise relative-output-dir behavior can't leak its CWD into other tests.
namespace {
struct CwdGuard {
    fs::path previous;
    explicit CwdGuard(const fs::path& to) : previous(fs::current_path()) {
        fs::current_path(to);
    }
    ~CwdGuard() {
        std::error_code ec;
        fs::current_path(previous, ec);
    }
};
} // namespace

// Regression for F-003: the containment check must compare two paths that share
// the same base. When outputDir is RELATIVE (the CLI defaults to "."), canonRoot
// resolves to an absolute path via weakly_canonical while the target stayed
// lexical/relative — lexically_relative() then returned empty and every benign
// entry was false-rejected. A legitimate package extracted into "." must succeed.
TEST_F(PackageTest, ExtractVariant_AllowsBenignPaths_RelativeOutputDir) {
    fs::path pkgPath = tempDir / "test.lgx";  // absolute — unaffected by chdir
    Package::create(pkgPath, "testpkg");

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {
        {"index.js", "console.log('hello')"},
        {"sub/dir/lib.js", "export {}"}
    });

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("web", testDir, "index.js");
    pkg->save(pkgPath);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());

    // Extract into "." from a fresh working directory.
    fs::path workDir = tempDir / "workdir";
    fs::create_directories(workDir);
    CwdGuard guard(workDir);

    auto result = pkg->extractVariant("web", ".");

    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(fs::exists(workDir / "web" / "index.js"));
    EXPECT_TRUE(fs::exists(workDir / "web" / "sub" / "dir" / "lib.js"));
}

// Regression for F-003: a traversal entry must still be rejected when the output
// directory is relative — the fix for the false-rejection above must not weaken
// the zip-slip guard on the relative path.
TEST_F(PackageTest, ExtractVariant_RejectsPathTraversal_RelativeOutputDir) {
    fs::path pkgPath = tempDir / "evil.lgx";

    // After the "variants/linux-x86_64/" prefix this is "../../pwned.txt", which
    // from <workdir>/. resolves to <tempDir>/pwned.txt — outside the output dir.
    writeCraftedPackage(pkgPath,
                        "variants/linux-x86_64/../../pwned.txt",
                        "owned");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value()) << Package::getLastError();
    ASSERT_TRUE(pkg->hasVariant("linux-x86_64"));

    fs::path workDir = tempDir / "workdir";
    fs::create_directories(workDir);
    fs::path escapeTarget = tempDir / "pwned.txt";
    CwdGuard guard(workDir);

    auto result = pkg->extractVariant("linux-x86_64", ".");

    EXPECT_FALSE(result.success) << "extractVariant accepted a traversal path";
    EXPECT_FALSE(fs::exists(escapeTarget))
        << "zip-slip: file written outside output dir at " << escapeTarget.string();
}

TEST_F(PackageTest, ExtractVariant_CaseInsensitive) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");
    
    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");
    
    auto pkg = Package::load(pkgPath);
    pkg->addVariant("Linux-AMD64", file);
    pkg->save(pkgPath);
    
    pkg = Package::load(pkgPath);
    
    fs::path extractDir = tempDir / "extracted";
    
    auto result = pkg->extractVariant("LINUX-AMD64", extractDir);
    EXPECT_TRUE(result.success) << result.error;
    
    EXPECT_TRUE(fs::exists(extractDir / "linux-amd64" / "lib.so"));
}

// =============================================================================
// Mandatory Hashes Tests
// =============================================================================

TEST_F(PackageTest, AddVariant_ComputesHashes) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "library content");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("linux-amd64", file);

    // Hashes should be present after addVariant
    EXPECT_FALSE(pkg->getManifest().hashes.empty());
    EXPECT_TRUE(pkg->getManifest().hashes.count("root") > 0);
    EXPECT_TRUE(pkg->getManifest().hashes.count("variants") > 0);
    EXPECT_TRUE(pkg->getManifest().hashes.count("variants/linux-amd64") > 0);
}

TEST_F(PackageTest, AddVariant_HashesUpdateOnReplace) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file1 = tempDir / "old.so";
    createTestFile(file1, "old content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    auto hashesAfterFirst = pkg->getManifest().hashes;

    // Replace with different content
    fs::path file2 = tempDir / "new.so";
    createTestFile(file2, "new different content");
    pkg->addVariant("linux-amd64", file2);
    auto hashesAfterSecond = pkg->getManifest().hashes;

    // Hashes should change when content changes
    EXPECT_NE(hashesAfterFirst["root"], hashesAfterSecond["root"]);
}

TEST_F(PackageTest, RemoveVariant_UpdatesHashes) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.so";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("darwin-arm64", file2);
    auto hashesWithTwo = pkg->getManifest().hashes;

    pkg->removeVariant("linux-amd64");
    auto hashesWithOne = pkg->getManifest().hashes;

    // Root hash should change
    EXPECT_NE(hashesWithTwo["root"], hashesWithOne["root"]);
    // Removed variant hash should be gone
    EXPECT_EQ(hashesWithOne.count("variants/linux-amd64"), 0u);
    // Remaining variant hash should still exist
    EXPECT_TRUE(hashesWithOne.count("variants/darwin-arm64") > 0);
}

TEST_F(PackageTest, Hashes_PersistThroughSaveLoad) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    auto originalHashes = pkg->getManifest().hashes;
    pkg->save(pkgPath);

    // Reload and check hashes are preserved
    auto pkg2 = Package::load(pkgPath);
    ASSERT_TRUE(pkg2.has_value());
    EXPECT_EQ(pkg2->getManifest().hashes, originalHashes);
}

TEST_F(PackageTest, Verify_UnsignedPackage_ChecksHashes) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    pkg->save(pkgPath);

    // Verify should pass for unsigned package with valid hashes
    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << "Errors: " <<
        (result.errors.empty() ? "none" : result.errors[0]);
}

TEST_F(PackageTest, ClearSignature_PreservesHashes) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    auto hashesBeforeClear = pkg->getManifest().hashes;

    pkg->clearSignature();

    EXPECT_EQ(pkg->getManifest().hashes, hashesBeforeClear);
    EXPECT_FALSE(pkg->getManifest().hashes.empty());
}

TEST_F(PackageTest, RecomputeHashes_EmptyPackage) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());

    pkg->recomputeHashes();
    // Empty skeleton has no hashable content (just variants/ dir)
    EXPECT_TRUE(pkg->getManifest().hashes.empty());
}

TEST_F(PackageTest, RecomputeHashes_Idempotent) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);
    auto hashes1 = pkg->getManifest().hashes;

    pkg->recomputeHashes();
    auto hashes2 = pkg->getManifest().hashes;

    EXPECT_EQ(hashes1, hashes2);
}

TEST_F(PackageTest, Hashes_MultipleVariants) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.dylib";
    createTestFile(file1, "linux content");
    createTestFile(file2, "darwin content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("darwin-arm64", file2);

    auto& hashes = pkg->getManifest().hashes;
    EXPECT_TRUE(hashes.count("root") > 0);
    EXPECT_TRUE(hashes.count("variants") > 0);
    EXPECT_TRUE(hashes.count("variants/linux-amd64") > 0);
    EXPECT_TRUE(hashes.count("variants/darwin-arm64") > 0);

    // Each variant should have a different hash (different content)
    EXPECT_NE(hashes["variants/linux-amd64"], hashes["variants/darwin-arm64"]);
}

// =============================================================================
// Package Signing Tests
// =============================================================================

TEST_F(PackageTest, SignPackage_Basic) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "library content");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->addVariant("linux-amd64", file);

    auto kp = crypto::generateKeypair();
    auto result = pkg->signPackage(kp.secretKey);
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_TRUE(pkg->isSigned());
}

TEST_F(PackageTest, SignPackage_WithMetadata) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);

    auto kp = crypto::generateKeypair();
    auto result = pkg->signPackage(kp.secretKey, "Test Publisher", "https://example.com");
    EXPECT_TRUE(result.success);

    // Verify signature info contains metadata
    auto sigInfo = pkg->verifySignature();
    EXPECT_TRUE(sigInfo.is_signed);
    EXPECT_TRUE(sigInfo.signature_valid);
    EXPECT_TRUE(sigInfo.package_valid);
    EXPECT_EQ(sigInfo.signer_name, "Test Publisher");
    EXPECT_EQ(sigInfo.signer_url, "https://example.com");
    EXPECT_EQ(sigInfo.signer_did, crypto::publicKeyToDid(kp.publicKey));
}

TEST_F(PackageTest, SignVerify_RoundTrip) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);

    auto kp = crypto::generateKeypair();
    pkg->signPackage(kp.secretKey);
    pkg->save(pkgPath);

    // Load and verify
    auto pkg2 = Package::load(pkgPath);
    ASSERT_TRUE(pkg2.has_value());
    EXPECT_TRUE(pkg2->isSigned());

    auto sigInfo = pkg2->verifySignature();
    EXPECT_TRUE(sigInfo.is_signed);
    EXPECT_TRUE(sigInfo.signature_valid);
    EXPECT_TRUE(sigInfo.package_valid);
    EXPECT_TRUE(sigInfo.error.empty()) << sigInfo.error;
}

TEST_F(PackageTest, VerifySignature_Unsigned) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());

    auto sigInfo = pkg->verifySignature();
    EXPECT_FALSE(sigInfo.is_signed);
    EXPECT_FALSE(sigInfo.signature_valid);
    EXPECT_TRUE(sigInfo.package_valid);  // Package structure is valid even if unsigned
}

TEST_F(PackageTest, VerifySignature_Did) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);

    auto kp = crypto::generateKeypair();
    std::string expectedDid = crypto::publicKeyToDid(kp.publicKey);

    pkg->signPackage(kp.secretKey);

    auto sigInfo = pkg->verifySignature();
    EXPECT_EQ(sigInfo.signer_did, expectedDid);
    EXPECT_EQ(sigInfo.signer_did.substr(0, 8), "did:jwk:");
}

TEST_F(PackageTest, AddVariant_ClearsSignature) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.so";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);

    auto kp = crypto::generateKeypair();
    pkg->signPackage(kp.secretKey);
    EXPECT_TRUE(pkg->isSigned());

    // Adding another variant should clear the signature
    pkg->addVariant("darwin-arm64", file2);
    EXPECT_FALSE(pkg->isSigned());

    // But hashes should still be present and updated
    EXPECT_FALSE(pkg->getManifest().hashes.empty());
    EXPECT_TRUE(pkg->getManifest().hashes.count("variants/darwin-arm64") > 0);
}

TEST_F(PackageTest, RemoveVariant_ClearsSignature) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file1 = tempDir / "lib1.so";
    fs::path file2 = tempDir / "lib2.so";
    createTestFile(file1, "content1");
    createTestFile(file2, "content2");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file1);
    pkg->addVariant("darwin-arm64", file2);

    auto kp = crypto::generateKeypair();
    pkg->signPackage(kp.secretKey);
    EXPECT_TRUE(pkg->isSigned());

    // Removing a variant should clear the signature
    pkg->removeVariant("darwin-arm64");
    EXPECT_FALSE(pkg->isSigned());
}

TEST_F(PackageTest, SignPackage_EmptyPackage_Succeeds) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());

    // Empty skeleton is structurally valid, so signing succeeds
    auto kp = crypto::generateKeypair();
    auto result = pkg->signPackage(kp.secretKey);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(pkg->isSigned());
}

TEST_F(PackageTest, Verify_SignedPackage_ValidHashes) {
    ASSERT_TRUE(crypto::init());

    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    pkg->addVariant("linux-amd64", file);

    auto kp = crypto::generateKeypair();
    pkg->signPackage(kp.secretKey, "Publisher", "https://example.com");
    pkg->save(pkgPath);

    // verify() (the static method) should pass
    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << "Errors: " <<
        (result.errors.empty() ? "none" : result.errors[0]);
}

// =============================================================================
// Icon Contract (manifest 0.4.0+) — plan.md §3.4, §3.7
// =============================================================================

TEST_F(PackageTest, Icon_ConformingIconValidates) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << "Errors: " <<
        (result.errors.empty() ? "none" : result.errors[0]);
}

// The icon lands at the canonical root path, NOT inside a variant — that is
// what makes it readable without unpacking a platform build.
TEST_F(PackageTest, Icon_StoredAtCanonicalRootPath) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->getManifest().icon, std::string("assets/icon.png"));

    bool found = false;
    for (const auto& e : pkg->getEntries())
        if (e.path == "assets/icon.png" && !e.isDirectory) found = true;
    EXPECT_TRUE(found) << "icon must live at assets/icon.png";
}

TEST_F(PackageTest, Icon_WrongDimensionsRejected) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng(512, 512)).success);

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.errors.empty());
    // The message must name the standard and both sizes, not just assert.
    EXPECT_NE(result.errors[0].find("256x256"), std::string::npos);
    EXPECT_NE(result.errors[0].find("512x512"), std::string::npos);
}

TEST_F(PackageTest, Icon_NonPngRejected) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    std::vector<uint8_t> notPng(64, 0x41);  // "AAAA..."
    ASSERT_TRUE(pkg->setIcon(notPng).success);

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find("not a PNG"), std::string::npos);
}

TEST_F(PackageTest, Icon_MissingOnUiQmlRejected) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_FALSE(result.valid);
}

// Core modules appear in package lists but render no tile, so an icon stays
// optional for them. Requiring one would turn a 12-module migration into a
// 33-module one for no visual benefit.
TEST_F(PackageTest, Icon_OptionalForCoreType) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "core";

    fs::path testFile = tempDir / "mod.so";
    createTestFile(testFile, "binary");
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testFile).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << "Errors: " <<
        (result.errors.empty() ? "none" : result.errors[0]);
}

// THE backward-compatibility regression. A 0.3.0 ui_qml package with icon:""
// was legal and is published in the wild. Enforcing the 0.4.0 contract on it
// would make the entire existing catalog fail verification and become
// uninstallable. See plan.md §3.7.
TEST_F(PackageTest, Icon_LegacyManifestExemptFromContract) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().manifestVersion = "0.3.0";
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    pkg->getManifest().icon = "";

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << "0.3.0 package must remain valid. Errors: " <<
        (result.errors.empty() ? "none" : result.errors[0]);
}

// Mutating the icon must change the Merkle root — assets/ is inside the tree,
// which is what makes the icon signature-covered for free.
TEST_F(PackageTest, Icon_ParticipatesInMerkleTree) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);
    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    const std::string rootA = pkg->getManifest().hashes.count("root")
                              ? pkg->getManifest().hashes.at("root") : "";
    ASSERT_FALSE(rootA.empty());

    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng(256, 256)).success);
    // Same dimensions but different pixel payload would be identical here, so
    // use a differently-sized image to guarantee different bytes.
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng(128, 128)).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);
    pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    const std::string rootB = pkg->getManifest().hashes.count("root")
                              ? pkg->getManifest().hashes.at("root") : "";

    EXPECT_NE(rootA, rootB) << "assets/ must be covered by the Merkle tree";
}

// The gap that let the icon regression ship: every existing test checked the
// archive's CONTENTS, none checked what lands on disk after extraction. The
// manifest documents `icon` as relative to the installed package root, so the
// contract is "extract, then resolve manifest.icon" — assert exactly that.
TEST_F(PackageTest, Icon_ExtractedToInstalledPackageRoot) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    fs::path out = tempDir / "installed";
    auto loaded = Package::load(pkgPath);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->extractVariant("linux-amd64", out).success);

    // This is the exact join UIPluginManager::pluginIconUrl() performs.
    const fs::path installDir = out / "linux-amd64";
    const fs::path resolved   = installDir / loaded->getManifest().icon;
    EXPECT_TRUE(fs::exists(resolved))
        << "manifest icon '" << loaded->getManifest().icon
        << "' must resolve under the installed package root";

    // Variant payload still lands where it always did.
    EXPECT_TRUE(fs::exists(installDir / "qml" / "Main.qml"));

    // And the extracted icon is byte-identical to what was packaged.
    std::ifstream f(resolved, std::ios::binary);
    std::vector<uint8_t> onDisk((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    EXPECT_EQ(onDisk, lgx_test::makePng());
}

// Assets are variant-independent: extracting a different variant must still
// produce the icon, or a darwin install would lose what a linux install kept.
TEST_F(PackageTest, Icon_ExtractedForEveryVariant) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);

    fs::path a = tempDir / "a"; createTestFile(a / "mod.so", "x");
    fs::path b = tempDir / "b"; createTestFile(b / "mod.dylib", "y");
    ASSERT_TRUE(pkg->addVariant("linux-amd64", a, "mod.so").success);
    ASSERT_TRUE(pkg->addVariant("darwin-arm64", b, "mod.dylib").success);
    ASSERT_TRUE(pkg->save(pkgPath).success);

    auto loaded = Package::load(pkgPath);
    ASSERT_TRUE(loaded.has_value());
    for (const std::string v : {"linux-amd64", "darwin-arm64"}) {
        fs::path out = tempDir / ("out-" + v);
        ASSERT_TRUE(loaded->extractVariant(v, out).success) << v;
        EXPECT_TRUE(fs::exists(out / v / "assets" / "icon.png"))
            << "variant " << v << " lost the root asset";
    }
}

// Review fix: a 0.4.0 package could previously pass with `icon` pointing
// anywhere, contradicting the canonical-path contract that hosts and the
// release tool both rely on.
TEST_F(PackageTest, Icon_NonCanonicalPathRejected) {
    fs::path pkgPath = tempDir / "test.lgx";
    Package::create(pkgPath, "testpkg");

    auto pkg = Package::load(pkgPath);
    ASSERT_TRUE(pkg.has_value());
    pkg->getManifest().type = "ui_qml";
    pkg->getManifest().view = "qml/Main.qml";
    ASSERT_TRUE(pkg->setIcon(lgx_test::makePng()).success);
    // Point the manifest somewhere else under assets/ — file still exists.
    pkg->getManifest().icon = "assets/elsewhere.png";

    fs::path testDir = tempDir / "dist";
    createTestDirectory(testDir, {{"qml/Main.qml", "import QtQuick\nItem {}"}});
    ASSERT_TRUE(pkg->addVariant("linux-amd64", testDir).success);
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_FALSE(result.valid);
    ASSERT_FALSE(result.errors.empty());
    EXPECT_NE(result.errors[0].find("assets/icon.png"), std::string::npos);
}

// =============================================================================
// Variant vocabulary
//
// A variant name is a key inside the signed hash tree, so a misspelled one is
// not a cosmetic problem: it produces a package that resolves on no host at
// all, and the failure surfaces at install time on a user's device rather than
// at build time. `lgx add` and `lgx verify` are where it is cheap to catch.
// =============================================================================

TEST_F(PackageTest, Verify_AcceptsEveryTargetOfTheStoreShell) {
    fs::path pkgPath = tempDir / "shell.lgx";
    Package::create(pkgPath, "shellpkg");

    fs::path nativeFile = tempDir / "lib.so";
    createTestFile(nativeFile, "native content");
    fs::path webDir = tempDir / "dist";
    createTestDirectory(webDir, {{"index.js", "console.log(1)"}});

    auto pkg = Package::load(pkgPath);
    for (const char* v : { "android-arm64", "android-x86_64", "ios-arm64", "ios-sim-arm64" }) {
        auto added = pkg->addVariant(v, nativeFile);
        EXPECT_TRUE(added.success) << v << ": " << added.error;
    }
    auto web = pkg->addVariant("web", webDir, std::string("index.js"));
    EXPECT_TRUE(web.success) << web.error;
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(PackageTest, Add_RejectsAMisspelledVariantAndNamesTheOneMeant) {
    fs::path pkgPath = tempDir / "typo.lgx";
    Package::create(pkgPath, "typopkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    auto result = pkg->addVariant("ios_arm64", file);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("ios_arm64"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("ios-arm64"), std::string::npos) << result.error;
}

TEST_F(PackageTest, Add_StillAcceptsANameTheVocabularyHasNoOpinionOn) {
    // The vocabulary corrects near misses; it does not own the namespace. A
    // name that resembles nothing in it belongs to someone else.
    fs::path pkgPath = tempDir / "private.lgx";
    Package::create(pkgPath, "privatepkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    auto result = pkg->addVariant("my-own-target", file);
    EXPECT_TRUE(result.success) << result.error;
}

TEST_F(PackageTest, Verify_RejectsAMisspelledVariantAndNamesTheOneMeant) {
    // Built by hand: addVariant refuses the name, which is the point, so the
    // only way a package carries one is a producer that wrote the tar itself.
    fs::path pkgPath = tempDir / "typo.lgx";

    Manifest manifest;
    manifest.name = "typopkg";
    manifest.version = "1.0.0";
    manifest.setMain("ios_arm64", "lib.dylib");

    DeterministicTarWriter writer;
    writer.addFile("manifest.json", manifest.toJson());
    writer.addDirectory("variants");
    writer.addDirectory("variants/ios_arm64");
    writer.addFile("variants/ios_arm64/lib.dylib", "content");
    auto tarData = writer.finalize();
    auto gzipData = GzipHandler::compress(tarData);
    ASSERT_FALSE(gzipData.empty());
    { std::ofstream out(pkgPath, std::ios::binary);
      out.write(reinterpret_cast<const char*>(gzipData.data()),
                static_cast<std::streamsize>(gzipData.size())); }

    auto result = Package::verify(pkgPath);
    EXPECT_FALSE(result.valid);
    bool named = false;
    for (const auto& e : result.errors) {
        if (e.find("ios_arm64") != std::string::npos
            && e.find("ios-arm64") != std::string::npos) named = true;
    }
    EXPECT_TRUE(named) << (result.errors.empty() ? "no errors at all" : result.errors[0]);
}

TEST_F(PackageTest, Verify_AcceptsTheDevFlavourOfEveryTarget) {
    fs::path pkgPath = tempDir / "dev.lgx";
    Package::create(pkgPath, "devpkg");

    fs::path file = tempDir / "lib.so";
    createTestFile(file, "content");

    auto pkg = Package::load(pkgPath);
    for (const char* v : { "darwin-arm64-dev", "linux-amd64-dev", "ios-arm64-dev", "web-dev" }) {
        auto added = pkg->addVariant(v, file);
        EXPECT_TRUE(added.success) << v << ": " << added.error;
    }
    pkg->save(pkgPath);

    auto result = Package::verify(pkgPath);
    EXPECT_TRUE(result.valid) << (result.errors.empty() ? "" : result.errors[0]);
}
