// SPDX-License-Identifier: GPL-3.0-or-later

// klipper-toolchanger mock hardware (HELIX_MOCK_AMS=toolchanger): the active tool,
// per-tool gcode offsets and the offset calibration macro.

#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace helix;
using mock_internal::effective_mock_ams_env;

bool MoonrakerClientMock::mock_toolchanger_selected() {
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc";
}

bool MoonrakerClientMock::is_mock_toolchanger() const {
    return mock_toolchanger_selected();
}

void MoonrakerClientMock::dispatch_toolchanger_tool(int tool) {
    // klipper-toolchanger publishes `toolchanger.tool` as the gcode name itself,
    // so this is the wire value the mock reproduces, not a label the UI renders.
    json update = {
        {"toolchanger",
         {{"tool_number", tool},
          {"tool", "T" + std::to_string(tool)}}}}; // DISPLAY_NUMBERING_OK: wire value, not a label
    dispatch_status_update(update);
    // The mock AMS backend does not read toolchanger.tool_number (a production
    // backend does); it follows the simulator through this hook instead, the
    // same one a print's tool changes use. Without it ToolState's active tool
    // never moves under --test and no row ever reads Measuring.
    notify_active_gcode_tool_observers(tool);
}

void MoonrakerClientMock::apply_calibrated_tool_offset(int tool, double x, double y, double z) {
    const double values[] = {x, y, z};
    for (helix::Axis axis : helix::kAllAxes) {
        const double value = values[helix::axis_index(axis)];
        {
            std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
            tool_offsets_[tool][axis] = value;
        }
        dispatch_tool_update(tool, axis);
        char text[32];
        std::snprintf(text, sizeof(text), "%.6g", value);
        stage_config_change("tool T" + std::to_string(tool), tool_offset_param(axis), text);
    }
    spdlog::info("[MoonrakerClientMock] T{} offsets calibrated: x={:.4f} y={:.4f} z={:.4f} "
                 "(staged, awaiting SAVE_CONFIG)",
                 tool, x, y, z);
}

bool MoonrakerClientMock::simulate_tool_offset_calibration(
    const std::string& script, helix::SharedCallback<void(const nlohmann::json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    // Whole-line match: the macro takes no parameters worth modelling.
    std::string cmd = script;
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
        cmd.pop_back();
    }
    if (cmd.rfind("CALIBRATE_TOOL_OFFSETS", 0) != 0) {
        return false;
    }

    const int tool_count = static_cast<int>(discovery_.hardware().tool_names().size());
    if (tool_count < 1) {
        if (error_cb) {
            MoonrakerError err;
            err.message = "Unknown command:\"CALIBRATE_TOOL_OFFSETS\"";
            error_cb(err);
        }
        return true;
    }

    int fail_tool = -1;
    if (const char* env = std::getenv("HELIX_MOCK_TOOL_CAL_FAIL")) {
        try {
            fail_tool = std::stoi(env);
        } catch (const std::exception&) {
            fail_tool = -1;
        }
    }
    spdlog::info(
        "[MoonrakerClientMock] CALIBRATE_TOOL_OFFSETS: {} tools{}", tool_count,
        fail_tool >= 0 ? fmt::format(", T{} will fail (HELIX_MOCK_TOOL_CAL_FAIL)", fail_tool) : "");

    // Two ticks per tool - select, then measure - and a final park on T0. A
    // failing tool's measure tick ends the run instead.
    const auto select = [this](int tool) {
        dispatch_gcode_response(fmt::format("Selected tool {} (T{})", tool, tool));
        dispatch_toolchanger_tool(tool);
    };
    // Measuring. The probe prints a contact per sample; the numbers only
    // need to look like a nozzle near the sensor.
    const double sensor_x = 229.0, sensor_y = 2.5, sensor_z = 1.25;
    const auto probe = [this, sensor_x, sensor_y, sensor_z](int tool) {
        // Distinct from the per-tool seed on EVERY axis (see tool_offset()):
        // a measured value equal to the seed leaves that axis clean after a
        // run, so nothing stages it and the save path's handling of it is
        // never exercised.
        const double dx = 0.12 * tool, dy = -0.07 * tool, dz = -0.03 * tool;
        for (int sample = 0; sample < 3; ++sample) {
            dispatch_gcode_response(fmt::format("Probe made contact at {:.6f},{:.6f},{:.6f}",
                                                sensor_x + dx + 0.001 * sample, sensor_y + dy,
                                                sensor_z + dz));
        }
    };
    const auto measure = [this, probe, sensor_x, sensor_y, sensor_z](int tool) {
        probe(tool);
        if (tool == 0) {
            dispatch_gcode_response(fmt::format("Sensor location at {:.6f},{:.6f},{:.6f}", sensor_x,
                                                sensor_y, sensor_z));
            return;
        }
        const double dx = 0.12 * tool, dy = -0.07 * tool, dz = -0.03 * tool;
        dispatch_gcode_response(fmt::format("Tool offset is {:.6f},{:.6f},{:.6f}", dx, dy, dz));
        apply_calibrated_tool_offset(tool, dx, dy, dz);
    };

    const bool fails = fail_tool >= 0 && fail_tool < tool_count;
    const int last_tool = fails ? fail_tool : tool_count - 1;
    std::vector<helix::SharedCallback<void()>> steps;
    for (int tool = 0; tool <= last_tool; ++tool) {
        steps.emplace_back([select, tool] { select(tool); });
        if (tool != fail_tool) {
            steps.emplace_back([measure, tool] { measure(tool); });
        }
    }

    helix::SharedCallback<void()> on_done;
    if (fails) {
        on_done = [this, probe, fail_tool, error_cb = std::move(error_cb)] {
            probe(fail_tool);
            const std::string error = "Probe samples exceed samples_tolerance";
            dispatch_gcode_response("!! " + error);
            if (error_cb) {
                MoonrakerError err;
                err.message = error;
                error_cb(err);
            }
        };
    } else {
        on_done = [this, success_cb = std::move(success_cb)] {
            // Park on the reference tool, as the macro's last SELECT_TOOL does.
            dispatch_gcode_response("Selected tool 0 (T0)");
            dispatch_toolchanger_tool(0);
            if (success_cb) {
                success_cb(json{{"result", "ok"}});
            }
        };
    }
    play_console_steps(std::move(steps), 600, std::move(on_done));
    return true;
}

