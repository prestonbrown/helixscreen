// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_state_data_isolation.cpp
 * @brief PrinterState's plain (non-subject) data must not cross a test boundary.
 *
 * get_printer_state() is a Meyers singleton: it lives for the whole test binary.
 * Its thirteen domain components pair init_subjects() with deinit_subjects(), and
 * those manage the SUBJECTS only — the plain members alongside them (the
 * excluded/defined object sets, the hardware validation result, the selected
 * printer type) have no lifetime hook at all. Nothing ever cleared them, so a
 * test that wrote one poisoned every later test in the same binary, silently:
 * the next test read a plausible value it never set.
 *
 * HelixTestFixture::reset_all() now calls PrinterStateTestAccess::clear_data() in
 * both its constructor and its destructor, so every fixture in the suite gets a
 * clean singleton without its author having to know these members exist.
 *
 * These tests fail if that call is removed.
 */

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "hardware_validator.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Write plain members across three domains, the way an ordinary test does.
void dirty_printer_state() {
    PrinterState& ps = get_printer_state();

    // PrinterExcludedObjectsState — the reported case. All four members are
    // plain containers behind a version subject.
    ps.get_excluded_objects_state()->set_defined_objects({"Part_A", "Part_B"});
    ps.set_excluded_objects({"Part_A"});
    ps.get_excluded_objects_state()->set_current_object("Part_B");

    // PrinterHardwareValidationState — a struct member, not a subject.
    HardwareValidationResult result;
    result.critical_missing.push_back(
        HardwareIssue::critical("heater_bed", HardwareType::HEATER, "missing"));
    ps.set_hardware_validation_result(result);

    // PrinterState's own printer_type_.
    ps.set_printer_type_sync("Voron 2.4");
}

/// Assert none of it survived.
void require_clean_printer_state() {
    PrinterState& ps = get_printer_state();

    CHECK(ps.get_excluded_objects_state()->get_defined_objects().empty());
    CHECK(ps.get_excluded_objects().empty());
    CHECK(ps.get_excluded_objects_state()->get_current_object().empty());
    CHECK_FALSE(ps.get_excluded_objects_state()->has_objects());
    CHECK_FALSE(ps.get_hardware_validation_result().has_issues());
    CHECK(ps.get_printer_type().empty());
}

} // namespace

// A TEST_CASE boundary is exactly a fixture destructor followed by the next
// fixture's constructor, so two scoped fixtures reproduce it without depending on
// Catch2 running the cases in declaration order (--order rand would break that).
TEST_CASE("PrinterState plain data does not survive a fixture boundary",
          "[core][isolation][printer_state]") {
    {
        LVGLTestFixture writer;
        get_printer_state().init_subjects(false);
        dirty_printer_state();

        // The setup must actually have taken, or the assertions below are vacuous
        // against a PrinterState that simply rejected every write.
        REQUIRE(get_printer_state().get_excluded_objects_state()->has_objects());
        REQUIRE(get_printer_state().get_excluded_objects().count("Part_A") == 1);
        REQUIRE(get_printer_state().get_hardware_validation_result().has_issues());
        REQUIRE(get_printer_state().get_printer_type() == "Voron 2.4");
    }

    {
        LVGLTestFixture reader;
        get_printer_state().init_subjects(false);
        require_clean_printer_state();
    }
}

// The realistic shape: one TEST_CASE dirties the singleton, the next asserts it is
// clean. Order-dependent by construction (that IS the bug being pinned), so it is
// only meaningful under Catch2's default declaration order.
TEST_CASE("PrinterState data isolation: writer", "[core][isolation][printer_state]") {
    LVGLTestFixture fx;
    get_printer_state().init_subjects(false);
    dirty_printer_state();
    REQUIRE(get_printer_state().get_excluded_objects_state()->has_objects());
}

TEST_CASE("PrinterState data isolation: successor sees a clean singleton",
          "[core][isolation][printer_state]") {
    LVGLTestFixture fx;
    get_printer_state().init_subjects(false);
    require_clean_printer_state();
}

// clear_data() must be safe when the subject tree was never built — it runs from
// HelixTestFixture's ctor/dtor, which non-LVGL tests inherit too.
TEST_CASE("clear_data is safe with subjects torn down", "[core][isolation][printer_state]") {
    LVGLTestFixture fx;
    get_printer_state().init_subjects(false);
    dirty_printer_state();
    get_printer_state().deinit_subjects();

    REQUIRE_NOTHROW(PrinterStateTestAccess::clear_data(get_printer_state()));
    require_clean_printer_state();
}
