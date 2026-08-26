// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log_redact.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace helix::redact {

namespace {

// Domain separators, so the same bytes hashed as an SSID and as a MAC produce
// different tokens. Without this a bundle reader could tell that some AP's
// BSSID matched some other field, which is itself a disclosure.
constexpr uint64_t SSID_DOMAIN = 0x5353'4944'0000'0001ULL;
constexpr uint64_t MAC_DOMAIN = 0x4D41'4300'0000'0002ULL;
constexpr uint64_t IP_DOMAIN = 0x4950'0000'0000'0003ULL;

// Per-boot salt, resolved once on first use. Regenerated every launch so
// tokens cannot be correlated across sessions, and so a precomputed table of
// common SSIDs ("linksys", "NETGEAR", the ISP default patterns) cannot be
// matched against a bundle. Thread-safe: the wifi backend calls this from its
// own thread, and C++11 guarantees the static initializer runs exactly once.
uint64_t boot_salt() {
    static const uint64_t value = [] {
        uint64_t v = 0;
        try {
            std::random_device rd;
            v = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
        } catch (const std::exception&) {
            v = 0; // fall through to the clock below
        }
        if (v == 0) {
            // Some embedded libcs ship a std::random_device that returns a
            // constant. A predictable salt is still far better than none, but
            // never leave it zero.
            v = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) |
                1ULL;
        }
        return v;
    }();
    return value;
}

// FNV-1a, salted through the offset basis. Not cryptographic — it does not
// need to be. The salt is random per boot and never logged, so without it the
// truncated digest we emit is not invertible; with it, the only thing
// recoverable is which lines refer to the same network, which is the point.
uint64_t hash_with(std::string_view data, uint64_t domain, uint64_t salt) {
    uint64_t h = (salt ^ domain) ^ 0xcbf2'9ce4'8422'2325ULL;
    for (unsigned char c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x0000'0100'0000'01b3ULL;
    }
    return h;
}

std::string token(std::string_view value, const char* prefix, uint64_t domain, uint64_t salt) {
    if (value.empty()) {
        return std::string(prefix) + "<none>";
    }
    // 24 bits distinguishes the handful of APs in one scan with a collision
    // chance around 1 in 400,000, while keeping the ring compact on a 473MB
    // device. 16 bits was small enough that a token could incidentally contain
    // a two-hex-digit fragment of its own input.
    const auto digest = static_cast<unsigned>(hash_with(value, domain, salt) & 0xFF'FFFFULL);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06x", digest);
    return std::string(prefix) + buf;
}

// =============================================================================
// IP address classification
//
// Hand-rolled rather than inet_pton() so the same translation unit still builds
// for the splash and watchdog binaries without dragging in a socket header, and
// hand-rolled rather than std::regex because the debug bundle runs this over
// every line of a multi-megabyte log tail.
// =============================================================================

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool is_word_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '-';
}

// One decimal octet. Leading zeros are rejected: nothing writes an address that
// way, some resolvers read "010" as octal, and allowing them turns date-shaped
// text ("2026.08.25.1") into a false positive.
bool parse_octet(std::string_view s, uint8_t& out) {
    if (s.empty() || s.size() > 3)
        return false;
    if (s.size() > 1 && s[0] == '0')
        return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v > 255)
        return false;
    out = static_cast<uint8_t>(v);
    return true;
}

// Whole-string dotted quad. A fifth group or a trailing letter fails, because
// the last octet is parsed from everything after the third dot.
bool parse_ipv4(std::string_view s, uint8_t out[4]) {
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t dot = s.find('.', start);
        if (i < 3 && dot == std::string_view::npos)
            return false;
        const std::string_view part = (i == 3) ? s.substr(start) : s.substr(start, dot - start);
        if (!parse_octet(part, out[i]))
            return false;
        start = dot + 1;
    }
    return true;
}

