#include "path_normalizer.h"

#if defined(LGX_UNICODE_COREFOUNDATION)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#include <unicode/uchar.h>
#include <unicode/utypes.h>
#endif

#include <algorithm>
#include <sstream>

namespace lgx {

#if defined(LGX_UNICODE_COREFOUNDATION)
namespace {
// CFMutableString round trip: UTF-8 in, transformed, UTF-8 out.
std::optional<std::string> cfTransform(const std::string& in, void (*op)(CFMutableStringRef)) {
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(in.data()), static_cast<CFIndex>(in.size()),
        kCFStringEncodingUTF8, false);
    if (!s) return std::nullopt;
    CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
    CFRelease(s);
    if (!m) return std::nullopt;
    op(m);
    const CFRange all = CFRangeMake(0, CFStringGetLength(m));
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(all.length, kCFStringEncodingUTF8);
    std::string out(static_cast<size_t>(capacity), '\0');
    CFIndex used = 0;
    CFStringGetBytes(m, all, kCFStringEncodingUTF8, 0, false,
        reinterpret_cast<UInt8*>(&out[0]), capacity, &used);
    out.resize(static_cast<size_t>(used));
    CFRelease(m);
    return out;
}
void nfc(CFMutableStringRef m) { CFStringNormalize(m, kCFStringNormalizationFormC); }
void lower(CFMutableStringRef m) { CFStringLowercase(m, nullptr); }
} // namespace

std::optional<std::string> PathNormalizer::toNFC(const std::string& path) {
    return cfTransform(path, nfc);
}

bool PathNormalizer::isNFC(const std::string& str) {
    auto n = cfTransform(str, nfc);
    return n && *n == str;
}

std::string PathNormalizer::toLowercase(const std::string& str) {
    return cfTransform(str, lower).value_or(str);
}
#else

std::optional<std::string> PathNormalizer::toNFC(const std::string& path) {
    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* normalizer = icu::Normalizer2::getNFCInstance(status);
    
    if (U_FAILURE(status)) {
        return std::nullopt;
    }
    
    // Convert UTF-8 to ICU UnicodeString
    icu::UnicodeString ustr = icu::UnicodeString::fromUTF8(path);
    
    // Normalize to NFC
    icu::UnicodeString normalized;
    normalizer->normalize(ustr, normalized, status);
    
    if (U_FAILURE(status)) {
        return std::nullopt;
    }
    
    // Convert back to UTF-8
    std::string result;
    normalized.toUTF8String(result);
    
    return result;
}

bool PathNormalizer::isNFC(const std::string& str) {
    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* normalizer = icu::Normalizer2::getNFCInstance(status);
    
    if (U_FAILURE(status)) {
        return false;
    }
    
    icu::UnicodeString ustr = icu::UnicodeString::fromUTF8(str);
    return normalizer->isNormalized(ustr, status) && U_SUCCESS(status);
}

std::string PathNormalizer::toLowercase(const std::string& str) {
    UErrorCode status = U_ZERO_ERROR;
    icu::UnicodeString ustr = icu::UnicodeString::fromUTF8(str);
    ustr.toLower();
    
    std::string result;
    ustr.toUTF8String(result);
    return result;
}
#endif // LGX_UNICODE_COREFOUNDATION

PathNormalizer::ValidationResult PathNormalizer::validateArchivePath(const std::string& archivePath) {
    // Check for empty path
    if (archivePath.empty()) {
        return ValidationResult::fail("Path is empty");
    }
    
    // Check for backslashes (forbidden in archive paths)
    if (archivePath.find('\\') != std::string::npos) {
        return ValidationResult::fail("Path contains backslashes");
    }
    
    // Check for absolute path
    if (isAbsolute(archivePath)) {
        return ValidationResult::fail("Path is absolute");
    }
    
    // Split and check for ".." segments
    auto components = splitPath(archivePath);
    for (const auto& component : components) {
        if (component == "..") {
            return ValidationResult::fail("Path contains '..' segment");
        }
    }
    
    // Verify NFC normalization
    if (!isNFC(archivePath)) {
        return ValidationResult::fail("Path is not NFC-normalized");
    }
    
    return ValidationResult::ok();
}

std::string PathNormalizer::normalizeSeparators(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    
    bool lastWasSep = false;
    for (char c : path) {
        if (c == '\\' || c == '/') {
            if (!lastWasSep) {
                result += '/';
                lastWasSep = true;
            }
        } else {
            result += c;
            lastWasSep = false;
        }
    }
    
    // Remove trailing slash unless it's the root
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
    return result;
}

std::string PathNormalizer::joinPath(const std::vector<std::string>& components) {
    if (components.empty()) {
        return "";
    }
    
    std::string result;
    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0) {
            result += '/';
        }
        result += components[i];
    }
    return result;
}

std::string PathNormalizer::joinPath(const std::string& base, const std::string& relative) {
    if (base.empty()) {
        return relative;
    }
    if (relative.empty()) {
        return base;
    }
    
    std::string result = base;
    if (result.back() != '/') {
        result += '/';
    }
    
    // Skip leading slash in relative if present
    if (relative[0] == '/') {
        result += relative.substr(1);
    } else {
        result += relative;
    }
    
    return result;
}

std::string PathNormalizer::basename(const std::string& path) {
    auto normalized = normalizeSeparators(path);
    auto pos = normalized.rfind('/');
    if (pos == std::string::npos) {
        return normalized;
    }
    return normalized.substr(pos + 1);
}

std::string PathNormalizer::dirname(const std::string& path) {
    auto normalized = normalizeSeparators(path);
    auto pos = normalized.rfind('/');
    if (pos == std::string::npos) {
        return "";
    }
    if (pos == 0) {
        return "/";
    }
    return normalized.substr(0, pos);
}

bool PathNormalizer::isAbsolute(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    
    // Unix absolute path
    if (path[0] == '/') {
        return true;
    }
    
    // Windows absolute path (e.g., C:\)
    if (path.size() >= 3 && std::isalpha(path[0]) && path[1] == ':' && 
        (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    
    return false;
}

std::vector<std::string> PathNormalizer::splitPath(const std::string& path) {
    std::vector<std::string> components;
    auto normalized = normalizeSeparators(path);
    
    std::stringstream ss(normalized);
    std::string component;
    
    while (std::getline(ss, component, '/')) {
        if (!component.empty() && component != ".") {
            components.push_back(component);
        }
    }
    
    return components;
}

std::string PathNormalizer::getRootComponent(const std::string& archivePath) {
    auto components = splitPath(archivePath);
    if (components.empty()) {
        return "";
    }
    return components[0];
}

} // namespace lgx
