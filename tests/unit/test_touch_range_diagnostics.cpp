// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC
//
// Making a lying touch digitizer diagnosable from a debug bundle alone
// (prestonbrown/helixscreen#1259, #1276).
//
// The FLSUN T1 Pro's tlsc6x panel declares ABS X 0..480 / Y 0..272 and then emits
// 16..243 on X and 13..470 on Y: transposed, and half of Y outside anything the
// declaration allows for. lv_evdev scales against the declaration and then CLAMPS
// to the display, so by the time a coordinate reaches our affine the evidence is
// gone. The only place the truth is still visible is the pre-scale reading behind
// each event, which calibrated_read_cb already fetches.
//
// Two signals come out of that, and they are NOT the same kind of claim:
//
//   out-of-range  a reading fell outside the configured [min,max]. One sample
//                 proves the declaration wrong. No threshold, no sample floor.
//                 This is what fires telemetry.
//   compressed    the observed span is much narrower than the configured span.
//                 A user who never touched the edges produces exactly this on a
//                 perfectly healthy panel, so it is reported and never judged.
//
// The tests below exist mostly to keep those two apart.

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "system/debug_bundle_collector.h"
#include "touch_calibration.h"
#include "touch_calibration_wrapper.h"

#include <initializer_list>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using Catch::Approx;
using json = nlohmann::json;
using namespace helix;

namespace {

/// A pipeline programmed with a plain, non-inverted, non-swapped range.
TouchPipelineInfo make_pipeline(int min_x, int max_x, int min_y, int max_y,
                                bool swap_axes = false) {
    TouchPipelineInfo p;
    p.known = true;
    p.device_name = "tlsc6x_touch";
    p.device_path = "/dev/input/event2";
    p.driver = "evdev";
    p.declared_valid = true;
    p.declared_min_x = min_x;
    p.declared_max_x = max_x;
    p.declared_min_y = min_y;
    p.declared_max_y = max_y;
    p.source = TouchRangeSource::Declared;
    p.configured_valid = true;
    p.swap_axes = swap_axes;
    p.min_x = min_x;
    p.max_x = max_x;
    p.min_y = min_y;
    p.max_y = max_y;
    return p;
}

TouchObservedExtremes observe_all(std::initializer_list<Point> samples) {
    TouchObservedExtremes obs;
    for (const Point& p : samples) {
        obs.observe(p.x, p.y);
    }
    return obs;
}

} // namespace

// ============================================================================
// The out-of-range predicate: unambiguous, no threshold, no sample floor
// ============================================================================

TEST_CASE("touch_range_violation: readings inside a normal range prove nothing",
          "[touch][touch-diagnostics]") {
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);
    const TouchObservedExtremes obs = observe_all({{5, 5}, {240, 136}, {470, 260}});

    const TouchRangeViolation v = touch_range_violation(obs, cfg);
    CHECK_FALSE(v.x);
    CHECK_FALSE(v.y);
    CHECK_FALSE(v.any());
}

TEST_CASE("touch_range_violation: the endpoints themselves are inside the range",
          "[touch][touch-diagnostics]") {
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);

    // Exactly at both bounds on both axes. lv_evdev maps min -> 0 and max -> res-1,
    // so these are the two coordinates the declaration explicitly promises.
    const TouchObservedExtremes obs = observe_all({{0, 0}, {480, 272}});

    const TouchRangeViolation v = touch_range_violation(obs, cfg);
    CHECK_FALSE(v.x);
    CHECK_FALSE(v.y);
}

TEST_CASE("touch_range_violation: one unit past a bound flags that axis alone",
          "[touch][touch-diagnostics]") {
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);

    SECTION("past the maximum") {
        const TouchObservedExtremes obs = observe_all({{240, 136}, {481, 136}});
        const TouchRangeViolation v = touch_range_violation(obs, cfg);
        CHECK(v.x);
        CHECK_FALSE(v.y);
        CHECK(v.any());
    }

    SECTION("below the minimum") {
        const TouchObservedExtremes obs = observe_all({{240, 136}, {240, -1}});
        const TouchRangeViolation v = touch_range_violation(obs, cfg);
        CHECK_FALSE(v.x);
        CHECK(v.y);
    }

    SECTION("a non-zero minimum is still a floor") {
        const TouchPipelineInfo shifted = make_pipeline(100, 3900, 200, 3800);
        const TouchObservedExtremes obs = observe_all({{99, 1000}});
        const TouchRangeViolation v = touch_range_violation(obs, shifted);
        CHECK(v.x);
        CHECK_FALSE(v.y);
    }
}

