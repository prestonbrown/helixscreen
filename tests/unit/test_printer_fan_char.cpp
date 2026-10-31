// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_fan_char.cpp
 * @brief Characterization tests for PrinterState fan domain
 *
 * These tests capture the CURRENT behavior of fan-related subjects
 * in PrinterState before extraction to a dedicated PrinterFanState class.
 *
 * Static subjects (2 total):
 * - fan_speed_ (int, 0-100% - main part cooling fan speed)
 * - fans_version_ (int, incremented on fan list changes)
 *
 * Dynamic subjects (per-fan):
 * - fan_speed_subjects_[name] (int, 0-100% for each discovered fan)
 *
 * JSON format: {"fan": {"speed": 0.75}} or {"heater_fan hotend_fan": {"speed": 0.5}}
 * - Values are 0.0-1.0 floats, converted to 0-100% integers
 *
 * Fan types:
 * - "fan" -> PART_COOLING (controllable)
 * - "heater_fan *" -> HEATER_FAN (not controllable)
 * - "controller_fan *" -> CONTROLLER_FAN (not controllable)
 * - "fan_generic *" -> GENERIC_FAN (controllable)
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "printer_fan_state.h"
#include "printer_state.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;
using helix::FanType;

namespace {

/// The process-wide PrinterState, reset and with its subjects initialised
/// without XML registration.
PrinterState& fresh_state() {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    return state;
}

} // namespace

// ============================================================================
// Initial State Tests - Document default initialization behavior
// ============================================================================

TEST_CASE("Fan characterization: initial values after init", "[characterization][fan][init]") {
    PrinterState& state = fresh_state();

    SECTION("no per-fan subjects initially") {
        // Before init_fans(), no per-fan subjects exist
        REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);
        REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") == nullptr);
    }

    SECTION("fans vector is empty initially") {
        REQUIRE(state.fan_state().get_fans().empty());
    }
}

// ============================================================================
// init_fans() Tests - Fan discovery and per-fan subject creation
// ============================================================================

TEST_CASE("Fan characterization: init_fans creates per-fan subjects",
          "[characterization][fan][init_fans]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux_fan"});

    SECTION("per-fan subjects created for each fan") {
        REQUIRE(state.fan_state().get_fan_speed_subject("fan") != nullptr);
        REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") != nullptr);
        REQUIRE(state.fan_state().get_fan_speed_subject("fan_generic aux_fan") != nullptr);
    }

    SECTION("unknown fan returns nullptr") {
        REQUIRE(state.fan_state().get_fan_speed_subject("nonexistent") == nullptr);
        REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan other_fan") == nullptr);
    }

    SECTION("fans_version increments on init_fans") {
        int initial_version = lv_subject_get_int(state.fan_state().get_fans_version_subject());
        // First init_fans already bumped it, so initial_version should be 1
        REQUIRE(initial_version == 1);
    }
}

TEST_CASE("Fan characterization: init_fans classifies each fan by its object name",
          "[characterization][fan][init_fans][type][controllable]") {
    PrinterState& state = fresh_state();
    struct Row {
        const char* object_name;
        FanType type;
        bool controllable;
    };
    const Row rows[] = {
        {"fan", FanType::PART_COOLING, true},
        {"heater_fan hotend_fan", FanType::HEATER_FAN, false},
        {"controller_fan mcu_fan", FanType::CONTROLLER_FAN, false},
        {"fan_generic aux", FanType::GENERIC_FAN, true},
    };
    std::vector<std::string> names;
    for (const auto& row : rows) {
        names.emplace_back(row.object_name);
    }
    state.fan_state().init_fans(names);

    const auto& fans = state.fan_state().get_fans();
    REQUIRE(fans.size() == std::size(rows));
    for (size_t i = 0; i < fans.size(); ++i) {
        INFO(rows[i].object_name);
        CHECK(fans[i].object_name == rows[i].object_name);
        CHECK(fans[i].type == rows[i].type);
        CHECK(fans[i].is_controllable == rows[i].controllable);
        CHECK(fans[i].speed_percent == 0);
    }
}

// ============================================================================
// Fan Speed Update Tests - JSON parsing and subject updates
// ============================================================================

TEST_CASE("Fan characterization: a status speed becomes a percent on the fan's subjects",
          "[characterization][fan][update][per-fan][faninfo]") {
    // Klipper reports 0.0-1.0; the part fan also drives the main fan_speed_ subject.
    struct Row {
        const char* object;
        double speed;
        int percent;
    };
    const Row rows[] = {
        {"fan", 1.0, 100},
        {"fan", 0.75, 75},
        {"fan", 0.5, 50},
        {"fan", 0.25, 25},
        {"fan", 0.8, 80},
        {"fan", 0.65, 65},
        {"heater_fan hotend_fan", 0.6, 60},
        {"heater_fan hotend_fan", 0.45, 45},
        {"fan_generic aux", 0.4, 40},
    };
    const std::vector<std::string> objects = {"fan", "heater_fan hotend_fan", "fan_generic aux"};
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.object << " at " << row.speed) {
            PrinterState& state = fresh_state();
            state.fan_state().init_fans(objects);
            state.update_from_status(json{{row.object, {{"speed", row.speed}}}});

            CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject(row.object)) ==
                  row.percent);
            const auto& fans = state.fan_state().get_fans();
            const auto fan = std::find_if(fans.begin(), fans.end(), [&](const auto& f) {
                return f.object_name == row.object;
            });
            REQUIRE(fan != fans.end());
            CHECK(fan->speed_percent == row.percent);
            if (std::string(row.object) == "fan") {
                CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == row.percent);
            }
        }
    }
}

