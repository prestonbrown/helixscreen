// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Linux-only backend: the shared helix::ethernet:: classifiers it calls are compiled
// out on non-Linux targets (see ethernet_backend.cpp), so this TU must be too.
#if !defined(__ANDROID__) && !defined(__APPLE__)

#include "ethernet_backend_linux.h"

#include "ifconfig.h" // libhv's cross-platform ifconfig utility

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sys/stat.h>

namespace {

int list_with_ifconfig(std::vector<helix::ethernet::InterfaceState>& rows) {
    std::vector<ifconfig_t> interfaces;
    const int result = ifconfig(interfaces);
    if (result != 0) {
        return result;
    }
    for (const auto& iface : interfaces) {
        rows.push_back({iface.name, iface.ip, iface.mac, ""});
    }
    return 0;
}

} // namespace

EthernetBackendLinux::EthernetBackendLinux() : lister_(list_with_ifconfig) {
    spdlog::debug("[EthernetLinux] Linux backend created");
}

EthernetBackendLinux::~EthernetBackendLinux() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[EthernetLinux] Linux backend destroyed\n");
}

bool EthernetBackendLinux::is_ethernet_interface(const std::string& name) {
    // Classification lives in one shared place (helix::ethernet::is_ethernet_interface,
    // ethernet_backend.cpp) — the daemon-backed backend classifies with the
    // same rule, and two copies would silently disagree about a machine.
    return helix::ethernet::is_ethernet_interface(name, sysfs_root_);
}

std::string EthernetBackendLinux::read_operstate(const std::string& interface) {
    // Read <sysfs root>/class/net/<interface>/operstate
    std::string path = sysfs_root_ + "/class/net/" + interface + "/operstate";
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
    // Shared implementation (helix::ethernet::scan_sysfs_interfaces).
    return helix::ethernet::scan_sysfs_interfaces(sysfs_root_);
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

    std::vector<helix::ethernet::InterfaceState> interfaces;
    int result = lister_(interfaces);

    if (result != 0) {
        spdlog::error("[EthernetLinux] ifconfig() failed with code: {}", result);
        info.status = "Error querying interfaces";
        return info;
    }

    std::vector<helix::ethernet::InterfaceState> ethernet_interfaces;
    for (auto& iface : interfaces) {
        if (is_ethernet_interface(iface.name)) {
            iface.operstate = read_operstate(iface.name);
            ethernet_interfaces.push_back(std::move(iface));
        }
    }

    // ifconfig() can leave out an interface with no address; sysfs lists it.
    if (ethernet_interfaces.empty()) {
        for (const auto& name : scan_sysfs_interfaces()) {
            ethernet_interfaces.push_back({name, "", "", read_operstate(name)});
        }
    }

    return helix::ethernet::select_interface(ethernet_interfaces);
}

#endif // !__ANDROID__ && !__APPLE__
