// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"

/**
 * @brief macOS Ethernet backend implementation
 *
 * Uses libhv's cross-platform ifconfig() utility to enumerate network
 * interfaces and detect Ethernet connectivity.
 *
 * Interface detection strategy:
 * - Filters for Ethernet interfaces (en1, en2, en3, etc.)
 * - Excludes loopback and common WiFi interfaces (en0 on some Macs)
 * - Returns first interface with valid IP address
 * - Falls back to any en* interface if found
 *
 * Note: macOS network interfaces are named differently from Linux:
 * - en0: Often WiFi (but sometimes Ethernet on Mac Minis/iMacs)
 * - en1, en2, en3: Typically Thunderbolt/USB Ethernet adapters
 * - en4+: Additional adapters
 */
class EthernetBackendMacOS : public EthernetBackend {
  public:
    EthernetBackendMacOS();
    ~EthernetBackendMacOS() override;

    // EthernetBackend Interface Implementation

    bool has_interface() override;
    EthernetInfo get_info() override;

  private:
    /**
     * @brief Check if interface name looks like Ethernet
     *
     * @param name Interface name (e.g., "en0", "en1")
     * @return true if name matches Ethernet pattern
     */
    bool is_ethernet_interface(const std::string& name);
};
