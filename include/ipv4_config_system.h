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
 * - NetworkManager: the connection profile in use on the interface, or with
 *   none active the link's most recently used one (`nmcli connection
 *   modify`), reapplied to the device. Needs the daemon running, which is
 *   how the backend factory already decides NetworkManager owns the box.
 * - dhcpcd: a block in /etc/dhcpcd.conf, then `dhcpcd -n <iface>`. Needs
 *   the daemon running and either the file writable by this process (root)
 *   or a service user whose `sudo -n` needs no password; then the file is
 *   staged privately and installed over the original through sudo. Neither,
 *   and the setting stays hidden.
 *
 * Anything else (BusyBox udhcpc started by firmware init scripts, ifupdown,
 * systemd-networkd, the printer network daemon) has no store here and
 * reports unsupported.
 *
 * @threading Everything here BLOCKS on subprocesses and file I/O: worker threads only.
 */

#pragma once

#include "ipv4_config.h"

#include <memory>
#include <string>

namespace helix::net {

class Ipv4SystemStore {
  public:
    virtual ~Ipv4SystemStore() = default;
    /// False while the store cannot edit anything yet or ever (dhcpcd still
    /// probing sudo, or no way to write its file). Never blocks.
    virtual bool available() const {
        return true;
    }
    virtual Ipv4Result read(const std::string& iface, Ipv4Link link, Ipv4Config& out) = 0;
    virtual Ipv4Result prepare(const std::string& iface, Ipv4Link link, const Ipv4Config& next,
                               Ipv4Snapshot& out) = 0;
    /// Edits exactly what @p target names, never what happens to be active now.
    virtual Ipv4Result apply(const Ipv4Snapshot& target, const Ipv4Config& config) = 0;
};

/// What a backend's Ipv4Configurable overrides forward to: unsupported with
/// no store, failed with no interface, else the store's answer.
bool store_supported(const Ipv4SystemStore* store);
Ipv4Result store_read(Ipv4SystemStore* store, const std::string& iface, Ipv4Link link,
                      Ipv4Config& out);
Ipv4Result store_prepare(Ipv4SystemStore* store, const std::string& iface, Ipv4Link link,
                         const Ipv4Config& next, Ipv4Snapshot& out);
Ipv4Result store_apply(Ipv4SystemStore* store, const Ipv4Snapshot& target,
                       const Ipv4Config& config);

/// Put back what @p snapshot recorded: a NetworkManager profile's values by
/// UUID, whether or not it is active, or the dhcpcd.conf text as it was,
/// unless someone else has edited the file since. Needs no backend, so it
/// works before the network is up. BLOCKING.
Ipv4Result restore_ipv4_snapshot(const Ipv4Snapshot& snapshot);

/**
 * @brief One change, under ipv4_change_mutex(): snapshot, record, apply
 *
 * Refused, with nothing touched and no record written, when @p abandoned is
 * already set, when an earlier change still waits to be undone (only a record
 * the restore gave up on may be replaced), or when the snapshot or the record
 * cannot be made. Failed means it may have been partly applied, with the
 * record on disk for the revert. BLOCKING.
 */
Ipv4Result apply_ipv4_change(Ipv4Configurable& backend, Ipv4Link link, const Ipv4Config& previous,
                             const Ipv4Config& next, const std::atomic<bool>* abandoned);

/// What restore_pending_ipv4() did. Only Restored means the previous config
/// is back; Retry may succeed later; Terminal never will (the target was
/// edited or is gone, or the record cannot be read).
struct Ipv4RestoreOutcome {
    enum class Kind { Restored, Nothing, Skipped, Retry, Terminal };
    Kind kind = Kind::Nothing;
    std::string detail;
};

/// Restore and remove the pending record for @p link. Skipped for another
/// link's record, a failed one, or, without @p include_own, one this process's
/// countdown owns. BLOCKING.
Ipv4RestoreOutcome restore_pending_ipv4(Ipv4Link link, bool include_own);

/// Mark @p link's record as given up: kept, with @p why, never retried. BLOCKING.
bool mark_pending_ipv4_failed(Ipv4Link link, const std::string& why);

/// Whether NetworkManager is the active network owner (nmcli + its runtime dir).
bool network_manager_active();

/// A NetworkManager store, or null when NetworkManager is not active.
std::unique_ptr<Ipv4SystemStore> make_network_manager_ipv4_store();

/// A dhcpcd store, or null when dhcpcd is not running. It is available once
/// this process is known to be able to write dhcpcd.conf, directly or through
/// sudo; the sudo probe runs on a worker.
std::unique_ptr<Ipv4SystemStore> make_dhcpcd_ipv4_store();

/// Whichever owns this system's interfaces: NetworkManager first, then dhcpcd.
std::unique_ptr<Ipv4SystemStore> make_system_ipv4_store();

} // namespace helix::net
