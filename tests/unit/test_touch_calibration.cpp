// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "lvgl_test_fixture.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_wrapper.h"

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix;

// ============================================================================
// Coefficient Computation Tests
// ============================================================================

TEST_CASE("TouchCalibration: identity transformation", "[touch-calibration][compute]") {
    // When screen points equal touch points, coefficients should give identity
    // identity: a=1, b=0, c=0, d=0, e=1, f=0
    Point screen_points[3] = {{0, 0}, {100, 0}, {0, 100}};
    Point touch_points[3] = {{0, 0}, {100, 0}, {0, 100}};

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);
    REQUIRE(cal.a == Approx(1.0f));
    REQUIRE(cal.b == Approx(0.0f));
    REQUIRE(cal.c == Approx(0.0f));
    REQUIRE(cal.d == Approx(0.0f));
    REQUIRE(cal.e == Approx(1.0f));
    REQUIRE(cal.f == Approx(0.0f));
}

TEST_CASE("TouchCalibration: simple scaling", "[touch-calibration][compute]") {
    // Touch range 0-1000 maps to screen 0-800 x 0-480
    Point screen_points[3] = {{0, 0}, {800, 0}, {0, 480}};
    Point touch_points[3] = {{0, 0}, {1000, 0}, {0, 1000}};

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);

    // Verify transformation produces correct screen coordinates
    Point p1 = transform_point(cal, {1000, 0});
    REQUIRE(p1.x == Approx(800).margin(1));
    REQUIRE(p1.y == Approx(0).margin(1));

    Point p2 = transform_point(cal, {0, 1000});
    REQUIRE(p2.x == Approx(0).margin(1));
    REQUIRE(p2.y == Approx(480).margin(1));

    Point p3 = transform_point(cal, {500, 500});
    REQUIRE(p3.x == Approx(400).margin(1));
    REQUIRE(p3.y == Approx(240).margin(1));
}

TEST_CASE("TouchCalibration: translation offset", "[touch-calibration][compute]") {
    // Touch 0,0 maps to screen 100,100 (offset mapping)
    Point screen_points[3] = {{100, 100}, {700, 100}, {100, 380}};
    Point touch_points[3] = {{0, 0}, {600, 0}, {0, 280}};

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);

    // transform(0, 0) should give (100, 100)
    Point p = transform_point(cal, {0, 0});
    REQUIRE(p.x == Approx(100).margin(1));
    REQUIRE(p.y == Approx(100).margin(1));

    // transform(600, 0) should give (700, 100)
    Point p2 = transform_point(cal, {600, 0});
    REQUIRE(p2.x == Approx(700).margin(1));
    REQUIRE(p2.y == Approx(100).margin(1));

    // transform(0, 280) should give (100, 380)
    Point p3 = transform_point(cal, {0, 280});
    REQUIRE(p3.x == Approx(100).margin(1));
    REQUIRE(p3.y == Approx(380).margin(1));
}

TEST_CASE("TouchCalibration: AD5M-like calibration", "[touch-calibration][compute][ad5m]") {
    // Real-world scenario: 800x480 screen with 15% inset calibration points
    // Screen points at 15% inset from edges
    Point screen_points[3] = {
        {120, 144}, // 15% from left, 30% from top
        {400, 408}, // center-ish X, 85% from top
        {680, 72}   // 85% from left, 15% from top
    };

    // Simulated raw touch values from resistive touchscreen
    Point touch_points[3] = {
        {500, 3200}, // Top-left region
        {2040, 900}, // Bottom-center region
        {3580, 3500} // Top-right region
    };

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);

    // Verify the calibration points transform correctly
    Point p1 = transform_point(cal, {500, 3200});
    REQUIRE(p1.x == Approx(120).margin(2));
    REQUIRE(p1.y == Approx(144).margin(2));

    Point p2 = transform_point(cal, {2040, 900});
    REQUIRE(p2.x == Approx(400).margin(2));
    REQUIRE(p2.y == Approx(408).margin(2));

    Point p3 = transform_point(cal, {3580, 3500});
    REQUIRE(p3.x == Approx(680).margin(2));
    REQUIRE(p3.y == Approx(72).margin(2));
}

TEST_CASE("TouchCalibration: Y-axis inversion", "[touch-calibration][compute]") {
    // Common on resistive touchscreens: raw Y increases but screen Y decreases
    // Screen: origin at top-left, Y increases downward
    // Touch: origin at bottom-left, Y increases upward
    Point screen_points[3] = {{0, 0}, {800, 0}, {0, 480}};
    Point touch_points[3] = {{0, 480}, {800, 480}, {0, 0}}; // Y inverted

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);

    // Touch (0, 480) -> Screen (0, 0)
    Point p1 = transform_point(cal, {0, 480});
    REQUIRE(p1.x == Approx(0).margin(1));
    REQUIRE(p1.y == Approx(0).margin(1));

    // Touch (0, 0) -> Screen (0, 480)
    Point p2 = transform_point(cal, {0, 0});
    REQUIRE(p2.x == Approx(0).margin(1));
    REQUIRE(p2.y == Approx(480).margin(1));

    // Touch (400, 240) -> Screen (400, 240) - center stays center
    Point p3 = transform_point(cal, {400, 240});
    REQUIRE(p3.x == Approx(400).margin(1));
    REQUIRE(p3.y == Approx(240).margin(1));
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE("TouchCalibration: degenerate points - collinear", "[touch-calibration][edge]") {
    // All three touch points on a line - cannot compute unique transform
    Point screen_points[3] = {{0, 0}, {100, 100}, {200, 200}};
    Point touch_points[3] = {{0, 0}, {100, 100}, {200, 200}}; // All on diagonal

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == false);
    REQUIRE(cal.valid == false);
}

TEST_CASE("TouchCalibration: degenerate points - duplicates", "[touch-calibration][edge]") {
    // Two identical touch points
    Point screen_points[3] = {{0, 0}, {100, 0}, {0, 100}};
    Point touch_points[3] = {{50, 50}, {50, 50}, {100, 100}}; // First two identical

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == false);
    REQUIRE(cal.valid == false);
}

TEST_CASE("TouchCalibration: degenerate points - nearly collinear", "[touch-calibration][edge]") {
    // Points almost on a line - should detect and fail
    Point screen_points[3] = {{0, 0}, {100, 100}, {200, 201}}; // Third point barely off line
    Point touch_points[3] = {{0, 0}, {100, 100}, {200, 200}};  // Collinear

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == false);
    REQUIRE(cal.valid == false);
}

// ============================================================================
// Point Transformation Tests
// ============================================================================

TEST_CASE("TouchCalibration: transform maintains precision", "[touch-calibration][transform]") {
    // Set up a known scaling transformation
    Point screen_points[3] = {{0, 0}, {100, 0}, {0, 100}};
    Point touch_points[3] = {{0, 0}, {200, 0}, {0, 200}}; // 2x touch range

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal) == true);

    // Test various points
    SECTION("origin") {
        Point p = transform_point(cal, {0, 0});
        REQUIRE(p.x == Approx(0).margin(1));
        REQUIRE(p.y == Approx(0).margin(1));
    }

    SECTION("max x") {
        Point p = transform_point(cal, {200, 0});
        REQUIRE(p.x == Approx(100).margin(1));
        REQUIRE(p.y == Approx(0).margin(1));
    }

    SECTION("max y") {
        Point p = transform_point(cal, {0, 200});
        REQUIRE(p.x == Approx(0).margin(1));
        REQUIRE(p.y == Approx(100).margin(1));
    }

    SECTION("center") {
        Point p = transform_point(cal, {100, 100});
        REQUIRE(p.x == Approx(50).margin(1));
        REQUIRE(p.y == Approx(50).margin(1));
    }
}

