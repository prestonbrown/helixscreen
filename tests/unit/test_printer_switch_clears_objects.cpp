// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_switch_clears_objects.cpp
 * @brief exclude_object state is dropped when the active printer changes
 *
 * Run with: ./build/bin/helix-tests "[exclude_object][printer_switch]"
 *
 * PrinterState is a Meyers singleton that outlives every printer switch, and
 * update_from_status() only refreshes exclude_object state when the incoming
 * payload actually carries an "exclude_object" object
 * (src/printer/printer_state.cpp:440). Switching to a printer that has no
 * [exclude_object] section therefore never overwrote the sets, and the previous
 * printer's object list stayed on screen indefinitely.
 *
 * The fix hangs excluded_objects_state_.clear_objects() off the PrinterCacheRegistry
 * invalidator that PrinterState already registers. These tests drive the real
 * invalidate_all() entry point rather than calling clear_objects() directly, so
 * unhooking the invalidator fails them.
 */

#include "../lvgl_ui_test_fixture.h"
#include "printer_cache_registry.h"
#include "printer_excluded_objects_state.h"
#include "printer_state.h"

#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

/// Two objects with geometry, one of them excluded and one printing — the state a
/// printer mid-print leaves behind.
void seed_two_objects(helix::PrinterExcludedObjectsState& eo) {
    std::vector<helix::PrinterExcludedObjectsState::ObjectInfo> objects = {
        {"benchy", {100.0f, 100.0f}, {80.0f, 80.0f}, {120.0f, 120.0f}, {}, true, true},
        {"cube", {200.0f, 150.0f}, {180.0f, 130.0f}, {220.0f, 170.0f}, {}, true, true},
    };
    eo.set_defined_objects_with_geometry(objects);
    eo.set_excluded_objects({"benchy"});
    eo.set_current_object("cube");
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "exclude_object state survives an unrelated status update",
                 "[exclude_object][printer_switch]") {
    auto* eo = state().get_excluded_objects_state();
    REQUIRE(eo != nullptr);
    seed_two_objects(*eo);
    REQUIRE(eo->get_defined_objects().size() == 2);

    // The gate that made the bug possible, pinned deliberately: a payload with no
    // "exclude_object" key leaves the sets untouched. This is correct on its own —
    // Moonraker sends partial status updates constantly — which is exactly why the
    // clearing has to be driven by the printer switch instead.
    json status = {{"print_stats", {{"state", "printing"}}}};
    state().update_from_status(status);

    CHECK(eo->get_defined_objects().size() == 2);
    CHECK(eo->get_excluded_objects().count("benchy") == 1);
    CHECK(eo->get_current_object() == "cube");
}

TEST_CASE_METHOD(LVGLUITestFixture, "switching printers clears every exclude_object field",
                 "[exclude_object][printer_switch]") {
    auto* eo = state().get_excluded_objects_state();
    REQUIRE(eo != nullptr);
    seed_two_objects(*eo);
    REQUIRE(eo->has_objects());
    REQUIRE(eo->get_object_geometry("benchy").has_value());

    // The real path Application::switch_printer() takes.
    helix::PrinterCacheRegistry::instance().invalidate_all();

    CHECK(eo->get_defined_objects().empty());
    CHECK(eo->get_excluded_objects().empty());
    CHECK(eo->get_current_object().empty());
    CHECK_FALSE(eo->has_objects());
    // Geometry is a separate map from the name list; clearing one without the other
    // would leave the map view able to draw a shape for an object nothing lists.
    CHECK_FALSE(eo->get_object_geometry("benchy").has_value());
    CHECK_FALSE(eo->get_object_geometry("cube").has_value());
}

TEST_CASE_METHOD(LVGLUITestFixture, "switching printers notifies exclude_object observers",
                 "[exclude_object][printer_switch]") {
    auto* eo = state().get_excluded_objects_state();
    REQUIRE(eo != nullptr);
    seed_two_objects(*eo);

    // Without a version bump the sets empty out but the map view keeps drawing the
    // old objects, which looks identical to the bug being fixed.
    const int defined_before = lv_subject_get_int(eo->get_defined_objects_version_subject());
    const int excluded_before = lv_subject_get_int(eo->get_excluded_objects_version_subject());

    helix::PrinterCacheRegistry::instance().invalidate_all();

    CHECK(lv_subject_get_int(eo->get_defined_objects_version_subject()) > defined_before);
    CHECK(lv_subject_get_int(eo->get_excluded_objects_version_subject()) > excluded_before);
}

TEST_CASE_METHOD(LVGLUITestFixture, "clearing exclude_object state is idempotent",
                 "[exclude_object][printer_switch]") {
    auto* eo = state().get_excluded_objects_state();
    REQUIRE(eo != nullptr);
    REQUIRE(eo->get_defined_objects().empty());

    // A switch between two printers that both lack [exclude_object] must not bump
    // versions — the setters' change detection is what keeps this from waking every
    // observer on an unrelated printer switch.
    const int defined_before = lv_subject_get_int(eo->get_defined_objects_version_subject());

    helix::PrinterCacheRegistry::instance().invalidate_all();

    CHECK(eo->get_defined_objects().empty());
    CHECK(lv_subject_get_int(eo->get_defined_objects_version_subject()) == defined_before);
}
