// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui::temperature;

// ============================================================================
// validate_and_clamp() Tests
// ============================================================================

TEST_CASE("Temperature Utils: validate_and_clamp - valid temperature", "[temp_utils][validate]") {
    int temp = 200;
    bool result = validate_and_clamp(temp, 0, 300, "Test", "current");

    REQUIRE(result == true);
    REQUIRE(temp == 200);
}

TEST_CASE("Temperature Utils: validate_and_clamp - boundary values", "[temp_utils][validate]") {
    SECTION("At minimum boundary") {
        int temp = 0;
        bool result = validate_and_clamp(temp, 0, 300, "Test", "current");
        REQUIRE(result == true);
        REQUIRE(temp == 0);
    }

    SECTION("At maximum boundary") {
        int temp = 300;
        bool result = validate_and_clamp(temp, 0, 300, "Test", "current");
        REQUIRE(result == true);
        REQUIRE(temp == 300);
    }
}

TEST_CASE("Temperature Utils: validate_and_clamp - below minimum",
          "[temp_utils][validate][clamp]") {
    int temp = -10;
    bool result = validate_and_clamp(temp, 0, 300, "Test", "current");

    REQUIRE(result == false);
    REQUIRE(temp == 0); // Clamped to minimum
}

TEST_CASE("Temperature Utils: validate_and_clamp - above maximum",
          "[temp_utils][validate][clamp]") {
    int temp = 350;
    bool result = validate_and_clamp(temp, 0, 300, "Test", "current");

    REQUIRE(result == false);
    REQUIRE(temp == 300); // Clamped to maximum
}

TEST_CASE("Temperature Utils: validate_and_clamp - extreme values",
          "[temp_utils][validate][edge]") {
    SECTION("Very negative") {
        int temp = -1000;
        validate_and_clamp(temp, 0, 300, "Test", "current");
        REQUIRE(temp == 0);
    }

    SECTION("Very high") {
        int temp = 10000;
        validate_and_clamp(temp, 0, 300, "Test", "current");
        REQUIRE(temp == 300);
    }
}

TEST_CASE("Temperature Utils: validate_and_clamp - typical ranges", "[temp_utils][validate]") {
    SECTION("Bed temperature range (0-120°C)") {
        int temp = 60;
        bool result = validate_and_clamp(temp, 0, 120, "Bed", "target");
        REQUIRE(result == true);
        REQUIRE(temp == 60);

        temp = 130;
        result = validate_and_clamp(temp, 0, 120, "Bed", "target");
        REQUIRE(result == false);
        REQUIRE(temp == 120);
    }

    SECTION("Nozzle temperature range (0-300°C)") {
        int temp = 210;
        bool result = validate_and_clamp(temp, 0, 300, "Nozzle", "target");
        REQUIRE(result == true);
        REQUIRE(temp == 210);

        temp = 350;
        result = validate_and_clamp(temp, 0, 300, "Nozzle", "target");
        REQUIRE(result == false);
        REQUIRE(temp == 300);
    }
}

// ============================================================================
// validate_and_clamp_pair() Tests
// ============================================================================

TEST_CASE("Temperature Utils: validate_and_clamp_pair - both valid", "[temp_utils][validate]") {
    int current = 200;
    int target = 210;

    bool result = validate_and_clamp_pair(current, target, 0, 300, "Test");

    REQUIRE(result == true);
    REQUIRE(current == 200);
    REQUIRE(target == 210);
}

TEST_CASE("Temperature Utils: validate_and_clamp_pair - current invalid",
          "[temp_utils][validate][clamp]") {
    int current = -10;
    int target = 210;

    bool result = validate_and_clamp_pair(current, target, 0, 300, "Test");

    REQUIRE(result == false);
    REQUIRE(current == 0);  // Clamped
    REQUIRE(target == 210); // Unchanged
}

TEST_CASE("Temperature Utils: validate_and_clamp_pair - target invalid",
          "[temp_utils][validate][clamp]") {
    int current = 200;
    int target = 350;

    bool result = validate_and_clamp_pair(current, target, 0, 300, "Test");

    REQUIRE(result == false);
    REQUIRE(current == 200); // Unchanged
    REQUIRE(target == 300);  // Clamped
}

