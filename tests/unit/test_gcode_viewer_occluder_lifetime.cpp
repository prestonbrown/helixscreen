// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The bottom occluder (the translucent metadata strip that crops the preview)
// and the viewer that measures it are SIBLINGS in one subtree, so teardown can
// destroy them in either order.
//
// ui_gcode_viewer_set_bottom_occluder() registers an LV_EVENT_DELETE handler
// ON the occluder that reaches back INTO the viewer through user_data. That
// covers the occluder-dies-first order. The other order was unguarded:
// obj_delete_core (lv_obj_tree.c:761) sends LV_EVENT_DELETE, then clears only
// the deleted object's OWN event list, then recurses children, then lv_free()s.
// A handler living in the occluder's list therefore survives the viewer and
// fires later in the same recursion, dereferencing freed memory.
//
// The viewer is the earlier sibling in both layouts that set an occluder
// (print_status_preview_card.xml:29 before :154, print_file_detail.xml:39
// before :78), so the viewer always went first. Three field crashes on
// AD5M/AD5X, where MALLOC_PERTURB_=165 makes the freed read come back as
// 0xa5a5a5a5 (non-null, so the handler's `if (st)` guard passes) and the store
// lands at 0xa5a5a6ad (prestonbrown/helixscreen#1347).
//
// Note this cannot be pinned by "does it crash": the viewer's own delete
// handler nulls its user_data before the object is freed, so without the
// allocator poison the stale handler reads a benign nullptr. What is asserted
// here is the invariant itself - after either object is destroyed, no callback
// pointing at it may remain registered on the other.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"

#include <cstdint>

#include "../catch_amalgamated.hpp"

namespace {

/// Counts callbacks on `obj` whose user_data is `addr`. Compares addresses
/// only - `addr` is expected to be a freed pointer at the call site.
uint32_t callbacks_pointing_at(lv_obj_t* obj, uintptr_t addr) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && reinterpret_cast<uintptr_t>(lv_event_dsc_get_user_data(dsc)) == addr) {
            hits++;
        }
    }
    return hits;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "gcode viewer: deleting the viewer detaches its occluder callback",
                 "[gcode_viewer][occluder][lifetime][1347]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);

    // Sibling order mirrors the shipped layouts: viewer first, occluder second.
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_t* occluder = lv_obj_create(parent);
    REQUIRE(occluder != nullptr);

    const uint32_t baseline = lv_obj_get_event_count(occluder);
    const uintptr_t viewer_addr = reinterpret_cast<uintptr_t>(viewer);

    ui_gcode_viewer_set_bottom_occluder(viewer, occluder);
    REQUIRE(callbacks_pointing_at(occluder, viewer_addr) == 1);

    lv_obj_delete(viewer);

    // The occluder outlives the viewer. Any surviving callback keyed to the
    // freed viewer detonates when the occluder is itself deleted.
    CHECK(callbacks_pointing_at(occluder, viewer_addr) == 0);
    CHECK(lv_obj_get_event_count(occluder) == baseline);

    // The order the field crash actually took: occluder destroyed after the
    // viewer, inside one obj_delete_core recursion over the shared parent.
    lv_obj_delete(parent);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "gcode viewer: deleting the occluder first leaves no reference to walk",
                 "[gcode_viewer][occluder][lifetime][1347]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_t* occluder = lv_obj_create(parent);
    REQUIRE(occluder != nullptr);

    ui_gcode_viewer_set_bottom_occluder(viewer, occluder);

    // Reverse order. The occluder's own handler must clear the viewer's stored
    // pointer, or the viewer's delete path below walks freed memory trying to
    // detach from it (a UAF ASAN would catch; benign-looking otherwise).
    lv_obj_delete(occluder);
    lv_obj_delete(viewer);

    SUCCEED("both destruction orders complete without touching freed memory");
}

TEST_CASE_METHOD(LVGLTestFixture, "gcode viewer: replacing the occluder detaches the old one",
                 "[gcode_viewer][occluder][lifetime][1347]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    REQUIRE(parent != nullptr);
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_t* first = lv_obj_create(parent);
    lv_obj_t* second = lv_obj_create(parent);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    const uintptr_t viewer_addr = reinterpret_cast<uintptr_t>(viewer);

    ui_gcode_viewer_set_bottom_occluder(viewer, first);
    REQUIRE(callbacks_pointing_at(first, viewer_addr) == 1);

    // A strip swapped out must stop reporting, or its later delete clears a
    // reference that now belongs to the replacement.
    ui_gcode_viewer_set_bottom_occluder(viewer, second);
    CHECK(callbacks_pointing_at(first, viewer_addr) == 0);
    CHECK(callbacks_pointing_at(second, viewer_addr) == 1);

    lv_obj_delete(parent);
}
