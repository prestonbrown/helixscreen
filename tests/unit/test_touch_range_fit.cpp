// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
//
// Solving the evdev stage from a three-point calibration
// (prestonbrown/helixscreen#1259, #1276).
//
// The touch pipeline has two stages before LVGL sees a coordinate:
//
//   kernel raw -> lv_evdev [swap, scale by the DECLARED ABS range, CLAMP to the
//                display] -> helix::calibrated_read_cb [affine a..f, clamp]
//
// The clamp in the first stage is what makes a wrong declared range unfixable
// from the second: coordinates arrive compressed AND flattened at the edges, and
// an affine cannot un-flatten them. compute_range_fit() therefore solves the
// first stage directly, and hands back whatever rotation or shear an axis-aligned
// range cannot express as a residual affine for the second stage to carry.
//
// Every test here models the real evdev arithmetic (integer, clamped) rather than
// asserting only on the recovered numbers, because reproducing the targets
// through the actual pipeline shape is the property that matters.

#include "config.h"
#include "lvgl_test_fixture.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_session.h"
#include "touch_calibration_wrapper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix;

namespace {

/// lv_evdev's _evdev_calibrate(), verbatim: integer scale, unconditional clamp.
/// Note that min == max is NOT a passthrough - the scale is skipped but the clamp
/// is not, which is why a degenerate range collapses the whole panel onto a pixel.
int evdev_calibrate(int v, int in_min, int in_max, int out_min, int out_max) {
    if (in_min != in_max) {
        v = (v - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    }
    return std::max(out_min, std::min(v, out_max));
}

/// The full first stage: swap, then scale each axis onto [0, res-1].
Point evdev_stage(const TouchRangeFit& fit, Point raw, int w, int h) {
    const int sx = fit.swap_axes ? raw.y : raw.x;
    const int sy = fit.swap_axes ? raw.x : raw.y;
    return Point{evdev_calibrate(sx, fit.min_x, fit.max_x, 0, w - 1),
                 evdev_calibrate(sy, fit.min_y, fit.max_y, 0, h - 1)};
}

/// Both stages, as the device would run them after a commit.
Point full_pipeline(const TouchRangeFit& fit, Point raw, int w, int h) {
    Point ev = evdev_stage(fit, raw, w, h);
    if (!fit.residual.valid) {
        return ev;
    }
    return transform_point(fit.residual, ev, w - 1, h - 1);
}

/// The three targets TouchCalibrationPanel puts on screen, at the same ratios.
void targets(int w, int h, Point out[3]) {
    out[0] = Point{static_cast<int>(w * 0.15f), static_cast<int>(h * 0.20f)};
    out[1] = Point{static_cast<int>(w * 0.50f), static_cast<int>(h * 0.78f)};
    out[2] = Point{static_cast<int>(w * 0.85f), static_cast<int>(h * 0.20f)};
}

int round_to_int(double v) {
    return static_cast<int>(std::lround(v));
}

/// Invert a linear axis map: which raw value lands on this display pixel?
int raw_for_pixel(int pixel, int out_span, int min_v, int max_v) {
    return round_to_int(static_cast<double>(min_v) +
                        static_cast<double>(pixel) * (max_v - min_v) / out_span);
}

} // namespace

// ============================================================================
// A panel that already reports in screen space
// ============================================================================

TEST_CASE("compute_range_fit: identity panel recovers the display range",
          "[touch][touch-calibration][range-fit]") {
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    // The digitizer already emits display coordinates, so raw == screen.
    Point raw[3] = {screen[0], screen[1], screen[2]};

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);

    REQUIRE(fit.valid);
    CHECK_FALSE(fit.swap_axes);
    CHECK(fit.min_x == 0);
    CHECK(fit.max_x == W - 1);
    CHECK(fit.min_y == 0);
    CHECK(fit.max_y == H - 1);
    CHECK(fit.residual_px == Approx(0.0f).margin(0.01));

    // Nothing left for the affine stage to do: a square panel is exactly what the
    // range alone can express, and leaving the affine switched off keeps it out of
    // the pipeline entirely.
    CHECK_FALSE(fit.residual.valid);

    for (int i = 0; i < 3; i++) {
        Point mapped = full_pipeline(fit, raw[i], W, H);
        CHECK(mapped.x == screen[i].x);
        CHECK(mapped.y == screen[i].y);
    }
}

