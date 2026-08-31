// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

/**
 * @brief Shared sysfs-backed Ethernet interface classification
 *
 * The one place that decides whether a netdev name is a physical Ethernet
 * interface. Both Linux ethernet backends (the kernel-state reader and the
 * network-daemon reader) call these — two hand-maintained copies of this
 * rule would silently disagree about the same machine.
 */
namespace ethernet {

/**
 * @brief Classify one interface name as physical Ethernet
 *
 * Three tiers: reject loopback/virtual/wireless prefixes, fast-accept the
 * well-known physical naming schemes, then probe sysfs (backing device
 * present, no wireless dir, ARPHRD_ETHER) for kernel-renamed NICs on
 * embedded boards whose names match no prefix.
 *
 * @param name Interface name (e.g. "eth0", "enp3s0")
 * @param sysfs_root Sysfs mount to probe under (injectable for tests)
 */
bool is_ethernet_interface(const std::string& name, const std::string& sysfs_root = "/sys");

/**
 * @brief Scan <sysfs_root>/class/net/ for Ethernet interfaces
 *
 * @return Ethernet-style interface names, in directory order
 */
std::vector<std::string> scan_sysfs_interfaces(const std::string& sysfs_root = "/sys");

} // namespace ethernet

/**
 * @brief Ethernet connection information
 */
struct EthernetInfo {
    bool connected;          ///< True if interface is up with valid IP
    std::string interface;   ///< Interface name (e.g., "eth0", "en0")
    std::string ip_address;  ///< IPv4 address (e.g., "192.168.1.100")
    std::string mac_address; ///< MAC address (e.g., "aa:bb:cc:dd:ee:ff")
    std::string status;      ///< Human-readable status ("Connected", "No cable", "Unknown")

    EthernetInfo()
        : connected(false), interface(""), ip_address(""), mac_address(""), status("Unknown") {}
};

/**
 * @brief Abstract Ethernet backend interface
 *
 * Provides a clean, platform-agnostic API for querying Ethernet status.
 * Concrete implementations handle platform-specific details:
 * - EthernetBackendMacOS: macOS native APIs + libhv ifconfig
 * - EthernetBackendLinux: Linux /sys/class/net + libhv ifconfig
 * - EthernetBackendMock: Simulator mode with fake data
 *
 * Design principles:
 * - Query-only API (no enable/disable, no configuration)
 * - Synchronous operations (no async complexity)
 * - Simple status checking for UI display
 * - Clean error handling with meaningful messages
 */
class EthernetBackend {
  public:
    virtual ~EthernetBackend() = default;

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Check if any Ethernet interface exists
     *
     * Returns true if hardware is detected, regardless of connection status.
     *
     * @return true if at least one Ethernet interface is present
     */
    virtual bool has_interface() = 0;

    /**
     * @brief Get detailed Ethernet connection information
     *
     * Returns comprehensive status including IP address, MAC, and link state.
     * If multiple Ethernet interfaces exist, returns info for the first
     * connected interface, or first interface if none connected.
     *
     * @return EthernetInfo struct with current state
     */
    virtual EthernetInfo get_info() = 0;

    // ========================================================================
    // Factory Methods
    // ========================================================================

    /**
     * @brief Create appropriate backend for current platform
     *
     * - macOS: EthernetBackendMacOS
     * - Linux: EthernetBackendNetd when the printer's network daemon is
     *          present (interface truth comes from the daemon, which may
     *          have downed eth0 to enforce its single transport), else
     *          EthernetBackendLinux
     * - test mode: EthernetBackendMock (unless --real-ethernet)
     *
     * There is no failure fallback between the platform backends: create()
     * never probes the network, so the choice is a cheap filesystem
     * question answered before any backend runs.
     *
     * @return Unique pointer to backend instance (null on Android)
     */
    static std::unique_ptr<EthernetBackend> create();
};
