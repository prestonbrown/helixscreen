// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_graph_reattach.cpp
 * @brief Reconnect re-attach behaviour of TempGraphController (#1245)
 *
 * TempGraphController::reattach_observers() rebinds every observer to whatever
 * subjects are live after a reconnect, without tearing the chart down. The
 * hazard it has to avoid is the attach-time fire: observe_int_sync() attaches
 * with lv_subject_add_observer_obj(), which fires the observer once
 * immediately, and the subjects still hold their PRE-disconnect values. The
 * live handler stamps whatever it receives with a fresh `now`, so an
 * unsuppressed attach fire draws a phantom spike bridging the entire
 * disconnect gap.
 *
 * What makes this easy to get wrong — and what these tests pin — is that the
 * attach fire is not synchronous. observe_int_sync's LVGL callback only QUEUES
 * the handler (observer_factory.h), so it runs on a later UpdateQueue tick,
 * long after setup_observers() has returned. Any suppression scoped to the
 * body of reattach_observers() is a no-op.
 */

#include "ui_update_queue.h"

#include "../../include/temp_graph_controller.h"
#include "../../include/ui_temp_graph.h"
#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Connection-state subject values, mirroring setup_connection_observer().
constexpr int CONN_DISCONNECTED = 0;
constexpr int CONN_CONNECTED = 2;

class TempGraphReattachFixture : public LVGLTestFixture {
  public:
    TempGraphReattachFixture() {
        get_printer_state().init_subjects(false);

        // Zero the bed subject before any controller attaches to it. The
        // subject is process-global and keeps whatever the previous test left
        // in it; a non-zero value passes the handler's validity filter, so the
        // attach fire at construction lands a real point AND arms that series'
        // SAMPLE_INTERVAL_SEC throttle. The follow-up reading these tests push
        // would then be throttled away, and the assertion would blame the
        // suppression flag for a co-tenant's leftover state.
        if (auto* bed = get_printer_state().get_bed_temp_subject()) {
            lv_subject_set_int(bed, 0);
        }
        settle();
    }

    ~TempGraphReattachFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Run the queue until nothing new is scheduled. A single drain() is not
    /// enough here: process_pending() swaps the queue out before running it, so
    /// callbacks queued FROM a callback (the attach fires, then the suppression
    /// clear behind them) land on the following tick.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Drive a disconnect → reconnect transition past the controller's
    /// connection observer, then let everything it scheduled run.
    static void reconnect() {
        auto* conn = get_printer_state().get_printer_connection_state_subject();
        REQUIRE(conn != nullptr);
        lv_subject_set_int(conn, CONN_DISCONNECTED);
        settle();
        lv_subject_set_int(conn, CONN_CONNECTED);
        settle();
    }
};

/// Build a bed-only controller and park a stale reading in the bed subject
/// WITHOUT letting it reach the chart, so the series' throttle timestamp stays
/// at zero. That is the state a backgrounded graph resumes in: the subject
/// still holds the pre-suspend temperature, and the series has not sampled for
/// far longer than the 3 s throttle window, so the throttle will not save us.
std::unique_ptr<TempGraphController> make_stale_controller(lv_obj_t* parent, int stale_deci) {
    TempGraphControllerConfig cfg;
    cfg.series = {{"heater_bed", lv_color_hex(0x88C0D0), true, "Bed"}};

    auto controller = std::make_unique<TempGraphController>(parent, cfg);
    REQUIRE(controller->is_valid());
    TempGraphReattachFixture::settle();

    // Pin the precondition the throttle reasoning depends on: nothing has been
    // sampled yet, so a later reading cannot be rejected for arriving too soon.
    REQUIRE(controller->graph()->visible_point_count == 0);

    auto* bed = get_printer_state().get_bed_temp_subject();
    REQUIRE(bed != nullptr);

    controller->pause();
    lv_subject_set_int(bed, stale_deci);
    TempGraphReattachFixture::settle();
    controller->resume();
    TempGraphReattachFixture::settle();

    return controller;
}

} // namespace

// ============================================================================
// The attach-time fire must not reach the chart
// ============================================================================

TEST_CASE_METHOD(TempGraphReattachFixture,
                 "Reattach does not push the stale subject value onto the chart",
                 "[controller][temp_graph_controller][1245]") {
    auto controller = make_stale_controller(test_screen(), 2000); // 200.0 C, stale

    const int baseline = controller->graph()->visible_point_count;

    reconnect();

    // Nothing new on the chart. Before the fix this was baseline + 1: the
    // attach fire ran a tick after reattach_observers() had already restored
    // the suppression flag, so it sailed through and stamped 200.0 C at `now`.
    REQUIRE(controller->graph()->visible_point_count == baseline);

    controller.reset();
    settle();
}

TEST_CASE_METHOD(TempGraphReattachFixture, "Reattach suppression clears for the next real sample",
                 "[controller][temp_graph_controller][1245]") {
    auto controller = make_stale_controller(test_screen(), 2000);

    const int baseline = controller->graph()->visible_point_count;
    reconnect();
    REQUIRE(controller->graph()->visible_point_count == baseline);

    // A genuinely fresh reading after the reconnect must land. This is the half
    // that fails if the suppression is set and never cleared — the graph would
    // simply freeze for the rest of the session.
    auto* bed = get_printer_state().get_bed_temp_subject();
    lv_subject_set_int(bed, 2100);
    settle();

    REQUIRE(controller->graph()->visible_point_count == baseline + 1);

    controller.reset();
    settle();
}

TEST_CASE_METHOD(TempGraphReattachFixture, "Reattach keeps the graph and its series alive",
                 "[controller][temp_graph_controller][1245]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true, "Nozzle"},
        {"heater_bed", lv_color_hex(0x88C0D0), true, "Bed"},
    };
    auto controller = std::make_unique<TempGraphController>(test_screen(), cfg);
    REQUIRE(controller->is_valid());
    settle();

    auto* graph_before = controller->graph();
    const int bed_id = controller->series_id_for("heater_bed");
    REQUIRE(bed_id >= 0);

    reconnect();

    // reattach_observers() explicitly does NOT rebuild: the widget, the series
    // IDs, and therefore the chart data all survive.
    REQUIRE(controller->graph() == graph_before);
    REQUIRE(controller->series_id_for("heater_bed") == bed_id);
    REQUIRE(controller->series_id_for("extruder") >= 0);

    controller.reset();
    settle();
}
