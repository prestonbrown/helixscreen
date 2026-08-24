// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if !defined(__ANDROID__)

#include "ethernet_backend_linux.h"

#include "ifconfig.h" // libhv's cross-platform ifconfig utility

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>

EthernetBackendLinux::EthernetBackendLinux() {
    spdlog::debug("[EthernetLinux] Linux backend created");
}

EthernetBackendLinux::~EthernetBackendLinux() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[EthernetLinux] Linux backend destroyed\n");
}

bool EthernetBackendLinux::is_ethernet_interface(const std::string& name) {
    // Linux physical Ethernet interface detection.
    //
    // Strategy:
    //   1. Reject loopback and known virtual / wireless prefixes up front.
    //   2. Fast-path accept well-known physical Ethernet prefixes.
    //   3. For anything else, consult sysfs: accept only if it has a real
    //      backing device, is not wireless, and reports ARPHRD_ETHER.
    //
    // The sysfs fallback catches interfaces that don't match any known prefix
    // (e.g. USB NICs named `enx<mac>`, Rockchip `end0`, older Dell `em0`, or
    // kernel-renamed interfaces on embedded boards).

    if (name.empty() || name == "lo") {
        return false;
    }

    // Reject obviously non-Ethernet prefixes.
    static const char* const REJECT_PREFIXES[] = {
        "wlan",   "wlp", "wlx",    // WiFi
        "docker", "br-", "virbr",  // Virtual bridges
        "veth",                    // Container virtual Ethernet pairs
        "tun",    "tap",           // VPN / tunnels
        "bond",                    // Bonded interfaces (aggregate, not physical)
        "ppp",                     // Point-to-point (cellular, dial-up)
        "can",                     // CAN bus
        "sit",    "gre", "ip6tnl", // IP-over-IP tunnels
    };
    for (const char* prefix : REJECT_PREFIXES) {
        size_t len = std::strlen(prefix);
        if (name.compare(0, len, prefix) == 0) {
            return false;
        }
    }

    // Fast-path: well-known physical Ethernet naming schemes. Accepting these
    // without sysfs lets `get_info()` work against libhv's ifconfig_t list
    // even if sysfs is unavailable (e.g. containerized test environments).
    static const char* const ETHERNET_PREFIXES[] = {
        "eth", // Traditional kernel naming (eth0, eth1, ...)
        "eno", // systemd onboard / firmware index (eno1, ...)
        "enp", // systemd PCI bus/slot (enp3s0, ...)
        "enP", // Rockchip / Orange Pi PCI domain (enP4p65s0, ...)
        "ens", // systemd hot-plug slot (ens33, ...)
        "end", // RK3588 / NanoPi / some Radxa boards (end0, ...)
        "enx", // systemd MAC-based (USB NICs: enx001122334455)
        "em",  // biosdevname (older Dell / Fedora: em1, em2)
    };
    for (const char* prefix : ETHERNET_PREFIXES) {
        size_t len = std::strlen(prefix);
        if (name.compare(0, len, prefix) == 0) {
            return true;
        }
    }

    // Unknown naming scheme — probe sysfs to classify it.
    const std::string base = "/sys/class/net/" + name;
    struct stat st;

    // Must have a backing hardware device (excludes most virtual interfaces
    // that slipped past the reject list).
    if (stat((base + "/device").c_str(), &st) != 0) {
        return false;
    }

    // Must not be a wireless device (WiFi drivers expose a `wireless/` dir).
    if (stat((base + "/wireless").c_str(), &st) == 0) {
        return false;
    }

    // Check ARPHRD type — 1 == ARPHRD_ETHER. Anything else (772=loopback,
    // 776=SIT tunnel, 778=IPGRE, 823=IEEE802154, etc.) is not Ethernet.
    std::ifstream type_file(base + "/type");
    if (!type_file.is_open()) {
        return false;
    }
    int arp_type = -1;
    type_file >> arp_type;
    if (arp_type != 1) {
        return false;
    }

    spdlog::debug("[EthernetLinux] Accepted interface via sysfs probe: {}", name);
    return true;
}

std::string EthernetBackendLinux::read_operstate(const std::string& interface) {
    // Read /sys/class/net/<interface>/operstate
    std::string path = "/sys/class/net/" + interface + "/operstate";
    std::ifstream file(path);

    if (!file.is_open()) {
        spdlog::warn("[EthernetLinux] Cannot read operstate: {}", path);
        return "";
    }

    std::string state;
    std::getline(file, state);
    file.close();

    // Trim whitespace
    state.erase(std::remove_if(state.begin(), state.end(), ::isspace), state.end());

    spdlog::trace("[EthernetLinux] {} operstate: {}", interface, state);
    return state;
}

