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

// =============================================================================
// IP addresses (#1352)
//
// Fixtures for the routable side are drawn from the RFC5737 / RFC3849
// documentation ranges, for the same reason the SSIDs above are invented: a
// real routable address committed to a public repo is somebody's house.
// =============================================================================

using helix::redact::ip_scope;
using helix::redact::ips_in_text;
using helix::redact::IpScope;

TEST_CASE("redact::ip_scope keeps private, loopback, link-local and CGNAT IPv4",
          "[redact][privacy][1352]") {
    const char* local[] = {
        "192.168.1.100",   "192.168.0.1", "10.0.0.5",    "10.255.255.254",  "172.16.0.1",
        "172.31.255.254",  "127.0.0.1",   "127.0.0.53",  "169.254.10.20",   "100.64.0.1",
        "100.127.255.255", "0.0.0.0",     "224.0.0.251", "255.255.255.255",
    };
    for (const char* a : local) {
        INFO(a);
        REQUIRE(ip_scope(a) == IpScope::Local);
    }
}

TEST_CASE("redact::ip_scope flags routable IPv4, including just outside each private block",
          "[redact][privacy][1352]") {
    const char* global[] = {
        "203.0.113.5",  // TEST-NET-3
        "198.51.100.7", // TEST-NET-2
        "192.0.2.1",    // TEST-NET-1
        "172.15.0.1",   // one below 172.16/12
        "172.32.0.1",   // one above 172.16/12
        "192.169.0.1",  // one above 192.168/16
        "100.63.0.1",   // one below 100.64/10
        "100.128.0.1",  // one above 100.64/10
        "9.255.255.255", "11.0.0.1", "126.0.0.1", "128.0.0.1",
    };
    for (const char* a : global) {
        INFO(a);
        REQUIRE(ip_scope(a) == IpScope::Global);
    }
}

TEST_CASE("redact::ip_scope keeps loopback, link-local, ULA and multicast IPv6",
          "[redact][privacy][1352]") {
    const char* local[] = {
        "::1",
        "::",
        "fe80::1",
        "fe80::dead:beef:1:2",
        "FE80::1",
        "febf::1", // top of fe80::/10
        "fc00::1",
        "fd12:3456:789a::1",
        "fdff:ffff::1", // top of fc00::/7
        "ff02::fb",     // mDNS
        "::ffff:192.168.1.100",
    };
    for (const char* a : local) {
        INFO(a);
        REQUIRE(ip_scope(a) == IpScope::Local);
    }
}

TEST_CASE("redact::ip_scope flags routable IPv6, including just outside each local block",
          "[redact][privacy][1352]") {
    const char* global[] = {
        "2001:db8::1",                  // RFC3849 documentation prefix
        "2001:db8:85a3::8a2e:370:7334", //
        "2001:0db8:0000:0000:0000:0000:0000:0001",
        "fbff::1",            // one below fc00::/7
        "fe00::1",            // below fe80::/10
        "fec0::1",            // deprecated site-local, above fe80::/10
        "::ffff:203.0.113.5", // IPv4-mapped routable
    };
    for (const char* a : global) {
        INFO(a);
        REQUIRE(ip_scope(a) == IpScope::Global);
    }
}

TEST_CASE("redact::ip_scope rejects things that merely look like addresses",
          "[redact][privacy][1352]") {
    const char* not_ip[] = {
        "",
        "hello world",
        "1.2.3",             // three groups
        "1.2.3.4.5",         // five groups
        "256.1.1.1",         // octet out of range
        "192.168.001.1",     // leading zeros
        "0.99.115",          // a HelixScreen version
        "2026.08.25.1",      // a date
        "aa:bb:cc:dd:ee:ff", // a MAC, not a six-group IPv6
        "12:34:56.789",      // a log timestamp
        "1:2:3:4:5:6:7:8:9", // nine groups
        "2001:db8::1::2",    // two "::"
        "gggg::1",           // not hex
    };
    for (const char* a : not_ip) {
        INFO(a);
        REQUIRE(ip_scope(a) == IpScope::NotAnIp);
    }
}

TEST_CASE("redact::ip leaves private addresses alone and tokenises routable ones",
          "[redact][privacy][1352]") {
    REQUIRE(helix::redact::ip("192.168.1.100") == "192.168.1.100");
    REQUIRE(helix::redact::ip("fe80::1") == "fe80::1");
    REQUIRE(helix::redact::ip("not an address") == "not an address");

    const std::string v4 = helix::redact::ip("203.0.113.5");
    INFO("v4=" << v4);
    REQUIRE_FALSE(leaks(v4, "203.0.113.5"));
    REQUIRE(v4.rfind("ip#", 0) == 0);

    const std::string v6 = helix::redact::ip("2001:db8:85a3::8a2e:370:7334");
    INFO("v6=" << v6);
    REQUIRE_FALSE(leaks(v6, "2001:db8:85a3::8a2e:370:7334"));
    REQUIRE_FALSE(leaks(v6, "8a2e:370:7334"));
    REQUIRE(v6.rfind("ip#", 0) == 0);
}