TEST_CASE("touch_range_violation: an INVERTED range is a range, not an error",
          "[touch][touch-diagnostics]") {
    // min > max is legal and is how a panel wired upside down calibrates: the
    // lv_evdev scale simply gets a negative denominator. Comparing without
    // ordering the pair first would flag every reading on such a panel.
    const TouchPipelineInfo inverted = make_pipeline(3200, 900, 3100, 800);

    SECTION("readings inside the inverted span are fine") {
        const TouchObservedExtremes obs = observe_all({{900, 800}, {2000, 2000}, {3200, 3100}});
        const TouchRangeViolation v = touch_range_violation(obs, inverted);
        CHECK_FALSE(v.x);
        CHECK_FALSE(v.y);
    }

    SECTION("outside the inverted span is still outside") {
        const TouchObservedExtremes obs = observe_all({{899, 2000}, {2000, 3101}});
        const TouchRangeViolation v = touch_range_violation(obs, inverted);
        CHECK(v.x);
        CHECK(v.y);
    }
}

TEST_CASE("touch_range_violation: the T1 Pro's transposed panel is caught",
          "[touch][touch-diagnostics]") {
    // #1259 verbatim: the declaration says the panel is 480 wide and 272 tall,
    // the digitizer emits 16..243 across and 13..470 down. Y alone is impossible.
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);
    const TouchObservedExtremes obs = observe_all({{16, 13}, {130, 240}, {243, 470}});

    const TouchRangeViolation v = touch_range_violation(obs, cfg);
    CHECK_FALSE(v.x); // 16..243 fits inside 0..480, narrow but legal
    CHECK(v.y);       // 470 cannot come out of a panel declared 0..272
    CHECK(v.any());
}

TEST_CASE("touch_range_violation: a single sample is enough", "[touch][touch-diagnostics]") {
    // Deliberately no sample floor. The claim is "this reading cannot exist under
    // that declaration", which one reading settles.
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);
    TouchObservedExtremes obs;
    obs.observe(240, 470);

    REQUIRE(obs.distinct_samples == 1);
    const TouchRangeViolation v = touch_range_violation(obs, cfg);
    CHECK(v.y);
}

TEST_CASE("touch_range_violation: a compressed span is NOT an out-of-range finding",
          "[touch][touch-diagnostics]") {
    // The separation the whole design turns on. A user who tapped four times near
    // the middle of a 12-bit digitizer produces a span ratio of 0.07 and has
    // proven precisely nothing about the declaration.
    const TouchPipelineInfo cfg = make_pipeline(0, 4095, 0, 4095);
    const TouchObservedExtremes obs = observe_all({{1900, 1950}, {2200, 2050}});

    const TouchRangeViolation v = touch_range_violation(obs, cfg);
    CHECK_FALSE(v.x);
    CHECK_FALSE(v.y);
    CHECK_FALSE(v.any());

    // It is still reported, as a number, with no verdict attached.
    CHECK(touch_axis_span_ratio(obs.min_x, obs.max_x, cfg.min_x, cfg.max_x) ==
          Approx(300.0 / 4095.0));
    CHECK(touch_axis_span_ratio(obs.min_y, obs.max_y, cfg.min_y, cfg.max_y) ==
          Approx(100.0 / 4095.0));
}

TEST_CASE("touch_range_violation: nothing observed proves nothing", "[touch][touch-diagnostics]") {
    const TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);
    const TouchObservedExtremes empty;

    REQUIRE(empty.distinct_samples == 0);
    const TouchRangeViolation v = touch_range_violation(empty, cfg);
    CHECK_FALSE(v.any());
}