// Colon-separated hex groups, optionally ending in an embedded dotted quad.
// Returns bytes written, or -1 on error.
int emit_groups(std::string_view part, uint8_t* dst) {
    if (part.empty())
        return 0;
    int written = 0;
    size_t start = 0;
    for (;;) {
        const size_t colon = part.find(':', start);
        const std::string_view g = (colon == std::string_view::npos)
                                       ? part.substr(start)
                                       : part.substr(start, colon - start);
        if (g.empty())
            return -1;
        if (g.find('.') != std::string_view::npos) {
            if (colon != std::string_view::npos)
                return -1; // an IPv4 tail may only be the final group
            if (written + 4 > 16)
                return -1;
            uint8_t quad[4];
            if (!parse_ipv4(g, quad))
                return -1;
            std::memcpy(dst + written, quad, 4);
            return written + 4;
        }
        if (g.size() > 4)
            return -1;
        uint16_t v = 0;
        for (char c : g) {
            const int d = hex_value(c);
            if (d < 0)
                return -1;
            v = static_cast<uint16_t>(v * 16 + d);
        }
        if (written + 2 > 16)
            return -1;
        dst[written++] = static_cast<uint8_t>(v >> 8);
        dst[written++] = static_cast<uint8_t>(v & 0xFF);
        if (colon == std::string_view::npos)
            return written;
        start = colon + 1;
    }
}

bool parse_ipv6(std::string_view s, uint8_t out[16]) {
    if (s.size() < 2 || s.find(':') == std::string_view::npos)
        return false;
    if (s.find(":::") != std::string_view::npos)
        return false;
    const size_t dc = s.find("::");
    if (dc != std::string_view::npos && s.find("::", dc + 1) != std::string_view::npos)
        return false;
    // A leading or trailing ':' is legal only as half of a "::".
    if (s.front() == ':' && s[1] != ':')
        return false;
    if (s.back() == ':' && s[s.size() - 2] != ':')
        return false;

    uint8_t head[16];
    uint8_t tail[16];
    const int head_len = emit_groups(dc == std::string_view::npos ? s : s.substr(0, dc), head);
    if (head_len < 0)
        return false;
    const int tail_len = (dc == std::string_view::npos) ? 0 : emit_groups(s.substr(dc + 2), tail);
    if (tail_len < 0)
        return false;

    if (dc == std::string_view::npos) {
        if (head_len != 16)
            return false;
        std::memcpy(out, head, 16);
        return true;
    }
    // "::" stands for at least one elided all-zero group.
    if (head_len + tail_len > 14)
        return false;
    std::memset(out, 0, 16);
    std::memcpy(out, head, static_cast<size_t>(head_len));
    std::memcpy(out + 16 - tail_len, tail, static_cast<size_t>(tail_len));
    return true;
}

IpScope scope_v4(const uint8_t b[4]) {
    if (b[0] == 0) // 0.0.0.0/8 "this network"
        return IpScope::Local;
    if (b[0] == 10) // RFC1918
        return IpScope::Local;
    if (b[0] == 127) // loopback
        return IpScope::Local;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) // 172.16/12
        return IpScope::Local;
    if (b[0] == 192 && b[1] == 168) // RFC1918
        return IpScope::Local;
    if (b[0] == 169 && b[1] == 254) // link-local
        return IpScope::Local;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) // CGNAT 100.64/10
        return IpScope::Local;
    if (b[0] >= 224) // multicast, reserved, broadcast
        return IpScope::Local;
    return IpScope::Global;
}

IpScope scope_v6(const uint8_t b[16]) {
    bool zero_prefix = true;
    for (int i = 0; i < 10; ++i) {
        if (b[i] != 0)
            zero_prefix = false;
    }
    if (zero_prefix) {
        // ::ffff:a.b.c.d (mapped) and ::a.b.c.d (compatible) are exactly as
        // sensitive as the IPv4 they carry; :: and ::1 are local.
        if (b[10] == 0xFF && b[11] == 0xFF)
            return scope_v4(b + 12);
        if (b[10] == 0 && b[11] == 0) {
            if (b[12] == 0 && b[13] == 0 && b[14] == 0)
                return IpScope::Local; // :: and ::1
            return scope_v4(b + 12);
        }
    }
    if ((b[0] & 0xFE) == 0xFC) // fc00::/7 unique local
        return IpScope::Local;
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) // fe80::/10 link-local
        return IpScope::Local;
    if (b[0] == 0xFF) // ff00::/8 multicast
        return IpScope::Local;
    return IpScope::Global;
}

