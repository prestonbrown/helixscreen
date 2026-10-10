// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend_mock.h"

#include "ifconfig.h" // libhv's cross-platform ifconfig utility
#include "log_redact.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <vector>

EthernetBackendMock::EthernetBackendMock() {
    spdlog::debug("[EthernetMock] Mock backend created");

    // Try to get real MAC address from system for realistic demo display
    std::vector<ifconfig_t> interfaces;
    int result = ifconfig(interfaces);

    if (result == 0) {
        // Find first interface with a valid MAC
        for (const auto& iface : interfaces) {
            // iface.mac is a char array, check if it's non-empty and not all zeros
            if (iface.mac[0] != '\0' && strcmp(iface.mac, "00:00:00:00:00:00") != 0) {
                real_mac_ = iface.mac;
                spdlog::debug("[EthernetMock] Using real MAC from {}: {}", iface.name,
                              helix::redact::mac(real_mac_));
                break;
            }
        }
    }

    if (real_mac_.empty()) {
        // Fallback to realistic-looking fake MAC (locally administered)
        real_mac_ = "02:42:ac:11:00:02";
        spdlog::debug("[EthernetMock] Using fallback MAC: {}", helix::redact::mac(real_mac_));
    }
}

EthernetBackendMock::~EthernetBackendMock() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[EthernetMock] Mock backend destroyed\n");
}

bool EthernetBackendMock::has_interface() {
    // Always report Ethernet available in mock mode
    return true;
}

EthernetInfo EthernetBackendMock::get_info() {
    // Return mock data with real MAC for realistic demo
    EthernetInfo info;
    const bool connected = connected_.load();
    info.connected = connected;
    info.interface = "en0";
    info.ip_address = connected ? "192.168.1.150" : "";
    info.mac_address = real_mac_;
    info.status = connected ? "Connected" : "No cable";

    spdlog::trace("[EthernetMock] get_info() → {} ({})", info.ip_address, info.status);
    return info;
}
