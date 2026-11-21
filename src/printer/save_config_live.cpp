// SPDX-License-Identifier: GPL-3.0-or-later

#include "save_config_live.h"

#include "printer_discovery.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

#include "hv/json.hpp"

namespace helix::save_config {
namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool one_of(std::string_view s, std::initializer_list<std::string_view> names) {
    return std::find(names.begin(), names.end(), s) != names.end();
}

/// Every option of @p options is in @p allowed.
bool only_options(const nlohmann::json& options, std::initializer_list<std::string_view> allowed) {
    if (!options.is_object()) {
        return false;
    }
    for (const auto& [option, value] : options.items()) {
        if (!one_of(option, allowed)) {
            return false;
        }
    }
    return true;
}

// --- Bed mesh profiles --------------------------------------------------------
// bed_mesh.py ProfileManager.save_profile() copies the mesh into self.profiles
// as it stages [bed_mesh <name>], and remove_profile() drops it from
// self.profiles as it stages the removal (null). BED_MESH_CALIBRATE calls
// set_mesh() on the new mesh before save_profile(). The file write adds
// nothing the running Klipper does not already have.
bool bed_mesh_profile_live(std::string_view section, const nlohmann::json& options,
                           const PrinterDiscovery& /*hw*/) {
    return starts_with(section, "bed_mesh ") && (options.is_null() || options.is_object());
}

// --- Heater PID ---------------------------------------------------------------
// pid_calibrate.py PIDCalibrate.cmd_PID_CALIBRATE switches the heater to the
// tuned control (set_control(heater.lookup_control(profile, True))) and then
// heaters.py ProfileManager.save_profile() stages the ACTIVE control's values
// into [<heater>] (the default profile) or [pid_profile <heater> <name>].
// MPC does not qualify: control_mpc.py's MPC_CALIBRATE restores the old control
// in its finally block, so its control: mpc values only take effect on restart.
bool is_heater_section(std::string_view section) {
    if (section == "heater_bed" || starts_with(section, "heater_generic ") ||
        starts_with(section, "pid_profile ")) {
        return true;
    }
    if (!starts_with(section, "extruder")) {
        return false;
    }
    const std::string_view rest = section.substr(8);
    return std::all_of(rest.begin(), rest.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

bool heater_pid_live(std::string_view section, const nlohmann::json& options,
                     const PrinterDiscovery& /*hw*/) {
    if (!is_heater_section(section) ||
        !only_options(options, {"pid_version", "pid_target", "pid_tolerance", "control",
                                "smooth_time", "pid_kp", "pid_ki", "pid_kd", "inner_pid_kp",
                                "inner_pid_ki", "inner_pid_kd", "max_delta"})) {
        return false;
    }
    const auto control = options.find("control");
    return control == options.end() ||
           (control->is_string() &&
            one_of(control->get<std::string>(), {"pid", "pid_v", "dual_loop_pid", "watermark"}));
}

// --- Input shaper -------------------------------------------------------------
// resonance_tester.py ResonanceTester.cmd_SHAPER_CALIBRATE calls
// shaper_calibrate.py ShaperCalibrate.apply_params() (SET_INPUT_SHAPER) before
// save_params() stages [input_shaper] shaper_type_<axis>/shaper_freq_<axis>,
// but only when an [input_shaper] is configured; without one the section is new
// and nothing shapes until a restart.
bool input_shaper_live(std::string_view section, const nlohmann::json& options,
                       const PrinterDiscovery& hw) {
    if (section != "input_shaper" || !only_options(options, {"shaper_type_x", "shaper_type_y",
                                                             "shaper_freq_x", "shaper_freq_y"})) {
        return false;
    }
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), "input_shaper") != objects.end();
}

// --- Tool offsets (klipper-toolchanger) ---------------------------------------
// klipper/extras/tool.py Tool.set_parameter() applies gcode_{x,y,z}_offset to
// the tool object (_apply_param) when SET_TOOL_PARAMETER runs;
// SAVE_TOOL_PARAMETER's save_parameter() only stages that same value into
// [tool <name>].
bool tool_offset_live(std::string_view section, const nlohmann::json& options,
                      const PrinterDiscovery& /*hw*/) {
    return starts_with(section, "tool ") &&
           only_options(options, {"gcode_x_offset", "gcode_y_offset", "gcode_z_offset"});
}

using LiveRow = bool (*)(std::string_view section, const nlohmann::json& options,
                         const PrinterDiscovery& hw);

// Not here, so they restart: [probe] (and every probe flavor) z_offset from
// probe.py probe_calibrate_finalize / Z_OFFSET_APPLY_PROBE, and [stepper_z]
// position_endstop from manual_probe.py Z_ENDSTOP_CALIBRATE /
// Z_OFFSET_APPLY_ENDSTOP: both only call configfile.set(), and the running
// Klipper read the old value at startup.
constexpr LiveRow kLiveRows[] = {
    bed_mesh_profile_live,
    heater_pid_live,
    input_shaper_live,
    tool_offset_live,
};

bool item_live(const std::string& section, const nlohmann::json& options,
               const PrinterDiscovery& hw) {
    return std::any_of(std::begin(kLiveRows), std::end(kLiveRows),
                       [&](LiveRow row) { return row(section, options, hw); });
}

} // namespace

bool restart_required(const PrinterDiscovery& hw, const nlohmann::json& pending_items) {
    if (!hw.save_config_takes_restart_param() || !pending_items.is_object()) {
        return true;
    }
    for (const auto& [section, options] : pending_items.items()) {
        if (!item_live(section, options, hw)) {
            return true;
        }
    }
    return false;
}

} // namespace helix::save_config
