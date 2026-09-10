#include "platform_variant.h"

#include <algorithm>
#include <cctype>
#include <map>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace lgx {

namespace {

// The one place a variant-name SPELLING is enumerated.
//
// A variant name is "<os>-<architecture>", and both halves have more than one
// live spelling in this ecosystem. Canonical vocabulary is the one
// logos-module-builder's lib/resolvePlatforms.nix pins -- os in
// {linux, darwin, windows}, architecture in {x86_64, aarch64} -- so the first
// entry of every row below is the canonical spelling and the rest are legacy
// ones that some producer actually writes:
//
//   nix-bundle-lgx/flake.nix:52                   darwin-amd64  linux-amd64
//                                                 darwin-arm64  linux-arm64
//   nix-bundle-logos-module-install/flake.nix:49  darwin-x86_64 linux-x86_64
//                                                 darwin-arm64  linux-arm64
//
// Those two producers disagree with each other while the SECOND CONSUMES THE
// FIRST -- it bundles a .lgx the first named and then calls the package
// manager with --platform spelled its own way -- so a CONSUMER is where the
// disagreement has to be absorbed, and this table is the one every consumer
// absorbs it from.
//
// Only the ARCHITECTURE is aliased. The OS half is matched verbatim on
// purpose: aliasing it would weaken the fail-closed check that stops a Windows
// package being installed as a macOS one, and "macos" (logos-release-set's
// release-ASSET naming) is a different namespace that is translated to
// "darwin" before it ever reaches an .lgx.
//
// Adding a spelling here is safe -- it only ever widens what an EXISTING
// package resolves to, and rows are per-architecture so a new OS inherits
// every alias without a new code path. Removing one is not: variant names sit
// inside the signed hash tree (this library writes hashes["variants/<name>"]),
// so a published package can never be renamed on disk. Alias on read.
const std::vector<std::vector<std::string>>& architectureSpellings()
{
    static const std::vector<std::vector<std::string>> kSpellings = {
        { "x86_64",  "amd64" },
        { "aarch64", "arm64" },
    };
    return kSpellings;
}

// The flavour suffix a non-portable build of the package manager appends. It
// is a property of the CONSUMER's build, not of the target, so it rides on top
// of every name here rather than doubling the table.
const char kDevSuffix[] = "-dev";
constexpr std::string::size_type kDevSuffixLength = sizeof(kDevSuffix) - 1;

bool endsWithDevSuffix(const std::string& variant)
{
    return variant.size() > kDevSuffixLength
        && variant.compare(variant.size() - kDevSuffixLength, kDevSuffixLength, kDevSuffix) == 0;
}

std::string withoutDevSuffix(const std::string& variant)
{
    return endsWithDevSuffix(variant)
         ? variant.substr(0, variant.size() - kDevSuffixLength)
         : variant;
}

// Every name a producer SHOULD write, one per target this ecosystem ships.
//
// Mobile and web joined the desktop three for the store shell. Two rules the
// desktop rows left implicit and these make load-bearing:
//
//   ios-sim-arm64 is not ios-arm64. Same chip, different ABI -- a device
//   framework does not load on the simulator -- so they are separate targets
//   whose OS halves ("ios-sim" / "ios") never alias to each other, exactly as
//   "android" never aliases to "linux" despite the shared kernel.
//
//   web has no architecture half at all. The Web container runs the same bytes
//   everywhere, so there is nothing to alias and no native host ever resolves
//   to it: a web payload needs the container, and a host without one would
//   install JavaScript where it loads a plugin.
//
// linux-x86, windows-x86 and ios-sim-x86_64 are here because hostVariant() can
// still compute them; nothing in this milestone builds for them.
const std::vector<std::string>& canonicalVariants()
{
    static const std::vector<std::string> kCanonical = {
        "linux-x86_64",   "linux-arm64",   "linux-x86",
        "darwin-x86_64",  "darwin-arm64",
        "windows-x86_64", "windows-arm64", "windows-x86",
        "android-arm64",  "android-x86_64",
        "ios-arm64",
        "ios-sim-arm64",  "ios-sim-x86_64",
        "web",
    };
    return kCanonical;
}

// Fold a written name onto the key that it and every plausible misspelling of
// it collapse to: case, '-' and '_' carry no meaning between producers, so
// "ios_arm64", "iosarm64" and "ios-arm64" are one intent, and "linux-x8664" is
// "linux-x86_64" with a lost underscore.
std::string spellingKey(const std::string& name)
{
    std::string key;
    key.reserve(name.size());
    for (char c : name) {
        if (c == '-' || c == '_') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return key;
}

// Names OTHER toolchains use for a target this vocabulary already has, keyed
// by spellingKey(). They are not accepted -- a package carrying one installs
// nowhere -- but each is what some real build system calls the same thing, so
// each is worth naming the intended target for rather than shrugging at:
// Apple's SDK names (iphoneos / iphonesimulator) and CMake's ios-simulator,
// Android's ABI names (arm64-v8a), the Emscripten toolchain's names for the
// web target (wasm / emscripten), and the macOS-facing spellings of darwin.
const std::map<std::string, std::string>& foreignSpellings()
{
    static const std::map<std::string, std::string> kForeign = {
        { "macosarm64",           "darwin-arm64"   },
        { "macosx8664",           "darwin-x86_64"  },
        { "macosamd64",           "darwin-x86_64"  },
        { "macosaarch64",         "darwin-arm64"   },
        { "osxarm64",             "darwin-arm64"   },
        { "osxx8664",             "darwin-x86_64"  },
        { "iphoneosarm64",        "ios-arm64"      },
        { "iphoneos",             "ios-arm64"      },
        { "iossimulatorarm64",    "ios-sim-arm64"  },
        { "iossimulatorx8664",    "ios-sim-x86_64" },
        { "iphonesimulatorarm64", "ios-sim-arm64"  },
        { "iphonesimulatorx8664", "ios-sim-x86_64" },
        { "iphonesimulator",      "ios-sim-arm64"  },
        { "arm64v8a",             "android-arm64"  },
        { "androidarm64v8a",      "android-arm64"  },
        { "androidx8664abi",      "android-x86_64" },
        { "wasm",                 "web"            },
        { "wasm32",               "web"            },
        { "webassembly",          "web"            },
        { "emscripten",           "web"            },
        { "win32x8664",           "windows-x86_64" },
        { "winx8664",             "windows-x86_64" },
        { "winarm64",             "windows-arm64"  },
    };
    return kForeign;
}

// Every spelling this library ACCEPTS as written -- each canonical name plus
// its architecture aliases -- keyed by spellingKey(), so a fold-equal
// misspelling lands on the canonical name it meant.
const std::map<std::string, std::string>& acceptedSpellings()
{
    static const std::map<std::string, std::string> kAccepted = [] {
        std::map<std::string, std::string> accepted;
        for (const auto& canonical : canonicalVariants()) {
            for (const auto& spelling : variantSpellings(canonical)) {
                accepted.emplace(spellingKey(spelling), canonical);
            }
        }
        return accepted;
    }();
    return kAccepted;
}

// Edit distance, capped: nothing past `limit` can become a suggestion, so a
// row is abandoned as soon as it cannot come back under the cap.
std::size_t editDistanceWithin(const std::string& a, const std::string& b, std::size_t limit)
{
    const std::size_t over = limit + 1;
    if (a.size() > b.size() + limit || b.size() > a.size() + limit) return over;

    std::vector<std::size_t> previous(b.size() + 1);
    std::vector<std::size_t> current(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) previous[j] = j;

    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = i;
        std::size_t rowBest = current[0];
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            current[j] = std::min({ previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost });
            rowBest = std::min(rowBest, current[j]);
        }
        if (rowBest > limit) return over;
        previous.swap(current);
    }
    return previous[b.size()];
}

