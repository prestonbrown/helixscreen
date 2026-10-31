// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "lvgl_test_fixture.h"
#include "test_helpers/scoped_board.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_wrapper.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix;

namespace {

using Triple = std::array<Point, 3>;

/// Solves `screen` from `touch`, or fails the case.
TouchCalibration solved(const Triple& screen, const Triple& touch) {
    TouchCalibration cal;
    REQUIRE(compute_calibration(screen.data(), touch.data(), cal));
    REQUIRE(cal.valid);
    return cal;
}

/// |b| + |d| over |a| + |e|: how much each screen axis leans on the other touch axis.
float cross_coupling(const TouchCalibration& cal) {
    return (std::abs(cal.b) + std::abs(cal.d)) /
           std::max(std::abs(cal.a) + std::abs(cal.e), 0.001f);
}

} // namespace

// ============================================================================
// The three-point solve
// ============================================================================

TEST_CASE("TouchCalibration: the three-point solve maps each touch onto its screen point",
          "[touch-calibration][compute][transform][coefficients]") {
    struct Probe {
        Point touch, screen;
    };
    struct Coeffs {
        float a, b, c, d, e, f;
    };
    struct Row {
        const char* what;
        Triple screen, touch;
        int margin;
        std::vector<Probe> probes;
        std::optional<Coeffs> coeffs;
    };
    const Row rows[] = {
        {"identity",
         {{{0, 0}, {100, 0}, {0, 100}}},
         {{{0, 0}, {100, 0}, {0, 100}}},
         1,
         {},
         Coeffs{1, 0, 0, 0, 1, 0}},
        {"touch 0-1000 onto 800x480",
         {{{0, 0}, {800, 0}, {0, 480}}},
         {{{0, 0}, {1000, 0}, {0, 1000}}},
         1,
         {{{1000, 0}, {800, 0}}, {{0, 1000}, {0, 480}}, {{500, 500}, {400, 240}}},
         std::nullopt},
        {"an offset mapping",
         {{{100, 100}, {700, 100}, {100, 380}}},
         {{{0, 0}, {600, 0}, {0, 280}}},
         1,
         {{{0, 0}, {100, 100}}, {{600, 0}, {700, 100}}, {{0, 280}, {100, 380}}},
         std::nullopt},
        // An 800x480 screen's inset targets against raw resistive readings.
        {"AD5M-like raw readings",
         {{{120, 144}, {400, 408}, {680, 72}}},
         {{{500, 3200}, {2040, 900}, {3580, 3500}}},
         2,
         {{{500, 3200}, {120, 144}}, {{2040, 900}, {400, 408}}, {{3580, 3500}, {680, 72}}},
         std::nullopt},
        // Raw Y grows upward on many resistive panels; screen Y grows downward.
        {"Y inverted",
         {{{0, 0}, {800, 0}, {0, 480}}},
         {{{0, 480}, {800, 480}, {0, 0}}},
         1,
         {{{0, 480}, {0, 0}}, {{0, 0}, {0, 480}}, {{400, 240}, {400, 240}}},
         std::nullopt},
        {"half scale",
         {{{0, 0}, {100, 0}, {0, 100}}},
         {{{0, 0}, {200, 0}, {0, 200}}},
         1,
         {{{0, 0}, {0, 0}}, {{200, 0}, {100, 0}}, {{0, 200}, {0, 100}}, {{100, 100}, {50, 50}}},
         std::nullopt},
        {"a quarter turn",
         {{{0, 0}, {0, 100}, {100, 0}}},
         {{{0, 0}, {100, 0}, {0, 100}}},
         1,
         {{{100, 0}, {0, 100}}, {{0, 100}, {100, 0}}},
         std::nullopt},
        {"extrapolates outside the triangle",
         {{{100, 100}, {200, 100}, {100, 200}}},
         {{{100, 100}, {200, 100}, {100, 200}}},
         1,
         {{{300, 300}, {300, 300}}, {{0, 0}, {0, 0}}},
         std::nullopt},
        {"X scaled by 0.8",
         {{{0, 0}, {80, 0}, {0, 100}}},
         {{{0, 0}, {100, 0}, {0, 100}}},
         1,
         {},
         Coeffs{0.8f, 0, 0, 0, 1, 0}},
        {"Y scaled by 0.48",
         {{{0, 0}, {100, 0}, {0, 48}}},
         {{{0, 0}, {100, 0}, {0, 100}}},
         1,
         {},
         Coeffs{1, 0, 0, 0, 0.48f, 0}},
        {"translated by (50, 30)",
         {{{50, 30}, {150, 30}, {50, 130}}},
         {{{0, 0}, {100, 0}, {0, 100}}},
         1,
         {},
         Coeffs{1, 0, 50, 0, 1, 30}},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const TouchCalibration cal = solved(row.screen, row.touch);
        for (const auto& probe : row.probes) {
            CAPTURE(probe.touch.x, probe.touch.y);
            const Point p = transform_point(cal, probe.touch);
            CHECK(p.x == Approx(probe.screen.x).margin(row.margin));
            CHECK(p.y == Approx(probe.screen.y).margin(row.margin));
        }
        if (row.coeffs) {
            CHECK(cal.a == Approx(row.coeffs->a).margin(0.001f));
            CHECK(cal.b == Approx(row.coeffs->b).margin(0.001f));
            CHECK(cal.c == Approx(row.coeffs->c).margin(0.001f));
            CHECK(cal.d == Approx(row.coeffs->d).margin(0.001f));
            CHECK(cal.e == Approx(row.coeffs->e).margin(0.001f));
            CHECK(cal.f == Approx(row.coeffs->f).margin(0.001f));
        }
    }
}

