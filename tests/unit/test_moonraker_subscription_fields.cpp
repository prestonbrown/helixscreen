// SPDX-License-Identifier: GPL-3.0-or-later
//
// Locks the per-object field lists in
// MoonrakerDiscoverySequence::build_subscription_objects against parser drift.
//
// The contract under test: each Klipper object subscription must include
// every field the corresponding HelixScreen parser reads. A missing field
// causes a silent regression — the value never updates past whatever the
// subscription response carried as initial state.
//
// Each test names the specific parser file it is mirroring; if you add a new
// field read in that parser, update both the subscription and this test.

#include "moonraker_discovery_sequence.h"
#include "printer_discovery.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::MoonrakerDiscoverySequence;
using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

// All `printer.objects.list` entries we plumb through both the discovery-
// sequence's own vectors and the unified PrinterDiscovery snapshot.
struct DiscoveryFixture {
    std::vector<std::string> heaters;
    std::vector<std::string> sensors;
    std::vector<std::string> fans;
    std::vector<std::string> leds;
    std::vector<std::string> afc_objects;
    std::vector<std::string> filament_sensors;
    std::vector<std::string> mcus;
    json all_objects = json::array();

    void add(const std::string& name, std::initializer_list<const char*> categories) {
        all_objects.push_back(name);
        for (const auto& cat : categories) {
            std::string c = cat;
            if (c == "heater")
                heaters.push_back(name);
            else if (c == "sensor")
                sensors.push_back(name);
            else if (c == "fan")
                fans.push_back(name);
            else if (c == "led")
                leds.push_back(name);
            else if (c == "afc")
                afc_objects.push_back(name);
            else if (c == "filament_sensor")
                filament_sensors.push_back(name);
            else if (c == "mcu")
                mcus.push_back(name);
        }
    }

    json build() const {
        PrinterDiscovery hw;
        hw.parse_objects(all_objects);
        return MoonrakerDiscoverySequence::build_subscription_objects(
            hw, heaters, sensors, fans, leds, afc_objects, filament_sensors, mcus);
    }
};

bool has_field(const json& subs, const std::string& obj, const std::string& field) {
    if (!subs.contains(obj))
        return false;
    const auto& fields = subs[obj];
    if (!fields.is_array())
        return false;
    return std::any_of(fields.begin(), fields.end(),
                       [&](const json& f) { return f.is_string() && f == field; });
}

} // namespace

TEST_CASE("Subscription: core motion objects subscribe every field their parsers read",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    SECTION("toolhead — printer_motion_state.cpp lines 70-107") {
        // Parser reads: position, max_velocity, homed_axes,
        //               axis_minimum, axis_maximum (kinematic envelope)
        REQUIRE(has_field(subs, "toolhead", "position"));
        REQUIRE(has_field(subs, "toolhead", "max_velocity"));
        REQUIRE(has_field(subs, "toolhead", "homed_axes"));
        REQUIRE(has_field(subs, "toolhead", "axis_minimum"));
        REQUIRE(has_field(subs, "toolhead", "axis_maximum"));
    }

    SECTION("gcode_move — printer_motion_state.cpp lines 117-167") {
        // Parser reads: gcode_position, speed, speed_factor, extrude_factor, homing_origin
        REQUIRE(has_field(subs, "gcode_move", "gcode_position"));
        REQUIRE(has_field(subs, "gcode_move", "speed"));
        REQUIRE(has_field(subs, "gcode_move", "speed_factor"));
        REQUIRE(has_field(subs, "gcode_move", "extrude_factor"));
        REQUIRE(has_field(subs, "gcode_move", "homing_origin"));
    }

    SECTION("motion_report — printer_motion_state.cpp line 171") {
        REQUIRE(has_field(subs, "motion_report", "live_extruder_velocity"));
    }

    SECTION("webhooks — printer_state.cpp lines 441-463") {
        REQUIRE(has_field(subs, "webhooks", "state"));
        REQUIRE(has_field(subs, "webhooks", "state_message"));
    }

    SECTION("display_status — printer_print_state.cpp + print_start_collector.cpp") {
        REQUIRE(has_field(subs, "display_status", "message"));
        REQUIRE(has_field(subs, "display_status", "progress"));
    }
}

