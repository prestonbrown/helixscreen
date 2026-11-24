// SPDX-License-Identifier: GPL-3.0-or-later
//
// Safe hostname resolution that avoids glibc __check_pf() SIGSEGV on
// embedded 32-bit ARM/MIPS glibc builds (AD5M, SonicPad, K1).
// On those platforms uses libhv's direct UDP DNS resolver and fails closed
// on error — calling getaddrinfo() reintroduces the crash this guards against.
// Elsewhere (Pi/aarch64, desktop) falls through to getaddrinfo().

#pragma once

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>

#if defined(__GLIBC__) && (defined(__arm__) || defined(__mips__))
extern "C" {
#include "base/dns_resolv.h" // libhv's direct UDP DNS resolver
}
#endif

namespace helix {

/// Resolve hostname + port to a connected-ready sockaddr_in.
/// Returns 0 on success, non-zero on failure.
inline int safe_resolve(const std::string& host, int port, struct sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    // Try numeric address first (no DNS needed)
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        return 0;
    }

#if defined(__GLIBC__) && (defined(__arm__) || defined(__mips__))
    // On embedded 32-bit glibc, getaddrinfo() can SIGSEGV inside __check_pf/
    // gaih_inet even when libnss_dns.so loads (seen as an AD5M crash,
    // fault_addr 0x62). Use direct UDP DNS resolution and fail closed if it
    // doesn't succeed — falling through to getaddrinfo() reintroduces the
    // very crash this function was written to prevent.
    {
        struct in_addr resolved;
        if (dns_resolv_resolve(host.c_str(), &resolved) == 0) {
            addr.sin_addr = resolved;
            return 0;
        }
        return -1;
    }
#else
    // Standard resolution (safe on desktop/Pi)
    struct addrinfo hints {
    }, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (err != 0 || !result) {
        return err ? err : -1;
    }
    auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    addr.sin_addr = sin->sin_addr;
    freeaddrinfo(result);
    return 0;
#endif
}

} // namespace helix