TEST_CASE("TouchCalibration: degenerate touch points give no calibration",
          "[touch-calibration][edge]") {
    struct Row {
        const char* what;
        Triple screen, touch;
    };
    const Row rows[] = {
        {"collinear", {{{0, 0}, {100, 100}, {200, 200}}}, {{{0, 0}, {100, 100}, {200, 200}}}},
        {"two identical", {{{0, 0}, {100, 0}, {0, 100}}}, {{{50, 50}, {50, 50}, {100, 100}}}},
        // The screen side is barely off the line; the touch side is on it.
        {"nearly collinear",
         {{{0, 0}, {100, 100}, {200, 201}}},
         {{{0, 0}, {100, 100}, {200, 200}}}},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        TouchCalibration cal;
        CHECK_FALSE(compute_calibration(row.screen.data(), row.touch.data(), cal));
        CHECK_FALSE(cal.valid);
    }
}

TEST_CASE("TouchCalibration: an invalid calibration passes points through untouched",
          "[touch-calibration][state]") {
    TouchCalibration cal;
    REQUIRE_FALSE(cal.valid); // the default
    const Point result = transform_point(cal, {500, 300});
    CHECK(result.x == 500);
    CHECK(result.y == 300);
}

// ============================================================================
// Device classification
// ============================================================================