TEST_CASE("Subscription: print_stats fields cover printer_print_state reads",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    // printer_print_state reads: state, filename, info, estimated_time,
    //   filament_used, print_duration, total_duration
    REQUIRE(has_field(subs, "print_stats", "state"));
    REQUIRE(has_field(subs, "print_stats", "filename"));
    REQUIRE(has_field(subs, "print_stats", "info"));
    REQUIRE(has_field(subs, "print_stats", "estimated_time"));
    REQUIRE(has_field(subs, "print_stats", "filament_used"));
    REQUIRE(has_field(subs, "print_stats", "print_duration"));
    REQUIRE(has_field(subs, "print_stats", "total_duration"));
}

TEST_CASE("Subscription: virtual_sdcard fields cover printer_print_state reads",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    // virtual_sdcard provides: progress (always), layer / layer_count
    // (fallback for SET_PRINT_STATS_INFO-less slicers — see
    // PrinterPrintState::update_from_status precedence logic), and
    // is_active (drives Resume vs Restart UX when SD playback is
    // deactivated by a firmware exception).
    REQUIRE(has_field(subs, "virtual_sdcard", "progress"));
    REQUIRE(has_field(subs, "virtual_sdcard", "layer"));
    REQUIRE(has_field(subs, "virtual_sdcard", "layer_count"));
    REQUIRE(has_field(subs, "virtual_sdcard", "is_active"));
}

TEST_CASE("Subscription: heaters narrow to {temperature, target}", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("extruder", {"heater"});
    fx.add("extruder1", {"heater"});
    fx.add("heater_bed", {"heater"});
    fx.add("heater_generic chamber", {"heater"});

    json subs = fx.build();

    for (const auto& heater : {"extruder", "extruder1", "heater_bed", "heater_generic chamber"}) {
        CAPTURE(heater);
        REQUIRE(has_field(subs, heater, "temperature"));
        REQUIRE(has_field(subs, heater, "target"));
        // Should NOT contain extra fields — narrowing contract
        REQUIRE(subs[heater].size() == 2);
    }
}

TEST_CASE("Subscription: temp sensors narrow to {temperature, humidity}",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("temperature_sensor enclosure", {"sensor"});
    fx.add("tmc2240 stepper_x", {"sensor"});
    fx.add("tmc5160 stepper_y", {"sensor"});

    json subs = fx.build();

    // "humidity" is requested alongside "temperature" so humidity-capable env
    // sensors (Happy Hare filament dryer) stream %RH; temperature-only sensors
    // simply have Moonraker omit the absent field. Narrowing contract: no MORE
    // than these two fields stream back.
    for (const auto& s :
         {"temperature_sensor enclosure", "tmc2240 stepper_x", "tmc5160 stepper_y"}) {
        CAPTURE(s);
        REQUIRE(has_field(subs, s, "temperature"));
        REQUIRE(has_field(subs, s, "humidity"));
        REQUIRE(subs[s].size() == 2);
    }
}

TEST_CASE("Subscription: humidity sensor chips are subscribed with humidity",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("htu21d box", {"sensor"});
    fx.add("bme280 chamber", {"sensor"});
    fx.add("sht3x enclosure", {"sensor"});
    fx.add("aht10 dryer", {"sensor"});
    fx.add("aht20 box", {"sensor"});
    // QIDI's box temp/humidity chip ("aht20_f heater_box1" → {temperature,
    // humidity}); confirmed on a stock Q2 (#1022). Without humidity in its
    // subscription the dryer overlay never gets box humidity.
    fx.add("aht20_f heater_box1", {"sensor"});

    json subs = fx.build();

    for (const auto& s : {"htu21d box", "bme280 chamber", "sht3x enclosure", "aht10 dryer",
                          "aht20 box", "aht20_f heater_box1"}) {
        CAPTURE(s);
        REQUIRE(has_field(subs, s, "temperature"));
        REQUIRE(has_field(subs, s, "humidity"));
    }
}

