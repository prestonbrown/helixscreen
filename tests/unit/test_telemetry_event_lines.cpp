// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_telemetry_data.h"

#include "../helix_test_fixture.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::settings::describe_telemetry_event;
using nlohmann::json;

namespace {

std::vector<std::string> texts(const json& event) {
    std::vector<std::string> out;
    for (const auto& line : describe_telemetry_event(event)) {
        out.push_back(line.text);
    }
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

bool any_line_starts_with(const std::vector<std::string>& lines, const std::string& prefix) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& l) { return l.rfind(prefix, 0) == 0; });
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: heading, timestamp and device",
                 "[telemetry_data]") {
    auto lines = texts({{"event", "crash"}, {"timestamp", "T0"}, {"device_id", "abc"}});
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "Crash Report");
    CHECK(lines[1] == "T0");
    CHECK(lines[2] == "Device: abc");

    auto odd = texts({{"event", 5}, {"timestamp", 7}, {"device_id", nullptr}});
    REQUIRE(odd.size() == 1);
    CHECK(odd[0] == "Unknown Event");

    CHECK(texts(json::array({1})) == std::vector<std::string>{"Unknown Event"});
}

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: flat string and number fields",
                 "[telemetry_data]") {
    auto lines = texts({{"event", "print_outcome"},
                        {"outcome", "success"},
                        {"filament_type", 3},
                        {"duration_sec", 42},
                        {"nozzle_temp", 215.25},
                        {"bed_temp", "60"}});
    CHECK(has_line(lines, "Outcome: success"));
    CHECK_FALSE(any_line_starts_with(lines, "Filament"));
    CHECK(has_line(lines, "Duration: 42s"));
    CHECK(has_line(lines, "Nozzle: 215.2\xC2\xB0"
                          "C"));
    CHECK_FALSE(any_line_starts_with(lines, "Bed"));
}

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: session sections", "[telemetry_data]") {
    json event = {{"event", "session"},
                  {"app",
                   {{"version", "1.2"},
                    {"platform", "pi"},
                    {"display", ""},
                    {"display_backend", 4},
                    {"theme", true},
                    {"locale", "de"}}},
                  {"printer",
                   {{"kinematics", "corexy"},
                    {"mcu", "stm32"},
                    {"mcu_count", 2.0},
                    {"extruder_count", 2},
                    {"klipper_version", "v0.12"}}},
                  {"features", json::array({"a", 1, "b"})},
                  {"host", {{"arch", "aarch64"}, {"cpu_cores", 4.0}, {"ram_total_mb", 1024}}}};
    auto lines = texts(event);
    CHECK(has_line(lines, "Version: 1.2"));
    CHECK(has_line(lines, "Platform: pi | Display: "));
    CHECK(has_line(lines, "Locale: de"));
    CHECK(has_line(lines, "Printer: corexy"));
    CHECK(has_line(lines, "MCU: stm32 | 2 extruders"));
    CHECK(has_line(lines, "Klipper: v0.12"));
    CHECK_FALSE(any_line_starts_with(lines, "Moonraker"));
    CHECK(has_line(lines, "Features: a, b"));
    CHECK(has_line(lines, "Host: aarch64, 1024 MB RAM"));

    event["printer"] = "corexy";
    auto bare = texts(event);
    CHECK_FALSE(any_line_starts_with(bare, "Printer"));
    CHECK_FALSE(any_line_starts_with(bare, "MCU"));
}

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: hardware profile", "[telemetry_data]") {
    json event = {
        {"event", "hardware_profile"},
        {"printer", {{"detected_model", "Voron"}, {"kinematics", 1}}},
        {"mcus", {{"primary", "rp2040"}, {"count", 3}}},
        {"build_volume", {{"x_mm", 250.9}, {"y_mm", 250}, {"z_mm", "300"}}},
        {"extruders", {{"count", 1}, {"has_heater_bed", true}, {"has_chamber_heater", 1}}},
        {"fans", {{"total", 4.5}}},
        {"steppers", {{"count", "5"}}},
        {"leds", {{"count", 2}}},
        {"capabilities", {{"has_probe", true}, {"quad_gantry", true}, {"has_x", 1}}},
        {"ams", {{"type", "afc"}, {"total_slots", 4.0}}},
        {"tools", {{"count", 2}, {"is_multi_tool", "true"}}}};
    auto lines = texts(event);
    CHECK(has_line(lines, "Printer: Voron"));
    CHECK(has_line(lines, "MCU: rp2040 (x3)"));
    CHECK(has_line(lines, "Build Volume: 250x250 mm"));
    CHECK(has_line(lines, "1 extruder(s), heated bed"));
    CHECK(has_line(lines, "4 fans, 2 LEDs"));
    CHECK(has_line(lines, "Capabilities: probe, quad_gantry"));
    CHECK(has_line(lines, "AMS: afc"));
    CHECK(has_line(lines, "Tools: 2 tool(s)"));

    event["mcus"]["count"] = 3.0;
    event["build_volume"]["y_mm"] = "250";
    event["tools"]["count"] = 2.0;
    auto strict = texts(event);
    CHECK(has_line(strict, "MCU: rp2040"));
    CHECK_FALSE(any_line_starts_with(strict, "Build Volume"));
    CHECK_FALSE(any_line_starts_with(strict, "Tools"));
}

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: panel usage maps skip non-numbers",
                 "[telemetry_data]") {
    json event = {{"event", "panel_usage"},
                  {"panel_time_sec", {{"home", 12.7}, {"bad", "x"}}},
                  {"overlay_visits", {{"wifi", 3}}},
                  {"panel_visits", {{"bad", nullptr}}}};
    auto lines = texts(event);
    CHECK(has_line(lines, "Time: home: 12s"));
    CHECK(has_line(lines, "Overlay Visits: wifi: 3"));
    CHECK_FALSE(any_line_starts_with(lines, "Visits"));
}

TEST_CASE_METHOD(HelixTestFixture, "Telemetry event card: boolean fields need a JSON boolean",
                 "[telemetry_data]") {
    auto lines = texts({{"event", "print_start_context"},
                        {"has_thumbnail", false},
                        {"ams_active", 1},
                        {"slicer", "orca"}});
    CHECK(has_line(lines, "Thumbnail: No"));
    CHECK_FALSE(any_line_starts_with(lines, "AMS Active"));
    CHECK(has_line(lines, "Slicer: orca"));
}
