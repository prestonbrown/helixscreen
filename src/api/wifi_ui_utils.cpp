// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_ui_utils.h"

#include "log_redact.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef __APPLE__
#include <array>
#include <memory>
#endif

#if !defined(__APPLE__) && !defined(HELIX_PLATFORM_ESP32)
#include <arpa/inet.h>
#include <ifaddrs.h> // no ifaddrs on newlib (ESP-IDF)
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace helix {
namespace ui {
namespace wifi {

namespace {

namespace fs = std::filesystem;

// Read the first line of a small sysfs file, trimmed of trailing whitespace.
// Returns empty string if the file is missing or unreadable.
std::string read_trimmed_line(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    std::getline(file, line);
    line.erase(
        std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) { return !std::isspace(ch); })
            .base(),
        line.end());
    return line;
}

// Decide whether an interface under <net_dir>/<iface> is wireless. An iface is
// wireless if it exposes a `wireless/` or `phy80211` subdirectory in sysfs.
bool sysfs_iface_is_wireless(const fs::path& iface_dir) {
    std::error_code ec;
    if (fs::exists(iface_dir / "wireless", ec)) {
        return true;
    }
    ec.clear();
    if (fs::exists(iface_dir / "phy80211", ec)) {
        return true;
    }
    return false;
}

// Parse interface names out of /proc/net/wireless. The file has two header
// lines, then one line per wireless iface of the form "  wlan0: 0000 ...".
std::vector<std::string> proc_wireless_ifaces(const std::string& proc_root) {
    std::vector<std::string> result;
    std::ifstream file(proc_root + "/net/wireless");
    if (!file.is_open()) {
        return result;
    }
    std::string line;
    while (std::getline(file, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue; // header lines have no colon
        }
        // The iface name is the leading token before the colon, sans whitespace.
        std::string name = line.substr(0, colon);
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (!name.empty()) {
            result.push_back(name);
        }
    }
    return result;
}

// Best-effort: does this interface have a routable (non-link-local) IPv4?
// Only meaningful against the live kernel socket table, so callers gate this
// on probing the real sysfs root.
bool iface_has_global_ipv4([[maybe_unused]] const std::string& iface) {
#if defined(__APPLE__) || defined(HELIX_PLATFORM_ESP32)
    return false;
#else
    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        return false;
    }
    bool found = false;
    for (auto* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!p->ifa_name || iface != p->ifa_name) {
            continue;
        }
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        const uint32_t addr = ntohl(sin->sin_addr.s_addr);
        // Skip 169.254.0.0/16 (link-local / APIPA) and 127.0.0.0/8.
        if ((addr & 0xFFFF0000u) == 0xA9FE0000u) {
            continue;
        }
        if ((addr & 0xFF000000u) == 0x7F000000u) {
            continue;
        }
        found = true;
        break;
    }
    freeifaddrs(ifap);
    return found;
#endif
}

} // namespace

int wifi_compute_signal_icon_state(int strength_percent, bool secured) {
    // Clamp to valid range
    strength_percent = std::max(0, std::min(100, strength_percent));

    // Determine base state from signal strength (1-4)
    int base_state;
    if (strength_percent <= 25) {
        base_state = 1; // Weak
    } else if (strength_percent <= 50) {
        base_state = 2; // Fair
    } else if (strength_percent <= 75) {
        base_state = 3; // Good
    } else {
        base_state = 4; // Excellent
    }

    // Add 4 for secured networks (1-4 = open, 5-8 = secured)
    return secured ? base_state + 4 : base_state;
}

std::string wifi_format_band_label(uint8_t band_mask) {
    std::string label;
    auto append = [&label](const char* part) {
        if (!label.empty()) {
            label += "/";
        }
        label += part;
    };

    if (band_mask & WIFI_BAND_2_4GHZ) {
        append("2.4");
    }
    if (band_mask & WIFI_BAND_5GHZ) {
        append("5");
    }
    if (band_mask & WIFI_BAND_6GHZ) {
        append("6");
    }

    if (label.empty()) {
        return label;
    }
    return label + "G";
}

bool wifi_scan_spans_multiple_bands(const std::vector<WiFiNetwork>& networks) {
    uint8_t seen = 0;
    for (const auto& net : networks) {
        seen = static_cast<uint8_t>(seen | net.band_mask);
    }
    // More than one bit set.
    return seen != 0 && (seen & (seen - 1)) != 0;
}

