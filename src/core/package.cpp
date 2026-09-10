#include "package.h"
#include "gzip_handler.h"
#include "path_normalizer.h"
#include "platform_variant.h"

#include <fstream>
#include <algorithm>
#include <unordered_set>

namespace lgx {

thread_local std::string Package::lastError_;

namespace {

// PNG header layout is fixed-offset, so dimensions come out of the first 26
// bytes with no image library: 8-byte signature, 4-byte IHDR length, 4-byte
// "IHDR" tag, then width and height as big-endian uint32.
//
// This validates the *declared* dimensions only. It is deliberately not a
// defence against a malicious payload — a PNG can claim 256x256 and carry an
// enormous IDAT. Byte-size ceilings and decoder allocation limits belong at
// the fetch/decode boundary (plan.md §3.2.2), not here.
struct PngHeader {
    bool valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
};

PngHeader readPngHeader(const std::vector<uint8_t>& data) {
    static const uint8_t kSignature[8] =
        {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    PngHeader h;
    if (data.size() < 26) return h;
    if (!std::equal(std::begin(kSignature), std::end(kSignature), data.begin()))
        return h;
    if (data[12] != 'I' || data[13] != 'H' ||
        data[14] != 'D' || data[15] != 'R') return h;

    auto be32 = [&data](size_t off) {
        return (static_cast<uint32_t>(data[off])     << 24)
             | (static_cast<uint32_t>(data[off + 1]) << 16)
             | (static_cast<uint32_t>(data[off + 2]) << 8)
             |  static_cast<uint32_t>(data[off + 3]);
    };

    h.width  = be32(16);
    h.height = be32(20);
    h.valid  = true;
    return h;
}

// A variant name that is a MISSPELLING of one this library knows resolves on
// no host at all, so the package is dead on arrival everywhere. Caught at
// `lgx add` / `lgx verify` time rather than at install time on a user's
// device. A name that resembles nothing known is left alone: private targets
// exist and this vocabulary does not own the whole namespace.
//
// Returns the error to report, or empty when the name is not a near miss.
std::string misspelledVariantError(const std::string& variant) {
    const std::string meant = suggestVariantName(variant);
    if (meant.empty()) return {};
    return "Unknown variant '" + variant + "': did you mean '" + meant + "'?";
}

} // namespace

void Package::validateIconAsset(VerifyResult& result) const {
    const std::vector<uint8_t>* iconData = nullptr;
    for (const auto& entry : entries_) {
        if (entry.path == manifest_.icon && !entry.isDirectory) {
            iconData = &entry.data;
            break;
        }
    }
    validateIconContract(manifest_, iconData, result);
}

void Package::validateIconContract(const Manifest& manifest,
                                   const std::vector<uint8_t>* iconData,
                                   VerifyResult& result) {
    // Version gate. 0.2.x/0.3.x packages predate the assets/ slot and were
    // free to carry `icon: ""`; enforcing the contract on them would render
    // the entire already-published catalog uninstallable. See plan.md §3.7.
    if (!Manifest::requiresIconContract(manifest.manifestVersion)) return;

    // Icons are required for UI packages, which are the ones that render a
    // tile. Core modules appear in package lists but have no launcher or
    // sidebar presence, so an icon stays optional for them.
    const bool iconRequired = (manifest.type == "ui_qml");

    if (manifest.icon.empty()) {
        if (iconRequired) {
            result.valid = false;
            result.errors.push_back(
                std::string("Missing 'icon': ") + Manifest::ICON_PATH +
                " is required for type '" + manifest.type + "' packages");
        }
        return;
    }

    // The canonical location is part of the contract, not a convention: a
    // host resolves <installDir>/assets/icon.png without consulting the
    // manifest, and the release tool globs for it. Allowing `icon` to point
    // anywhere would make both unpredictable.
    if (manifest.icon != Manifest::ICON_PATH) {
        result.valid = false;
        result.errors.push_back(
            std::string("Manifest 'icon' must be '") + Manifest::ICON_PATH +
            "' at manifestVersion 0.4.0+, got '" + manifest.icon + "'");
        return;
    }

    if (!iconData) {
        result.valid = false;
        result.errors.push_back(
            "Manifest 'icon' points at '" + manifest.icon +
            "' which is not present in the package");
        return;
    }

    const PngHeader header = readPngHeader(*iconData);
    if (!header.valid) {
        result.valid = false;
        result.errors.push_back(
            "Icon '" + manifest.icon + "' does not match the Logos icon "
            "standard: expected PNG, exactly 256x256; actual: not a PNG");
        return;
    }

    const uint32_t want = static_cast<uint32_t>(Manifest::ICON_SIZE_PX);
    if (header.width != want || header.height != want) {
        result.valid = false;
        result.errors.push_back(
            "Icon '" + manifest.icon + "' does not match the Logos icon "
            "standard: expected PNG, exactly 256x256; actual: PNG, " +
            std::to_string(header.width) + "x" + std::to_string(header.height));
    }
}

const std::set<std::string> Package::ALLOWED_ROOT_ENTRIES = {
    "manifest.json",
    "manifest.sig",
    "variants",
    "assets",
    "docs",
    "licenses"
};

Package::Result Package::create(
    const std::filesystem::path& outputPath,
    const std::string& name
) {
    Package pkg;
    
    // Set up manifest with default values
    pkg.manifest_.name = PathNormalizer::toLowercase(name);
    pkg.manifest_.version = "0.0.1";
    pkg.manifest_.description = "";
    pkg.manifest_.author = "";
    pkg.manifest_.type = "";
    pkg.manifest_.category = "";
    pkg.manifest_.icon = "";
    pkg.manifest_.dependencies = {};
    
    // Create skeleton with manifest and variants directory
    pkg.entries_.clear();
    
    // Add variants directory
    TarEntry variantsDir;
    variantsDir.path = "variants";
    variantsDir.isDirectory = true;
    pkg.entries_.push_back(variantsDir);
    
    return pkg.save(outputPath);
}

std::optional<Package> Package::load(const std::filesystem::path& lgxPath) {
    // Read file
    std::ifstream file(lgxPath, std::ios::binary);
    if (!file) {
        lastError_ = "Cannot open file: " + lgxPath.string();
        return std::nullopt;
    }
    
    std::vector<uint8_t> gzipData(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );
    file.close();
    
    // Decompress
    auto tarData = GzipHandler::decompress(gzipData);
    if (tarData.empty() && !gzipData.empty()) {
        lastError_ = "Failed to decompress: " + GzipHandler::getLastError();
        return std::nullopt;
    }
    
    // Read tar
    auto readResult = TarReader::read(tarData);
    if (!readResult.success) {
        lastError_ = "Failed to read tar: " + readResult.error;
        return std::nullopt;
    }
    
    Package pkg;
    pkg.entries_ = std::move(readResult.entries);
    
    // Find and parse manifest and signature
    for (const auto& entry : pkg.entries_) {
        if (entry.path == "manifest.json" && !entry.isDirectory) {
            std::string jsonStr(entry.data.begin(), entry.data.end());
            auto manifestOpt = Manifest::fromJson(jsonStr);
            if (!manifestOpt) {
                lastError_ = "Failed to parse manifest: " + Manifest::getLastError();
                return std::nullopt;
            }
            pkg.manifest_ = std::move(*manifestOpt);
        }
        if (entry.path == "manifest.sig" && !entry.isDirectory) {
            std::string sigStr(entry.data.begin(), entry.data.end());
            auto sigOpt = crypto::ManifestSig::fromJson(sigStr);
            if (sigOpt) {
                pkg.manifestSig_ = std::move(*sigOpt);
            } else {
                // manifest.sig is present but could not be parsed
                pkg.manifestSigParseError_ = true;
            }
        }
    }

    return pkg;
}

Package::Result Package::save(const std::filesystem::path& lgxPath) const {
    DeterministicTarWriter writer;
    
    // Add manifest first
    std::string manifestJson = manifest_.toJson();
    writer.addFile("manifest.json", manifestJson);

    // Add manifest.sig if present
    if (manifestSig_.has_value()) {
        std::string sigJson = manifestSig_->toJson();
        writer.addFile("manifest.sig", sigJson);
    }

    // Track which directories we've added
    std::set<std::string> addedDirs;

    // Add all other entries
    for (const auto& entry : entries_) {
        if (entry.path == "manifest.json" || entry.path == "manifest.sig") {
            continue;  // Already added above
        }
        
        // Ensure parent directories exist
        auto requiredDirs = getRequiredDirectories(entry.path);
        for (const auto& dir : requiredDirs) {
            if (addedDirs.find(dir) == addedDirs.end()) {
                writer.addDirectory(dir);
                addedDirs.insert(dir);
            }
        }
        
        if (entry.isDirectory) {
            // Normalize directory path
            std::string dirPath = entry.path;
            while (!dirPath.empty() && dirPath.back() == '/') {
                dirPath.pop_back();
            }
            if (addedDirs.find(dirPath) == addedDirs.end()) {
                writer.addDirectory(dirPath);
                addedDirs.insert(dirPath);
            }
        } else {
            writer.addEntry(entry);
        }
    }
    
    // Ensure variants directory exists even if empty
    if (addedDirs.find("variants") == addedDirs.end()) {
        writer.addDirectory("variants");
    }
    
    // Finalize tar
    auto tarData = writer.finalize();
    
    // Compress
    auto gzipData = GzipHandler::compress(tarData);
    if (gzipData.empty() && !tarData.empty()) {
        return Result::fail("Failed to compress: " + GzipHandler::getLastError());
    }
    
    // Write file
    std::ofstream file(lgxPath, std::ios::binary);
    if (!file) {
        return Result::fail("Cannot write file: " + lgxPath.string());
    }
    
    file.write(reinterpret_cast<const char*>(gzipData.data()), gzipData.size());
    if (!file) {
        return Result::fail("Failed to write file: " + lgxPath.string());
    }
    
    return Result::ok();
}

Package::VerifyResult Package::validatePackage() const {
    VerifyResult result = VerifyResult::ok();

    // Validate manifest
    auto manifestValidation = manifest_.validate();
    if (!manifestValidation.valid) {
        result.valid = false;
        for (const auto& err : manifestValidation.errors) {
            result.errors.push_back("Manifest: " + err);
        }
    }

    // Check root layout restrictions
    std::set<std::string> foundRoots;
    std::set<std::string> foundVariants;
    bool hasManifest = false;
    bool hasVariantsDir = false;

    for (const auto& entry : entries_) {
        std::string rootComponent = PathNormalizer::getRootComponent(entry.path);

        // Check if root entry is allowed
        if (ALLOWED_ROOT_ENTRIES.find(rootComponent) == ALLOWED_ROOT_ENTRIES.end()) {
            result.valid = false;
            result.errors.push_back("Forbidden root entry: " + rootComponent);
        }

        if (entry.path == "manifest.json") {
            hasManifest = true;
        }

        if (rootComponent == "variants") {
            hasVariantsDir = true;

            // Check variant structure
            auto pathComponents = PathNormalizer::splitPath(entry.path);
            if (pathComponents.size() >= 2) {
                std::string variantName = PathNormalizer::toLowercase(pathComponents[1]);
                foundVariants.insert(variantName);
            }

            // Check that nothing is directly under variants/ (only directories)
            if (pathComponents.size() == 2 && !entry.isDirectory) {
                result.valid = false;
                result.errors.push_back("File directly under variants/: " + entry.path);
            }
        }

        // Validate path
        auto pathValidation = PathNormalizer::validateArchivePath(entry.path);
        if (!pathValidation.valid) {
            result.valid = false;
            result.errors.push_back("Invalid path '" + entry.path + "': " + pathValidation.error);
        }

        // Check for forbidden file types (handled by tar reader, but double-check)
        // TarReader already filters these, but we verify the entries are regular files or dirs
    }

    if (!hasManifest) {
        result.valid = false;
        result.errors.push_back("Missing manifest.json");
    }

    if (!hasVariantsDir) {
        result.valid = false;
        result.errors.push_back("Missing variants/ directory");
    }

    validateIconAsset(result);

    for (const auto& variant : foundVariants) {
        const std::string error = misspelledVariantError(variant);
        if (!error.empty()) {
            result.valid = false;
            result.errors.push_back(error);
        }
    }

    // Validate completeness (variants <-> main mapping)
    auto completenessResult = manifest_.validateCompleteness(foundVariants);
    if (!completenessResult.valid) {
        result.valid = false;
        for (const auto& err : completenessResult.errors) {
            result.errors.push_back(err);
        }
    }

    // Build a set of normalized file paths for O(1) lookups
    std::unordered_set<std::string> filePaths;
    for (const auto& entry : entries_) {
        if (entry.isDirectory) continue;
        std::string p = entry.path;
        while (!p.empty() && p.back() == '/') p.pop_back();
        filePaths.insert(std::move(p));
    }

    for (const auto& [variant, mainPath] : manifest_.main) {
        std::string fullPath = "variants/" + variant + "/" + mainPath;
        if (filePaths.find(fullPath) == filePaths.end()) {
            result.valid = false;
            result.errors.push_back("main[" + variant + "] points to non-existent file: " + mainPath);
        }
    }

    if (manifest_.type == "ui_qml" && !manifest_.view.empty()) {
        for (const auto& variant : foundVariants) {
            std::string fullPath = "variants/" + variant + "/" + manifest_.view;
            if (filePaths.find(fullPath) == filePaths.end()) {
                result.valid = false;
                result.errors.push_back("view file missing for variant '" + variant + "': " + manifest_.view);
            }
        }
    }

    // Verify content hashes (mandatory when package has content)
    if (!crypto::init()) {
        result.valid = false;
        result.errors.push_back("Failed to initialize crypto library for hash verification");
        return result;
    }
    {
        auto recomputedHashes = crypto::computeMerkleTree(entries_);
        bool hasContent = !recomputedHashes.empty();

        if (hasContent && manifest_.hashes.empty()) {
            result.valid = false;
            result.errors.push_back("Missing content hashes in manifest");
        } else if (hasContent) {
            auto rootIt = manifest_.hashes.find("root");
            auto recomputedRootIt = recomputedHashes.find("root");

            if (rootIt == manifest_.hashes.end()) {
                result.valid = false;
                result.errors.push_back("Missing 'root' hash in manifest");
            } else if (recomputedRootIt == recomputedHashes.end()) {
                result.valid = false;
                result.errors.push_back("Cannot compute root hash (no hashable content)");
            } else if (rootIt->second != recomputedRootIt->second) {
                result.valid = false;
                result.errors.push_back("Content hash mismatch: package content does not match manifest hashes");
            }
        }
    }

    return result;
}

Package::VerifyResult Package::verify(const std::filesystem::path& lgxPath) {
    // Load package
    auto pkgOpt = load(lgxPath);
    if (!pkgOpt) {
        VerifyResult result;
        result.valid = false;
        result.errors.push_back(lastError_);
        return result;
    }

    return pkgOpt->validatePackage();
}

Package::Result Package::addVariant(
    const std::string& variant,
    const std::filesystem::path& filesPath,
    const std::optional<std::string>& mainPath
) {
    namespace fs = std::filesystem;
    
    std::string variantLc = PathNormalizer::toLowercase(variant);
    
    // Validate variant name
    if (variantLc.empty()) {
        return Result::fail("Variant name cannot be empty");
    }

    if (const std::string error = misspelledVariantError(variantLc); !error.empty()) {
        return Result::fail(error);
    }

    // Check if path exists
    std::error_code ec;
    if (!fs::exists(filesPath, ec)) {
        return Result::fail("Path does not exist: " + filesPath.string());
    }
    
    bool isDir = fs::is_directory(filesPath, ec);
    const bool uiQmlPackage = manifest_.type == "ui_qml";
    
    // Determine main path
    std::string resolvedMain;
    if (isDir) {
        if (!mainPath && !uiQmlPackage) {
            return Result::fail("--main is required when --files is a directory");
        }
        if (mainPath) {
            resolvedMain = *mainPath;
        }
    } else {
        // Single file: main is the basename
        resolvedMain = mainPath.value_or(filesPath.filename().string());
    }
    
    if (!resolvedMain.empty()) {
        // Validate main path
        auto mainValidation = PathNormalizer::validateArchivePath(resolvedMain);
        if (!mainValidation.valid) {
            return Result::fail("Invalid main path: " + mainValidation.error);
        }
    }
    
    // Remove existing variant entries
    removeVariantEntries(variantLc);
    
    // Build archive base path
    std::string archiveBase = "variants/" + variantLc;
    
    if (isDir) {
        // Directory contents go directly under variant root
        // archiveBase stays as "variants/<variant>"
    } else {
        // Single file: include filename in path
        std::string fileName = filesPath.filename().string();
        archiveBase = archiveBase + "/" + fileName;
    }
    
    // Add variant directory entry
    TarEntry variantDirEntry;
    variantDirEntry.path = "variants/" + variantLc;
    variantDirEntry.isDirectory = true;
    entries_.push_back(variantDirEntry);
    
    // Add new entries
    auto addResult = addFilesystemEntries(filesPath, archiveBase);
    if (!addResult.success) {
        return addResult;
    }
    
    // Update manifest main. ui_qml directory variants may legitimately omit
    // backend metadata, so clear any stale entry when main is absent.
    if (!resolvedMain.empty()) {
        manifest_.setMain(variantLc, resolvedMain);
    } else {
        manifest_.removeMain(variantLc);
    }

    // Invalidate signature and recompute hashes (content changed)
    clearSignature();
    auto hashResult = recomputeHashes();
    if (!hashResult.success) {
        return hashResult;
    }

    return Result::ok();
}

Package::Result Package::removeVariant(const std::string& variant) {
    std::string variantLc = PathNormalizer::toLowercase(variant);

    if (!hasVariant(variantLc)) {
        return Result::fail("Variant does not exist: " + variant);
    }

    removeVariantEntries(variantLc);
    manifest_.removeMain(variantLc);

    // Invalidate signature and recompute hashes (content changed)
    clearSignature();
    auto hashResult = recomputeHashes();
    if (!hashResult.success) {
        return hashResult;
    }

    return Result::ok();
}

bool Package::hasVariant(const std::string& variant) const {
    std::string variantLc = PathNormalizer::toLowercase(variant);
    std::string prefix = "variants/" + variantLc + "/";
    std::string exactDir = "variants/" + variantLc;
    
    for (const auto& entry : entries_) {
        std::string path = entry.path;
        // Normalize trailing slash
        if (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
        
        if (path == exactDir || path.substr(0, prefix.length()) == prefix) {
            return true;
        }
    }
    
    return false;
}

std::set<std::string> Package::getVariants() const {
    std::set<std::string> variants;
    
    for (const auto& entry : entries_) {
        auto components = PathNormalizer::splitPath(entry.path);
        if (components.size() >= 2 && components[0] == "variants") {
            variants.insert(PathNormalizer::toLowercase(components[1]));
        }
    }
    
    return variants;
}

bool Package::wouldMainChange(const std::string& variant, const std::string& newMain) const {
    auto currentMain = manifest_.getMain(variant);
    if (!currentMain) {
        return false;
    }
    return *currentMain != newMain;
}

std::string Package::getLastError() {
    return lastError_;
}

void Package::removeVariantEntries(const std::string& variant) {
    std::string variantLc = PathNormalizer::toLowercase(variant);
    std::string prefix = "variants/" + variantLc + "/";
    std::string exactDir = "variants/" + variantLc;
    
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const TarEntry& entry) {
                std::string path = entry.path;
                if (!path.empty() && path.back() == '/') {
                    path.pop_back();
                }
                return path == exactDir || path.substr(0, prefix.length()) == prefix;
            }),
        entries_.end()
    );
}

