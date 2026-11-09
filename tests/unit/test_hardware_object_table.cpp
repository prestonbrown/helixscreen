// SPDX-License-Identifier: GPL-3.0-or-later

#include "accel_sensor_types.h"
#include "hardware_object_table.h"
#include "humidity_sensor_types.h"
#include "probe_sensor_types.h"

#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::hw;
using helix::sensors::AccelSensorType;
using helix::sensors::HumiditySensorType;
using helix::sensors::ProbeSensorType;

namespace {

ObjectClass obj(const char* name) {
    return classify_object(name);
}

} // namespace

TEST_CASE("hw table: every kitchen-sink name classifies", "[hw_object_table]") {
    struct Expect {
        const char* name;
        ObjectKind kind;
        const char* instance;
    };
    const Expect expected[] = {
        {"extruder", ObjectKind::Heater, ""},
        {"heater_bed", ObjectKind::Heater, ""},
        {"output_pin exhaust_fan", ObjectKind::OutputPinFan, "exhaust_fan"},
        {"output_pin fan_light", ObjectKind::OutputPinFan, "fan_light"},
        {"output_pin led_fan", ObjectKind::OutputPinFan, "led_fan"},
        {"output_pin beeper", ObjectKind::Speaker, "beeper"},
        {"neopixel", ObjectKind::Led, ""},
        {"dotstar", ObjectKind::Led, ""},
        {"neopixel strip", ObjectKind::Led, "strip"},
        {"aht20_f heater_box1", ObjectKind::HumidityChip, "heater_box1"},
        {"bme280 enclosure", ObjectKind::HumidityChip, "enclosure"},
        {"temperature_sensor extruder_board", ObjectKind::TemperatureSensor, "extruder_board"},
        {"heater_generic chamber_heater", ObjectKind::GenericHeater, "chamber_heater"},
        {"temperature_fan exhaust", ObjectKind::TemperatureFan, "exhaust"},
        {"tmc2240 stepper_x", ObjectKind::DriverTemperature, "stepper_x"},
        {"mcu", ObjectKind::Mcu, ""},
        {"mcu toolhead", ObjectKind::Mcu, "toolhead"},
        {"zmod_ifs_switch_sensor head", ObjectKind::BackendFilamentSensor, "head"},
        {"filament_switch_sensor runout", ObjectKind::FilamentSwitch, "runout"},
        {"filament_motion_sensor encoder", ObjectKind::FilamentMotion, "encoder"},
        {"hall_filament_width_sensor", ObjectKind::WidthSensor, ""},
        {"load_cell scale", ObjectKind::LoadCell, "scale"},
        {"probe", ObjectKind::Probe, ""},
        {"bltouch", ObjectKind::Probe, ""},
        {"beacon", ObjectKind::Probe, ""},
        {"cartographer", ObjectKind::Probe, ""},
        {"probe_eddy_current btt", ObjectKind::Probe, "btt"},
        {"gcode_button door", ObjectKind::Input, "door"},
        {"power_loss_check", ObjectKind::PowerLossMonitor, ""},
        {"led_effect rainbow", ObjectKind::LedEffect, "rainbow"},
        {"fan_feedback", ObjectKind::FanFeedback, ""},
        {"heater_fan hotend_fan", ObjectKind::HeaterFan, "hotend_fan"},
        {"heater_fan_throttled chamber_heat_fan", ObjectKind::HeaterFan, "chamber_heat_fan"},
        {"controller_fan board", ObjectKind::ControllerFan, "board"},
        {"fan_generic aux", ObjectKind::GenericFan, "aux"},
        {"fan", ObjectKind::Fan, ""},
    };
    for (const auto& e : expected) {
        INFO(e.name);
        const auto c = obj(e.name);
        CHECK(c.kind == e.kind);
        CHECK(c.instance == e.instance);
    }
}