TEST_CASE("Fan characterization: each fan keeps its own speed, and the part fan turns off",
          "[characterization][fan][update][per-fan]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});

    state.update_from_status(json{{"fan", {{"speed", 0.9}}}});
    state.update_from_status(json{{"heater_fan hotend_fan", {{"speed", 0.3}}}});
    CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 90);
    CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) ==
          30);
    CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan_generic aux")) == 0);

    state.update_from_status(json{{"fan", {{"speed", 0.0}}}});
    CHECK(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 0);
}

// ============================================================================
// max_power normalization — Klipper reports `speed` scaled by the fan's
// configured max_power (last_fan_value = value * max_power). Divide it back out
// so a fan running full-on reads 100%, matching Mainsail. (heater-fan 50% bug)
// ============================================================================

TEST_CASE("Fan max_power normalization", "[fan][update][max_power]") {
    PrinterState& state = fresh_state();

    SECTION("heater_fan full-on with max_power 0.5 reads 100%") {
        // Klipper reports speed=0.5 for a fan configured max_power: 0.5 running
        // at logical full. Without normalization HelixScreen showed 50%.
        state.fan_state().init_fans({"heater_fan hotend_fan"}, {},
                                    {{"heater_fan hotend_fan", 0.5}});
        json status = {{"heater_fan hotend_fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) == 100);
    }

    SECTION("no max_power configured leaves speed unchanged (default 1.0)") {
        state.fan_state().init_fans({"heater_fan hotend_fan"});
        json status = {{"heater_fan hotend_fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) == 50);
    }

    SECTION("part fan normalization drives the main fan_speed_ subject") {
        // Part fan with max_power 0.8: a logical 50% request stores 0.4; dividing
        // back out yields 50% on the hero slider subject.
        state.fan_state().init_fans({"fan"}, {}, {{"fan", 0.8}});
        json status = {{"fan", {{"speed", 0.4}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 50);
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 50);
    }

    SECTION("normalized result clamps to 100%") {
        // Defensive: even if a report exceeds max_power, don't overshoot 100.
        state.fan_state().init_fans({"heater_fan hotend_fan"}, {},
                                    {{"heater_fan hotend_fan", 0.5}});
        json status = {{"heater_fan hotend_fan", {{"speed", 0.6}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) == 100);
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on fan changes
// ============================================================================

TEST_CASE("Fan characterization: observer fires when fan_speed changes",
          "[characterization][fan][observer]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan"});

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer =
        lv_subject_add_observer(state.fan_state().get_fan_speed_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0

    // Update fan speed
    json status = {{"fan", {{"speed", 0.75}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2); // At least one more notification
    REQUIRE(user_data[1] == 75);

    lv_observer_remove(observer);
}

TEST_CASE("Fan characterization: observer fires on per-fan subject change",
          "[characterization][fan][observer][per-fan]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"heater_fan hotend_fan"});

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_subject_t* per_fan_subject =
        state.fan_state().get_fan_speed_subject("heater_fan hotend_fan");
    REQUIRE(per_fan_subject != nullptr);

    lv_observer_t* observer = lv_subject_add_observer(per_fan_subject, observer_cb, user_data);

    // Initial notification on add
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0);

    // Update fan speed
    json status = {{"heater_fan hotend_fan", {{"speed", 0.5}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 50);

    lv_observer_remove(observer);
}

TEST_CASE("Fan characterization: fans_version observer fires on init_fans",
          "[characterization][fan][observer][version]") {
    PrinterState& state = fresh_state();

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer = lv_subject_add_observer(state.fan_state().get_fans_version_subject(),
                                                      observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0);

    // init_fans should bump version
    state.fan_state().init_fans({"fan"});

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 1);

    // Calling init_fans again should bump version again
    state.fan_state().init_fans({"fan", "heater_fan hotend"});

    REQUIRE(user_data[0] >= 3);
    REQUIRE(user_data[1] == 2);

    lv_observer_remove(observer);
}

// ============================================================================
// Update Ignored Tests - Updates without init_fans or for unknown fans
// ============================================================================

TEST_CASE("Fan characterization: updates before init_fans", "[characterization][fan][no_init]") {
    PrinterState& state = fresh_state();

    // Do NOT call init_fans

    SECTION("main fan subject still updates (static subject)") {
        json status = {{"fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        // The static fan_speed_ subject should still update
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 50);
    }

    SECTION("per-fan subject returns nullptr without init_fans") {
        // Without init_fans, no per-fan subjects exist
        REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);
    }
}

TEST_CASE("Fan characterization: update for undiscovered fan is ignored",
          "[characterization][fan][unknown]") {
    PrinterState& state = fresh_state();

    // Only init some fans
    state.fan_state().init_fans({"fan"});

    SECTION("update for unknown heater_fan does not create subject") {
        json status = {{"heater_fan hotend_fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        // Should not create a subject for unknown fan
        REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") == nullptr);
    }

    SECTION("known fan still updates correctly") {
        json status = {{"fan", {{"speed", 0.75}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 75);
    }
}

// ============================================================================
// Reset Cycle Tests - Verify behavior across reset_for_testing cycles
// ============================================================================

TEST_CASE("Fan characterization: per-fan subjects cleared on reset",
          "[characterization][fan][reset]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"});

    // Verify subjects exist
    REQUIRE(state.fan_state().get_fan_speed_subject("fan") != nullptr);
    REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") != nullptr);

    // Update values
    json status = {{"fan", {{"speed", 0.8}}}};
    state.update_from_status(status);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 80);

    // Reset
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Per-fan subjects should be cleared
    REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);
    REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") == nullptr);

    // fans_ is cleared too. It used to survive reset() — the characterization
    // note here called that out and left the refactor for later. It stopped being
    // cosmetic once init_fans() began carrying speed_percent/ever_ran/rpm across a
    // re-init (#1181): a surviving fans_ leaks live readings into the next test.
    REQUIRE(state.fan_state().get_fans().empty());
}

TEST_CASE("Fan characterization: static subjects reset to defaults",
          "[characterization][fan][reset]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan"});

    // Set values
    json status = {{"fan", {{"speed", 0.75}}}};
    state.update_from_status(status);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 75);

    int version_before = lv_subject_get_int(state.fan_state().get_fans_version_subject());
    REQUIRE(version_before == 1);

    // Reset
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Static subjects should be back to defaults
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fans_version_subject()) == 0);
}

TEST_CASE("Fan characterization: reinitializing fans replaces previous subjects",
          "[characterization][fan][reinit]") {
    PrinterState& state = fresh_state();

    // First init
    state.fan_state().init_fans({"fan"});
    lv_subject_t* fan_subject_v1 = state.fan_state().get_fan_speed_subject("fan");
    REQUIRE(fan_subject_v1 != nullptr);

    json status = {{"fan", {{"speed", 0.5}}}};
    state.update_from_status(status);
    REQUIRE(lv_subject_get_int(fan_subject_v1) == 50);

    // Reinit with different fans
    state.fan_state().init_fans({"heater_fan hotend_fan"});

    // Old fan subject should be gone
    REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);

    // New fan subject should exist
    REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan") != nullptr);

    // fans_version should have incremented
    REQUIRE(lv_subject_get_int(state.fan_state().get_fans_version_subject()) == 2);
}

// ============================================================================
// Independence Tests - Verify fan updates don't affect other subjects
// ============================================================================

TEST_CASE("Fan characterization: fan update does not affect non-fan subjects",
          "[characterization][fan][independence]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan"});

    // Set some non-fan values first
    json initial = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
    state.update_from_status(initial);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000); // centimm

    // Now update fan
    json fan_update = {{"fan", {{"speed", 0.75}}}};
    state.update_from_status(fan_update);

    // Fan value should be updated
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 75);

    // Position should be unchanged (in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000);
}