TEST_CASE("Subscription: fan field shape varies by object type", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("fan", {"fan"});
    fx.add("heater_fan hotend_fan", {"fan"});
    fx.add("fan_generic part_cooling", {"fan"});
    fx.add("controller_fan board_fan", {"fan"});
    fx.add("temperature_fan chamber", {"sensor", "fan"});
    fx.add("output_pin fan0", {"fan"});

    json subs = fx.build();

    SECTION("standard fans: {speed}") {
        for (const auto& f : {"fan", "heater_fan hotend_fan", "fan_generic part_cooling",
                              "controller_fan board_fan"}) {
            CAPTURE(f);
            REQUIRE(has_field(subs, f, "speed"));
            REQUIRE(subs[f].size() == 1);
        }
    }

    SECTION("temperature_fan: {temperature, target, speed} (sensor + fan union)") {
        REQUIRE(has_field(subs, "temperature_fan chamber", "temperature"));
        REQUIRE(has_field(subs, "temperature_fan chamber", "target"));
        REQUIRE(has_field(subs, "temperature_fan chamber", "speed"));
    }

    SECTION("output_pin fan: {value} (Creality-style)") {
        REQUIRE(has_field(subs, "output_pin fan0", "value"));
        REQUIRE(subs["output_pin fan0"].size() == 1);
    }
}

TEST_CASE("Subscription: LEDs narrow by type", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("neopixel chamber", {"led"});
    fx.add("dotstar accent", {"led"});
    fx.add("led status", {"led"});
    fx.add("output_pin lamp", {"led"});

    json subs = fx.build();

    SECTION("native LEDs: {color_data}") {
        for (const auto& led : {"neopixel chamber", "dotstar accent", "led status"}) {
            CAPTURE(led);
            REQUIRE(has_field(subs, led, "color_data"));
            REQUIRE(subs[led].size() == 1);
        }
    }

    SECTION("output_pin LEDs: {value}") {
        REQUIRE(has_field(subs, "output_pin lamp", "value"));
        REQUIRE(subs["output_pin lamp"].size() == 1);
    }
}

TEST_CASE("Subscription: led_effect objects narrow to {enabled} (per-frame storm)",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("led_effect breathing", {});
    fx.add("led_effect fire", {});
    fx.add("neopixel chamber", {"led"});

    json subs = fx.build();

    REQUIRE(has_field(subs, "led_effect breathing", "enabled"));
    REQUIRE(has_field(subs, "led_effect fire", "enabled"));
    // Must be exactly {enabled} — anything else burns the per-frame win
    REQUIRE(subs["led_effect breathing"].size() == 1);
    REQUIRE(subs["led_effect fire"].size() == 1);
}