TEST_CASE("touch_range_violation: an unconfigured or degenerate range is not judged",
          "[touch][touch-diagnostics]") {
    const TouchObservedExtremes obs = observe_all({{9999, 9999}});

    SECTION("no range was recorded at all") {
        TouchPipelineInfo cfg = make_pipeline(0, 480, 0, 272);
        cfg.configured_valid = false;
        CHECK_FALSE(touch_range_violation(obs, cfg).any());
    }

    SECTION("min == max collapses the axis onto one pixel, so nothing is outside it") {
        // lv_evdev skips the scale when min == max but still clamps, so the whole
        // panel lands on a single coordinate. That is a broken range, but it is not
        // a range a reading can fall outside of.
        const TouchPipelineInfo cfg = make_pipeline(100, 100, 100, 100);
        CHECK_FALSE(touch_range_violation(obs, cfg).any());
    }
}

TEST_CASE("touch_range_violation: the swap runs BEFORE the scale", "[touch][touch-diagnostics]") {
    // lv_evdev._evdev_read swaps first and scales second, so on a swapped panel
    // the configured X range is applied to the RAW Y reading. Ignoring the swap
    // does not merely mislabel the axis - it changes the verdict.
    //
    // Configured X is narrow (0..300) and configured Y is wide (0..4000); the
    // digitizer emits x=3500, y=100.
    TouchObservedExtremes obs;
    obs.observe(3500, 100);

    // Swapped: raw y=100 drives configured X (fits), raw x=3500 drives configured
    // Y (fits). Nothing is out of range, and a swap-blind comparison would have
    // reported a violation that does not exist.
    const TouchPipelineInfo swapped = make_pipeline(0, 300, 0, 4000, /*swap_axes=*/true);
    const TouchRangeViolation sv = touch_range_violation(obs, swapped);
    CHECK_FALSE(sv.x);
    CHECK_FALSE(sv.y);

    // Unswapped, the same readings against the same numbers ARE a violation:
    // raw x=3500 cannot come out of a 0..300 declaration.
    const TouchPipelineInfo straight = make_pipeline(0, 300, 0, 4000);
    const TouchRangeViolation v = touch_range_violation(obs, straight);
    CHECK(v.x);
    CHECK_FALSE(v.y);
}

TEST_CASE("touch_axis_span_ratio: degenerate inputs return zero rather than divide",
          "[touch][touch-diagnostics]") {
    CHECK(touch_axis_span_ratio(10, 20, 100, 100) == Approx(0.0));
    CHECK(touch_axis_span_ratio(0, 0, 0, 480) == Approx(0.0));
    // An inverted configured range has the same span as its ordered twin.
    CHECK(touch_axis_span_ratio(100, 200, 3200, 900) == Approx(100.0 / 2300.0));
}

// ============================================================================
// The accumulator
// ============================================================================

TEST_CASE("TouchObservedExtremes: the first sample seeds both bounds",
          "[touch][touch-diagnostics]") {
    TouchObservedExtremes obs;
    REQUIRE(obs.distinct_samples == 0);

    obs.observe(1500, 2500);
    CHECK(obs.distinct_samples == 1);
    CHECK(obs.min_x == 1500);
    CHECK(obs.max_x == 1500);
    CHECK(obs.min_y == 2500);
    CHECK(obs.max_y == 2500);

    // A default-constructed accumulator must NOT behave as if it had seen (0,0):
    // a zeroed min would make every real reading look in-range from below.
    obs.observe(1200, 2900);
    CHECK(obs.min_x == 1200);
    CHECK(obs.max_x == 1500);
    CHECK(obs.min_y == 2500);
    CHECK(obs.max_y == 2900);
    CHECK(obs.distinct_samples == 2);
}

TEST_CASE("TouchObservedExtremes: negative readings widen the low bound",
          "[touch][touch-diagnostics]") {
    TouchObservedExtremes obs;
    obs.observe(10, 10);
    obs.observe(-40, -3);
    CHECK(obs.min_x == -40);
    CHECK(obs.min_y == -3);
    CHECK(obs.max_x == 10);
    CHECK(obs.max_y == 10);
    CHECK(obs.distinct_samples == 2);
}