// ============================================================================
// #1259: transposed axes with one of them inverted (T1 Pro class)
// ============================================================================
//
// The panel is wired so raw Y drives screen X (13 -> 0, 470 -> 479) and raw X
// drives screen Y INVERTED (243 -> 0, 16 -> 271), on a 480x272 display. That is
// exactly the shape lv_evdev can express and the affine stage cannot rescue,
// because the declared range clamps before the affine ever runs.

TEST_CASE("compute_range_fit: T1 Pro transposed panel with an inverted axis",
          "[touch][touch-calibration][range-fit][1259]") {
    constexpr int W = 480;
    constexpr int H = 272;
    // Ground truth: what the digitizer really emits at each display edge.
    constexpr int TRUE_MIN_X = 13;  // raw Y at screen x = 0
    constexpr int TRUE_MAX_X = 470; // raw Y at screen x = W-1
    constexpr int TRUE_MIN_Y = 243; // raw X at screen y = 0
    constexpr int TRUE_MAX_Y = 16;  // raw X at screen y = H-1  (inverted)

    Point screen[3];
    targets(W, H, screen);

    Point raw[3];
    for (int i = 0; i < 3; i++) {
        // Screen X is fed by raw Y, screen Y by raw X.
        raw[i].y = raw_for_pixel(screen[i].x, W - 1, TRUE_MIN_X, TRUE_MAX_X);
        raw[i].x = raw_for_pixel(screen[i].y, H - 1, TRUE_MIN_Y, TRUE_MAX_Y);
    }

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);

    REQUIRE(fit.valid);
    INFO("recovered swap=" << fit.swap_axes << " X(" << fit.min_x << ".." << fit.max_x << ") Y("
                           << fit.min_y << ".." << fit.max_y << ")");
    CHECK(fit.swap_axes);

    // Rounding the three raw captures to integers perturbs the fit by a raw unit
    // or two; the range must land on the hardware, not on the exact constants.
    CHECK(std::abs(fit.min_x - TRUE_MIN_X) <= 3);
    CHECK(std::abs(fit.max_x - TRUE_MAX_X) <= 3);
    CHECK(std::abs(fit.min_y - TRUE_MIN_Y) <= 3);
    CHECK(std::abs(fit.max_y - TRUE_MAX_Y) <= 3);

    // The inverted axis MUST come back as max < min. That is not a bug to correct:
    // lv_evdev's negative denominator is exactly how an axis gets inverted, and
    // "fixing" the ordering would flip the panel upside down.
    CHECK(fit.max_y < fit.min_y);

    // Axis-aligned, so the affine stage stays out of it.
    CHECK(fit.residual_px == Approx(0.0f).margin(0.5));
    CHECK_FALSE(fit.residual.valid);

    // The whole point: run the three captures back through the pipeline the commit
    // installs and land on the targets.
    for (int i = 0; i < 3; i++) {
        Point mapped = full_pipeline(fit, raw[i], W, H);
        INFO("point " << i << " raw(" << raw[i].x << "," << raw[i].y << ") -> (" << mapped.x << ","
                      << mapped.y << ") want (" << screen[i].x << "," << screen[i].y << ")");
        CHECK(std::abs(mapped.x - screen[i].x) <= 2);
        CHECK(std::abs(mapped.y - screen[i].y) <= 2);
    }
}

TEST_CASE("compute_range_fit: T1 Pro range reaches both display edges",
          "[touch][touch-calibration][range-fit][1259]") {
    // The failure #1259 reports is that part of the panel is unreachable: the
    // declared range over-covers what the digitizer emits, so a finger at the far
    // edge never produces a coordinate past some fraction of the screen. With the
    // solved range, the extremes of the emitted range must map to the extremes of
    // the display.
    constexpr int W = 480;
    constexpr int H = 272;
    constexpr int TRUE_MIN_X = 13, TRUE_MAX_X = 470, TRUE_MIN_Y = 243, TRUE_MAX_Y = 16;

    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    for (int i = 0; i < 3; i++) {
        raw[i].y = raw_for_pixel(screen[i].x, W - 1, TRUE_MIN_X, TRUE_MAX_X);
        raw[i].x = raw_for_pixel(screen[i].y, H - 1, TRUE_MIN_Y, TRUE_MAX_Y);
    }

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    REQUIRE(fit.valid);

    Point top_left = full_pipeline(fit, Point{TRUE_MIN_Y, TRUE_MIN_X}, W, H);
    Point bottom_right = full_pipeline(fit, Point{TRUE_MAX_Y, TRUE_MAX_X}, W, H);

    // Within a few pixels of each corner. The three captures are integers, so the
    // recovered endpoints carry a raw unit or two of rounding; what matters is
    // that the extremes reach the extremes instead of stopping a third of the way
    // in, which is the #1259 symptom.
    INFO("top-left -> (" << top_left.x << "," << top_left.y << "), bottom-right -> ("
                         << bottom_right.x << "," << bottom_right.y << ")");
    CHECK(top_left.x <= 4);
    CHECK(top_left.y <= 4);
    CHECK(bottom_right.x >= W - 5);
    CHECK(bottom_right.y >= H - 5);
}

