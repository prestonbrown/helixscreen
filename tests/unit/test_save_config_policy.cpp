// SPDX-License-Identifier: GPL-3.0-or-later

// Whether a SAVE_CONFIG may skip the Klipper restart. Only a firmware that takes
// RESTART=0 (Kalico) may, and only when every pending item is one the command
// that staged it already applied to the running Klipper. Anything else keeps
// its old value until a restart, so it restarts.

#include "moonraker_advanced_api.h"
#include "printer_discovery.h"
#include "save_config_live.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using helix::save_config::restart_required;
using nlohmann::json;

namespace {

PrinterDiscovery kalico(json objects = json::array()) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    hw.apply_printer_info(json{{"app", "Kalico"}, {"software_version", "v2025.1"}});
    return hw;
}

const json kMesh = {{"bed_mesh default", {{"version", "1"}, {"points", "0.1, 0.2"}}}};
const json kZOffset = {{"probe", {{"z_offset", "1.234"}}}};

} // namespace

TEST_CASE("save config: mainline Klipper always restarts", "[save_config_policy]") {
    PrinterDiscovery hw;
    hw.apply_printer_info(json{{"app", "Klipper"}, {"software_version", "v0.13.0"}});
    CHECK(restart_required(hw, kMesh));
    CHECK(restart_required(hw, json::object()));
    // Never send RESTART=0 to a firmware not known to take it.
    CHECK(restart_required(PrinterDiscovery{}, kMesh));
}

TEST_CASE("save config: on Kalico a bed mesh profile is already live", "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    CHECK_FALSE(restart_required(hw, kMesh));
    CHECK_FALSE(restart_required(hw, json{{"bed_mesh old", nullptr}})); // a removed profile
    CHECK_FALSE(restart_required(hw, json::object()));                  // nothing pending
}

TEST_CASE("save config: on Kalico a z_offset or endstop position restarts",
          "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    CHECK(restart_required(hw, kZOffset));
    CHECK(restart_required(hw, json{{"stepper_z", {{"position_endstop", "0.5"}}}}));
    // One item that is not live is enough, whatever else is pending.
    json mixed = kMesh;
    mixed.update(kZOffset);
    CHECK(restart_required(hw, mixed));
}

TEST_CASE("save config: on Kalico an unknown section or a malformed set restarts",
          "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    CHECK(restart_required(hw, json{{"axis_twist_compensation", {{"z_compensations", "0"}}}}));
    CHECK(restart_required(hw, json{{"bed_mesh", {{"probe_count", "5,5"}}}}));
    CHECK(restart_required(hw, json{{"extruder", nullptr}}));
    CHECK(restart_required(hw, json::array()));
}

TEST_CASE("save config: on Kalico a PID result is live and an MPC result is not",
          "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    CHECK_FALSE(restart_required(
        hw, json{{"extruder", {{"pid_version", "1"}, {"control", "pid"}, {"pid_kp", "22.1"}}},
                 {"heater_bed", {{"control", "pid_v"}, {"pid_ki", "1.2"}}},
                 {"pid_profile extruder hot", {{"pid_kd", "101.0"}}}}));
    CHECK(restart_required(hw, json{{"extruder", {{"control", "mpc"}}}}));
    CHECK(restart_required(hw, json{{"extruder", {{"block_heat_capacity", "18.4"}}}}));
    // A heater option PID calibration never stages.
    CHECK(restart_required(hw, json{{"extruder", {{"rotation_distance", "22"}}}}));
    // A section that only starts like a heater.
    CHECK(restart_required(hw, json{{"extruder_stepper belt", {{"pid_kp", "1"}}}}));
}

TEST_CASE("save config: on Kalico an input shaper result is live only with [input_shaper]",
          "[save_config_policy]") {
    const json shaper = {{"input_shaper", {{"shaper_type_x", "mzv"}, {"shaper_freq_x", "52.0"}}}};
    CHECK_FALSE(restart_required(kalico(json::array({"input_shaper"})), shaper));
    CHECK(restart_required(kalico(), shaper));
}

TEST_CASE("save config: on Kalico a toolchanger tool offset is live", "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    CHECK_FALSE(restart_required(hw, json{{"tool T1", {{"gcode_x_offset", "0.12"}}}}));
    CHECK(restart_required(hw, json{{"tool T1", {{"params_park_x", "10"}}}}));
}

TEST_CASE("save config: a cleared discovery forgets the firmware", "[save_config_policy]") {
    PrinterDiscovery hw = kalico();
    hw.clear();
    CHECK(restart_required(hw, kMesh));
}

TEST_CASE("save config: on Kalico a reply without the pending items restarts",
          "[save_config_policy]") {
    const PrinterDiscovery hw = kalico();
    const json reply = {
        {"result", {{"status", {{"configfile", {{"save_config_pending_items", kMesh}}}}}}}};
    CHECK_FALSE(restart_required(hw, MoonrakerAdvancedAPI::pending_items_from(reply)));
    const json partial = {{"result", {{"status", {{"configfile", json::object()}}}}}};
    CHECK(restart_required(hw, MoonrakerAdvancedAPI::pending_items_from(partial)));
}
