#include <gtest/gtest.h>

#include "core/platform_variant.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace lgx;

namespace {

bool accepts(const std::vector<std::string>& variants, const std::string& v) {
    return std::find(variants.begin(), variants.end(), v) != variants.end();
}

std::string join(const std::vector<std::string>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += v[i];
    }
    return out + "]";
}

std::string osHalf(const std::string& variant) {
    return variant.substr(0, variant.rfind('-'));
}

} // namespace

// =============================================================================
// Producer/consumer spelling agreement
//
// A TABLE over every target this ecosystem ships, not a probe of the host,
// because the defect being pinned is invisible from any single host: an arm64
// Mac never computes darwin-x86_64 and so never meets it.
//
// Left column is what a host computes for itself (hostVariant); right column
// is a spelling some producer actually writes into a .lgx, so it has to
// resolve on that host. The producers disagree with EACH OTHER --
//
//   nix-bundle-lgx/flake.nix:52                  darwin-amd64  linux-amd64
//                                                darwin-arm64  linux-arm64
//   nix-bundle-logos-module-install/flake.nix:49 darwin-x86_64 linux-x86_64
//                                                darwin-arm64  linux-arm64
//
// -- and the second CONSUMES the first, calling lgpm --platform with its own
// spelling on a package the first named. So both columns must resolve here.
// =============================================================================

namespace {
struct VariantAliasCase {
    const char* host;          // what hostVariant() would return
    const char* alsoAccepted;  // a producer spelling that must still resolve
};

const VariantAliasCase kAliasCases[] = {
    { "linux-x86_64",   "linux-amd64"     },
    { "linux-amd64",    "linux-x86_64"    },
    { "linux-arm64",    "linux-aarch64"   },
    { "linux-aarch64",  "linux-arm64"     },
    { "darwin-x86_64",  "darwin-amd64"    },
    { "darwin-amd64",   "darwin-x86_64"   },
    { "darwin-arm64",   "darwin-aarch64"  },
    { "darwin-aarch64", "darwin-arm64"    },
    { "windows-x86_64", "windows-amd64"   },
    { "windows-amd64",  "windows-x86_64"  },
    { "windows-arm64",  "windows-aarch64" },
    { "windows-aarch64","windows-arm64"   },
};
} // namespace

TEST(PlatformVariantTest, EveryTargetAcceptsBothArchSpellings) {
    for (const auto& c : kAliasCases) {
        auto variants = variantSpellings(c.host);
        EXPECT_TRUE(accepts(variants, c.host))
            << "host " << c.host << " does not accept its own spelling; got " << join(variants);
        EXPECT_TRUE(accepts(variants, c.alsoAccepted))
            << "host " << c.host << " does not accept producer spelling "
            << c.alsoAccepted << "; got " << join(variants);
    }
}

TEST(PlatformVariantTest, TheCallersOwnSpellingLeads) {
    for (const auto& c : kAliasCases) {
        auto variants = variantSpellings(c.host);
        ASSERT_FALSE(variants.empty());
        EXPECT_EQ(variants.front(), c.host) << join(variants);
    }
}

TEST(PlatformVariantTest, NoSpellingIsListedTwice) {
    for (const auto& c : kAliasCases) {
        auto variants = variantSpellings(c.host);
        std::set<std::string> unique(variants.begin(), variants.end());
        EXPECT_EQ(unique.size(), variants.size()) << join(variants);
    }
}

// =============================================================================
// Fail-closed guarantees. Aliasing must WIDEN within one target and never
// across targets: a Windows package on a Mac, or an arm64 package on an x86_64
// host, must still be refused.
// =============================================================================

