// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_toolchanger_actions.cpp
 * @brief Unit tests for toolchanger clickable toolhead actions
 *
 * Tests for toolchanger-specific behavior:
 * - Mock toolchanger mode basics (topology, type)
 * - change_tool sets SELECTING immediately (race prevention)
 * - Lockout during in-flight operations (busy rejection)
 * - load_filament delegates to change_tool
 * - change_tool with invalid slot returns error
 * - unload_filament works when a tool is mounted
 * - Sequential tool changes succeed
 */

#include "ams_backend_mock.h"
#include "runtime_config.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsAction;
using helix::AmsType;
using helix::PathTopology;

// RAII helper to set up fast timing and restore on exit
class FastTimingScopeTC {
  public:
    FastTimingScopeTC() {
        auto* config = get_runtime_config();
        original_speedup_ = config->sim_speedup;
        config->sim_speedup = 1000.0;
    }
    ~FastTimingScopeTC() {
        auto* config = get_runtime_config();
        config->sim_speedup = original_speedup_;
    }

  private:
    double original_speedup_ = 1.0;
};

/// A tool change is unload-then-load; a fixed margin underneath the first
/// phase asserts the end of the operation before it can have happened
/// (prestonbrown/helixscreen#1539). Poll instead: finishes as soon as the
/// transition lands, and fails only if it genuinely never does.
bool wait_until_ams_idle(helix::AmsBackendMock& backend, std::chrono::milliseconds ceiling) {
    const auto deadline = std::chrono::steady_clock::now() + ceiling;
    do {
        if (backend.get_current_action() == AmsAction::IDLE) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// =============================================================================
// Mock toolchanger mode basics
// =============================================================================

TEST_CASE("Mock toolchanger mode gives PARALLEL topology and correct type",
          "[ams][toolchanger][toolchanger_actions]") {
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("type is TOOL_CHANGER") {
        CHECK(backend.get_type() == AmsType::TOOL_CHANGER);
    }

    SECTION("topology is PARALLEL") {
        CHECK(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("system info reports correct type") {
        auto info = backend.get_system_info();
        CHECK(info.type == AmsType::TOOL_CHANGER);
        CHECK(info.type_name == "Tool Changer (Mock)");
    }

    SECTION("has 4 slots with correct initial state") {
        auto info = backend.get_system_info();
        CHECK(info.total_slots == 4);
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slots.size() == 4);

        for (int i = 0; i < 4; ++i) {
            auto slot = backend.get_slot_info(i);
            CAPTURE(i);
            CHECK(slot.slot_index == i);
            CHECK(slot.mapped_tool == i);
        }
    }

    SECTION("bypass is not supported") {
        CHECK_FALSE(backend.is_bypass_active());
        auto info = backend.get_system_info();
        CHECK_FALSE(info.supports_bypass);
    }

    backend.stop();
}

// =============================================================================
// change_tool sets SELECTING immediately
// =============================================================================

TEST_CASE("change_tool sets SELECTING immediately in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    // Declare before backend so they outlive it (backend destructor joins threads)
    std::mutex actions_mtx;
    std::vector<AmsAction> observed_actions;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(50); // Nonzero so operation is still in flight
    REQUIRE(backend.start());
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == helix::AmsBackend::EVENT_STATE_CHANGED) {
            auto action = backend.get_current_action();
            std::lock_guard<std::mutex> lock(actions_mtx);
            if (observed_actions.empty() || observed_actions.back() != action) {
                observed_actions.push_back(action);
            }
        }
    });

    SECTION("action transitions away from IDLE immediately after change_tool") {
        auto result = backend.change_tool(1);
        REQUIRE(result);

        // change_tool sets action synchronously before returning, so the
        // observed_actions callback should have captured the transition even
        // if the background thread completes before we can poll.
        // Wait briefly for completion then verify the transition was recorded.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(actions_mtx);
        REQUIRE_FALSE(observed_actions.empty());
        CHECK(observed_actions.front() != AmsAction::IDLE);
    }

    SECTION("first observed action includes UNLOADING or SELECTING") {
        auto result = backend.change_tool(2);
        REQUIRE(result);

        // Wait for completion
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // The mock change_tool starts with UNLOADING (unload current + load new)
        std::lock_guard<std::mutex> lock(actions_mtx);
        REQUIRE_FALSE(observed_actions.empty());
        // First action should be UNLOADING (mock starts tool change with unload phase)
        CHECK((observed_actions[0] == AmsAction::UNLOADING ||
               observed_actions[0] == AmsAction::SELECTING));
    }

    backend.stop();
}

