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

// Each row: an event, lines its card must show, and line prefixes a missing or
// wrong-typed field must leave off.
TEST_CASE_METHOD(HelixTestFixture, "Telemetry event cards show only well-typed fields",
                 "[telemetry_data]") {
    struct Row {
        json event;
        std::vector<std::string> shown;
        std::vector<std::string> absent;
    };
    const std::vector<Row> rows = {
        {{{"event", "crash"}, {"timestamp", "T0"}, {"device_id", "abc"}},
         {"Crash Report", "T0", "Device: abc"},
         {}},
        {{{"event", 5}, {"timestamp", 7}, {"device_id", nullptr}}, {"Unknown Event"}, {"Device"}},
        {{{"event", "print_outcome"},
          {"outcome", "ok"},
          {"filament_type", 3},
          {"duration_sec", 42},
          {"nozzle_temp", 215.25},
          {"bed_temp", "60"}},
         {"Outcome: ok", "Duration: 42s",
          "Nozzle: 215.2\xC2\xB0"
          "C"},
         {"Filament", "Bed"}},
        {{{"event", "session"},
          {"app", {{"version", "1.2"}, {"platform", "pi"}, {"display", ""}, {"theme", true}}},
          {"printer", {{"mcu", "stm32"}, {"mcu_count", 2.0}, {"extruder_count", 2}}},
          {"features", json::array({"a", 1, "b"})},
          {"host", {{"arch", "arm"}, {"cpu_cores", 4.0}, {"ram_total_mb", 1024}}}},
         {"Version: 1.2", "Platform: pi | Display: ", "MCU: stm32 | 2 extruders", "Features: a, b",
          "Host: arm, 1024 MB RAM"},
         {"Theme", "Printer:"}},
        {{{"event", "hardware_profile"},
          {"mcus", {{"primary", "rp"}, {"count", 3.0}}},
          {"build_volume", {{"x_mm", 250.9}, {"y_mm", 250}, {"z_mm", "300"}}},
          {"extruders", {{"count", 1}, {"has_heater_bed", true}, {"has_chamber_heater", 1}}},
          {"fans", {{"total", 4.5}}},
          {"steppers", {{"count", "5"}}},
          {"capabilities", {{"has_probe", true}, {"has_x", 1}}},
          {"tools", {{"count", 2.0}}}},
         {"MCU: rp", "Build Volume: 250x250 mm", "1 extruder(s), heated bed", "4 fans",
          "Capabilities: probe"},
         {"Tools"}},
        {{{"event", "panel_usage"},
          {"panel_time_sec", {{"home", 12.7}, {"bad", "x"}}},
          {"panel_visits", {{"bad", nullptr}}}},
         {"Time: home: 12s"},
         {"Visits"}},
        {{{"event", "print_start_context"}, {"has_thumbnail", false}, {"ams_active", 1}},
         {"Thumbnail: No"},
         {"AMS Active"}},
    };
    for (const auto& row : rows) {
        std::vector<std::string> lines;
        for (const auto& line : describe_telemetry_event(row.event)) {
            lines.push_back(line.text);
        }
        CAPTURE(row.event.dump(), lines);
        for (const auto& want : row.shown) {
            CHECK(std::count(lines.begin(), lines.end(), want) == 1);
        }
        for (const auto& prefix : row.absent) {
            CHECK(std::none_of(lines.begin(), lines.end(),
                               [&](const std::string& l) { return l.rfind(prefix, 0) == 0; }));
        }
    }
}
