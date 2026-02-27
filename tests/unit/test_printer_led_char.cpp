// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_led_char.cpp
 * @brief Characterization tests for PrinterState LED domain
 *
 * These tests capture the CURRENT behavior of LED-related subjects
 * in PrinterState before extraction to a dedicated PrinterLedState class.
 *
 * LED subjects (6 total):
 * - led_state_ (int, 0=off, 1=on - derived from color data)
 * - led_r_ (int, 0-255 - red channel)
 * - led_g_ (int, 0-255 - green channel)
 * - led_b_ (int, 0-255 - blue channel)
 * - led_w_ (int, 0-255 - white channel)
 * - led_brightness_ (int, 0-100 - max of RGBW channels as percentage)
 *
 * JSON format: {"neopixel led_strip": {"color_data": [[0.5, 0.25, 0.75, 1.0]]}}
 * - Values are 0.0-1.0 floats, converted to 0-255 integers with rounding
 * - Array is [R, G, B, W], W is optional (defaults to 0)
 * - led_state_ = 1 when any channel > 0, 0 when all channels are 0
 * - led_brightness_ = max(R,G,B,W) * 100 / 255
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Tracked LED Tests - set_tracked_led / get_tracked_led / has_tracked_led
// ============================================================================

TEST_CASE("LED characterization: tracked LED management", "[characterization][led][tracking]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("has_tracked_led returns false initially") {
        REQUIRE(state.has_tracked_led() == false);
    }

    SECTION("get_tracked_led returns empty string initially") {
        REQUIRE(state.get_tracked_led() == "");
    }

    SECTION("set_tracked_led sets the LED name") {
        state.set_tracked_led("neopixel led_strip");
        REQUIRE(state.get_tracked_led() == "neopixel led_strip");
        REQUIRE(state.has_tracked_led() == true);
    }

    SECTION("set_tracked_led with empty string clears tracking") {
        state.set_tracked_led("neopixel chamber_light");
        REQUIRE(state.has_tracked_led() == true);

        state.set_tracked_led("");
        REQUIRE(state.has_tracked_led() == false);
        REQUIRE(state.get_tracked_led() == "");
    }

    SECTION("set_tracked_led can change tracked LED") {
        state.set_tracked_led("neopixel led_strip");
        REQUIRE(state.get_tracked_led() == "neopixel led_strip");

        state.set_tracked_led("led status_led");
        REQUIRE(state.get_tracked_led() == "led status_led");
    }
}

// ============================================================================
// LED Update Tests - Verify color_data parsing and conversion
// ============================================================================

TEST_CASE("LED characterization: LED updates from JSON", "[characterization][led][update]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Must set tracked LED before updates will apply
    state.set_tracked_led("neopixel led_strip");

    SECTION("full brightness white LED (all channels 1.0)") {
        json status = {{"neopixel led_strip", {{"color_data", {{1.0, 1.0, 1.0, 1.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 100);
    }

    SECTION("LED off (all channels 0.0)") {
        // First turn on
        json on_status = {{"neopixel led_strip", {{"color_data", {{1.0, 1.0, 1.0, 1.0}}}}}};
        state.update_from_status(on_status);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);

        // Then turn off
        json off_status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(off_status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 0);
    }

    SECTION("half brightness conversion (0.5 -> 128)") {
        // 0.5 * 255 + 0.5 = 128.0 -> rounds to 128
        json status = {{"neopixel led_strip", {{"color_data", {{0.5, 0.5, 0.5, 0.5}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 128);
        // brightness = 128 * 100 / 255 = 50
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 50);
    }

    SECTION("red only LED") {
        json status = {{"neopixel led_strip", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 100);
    }

    SECTION("mixed color LED") {
        // R=0.5 (128), G=0.25 (64), B=0.75 (191), W=0.0 (0)
        json status = {{"neopixel led_strip", {{"color_data", {{0.5, 0.25, 0.75, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 64);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 191);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
        // brightness = max(128, 64, 191, 0) * 100 / 255 = 191 * 100 / 255 = 74
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 74);
    }

    SECTION("RGB only LED (no W channel in data)") {
        // When W is not present in data, it defaults to 0
        json status = {{"neopixel led_strip", {{"color_data", {{0.8, 0.6, 0.4}}}}}};
        state.update_from_status(status);

        // 0.8 * 255 + 0.5 = 204.5 -> 204
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 204);
        // 0.6 * 255 + 0.5 = 153.5 -> 154
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 153);
        // 0.4 * 255 + 0.5 = 102.5 -> 102
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 102);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
    }

    SECTION("white only LED") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 100);
    }
}

// ============================================================================
// Brightness Calculation Tests - Verify derived brightness value
// ============================================================================

TEST_CASE("LED characterization: brightness calculation", "[characterization][led][brightness]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    SECTION("brightness is max of RGBW channels") {
        // R=50, G=100, B=200, W=150 -> max=200 -> brightness = 200*100/255 = 78
        // 50/255 = 0.196, 100/255 = 0.392, 200/255 = 0.784, 150/255 = 0.588
        json status = {{"neopixel led_strip", {{"color_data", {{0.196, 0.392, 0.784, 0.588}}}}}};
        state.update_from_status(status);

        // Values after rounding: R=50, G=100, B=200, W=150
        int max_channel = std::max({lv_subject_get_int(state.get_led_r_subject()),
                                    lv_subject_get_int(state.get_led_g_subject()),
                                    lv_subject_get_int(state.get_led_b_subject()),
                                    lv_subject_get_int(state.get_led_w_subject())});

        int expected_brightness = (max_channel * 100) / 255;
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == expected_brightness);
    }

    SECTION("brightness 0 when all channels are 0") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 0);
    }

    SECTION("brightness 100 when any channel is 255") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 1.0, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 100);
    }
}

