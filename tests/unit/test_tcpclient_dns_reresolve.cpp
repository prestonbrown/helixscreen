// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tcpclient_dns_reresolve.cpp
 * @brief Reconnect attempts must re-resolve the hostname.
 *
 * hv::TcpClient resolves remote_host into remote_addr exactly once, at
 * open()/createsocket() time. Every auto-reconnect retry then goes through
 * createsocket(sockaddr*) — the overload that never resolves — so a hostname
 * whose IP changed during a network blip (router reboot re-lease, mesh
 * roaming, mDNS re-announce) is dialed at its stale address forever. That is
 * the reported Android failure: mid-print WiFi drop, app never reconnected
 * until the tablet itself was restarted (a fresh process re-resolves).
 *
 * The fix is TcpClient::refresh_remote_addr(): re-run the resolution before
 * each dial attempt, keep the last known address when DNS cannot answer yet,
 * and never break the retry chain.
 */
//
// TEST_MIRROR_OK: exercises TcpClient::refresh_remote_addr(), which
//                 patches/libhv-tcpclient-reconnect-resilience.patch adds to the
//                 libhv submodule - there is no include/ or src/ header to include.

#include "hv/TcpClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>

#include "../catch_amalgamated.hpp"

namespace {

bool is_loopback(const sockaddr_u& addr) {
    if (addr.sa.sa_family == AF_INET) {
        return (addr.sin.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    }
    if (addr.sa.sa_family == AF_INET6) {
        const auto& v6 = addr.sin6.sin6_addr;
        static constexpr unsigned char V6_LOOPBACK[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                          0, 0, 0, 0, 0, 0, 0, 1};
        return std::memcmp(&v6, V6_LOOPBACK, 16) == 0;
    }
    return false;
}

} // namespace

TEST_CASE("TcpClient re-resolves the hostname for reconnect dials", "[libhv][reconnect][dns]") {
    hv::TcpClient client;

    SECTION("refresh_remote_addr resolves a hostname into remote_addr") {
        client.remote_host = "localhost";
        client.remote_port = 7125;
        std::memset(&client.remote_addr, 0, sizeof(client.remote_addr));

        REQUIRE(client.refresh_remote_addr());
        CHECK(is_loopback(client.remote_addr));
        CHECK(ntohs(client.remote_addr.sin.sin_port) == 7125);
    }

    SECTION("a hostname that changed IP replaces the stale address") {
        client.remote_host = "localhost";
        client.remote_port = 7125;
        // Simulate the stale address left over from open(): wrong IP, wrong
        // port. refresh_remote_addr() must overwrite it wholesale.
        std::memset(&client.remote_addr, 0, sizeof(client.remote_addr));
        client.remote_addr.sin.sin_family = AF_INET;
        client.remote_addr.sin.sin_addr.s_addr = htonl(0x0a000001); // 10.0.0.1
        client.remote_addr.sin.sin_port = htons(1);

        REQUIRE(client.refresh_remote_addr());
        CHECK(is_loopback(client.remote_addr));
        CHECK(ntohs(client.remote_addr.sin.sin_port) == 7125);
    }

    SECTION("unresolvable hostname keeps the last known address") {
        client.remote_host = "localhost";
        client.remote_port = 7125;
        REQUIRE(client.refresh_remote_addr());

        // DNS is down / record gone mid-reconnect. The retry chain must keep
        // dialing the last known address, not zero it out or kill the chain.
        client.remote_host = "helix-nonexistent-host.invalid";
        CHECK_FALSE(client.refresh_remote_addr());
        CHECK(is_loopback(client.remote_addr));
        CHECK(ntohs(client.remote_addr.sin.sin_port) == 7125);
    }

    SECTION("literal IP refreshes without DNS") {
        client.remote_host = "127.0.0.1";
        client.remote_port = 7125;
        std::memset(&client.remote_addr, 0, sizeof(client.remote_addr));

        REQUIRE(client.refresh_remote_addr());
        CHECK(is_loopback(client.remote_addr));
        CHECK(ntohs(client.remote_addr.sin.sin_port) == 7125);
    }

    SECTION("empty host is a no-op failure") {
        client.remote_host = "";
        client.remote_port = 7125;
        std::memset(&client.remote_addr, 0, sizeof(client.remote_addr));
        CHECK_FALSE(client.refresh_remote_addr());
        CHECK(client.remote_addr.sin.sin_port == 0);
    }
}