Package::Result Package::addFilesystemEntries(
    const std::filesystem::path& fsPath,
    const std::string& archiveBasePath
) {
    namespace fs = std::filesystem;
    std::error_code ec;
    
    // NFC normalize the archive path
    auto normalizedBaseOpt = PathNormalizer::toNFC(archiveBasePath);
    if (!normalizedBaseOpt) {
        return Result::fail("Failed to NFC-normalize path: " + archiveBasePath);
    }
    std::string normalizedBase = *normalizedBaseOpt;
    
    if (fs::is_regular_file(fsPath, ec)) {
        // Single file
        std::ifstream file(fsPath, std::ios::binary);
        if (!file) {
            return Result::fail("Cannot read file: " + fsPath.string());
        }
        
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );
        
        TarEntry entry;
        entry.path = normalizedBase;
        entry.data = std::move(data);
        entry.isDirectory = false;
        
        auto status = fs::status(fsPath, ec);
        if (!ec) {
            entry.mode = static_cast<uint32_t>(status.permissions() & fs::perms::mask);
        }
        
        entries_.push_back(std::move(entry));
    } else if (fs::is_directory(fsPath, ec)) {
        // Directory - add entry for the directory itself
        TarEntry dirEntry;
        dirEntry.path = normalizedBase;
        dirEntry.isDirectory = true;
        
        auto status = fs::status(fsPath, ec);
        if (!ec) {
            dirEntry.mode = static_cast<uint32_t>(status.permissions() & fs::perms::mask);
        }
        
        entries_.push_back(dirEntry);
        
        // Recursively add contents
        for (const auto& item : fs::recursive_directory_iterator(fsPath, ec)) {
            // Get relative path from fsPath
            auto relPath = fs::relative(item.path(), fsPath, ec);
            if (ec) {
                continue;
            }
            
            // Build archive path
            std::string archivePath = normalizedBase + "/" + relPath.generic_string();
            
            // NFC normalize
            auto normalizedPathOpt = PathNormalizer::toNFC(archivePath);
            if (!normalizedPathOpt) {
                return Result::fail("Failed to NFC-normalize: " + archivePath);
            }
            
            if (fs::is_directory(item.path(), ec)) {
                TarEntry entry;
                entry.path = *normalizedPathOpt;
                entry.isDirectory = true;
                
                auto status = fs::status(item.path(), ec);
                if (!ec) {
                    entry.mode = static_cast<uint32_t>(status.permissions() & fs::perms::mask);
                }
                
                entries_.push_back(std::move(entry));
            } else if (fs::is_regular_file(item.path(), ec)) {
                std::ifstream file(item.path(), std::ios::binary);
                if (!file) {
                    return Result::fail("Cannot read file: " + item.path().string());
                }
                
                std::vector<uint8_t> data(
                    (std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>()
                );
                
                TarEntry entry;
                entry.path = *normalizedPathOpt;
                entry.data = std::move(data);
                entry.isDirectory = false;
                
                auto status = fs::status(item.path(), ec);
                if (!ec) {
                    entry.mode = static_cast<uint32_t>(status.permissions() & fs::perms::mask);
                }
                
                entries_.push_back(std::move(entry));
            } else {
                // Skip symlinks, special files, etc.
                // Could add a warning here
            }
        }
    } else {
        return Result::fail("Path is not a regular file or directory: " + fsPath.string());
    }
    
    return Result::ok();
}