TEST_CASE("Subscription: AFC objects narrow per type", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("AFC", {"afc"});
    fx.add("AFC_stepper lane1", {"afc"});
    fx.add("AFC_lane lane2", {"afc"});
    fx.add("AFC_hub turtle1", {"afc"});
    fx.add("AFC_buffer buffer1", {"afc"});
    fx.add("AFC_extruder extruder", {"afc"});
    fx.add("AFC_BoxTurtle turtle1", {"afc"});
    fx.add("AFC_led lane1_led", {"afc"});

    json subs = fx.build();

    SECTION("AFC top-level reads ams_backend_afc.cpp parse_afc_state") {
        REQUIRE(has_field(subs, "AFC", "current_load"));
        REQUIRE(has_field(subs, "AFC", "current_lane"));
        REQUIRE(has_field(subs, "AFC", "current_state"));
        REQUIRE(has_field(subs, "AFC", "filament_loaded"));
        REQUIRE(has_field(subs, "AFC", "lane_loaded"));
        REQUIRE(has_field(subs, "AFC", "system"));
        REQUIRE(has_field(subs, "AFC", "units"));
    }

    SECTION("AFC_stepper / AFC_lane: parse_afc_stepper fields") {
        for (const auto& obj : {"AFC_stepper lane1", "AFC_lane lane2"}) {
            CAPTURE(obj);
            REQUIRE(has_field(subs, obj, "prep"));
            REQUIRE(has_field(subs, obj, "load"));
            REQUIRE(has_field(subs, obj, "color"));
            REQUIRE(has_field(subs, obj, "material"));
            REQUIRE(has_field(subs, obj, "spool_id"));
            REQUIRE(has_field(subs, obj, "tool_loaded"));
            REQUIRE(has_field(subs, obj, "weight"));
            REQUIRE(has_field(subs, obj, "buffer_status"));
            REQUIRE(has_field(subs, obj, "filament_status"));
        }
    }

    SECTION("AFC_hub: parse_afc_hub fields") {
        REQUIRE(has_field(subs, "AFC_hub turtle1", "state"));
        REQUIRE(has_field(subs, "AFC_hub turtle1", "afc_bowden_length"));
    }

    SECTION("AFC_buffer: parse_afc_buffer fields") {
        REQUIRE(has_field(subs, "AFC_buffer buffer1", "state"));
        REQUIRE(has_field(subs, "AFC_buffer buffer1", "distance_to_fault"));
        REQUIRE(has_field(subs, "AFC_buffer buffer1", "error_sensitivity"));
        REQUIRE(has_field(subs, "AFC_buffer buffer1", "fault_detection_enabled"));
        REQUIRE(has_field(subs, "AFC_buffer buffer1", "lanes"));
    }

    SECTION("AFC_extruder: parse_afc_extruder fields") {
        REQUIRE(has_field(subs, "AFC_extruder extruder", "lane_loaded"));
        REQUIRE(has_field(subs, "AFC_extruder extruder", "tool_end_status"));
        REQUIRE(has_field(subs, "AFC_extruder extruder", "tool_start_status"));
    }

    SECTION("Unit-level (AFC_BoxTurtle): parse_afc_unit_object fields") {
        REQUIRE(has_field(subs, "AFC_BoxTurtle turtle1", "lanes"));
        REQUIRE(has_field(subs, "AFC_BoxTurtle turtle1", "extruders"));
        REQUIRE(has_field(subs, "AFC_BoxTurtle turtle1", "hubs"));
        REQUIRE(has_field(subs, "AFC_BoxTurtle turtle1", "buffers"));
    }

    SECTION("AFC_led: dropped entirely (no parser)") {
        REQUIRE_FALSE(subs.contains("AFC_led lane1_led"));
    }
}

TEST_CASE("Subscription: bed_mesh covers moonraker_advanced_api parser",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    REQUIRE(has_field(subs, "bed_mesh", "profile_name"));
    REQUIRE(has_field(subs, "bed_mesh", "probed_matrix"));
    REQUIRE(has_field(subs, "bed_mesh", "mesh_min"));
    REQUIRE(has_field(subs, "bed_mesh", "mesh_max"));
    REQUIRE(has_field(subs, "bed_mesh", "mesh_params"));
    REQUIRE(has_field(subs, "bed_mesh", "profiles"));
}

TEST_CASE("Subscription: exclude_object covers printer_state reads", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    REQUIRE(has_field(subs, "exclude_object", "objects"));
    REQUIRE(has_field(subs, "exclude_object", "excluded_objects"));
    REQUIRE(has_field(subs, "exclude_object", "current_object"));
}

TEST_CASE("Subscription: calibration objects narrow to read fields", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    SECTION("manual_probe — printer_calibration_state.cpp lines 61-89") {
        REQUIRE(has_field(subs, "manual_probe", "is_active"));
        REQUIRE(has_field(subs, "manual_probe", "z_position"));
    }

    SECTION("stepper_enable — printer_calibration_state.cpp lines 107-128") {
        REQUIRE(has_field(subs, "stepper_enable", "steppers"));
    }
}

TEST_CASE("Subscription: firmware_retraction subscribed only when discovered",
          "[moonraker][subscription]") {
    SECTION("absent when no firmware_retraction in objects list") {
        DiscoveryFixture fx;
        json subs = fx.build();
        REQUIRE_FALSE(subs.contains("firmware_retraction"));
    }

    SECTION("present with all four tunables when discovered") {
        DiscoveryFixture fx;
        fx.add("firmware_retraction", {});
        json subs = fx.build();
        REQUIRE(has_field(subs, "firmware_retraction", "retract_length"));
        REQUIRE(has_field(subs, "firmware_retraction", "retract_speed"));
        REQUIRE(has_field(subs, "firmware_retraction", "unretract_extra_length"));
        REQUIRE(has_field(subs, "firmware_retraction", "unretract_speed"));
    }
}

