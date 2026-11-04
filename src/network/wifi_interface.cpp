// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_interface.h"

#include "core/text_io.h"
#include "helix_fs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace hfs = helix::fs;
namespace tio = helix::text_io;

namespace helix::wifi {

namespace detail {

std::string parse_wpa_state(const std::string& status_reply) {
    static const std::string prefix = "wpa_state=";
    for (std::string_view sv : tio::lines(status_reply)) {
        std::string line(sv);
        if (line.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string value = line.substr(prefix.size());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        return value;
    }
    return {};
}

std::vector<DaemonInfo> list_wpa_daemons(const std::string& proc_root) {
    std::vector<DaemonInfo> daemons;

    if (proc_root.empty() || !hfs::is_directory(proc_root))
        return daemons;

    if (auto entries = hfs::list_dir(proc_root)) {
        for (const auto& e : *entries) {
            // PID directories only.
            const std::string name = e.name;
            if (name.empty() || !std::all_of(name.begin(), name.end(),
                                             [](unsigned char c) { return std::isdigit(c); }))
                continue;

            // /proc/<pid>/cmdline is NUL-separated argv.
            tio::LineReader cmd(hfs::join_path(e.path, "cmdline"), '\0');
            if (!cmd)
                continue;
            std::vector<std::string> argv;
            std::string arg;
            while (cmd.next(arg))
                argv.push_back(arg);
            if (argv.empty())
                continue;

            // argv[0] basename must be wpa_supplicant.
            if (hfs::filename(argv[0]) != "wpa_supplicant")
                continue;

            // Collect -i/-iVALUE (interface) and -c/-cVALUE (config path). Both
            // accept a separate "-X value" or a joined "-Xvalue" form.
            std::string i_val, c_val;
            for (size_t i = 1; i < argv.size(); ++i) {
                const std::string& a = argv[i];
                if (a == "-i" && i + 1 < argv.size()) {
                    i_val = argv[++i];
                } else if (a.rfind("-i", 0) == 0 && a.size() > 2) {
                    i_val = a.substr(2);
                } else if (a == "-c" && i + 1 < argv.size()) {
                    c_val = argv[++i];
                } else if (a.rfind("-c", 0) == 0 && a.size() > 2) {
                    c_val = a.substr(2);
                }
            }

            DaemonInfo d;
            d.pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
            d.iface = std::move(i_val);
            d.conf_path = std::move(c_val);
            daemons.push_back(std::move(d));
        }
    }
    return daemons;
}

pid_t find_daemon_for_interface(const std::string& proc_root, const std::string& netdev,
                                std::string& conf_path_out) {
    conf_path_out.clear();

    // Not the daemon that owns the interface we care about — keep scanning.
    // This is the fix: the code this replaces returned on the first
    // wpa_supplicant found, regardless of which interface it owned.
    for (const auto& d : list_wpa_daemons(proc_root)) {
        if (d.iface.empty() || d.iface != netdev)
            continue;

        conf_path_out = d.conf_path;
        return d.pid;
    }
    return -1;
}

std::string find_rfkill_node(const std::string& sys_root, const std::string& netdev) {
    // Try to find an rfkill entry in the netdev's phy80211 directory.
    const std::string phy_dir = sys_root + "/class/net/" + netdev + "/phy80211";
    if (hfs::is_directory(phy_dir)) {
        if (auto entries = hfs::list_dir(phy_dir)) {
            for (const auto& e : *entries) {
                if (e.name.compare(0, 6, "rfkill") == 0) {
                    return sys_root + "/class/rfkill/" + e.name;
                }
            }
        }
    }

    // Fall back to the first "wlan" typed switch in /sys/class/rfkill/.
    const std::string rfkill_dir = sys_root + "/class/rfkill";
    if (hfs::is_directory(rfkill_dir)) {
        if (auto entries = hfs::list_dir(rfkill_dir)) {
            for (const auto& e : *entries) {
                const std::string rfkill_name = e.name;
                const std::string type_file = hfs::join_path(e.path, "type");

                auto type_line = tio::read_first_line(type_file);
                if (!type_line)
                    continue;

                std::string type_value = std::move(*type_line);
                // Trim trailing whitespace.
                while (!type_value.empty() &&
                       (type_value.back() == '\r' || type_value.back() == '\n' ||
                        type_value.back() == ' ' || type_value.back() == '\t'))
                    type_value.pop_back();

                if (type_value == "wlan") {
                    return rfkill_dir + "/" + rfkill_name;
                }
            }
        }
    }

    return "";
}

bool has_non_wifi_network_path(const std::string& sys_root, const std::string& wifi_netdev) {
    const std::string net_dir = sys_root + "/class/net";
    if (!hfs::is_directory(net_dir))
        return false;

    if (auto entries = hfs::list_dir(net_dir)) {
        for (const auto& e : *entries) {
            const std::string name = e.name;
            if (name == "lo" || name == wifi_netdev)
                continue;

            // A second wireless interface is not a fallback — it shares the same
            // failure domain (radio) as the primary WiFi interface.
            if (hfs::is_directory(hfs::join_path(e.path, "wireless")))
                continue;

            auto operstate_line = tio::read_first_line(hfs::join_path(e.path, "operstate"));
            if (!operstate_line)
                continue;
            std::string operstate = std::move(*operstate_line);
            while (!operstate.empty() && (operstate.back() == '\r' || operstate.back() == '\n' ||
                                          operstate.back() == ' '))
                operstate.pop_back();

            if (operstate == "up")
                return true;
        }
    }
    return false;
}

} // namespace detail

std::optional<WifiInterface> resolve_interface(const Roots& roots, const StatusProbe& probe) {
    if (roots.ctrl.empty() || !hfs::is_directory(roots.ctrl))
        return std::nullopt;

    std::vector<std::string> names;
    if (auto entries = hfs::list_dir(roots.ctrl)) {
        for (const auto& e : *entries) {
            names.push_back(e.name);
        }
    }
    // Sorted purely for determinism among equally-ranked candidates.
    std::sort(names.begin(), names.end());

    std::string completed_name;
    std::string fallback_name;
    bool have_completed = false;
    bool have_fallback = false;

    for (const auto& name : names) {
        const std::string socket_path = roots.ctrl + "/" + name;
        const std::string reply = probe(socket_path);
        if (reply.empty())
            continue;

        // Filter out control-only sockets (p2p-dev-*, etc.) that have no
        // backing wireless netdev.
        const std::string wireless_dir = roots.sys + "/class/net/" + name + "/wireless";
        if (!hfs::is_directory(wireless_dir))
            continue;

        if (!have_fallback) {
            fallback_name = name;
            have_fallback = true;
        }
        if (!have_completed && detail::parse_wpa_state(reply) == "COMPLETED") {
            completed_name = name;
            have_completed = true;
        }
    }

    if (!have_completed && !have_fallback)
        return std::nullopt;

    WifiInterface result;
    result.netdev = have_completed ? completed_name : fallback_name;
    result.ctrl_socket = roots.ctrl + "/" + result.netdev;
    result.associated = have_completed;
    result.daemon_pid =
        detail::find_daemon_for_interface(roots.proc, result.netdev, result.conf_path);
    result.rfkill_node = detail::find_rfkill_node(roots.sys, result.netdev);

    return result;
}

} // namespace helix::wifi