TEST_CASE("Temperature Utils: validate_and_clamp_pair - both invalid",
          "[temp_utils][validate][clamp]") {
    int current = -50;
    int target = 400;

    bool result = validate_and_clamp_pair(current, target, 0, 300, "Test");

    REQUIRE(result == false);
    REQUIRE(current == 0);  // Clamped to min
    REQUIRE(target == 300); // Clamped to max
}

TEST_CASE("Temperature Utils: validate_and_clamp_pair - realistic scenarios",
          "[temp_utils][validate]") {
    SECTION("Heating up bed") {
        int current = 25; // Room temp
        int target = 60;

        bool result = validate_and_clamp_pair(current, target, 0, 120, "Bed");

        REQUIRE(result == true);
        REQUIRE(current == 25);
        REQUIRE(target == 60);
    }

    SECTION("Cooling down nozzle") {
        int current = 180;
        int target = 0;

        bool result = validate_and_clamp_pair(current, target, 0, 300, "Nozzle");

        REQUIRE(result == true);
        REQUIRE(current == 180);
        REQUIRE(target == 0);
    }

    SECTION("At target temperature") {
        int current = 210;
        int target = 210;

        bool result = validate_and_clamp_pair(current, target, 0, 300, "Nozzle");

        REQUIRE(result == true);
        REQUIRE(current == 210);
        REQUIRE(target == 210);
    }
}

// ============================================================================
// is_extrusion_safe() Tests
// ============================================================================

TEST_CASE("Temperature Utils: is_extrusion_safe - above minimum", "[temp_utils][safety]") {
    REQUIRE(is_extrusion_safe(200, 170) == true);
    REQUIRE(is_extrusion_safe(250, 170) == true);
    REQUIRE(is_extrusion_safe(300, 170) == true);
}

TEST_CASE("Temperature Utils: is_extrusion_safe - at minimum", "[temp_utils][safety]") {
    REQUIRE(is_extrusion_safe(170, 170) == true);
}

TEST_CASE("Temperature Utils: is_extrusion_safe - below minimum", "[temp_utils][safety]") {
    REQUIRE(is_extrusion_safe(169, 170) == false);
    REQUIRE(is_extrusion_safe(100, 170) == false);
    REQUIRE(is_extrusion_safe(25, 170) == false);
    REQUIRE(is_extrusion_safe(0, 170) == false);
}

TEST_CASE("Temperature Utils: is_extrusion_safe - edge cases", "[temp_utils][safety][edge]") {
    SECTION("Exactly at boundary") {
        REQUIRE(is_extrusion_safe(170, 170) == true);
    }

    SECTION("One degree below") {
        REQUIRE(is_extrusion_safe(169, 170) == false);
    }

    SECTION("One degree above") {
        REQUIRE(is_extrusion_safe(171, 170) == true);
    }
}

TEST_CASE("Temperature Utils: is_extrusion_safe - different minimums", "[temp_utils][safety]") {
    SECTION("Low minimum (150°C)") {
        REQUIRE(is_extrusion_safe(160, 150) == true);
        REQUIRE(is_extrusion_safe(140, 150) == false);
    }

    SECTION("High minimum (200°C)") {
        REQUIRE(is_extrusion_safe(210, 200) == true);
        REQUIRE(is_extrusion_safe(190, 200) == false);
    }

    SECTION("Zero minimum (testing only)") {
        REQUIRE(is_extrusion_safe(0, 0) == true);
        REQUIRE(is_extrusion_safe(100, 0) == true);
    }
}

// ============================================================================
// get_extrusion_safety_status() Tests
// ============================================================================

TEST_CASE("Temperature Utils: get_extrusion_safety_status - safe", "[temp_utils][safety]") {
    const char* status = get_extrusion_safety_status(200, 170);
    REQUIRE(std::string(status) == "Ready");
}

TEST_CASE("Temperature Utils: get_extrusion_safety_status - at minimum", "[temp_utils][safety]") {
    const char* status = get_extrusion_safety_status(170, 170);
    REQUIRE(std::string(status) == "Ready");
}

TEST_CASE("Temperature Utils: get_extrusion_safety_status - heating", "[temp_utils][safety]") {
    SECTION("10°C below minimum") {
        const char* status = get_extrusion_safety_status(160, 170);
        std::string status_str(status);
        REQUIRE(status_str.find("Heating") != std::string::npos);
        REQUIRE(status_str.find("10") != std::string::npos);
    }

    SECTION("50°C below minimum") {
        const char* status = get_extrusion_safety_status(120, 170);
        std::string status_str(status);
        REQUIRE(status_str.find("Heating") != std::string::npos);
        REQUIRE(status_str.find("50") != std::string::npos);
    }

    SECTION("1°C below minimum") {
        const char* status = get_extrusion_safety_status(169, 170);
        std::string status_str(status);
        REQUIRE(status_str.find("Heating") != std::string::npos);
        REQUIRE(status_str.find("1") != std::string::npos);
    }
}