TEST_CASE("redact::ips_in_text keeps every local address embedded in a line",
          "[redact][privacy][1352]") {
    const char* unchanged[] = {
        "[WS] connecting to 192.168.1.50:7125",
        "trusted_clients = 192.168.1.0/24, 10.0.0.0/8",
        "bound 127.0.0.1:7125",
        "iface wlan0 addr fe80::1%wlan0 scope link",
        "ula fd12:3456:789a::1 up",
        "avahi joined ff02::fb",
        "no lease, fell back to 169.254.10.20",
    };
    for (const char* line : unchanged) {
        INFO(line);
        REQUIRE(ips_in_text(line) == line);
    }
}

TEST_CASE("redact::ips_in_text does not mangle version strings or dotted-quad lookalikes",
          "[redact][privacy][1352]") {
    // The whole reason this is a scanner and not a "four numbers with dots"
    // regex. A false positive here silently destroys a bundle's version data.
    const char* unchanged[] = {
        "HelixScreen 0.99.115 (LVGL 9.5.0)",
        "klipper v0.12.0-266-g7ed3e3e5",
        "firmware v1.2.3.4 loaded",
        "sequence 1.2.3.4.5 rejected",
        "snapshot 2026.08.25.1",
        "moonraker 0.9.3 on port 7125",
        "took 3.14159 seconds",
        "[10:04:30.373] [debug] nothing sensitive here",
    };
    for (const char* line : unchanged) {
        INFO(line);
        REQUIRE(ips_in_text(line) == line);
    }
}

TEST_CASE("redact::ips_in_text replaces routable addresses and keeps what surrounds them",
          "[redact][privacy][1352]") {
    const std::string v4 = ips_in_text("GET http://203.0.113.5:7125/printer/info failed");
    INFO("v4=" << v4);
    REQUIRE_FALSE(leaks(v4, "203.0.113.5"));
    REQUIRE(v4.find("ip#") != std::string::npos);
    REQUIRE(v4.find(":7125/printer/info failed") != std::string::npos);
    REQUIRE(v4.find("GET http://") == 0);

    // A sentence-final period is punctuation, not a fifth octet.
    const std::string dot = ips_in_text("reached 198.51.100.7.");
    INFO("dot=" << dot);
    REQUIRE_FALSE(leaks(dot, "198.51.100.7"));
    REQUIRE(dot.back() == '.');

    const std::string v6 = ips_in_text("wlan0 inet6 2001:db8:85a3::8a2e:370:7334/64 scope global");
    INFO("v6=" << v6);
    REQUIRE_FALSE(leaks(v6, "2001:db8:85a3::8a2e:370:7334"));
    REQUIRE(v6.find("ip#") != std::string::npos);
    REQUIRE(v6.find("/64 scope global") != std::string::npos);

    // A line carrying both: the private one survives, the routable one does not.
    const std::string both = ips_in_text("lan 192.168.1.50 wan 203.0.113.5");
    INFO("both=" << both);
    REQUIRE(both.find("192.168.1.50") != std::string::npos);
    REQUIRE_FALSE(leaks(both, "203.0.113.5"));
}

TEST_CASE("redact::ips_in_text tokens are stable within a line and distinguish hosts",
          "[redact][1352]") {
    const std::string same = ips_in_text("203.0.113.5 -> 203.0.113.5");
    const size_t first = same.find("ip#");
    REQUIRE(first != std::string::npos);
    const std::string tok = same.substr(first, 9);
    REQUIRE(same.find(tok, first + 1) != std::string::npos);

    REQUIRE(helix::redact::ip("203.0.113.5") != helix::redact::ip("203.0.113.6"));

    // Separate hash domain from mac(), so a bundle reader cannot notice that
    // some address and some hardware address share bytes. Compare the digests,
    // not the whole tokens -- the prefixes differ trivially.
    const std::string as_ip = helix::redact::ip_with_salt("203.0.113.5", 99);
    const std::string as_mac = helix::redact::mac_with_salt("203.0.113.5", 99);
    REQUIRE(as_ip.substr(3) != as_mac.substr(4));
}