// =============================================================================
// Lockout during in-flight operations
// =============================================================================

TEST_CASE("Lockout rejects operations during in-flight tool change",
          "[ams][toolchanger][toolchanger_actions]") {
    // NOTE: deliberately NOT using FastTimingScopeTC here. The 1000x sim_speedup
    // would shrink the operation delay below to ~1ms, so the async completion
    // thread can clear the in-flight state before the immediate second call runs
    // — racing this test's whole premise. Use a real (un-scaled) delay long
    // enough that the operation is reliably still in-flight on slow CI.
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(500); // real ms — kept in-flight for the immediate reject check
    REQUIRE(backend.start());

    SECTION("change_tool rejected while load is in progress") {
        // Start a load operation
        auto result1 = backend.load_filament(1);
        REQUIRE(result1);

        // Immediately try another operation -- should be rejected as BUSY
        auto result2 = backend.change_tool(2);
        CHECK_FALSE(result2);
        CHECK(result2.result == helix::AmsResult::BUSY);

        // Wait for first operation to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    SECTION("load_filament rejected while change_tool is in progress") {
        auto result1 = backend.change_tool(1);
        REQUIRE(result1);

        auto result2 = backend.load_filament(2);
        CHECK_FALSE(result2);
        CHECK(result2.result == helix::AmsResult::BUSY);

        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    SECTION("unload_filament rejected while change_tool is in progress") {
        auto result1 = backend.change_tool(1);
        REQUIRE(result1);

        auto result2 = backend.unload_active_filament();
        CHECK_FALSE(result2);
        CHECK(result2.result == helix::AmsResult::BUSY);

        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    backend.stop();
}

// =============================================================================
// load_filament delegates to change_tool
// =============================================================================

TEST_CASE("load_filament delegates to change_tool in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    SECTION("load_filament succeeds and mounts the requested tool") {
        // Unload first so we can load a specific tool
        backend.unload_active_filament();
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        auto result = backend.load_filament(2);
        REQUIRE(result);

        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        auto info = backend.get_system_info();
        CHECK(info.current_slot == 2);
        CHECK(info.filament_loaded == true);
    }

    SECTION("load_filament with invalid slot returns error") {
        auto result = backend.load_filament(99);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::INVALID_SLOT);
    }

    SECTION("load_filament with negative slot returns error") {
        auto result = backend.load_filament(-1);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::INVALID_SLOT);
    }

    backend.stop();
}

// =============================================================================
// change_tool with invalid slot
// =============================================================================

TEST_CASE("change_tool with invalid slot returns error in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("negative tool number returns INVALID_TOOL") {
        auto result = backend.change_tool(-1);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::INVALID_TOOL);
    }

    SECTION("out-of-range tool number returns INVALID_TOOL") {
        auto result = backend.change_tool(99);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::INVALID_TOOL);
    }

    SECTION("tool number equal to slot count returns error") {
        auto result = backend.change_tool(4); // 0-3 are valid
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::INVALID_TOOL);
    }

    backend.stop();
}

// =============================================================================
// unload_filament works
// =============================================================================

TEST_CASE("unload_filament works in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    SECTION("unload succeeds when a tool is mounted") {
        // Mock starts with slot 0 loaded
        REQUIRE(backend.is_filament_loaded());

        auto result = backend.unload_active_filament();
        REQUIRE(result);

        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        CHECK(backend.get_current_action() == AmsAction::IDLE);
        CHECK_FALSE(backend.is_filament_loaded());
        CHECK(backend.get_current_slot() == -1);
    }

    SECTION("unload returns error when nothing is loaded") {
        // First unload
        backend.unload_active_filament();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE_FALSE(backend.is_filament_loaded());

        // Second unload should fail — nothing loaded
        auto result = backend.unload_active_filament();
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::WRONG_STATE);
    }

    backend.stop();
}