TEST_CASE("Temperature Utils: get_extrusion_safety_status - cold start", "[temp_utils][safety]") {
    const char* status = get_extrusion_safety_status(25, 170);
    std::string status_str(status);

    REQUIRE(status_str.find("Heating") != std::string::npos);
    REQUIRE(status_str.find("145") != std::string::npos); // 170 - 25 = 145
}

TEST_CASE("Temperature Utils: get_extrusion_safety_status - edge cases",
          "[temp_utils][safety][edge]") {
    SECTION("One degree below") {
        const char* status = get_extrusion_safety_status(169, 170);
        std::string status_str(status);
        REQUIRE(status_str.find("1") != std::string::npos);
        REQUIRE(status_str.find("below minimum") != std::string::npos);
    }

    SECTION("Exactly at minimum") {
        const char* status = get_extrusion_safety_status(170, 170);
        REQUIRE(std::string(status) == "Ready");
    }

    SECTION("Well above minimum") {
        const char* status = get_extrusion_safety_status(250, 170);
        REQUIRE(std::string(status) == "Ready");
    }
}

// ============================================================================
// Integration Scenarios
// ============================================================================

TEST_CASE("Temperature Utils: Integration - PLA printing scenario", "[temp_utils][integration]") {
    int nozzle_current = 205;
    int nozzle_target = 210;
    int bed_current = 60;
    int bed_target = 60;

    // Validate nozzle temps
    bool nozzle_valid = validate_and_clamp_pair(nozzle_current, nozzle_target, 0, 300, "Nozzle");
    REQUIRE(nozzle_valid == true);

    // Validate bed temps
    bool bed_valid = validate_and_clamp_pair(bed_current, bed_target, 0, 120, "Bed");
    REQUIRE(bed_valid == true);

    // Check extrusion safety
    REQUIRE(is_extrusion_safe(nozzle_current, 170) == true);

    const char* status = get_extrusion_safety_status(nozzle_current, 170);
    REQUIRE(std::string(status) == "Ready");
}

TEST_CASE("Temperature Utils: Integration - Cold start scenario", "[temp_utils][integration]") {
    int nozzle_current = 22; // Room temperature
    int nozzle_target = 210;

    // Validate temps
    bool valid = validate_and_clamp_pair(nozzle_current, nozzle_target, 0, 300, "Nozzle");
    REQUIRE(valid == true);

    // Not safe for extrusion yet
    REQUIRE(is_extrusion_safe(nozzle_current, 170) == false);

    const char* status = get_extrusion_safety_status(nozzle_current, 170);
    std::string status_str(status);
    REQUIRE(status_str.find("Heating") != std::string::npos);
    REQUIRE(status_str.find("148") != std::string::npos); // 170 - 22 = 148
}

TEST_CASE("Temperature Utils: Integration - Invalid input handling",
          "[temp_utils][integration][error]") {
    int nozzle_current = 500; // Way too high
    int nozzle_target = -50;  // Invalid negative

    // Should clamp both
    bool valid = validate_and_clamp_pair(nozzle_current, nozzle_target, 0, 300, "Nozzle");
    REQUIRE(valid == false);
    REQUIRE(nozzle_current == 300); // Clamped to max
    REQUIRE(nozzle_target == 0);    // Clamped to min

    // After clamping, nozzle is safe for extrusion
    REQUIRE(is_extrusion_safe(nozzle_current, 170) == true);
}

TEST_CASE("Temperature Utils: Integration - ABS printing scenario", "[temp_utils][integration]") {
    int nozzle_current = 245;
    int nozzle_target = 250;
    int bed_current = 100;
    int bed_target = 100;

    // Validate nozzle temps (higher for ABS)
    bool nozzle_valid = validate_and_clamp_pair(nozzle_current, nozzle_target, 0, 300, "Nozzle");
    REQUIRE(nozzle_valid == true);

    // Validate bed temps (higher for ABS)
    bool bed_valid = validate_and_clamp_pair(bed_current, bed_target, 0, 120, "Bed");
    REQUIRE(bed_valid == true);

    // Check extrusion safety (higher minimum for ABS)
    REQUIRE(is_extrusion_safe(nozzle_current, 220) == true);

    const char* status = get_extrusion_safety_status(nozzle_current, 220);
    REQUIRE(std::string(status) == "Ready");
}

