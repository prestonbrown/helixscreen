// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/temp_graph_internal.h"
#include "../../include/temp_graph_tooltip.h"
#include "../../include/theme_manager.h"
#include "../../include/ui_temp_graph.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using helix::temp_graph_internal::find_meta_by_id;
using helix::temp_graph_internal::temp_graph_compute_geometry;
using helix::temp_graph_internal::temp_graph_geometry_t;
using helix::temp_graph_internal::temp_graph_tooltip_box_area;
using helix::temp_graph_internal::TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX;
using helix::temp_graph_internal::tooltip_hit_test;

// Mirrors TempGraphTestFixture in test_temp_graph.cpp (headless display + a
// parent screen). Kept local rather than shared because that fixture is defined
// inline in its own TU.
class TooltipTestFixture {
  public:
    TooltipTestFixture() {
        lv_init_safe();
        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
        screen = lv_obj_create(NULL);
    }
    lv_obj_t* screen;

    /// Graph with a settled layout, a 0-300 deci range, and a known plot rect.
    ui_temp_graph_t* make_graph() {
        ui_temp_graph_t* g = ui_temp_graph_create(screen);
        REQUIRE(g != nullptr);
        lv_obj_set_size(g->chart, 600, 300);
        ui_temp_graph_set_temp_range(g, 0.0f, 300.0f);
        lv_obj_update_layout(screen);
        return g;
    }

    /// Absolute pixel position of logical sample `idx` for `series_id`, using the
    /// same mapping the chart draws with.
    lv_point_t point_pos(ui_temp_graph_t* g, int series_id, int idx) {
        temp_graph_geometry_t geo{};
        REQUIRE(temp_graph_compute_geometry(g, &geo));
        const int32_t pc = static_cast<int32_t>(geo.point_count);
        int32_t* y = lv_chart_get_y_array(g->chart, g->series_meta[series_id].chart_series);
        uint32_t sp = lv_chart_get_x_start_point(g->chart, g->series_meta[series_id].chart_series);
        int32_t v = y[(sp + idx) % pc];
        lv_point_t p;
        p.x = geo.cx1 + idx * (geo.cw - 1) / (pc - 1);
        p.y = (geo.cy1 + geo.ch) - lv_map(v, geo.y_min, geo.y_max, 0, geo.ch);
        return p;
    }
};

TEST_CASE_METHOD(TooltipTestFixture, "hit test on an empty graph misses", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    REQUIRE_FALSE(tooltip_hit_test(g, 300, 150).has_value());
    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "hit test is null-safe", "[ui][tooltip]") {
    REQUIRE_FALSE(tooltip_hit_test(nullptr, 0, 0).has_value());
}

TEST_CASE_METHOD(TooltipTestFixture, "hit test finds a sample under the tap", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 20; i++) {
        ui_temp_graph_update_series_with_time(g, id, 100.0f + i, 1000000000000LL + i * 3000);
    }
    const int last = g->point_count - 1;
    lv_point_t p = point_pos(g, id, last);

    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    CHECK(hit->series_id == id);
    CHECK(hit->logical_index == last);
    CHECK(hit->deci_temp == 1190); // 119.0 deg, the 20th sample
    CHECK(hit->timestamp_ms == g->latest_point_time_ms);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "hit test misses beyond the radius", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 20; i++) {
        ui_temp_graph_update_series_with_time(g, id, 100.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);

    // Just inside, then clearly outside, straight down from the point.
    REQUIRE(tooltip_hit_test(g, p.x, p.y + TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX - 2).has_value());
    REQUIRE_FALSE(tooltip_hit_test(g, p.x, p.y + TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX + 5).has_value());

    ui_temp_graph_destroy(g);
}