// Candidate character classes for the free-text scanners. IPv6 candidates are
// maximal runs of hex/colon/dot; IPv4 candidates are maximal runs of digit/dot.
bool is_v6_char(char c) {
    return hex_value(c) >= 0 || c == ':' || c == '.';
}

bool is_v4_char(char c) {
    return (c >= '0' && c <= '9') || c == '.';
}

// Replace every globally routable IPv6 literal. Runs before the IPv4 pass so a
// mapped form (::ffff:203.0.113.5) is consumed whole instead of being
// half-matched on its dotted tail.
std::string redact_v6(std::string_view text, uint64_t salt) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (!is_v6_char(text[i])) {
            out += text[i];
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < n && is_v6_char(text[i]))
            ++i;
        std::string_view run = text.substr(start, i - start);
        const bool left_ok = start == 0 || !is_word_char(text[start - 1]);
        // A trailing '.' is sentence punctuation, never part of an address.
        while (!run.empty() && run.back() == '.')
            run.remove_suffix(1);
        uint8_t bytes[16];
        if (left_ok && run.find(':') != std::string_view::npos && parse_ipv6(run, bytes) &&
            scope_v6(bytes) == IpScope::Global) {
            out += token(run, "ip#", IP_DOMAIN, salt);
            out.append(text.substr(start + run.size(), i - start - run.size()));
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

std::string redact_v4(std::string_view text, uint64_t salt) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        if (!is_v4_char(text[i])) {
            out += text[i];
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < n && is_v4_char(text[i]))
            ++i;
        std::string_view run = text.substr(start, i - start);
        // A version glued to a word is not an address: "v1.2.3.4" and
        // "1.2.3.4-rc1" are rejected on their boundaries, "1.2.3.4.5" fails to
        // parse. The right edge is tested against the character after the whole
        // digit/dot run, so a sentence-final "1.2.3.4." still matches.
        const bool left_ok = start == 0 || !is_word_char(text[start - 1]);
        const bool right_ok = i >= n || !is_word_char(text[i]);
        while (!run.empty() && run.back() == '.')
            run.remove_suffix(1);
        uint8_t quad[4];
        if (left_ok && right_ok && parse_ipv4(run, quad) && scope_v4(quad) == IpScope::Global) {
            out += token(run, "ip#", IP_DOMAIN, salt);
            out.append(text.substr(start + run.size(), i - start - run.size()));
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

} // namespace

std::string ssid(std::string_view ssid) {
    return token(ssid, "net#", SSID_DOMAIN, boot_salt());
}

std::string mac(std::string_view mac) {
    return token(mac, "mac#", MAC_DOMAIN, boot_salt());
}

std::string ssid_with_salt(std::string_view ssid, uint64_t salt) {
    return token(ssid, "net#", SSID_DOMAIN, salt);
}

std::string mac_with_salt(std::string_view mac, uint64_t salt) {
    return token(mac, "mac#", MAC_DOMAIN, salt);
}

IpScope ip_scope(std::string_view addr) {
    if (addr.empty())
        return IpScope::NotAnIp;
    if (addr.find(':') != std::string_view::npos) {
        uint8_t bytes[16];
        if (!parse_ipv6(addr, bytes))
            return IpScope::NotAnIp;
        return scope_v6(bytes);
    }
    uint8_t quad[4];
    if (!parse_ipv4(addr, quad))
        return IpScope::NotAnIp;
    return scope_v4(quad);
}

std::string ip(std::string_view addr) {
    return ip_with_salt(addr, boot_salt());
}

std::string ip_with_salt(std::string_view addr, uint64_t salt) {
    if (ip_scope(addr) != IpScope::Global)
        return std::string(addr);
    return token(addr, "ip#", IP_DOMAIN, salt);
}

std::string ips_in_text(std::string_view text) {
    if (text.empty())
        return {};
    const uint64_t salt = boot_salt();
    return redact_v4(redact_v6(text, salt), salt);
}

} // namespace helix::redact