// ============================================================================
// format_temperature() Tests
// ============================================================================

TEST_CASE("Temperature Utils: format_temperature - basic formatting", "[temp_utils][format]") {
    char buf[16];

    SECTION("Typical nozzle temp") {
        format_temperature(210, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "210°C");
    }

    SECTION("Typical bed temp") {
        format_temperature(60, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60°C");
    }

    SECTION("Zero temperature") {
        format_temperature(0, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "0°C");
    }

    SECTION("High temperature") {
        format_temperature(300, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "300°C");
    }
}

TEST_CASE("Temperature Utils: format_temperature - returns buffer pointer",
          "[temp_utils][format]") {
    char buf[16];
    char* result = format_temperature(210, buf, sizeof(buf));
    REQUIRE(result == buf);
}

// ============================================================================
// format_temperature_pair() Tests
// ============================================================================

TEST_CASE("Temperature Utils: format_temperature_pair - basic formatting", "[temp_utils][format]") {
    char buf[24];

    SECTION("Heating up") {
        format_temperature_pair(180, 210, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "180 / 210°C");
    }

    SECTION("At target") {
        format_temperature_pair(210, 210, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "210 / 210°C");
    }

    SECTION("Heater off (target = 0)") {
        format_temperature_pair(180, 0, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "180 / —°C"); // em-dash for unknown
    }

    SECTION("Cold with target") {
        format_temperature_pair(25, 60, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "25 / 60°C");
    }
}

// ============================================================================
// format_temperature_f() Tests (NEW - float formatting)
// ============================================================================

TEST_CASE("Temperature Utils: format_temp_number - compact-wide rule", "[temp_utils][format]") {
    char buf[16];

    SECTION("Under 100 keeps one decimal") {
        format_temp_number(60.5f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60.5");
    }

    SECTION("100 and above drops the decimal") {
        format_temp_number(210.4f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "210");
    }

    SECTION("Boundary: 99.9 keeps decimal") {
        format_temp_number(99.9f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "99.9");
    }

    SECTION("Boundary: 100 drops decimal") {
        format_temp_number(100.4f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "100");
    }

    // Pins the rounding RULE: printf %.0f rounds half-to-even (banker's),
    // not half-up. 210.5 -> "210" (even), 211.5 -> "212" (even). If this is
    // ever switched to round-half-up these assertions go red.
    SECTION("Rounding is half-to-even at the .5 boundary") {
        format_temp_number(210.5f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "210");
        format_temp_number(211.5f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "212");
    }

    SECTION("Negative (sub-100) keeps decimal") {
        format_temp_number(-10.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "-10.0");
    }
}

// ============================================================================
// displayed_deci() — the reading as it actually renders
//
// format_temp_number() drops the decimal at and above 100C, so the number on a
// card is a ROUNDED value while the heating color and the status word beside it
// used to be derived from a TRUNCATED one. displayed_deci() is the single place
// that snaps a reading to the precision the UI shows, so all three agree.
// ============================================================================

TEST_CASE("Temperature Utils: displayed_deci snaps to the rendered precision",
          "[temp_utils][format]") {
    SECTION("At or above 100C the decimal is gone, so the value rounds") {
        REQUIRE(displayed_deci(2196) == 2200); // 219.6 renders "220"
        REQUIRE(displayed_deci(2229) == 2230); // 222.9 renders "223"
        REQUIRE(displayed_deci(2204) == 2200); // 220.4 renders "220"
    }

    SECTION("Rounding matches printf %.0f: half to even") {
        REQUIRE(displayed_deci(2105) == 2100); // 210.5 -> "210"
        REQUIRE(displayed_deci(2115) == 2120); // 211.5 -> "212"
    }

    SECTION("Below 100C the decimal is still shown, so the reading is untouched") {
        REQUIRE(displayed_deci(596) == 596); // 59.6 renders "59.6"
        REQUIRE(displayed_deci(999) == 999); // 99.9 renders "99.9"
        REQUIRE(displayed_deci(-105) == -105);
    }

    SECTION("Boundary: exactly 100C") {
        REQUIRE(displayed_deci(1000) == 1000);
        REQUIRE(displayed_deci(1004) == 1000); // 100.4 renders "100"
    }
}

TEST_CASE("Temperature Utils: the status word never contradicts the number shown",
          "[temp_utils][heater_display]") {
    // 222.9 with a 220 target renders as "223". Classifying the truncated 222
    // put a green "Ready" next to a card reading 223 / 220.
    SECTION("Above the band: the rounded reading is what gets classified") {
        auto result = heater_display(2229, 2200);
        REQUIRE(result.temp == "223 / 220°C");
        REQUIRE(result.status == "Cooling");
    }

    SECTION("Just under target: the number reads as the target and so does the state") {
        auto result = heater_display(2196, 2200);
        REQUIRE(result.temp == "220 / 220°C");
        REQUIRE(result.status == "Ready");
    }

    // The invariant behind both cases: whenever the current and target render as
    // the same number, the heater cannot claim to be moving toward it.
    SECTION("A reading that displays as the target is never Heating or Cooling") {
        constexpr int target_deci = 2200;
        char cur_buf[16];
        char tgt_buf[16];
        format_temp_number(deci_to_degrees_f(target_deci), tgt_buf, sizeof(tgt_buf));
        for (int deci = target_deci - 60; deci <= target_deci + 60; ++deci) {
            format_temp_number(deci_to_degrees_f(deci), cur_buf, sizeof(cur_buf));
            if (std::string(cur_buf) != std::string(tgt_buf)) {
                continue;
            }
            INFO("current decidegrees " << deci << " renders as " << cur_buf);
            auto result = heater_display(deci, target_deci);
            REQUIRE(result.status == "Ready");
        }
    }
}

TEST_CASE("Temperature Utils: format_temperature_f - float formatting", "[temp_utils][format]") {
    char buf[16];

    SECTION("Whole number") {
        format_temperature_f(210.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "210°C");
    }

    SECTION("One decimal place") {
        format_temperature_f(60.5f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60.5°C");
    }

    SECTION("Rounding under 100 keeps decimal") {
        format_temperature_f(60.99f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "61.0°C");
    }

    SECTION("Three-digit drops decimal") {
        format_temperature_f(254.7f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "255°C");
    }

    SECTION("Boundary: 99.9 keeps decimal") {
        format_temperature_f(99.9f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "99.9°C");
    }

    SECTION("Boundary: 100 drops decimal") {
        format_temperature_f(100.4f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "100°C");
    }

    SECTION("Zero") {
        format_temperature_f(0.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "0.0°C");
    }
}

// ============================================================================
// format_temperature_pair_f() Tests (NEW - float pair formatting)
// ============================================================================

TEST_CASE("Temperature Utils: format_temperature_pair_f - float pair formatting",
          "[temp_utils][format]") {
    char buf[32];

    SECTION("Both floats under 100 keep decimals") {
        format_temperature_pair_f(60.5f, 65.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60.5 / 65.0°C");
    }

    SECTION("Three-digit values drop decimals") {
        format_temperature_pair_f(254.7f, 215.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "255 / 215°C");
    }

    SECTION("Mixed: current >= 100, target under 100") {
        format_temperature_pair_f(254.7f, 60.5f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "255 / 60.5°C");
    }

    SECTION("Target zero (heater off)") {
        format_temperature_pair_f(180.6f, 0.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "181 / —°C"); // em-dash for unknown
    }

    SECTION("At target, under 100") {
        format_temperature_pair_f(60.0f, 60.0f, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60.0 / 60.0°C");
    }
}

// ============================================================================
// format_temperature_range() Tests (NEW - range formatting for AMS)
// ============================================================================

TEST_CASE("Temperature Utils: format_temperature_range - AMS material temps",
          "[temp_utils][format]") {
    char buf[16];

    SECTION("PLA nozzle range") {
        format_temperature_range(200, 230, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "200-230°C");
    }

    SECTION("ABS nozzle range") {
        format_temperature_range(240, 260, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "240-260°C");
    }

    SECTION("Bed range") {
        format_temperature_range(55, 65, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "55-65°C");
    }

    SECTION("Same min and max") {
        format_temperature_range(60, 60, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "60-60°C");
    }

    SECTION("Zero range") {
        format_temperature_range(0, 0, buf, sizeof(buf));
        REQUIRE(std::string(buf) == "0-0°C");
    }
}
