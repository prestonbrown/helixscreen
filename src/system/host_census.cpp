// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/host_census.h"

#include "core/text_io.h"
#include "system/log_collector.h"
#include "system/moonraker_local_probe.h"
#include "system_power.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace helix::diag {

namespace {

constexpr int kMaxFailedUnits = 20;
constexpr size_t kMaxProcesses = 32;

bool is_display_device(const std::string& target) {
    return target.rfind("/dev/fb", 0) == 0 || target.rfind("/dev/dri/", 0) == 0;
}

std::string argv0_basename(const std::string& cmdline) {
    const std::string argv0 = cmdline.substr(0, cmdline.find(' '));
    return fs::path(argv0).filename().string();
}

/// Absolute path of @p name in the usual bin dirs under @p root, or "".
std::string find_binary(const std::string& root, const char* name) {
    std::error_code ec;
    for (const char* dir : {"/usr/bin", "/bin", "/usr/sbin", "/sbin"}) {
        const std::string path = std::string(dir) + "/" + name;
        if (fs::exists(root + path, ec)) {
            return path;
        }
    }
    return {};
}

/// Display devices @p pid holds open. Reading another user's fd table needs
/// root; without it the process is simply not seen as a holder.
std::vector<std::string> held_display_devices(const std::string& proc_root, long pid) {
    std::vector<std::string> held;
    std::error_code ec;
    const fs::path fd_dir = fs::path(proc_root) / std::to_string(pid) / "fd";
    // increment(ec), never ++: the process can exit mid-walk, and operator++
    // reports that by throwing.
    for (fs::directory_iterator it(fd_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code link_ec;
        const std::string target = fs::read_symlink(it->path(), link_ec).string();
        if (!link_ec && is_display_device(target) &&
            std::find(held.begin(), held.end(), target) == held.end()) {
            held.push_back(target);
        }
    }
    return held;
}

} // namespace

const std::vector<std::string>& competing_ui_names() {
    static const std::vector<std::string> names = {
        "guppyscreen",   "GuppyScreen",   "grumpyscreen",  "Grumpyscreen",
        "KlipperScreen", "klipperscreen", "featherscreen", "FeatherScreen",
        "mksclient",     "qidi-client",   "qidiclient",    "makerbase-client"};
    return names;
}

std::string os_release_pretty_name(const std::string& os_release) {
    std::istringstream in(os_release);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) {
            continue;
        }
        std::string value = line.substr(12);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return {};
}

std::vector<std::string> parse_failed_units(const std::string& list_units_output) {
    std::vector<std::string> units;
    std::istringstream in(list_units_output);
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string unit;
        if (fields >> unit) {
            units.push_back(unit);
        }
    }
    return units;
}

std::optional<std::vector<std::string>> failed_units_from(int exit_status,
                                                          const std::string& output) {
    if (exit_status != 0 && output.empty()) {
        return std::nullopt;
    }
    return parse_failed_units(output);
}

std::vector<CensusProcess> census_processes(const std::string& proc_root, size_t cap) {
    std::vector<CensusProcess> out;
    // A partial census is the useful answer: the caller's section also carries
    // the platform, CPU and uptime, and one throw would cost all of it.
    try {
        for (const auto& proc : read_process_table(proc_root)) {
            if (out.size() >= cap) {
                break;
            }
            CensusProcess entry;
            entry.pid = proc.pid;
            entry.name = argv0_basename(proc.cmdline);
            for (const auto& needle : competing_ui_names()) {
                if (proc.cmdline.find(needle) != std::string::npos) {
                    entry.reasons.push_back("competing_ui:" + needle);
                    break;
                }
            }
            for (auto& device : held_display_devices(proc_root, proc.pid)) {
                entry.reasons.push_back(std::move(device));
            }
            if (!entry.reasons.empty()) {
                out.push_back(std::move(entry));
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("[HostCensus] process walk stopped early: {}", e.what());
    }
    return out;
}

HostCensus collect_host_census(const std::string& root) {
    HostCensus census;
    for (const char* rel : {"/etc/os-release", "/usr/lib/os-release"}) {
        if (auto body = helix::text_io::read_file(root + rel)) {
            census.os_pretty_name = os_release_pretty_name(*body);
            break;
        }
    }

    // Run only against the live machine; a test root has nothing to run.
    if (root.empty() && helix::host_runs_systemd(root) && !find_binary(root, "systemctl").empty()) {
        // `timeout` keeps a wedged systemd from stalling the bundle upload.
        const std::string timeout = find_binary(root, "timeout");
        int status = -1;
        const std::string output = helix::logs::run_capture_tail(
            (timeout.empty() ? "" : timeout + " 5 ") +
                "systemctl list-units --state=failed --plain --no-legend --no-pager 2>/dev/null",
            kMaxFailedUnits, "systemctl", &status);
        census.failed_units = failed_units_from(status, output);
    }

    census.processes = census_processes(root.empty() ? "/proc" : root + "/proc", kMaxProcesses);
    return census;
}

} // namespace helix::diag