// Distance is measured to the drawn LINE, not only to the sample points. On a
// steep run (a heater ramp) consecutive samples are far apart vertically, so a
// tap landing squarely on the visible line can be outside the radius of BOTH
// endpoints. The two REQUIRE(far(...)) lines below are what give this test its
// teeth: they assert the tap is in exactly the region point-only hit testing
// cannot serve, so reverting to sample-only distance turns the final
// REQUIRE(hit.has_value()) red.
TEST_CASE_METHOD(TooltipTestFixture, "tap on a steep segment between samples still hits",
                 "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    // Alternating extremes make each drawn segment nearly vertical: adjacent
    // samples sit ~1.5px apart in x (600px / 399 gaps) but ~280px apart in y.
    for (int i = 0; i < 10; i++) {
        ui_temp_graph_update_series_with_time(g, id, (i % 2) ? 290.0f : 10.0f,
                                              1000000000000LL + i * 3000);
    }
    const int last = g->point_count - 1;
    const lv_point_t a = point_pos(g, id, last - 1);
    const lv_point_t b = point_pos(g, id, last);

    const int32_t mx = (a.x + b.x) / 2;
    const int32_t my = (a.y + b.y) / 2;

    constexpr int64_t r2 =
        static_cast<int64_t>(TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX) * TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX;
    auto far_from = [&](lv_point_t p) {
        const int64_t dx = p.x - mx;
        const int64_t dy = p.y - my;
        return dx * dx + dy * dy > r2;
    };
    REQUIRE(far_from(a));
    REQUIRE(far_from(b));

    auto hit = tooltip_hit_test(g, mx, my);
    REQUIRE(hit.has_value());
    CHECK(hit->series_id == id);
    // Attributed to a real sample, never an interpolated point.
    CHECK((hit->logical_index == last || hit->logical_index == last - 1));

    ui_temp_graph_destroy(g);
}

// A gap breaks the drawn line, so no segment may span one: the midpoint between
// a sample and a POINT_NONE slot is not on anything the user can see.
TEST_CASE_METHOD(TooltipTestFixture, "no segment spans a POINT_NONE gap", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    // Array mode clears to POINT_NONE, then writes exactly `count` points, so
    // slot pc-3 is real and pc-4 is a gap.
    float temps[3] = {10.0f, 290.0f, 10.0f};
    ui_temp_graph_set_series_data(g, id, temps, 3);

    temp_graph_geometry_t geo{};
    REQUIRE(temp_graph_compute_geometry(g, &geo));
    const int oldest_real = g->point_count - 3;
    const lv_point_t first = point_pos(g, id, oldest_real);

    // Halfway between the oldest real sample and the plot's left edge at the
    // same height: over the gap, well away from any drawn line.
    const int32_t mx = (geo.cx1 + first.x) / 2;
    REQUIRE(first.x - mx > TEMP_GRAPH_TOOLTIP_HIT_RADIUS_PX);
    REQUIRE_FALSE(tooltip_hit_test(g, mx, first.y).has_value());

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "hit test picks the nearest of two series", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int hot = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    int cold = ui_temp_graph_add_series(g, "Bed", lv_color_hex(0x44FF44));
    for (int i = 0; i < 20; i++) {
        int64_t ts = 1000000000000LL + i * 3000;
        ui_temp_graph_update_series_with_time(g, hot, 200.0f, ts);
        ui_temp_graph_update_series_with_time(g, cold, 60.0f, ts);
    }
    const int last = g->point_count - 1;
    lv_point_t hot_p = point_pos(g, hot, last);

    // 3px below the hot line: far from the cold line, which sits much lower.
    auto hit = tooltip_hit_test(g, hot_p.x, hot_p.y + 3);
    REQUIRE(hit.has_value());
    CHECK(hit->series_id == hot);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "hidden series are never candidates", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 20; i++) {
        ui_temp_graph_update_series_with_time(g, id, 150.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);
    REQUIRE(tooltip_hit_test(g, p.x, p.y).has_value());

    ui_temp_graph_show_series(g, id, false);
    REQUIRE_FALSE(tooltip_hit_test(g, p.x, p.y).has_value());

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "empty leading slots are never candidates", "[ui][tooltip]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    // update_series_with_time backfills the WHOLE buffer to the first pushed value
    // (avoids a visual ramp from zero — see ui_temp_graph.cpp), so 3 pushes through
    // it leave no LV_CHART_POINT_NONE slots at all. set_series_data (array mode) is
    // the API that actually produces the "only 3 real samples" state: it clears to
    // POINT_NONE, then writes exactly `count` points. Only 3 samples: logical slots
    // [0, pc-4] are LV_CHART_POINT_NONE.
    float temps[3] = {150.0f, 150.0f, 150.0f};
    ui_temp_graph_set_series_data(g, id, temps, 3);
    temp_graph_geometry_t geo{};
    REQUIRE(temp_graph_compute_geometry(g, &geo));

    // A broken impl that forgot to skip POINT_NONE would place those slots at the
    // TOP-LEFT of the plot: LV_CHART_POINT_NONE is INT32_MAX, and lv_map clamps it
    // to max_out, giving py = floor_y - ch = cy1. Tap exactly there: the real samples
    // are all at the far right, so a correct impl finds nothing within the radius.
    REQUIRE_FALSE(tooltip_hit_test(g, geo.cx1 + 2, geo.cy1).has_value());

    ui_temp_graph_destroy(g);
}

