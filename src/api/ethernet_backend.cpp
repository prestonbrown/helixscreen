// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ethernet_backend.h"

#ifdef HELIX_ENABLE_MOCKS
#include "ethernet_backend_mock.h"
#endif
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include "ethernet_backend_macos.h"
#elif !defined(__ANDROID__)
#include "ethernet_backend_linux.h"
#include "ethernet_backend_netd.h"
#include "netd_protocol.h"
#endif

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>

namespace {

// Shared classification tables — see ethernet::is_ethernet_interface.
// Reject first (loopback is equality-matched by the caller), then accept.

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

bool starts_with_any(const std::string& name, const char* const* prefixes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const size_t len = std::strlen(prefixes[i]);
        if (name.compare(0, len, prefixes[i]) == 0)
            return true;
    }
    return false;
}

} // namespace

namespace ethernet {

bool is_ethernet_interface(const std::string& name, const std::string& sysfs_root) {
    if (name.empty() || name == "lo")
        return false;
    if (starts_with_any(name, kRejectPrefixes, std::size(kRejectPrefixes)))
        return false;
    // Fast-accept well-known physical naming schemes without sysfs, so the
    // classification also works where sysfs is unavailable (containers).
    if (starts_with_any(name, kEthernetPrefixes, std::size(kEthernetPrefixes)))
        return true;

    // Unknown naming scheme — probe sysfs: a real backing device, not
    // wireless, ARPHRD_ETHER. This is the tier that catches kernel-renamed
    // NICs on embedded boards.
    const std::string base = sysfs_root + "/class/net/" + name;
    struct stat st {};
    if (::stat((base + "/device").c_str(), &st) != 0)
        return false;
    if (::stat((base + "/wireless").c_str(), &st) == 0)
        return false;
    std::ifstream type_file(base + "/type");
    if (!type_file.is_open())
        return false;
    int arp_type = -1;
    type_file >> arp_type;
    if (arp_type != 1)
        return false;
    spdlog::debug("[Ethernet] Accepted interface via sysfs probe: {}", name);
    return true;
}

std::vector<std::string> scan_sysfs_interfaces(const std::string& sysfs_root) {
    std::vector<std::string> ethernet_interfaces;
    const std::string net_dir = sysfs_root + "/class/net";

    auto dir_deleter = [](DIR* d) {
        if (d)
            closedir(d);
    };
    std::unique_ptr<DIR, decltype(dir_deleter)> dir(opendir(net_dir.c_str()), dir_deleter);
    if (!dir) {
        spdlog::trace("[Ethernet] Cannot open {}", net_dir);
        return ethernet_interfaces;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (entry->d_name[0] == '.')
            continue; // "." and ".."
        if (is_ethernet_interface(entry->d_name, sysfs_root))
            ethernet_interfaces.emplace_back(entry->d_name);
    }
    return ethernet_interfaces;
}

} // namespace ethernet
#endif // !__APPLE__ && !__ANDROID__

std::unique_ptr<EthernetBackend> EthernetBackend::create() {
    // In test mode, always use mock unless --real-ethernet was specified
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_ethernet()) {
        spdlog::debug("[EthernetBackend] Test mode: using mock backend");
        return std::make_unique<EthernetBackendMock>();
    }
#endif

#ifdef __APPLE__
    // macOS: Use native backend (handles missing interface gracefully)
    spdlog::debug("[EthernetBackend] Creating macOS backend");
    auto backend = std::make_unique<EthernetBackendMacOS>();

    if (backend->has_interface()) {
        spdlog::debug("[EthernetBackend] macOS backend initialized (interface found)");
    } else {
        spdlog::info("[EthernetBackend] No Ethernet interface found");
    }
    return backend;
#elif defined(__ANDROID__)
    // Android: Ethernet managed by the OS
    spdlog::info("[EthernetBackend] Android platform - Ethernet not managed natively");
    return nullptr;
#else
    // Linux: when the printer's network daemon owns the network, interface
    // truth comes from it (a daemon enforcing single-transport downs eth0,
    // which ioctls then misread as "no cable"); otherwise ask the OS.
    if (helix::netd::available()) {
        spdlog::debug("[EthernetBackend] Creating netd backend");
        auto backend = std::make_unique<EthernetBackendNetd>();

        if (backend->has_interface()) {
            spdlog::debug("[EthernetBackend] netd backend initialized (interface found)");
        } else {
            spdlog::info("[EthernetBackend] No Ethernet interface found");
        }
        return backend;
    }

    // Linux: Use native backend (handles missing interface gracefully)
    spdlog::debug("[EthernetBackend] Creating Linux backend");
    auto backend = std::make_unique<EthernetBackendLinux>();

    if (backend->has_interface()) {
        spdlog::debug("[EthernetBackend] Linux backend initialized (interface found)");
    } else {
        spdlog::info("[EthernetBackend] No Ethernet interface found");
    }
    return backend;
#endif
}