// ============================================================================
// #1276: the digitizer emits a narrow slice of the range it declares
// ============================================================================

TEST_CASE("compute_range_fit: compressed emitted range (SV06 Ace class)",
          "[touch][touch-calibration][range-fit][1276]") {
    constexpr int W = 800;
    constexpr int H = 480;
    // The driver declares 0..4095 on both axes; the panel only ever emits these.
    constexpr int TRUE_MIN_X = 900, TRUE_MAX_X = 3200;
    constexpr int TRUE_MIN_Y = 700, TRUE_MAX_Y = 3800;

    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    for (int i = 0; i < 3; i++) {
        raw[i].x = raw_for_pixel(screen[i].x, W - 1, TRUE_MIN_X, TRUE_MAX_X);
        raw[i].y = raw_for_pixel(screen[i].y, H - 1, TRUE_MIN_Y, TRUE_MAX_Y);
    }

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);

    REQUIRE(fit.valid);
    CHECK_FALSE(fit.swap_axes);
    INFO("recovered X(" << fit.min_x << ".." << fit.max_x << ") Y(" << fit.min_y << ".."
                        << fit.max_y << ")");
    CHECK(std::abs(fit.min_x - TRUE_MIN_X) <= 4);
    CHECK(std::abs(fit.max_x - TRUE_MAX_X) <= 4);
    CHECK(std::abs(fit.min_y - TRUE_MIN_Y) <= 4);
    CHECK(std::abs(fit.max_y - TRUE_MAX_Y) <= 4);
    CHECK_FALSE(fit.residual.valid);

    // Under the DECLARED 0..4095 range the same taps compress toward the top-left
    // and the panel's own edges never reach the display's - the reported symptom.
    // Under the solved range they do.
    TouchRangeFit declared{};
    declared.valid = true;
    declared.min_x = 0;
    declared.max_x = 4095;
    declared.min_y = 0;
    declared.max_y = 4095;
    Point declared_far = evdev_stage(declared, Point{TRUE_MAX_X, TRUE_MAX_Y}, W, H);
    INFO("far corner under the declared range -> (" << declared_far.x << "," << declared_far.y
                                                    << ")");
    CHECK(declared_far.x < W - 100);

    Point solved_far = full_pipeline(fit, Point{TRUE_MAX_X, TRUE_MAX_Y}, W, H);
    CHECK(solved_far.x >= W - 3);
    CHECK(solved_far.y >= H - 3);
}

// ============================================================================
// Rotation and shear: what the axis-aligned range cannot carry
// ============================================================================

namespace {

/// Rotate the three targets by `degrees` to synthesise a panel mounted crooked,
/// so `raw` needs de-rotating to land back on `screen`.
void rotated_raw(const Point screen[3], double degrees, Point raw[3]) {
    const double theta = degrees * 3.14159265358979 / 180.0;
    for (int i = 0; i < 3; i++) {
        const double sx = screen[i].x;
        const double sy = screen[i].y;
        raw[i].x = round_to_int(sx * std::cos(theta) - sy * std::sin(theta));
        raw[i].y = round_to_int(sx * std::sin(theta) + sy * std::cos(theta));
    }
}

} // namespace

TEST_CASE("compute_range_fit: a slightly crooked panel keeps the affine stage",
          "[touch][touch-calibration][range-fit]") {
    // Two degrees is the scale of skew a laminated panel actually has. The range
    // cannot express it, so the residual affine must be kept and must carry it.
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    rotated_raw(screen, 2.0, raw);

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);

    REQUIRE(fit.valid);
    CHECK_FALSE(fit.swap_axes);

    INFO("residual_px=" << fit.residual_px);
    CHECK(fit.residual_px > 10.0f);
    REQUIRE(fit.residual.valid);

    for (int i = 0; i < 3; i++) {
        Point mapped = full_pipeline(fit, raw[i], W, H);
        INFO("point " << i << " -> (" << mapped.x << "," << mapped.y << ") want (" << screen[i].x
                      << "," << screen[i].y << ")");
        CHECK(std::abs(mapped.x - screen[i].x) <= 4);
        CHECK(std::abs(mapped.y - screen[i].y) <= 4);
    }
}