using helix::temp_graph_internal::target_deci_at;

TEST_CASE_METHOD(TooltipTestFixture, "target lookup handles a partially filled buffer",
                 "[ui][tooltip][target]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));

    // 5 samples, each with a DIFFERENT target, so an off-by-N is visible.
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_set_current_target(g, id, 200.0f + i, true);
        ui_temp_graph_update_series_with_time(g, id, 150.0f + i, 1000000000000LL + i * 3000);
    }

    const ui_temp_series_meta_t* meta = &g->series_meta[id];
    REQUIRE(meta->target_head == 5);
    const int pc = g->point_count;

    // Newest chart slot must report the newest target (204.0 -> 2040).
    CHECK(target_deci_at(meta, pc, pc - 1) == 2040);
    // Oldest real sample must report the oldest target (200.0 -> 2000).
    CHECK(target_deci_at(meta, pc, pc - 5) == 2000);
    // A slot before any data has no target.
    CHECK(target_deci_at(meta, pc, pc - 6) == 0);
    CHECK(target_deci_at(meta, pc, 0) == 0);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "target lookup is null-safe", "[ui][tooltip][target]") {
    CHECK(target_deci_at(nullptr, 400, 10) == 0);
}

TEST_CASE_METHOD(TooltipTestFixture, "hit reports the target at that sample",
                 "[ui][tooltip][target]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_set_current_target(g, id, 200.0f + i, true);
        ui_temp_graph_update_series_with_time(g, id, 150.0f + i, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);

    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    CHECK(hit->deci_target == 2040);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "heater off reports no target", "[ui][tooltip][target]") {
    ui_temp_graph_t* g = make_graph();
    int id = ui_temp_graph_add_series(g, "MCU", lv_color_hex(0x4444FF));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_update_series_with_time(g, id, 40.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);

    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    CHECK(hit->deci_target == 0);

    ui_temp_graph_destroy(g);
}

using helix::temp_graph_internal::temp_graph_tooltip_on_sample_pushed;
using helix::temp_graph_internal::temp_graph_tooltip_pin;
using helix::temp_graph_internal::temp_graph_tooltip_pinned;