TEST_CASE("TouchCalibration: name and bus classifiers",
          "[touch-calibration][usb-detect]"
          "[touchscreen-detect][resistive-detection][scoring]") {
    struct Row {
        const char* input;
        bool expect;
    };
    SECTION("is_usb_input_phys") {
        const Row rows[] = {
            {"usb-0000:01:00.0-1.3/input0", true}, // BTT HDMI, Waveshare
            {"usb-3f980000.usb-1.2/input0", true},
            {"usb-xhci-hcd.0-1/input1", true}, // composite device
            {"usb-0000:01:00.0-1.4/input0", true},
            {"", false},         // AD5M sun4i_ts
            {"sun4i_ts", false}, // platform resistive
            {"i2c-1/1-005d", false},
            {"spi0.0/input0", false},
            {"spi0.1/input0", false}, // ADS7846 phantom (#117)
        };
        for (const auto& row : rows) {
            CAPTURE(row.input);
            CHECK(is_usb_input_phys(row.input) == row.expect);
        }
    }
    SECTION("is_known_touchscreen_name") {
        const Row rows[] = {
            {"sun4i-ts", true},
            {"Goodix Capacitive TouchScreen", true},
            {"ft5x06_ts", true},
            {"gt911", true},
            {"ili2130_ts", true},
            {"Generic Touchscreen", true},
            {"edt-ft5x06", true},
            {"GOODIX Touch", true},
            {"SUN4I-TS", true},
            {"ns2009", true},
            {"NS2009", true},
            {"ns2016", true},
            {"BIQU BTT-HDMI5 Touchscreen", true},
            // A phantom SPI device still matches "touch" (#117): bus and
            // PROP_DIRECT outscore it, not the name.
            {"ADS7846 Touchscreen", true},
            // A generic USB HID name relies on bus and PROP_DIRECT instead.
            {"ILITEK ILITEK-TP", false},
            {"vc4-hdmi", false},
            {"vc4-hdmi HDMI Jack", false},
            {"AT Translated Set 2 keyboard", false},
            {"Logitech USB Mouse", false},
            {"Power Button", false},
            {"gpio-keys", false},
            {"", false},
            {"rc-cec", false},
        };
        for (const auto& row : rows) {
            CAPTURE(row.input);
            CHECK(is_known_touchscreen_name(row.input) == row.expect);
        }
    }
    SECTION("is_resistive_touchscreen_name") {
        const Row rows[] = {
            {"sun4i-ts", true},      {"rtp", true},
            {"tsc2046", true},       {"SUN4I-TS", true},
            {"ns2009", true}, // Nebula Pad, I2C
            {"NS2016", true},        {"Goodix Capacitive TouchScreen", false},
            {"gt911", false},        {"ft5x06_ts", false},
            {"ili2130_ts", false},   {"edt-ft5x06", false},
            {"atmel_mxt_ts", false},
        };
        for (const auto& row : rows) {
            CAPTURE(row.input);
            CHECK(is_resistive_touchscreen_name(row.input) == row.expect);
        }
    }
}

TEST_CASE("TouchCalibration: only a resistive panel auto-fires the wizard; any real panel can "
          "run it",
          "[touch-calibration][calibration-decision][1259]") {
    struct Row {
        const char* name;
        const char* phys;
        bool has_abs;
        bool needs;
        bool supports;
    };
    const Row rows[] = {
        {"sun4i-ts", "sun4i_ts", true, true, true}, // AD5M
        {"rtp", "", true, true, true},
        {"ns2009", "input/ts", true, true, true},
        {"tsc2007", "", true, true, true},
        // Capacitive panels are factory-calibrated, but one can still be
        // mounted rotated from the display (tlsc6x on an FLSUN T1 Pro).
        {"Goodix Capacitive TouchScreen", "", true, false, true},
        {"gt911", "", true, false, true},
        {"ft5x06_ts", "", true, false, true},
        {"edt-ft5x06", "", true, false, true},
        {"Goodix-TS gt9xxnew_ts", "", true, false, true}, // found through its MT axes
        {"gt9xx_ts", "", true, false, true},
        {"tlsc6x_touch", "", true, false, true},
        // USB HID reports mapped coordinates; the panel can still sit rotated.
        {"BIQU BTT-HDMI5", "usb-5101400.usb-1/input0", true, false, true},
        {"USB Touchscreen", "usb-0000:01:00.0-1.3/input0", true, false, true},
        {"Random Input Device", "", true, false, true},
        {"some-unknown-ts", "", true, false, true},
        // A nameless platform touchscreen is what the name heuristic cannot
        // classify, so it keeps the manual path.
        {"", "", true, false, true},
        // VNC injection feeds mapped coordinates: nothing to correct.
        {"virtual-touchscreen", "", true, false, false},
        // No ABS axes: not a touch panel, whatever its name.
        {"Goodix Touch", "", false, false, false},
        {"vc4-hdmi", "vc4-hdmi/input0", false, false, false},
        {"vc4-hdmi HDMI Jack", "ALSA", false, false, false},
        {"AT Translated Set 2 keyboard", "", false, false, false},
        {"Logitech USB Mouse", "usb-0000:00:14.0-1/input0", false, false, false},
        {"gpio-keys", "", false, false, false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.name, row.phys, row.has_abs);
        CHECK(device_needs_calibration(row.name, row.phys, row.has_abs) == row.needs);
        CHECK(device_supports_calibration(row.name, row.has_abs) == row.supports);
    }
    // The empty, ABS-less device from the auto-fire side.
    CHECK_FALSE(device_needs_calibration("", "", false));
}