TEST_CASE("hw table: kitchen-sink fixture is fully covered", "[hw_object_table]") {
    std::ifstream f("tests/fixtures/sensor_discovery_golden/kitchen_sink_objects.json");
    REQUIRE(f.good());
    const auto names = nlohmann::json::parse(f, nullptr, false);
    REQUIRE(names.is_array());
    REQUIRE_FALSE(names.empty());
    for (const auto& n : names) {
        INFO(n.get<std::string>());
        CHECK(obj(n.get<std::string>().c_str()).kind != ObjectKind::Unknown);
    }
}

TEST_CASE("hw table: extruder naming keeps the three-digit bound", "[hw_object_table]") {
    CHECK(obj("extruder1").kind == ObjectKind::Heater);
    CHECK(obj("extruder123").kind == ObjectKind::Heater);
    CHECK(obj("extruder1234").kind == ObjectKind::Unknown);
    CHECK(obj("extruder_stepper x").kind == ObjectKind::Unknown);
    CHECK(obj("extruder_offset_calibration").kind == ObjectKind::Unknown);
}

TEST_CASE("hw table: output_pin keyword rule", "[hw_object_table]") {
    CHECK(obj("output_pin exhaust_fan").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin fan_light").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin led_fan").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin case_fans").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin fan0").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin Enclosure_LEDs").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin case_lights").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin FAN2").kind == ObjectKind::OutputPinFan);
    CHECK(obj("output_pin case_light").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin LED_strip").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin lamp").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin beeper").kind == ObjectKind::Speaker);
    CHECK(obj("output_pin Buzzer").kind == ObjectKind::Speaker);
    CHECK(obj("output_pin speaker").kind == ObjectKind::Speaker);
    CHECK(obj("output_pin relay").kind == ObjectKind::Unknown);
    // Matching is a substring, not a token.
    CHECK(obj("output_pin caselight").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin Enclosure_LEDs").kind == ObjectKind::OutputPinLed);
    CHECK(obj("output_pin chamberfan").kind == ObjectKind::OutputPinFan);
}

TEST_CASE("hw table: bare and named neopixel and dotstar are LEDs", "[hw_object_table]") {
    CHECK(obj("neopixel").kind == ObjectKind::Led);
    CHECK(obj("dotstar").kind == ObjectKind::Led);
    CHECK(obj("dotstar chain").instance == "chain");
    CHECK(obj("led status").kind == ObjectKind::Led);
    CHECK(obj("led_effect x").kind == ObjectKind::LedEffect);
    CHECK(obj("neopixelx").kind == ObjectKind::Unknown);
}

TEST_CASE("hw table: humidity chips carry their chip index and display name", "[hw_object_table]") {
    const auto c = obj("aht20_f heater_box1");
    CHECK(c.kind == ObjectKind::HumidityChip);
    CHECK(c.chip == static_cast<uint8_t>(HumiditySensorType::AHT20_F));
    CHECK(chip_name(ObjectKind::HumidityChip, c.chip) == "AHT20-F");
    CHECK(obj("aht20 x").chip == static_cast<uint8_t>(HumiditySensorType::AHT20));
    CHECK(obj("temperature_oams oams1").chip ==
          static_cast<uint8_t>(HumiditySensorType::TEMPERATURE_OAMS));
    CHECK(obj("bme280").kind == ObjectKind::Unknown);
    for (const auto& chip : helix::sensors::humidity_sensor_chips()) {
        INFO(std::string(chip.klipper_prefix));
        CHECK(classify_object(std::string(chip.klipper_prefix) + "x").kind ==
              ObjectKind::HumidityChip);
        CHECK(chip_name(ObjectKind::HumidityChip, static_cast<uint8_t>(chip.type)) ==
              chip.display_name);
    }
}

TEST_CASE("hw table: humidity_chip_for_object delegates to the table", "[hw_object_table]") {
    const auto* chip = helix::sensors::humidity_chip_for_object("aht20_f heater_box1");
    REQUIRE(chip != nullptr);
    CHECK(chip->type == HumiditySensorType::AHT20_F);
    CHECK(helix::sensors::humidity_chip_for_object("temperature_sensor x") == nullptr);
}

