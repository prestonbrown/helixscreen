// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace helix::wifi {

/**
 * @file wifi_interface.h
 * @brief Resolve ONE coherent WiFi identity instead of three separate guesses.
 *
 * Three uncorrelated first-match guesses (the netdev: first /sys/class/net/wlan*
 * with a wireless/ dir; the control socket: first entry in the ctrl dir; the
 * config path: first wpa_supplicant in /proc) can disagree. On an Adventurer 5M
 * the first netdev is wlan1 while the answering socket is
 * /run/wpa_supplicant/wlan0.
 *
 * Resolution starts from the control socket that actually answers STATUS,
 * because that is the only one of the three we can verify, and derives the rest
 * from it.
 */

/// Filesystem roots, injectable so resolution is testable against a fake tree.
struct Roots {
    std::string sys = "/sys";
    std::string proc = "/proc";
    std::string ctrl; ///< wpa_supplicant control dir; caller supplies the detected one.
};

/// Sends STATUS to a control socket and returns the raw reply ("" on failure).
using StatusProbe = std::function<std::string(const std::string& socket_path)>;

struct WifiInterface {
    std::string netdev;      ///< "wlan0"
    std::string ctrl_socket; ///< "/run/wpa_supplicant/wlan0"
    std::string conf_path;   ///< the -c of the daemon behind that socket ("" if none)
    pid_t daemon_pid = -1;   ///< -1 when no matching daemon was found
    std::string rfkill_node; ///< "/sys/class/rfkill/rfkill0", "" when the driver exposes none
    bool associated = false; ///< true when STATUS reported wpa_state=COMPLETED
};

/// Resolve the interface this process should manage.
///
/// Preference order among sockets that answer @p probe:
///   1. wpa_state=COMPLETED (the radio actually carrying traffic)
///   2. any other answering socket, first by name for determinism
///
/// A candidate is rejected unless `<sys>/class/net/<name>/wireless` exists,
/// which filters out p2p-dev and other control-only sockets.
///
/// @return std::nullopt when no socket answers or none maps to a wireless
///         netdev. Callers must fall back to previous behaviour rather than
///         treating this as fatal.
std::optional<WifiInterface> resolve_interface(const Roots& roots, const StatusProbe& probe);

namespace detail {

/// Extract `wpa_state=` from a raw STATUS reply ("" when absent).
std::string parse_wpa_state(const std::string& status_reply);

/// One running wpa_supplicant process, as seen in /proc.
struct DaemonInfo {
    pid_t pid;
    std::string iface;     ///< The -i value ("" when the daemon was launched without one).
    std::string conf_path; ///< The -c value ("" when the daemon was launched without one).
};

/// Walk @p proc_root once and return every running wpa_supplicant process
/// along with the -i/-c values from its cmdline. Used both to resolve a
/// specific interface's daemon (find_daemon_for_interface) and to log every
/// daemon present for diagnostics — one /proc walk serves both.
std::vector<DaemonInfo> list_wpa_daemons(const std::string& proc_root);

/// Find the wpa_supplicant PID whose argv names @p netdev via -i, and return
/// its -c value through @p conf_path_out. Returns -1 when no daemon matches.
/// Unlike the walk it replaces, this does NOT return on the first
/// wpa_supplicant found — it keeps looking for the one owning @p netdev.
pid_t find_daemon_for_interface(const std::string& proc_root, const std::string& netdev,
                                std::string& conf_path_out);

/// Find the rfkill switch controlling @p netdev's radio.
/// Prefers the device's own PHY rfkill entry (phy80211/rfkill*/), and falls back
/// to the first "wlan" typed switch in /sys/class/rfkill/. Returns "" if none.
std::string find_rfkill_node(const std::string& sys_root, const std::string& netdev);

/// True when @p sys_root has at least one non-WiFi network interface that is
/// UP — i.e. a wired (or otherwise non-radio) path this device could still be
/// reached through if @p wifi_netdev's radio were disabled.
///
/// Iterates `<sys_root>/class/net/*`, skipping `lo`, skipping @p wifi_netdev,
/// and skipping any entry with a `wireless/` subdirectory (a second wireless
/// interface shares the same failure domain as the primary radio, so it is
/// never counted as a fallback). Every remaining entry must report
/// `operstate` == "up" to count.
///
/// Used to gate WiFiManager's stored-radio-state reassert: reasserting "off"
/// on a device whose only network path is the radio just disabled would
/// strand it (helixscreen wifi-interface-identity Task 15, CC1 incident).
///
/// Guards every filesystem walk with std::error_code; a missing or unreadable
/// `<sys_root>/class/net` returns false rather than throwing.
bool has_non_wifi_network_path(const std::string& sys_root, const std::string& wifi_netdev);

} // namespace detail

} // namespace helix::wifi