TEST_CASE("Subscription: filament + width sensors narrow correctly", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("filament_switch_sensor runout", {"filament_sensor"});
    fx.add("filament_motion_sensor encoder", {"filament_sensor"});
    fx.add("hall_filament_width_sensor", {});
    fx.add("tsl1401cl_filament_width_sensor", {});

    json subs = fx.build();

    SECTION("filament sensors: {filament_detected, enabled, detection_count}") {
        for (const auto& s : {"filament_switch_sensor runout", "filament_motion_sensor encoder"}) {
            CAPTURE(s);
            REQUIRE(has_field(subs, s, "filament_detected"));
            REQUIRE(has_field(subs, s, "enabled"));
            REQUIRE(has_field(subs, s, "detection_count"));
        }
    }

    SECTION("width sensors: {Diameter, Raw}") {
        for (const auto& s : {"hall_filament_width_sensor", "tsl1401cl_filament_width_sensor"}) {
            CAPTURE(s);
            REQUIRE(has_field(subs, s, "Diameter"));
            REQUIRE(has_field(subs, s, "Raw"));
        }
    }
}

TEST_CASE("Subscription: toolchanger + per-tool fields cover ToolState reads",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("toolchanger", {});
    fx.add("tool T0", {});
    fx.add("tool T1", {});
    json subs = fx.build();

    SECTION("toolchanger reads status, tool_number, tool_numbers") {
        REQUIRE(has_field(subs, "toolchanger", "status"));
        REQUIRE(has_field(subs, "toolchanger", "tool_number"));
        REQUIRE(has_field(subs, "toolchanger", "tool_numbers"));
    }

    SECTION("per-tool reads active/mounted/detect_state/offsets/extruder/fan") {
        for (const auto& tool : {"tool T0", "tool T1"}) {
            CAPTURE(tool);
            REQUIRE(has_field(subs, tool, "active"));
            REQUIRE(has_field(subs, tool, "mounted"));
            REQUIRE(has_field(subs, tool, "detect_state"));
            REQUIRE(has_field(subs, tool, "gcode_x_offset"));
            REQUIRE(has_field(subs, tool, "gcode_y_offset"));
            REQUIRE(has_field(subs, tool, "gcode_z_offset"));
            REQUIRE(has_field(subs, tool, "extruder"));
            REQUIRE(has_field(subs, tool, "fan"));
        }
    }
}

TEST_CASE("Subscription: print-start macros narrow to boolean flags", "[moonraker][subscription]") {
    DiscoveryFixture fx;
    json subs = fx.build();

    REQUIRE(has_field(subs, "gcode_macro _START_PRINT", "print_started"));
    REQUIRE(has_field(subs, "gcode_macro START_PRINT", "preparation_done"));
    REQUIRE(has_field(subs, "gcode_macro _HELIX_STATE", "print_started"));
}

TEST_CASE("Subscription: system_stats dropped; idle_timeout narrowed to state",
          "[moonraker][subscription]") {
    DiscoveryFixture fx;
    fx.add("idle_timeout", {});
    fx.add("system_stats", {});
    json subs = fx.build();

    // system_stats is unread by any production parser; subscription must skip it.
    REQUIRE_FALSE(subs.contains("system_stats"));

    // idle_timeout IS subscribed — its state is Klipper's canonical busy flag,
    // read by PrinterCalibrationState to drive the blocking-non-print-operation
    // signal. Narrowed to the single "state" field to limit notifications.
    REQUIRE(subs.contains("idle_timeout"));
    REQUIRE(has_field(subs, "idle_timeout", "state"));
}

TEST_CASE("Subscription: fan_feedback subscribed only when present", "[moonraker][subscription]") {
    SECTION("absent on hardware without fan_feedback") {
        DiscoveryFixture fx;
        json subs = fx.build();
        REQUIRE_FALSE(subs.contains("fan_feedback"));
    }

    SECTION("present with fan0..fan9_speed when discovered") {
        DiscoveryFixture fx;
        fx.add("fan_feedback", {});
        json subs = fx.build();
        for (int i = 0; i < 10; ++i) {
            std::string field = "fan" + std::to_string(i) + "_speed";
            CAPTURE(field);
            REQUIRE(has_field(subs, "fan_feedback", field));
        }
    }
}