TEST_CASE("TouchCalibration: transform with rotation", "[touch-calibration][transform]") {
    // 90-degree rotation: touch X becomes screen Y, touch Y becomes -screen X
    Point screen_points[3] = {{0, 0}, {0, 100}, {100, 0}}; // Rotated
    Point touch_points[3] = {{0, 0}, {100, 0}, {0, 100}};  // Normal

    TouchCalibration cal;
    bool result = compute_calibration(screen_points, touch_points, cal);

    REQUIRE(result == true);
    REQUIRE(cal.valid == true);

    // Touch (100, 0) -> Screen (0, 100)
    Point p1 = transform_point(cal, {100, 0});
    REQUIRE(p1.x == Approx(0).margin(1));
    REQUIRE(p1.y == Approx(100).margin(1));

    // Touch (0, 100) -> Screen (100, 0)
    Point p2 = transform_point(cal, {0, 100});
    REQUIRE(p2.x == Approx(100).margin(1));
    REQUIRE(p2.y == Approx(0).margin(1));
}

TEST_CASE("TouchCalibration: transform extrapolation beyond calibration points",
          "[touch-calibration][transform]") {
    // Verify transform works for points outside the calibration triangle
    Point screen_points[3] = {{100, 100}, {200, 100}, {100, 200}};
    Point touch_points[3] = {{100, 100}, {200, 100}, {100, 200}}; // Identity at offset

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal) == true);

    // Point outside calibration triangle
    Point p = transform_point(cal, {300, 300});
    REQUIRE(p.x == Approx(300).margin(1));
    REQUIRE(p.y == Approx(300).margin(1));

    // Point at origin (outside triangle)
    Point p2 = transform_point(cal, {0, 0});
    REQUIRE(p2.x == Approx(0).margin(1));
    REQUIRE(p2.y == Approx(0).margin(1));
}

// ============================================================================
// Coefficient Validation Tests
// ============================================================================

TEST_CASE("TouchCalibration: coefficient values for known transforms",
          "[touch-calibration][coefficients]") {
    SECTION("pure X scaling by 0.8") {
        // screen_x = 0.8 * touch_x + 0 * touch_y + 0
        // screen_y = 0 * touch_x + 1 * touch_y + 0
        Point screen_points[3] = {{0, 0}, {80, 0}, {0, 100}};
        Point touch_points[3] = {{0, 0}, {100, 0}, {0, 100}};

        TouchCalibration cal;
        REQUIRE(compute_calibration(screen_points, touch_points, cal) == true);

        REQUIRE(cal.a == Approx(0.8f).margin(0.001f));
        REQUIRE(cal.b == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.c == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.d == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.e == Approx(1.0f).margin(0.001f));
        REQUIRE(cal.f == Approx(0.0f).margin(0.001f));
    }

    SECTION("pure Y scaling by 0.48") {
        // screen_x = 1 * touch_x + 0 * touch_y + 0
        // screen_y = 0 * touch_x + 0.48 * touch_y + 0
        Point screen_points[3] = {{0, 0}, {100, 0}, {0, 48}};
        Point touch_points[3] = {{0, 0}, {100, 0}, {0, 100}};

        TouchCalibration cal;
        REQUIRE(compute_calibration(screen_points, touch_points, cal) == true);

        REQUIRE(cal.a == Approx(1.0f).margin(0.001f));
        REQUIRE(cal.b == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.c == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.d == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.e == Approx(0.48f).margin(0.001f));
        REQUIRE(cal.f == Approx(0.0f).margin(0.001f));
    }

    SECTION("pure translation") {
        // screen_x = 1 * touch_x + 0 * touch_y + 50
        // screen_y = 0 * touch_x + 1 * touch_y + 30
        Point screen_points[3] = {{50, 30}, {150, 30}, {50, 130}};
        Point touch_points[3] = {{0, 0}, {100, 0}, {0, 100}};

        TouchCalibration cal;
        REQUIRE(compute_calibration(screen_points, touch_points, cal) == true);

        REQUIRE(cal.a == Approx(1.0f).margin(0.001f));
        REQUIRE(cal.b == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.c == Approx(50.0f).margin(0.001f));
        REQUIRE(cal.d == Approx(0.0f).margin(0.001f));
        REQUIRE(cal.e == Approx(1.0f).margin(0.001f));
        REQUIRE(cal.f == Approx(30.0f).margin(0.001f));
    }
}

// ============================================================================
// Invalid Calibration State Tests
// ============================================================================

TEST_CASE("TouchCalibration: default state is invalid", "[touch-calibration][state]") {
    TouchCalibration cal;
    REQUIRE(cal.valid == false);
}

TEST_CASE("TouchCalibration: transform with invalid calibration", "[touch-calibration][state]") {
    TouchCalibration cal;
    cal.valid = false;

    // Transformation with invalid calibration should return input unchanged
    // (or some sensible default behavior)
    Point raw = {500, 300};
    Point result = transform_point(cal, raw);

    // Expected behavior: return input unchanged when calibration is invalid
    REQUIRE(result.x == raw.x);
    REQUIRE(result.y == raw.y);
}

// ============================================================================
// USB Input Device Detection Tests
// ============================================================================

TEST_CASE("TouchCalibration: USB input phys detection", "[touch-calibration][usb-detect]") {
    SECTION("typical USB HID touchscreen") {
        // BTT HDMI touchscreens, Waveshare, etc.
        REQUIRE(is_usb_input_phys("usb-0000:01:00.0-1.3/input0") == true);
    }

    SECTION("USB with different bus format") {
        REQUIRE(is_usb_input_phys("usb-3f980000.usb-1.2/input0") == true);
    }

    SECTION("platform resistive touchscreen (empty phys)") {
        // AD5M sun4i_ts has empty phys
        REQUIRE(is_usb_input_phys("") == false);
    }

    SECTION("platform resistive touchscreen (named phys)") {
        REQUIRE(is_usb_input_phys("sun4i_ts") == false);
    }

    SECTION("I2C capacitive touchscreen") {
        // Goodix/FocalTech over I2C
        REQUIRE(is_usb_input_phys("i2c-1/1-005d") == false);
    }

    SECTION("SPI touchscreen") {
        REQUIRE(is_usb_input_phys("spi0.0/input0") == false);
    }

    SECTION("USB composite device with touch") {
        REQUIRE(is_usb_input_phys("usb-xhci-hcd.0-1/input1") == true);
    }
}

// ============================================================================
// Known Touchscreen Name Detection Tests
// ============================================================================

