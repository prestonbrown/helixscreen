// SPDX-License-Identifier: GPL-3.0-or-later

#include "capability_chip_text.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

BuildVolume volume(float x, float y, float z) {
    BuildVolume v;
    v.x_max = x;
    v.y_max = y;
    v.z_max = z;
    return v;
}

} // namespace

TEST_CASE("capability_chip_text: probe carries its type", "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.probe_type = "Beacon";
    CHECK(capability_chip_text(CapChip::Probe, f) == "Probe · Beacon");
    f.probe_type.clear();
    CHECK(capability_chip_text(CapChip::Probe, f) == "Probe");
}

TEST_CASE("capability_chip_text: a forced-on capability says so", "[capability_chip_text]") {
    CapabilityChipFacts f;
    CHECK(capability_chip_text(CapChip::Qgl, f) == "QGL");
    f.forced_on.insert(CapChip::Qgl);
    CHECK(capability_chip_text(CapChip::Qgl, f) == "QGL · set manually");
    CHECK(capability_chip_text(CapChip::ZTilt, f) == "Z-Tilt");
}

TEST_CASE("capability_chip_text: accelerometer and webcam count only past one",
          "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.accel_count = 1;
    f.webcam_count = 1;
    CHECK(capability_chip_text(CapChip::Accelerometer, f) == "Accelerometer");
    CHECK(capability_chip_text(CapChip::Webcam, f) == "Webcam");
    f.accel_count = 2;
    f.webcam_count = 3;
    CHECK(capability_chip_text(CapChip::Accelerometer, f) == "Accelerometer · 2");
    CHECK(capability_chip_text(CapChip::Webcam, f) == "Webcam · 3");
}

TEST_CASE("capability_chip_text: counted chips always show a count", "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.power_device_count = 1;
    f.filament_sensor_count = 1;
    f.led_count = 2;
    CHECK(capability_chip_text(CapChip::Power, f) == "Power · 1");
    CHECK(capability_chip_text(CapChip::FilamentSensors, f) == "Filament Sensors · 1");
    CHECK(capability_chip_text(CapChip::Leds, f) == "LEDs · 2");
}

TEST_CASE("capability_chip_text: build volume", "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.build_volume = volume(350, 350, 340);
    CHECK(capability_chip_text(CapChip::BuildVolume, f) == "350×350×340");
    f.build_volume.x_min = 5.0f;
    f.build_volume.x_max = 300.0f;
    CHECK(capability_chip_text(CapChip::BuildVolume, f) == "295×350×340");

    // Endstop overtravel behind the bed is not printable room.
    f.build_volume = volume(461, 433.65f, 400);
    f.build_volume.y_min = -14.35f;
    CHECK(capability_chip_text(CapChip::BuildVolume, f) == "461×434×400");

    // A centre-origin range (delta) spans both sides of 0.
    f.build_volume = volume(100, 100, 300);
    f.build_volume.x_min = -100.0f;
    f.build_volume.y_min = -100.0f;
    CHECK(capability_chip_text(CapChip::BuildVolume, f) == "200×200×300");

    // A bed the firmware declares wins over the travel.
    f.build_volume = volume(352.5f, 400, 350);
    f.build_volume.x_min = -10.0f;
    f.build_volume.declared_bed_x = 350.0f;
    f.build_volume.declared_bed_y = 350.0f;
    CHECK(capability_chip_text(CapChip::BuildVolume, f) == "350×350×350");

    f.build_volume = volume(350, 350, 0);
    CHECK(capability_chip_text(CapChip::BuildVolume, f).empty());
}

TEST_CASE("capability_chip_text: filament systems leave the tool changer to its own chip",
          "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.filament_systems = {{"Happy Hare", false, "8 gates"}, {"Tool Changer", true, "4 tools"}};
    CHECK(capability_chip_text(CapChip::FilamentSystem, f) == "Happy Hare · 8 gates");

    f.filament_systems = {{"Tool Changer", true, "4 tools"}};
    CHECK(capability_chip_text(CapChip::FilamentSystem, f).empty());

    // A system that reports no positions shows its name alone.
    f.filament_systems = {{"Happy Hare", false, "8 gates"}, {"CFS", false, ""}};
    CHECK(capability_chip_text(CapChip::FilamentSystem, f) == "Happy Hare · 8 gates, CFS");

    f.has_tool_changer = true;
    f.head_count = 4;
    CHECK(capability_chip_text(CapChip::ToolChanger, f) == "Tool Changer · 4 Tools");
}

TEST_CASE("capability_chip_text: several heads without a tool changer are extruders",
          "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.head_count = 1;
    CHECK(capability_chip_text(CapChip::ToolChanger, f).empty());
    f.head_count = 2;
    CHECK(capability_chip_text(CapChip::ToolChanger, f) == "2 Extruders");
    f.has_tool_changer = true;
    CHECK(capability_chip_text(CapChip::ToolChanger, f) == "Tool Changer · 2 Tools");
}

TEST_CASE("capability_chip_text: MCUs", "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.mcus = {"stm32h723"};
    CHECK(capability_chip_text(CapChip::Mcus, f) == "stm32h723");
    f.mcus = {"stm32h723", "rp2040"};
    CHECK(capability_chip_text(CapChip::Mcus, f) == "MCUs · 2");
}

TEST_CASE("kinematics_display_name", "[capability_chip_text]") {
    const std::pair<const char*, const char*> cases[] = {
        {"corexy", "CoreXY"}, {"limited_corexy", "CoreXY"},     {"hybrid_corexy", "Hybrid CoreXY"},
        {"corexz", "CoreXZ"}, {"cartesian", "Cartesian"},       {"limited_cartesian", "Cartesian"},
        {"delta", "Delta"},   {"rotary_delta", "Rotary Delta"}, {"polar", "Polar"},
        {"winch", "Winch"},   {"deltesian", "deltesian"},       {"", ""},
    };
    for (const auto& [klipper, display] : cases) {
        CAPTURE(klipper);
        CHECK(kinematics_display_name(klipper) == display);
    }
    CapabilityChipFacts f;
    f.kinematics = "corexy";
    CHECK(capability_chip_text(CapChip::Kinematics, f) == "CoreXY");
}

TEST_CASE("capability_chip_text: every chip has text when every fact is known",
          "[capability_chip_text]") {
    CapabilityChipFacts f;
    f.kinematics = "corexy";
    f.build_volume = volume(350, 350, 340);
    f.probe_type = "Beacon";
    f.accel_count = 1;
    f.fan_count = 4;
    f.head_count = 4;
    f.has_tool_changer = true;
    f.filament_systems = {{"Happy Hare", false, "8 gates"}};
    f.filament_sensor_count = 2;
    f.width_sensor_count = 1;
    f.mcus = {"stm32h723"};
    f.power_device_count = 1;
    f.led_count = 1;
    f.led_effect_count = 2;
    f.webcam_count = 1;
    for (size_t i = 0; i < kCapChipCount; ++i) {
        CAPTURE(i);
        CHECK_FALSE(capability_chip_text(static_cast<CapChip>(i), f).empty());
    }
}