void MoonrakerClientMock::dispatch_tool_update(int tool, helix::Axis axis) {
    double value = 0.0;
    {
        std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
        auto tool_it = tool_offsets_.find(tool);
        if (tool_it == tool_offsets_.end()) {
            return;
        }
        auto axis_it = tool_it->second.find(axis);
        if (axis_it == tool_it->second.end()) {
            return;
        }
        value = axis_it->second;
    }
    // Only the field that changed, matching Moonraker: it republishes just the
    // deltas, and code that assumes a full object here is code that would break
    // against a real printer.
    json update = {{"tool T" + std::to_string(tool), {{tool_offset_param(axis), value}}}};
    dispatch_status_update(update);
}

const char* MoonrakerClientMock::tool_offset_param(helix::Axis axis) {
    static constexpr const char* names[] = {"gcode_x_offset", "gcode_y_offset", "gcode_z_offset"};
    return names[helix::axis_index(axis)];
}

std::optional<helix::Axis> MoonrakerClientMock::tool_offset_axis(const std::string& param) {
    for (helix::Axis axis : helix::kAllAxes) {
        if (param == tool_offset_param(axis)) {
            return axis;
        }
    }
    return std::nullopt;
}

double MoonrakerClientMock::tool_offset_seed(int tool, helix::Axis axis) {
    // Distinct per-tool AND per-axis. All-zero would make "every tool shows the
    // same number" — the characteristic per-tool display bug — look correct,
    // and equal X/Y/Z would hide an axis mix-up the same way. T0 is the
    // reference tool and sits at zero on every axis.
    static constexpr double seed_per_tool[] = {0.100, -0.050, -0.025};
    return seed_per_tool[helix::axis_index(axis)] * tool;
}

double MoonrakerClientMock::tool_offset(int tool, helix::Axis axis) const {
    std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
    auto tool_it = tool_offsets_.find(tool);
    if (tool_it != tool_offsets_.end()) {
        auto axis_it = tool_it->second.find(axis);
        if (axis_it != tool_it->second.end()) {
            return axis_it->second;
        }
    }
    return tool_offset_seed(tool, axis);
}
