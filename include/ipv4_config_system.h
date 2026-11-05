// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ipv4_config_system.h
 * @brief Where a Linux system persists an interface's IPv4 settings
 *
 * The one module that knows which network configuration owners exist on
 * Linux and how each persists a static address. The Wi-Fi and Ethernet
 * backends hold one of these and answer Ipv4Configurable through it:
 *
 * - NetworkManager: the active connection profile (`nmcli connection
 *   modify`), reapplied to the device. Needs the daemon running, which is
 *   how the backend factory already decides NetworkManager owns the box.
 * - dhcpcd: a block in /etc/dhcpcd.conf, then `dhcpcd -n <iface>`. Needs
 *   the daemon running and the file writable by this process (root); a
 *   non-root service user cannot edit it, so the setting stays hidden.
 *
 * Anything else (BusyBox udhcpc started by firmware init scripts, ifupdown,
 * systemd-networkd, the printer network daemon) has no store here and
 * reports unsupported.
 *
 * @threading read()/apply() BLOCK on subprocesses and file I/O: worker threads only.
 */

#pragma once

#include "ipv4_config.h"

#include <memory>
#include <string>

namespace helix::net {

class Ipv4SystemStore {
  public:
    virtual ~Ipv4SystemStore() = default;
    virtual Ipv4Result read(const std::string& iface, Ipv4Config& out) = 0;
    virtual Ipv4Result apply(const std::string& iface, const Ipv4Config& config) = 0;
};

/// Whether NetworkManager is the active network owner (nmcli + its runtime dir).
bool network_manager_active();

/// A NetworkManager store, or null when NetworkManager is not active.
std::unique_ptr<Ipv4SystemStore> make_network_manager_ipv4_store();

/// A dhcpcd store, or null when dhcpcd is not running or its config is not
/// writable by this process.
std::unique_ptr<Ipv4SystemStore> make_dhcpcd_ipv4_store();

/// Whichever owns this system's interfaces: NetworkManager first, then dhcpcd.
std::unique_ptr<Ipv4SystemStore> make_system_ipv4_store();

} // namespace helix::net