// ============================================================================
// LED State (on/off) Tests - Verify derived on/off state
// ============================================================================

TEST_CASE("LED characterization: led_state derivation", "[characterization][led][state]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    SECTION("led_state is 1 when any channel > 0") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.01, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        // 0.01 * 255 + 0.5 = 3.05 -> 3, which is > 0
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 3);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
    }

    SECTION("led_state is 0 when all channels are 0") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }

    SECTION("led_state is 1 when only W channel > 0") {
        json status = {{"neopixel led_strip", {{"color_data", {{0.0, 0.0, 0.0, 0.5}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
    }
}

// ============================================================================
// No Tracked LED Tests - Verify updates are ignored without tracked LED
// ============================================================================

TEST_CASE("LED characterization: updates ignored without tracked LED",
          "[characterization][led][no_tracking]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Explicitly clear any previously set tracked LED
    // (reset_for_testing does NOT clear tracked_led_name_)
    state.set_tracked_led("");

    SECTION("LED update is ignored when no LED is tracked") {
        REQUIRE(state.has_tracked_led() == false);

        json status = {{"neopixel led_strip", {{"color_data", {{1.0, 1.0, 1.0, 1.0}}}}}};
        state.update_from_status(status);

        // Values should remain at initial 0
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 0);
    }

    SECTION("LED update for different LED name is ignored") {
        state.set_tracked_led("neopixel chamber_light");

        // Update for a different LED name
        json status = {{"neopixel led_strip", {{"color_data", {{1.0, 1.0, 1.0, 1.0}}}}}};
        state.update_from_status(status);

        // Values should remain at initial 0 (wrong LED name)
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on LED changes
// ============================================================================

TEST_CASE("LED characterization: observer fires when led_state changes",
          "[characterization][led][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_led_state_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0 (off)

    // Turn LED on
    json status = {{"neopixel led_strip", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2); // At least one more notification
    REQUIRE(user_data[1] == 1); // LED is now on

    lv_observer_remove(observer);
}

TEST_CASE("LED characterization: observer fires when led_r changes",
          "[characterization][led][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_led_r_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0);

    // Update red channel
    json status = {{"neopixel led_strip", {{"color_data", {{0.5, 0.0, 0.0, 0.0}}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 128); // 0.5 * 255 + 0.5 = 128

    lv_observer_remove(observer);
}

TEST_CASE("LED characterization: observer fires when led_brightness changes",
          "[characterization][led][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer =
        lv_subject_add_observer(state.get_led_brightness_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0);

    // Set to full brightness
    json status = {{"neopixel led_strip", {{"color_data", {{1.0, 1.0, 1.0, 1.0}}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 100); // Full brightness

    lv_observer_remove(observer);
}

// ============================================================================
// Partial Update Tests - Verify LED updates don't affect other subjects
// ============================================================================

TEST_CASE("LED characterization: LED update does not affect non-LED subjects",
          "[characterization][led][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    // Set some non-LED values first
    json initial = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
    state.update_from_status(initial);

    REQUIRE(lv_subject_get_int(state.get_position_x_subject()) == 10000); // centimm

    // Now update LED
    json led_update = {{"neopixel led_strip", {{"color_data", {{1.0, 0.5, 0.25, 0.0}}}}}};
    state.update_from_status(led_update);

    // LED values should be updated
    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
    REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 128);
    REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 64);

    // Position should be unchanged (in centimm)
    REQUIRE(lv_subject_get_int(state.get_position_x_subject()) == 10000);
}

TEST_CASE("LED characterization: non-LED update does not affect LED subjects",
          "[characterization][led][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    // Set LED values first
    json led_status = {{"neopixel led_strip", {{"color_data", {{1.0, 0.5, 0.25, 0.0}}}}}};
    state.update_from_status(led_status);

    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
    REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 128);

    // Now update position (non-LED)
    json position_update = {{"toolhead", {{"position", {50.0, 75.0, 10.0}}}}};
    state.update_from_status(position_update);

    // LED values should be unchanged
    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
    REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 128);
    REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 64);
}

// ============================================================================
// Reset Cycle Tests - Verify subjects survive reset_for_testing cycle
// ============================================================================

TEST_CASE("LED characterization: subjects survive reset_for_testing cycle",
          "[characterization][led][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    // Set LED values
    json status = {{"neopixel led_strip", {{"color_data", {{1.0, 0.5, 0.25, 0.75}}}}}};
    state.update_from_status(status);

    // Verify values were set
    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
    REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // After reset, subject values should be back to defaults
    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 0);

    // NOTE: tracked_led_name_ is NOT cleared by reset_for_testing()
    // This is the current behavior - the tracked LED persists across resets
    REQUIRE(state.has_tracked_led() == true);
    REQUIRE(state.get_tracked_led() == "neopixel led_strip");

    // Subjects should still be functional after reset
    json new_status = {{"neopixel led_strip", {{"color_data", {{0.5, 0.5, 0.5, 0.5}}}}}};
    state.update_from_status(new_status);

    REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 128);
}

TEST_CASE("LED characterization: subject pointers remain valid after reset",
          "[characterization][led][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Capture subject pointers
    lv_subject_t* led_state_before = state.get_led_state_subject();
    lv_subject_t* led_r_before = state.get_led_r_subject();
    lv_subject_t* led_brightness_before = state.get_led_brightness_subject();

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Pointers should be the same (singleton subjects are reused)
    lv_subject_t* led_state_after = state.get_led_state_subject();
    lv_subject_t* led_r_after = state.get_led_r_subject();
    lv_subject_t* led_brightness_after = state.get_led_brightness_subject();

    REQUIRE(led_state_before == led_state_after);
    REQUIRE(led_r_before == led_r_after);
    REQUIRE(led_brightness_before == led_brightness_after);
}

// ============================================================================
// Edge Cases - Boundary values and unusual inputs
// ============================================================================

TEST_CASE("LED characterization: edge cases and boundary values", "[characterization][led][edge]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    SECTION("values are clamped to 0-255 range") {
        // Values > 1.0 should clamp to 255
        json status = {{"neopixel led_strip", {{"color_data", {{1.5, 2.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 255);
    }

    SECTION("negative values are clamped to 0") {
        json status = {{"neopixel led_strip", {{"color_data", {{-0.5, -1.0, 0.5, 0.0}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 128);
    }

    SECTION("very small positive values round correctly") {
        // 0.001 * 255 + 0.5 = 0.755 -> rounds to 1
        json status = {{"neopixel led_strip", {{"color_data", {{0.002, 0.0, 0.0, 0.0}}}}}};
        state.update_from_status(status);

        // 0.002 * 255 + 0.5 = 1.01 -> 1
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 1);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
    }

    SECTION("empty color_data array is handled gracefully") {
        json status = {{"neopixel led_strip", {{"color_data", json::array()}}}};
        state.update_from_status(status);

        // Values should remain unchanged (no crash)
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }

    SECTION("missing color_data field is handled gracefully") {
        json status = {{"neopixel led_strip", {{"other_field", 123}}}};
        state.update_from_status(status);

        // Values should remain unchanged (no crash)
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }
}

// ============================================================================
// Output Pin Format Tests - Verify output_pin LED parsing (value field, no color_data)
// ============================================================================

TEST_CASE("LED characterization: output_pin format updates",
          "[characterization][led][output_pin]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // output_pin LEDs use "value" field instead of "color_data"
    state.set_tracked_led("output_pin chamber_light");

    SECTION("output_pin full on (value=1.0)") {
        json status = {{"output_pin chamber_light", {{"value", 1.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 255);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 100);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
    }

    SECTION("output_pin off (value=0.0)") {
        // First turn on
        json on_status = {{"output_pin chamber_light", {{"value", 1.0}}}};
        state.update_from_status(on_status);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);

        // Then turn off
        json off_status = {{"output_pin chamber_light", {{"value", 0.0}}}};
        state.update_from_status(off_status);

        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }

    SECTION("output_pin half brightness (value=0.5)") {
        json status = {{"output_pin chamber_light", {{"value", 0.5}}}};
        state.update_from_status(status);

        // 0.5 * 255 + 0.5 = 128
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == 128);
        REQUIRE(lv_subject_get_int(state.get_led_brightness_subject()) == 50);
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 1);
    }

    SECTION("output_pin sets RGB equally, W stays 0") {
        json status = {{"output_pin chamber_light", {{"value", 0.75}}}};
        state.update_from_status(status);

        // 0.75 * 255 + 0.5 = 191.75 -> 191
        int expected = 191;
        REQUIRE(lv_subject_get_int(state.get_led_r_subject()) == expected);
        REQUIRE(lv_subject_get_int(state.get_led_g_subject()) == expected);
        REQUIRE(lv_subject_get_int(state.get_led_b_subject()) == expected);
        REQUIRE(lv_subject_get_int(state.get_led_w_subject()) == 0);
    }

    SECTION("non-output_pin LED with missing color_data is ignored") {
        // If LED name does NOT start with "output_pin ", missing color_data → no update
        state.set_tracked_led("neopixel led_strip");

        json status = {{"neopixel led_strip", {{"value", 1.0}}}};
        state.update_from_status(status);

        // Should remain at 0 — neopixel without color_data is ignored
        REQUIRE(lv_subject_get_int(state.get_led_state_subject()) == 0);
    }
}

// ============================================================================
// Observer Independence Tests - Verify observer isolation
// ============================================================================

TEST_CASE("LED characterization: observers on different LED subjects are independent",
          "[characterization][led][observer][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    int r_count = 0;
    int state_count = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* r_observer =
        lv_subject_add_observer(state.get_led_r_subject(), observer_cb, &r_count);
    lv_observer_t* state_observer =
        lv_subject_add_observer(state.get_led_state_subject(), observer_cb, &state_count);

    // Both observers fire on initial add
    REQUIRE(r_count == 1);
    REQUIRE(state_count == 1);

    // Update LED
    json status = {{"neopixel led_strip", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}};
    state.update_from_status(status);

    // Both should have received notifications
    REQUIRE(r_count >= 2);
    REQUIRE(state_count >= 2);

    lv_observer_remove(r_observer);
    lv_observer_remove(state_observer);
}

TEST_CASE("LED characterization: multiple observers on same LED subject all fire",
          "[characterization][led][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    state.set_tracked_led("neopixel led_strip");

    int count1 = 0, count2 = 0, count3 = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer1 =
        lv_subject_add_observer(state.get_led_r_subject(), observer_cb, &count1);
    lv_observer_t* observer2 =
        lv_subject_add_observer(state.get_led_r_subject(), observer_cb, &count2);
    lv_observer_t* observer3 =
        lv_subject_add_observer(state.get_led_r_subject(), observer_cb, &count3);

    // All observers fire on initial add
    REQUIRE(count1 == 1);
    REQUIRE(count2 == 1);
    REQUIRE(count3 == 1);

    // Single update should fire all three
    json status = {{"neopixel led_strip", {{"color_data", {{0.5, 0.0, 0.0, 0.0}}}}}};
    state.update_from_status(status);

    REQUIRE(count1 >= 2);
    REQUIRE(count2 >= 2);
    REQUIRE(count3 >= 2);

    lv_observer_remove(observer1);
    lv_observer_remove(observer2);
    lv_observer_remove(observer3);
}
