// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_graph_gradient_coalesce.cpp
 * @brief The gradient recompute is requested per drawn frame — it must coalesce,
 *        and it must not survive the graph it reaches into.
 *
 * draw_gradient_cb asks for an out-of-render-pass recompute on every frame it
 * finds the cache stale. Two properties follow, and neither is free:
 *
 *   1. Repeat requests while one is pending must not each allocate. They also
 *      must not push the deadline out — a graph animating at frame rate would
 *      never rebuild its cache, and would software-render every column every
 *      frame forever (#979, the shape that froze the K2 Plus touch UI).
 *   2. The request holds the graph pointer, so ui_temp_graph_destroy() must
 *      retract it. The recompute otherwise lands on freed storage.
 *
 * The draw callback is private to the widget, so these drive the timer the
 * callback schedules on — `graph->gradient_refresh` — which is the seam that
 * carries both properties.
 */

#include "ui_temp_graph.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

/// Live entries in LVGL timer list. One request must cost one timer however
/// many frames ask, which is the allocation claim the recompute counter cannot
/// make: the stored callback is consumed by the first firing, so surplus timers
/// fire into an empty slot and go unseen.
int live_timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "TempGraph: per-frame recompute requests coalesce to one",
                 "[ui][temp_graph][coalesced_timer]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(test_screen());
    REQUIRE(graph != nullptr);

    int recomputes = 0;
    const int before = live_timer_count();
    // Ten frames' worth of requests, none of them allowed to run yet.
    for (int i = 0; i < 10; i++) {
        graph->gradient_refresh.schedule_once([&recomputes]() { recomputes++; });
    }
    REQUIRE(graph->gradient_refresh.pending());

    // Ten requests, one timer. This is the assertion that fails when the
    // coalescing is removed; against lv_async_call it reads ten.
    REQUIRE(live_timer_count() - before == 1);

    process_lvgl(50);
    REQUIRE(recomputes == 1);

    // The next stale frame gets its own recompute — the claim is released once
    // the work has run, otherwise the cache would rebuild exactly once ever.
    graph->gradient_refresh.schedule_once([&recomputes]() { recomputes++; });
    process_lvgl(50);
    REQUIRE(recomputes == 2);

    ui_temp_graph_destroy(graph);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraph: a request arriving every frame still fires",
                 "[ui][temp_graph][coalesced_timer]") {
    // The starvation case, at the seam that would starve: a graph whose cache is
    // stale re-requests every frame. A trailing-edge debounce would reset the
    // deadline each time and never come due.
    ui_temp_graph_t* graph = ui_temp_graph_create(test_screen());
    REQUIRE(graph != nullptr);

    int recomputes = 0;
    for (int frame = 0; frame < 12; frame++) {
        graph->gradient_refresh.schedule_once([&recomputes]() { recomputes++; });
        process_lvgl(5);
    }
    REQUIRE(recomputes >= 1);

    ui_temp_graph_destroy(graph);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "TempGraph: destroy retracts a pending gradient recompute",
                 "[ui][temp_graph][coalesced_timer]") {
    // The recompute dereferences the graph. Held in a shared_ptr so the flag
    // outlives the graph and can still be read after the struct is gone.
    auto fired = std::make_shared<bool>(false);

    ui_temp_graph_t* graph = ui_temp_graph_create(test_screen());
    REQUIRE(graph != nullptr);

    graph->gradient_refresh.schedule_once([fired]() { *fired = true; });
    REQUIRE(graph->gradient_refresh.pending());

    ui_temp_graph_destroy(graph);

    // Well past any plausible period. A recompute here would be running against
    // freed storage in production; the flag is the visible half of that.
    process_lvgl(200);
    REQUIRE_FALSE(*fired);
}