TEST_CASE("TouchCalibration: known touchscreen name detection",
          "[touch-calibration][touchscreen-detect]") {
    // --- Real touchscreen controllers should match ---

    SECTION("AD5M sun4i resistive touchscreen") {
        REQUIRE(is_known_touchscreen_name("sun4i-ts") == true);
    }

    SECTION("Goodix capacitive touchscreen") {
        REQUIRE(is_known_touchscreen_name("Goodix Capacitive TouchScreen") == true);
    }

    SECTION("FocalTech FT5x touchscreen") {
        REQUIRE(is_known_touchscreen_name("ft5x06_ts") == true);
    }

    SECTION("Goodix GT911 touchscreen") {
        REQUIRE(is_known_touchscreen_name("gt911") == true);
    }

    SECTION("ILI2130 touchscreen") {
        REQUIRE(is_known_touchscreen_name("ili2130_ts") == true);
    }

    SECTION("generic touch device") {
        REQUIRE(is_known_touchscreen_name("Generic Touchscreen") == true);
    }

    SECTION("EDT FocalTech display") {
        REQUIRE(is_known_touchscreen_name("edt-ft5x06") == true);
    }

    SECTION("case insensitive matching") {
        REQUIRE(is_known_touchscreen_name("GOODIX Touch") == true);
        REQUIRE(is_known_touchscreen_name("SUN4I-TS") == true);
    }

    // --- Non-touch devices must NOT match ---

    SECTION("HDMI CEC remote control") {
        REQUIRE(is_known_touchscreen_name("vc4-hdmi") == false);
    }

    SECTION("HDMI CEC variant") {
        REQUIRE(is_known_touchscreen_name("vc4-hdmi HDMI Jack") == false);
    }

    SECTION("generic keyboard") {
        REQUIRE(is_known_touchscreen_name("AT Translated Set 2 keyboard") == false);
    }

    SECTION("USB mouse") {
        REQUIRE(is_known_touchscreen_name("Logitech USB Mouse") == false);
    }

    SECTION("power button") {
        REQUIRE(is_known_touchscreen_name("Power Button") == false);
    }

    SECTION("GPIO keys") {
        REQUIRE(is_known_touchscreen_name("gpio-keys") == false);
    }

    SECTION("empty name") {
        REQUIRE(is_known_touchscreen_name("") == false);
    }

    SECTION("IR remote") {
        REQUIRE(is_known_touchscreen_name("rc-cec") == false);
    }
}

// ============================================================================
// Unified Calibration Decision Tests (device_needs_calibration)
// ============================================================================

// ============================================================================
// Resistive Touchscreen Detection Tests (is_resistive_touchscreen_name)
// ============================================================================

TEST_CASE("TouchCalibration: is_resistive_touchscreen_name",
          "[touch-calibration][resistive-detection]") {
    // --- Resistive controllers that NEED calibration ---

    SECTION("sun4i resistive (AD5M)") {
        REQUIRE(is_resistive_touchscreen_name("sun4i-ts") == true);
    }

    SECTION("resistive touch panel") {
        REQUIRE(is_resistive_touchscreen_name("rtp") == true);
    }

    SECTION("touch screen controller") {
        REQUIRE(is_resistive_touchscreen_name("tsc2046") == true);
    }

    SECTION("case insensitive") {
        REQUIRE(is_resistive_touchscreen_name("SUN4I-TS") == true);
    }

    SECTION("NS2009 I2C resistive (Nebula Pad)") {
        REQUIRE(is_resistive_touchscreen_name("ns2009") == true);
    }

    SECTION("NS2016 I2C resistive") {
        REQUIRE(is_resistive_touchscreen_name("NS2016") == true);
    }

    // --- Capacitive controllers that do NOT need calibration ---

    SECTION("Goodix capacitive") {
        REQUIRE(is_resistive_touchscreen_name("Goodix Capacitive TouchScreen") == false);
    }

    SECTION("Goodix GT911") {
        REQUIRE(is_resistive_touchscreen_name("gt911") == false);
    }

    SECTION("FocalTech capacitive") {
        REQUIRE(is_resistive_touchscreen_name("ft5x06_ts") == false);
    }

    SECTION("ILI capacitive") {
        REQUIRE(is_resistive_touchscreen_name("ili2130_ts") == false);
    }

    SECTION("EDT FocalTech") {
        REQUIRE(is_resistive_touchscreen_name("edt-ft5x06") == false);
    }

    SECTION("Atmel capacitive") {
        REQUIRE(is_resistive_touchscreen_name("atmel_mxt_ts") == false);
    }
}

// ============================================================================
// Unified Calibration Decision Tests (device_needs_calibration)
// ============================================================================

TEST_CASE("TouchCalibration: device_needs_calibration",
          "[touch-calibration][calibration-decision]") {
    // --- Devices that NEED calibration (resistive touchscreens only) ---

    SECTION("AD5M sun4i resistive touchscreen needs calibration") {
        // Platform resistive touchscreen: has ABS, not USB, resistive controller
        REQUIRE(device_needs_calibration("sun4i-ts", "sun4i_ts", true) == true);
    }

    SECTION("Generic resistive touch panel needs calibration") {
        REQUIRE(device_needs_calibration("rtp", "", true) == true);
    }

    SECTION("NS2009 I2C resistive needs calibration") {
        REQUIRE(device_needs_calibration("ns2009", "input/ts", true) == true);
    }

    // --- Capacitive touchscreens do NOT need calibration ---

    SECTION("Goodix I2C capacitive (BTT HDMI7) does not need calibration") {
        // I2C Goodix: has ABS, not USB, but capacitive — factory-calibrated
        REQUIRE(device_needs_calibration("Goodix Capacitive TouchScreen", "", true) == false);
    }

    SECTION("Goodix GT911 I2C does not need calibration") {
        REQUIRE(device_needs_calibration("gt911", "", true) == false);
    }

    SECTION("FocalTech capacitive does not need calibration") {
        REQUIRE(device_needs_calibration("ft5x06_ts", "", true) == false);
    }

    SECTION("EDT FocalTech display does not need calibration") {
        REQUIRE(device_needs_calibration("edt-ft5x06", "", true) == false);
    }

    // --- USB devices do NOT need calibration ---

    SECTION("USB HID touchscreen (BTT HDMI5) does not need calibration") {
        // USB touchscreen: has ABS, IS USB → no calibration
        REQUIRE(device_needs_calibration("BIQU BTT-HDMI5", "usb-5101400.usb-1/input0", true) ==
                false);
    }

    SECTION("USB HID generic touchscreen does not need calibration") {
        REQUIRE(device_needs_calibration("USB Touchscreen", "usb-0000:01:00.0-1.3/input0", true) ==
                false);
    }

    // --- Other non-calibration devices ---

    SECTION("Virtual touchscreen (VNC uinput) does not need calibration") {
        // Virtual device: has ABS, not USB, but name contains "virtual"
        REQUIRE(device_needs_calibration("virtual-touchscreen", "", true) == false);
    }

    SECTION("HDMI CEC remote does not need calibration") {
        // CEC remote: no ABS capabilities
        REQUIRE(device_needs_calibration("vc4-hdmi", "vc4-hdmi/input0", false) == false);
    }

    SECTION("HDMI audio jack does not need calibration") {
        REQUIRE(device_needs_calibration("vc4-hdmi HDMI Jack", "ALSA", false) == false);
    }

    SECTION("Device without ABS never needs calibration") {
        // Even a known touchscreen name without ABS should not trigger calibration
        REQUIRE(device_needs_calibration("Goodix Touch", "", false) == false);
    }

    SECTION("Unknown device with ABS does not need calibration") {
        // Has ABS but unrecognized name → safer to skip (not a known touchscreen)
        REQUIRE(device_needs_calibration("Random Input Device", "", true) == false);
    }

    SECTION("Keyboard does not need calibration") {
        REQUIRE(device_needs_calibration("AT Translated Set 2 keyboard", "", false) == false);
    }

    SECTION("USB mouse does not need calibration") {
        REQUIRE(device_needs_calibration("Logitech USB Mouse", "usb-0000:00:14.0-1/input0",
                                         false) == false);
    }

    SECTION("Empty device does not need calibration") {
        REQUIRE(device_needs_calibration("", "", false) == false);
    }

    SECTION("GPIO keys do not need calibration") {
        REQUIRE(device_needs_calibration("gpio-keys", "", false) == false);
    }
}