std::string wifi_get_device_mac(const std::string& interface) {
#ifdef __APPLE__
    // macOS: Parse ifconfig output for ether line
    std::string command = "ifconfig " + interface + " 2>/dev/null";
    std::array<char, 256> buffer;
    std::string result;

    // Use popen to execute command and read output
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        spdlog::error("[wifi_ui] Failed to execute ifconfig for interface '{}'", interface);
        return "";
    }

    // Read command output line by line
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    // Parse for "ether XX:XX:XX:XX:XX:XX" line
    const std::string ether_prefix = "ether ";
    size_t ether_pos = result.find(ether_prefix);
    if (ether_pos == std::string::npos) {
        spdlog::debug("[wifi_ui] No ether address found for interface '{}'", interface);
        return "";
    }

    // Extract MAC address (format: XX:XX:XX:XX:XX:XX)
    size_t mac_start = ether_pos + ether_prefix.length();
    size_t mac_end = result.find_first_of(" \t\n", mac_start);
    if (mac_end == std::string::npos) {
        mac_end = result.length();
    }

    std::string mac = result.substr(mac_start, mac_end - mac_start);

    // Remove trailing whitespace
    mac.erase(
        std::find_if(mac.rbegin(), mac.rend(), [](unsigned char ch) { return !std::isspace(ch); })
            .base(),
        mac.end());

    spdlog::debug("[wifi_ui] Found MAC address for '{}': {}", interface, helix::redact::mac(mac));
    return mac;

#else
    // Linux: Read from /sys/class/net/{interface}/address
    std::string path = "/sys/class/net/" + interface + "/address";
    std::ifstream file(path);

    if (!file.is_open()) {
        spdlog::debug("[wifi_ui] Failed to open {} (interface may not exist)", path);
        return "";
    }

    std::string mac;
    std::getline(file, mac);

    // Remove trailing newline/whitespace
    mac.erase(
        std::find_if(mac.rbegin(), mac.rend(), [](unsigned char ch) { return !std::isspace(ch); })
            .base(),
        mac.end());

    if (mac.empty()) {
        spdlog::error("[wifi_ui] MAC address file {} is empty", path);
        return "";
    }

    spdlog::debug("[wifi_ui] Found MAC address for '{}': {}", interface, helix::redact::mac(mac));
    return mac;
#endif
}

OsWifiLink probe_os_wifi_link(const std::string& sysfs_root, const std::string& proc_root) {
    OsWifiLink link;

    const std::string net_path = sysfs_root + "/class/net";
    std::error_code ec;
    if (!fs::exists(net_path, ec)) {
        return link;
    }

    // Names appearing in /proc/net/wireless are wireless even if their sysfs
    // node lacks the expected subdirs (some drivers/older kernels).
    std::vector<std::string> proc_names = proc_wireless_ifaces(proc_root);

    for (const auto& entry : fs::directory_iterator(net_path, ec)) {
        const fs::path iface_dir = entry.path();
        const std::string iface = iface_dir.filename().string();

        const bool wireless =
            sysfs_iface_is_wireless(iface_dir) ||
            std::find(proc_names.begin(), proc_names.end(), iface) != proc_names.end();
        if (!wireless) {
            continue;
        }

        // operstate "up" is the authoritative signal; carrier == "1" is a
        // fallback for drivers that leave operstate at "unknown" while the
        // physical link is genuinely associated.
        const std::string operstate = read_trimmed_line((iface_dir / "operstate").string());
        const std::string carrier = read_trimmed_line((iface_dir / "carrier").string());
        const bool up = (operstate == "up") || (carrier == "1");

        if (up) {
            link.has_link = true;
            link.iface = iface;
            // Live socket table only corresponds to the real root; don't probe
            // getifaddrs against a fixture tree (it would report the dev box).
            if (sysfs_root == "/sys") {
                link.has_ip = iface_has_global_ipv4(iface);
            }
            return link; // first up wireless iface wins
        }

        // Remember a wireless iface even if down, so the caller can see one
        // exists; the first up iface above takes precedence over this.
        if (link.iface.empty()) {
            link.iface = iface;
        }
    }

    return link;
}

} // namespace wifi
} // namespace ui
} // namespace helix
