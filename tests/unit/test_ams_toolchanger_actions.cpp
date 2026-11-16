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
 * - Load and Unload refuse: they never mount or park
 * - change_tool with invalid slot returns error
 * - park_active_tool works when a tool is mounted
 * - Sequential tool changes succeed
 */

#include "ams_backend_mock.h"
#include "runtime_config.h"

#include <atomic>
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

/// Releases a held backend after two seconds unless destroyed first, so a
/// missing lockout fails its assertion instead of deadlocking: a second
/// operation the backend accepts joins the held one before it returns.
class HoldReleaseGuard {
  public:
    explicit HoldReleaseGuard(helix::AmsBackendMock& backend)
        : thread_([this, &backend] {
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
              while (!done_ && std::chrono::steady_clock::now() < deadline) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
              backend.hold_operations(false);
          }) {}
    ~HoldReleaseGuard() {
        done_ = true;
        thread_.join();
    }

  private:
    std::atomic<bool> done_{false};
    std::thread thread_;
};

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
    // Held, the first operation stays in flight through the second call, so
    // the completion thread cannot clear it first at any delay.
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(0);
    backend.hold_operations(true);
    REQUIRE(backend.start());
    HoldReleaseGuard release_guard(backend);

    SECTION("change_tool rejected while a park is in progress") {
        // The mock starts with T0 mounted.
        auto result1 = backend.park_active_tool();
        REQUIRE(result1);

        // Immediately try another operation -- should be rejected as BUSY
        auto result2 = backend.change_tool(2);
        CHECK_FALSE(result2);
        CHECK(result2.result == helix::AmsResult::BUSY);

        backend.hold_operations(false);
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));
    }

    SECTION("park rejected while change_tool is in progress") {
        auto result1 = backend.change_tool(1);
        REQUIRE(result1);

        auto result2 = backend.park_active_tool();
        CHECK_FALSE(result2);
        CHECK(result2.result == helix::AmsResult::BUSY);

        backend.hold_operations(false);
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));
    }

    backend.stop();
}

// =============================================================================
// Load and Unload never mount or park
// =============================================================================

TEST_CASE("Load and Unload refuse in mock toolchanger mode, as the real backend does",
          "[ams][toolchanger][toolchanger_actions]") {
    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());
    REQUIRE(backend.get_current_slot() == 0);

    const auto load = backend.load_filament(2);
    CHECK_FALSE(load);
    CHECK(load.result == helix::AmsResult::NOT_SUPPORTED);
    CHECK_FALSE(load.user_msg.empty());

    const auto unload = backend.unload_filament(0);
    CHECK_FALSE(unload);
    CHECK(unload.result == helix::AmsResult::NOT_SUPPORTED);

    // Nothing moved: T0 is still the mounted tool.
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK(backend.get_current_slot() == 0);
    CHECK(backend.head_filament_ops(0).load == helix::FilamentOpSupport::Manual);

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
// park_active_tool works
// =============================================================================

TEST_CASE("park_active_tool works in mock toolchanger mode",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    SECTION("park succeeds when a tool is mounted") {
        // Mock starts with slot 0 mounted
        REQUIRE(backend.get_current_slot() == 0);

        auto result = backend.park_active_tool();
        REQUIRE(result);

        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

        CHECK(backend.get_current_action() == AmsAction::IDLE);
        CHECK(backend.get_current_slot() == -1);
    }

    SECTION("park returns error when nothing is mounted") {
        backend.park_active_tool();
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));
        REQUIRE(backend.get_current_slot() == -1);

        auto result = backend.park_active_tool();
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

    SECTION("park fails when not started") {
        auto result = backend.park_active_tool();
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

        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));

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

// =============================================================================
// A tool change never reads IDLE until the new tool is loaded
// =============================================================================

// The unload phase ends by clearing the loaded slot. If the action read IDLE
// at that moment, a caller waiting for IDLE would take the half-finished
// change (nothing loaded, slot -1) for the finished one.
TEST_CASE("Mock tool change never reads IDLE before the new tool is loaded",
          "[ams][toolchanger][toolchanger_actions]") {
    FastTimingScopeTC timing_guard;

    const bool realistic = GENERATE(false, true);
    CAPTURE(realistic);

    helix::AmsBackendMock backend(4);
    backend.set_tool_changer_mode(true);
    backend.set_realistic_mode(realistic);
    backend.set_operation_delay(10);
    REQUIRE(backend.start());

    for (int t = 1; t < 4; ++t) {
        CAPTURE(t);
        REQUIRE(backend.change_tool(t));

        int early_idle_reads = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.get_current_action() != AmsAction::IDLE) {
                continue;
            }
            if (backend.get_current_slot() == t && backend.is_filament_loaded()) {
                break;
            }
            ++early_idle_reads;
        }
        CHECK(early_idle_reads == 0);
        REQUIRE(wait_until_ams_idle(backend, std::chrono::seconds(10)));
        CHECK(backend.get_current_slot() == t);
    }

    backend.stop();
}
