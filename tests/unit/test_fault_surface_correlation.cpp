// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One firmware failure often reaches the console twice: the cause, then the
// same cause under its context. The router's toast dedup reads the second as
// the first (prestonbrown/helixscreen#2086) without letting a shared phrase
// hide a different fault.

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "core/update_queue.h"
#include "fault_surface_correlation.h"
#include "gcode_error_router.h"
#include "recovery_modal_presenter.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::fault_surface_correlation::only_wraps;

TEST_CASE("only_wraps finds a cause behind its context", "[gcode_error][fault_surface][2086]") {
    const std::string cause = "Failed to extract filament from extruder";

    CHECK(only_wraps(cause, cause));
    CHECK(only_wraps("Filament change error: " + cause, cause));
}

TEST_CASE("only_wraps leaves a text that says more than the cause",
          "[gcode_error][fault_surface][2086]") {
    const std::string cause = "Failed to extract filament from extruder";

    // Z-Mod's wrapper, verbatim from bundle KHACGM8W: the cancel is news.
    CHECK_FALSE(only_wraps("Filament change error: " + cause + "\nPrint cancelled", cause));
    CHECK_FALSE(only_wraps(cause + "\nHeater extruder not heating", cause));
    CHECK_FALSE(only_wraps("Print cancelled\n" + cause, cause));
    CHECK_FALSE(only_wraps("Move out of range: 300.000 0.000", "Move out of range: 300"));
    CHECK_FALSE(only_wraps("Heater extruder not heating at expected rate", "extruder"));
    CHECK_FALSE(only_wraps("Not " + cause, cause));
    CHECK_FALSE(only_wraps(cause + " 2", cause));
    CHECK_FALSE(only_wraps(": " + cause + "x", cause));
    CHECK_FALSE(only_wraps("anything", ""));
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

TEST_CASE_METHOD(LVGLUITestFixture, "a cause and its bare wrapper show one toast",
                 "[gcode_error][fault_surface][2086]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);

    GcodeErrorRouterTestAccess::process_line(router, "!! Failed to extract filament from extruder");
    GcodeErrorRouterTestAccess::process_line(
        router, "!! Filament change error: Failed to extract filament from extruder");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    REQUIRE(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 1);

    // A different fault in the same window still surfaces.
    GcodeErrorRouterTestAccess::process_line(router, "!! Purge bin is full");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    CHECK(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 2);
}

TEST_CASE_METHOD(LVGLUITestFixture, "a wrapper that adds a line after the cause still shows",
                 "[gcode_error][fault_surface][2086]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::GcodeErrorRouter router(nullptr, nullptr, presenter);

    GcodeErrorRouterTestAccess::process_line(router, "!! Failed to extract filament from extruder");
    // Z-Mod's own second message: the print was cancelled, which the user must see.
    GcodeErrorRouterTestAccess::process_line(
        router,
        "!! Filament change error: Failed to extract filament from extruder\nPrint cancelled");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(250);
    CHECK(GcodeErrorRouterTestAccess::deferred_shown_count(router) == 2);
}