TEST_CASE("compute_range_fit: a panel mounted at a real angle is declined",
          "[touch][touch-calibration][range-fit][edge]") {
    // At fifteen degrees the axis-aligned range would cost a wide band of reach
    // along every edge, because lv_evdev clamps before the residual affine runs.
    // The affine alone has no clamp in front of it and handles the shape properly,
    // so this case must stay exactly where it was.
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    rotated_raw(screen, 15.0, raw);

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    CHECK_FALSE(fit.valid);

    // ...and the affine that would carry it is still perfectly computable, which
    // is the fallback the panel keeps.
    TouchCalibration affine;
    REQUIRE(compute_calibration(screen, raw, affine));
    CHECK(affine.valid);
}

TEST_CASE("compute_range_fit: a sub-pixel residual is left switched off",
          "[touch][touch-calibration][range-fit]") {
    // A whisker of shear, far below a pixel across the panel. Storing an affine
    // for that would put a whole extra stage in the pipeline to move nothing.
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    for (int i = 0; i < 3; i++) {
        raw[i].x = screen[i].x;
        raw[i].y = screen[i].y;
    }
    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    REQUIRE(fit.valid);
    CHECK(fit.residual_px < 0.5f);
    CHECK_FALSE(fit.residual.valid);
}

// ============================================================================
// Degenerate inputs must decline, not guess
// ============================================================================

TEST_CASE("compute_range_fit: collinear raw points yield no fit",
          "[touch][touch-calibration][range-fit][edge]") {
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3] = {{100, 100}, {200, 200}, {300, 300}};

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    CHECK_FALSE(fit.valid);
}

TEST_CASE("compute_range_fit: duplicate raw points yield no fit",
          "[touch][touch-calibration][range-fit][edge]") {
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3] = {{500, 500}, {500, 500}, {700, 300}};

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    CHECK_FALSE(fit.valid);
}

TEST_CASE("compute_range_fit: a near-zero slope is not a range",
          "[touch][touch-calibration][range-fit][edge]") {
    // A digitizer whose readings barely move across the whole panel implies a span
    // of millions of raw units. Nothing emits that, and lv_evdev's integer scale
    // would overflow long before it got there.
    constexpr int W = 800;
    constexpr int H = 480;
    Point screen[3];
    targets(W, H, screen);
    Point raw[3];
    for (int i = 0; i < 3; i++) {
        raw[i].x = round_to_int(screen[i].x * 6000.0);
        raw[i].y = round_to_int(screen[i].y * 6000.0);
    }

    TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
    INFO("recovered X(" << fit.min_x << ".." << fit.max_x << ")");
    CHECK_FALSE(fit.valid);
}

TEST_CASE("compute_range_fit: a degenerate display size yields no fit",
          "[touch][touch-calibration][range-fit][edge]") {
    Point screen[3] = {{0, 0}, {0, 0}, {0, 0}};
    Point raw[3] = {{0, 0}, {100, 0}, {0, 100}};
    CHECK_FALSE(compute_range_fit(screen, raw, 1, 1).valid);
    CHECK_FALSE(compute_range_fit(screen, raw, 0, 480).valid);
}

// ============================================================================
// The solved range never collapses the panel
// ============================================================================

TEST_CASE("compute_range_fit: a valid fit never returns min == max on an axis",
          "[touch][touch-calibration][range-fit][edge]") {
    // lv_evdev skips the scale when min == max but still clamps, which pins every
    // touch to a single pixel - a dead panel. No fit may ever produce that.
    constexpr int W = 480;
    constexpr int H = 272;
    Point screen[3];
    targets(W, H, screen);

    const std::vector<std::pair<int, int>> spans = {{0, 479}, {900, 3200}, {243, 16}, {4095, 0}};
    for (const auto& sx : spans) {
        for (const auto& sy : spans) {
            Point raw[3];
            for (int i = 0; i < 3; i++) {
                raw[i].x = raw_for_pixel(screen[i].x, W - 1, sx.first, sx.second);
                raw[i].y = raw_for_pixel(screen[i].y, H - 1, sy.first, sy.second);
            }
            TouchRangeFit fit = compute_range_fit(screen, raw, W, H);
            if (!fit.valid) {
                continue;
            }
            INFO("span x(" << sx.first << ".." << sx.second << ") y(" << sy.first << ".."
                           << sy.second << ")");
            CHECK(fit.min_x != fit.max_x);
            CHECK(fit.min_y != fit.max_y);
        }
    }
}