// ============================================================================
// Touch Device Scoring Scenario Tests
// ============================================================================
// These test the individual scoring factors (name recognition, USB detection)
// that auto_detect_touch_device() uses. The actual scoring loop requires sysfs
// access, but these verify the building blocks produce correct results for the
// scenarios described in issue #117.

TEST_CASE("TouchCalibration: phantom SPI vs real USB touchscreen scoring factors",
          "[touch-calibration][scoring]") {
    // Issue #117: ADS7846 SPI phantom device matched "touch" pattern but is not
    // the real touchscreen. The USB HDMI screen should win via PROP_DIRECT + USB.

    SECTION("ADS7846 Touchscreen matches known name (score +2)") {
        // Phantom ADS7846 has "touch" in its name, so it matches the known patterns
        REQUIRE(is_known_touchscreen_name("ADS7846 Touchscreen") == true);
    }

    SECTION("ADS7846 is SPI, not USB (no USB score bonus)") {
        REQUIRE(is_usb_input_phys("spi0.1/input0") == false);
    }

    SECTION("USB HDMI touchscreen is USB (score +1)") {
        REQUIRE(is_usb_input_phys("usb-0000:01:00.0-1.4/input0") == true);
    }

    SECTION("USB HDMI touchscreen with generic name does not match known patterns") {
        // Some USB HID touchscreens report generic names like "ILITEK ILITEK-TP"
        // They rely on PROP_DIRECT + USB bus for scoring, not name patterns
        REQUIRE(is_known_touchscreen_name("ILITEK ILITEK-TP") == false);
    }

    SECTION("BTT HDMI5 USB touchscreen matches known name") {
        REQUIRE(is_known_touchscreen_name("BIQU BTT-HDMI5 Touchscreen") == true);
    }
}

// ============================================================================
// ABS Range Mismatch Detection Tests (has_abs_display_mismatch)
// ============================================================================

TEST_CASE("TouchCalibration: has_abs_display_mismatch", "[touch-calibration][abs-mismatch]") {
    SECTION("matching ABS and display — no mismatch") {
        // ABS max matches display resolution exactly
        REQUIRE(has_abs_display_mismatch(800, 480, 800, 480) == false);
    }

    SECTION("matching within 5% tolerance — no mismatch") {
        // ABS max is ~4% off from display — within tolerance
        REQUIRE(has_abs_display_mismatch(832, 480, 800, 480) == false);
    }

    SECTION("SV06 Ace scenario: Goodix reports 800x480, display is 480x272") {
        // This is the exact bug scenario from issue #123
        REQUIRE(has_abs_display_mismatch(800, 480, 480, 272) == true);
    }

    SECTION("mismatch on X axis only") {
        REQUIRE(has_abs_display_mismatch(1024, 480, 800, 480) == true);
    }

    SECTION("mismatch on Y axis only") {
        REQUIRE(has_abs_display_mismatch(800, 600, 800, 480) == true);
    }

    SECTION("both axes mismatched") {
        REQUIRE(has_abs_display_mismatch(1024, 768, 800, 480) == true);
    }

    SECTION("invalid ABS ranges return false (can't determine)") {
        REQUIRE(has_abs_display_mismatch(0, 480, 800, 480) == false);
        REQUIRE(has_abs_display_mismatch(800, 0, 800, 480) == false);
        REQUIRE(has_abs_display_mismatch(-1, 480, 800, 480) == false);
        REQUIRE(has_abs_display_mismatch(800, -1, 800, 480) == false);
    }

    SECTION("invalid display dimensions return false") {
        REQUIRE(has_abs_display_mismatch(800, 480, 0, 480) == false);
        REQUIRE(has_abs_display_mismatch(800, 480, 800, 0) == false);
    }

    SECTION("ABS slightly smaller than display — within tolerance") {
        // ABS 770x460 vs display 800x480: ~3.75% and ~4.2%, within 5%
        REQUIRE(has_abs_display_mismatch(770, 460, 800, 480) == false);
    }

    SECTION("ABS at exactly 5% boundary") {
        // 5% of 800 = 40, so ABS 840 is right at the edge
        // 5% of 480 = 24, so ABS 504 is right at the edge
        // At exactly 5% the ratio equals TOLERANCE, which is not > TOLERANCE
        REQUIRE(has_abs_display_mismatch(840, 504, 800, 480) == false);
    }

    SECTION("ABS just beyond 5% boundary triggers mismatch") {
        // Just past 5% on X axis
        REQUIRE(has_abs_display_mismatch(841, 480, 800, 480) == true);
    }

    SECTION("generic HID range 4096x4096 — no mismatch (BTT HDMI5 scenario)") {
        // BTT HDMI5 reports 4096x4096, display is 800x480.
        // This is a generic HID range, NOT a real panel resolution.
        // LVGL's evdev driver maps it linearly — no calibration needed.
        REQUIRE(has_abs_display_mismatch(4096, 4096, 800, 480) == false);
    }

    SECTION("generic HID range 4095x4095 — no mismatch") {
        // 12-bit range (2^12 - 1), common USB HID touchscreens
        REQUIRE(has_abs_display_mismatch(4095, 4095, 800, 480) == false);
    }

    SECTION("generic HID range 32767x32767 — no mismatch") {
        // 15-bit range, another common USB HID format
        REQUIRE(has_abs_display_mismatch(32767, 32767, 1024, 600) == false);
    }

    SECTION("generic HID range 65535x65535 — no mismatch") {
        // 16-bit range
        REQUIRE(has_abs_display_mismatch(65535, 65535, 480, 272) == false);
    }

    SECTION("mixed generic/non-generic still triggers mismatch") {
        // One axis is generic HID, the other is a real resolution
        // Both must be generic to skip
        REQUIRE(has_abs_display_mismatch(4096, 480, 800, 480) == true);
        REQUIRE(has_abs_display_mismatch(800, 4096, 800, 480) == true);
    }

    SECTION("Goodix on Nebula Pad: 800x480 ABS on 480x272 display") {
        // Real panel resolution that doesn't match display — should trigger
        REQUIRE(has_abs_display_mismatch(800, 480, 480, 272) == true);
    }
}