std::vector<std::string> Package::getRequiredDirectories(const std::string& path) {
    std::vector<std::string> dirs;
    auto components = PathNormalizer::splitPath(path);
    
    // Build up directory paths
    std::string current;
    for (size_t i = 0; i < components.size() - 1; ++i) {
        if (!current.empty()) {
            current += '/';
        }
        current += components[i];
        dirs.push_back(current);
    }
    
    return dirs;
}

Package::Result Package::extractVariant(
    const std::string& variant,
    const std::filesystem::path& outputDir
) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    
    std::string variantLc = PathNormalizer::toLowercase(variant);
    
    if (!hasVariant(variantLc)) {
        return Result::fail("Variant does not exist: " + variant);
    }
    
    fs::path variantOutputDir = outputDir / variantLc;

    // Resolve the containment root once. weakly_canonical works even if the
    // directory does not exist yet; fall back to lexical normalization if the
    // filesystem cannot resolve it.
    fs::path canonRoot = fs::weakly_canonical(variantOutputDir, ec);
    if (ec || canonRoot.empty()) {
        canonRoot = variantOutputDir.lexically_normal();
        ec.clear();
    }

    std::string prefix = "variants/" + variantLc + "/";

    // Root-level `assets/` is variant-independent and must land in the SAME
    // output directory as the variant contents, because the manifest's `icon`
    // is documented as relative to the installed package root. Extracting
    // only `variants/<v>/` left `assets/icon.png` on the floor, so every
    // installed 0.4.0 package resolved its icon to a missing file and fell
    // back to the monogram — a regression against 0.3.x, where the icon lived
    // inside the variant and therefore did extract.
    const std::string assetsPrefix = "assets/";

    for (const auto& entry : entries_) {
        const bool inVariant = entry.path.compare(0, prefix.length(), prefix) == 0;
        const bool inAssets =
            entry.path.compare(0, assetsPrefix.length(), assetsPrefix) == 0;
        if (!inVariant && !inAssets) {
            continue;
        }

        // Zip-slip defense. Package::load() stores tar entry paths verbatim and
        // never runs validateArchivePath() on the load->extract path, so a
        // crafted .lgx can carry a "variants/<v>/../../.." entry that satisfies
        // the prefix test above. Reject any entry the archive validator would
        // reject (absolute paths, ".." segments, backslashes, non-NFC) before
        // touching the filesystem.
        auto pathValidation = PathNormalizer::validateArchivePath(entry.path);
        if (!pathValidation.valid) {
            return Result::fail("Refusing to extract unsafe archive path '" +
                                entry.path + "': " + pathValidation.error);
        }

        // Variant entries are rebased to the variant root; asset entries keep
        // their `assets/...` path so the installed layout matches the manifest.
        std::string relativePath =
            inVariant ? entry.path.substr(prefix.length()) : entry.path;
        if (relativePath.empty()) {
            continue;
        }

        fs::path fullPath = variantOutputDir / relativePath;

        // Defense in depth: the normalized target must stay under canonRoot.
        // Guards against escapes that the per-entry check might miss (e.g. via
        // already-resolved separators) without depending on the file existing.
        //
        // canonFull is built by joining relativePath onto canonRoot (not from
        // fullPath directly) so both sides of the comparison always share the
        // same base. canonRoot is absolute when weakly_canonical succeeds but
        // fullPath stays relative when outputDir is relative (e.g. the CLI's
        // default "."); comparing an absolute base against a relative target
        // makes lexically_relative() return empty and false-rejects every
        // benign entry. Anchoring to canonRoot keeps them consistent.
        fs::path canonFull = (canonRoot / relativePath).lexically_normal();
        fs::path rel = canonFull.lexically_relative(canonRoot);
        if (rel.empty() || *rel.begin() == "..") {
            return Result::fail("Path escapes output directory: " + fullPath.string());
        }

        if (entry.isDirectory) {
            if (!fs::create_directories(fullPath, ec) && ec) {
                return Result::fail("Failed to create directory: " + fullPath.string() + " - " + ec.message());
            }
        } else {
            fs::path parentDir = fullPath.parent_path();
            if (!parentDir.empty() && !fs::exists(parentDir)) {
                if (!fs::create_directories(parentDir, ec) && ec) {
                    return Result::fail("Failed to create directory: " + parentDir.string() + " - " + ec.message());
                }
            }
            
            std::ofstream file(fullPath, std::ios::binary);
            if (!file) {
                return Result::fail("Failed to create file: " + fullPath.string());
            }
            
            file.write(reinterpret_cast<const char*>(entry.data.data()), entry.data.size());
            if (!file) {
                return Result::fail("Failed to write file: " + fullPath.string());
            }
            file.close();

            if (entry.mode != 0) {
                // Honour the archived mode, but NEVER extract a file the
                // extracting user cannot delete.
                //
                // A .lgx built by nix-bundle-lgx carries store modes, and the
                // Nix store is 0444, so payload files can arrive read-only.
                // std::filesystem maps a mode with no write bit to
                // FILE_ATTRIBUTE_READONLY on Windows, and Windows refuses to
                // DELETE such a file -- POSIX consults only the parent
                // directory's write bit, which is why this is invisible
                // everywhere else. The consequence was severe and remote from
                // here: the package manager's temp-directory cleanup threw an
                // uncaught filesystem_error AFTER a successful install, so the
                // install reply was never sent and the UI reported failure for
                // a package that had installed perfectly; uninstall and upgrade
                // failed the same way with "Access is denied".
                //
                // The producer no longer ships read-only icons and the consumer
                // no longer dies on cleanup, but this is the mechanism itself,
                // and it protects every OTHER consumer of lgx_extract -- plus
                // the packages already published with the bit set.
                auto mode = static_cast<fs::perms>(entry.mode & 0777);
                mode |= fs::perms::owner_write;
                fs::permissions(fullPath, mode, ec);
                if (ec) {
                    return Result::fail("Failed to set permissions on: " + fullPath.string() + " - " + ec.message());
                }
            }
        }
    }
    
    return Result::ok();
}

