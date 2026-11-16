// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_modals_char.cpp
 * @brief Characterization tests for print status panel modal classes
 *
 * These tests document the EXISTING behavior of modal classes before extraction.
 * Run with: ./build/bin/helix-tests "[characterization][modals]"
 *
 * Modal classes tested:
 * - PrintCancelModal: Confirmation dialog for canceling an active print
 * - ExcludeObjectModal: Confirmation dialog for excluding objects during print
 * - RunoutGuidanceModal: Multi-button modal for filament runout handling
 */

#include "ui_panel_print_status.h"

#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Wrapper Classes - Expose protected methods for testing
// ============================================================================

/**
 * @brief Test wrapper that exposes protected hook methods for direct testing
 *
 * These wrappers allow characterization tests to verify callback behavior
 * without needing full LVGL/XML infrastructure for button wiring.
 */
class TestablePrintCancelModal : public PrintCancelModal {
  public:
    using PrintCancelModal::on_cancel;
    using PrintCancelModal::on_ok;
};

class TestableExcludeObjectModal : public ExcludeObjectModal {
  public:
    using ExcludeObjectModal::on_cancel;
    using ExcludeObjectModal::on_ok;
};

class TestableRunoutGuidanceModal : public RunoutGuidanceModal {
  public:
    using RunoutGuidanceModal::on_cancel;
    using RunoutGuidanceModal::on_ok;
    using RunoutGuidanceModal::on_quaternary;
    using RunoutGuidanceModal::on_quinary;
    using RunoutGuidanceModal::on_senary;
    using RunoutGuidanceModal::on_tertiary;
};

// ============================================================================
// CHARACTERIZATION: PrintCancelModal
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "CHAR: PrintCancelModal callback invocation",
                 "[characterization][modals]") {
    TestablePrintCancelModal modal;
    bool confirm_called = false;

    modal.set_on_confirm([&]() { confirm_called = true; });

    SECTION("on_ok triggers confirm callback") {
        // Call the hook directly (bypassing show/button wiring)
        modal.on_ok();
        REQUIRE(confirm_called == true);
    }

    SECTION("on_ok with no callback doesn't crash") {
        TestablePrintCancelModal modal2;
        REQUIRE_NOTHROW(modal2.on_ok());
    }

    SECTION("on_cancel with no callback doesn't crash") {
        // Default on_cancel from Modal base class just calls hide()
        TestablePrintCancelModal modal2;
        REQUIRE_NOTHROW(modal2.on_cancel());
    }
}

// ============================================================================
// CHARACTERIZATION: ExcludeObjectModal
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "CHAR: ExcludeObjectModal callback invocation",
                 "[characterization][modals]") {
    TestableExcludeObjectModal modal;
    bool confirm_called = false;
    bool cancel_called = false;

    modal.set_on_confirm([&]() { confirm_called = true; });
    modal.set_on_cancel([&]() { cancel_called = true; });

    SECTION("on_ok triggers confirm callback") {
        modal.on_ok();
        REQUIRE(confirm_called == true);
        REQUIRE(cancel_called == false);
    }

    SECTION("on_cancel triggers cancel callback") {
        modal.on_cancel();
        REQUIRE(cancel_called == true);
        REQUIRE(confirm_called == false);
    }

    SECTION("on_ok with no callback doesn't crash") {
        TestableExcludeObjectModal modal2;
        REQUIRE_NOTHROW(modal2.on_ok());
    }

    SECTION("on_cancel with no callback doesn't crash") {
        TestableExcludeObjectModal modal2;
        REQUIRE_NOTHROW(modal2.on_cancel());
    }
}

