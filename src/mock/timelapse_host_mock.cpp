// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_host_mock.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <thread>

namespace helix::mock {

namespace {
TimelapseHost host_from_env() {
    return parse_timelapse_host(std::getenv("HELIX_MOCK_TIMELAPSE"));
}

std::atomic<bool>& linked() {
    static std::atomic<bool> value{false};
    return value;
}

std::atomic<bool>& ffmpeg_installed() {
    static std::atomic<bool> value{false};
    return value;
}

// Long enough to see each step's status on screen.
constexpr auto kStepDelay = std::chrono::milliseconds(800);
} // namespace

TimelapseHost parse_timelapse_host(const char* value) {
    const std::string_view v = value ? value : "";
    if (v == "absent") {
        return TimelapseHost::Absent;
    }
    if (v == "no-ffmpeg") {
        return TimelapseHost::NoFfmpeg;
    }
    if (v == "no-sudo") {
        return TimelapseHost::NoSudo;
    }
    return TimelapseHost::Present;
}

bool timelapse_plugin_present() {
    return host_from_env() == TimelapseHost::Present || linked().load();
}

ProcessRunner timelapse_host_runner() {
    return [](const std::vector<std::string>& argv, int /*timeout_seconds*/) {
        const TimelapseHost host = host_from_env();
        CommandResult r;
        r.started = true;
        r.exit_code = 0;
        std::string cmd;
        for (const auto& a : argv) {
            cmd += (cmd.empty() ? "" : " ") + a;
        }
        spdlog::info("[MockTimelapseHost] {}", cmd);
        std::this_thread::sleep_for(kStepDelay);
        if (cmd.find("--link-timelapse") != std::string::npos) {
            linked().store(true);
        } else if (cmd == "sh -c command -v ffmpeg") {
            const bool has = host == TimelapseHost::Absent || host == TimelapseHost::Present ||
                             ffmpeg_installed().load();
            r.exit_code = has ? 0 : 1;
        } else if (cmd == "sudo -n true") {
            r.exit_code = host == TimelapseHost::NoSudo ? 1 : 0;
            if (r.exit_code != 0) {
                r.output = "sudo: a password is required\n";
            }
        } else if (cmd.find("apt-get install") != std::string::npos) {
            ffmpeg_installed().store(true);
        }
        return r;
    };
}

} // namespace helix::mock
