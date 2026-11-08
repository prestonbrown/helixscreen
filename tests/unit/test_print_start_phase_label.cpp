// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "print_start_phase_label.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::OperationCategory;
using helix::phase_of_category;
using helix::print_start_phase_of_option;
using helix::print_start_phase_signal_message;
using helix::print_start_phase_step_key;
using helix::print_start_phase_step_label;
using helix::PrintStartPhase;

TEST_CASE("phase of category: every operation category, the non-phases included",
          "[print_start_phase_label]") {
    CHECK(phase_of_category(OperationCategory::BED_MESH) == PrintStartPhase::BED_MESH);
    CHECK(phase_of_category(OperationCategory::QGL) == PrintStartPhase::QGL);
    CHECK(phase_of_category(OperationCategory::Z_TILT) == PrintStartPhase::Z_TILT);
    CHECK(phase_of_category(OperationCategory::NOZZLE_CLEAN) == PrintStartPhase::CLEANING);
    CHECK(phase_of_category(OperationCategory::PURGE_LINE) == PrintStartPhase::PURGING);
    CHECK(phase_of_category(OperationCategory::HOMING) == PrintStartPhase::HOMING);
    CHECK(phase_of_category(OperationCategory::CHAMBER_SOAK) == PrintStartPhase::SOAKING);

    // A parent category, a correction and the start macro itself are no
    // pre-print phase of their own.
    CHECK_FALSE(phase_of_category(OperationCategory::BED_LEVEL).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::SKEW_CORRECT).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::START_PRINT).has_value());
    CHECK_FALSE(phase_of_category(OperationCategory::UNKNOWN).has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "phase label: the short and long name of every phase",
                 "[print_start_phase_label]") {
    lv_init_safe();
    struct Row {
        PrintStartPhase phase;
        const char* step;
        const char* banner;
    };
    const Row rows[] = {
        {PrintStartPhase::IDLE, "", ""},
        {PrintStartPhase::INITIALIZING, "", "Preparing Print..."},
        {PrintStartPhase::HOMING, "Homing", "Homing..."},
        {PrintStartPhase::HEATING_BED, "Heating bed", "Heating Bed..."},
        {PrintStartPhase::SOAKING, "Heat soak", "Heat Soaking..."},
        {PrintStartPhase::HEATING_CHAMBER, "Heating chamber", "Heating Chamber..."},
        {PrintStartPhase::HEATING_NOZZLE, "Heating nozzle", "Heating Nozzle..."},
        {PrintStartPhase::QGL, "Quad gantry level", "Leveling Gantry..."},
        {PrintStartPhase::Z_TILT, "Z tilt", "Z Tilt Adjust..."},
        {PrintStartPhase::BED_MESH, "Bed mesh", "Loading Bed Mesh..."},
        {PrintStartPhase::CLEANING, "Cleaning nozzle", "Cleaning Nozzle..."},
        {PrintStartPhase::PURGING, "Purge", "Purging..."},
        {PrintStartPhase::COMPLETE, "", "Starting Print..."},
    };
    for (const Row& row : rows) {
        CAPTURE(static_cast<int>(row.phase));
        CHECK(std::string(print_start_phase_step_key(row.phase)) == row.step);
        CHECK(std::string(print_start_phase_step_label(row.phase)) == row.step);
        CHECK(std::string(print_start_phase_signal_message(row.phase)) == row.banner);
    }
}

TEST_CASE("phase label: the phase each pre-print option toggles", "[print_start_phase_label]") {
    CHECK(print_start_phase_of_option("bed_mesh") == PrintStartPhase::BED_MESH);
    CHECK(print_start_phase_of_option("qgl") == PrintStartPhase::QGL);
    CHECK(print_start_phase_of_option("z_tilt") == PrintStartPhase::Z_TILT);
    CHECK(print_start_phase_of_option("nozzle_clean") == PrintStartPhase::CLEANING);
    CHECK(print_start_phase_of_option("purge_line") == PrintStartPhase::PURGING);

    CHECK_FALSE(print_start_phase_of_option("timelapse").has_value());
    CHECK_FALSE(print_start_phase_of_option("ai_detect").has_value());
    CHECK_FALSE(print_start_phase_of_option("").has_value());
}