// ============================================================================
// Panel plumbing: the raw reading has to survive the capture path
// ============================================================================
//
// Modelled on the #1276 shape: the driver declares 0..4095 on both axes, the
// glass only emits 900..3200 / 700..3800, so what the panel captures (affine
// disabled, but the evdev stage still running) is the compressed coordinate,
// while the digitizer reading behind it is the honest one.

namespace {

constexpr int PANEL_W = 800;
constexpr int PANEL_H = 480;
constexpr int EMIT_MIN_X = 900, EMIT_MAX_X = 3200;
constexpr int EMIT_MIN_Y = 700, EMIT_MAX_Y = 3800;
constexpr int DECLARED_MAX = 4095;

void compressed_capture(Point screen, Point& out_touch, Point& out_raw) {
    out_raw.x = raw_for_pixel(screen.x, PANEL_W - 1, EMIT_MIN_X, EMIT_MAX_X);
    out_raw.y = raw_for_pixel(screen.y, PANEL_H - 1, EMIT_MIN_Y, EMIT_MAX_Y);
    // What lv_evdev hands us under the wrong declared range.
    out_touch.x = evdev_calibrate(out_raw.x, 0, DECLARED_MAX, 0, PANEL_W - 1);
    out_touch.y = evdev_calibrate(out_raw.y, 0, DECLARED_MAX, 0, PANEL_H - 1);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "TouchCalibrationPanel: raw readings reach the range solver",
                 "[touch][touch-calibration][range-fit][1276]") {
    TouchCalibrationPanel panel;
    panel.set_screen_size(PANEL_W, PANEL_H);
    panel.start();

    for (int step = 0; step < 3; step++) {
        Point target = panel.get_target_position(step);
        Point touch, raw;
        compressed_capture(target, touch, raw);
        for (int s = 0; s < 3; s++) {
            panel.add_sample(touch, &raw);
        }
    }

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    const TouchCalibration* cal = panel.get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid);

    const TouchRangeFit& fit = panel.get_range_fit();
    REQUIRE(fit.valid);
    INFO("recovered X(" << fit.min_x << ".." << fit.max_x << ") Y(" << fit.min_y << ".."
                        << fit.max_y << ")");
    CHECK_FALSE(fit.swap_axes);
    CHECK(std::abs(fit.min_x - EMIT_MIN_X) <= 10);
    CHECK(std::abs(fit.max_x - EMIT_MAX_X) <= 10);
    CHECK(std::abs(fit.min_y - EMIT_MIN_Y) <= 10);
    CHECK(std::abs(fit.max_y - EMIT_MAX_Y) <= 10);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "TouchCalibrationPanel: no raw source degrades to the affine-only result",
                 "[touch][touch-calibration][range-fit]") {
    // The safety property for every device that cannot supply a raw reading
    // (desktop, libinput, an older kernel driver): the affine that comes out must
    // be bit-for-bit what it was before any of this existed, and no range may be
    // offered for persistence.
    TouchCalibration with_raw{};
    TouchCalibration without_raw{};

    {
        TouchCalibrationPanel panel;
        panel.set_screen_size(PANEL_W, PANEL_H);
        panel.start();
        for (int step = 0; step < 3; step++) {
            Point touch, raw;
            compressed_capture(panel.get_target_position(step), touch, raw);
            for (int s = 0; s < 3; s++) {
                panel.add_sample(touch, &raw);
            }
        }
        REQUIRE(panel.get_calibration() != nullptr);
        with_raw = *panel.get_calibration();
        REQUIRE(panel.get_range_fit().valid);
    }

    {
        TouchCalibrationPanel panel;
        panel.set_screen_size(PANEL_W, PANEL_H);
        panel.start();
        for (int step = 0; step < 3; step++) {
            Point touch, raw;
            compressed_capture(panel.get_target_position(step), touch, raw);
            for (int s = 0; s < 3; s++) {
                panel.add_sample(touch); // no digitizer reading available
            }
        }
        REQUIRE(panel.get_calibration() != nullptr);
        without_raw = *panel.get_calibration();
        CHECK_FALSE(panel.get_range_fit().valid);
    }

    CHECK(without_raw.a == with_raw.a);
    CHECK(without_raw.b == with_raw.b);
    CHECK(without_raw.c == with_raw.c);
    CHECK(without_raw.d == with_raw.d);
    CHECK(without_raw.e == with_raw.e);
    CHECK(without_raw.f == with_raw.f);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "TouchCalibrationPanel: one point without a raw reading vetoes the range fit",
                 "[touch][touch-calibration][range-fit][edge]") {
    // A fit needs all three. Solving from two real readings and one stale one
    // would produce a plausible-looking range that maps the panel wrongly, which
    // is worse than not solving at all.
    TouchCalibrationPanel panel;
    panel.set_screen_size(PANEL_W, PANEL_H);
    panel.start();

    for (int step = 0; step < 3; step++) {
        Point touch, raw;
        compressed_capture(panel.get_target_position(step), touch, raw);
        for (int s = 0; s < 3; s++) {
            panel.add_sample(touch, step == 1 ? nullptr : &raw);
        }
    }

    REQUIRE(panel.get_state() == TouchCalibrationPanel::State::VERIFY);
    CHECK(panel.get_calibration() != nullptr);
    CHECK_FALSE(panel.get_range_fit().valid);
}

