// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/release_signature.h"

#include "monocypher-ed25519.h"
#include "monocypher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::release_signature {

namespace {

// minisign's binary layouts: a public key is algorithm(2) + key id(8) + Ed25519 key(32); a
// signature line is algorithm(2) + key id(8) + Ed25519 signature(64).
constexpr size_t kAlgLen = 2;
constexpr size_t kKeyIdLen = 8;
constexpr size_t kEdKeyLen = 32;
constexpr size_t kEdSigLen = 64;
constexpr size_t kPubKeyLen = kAlgLen + kKeyIdLen + kEdKeyLen;
constexpr size_t kSigLen = kAlgLen + kKeyIdLen + kEdSigLen;
constexpr size_t kPrehashLen = 64; // BLAKE2b-512

constexpr std::string_view kUntrustedPrefix = "untrusted comment: ";
constexpr std::string_view kTrustedPrefix = "trusted comment: ";

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

// Strict padded base64. Local rather than hv::Base64Decode because the ESP32 image compiles
// this file and has no libhv.
std::optional<std::vector<uint8_t>> b64_decode(std::string_view in) {
    if (in.empty() || in.size() % 4 != 0)
        return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=' && last && k >= 2) {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad > 0)
                return std::nullopt; // data after padding
            v[k] = b64_value(c);
            if (v[k] < 0)
                return std::nullopt;
        }
        const uint32_t n = (static_cast<uint32_t>(v[0]) << 18) |
                           (static_cast<uint32_t>(v[1]) << 12) |
                           (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2)
            out.push_back(static_cast<uint8_t>(n >> 8));
        if (pad < 1)
            out.push_back(static_cast<uint8_t>(n));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() &&
           (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// Lines of @p text without their '\n' or "\r\n" terminators.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return lines;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

// The key line of a .pub file's text, or the text itself when it is a bare key line.
std::string_view public_key_line(std::string_view text) {
    const auto lines = split_lines(trim(text));
    if (lines.size() >= 2 && starts_with(lines[0], kUntrustedPrefix))
        return trim(lines[1]);
    return lines.size() == 1 ? trim(lines[0]) : std::string_view{};
}

} // namespace

Verdict verify(std::string_view message, std::string_view minisig, std::string_view public_key) {
    const auto key = b64_decode(public_key_line(public_key));
    if (!key || key->size() != kPubKeyLen || (*key)[0] != 'E' || (*key)[1] != 'd')
        return Verdict::MalformedKey;

    const auto lines = split_lines(trim(minisig));
    if (lines.size() != 4 || !starts_with(lines[0], kUntrustedPrefix) ||
        !starts_with(lines[2], kTrustedPrefix))
        return Verdict::MalformedSignature;
    const auto sig = b64_decode(trim(lines[1]));
    const auto global_sig = b64_decode(trim(lines[3]));
    if (!sig || sig->size() != kSigLen || !global_sig || global_sig->size() != kEdSigLen)
        return Verdict::MalformedSignature;

    if ((*sig)[0] == 'E' && (*sig)[1] == 'd')
        return Verdict::LegacySignature;
    if ((*sig)[0] != 'E' || (*sig)[1] != 'D')
        return Verdict::MalformedSignature;

    for (size_t i = 0; i < kKeyIdLen; ++i) {
        if ((*sig)[kAlgLen + i] != (*key)[kAlgLen + i])
            return Verdict::WrongKey;
    }

    const uint8_t* ed_key = key->data() + kAlgLen + kKeyIdLen;
    const uint8_t* ed_sig = sig->data() + kAlgLen + kKeyIdLen;

    std::array<uint8_t, kPrehashLen> digest{};
    crypto_blake2b(digest.data(), digest.size(), reinterpret_cast<const uint8_t*>(message.data()),
                   message.size());
    if (crypto_ed25519_check(ed_sig, ed_key, digest.data(), digest.size()) != 0)
        return Verdict::BadSignature;

    // The global signature covers the file signature followed by the trusted comment's text.
    const std::string_view comment = lines[2].substr(kTrustedPrefix.size());
    std::vector<uint8_t> signed_comment(ed_sig, ed_sig + kEdSigLen);
    signed_comment.insert(signed_comment.end(), comment.begin(), comment.end());
    if (crypto_ed25519_check(global_sig->data(), ed_key, signed_comment.data(),
                             signed_comment.size()) != 0)
        return Verdict::BadSignature;

    return Verdict::Ok;
}

const char* describe(Verdict v) {
    switch (v) {
    case Verdict::Ok:
        return "signature verified";
    case Verdict::MalformedKey:
        return "the release public key is malformed";
    case Verdict::MalformedSignature:
        return "the signature file is malformed";
    case Verdict::LegacySignature:
        return "the signature is not prehashed (legacy minisign)";
    case Verdict::WrongKey:
        return "signed with a key other than the release key";
    case Verdict::BadSignature:
        return "the signature does not match the manifest";
    }
    return "unknown signature verdict";
}

} // namespace helix::release_signature
