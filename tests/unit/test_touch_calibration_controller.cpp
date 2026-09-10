// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file test_touch_calibration_controller.cpp
 * @brief The calibration logic both entry points share
 *
 * The wizard step and the Settings overlay drive the same controller, so what it
 * decides - which target is live, where it sits, which sink every phase drives -
 * is decided once for both. These cases pin those decisions on the controller
 * itself, where neither view can shadow them.
 */

#include "touch_calibration_controller.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// Minimal sink: enough to prove the controller drives one, and which one.
struct RecordingSink : ICalibrationSink {
    TouchCalibration stored{};
    int capture_active_calls = 0;
    bool capture_active = false;

    TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const TouchCalibration& cal) override {
        stored = cal;
        return true;
    }
    void disable_affine() override {}
    void enable_affine() override {}
    void clear_calibration() override {
        stored = TouchCalibration{};
    }
    void set_capture_active(bool active) override {
        capture_active = active;
        ++capture_active_calls;
    }
};

/// Counts what the controller asks a view to draw.
struct CountingView : ITouchCalibrationView {
    int progress = 0;
    int capture_feedback = 0;
    Point last_landed{};

    void on_progress() override {
        ++progress;
    }
    void on_capture_feedback(Point landed) override {
        ++capture_feedback;
        last_landed = landed;
    }
};

} // namespace

TEST_CASE("controller: the live target advances with the capture sequence",
          "[touch-calibration][controller]") {
    TouchCalibrationController controller;
    TouchCalibrationPanel* panel = controller.panel();
    REQUIRE(panel != nullptr);
    panel->set_screen_size(800, 480);

    SECTION("idle has no live target") {
        REQUIRE(controller.active_target_index() == -1);
    }

    SECTION("the sequence walks 0, 1, 2 and then stops") {
        panel->start();
        REQUIRE(controller.active_target_index() == 0);

        panel->capture_point(Point{10, 10});
        REQUIRE(controller.active_target_index() == 1);

        panel->capture_point(Point{20, 20});
        REQUIRE(controller.active_target_index() == 2);

        // The third capture solves and leaves the point sequence.
        panel->capture_point(Point{30, 400});
        REQUIRE(controller.active_target_index() == -1);
    }

    SECTION("the live target's position is the panel's target for that step") {
        panel->start();
        for (int step = 0; step < 3; ++step) {
            INFO("step " << step);
            REQUIRE(controller.active_target_index() == step);
            const Point expected = panel->get_target_position(step);
            const Point actual = controller.active_target_position();
            REQUIRE(actual.x == expected.x);
            REQUIRE(actual.y == expected.y);
            panel->capture_point(Point{10 * (step + 1), 30 * (step + 1) + 5});
        }
    }

    SECTION("with no live target the position is not a stale one") {
        const Point idle = controller.active_target_position();
        REQUIRE(idle.x == 0);
        REQUIRE(idle.y == 0);
    }
}

TEST_CASE("controller: every phase drives the injected sink", "[touch-calibration][controller]") {
    TouchCalibrationController controller;
    RecordingSink sink;
    controller.set_sink_override(&sink);

    REQUIRE(controller.sink() == &sink);

    CountingView view;
    controller.attach(view);
    controller.panel()->set_screen_size(800, 480);

    // begin() suppresses the global debug ripple through the sink rather than
    // reaching for DisplayManager, which is what lets a test run this at all.
    controller.begin();
    REQUIRE(sink.capture_active == true);

    controller.end();
    REQUIRE(sink.capture_active == false);
    REQUIRE(sink.capture_active_calls == 2);
}

TEST_CASE("controller: an unsolved session commits nothing", "[touch-calibration][controller]") {
    TouchCalibrationController controller;
    RecordingSink sink;
    controller.set_sink_override(&sink);
    controller.panel()->set_screen_size(800, 480);

    // No calibration has been solved, so there is nothing to persist and the
    // device must be left alone rather than handed an invalid matrix.
    REQUIRE(controller.commit() == CommitOutcome::NoCalibration);
    REQUIRE(sink.stored.valid == false);
}

TEST_CASE("controller: a view is optional", "[touch-calibration][controller]") {
    // The Q2 marker tests arm a session through the test-access helper without
    // ever calling begin(), so a controller with no attached view has to keep
    // working rather than dropping the capture on the floor.
    TouchCalibrationController controller;
    RecordingSink sink;
    controller.set_sink_override(&sink);
    controller.panel()->set_screen_size(800, 480);
    controller.panel()->start();

    REQUIRE_NOTHROW(controller.on_release());
    REQUIRE(controller.active_target_index() == 0);
}