// ============================================================================
// The read-callback seam that supplies those raw readings
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "calibrated_read_cb: stashes the raw reading from raw_source",
                 "[touch][touch-calibration][range-fit][wrapper]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    TouchCalibration cal;
    cal.valid = true;
    cal.a = 1.0f, cal.b = 0.0f, cal.c = 100.0f;
    cal.d = 0.0f, cal.e = 1.0f, cal.f = 50.0f;

    CalibrationContext ctx;
    install_calibration_wrapper(indev, ctx, cal, 800, 480);

    // Nothing offered yet: the affine-only path must see no raw reading at all,
    // not a zero-initialised one that would look like a corner tap.
    Point probe{-1, -1};
    CHECK_FALSE(get_last_raw_touch(probe));
    CHECK(probe.x == -1);

    int served = 0;
    ctx.raw_source = [&served](int& x, int& y) {
        served++;
        x = 2733;
        y = 1904;
        return true;
    };

    lv_indev_data_t data{};
    data.point.x = 10;
    data.point.y = 20;
    data.state = LV_INDEV_STATE_PRESSED;
    calibrated_read_cb(indev, &data);

    CHECK(served == 1);
    Point raw{};
    REQUIRE(get_last_raw_touch(raw));
    CHECK(raw.x == 2733);
    CHECK(raw.y == 1904);

    // The affine still runs on the mapped coordinate - reading the raw value must
    // not change what LVGL is handed.
    const Point expected = transform_point(cal, {10, 20}, 799, 479);
    CHECK(data.point.x == expected.x);
    CHECK(data.point.y == expected.y);

    // A source that reports "no reading" leaves the last good one alone rather
    // than clearing it: the press edge and the read that carried it are separate
    // callbacks, and a gap between them must not blank the capture.
    ctx.raw_source = [](int&, int&) { return false; };
    lv_indev_data_t data2{};
    data2.point.x = 11;
    data2.point.y = 21;
    calibrated_read_cb(indev, &data2);
    Point raw2{};
    REQUIRE(get_last_raw_touch(raw2));
    CHECK(raw2.x == 2733);

    uninstall_calibration_wrapper(indev, ctx);
    Point after{};
    CHECK_FALSE(get_last_raw_touch(after));

    lv_indev_delete(indev);
}

// ============================================================================
// Committing: exactly one of the two shapes, never both
// ============================================================================

namespace {

struct RangeFakeSink : ICalibrationSink {
    TouchCalibration stored{};
    bool range_accepted = true;
    bool range_called = false;
    bool cleared = false;
    int applied_count = 0;
    bool last_swap = false;
    int last_min_x = 0, last_max_x = 0, last_min_y = 0, last_max_y = 0;

    TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const TouchCalibration& cal) override {
        if (!cal.valid) {
            return false;
        }
        stored = cal;
        applied_count++;
        return true;
    }
    void disable_affine() override {}
    void enable_affine() override {}
    void clear_calibration() override {
        stored = TouchCalibration{};
        cleared = true;
    }
    bool apply_touch_range(bool swap, int min_x, int min_y, int max_x, int max_y) override {
        range_called = true;
        last_swap = swap;
        last_min_x = min_x;
        last_min_y = min_y;
        last_max_x = max_x;
        last_max_y = max_y;
        return range_accepted;
    }
};