// The canonical name behind the nearest accepted spelling, or empty.
//
// Only long names are matched by distance: at three characters "web" is within
// two edits of a great many short words, so it is reachable through the
// explicit foreign-spelling table alone. A near miss on a long name is
// overwhelmingly a typo; a short one is overwhelmingly someone else's
// vocabulary, and this library does not own those.
std::string nearestAcceptedSpelling(const std::string& key)
{
    constexpr std::size_t kMinimumLength = 6;
    constexpr std::size_t kMaximumDistance = 2;
    if (key.size() < kMinimumLength) return {};

    std::string best;
    std::size_t bestDistance = kMaximumDistance + 1;
    for (const auto& entry : acceptedSpellings()) {
        if (entry.first.size() < kMinimumLength) continue;
        const std::size_t distance = editDistanceWithin(key, entry.first, kMaximumDistance);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = entry.second;
        }
    }
    return bestDistance <= kMaximumDistance ? best : std::string();
}

} // namespace

std::string hostVariant()
{
// Emscripten first: it also defines __linux__, and the web target has no
// architecture half to fall through to.
#if defined(__EMSCRIPTEN__)
    return "web";
// Android before Linux for the same reason -- the kernel is Linux, but the
// ABI, the packaging and the runtime-code policy are not.
#elif defined(__ANDROID__)
    #if defined(__aarch64__)
        return "android-arm64";
    #elif defined(__x86_64__)
        return "android-x86_64";
    #else
        // 32-bit Android: Play has required 64-bit since 2019 and nothing in
        // this ecosystem builds for it. Naming it would imply a target exists.
        return "unknown";
    #endif
#elif defined(__APPLE__)
    // TARGET_OS_SIMULATOR before TARGET_OS_IPHONE: the simulator sets both,
    // and its slice is not the device's.
    #if TARGET_OS_SIMULATOR
        #if defined(__aarch64__)
            return "ios-sim-arm64";
        #else
            return "ios-sim-x86_64";
        #endif
    #elif TARGET_OS_IPHONE
        return "ios-arm64";
    #elif defined(__aarch64__)
        return "darwin-arm64";
    #else
        return "darwin-x86_64";
    #endif
#elif defined(__linux__)
    #if defined(__x86_64__)
        return "linux-x86_64";
    #elif defined(__aarch64__)
        return "linux-arm64";
    #else
        return "linux-x86";
    #endif
#elif defined(_WIN32)
    #if defined(_M_X64) || defined(__x86_64__)
        return "windows-x86_64";
    #else
        return "windows-x86";
    #endif
#else
    return "unknown";
#endif
}

