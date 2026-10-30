// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wifi_backend.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief WiFi UI utilities namespace
 *
 * Shared utility functions for WiFi UI components (wizard, settings panel, etc.)
 * Provides signal strength icon calculations and device information.
 */
namespace helix {
namespace ui {
namespace wifi {

/**
 * @brief Compute signal icon state from signal strength and security status
 *
 * Returns a state value 1-8 for use with multi-state signal icon images.
 * This enables reactive UI bindings where icon visibility is controlled by
 * comparing the current state to a reference value.
 *
 * State mapping:
 * - 1-4: Open networks (weak to strong signal)
 * - 5-8: Secured networks (weak to strong signal)
 *
 * Signal strength thresholds:
 * - State 1/5: 0-25% (weak)
 * - State 2/6: 26-50% (fair)
 * - State 3/7: 51-75% (good)
 * - State 4/8: 76-100% (excellent)
 *
 * @param strength_percent Signal strength as percentage (0-100)
 * @param secured Whether network is password-protected
 * @return State value 1-8 for icon visibility binding
 *
 * @example
 * int state = wifi_compute_signal_icon_state(75, true);  // Returns 7 (secured, good signal)
 * lv_obj_bind_flag_if_not_eq(icon_obj, state_subject, LV_OBJ_FLAG_HIDDEN, state);
 */
int wifi_compute_signal_icon_state(int strength_percent, bool secured);

/**
 * @brief Render a WiFiNetwork::band_mask as a compact row badge
 *
 * Numeric, unit-suffixed and deliberately not translated — "2.4G" / "5G" read
 * the same in every locale and fit a 480x272 list row.
 *
 * @param band_mask OR of WiFiBandFlag values
 * @return "2.4G", "5G", "6G", "2.4/5G", … or "" when no band is known
 */
std::string wifi_format_band_label(uint8_t band_mask);

/**
 * @brief True when a scan saw APs on more than one band
 *
 * Drives whether per-row band badges are worth showing at all: on a 2.4GHz-only
 * radio every row would say "2.4G", which is noise. Derived from the scan itself
 * rather than adapter capability, so it stays right when capability detection
 * fails but 5GHz APs are plainly visible.
 *
 * @param networks Scan results as handed to the picker
 * @return true if the union of all band flags has more than one bit set
 */
bool wifi_scan_spans_multiple_bands(const std::vector<WiFiNetwork>& networks);

/**
 * @brief Get device MAC address for specified network interface
 *
 * Retrieves the hardware MAC address from the system in formatted form.
 *
 * Platform-specific implementation:
 * - Linux: Reads from /sys/class/net/{interface}/address
 * - macOS: Parses `ifconfig {interface}` output for ether line
 *
 * @param interface Network interface name (default: "wlan0")
 * @return Formatted MAC address (e.g., "50:41:1C:XX:XX:XX") or empty string on error
 *
 * @example
 * std::string mac = wifi_get_device_mac("wlan0");
 * if (!mac.empty()) {
 *     spdlog::info("Device MAC: {}", mac);
 * }
 */
std::string wifi_get_device_mac(const std::string& interface = "wlan0");

/**
 * @brief Result of an OS-level wireless link probe.
 *
 * Describes whether the kernel reports a wireless interface with an up link,
 * independent of any managed backend (wpa_supplicant / NetworkManager). This is
 * the ground truth used as a fallback when the managed backend cannot reach its
 * control socket but the link is in fact live (helixscreen#1059, Qidi Q2).
 */
struct OsWifiLink {
    bool has_link = false; ///< Kernel reports the wireless iface link is up.
    std::string iface;     ///< Name of the wireless iface that was found (first match).
    bool has_ip = false;   ///< Iface has a non-link-local IPv4 (best-effort).
};

/**
 * @brief Probe the OS for a live wireless link, without side effects.
 *
 * Enumerates network interfaces under <sysfs_root>/class/net and identifies
 * wireless ones (a `wireless/` or `phy80211` subdir, or a name listed in
 * <proc_root>/net/wireless). For the first wireless iface found it reads
 * `operstate` and `carrier` to decide whether the link is up.
 *
 * `has_ip` is filled best-effort via getifaddrs() — but ONLY when probing the
 * real sysfs root ("/sys"). For a fixture root the live socket table does not
 * correspond to the fixture, so has_ip is left false and tests should not rely
 * on it. has_link (operstate/carrier) is the load-bearing signal and is fully
 * driven by the fixture tree.
 *
 * Never shells out to a subprocess; reads only sysfs/proc files and getifaddrs.
 *
 * @param sysfs_root Root for /sys reads (default "/sys"; override for tests).
 * @param proc_root  Root for /proc reads (default "/proc"; override for tests).
 * @return OsWifiLink with has_link/iface/has_ip populated.
 */
OsWifiLink probe_os_wifi_link(const std::string& sysfs_root = "/sys",
                              const std::string& proc_root = "/proc");

/**
 * @brief The routable IPv4 address the kernel holds on an interface
 *
 * Reads the live socket table (getifaddrs). Link-local (169.254/16) and
 * loopback (127/8) addresses are not routable and are skipped.
 *
 * @param iface Interface name (e.g. "wlan0")
 * @return Dotted-quad address, or "" when the interface has none (always ""
 *         on macOS and ESP32, which have no such table here)
 */
std::string iface_global_ipv4(const std::string& iface);

} // namespace wifi
} // namespace ui
} // namespace helix