Package::Result Package::extractAll(const std::filesystem::path& outputDir) const {
    auto variants = getVariants();
    
    for (const auto& variant : variants) {
        auto result = extractVariant(variant, outputDir);
        if (!result.success) {
            return result;
        }
    }
    
    return Result::ok();
}

Package::Result Package::signPackage(const crypto::SecretKey& sk,
                                      const std::string& signerName,
                                      const std::string& signerUrl) {
    if (!crypto::init()) {
        return Result::fail("Failed to initialize crypto library");
    }

    // 1. Validate package (structure + hashes)
    auto validation = validatePackage();
    if (!validation.valid) {
        std::string err = validation.errors.empty() ? "Package validation failed"
                          : validation.errors[0];
        return Result::fail("Cannot sign invalid package: " + err);
    }

    // 2. Get deterministic manifest JSON bytes
    std::string manifestJson = manifest_.toJson();
    std::vector<uint8_t> manifestBytes(manifestJson.begin(), manifestJson.end());

    // 4. Sign with Ed25519
    auto signature = crypto::sign(manifestBytes, sk);

    // 5. Build ManifestSig with DID
    auto pk = crypto::extractPublicKey(sk);
    crypto::ManifestSig sig;
    sig.version = 1;
    sig.algorithm = "ed25519";
    sig.did = crypto::publicKeyToDid(pk);
    sig.signature = crypto::base64Encode(signature.data(), signature.size());
    sig.signerName = signerName;
    sig.signerUrl = signerUrl;

    manifestSig_ = std::move(sig);

    return Result::ok();
}

