// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_exclude_object_manager.cpp
 * @brief Tests for PrintExcludeObjectManager — specifically the status-driven
 *        truth model introduced to fix spurious EXCLUDE_OBJECT timeout toasts.
 *
 * Background: printer.gcode.script blocks until Klipper executes the queued gcode,
 * so during pre-print an EXCLUDE_OBJECT RPC can sit queued for minutes and hit the
 * 60s transport timeout before it returns. The fix:
 *   1. MoonrakerAPI::exclude_object() now uses silent=true + 15min timeout.
 *   2. The request tracker suppresses REQUEST_TIMEOUT events for silent requests.
 *   3. PrintExcludeObjectManager treats TIMEOUT errors as advisory (waits for the
 *      exclude_object status subscription to confirm) and only reverts / toasts for
 *      hard errors (validation, connection lost, JSON-RPC error).
 *
 * These tests pin the manager half of that contract: status subscription promotes
 * awaiting-confirmation objects into confirmed-excluded, and nothing else should
 * silently mutate excluded_objects_ out from under that flow.
 *
 * Per L048: calls that enqueue work via UpdateQueue must be followed by drain().
 * Per L053: static LVGL fixtures reset between tests as needed.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_error.h"
#include "../../include/printer_state.h"
#include "../../include/ui_print_exclude_object_manager.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::ui;

/// Friend-class test accessor (L065 / scripts/code_lint.sh): keeps production
/// headers free of `_for_testing` methods while letting tests drive the
/// awaiting-confirmation set and RPC-error branch directly.
class PrintExcludeObjectManagerTestAccess {
  public:
    static void add_awaiting_confirmation(PrintExcludeObjectManager& m, const std::string& name) {
        m.awaiting_confirmation_.insert(name);
    }

    static bool is_awaiting_confirmation(const PrintExcludeObjectManager& m,
                                         const std::string& name) {
        return m.awaiting_confirmation_.count(name) > 0;
    }

    static void handle_rpc_error(PrintExcludeObjectManager& m, const std::string& object_name,
                                 const MoonrakerError& err) {
        m.on_exclude_rpc_error(object_name, err);
    }
};

namespace {

/// Fixture: manager wired to mock client and PrinterState, no gcode_viewer widget.
/// The manager's viewer-touching branches are all guarded by `if (gcode_viewer_)`,
/// so passing nullptr keeps us out of the LVGL widget graph while still exercising
/// the observer, awaiting-confirmation, and excluded_objects_ bookkeeping.
class ExcludeManagerFixture : public LVGLTestFixture {
  public:
    ExcludeManagerFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
        manager = std::make_unique<PrintExcludeObjectManager>(api.get(), state, nullptr);
        manager->init();
        // observe<int> defers an initial-value callback through UpdateQueue
        // (per L048 / observer_factory.h). Drain it now so the print-state
        // watchdog's STANDBY=0 fire-on-subscribe doesn't clear test-injected
        // awaiting_confirmation_ entries on the first real drain.
        UpdateQueue::instance().drain();
    }

    ~ExcludeManagerFixture() {
        if (manager) {
            manager->deinit();
            manager.reset();
        }
        // Drain any pending UI updates the manager queued so they don't leak into
        // the next test's static singleton state (L053).
        UpdateQueue::instance().drain();
        api.reset();
        mock_client.disconnect();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<PrintExcludeObjectManager> manager;
};

/// Helper to drive the print-state subject the same way Moonraker's status push does —
/// we feed a minimal notification payload through update_from_status().
void set_print_state_str(PrinterState& state, const char* moonraker_state) {
    json status = {{"print_stats", {{"state", moonraker_state}}}};
    state.update_from_status(status);
}

} // namespace