TEST_CASE("TouchCalibration: is_generic_hid_abs_range", "[touch-calibration][abs-mismatch]") {
    SECTION("known generic HID ranges") {
        REQUIRE(is_generic_hid_abs_range(255) == true);
        REQUIRE(is_generic_hid_abs_range(1023) == true);
        REQUIRE(is_generic_hid_abs_range(4095) == true);
        REQUIRE(is_generic_hid_abs_range(4096) == true);
        REQUIRE(is_generic_hid_abs_range(8191) == true);
        REQUIRE(is_generic_hid_abs_range(16383) == true);
        REQUIRE(is_generic_hid_abs_range(32767) == true);
        REQUIRE(is_generic_hid_abs_range(65535) == true);
    }

    SECTION("real panel resolutions are NOT generic") {
        REQUIRE(is_generic_hid_abs_range(800) == false);
        REQUIRE(is_generic_hid_abs_range(480) == false);
        REQUIRE(is_generic_hid_abs_range(1024) == false);
        REQUIRE(is_generic_hid_abs_range(600) == false);
        REQUIRE(is_generic_hid_abs_range(272) == false);
        REQUIRE(is_generic_hid_abs_range(1280) == false);
    }
}

TEST_CASE("TouchCalibration: scoring factors for common touchscreen types",
          "[touch-calibration][scoring]") {
    SECTION("platform resistive (sun4i): known name, SPI bus") {
        REQUIRE(is_known_touchscreen_name("sun4i-ts") == true);
        REQUIRE(is_usb_input_phys("sun4i_ts") == false);
        // Score: 2 (known name) + 0 (not USB) = 2, plus PROP_DIRECT on real hw
    }

    SECTION("USB HID screen: USB bus, may or may not match name") {
        REQUIRE(is_usb_input_phys("usb-3f980000.usb-1.2/input0") == true);
        // Score: 0-2 (name) + 1 (USB) + potentially 2 (PROP_DIRECT) = 1-5
    }

    SECTION("I2C Goodix capacitive: known name, not USB") {
        REQUIRE(is_known_touchscreen_name("Goodix Capacitive TouchScreen") == true);
        REQUIRE(is_usb_input_phys("i2c-1/1-005d") == false);
        // Score: 2 (known name) + 0 (not USB) = 2, plus PROP_DIRECT on real hw
    }
}

// ============================================================================
// Post-Compute Validation Tests
// ============================================================================

TEST_CASE("TouchCalibration: validate_calibration_result accepts good calibration",
          "[touch-calibration][validate]") {
    // Identity calibration: residuals should be 0
    Point screen_points[3] = {{120, 86}, {400, 408}, {680, 86}};
    Point touch_points[3] = {{120, 86}, {400, 408}, {680, 86}};
    TouchCalibration cal;
    compute_calibration(screen_points, touch_points, cal);

    REQUIRE(validate_calibration_result(cal, screen_points, touch_points, 800, 480) == true);
}

TEST_CASE("TouchCalibration: validate_calibration_result rejects high residual",
          "[touch-calibration][validate]") {
    // Manually craft a calibration with large back-transform error
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 0.5f;
    cal.b = 0.0f;
    cal.c = 0.0f;
    cal.d = 0.0f;
    cal.e = 0.5f;
    cal.f = 0.0f;

    Point screen_points[3] = {{120, 86}, {400, 408}, {680, 86}};
    Point touch_points[3] = {{120, 86}, {400, 408}, {680, 86}};

    REQUIRE(validate_calibration_result(cal, screen_points, touch_points, 800, 480) == false);
}

TEST_CASE("TouchCalibration: validate_calibration_result rejects off-screen center",
          "[touch-calibration][validate]") {
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.0f;
    cal.b = 0.0f;
    cal.c = 5000.0f;
    cal.d = 0.0f;
    cal.e = 1.0f;
    cal.f = 0.0f;

    Point screen_points[3] = {{120, 86}, {400, 408}, {680, 86}};
    Point touch_points[3] = {{500, 500}, {2000, 3500}, {3500, 500}};

    REQUIRE(validate_calibration_result(cal, screen_points, touch_points, 800, 480) == false);
}

TEST_CASE("TouchCalibration: validate_calibration_result accepts real ns2009 calibration",
          "[touch-calibration][validate]") {
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 0.1258f;
    cal.b = -0.0025f;
    cal.c = -12.63f;
    cal.d = -0.0005f;
    cal.e = 0.0748f;
    cal.f = -16.20f;

    // Approximate raw->screen mapping for 480x272 display with 12-bit ADC
    Point screen_points[3] = {{72, 49}, {240, 231}, {408, 49}};
    Point touch_points[3] = {{673, 872}, {2007, 3307}, {3342, 872}};

    REQUIRE(validate_calibration_result(cal, screen_points, touch_points, 480, 272) == true);
}

TEST_CASE("TouchCalibration: validate_calibration_result accepts narrow-range capacitive device",
          "[touch-calibration][validate]") {
    // Mellow FLY-TFT35 with a Goodix capacitive touch IC whose config bytes
    // emit a compressed sub-range of the chip's declared 0..4095 ABS axes:
    // X spans ~7..48, Y spans ~299..313 across the full 480x320 panel. The
    // affine fit needs scale coefficients around a≈8.4, e≈13.2 — well above
    // the old 10x cap that wrongly rejected this device.
    Point screen_points[3] = {{72, 64}, {240, 249}, {408, 64}};
    Point touch_points[3] = {{8, 300}, {28, 313}, {48, 299}};
    TouchCalibration cal;
    compute_calibration(screen_points, touch_points, cal);

    REQUIRE(validate_calibration_result(cal, screen_points, touch_points, 480, 320) == true);
}

// ============================================================================
// Multi-Sample Input Filtering Tests
// ============================================================================

TEST_CASE("TouchCalibrationPanel: accepts clean samples after threshold",
          "[touch-calibration][filtering]") {
    helix::TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);
    panel.start();

    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_1);

    // Feed 3 clean samples — should advance to POINT_2
    for (int i = 0; i < 3; i++) {
        panel.add_sample({400, 240});
    }
    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_2);
}

TEST_CASE("TouchCalibrationPanel: rejects ADC-saturated samples",
          "[touch-calibration][filtering]") {
    helix::TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);
    panel.start();

    // Feed 2 clean + 1 saturated (X=4095) — should still advance (2 valid >= 2 minimum)
    panel.add_sample({400, 240});
    panel.add_sample({400, 240});
    panel.add_sample({4095, 240});
    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_2);
}

TEST_CASE("TouchCalibrationPanel: fails when too many saturated samples",
          "[touch-calibration][filtering]") {
    helix::TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    bool failure_called = false;
    panel.set_failure_callback([&](const char*) { failure_called = true; });
    panel.start();

    // Feed 1 clean + 2 saturated — only 1 valid, below minimum of 2
    panel.add_sample({400, 240});
    panel.add_sample({4095, 3500});
    panel.add_sample({4095, 3500});

    // Should still be on POINT_1 (not advanced) and failure callback fired
    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called == true);
}

TEST_CASE("TouchCalibrationPanel: rejects calibration with bad matrix",
          "[touch-calibration][panel-validate]") {
    helix::TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);

    bool failure_called = false;
    std::string failure_reason;
    panel.set_failure_callback([&](const char* reason) {
        failure_called = true;
        failure_reason = reason;
    });
    panel.start();

    // Capture 3 points that produce a valid but terrible matrix
    // Points very close together (not collinear, so compute_calibration succeeds)
    // but resulting matrix will have huge residuals
    panel.capture_point({100, 100});
    panel.capture_point({102, 100});
    panel.capture_point({100, 102});

    // Should restart to POINT_1 (not enter VERIFY)
    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called == true);
    REQUIRE(failure_reason.find("unusual") != std::string::npos);
}

