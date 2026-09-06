// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rendering guards for the filament_path_canvas detail panel.
//
// The widget paints its content into two lv_canvas children from a deferred
// refresh (layered_refresh), which early-returns while the widget still has a
// zero layout size. The widget must re-schedule that refresh once
// layout assigns a non-zero size (LV_EVENT_SIZE_CHANGED handler); otherwise the
// canvas stays permanently blank when the async outruns the first layout pass.
//
// These tests assert: (1) MIXED and PARALLEL topologies paint non-zero opaque
// overlay pixels once laid out, and (2) the canvas repaints when layout lands
// AFTER the create-time refresh already bailed pre-layout — the SIZE_CHANGED
// handler must re-schedule the refresh. (2) fails if that re-schedule is removed.

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {

// Find the overlay canvas child of the path-canvas widget. The widget creates
// two lv_canvas children in order: [0] static topology, [1] state overlay.
// MIXED draws all of its lanes/dots/hub/nozzles into the overlay canvas.
lv_obj_t* overlay_canvas_of(lv_obj_t* widget) {
    lv_obj_t* last = nullptr;
    uint32_t n = lv_obj_get_child_count(widget);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(widget, i);
        if (lv_obj_check_type(c, &lv_canvas_class))
            last = c; // overlay is the second (last-created) canvas
    }
    return last;
}

// Count pixels with non-zero alpha in the canvas image buffer. The overlay is
// ARGB8888 and cleared fully transparent each render, so any drawn geometry
// shows up as alpha > 0.
long count_opaque_pixels(lv_obj_t* canvas) {
    if (!canvas)
        return -1;
    int32_t w = lv_obj_get_width(canvas);
    int32_t h = lv_obj_get_height(canvas);
    long count = 0;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            lv_color32_t px = lv_canvas_get_px(canvas, x, y);
            if (px.alpha != 0)
                ++count;
        }
    }
    return count;
}

// Configure the widget as a single-unit MIXED detail panel: 4 lanes, the last
// two hub-routed, with installed filament so lanes draw in filament color.
void configure_mixed(lv_obj_t* w) {
    ui_filament_path_canvas_set_slot_count(w, 4);
    ui_filament_path_canvas_set_topology(w, static_cast<int>(PathTopology::MIXED));
    // {direct, direct, hub-routed, hub-routed}
    ui_filament_path_canvas_set_slot_hub_routed(w, 0, false);
    ui_filament_path_canvas_set_slot_hub_routed(w, 1, false);
    ui_filament_path_canvas_set_slot_hub_routed(w, 2, true);
    ui_filament_path_canvas_set_slot_hub_routed(w, 3, true);
    // Installed filament on each lane (non-NONE segment so lanes are "filled").
    ui_filament_path_canvas_set_slot_filament(w, 0, static_cast<int>(PathSegment::NOZZLE),
                                              0xE53935);
    ui_filament_path_canvas_set_slot_filament(w, 1, static_cast<int>(PathSegment::SPOOL), 0x1E88E5);
    ui_filament_path_canvas_set_slot_filament(w, 2, static_cast<int>(PathSegment::NOZZLE),
                                              0x43A047);
    ui_filament_path_canvas_set_slot_filament(w, 3, static_cast<int>(PathSegment::SPOOL), 0xFDD835);
    ui_filament_path_canvas_set_slot_mapped_tool(w, 0, 0);
    ui_filament_path_canvas_set_slot_mapped_tool(w, 1, 1);
    ui_filament_path_canvas_set_slot_mapped_tool(w, 2, 2);
    ui_filament_path_canvas_set_slot_mapped_tool(w, 3, 2);
    ui_filament_path_canvas_set_active_slot(w, 0);
}

} // namespace

// The widget renders its canvas children from an lv_async_call that early-
// returns while the widget still has zero layout coords. In the live app a
// display refresh lays the widget out before that async fires; headless we must
// force the layout pass ourselves (then process_lvgl drives the async + render).
#define FORCE_RENDER()                                                                             \
    do {                                                                                           \
        lv_obj_update_layout(test_screen());                                                       \
        process_lvgl(120);                                                                         \
    } while (0)