TEST_CASE("Fan characterization: non-fan update does not affect fan subjects",
          "[characterization][fan][independence]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan"});

    // Set fan value first
    json fan_status = {{"fan", {{"speed", 0.8}}}};
    state.update_from_status(fan_status);

    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 80);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 80);

    // Now update position (non-fan)
    json position_update = {{"toolhead", {{"position", {50.0, 75.0, 10.0}}}}};
    state.update_from_status(position_update);

    // Fan values should be unchanged
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 80);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 80);
}

// ============================================================================
// Multiple Observer Tests - Verify observer isolation and independence
// ============================================================================

TEST_CASE("Fan characterization: observers on different fan subjects are independent",
          "[characterization][fan][observer][independence]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"});

    int main_count = 0;
    int per_fan_count = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* main_observer = lv_subject_add_observer(
        state.fan_state().get_fan_speed_subject(), observer_cb, &main_count);
    lv_observer_t* per_fan_observer = lv_subject_add_observer(
        state.fan_state().get_fan_speed_subject("fan"), observer_cb, &per_fan_count);

    // Both observers fire on initial add
    REQUIRE(main_count == 1);
    REQUIRE(per_fan_count == 1);

    // Update main fan
    json status = {{"fan", {{"speed", 0.5}}}};
    state.update_from_status(status);

    // Both should have received notifications (main fan update affects both subjects)
    REQUIRE(main_count >= 2);
    REQUIRE(per_fan_count >= 2);

    lv_observer_remove(main_observer);
    lv_observer_remove(per_fan_observer);
}

// ============================================================================
// Edge Cases - Boundary values and unusual inputs
// ============================================================================

