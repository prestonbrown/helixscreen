// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_host_mock.h"

#include "core/text_io.h"
#include "helix_fs.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

// Long enough to see each step's status on screen.
constexpr auto kStepDelay = std::chrono::milliseconds(800);
} // namespace

namespace {
/// Calls @p fn on each comma-separated word of @p value until it returns true.
template <typename Fn> bool any_word(const char* value, Fn fn) {
    std::string_view rest = value ? value : "";
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        if (fn(text_io::trim(rest.substr(0, comma)))) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return false;
}
} // namespace

TimelapseHost parse_timelapse_host(const char* value) {
    TimelapseHost host = TimelapseHost::Present;
    any_word(value, [&](std::string_view w) {
        if (w == "absent") {
            host = TimelapseHost::Absent;
            return true;
        }
        if (w == "no-ffmpeg") {
            host = TimelapseHost::NoFfmpeg;
            return true;
        }
        return false;
    });
    return host;
}

bool parse_timelapse_helix_macros(const char* value) {
    return any_word(value, [](std::string_view w) { return w == "helix-macros"; });
}

bool timelapse_helix_macros_loaded() {
    return parse_timelapse_helix_macros(std::getenv("HELIX_MOCK_TIMELAPSE"));
}

std::string& timelapse_mock_helix_macros_file() {
    static std::string content = "# helix_macros (mock, HELIX_MOCK_TIMELAPSE=helix-macros)\n";
    return content;
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
        if (std::find(argv.begin(), argv.end(), "clone") != argv.end()) {
            const std::string dest = argv.back();
            r.exit_code = fs::create_directories(dest + "/component") &&
                                  text_io::write_file(dest + "/component/timelapse.py", "")
                              ? 0
                              : 1;
        } else if (cmd.find("--link-timelapse") != std::string::npos) {
            linked().store(true);
        } else if (cmd.find("command -v") != std::string::npos &&
                   cmd.find("ffmpeg") != std::string::npos) {
            r.exit_code = host == TimelapseHost::NoFfmpeg ? 1 : 0;
        }
        return r;
    };
}

} // namespace helix::mock