std::vector<std::string> variantSpellings(const std::string& variant)
{
    std::vector<std::string> variants;
    if (variant.empty()) {
        return variants;
    }

    // The caller's own spelling always leads: it is what diagnostics print as
    // "this machine", and what an error names as the platform that was wanted.
    variants.emplace_back(variant);

    // Split on the LAST '-' so the architecture is the trailing component
    // whatever the OS half turns out to contain ("ios-sim" is one OS).
    const std::string::size_type sep = variant.rfind('-');
    if (sep != std::string::npos) {
        const std::string os = variant.substr(0, sep);
        const std::string arch = variant.substr(sep + 1);
        for (const auto& row : architectureSpellings()) {
            if (std::find(row.begin(), row.end(), arch) == row.end())
                continue;
            for (const auto& spelling : row) {
                if (spelling != arch)
                    variants.emplace_back(os + "-" + spelling);
            }
            break;
        }
    }

    return variants;
}

const std::vector<std::string>& knownVariants()
{
    return canonicalVariants();
}

std::string canonicalVariant(const std::string& variant)
{
    if (variant.empty()) return {};

    // "-dev" is a flavour of a target, not a target of its own: strip it,
    // resolve the target, put it back.
    const std::string bare = withoutDevSuffix(variant);
    const auto& accepted = acceptedSpellings();
    const auto it = accepted.find(spellingKey(bare));
    if (it == accepted.end()) return {};

    // A name that FOLDS onto an accepted spelling without BEING one
    // ("ios_arm64") is a misspelling, not an alias: suggestVariantName's job.
    const auto spellings = variantSpellings(it->second);
    if (std::find(spellings.begin(), spellings.end(), bare) == spellings.end())
        return {};

    return bare == variant ? it->second : it->second + kDevSuffix;
}

bool isKnownVariant(const std::string& variant)
{
    return !canonicalVariant(variant).empty();
}

std::string suggestVariantName(const std::string& variant)
{
    if (variant.empty() || isKnownVariant(variant)) return {};

    const std::string bare = withoutDevSuffix(variant);
    const std::string key = spellingKey(bare);

    std::string suggestion;
    const auto& accepted = acceptedSpellings();
    const auto acceptedIt = accepted.find(key);
    if (acceptedIt != accepted.end()) {
        suggestion = acceptedIt->second;
    } else {
        const auto& foreign = foreignSpellings();
        const auto foreignIt = foreign.find(key);
        suggestion = foreignIt != foreign.end() ? foreignIt->second
                                                : nearestAcceptedSpelling(key);
    }

    if (suggestion.empty()) return {};
    return bare == variant ? suggestion : suggestion + kDevSuffix;
}

} // namespace lgx
