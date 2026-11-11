// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The --real-ams Happy Hare mock's v4 controls: printer.mmu's sync_feedback_enabled
// and fans, and the MMU_SYNC_FEEDBACK / MMU_FAN commands that change them.

#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <optional>
#include <string>

using namespace helix;

namespace {

/// The integer value of `KEY=` on @p gcode, or nullopt when it is absent.
std::optional<int> int_param(const std::string& gcode, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t pos = gcode.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return std::atoi(gcode.c_str() + pos + needle.size());
}

bool is_command(const std::string& gcode, const char* command) {
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t"));
    return cmd == command;
}

} // namespace

nlohmann::json MoonrakerClientMock::happy_hare_mmu_status() const {
    nlohmann::json mmu = mock_internal::get_mock_mmu_status();
    mmu["sync_feedback_enabled"] = hh_sync_feedback_enabled_.load();
    mmu["fans"] = nlohmann::json::array({{{"unit", "unit0"},
                                          {"first_gate", 0},
                                          {"per_gate", false},
                                          {"enabled", hh_fan_enabled_.load()},
                                          {"modes", {hh_fan_mode_.load()}}}});
    return mmu;
}

// Parameters as Happy Hare's own commands take them (extras/mmu/commands/
// mmu_sync_feedback.py, mmu_fan.py); RESET, ADJUST_TENSION and AUTOTUNE change
// nothing the status reports, so they are only acknowledged.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_happy_hare(const std::string& gcode) {
    if (is_command(gcode, "MMU_SYNC_FEEDBACK")) {
        if (const auto enable = int_param(gcode, "ENABLE")) {
            hh_sync_feedback_enabled_ = *enable != 0;
        }
    } else if (is_command(gcode, "MMU_FAN")) {
        if (const auto enable = int_param(gcode, "ENABLE")) {
            hh_fan_enabled_ = *enable != 0;
        }
        if (const auto forced = int_param(gcode, "FAN_FORCED");
            forced && *forced >= 0 && *forced <= 2) {
            hh_fan_mode_ = *forced;
        }
    } else {
        return std::nullopt;
    }
    spdlog::debug("[MoonrakerClientMock] Happy Hare: {}", gcode);
    const nlohmann::json mmu = happy_hare_mmu_status();
    dispatch_status_update(
        {{"mmu",
          {{"sync_feedback_enabled", mmu["sync_feedback_enabled"]}, {"fans", mmu["fans"]}}}});
    return 0;
}