// ============================================================================
// The read-callback seam that feeds it
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "touch diagnostics: no raw source means no diagnostics at all",
                 "[touch][touch-diagnostics][wrapper]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    TouchCalibration cal;
    CalibrationContext ctx;
    install_calibration_wrapper(indev, ctx, cal, 800, 480);

    // Desktop/SDL and libinput pointers never populate raw_source. Feeding events
    // through the wrapper must accumulate nothing and claim nothing.
    for (int i = 0; i < 20; i++) {
        lv_indev_data_t data{};
        data.point.x = 10 + i;
        data.point.y = 20 + i;
        data.state = LV_INDEV_STATE_PRESSED;
        calibrated_read_cb(indev, &data);
    }

    TouchRangeDiagnostics diag;
    CHECK_FALSE(get_touch_range_diagnostics(diag));
    CHECK_FALSE(diag.available);
    CHECK_FALSE(diag.unavailable_reason.empty());
    CHECK(diag.observed.distinct_samples == 0);

    uninstall_calibration_wrapper(indev, ctx);
    lv_indev_delete(indev);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "touch diagnostics: extremes accumulate across reads, repeats do not count",
                 "[touch][touch-diagnostics][wrapper]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    TouchCalibration cal;
    CalibrationContext ctx;
    install_calibration_wrapper(indev, ctx, cal, 480, 272);
    set_touch_pipeline_info(make_pipeline(0, 480, 0, 272));

    // lv_evdev_get_last_raw() keeps serving the last reading until a new event
    // arrives, and the read callback runs at the indev poll rate. Counting every
    // invocation would report 400 samples for four taps and make the count
    // useless as a confidence signal, which is the only reason it is shipped.
    int raw_x = 0;
    int raw_y = 0;
    ctx.raw_source = [&raw_x, &raw_y](int& x, int& y) {
        x = raw_x;
        y = raw_y;
        return true;
    };

    auto feed = [&](int rx, int ry, int repeats) {
        raw_x = rx;
        raw_y = ry;
        for (int i = 0; i < repeats; i++) {
            lv_indev_data_t data{};
            data.point.x = 1;
            data.point.y = 1;
            data.state = LV_INDEV_STATE_PRESSED;
            calibrated_read_cb(indev, &data);
        }
    };

    feed(16, 13, 30);
    feed(130, 240, 30);
    feed(243, 470, 30);

    TouchRangeDiagnostics diag;
    REQUIRE(get_touch_range_diagnostics(diag));
    CHECK(diag.available);
    CHECK(diag.observed.distinct_samples == 3);
    CHECK(diag.observed.min_x == 16);
    CHECK(diag.observed.max_x == 243);
    CHECK(diag.observed.min_y == 13);
    CHECK(diag.observed.max_y == 470);

    // And the pipeline the backend recorded comes back with it.
    CHECK(diag.pipeline.known);
    CHECK(diag.pipeline.device_name == "tlsc6x_touch");
    CHECK(diag.pipeline.max_y == 272);
    CHECK(diag.pipeline.source == TouchRangeSource::Declared);

    // The predicate over the pair is the T1 Pro finding.
    const TouchRangeViolation v = touch_range_violation(diag.observed, diag.pipeline);
    CHECK(v.y);

    uninstall_calibration_wrapper(indev, ctx);
    lv_indev_delete(indev);
}

TEST_CASE_METHOD(LVGLTestFixture, "touch diagnostics: a torn-down wrapper reports nothing",
                 "[touch][touch-diagnostics][wrapper]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    TouchCalibration cal;
    {
        CalibrationContext ctx;
        install_calibration_wrapper(indev, ctx, cal, 480, 272);
        ctx.raw_source = [](int& x, int& y) {
            x = 100;
            y = 100;
            return true;
        };
        lv_indev_data_t data{};
        data.state = LV_INDEV_STATE_PRESSED;
        calibrated_read_cb(indev, &data);

        TouchRangeDiagnostics live;
        REQUIRE(get_touch_range_diagnostics(live));

        uninstall_calibration_wrapper(indev, ctx);
    }

    // The context is gone; the snapshot must not reach through the stale handle.
    TouchRangeDiagnostics diag;
    CHECK_FALSE(get_touch_range_diagnostics(diag));

    lv_indev_delete(indev);
}