TEST_CASE("TouchCalibration: ABS range against the display", "[touch-calibration][abs-mismatch]") {
    struct Row {
        const char* what;
        int abs_x, abs_y, disp_w, disp_h;
        bool mismatch, transposed;
    };
    const Row rows[] = {
        {"exact match", 800, 480, 800, 480, false, false},
        {"within 5%", 832, 480, 800, 480, false, false},
        {"slightly smaller, within 5%", 770, 460, 800, 480, false, false},
        // At exactly 5% the ratio equals the tolerance, which is not past it.
        {"exactly 5%", 840, 504, 800, 480, false, false},
        {"just past 5%", 841, 480, 800, 480, true, false},
        // A Goodix reporting 800x480 on a 480x272 panel (SV06 Ace #123, Nebula Pad).
        {"a panel resolution that is not the display's", 800, 480, 480, 272, true, false},
        {"X only", 1024, 480, 800, 480, true, false},
        {"Y only", 800, 600, 800, 480, true, false},
        {"both axes", 1024, 768, 800, 480, true, false},
        // Generic HID ranges map linearly in LVGL's evdev driver (BTT HDMI5).
        {"generic 4096", 4096, 4096, 800, 480, false, false},
        {"generic 4095", 4095, 4095, 800, 480, false, false},
        {"generic 32767", 32767, 32767, 1024, 600, false, false},
        {"generic 65535", 65535, 65535, 480, 272, false, false},
        {"generic 4095, portrait display", 4095, 4095, 480, 800, false, false},
        {"generic 32767, landscape", 32767, 32767, 800, 480, false, false},
        // Both axes must be generic to skip the check.
        {"generic X, real Y", 4096, 480, 800, 480, true, false},
        {"real X, generic Y", 800, 4096, 800, 480, true, false},
        {"zero ABS X", 0, 480, 800, 480, false, false},
        {"zero ABS Y", 800, 0, 800, 480, false, false},
        {"negative ABS X", -1, 480, 800, 480, false, false},
        {"negative ABS Y", 800, -1, 800, 480, false, false},
        {"zero display width", 800, 480, 0, 480, false, false},
        {"zero display height", 800, 480, 800, 0, false, false},
        {"zero ABS X, portrait", 0, 480, 480, 800, false, false},
        {"zero ABS Y, portrait", 800, 0, 480, 800, false, false},
        {"negative ABS X, portrait", -1, 480, 480, 800, false, false},
        {"negative ABS Y, portrait", 800, -1, 480, 800, false, false},
        {"zero display width, portrait", 800, 480, 0, 800, false, false},
        // Creator 5 Goodix: the range advertised the other way round.
        {"800x480 range on a 480x800 display", 800, 480, 480, 800, true, true},
        {"480x800 range on an 800x480 display", 480, 800, 800, 480, true, true},
        {"transposed within 5%", 810, 470, 480, 800, true, true},
        {"portrait range on a portrait display", 480, 800, 480, 800, false, false},
        // A square display reads the same both ways, so nothing is transposed.
        {"square on square", 480, 480, 480, 480, false, false},
        {"square range missing a square display", 720, 720, 480, 480, true, false},
        {"non-square range on a square display", 800, 480, 480, 480, true, false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        CHECK(has_abs_display_mismatch(row.abs_x, row.abs_y, row.disp_w, row.disp_h) ==
              row.mismatch);
        CHECK(has_transposed_abs_range(row.abs_x, row.abs_y, row.disp_w, row.disp_h) ==
              row.transposed);
    }
    // The transposed check alone with a zero display height on a portrait range.
    CHECK_FALSE(has_transposed_abs_range(800, 480, 480, 0));

    for (int generic : {255, 1023, 4095, 4096, 8191, 16383, 32767, 65535}) {
        CAPTURE(generic);
        CHECK(is_generic_hid_abs_range(generic));
    }
    for (int panel : {800, 480, 1024, 600, 272, 1280}) {
        CAPTURE(panel);
        CHECK_FALSE(is_generic_hid_abs_range(panel));
    }
}

// ============================================================================
// Post-solve validation
// ============================================================================

TEST_CASE("TouchCalibration: validate_calibration_result", "[touch-calibration][validate]") {
    const Triple targets_800 = {{{120, 86}, {400, 408}, {680, 86}}};
    struct Row {
        const char* what;
        Triple screen, touch;
        std::optional<TouchCalibration> cal; // nullopt: solve it
        int width, height;
        bool valid;
    };
    const Row rows[] = {
        {"an exact identity solve", targets_800, targets_800, std::nullopt, 800, 480, true},
        {"a large back-transform residual", targets_800, targets_800,
         TouchCalibration{true, 0.5f, 0, 0, 0, 0.5f, 0, false}, 800, 480, false},
        {"the centre lands off-screen",
         targets_800,
         {{{500, 500}, {2000, 3500}, {3500, 500}}},
         TouchCalibration{true, 1, 0, 5000, 0, 1, 0, false},
         800,
         480,
         false},
        // A real ns2009 on a 480x272 display with a 12-bit ADC.
        {"a real ns2009 fit",
         {{{72, 49}, {240, 231}, {408, 49}}},
         {{{673, 872}, {2007, 3307}, {3342, 872}}},
         TouchCalibration{true, 0.1258f, -0.0025f, -12.63f, -0.0005f, 0.0748f, -16.20f, false},
         480,
         272,
         true},
        // A Mellow FLY-TFT35 whose Goodix emits a compressed sub-range (X ~7..48,
        // Y ~299..313) over the whole 480x320 panel: scale terms near 8.4 and 13.2.
        {"a narrow-range capacitive device",
         {{{72, 64}, {240, 249}, {408, 64}}},
         {{{8, 300}, {28, 313}, {48, 299}}},
         std::nullopt,
         480,
         320,
         true},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        TouchCalibration cal;
        if (row.cal) {
            cal = *row.cal;
        } else {
            compute_calibration(row.screen.data(), row.touch.data(), cal);
        }
        CHECK(validate_calibration_result(cal, row.screen.data(), row.touch.data(), row.width,
                                          row.height) == row.valid);
    }
}

// ============================================================================
// The wizard's sample filtering
// ============================================================================

TEST_CASE("TouchCalibrationPanel: a point needs two clean samples of three",
          "[touch-calibration][filtering]") {
    using State = helix::TouchCalibrationPanel::State;
    struct Row {
        const char* what;
        std::array<Point, 3> samples;
        State after;
        bool failed;
    };
    const Row rows[] = {
        {"three clean", {{{400, 240}, {400, 240}, {400, 240}}}, State::POINT_2, false},
        {"three that differ by a pixel",
         {{{399, 239}, {400, 240}, {401, 241}}},
         State::POINT_2,
         false},
        // X=4095 is the ADC's ceiling.
        {"one saturated", {{{400, 240}, {400, 240}, {4095, 240}}}, State::POINT_2, false},
        {"two saturated", {{{400, 240}, {4095, 3500}, {4095, 3500}}}, State::POINT_1, true},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        helix::TouchCalibrationPanel panel;
        panel.set_screen_size(800, 480);
        bool failed = false;
        panel.set_failure_callback([&](const char*) { failed = true; });
        panel.start();
        REQUIRE(panel.get_state() == State::POINT_1);
        for (const auto& s : row.samples) {
            panel.add_sample(s);
        }
        CHECK(panel.get_state() == row.after);
        CHECK(failed == row.failed);
    }
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

    // Close together but not collinear: the solve succeeds with huge residuals.
    panel.capture_point({100, 100});
    panel.capture_point({102, 100});
    panel.capture_point({100, 102});

    // Should restart to POINT_1 (not enter VERIFY)
    REQUIRE(panel.get_state() == helix::TouchCalibrationPanel::State::POINT_1);
    REQUIRE(failure_called == true);
    REQUIRE(failure_reason.find("unusual") != std::string::npos);
}

// ============================================================================
// sysfs capability parsing
// ============================================================================

TEST_CASE("TouchCalibration: parse_abs_capabilities", "[touch-calibration][capabilities]") {
    // Words arrive most significant first. Single-touch is ABS_X + ABS_Y (bits
    // 0, 1); multitouch is ABS_MT_POSITION_X + _Y (bits 53, 54), in the second
    // 32-bit word or high in one 64-bit word.
    struct Row {
        const char* what;
        const char* abs;
        bool single_touch, multitouch;
    };
    const Row rows[] = {
        {"ABS_X + ABS_Y (sun4i_ts, USB HID)", "3", true, false},
        // Bits 21, 22 of the first word are ABS_HAT0X/Y, not multitouch.
        {"bits 21, 22 of the first word", "600003", true, false},
        {"MT only (Goodix gt9xxnew_ts on a Nebula Pad)", "600000 0", false, true},
        {"both, two words", "600000 3", true, true},
        {"nothing", "0", false, false},
        {"empty", "", false, false},
        {"ABS_X alone", "1", false, false},
        {"ABS_Y alone", "2", false, false},
        // Bits 48, 50, 53, 54, 57 of one 64-bit word (Goodix gt9xx_ts, Allwinner H616).
        {"one 64-bit word", "265000000000000", false, true},
        {"both, GT911", "660000 3", true, true},
        {"three words", "0 600000 3", true, true},
        {"leading zeros", "0000600000 00000003", true, true},
        {"two spaces between words", "600000  3", true, true},
        {"not hex", "xyz", false, false},
        {"ABS_MT_POSITION_X alone", "200000 0", false, false},
        {"ABS_MT_POSITION_Y alone", "400000 0", false, false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const auto caps = parse_abs_capabilities(row.abs);
        CHECK(caps.has_single_touch == row.single_touch);
        CHECK(caps.has_multitouch == row.multitouch);
    }
}

TEST_CASE("TouchCalibration: parse_input_prop_direct", "[touch-calibration][capabilities]") {
    // INPUT_PROP_POINTER is bit 0, DIRECT bit 1, BUTTONPAD bit 2, SEMI_MT bit 3.
    struct Row {
        const char* props;
        bool direct;
    };
    const Row rows[] = {
        {"2", true}, // Creality K2 Plus / K2 Pro Goodix gt9xxnew_ts
        {"1", false},   {"3", true},   {"0", false},   {"", false},
        {"xyz", false}, {"0 2", true}, {"2 0", false}, // the rightmost word is the low bits
        {"4", false},   {"8", false},
    };
    for (const auto& row : rows) {
        CAPTURE(row.props);
        CHECK(parse_input_prop_direct(row.props) == row.direct);
    }
}

// ============================================================================
// Axis transposition
// ============================================================================

TEST_CASE("TouchCalibration: detect_axis_transposition flags only a cross-coupled solve",
          "[touch-calibration][axis-swap]") {
    const Triple targets_800 = {{{120, 96}, {400, 374}, {680, 96}}};
    const Triple targets_480 = {{{72, 54}, {240, 212}, {408, 54}}};
    // About 5 degrees of rotation: some cross-coupling, the diagonal still dominant.
    const auto rotated = [](Point p) {
        return Point{static_cast<int>(p.x * 0.996 + p.y * 0.087),
                     static_cast<int>(-p.x * 0.087 + p.y * 0.996)};
    };
    struct Row {
        const char* what;
        Triple screen, touch;
        bool swapped;
        float min_ratio, max_ratio; // the solve's cross-coupling before detection; -1: no floor
    };
    const Row rows[] = {
        {"800x480, swapped in screen-like units",
         targets_800,
         {{{96, 120}, {374, 400}, {96, 680}}},
         true,
         0.5f,
         1e9f},
        {"800x480, swapped 12-bit ADC",
         targets_800,
         {{{820, 614}, {3190, 2048}, {820, 3482}}},
         true,
         -1.0f,
         1e9f},
        // The Ender 5 Max: the diagonal terms vanish.
        {"Ender 5 Max-like", targets_800, {{{50, 100}, {250, 350}, {50, 600}}}, true, 0.5f, 1e9f},
        // NS2009 with a hardware swap plus channel cross-talk.
        {"480x272, swapped ADC with diagonal leak",
         targets_480,
         {{{728, 830}, {2693, 2896}, {2072, 3698}}},
         true,
         0.3f,
         3.0f},
        {"800x480, a small offset",
         targets_800,
         {{{125, 100}, {405, 378}, {685, 100}}},
         false,
         -1.0f,
         1e9f},
        {"800x480, rotated 5 degrees",
         targets_800,
         {{rotated({120, 96}), rotated({400, 374}), rotated({680, 96})}},
         false,
         -1.0f,
         1e9f},
        {"480x272, well aligned",
         targets_480,
         {{{776, 632}, {2120, 1896}, {3464, 632}}},
         false,
         -1.0f,
         0.3f},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        TouchCalibration cal = solved(row.screen, row.touch);
        const float ratio = cross_coupling(cal);
        CAPTURE(ratio);
        CHECK(ratio > row.min_ratio);
        CHECK(ratio < row.max_ratio);

        CHECK(detect_axis_transposition(cal, row.screen.data(), row.touch.data()) == row.swapped);
        CHECK(cal.axes_swapped == row.swapped);
        // The matrix still lands each tap as reported on its target: the read
        // callback has no swap to apply on the way in.
        for (int i = 0; i < 3; i++) {
            CAPTURE(i);
            const Point p = transform_point(cal, row.touch[i]);
            CHECK(p.x == Approx(row.screen[i].x).margin(2));
            CHECK(p.y == Approx(row.screen[i].y).margin(2));
        }
    }
}

TEST_CASE("TouchCalibration: a transposed panel is exact before and after detection",
          "[touch-calibration][axis-swap]") {
    // A panel mounted a quarter turn from the display: each tap arrives transposed.
    const Triple screen = {{{120, 96}, {400, 374}, {680, 96}}};
    Triple touch;
    for (int i = 0; i < 3; i++) {
        touch[i] = {screen[i].y, screen[i].x};
    }

    TouchCalibration cal = solved(screen, touch);
    const auto lands_on_targets = [&]() {
        for (int i = 0; i < 3; i++) {
            CAPTURE(i);
            const Point out = transform_point(cal, touch[i]);
            CHECK(out.x == Approx(screen[i].x).margin(2));
            CHECK(out.y == Approx(screen[i].y).margin(2));
        }
    };
    // An affine through three points is exact: the cross terms carry the turn.
    lands_on_targets();
    detect_axis_transposition(cal, screen.data(), touch.data());
    lands_on_targets();
}

// ============================================================================
// Stored calibrations from older builds
// ============================================================================

TEST_CASE("TouchCalibration: should_invalidate_unstamped_calibration truth table",
          "[touch-calibration][rotation][provenance][1394]") {
    // The only combination that discards: no recorded basis, and a rotation that
    // makes the assumed zero a guess.
    REQUIRE(should_invalidate_unstamped_calibration(/*has_capture_rotation*/ false,
                                                    /*applied_rotation*/ 270));
    REQUIRE(should_invalidate_unstamped_calibration(false, 90));
    REQUIRE(should_invalidate_unstamped_calibration(false, 180));

    // Unrotated: zero is what the record meant, so it is left completely alone.
    REQUIRE_FALSE(should_invalidate_unstamped_calibration(false, 0));

    // A record that names its own basis is placeable at any rotation.
    REQUIRE_FALSE(should_invalidate_unstamped_calibration(true, 270));
    REQUIRE_FALSE(should_invalidate_unstamped_calibration(true, 0));
}

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
// The calibrated read path keeps its CalibrationContext in its own static
// storage, so a stale or corrupted indev user_data slot cannot reach the
// transform (prestonbrown/helixscreen#1112).

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

    // Poison user_data with non-null garbage (#1112): the callback must not read it.
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

// ============================================================================
// Shipped platform defaults
// ============================================================================
//
// A printer-embedded package ships for exactly one panel. Where we have measured
// that panel, the coefficients ride along so touch works before the user has
// calibrated anything.
//
// The fits are checked here as DATA, not through platform_default_calibration(),
// because that function compiles down to whichever platform this test binary was
// built for — on a desktop build it returns {valid=false} and a test written
// against it would assert nothing at all.

namespace {

struct ShippedFit {
    const char* name;
    helix::TouchCalibration cal;
};

// Keep in sync with platform_default_calibration() in src/ui/touch_calibration.cpp.
const ShippedFit kShippedFits[] = {
    {"ad5x",
     {true, 1.171731f, -0.043628f, -66.965828f, -0.006188f, 1.478954f, -118.906227f, false}},
    {"ad5m", {true, 1.174004f, 0.009220f, -79.064400f, -0.000000f, -1.455497f, 570.492126f, false}},
};

} // namespace

TEST_CASE("shipped panel fits validate, span a sane raw range and keep both edges on the "
          "digitizer",
          "[touch][calibration]") {
    for (const auto& fit : kShippedFits) {
        CAPTURE(fit.name);
        CHECK(helix::is_calibration_valid(fit.cal));
        const float x_span = 800.0f / std::fabs(fit.cal.a);
        const float y_span = 480.0f / std::fabs(fit.cal.e);
        CHECK(x_span > 200.0f);
        CHECK(x_span < 4096.0f);
        CHECK(y_span > 100.0f);
        CHECK(y_span < 4096.0f);
        // Inverting the fit for screen y=0 and y=480 must give non-negative raw
        // values. This is what catches a sign error in e or a wrong f.
        const float raw_top = (0.0f - fit.cal.f) / fit.cal.e;
        const float raw_bottom = (480.0f - fit.cal.f) / fit.cal.e;
        CHECK(raw_top >= 0.0f);
        CHECK(raw_bottom >= 0.0f);
    }
}

// The AD5X and AD5M ship the same physical panel; the AD5M reports Y inverted.
// If a future re-measurement breaks that relationship, one of the two fits is
// probably wrong, because they cross-validate each other.
TEST_CASE("shipped AD5X and AD5M fits describe the same panel", "[touch][calibration]") {
    const helix::TouchCalibration& x = kShippedFits[0].cal;
    const helix::TouchCalibration& m = kShippedFits[1].cal;

    CHECK((x.e > 0.0f) != (m.e > 0.0f)); // opposite Y polarity

    const float x_span_a = 800.0f / std::fabs(x.a);
    const float x_span_b = 800.0f / std::fabs(m.a);
    CHECK(std::fabs(x_span_a - x_span_b) / x_span_b < 0.05f);

    const float y_span_a = 480.0f / std::fabs(x.e);
    const float y_span_b = 480.0f / std::fabs(m.e);
    CHECK(std::fabs(y_span_a - y_span_b) / y_span_b < 0.05f);
}

namespace {

void check_fit_equals(const helix::TouchCalibration& cal, const helix::TouchCalibration& expected) {
    CHECK(cal.valid);
    CHECK(cal.a == Catch::Approx(expected.a));
    CHECK(cal.b == Catch::Approx(expected.b));
    CHECK(cal.c == Catch::Approx(expected.c));
    CHECK(cal.d == Catch::Approx(expected.d).margin(1e-6));
    CHECK(cal.e == Catch::Approx(expected.e));
    CHECK(cal.f == Catch::Approx(expected.f));
}

} // namespace

// Only the AD5X and AD5M have a measured panel. The Creator 5's digitizer already spans
// its panel, so the AD5X fit would throw its taps off-screen; every other board runs the
// calibration wizard on first use.
TEST_CASE("factory_touch_fit ships fits for the AD5X and AD5M only", "[touch][calibration]") {
    using helix::board::Board;
    const auto ad5x = helix::factory_touch_fit(Board::Ad5x);
    const auto ad5m = helix::factory_touch_fit(Board::Ad5m);
    REQUIRE(ad5x.has_value());
    REQUIRE(ad5m.has_value());
    check_fit_equals(*ad5x, kShippedFits[0].cal);
    check_fit_equals(*ad5m, kShippedFits[1].cal);

    for (Board b : {Board::Creator5, Board::Creator5Pro, Board::K1, Board::K2, Board::Cc1,
                    Board::SnapmakerU1, Board::Generic}) {
        INFO("board: " << helix::board::info(b).key);
        CHECK_FALSE(helix::factory_touch_fit(b).has_value());
    }
}

TEST_CASE("platform_default_calibration follows the board", "[touch][calibration]") {
    {
        helix::test::ScopedBoard board(helix::board::Board::Ad5x);
        const helix::TouchCalibration cal = helix::platform_default_calibration();
        REQUIRE(cal.valid);
        CHECK(helix::is_calibration_valid(cal));
        check_fit_equals(cal, kShippedFits[0].cal);
    }
    {
        helix::test::ScopedBoard board(helix::board::Board::Creator5);
        CHECK_FALSE(helix::platform_default_calibration().valid);
    }
}
