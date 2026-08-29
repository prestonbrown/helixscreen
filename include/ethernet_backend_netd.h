// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"

#include <string>
#include <vector>

/**
 * @brief Ethernet backend that asks the printer's network daemon
 *
 * helix::netd (netd_protocol.h) owns the wire format and the one-shot query;
 * this class maps a daemon snapshot onto EthernetInfo. Query-only and
 * synchronous: no events, no threads, no sockets of its own — every
 * get_info() opens one connection, sends one GET, and closes.
 *
 * has_interface() is deliberately daemon-free: it scans sysfs directly, so
 * the Ethernet row's visibility never flaps when the daemon restarts, and a
 * downed eth0 (which still exists in sysfs) still counts as present.
 *
 * Interface detection mirrors EthernetBackendLinux's name classification:
 * accept the well-known physical prefixes (eth/eno/enp/enP/ens/end/enx/em),
 * reject loopback, wireless, bridge and container names.
 *
 * Both calls are safe to invoke from any thread except the LVGL thread:
 * get_info() blocks for up to helix::netd::query_snapshot()'s default
 * 500 ms timeout while the daemon answers. EthernetManager already runs it
 * on a worker thread.
 */
class EthernetBackendNetd : public EthernetBackend {
  public:
    /**
     * @param sysfs_root Sysfs mount to read <root>/class/net/ under.
     *                   Injectable so tests can point at a fake tree;
     *                   production uses the default "/sys".
     */
    explicit EthernetBackendNetd(const std::string& sysfs_root = "/sys");
    ~EthernetBackendNetd() override;

    // ========================================================================
    // EthernetBackend Interface Implementation
    // ========================================================================

    bool has_interface() override;
    EthernetInfo get_info() override;

  private:
    /**
     * @brief Check if an interface name looks like physical Ethernet
     *
     * Prefix tables only (no sysfs probe): reject loopback, wireless and
     * virtual prefixes first, then accept the well-known physical Ethernet
     * naming schemes.
     *
     * @param name Interface name (e.g., "eth0", "enp3s0")
     * @return true if the name matches an Ethernet pattern
     */
    static bool is_ethernet_interface_name(const std::string& name);

    /**
     * @brief Scan <sysfs_root>/class/net/ for ethernet interfaces
     *
     * @return Ethernet-style interface names (e.g., {"eth0", "enp3s0"})
     */
    std::vector<std::string> scan_sysfs_interfaces() const;

    /**
     * @brief Read <sysfs_root>/class/net/<interface>/address, trimmed
     *
     * @return The MAC string, or "" when unreadable (logged at trace only:
     *         a MAC is PII)
     */
    std::string read_mac_address(const std::string& interface) const;

    std::string sysfs_root_;
};