TEST(PlatformVariantTest, NeverAcceptsAnotherOperatingSystem) {
    const char* const hosts[] = { "darwin-x86_64", "darwin-arm64", "linux-x86_64",
                                  "linux-arm64", "windows-x86_64" };
    const char* const foreign[] = { "darwin-x86_64", "darwin-amd64", "darwin-arm64",
                                    "darwin-aarch64", "linux-x86_64", "linux-amd64",
                                    "linux-arm64", "linux-aarch64", "windows-x86_64",
                                    "windows-amd64", "windows-arm64", "windows-aarch64" };
    for (const char* host : hosts) {
        auto variants = variantSpellings(host);
        for (const char* f : foreign) {
            if (osHalf(f) == osHalf(host)) continue;
            EXPECT_FALSE(accepts(variants, f))
                << "host " << host << " accepted foreign-OS variant " << f
                << "; got " << join(variants);
        }
    }
}

TEST(PlatformVariantTest, NeverAcceptsAnotherArchitecture) {
    struct { const char* host; const char* foreignArch; } cases[] = {
        { "darwin-x86_64",  "darwin-arm64"    },
        { "darwin-x86_64",  "darwin-aarch64"  },
        { "darwin-arm64",   "darwin-x86_64"   },
        { "darwin-arm64",   "darwin-amd64"    },
        { "linux-x86_64",   "linux-arm64"     },
        { "linux-arm64",    "linux-amd64"     },
        { "windows-x86_64", "windows-arm64"   },
    };
    for (const auto& c : cases) {
        auto variants = variantSpellings(c.host);
        EXPECT_FALSE(accepts(variants, c.foreignArch))
            << "host " << c.host << " accepted foreign-arch variant " << c.foreignArch
            << "; got " << join(variants);
    }
}

TEST(PlatformVariantTest, UnknownArchitectureGetsNoAliasesAndStillNamesItself) {
    // A spelling no table row knows must degrade to "itself only" rather than
    // throwing away the caller's own variant.
    auto variants = variantSpellings("linux-riscv64");
    EXPECT_TRUE(accepts(variants, "linux-riscv64")) << join(variants);
    EXPECT_EQ(variants.size(), 1u) << join(variants);
}

TEST(PlatformVariantTest, ANameWithNoSeparatorHasNoArchitectureToAlias) {
    auto variants = variantSpellings("unknown");
    EXPECT_EQ(variants, std::vector<std::string>{ "unknown" }) << join(variants);
}

TEST(PlatformVariantTest, ASuffixedSpellingAliasesNothingAndStillNamesItself) {
    // lgpm appends "-dev" for a non-portable build, which lands INSIDE the
    // architecture component. Splitting on the last '-' is what keeps those
    // names out of the table entirely, so they alias nothing and still name
    // themselves -- the fail-closed degrade, not a silent widening.
    auto variants = variantSpellings("darwin-arm64-dev");
    EXPECT_EQ(variants, std::vector<std::string>{ "darwin-arm64-dev" }) << join(variants);
}

TEST(PlatformVariantTest, EmptyInEmptyOut) {
    EXPECT_TRUE(variantSpellings("").empty());
}

// =============================================================================
// The host's own name
// =============================================================================

TEST(PlatformVariantTest, HostVariantIsAnOsDashArchName) {
    const std::string host = hostVariant();
    ASSERT_FALSE(host.empty());
    EXPECT_NE(host, "unknown") << "no variant rule for this build target";
    EXPECT_NE(host.rfind('-'), std::string::npos) << host;
}

TEST(PlatformVariantTest, TheHostAcceptsItsOwnSpelling) {
    EXPECT_TRUE(accepts(variantSpellings(hostVariant()), hostVariant()));
}

// =============================================================================
// Mobile and web targets
//
// A store shell ships three more targets than the desktop table above knows,
// and each has to resolve on the SAME rules -- own spelling first, the other
// live spelling of its architecture next, nothing across a target boundary.
// The rows are a table for the same reason the desktop ones are: a Mac build
// of this test binary never computes android-arm64 and so never meets it.
// =============================================================================

namespace {
const VariantAliasCase kMobileAliasCases[] = {
    { "android-arm64",    "android-aarch64"  },
    { "android-aarch64",  "android-arm64"    },
    { "android-x86_64",   "android-amd64"    },
    { "android-amd64",    "android-x86_64"   },
    { "ios-arm64",        "ios-aarch64"      },
    { "ios-aarch64",      "ios-arm64"        },
    { "ios-sim-arm64",    "ios-sim-aarch64"  },
    { "ios-sim-aarch64",  "ios-sim-arm64"    },
};
} // namespace