TouchCalibration some_affine() {
    TouchCalibration c{};
    c.valid = true;
    c.a = 1.7f;
    c.b = 0.0f;
    c.c = -3.0f;
    c.d = 0.0f;
    c.e = 1.6f;
    c.f = -2.0f;
    return c;
}

/// Leave the shared Config singleton the way a fresh one looks for these keys.
void reset_stored_calibration_keys() {
    if (Config* cfg = Config::get_instance()) {
        cfg->set<bool>("/input/calibration/valid", false);
        cfg->set<bool>("/input/touch_range/valid", false);
    }
}

} // namespace

TEST_CASE("commit_calibration_result: a solved range replaces the full affine",
          "[touch][touch-calibration][range-fit][commit]") {
    RangeFakeSink sink;
    TouchRangeFit fit{};
    fit.valid = true;
    fit.swap_axes = true;
    fit.min_x = 13;
    fit.max_x = 470;
    fit.min_y = 243;
    fit.max_y = 16;
    fit.residual_px = 0.1f;
    fit.residual.valid = false; // axis-aligned: nothing left for the affine stage

    CHECK(commit_calibration_result(&sink, some_affine(), fit));

    CHECK(sink.range_called);
    CHECK(sink.last_swap);
    CHECK(sink.last_min_x == 13);
    CHECK(sink.last_max_x == 470);
    CHECK(sink.last_min_y == 243);
    CHECK(sink.last_max_y == 16);

    // The range carries the whole mapping, so the device's affine slot must be
    // emptied rather than left holding the pre-session matrix.
    CHECK(sink.cleared);
    CHECK(sink.applied_count == 0);

    if (Config* cfg = Config::get_instance()) {
        CHECK(cfg->get<bool>("/input/touch_range/valid", false));
        CHECK(cfg->get<bool>("/input/touch_range/swap_axes", false));
        CHECK(cfg->get<int>("/input/touch_range/min_y", 0) == 243);
        CHECK(cfg->get<int>("/input/touch_range/max_y", 0) == 16);
        CHECK_FALSE(cfg->get<bool>("/input/calibration/valid", true));
    }
    reset_stored_calibration_keys();
}

TEST_CASE("commit_calibration_result: a rotated panel keeps the residual affine",
          "[touch][touch-calibration][range-fit][commit]") {
    RangeFakeSink sink;
    TouchRangeFit fit{};
    fit.valid = true;
    fit.min_x = 0;
    fit.max_x = 799;
    fit.min_y = 0;
    fit.max_y = 479;
    fit.residual_px = 40.0f;
    fit.residual = some_affine();

    CHECK(commit_calibration_result(&sink, some_affine(), fit));
    CHECK(sink.range_called);
    CHECK(sink.applied_count == 1);
    CHECK_FALSE(sink.cleared);

    if (Config* cfg = Config::get_instance()) {
        CHECK(cfg->get<bool>("/input/touch_range/valid", false));
        CHECK(cfg->get<bool>("/input/calibration/valid", false));
        CHECK(cfg->get<double>("/input/calibration/a", 0.0) == Approx(1.7));
    }
    reset_stored_calibration_keys();
}

TEST_CASE("commit_calibration_result: no fit persists the full affine and clears the range",
          "[touch][touch-calibration][range-fit][commit]") {
    // The pre-#1259 path, and the one every non-evdev install stays on. A range
    // left over from an earlier calibration must be cleared here: stacked under a
    // full-pipeline affine it would apply both stages.
    if (Config* cfg = Config::get_instance()) {
        cfg->set<bool>("/input/touch_range/valid", true);
        cfg->set<int>("/input/touch_range/min_x", 111);
    }

    RangeFakeSink sink;
    TouchRangeFit fit{}; // valid == false

    CHECK(commit_calibration_result(&sink, some_affine(), fit));
    CHECK_FALSE(sink.range_called);
    CHECK(sink.applied_count == 1);
    CHECK(sink.stored.a == Approx(1.7f));

    if (Config* cfg = Config::get_instance()) {
        CHECK_FALSE(cfg->get<bool>("/input/touch_range/valid", true));
        CHECK(cfg->get<bool>("/input/calibration/valid", false));
    }
    reset_stored_calibration_keys();
}