Package::SignatureInfo Package::verifySignature() const {
    SignatureInfo info{};
    info.is_signed = false;
    info.signature_valid = false;
    info.package_valid = false;

    // Validate package structure and content hashes first
    auto pkgValidation = validatePackage();
    if (!pkgValidation.valid) {
        info.package_valid = false;
        info.error = pkgValidation.errors.empty() ? "Package validation failed"
                     : pkgValidation.errors[0];
        return info;
    }
    info.package_valid = true;

    if (!manifestSig_.has_value()) {
        if (manifestSigParseError_) {
            info.is_signed = true;
            info.signature_valid = false;
            info.error = "Failed to parse manifest.sig";
        }
        return info;
    }

    info.is_signed = true;
    info.signer_did = manifestSig_->did;
    info.signer_name = manifestSig_->signerName;
    info.signer_url = manifestSig_->signerUrl;

    if (!crypto::init()) {
        info.error = "Failed to initialize crypto library";
        return info;
    }

    // Extract public key from DID
    auto pkOpt = crypto::didToPublicKey(manifestSig_->did);
    if (!pkOpt) {
        info.error = "Invalid DID in manifest.sig: " + manifestSig_->did;
        return info;
    }
    crypto::PublicKey pk = *pkOpt;

    // Decode signature
    auto sigBytes = crypto::base64Decode(manifestSig_->signature);
    if (!sigBytes || sigBytes->size() != crypto::SIGNATURE_SIZE) {
        info.error = "Invalid signature in manifest.sig";
        return info;
    }
    crypto::Signature sig;
    std::copy(sigBytes->begin(), sigBytes->end(), sig.begin());

    // Verify Ed25519 signature over manifest.toJson() bytes
    std::string manifestJson = manifest_.toJson();
    std::vector<uint8_t> manifestBytes(manifestJson.begin(), manifestJson.end());

    if (!crypto::verify(manifestBytes, pk, sig)) {
        info.error = "Ed25519 signature verification failed";
        return info;
    }

    info.signature_valid = true;
    return info;
}

void Package::clearSignature() {
    manifestSig_ = std::nullopt;
}

Package::Result Package::setIcon(const std::vector<uint8_t>& pngData) {
    if (pngData.empty()) {
        return Result::fail("Icon data is empty");
    }

    const std::string iconPath = Manifest::ICON_PATH;

    // Replace any existing entry at the canonical path.
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&iconPath](const TarEntry& e) {
                           return e.path == iconPath;
                       }),
        entries_.end());

    TarEntry iconEntry;
    iconEntry.path = iconPath;
    iconEntry.data = pngData;
    iconEntry.isDirectory = false;
    entries_.push_back(std::move(iconEntry));

    manifest_.icon = iconPath;

    // Invalidate signature and recompute hashes (content changed)
    clearSignature();
    auto hashResult = recomputeHashes();
    if (!hashResult.success) {
        return hashResult;
    }

    return Result::ok();
}

Package::Result Package::recomputeHashes() {
    if (!crypto::init()) {
        return Result::fail("Failed to initialize crypto library — cannot compute content hashes");
    }
    auto hashes = crypto::computeMerkleTree(entries_);
    manifest_.hashes = std::move(hashes);
    return Result::ok();
}

} // namespace lgx