TEST_CASE("Fan characterization: edge cases and boundary values", "[characterization][fan][edge]") {
    PrinterState& state = fresh_state();
    state.fan_state().init_fans({"fan"});

    SECTION("very small speed values") {
        json status = {{"fan", {{"speed", 0.01}}}};
        state.update_from_status(status);

        // 0.01 * 100 = 1%
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 1);
    }

    SECTION("speed value exactly 0.5") {
        json status = {{"fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 50);
    }

    SECTION("speed value exactly 1.0") {
        json status = {{"fan", {{"speed", 1.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 100);
    }

    SECTION("speed value slightly above 1.0 (clamping behavior)") {
        json status = {{"fan", {{"speed", 1.01}}}};
        state.update_from_status(status);

        // Depends on implementation - typically clamped to 100
        int speed = lv_subject_get_int(state.fan_state().get_fan_speed_subject());
        REQUIRE(speed <= 101); // Allow for 101 if not clamped
    }

    SECTION("missing speed field is handled gracefully") {
        json status = {{"fan", {{"rpm", 5000}}}};
        state.update_from_status(status);

        // Value should remain at initial 0 (no crash)
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 0);
    }

    SECTION("non-number speed field is handled gracefully") {
        json status = {{"fan", {{"speed", "fast"}}}};
        state.update_from_status(status);

        // Value should remain at initial 0 (no crash)
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 0);
    }
}

TEST_CASE("Fan characterization: empty init_fans", "[characterization][fan][edge]") {
    PrinterState& state = fresh_state();

    SECTION("init_fans with empty vector") {
        state.fan_state().init_fans({});

        REQUIRE(state.fan_state().get_fans().empty());
        // Version should still increment
        REQUIRE(lv_subject_get_int(state.fan_state().get_fans_version_subject()) == 1);
    }
}

TEST_CASE("Fan characterization: fan with unusual name format", "[characterization][fan][edge]") {
    PrinterState& state = fresh_state();

    SECTION("fan_generic with underscore in name") {
        state.fan_state().init_fans({"fan_generic aux_cooling_fan"});

        REQUIRE(state.fan_state().get_fan_speed_subject("fan_generic aux_cooling_fan") != nullptr);
        REQUIRE(state.fan_state().get_fans()[0].type == FanType::GENERIC_FAN);
        REQUIRE(state.fan_state().get_fans()[0].is_controllable == true);
    }

    SECTION("heater_fan with multiple words") {
        state.fan_state().init_fans({"heater_fan my_custom_hotend_fan"});

        REQUIRE(state.fan_state().get_fan_speed_subject("heater_fan my_custom_hotend_fan") !=
                nullptr);
        REQUIRE(state.fan_state().get_fans()[0].type == FanType::HEATER_FAN);
        REQUIRE(state.fan_state().get_fans()[0].is_controllable == false);
    }
}

// ============================================================================
// Init Ordering Tests - Verify init_fans must precede update_from_status
// ============================================================================

TEST_CASE("Fan characterization: init_fans before update populates per-fan speeds",
          "[characterization][fan][ordering]") {
    PrinterState& state = fresh_state();

    // Correct order: init fans FIRST, then send status updates
    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"});

    json status = {{"fan", {{"speed", 0.75}}}, {"heater_fan hotend_fan", {{"speed", 0.6}}}};
    state.update_from_status(status);

    SECTION("per-fan subjects reflect updated speeds") {
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 75);
        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) == 60);
    }

    SECTION("FanInfo speed_percent reflects updated speeds") {
        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 75);
        REQUIRE(fans[1].speed_percent == 60);
    }
}

TEST_CASE("Fan characterization: update before init_fans drops per-fan speeds",
          "[characterization][fan][ordering]") {
    PrinterState& state = fresh_state();

    // Wrong order: send status updates BEFORE init_fans
    json status = {{"fan", {{"speed", 0.75}}}, {"heater_fan hotend_fan", {{"speed", 0.6}}}};
    state.update_from_status(status);

    // Now init fans (after updates were already sent)
    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"});

    SECTION("per-fan subjects are at 0 — updates were dropped") {
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 0);
        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) == 0);
    }

    SECTION("FanInfo speed_percent is 0 — updates were dropped") {
        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 0);
        REQUIRE(fans[1].speed_percent == 0);
    }

    SECTION("static fan_speed subject was updated despite wrong order") {
        // The static fan_speed_ subject updates regardless of init_fans,
        // but init_fans resets it to 0 when creating subjects
        // After init_fans, subsequent updates will work
        json new_status = {{"fan", {{"speed", 0.5}}}};
        state.update_from_status(new_status);
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 50);
    }
}

// ============================================================================
// FanRoleConfig Tests - Configured fan role classification and naming
// ============================================================================

