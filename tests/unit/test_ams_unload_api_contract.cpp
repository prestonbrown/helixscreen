// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_unload_api_contract.cpp
 * @brief Locks in the post-DRY unload API contract.
 *
 * Run with: ./build/bin/helix-tests "[ams][unload][contract]"
 *
 * Background — the U1 Filament-panel-unload wrong-tool bug
 * (Discord report from Bart, 2026-07-20):
 *
 *   With T3 loaded, hitting Unload on the Filament panel caused the printer
 *   to visit T0 briefly before unloading T3. Root cause: FilamentPanel passed
 *   no slot to the backend, and the Snapmaker backend's internal fallback re-read
 *   current_slot — landing on a stale value (0) that disagreed with the snapshot
 *   the UI's "is anything loaded?" guard had just used.
 *
 * Fix (this refactor):
 *   - virtual unload_filament(int slot_index) lost its `= -1` default arg
 *     (compiler-enforced: callers must be explicit).
 *   - AmsBackend::unload_active_filament() resolves current_slot ONCE in the
 *     base class and forwards — single source of truth, no per-backend fallback
 *     can diverge from the UI's snapshot.
 *
 * These tests lock in:
 *   1. unload_active_filament() reads current_slot once and forwards it.
 *   2. unload_filament() with no arg does NOT compile (verified by the absence
 *      of any such callsite in src/ — characterization tests here document why).
 *   3. Per-backend unload_filament(N) sends the right gcode for slot N.
 *   4. Regression: the per-lane context-menu path (which passes an explicit
 *      slot) is unaffected and still produces slot-specific gcode.
 */

#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using std::string;
using std::vector;

// =============================================================================
// Capture helper — same idiom as test_ams_backend_snapmaker.cpp's
// CapturingSnapmakerBackend. Inline here so this test file is self-contained.
// =============================================================================

namespace {

class CapturingSnapmaker : public AmsBackendSnapmaker {
  public:
    // running_ has to be set: unload_filament() gates on check_preconditions(),
    // which answers not_connected on a backend that never started. api_ stays
    // null so the print-active half of that gate passes; it is covered on its
    // own in test_ams_paused_filament_ops.cpp.
    CapturingSnapmaker() : AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    vector<string> captured_gcodes;

    AmsError execute_gcode(const string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const string& gcode, std::function<void()> /*on_complete*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    void set_loaded_slot(int slot) {
        system_info_.current_slot = slot;
    }
};

} // namespace

// =============================================================================
// 1. unload_active_filament() resolves current_slot ONCE and forwards
// =============================================================================

TEST_CASE("unload_active_filament forwards current_slot from system_info_",
          "[ams][unload][contract]") {
    SECTION("forwards current_slot when set") {
        // The contract: the base helper reads get_system_info().current_slot
        // and passes it explicitly to the backend's slot-specific override.
        // For the Snapmaker backend that means AUTO_FEEDING EXTRUDER={slot}.
        CapturingSnapmaker backend;
        backend.set_loaded_slot(2);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 UNLOAD=1");
    }

    SECTION("forwards -1 when no slot is active (backend keeps leaf-macro fallback)") {
        // current_slot == -1: the helper forwards -1 to the backend override.
        // Snapmaker's override falls back to bare INNER_FILAMENT_UNLOAD — its
        // "trust the firmware" path. Per-backend -1 behavior is preserved.
        CapturingSnapmaker backend; // current_slot defaults to -1
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "INNER_FILAMENT_UNLOAD");
    }

    SECTION("each call re-resolves current_slot (no caching across calls)") {
        // The helper must NOT cache the resolved slot. Two successive calls
        // with different current_slot values must dispatch different gcodes.
        CapturingSnapmaker backend;

        backend.set_loaded_slot(1);
        REQUIRE(backend.unload_active_filament().success());

        backend.set_loaded_slot(3);
        REQUIRE(backend.unload_active_filament().success());

        REQUIRE(backend.captured_gcodes.size() == 2);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1");
        REQUIRE(backend.captured_gcodes[1] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }
}

// =============================================================================
// 2. Per-backend unload_filament(N) is slot-explicit (regression suite)
// =============================================================================

TEST_CASE("Per-backend unload_filament(N) sends slot-specific gcode", "[ams][unload][contract]") {
    SECTION("Snapmaker unload_filament(3) sends EXTRUDER=3, never EXTRUDER=0") {
        // Regression for the U1 field bug: with T3 loaded, the Filament panel
        // used to dispatch EXTRUDER=0 because the backend re-resolved -1 →
        // current_slot and landed on a stale 0. Now unload_active_filament()
        // resolves once in the base and the Filament panel uses that helper;
        // the slot-explicit overload (used by the AMS context menu) is
        // unaffected and must continue to dispatch the requested slot.
        CapturingSnapmaker backend;
        auto err = backend.unload_filament(3);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
    }
}

// =============================================================================
// 3. Regression: AmsBackendMock honors the new contract
// =============================================================================

TEST_CASE("AmsBackendMock unload_filament requires explicit slot",
          "[ams][unload][contract][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("unload_filament(0) succeeds for slot 0") {
        // Mock starts with slot 0 loaded in default mode.
        auto result = backend.unload_filament(0);
        CHECK(result);
    }

    SECTION("unload_active_filament dispatches against current_slot") {
        // The helper on a mock backend must also forward current_slot.
        // Default mock state: slot 0 loaded.
        auto info = backend.get_system_info();
        REQUIRE(info.current_slot == 0);
        auto result = backend.unload_active_filament();
        CHECK(result);
    }

    backend.stop();
}