TEST_CASE("hw table: probe list", "[hw_object_table]") {
    for (const char* n :
         {"probe", "bltouch", "smart_effector", "cartographer", "beacon", "probe_eddy_current x"}) {
        INFO(n);
        CHECK(obj(n).kind == ObjectKind::Probe);
        CHECK(classify_section(n, nlohmann::json::object()).kind == ObjectKind::Probe);
    }
    CHECK(obj("probe").chip == static_cast<uint8_t>(ProbeSensorType::STANDARD));
    CHECK(obj("beacon").chip == static_cast<uint8_t>(ProbeSensorType::BEACON));
    CHECK(obj("probe_eddy_current btt").chip ==
          static_cast<uint8_t>(ProbeSensorType::EDDY_CURRENT));
    CHECK(chip_name(ObjectKind::Probe, obj("bltouch").chip) == "BLTouch");

    // load_cell_probe and dockable_probe are config sections with no probe object name.
    const auto lcp = classify_section("load_cell_probe", nlohmann::json::object());
    CHECK(lcp.kind == ObjectKind::Probe);
    CHECK(lcp.chip == static_cast<uint8_t>(ProbeSensorType::LOADCELL));
    CHECK(classify_section("dockable_probe", nlohmann::json::object()).kind == ObjectKind::Probe);
    CHECK(obj("load_cell_probe").kind == ObjectKind::Unknown);
    CHECK(obj("dockable_probe").kind == ObjectKind::Unknown);
}

TEST_CASE("hw table: accelerometer sections", "[hw_object_table]") {
    const nlohmann::json empty = nlohmann::json::object();
    const auto adxl = classify_section("adxl345 bed", empty);
    CHECK(adxl.kind == ObjectKind::Accelerometer);
    CHECK(adxl.instance == "bed");
    CHECK(adxl.chip == static_cast<uint8_t>(AccelSensorType::ADXL345));
    CHECK(classify_section("adxl345", empty).kind == ObjectKind::Accelerometer);
    CHECK(classify_section("icm20948 x", empty).chip ==
          static_cast<uint8_t>(AccelSensorType::ICM20948));
    CHECK(chip_name(ObjectKind::Accelerometer, adxl.chip) == "ADXL345");
    CHECK(classify_section("adxl3450", empty).kind == ObjectKind::Unknown);
    // Accelerometers have no get_status(), so objects.list never names one.
    CHECK(obj("adxl345 bed").kind == ObjectKind::Unknown);
}

TEST_CASE("hw table: resonance_tester is an accelerometer only when it names Beacon",
          "[hw_object_table]") {
    CHECK(classify_section("resonance_tester", {{"accel_chip", "beacon"}}).kind ==
          ObjectKind::Accelerometer);
    CHECK(classify_section("resonance_tester", {{"accel_chip_y", "beacon x"}}).kind ==
          ObjectKind::Accelerometer);
    CHECK(classify_section("resonance_tester", {{"accel_chip", "beaconx"}}).kind ==
          ObjectKind::Unknown);
    CHECK(classify_section("resonance_tester", {{"accel_chip", 5}}).kind == ObjectKind::Unknown);
    CHECK(classify_section("resonance_tester", nlohmann::json::object()).kind ==
          ObjectKind::Unknown);
    CHECK(classify_section("resonance_tester", nlohmann::json()).kind == ObjectKind::Unknown);
}

TEST_CASE("hw table: Beacon section is a probe unless it carries accelerometer fields",
          "[hw_object_table]") {
    const auto plain = classify_section("beacon", {{"x_offset", 1}});
    CHECK(plain.kind == ObjectKind::Probe);
    const auto with_scale = classify_section("beacon", {{"accel_scale", 1}});
    CHECK(with_scale.kind == ObjectKind::Accelerometer);
    CHECK(with_scale.chip == static_cast<uint8_t>(AccelSensorType::LIS2DW));
    CHECK(classify_section("beacon", {{"accel_axes_map", "x,y,z"}}).kind ==
          ObjectKind::Accelerometer);
}

