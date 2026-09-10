#pragma once

#include <string>
#include <vector>

namespace lgx {

// Variant NAMES: which spellings of "<os>-<architecture>" name the same thing,
// which ones are the vocabulary, and which one this build is.
//
// A variant name is not free text -- it is a key inside the signed hash tree
// (hashes["variants/<name>"]), so this library is where the vocabulary belongs
// and every consumer reads it from here rather than keeping its own.

/**
 * The variant this build of the library targets, e.g. "darwin-arm64".
 *
 * Compile-time, from the predefined macros: it describes the machine the
 * binary runs on and reads nothing. "unknown" for a target with no rule here.
 * Any other answer is a name knownVariants() contains.
 */
std::string hostVariant();

/**
 * `variant` first, then every other live spelling of its ARCHITECTURE half:
 * the list a consumer walks when looking for its own variant in a package.
 *
 * Empty in, empty out. An architecture no row knows degrades to the input
 * alone, which is the pre-alias behaviour and still fail-closed.
 */
std::vector<std::string> variantSpellings(const std::string& variant);

/**
 * The canonical name of every target this ecosystem ships, desktop, mobile
 * and web, in no particular order.
 *
 * These are the names a producer should write. The other accepted spellings
 * (the architecture aliases) are legacy and canonicalVariant() maps them here.
 */
const std::vector<std::string>& knownVariants();

/**
 * Whether this vocabulary vouches for `variant` as written: a canonical name,
 * one of its architecture aliases, or either with the "-dev" flavour suffix a
 * non-portable build appends.
 *
 * False is NOT "invalid" -- a private target this library has never heard of
 * is also false. It means "this library cannot confirm the name", which is
 * only actionable when suggestVariantName() has a correction to offer.
 */
bool isKnownVariant(const std::string& variant);

/**
 * The canonical spelling of a known name, preserving any "-dev" suffix;
 * empty when the name is not one isKnownVariant() vouches for.
 */
std::string canonicalVariant(const std::string& variant);

/**
 * The canonical name `variant` was probably meant to be, or empty.
 *
 * Empty means "nothing to say": either the name is already accepted, or it
 * resembles nothing in the vocabulary and so belongs to someone else. A
 * non-empty answer is a name that will NEVER resolve on any host -- a
 * misspelling caught at `lgx add` / `lgx verify` time instead of at install
 * time on a user's phone.
 */
std::string suggestVariantName(const std::string& variant);

} // namespace lgx