TEST_CASE_METHOD(LVGLTestFixture, "touch diagnostics: the span check latches for the bundle",
                 "[touch][touch-diagnostics][wrapper]") {
    lv_indev_t* indev = lv_indev_create();
    REQUIRE(indev != nullptr);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);

    TouchCalibration cal;
    CalibrationContext ctx;
    install_calibration_wrapper(indev, ctx, cal, 480, 272);
    ctx.raw_source = [](int& x, int& y) {
        x = 50;
        y = 60;
        return true;
    };
    lv_indev_data_t data{};
    data.state = LV_INDEV_STATE_PRESSED;
    calibrated_read_cb(indev, &data);

    // Nothing calibrated yet this session, so the ratio must be absent rather
    // than a fabricated 0.0 that reads as a catastrophic compression.
    TouchRangeDiagnostics before;
    REQUIRE(get_touch_range_diagnostics(before));
    CHECK_FALSE(before.span_check_seen);

    record_touch_span_check(0.57, 0.49);

    TouchRangeDiagnostics after;
    REQUIRE(get_touch_range_diagnostics(after));
    CHECK(after.span_check_seen);
    CHECK(after.span_check_x == Approx(0.57));
    CHECK(after.span_check_y == Approx(0.49));

    uninstall_calibration_wrapper(indev, ctx);
    lv_indev_delete(indev);
}

// ============================================================================
// The bundle section
// ============================================================================

TEST_CASE("DebugBundleCollector: build_touch_info degrades to a reason when unavailable",
          "[debug-bundle][touch-diagnostics]") {
    TouchRangeDiagnostics diag;
    diag.available = false;
    diag.unavailable_reason = "no-raw-source";

    const json j = DebugBundleCollector::build_touch_info(diag);
    REQUIRE(j.is_object());
    CHECK(j["available"] == false);
    CHECK(j["reason"] == "no-raw-source");

    // No fabricated numbers: a reader must not be able to mistake an absent
    // observation for an observed zero.
    CHECK_FALSE(j.contains("observed"));
    CHECK_FALSE(j.contains("configured"));
    CHECK_FALSE(j.contains("out_of_range"));
}

TEST_CASE("DebugBundleCollector: build_touch_info carries the T1 Pro evidence",
          "[debug-bundle][touch-diagnostics]") {
    TouchRangeDiagnostics diag;
    diag.available = true;
    diag.pipeline = make_pipeline(0, 480, 0, 272);
    diag.observed = observe_all({{16, 13}, {130, 240}, {243, 470}});
    diag.affine_valid = false;

    const json j = DebugBundleCollector::build_touch_info(diag);
    REQUIRE(j.is_object());
    CHECK(j["available"] == true);

    CHECK(j["device"]["name"] == "tlsc6x_touch");
    CHECK(j["device"]["path"] == "/dev/input/event2");
    CHECK(j["device"]["driver"] == "evdev");

    CHECK(j["declared_abs"]["valid"] == true);
    CHECK(j["declared_abs"]["max_y"] == 272);
    CHECK(j["declared_abs"]["mt_fallback"] == false);

    CHECK(j["configured"]["valid"] == true);
    CHECK(j["configured"]["source"] == "declared");
    CHECK(j["configured"]["swap_axes"] == false);
    CHECK(j["configured"]["min_x"] == 0);
    CHECK(j["configured"]["max_x"] == 480);
    CHECK(j["configured"]["max_y"] == 272);

    // The sample count is load-bearing: "observed up to 470" means one thing
    // after 3 readings and another after 300, and the reader cannot tell which
    // without it.
    CHECK(j["observed"]["distinct_samples"] == 3);
    CHECK(j["observed"]["min_x"] == 16);
    CHECK(j["observed"]["max_x"] == 243);
    CHECK(j["observed"]["min_y"] == 13);
    CHECK(j["observed"]["max_y"] == 470);

    CHECK(j["out_of_range"]["x"] == false);
    CHECK(j["out_of_range"]["y"] == true);

    CHECK(j["span_ratio"]["x"].get<double>() == Approx(227.0 / 480.0));
    CHECK(j["span_ratio"]["y"].get<double>() == Approx(457.0 / 272.0));

    CHECK(j["affine_valid"] == false);
    CHECK(j["stored_range"]["valid"] == false);
    CHECK_FALSE(j.contains("span_check"));
}

