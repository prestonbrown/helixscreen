// SPDX-License-Identifier: GPL-3.0-or-later

// The U1 top module's purifier in the mock: its status object, the mode and fan
// commands, and the inner fan's work time accruing with the simulation clock.

#include "core/text_io.h"
#include "gcode_parser.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool is_command(const std::string& gcode, const std::string& command) {
    return gcode == command || gcode.find(command + " ") == 0;
}

} // namespace

nlohmann::json MoonrakerClientMock::purifier_status() const {
    if (printer_type_ != PrinterType::SNAPMAKER_U1) {
        return nlohmann::json::object();
    }
    std::lock_guard<std::mutex> lock(purifier_mutex_);
    const auto& p = purifier_;
    auto fan = [](double speed, double threshold) {
        return nlohmann::json{{"speed", speed}, {"delay", 180}, {"speed_threshold", threshold}};
    };
    return nlohmann::json{{"power_detected", p.present},
                          {"power_det_value", p.present ? 0.4 : 3.3},
                          {"mode", p.mode},
                          {"desired_chamber_temp", p.desired_c},
                          {"critical_chamber_temp", p.critical_c},
                          {"inner_fan_work_time", p.work_s},
                          {"inner_fan_rpm", p.inner > 0 ? 3200.0 * p.inner : 0.0},
                          {"exhaust_fan", fan(p.exhaust, 0.6)},
                          {"inner_fan", fan(p.inner, 0.6)}};
}

void MoonrakerClientMock::publish_purifier() {
    dispatch_status_update({{"purifier", purifier_status()}});
}

void MoonrakerClientMock::service_purifier(double dt) {
    if (printer_type_ != PrinterType::SNAPMAKER_U1) {
        return;
    }
    std::lock_guard<std::mutex> lock(purifier_mutex_);
    if ((purifier_.mode == 2 || purifier_.mode == 3) && purifier_.inner > 0) {
        purifier_.work_s += dt;
    }
}

// klippy/extras/purifier.py (2.x): with the top off every non-zero speed or mode
// is refused and only logged; lifting it off forces both fans off and the mode
// to idle. WORK= is accepted either way.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_u1_purifier(const std::string& gcode) {
    if (printer_type_ != PrinterType::SNAPMAKER_U1) {
        return std::nullopt;
    }
    auto number = [&gcode](std::string_view key) -> std::optional<double> {
        auto v = helix::gcode::gcode_param_value(gcode, key);
        if (!v) {
            return std::nullopt;
        }
        return helix::text_io::parse_leading<double>(*v);
    };
    if (is_command(gcode, "HELIX_MOCK_TOP_COVER")) {
        const auto present = number("PRESENT");
        {
            std::lock_guard<std::mutex> lock(purifier_mutex_);
            purifier_.present = present.value_or(1.0) != 0.0;
            if (!purifier_.present) {
                purifier_.mode = 0;
                purifier_.exhaust = purifier_.inner = 0.0;
            }
        }
        spdlog::info("[MoonrakerClientMock] U1 top cover {}",
                     present.value_or(1.0) != 0.0 ? "fitted" : "removed");
        publish_purifier();
        return 0;
    }
    if (is_command(gcode, "SET_PURIFIER_MODE")) {
        const int mode = static_cast<int>(number("MODE").value_or(-1));
        const double fan = number("FAN_SPEED").value_or(0.6);
        if (mode < 0 || mode > 3) {
            dispatch_gcode_response("!! Invalid purifier mode " + std::to_string(mode));
            return 0;
        }
        {
            std::unique_lock<std::mutex> lock(purifier_mutex_);
            if (!purifier_.present) {
                lock.unlock();
                dispatch_gcode_response("// [purifier] purifier not exist!");
                return 0;
            }
            purifier_.mode = mode;
            purifier_.exhaust = mode == 1 ? fan : 0.0;
            purifier_.inner = mode >= 2 ? fan : 0.0;
            purifier_.desired_c = mode == 0 ? 0.0 : number("DESIRE_TEMP").value_or(42.0);
            purifier_.critical_c = mode == 1 ? number("ALARM_TEMP").value_or(43.0) : 0.0;
        }
        spdlog::info("[MoonrakerClientMock] U1 SET_PURIFIER_MODE {}", mode);
        publish_purifier();
        return 0;
    }
    if (is_command(gcode, "SET_PURIFIER")) {
        const auto fan = helix::gcode::gcode_param_value(gcode, "FAN");
        if (!fan || (*fan != "exhaust" && *fan != "inner")) {
            dispatch_gcode_response("!! [purifier] fan name error!");
            return 0;
        }
        {
            std::unique_lock<std::mutex> lock(purifier_mutex_);
            if (auto work = number("WORK")) {
                purifier_.work_s = std::max(0.0, *work);
            }
            if (auto speed = number("SPEED")) {
                if (*speed > 0 && !purifier_.present) {
                    lock.unlock();
                    dispatch_gcode_response("!! [purifier] purifier not exist!");
                    return 0;
                }
                (*fan == "exhaust" ? purifier_.exhaust : purifier_.inner) =
                    std::clamp(*speed, 0.0, 1.0);
            }
        }
        publish_purifier();
        return 0;
    }
    return std::nullopt;
}