// Control: PARALLEL topology (verified-working per the bug report) must paint
// pixels with the SAME harness. If this is blank too, the harness — not the
// renderer — is at fault.
TEST_CASE_METHOD(LVGLTestFixture, "PARALLEL detail canvas renders pixels (harness control)",
                 "[filament-path][canvas][mixed][regression]") {
    lv_obj_t* w = ui_filament_path_canvas_create(test_screen());
    REQUIRE(w != nullptr);
    lv_obj_set_size(w, 400, 240);

    ui_filament_path_canvas_set_slot_count(w, 4);
    ui_filament_path_canvas_set_topology(w, static_cast<int>(PathTopology::PARALLEL));
    ui_filament_path_canvas_set_slot_filament(w, 0, static_cast<int>(PathSegment::NOZZLE),
                                              0xE53935);
    ui_filament_path_canvas_set_slot_filament(w, 1, static_cast<int>(PathSegment::SPOOL), 0x1E88E5);
    ui_filament_path_canvas_set_active_slot(w, 0);

    FORCE_RENDER();

    lv_obj_t* overlay = overlay_canvas_of(w);
    REQUIRE(overlay != nullptr);
    long pixels = count_opaque_pixels(overlay);
    INFO("PARALLEL opaque overlay pixels: " << pixels);
    REQUIRE(pixels > 200);
}

TEST_CASE_METHOD(LVGLTestFixture, "MIXED detail canvas renders pixels (hub_only=true)",
                 "[filament-path][canvas][mixed][regression]") {
    lv_obj_t* w = ui_filament_path_canvas_create(test_screen());
    REQUIRE(w != nullptr);
    lv_obj_set_size(w, 400, 240);

    configure_mixed(w);
    ui_filament_path_canvas_set_hub_only(w, true);

    // Let the async canvas refresh + render pass run.
    FORCE_RENDER();

    lv_obj_t* overlay = overlay_canvas_of(w);
    REQUIRE(overlay != nullptr);

    long pixels = count_opaque_pixels(overlay);
    INFO("hub_only=true opaque overlay pixels: " << pixels);
    // A populated MIXED panel paints lanes, sensor dots, the hub box, and
    // nozzles — hundreds of pixels minimum. Blank canvas (the bug) is 0.
    REQUIRE(pixels > 200);
}

TEST_CASE_METHOD(LVGLTestFixture, "MIXED detail canvas renders pixels (hub_only=false)",
                 "[filament-path][canvas][mixed][regression]") {
    lv_obj_t* w = ui_filament_path_canvas_create(test_screen());
    REQUIRE(w != nullptr);
    lv_obj_set_size(w, 400, 240);

    configure_mixed(w);
    ui_filament_path_canvas_set_hub_only(w, false);

    FORCE_RENDER();

    lv_obj_t* overlay = overlay_canvas_of(w);
    REQUIRE(overlay != nullptr);

    long pixels = count_opaque_pixels(overlay);
    INFO("hub_only=false opaque overlay pixels: " << pixels);
    REQUIRE(pixels > 200);
}

// Pins the blank-canvas race fix. Reproduces the live ordering where the
// initial refresh (scheduled at create time) runs BEFORE layout assigns the
// widget a real size: layered_refresh() early-returns on its `w <= 0` guard and
// the pending refresh is consumed. The only thing that can repaint the
// canvas afterwards is the widget re-scheduling the refresh when layout finally
// gives it a non-zero size (LV_EVENT_SIZE_CHANGED handler). Without that
// handler the canvas stays blank forever — the freshly laid-out widget has no
// pending refresh — so pixels == 0 and this fails.
TEST_CASE_METHOD(LVGLTestFixture,
                 "canvas repaints when layout follows a pre-layout refresh (race fix)",
                 "[filament-path][canvas][mixed][race]") {
    lv_obj_t* w = ui_filament_path_canvas_create(test_screen());
    REQUIRE(w != nullptr);
    lv_obj_set_size(w, 400, 240);

    configure_mixed(w);
    ui_filament_path_canvas_set_hub_only(w, true);

    // Drain the create-time async while the widget still has zero layout size
    // (width is 0 until lv_obj_update_layout runs). The refresh bails on its
    // w<=0 guard and is consumed — the canvas is blank at this point.
    process_lvgl(20);
    REQUIRE(lv_obj_get_width(w) == 0); // confirms the async outran layout

    // Now layout assigns the real size, firing LV_EVENT_SIZE_CHANGED. The fix
    // re-schedules the refresh here; the next pump must render. (Pre-fix: no
    // re-schedule, nothing repaints, overlay stays empty.)
    lv_obj_update_layout(test_screen());
    process_lvgl(120);

    lv_obj_t* overlay = overlay_canvas_of(w);
    REQUIRE(overlay != nullptr);

    long pixels = count_opaque_pixels(overlay);
    INFO("post-layout opaque overlay pixels: " << pixels);
    REQUIRE(pixels > 200);
}