// =============================================================================
// Sequential tool changes
// =============================================================================

TEST_CASE("Sequential tool changes succeed in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    SECTION("change_tool(0) then change_tool(1) both succeed") {
        // First tool change to T0 (may already be loaded, but the mock allows it)
        auto result1 = backend.change_tool(0);
        REQUIRE(result1);
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        auto info1 = backend.get_system_info();
        CHECK(info1.current_slot == 0);
        CHECK(info1.filament_loaded == true);

        // Second tool change to T1
        auto result2 = backend.change_tool(1);
        REQUIRE(result2);
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        auto info2 = backend.get_system_info();
        CHECK(info2.current_slot == 1);
        CHECK(info2.filament_loaded == true);
    }

    SECTION("change through all 4 tools sequentially") {
        for (int t = 0; t < 4; ++t) {
            CAPTURE(t);
            auto result = backend.change_tool(t);
            REQUIRE(result);
            REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

            auto info = backend.get_system_info();
            CHECK(info.current_slot == t);
            CHECK(info.filament_loaded == true);
        }
    }

    backend.stop();
}

// =============================================================================
// change_tool on already-active tool (skip/no-op behavior)
// =============================================================================

TEST_CASE("change_tool on already-active tool in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    // The mock starts with slot 0 loaded
    REQUIRE(backend.get_current_slot() == 0);

    SECTION("calling change_tool on current slot still succeeds") {
        // At the mock backend level, calling change_tool(0) when T0 is active
        // is allowed (the mock doesn't short-circuit). The UI layer handles
        // the skip logic in on_path_slot_clicked.
        auto result = backend.change_tool(0);
        REQUIRE(result);

        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));
        CHECK(backend.get_current_action() == AmsAction::IDLE);
        CHECK(backend.get_current_slot() == 0);
    }

    backend.stop();
}

// =============================================================================
// Operations rejected when backend not started
// =============================================================================

TEST_CASE("Operations rejected when mock toolchanger backend not started",
          "[ams][toolchanger][toolchanger_actions]") {
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(0);
    // Intentionally NOT calling start()

    SECTION("change_tool fails when not started") {
        auto result = backend.change_tool(0);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::NOT_CONNECTED);
    }

    SECTION("load_filament fails when not started") {
        auto result = backend.load_filament(0);
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::NOT_CONNECTED);
    }

    SECTION("unload_filament fails when not started") {
        auto result = backend.unload_active_filament();
        CHECK_FALSE(result);
        CHECK(result.result == helix::AmsResult::NOT_CONNECTED);
    }
}

// =============================================================================
// Realistic mode tool change phases
// =============================================================================

TEST_CASE("Realistic mode tool change shows SELECTING phase in toolchanger mode",
          "[ams][toolchanger][toolchanger_actions][slow]") {
    FastTimingScopeTC timing_guard;

    // Declare before backend so they outlive it (backend destructor joins threads)
    std::mutex actions_mtx;
    std::vector<AmsAction> observed_actions;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    backend.set_realistic_mode(true);
    REQUIRE(backend.start());
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == helix::AmsBackend::EVENT_STATE_CHANGED) {
            auto action = backend.get_current_action();
            std::lock_guard<std::mutex> lock(actions_mtx);
            if (observed_actions.empty() || observed_actions.back() != action) {
                observed_actions.push_back(action);
            }
        }
    });

    SECTION("tool change includes SELECTING phase") {
        auto result = backend.change_tool(2);
        REQUIRE(result);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Should see SELECTING somewhere in the action sequence
        std::lock_guard<std::mutex> lock(actions_mtx);
        bool found_selecting = false;
        for (const auto& action : observed_actions) {
            if (action == AmsAction::SELECTING) {
                found_selecting = true;
                break;
            }
        }
        CHECK(found_selecting);

        // Should end in IDLE
        CHECK(backend.get_current_action() == AmsAction::IDLE);
    }

    backend.stop();
}
