// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_assignment_options.h"

#include "../catch_amalgamated.hpp"

using helix::settings::build_chamber_assignment_options;
using helix::settings::ChamberAssignmentLabels;

namespace {

// The overlay resolves these through lv_tr(); the builder takes them already
// translated so it stays free of LVGL.
ChamberAssignmentLabels labels() {
    return ChamberAssignmentLabels{"Auto", "(none detected)", "not detected", "None (disable)"};
}

// Split the newline-separated list lv_dropdown_set_options() consumes.
std::vector<std::string> split_options(const std::string& options) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t nl = options.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(options.substr(start));
            break;
        }
        out.push_back(options.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

const std::vector<std::string> kSensors = {"temperature_sensor mcu_temp",
                                           "temperature_sensor chamber"};

} // namespace

TEST_CASE("build_chamber_assignment_options - auto selects the first option",
          "[settings][chamber][assignment]") {
    auto built = build_chamber_assignment_options(kSensors, "temperature_sensor chamber", "auto",
                                                  "temperature_sensor ", labels());

    REQUIRE(built.selected == 0);
    REQUIRE(built.names.size() == 2);

    auto opts = split_options(built.options);
    REQUIRE(opts.size() == 4);
    REQUIRE(opts[0] == "Auto (chamber)");
    REQUIRE(opts[1] == "mcu_temp");
    REQUIRE(opts[2] == "chamber");
    REQUIRE(opts[3] == "None (disable)");
}

TEST_CASE("build_chamber_assignment_options - auto reports when discovery found nothing",
          "[settings][chamber][assignment]") {
    auto built = build_chamber_assignment_options({}, "", "auto", "temperature_sensor ", labels());

    auto opts = split_options(built.options);
    REQUIRE(opts[0] == "Auto (none detected)");
    REQUIRE(built.selected == 0);
}

TEST_CASE("build_chamber_assignment_options - none selects the last option",
          "[settings][chamber][assignment]") {
    auto built = build_chamber_assignment_options(kSensors, "temperature_sensor chamber", "none",
                                                  "temperature_sensor ", labels());

    REQUIRE(built.names.size() == 2);
    REQUIRE(built.selected == built.names.size() + 1);

    auto opts = split_options(built.options);
    REQUIRE(built.selected == opts.size() - 1);
    REQUIRE(opts[built.selected] == "None (disable)");
}

TEST_CASE("build_chamber_assignment_options - a discovered assignment selects its own option",
          "[settings][chamber][assignment]") {
    auto built = build_chamber_assignment_options(kSensors, "temperature_sensor chamber",
                                                  "temperature_sensor mcu_temp",
                                                  "temperature_sensor ", labels());

    REQUIRE(built.selected == 1);
    REQUIRE(built.names.size() == 2);
    REQUIRE(built.names[built.selected - 1] == "temperature_sensor mcu_temp");

    // Nothing is appended for an assignment discovery already returned.
    auto opts = split_options(built.options);
    REQUIRE(opts.size() == 4);
}

TEST_CASE("build_chamber_assignment_options - an undiscovered assignment gets a marked option",
          "[settings][chamber][assignment]") {
    // A preset can name an object the printer's config leaves commented out, so
    // the saved assignment is absent from discovery.
    auto built = build_chamber_assignment_options(kSensors, "temperature_sensor chamber",
                                                  "temperature_sensor chamber_temp",
                                                  "temperature_sensor ", labels());

    auto opts = split_options(built.options);
    REQUIRE(opts.size() == 5);
    REQUIRE(opts[3] == "chamber_temp (not detected)");

    // Selected, so the overlay shows the stale value instead of reading "Auto".
    REQUIRE(built.selected == 3);
    REQUIRE(built.names.size() == 3);
    REQUIRE(built.names[built.selected - 1] == "temperature_sensor chamber_temp");
}

TEST_CASE("build_chamber_assignment_options - disable stays at names.size() + 1 past a stale entry",
          "[settings][chamber][assignment]") {
    // The dropdown's value-changed handler reads "none" off index names.size() + 1
    // and an object off names[sel - 1]. An entry appended after the disable option
    // would push disable out of that slot and map it onto a name.
    auto built = build_chamber_assignment_options(kSensors, "", "temperature_sensor chamber_temp",
                                                  "temperature_sensor ", labels());

    auto opts = split_options(built.options);
    REQUIRE(built.names.size() == 3);
    REQUIRE(opts.size() == built.names.size() + 2);
    REQUIRE(opts.back() == "None (disable)");
    REQUIRE(opts[built.names.size() + 1] == "None (disable)");

    // Every option between auto and disable resolves to a name.
    for (size_t sel = 1; sel <= built.names.size(); sel++) {
        REQUIRE_FALSE(built.names[sel - 1].empty());
    }
}

TEST_CASE("build_chamber_assignment_options - heater prefix is stripped for display",
          "[settings][chamber][assignment]") {
    auto built = build_chamber_assignment_options(
        {"heater_generic chamber_heater"}, "heater_generic chamber_heater",
        "heater_generic missing_heater", "heater_generic ", labels());

    auto opts = split_options(built.options);
    REQUIRE(opts[0] == "Auto (chamber_heater)");
    REQUIRE(opts[1] == "chamber_heater");
    REQUIRE(opts[2] == "missing_heater (not detected)");
    REQUIRE(opts[3] == "None (disable)");
    REQUIRE(built.selected == 2);

    // The names vector keeps the full Klipper object name for persistence.
    REQUIRE(built.names[1] == "heater_generic missing_heater");
}

TEST_CASE("build_chamber_assignment_options - an unset assignment falls back to auto",
          "[settings][chamber][assignment]") {
    auto built =
        build_chamber_assignment_options(kSensors, "", "", "temperature_sensor ", labels());

    REQUIRE(built.selected == 0);
    REQUIRE(built.names.size() == 2);
    REQUIRE(split_options(built.options).size() == 4);
}