// ============================================================================
// Status subscription is the source of truth
// ============================================================================

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "status subscription promotes awaiting-confirmation to excluded",
                 "[exclude_object][manager][regression]") {
    // Manager has dispatched an EXCLUDE_OBJECT RPC for "Part_1"; visual is optimistic
    // but excluded_objects_ is still empty because we now wait for Klipper to confirm
    // via the exclude_object.excluded_objects status field.
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "Part_1");
    REQUIRE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "Part_1"));
    REQUIRE(manager->get_excluded_objects().count("Part_1") == 0);

    // Klipper pushes a status update via Moonraker subscription: Part_1 is now excluded.
    state.excluded_objects_state().set_excluded_objects({"Part_1"});
    UpdateQueue::instance().drain();

    SECTION("object moves into confirmed excluded set") {
        REQUIRE(manager->get_excluded_objects().count("Part_1") == 1);
    }

    SECTION("awaiting-confirmation is cleared once Klipper confirms") {
        REQUIRE_FALSE(
            PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "Part_1"));
    }
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "exclusions from other clients (no prior dispatch) still sync in",
                 "[exclude_object][manager]") {
    // No awaiting entry — this simulates another frontend (e.g. Mainsail) excluding
    // an object. The manager must still pick it up so its UI stays consistent.
    REQUIRE_FALSE(
        PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "Foreign_Part"));

    state.excluded_objects_state().set_excluded_objects({"Foreign_Part"});
    UpdateQueue::instance().drain();

    REQUIRE(manager->get_excluded_objects().count("Foreign_Part") == 1);
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "awaiting-confirmation survives a status update that doesn't confirm it",
                 "[exclude_object][manager][regression]") {
    // Manager is waiting on Part_B, but a status push arrives carrying only Part_A
    // (e.g. another object was excluded from a different client). Our pending entry
    // must remain awaiting — clearing it prematurely would let a later TIMEOUT error
    // silently forget the object.
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "Part_B");

    state.excluded_objects_state().set_excluded_objects({"Part_A"});
    UpdateQueue::instance().drain();

    REQUIRE(manager->get_excluded_objects().count("Part_A") == 1);
    REQUIRE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "Part_B"));
    REQUIRE(manager->get_excluded_objects().count("Part_B") == 0);
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "status subscription removes entries Klipper dropped (RESET_EXCLUDE)",
                 "[exclude_object][manager][regression]") {
    // Klipper confirms two exclusions, promoting them into the local confirmed set.
    state.excluded_objects_state().set_excluded_objects({"Part_A", "Part_B"});
    UpdateQueue::instance().drain();
    REQUIRE(manager->get_excluded_objects().count("Part_A") == 1);
    REQUIRE(manager->get_excluded_objects().count("Part_B") == 1);

    SECTION("RESET_EXCLUDE (empty set) clears the local cache") {
        // RESET_EXCLUDE macro / print-end reset drops everything from Klipper's set.
        // Our local cache must follow, otherwise the gcode viewer keeps ghosting objects
        // that Klipper will happily print again on the next run.
        state.excluded_objects_state().set_excluded_objects({});
        UpdateQueue::instance().drain();

        REQUIRE(manager->get_excluded_objects().empty());
    }

    SECTION("partial drop removes just the dropped entry") {
        state.excluded_objects_state().set_excluded_objects({"Part_A"});
        UpdateQueue::instance().drain();

        REQUIRE(manager->get_excluded_objects().count("Part_A") == 1);
        REQUIRE(manager->get_excluded_objects().count("Part_B") == 0);
    }

    SECTION("awaiting-confirmation entries are untouched by the sync-to-set") {
        // An object with a dispatched-but-unconfirmed RPC must not be prematurely
        // synthesized into excluded_objects_ just because Klipper's set is empty.
        PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "InFlight");
        state.excluded_objects_state().set_excluded_objects({});
        UpdateQueue::instance().drain();

        REQUIRE(manager->get_excluded_objects().empty());
        REQUIRE(
            PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "InFlight"));
    }
}

// ============================================================================
// RPC error branching — TIMEOUT is advisory, other errors are fatal-to-the-dispatch
// ============================================================================

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "RPC TIMEOUT error leaves awaiting entry intact for status to confirm later",
                 "[exclude_object][manager][regression]") {
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "LateExclude");

    MoonrakerError err;
    err.type = MoonrakerErrorType::TIMEOUT;
    err.method = "printer.gcode.script";
    err.message = "Printer command 'printer.gcode.script' timed out after 900000ms";

    PrintExcludeObjectManagerTestAccess::handle_rpc_error(*manager, "LateExclude", err);
    UpdateQueue::instance().drain();

    SECTION("awaiting entry remains — Klipper may still run the gcode") {
        REQUIRE(
            PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "LateExclude"));
    }

    SECTION("excluded_objects_ is not mutated by the error path") {
        REQUIRE(manager->get_excluded_objects().empty());
    }

    // Simulate Klipper eventually confirming via the status subscription.
    state.excluded_objects_state().set_excluded_objects({"LateExclude"});
    UpdateQueue::instance().drain();

    SECTION("late status arrival still promotes to confirmed") {
        REQUIRE(manager->get_excluded_objects().count("LateExclude") == 1);
        REQUIRE_FALSE(
            PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "LateExclude"));
    }
}

TEST_CASE_METHOD(ExcludeManagerFixture, "RPC VALIDATION_ERROR clears awaiting and reverts",
                 "[exclude_object][manager][regression]") {
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "BadName");

    MoonrakerError err;
    err.type = MoonrakerErrorType::VALIDATION_ERROR;
    err.method = "exclude_object";
    err.message = "Invalid object name contains illegal characters";

    PrintExcludeObjectManagerTestAccess::handle_rpc_error(*manager, "BadName", err);
    UpdateQueue::instance().drain();

    // The error path removes the awaiting entry so no late status push can resurrect it.
    REQUIRE_FALSE(
        PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "BadName"));
    REQUIRE(manager->get_excluded_objects().count("BadName") == 0);
}