// MCU subscriptions cover MoonrakerPerformanceSource's reads. These MUST ride
// the single union subscription built here — Moonraker docs:
// "A new request will override a previous request." A separate
// printer.objects.subscribe call from PerformanceSource would wipe the entire
// subscription, severing notify_status_update for heaters / print_stats /
// fans / AFC. Regression of this guard reintroduces the v0.99.68 outage.
TEST_CASE("Subscription: MCU objects narrow to PerformanceSource reads",
          "[moonraker][subscription]") {
    SECTION("none subscribed when no MCU objects discovered") {
        DiscoveryFixture fx;
        // No MCU added — this is the "before discovery" / mock-printer case.
        json subs = fx.build();
        REQUIRE_FALSE(subs.contains("mcu"));
    }

    SECTION("primary mcu subscribes last_stats") {
        DiscoveryFixture fx;
        fx.add("mcu", {"mcu"});
        json subs = fx.build();

        // on_mcu_status_update reads last_stats.mcu_awake and
        // last_stats.bytes_retransmit — both arrive in the same last_stats
        // dict (Klipper Status_Reference.html#mcu). Top-level
        // bytes_retransmit does NOT exist; subscribing to it was a no-op.
        REQUIRE(has_field(subs, "mcu", "last_stats"));
    }

    SECTION("secondary MCUs (host + per-toolhead) each subscribe last_stats") {
        DiscoveryFixture fx;
        fx.add("mcu", {"mcu"});
        fx.add("mcu host", {"mcu"});
        fx.add("mcu e0", {"mcu"});
        fx.add("mcu e1", {"mcu"});
        json subs = fx.build();

        for (const char* name : {"mcu", "mcu host", "mcu e0", "mcu e1"}) {
            CAPTURE(name);
            REQUIRE(has_field(subs, name, "last_stats"));
        }
    }

    SECTION("MCU subscription coexists with heater/print_stats — the v0.99.68 outage was about "
            "replacement") {
        DiscoveryFixture fx;
        fx.add("mcu", {"mcu"});
        fx.add("mcu host", {"mcu"});
        fx.add("extruder", {"heater"});
        fx.add("heater_bed", {"heater"});
        json subs = fx.build();

        // The point of the union subscription: adding MCU objects must NOT
        // wipe the heaters that v0.99.68's separate subscribe call clobbered.
        REQUIRE(has_field(subs, "mcu", "last_stats"));
        REQUIRE(has_field(subs, "mcu host", "last_stats"));
        REQUIRE(has_field(subs, "extruder", "temperature"));
        REQUIRE(has_field(subs, "extruder", "target"));
        REQUIRE(has_field(subs, "heater_bed", "temperature"));
        REQUIRE(has_field(subs, "heater_bed", "target"));
        // print_stats and virtual_sdcard are core; should always be present.
        REQUIRE(has_field(subs, "print_stats", "state"));
        REQUIRE(has_field(subs, "virtual_sdcard", "progress"));
    }
}

TEST_CASE("Subscription: ZMOD printers subscribe save_variables for the persisted z-offset",
          "[moonraker][subscription][zmod]") {
    SECTION("SAVE_ZMOD_DATA present -> save_variables subscribed") {
        // ZMOD zeroes gcode_move's live offset outside a print and keeps the real
        // one in save_variables' gcode_offsets. Without this subscription the
        // Z-offset row reads 0.000 whenever the printer is idle.
        DiscoveryFixture fx;
        fx.add("gcode_macro SAVE_ZMOD_DATA", {});
        fx.add("extruder", {"heater"});
        json subs = fx.build();

        REQUIRE(subs.contains("save_variables"));
    }

    SECTION("no ZMOD macro -> save_variables not subscribed") {
        // Every non-ZMOD printer must not pay for an object nothing reads.
        DiscoveryFixture fx;
        fx.add("gcode_macro START_PRINT", {});
        fx.add("extruder", {"heater"});
        json subs = fx.build();

        REQUIRE_FALSE(subs.contains("save_variables"));
    }

    SECTION("detection is case-insensitive like every other macro gate") {
        DiscoveryFixture fx;
        fx.add("gcode_macro save_zmod_data", {});
        json subs = fx.build();

        REQUIRE(subs.contains("save_variables"));
    }
}
