// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EthernetBackendLinux::get_info() over a controlled interface listing and a
 * fixture sysfs tree, so the path from the kernel's answers to the reported
 * status is pinned end to end.
 */

// Subject TU is Linux-only (ethernet_backend_linux.cpp guards __APPLE__ too).
#if !defined(__APPLE__) && !defined(__ANDROID__)

#include "ethernet_backend_linux.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// A sysfs root under /tmp holding class/net/<iface>/operstate files.
struct FakeSysfs {
    std::string root;

    FakeSysfs() {
        char tmpl[] = "/tmp/helix_eth_sysfs_XXXXXX";
        char* d = ::mkdtemp(tmpl);
        REQUIRE(d != nullptr);
        root = d;
    }
    ~FakeSysfs() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void set_operstate(const std::string& iface, const std::string& state) {
        const std::string dir = root + "/class/net/" + iface;
        std::filesystem::create_directories(dir);
        std::ofstream(dir + "/operstate") << state << "\n";
    }
};

EthernetInfo info_for(const FakeSysfs& sysfs, std::vector<helix::ethernet::InterfaceState> listed) {
    EthernetBackendLinux backend;
    backend.set_sources_for_test(
        [listed](std::vector<helix::ethernet::InterfaceState>& rows) {
            rows = listed;
            return 0;
        },
        sysfs.root);
    return backend.get_info();
}

} // namespace

TEST_CASE("Linux Ethernet get_info reports an unplugged port with an address as no cable",
          "[network][ethernet_link]") {
    FakeSysfs sysfs;

    SECTION("operstate down: no cable, and the leftover address is not shown") {
        sysfs.set_operstate("eth0", "down");
        const EthernetInfo info =
            info_for(sysfs, {{"eth0", "192.168.1.50", "aa:bb:cc:dd:ee:ff", ""}});
        CHECK(info.interface == "eth0");
        CHECK_FALSE(info.connected);
        CHECK(info.status == "No cable");
        CHECK(info.ip_address.empty());
    }

    SECTION("operstate up: connected with its address") {
        sysfs.set_operstate("eth0", "up");
        const EthernetInfo info =
            info_for(sysfs, {{"eth0", "192.168.1.50", "aa:bb:cc:dd:ee:ff", ""}});
        CHECK(info.connected);
        CHECK(info.status == "Connected");
        CHECK(info.ip_address == "192.168.1.50");
    }

    SECTION("a non-Ethernet interface in the listing is not reported") {
        sysfs.set_operstate("eth0", "down");
        sysfs.set_operstate("wlan0", "up");
        const EthernetInfo info =
            info_for(sysfs, {{"wlan0", "10.0.0.9", "", ""}, {"eth0", "192.168.1.50", "", ""}});
        CHECK(info.interface == "eth0");
        CHECK(info.status == "No cable");
    }
}

TEST_CASE("Linux Ethernet get_info reports a listing failure", "[network][ethernet_link]") {
    FakeSysfs sysfs;
    EthernetBackendLinux backend;
    backend.set_sources_for_test([](std::vector<helix::ethernet::InterfaceState>&) { return -1; },
                                 sysfs.root);
    const EthernetInfo info = backend.get_info();
    CHECK_FALSE(info.connected);
    CHECK(info.status == "Error querying interfaces");
}

#endif // !__APPLE__ && !__ANDROID__
