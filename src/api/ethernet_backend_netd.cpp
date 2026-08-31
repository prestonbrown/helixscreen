// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend_netd.h"

#include "ethernet_backend_linux.h"
#include "netd_protocol.h"
#include "spdlog/spdlog.h"

// Linux-only backend: falls back to an EthernetBackendLinux instance and the
// ethernet:: sysfs classifiers, both compiled out on non-Linux targets.
#if !defined(__ANDROID__) && !defined(__APPLE__)

namespace {

/// Human status for an ethernet-mode snapshot that is not connected. Short,
/// in the linux backend's tone ("No cable", "No connection"); never the raw
/// ALL-CAPS wire value. Diagnostic only: the UI never renders EthernetInfo
/// .status (it reads connected/ip/mac), so these stay untranslated like the
/// linux backend's statuses.
std::string ethernet_wait_status(const helix::netd::NetdSnapshot& snap) {
    const std::string& why = snap.reason.empty() ? snap.state : snap.reason;
    if (snap.state == "DHCP_WAIT" || why == "DHCP_WAIT")
        return "Waiting for address"; // i18n: do not translate
    if (snap.state == "NO_CARRIER" || why == "NO_CARRIER")
        return "No carrier"; // i18n: do not translate
    if (snap.state == "CONNECTING" || snap.state == "RETRYING")
        return "Connecting"; // i18n: do not translate
    if (snap.connected_state())
        return "No address yet"; // i18n: do not translate - link is up, no address arrived
    return "No connection";      // i18n: do not translate
}

} // namespace

// ============================================================================
// Construction / teardown
// ============================================================================

EthernetBackendNetd::EthernetBackendNetd(const std::string& sysfs_root,
                                         std::function<EthernetInfo()> kernel_state)
    : sysfs_root_(sysfs_root), kernel_state_(std::move(kernel_state)) {
    if (!kernel_state_) {
        kernel_state_ = [] {
            EthernetBackendLinux linux_backend;
            return linux_backend.get_info();
        };
    }
    spdlog::debug("[EthernetNetd] netd backend created (sysfs root: {})", sysfs_root_);
}

EthernetBackendNetd::~EthernetBackendNetd() {
    spdlog::trace("[EthernetNetd] netd backend destroyed");
}

// ============================================================================
// EthernetBackend Interface Implementation
// ============================================================================

bool EthernetBackendNetd::has_interface() {
    // Pure sysfs: the daemon is never consulted, so the Ethernet row's
    // visibility cannot flap when netd restarts. A downed eth0 still exists
    // in sysfs and still counts. Classification is the shared rule
    // (ethernet::scan_sysfs_interfaces) — same answer as the kernel-state
    // backend on the same machine.
    const std::vector<std::string> interfaces = ethernet::scan_sysfs_interfaces(sysfs_root_);
    spdlog::trace("[EthernetNetd] has_interface() = {} ({} candidates)", !interfaces.empty(),
                  interfaces.size());
    return !interfaces.empty();
}

EthernetInfo EthernetBackendNetd::get_info() {
    // Kernel state first: the reader's connected-first preference picks the
    // adapter a multi-NIC box is actually using, and its reading is the
    // whole answer when the daemon is unreachable (a live kernel address
    // must not go dark because its daemon died).
    EthernetInfo info = kernel_state_();

    // One blocking one-shot query with the protocol default timeout. Never
    // call this from the LVGL thread (see header).
    const helix::netd::QueryResult result = helix::netd::query_snapshot();

    if (!result.reached || !result.saw_mode) {
        // Daemon unreachable, or it answered without a MODE= line (an ERR
        // verdict): either way it said nothing authoritative about which
        // transport owns the link, and its mode-less snapshot must not blank
        // a row whose kernel address is live. Kernel truth stands, marked
        // degraded without hiding the state.
        if (info.connected) {
            info.status = "Connected (daemon unavailable)";
        } else if (info.status.empty() || info.status == "Unknown") {
            info.status = "Network daemon unavailable";
        }
        spdlog::debug("[EthernetNetd] Daemon not authoritative (reached={} mode={}): kernel "
                      "state: connected={} iface '{}'",
                      result.reached, result.saw_mode, info.connected, info.interface);
        return info;
    }

    const helix::netd::NetdSnapshot& snap = result.snapshot;

    if (snap.mode != "ETHERNET") {
        // Another transport owns the link; the Ethernet row stays idle.
        info.connected = false;
        info.ip_address.clear();
        info.status = snap.mode == "WIFI" ? "WiFi is the active connection" : "No connection";
        spdlog::debug("[EthernetNetd] Active transport is mode '{}'", snap.mode);
        return info;
    }

    if (snap.connected_state() && !snap.ip.empty()) {
        info.connected = true;
        info.ip_address = snap.ip;
        info.status = "Connected";
        spdlog::debug("[EthernetNetd] Ethernet connected: {} ({})", info.interface,
                      info.ip_address);
        return info;
    }

    info.connected = false;
    info.ip_address.clear();
    info.status = ethernet_wait_status(snap);
    spdlog::debug("[EthernetNetd] Ethernet not connected (state '{}' reason '{}'): {}", snap.state,
                  snap.reason, info.status);
    return info;
}

#endif // !__ANDROID__ && !__APPLE__