TEST_CASE("commit_calibration_result: a backend that refuses the range falls back to the affine",
          "[touch][touch-calibration][range-fit][commit]") {
    // A solved range that the device will not install must not be persisted:
    // next boot would program nothing and the residual affine alone would map the
    // panel wrongly.
    RangeFakeSink sink;
    sink.range_accepted = false;
    TouchRangeFit fit{};
    fit.valid = true;
    fit.min_x = 900;
    fit.max_x = 3200;
    fit.min_y = 700;
    fit.max_y = 3800;

    CHECK(commit_calibration_result(&sink, some_affine(), fit));
    CHECK(sink.range_called);
    CHECK(sink.applied_count == 1);
    CHECK(sink.stored.a == Approx(1.7f));

    if (Config* cfg = Config::get_instance()) {
        CHECK_FALSE(cfg->get<bool>("/input/touch_range/valid", true));
        CHECK(cfg->get<bool>("/input/calibration/valid", false));
    }
    reset_stored_calibration_keys();
}

TEST_CASE("commit_calibration_result: a null sink still persists the affine-only shape",
          "[touch][touch-calibration][range-fit][commit]") {
    TouchRangeFit fit{};
    fit.valid = true;
    fit.min_x = 900;
    fit.max_x = 3200;
    fit.min_y = 700;
    fit.max_y = 3800;

    CHECK_FALSE(commit_calibration_result(nullptr, some_affine(), fit));

    if (Config* cfg = Config::get_instance()) {
        CHECK_FALSE(cfg->get<bool>("/input/touch_range/valid", true));
        CHECK(cfg->get<bool>("/input/calibration/valid", false));
        CHECK(cfg->get<double>("/input/calibration/e", 0.0) == Approx(1.6));
    }
    reset_stored_calibration_keys();
}

// ============================================================================
// A driver whose advertised ABS range is the display transposed
// ============================================================================

// The FlashForge Creator 5's Goodix controller advertises ABS_MT_POSITION_X/Y as
// 800x480 while the framebuffer is 480x800 portrait, and emits coordinates that
// span the framebuffer the normal way round. The advertised range is the display's
// own dimensions with the axes swapped, so it is not a resolution mismatch the
// wizard can fit an affine to — it is a range that is simply the wrong way up.
TEST_CASE("A transposed declared ABS range is scaled by the display size instead",
          "[touch][touch-calibration][range-fit][abs-transposed]") {
    constexpr int W = 480;
    constexpr int H = 800;
    constexpr int DECLARED_MAX_X = 800;
    constexpr int DECLARED_MAX_Y = 480;
    // What the digitizer actually emits.
    constexpr int RAW_MAX_X = 480;
    constexpr int RAW_MAX_Y = 800;

    REQUIRE(has_transposed_abs_range(DECLARED_MAX_X, DECLARED_MAX_Y, W, H));

    SECTION("scaling by the advertised range loses reach on X and flattens Y") {
        auto declared = [](int rx, int ry) {
            return Point{evdev_calibrate(rx, 0, DECLARED_MAX_X, 0, W - 1),
                         evdev_calibrate(ry, 0, DECLARED_MAX_Y, 0, H - 1)};
        };

        // The far corner lands 40% short across X.
        CHECK(declared(RAW_MAX_X, RAW_MAX_Y).x == 287);
        // Every raw Y at or past the advertised maximum clamps onto the bottom edge,
        // so the lower 40% of the panel collapses to one row.
        CHECK(declared(RAW_MAX_X, DECLARED_MAX_Y).y == H - 1);
        CHECK(declared(RAW_MAX_X, RAW_MAX_Y).y == H - 1);
        // A touch at the centre of the panel reports up and to the left of it.
        CHECK(declared(RAW_MAX_X / 2, RAW_MAX_Y / 2).x == 143);
        CHECK(declared(RAW_MAX_X / 2, RAW_MAX_Y / 2).y == 665);
    }

    SECTION("scaling by the display size maps the panel corner to corner") {
        // The calibration the backend applies: lv_evdev_set_calibration(touch_, 0, 0,
        // screen_width_, screen_height_).
        auto by_display = [](int rx, int ry) {
            return Point{evdev_calibrate(rx, 0, W, 0, W - 1),
                         evdev_calibrate(ry, 0, H, 0, H - 1)};
        };

        CHECK(by_display(0, 0).x == 0);
        CHECK(by_display(0, 0).y == 0);
        CHECK(by_display(RAW_MAX_X, RAW_MAX_Y).x == W - 1);
        CHECK(by_display(RAW_MAX_X, RAW_MAX_Y).y == H - 1);
        CHECK(by_display(RAW_MAX_X / 2, RAW_MAX_Y / 2).x == 239);
        CHECK(by_display(RAW_MAX_X / 2, RAW_MAX_Y / 2).y == 399);
    }
}