TEST_CASE("Fan role config: configured part fan classified as PART_COOLING", "[fan][role_config]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic Fanm106";

    // bare "fan" is skipped when a different fan is configured as part cooling
    state.fan_state().init_fans(
        {"fan", "fan_generic Fanm106", "heater_fan heat_fan", "fan_generic chamber_fan"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("bare 'fan' is skipped — only 3 fans registered") {
        REQUIRE(fans.size() == 3);
    }

    SECTION("configured part fan is classified as PART_COOLING") {
        REQUIRE(fans[0].type == helix::FanType::PART_COOLING);
        REQUIRE(fans[0].is_controllable == true);
    }

    SECTION("other fans retain normal classification") {
        REQUIRE(fans[1].type == helix::FanType::HEATER_FAN);
        REQUIRE(fans[2].type == helix::FanType::GENERIC_FAN);
    }
}

TEST_CASE("Fan role config: display name overrides from configured roles",
          "[fan][role_config][display_name]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic Fanm106";
    roles.hotend_fan = "heater_fan heat_fan";
    roles.chamber_fan = "fan_generic chamber_fan";
    roles.exhaust_fan = "fan_generic external_fan";

    // bare "fan" is skipped when a different fan is configured as part cooling
    state.fan_state().init_fans({"fan", "fan_generic Fanm106", "heater_fan heat_fan",
                                 "fan_generic chamber_fan", "fan_generic external_fan",
                                 "controller_fan driver_fan"},
                                roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("bare 'fan' is skipped — only 5 fans registered") {
        REQUIRE(fans.size() == 5);
    }

    SECTION("configured part fan gets 'Part Fan' display name") {
        REQUIRE(fans[0].display_name == "Part Fan");
    }

    SECTION("configured hotend fan gets 'Hotend Fan' display name") {
        REQUIRE(fans[1].display_name == "Hotend Fan");
    }

    SECTION("configured chamber fan gets 'Chamber Fan' display name") {
        REQUIRE(fans[2].display_name == "Chamber Fan");
    }

    SECTION("configured exhaust fan gets 'Exhaust Fan' display name") {
        REQUIRE(fans[3].display_name == "Exhaust Fan");
    }

    SECTION("unconfigured fan uses auto-generated display name") {
        // "controller_fan driver_fan" not in any role config -> auto-generated
        REQUIRE(fans[4].display_name == "Driver Fan");
    }
}

TEST_CASE("Fan labels: every role at every length", "[fan][fan_labels]") {
    struct Row {
        FanRole role;
        const char* chip;
        const char* word;
        const char* full;
    };
    const Row rows[] = {
        {FanRole::Part, "Part", "Part", "Part Fan"},
        {FanRole::Hotend, "HE", "Hotend", "Hotend Fan"},
        {FanRole::Chamber, "Chamber", "Chamber", "Chamber Fan"},
        {FanRole::Exhaust, "Exhaust", "Exhaust", "Exhaust Fan"},
        {FanRole::Aux, "Aux", "Aux", "Aux Fan"},
    };
    for (const Row& row : rows) {
        CAPTURE(static_cast<int>(row.role));
        CHECK(std::string(fan_role_label(row.role, FanLabelLength::Chip)) == row.chip);
        CHECK(std::string(fan_role_label(row.role, FanLabelLength::Word)) == row.word);
        CHECK(std::string(fan_role_label(row.role, FanLabelLength::Full)) == row.full);
    }
}

TEST_CASE("Fan labels: a type pill reads as the role its type fills", "[fan][fan_labels]") {
    CHECK(std::string(fan_type_label(FanType::PART_COOLING)) == "Part");
    CHECK(std::string(fan_type_label(FanType::HEATER_FAN)) == "HE");
    CHECK(std::string(fan_type_label(FanType::CONTROLLER_FAN)) == "Controller");
    CHECK(std::string(fan_type_label(FanType::TEMPERATURE_FAN)) == "Temp");
    CHECK(std::string(fan_type_label(FanType::GENERIC_FAN)) == "Generic");
    CHECK(std::string(fan_type_label(FanType::OUTPUT_PIN_FAN)) == "Output Pin");
}

TEST_CASE("Fan role config: a configured aux fan is named by its role",
          "[fan][role_config][display_name]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.aux_fan = "fan_generic nevermore";
    state.fan_state().init_fans({"fan", "fan_generic nevermore"}, roles);

    const auto& fans = state.fan_state().get_fans();
    REQUIRE(fans.size() == 2);
    CHECK(fans[0].display_name == "Part Cooling Fan");
    CHECK(fans[1].display_name == "Aux Fan");
}

TEST_CASE("Fan role config: empty roles uses default behavior", "[fan][role_config]") {
    PrinterState& state = fresh_state();

    // Default-constructed FanRoleConfig has empty strings
    helix::FanRoleConfig roles;

    state.fan_state().init_fans({"fan", "fan_generic Fanm106"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("without role config, fan_generic is GENERIC_FAN") {
        REQUIRE(fans[1].type == helix::FanType::GENERIC_FAN);
    }

    SECTION("without role config, fan_generic gets auto-generated name") {
        REQUIRE(fans[1].display_name == "Fanm106 Fan");
    }
}

TEST_CASE("Fan role config: configured part fan updates hero slider subject",
          "[fan][role_config][update]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic Fanm106";

    state.fan_state().init_fans({"fan_generic Fanm106"}, roles);

    SECTION("configured part fan speed updates main fan_speed subject") {
        json status = {{"fan_generic Fanm106", {{"speed", 0.69}}}};
        state.update_from_status(status);

        // Main hero slider subject should reflect configured part fan speed
        REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 69);
    }

    SECTION("per-fan subject also updates") {
        json status = {{"fan_generic Fanm106", {{"speed", 0.42}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(
                    state.fan_state().get_fan_speed_subject("fan_generic Fanm106")) == 42);
    }
}

TEST_CASE("Fan role config: bare 'fan' skipped when different part fan configured",
          "[fan][role_config]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic fanM106";

    state.fan_state().init_fans({"fan", "fan_generic fanM106", "heater_fan heat_fan"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("bare 'fan' is excluded from fans list") {
        REQUIRE(fans.size() == 2);
        REQUIRE(fans[0].object_name == "fan_generic fanM106");
        REQUIRE(fans[1].object_name == "heater_fan heat_fan");
    }

    SECTION("no per-fan subject created for bare 'fan'") {
        REQUIRE(state.fan_state().get_fan_speed_subject("fan") == nullptr);
    }

    SECTION("configured part fan subject exists") {
        REQUIRE(state.fan_state().get_fan_speed_subject("fan_generic fanM106") != nullptr);
    }
}

TEST_CASE("Fan role config: bare 'fan' NOT skipped when it IS the part fan", "[fan][role_config]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles;
    roles.part_fan = "fan";

    state.fan_state().init_fans({"fan", "heater_fan heat_fan"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("bare 'fan' is included when it IS the configured part fan") {
        REQUIRE(fans.size() == 2);
        REQUIRE(fans[0].object_name == "fan");
        REQUIRE(fans[1].object_name == "heater_fan heat_fan");
    }

    SECTION("per-fan subject exists for bare 'fan'") {
        REQUIRE(state.fan_state().get_fan_speed_subject("fan") != nullptr);
    }
}

TEST_CASE("Fan role config: bare 'fan' NOT skipped when roles are empty (default)",
          "[fan][role_config]") {
    PrinterState& state = fresh_state();

    helix::FanRoleConfig roles; // default: part_fan is empty

    state.fan_state().init_fans({"fan", "heater_fan heat_fan"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("bare 'fan' is included with default (empty) role config") {
        REQUIRE(fans.size() == 2);
        REQUIRE(fans[0].object_name == "fan");
    }
}

TEST_CASE("Fan role config: canonical 'fan' part_fan does not create redundant override",
          "[fan][role_config]") {
    PrinterState& state = fresh_state();

    // When the configured part fan IS the canonical "fan", don't add a role override
    // (it already has a direct mapping to "Part Cooling Fan")
    helix::FanRoleConfig roles;
    roles.part_fan = "fan";

    state.fan_state().init_fans({"fan"}, roles);

    const auto& fans = state.fan_state().get_fans();

    SECTION("canonical fan keeps direct mapping name") {
        REQUIRE(fans[0].display_name == "Part Cooling Fan");
    }

    SECTION("still classified as PART_COOLING") {
        REQUIRE(fans[0].type == helix::FanType::PART_COOLING);
    }
}

TEST_CASE("Fan characterization: output_pin fan type classification",
          "[characterization][fan][type][output_pin]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans(
        {"output_pin fan0", "output_pin fan1", "output_pin fan2", "heater_fan hotend_fan"});

    const auto& fans = state.fan_state().get_fans();

    SECTION("output_pin fan0 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[0].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("output_pin fan1 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[1].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("output_pin fan2 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[2].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("OUTPUT_PIN_FAN is controllable") {
        REQUIRE(fans[0].is_controllable == true);
    }

    SECTION("heater_fan is still HEATER_FAN") {
        REQUIRE(fans[3].type == FanType::HEATER_FAN);
    }
}

TEST_CASE("Fan characterization: FanInfo rpm field", "[characterization][fan][rpm]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"output_pin fan0"});

    const auto& fans = state.fan_state().get_fans();

    SECTION("rpm is nullopt by default") {
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }
}

TEST_CASE("Fan characterization: output_pin fan speed from value field",
          "[characterization][fan][update][output_pin]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"output_pin fan0", "output_pin fan1", "heater_fan hotend_fan"});

    SECTION("output_pin value 1.0 -> 100%") {
        json status = {{"output_pin fan0", {{"value", 1.0}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 100);
    }

    SECTION("output_pin value 0.5 -> 50%") {
        json status = {{"output_pin fan0", {{"value", 0.5}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 50);
    }

    SECTION("output_pin value 0.0 -> 0%") {
        json status = {{"output_pin fan0", {{"value", 0.0}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 0);
    }

    SECTION("multiple output_pin updates in one status") {
        json status = {{"output_pin fan0", {{"value", 0.75}}},
                       {"output_pin fan1", {{"value", 0.25}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 75);
        REQUIRE(fans[1].speed_percent == 25);
    }

    SECTION("output_pin update does not affect heater_fan") {
        json status = {{"output_pin fan0", {{"value", 1.0}}},
                       {"heater_fan hotend_fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].speed_percent == 100); // output_pin fan0
        REQUIRE(fans[2].speed_percent == 50);  // heater_fan hotend_fan
    }
}

TEST_CASE("Fan characterization: fan_feedback RPM updates",
          "[characterization][fan][update][fan_feedback]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2"});

    SECTION("fan_feedback maps fanN_speed to output_pin fanN rpm") {
        json status = {
            {"fan_feedback", {{"fan0_speed", 16000}, {"fan1_speed", 3692}, {"fan2_speed", 0}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE(fans[0].rpm.has_value());
        REQUIRE(fans[0].rpm.value() == 16000);
        REQUIRE(fans[1].rpm.has_value());
        REQUIRE(fans[1].rpm.value() == 3692);
        REQUIRE(fans[2].rpm.has_value());
        REQUIRE(fans[2].rpm.value() == 0);
    }

    SECTION("fan_feedback for unknown fanN is ignored") {
        json status = {{"fan_feedback", {{"fan5_speed", 1000}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }

    SECTION("fan_feedback with non-numeric value is ignored") {
        json status = {{"fan_feedback", {{"fan0_speed", nullptr}}}};
        state.update_from_status(status);

        const auto& fans = state.fan_state().get_fans();
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }
}

TEST_CASE("Fan characterization: duplicate chamber_fan disambiguated by role",
          "[characterization][fan][names][chamber]") {
    PrinterState& state = fresh_state();

    // A Creality K2 Plus exposes TWO distinct Klipper objects that share the
    // suffix "chamber_fan": the PTC heater element's cooling fan and the chamber
    // cooling fan. Both must read as distinct, role-specific names rather than
    // colliding on a flat "Chamber Fan".
    state.fan_state().init_fans({"heater_fan chamber_fan", "temperature_fan chamber_fan"});

    const auto& fans = state.fan_state().get_fans();

    SECTION("heater_fan chamber_fan reads as 'Chamber Heater Fan'") {
        REQUIRE(fans[0].display_name == "Chamber Heater Fan");
    }

    SECTION("temperature_fan chamber_fan reads as 'Chamber Cooling Fan'") {
        REQUIRE(fans[1].display_name == "Chamber Cooling Fan");
    }

    SECTION("the two chamber fans have distinct names") {
        REQUIRE(fans[0].display_name != fans[1].display_name);
    }
}

TEST_CASE("Fan characterization: single chamber fan still reads sensibly",
          "[characterization][fan][names][chamber]") {
    PrinterState& state = fresh_state();

    SECTION("lone heater_fan chamber_fan reads as 'Chamber Heater Fan'") {
        state.fan_state().init_fans({"heater_fan chamber_fan"});
        REQUIRE(state.fan_state().get_fans()[0].display_name == "Chamber Heater Fan");
    }

    SECTION("lone temperature_fan chamber_fan reads as 'Chamber Cooling Fan'") {
        state.fan_state().init_fans({"temperature_fan chamber_fan"});
        REQUIRE(state.fan_state().get_fans()[0].display_name == "Chamber Cooling Fan");
    }

    SECTION("a user-set custom name still wins over role disambiguation") {
        auto* config = Config::get_instance();
        const std::string name_key = config->df() + "fans/names/heater_fan chamber_fan";
        const std::string name_orig = config->get<std::string>(name_key, "");
        config->set(name_key, std::string("My Fan"));

        state.fan_state().init_fans({"heater_fan chamber_fan"});
        REQUIRE(state.fan_state().get_fans()[0].display_name == "My Fan");

        config->set(name_key, name_orig); // restore
    }
}

TEST_CASE("Fan characterization: custom display names from config",
          "[characterization][fan][names]") {
    PrinterState& state = fresh_state();

    // Simulate saved custom names in config
    auto* config = Config::get_instance();
    const std::string key0 = config->df() + "fans/names/output_pin fan0";
    const std::string key1 = config->df() + "fans/names/output_pin fan1";
    const std::string orig0 = config->get<std::string>(key0, "");
    const std::string orig1 = config->get<std::string>(key1, "");
    config->set(key0, std::string("Part Fan"));
    config->set(key1, std::string("Electronics Fan"));

    state.fan_state().init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2"});

    const auto& fans = state.fan_state().get_fans();

    SECTION("fan with custom name uses it") {
        REQUIRE(fans[0].display_name == "Part Fan");
        REQUIRE(fans[1].display_name == "Electronics Fan");
    }

    SECTION("fan without custom name gets auto-generated name") {
        // fan2 has no custom name, should get default
        REQUIRE_FALSE(fans[2].display_name.empty());
    }

    // Restore original values
    config->set(key0, orig0);
    config->set(key1, orig1);
}

// ============================================================================
// Task 4: FanRoleConfig::from_config with live fan list (auto-heal stale roles)
// ============================================================================

TEST_CASE("FanRoleConfig::from_config auto-heals stale part fan to live fan",
          "[characterization][fan][role][hwrole]") {
    lv_init_safe();
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + "fans/part";
    const std::string orig = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string("output_pin fan0"));

    FanRoleConfig roles = FanRoleConfig::from_config(
        cfg, {"fan", "heater_fan hotend_fan", "fan_generic Aux_Cooling_Fan"});

    REQUIRE(roles.part_fan == "fan");

    // Restore
    cfg->set<std::string>(key, orig);
}

TEST_CASE("init_fans: healed roles correctly register 'fan' subject when stale role is restored",
          "[characterization][fan][role][hwrole]") {
    // Exercises the real path: Config has a stale role, from_config heals it, and the
    // healed roles are passed to init_fans so the subject for the correct fan is created.
    PrinterState& state = fresh_state();

    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + "fans/part";
    const std::string orig = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string("output_pin fan0")); // stale role

    FanRoleConfig roles = FanRoleConfig::from_config(cfg, {"fan", "heater_fan hotend_fan"});
    // from_config must have healed the stale role to the canonical "fan"
    REQUIRE(roles.part_fan == "fan");

    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"}, roles);

    // The healed part fan subject must exist
    REQUIRE(state.fan_state().get_fan_speed_subject("fan") != nullptr);

    // Restore
    cfg->set<std::string>(key, orig);
}

TEST_CASE("init_fans keeps [fan] when part role points to an absent object",
          "[characterization][fan][role][hwrole]") {
    PrinterState& state = fresh_state();

    // Defense in depth: a role that resolution did NOT touch (set directly), naming
    // an object absent from the discovered list, must NOT cause [fan] to be skipped.
    FanRoleConfig roles;
    roles.part_fan = "output_pin fan0"; // absent from fan_objects below

    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"}, roles);

    REQUIRE(state.fan_state().get_fan_speed_subject("fan") != nullptr); // not skipped
}

// ============================================================================
// #1181: update_fan_speed() must write the per-fan subject unconditionally,
// never gated on the struct comparison.
//
// These two force a struct/subject split by hand. No supported path produces
// one — init_fans() carries both forward together — so read them as locking the
// property, not as reproducing a field scenario: a gated write can only restore
// the subject when the struct also moves, so any divergence that did arise
// would be permanent on a differential feed. The re-apply scenario that
// actually caused #1181 is covered further down.
// ============================================================================

TEST_CASE("Fan subject is not gated on the struct comparison (#1181)",
          "[fan][reinit][subject_sync]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan"});

    // Fan ramps to 100% — struct and subject agree.
    state.update_from_status({{"fan", {{"speed", 1.0}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 100);

    // Force a split no production path creates: subject to 0, struct still 100.
    lv_subject_set_int(state.fan_state().get_fan_speed_subject("fan"), 0);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 0);

    // Now push the SAME speed. With the old struct-gated code, the gate
    // (fan.speed_percent(100) != 100) would be FALSE and the subject would
    // stay at 0 forever. The fix writes the subject unconditionally.
    state.update_from_status({{"fan", {{"speed", 1.0}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 100);
}

TEST_CASE("Fan subject always reflects latest update even when struct unchanged (#1181)",
          "[fan][update][subject_sync]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan", "heater_fan hotend_fan"});

    // Set both fans to known values.
    state.update_from_status({{"fan", {{"speed", 0.8}}}});
    state.update_from_status({{"heater_fan hotend_fan", {{"speed", 0.6}}}});

    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 80);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) ==
            60);

    // Same forced split as above, with a second fan present to prove the
    // unconditional write stays scoped to the fan that was updated.
    lv_subject_set_int(state.fan_state().get_fan_speed_subject("fan"), 0);

    // Push the same 80% again. Old code: struct(80) != 80 → false → subject
    // stays 0. New code: subject written unconditionally → 80.
    state.update_from_status({{"fan", {{"speed", 0.8}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 80);

    // heater_fan should be unaffected.
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("heater_fan hotend_fan")) ==
            60);
}

// ============================================================================
// #1181: a re-init must not amnesia a fan that is still spinning.
//
// reapply_hardware_roles() calls init_fans() with the subscription already
// live, and Moonraker's notify_status_update is DIFFERENTIAL — a fan holding a
// steady speed reports nothing afterwards. If init_fans() zeroes speed_percent
// and ever_ran, there is no event that ever restores them, so both the speed
// readout and the part-fan classification stay wrong indefinitely.
// ============================================================================

TEST_CASE("Steady-speed fan survives a role re-apply with no new status update (#1181)",
          "[fan][reinit][subject_sync]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan"});
    state.update_from_status({{"fan", {{"speed", 0.6}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 60);

    // Same fan list, same physical fan — this is reapply_hardware_roles(), not a
    // new printer. Nothing follows it, because 60% is unchanged and the feed is
    // differential.
    state.fan_state().init_fans({"fan"});

    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan")) == 60);
}

TEST_CASE("apply_roles re-shadows and un-shadows bare [fan] from the discovered list",
          "[fan][reinit][roles]") {
    PrinterState& state = fresh_state();

    auto names = [&state]() {
        std::vector<std::string> out;
        for (const auto& f : state.fan_state().get_fans())
            out.push_back(f.object_name);
        return out;
    };
    auto has = [&names](const std::string& n) {
        auto v = names();
        return std::find(v.begin(), v.end(), n) != v.end();
    };

    state.fan_state().init_fans({"fan", "fan_generic part_cooling"});
    REQUIRE(has("fan"));

    // Naming the generic fan as part cooling shadows the bare [fan].
    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic part_cooling";
    state.fan_state().apply_roles(roles);
    REQUIRE_FALSE(has("fan"));
    REQUIRE(has("fan_generic part_cooling"));

    // Handing the role back must bring it home. This is the assertion that needs
    // the retained discovery list: [fan] is no longer in fans_, so nothing else
    // remembers it was ever there.
    helix::FanRoleConfig back;
    back.part_fan = "fan";
    state.fan_state().apply_roles(back);
    REQUIRE(has("fan"));
}

TEST_CASE("apply_roles carries live readings, like any other re-init (#1181)",
          "[fan][reinit][roles]") {
    PrinterState& state = fresh_state();

    state.fan_state().init_fans({"fan", "fan_generic aux"});
    state.update_from_status({{"fan_generic aux", {{"speed", 0.4}}}});
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan_generic aux")) == 40);

    helix::FanRoleConfig roles;
    roles.chamber_fan = "fan_generic aux";
    state.fan_state().apply_roles(roles);

    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject("fan_generic aux")) == 40);
}

TEST_CASE("Promoted part fan survives a role re-apply (#1181)", "[fan][reinit][classification]") {
    PrinterState& state = fresh_state();

    // The "stale [fan]" shape #1124 was about: bare [fan] exists but never
    // reports, and the real part cooler is a named generic fan.
    state.fan_state().init_fans({"fan", "fan_generic part_cooling"});
    state.update_from_status({{"fan_generic part_cooling", {{"speed", 0.6}}}});
    REQUIRE(state.fan_state().classify_primary_fans().part == "fan_generic part_cooling");

    // After a re-apply the promotion must hold. Losing ever_ran drops the slot
    // back to the front-most commandable fan — the dead [fan], sitting at 0% —
    // which is the compact row frozen at 0% while All Fans stays correct.
    state.fan_state().init_fans({"fan", "fan_generic part_cooling"});

    REQUIRE(state.fan_state().classify_primary_fans().part == "fan_generic part_cooling");
}

// ============================================================================
// Role display names
// ============================================================================

TEST_CASE("Fan role display names cover every assigned role", "[fan][roles][display]") {
    lv_init_safe();

    helix::PrinterFanState fans;
    fans.init_subjects(/*register_xml=*/false);

    helix::FanRoleConfig roles;
    roles.part_fan = "fan_generic fanM106";
    roles.hotend_fan = "heater_fan heat_fan";
    roles.chamber_fan = "fan_generic chamber_fan";
    roles.exhaust_fan = "fan_generic external_fan";
    roles.aux_fan = "fan_generic internal_fan";

    fans.init_fans({"fan_generic fanM106", "heater_fan heat_fan", "fan_generic chamber_fan",
                    "fan_generic external_fan", "fan_generic internal_fan"},
                   roles);

    auto name_of = [&fans](const std::string& object_name) {
        for (const auto& f : fans.get_fans()) {
            if (f.object_name == object_name) {
                return f.display_name;
            }
        }
        return std::string{"<not discovered>"};
    };

    CHECK(name_of("fan_generic fanM106") == "Part Fan");
    CHECK(name_of("heater_fan heat_fan") == "Hotend Fan");
    CHECK(name_of("fan_generic chamber_fan") == "Chamber Fan");
    CHECK(name_of("fan_generic external_fan") == "Exhaust Fan");
    CHECK(name_of("fan_generic internal_fan") == "Aux Fan");
}
