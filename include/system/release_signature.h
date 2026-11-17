// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

/// Checks minisign signatures on release manifests (docs/devel/RELEASE_SIGNING.md).
///
/// Only prehashed signatures (`minisign -S -H`, algorithm "ED": Ed25519 over the BLAKE2b-512
/// of the file) are accepted; a legacy signature over the raw file is refused, as the release
/// workflow and the easy installer refuse it. The trusted comment's global signature is checked
/// too, so a comment cannot be swapped onto someone else's signature.
namespace helix::release_signature {

/// The base64 key line of `.github/release-minisign.pub`, the public half of the offline
/// release key the release workflow signs every channel manifest with. A test pins the two
/// equal.
inline constexpr std::string_view kReleasePublicKey =
    "RWRXImmr5YYfqd5aaV48WN+50k5e83NgKuEtxECC6agLUBkXG7t7AH9S";

enum class Verdict {
    Ok,
    MalformedKey,       ///< the public key is not a minisign Ed25519 key
    MalformedSignature, ///< not the four-line .minisig layout, or bad base64 or lengths
    LegacySignature,    ///< signed without -H; refused
    WrongKey,           ///< signed by a key other than the one given
    BadSignature,       ///< the message or the trusted comment does not match the signature
};

/// Whether @p minisig (the text of a .minisig file) is a valid prehashed signature over
/// @p message by @p public_key (the base64 key line of a minisign .pub file, or the whole
/// file's text).
Verdict verify(std::string_view message, std::string_view minisig, std::string_view public_key);

/// A short English description of @p v for logs and error text.
const char* describe(Verdict v);

} // namespace helix::release_signature