TEST_CASE_METHOD(ExcludeManagerFixture, "RPC CONNECTION_LOST clears awaiting",
                 "[exclude_object][manager]") {
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "OrphanedExclude");

    MoonrakerError err;
    err.type = MoonrakerErrorType::CONNECTION_LOST;
    err.method = "printer.gcode.script";
    err.message = "WebSocket disconnected before response";

    PrintExcludeObjectManagerTestAccess::handle_rpc_error(*manager, "OrphanedExclude", err);
    UpdateQueue::instance().drain();

    // Connection loss is a real failure — drop the awaiting entry. Self-healing case:
    // on reconnect, status subscription will re-sync any exclusions that did run.
    REQUIRE_FALSE(
        PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "OrphanedExclude"));
}

// ============================================================================
// Print-state watchdog — drop stuck optimistic visuals when the print ends
// ============================================================================

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "print ending clears awaiting-confirmation entries silently",
                 "[exclude_object][manager][watchdog]") {
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "UnconfirmedPart");

    // Print starts and runs — the awaiting visual is legitimate.
    set_print_state_str(state, "printing");
    UpdateQueue::instance().drain();
    REQUIRE(
        PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "UnconfirmedPart"));

    SECTION("user cancels the print — drop the optimistic visual") {
        set_print_state_str(state, "cancelled");
        UpdateQueue::instance().drain();
        REQUIRE_FALSE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(
            *manager, "UnconfirmedPart"));
        // Nothing was ever confirmed, so confirmed set stays empty.
        REQUIRE(manager->get_excluded_objects().empty());
    }

    SECTION("print completes without Klipper confirming — drop the visual") {
        set_print_state_str(state, "complete");
        UpdateQueue::instance().drain();
        REQUIRE_FALSE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(
            *manager, "UnconfirmedPart"));
    }

    SECTION("print errors out — drop the visual") {
        set_print_state_str(state, "error");
        UpdateQueue::instance().drain();
        REQUIRE_FALSE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(
            *manager, "UnconfirmedPart"));
    }

    SECTION("print drops back to standby — drop the visual") {
        set_print_state_str(state, "standby");
        UpdateQueue::instance().drain();
        REQUIRE_FALSE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(
            *manager, "UnconfirmedPart"));
    }
}

TEST_CASE_METHOD(ExcludeManagerFixture, "paused print keeps awaiting-confirmation — still active",
                 "[exclude_object][manager][watchdog]") {
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "PausedPart");

    set_print_state_str(state, "printing");
    UpdateQueue::instance().drain();
    set_print_state_str(state, "paused");
    UpdateQueue::instance().drain();

    // Pause is not a terminal state — Klipper can still execute the queued gcode
    // when the print resumes, so the awaiting entry stays.
    REQUIRE(PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "PausedPart"));
}

TEST_CASE_METHOD(ExcludeManagerFixture, "confirmed exclusions survive the watchdog",
                 "[exclude_object][manager][watchdog]") {
    // Dispatch, Klipper confirms, THEN the user cancels. The confirmed exclusion
    // must stay in excluded_objects_ — only un-confirmed awaiting entries get dropped.
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "ConfirmedPart");
    state.excluded_objects_state().set_excluded_objects({"ConfirmedPart"});
    UpdateQueue::instance().drain();
    REQUIRE(manager->get_excluded_objects().count("ConfirmedPart") == 1);

    set_print_state_str(state, "cancelled");
    UpdateQueue::instance().drain();

    REQUIRE(manager->get_excluded_objects().count("ConfirmedPart") == 1);
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "clear_excluded_objects() also clears awaiting-confirmation",
                 "[exclude_object][manager]") {
    // A new print starts — both confirmed exclusions AND any in-flight dispatches
    // must reset, otherwise a stale awaiting entry could promote itself when the
    // next print's first status push arrives.
    PrintExcludeObjectManagerTestAccess::add_awaiting_confirmation(*manager, "StalePart");
    state.excluded_objects_state().set_excluded_objects({"Confirmed"});
    UpdateQueue::instance().drain();
    REQUIRE(manager->get_excluded_objects().count("Confirmed") == 1);

    manager->clear_excluded_objects();

    REQUIRE(manager->get_excluded_objects().empty());
    REQUIRE_FALSE(
        PrintExcludeObjectManagerTestAccess::is_awaiting_confirmation(*manager, "StalePart"));
}