TEST_CASE_METHOD(TooltipTestFixture, "tooltip is off by default", "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    CHECK_FALSE(ui_temp_graph_tooltip_is_enabled(g));
    CHECK(temp_graph_tooltip_pinned(g) == nullptr);
    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "pin rides left and dismisses off the edge",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_update_series_with_time(g, id, 150.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);
    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    temp_graph_tooltip_pin(g, *hit);

    const int pinned_at = temp_graph_tooltip_pinned(g)->logical_index;
    REQUIRE(pinned_at == g->point_count - 1);

    // Each push to THIS series walks the pin one slot left.
    ui_temp_graph_update_series_with_time(g, id, 151.0f, 1000000000000LL + 5 * 3000);
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);
    CHECK(temp_graph_tooltip_pinned(g)->logical_index == pinned_at - 1);
    // Text never changes: it still describes the original sample.
    CHECK(temp_graph_tooltip_pinned(g)->deci_temp == 1500);

    // Walk it off the left edge.
    for (int i = 0; i < g->point_count + 2; i++) {
        ui_temp_graph_update_series_with_time(g, id, 152.0f, 1000000000000LL + (6 + i) * 3000);
    }
    CHECK(temp_graph_tooltip_pinned(g) == nullptr);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "a push to another series does not move the pin",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int a = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    int b = ui_temp_graph_add_series(g, "Bed", lv_color_hex(0x44FF44));
    for (int i = 0; i < 5; i++) {
        int64_t ts = 1000000000000LL + i * 3000;
        ui_temp_graph_update_series_with_time(g, a, 200.0f, ts);
        ui_temp_graph_update_series_with_time(g, b, 60.0f, ts);
    }
    lv_point_t p = point_pos(g, a, g->point_count - 1);
    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    REQUIRE(hit->series_id == a);
    temp_graph_tooltip_pin(g, *hit);
    const int before = temp_graph_tooltip_pinned(g)->logical_index;

    ui_temp_graph_update_series_with_time(g, b, 61.0f, 1000000000000LL + 5 * 3000);
    CHECK(temp_graph_tooltip_pinned(g)->logical_index == before);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "hiding the pinned series dismisses it",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int a = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    int b = ui_temp_graph_add_series(g, "Bed", lv_color_hex(0x44FF44));
    for (int i = 0; i < 5; i++) {
        int64_t ts = 1000000000000LL + i * 3000;
        ui_temp_graph_update_series_with_time(g, a, 200.0f, ts);
        ui_temp_graph_update_series_with_time(g, b, 60.0f, ts);
    }
    lv_point_t p = point_pos(g, a, g->point_count - 1);
    temp_graph_tooltip_pin(g, *tooltip_hit_test(g, p.x, p.y));
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    ui_temp_graph_show_series(g, b, false); // unrelated series
    CHECK(temp_graph_tooltip_pinned(g) != nullptr);

    ui_temp_graph_show_series(g, a, false); // the pinned one
    CHECK(temp_graph_tooltip_pinned(g) == nullptr);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "disabling clears the pin", "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_update_series_with_time(g, id, 150.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);
    temp_graph_tooltip_pin(g, *tooltip_hit_test(g, p.x, p.y));
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    ui_temp_graph_set_tooltip_enabled(g, false);
    CHECK(temp_graph_tooltip_pinned(g) == nullptr);
    CHECK_FALSE(ui_temp_graph_tooltip_is_enabled(g));

    ui_temp_graph_destroy(g);
}

// temp_graph_tooltip_draw_cb is registered exactly once, unconditionally, at
// graph creation (ui_temp_graph.cpp) and is never re-added on enable. A
// disable must therefore leave it in place (only real teardown may sever it),
// or a re-enabled graph pins state on tap but draws nothing. This checks both
// directions at once: draw_cb must survive the disable, and enabling again
// must not have doubled it up.
TEST_CASE_METHOD(TooltipTestFixture, "re-enabling after a disable still draws a new pin",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_update_series_with_time(g, id, 150.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, id, g->point_count - 1);
    temp_graph_tooltip_pin(g, *tooltip_hit_test(g, p.x, p.y));
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    ui_temp_graph_set_tooltip_enabled(g, false);
    REQUIRE(temp_graph_tooltip_pinned(g) == nullptr);

    ui_temp_graph_set_tooltip_enabled(g, true);
    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    temp_graph_tooltip_pin(g, *hit);
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    int draw_cb_count = 0;
    const uint32_t count = lv_obj_get_event_count(g->chart);
    for (uint32_t i = 0; i < count; i++) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(g->chart, i);
        if (lv_event_dsc_get_cb(dsc) == helix::temp_graph_internal::temp_graph_tooltip_draw_cb) {
            draw_cb_count++;
        }
    }
    CHECK(draw_cb_count == 1);

    ui_temp_graph_destroy(g);
}

TEST_CASE_METHOD(TooltipTestFixture, "removing the pinned series dismisses it",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int a = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    for (int i = 0; i < 5; i++) {
        ui_temp_graph_update_series_with_time(g, a, 200.0f, 1000000000000LL + i * 3000);
    }
    lv_point_t p = point_pos(g, a, g->point_count - 1);
    temp_graph_tooltip_pin(g, *tooltip_hit_test(g, p.x, p.y));
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    ui_temp_graph_remove_series(g, a);
    CHECK(temp_graph_tooltip_pinned(g) == nullptr);

    ui_temp_graph_destroy(g);
}

