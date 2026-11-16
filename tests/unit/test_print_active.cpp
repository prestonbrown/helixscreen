// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_active.cpp
 * @brief Unit tests for PrinterState::print_active subject
 *
 * Tests the derived print_active subject that combines PRINTING/PAUSED states
 * into a single boolean for simpler XML bindings.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "printer_print_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

// ============================================================================
// Test Fixture
// ============================================================================

class PrintActiveTestFixture : public LVGLTestFixture {
  public:
    PrintActiveTestFixture() {
        // Reset PrinterState for test isolation
        PrinterStateTestAccess::reset(state_);

        // Initialize subjects (without XML registration in tests)
        state_.init_subjects(false);
    }

    ~PrintActiveTestFixture() {
        // Reset after each test
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    // Helper to update print state via status JSON
    void set_print_state(const char* state_str) {
        json status = {{"print_stats", {{"state", state_str}}}};
        state_.update_from_status(status);
    }

    // Get current print_active value
    int get_print_active() {
        return lv_subject_get_int(state_.print_state().get_print_active_subject());
    }

    // Get current print_state_enum value
    PrintJobState get_print_state_enum() {
        return static_cast<PrintJobState>(
            lv_subject_get_int(state_.print_state().get_print_state_enum_subject()));
    }

  private:
    PrinterState state_;
};

// ============================================================================
// Test Cases
// ============================================================================

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: initial state is 0",
                 "[core][printer_state][print_active]") {
    // After init, print_active should be 0 (not printing)
    REQUIRE(get_print_active() == 0);
    REQUIRE(get_print_state_enum() == PrintJobState::STANDBY);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: STANDBY -> print_active=0",
                 "[core][printer_state][print_active]") {
    set_print_state("standby");
    REQUIRE(get_print_active() == 0);
    REQUIRE(get_print_state_enum() == PrintJobState::STANDBY);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: PRINTING -> print_active=1",
                 "[core][printer_state][print_active]") {
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);
    REQUIRE(get_print_state_enum() == PrintJobState::PRINTING);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: PAUSED -> print_active=1",
                 "[core][printer_state][print_active]") {
    set_print_state("paused");
    REQUIRE(get_print_active() == 1);
    REQUIRE(get_print_state_enum() == PrintJobState::PAUSED);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: COMPLETE -> print_active=0",
                 "[core][printer_state][print_active]") {
    // First start printing
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);

    // Then complete
    set_print_state("complete");
    REQUIRE(get_print_active() == 0);
    REQUIRE(get_print_state_enum() == PrintJobState::COMPLETE);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: CANCELLED -> print_active=0",
                 "[core][printer_state][print_active]") {
    // First start printing
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);

    // Then cancel
    set_print_state("cancelled");
    REQUIRE(get_print_active() == 0);
    REQUIRE(get_print_state_enum() == PrintJobState::CANCELLED);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: ERROR -> print_active=0",
                 "[core][printer_state][print_active]") {
    set_print_state("error");
    REQUIRE(get_print_active() == 0);
    REQUIRE(get_print_state_enum() == PrintJobState::ERROR);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: PRINTING -> PAUSED -> print_active stays 1",
                 "[core][printer_state][print_active]") {
    // Start printing
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);

    // Pause
    set_print_state("paused");
    REQUIRE(get_print_active() == 1); // Still active!

    // Resume
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);
}

TEST_CASE_METHOD(PrintActiveTestFixture, "PrintActive: full lifecycle test",
                 "[core][printer_state][print_active]") {
    // Start: standby
    set_print_state("standby");
    REQUIRE(get_print_active() == 0);

    // Print starts
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);

    // Pause
    set_print_state("paused");
    REQUIRE(get_print_active() == 1);

    // Resume
    set_print_state("printing");
    REQUIRE(get_print_active() == 1);

    // Complete
    set_print_state("complete");
    REQUIRE(get_print_active() == 0);

    // Back to standby
    set_print_state("standby");
    REQUIRE(get_print_active() == 0);
}

// ============================================================================
// Pure predicate: PrinterPrintState::status_indicates_active_print
//
// This is the exact decision used by BOTH the print_active subject update and
// the discovery-time idle gate in Application::on_discovery_complete. The gate
// must consult the just-arrived status (the subject is stale at that point),
// so this predicate is what blocks a reconfig wizard from launching over a live
// print on a fresh connection. These cases lock that behavior down.
// ============================================================================

TEST_CASE("status_indicates_active_print: printing -> true",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", {{"state", "printing"}}}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == true);
}

TEST_CASE("status_indicates_active_print: paused -> true",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", {{"state", "paused"}}}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == true);
}

TEST_CASE("status_indicates_active_print: standby -> false",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", {{"state", "standby"}}}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == false);
}

TEST_CASE("status_indicates_active_print: complete -> false",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", {{"state", "complete"}}}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == false);
}

TEST_CASE("status_indicates_active_print: cancelled -> false",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", {{"state", "cancelled"}}}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == false);
}

TEST_CASE("status_indicates_active_print: missing print_stats -> false",
          "[core][printer_state][print_active][print_state]") {
    json status = json::object();
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == false);
}

TEST_CASE("status_indicates_active_print: print_stats without state -> false",
          "[core][printer_state][print_active][print_state]") {
    json status = {{"print_stats", json::object()}};
    REQUIRE(PrinterPrintState::status_indicates_active_print(status) == false);
}
