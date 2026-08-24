// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_log_redact.cpp
 * @brief Unit tests for helix::redact — SSID/MAC tokens for log output.
 *
 * The property under test is containment: nothing derived from the input may
 * appear in the output, because the failure mode is a substring surviving
 * rather than a crash.
 *
 * Fixtures are invented, deliberately. Testing a privacy helper against network
 * names copied out of somebody's bug report would put those names in a public
 * repository forever — which is the exact disclosure this file exists to
 * prevent. They still cover the shapes that matter: embedded spaces, ASCII
 * punctuation, a vendor-style name with a hex suffix, and non-ASCII bytes.
 */

#include "../../include/log_redact.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::redact::mac;
using helix::redact::ssid;

namespace {

// Lowercase a copy so containment checks are case-insensitive — a redactor
// that merely changed case would otherwise pass.
std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool leaks(const std::string& token, const std::string& secret) {
    return lower(token).find(lower(secret)) != std::string::npos;
}

} // namespace

// =============================================================================
// The core property: the input must not survive into the output
// =============================================================================

TEST_CASE("redact::ssid does not leak the network name", "[redact][privacy]") {
    // Shapes a real scan produces: spaces, punctuation, a vendor default with a
    // hex suffix. Invented names -- see the file comment.
    const std::vector<std::string> sample_ssids = {
        "Pretzel Logic Cafe",        "((o))", "Basement Lab 5G", "tin can and string",
        "DIRECT-xy-ExamplePrn-1A2B",
    };

    for (const auto& s : sample_ssids) {
        const std::string token = ssid(s);
        INFO("ssid=" << s << " token=" << token);
        REQUIRE_FALSE(leaks(token, s));
    }
}

TEST_CASE("redact::ssid does not leak any word of a multi-word name", "[redact][privacy]") {
    // A redactor that kept "the first word" or "the last word" would pass a
    // whole-string containment check. Pin each word separately.
    const std::string token = ssid("Pretzel Logic Cafe");
    for (const std::string word : {"pretzel", "logic", "cafe"}) {
        INFO("word=" << word << " token=" << token);
        REQUIRE_FALSE(leaks(token, word));
    }
}

TEST_CASE("redact::mac does not leak the hardware address", "[redact][privacy]") {
    const std::vector<std::string> addrs = {
        "a4:83:e7:1f:2b:9c", // adapter MAC shape
        "A4-83-E7-1F-2B-9C", // dash-separated
        "3c:37:86:04:d1:88", // BSSID shape
    };

    for (const auto& m : addrs) {
        const std::string token = mac(m);
        INFO("mac=" << m << " token=" << token);
        REQUIRE_FALSE(leaks(token, m));
        // Nothing longer than a single byte of the address survives. Checking
        // individual octets would be wrong, not strict: the token is a short
        // hex digest, so it contains some two-hex-digit pair by construction --
        // an "a4" check tripped on "mac#e4a4" with nothing actually leaked.
        // The structural check below is the real guarantee.
        REQUIRE_FALSE(leaks(token, m.substr(0, 5))); // "a4:83"
        REQUIRE_FALSE(leaks(token, m.substr(12)));   // ":9c" tail
    }
}

TEST_CASE("redact::mac emits a fixed-width token that cannot embed an address",
          "[redact][privacy]") {
    // Structural guarantee behind the containment checks above: the output is
    // always "mac#" plus exactly six hex digits, so no six-octet address can
    // fit inside it regardless of input or salt.
    for (const std::string& m : {"a4:83:e7:1f:2b:9c", "ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00"}) {
        const std::string token = mac(m);
        INFO("token=" << token);
        REQUIRE(token.size() == 10);
        REQUIRE(token.rfind("mac#", 0) == 0);
        REQUIRE(token.find_first_not_of("0123456789abcdef", 4) == std::string::npos);
    }
}

// =============================================================================
// Usefulness: tokens must still correlate within a session
// =============================================================================

TEST_CASE("redact::ssid is stable within a process", "[redact]") {
    // This is the whole reason for hashing rather than dropping the field:
    // "connect attempt to X failed, then X succeeded" must remain readable.
    REQUIRE(ssid("Pretzel Logic Cafe") == ssid("Pretzel Logic Cafe"));
    REQUIRE(mac("a4:83:e7:1f:2b:9c") == mac("a4:83:e7:1f:2b:9c"));
}

TEST_CASE("redact::ssid distinguishes different networks", "[redact]") {
    // Fixed salt so the assertion is deterministic rather than depending on
    // this boot's random value.
    constexpr uint64_t SALT = 0xC0FFEE0000000001ULL;

    std::set<std::string> tokens;
    for (const std::string s :
         {"Pretzel Logic Cafe", "Basement Lab 5G", "tin can and string", "((o))"}) {
        tokens.insert(helix::redact::ssid_with_salt(s, SALT));
    }
    // A redactor returning a constant would leak nothing but tell you nothing.
    REQUIRE(tokens.size() == 4);
}

TEST_CASE("redact::ssid separates the ssid and mac namespaces", "[redact]") {
    // Same bytes through both helpers must not collide — otherwise a bundle
    // reader could tell that some AP's BSSID equals some other field.
    REQUIRE(ssid("aa:bb:cc:dd:ee:ff") != mac("aa:bb:cc:dd:ee:ff"));
}

// =============================================================================
// Shape and edge cases
// =============================================================================

TEST_CASE("redact::ssid handles an empty name", "[redact]") {
    REQUIRE(ssid("") == "net#<none>");
    REQUIRE(mac("") == "mac#<none>");
}

TEST_CASE("redact tokens are short and bounded", "[redact]") {
    // These land in a 2000-line ring on a 473MB device; they must not bloat it.
    const std::string long_ssid(4096, 'x');
    REQUIRE(ssid(long_ssid).size() <= 16);
    REQUIRE(ssid("Pretzel Logic Cafe").size() <= 16);
}

TEST_CASE("redact tokens are prefixed so bundle readers know what was dropped", "[redact]") {
    REQUIRE(ssid("Pretzel Logic Cafe").rfind("net#", 0) == 0);
    REQUIRE(mac("a4:83:e7:1f:2b:9c").rfind("mac#", 0) == 0);
}

TEST_CASE("redact::ssid handles non-ASCII and control bytes", "[redact][privacy]") {
    // SSIDs are arbitrary octets, not UTF-8. A redactor that passed bytes
    // through on a decode failure would leak.
    const std::string emoji_ssid = "\xf0\x9f\x8f\xa0 Casa";
    const std::string token = ssid(emoji_ssid);
    REQUIRE_FALSE(leaks(token, "Casa"));
    REQUIRE(token.rfind("net#", 0) == 0);
}

// =============================================================================
// Salt behaviour
// =============================================================================

TEST_CASE("redact::ssid changes token when the salt changes", "[redact][privacy]") {
    // Per-boot salting is what stops tokens being compared across sessions or
    // matched against a precomputed table of common SSIDs.
    using helix::redact::ssid_with_salt;

    const std::string a = ssid_with_salt("Pretzel Logic Cafe", 0x1111111111111111ULL);
    const std::string b = ssid_with_salt("Pretzel Logic Cafe", 0x2222222222222222ULL);
    REQUIRE(a != b);

    // Same salt, same token — proves the salt is the only varying input, i.e.
    // the hash itself is deterministic.
    REQUIRE(ssid_with_salt("Pretzel Logic Cafe", 0x1111111111111111ULL) == a);
}
