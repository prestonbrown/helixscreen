// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_identity.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#if !defined(HELIX_PLATFORM_ESP32)
// Interface enumeration only; newlib has no ifaddrs and the URL parsing below
// needs none of it.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <unistd.h>
#include <unordered_map>

namespace helix {
namespace {

std::mutex g_cache_mutex;
std::unordered_map<std::string, bool> g_cache;

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_loopback_literal(std::string_view host) {
    if (host.empty())
        return true;
    const std::string h = to_lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "::1";
}

#if defined(HELIX_PLATFORM_ESP32)
// IDF has no gethostname(). A firmware panel is never the machine klipper runs
// on, so co-location is false there by construction rather than by measurement.
bool matches_own_hostname(std::string_view) {
    return false;
}
#else
bool matches_own_hostname(std::string_view host) {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) != 0)
        return false;
    return to_lower(host) == to_lower(buf);
}
#endif

#if defined(HELIX_PLATFORM_ESP32)
// newlib has no ifaddrs; a single-NIC firmware cannot be reached at one of its
// own addresses under a different name, so the hostname check above settles it.
bool matches_local_interface_ip(std::string_view) {
    return false;
}
#else
bool matches_local_interface_ip(std::string_view host) {
    in_addr v4{};
    in6_addr v6{};
    const std::string h(host);
    const bool is_v4 = inet_pton(AF_INET, h.c_str(), &v4) == 1;
    const bool is_v6 = !is_v4 && inet_pton(AF_INET6, h.c_str(), &v6) == 1;
    if (!is_v4 && !is_v6)
        return false;

    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0)
        return false;

    bool found = false;
    for (auto* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr)
            continue;
        if (is_v4 && p->ifa_addr->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            if (sin->sin_addr.s_addr == v4.s_addr) {
                found = true;
                break;
            }
        } else if (is_v6 && p->ifa_addr->sa_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(p->ifa_addr);
            if (std::memcmp(&sin6->sin6_addr, &v6, sizeof(v6)) == 0) {
                found = true;
                break;
            }
        }
    }
    freeifaddrs(ifap);
    return found;
}
#endif

} // namespace

bool is_moonraker_on_same_host(std::string_view host) {
    std::string key(host);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end())
            return it->second;
    }

    const bool result =
        is_loopback_literal(host) || matches_own_hostname(host) || matches_local_interface_ip(host);

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.emplace(std::move(key), result);
    return result;
}

void invalidate_host_identity_cache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.clear();
}

std::string extract_host_from_websocket_url(const std::string& url) {
    // Expected format: ws://host:port/websocket or wss://host:port/websocket
    // or: ws://[ipv6]:port/websocket

    if (url.empty()) {
        return "";
    }

    std::string remainder;

    // Check for ws:// or wss:// prefix
    const std::string ws_prefix = "ws://";
    const std::string wss_prefix = "wss://";

    if (url.find(ws_prefix) == 0) {
        remainder = url.substr(ws_prefix.length());
    } else if (url.find(wss_prefix) == 0) {
        remainder = url.substr(wss_prefix.length());
    } else {
        return ""; // Unknown scheme
    }

    // Handle IPv6 addresses in brackets [::1]
    if (!remainder.empty() && remainder[0] == '[') {
        auto close_bracket = remainder.find(']');
        if (close_bracket != std::string::npos) {
            // Return content between brackets (the IPv6 address)
            return remainder.substr(1, close_bracket - 1);
        }
        return ""; // Malformed IPv6
    }

    // Find the port separator
    auto colon_pos = remainder.find(':');
    if (colon_pos == std::string::npos) {
        // No port - find the path separator
        auto slash_pos = remainder.find('/');
        if (slash_pos != std::string::npos) {
            return remainder.substr(0, slash_pos);
        }
        return remainder; // Just hostname
    }

    // Return everything before the colon (the host)
    return remainder.substr(0, colon_pos);
}

} // namespace helix