TEST(PlatformVariantTest, MobileTargetsAcceptBothArchSpellings) {
    for (const auto& c : kMobileAliasCases) {
        auto variants = variantSpellings(c.host);
        EXPECT_EQ(variants.front(), c.host) << join(variants);
        EXPECT_TRUE(accepts(variants, c.alsoAccepted))
            << "host " << c.host << " does not accept producer spelling "
            << c.alsoAccepted << "; got " << join(variants);
    }
}

TEST(PlatformVariantTest, TheSimulatorIsNotTheDeviceAndTheDeviceIsNotDarwin) {
    // ios-sim-arm64 and ios-arm64 are different ABIs on the same chip: a
    // device framework does not run on the simulator, and neither is a macOS
    // dylib. The OS half being matched verbatim is what keeps all three apart.
    auto sim = variantSpellings("ios-sim-arm64");
    EXPECT_FALSE(accepts(sim, "ios-arm64")) << join(sim);
    EXPECT_FALSE(accepts(sim, "darwin-arm64")) << join(sim);

    auto device = variantSpellings("ios-arm64");
    EXPECT_FALSE(accepts(device, "ios-sim-arm64")) << join(device);
    EXPECT_FALSE(accepts(device, "darwin-arm64")) << join(device);

    auto mac = variantSpellings("darwin-arm64");
    EXPECT_FALSE(accepts(mac, "ios-arm64")) << join(mac);
    EXPECT_FALSE(accepts(mac, "ios-sim-arm64")) << join(mac);
}

TEST(PlatformVariantTest, AndroidIsNotLinuxEvenThoughItsKernelIs) {
    auto android = variantSpellings("android-arm64");
    EXPECT_FALSE(accepts(android, "linux-arm64")) << join(android);
    EXPECT_FALSE(accepts(android, "linux-aarch64")) << join(android);

    auto linux = variantSpellings("linux-arm64");
    EXPECT_FALSE(accepts(linux, "android-arm64")) << join(linux);
}

TEST(PlatformVariantTest, WebIsArchitectureFreeAndResolvesToItselfAlone) {
    // "web" is the one variant with no architecture half: the same bytes run
    // wherever the Web container runs. No separator means nothing to alias.
    EXPECT_EQ(variantSpellings("web"), std::vector<std::string>{ "web" });
}

TEST(PlatformVariantTest, NoNativeHostSilentlyAcceptsTheWebVariant) {
    // Web payloads need the Web container, which a bare native host does not
    // have. Selecting one here would install JS where a plugin is loaded.
    const char* const hosts[] = { "darwin-arm64", "linux-x86_64", "windows-x86_64",
                                  "android-arm64", "ios-arm64", "ios-sim-arm64" };
    for (const char* h : hosts) {
        auto variants = variantSpellings(h);
        EXPECT_FALSE(accepts(variants, "web")) << h << " -> " << join(variants);
    }
}

// =============================================================================
// The canonical vocabulary
//
// Every variant name a producer may write is one of a closed set. A name that
// is a MISSPELLING of one of them is a build that will never install anywhere,
// so it is caught at `lgx add`/`lgx verify` time with the name that was meant.
// A name that resembles nothing in the set is left alone -- private
// vocabularies exist and this library does not own them.
// =============================================================================

TEST(PlatformVariantTest, TheVocabularyCoversEveryTargetThisMilestoneShips) {
    const char* const expected[] = {
        "linux-x86_64", "linux-arm64", "darwin-x86_64", "darwin-arm64",
        "windows-x86_64", "windows-arm64",
        "android-arm64", "android-x86_64", "ios-arm64", "ios-sim-arm64", "web",
    };
    const auto& known = knownVariants();
    for (const char* e : expected) {
        EXPECT_TRUE(std::find(known.begin(), known.end(), e) != known.end())
            << e << " is missing from knownVariants(); got " << join(known);
    }
}

