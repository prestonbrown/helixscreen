// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log_redact.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace helix::redact {

namespace {

// Domain separators, so the same bytes hashed as an SSID and as a MAC produce
// different tokens. Without this a bundle reader could tell that some AP's
// BSSID matched some other field, which is itself a disclosure.
constexpr uint64_t SSID_DOMAIN = 0x5353'4944'0000'0001ULL;
constexpr uint64_t MAC_DOMAIN = 0x4D41'4300'0000'0002ULL;

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

} // namespace helix::redact
