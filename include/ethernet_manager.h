// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ethernet_backend.h"
#include "helix_shared_callback.h"

#include <functional>
#include <memory>
#include <string>

namespace helix {
class LifetimeToken;
}

/**
 * @brief Ethernet Manager - High-level interface for Ethernet status queries
 *
 * Provides simple API for checking Ethernet connectivity and retrieving
 * network information. Uses pluggable backend system:
 * - macOS: EthernetBackendMacOS (libhv ifconfig + native APIs)
 * - Linux: EthernetBackendLinux (libhv ifconfig + sysfs)
 * - Fallback: EthernetBackendMock (simulator/testing)
 *
 * Usage:
 * ```cpp
 * auto manager = std::make_unique<EthernetManager>();
 *
 * if (manager->has_interface()) {
 *     std::string ip = manager->get_ip_address();
 *     if (!ip.empty()) {
 *         // Display "Connected (192.168.1.100)"
 *     }
 * }
 * ```
 *
 * Key features:
 * - Query-only API (no configuration/enable/disable)
 * - Automatic backend selection per platform
 * - Synchronous operations (no async complexity)
 * - Simple error handling
 */
class EthernetManager {
  public:
    /**
     * @brief Initialize Ethernet manager with appropriate backend
     *
     * Automatically selects platform-appropriate backend:
     * - macOS: EthernetBackendMacOS
     * - Linux: EthernetBackendLinux
     * - Fallback: EthernetBackendMock (if no interface found)
     */
    EthernetManager();

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~EthernetManager();

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Check if any Ethernet interface is present
     *
     * Returns true if Ethernet hardware is detected, regardless of
     * connection status or IP assignment.
     *
     * @return true if at least one Ethernet interface exists
     */
    bool has_interface();

    /**
     * @brief Get detailed Ethernet connection information
     *
     * Returns comprehensive status including interface name, IP address,
     * MAC address, and connection status.
     *
     * @return EthernetInfo struct with current state
     */
    EthernetInfo get_info();

    /**
     * @brief Asynchronously retrieve Ethernet connection information
     *
     * Dispatches the backend probe (which may block on libhv ifconfig() +
     * sysfs reads) to a worker thread. The callback is invoked on the main
     * UI thread via helix::ui::UpdateQueue::queue_update() once the probe
     * completes.
     *
     * Returns to the caller immediately (typically in well under 1 ms) so
     * this is safe to call from the UI thread.
     *
     * IMPORTANT: The callback must be guarded against owner destruction by
     * the caller (e.g. capture a helix::LifetimeToken and check expired()
     * before touching `this`). EthernetManager does not track callback
     * lifetimes.
     *
     * @param callback Invoked on the UI thread with the populated EthernetInfo
     */
    void get_info_async(std::function<void(const EthernetInfo&)> callback);

    /**
     * @brief Get Ethernet IP address (convenience method)
     *
     * Returns IP address if connected, empty string otherwise.
     * Useful for quick status display in UI.
     *
     * @return IP address string (e.g., "192.168.1.100"), or empty if not connected
     */
    std::string get_ip_address();

    /// Whether the backend can persist a static IPv4 config
    /// (helix::net::Ipv4Configurable::supports_ipv4_config()).
    bool supports_ipv4_config();

    /**
     * @brief Read the interface's IPv4 config on an HTTP fast-lane worker
     *
     * @p on_done(ok, config) runs on the LVGL thread while @p token lives. A
     * backend without support answers at once with ok false.
     */
    void get_ipv4_config_async(
        helix::LifetimeToken token,
        helix::SharedCallback<void(bool ok, const helix::net::Ipv4Config& config)> on_done);

    /**
     * @brief On an HTTP fast-lane worker, apply @p next through helix::net::apply_ipv4_change()
     *
     * The revert record and its snapshot are written first. @p previous is
     * what the record shows; @p abandoned, once set, makes the apply restore
     * the snapshot itself. @p on_done(ok) runs on the LVGL thread while
     * @p token lives; the failure's detail is logged here.
     */
    void apply_ipv4_change_async(const helix::net::Ipv4Config& previous,
                                 const helix::net::Ipv4Config& next,
                                 std::shared_ptr<std::atomic<bool>> abandoned,
                                 helix::LifetimeToken token,
                                 helix::SharedCallback<void(bool ok)> on_done);

    friend class EthernetManagerTestAccess;

  private:
    // Held as shared_ptr so that an in-flight async worker (see
    // get_info_async) can keep the backend alive past EthernetManager's
    // destruction. The worker captures a copy of this shared_ptr by value;
    // if EthernetManager is destroyed first, the backend lives until the
    // worker thread finishes its probe.
    std::shared_ptr<EthernetBackend> backend_;
};
