// SPDX-License-Identifier: GPL-3.0-or-later

// klipper-toolchanger mock hardware (HELIX_MOCK_AMS=toolchanger): the active tool,
// per-tool gcode offsets and the offset calibration macro.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
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
    // Reforge's ff_toolchange publishes klipper-toolchanger's objects under AFC too.
    return mock_toolchanger_selected() || is_mock_reforge_afc();
}

void MoonrakerClientMock::seed_tool_lane_records() {
    // klipper-toolchanger reports no material, colour, brand or weight: the
    // override store is the whole of filament identity there, and without these
    // records every tool renders at kAmsDefaultSlotColor, which makes colour and
    // ghost bugs invisible. Each record mirrors a spool from the mock Spoolman
    // inventory (id, vendor, material, colour, weights), so the active-spool card
    // and the tool it names cannot describe different filament.
    struct Lane {
        int spoolman_id;
        const char* material;
        const char* brand;
        const char* spool_name;
        const char* color;
        double remaining_g;
    };
    static constexpr Lane kMedusaLanes[] = {
        {2, "Silk PLA", "eSUN", "Silk Blue", "#26DCD9", 750.0},
        {4, "ABS", "Flashforge", "Fire Engine Red", "#D20000", 100.0},
        {13, "PETG", "Bambu Lab", "Translucent Green PETG", "#29A261", 1000.0},
        {5, "PETG", "Kingroon", "Signal Yellow", "#F4E111", 1000.0},
        {3, "ASA", "Elegoo", "Pop Blue", "#00AEFF", 500.0},
        {15, "PLA", "Bambu Lab", "Red PLA", "#C12E1F", 1000.0},
    };
    static_assert(std::size(kMedusaLanes) == kMedusaMaxTools);
    // Spools 1-4 in order, the lanes the mock AMS seeds on every other mode.
    static constexpr Lane kToolChangerLanes[] = {
        {1, "PLA", "Polymaker", "Jet Black", "#1A1A2E", 850.0},
        {2, "Silk PLA", "eSUN", "Silk Blue", "#26DCD9", 750.0},
        {3, "ASA", "Elegoo", "Pop Blue", "#00AEFF", 500.0},
        {4, "ABS", "Flashforge", "Fire Engine Red", "#D20000", 100.0},
    };

    const Lane* lanes = nullptr;
    int count = 0;
    if (mock_medusa_variant() != MedusaVariant::NONE) {
        lanes = kMedusaLanes;
        count = medusa_tool_count();
    } else if (mock_toolchanger_selected()) {
        lanes = kToolChangerLanes;
        count = static_cast<int>(std::size(kToolChangerLanes));
    }
    for (int i = 0; i < count; ++i) {
        const Lane& l = lanes[i];
        // Outer key style is T<n> (lane_key_style_for), inner "lane" is 0-based.
        mock_db_set("lane_data",
                    "T" + std::to_string(i), // DISPLAY_NUMBERING_OK: database key mirrors
                                             // the T<n> wire format, not a display label
                    json{{"lane", std::to_string(i)},
                         {"spoolman_id", l.spoolman_id},
                         {"material", l.material},
                         {"brand", l.brand},
                         {"spool_name", l.spool_name},
                         {"color", l.color},
                         {"remaining_weight_g", l.remaining_g},
                         {"total_weight_g", 1000.0}});
    }
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_toolchanger(const std::string& gcode) {
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t"));
    int tool = -1;
    if (cmd == "SELECT_TOOL") {
        // TOOL="tool T<n>" names the tool object; T=<n> is a tool number.
        if (const size_t named = gcode.find("TOOL=\"tool T"); named != std::string::npos) {
            tool = helix::text_io::parse_leading<int>(gcode.substr(named + 12)).value_or(-1);
        } else if (const size_t t = gcode.find(" T="); t != std::string::npos) {
            tool = helix::text_io::parse_leading<int>(gcode.substr(t + 3)).value_or(-1);
        }
        const int count = static_cast<int>(discovery_.hardware().tool_names().size());
        if (tool < 0 || tool >= count) {
            dispatch_gcode_response(fmt::format("!! Unknown tool: {}", gcode));
            return 1;
        }
    } else if (cmd != "UNSELECT_TOOL") {
        return std::nullopt;
    }
    spdlog::info("[MoonrakerClientMock] klipper-toolchanger: {} -> tool_number {}", cmd, tool);
    dispatch_status_update({{"toolchanger", {{"status", "changing"}}}});
    std::vector<helix::SharedCallback<void()>> steps;
    steps.emplace_back([this, tool] {
        if (tool >= 0) {
            dispatch_gcode_response(fmt::format("Selected tool {} (T{})", tool, tool));
        }
    });
    play_console_steps(std::move(steps), 800, [this, tool] {
        dispatch_status_update({{"toolchanger", {{"status", "ready"}}}});
        dispatch_toolchanger_tool(tool);
    });
    return 0;
}

void MoonrakerClientMock::dispatch_toolchanger_tool(int tool) {
    toolchanger_tool_ = tool;
    // klipper-toolchanger publishes `toolchanger.tool` as the gcode name itself,
    // so this is the wire value the mock reproduces, not a label the UI renders.
    json update = {
        {"toolchanger",
         {{"tool_number", tool},
          {"tool", tool >= 0 ? json("T" + std::to_string(tool)) // DISPLAY_NUMBERING_OK: wire
                             : json(nullptr)}}}};               // value, not a label
    dispatch_status_update(update);
    // An AmsBackendMock reads no toolchanger.tool_number; it follows the
    // simulator through this hook, the same one a print's tool changes use.
    if (tool >= 0) {
        notify_active_gcode_tool_observers(tool);
    }
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
