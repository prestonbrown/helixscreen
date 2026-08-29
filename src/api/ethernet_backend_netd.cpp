// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend_netd.h"

#include "netd_protocol.h"
#include "spdlog/spdlog.h"

#if !defined(__ANDROID__)

#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>

namespace {

// Prefix tables mirroring EthernetBackendLinux's name classification: reject
// loopback/wireless/virtual names, then accept the well-known physical
// Ethernet naming schemes. Existence is decided here, never by the daemon.
constexpr const char* const kRejectPrefixes[] = {
    "wlan",   "wlp", "wlx",    // WiFi
    "docker", "br-", "virbr",  // Virtual bridges
    "bridge",                  // Bridges spelled out (busybox ifconfig)
    "veth",                    // Container virtual Ethernet pairs
    "tun",    "tap",           // VPN / tunnels
    "bond",                    // Bonded interfaces (aggregate, not physical)
    "ppp",                     // Point-to-point (cellular, dial-up)
    "can",                     // CAN bus
    "sit",    "gre", "ip6tnl", // IP-over-IP tunnels
};

constexpr const char* const kEthernetPrefixes[] = {
    "eth", // Traditional kernel naming (eth0, eth1, ...)
    "eno", // systemd onboard / firmware index (eno1, ...)
    "enp", // systemd PCI bus/slot (enp3s0, ...)
    "enP", // Rockchip / Orange Pi PCI domain (enP4p65s0, ...)
    "ens", // systemd hot-plug slot (ens33, ...)
    "end", // RK3588 / NanoPi / some Radxa boards (end0, ...)
    "enx", // systemd MAC-based (USB NICs: enx001122334455)
    "em",  // biosdevname (older Dell / Fedora: em1, em2)
};

template <size_t N>
bool starts_with_any(const std::string& name, const char* const (&prefixes)[N]) {
    for (const char* prefix : prefixes) {
        const size_t len = std::strlen(prefix);
        if (name.compare(0, len, prefix) == 0)
            return true;
    }
    return false;
}

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

EthernetBackendNetd::EthernetBackendNetd(const std::string& sysfs_root) : sysfs_root_(sysfs_root) {
    spdlog::debug("[EthernetNetd] netd backend created (sysfs root: {})", sysfs_root_);
}

EthernetBackendNetd::~EthernetBackendNetd() {
    spdlog::trace("[EthernetNetd] netd backend destroyed");
}

// ============================================================================
// Sysfs helpers — the daemon-free half
// ============================================================================

bool EthernetBackendNetd::is_ethernet_interface_name(const std::string& name) {
    if (name.empty() || name == "lo")
        return false;
    if (starts_with_any(name, kRejectPrefixes))
        return false;
    return starts_with_any(name, kEthernetPrefixes);
}

std::vector<std::string> EthernetBackendNetd::scan_sysfs_interfaces() const {
    std::vector<std::string> ethernet_interfaces;
    const std::string net_dir = sysfs_root_ + "/class/net";

    // RAII guard ensures closedir() is always called.
    auto dir_deleter = [](DIR* d) {
        if (d)
            closedir(d);
    };
    std::unique_ptr<DIR, decltype(dir_deleter)> dir(opendir(net_dir.c_str()), dir_deleter);
    if (!dir) {
        spdlog::trace("[EthernetNetd] Cannot open {}", net_dir);
        return ethernet_interfaces;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (entry->d_name[0] == '.')
            continue; // "." and ".."
        if (is_ethernet_interface_name(entry->d_name))
            ethernet_interfaces.emplace_back(entry->d_name);
    }
    return ethernet_interfaces;
}

std::string EthernetBackendNetd::read_mac_address(const std::string& interface) const {
    std::ifstream file(sysfs_root_ + "/class/net/" + interface + "/address");
    std::string mac;
    if (!file.is_open() || !std::getline(file, mac)) {
        spdlog::trace("[EthernetNetd] No MAC readable for {} under {}", interface, sysfs_root_);
        return "";
    }
    while (!mac.empty() && std::isspace(static_cast<unsigned char>(mac.back())))
        mac.pop_back();
    // Trace only: a MAC is identifying information.
    spdlog::trace("[EthernetNetd] {} address: {}", interface, mac);
    return mac;
}

// ============================================================================
// EthernetBackend Interface Implementation
// ============================================================================

bool EthernetBackendNetd::has_interface() {
    // Pure sysfs: the daemon is never consulted, so the Ethernet row's
    // visibility cannot flap when netd restarts. A downed eth0 still exists
    // in sysfs and still counts.
    const std::vector<std::string> interfaces = scan_sysfs_interfaces();
    spdlog::trace("[EthernetNetd] has_interface() = {} ({} candidates)", !interfaces.empty(),
                  interfaces.size());
    return !interfaces.empty();
}

EthernetInfo EthernetBackendNetd::get_info() {
    EthernetInfo info;

    // Adapter identity is daemon-free sysfs data; resolve it first so the row
    // keeps its interface and MAC whatever the daemon says (or fails to say).
    const std::vector<std::string> interfaces = scan_sysfs_interfaces();
    if (!interfaces.empty()) {
        info.interface = interfaces[0];
        info.mac_address = read_mac_address(info.interface);
    }

    // One blocking one-shot query with the protocol default timeout. Never
    // call this from the LVGL thread (see header).
    const helix::netd::QueryResult result = helix::netd::query_snapshot();

    if (!result.reached) {
        info.status = "Network daemon unavailable";
        spdlog::debug("[EthernetNetd] Daemon unreachable; interface '{}'", info.interface);
        return info;
    }

    const helix::netd::NetdSnapshot& snap = result.snapshot;

    if (snap.mode != "ETHERNET") {
        // Another transport owns the link; the Ethernet row stays idle.
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

    info.status = ethernet_wait_status(snap);
    spdlog::debug("[EthernetNetd] Ethernet not connected (state '{}' reason '{}'): {}", snap.state,
                  snap.reason, info.status);
    return info;
}

#endif // !__ANDROID__
