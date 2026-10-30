// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_graph_gradient_coalesce.cpp
 * @brief The temp graph's pending gradient recompute dies with the graph.
 *
 * draw_gradient_cb asks for an out-of-render-pass recompute through the graph's
 * `gradient_refresh` CoalescedTimer, and the request holds the graph pointer, so
 * ui_temp_graph_destroy() must retract it or the recompute lands on freed
 * storage. Coalescing and starvation are CoalescedTimer's own properties
 * (test_coalesced_timer.cpp); the draw callback is private, so this drives the
 * timer it schedules on.
 */

#include "ui_temp_graph.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include <memory>

#include "../catch_amalgamated.hpp"

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