// ============================================================================
// CHARACTERIZATION: RunoutGuidanceModal
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "CHAR: RunoutGuidanceModal callback invocation",
                 "[characterization][modals]") {
    TestableRunoutGuidanceModal modal;
    bool load_called = false;
    bool unload_called = false;
    bool purge_called = false;
    bool resume_called = false;
    bool cancel_print_called = false;
    bool ok_dismiss_called = false;

    modal.set_on_load_filament([&]() { load_called = true; });
    modal.set_on_unload_filament([&]() { unload_called = true; });
    modal.set_on_purge([&]() { purge_called = true; });
    modal.set_on_resume([&]() { resume_called = true; });
    modal.set_on_cancel_print([&]() { cancel_print_called = true; });
    modal.set_on_ok_dismiss([&]() { ok_dismiss_called = true; });

    SECTION("on_ok triggers load_filament callback") {
        // on_ok() maps to "Load Filament" button
        modal.on_ok();
        REQUIRE(load_called == true);
        REQUIRE(unload_called == false);
    }

    SECTION("on_cancel triggers resume callback") {
        // on_cancel() maps to "Resume" button
        modal.on_cancel();
        REQUIRE(resume_called == true);
        REQUIRE(cancel_print_called == false);
    }

    SECTION("on_tertiary triggers cancel_print callback") {
        // on_tertiary() maps to "Cancel Print" button
        modal.on_tertiary();
        REQUIRE(cancel_print_called == true);
        REQUIRE(resume_called == false);
    }

    SECTION("on_quaternary triggers unload_filament callback") {
        // on_quaternary() maps to "Unload Filament" button
        modal.on_quaternary();
        REQUIRE(unload_called == true);
        REQUIRE(load_called == false);
    }

    SECTION("on_quinary triggers purge callback") {
        // on_quinary() maps to "Purge" button
        modal.on_quinary();
        REQUIRE(purge_called == true);
    }

    SECTION("on_senary triggers ok_dismiss callback") {
        // on_senary() maps to "OK" button (dismiss when idle)
        modal.on_senary();
        REQUIRE(ok_dismiss_called == true);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "CHAR: RunoutGuidanceModal null callbacks don't crash",
                 "[characterization][modals]") {
    TestableRunoutGuidanceModal modal;
    // No callbacks set - all should be nullptr safe

    REQUIRE_NOTHROW(modal.on_ok());
    REQUIRE_NOTHROW(modal.on_cancel());
    REQUIRE_NOTHROW(modal.on_tertiary());
    REQUIRE_NOTHROW(modal.on_quaternary());
    REQUIRE_NOTHROW(modal.on_quinary());
    REQUIRE_NOTHROW(modal.on_senary());
}

// ============================================================================
// CHARACTERIZATION: Modal Base Class Behavior
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "CHAR: Modal hide() is safe when not visible",
                 "[characterization][modals]") {
    // Calling hide() on an already-hidden modal should be safe
    PrintCancelModal modal;
    REQUIRE_NOTHROW(modal.hide());
    REQUIRE_NOTHROW(modal.hide()); // Double-hide should also be safe
}

// ============================================================================
// Documentation: Modal Pattern Summary
// ============================================================================

/**
 * SUMMARY OF PRINT STATUS MODAL PATTERNS:
 *
 * 1. PrintCancelModal (Simple confirmation):
 *    - Single confirm callback
 *    - on_ok() calls callback then hides
 *    - on_cancel() just hides (inherited from Modal)
 *
 * 2. ExcludeObjectModal (Confirmation with both callbacks):
 *    - Separate confirm and cancel callbacks
 *    - on_ok() calls confirm callback then hides
 *    - on_cancel() calls cancel callback then hides
 *    - Has set_object_name() for dynamic content
 *
 * 3. RunoutGuidanceModal (Multi-button modal):
 *    - 6 different callbacks for different actions
 *    - Button mapping:
 *      - on_ok() = Load Filament (hides)
 *      - on_cancel() = Resume (hides)
 *      - on_tertiary() = Cancel Print (hides)
 *      - on_quaternary() = Unload Filament (does NOT hide - user may load after)
 *      - on_quinary() = Purge (does NOT hide - user may purge multiple times)
 *      - on_senary() = OK dismiss (hides)
 *
 * KEY OBSERVATIONS:
 * - All modals inherit from Modal base class
 * - Callbacks are std::function<void()> - no parameters
 * - nullptr callbacks are safely handled (no-op)
 * - Modals can be used without showing (for testing callback logic)
 * - RAII: destructor calls hide() if visible
 */
