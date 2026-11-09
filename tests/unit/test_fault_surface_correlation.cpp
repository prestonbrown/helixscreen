// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One firmware failure often reaches the console twice: the cause, then the
// same cause under its context. The router's toast dedup reads the second as
// the first (prestonbrown/helixscreen#2086) without letting a shared phrase
// hide a different fault.

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
#include "recovery_modal_presenter.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::fault_surface_correlation::contains_as_segment;

TEST_CASE("contains_as_segment finds a cause wrapped in its context",
          "[gcode_error][fault_surface][2086]") {
    const std::string cause = "Failed to extract filament from extruder";

    CHECK(contains_as_segment(cause, cause));
    // Z-Mod's wrapper, verbatim from bundle KHACGM8W.
    CHECK(contains_as_segment(
        "Filament change error: Failed to extract filament from extruder\nPrint cancelled", cause));
    CHECK(contains_as_segment("Filament change error: " + cause, cause));
    CHECK(contains_as_segment("Print cancelled\n" + cause, cause));
}

TEST_CASE("contains_as_segment does not match a phrase inside another sentence",
          "[gcode_error][fault_surface][2086]") {
    CHECK_FALSE(contains_as_segment("Move out of range: 300.000 0.000", "Move out of range: 300"));
    CHECK_FALSE(contains_as_segment("Heater extruder not heating at expected rate", "extruder"));
    CHECK_FALSE(
        contains_as_segment("Not Failed to extract filament", "Failed to extract filament"));
    CHECK_FALSE(contains_as_segment("Failed to extract filament from extruder 2",
                                    "Failed to extract filament from extruder"));
    CHECK_FALSE(contains_as_segment("short", "a longer needle"));
    CHECK_FALSE(contains_as_segment("anything", ""));
}

TEST_CASE("was_recently_surfaced_within matches a wrapper, the exact check does not",
          "[gcode_error][fault_surface][2086]") {
    namespace fsc = helix::fault_surface_correlation;
    fsc::clear_for_test();
    fsc::record_surfaced("Failed to extract filament from extruder");

    const std::string wrapper = "Filament change error: Failed to extract filament from extruder";
    CHECK(fsc::was_recently_surfaced_within(wrapper));
    CHECK_FALSE(fsc::was_recently_surfaced(wrapper));
    CHECK_FALSE(fsc::was_recently_surfaced_within("Heater extruder not heating at expected rate"));
    fsc::clear_for_test();
}

TEST_CASE_METHOD(LVGLUITestFixture, "one Z-Mod filament-change failure shows one toast",
                 "[gcode_error][fault_surface][2086]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);

    GcodeErrorRouterTestAccess::process_line(router, "!! Failed to extract filament from extruder");
    GcodeErrorRouterTestAccess::process_line(
        router,
        "!! Filament change error: Failed to extract filament from extruder\nPrint cancelled");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);

    // A different fault in the same window still surfaces.
    GcodeErrorRouterTestAccess::process_line(router, "!! Purge bin is full");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    CHECK(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 2);
}
