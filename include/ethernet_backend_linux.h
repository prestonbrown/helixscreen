// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"

#include <functional>
#include <string>
#include <vector>

/**
 * @brief Linux Ethernet backend implementation
 *
 * Uses libhv's cross-platform ifconfig() utility to enumerate network
 * interfaces, plus Linux-specific /sys/class/net for detailed status.
 *
 * Interface detection strategy:
 * - Accepts known physical Ethernet prefixes by name (fast path)
 * - Falls back to a sysfs probe (device/, wireless/, type) for unknown names
 * - Excludes loopback, WiFi, virtual interfaces (docker*, virbr*, veth*, ...)
 * - Checks /sys/class/net/<interface>/operstate for link status
 * - Returns first interface with "up" operstate and valid IP
 *
 * Linux interface naming accepted (fast path):
 * - eth0, eth1:   Traditional kernel naming
 * - eno1:         systemd onboard / firmware index
 * - enp3s0:       systemd PCI bus/slot
 * - enP4p65s0:    Rockchip / Orange Pi PCI domain
 * - ens33:        systemd hot-plug slot
 * - end0:         RK3588 / NanoPi / some Radxa boards
 * - enx001122...: systemd MAC-based (USB Ethernet adapters)
 * - em1:          biosdevname (older Dell / Fedora)
 *
 * Anything else (e.g. kernel-renamed interfaces on embedded boards) is
 * classified by probing /sys/class/net/<name>/ directly.
 */
class EthernetBackendLinux : public EthernetBackend {
  public:
    EthernetBackendLinux();
    ~EthernetBackendLinux() override;

    // ========================================================================
    // EthernetBackend Interface Implementation
    // ========================================================================

    bool has_interface() override;
    EthernetInfo get_info() override;

    /// Lists interfaces with their name, ip and mac (operstate stays empty;
    /// get_info() reads it from sysfs). Returns 0, or ifconfig()'s error code.
    using InterfaceLister = std::function<int(std::vector<helix::ethernet::InterfaceState>&)>;

    /// Replace the interface listing and the sysfs root that get_info() reads.
    void set_sources_for_test(InterfaceLister lister, std::string sysfs_root) {
        lister_ = std::move(lister);
        sysfs_root_ = std::move(sysfs_root);
    }

  private:
    InterfaceLister lister_;
    std::string sysfs_root_ = "/sys";

    /**
     * @brief Check if interface name looks like physical Ethernet
     *
     * @param name Interface name (e.g., "eth0", "enp3s0")
     * @return true if name matches Ethernet pattern
     */
    bool is_ethernet_interface(const std::string& name);

    /**
     * @brief Read interface operstate from sysfs
     *
     * Reads /sys/class/net/<interface>/operstate
     *
     * @param interface Interface name
     * @return "up", "down", "unknown", or empty string on error
     */
    std::string read_operstate(const std::string& interface);

    /**
     * @brief Scan /sys/class/net/ for ethernet interfaces
     *
     * This method scans the sysfs network directory directly, which finds
     * interfaces regardless of their IP address or connection state. This is
     * more reliable than ifconfig() which may not return interfaces without IPs.
     *
     * @return Vector of ethernet interface names (e.g., {"eth0", "enp3s0"})
     */
    std::vector<std::string> scan_sysfs_interfaces();
};