// series_meta is indexed by SLOT (first free array position at add time), not
// by `id` (ui_temp_graph_add_series's return value, next_series_id++, never
// reused). Remove-then-add recycles the slot but not the id, so after this
// sequence: a=id0/slot0, b=id1/slot1, remove a (frees slot0), c=id2/slot0 -
// c's id (2) and slot (0) diverge. temp_graph_tooltip_draw_cb resolves a pin
// through find_meta_by_id, exactly like this test does; the bug it replaces
// (series_meta[pin->series_id], i.e. series_meta[2]) would land on an
// untouched slot - never populated, so a silently blank caption - which the
// assertions below confirm directly since a caption's actual pixels aren't
// observable from a headless unit test (see report for why the end-to-end
// draw path isn't exercised here).
TEST_CASE_METHOD(TooltipTestFixture, "pinned caption resolves the correct series after slot reuse",
                 "[ui][tooltip][lifecycle]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int a = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));  // id 0, slot 0
    int b = ui_temp_graph_add_series(g, "Bed", lv_color_hex(0x44FF44));     // id 1, slot 1
    ui_temp_graph_remove_series(g, a);                                      // frees slot 0
    int c = ui_temp_graph_add_series(g, "Chamber", lv_color_hex(0x4444FF)); // reuses slot 0, id 2
    REQUIRE(c != a);
    // id (c) and slot (0, the one `a` vacated) really do diverge here.
    REQUIRE(c != 0);
    REQUIRE(&g->series_meta[0] == find_meta_by_id(g, c));

    for (int i = 0; i < 5; i++) {
        int64_t ts = 1000000000000LL + i * 3000;
        ui_temp_graph_update_series_with_time(g, b, 60.0f, ts);
        ui_temp_graph_update_series_with_time(g, c, 210.0f, ts);
    }
    // point_pos indexes series_meta by SLOT, not id - slot 0 is now `c`'s.
    lv_point_t p = point_pos(g, /*slot=*/0, g->point_count - 1);
    auto hit = tooltip_hit_test(g, p.x, p.y);
    REQUIRE(hit.has_value());
    REQUIRE(hit->series_id == c); // hit_test already resolves by id correctly
    temp_graph_tooltip_pin(g, *hit);

    const auto* pin = temp_graph_tooltip_pinned(g);
    REQUIRE(pin != nullptr);
    REQUIRE(pin->series_id == c);

    const ui_temp_series_meta_t* meta = find_meta_by_id(g, pin->series_id);
    REQUIRE(meta != nullptr);
    CHECK(std::strcmp(meta->name, "Chamber") == 0);
    CHECK(meta->chart_series != nullptr);
    CHECK(meta->visible);

    // The slot the OLD (buggy) code would have indexed into instead
    // (series_meta[pin->series_id] == series_meta[2]) was never populated.
    REQUIRE(pin->series_id < UI_TEMP_GRAPH_MAX_SERIES);
    CHECK(g->series_meta[pin->series_id].chart_series == nullptr);
    CHECK_FALSE(g->series_meta[pin->series_id].visible);

    (void)b;
    ui_temp_graph_destroy(g);
}