TEST(PlatformVariantTest, EveryKnownNameIsAcceptedAsWrittenAndSuggestsNothing) {
    for (const auto& v : knownVariants()) {
        EXPECT_TRUE(isKnownVariant(v)) << v;
        EXPECT_EQ(suggestVariantName(v), "") << v;
    }
}

TEST(PlatformVariantTest, EveryArchAliasIsAcceptedAndCanonicalises) {
    struct { const char* written; const char* canonical; } cases[] = {
        { "linux-amd64",     "linux-x86_64"   },
        { "darwin-aarch64",  "darwin-arm64"   },
        { "windows-amd64",   "windows-x86_64" },
        { "android-aarch64", "android-arm64"  },
        { "ios-aarch64",     "ios-arm64"      },
        { "ios-sim-aarch64", "ios-sim-arm64"  },
    };
    for (const auto& c : cases) {
        EXPECT_TRUE(isKnownVariant(c.written)) << c.written;
        EXPECT_EQ(canonicalVariant(c.written), c.canonical) << c.written;
        EXPECT_EQ(suggestVariantName(c.written), "") << c.written;
    }
}

TEST(PlatformVariantTest, AMisspellingIsRejectedWithTheNameThatWasMeant) {
    struct { const char* written; const char* meant; } cases[] = {
        { "ios_arm64",             "ios-arm64"      },
        { "iosarm64",              "ios-arm64"      },
        { "ios-simulator-arm64",   "ios-sim-arm64"  },
        { "iphonesimulator-arm64", "ios-sim-arm64"  },
        { "iphoneos-arm64",        "ios-arm64"      },
        { "android_arm64",         "android-arm64"  },
        { "arm64-v8a",             "android-arm64"  },
        { "wasm",                  "web"            },
        { "wasm32",                "web"            },
        { "emscripten",            "web"            },
        { "macos-arm64",           "darwin-arm64"   },
        { "osx-arm64",             "darwin-arm64"   },
        { "linux-x8664",           "linux-x86_64"   },
    };
    for (const auto& c : cases) {
        EXPECT_FALSE(isKnownVariant(c.written)) << c.written;
        EXPECT_EQ(suggestVariantName(c.written), c.meant) << c.written;
    }
}

TEST(PlatformVariantTest, ANameResemblingNothingKnownIsLeftAlone) {
    // Not every variant name in the world is ours. Only a name the vocabulary
    // recognises as a near miss is worth correcting; the rest pass through so
    // a private target keeps working.
    for (const char* v : { "test", "test-variant", "variant1", "nonexistent",
                           "linux-riscv64", "my-own-target" }) {
        EXPECT_EQ(suggestVariantName(v), "") << v;
    }
}

TEST(PlatformVariantTest, TheDevFlavourIsAcceptedOnEveryKnownName) {
    // lgpm appends "-dev" for a non-portable build, so "<known>-dev" is a name
    // real packages carry and must never be corrected away.
    for (const auto& v : knownVariants()) {
        const std::string dev = v + "-dev";
        EXPECT_TRUE(isKnownVariant(dev)) << dev;
        EXPECT_EQ(suggestVariantName(dev), "") << dev;
    }
}

TEST(PlatformVariantTest, TheDevFlavourOfAMisspellingIsCorrectedToTheDevFlavour) {
    EXPECT_EQ(suggestVariantName("ios_arm64-dev"), "ios-arm64-dev");
    EXPECT_EQ(suggestVariantName("wasm-dev"), "web-dev");
}

TEST(PlatformVariantTest, AnEmptyNameIsNeitherKnownNorCorrectable) {
    EXPECT_FALSE(isKnownVariant(""));
    EXPECT_EQ(suggestVariantName(""), "");
    EXPECT_EQ(canonicalVariant(""), "");
}

TEST(PlatformVariantTest, TheHostAlwaysNamesItselfInTheVocabulary) {
    EXPECT_TRUE(isKnownVariant(hostVariant())) << hostVariant();
}