std::vector<std::string> EthernetBackendLinux::scan_sysfs_interfaces() {
    std::vector<std::string> ethernet_interfaces;

    // Scan /sys/class/net/ directly - this finds interfaces regardless of IP assignment
    const char* sysfs_net = "/sys/class/net";

    // RAII guard: unique_ptr with custom deleter ensures closedir() is always called
    auto dir_deleter = [](DIR* d) {
        if (d)
            closedir(d);
    };
    std::unique_ptr<DIR, decltype(dir_deleter)> dir(opendir(sysfs_net), dir_deleter);

    if (!dir) {
        spdlog::warn("[EthernetLinux] Cannot open {}", sysfs_net);
        return ethernet_interfaces;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string name = entry->d_name;
        if (is_ethernet_interface(name)) {
            spdlog::debug("[EthernetLinux] Found Ethernet interface via sysfs: {}", name);
            ethernet_interfaces.push_back(name);
        }
    }

    // No manual closedir() needed - RAII handles cleanup
    return ethernet_interfaces;
}

bool EthernetBackendLinux::has_interface() {
    // Use sysfs scan which finds interfaces regardless of IP assignment
    // This is more reliable than ifconfig() which may not return interfaces without IPs
    auto interfaces = scan_sysfs_interfaces();

    if (!interfaces.empty()) {
        spdlog::debug("[EthernetLinux] has_interface() = true ({} found)", interfaces[0]);
        return true;
    }

    spdlog::debug("[EthernetLinux] No Ethernet interface found");
    return false;
}

EthernetInfo EthernetBackendLinux::get_info() {
    EthernetInfo info;

    std::vector<ifconfig_t> interfaces;
    int result = ifconfig(interfaces);

    if (result != 0) {
        spdlog::error("[EthernetLinux] ifconfig() failed with code: {}", result);
        info.status = "Error querying interfaces";
        return info;
    }

    // Strategy: Find first Ethernet interface with "up" operstate and valid IP
    // Preference order:
    // 1. First eth*/eno*/enp*/ens* with operstate "up" and valid IP
    // 2. First eth*/eno*/enp*/ens* with valid IP (ignore operstate)
    // 3. First eth*/eno*/enp*/ens* interface found (even without IP)

    ifconfig_t* first_ethernet = nullptr;
    ifconfig_t* ip_ethernet = nullptr;
    ifconfig_t* up_ethernet = nullptr;

    for (auto& iface : interfaces) {
        std::string name = iface.name;
        std::string ip = iface.ip;

        if (!is_ethernet_interface(name)) {
            continue;
        }

        // Remember first Ethernet interface
        if (!first_ethernet) {
            first_ethernet = &iface;
        }

        // Check if it has a valid IP
        bool has_ip = !ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1";
        if (has_ip && !ip_ethernet) {
            ip_ethernet = &iface;
        }

        // Check operstate
        std::string operstate = read_operstate(name);
        if (operstate == "up" && has_ip) {
            up_ethernet = &iface;
            break; // Found best match, use it
        }
    }

    // Use best available interface
    ifconfig_t* selected = up_ethernet ? up_ethernet : ip_ethernet ? ip_ethernet : first_ethernet;

    if (!selected) {
        // Fall back to sysfs scan for interfaces without IPs
        auto sysfs_interfaces = scan_sysfs_interfaces();
        if (sysfs_interfaces.empty()) {
            info.status = "No Ethernet interface";
            spdlog::debug("[EthernetLinux] No Ethernet interface found");
            return info;
        }

        // Found interface via sysfs - populate basic info
        info.interface = sysfs_interfaces[0];
        std::string operstate = read_operstate(info.interface);

        if (operstate == "down") {
            info.connected = false;
            info.status = "No cable";
            spdlog::debug("[EthernetLinux] Ethernet cable disconnected: {} (operstate: {})",
                          info.interface, operstate);
        } else {
            info.connected = false;
            info.status = "No connection";
            spdlog::debug("[EthernetLinux] Ethernet interface {} has no IP (operstate: {})",
                          info.interface, operstate);
        }

        return info;
    }

    // Populate info from selected interface
    info.interface = selected->name;
    info.ip_address = selected->ip;
    info.mac_address = selected->mac;

    // Read operstate for status determination
    std::string operstate = read_operstate(info.interface);

    // Determine connection status based on IP and operstate
    bool has_ip =
        !info.ip_address.empty() && info.ip_address != "0.0.0.0" && info.ip_address != "127.0.0.1";

    if (has_ip && operstate == "up") {
        info.connected = true;
        info.status = "Connected";
        spdlog::debug("[EthernetLinux] Ethernet connected: {} ({}, operstate: {})", info.interface,
                      info.ip_address, operstate);
    } else if (has_ip) {
        info.connected = true;
        info.status = "Connected";
        spdlog::info("[EthernetLinux] Ethernet has IP: {} ({}, operstate: {})", info.interface,
                     info.ip_address, operstate);
    } else if (operstate == "down") {
        info.connected = false;
        info.status = "No cable";
        spdlog::debug("[EthernetLinux] Ethernet cable disconnected: {} (operstate: {})",
                      info.interface, operstate);
    } else {
        info.connected = false;
        info.status = "No connection";
        spdlog::debug("[EthernetLinux] Ethernet interface {} has no IP (operstate: {})",
                      info.interface, operstate);
    }

    return info;
}

#endif // !__ANDROID__