// UAF regression (fix round 1 review): ui_temp_graph_destroy defers the chart's
// actual deletion via lv_obj_delete_async (L081 — see "destroy defers chart
// deletion" in test_temp_graph.cpp), so the chart stays alive, hidden, for one
// async tick after `g` is freed. Before this fix, tooltip_press_cb stayed
// registered on the chart with the now-freed `g` as its event user_data during
// that window; a CLICKED landing there would dereference freed memory.
//
// Rather than actually firing the stale callback — a real UAF read that may or
// may not crash depending on allocator/heap state without ASAN, i.e. a flaky
// assertion either way — this asserts the invariant directly: no event
// descriptor on the still-alive chart may carry the freed `g` pointer as its
// user_data. That is exactly, and only, what temp_graph_tooltip_destroy's
// severance call must guarantee.
//
// This loop is not scoped to tooltip_press_cb — it checks every descriptor on
// the chart — so it also covers temp_graph_tooltip_draw_cb (registered
// unconditionally at graph creation, same freed-`g` user_data hazard) without
// needing a second test.
TEST_CASE_METHOD(TooltipTestFixture, "destroy severs the press callback before chart deletion",
                 "[ui][tooltip][lifecycle][crash]") {
    ui_temp_graph_t* g = make_graph();
    ui_temp_graph_set_tooltip_enabled(g, true);
    int id = ui_temp_graph_add_series(g, "Nozzle", lv_color_hex(0xFF4444));
    ui_temp_graph_update_series_with_time(g, id, 150.0f, 1000000000000LL);
    lv_point_t p = point_pos(g, id, g->point_count - 1);
    temp_graph_tooltip_pin(g, *tooltip_hit_test(g, p.x, p.y));
    REQUIRE(temp_graph_tooltip_pinned(g) != nullptr);

    lv_obj_t* chart = g->chart; // captured before destroy frees `g`
    ui_temp_graph_destroy(g);

    const uint32_t count = lv_obj_get_event_count(chart);
    for (uint32_t i = 0; i < count; i++) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(chart, i);
        CHECK(lv_event_dsc_get_user_data(dsc) != static_cast<void*>(g));
    }
}

TEST_CASE("caption sits above the point when there is room", "[ui][tooltip][layout]") {
    helix::temp_graph_internal::temp_graph_geometry_t geo{};
    geo.cx1 = 100;
    geo.cy1 = 50;
    geo.cw = 400;
    geo.ch = 200;

    lv_area_t a = temp_graph_tooltip_box_area(geo, 300, 200, 120, 34);
    CHECK(a.y2 < 200);               // above the point
    CHECK((a.x1 + a.x2) / 2 == 300); // horizontally centered on it
}

TEST_CASE("caption flips below a point near the top", "[ui][tooltip][layout]") {
    helix::temp_graph_internal::temp_graph_geometry_t geo{};
    geo.cx1 = 100;
    geo.cy1 = 50;
    geo.cw = 400;
    geo.ch = 200;

    lv_area_t a = temp_graph_tooltip_box_area(geo, 300, 60, 120, 34);
    CHECK(a.y1 > 60);
}

TEST_CASE("caption clamps inside the plot horizontally", "[ui][tooltip][layout]") {
    helix::temp_graph_internal::temp_graph_geometry_t geo{};
    geo.cx1 = 100;
    geo.cy1 = 50;
    geo.cw = 400;
    geo.ch = 200;

    lv_area_t right = temp_graph_tooltip_box_area(geo, 498, 200, 120, 34);
    CHECK(right.x2 <= 500);
    lv_area_t left = temp_graph_tooltip_box_area(geo, 102, 200, 120, 34);
    CHECK(left.x1 >= 100);
}

// A long series name (meta->name is char[32], up to 31 characters) can measure
// wider than the plot on a 480x272 panel (~330px of content area). Both edges
// must hold at once — the original clamp-order bug satisfied the left edge
// while blowing past the right, so a one-sided assertion would have missed it.
TEST_CASE("caption shrinks to fit when wider than the plot", "[ui][tooltip][layout]") {
    helix::temp_graph_internal::temp_graph_geometry_t geo{};
    geo.cx1 = 100;
    geo.cy1 = 50;
    geo.cw = 400;
    geo.ch = 200;

    lv_area_t a = temp_graph_tooltip_box_area(geo, 300, 200, 450, 34);
    CHECK(a.x1 >= geo.cx1);
    CHECK(a.x2 <= geo.cx1 + geo.cw);
}

// Same defect, vertical axis.
TEST_CASE("caption shrinks to fit when taller than the plot", "[ui][tooltip][layout]") {
    helix::temp_graph_internal::temp_graph_geometry_t geo{};
    geo.cx1 = 100;
    geo.cy1 = 50;
    geo.cw = 400;
    geo.ch = 200;

    lv_area_t a = temp_graph_tooltip_box_area(geo, 300, 200, 120, 250);
    CHECK(a.y1 >= geo.cy1);
    CHECK(a.y2 <= geo.cy1 + geo.ch);
}