TEST_CASE("TouchCalibrationPanel: median filter removes outliers",
          "[touch-calibration][filtering]") {
    helix::TouchCalibrationPanel panel;
    panel.set_screen_size(800, 480);
    panel.start();

    // 3 samples, all consistent — median should be (400, 240)
    panel.add_sample({399, 239});
    panel.add_sample({400, 240});
    panel.add_sample({401, 241});

    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_2);
}

// ============================================================================
// ABS Capabilities Parsing Tests (parse_abs_capabilities)
// ============================================================================

TEST_CASE("TouchCalibration: parse_abs_capabilities basic cases",
          "[touch-calibration][capabilities]") {
    SECTION("single-touch only (ABS_X + ABS_Y)") {
        // "3" = 0x3 → bits 0 and 1 set
        auto caps = parse_abs_capabilities("3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("both single-touch and MT in one word") {
        // "600003" = 0x600003 → bits 0,1 (ST) and bits 21,22 of this word
        // But this is a SINGLE word covering bits 0-31 only.
        // MT bits 53,54 need word index 1 (bits 32-63).
        // So "600003" has ST but NOT MT (bits 21,22 are ABS_HAT0X/ABS_HAT0Y, not MT).
        auto caps = parse_abs_capabilities("600003");
        REQUIRE(caps.has_single_touch == true);
        // 0x600000 in word[0] is NOT MT — MT needs word[1]
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("MT-only (no legacy single-touch)") {
        // "600000 0" → word[1]=0x600000 (MT bits), word[0]=0 (no ST)
        // After reversal: words[0]=0, words[1]=0x600000
        auto caps = parse_abs_capabilities("600000 0");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("both MT and single-touch (two words)") {
        // "600000 3" → word[1]=0x600000, word[0]=3
        auto caps = parse_abs_capabilities("600000 3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("no touch capabilities") {
        auto caps = parse_abs_capabilities("0");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("empty string") {
        auto caps = parse_abs_capabilities("");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("only ABS_X, no ABS_Y") {
        // "1" = bit 0 only
        auto caps = parse_abs_capabilities("1");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("only ABS_Y, no ABS_X") {
        // "2" = bit 1 only
        auto caps = parse_abs_capabilities("2");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == false);
    }
}

TEST_CASE("TouchCalibration: parse_abs_capabilities real-world devices",
          "[touch-calibration][capabilities]") {
    SECTION("AD5M sun4i_ts resistive: single-touch only") {
        // sun4i_ts reports ABS_X + ABS_Y only (no MT)
        auto caps = parse_abs_capabilities("3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("Goodix gt9xxnew_ts MT-only (Nebula Pad bug scenario)") {
        // Goodix driver reports ABS_MT_POSITION_X (53) + ABS_MT_POSITION_Y (54)
        // but NOT legacy ABS_X (0) / ABS_Y (1).
        auto caps = parse_abs_capabilities("600000 0");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("Goodix gt9xx_ts on 64-bit Allwinner (real user device)") {
        // From /proc/bus/input/devices on Allwinner H616 (aarch64):
        //   B: ABS=265000000000000
        // Single 64-bit word (>8 hex digits). Bits set:
        //   48 (MT_TOUCH_MAJOR), 50 (MT_WIDTH_MAJOR),
        //   53 (MT_POSITION_X), 54 (MT_POSITION_Y), 57 (MT_TRACKING_ID)
        auto caps = parse_abs_capabilities("265000000000000");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("Goodix GT911 with both ST and MT") {
        // Many Goodix drivers report both legacy and MT axes
        auto caps = parse_abs_capabilities("660000 3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("BTT HDMI5 USB HID touchscreen") {
        // USB HID typically reports ABS_X + ABS_Y (single-touch only)
        auto caps = parse_abs_capabilities("3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("multi-word with three hex groups") {
        // "0 600000 3" → words[0]=3, words[1]=0x600000, words[2]=0
        auto caps = parse_abs_capabilities("0 600000 3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == true);
    }
}

TEST_CASE("TouchCalibration: parse_abs_capabilities edge cases",
          "[touch-calibration][capabilities]") {
    SECTION("leading zeros in hex") {
        auto caps = parse_abs_capabilities("0000600000 00000003");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("extra whitespace between words") {
        // istringstream handles multiple spaces
        auto caps = parse_abs_capabilities("600000  3");
        REQUIRE(caps.has_single_touch == true);
        REQUIRE(caps.has_multitouch == true);
    }

    SECTION("invalid hex returns no capabilities") {
        auto caps = parse_abs_capabilities("xyz");
        REQUIRE(caps.has_single_touch == false);
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("MT bit ABS_MT_POSITION_X only (no ABS_MT_POSITION_Y)") {
        // Only bit 53 (0x200000) set, not bit 54 — incomplete MT
        auto caps = parse_abs_capabilities("200000 0");
        REQUIRE(caps.has_multitouch == false);
    }

    SECTION("MT bit ABS_MT_POSITION_Y only (no ABS_MT_POSITION_X)") {
        // Only bit 54 (0x400000) set, not bit 53 — incomplete MT
        auto caps = parse_abs_capabilities("400000 0");
        REQUIRE(caps.has_multitouch == false);
    }
}

// ============================================================================
// Calibration Decision with MT-only devices
// ============================================================================

TEST_CASE("TouchCalibration: device_needs_calibration with MT-detected devices",
          "[touch-calibration][calibration-decision]") {
    SECTION("Goodix gt9xxnew_ts detected via MT — capacitive, no calibration needed") {
        // has_abs_xy is true (detected via MT fallback), but Goodix is capacitive
        REQUIRE(device_needs_calibration("Goodix-TS gt9xxnew_ts", "", true) == false);
    }

    SECTION("Goodix GT9xx detected via MT — capacitive, no calibration needed") {
        REQUIRE(device_needs_calibration("gt9xx_ts", "", true) == false);
    }

    SECTION("MT-only resistive (hypothetical) — would need calibration") {
        // If a resistive touchscreen only reported MT axes, it would still need cal
        REQUIRE(device_needs_calibration("sun4i-ts", "sun4i_ts", true) == true);
    }
}

// ============================================================================
// Manual Calibration Availability (device_supports_calibration)
// ============================================================================

TEST_CASE("TouchCalibration: device_supports_calibration", "[touch-calibration][1259]") {
    // --- Real touch panels: the manual entry point is always offered ---

    SECTION("Capacitive panel the auto-heuristic skips still offers manual calibration") {
        // tlsc6x_touch on an FLSUN T1 Pro: touch panel mounted 90° from the
        // display. device_needs_calibration() says no (not a known resistive
        // name), which is correct for auto-firing the wizard and fatal if it
        // also hides the manual path.
        REQUIRE(device_needs_calibration("tlsc6x_touch", "", true) == false);
        REQUIRE(device_supports_calibration("tlsc6x_touch", true) == true);
    }

    SECTION("Goodix capacitive offers manual calibration") {
        REQUIRE(device_supports_calibration("Goodix-TS gt9xxnew_ts", true) == true);
    }

    SECTION("Resistive panel offers manual calibration") {
        REQUIRE(device_supports_calibration("sun4i-ts", true) == true);
    }

    SECTION("USB HID touchscreen offers manual calibration") {
        // Auto-detection skips USB HID (it reports mapped coordinates), but a
        // USB panel can still be physically rotated inside an enclosure.
        REQUIRE(device_supports_calibration("BIQU BTT-HDMI5", true) == true);
    }

    SECTION("Unknown-but-real touchscreen offers manual calibration") {
        REQUIRE(device_supports_calibration("some-unknown-ts", true) == true);
    }

    // --- Not touch panels: nothing to calibrate ---

    SECTION("Device without ABS axes offers nothing") {
        REQUIRE(device_supports_calibration("gpio-keys", false) == false);
        REQUIRE(device_supports_calibration("AT Translated Set 2 keyboard", false) == false);
    }

    SECTION("Virtual/uinput device offers nothing") {
        // VNC injection feeds already-mapped screen coordinates; an affine on
        // top of that has nothing to correct.
        REQUIRE(device_supports_calibration("virtual-touchscreen", true) == false);
    }

    SECTION("Empty name with ABS axes still offers manual calibration") {
        // A nameless platform touchscreen is exactly the case the name-based
        // auto-heuristic cannot classify, so it must not lose the manual path.
        REQUIRE(device_supports_calibration("", true) == true);
    }

    // --- Relationship to the auto-fire decision ---

    SECTION("Every device that needs calibration also supports it") {
        // supports is a superset of needs: the wizard must never auto-fire on a
        // device whose manual entry point is hidden.
        const char* auto_fire_devices[] = {"sun4i-ts", "rtp", "ns2009", "tsc2007"};
        for (const char* name : auto_fire_devices) {
            INFO("device: " << name);
            REQUIRE(device_needs_calibration(name, "", true) == true);
            REQUIRE(device_supports_calibration(name, true) == true);
        }
    }
}

// ============================================================================
// Axis Swap Detection and Correction Tests
// ============================================================================

TEST_CASE("TouchCalibration: detect_and_correct_axis_swap with swapped 800x480 screen",
          "[touch-calibration][axis-swap]") {
    // Simulate a touchscreen where the controller reports X/Y swapped.
    // Screen targets are at the standard calibration positions for 800x480.
    Point screen_points[3] = {
        {120, 96},  // 15% from left, 20% from top
        {400, 374}, // center X, 78% from top
        {680, 96}   // 85% from left, 20% from top
    };

    // Touch points with X/Y swapped: what would be (120,96) in correct space
    // comes in as (96,120) from the swapped controller.
    // Use values in the LVGL-mapped range (post linear mapping, screen-ish coords).
    Point touch_points[3] = {
        {96, 120},  // Swapped version of (120, 96)
        {374, 400}, // Swapped version of (400, 374)
        {96, 680}   // Swapped version of (680, 96)
    };

    // First compute calibration from the swapped data
    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    // The original calibration should have high cross-coupling
    float orig_diagonal = std::abs(cal.a) + std::abs(cal.e);
    float orig_off_diag = std::abs(cal.b) + std::abs(cal.d);
    REQUIRE(orig_off_diag / orig_diagonal > 0.5f);

    // Now detect and correct
    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == true);
    REQUIRE(cal.axes_swapped == true);

    // After correction, diagonal should dominate
    float new_diagonal = std::abs(cal.a) + std::abs(cal.e);
    float new_off_diag = std::abs(cal.b) + std::abs(cal.d);
    REQUIRE(new_off_diag / new_diagonal < 0.5f);

    // The corrected calibration should produce correct screen coordinates
    // touch_points were swapped in-place, so transform them
    Point p0 = transform_point(cal, touch_points[0]);
    REQUIRE(p0.x == Approx(screen_points[0].x).margin(2));
    REQUIRE(p0.y == Approx(screen_points[0].y).margin(2));

    Point p1 = transform_point(cal, touch_points[1]);
    REQUIRE(p1.x == Approx(screen_points[1].x).margin(2));
    REQUIRE(p1.y == Approx(screen_points[1].y).margin(2));
}

TEST_CASE("TouchCalibration: detect_and_correct_axis_swap with non-swapped screen",
          "[touch-calibration][axis-swap]") {
    // Normal (non-swapped) calibration — should NOT trigger swap
    Point screen_points[3] = {{120, 96}, {400, 374}, {680, 96}};

    // Touch points match screen points with slight offset (typical for cap screens)
    Point touch_points[3] = {{125, 100}, {405, 378}, {685, 100}};

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    // Should NOT detect swap — diagonal is already dominant
    Point original_touch[3] = {touch_points[0], touch_points[1], touch_points[2]};
    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == false);
    REQUIRE(cal.axes_swapped == false);

    // Touch points should be unchanged
    for (int i = 0; i < 3; i++) {
        REQUIRE(touch_points[i].x == original_touch[i].x);
        REQUIRE(touch_points[i].y == original_touch[i].y);
    }
}

TEST_CASE("TouchCalibration: detect_and_correct_axis_swap with ADC-range swapped axes",
          "[touch-calibration][axis-swap]") {
    // Simulate resistive touchscreen with 12-bit ADC and swapped axes
    // on 800x480 screen
    Point screen_points[3] = {{120, 96}, {400, 374}, {680, 96}};

    // Raw ADC values (0-4095 range) but X/Y swapped
    Point touch_points[3] = {
        {820, 614},   // Swapped: should be (614, 820)
        {3190, 2048}, // Swapped: should be (2048, 3190)
        {820, 3482}   // Swapped: should be (3482, 820)
    };

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == true);
    REQUIRE(cal.axes_swapped == true);

    // Verify the corrected calibration maps correctly
    Point p = transform_point(cal, touch_points[0]);
    REQUIRE(p.x == Approx(screen_points[0].x).margin(2));
    REQUIRE(p.y == Approx(screen_points[0].y).margin(2));
}

TEST_CASE("TouchCalibration: axis swap with Ender 5 Max-like coefficients",
          "[touch-calibration][axis-swap]") {
    // Reproduce the actual broken calibration from the Ender 5 Max user
    // The original coefficients (d=1.187, e=2.367) indicate heavy cross-coupling
    // Simulate the touch input that would produce these coefficients

    Point screen_points[3] = {{120, 96}, {400, 374}, {680, 96}};

    // These touch points produce high cross-coupling when not swapped
    // (representing a screen where touch X/Y are transposed)
    Point touch_points[3] = {{50, 100}, {250, 350}, {50, 600}};

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    // Check if this triggers swap detection
    float diag = std::abs(cal.a) + std::abs(cal.e);
    float off_diag = std::abs(cal.b) + std::abs(cal.d);
    float ratio = off_diag / diag;

    if (ratio > 0.5f) {
        bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
        REQUIRE(swapped == true);

        // After swap, the calibration should produce valid screen coords
        Point p = transform_point(cal, touch_points[0]);
        REQUIRE(p.x == Approx(screen_points[0].x).margin(2));
        REQUIRE(p.y == Approx(screen_points[0].y).margin(2));
    }
}

TEST_CASE("TouchCalibration: axis swap detection does not trigger on slight rotation",
          "[touch-calibration][axis-swap]") {
    // A slightly rotated touchscreen has small cross-coupling but diagonal still dominates
    Point screen_points[3] = {{120, 96}, {400, 374}, {680, 96}};

    // Slight clockwise rotation (~5 degrees) in touch coordinates
    // cos(5°)≈0.996, sin(5°)≈0.087
    Point touch_points[3] = {
        {static_cast<int>(120 * 0.996 + 96 * 0.087), static_cast<int>(-120 * 0.087 + 96 * 0.996)},
        {static_cast<int>(400 * 0.996 + 374 * 0.087), static_cast<int>(-400 * 0.087 + 374 * 0.996)},
        {static_cast<int>(680 * 0.996 + 96 * 0.087), static_cast<int>(-680 * 0.087 + 96 * 0.996)},
    };

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == false);
}

TEST_CASE("TouchCalibration: NS2009 recognized as known touchscreen",
          "[touch-calibration][resistive-detection]") {
    REQUIRE(is_known_touchscreen_name("ns2009") == true);
    REQUIRE(is_known_touchscreen_name("NS2009") == true);
    REQUIRE(is_known_touchscreen_name("ns2016") == true);
}

TEST_CASE("TouchCalibration: detect axis swap on 480x272 with swapped ADC and diagonal leak",
          "[touch-calibration][axis-swap]") {
    // 480x272 screen — NS2009 resistive ADC with hardware X/Y swap
    // plus slight diagonal leakage from cross-talk in the ADC channels.
    // The cross-coupling ratio lands above 1.0 (off-diagonal dominates),
    // confirming axes are swapped. After correction, the ratio drops below 0.5.
    Point screen_points[3] = {
        {72, 54},   // 15% of 480, 20% of 272
        {240, 212}, // 50%, 78%
        {408, 54},  // 85%, 20%
    };

    // Raw ADC values: primarily swapped (physical X -> ABS_Y, physical Y -> ABS_X)
    // with small additive perturbation from diagonal cross-talk
    Point touch_points[3] = {
        {728, 830},
        {2693, 2896},
        {2072, 3698},
    };

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    // Verify the cross-coupling ratio indicates swapped axes (ratio > 1.0)
    float diag = std::abs(cal.a) + std::abs(cal.e);
    float off_diag = std::abs(cal.b) + std::abs(cal.d);
    float ratio = off_diag / std::max(diag, 0.001f);
    INFO("Cross-coupling ratio: " << ratio);
    REQUIRE(ratio > 0.3f);
    REQUIRE(ratio < 3.0f);

    // Swap should be detected and corrected
    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == true);
    REQUIRE(cal.axes_swapped == true);

    // After correction, transform should produce correct screen coordinates
    Point p0 = transform_point(cal, touch_points[0]);
    REQUIRE(p0.x == Approx(screen_points[0].x).margin(2));
    REQUIRE(p0.y == Approx(screen_points[0].y).margin(2));
}

TEST_CASE("TouchCalibration: no false axis swap on well-aligned 480x272 screen",
          "[touch-calibration][axis-swap]") {
    // 480x272 screen, non-swapped — touch axes are correct
    Point screen_points[3] = {
        {72, 54},
        {240, 212},
        {408, 54},
    };

    // Non-swapped ADC values (normal orientation, proportional to screen coords)
    Point touch_points[3] = {
        {776, 632},
        {2120, 1896},
        {3464, 632},
    };

    TouchCalibration cal;
    REQUIRE(compute_calibration(screen_points, touch_points, cal));

    // Should NOT detect swap — diagonal dominates completely
    float diag = std::abs(cal.a) + std::abs(cal.e);
    float off_diag = std::abs(cal.b) + std::abs(cal.d);
    float ratio = off_diag / std::max(diag, 0.001f);
    INFO("Cross-coupling ratio: " << ratio);
    REQUIRE(ratio < 0.3f);

    bool swapped = detect_and_correct_axis_swap(cal, screen_points, touch_points);
    REQUIRE(swapped == false);
}

// ============================================================================
// Legacy calibration invalidation (post-#943 upgrade) — truth table
// ============================================================================

TEST_CASE("TouchCalibration: should_invalidate_legacy_calibration truth table",
          "[touch-calibration][migration]") {
    // Resistive panels legitimately need their affine — never reset, even on mismatch.
    REQUIRE(should_invalidate_legacy_calibration(/*recheck*/ true, /*resistive*/ true,
                                                 /*mismatch*/ true) == false);

    // Non-resistive panel with an ABS/display mismatch, recheck pending → invalidate.
    // (Qidi Q2: 800x480 capacitive controller on a 480x272 panel.)
    REQUIRE(should_invalidate_legacy_calibration(/*recheck*/ true, /*resistive*/ false,
                                                 /*mismatch*/ true) == true);

    // No recheck pending → never touch the stored calibration.
    REQUIRE(should_invalidate_legacy_calibration(/*recheck*/ false, /*resistive*/ false,
                                                 /*mismatch*/ true) == false);

    // Non-resistive, no mismatch (e.g. generic HID range) → calibration was fine.
    REQUIRE(should_invalidate_legacy_calibration(/*recheck*/ true, /*resistive*/ false,
                                                 /*mismatch*/ false) == false);
}

// ============================================================================
// Read-callback wrapper: context handle, not indev user_data
// ============================================================================
//
// Regression for prestonbrown/helixscreen#1112 (bundle LG9X482B): the calibrated
// read path used to fetch its CalibrationContext via lv_indev_get_user_data(),
// and a stale/corrupted user_data slot (holding non-null garbage) crashed it. The
// wrapper now keeps the context in its own static storage and must ignore
// user_data entirely — so poisoning user_data must not affect the transform.

TEST_CASE_METHOD(LVGLTestFixture,
                 "calibrated_read_cb: transforms via static handle, ignores indev user_data",
                 "[touch-calibration][wrapper][regression]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    // A valid affine calibration: screen_x = x + 100, screen_y = y + 50.
    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.0f, cal.b = 0.0f, cal.c = 100.0f;
    cal.d = 0.0f, cal.e = 1.0f, cal.f = 50.0f;

    CalibrationContext ctx;
    install_calibration_wrapper(indev, ctx, cal, 800, 480);

    // The read callback must have been installed on the indev.
    CHECK(lv_indev_get_read_cb(indev) == helix::calibrated_read_cb);

    // Poison user_data with a non-null garbage value — the exact failure shape of
    // #1112. The old code dereferenced this; the new code must ignore it.
    lv_indev_set_user_data(indev, reinterpret_cast<void*>(0xDEADBEEFUL));

    lv_indev_data_t data{};
    data.point.x = 10;
    data.point.y = 20;
    data.state = LV_INDEV_STATE_PRESSED;
    calibrated_read_cb(indev, &data);

    const Point expected = transform_point(cal, {10, 20}, 799, 479);
    CHECK(data.point.x == expected.x);
    CHECK(data.point.y == expected.y);

    // After uninstall the callback is silenced and the static handle is dropped,
    // so a subsequent call is an inert passthrough (coordinates untouched).
    uninstall_calibration_wrapper(indev, ctx);
    CHECK(lv_indev_get_read_cb(indev) == nullptr);

    lv_indev_data_t data2{};
    data2.point.x = 10;
    data2.point.y = 20;
    calibrated_read_cb(indev, &data2);
    CHECK(data2.point.x == 10);
    CHECK(data2.point.y == 20);

    lv_indev_delete(indev);
}