TEST_CASE("hw table: vendor and unknown names", "[hw_object_table]") {
    CHECK(obj("zmod_ifs_motion_sensor head").kind == ObjectKind::BackendFilamentSensor);
    CHECK(obj("zmod_ifs_switch_sensor").kind == ObjectKind::Unknown);
    CHECK(obj("power_loss_check e0").kind == ObjectKind::Unknown);
    CHECK(obj("stepper_x").kind == ObjectKind::Stepper);
    CHECK(obj("tmc5160 stepper_z").kind == ObjectKind::DriverTemperature);
    CHECK(obj("tmc2209 stepper_x").kind == ObjectKind::Unknown);
    CHECK(obj("gcode_macro FOO").kind == ObjectKind::Unknown);
    CHECK(obj("").kind == ObjectKind::Unknown);
    CHECK(obj("temperature_sensor").kind == ObjectKind::Unknown);
}

namespace {
std::vector<std::string> fields_of(ObjectKind kind) {
    std::vector<std::string> out;
    for (const auto field : status_fields(kind)) {
        out.emplace_back(field);
    }
    return out;
}
} // namespace

TEST_CASE("hw table: status fields per kind", "[hw_object_table]") {
    using V = std::vector<std::string>;
    CHECK(fields_of(ObjectKind::LoadCell) == V{"force_g"});
    CHECK(fields_of(ObjectKind::Input) == V{"state"});
    CHECK(fields_of(ObjectKind::PowerLossMonitor) == V{"*"});
    CHECK(fields_of(ObjectKind::Accelerometer).empty());
    CHECK(fields_of(ObjectKind::Heater) == V{"temperature", "target", "power"});
    CHECK(fields_of(ObjectKind::TemperatureSensor) == V{"temperature", "humidity"});
    CHECK(fields_of(ObjectKind::HumidityChip) == V{"temperature", "humidity"});
    CHECK(fields_of(ObjectKind::TemperatureFan) == V{"temperature", "target", "speed"});
    CHECK(fields_of(ObjectKind::Fan) == V{"speed"});
    CHECK(fields_of(ObjectKind::OutputPinFan) == V{"value"});
    CHECK(fields_of(ObjectKind::OutputPinLed) == V{"value"});
    CHECK(fields_of(ObjectKind::Led) == V{"color_data"});
    CHECK(fields_of(ObjectKind::LedEffect) == V{"enabled"});
    CHECK(fields_of(ObjectKind::FanFeedback).size() == 10);
    CHECK(fields_of(ObjectKind::FanFeedback).front() == "fan0_speed");
    CHECK(fields_of(ObjectKind::FanFeedback).back() == "fan9_speed");
    CHECK(fields_of(ObjectKind::FilamentSwitch) ==
          V{"filament_detected", "enabled", "detection_count"});
    CHECK(fields_of(ObjectKind::BackendFilamentSensor) == fields_of(ObjectKind::FilamentMotion));
    CHECK(fields_of(ObjectKind::WidthSensor) == V{"Diameter", "Raw"});
    CHECK(fields_of(ObjectKind::Mcu) == V{"last_stats"});
    CHECK(fields_of(ObjectKind::Probe).empty());
    CHECK(fields_of(ObjectKind::Speaker).empty());
    CHECK(fields_of(ObjectKind::Stepper).empty());
    CHECK(fields_of(ObjectKind::Unknown).empty());
}

TEST_CASE("hw table: family predicates", "[hw_object_table]") {
    CHECK(is_heater(ObjectKind::Heater));
    CHECK(is_heater(ObjectKind::GenericHeater));
    CHECK_FALSE(is_heater(ObjectKind::TemperatureFan));
    CHECK(is_fan(ObjectKind::TemperatureFan));
    CHECK(is_fan(ObjectKind::OutputPinFan));
    CHECK_FALSE(is_fan(ObjectKind::FanFeedback));
    CHECK(is_led(ObjectKind::OutputPinLed));
    CHECK_FALSE(is_led(ObjectKind::LedEffect));
    CHECK(is_temperature(ObjectKind::HumidityChip));
    CHECK(is_temperature(ObjectKind::DriverTemperature));
    CHECK_FALSE(is_temperature(ObjectKind::Fan));
    CHECK(is_filament_sensor(ObjectKind::BackendFilamentSensor));
    CHECK_FALSE(is_filament_sensor(ObjectKind::WidthSensor));
}
