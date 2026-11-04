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

#include "../../include/core/log_redact.h"

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
    constexpr uint64_t kSalt = 0xC0FFEE0000000001ULL;

    std::set<std::string> tokens;
    for (const std::string s :
         {"Pretzel Logic Cafe", "Basement Lab 5G", "tin can and string", "((o))"}) {
        tokens.insert(helix::redact::ssid_with_salt(s, kSalt));
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

// ============================================================================
// telemetry_text: free text bound for anonymous telemetry (#1813)
// ============================================================================

using helix::redact::telemetry_text;

TEST_CASE("redact::telemetry_text removes paths, addresses, hosts and user names",
          "[redact][telemetry_text]") {
    SECTION("absolute and home-relative paths") {
        const std::string out = telemetry_text(
            "[ERROR] Cannot write /home/alice/printer_data/config/helixscreen.env", {}, 200);
        INFO(out);
        CHECK_FALSE(leaks(out, "alice"));
        CHECK_FALSE(leaks(out, "printer_data"));
        CHECK(out == "[ERROR] Cannot write <path>");

        CHECK(telemetry_text("missing ~/helixscreen/bin", {}, 200) == "missing <path>");
        CHECK(telemetry_text("dir=/usr/data/helixscreen: not writable", {}, 200) ==
              "dir=<path>: not writable");
        CHECK(telemetry_text("rm '/opt/helixscreen.old' failed (exit 1):", {}, 200) ==
              "rm '<path>' failed (exit 1):");
    }

    SECTION("IP literals of every scope") {
        const std::string out =
            telemetry_text("connect 192.168.1.50:7125 then 203.0.113.5 and fe80::1", {}, 200);
        INFO(out);
        CHECK_FALSE(leaks(out, "192.168.1.50"));
        CHECK_FALSE(leaks(out, "203.0.113.5"));
        CHECK_FALSE(leaks(out, "fe80::1"));
        CHECK(out == "connect <ip> then <ip> and <ip>");
    }

    SECTION("host names, URLs and user@host") {
        const std::string out = telemetry_text(
            "fetch https://mirror.example.net/a?token=x from printer.local as pi@k1max.lan", {},
            200);
        INFO(out);
        CHECK_FALSE(leaks(out, "mirror.example.net"));
        CHECK_FALSE(leaks(out, "token"));
        CHECK_FALSE(leaks(out, "printer.local"));
        CHECK_FALSE(leaks(out, "k1max"));
        CHECK(out == "fetch <url> from <host> as <user@host>");
    }

    SECTION("the device's own names, wherever they stand") {
        const std::string out = telemetry_text("user prestonb on K1C-7F3A, owner=prestonb",
                                               {"prestonb", "K1C-7F3A"}, 200);
        INFO(out);
        CHECK_FALSE(leaks(out, "prestonb"));
        CHECK_FALSE(leaks(out, "K1C-7F3A"));
        CHECK(out == "user <id> on <id>, owner=<id>");
        // A name is replaced as a word, never inside a longer one.
        CHECK(telemetry_text("pipeline ok", {"pipe"}, 200) == "pipeline ok");
    }

    SECTION("identifiers match in any case") {
        CHECK(telemetry_text("on MyPrinter and myprinter", {"myprinter"}, 200) ==
              "on <id> and <id>");
    }

    SECTION("short, generic and all-digit identifiers are ignored") {
        CHECK(telemetry_text("pi user root printer klipper admin",
                             {"pi", "user", "root", "printer", "klipper", "admin", "abc"},
                             200) == "pi user root printer klipper admin");
        // An all-digit identifier never splits an address.
        CHECK(telemetry_text("at 10.1680.1.5 and 10.0.168.1", {"1680", "168"}, 200) ==
              "at 10.1680.1.5 and <ip>");
    }

    SECTION("host:port and ip:port drop everything after the address") {
        CHECK(telemetry_text("reach printer.local:7125 failed", {}, 200) == "reach <host> failed");
        const std::string out =
            telemetry_text("GET 192.168.1.50:7125/printer/info?api_key=abc123 401", {}, 200);
        INFO(out);
        CHECK_FALSE(leaks(out, "abc123"));
        CHECK(out == "GET <ip> 401");
    }

    SECTION("scheme-less URLs") {
        const std::string out = telemetry_text("from github.com/foo/bar?token=abc", {}, 200);
        INFO(out);
        CHECK_FALSE(leaks(out, "abc"));
        CHECK(out == "from <path>");
    }

    SECTION("paths after any punctuation") {
        for (const char* shape :
             {"a,/home/alice/x", "->/home/alice/x", "-o/home/alice/x", "~alice/x"}) {
            const std::string out = telemetry_text(std::string("see ") + shape, {}, 200);
            INFO(shape << " -> " << out);
            CHECK_FALSE(leaks(out, "alice"));
            CHECK(out == "see <path>");
        }
        CHECK(telemetry_text("see </home/alice/x>", {}, 200) == "see <<path>>");
    }

    SECTION("credential values") {
        for (const char* shape : {"pass=hunter2", "password=hunter2", "token=hunter2",
                                  "api_key=hunter2", "secret:hunter2", "auth=hunter2",
                                  "Authorization: Bearer hunter2", "PASSWORD= hunter2"}) {
            const std::string out = telemetry_text(shape, {}, 200);
            INFO(shape << " -> " << out);
            CHECK_FALSE(leaks(out, "hunter2"));
            CHECK(out.find("<redacted>") != std::string::npos);
        }
    }

    SECTION("MAC addresses") {
        const std::string out =
            telemetry_text("wlan0 aa:bb:cc:dd:ee:0f and AA-BB-CC-DD-EE-0F", {}, 200);
        INFO(out);
        CHECK(out == "wlan0 <mac> and <mac>");
    }

    SECTION("diagnostic words survive") {
        CHECK(telemetry_text("install.sh failed (exit 1): settings.json missing", {}, 200) ==
              "install.sh failed (exit 1): settings.json missing");
        CHECK(telemetry_text("[2/6] Downloaded ... FAILED (interrupted)", {}, 200) ==
              "[2/6] Downloaded ... FAILED (interrupted)");
        CHECK(telemetry_text("need 120MB, have 80MB on v1.0.3", {}, 200) ==
              "need 120MB, have 80MB on v1.0.3");
    }
}

TEST_CASE("redact::telemetry_text caps the length without splitting a character",
          "[redact][telemetry_text]") {
    CHECK(telemetry_text("abcdefghij", {}, 4) == "abcd");
    // "é" is two bytes; a cut through it backs off to the character before.
    CHECK(telemetry_text("abc\xC3\xA9xyz", {}, 4) == "abc");
    // A placeholder is never left half-written.
    CHECK(telemetry_text("bad /etc/x", {}, 7) == "bad ");
    // A literal '<' that begins no placeholder is text, not a cut point.
    CHECK(telemetry_text("need <100MB free", {}, 9) == "need <100");
    CHECK(telemetry_text("", {}, 10).empty());
}