TEST_CASE("DebugBundleCollector: build_touch_info reports a compressed span without a verdict",
          "[debug-bundle][touch-diagnostics]") {
    TouchRangeDiagnostics diag;
    diag.available = true;
    diag.pipeline = make_pipeline(0, 4095, 0, 4095);
    diag.observed = observe_all({{1900, 1950}, {2200, 2050}});

    const json j = DebugBundleCollector::build_touch_info(diag);
    CHECK(j["out_of_range"]["x"] == false);
    CHECK(j["out_of_range"]["y"] == false);
    CHECK(j["span_ratio"]["x"].get<double>() == Approx(300.0 / 4095.0));
    CHECK(j["observed"]["distinct_samples"] == 2);
}

TEST_CASE("DebugBundleCollector: build_touch_info includes the stored range and span check",
          "[debug-bundle][touch-diagnostics]") {
    TouchRangeDiagnostics diag;
    diag.available = true;
    diag.pipeline = make_pipeline(900, 3200, 800, 3100, /*swap_axes=*/true);
    diag.pipeline.source = TouchRangeSource::Stored;
    diag.pipeline.declared_mt_fallback = true;
    diag.observed = observe_all({{1000, 1000}, {3000, 3000}});
    diag.pipeline.stored.valid = true;
    diag.pipeline.stored.swap_axes = true;
    diag.pipeline.stored.min_x = 900;
    diag.pipeline.stored.max_x = 3200;
    diag.pipeline.stored.min_y = 800;
    diag.pipeline.stored.max_y = 3100;
    diag.affine_valid = true;
    diag.span_check_seen = true;
    diag.span_check_x = 0.57;
    diag.span_check_y = 0.49;

    const json j = DebugBundleCollector::build_touch_info(diag);
    CHECK(j["configured"]["source"] == "stored");
    CHECK(j["configured"]["swap_axes"] == true);
    CHECK(j["declared_abs"]["mt_fallback"] == true);
    CHECK(j["stored_range"]["valid"] == true);
    CHECK(j["stored_range"]["swap_axes"] == true);
    CHECK(j["stored_range"]["min_x"] == 900);
    CHECK(j["stored_range"]["max_y"] == 3100);
    CHECK(j["affine_valid"] == true);
    REQUIRE(j.contains("span_check"));
    CHECK(j["span_check"]["x"].get<double>() == Approx(0.57));
    CHECK(j["span_check"]["y"].get<double>() == Approx(0.49));
}

TEST_CASE("DebugBundleCollector: build_touch_info names every range source",
          "[debug-bundle][touch-diagnostics]") {
    TouchRangeDiagnostics diag;
    diag.available = true;
    diag.observed = observe_all({{10, 10}});

    struct Case {
        TouchRangeSource source;
        const char* name;
    };
    const Case cases[] = {{TouchRangeSource::None, "none"},
                          {TouchRangeSource::Declared, "declared"},
                          {TouchRangeSource::Stored, "stored"},
                          {TouchRangeSource::Environment, "environment"}};

    for (const Case& c : cases) {
        diag.pipeline = make_pipeline(0, 480, 0, 272);
        diag.pipeline.source = c.source;
        const json j = DebugBundleCollector::build_touch_info(diag);
        CHECK(j["configured"]["source"] == c.name);
    }
}

TEST_CASE("DebugBundleCollector: collect() carries a touch section",
          "[debug-bundle][touch-diagnostics]") {
    const json bundle = DebugBundleCollector::collect();
    REQUIRE(bundle.contains("touch"));
    REQUIRE(bundle["touch"].is_object());
    // On a desktop test run there is no evdev raw source, so the section must be
    // the honest "unavailable" shape rather than a wall of zeroes.
    REQUIRE(bundle["touch"].contains("available"));
}
