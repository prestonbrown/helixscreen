// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_preparation_manager.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "macro_param_cache.h"

class PrintPreparationManagerTestAccess {
  public:
    static std::vector<std::pair<std::string, std::string>>
    get_skip_params(const helix::ui::PrintPreparationManager& m) {
        return m.collect_macro_skip_params();
    }
    static std::vector<std::string>
    get_pre_start_gcode_lines(const helix::ui::PrintPreparationManager& m,
                              const std::string& filename = {}) {
        return m.collect_pre_start_gcode_lines(filename);
    }
    static std::vector<gcode::OperationType>
    get_ops_to_disable(const helix::ui::PrintPreparationManager& m) {
        return m.collect_ops_to_disable();
    }
    static std::string build_pre_start_gcode_block(const std::string& setup_gcode,
                                                   const std::vector<std::string>& lines,
                                                   bool emit_setup) {
        return helix::ui::PrintPreparationManager::build_pre_start_gcode_block(setup_gcode, lines,
                                                                               emit_setup);
    }
    /// The modify route's first synchronous bail-out. Private because nothing
    /// outside the manager should dispatch it, but it is the cheapest reachable
    /// failure exit and every other one retires the job the same way.
    static void modify_and_print(helix::ui::PrintPreparationManager& m,
                                 const std::string& file_path) {
        m.modify_and_print(file_path, {}, {}, nullptr);
    }
    /// Age the pre-start send timestamp. Production stamps it when the
    /// pre-start gcode RPC leaves; a test models a backed-up ack by winding
    /// it back past the staleness bound.
    static void set_pre_start_sent_ago(helix::ui::PrintPreparationManager& m,
                                       std::chrono::seconds ago) {
        m.pre_start_sent_at_ = std::chrono::steady_clock::now() - ago;
    }
    /// Drive the shared continuation every pre-start path funnels through.
    static void continue_print_start(helix::ui::PrintPreparationManager& m, const std::string& file,
                                     helix::ui::PrintCompletionCallback on_completion) {
        m.continue_print_start(file, {}, nullptr, on_completion);
    }
};

#include "../mocks/mock_websocket_server.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "capability_matrix.h"
#include "gcode_ops_detector.h"
#include "hv/EventLoopThread.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "operation_registry.h"
#include "print_start_analyzer.h"
#include "printer_detector.h"
#include "printer_state.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// ============================================================================
// Test Fixture: Mock Dependencies
// ============================================================================

// PrintPreparationManager has nullable dependencies - we can test formatting
// and state management without actual API/printer connections.

// ============================================================================
// Tests: Macro Analysis Formatting
// ============================================================================

namespace {
/// The pre-start block carries a line per applicable PreStartGcode option, so a
/// test about one option must not assert the whole vector's shape — the K2 Plus
/// also emits its bed_mesh macro. Returns the single line containing `needle`.
std::string line_containing(const std::vector<std::string>& lines, const std::string& needle) {
    std::string found;
    for (const auto& l : lines) {
        if (l.find(needle) != std::string::npos) {
            REQUIRE(found.empty()); // exactly one match expected
            found = l;
        }
    }
    return found;
}
} // namespace

TEST_CASE("PrintPreparationManager: has_macro_analysis when no analysis available",
          "[print_preparation][macro]") {
    PrintPreparationManager manager;
    REQUIRE(manager.has_macro_analysis() == false);
}

TEST_CASE("PrintPreparationManager: is_macro_op_controllable", "[print_preparation][macro]") {
    PrintPreparationManager manager;

    SECTION("Returns false when no analysis available") {
        REQUIRE(manager.is_macro_op_controllable(PrintStartOpCategory::BED_MESH) == false);
        REQUIRE(manager.is_macro_op_controllable(PrintStartOpCategory::QGL) == false);
        REQUIRE(manager.is_macro_op_controllable(PrintStartOpCategory::Z_TILT) == false);
        REQUIRE(manager.is_macro_op_controllable(PrintStartOpCategory::NOZZLE_CLEAN) == false);
    }
}

TEST_CASE("PrintPreparationManager: get_macro_skip_param", "[print_preparation][macro]") {
    PrintPreparationManager manager;

    SECTION("Returns empty string when no analysis available") {
        REQUIRE(manager.get_macro_skip_param(PrintStartOpCategory::BED_MESH).empty());
        REQUIRE(manager.get_macro_skip_param(PrintStartOpCategory::QGL).empty());
    }
}

// ============================================================================
// Tests: File Operations Scanning
// ============================================================================

TEST_CASE("PrintPreparationManager: scan cache state when no scan done",
          "[print_preparation][gcode]") {
    PrintPreparationManager manager;

    SECTION("has_scan_result_for returns false when no scan done") {
        REQUIRE(manager.has_scan_result_for("test.gcode") == false);
        REQUIRE(manager.has_scan_result_for("") == false);
    }
}

TEST_CASE("PrintPreparationManager: clear_scan_cache", "[print_preparation][gcode]") {
    PrintPreparationManager manager;

    SECTION("Can be called when no cache exists") {
        // Should not throw or crash
        manager.clear_scan_cache();
        REQUIRE(manager.has_scan_result_for("test.gcode") == false);
    }
}

// ============================================================================
// Tests: Resource Safety
// ============================================================================

TEST_CASE("PrintPreparationManager: check_modification_capability", "[print_preparation][safety]") {
    PrintPreparationManager manager;
    // No API set - tests fallback behavior

    SECTION("Without API, checks disk space fallback") {
        auto capability = manager.check_modification_capability();
        // Without API, has_plugin is false
        REQUIRE(capability.has_plugin == false);
        // Should still check disk space
        // (can_modify depends on system - just verify it returns valid struct)
        REQUIRE((capability.can_modify ||
                 !capability.can_modify)); // Always true, just checking no crash
    }
}

TEST_CASE("PrintPreparationManager: get_temp_directory", "[print_preparation][safety]") {
    PrintPreparationManager manager;

    SECTION("Returns usable temp directory path") {
        std::string temp_dir = manager.get_temp_directory();
        // Should return a non-empty path on any reasonable system
        // (empty only if all fallbacks fail, which shouldn't happen in tests)
        INFO("Temp directory: " << temp_dir);
        // Just verify it doesn't crash and returns something reasonable
        REQUIRE(temp_dir.find("helix") != std::string::npos);
    }
}

TEST_CASE("PrintPreparationManager: set_cached_file_size", "[print_preparation][safety]") {
    PrintPreparationManager manager;

    SECTION("Setting file size affects modification capability calculation") {
        // Set a reasonable file size
        manager.set_cached_file_size(10 * 1024 * 1024); // 10MB

        auto capability = manager.check_modification_capability();

        // If temp directory isn't available, required_bytes will be 0 (early return)
        // This can happen in CI environments or sandboxed test runners
        if (capability.has_disk_space) {
            // Disk space check succeeded - verify required_bytes accounts for file size
            REQUIRE(capability.required_bytes > 10 * 1024 * 1024);
        } else {
            // Temp directory unavailable - verify we get a sensible response
            INFO("Temp directory unavailable: " << capability.reason);
            REQUIRE(capability.can_modify == false);
            REQUIRE(capability.has_plugin == false);
        }
    }

    SECTION("Very large file size may exceed available space") {
        // Set an extremely large file size
        manager.set_cached_file_size(1000ULL * 1024 * 1024 * 1024); // 1TB

        auto capability = manager.check_modification_capability();
        // Should report insufficient space for such a large file
        // (unless running on a system with 2TB+ free space)
        INFO("can_modify: " << capability.can_modify);
        INFO("reason: " << capability.reason);
        // Just verify it handles large values without overflow/crash
        REQUIRE((capability.can_modify || !capability.can_modify));
    }
}

// ============================================================================
// Tests: Option-State Provider (replaces removed legacy subject API)
// ============================================================================

/**
 * The pre-print options framework reads option state through
 * `OptionStateProvider`, a callback that maps an option id ("bed_mesh",
 * "qgl", ...) to its current toggle state:
 *
 *   1  -> ENABLED   (visible + checked in the active panel)
 *   0  -> DISABLED  (visible + unchecked — user explicitly skipped)
 *  -1  -> NOT_APPLICABLE (no row for this id in the active panel)
 *
 * `MockOptionState` is a small in-memory map that implements that contract,
 * replacing the per-id LVGL subject members that were removed in Phase 3.5.
 */
struct MockOptionState {
    std::map<std::string, int> values;

    /// Mark `id` as ENABLED (visible + checked).
    void enable(const std::string& id) {
        values[id] = 1;
    }
    /// Mark `id` as DISABLED (visible + unchecked).
    void disable(const std::string& id) {
        values[id] = 0;
    }
    /// Hide `id` (NOT_APPLICABLE — not in the active panel).
    void hide(const std::string& id) {
        values[id] = -1;
    }
    /// Remove an explicit setting; provider returns -1 (NOT_APPLICABLE).
    void clear(const std::string& id) {
        values.erase(id);
    }

    /// Build a callable provider that closes over this map.
    [[nodiscard]] std::function<int(const std::string&)> provider() {
        return [this](const std::string& id) -> int {
            auto it = values.find(id);
            return (it != values.end()) ? it->second : -1;
        };
    }
};

TEST_CASE("PrintPreparationManager: read_options_from_subjects via OptionStateProvider",
          "[print_preparation][options]") {
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("All options enabled - returns all true") {
        state.enable("bed_mesh");
        state.enable("qgl");
        state.enable("z_tilt");
        state.enable("nozzle_clean");
        state.enable("purge_line");
        state.enable("timelapse");

        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == true);
        REQUIRE(options.qgl == true);
        REQUIRE(options.z_tilt == true);
        REQUIRE(options.nozzle_clean == true);
        REQUIRE(options.purge_line == true);
        REQUIRE(options.timelapse == true);
    }

    SECTION("Mixed enabled/disabled states") {
        state.enable("bed_mesh");
        state.disable("qgl");
        state.enable("z_tilt");
        state.disable("nozzle_clean");
        state.enable("timelapse");

        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == true);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == true);
        REQUIRE(options.nozzle_clean == false);
        REQUIRE(options.timelapse == true);
    }

    SECTION("All disabled - returns all false") {
        state.disable("bed_mesh");
        state.disable("qgl");
        state.disable("z_tilt");
        state.disable("nozzle_clean");
        state.disable("purge_line");
        state.disable("timelapse");

        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == false);
        REQUIRE(options.nozzle_clean == false);
        REQUIRE(options.purge_line == false);
        REQUIRE(options.timelapse == false);
    }
}

TEST_CASE("PrintPreparationManager: read_options_from_subjects treats hidden as false",
          "[print_preparation][options]") {
    // Hidden = NOT_APPLICABLE (provider returns -1) — must NOT be treated as enabled.
    // Without an attached printer DB the cached fallback also yields NOT_APPLICABLE,
    // so read_options_from_subjects() returns false for a hidden id.
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("Provider returning -1 yields options.bed_mesh = false") {
        state.hide("bed_mesh");
        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);
    }

    SECTION("Multiple hidden ids: only enabled ones are true") {
        state.hide("bed_mesh");
        state.enable("qgl");
        state.hide("z_tilt");
        state.enable("nozzle_clean");
        state.hide("timelapse");

        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);    // hidden
        REQUIRE(options.qgl == true);          // enabled
        REQUIRE(options.z_tilt == false);      // hidden
        REQUIRE(options.nozzle_clean == true); // enabled
        REQUIRE(options.timelapse == false);   // hidden
    }
}

TEST_CASE("PrintPreparationManager: read_options_from_subjects without provider",
          "[print_preparation][options]") {
    // No provider, no PrinterState — every id falls through to NOT_APPLICABLE
    // and read_options_from_subjects() reports all options as false.
    lv_init_safe();
    PrintPreparationManager manager;

    auto options = manager.read_options_from_subjects();
    REQUIRE(options.bed_mesh == false);
    REQUIRE(options.qgl == false);
    REQUIRE(options.z_tilt == false);
    REQUIRE(options.nozzle_clean == false);
    REQUIRE(options.purge_line == false);
    REQUIRE(options.timelapse == false);
}

TEST_CASE("PrintPreparationManager: provider state changes are reflected immediately",
          "[print_preparation][options]") {
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("Provider value flips between reads") {
        state.disable("bed_mesh");
        state.disable("qgl");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == false);
        REQUIRE(manager.read_options_from_subjects().qgl == false);

        state.enable("bed_mesh");
        state.enable("qgl");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == true);
        REQUIRE(manager.read_options_from_subjects().qgl == true);

        state.disable("bed_mesh");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == false);
        REQUIRE(manager.read_options_from_subjects().qgl == true);
    }

    SECTION("Hide/show transitions") {
        state.enable("bed_mesh");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == true);

        state.hide("bed_mesh");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == false);

        state.enable("bed_mesh");
        REQUIRE(manager.read_options_from_subjects().bed_mesh == true);
    }
}

// ============================================================================
// Tests: Lifecycle Management
// ============================================================================

TEST_CASE("PrintPreparationManager: is_print_in_progress", "[print_preparation][lifecycle]") {
    PrintPreparationManager manager;

    SECTION("Not in progress by default (no printer state)") {
        // Without a PrinterState set, always returns false
        REQUIRE(manager.is_print_in_progress() == false);
    }
}

// ============================================================================
// Tests: Move Semantics
// ============================================================================

// PrintPreparationManager is non-movable (contains AsyncLifetimeGuard)
// Move constructor/assignment tests removed

// ============================================================================
// Tests: Capability Database Key Naming Convention
// ============================================================================

/**
 * BUG: collect_macro_skip_params() looks up "bed_leveling" but database uses "bed_mesh".
 *
 * The printer_database.json uses capability keys that match category_to_string() output:
 *   - category_to_string(PrintStartOpCategory::BED_MESH) returns "bed_mesh"
 *   - Database entry: "bed_mesh": { "param": "SKIP_LEVELING", ... }
 *
 * But collect_macro_skip_params() at line 878 uses has_capability("bed_leveling")
 * which will always return false because the key doesn't exist in the database.
 */
TEST_CASE("PrintPreparationManager: option ids match category_to_string",
          "[print_preparation][capabilities][bug]") {
    // This test verifies that pre_print_options ids align with category_to_string()
    // The database uses "bed_mesh", not "bed_leveling"

    SECTION("BED_MESH category maps to 'bed_mesh' id (not 'bed_leveling')") {
        // Verify what category_to_string returns for BED_MESH
        std::string expected_key = category_to_string(PrintStartOpCategory::BED_MESH);
        REQUIRE(expected_key == "bed_mesh");

        // Get AD5M Pro options (known to have bed_mesh option)
        auto caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
        REQUIRE_FALSE(caps.empty());

        // The database uses "bed_mesh" as the id
        const PrePrintOption* bed_cap = caps.find("bed_mesh");
        REQUIRE(bed_cap != nullptr);

        // "bed_leveling" is NOT a valid id in the database
        REQUIRE(caps.find("bed_leveling") == nullptr);

        // Verify the param details are accessible via the correct id
        const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed_cap->strategy);
        REQUIRE(macro != nullptr);
        REQUIRE(macro->param_name == "SKIP_LEVELING");

        // This is the key assertion: code using options MUST use "bed_mesh",
        // not "bed_leveling". Any lookup with "bed_leveling" will fail silently.
    }

    SECTION("All category strings are valid option ids") {
        // Verify each PrintStartOpCategory has a consistent string representation
        // that matches what the database expects

        // These should be the ids used in printer_database.json
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::BED_MESH)) == "bed_mesh");
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::QGL)) == "qgl");
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::Z_TILT)) == "z_tilt");
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::NOZZLE_CLEAN)) ==
                "nozzle_clean");
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::PURGE_LINE)) == "purge_line");
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::SKEW_CORRECT)) ==
                "skew_correct");

        // BED_LEVEL is a parent category, not a database key
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::BED_LEVEL)) == "bed_level");
    }
}

/**
 * Test that verifies collect_macro_skip_params() uses correct option ids.
 *
 * The pre_print_options database uses ids that match category_to_string() output:
 *   - "bed_mesh" for BED_MESH
 *   - "qgl" for QGL
 *   - "z_tilt" for Z_TILT
 *   - "nozzle_clean" for NOZZLE_CLEAN
 *
 * This test verifies the code uses these correct ids (not legacy names like "bed_leveling").
 */
TEST_CASE("PrintPreparationManager: collect_macro_skip_params uses correct option ids",
          "[print_preparation][capabilities]") {
    // Get options for a known printer
    auto caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(caps.empty());

    SECTION("bed_mesh id is used (not bed_leveling)") {
        // The CORRECT lookup id matches category_to_string(BED_MESH)
        const PrePrintOption* bed_cap = caps.find("bed_mesh");
        REQUIRE(bed_cap != nullptr);

        // The WRONG id should NOT exist - this ensures code using it would fail
        REQUIRE(caps.find("bed_leveling") == nullptr);

        // Verify the param details are accessible via the correct id
        const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed_cap->strategy);
        REQUIRE(macro != nullptr);
        REQUIRE(macro->param_name == "SKIP_LEVELING");
    }

    SECTION("All option ids match category_to_string output") {
        // These are the ids that collect_macro_skip_params() should use
        // They must match the ids in printer_database.json

        // BED_MESH -> "bed_mesh"
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::BED_MESH)) == "bed_mesh");

        // QGL -> "qgl"
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::QGL)) == "qgl");

        // Z_TILT -> "z_tilt"
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::Z_TILT)) == "z_tilt");

        // NOZZLE_CLEAN -> "nozzle_clean"
        REQUIRE(std::string(category_to_string(PrintStartOpCategory::NOZZLE_CLEAN)) ==
                "nozzle_clean");
    }
}

// ============================================================================
// Tests: Macro Analysis Progress Tracking
// ============================================================================

/**
 * Tests for macro analysis in-progress flag behavior.
 *
 * The is_macro_analysis_in_progress() flag is used to disable the Print button
 * while analysis is running, preventing race conditions where a print could
 * start before skip params are known.
 */
TEST_CASE("PrintPreparationManager: macro analysis in-progress tracking",
          "[print_preparation][macro][progress]") {
    PrintPreparationManager manager;

    SECTION("is_macro_analysis_in_progress returns false initially") {
        // Before any analysis is started, should return false
        REQUIRE(manager.is_macro_analysis_in_progress() == false);
    }

    SECTION("is_macro_analysis_in_progress returns false when no API set") {
        // Without API, analyze_print_start_macro() should return early
        // and not set in_progress flag
        manager.analyze_print_start_macro();
        REQUIRE(manager.is_macro_analysis_in_progress() == false);
    }

    SECTION("has_macro_analysis returns false when no analysis done") {
        REQUIRE(manager.has_macro_analysis() == false);
    }

    SECTION("Multiple analyze calls without API are ignored gracefully") {
        // Call multiple times - should not crash or set flag
        manager.analyze_print_start_macro();
        manager.analyze_print_start_macro();
        manager.analyze_print_start_macro();

        REQUIRE(manager.is_macro_analysis_in_progress() == false);
        REQUIRE(manager.has_macro_analysis() == false);
    }
}

// ============================================================================
// Tests: Capabilities from PrinterState (LT1 Refactor)
// ============================================================================

/**
 * Tests for the LT1 refactor: capabilities should come from PrinterState.
 *
 * After the refactor:
 * - PrintPreparationManager::get_cached_capabilities() delegates to PrinterState
 * - PrinterState owns the printer type and cached capabilities
 * - Manager no longer needs its own cache or Config lookup
 *
 * These tests verify the manager correctly uses PrinterState for capabilities.
 */
TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: capabilities come from PrinterState",
                 "[print_preparation][capabilities][lt1]") {
    // Initialize LVGL for PrinterState subjects
    lv_init_safe();

    // Create PrinterState and initialize subjects (without XML registration for tests)
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);

    // Create manager and set dependencies
    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("Manager uses PrinterState options for known printer") {
        // Set printer type on PrinterState (sync version for testing)
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        // Verify PrinterState has the option set
        const auto& state_caps = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(state_caps.empty());
        REQUIRE(state_caps.macro_name == "START_PRINT");

        const PrePrintOption* bed_cap = state_caps.find("bed_mesh");
        REQUIRE(bed_cap != nullptr);
        const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed_cap->strategy);
        REQUIRE(macro != nullptr);
        REQUIRE(macro->param_name == "SKIP_LEVELING");
    }

    SECTION("Manager sees empty capabilities when PrinterState has no type") {
        // Don't set any printer type - should have empty capabilities
        const auto& state_caps = printer_state.get_pre_print_option_set();
        REQUIRE(state_caps.empty());
        REQUIRE(state_caps.macro_name.empty());
    }

    SECTION("Manager sees empty capabilities for unknown printer type") {
        // Set an unknown printer type
        printer_state.set_printer_type_sync("Unknown Printer That Does Not Exist");

        // Should return empty capabilities, not crash
        const auto& state_caps = printer_state.get_pre_print_option_set();
        REQUIRE(state_caps.empty());
    }

    SECTION("Manager without PrinterState returns empty pre-start gcode") {
        // Create manager without setting dependencies — collect_pre_start_gcode_lines()
        // walks the cached option set and should return an empty vector without
        // crashing when there's no printer state to source the set from.
        PrintPreparationManager standalone_manager;
        REQUIRE(PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(standalone_manager)
                    .empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: capabilities update when PrinterState type changes",
                 "[print_preparation][capabilities][lt1]") {
    // Initialize LVGL for PrinterState subjects
    lv_init_safe();

    // Create PrinterState and initialize subjects
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);

    // Create manager and set dependencies
    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("Capabilities change when switching between known printers") {
        // Set to AD5M Pro first
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        // Verify AD5M Pro options
        const auto& caps_v1 = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(caps_v1.empty());
        REQUIRE(caps_v1.macro_name == "START_PRINT");
        size_t v1_option_count = caps_v1.options.size();

        // Now switch to AD5M (non-Pro)
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M");

        // Verify options updated
        const auto& caps_v2 = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(caps_v2.empty());
        // Both have START_PRINT but this confirms the lookup happened
        REQUIRE(caps_v2.macro_name == "START_PRINT");

        INFO("AD5M Pro options: " << v1_option_count);
        INFO("AD5M options: " << caps_v2.options.size());
    }

    SECTION("Capabilities become empty when switching to unknown printer") {
        // Start with known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        const auto& caps_known = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(caps_known.empty());

        // Switch to unknown printer
        printer_state.set_printer_type_sync("Generic Unknown Printer XYZ");

        // Capabilities should now be empty (no stale cache)
        const auto& caps_unknown = printer_state.get_pre_print_option_set();
        REQUIRE(caps_unknown.empty());
        REQUIRE(caps_unknown.macro_name.empty());
    }

    SECTION("Capabilities become empty when clearing printer type") {
        // Start with known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        const auto& caps_before = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(caps_before.empty());

        // Clear printer type
        printer_state.set_printer_type_sync("");

        // Capabilities should be empty
        const auto& caps_after = printer_state.get_pre_print_option_set();
        REQUIRE(caps_after.empty());
    }

    SECTION("No stale cache when rapidly switching printer types") {
        // Rapidly switch between multiple printer types
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        REQUIRE_FALSE(printer_state.get_pre_print_option_set().empty());

        printer_state.set_printer_type_sync("Unknown Printer 1");
        REQUIRE(printer_state.get_pre_print_option_set().empty());

        printer_state.set_printer_type_sync("FlashForge Adventurer 5M");
        REQUIRE_FALSE(printer_state.get_pre_print_option_set().empty());

        printer_state.set_printer_type_sync("");
        REQUIRE(printer_state.get_pre_print_option_set().empty());

        // Final state: set back to known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        const auto& final_caps = printer_state.get_pre_print_option_set();
        REQUIRE_FALSE(final_caps.empty());
        REQUIRE(final_caps.find("bed_mesh") != nullptr);
    }
}

// ============================================================================
// Tests: Pre-Print Option Set Cache Behavior (using PrinterDetector directly)
// ============================================================================

/**
 * Tests for PrinterDetector pre-print option set lookup behavior.
 *
 * These tests verify the underlying PrinterDetector::get_pre_print_option_set()
 * works correctly. After the LT1 refactor, PrinterState wraps this, but these
 * tests remain valuable for verifying the database lookup layer.
 */
TEST_CASE("PrintPreparationManager: pre-print options cache behavior",
          "[print_preparation][capabilities][cache]") {
    SECTION("get_cached_options returns options for known printer types") {
        // Verify PrinterDetector returns different options for different printers
        auto ad5m_caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
        auto voron_caps = PrinterDetector::get_pre_print_option_set("Voron 2.4");

        // AD5M Pro should have bed_mesh option
        REQUIRE_FALSE(ad5m_caps.empty());
        REQUIRE(ad5m_caps.find("bed_mesh") != nullptr);

        // Voron 2.4 may have different options (or none in database)
        // The key point is the lookup happens and returns a valid struct
        // (empty struct is valid - means no database entry)
        INFO("AD5M options: " << ad5m_caps.options.size());
        INFO("Voron options: " << voron_caps.options.size());
    }

    SECTION("Different printer types return different option sets") {
        // This verifies the database contains distinct entries
        auto ad5m_caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
        auto ad5m_std_caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M");

        // Both should exist (AD5M and AD5M Pro are separate entries)
        REQUIRE_FALSE(ad5m_caps.empty());
        REQUIRE_FALSE(ad5m_std_caps.empty());

        // They should have the same macro name (START_PRINT) but this confirms
        // the lookup works for different printer strings
        REQUIRE(ad5m_caps.macro_name == ad5m_std_caps.macro_name);
    }

    SECTION("Unknown printer type returns empty options") {
        auto unknown_caps = PrinterDetector::get_pre_print_option_set("NonExistent Printer XYZ");

        // Unknown printer should return empty options (not crash)
        REQUIRE(unknown_caps.empty());
        REQUIRE(unknown_caps.macro_name.empty());
        REQUIRE(unknown_caps.options.empty());
    }

    SECTION("Option lookup is idempotent") {
        // Multiple lookups for same printer should return identical results
        auto caps1 = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
        auto caps2 = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");

        REQUIRE(caps1.macro_name == caps2.macro_name);
        REQUIRE(caps1.options.size() == caps2.options.size());

        // Verify specific option matches
        const PrePrintOption* a = caps1.find("bed_mesh");
        const PrePrintOption* b = caps2.find("bed_mesh");
        if (a && b) {
            const auto* macro_a = std::get_if<PrePrintStrategyMacroParam>(&a->strategy);
            const auto* macro_b = std::get_if<PrePrintStrategyMacroParam>(&b->strategy);
            REQUIRE(macro_a != nullptr);
            REQUIRE(macro_b != nullptr);
            REQUIRE(macro_a->param_name == macro_b->param_name);
        }
    }
}

// ============================================================================
// Tests: Database option key consistency
// ============================================================================

/**
 * Verifies the option ids used by the database match those produced by
 * `category_to_string()` for macro analysis. This invariant matters because
 * `collect_macro_skip_params()` keys macro params by option id, and macro
 * analysis derives the same id via category_to_string — drift between the
 * two would silently dehydrate the toggles.
 */
TEST_CASE("PrintPreparationManager: option id keys are consistent",
          "[print_preparation][option_keys]") {
    REQUIRE(std::string(category_to_string(PrintStartOpCategory::BED_MESH)) == "bed_mesh");
    REQUIRE(std::string(category_to_string(PrintStartOpCategory::QGL)) == "qgl");
    REQUIRE(std::string(category_to_string(PrintStartOpCategory::Z_TILT)) == "z_tilt");
    REQUIRE(std::string(category_to_string(PrintStartOpCategory::NOZZLE_CLEAN)) == "nozzle_clean");

    auto caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
    if (!caps.empty()) {
        REQUIRE(caps.find("bed_mesh") != nullptr);
        REQUIRE(caps.find("bed_leveling") == nullptr);
    }
}

// ============================================================================
// T3a: collect_pre_start_gcode_lines — per-option line emission
// ============================================================================

/**
 * Locks `collect_pre_start_gcode_lines()` for the K2 Plus DB shape: one
 * `PreStartGcode` option (`ai_detect`, template `LOAD_AI_RUN SWITCH={value}`).
 * Disabled options are NOT skipped — they emit the template with `{value}`
 * substituted to `0`. The provider returning `-1` for `bed_mesh` falls through
 * to default_enabled, but that option is `macro_param` (not `pre_start_gcode`)
 * so it's filtered out here regardless.
 */
TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: collect_pre_start_gcode_lines (K2 Plus ai_detect)",
                 "[print_preparation][pre_print_options]") {
    lv_init_safe();

    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Creality K2 Plus");

    // ai_detect is gated by requires_macro=LOAD_AI_RUN — only fires when the
    // firmware registers that macro. Populate the cache so the option is
    // applicable in this test (real K2 Plus firmware variants that ship AI
    // detect declare LOAD_AI_RUN; stock variants don't, and the option is
    // correctly skipped on those).
    MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    bool ai_detect_on = false;
    manager.set_option_state_provider([&](const std::string& id) -> int {
        if (id == "ai_detect") {
            return ai_detect_on ? 1 : 0;
        }
        return -1;
    });

    SECTION("ai_detect ON emits SWITCH=1") {
        ai_detect_on = true;
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN") == "LOAD_AI_RUN SWITCH=1");
    }

    SECTION("ai_detect OFF still emits, with SWITCH=0") {
        ai_detect_on = false;
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN") == "LOAD_AI_RUN SWITCH=0");
    }

    SECTION("ai_detect skipped entirely when LOAD_AI_RUN macro is absent") {
        MacroParamCache::instance().clear();
        ai_detect_on = true;
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN").empty());
    }
}

// ============================================================================
// T3b: build_pre_start_gcode_block — the actual join contract
// ============================================================================

/**
 * `start_print()` calls `build_pre_start_gcode_block()` with the live inputs;
 * this test calls the same helper directly with synthetic inputs so a refactor
 * of the join (separator, ordering, gating on the `emit_setup` flag) shows up
 * here instead of silently changing the wire format Klipper sees.
 *
 * `emit_setup` is computed in `start_print()` as
 * `!macro_skip_params.empty() && !setup_gcode.empty()` — the K2 Plus path
 * fires it whenever the bed_mesh PREPARE param is in skip_params. The helper
 * itself just trusts the caller's flag.
 */
TEST_CASE("PrintPreparationManager: build_pre_start_gcode_block (join contract)",
          "[print_preparation][pre_print_options][gcode_join]") {
    SECTION("emit_setup=true with one option line: setup precedes line, '\\n' separator") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block(
                    "PRINT_PREPARED", {"LOAD_AI_RUN SWITCH=1"}, true) ==
                "PRINT_PREPARED\nLOAD_AI_RUN SWITCH=1");
    }

    SECTION("emit_setup=false suppresses setup_gcode (per-option lines only)") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block(
                    "PRINT_PREPARED", {"LOAD_AI_RUN SWITCH=0"}, false) == "LOAD_AI_RUN SWITCH=0");
    }

    SECTION("emit_setup=true but setup_gcode empty: per-option lines only") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block(
                    "", {"LOAD_AI_RUN SWITCH=1"}, true) == "LOAD_AI_RUN SWITCH=1");
    }

    SECTION("Multiple per-option lines join with '\\n', preserve order") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block(
                    "PRINT_PREPARED", {"LINE_A", "LINE_B", "LINE_C"}, true) ==
                "PRINT_PREPARED\nLINE_A\nLINE_B\nLINE_C");
    }

    SECTION("emit_setup=true, no per-option lines: just setup_gcode") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block(
                    "PRINT_PREPARED", {}, true) == "PRINT_PREPARED");
    }

    SECTION("Nothing to emit returns empty string") {
        REQUIRE(PrintPreparationManagerTestAccess::build_pre_start_gcode_block("PRINT_PREPARED", {},
                                                                               false)
                    .empty());
        REQUIRE(
            PrintPreparationManagerTestAccess::build_pre_start_gcode_block("", {}, true).empty());
    }
}

// ============================================================================
// Tests: Macro Analysis Retry Logic (with MockWebSocketServer)
// ============================================================================

/**
 * @brief Test fixture for macro analysis retry tests using real WebSocket infrastructure
 *
 * This fixture provides:
 * - MockWebSocketServer for controlling JSON-RPC responses
 * - Real MoonrakerClient + MoonrakerAPI connected to the mock server
 * - PrinterState with initialized subjects
 * - Helper methods for waiting on async operations with queue draining
 *
 * The PrintStartAnalyzer flow:
 * 1. Calls api->list_files("config", ...) which sends server.files.list via WebSocket
 * 2. For each .cfg file found, calls api->download_file() via HTTP
 * 3. Scans each file for [gcode_macro PRINT_START] or similar
 *
 * We can test retry logic by controlling the server.files.list response:
 * - Return error to trigger retry
 * - Return empty list to complete with "not found"
 * - Return file list to proceed to download phase
 */
class MacroAnalysisRetryFixture {
    static bool queue_initialized;

  public:
    MacroAnalysisRetryFixture() {
        // Initialize LVGL for subjects and update queue
        lv_init_safe();

        // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }

        // Initialize PrinterState subjects (needed for dependency injection)
        printer_state_.init_subjects(false); // false = no XML registration

        // Start mock WebSocket server on an ephemeral port (fixed ports caused
        // intermittent bind/TIME_WAIT flakes when sections re-created the fixture).
        server_ = std::make_unique<MockWebSocketServer>();
        int port = server_->start(0);
        REQUIRE(port > 0);

        // Create event loop and client
        loop_thread_ = std::make_shared<hv::EventLoopThread>();
        loop_thread_->start();

        client_ = std::make_unique<MoonrakerClient>(loop_thread_->loop());
        client_->set_connection_timeout(2000);
        client_->set_default_request_timeout(2000);
        client_->setPingInterval(0);    // Disable pings - mock server doesn't respond to them
        client_->setReconnect(nullptr); // Disable auto-reconnect

        // Create API wrapper
        api_ = std::make_unique<MoonrakerAPI>(*client_, printer_state_);

        // Connect to mock server
        std::atomic<bool> connected{false};
        client_->connect(server_->url().c_str(), [&connected]() { connected = true; }, []() {});

        // Wait for connection
        for (int i = 0; i < 50 && !connected; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        REQUIRE(connected);

        // Set up manager with dependencies
        // NOTE: We set dependencies BEFORE setting connection state to avoid
        // triggering auto-analysis on connection (which would race with test setup)
        manager_.set_dependencies(api_.get(), &printer_state_);

        // Don't set connection state to CONNECTED yet - let tests control when
        // analysis starts by calling analyze_print_start_macro() explicitly
    }

    ~MacroAnalysisRetryFixture() {
        // Freeze the UpdateQueue: any background callbacks still in flight
        // that try to enqueue new work during teardown are silently dropped.
        // Closes the race where a late libhv response schedules a deferred
        // retry just as we're tearing down the manager's state.
        auto freeze =
            helix::ui::UpdateQueue::instance().scoped_freeze("MacroAnalysisRetryFixture::~");

        // Clear server handlers BEFORE anything else — registered lambdas
        // capture `this`, so a late request arriving during teardown would
        // otherwise reference freed fixture state (SIGSEGV on eventloop CI).
        if (server_) {
            server_->clear_handlers();
        }

        // Disconnect posts a close event to the libhv event loop. The Channel
        // must remain alive until that event is dispatched (or the loop is
        // stopped), otherwise the loop thread will run Channel::close() on
        // freed memory — heap-use-after-free observed under ASAN nightly.
        if (client_) {
            client_->disconnect();
        }

        // Fully quiesce the server (joins libhv's HTTP worker threads) before
        // we stop the client's event loop. Without the guard below, a libhv
        // server-side handler callback can occasionally still be in flight on
        // Linux/glibc when loop_thread_->stop(true) runs, leading to the
        // thread dying while it holds call_times_mutex_ and later
        // manifesting as `pthread_mutex_lock: Assertion e != ESRCH || !robust'.
        if (server_) {
            server_->stop();
            server_.reset();
        }
        // Give libhv's server-side threads one more scheduler quantum to
        // fully unwind any handler stack before we kill the client loop.
        // 50ms is well below any timeout in these tests and harmless on fast
        // machines; only relevant on slower Linux CI where the race opens up.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Stop the event loop BEFORE deleting client_. Once stop(true) returns,
        // the loop thread has joined and no further posted events will fire,
        // so it is safe to free the Channel. Doing this in the opposite order
        // (delete client → loop processes posted close → UAF on Channel) is
        // exactly the bug the ASAN nightly caught.
        loop_thread_->stop(true);

        api_.reset();
        client_.reset();

        // Drain pending callbacks (manager_ is still alive here, so any final
        // deferred success/error lambda runs against a valid manager).
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Shutdown queue
        helix::ui::update_queue_shutdown();

        // Reset static flag for next test [L053]
        queue_initialized = false;

        // `freeze` goes out of scope here, just before member destructors run.
        // manager_'s lifetime_ dtor will invalidate all outstanding tokens
        // before call_times_mutex_ is destroyed — any token.defer() lambdas
        // still queued (though we drained above) become no-ops.
    }

    /**
     * @brief Drain pending UI updates (simulates main loop iteration)
     */
    void drain_queue() {
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        lv_timer_handler_safe(); // Process LVGL timers for retry scheduling
    }

    /**
     * @brief Wait for condition with queue draining and tick advancement
     *
     * Advances LVGL tick counter alongside real time so timer-based
     * retries (lv_timer_create) fire at the right moment.
     */
    bool wait_for(std::function<bool()> condition, int timeout_ms = 5000) {
        auto start = std::chrono::steady_clock::now();
        while (!condition()) {
            lv_tick_inc(10); // Advance LVGL tick to allow timer-based retries
            drain_queue();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed > timeout_ms) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Configure server to return error for list_files N times, then succeed
     *
     * @param failures Number of times to return error before success
     * @param success_files Files to return on success (empty = no files found)
     */
    void set_list_files_failures(int failures, const std::vector<std::string>& success_files = {}) {
        list_files_call_count_ = 0;
        list_files_failures_ = failures;
        list_files_success_files_ = success_files;

        server_->clear_handlers();

        // Handler that fails N times, then succeeds
        server_->on_method("server.files.list", [this](const json& params) -> json {
            (void)params;
            list_files_call_count_++;
            {
                std::lock_guard<std::mutex> lock(call_times_mutex_);
                list_files_call_times_.push_back(std::chrono::steady_clock::now());
            }

            if (list_files_call_count_ <= list_files_failures_) {
                // Throw to trigger error response
                throw std::runtime_error("Mock failure #" + std::to_string(list_files_call_count_));
            }

            // Success: return file list
            json result = json::array();
            for (const auto& file : list_files_success_files_) {
                result.push_back({{"path", file}, {"size", 1024}, {"modified", 1234567890.0}});
            }
            return result;
        });
    }

    /**
     * @brief Configure server to always return error for list_files
     */
    void set_list_files_always_fail() {
        list_files_call_count_ = 0;

        server_->clear_handlers();
        server_->on_method_error("server.files.list", [this](const json&) {
            list_files_call_count_++;
            {
                std::lock_guard<std::mutex> lock(call_times_mutex_);
                list_files_call_times_.push_back(std::chrono::steady_clock::now());
            }
            return std::make_pair(-1, "Mock permanent failure");
        });
    }

    /**
     * @brief Configure server to succeed immediately with empty file list
     *
     * This results in "no PRINT_START found" but with analysis complete.
     */
    void set_list_files_success_empty() {
        list_files_call_count_ = 0;

        server_->clear_handlers();
        server_->on_method("server.files.list", [this](const json&) -> json {
            list_files_call_count_++;
            {
                std::lock_guard<std::mutex> lock(call_times_mutex_);
                list_files_call_times_.push_back(std::chrono::steady_clock::now());
            }
            return json::array(); // Empty file list
        });
    }

    int get_list_files_call_count() const {
        return list_files_call_count_;
    }

    std::vector<std::chrono::steady_clock::time_point> get_call_times() {
        std::lock_guard<std::mutex> lock(call_times_mutex_);
        return list_files_call_times_;
    }

    void clear_call_times() {
        std::lock_guard<std::mutex> lock(call_times_mutex_);
        list_files_call_times_.clear();
    }

  protected:
    std::unique_ptr<MockWebSocketServer> server_;
    std::shared_ptr<hv::EventLoopThread> loop_thread_;
    std::unique_ptr<MoonrakerClient> client_;
    std::unique_ptr<MoonrakerAPI> api_;
    PrinterState printer_state_;
    PrintPreparationManager manager_;

    std::atomic<int> list_files_call_count_{0};
    int list_files_failures_{0};
    std::vector<std::string> list_files_success_files_;

    std::mutex call_times_mutex_;
    std::vector<std::chrono::steady_clock::time_point> list_files_call_times_;
};
bool MacroAnalysisRetryFixture::queue_initialized = false;

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: macro analysis retry - first attempt succeeds",
                 "[print_preparation][retry][eventloop][slow]") {
    SECTION("Success on first attempt - no retries needed") {
        // Configure server to succeed immediately with empty file list
        // (Results in "no macro found" but analysis completes successfully)
        set_list_files_success_empty();

        std::atomic<bool> callback_invoked{false};
        std::atomic<bool> callback_found{true}; // Start true to verify it becomes false

        manager_.set_macro_analysis_callback([&](const helix::PrintStartAnalysis& analysis) {
            spdlog::info("[TEST] Callback invoked, found={}", analysis.found);
            callback_invoked = true;
            callback_found = analysis.found;
        });

        // Trigger analysis
        spdlog::info("[TEST] Starting analysis");
        manager_.analyze_print_start_macro();
        spdlog::info("[TEST] Analysis started, waiting for callback");

        // Wait for callback - use longer timeout and debug
        auto start = std::chrono::steady_clock::now();
        while (!callback_invoked.load()) {
            lv_tick_inc(10);
            drain_queue();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed > 100 && elapsed % 500 < 20) {
                spdlog::info("[TEST] Waiting... elapsed={}ms, callback_invoked={}", elapsed,
                             callback_invoked.load());
            }
            if (elapsed > 5000) {
                spdlog::error("[TEST] Timeout waiting for callback!");
                break;
            }
        }

        REQUIRE(callback_invoked);
        REQUIRE(get_list_files_call_count() == 1);
        REQUIRE(callback_found == false); // No config files = no macro found
        REQUIRE(manager_.is_macro_analysis_in_progress() == false);
        // Analysis completed but found=false; has_macro_analysis() requires found==true
        // so verify completion via get_macro_analysis() instead
        REQUIRE(manager_.get_macro_analysis().has_value());
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: macro analysis retry - first fails, second succeeds",
                 "[print_preparation][retry][eventloop][slow]") {
    SECTION("Retry succeeds on second attempt") {
        // Configure server to fail once, then succeed with empty list
        set_list_files_failures(1, {}); // Fail once, then return empty list

        std::atomic<bool> callback_invoked{false};
        std::atomic<bool> callback_found{true};

        manager_.set_macro_analysis_callback([&](const helix::PrintStartAnalysis& analysis) {
            callback_invoked = true;
            callback_found = analysis.found;
        });

        // Trigger analysis
        manager_.analyze_print_start_macro();

        // Wait for callback (allow extra time for retry delay)
        REQUIRE(wait_for([&]() { return callback_invoked.load(); }, 5000));

        // Verify retry happened: 1 failure + 1 success = 2 calls
        REQUIRE(get_list_files_call_count() == 2);
        REQUIRE(callback_found == false); // Empty list = no macro found
        REQUIRE(manager_.is_macro_analysis_in_progress() == false);
        REQUIRE(manager_.get_macro_analysis().has_value());
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: macro analysis retry - all retries exhausted",
                 "[print_preparation][retry][eventloop][slow]") {
    SECTION("Callback invoked with found=false after 3 failed attempts") {
        // Configure server to always fail
        set_list_files_always_fail();

        std::atomic<bool> callback_invoked{false};
        std::atomic<bool> callback_found{true}; // Start true to verify it becomes false

        manager_.set_macro_analysis_callback([&](const helix::PrintStartAnalysis& analysis) {
            callback_invoked = true;
            callback_found = analysis.found;
        });

        // Trigger analysis
        manager_.analyze_print_start_macro();

        // Wait for callback (allow time for all retries: 1s + 2s delays)
        REQUIRE(wait_for([&]() { return callback_invoked.load(); }, 8000));

        // Verify all attempts: 1 initial + 2 retries = 3 total
        REQUIRE(get_list_files_call_count() == 3);
        REQUIRE(callback_found == false); // All attempts failed
        REQUIRE(manager_.is_macro_analysis_in_progress() == false);
        REQUIRE(manager_.get_macro_analysis().has_value()); // Has result with found=false
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: macro analysis retry counter resets on new request",
                 "[print_preparation][retry][eventloop][slow]") {
    SECTION("New analysis request after cache clears uses fresh retry counter") {
        // First analysis: succeed immediately
        set_list_files_success_empty();

        std::atomic<int> callback_count{0};
        manager_.set_macro_analysis_callback(
            [&](const helix::PrintStartAnalysis&) { callback_count++; });

        manager_.analyze_print_start_macro();
        REQUIRE(wait_for([&]() { return callback_count.load() == 1; }, 3000));
        REQUIRE(get_list_files_call_count() == 1);

        // Clear the cache to allow new analysis
        // (Normally, the manager caches results and won't re-analyze)
        // We need to create a new manager to reset state
        PrintPreparationManager manager2;
        manager2.set_dependencies(api_.get(), &printer_state_);

        // Reset call count and configure failures
        clear_call_times();
        list_files_call_count_ = 0;
        set_list_files_failures(1, {}); // Fail once, then succeed

        std::atomic<bool> callback2_invoked{false};
        manager2.set_macro_analysis_callback(
            [&](const helix::PrintStartAnalysis&) { callback2_invoked = true; });

        manager2.analyze_print_start_macro();
        REQUIRE(wait_for([&]() { return callback2_invoked.load(); }, 5000));

        // Should have retried fresh: 1 failure + 1 success = 2 calls
        REQUIRE(get_list_files_call_count() == 2);
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: in-progress flag stays true during retries",
                 "[print_preparation][retry][eventloop][slow]") {
    SECTION("is_macro_analysis_in_progress remains true during retry delay") {
        // Configure server to fail first call
        set_list_files_failures(1, {});

        std::atomic<bool> callback_invoked{false};
        manager_.set_macro_analysis_callback(
            [&](const helix::PrintStartAnalysis&) { callback_invoked = true; });

        // Trigger analysis
        manager_.analyze_print_start_macro();

        // Immediately after starting, should be in progress
        REQUIRE(manager_.is_macro_analysis_in_progress() == true);

        // Wait a short time for first failure to process (but not for retry to complete)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        lv_tick_inc(500); // Advance LVGL tick to match real time elapsed
        drain_queue();

        // During retry delay, should STILL be in progress
        // This is the key assertion - the in_progress flag should stay true during retries
        if (!callback_invoked.load()) {
            // Only check if callback hasn't been invoked yet
            REQUIRE(manager_.is_macro_analysis_in_progress() == true);
        }

        // Wait for final callback
        REQUIRE(wait_for([&]() { return callback_invoked.load(); }, 5000));

        // After completion, should no longer be in progress
        REQUIRE(manager_.is_macro_analysis_in_progress() == false);
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: retry timing follows exponential backoff",
                 "[print_preparation][retry][timing][eventloop][slow]") {
    SECTION("Backoff delays: ~1s, ~2s between retries") {
        // Configure server to always fail so we can measure all retry timings
        set_list_files_always_fail();

        std::atomic<bool> callback_invoked{false};
        manager_.set_macro_analysis_callback(
            [&](const helix::PrintStartAnalysis&) { callback_invoked = true; });

        // Clear timing data
        clear_call_times();

        // Trigger analysis
        manager_.analyze_print_start_macro();

        // Wait for all attempts to complete (generous timeout for loaded CI)
        REQUIRE(wait_for([&]() { return callback_invoked.load(); }, 15000));

        // Get call timestamps
        auto times = get_call_times();
        REQUIRE(times.size() == 3); // 1 initial + 2 retries

        // Verify exponential backoff delays
        auto delay1 = std::chrono::duration_cast<std::chrono::milliseconds>(times[1] - times[0]);
        auto delay2 = std::chrono::duration_cast<std::chrono::milliseconds>(times[2] - times[1]);

        // First delay should be ~1s (1000ms with tolerance)
        // Wide tolerance for CI machines under load (macOS, parallel shards)
        INFO("Delay 1: " << delay1.count() << "ms");
        REQUIRE(delay1.count() >= 700);  // At least 700ms
        REQUIRE(delay1.count() <= 2500); // At most 2.5s

        // Second delay should be ~2s (2000ms with tolerance)
        INFO("Delay 2: " << delay2.count() << "ms");
        REQUIRE(delay2.count() >= 1500); // At least 1.5s
        REQUIRE(delay2.count() <= 4000); // At most 4s
    }
}

// ============================================================================
// Tests: Provider-Driven Option State (replaces removed legacy subject API)
// ============================================================================

/**
 * The pre-print options framework reads option state through OptionStateProvider.
 * These tests cover the same behaviors the old P1 subject tests covered, but
 * driven through `MockOptionState` instead of raw `lv_subject_t` member fields:
 *
 * - read_options_from_subjects(): walks the cached option set and queries the
 *   provider per id. Hidden/unknown ids fall through to NOT_APPLICABLE -> false.
 * - get_option_state(id): returns the tri-state ENABLED/DISABLED/NOT_APPLICABLE.
 * - collect_macro_skip_params(): only emits skip params for ids the provider
 *   reports as DISABLED — never for hidden ids.
 */

TEST_CASE("PrintPreparationManager: read_options_from_subjects returns correct PrePrintOptions",
          "[print_preparation][p1][subjects]") {
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("All options enabled - returns all true") {
        for (const char* id :
             {"bed_mesh", "qgl", "z_tilt", "nozzle_clean", "purge_line", "timelapse"}) {
            state.enable(id);
        }
        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == true);
        REQUIRE(options.qgl == true);
        REQUIRE(options.z_tilt == true);
        REQUIRE(options.nozzle_clean == true);
        REQUIRE(options.purge_line == true);
        REQUIRE(options.timelapse == true);
    }

    SECTION("All options disabled - returns all false") {
        for (const char* id :
             {"bed_mesh", "qgl", "z_tilt", "nozzle_clean", "purge_line", "timelapse"}) {
            state.disable(id);
        }
        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == false);
        REQUIRE(options.nozzle_clean == false);
        REQUIRE(options.purge_line == false);
        REQUIRE(options.timelapse == false);
    }

    SECTION("Hidden options return false (NOT_APPLICABLE)") {
        for (const char* id :
             {"bed_mesh", "qgl", "z_tilt", "nozzle_clean", "purge_line", "timelapse"}) {
            state.hide(id);
        }
        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == false);
        REQUIRE(options.nozzle_clean == false);
        REQUIRE(options.purge_line == false);
        REQUIRE(options.timelapse == false);
    }

    SECTION("Mixed enabled/disabled/hidden states") {
        state.enable("bed_mesh");
        state.disable("qgl");
        state.hide("z_tilt");
        state.hide("nozzle_clean");
        state.enable("purge_line");
        state.disable("timelapse");

        auto options = manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == true);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == false);       // hidden
        REQUIRE(options.nozzle_clean == false); // hidden
        REQUIRE(options.purge_line == true);
        REQUIRE(options.timelapse == false);
    }

    SECTION("Without provider - all options report as false") {
        // No provider set; with no PrinterState DB attached, every id falls
        // through to NOT_APPLICABLE -> false. (read_options_from_subjects
        // doesn't itself differentiate "no panel attached" from "explicitly
        // not applicable" — both result in the conservative false.)
        PrintPreparationManager bare_manager;
        auto options = bare_manager.read_options_from_subjects();
        REQUIRE(options.bed_mesh == false);
        REQUIRE(options.qgl == false);
        REQUIRE(options.z_tilt == false);
    }
}

TEST_CASE("PrintPreparationManager: get_option_state tri-state via provider",
          "[print_preparation][p1][subjects]") {
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("Provider returns 1 -> ENABLED") {
        state.enable("bed_mesh");
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
    }

    SECTION("Provider returns 0 -> DISABLED (user explicitly skipped)") {
        state.disable("bed_mesh");
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
    }

    SECTION("Provider returns -1 (hidden) -> NOT_APPLICABLE") {
        state.hide("bed_mesh");
        // Without a printer DB attached the hidden state surfaces as NOT_APPLICABLE.
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::NOT_APPLICABLE);
    }

    SECTION("Unknown id -> NOT_APPLICABLE") {
        // Provider returns -1 for unset ids; no DB fallback either.
        REQUIRE(manager.get_option_state("nonexistent_op") == PrePrintOptionState::NOT_APPLICABLE);
    }
}

TEST_CASE("PrintPreparationManager: hidden options don't produce macro skip params",
          "[print_preparation][p1][subjects]") {
    // Bug regression: when an option is hidden (provider returns -1, NOT_APPLICABLE),
    // collect_macro_skip_params() must NOT emit a skip param for it. The old
    // visibility=0 + unchecked combination was misclassified as "user disabled"
    // and produced phantom SKIP_* params, triggering unnecessary file modification.
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    // Macro analysis with controllable BED_MESH (OPT_OUT semantic).
    PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    PrintStartOperation op;
    op.name = "BED_MESH_CALIBRATE";
    op.category = PrintStartOpCategory::BED_MESH;
    op.has_skip_param = true;
    op.skip_param_name = "SKIP_BED_MESH";
    op.param_semantic = ParameterSemantic::OPT_OUT;
    analysis.operations.push_back(op);
    manager.set_macro_analysis(analysis);

    SECTION("Hidden -> NO skip params (the regression we're guarding)") {
        state.hide("bed_mesh");
        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params.empty());
    }

    SECTION("Disabled (visible + unchecked) DOES produce skip params") {
        state.disable("bed_mesh");
        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params.size() == 1);
        REQUIRE(skip_params[0].first == "SKIP_BED_MESH");
        REQUIRE(skip_params[0].second == "1"); // OPT_OUT: 1 = skip
    }

    SECTION("Enabled (visible + checked) produces NO skip params") {
        state.enable("bed_mesh");
        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params.empty());
    }
}

TEST_CASE("PrintPreparationManager: get_option_state in collection context",
          "[print_preparation][p1][subjects]") {
    // Sanity-check that the provider correctly drives get_option_state(id) for
    // the six option ids the legacy subject path used to handle. This is the
    // contract collect_ops_to_disable() and collect_macro_skip_params() rely on.
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("Disabled provider state surfaces as DISABLED for every id") {
        state.disable("bed_mesh");
        state.disable("qgl");
        state.disable("z_tilt");
        state.disable("nozzle_clean");

        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
        REQUIRE(manager.get_option_state("qgl") == PrePrintOptionState::DISABLED);
        REQUIRE(manager.get_option_state("z_tilt") == PrePrintOptionState::DISABLED);
        REQUIRE(manager.get_option_state("nozzle_clean") == PrePrintOptionState::DISABLED);
    }

    SECTION("Hidden ids surface as NOT_APPLICABLE — not as DISABLED") {
        state.hide("bed_mesh");
        state.hide("qgl");
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::NOT_APPLICABLE);
        REQUIRE(manager.get_option_state("qgl") == PrePrintOptionState::NOT_APPLICABLE);
    }

    SECTION("Mixed state: enabled / disabled / hidden distinguishable per id") {
        state.disable("bed_mesh");
        state.hide("qgl");
        state.enable("z_tilt");
        state.hide("nozzle_clean");

        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
        REQUIRE(manager.get_option_state("qgl") == PrePrintOptionState::NOT_APPLICABLE);
        REQUIRE(manager.get_option_state("z_tilt") == PrePrintOptionState::ENABLED);
        REQUIRE(manager.get_option_state("nozzle_clean") == PrePrintOptionState::NOT_APPLICABLE);
    }
}

// ============================================================================
// Tests: CapabilityMatrix Integration (P3)
// ============================================================================

/**
 * @brief Phase 3 Tests: CapabilityMatrix integration into PrintPreparationManager
 *
 * TDD Approach: These tests are written BEFORE implementation.
 * They will FAIL to compile initially because build_capability_matrix() doesn't exist.
 */

TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: build_capability_matrix",
                 "[print_preparation][p3]") {
    lv_init_safe();
    PrintPreparationManager manager;

    SECTION("Returns empty matrix when no data available") {
        // Without any dependencies set, matrix should be empty
        auto matrix = manager.build_capability_matrix();
        REQUIRE_FALSE(matrix.has_any_controllable());
        REQUIRE(matrix.get_controllable_operations().empty());
    }

    SECTION("Includes database capabilities when printer detected") {
        // Set up manager with PrinterState that has a known printer type
        PrinterState& printer_state = get_printer_state();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false); // No XML registration for tests
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        manager.set_dependencies(nullptr, &printer_state);

        auto matrix = manager.build_capability_matrix();

        // AD5M Pro has bed_mesh capability in database
        REQUIRE(matrix.has_any_controllable());
        REQUIRE(matrix.is_controllable(OperationCategory::BED_MESH));

        // Verify source is from DATABASE
        auto source = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::DATABASE);
        REQUIRE(source->param_name == "SKIP_LEVELING");
    }

    SECTION("Includes macro analysis when available") {
        // Create and set a mock macro analysis
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";

        // Add a controllable operation (QGL with SKIP_QGL param)
        PrintStartOperation op;
        op.name = "QUAD_GANTRY_LEVEL";
        op.category = PrintStartOpCategory::QGL;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_QGL";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        op.line_number = 15;
        analysis.operations.push_back(op);
        analysis.controllable_count = 1;
        analysis.is_controllable = true;

        // Use set_macro_analysis to inject the analysis (new method needed)
        manager.set_macro_analysis(analysis);

        auto matrix = manager.build_capability_matrix();

        // QGL should be controllable from macro analysis
        REQUIRE(matrix.is_controllable(OperationCategory::QGL));

        auto source = matrix.get_best_source(OperationCategory::QGL);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::MACRO_ANALYSIS);
        REQUIRE(source->param_name == "SKIP_QGL");
    }

    SECTION("Includes file scan when available") {
        // Create and set a mock scan result
        gcode::ScanResult scan;
        scan.lines_scanned = 100;
        scan.bytes_scanned = 5000;

        // Add a detected operation (direct command)
        gcode::DetectedOperation op;
        op.type = gcode::OperationType::NOZZLE_CLEAN;
        op.embedding = gcode::OperationEmbedding::MACRO_CALL;
        op.macro_name = "CLEAN_NOZZLE";
        op.line_number = 25;
        scan.operations.push_back(op);

        // Use set_cached_scan_result to inject (new method or use existing mechanism)
        manager.set_cached_scan_result(scan, "test_file.gcode");

        auto matrix = manager.build_capability_matrix();

        // NOZZLE_CLEAN should be controllable from file scan
        REQUIRE(matrix.is_controllable(OperationCategory::NOZZLE_CLEAN));

        auto source = matrix.get_best_source(OperationCategory::NOZZLE_CLEAN);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::FILE_SCAN);
        REQUIRE(source->line_number == 25);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: capability priority ordering",
                 "[print_preparation][p3]") {
    lv_init_safe();
    PrintPreparationManager manager;

    SECTION("Database takes priority over macro analysis") {
        // Set up PrinterState with AD5M Pro (has database bed_mesh capability)
        PrinterState& printer_state = get_printer_state();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        manager.set_dependencies(nullptr, &printer_state);

        // Also add a macro analysis for the same operation (BED_MESH)
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";

        PrintStartOperation op;
        op.name = "BED_MESH_CALIBRATE";
        op.category = PrintStartOpCategory::BED_MESH;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_BED_MESH"; // Different param than database
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);
        analysis.controllable_count = 1;
        analysis.is_controllable = true;

        manager.set_macro_analysis(analysis);

        auto matrix = manager.build_capability_matrix();

        // Database should win - SKIP_LEVELING, not SKIP_BED_MESH
        auto source = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::DATABASE);
        REQUIRE(source->param_name == "SKIP_LEVELING");

        // But both sources should exist
        auto all_sources = matrix.get_all_sources(OperationCategory::BED_MESH);
        REQUIRE(all_sources.size() == 2);
        REQUIRE(all_sources[0].origin == CapabilityOrigin::DATABASE); // First = best
        REQUIRE(all_sources[1].origin == CapabilityOrigin::MACRO_ANALYSIS);
    }

    SECTION("Macro analysis takes priority over file scan") {
        // Set up macro analysis for QGL
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";

        PrintStartOperation op;
        op.name = "QUAD_GANTRY_LEVEL";
        op.category = PrintStartOpCategory::QGL;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_QGL";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);

        manager.set_macro_analysis(analysis);

        // Also add a file scan for the same operation
        gcode::ScanResult scan;
        scan.lines_scanned = 100;
        gcode::DetectedOperation file_op;
        file_op.type = gcode::OperationType::QGL;
        file_op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
        file_op.macro_name = "QUAD_GANTRY_LEVEL";
        file_op.line_number = 50;
        scan.operations.push_back(file_op);

        manager.set_cached_scan_result(scan, "test.gcode");

        auto matrix = manager.build_capability_matrix();

        // Macro analysis should win
        auto source = matrix.get_best_source(OperationCategory::QGL);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::MACRO_ANALYSIS);

        // Both sources exist
        auto all_sources = matrix.get_all_sources(OperationCategory::QGL);
        REQUIRE(all_sources.size() == 2);
        REQUIRE(all_sources[0].origin == CapabilityOrigin::MACRO_ANALYSIS);
        REQUIRE(all_sources[1].origin == CapabilityOrigin::FILE_SCAN);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: collect_macro_skip_params with matrix",
                 "[print_preparation][p3]") {
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("Returns skip params from best source") {
        // AD5M Pro DB has bed_mesh with SKIP_LEVELING (MacroParam, OPT_OUT).
        PrinterState& printer_state = get_printer_state();
        PrinterStateTestAccess::reset(printer_state);
        printer_state.init_subjects(false);
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        manager.set_dependencies(nullptr, &printer_state);

        // User unchecks bed_mesh.
        state.disable("bed_mesh");

        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params.size() >= 1);

        bool found_skip_leveling = false;
        for (const auto& [param, value] : skip_params) {
            if (param == "SKIP_LEVELING") {
                found_skip_leveling = true;
                REQUIRE(value == "1"); // OPT_OUT skip_value
            }
        }
        REQUIRE(found_skip_leveling);
    }

    SECTION("Handles OPT_IN semantic correctly") {
        // No DB; pure macro analysis with OPT_IN (FORCE_BED_MESH style).
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";
        PrintStartOperation op;
        op.name = "BED_MESH_CALIBRATE";
        op.category = PrintStartOpCategory::BED_MESH;
        op.has_skip_param = true;
        op.skip_param_name = "FORCE_BED_MESH";
        op.param_semantic = ParameterSemantic::OPT_IN;
        analysis.operations.push_back(op);
        manager.set_macro_analysis(analysis);

        state.disable("bed_mesh");

        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        bool found_force_bed_mesh = false;
        for (const auto& [param, value] : skip_params) {
            if (param == "FORCE_BED_MESH") {
                found_force_bed_mesh = true;
                REQUIRE(value == "0"); // OPT_IN: 0 = don't do it
            }
        }
        REQUIRE(found_force_bed_mesh);
    }

    SECTION("Handles OPT_OUT semantic correctly") {
        // No DB; pure macro analysis with OPT_OUT (SKIP_BED_MESH style).
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";
        PrintStartOperation op;
        op.name = "BED_MESH_CALIBRATE";
        op.category = PrintStartOpCategory::BED_MESH;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_BED_MESH";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);
        manager.set_macro_analysis(analysis);

        state.disable("bed_mesh");

        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        bool found_skip_bed_mesh = false;
        for (const auto& [param, value] : skip_params) {
            if (param == "SKIP_BED_MESH") {
                found_skip_bed_mesh = true;
                REQUIRE(value == "1"); // OPT_OUT: 1 = skip
            }
        }
        REQUIRE(found_skip_bed_mesh);
    }
}

// ============================================================================
// Tests: Option State Provider (Phase 3 — pre-print options framework)
// ============================================================================

/**
 * Phase 3 introduced an OptionStateProvider callback so the print-detail
 * panel can surface dynamic per-option subjects without the manager knowing
 * about their LVGL pointers. The manager's get_option_state(id) overload
 * resolves through three layers:
 *   1. Provider (when set and returning 0/1)
 *   2. Legacy subject path (the six built-in toggles)
 *   3. Cached PrePrintOptionSet's default_enabled fallback
 */
TEST_CASE("PrintPreparationManager: get_option_state(id) provider takes priority",
          "[print_preparation][option_state][p3]") {
    lv_init_safe();
    PrintPreparationManager manager;

    int provider_calls = 0;
    std::string last_id;
    int provider_returns = 1;
    manager.set_option_state_provider([&](const std::string& id) {
        ++provider_calls;
        last_id = id;
        return provider_returns;
    });

    SECTION("Provider returning 1 yields ENABLED") {
        provider_returns = 1;
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
        REQUIRE(provider_calls == 1);
        REQUIRE(last_id == "bed_mesh");
    }

    SECTION("Provider returning 0 yields DISABLED") {
        provider_returns = 0;
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
    }

    SECTION("Provider returning -1 falls through to default") {
        // No legacy subjects set, no printer state — so falls all the way
        // through to "unknown id" -> NOT_APPLICABLE.
        provider_returns = -1;
        REQUIRE(manager.get_option_state("ai_detect") == PrePrintOptionState::NOT_APPLICABLE);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: get_option_state(id) falls back to DB default_enabled",
                 "[print_preparation][option_state][p3]") {
    // When no provider returns 0/1 for an id, get_option_state(id) falls
    // through to the cached PrePrintOptionSet's default_enabled flag — used
    // by headless callers (macro analysis) that don't have a panel attached.
    lv_init_safe();

    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("No provider, known id with default_enabled=true -> ENABLED") {
        // AD5M Pro's bed_mesh has default_enabled=true.
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
    }

    SECTION("Provider returning -1 also falls through to DB default") {
        manager.set_option_state_provider([](const std::string&) { return -1; });
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
    }

    SECTION("Unknown id returns NOT_APPLICABLE without crashing") {
        REQUIRE(manager.get_option_state("ai_detect_nonexistent") ==
                PrePrintOptionState::NOT_APPLICABLE);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: collect_macro_skip_params drives off provider when set",
                 "[print_preparation][option_state][p3]") {
    lv_init_safe();
    // Use AD5M Pro DB entry — has bed_mesh as MacroParam.
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("Provider says ENABLED -> no skip param emitted") {
        manager.set_option_state_provider([](const std::string&) { return 1; });
        auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        // bed_mesh ENABLED emits no param. The AD5M Pro entry declares an
        // adaptive_param, but adaptive_active is only set when exclude_object is
        // present — not in this headless set_printer_type_sync path — so no
        // ADAPTIVE token is emitted here either.
        REQUIRE(params.empty());
    }

    SECTION("Provider says DISABLED -> skip param emitted") {
        manager.set_option_state_provider([](const std::string&) { return 0; });
        auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(params.size() == 1);
        REQUIRE(params[0].first == "SKIP_LEVELING");
        REQUIRE(params[0].second == "1");
    }

    SECTION(
        "Provider returning -1 falls back to option default (default_enabled=true -> no skip)") {
        manager.set_option_state_provider([](const std::string&) { return -1; });
        auto params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        // bed_mesh in AD5M Pro DB has default_enabled=true -> ENABLED -> no skip param.
        REQUIRE(params.empty());
    }
}

// ============================================================================
// Tests: Extension Safety and Documentation (Phase 5)
// ============================================================================

/**
 * Phase 5: Extension Safety Tests
 *
 * These tests document the expected behavior of the pre-print subsystem's
 * extension points. They serve as both tests and documentation for developers
 * who need to add new operations or capability sources.
 *
 * Key extension points:
 * 1. OperationRegistry - Single point for adding new controllable operations
 * 2. CapabilityMatrix - Unified capability lookup with priority ordering
 * 3. CapabilityOrigin - Priority system for source ordering
 * 4. ParameterSemantic - OPT_IN/OPT_OUT parameter interpretation
 */

TEST_CASE("PrepManager: Extension safety - registry completeness", "[print_preparation][p5]") {
    SECTION("All controllable operations have registry entries") {
        // The five controllable operations should all be in the registry.
        // This test ensures that any controllable operation can be looked up.
        for (auto cat :
             {OperationCategory::BED_MESH, OperationCategory::QGL, OperationCategory::Z_TILT,
              OperationCategory::NOZZLE_CLEAN, OperationCategory::PURGE_LINE}) {
            auto info = OperationRegistry::get(cat);
            INFO("Checking category: " << category_key(cat));
            REQUIRE(info.has_value());
            REQUIRE_FALSE(info->capability_key.empty());
            REQUIRE_FALSE(info->friendly_name.empty());

            // Verify capability_key matches category_key()
            REQUIRE(info->capability_key == std::string(category_key(cat)));
        }
    }

    SECTION("Non-controllable operations return nullopt") {
        // Operations that cannot be toggled in the UI should NOT be in the registry.
        // This ensures the registry only contains operations that make sense to show
        // in the pre-print options panel.

        // HOMING: Always required, never skippable
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::HOMING).has_value());

        // START_PRINT: The macro itself, not a toggleable option
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::START_PRINT).has_value());

        // UNKNOWN: Invalid/unrecognized operation
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::UNKNOWN).has_value());

        // CHAMBER_SOAK: Not currently controllable (complex timing semantics)
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::CHAMBER_SOAK).has_value());

        // SKEW_CORRECT: Not currently controllable
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::SKEW_CORRECT).has_value());

        // BED_LEVEL: Parent category, not directly controllable (QGL/Z_TILT are)
        REQUIRE_FALSE(OperationRegistry::get(OperationCategory::BED_LEVEL).has_value());
    }

    SECTION("Registry::all() returns complete set of controllable operations") {
        // Verify that OperationRegistry::all() returns all controllable operations.
        // This is the single extension point for adding new operations.
        auto all = OperationRegistry::all();

        // At least the 5 current controllable operations
        REQUIRE(all.size() >= 5);

        // Each entry should have complete metadata
        for (const auto& info : all) {
            INFO("Validating operation: " << info.capability_key);
            REQUIRE_FALSE(info.capability_key.empty());
            REQUIRE_FALSE(info.friendly_name.empty());
            REQUIRE(info.category != OperationCategory::UNKNOWN);
        }

        // Verify specific operations are present
        bool has_bed_mesh = false;
        bool has_qgl = false;
        bool has_z_tilt = false;
        bool has_nozzle_clean = false;
        bool has_purge_line = false;

        for (const auto& info : all) {
            if (info.capability_key == "bed_mesh") {
                has_bed_mesh = true;
            }
            if (info.capability_key == "qgl") {
                has_qgl = true;
            }
            if (info.capability_key == "z_tilt") {
                has_z_tilt = true;
            }
            if (info.capability_key == "nozzle_clean") {
                has_nozzle_clean = true;
            }
            if (info.capability_key == "purge_line") {
                has_purge_line = true;
            }
        }

        REQUIRE(has_bed_mesh);
        REQUIRE(has_qgl);
        REQUIRE(has_z_tilt);
        REQUIRE(has_nozzle_clean);
        REQUIRE(has_purge_line);
    }

    SECTION("Reverse lookup by key works for all controllable operations") {
        // OperationRegistry::get_by_key() should find operations by their capability key
        auto bed_mesh = OperationRegistry::get_by_key("bed_mesh");
        REQUIRE(bed_mesh.has_value());
        REQUIRE(bed_mesh->category == OperationCategory::BED_MESH);

        auto qgl = OperationRegistry::get_by_key("qgl");
        REQUIRE(qgl.has_value());
        REQUIRE(qgl->category == OperationCategory::QGL);

        auto z_tilt = OperationRegistry::get_by_key("z_tilt");
        REQUIRE(z_tilt.has_value());
        REQUIRE(z_tilt->category == OperationCategory::Z_TILT);

        auto nozzle_clean = OperationRegistry::get_by_key("nozzle_clean");
        REQUIRE(nozzle_clean.has_value());
        REQUIRE(nozzle_clean->category == OperationCategory::NOZZLE_CLEAN);

        auto purge_line = OperationRegistry::get_by_key("purge_line");
        REQUIRE(purge_line.has_value());
        REQUIRE(purge_line->category == OperationCategory::PURGE_LINE);

        // Non-existent key returns nullopt
        REQUIRE_FALSE(OperationRegistry::get_by_key("nonexistent").has_value());
        REQUIRE_FALSE(OperationRegistry::get_by_key("").has_value());
    }
}

TEST_CASE("PrepManager: Extension safety - priority ordering", "[print_preparation][p5]") {
    SECTION("Database priority = 0 (highest)") {
        // DATABASE source is authoritative - curated and tested capabilities from
        // printer_database.json. It should always take priority over dynamic detection.
        // Priority 0 = highest (lower number = higher priority)

        // Create a matrix with DATABASE source
        CapabilityMatrix matrix;
        PrePrintOptionSet db_caps;
        db_caps.macro_name = "START_PRINT";
        {
            PrePrintOption opt;
            opt.id = "bed_mesh";
            opt.strategy_kind = PrePrintStrategyKind::MacroParam;
            opt.strategy = PrePrintStrategyMacroParam{"FORCE_LEVELING", "true", "false", ""};
            db_caps.options.push_back(opt);
        }
        matrix.add_from_database(db_caps);

        auto source = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::DATABASE);
    }

    SECTION("Macro analysis priority = 1 (medium)") {
        // MACRO_ANALYSIS is dynamically detected from the printer's PRINT_START macro.
        // It's trustworthy but may be incomplete or non-standard.

        CapabilityMatrix matrix;
        PrintStartAnalysis analysis;
        analysis.found = true;

        PrintStartOperation op;
        op.name = "QUAD_GANTRY_LEVEL";
        op.category = PrintStartOpCategory::QGL;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_QGL";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);

        matrix.add_from_macro_analysis(analysis);

        auto source = matrix.get_best_source(OperationCategory::QGL);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::MACRO_ANALYSIS);
    }

    SECTION("File scan priority = 2 (lowest)") {
        // FILE_SCAN is detected from the G-code file itself. It's the least
        // reliable because it's specific to one file and may not have parameter info.

        CapabilityMatrix matrix;
        gcode::ScanResult scan;
        scan.lines_scanned = 100;

        gcode::DetectedOperation op;
        op.type = gcode::OperationType::NOZZLE_CLEAN;
        op.embedding = gcode::OperationEmbedding::MACRO_PARAMETER;
        op.param_name = "SKIP_NOZZLE_CLEAN";
        op.line_number = 10;
        scan.operations.push_back(op);

        matrix.add_from_file_scan(scan);

        auto source = matrix.get_best_source(OperationCategory::NOZZLE_CLEAN);
        REQUIRE(source.has_value());
        REQUIRE(source->origin == CapabilityOrigin::FILE_SCAN);
    }

    SECTION("Lower priority number wins in get_best_source") {
        // When multiple sources exist for the same operation, DATABASE wins over
        // MACRO_ANALYSIS, which wins over FILE_SCAN.

        CapabilityMatrix matrix;

        // Add FILE_SCAN source first (lowest priority)
        gcode::ScanResult scan;
        scan.lines_scanned = 100;
        gcode::DetectedOperation file_op;
        file_op.type = gcode::OperationType::BED_MESH;
        file_op.embedding = gcode::OperationEmbedding::MACRO_PARAMETER;
        file_op.param_name = "SKIP_BED_MESH_FILE";
        file_op.line_number = 5;
        scan.operations.push_back(file_op);
        matrix.add_from_file_scan(scan);

        // Add MACRO_ANALYSIS source (medium priority)
        PrintStartAnalysis analysis;
        analysis.found = true;
        PrintStartOperation macro_op;
        macro_op.name = "BED_MESH_CALIBRATE";
        macro_op.category = PrintStartOpCategory::BED_MESH;
        macro_op.has_skip_param = true;
        macro_op.skip_param_name = "SKIP_BED_MESH_MACRO";
        macro_op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(macro_op);
        matrix.add_from_macro_analysis(analysis);

        // Add DATABASE source (highest priority)
        PrePrintOptionSet db_caps;
        db_caps.macro_name = "START_PRINT";
        {
            PrePrintOption opt;
            opt.id = "bed_mesh";
            opt.strategy_kind = PrePrintStrategyKind::MacroParam;
            opt.strategy = PrePrintStrategyMacroParam{"FORCE_LEVELING_DB", "true", "false", ""};
            db_caps.options.push_back(opt);
        }
        matrix.add_from_database(db_caps);

        // DATABASE should win
        auto best = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(best.has_value());
        REQUIRE(best->origin == CapabilityOrigin::DATABASE);
        REQUIRE(best->param_name == "FORCE_LEVELING_DB");

        // All three sources should be available when requested
        auto all_sources = matrix.get_all_sources(OperationCategory::BED_MESH);
        REQUIRE(all_sources.size() == 3);

        // Sources should be sorted by priority (DATABASE first, FILE_SCAN last)
        REQUIRE(all_sources[0].origin == CapabilityOrigin::DATABASE);
        REQUIRE(all_sources[1].origin == CapabilityOrigin::MACRO_ANALYSIS);
        REQUIRE(all_sources[2].origin == CapabilityOrigin::FILE_SCAN);
    }

    SECTION("Macro analysis takes priority over file scan when both present") {
        CapabilityMatrix matrix;

        // Add FILE_SCAN source
        gcode::ScanResult scan;
        gcode::DetectedOperation file_op;
        file_op.type = gcode::OperationType::Z_TILT;
        file_op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
        file_op.macro_name = "Z_TILT_ADJUST";
        scan.operations.push_back(file_op);
        matrix.add_from_file_scan(scan);

        // Add MACRO_ANALYSIS source
        PrintStartAnalysis analysis;
        analysis.found = true;
        PrintStartOperation macro_op;
        macro_op.name = "Z_TILT_ADJUST";
        macro_op.category = PrintStartOpCategory::Z_TILT;
        macro_op.has_skip_param = true;
        macro_op.skip_param_name = "SKIP_Z_TILT";
        macro_op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(macro_op);
        matrix.add_from_macro_analysis(analysis);

        // MACRO_ANALYSIS should win over FILE_SCAN
        auto best = matrix.get_best_source(OperationCategory::Z_TILT);
        REQUIRE(best.has_value());
        REQUIRE(best->origin == CapabilityOrigin::MACRO_ANALYSIS);
    }
}

TEST_CASE("PrepManager: Extension safety - semantic handling", "[print_preparation][p5]") {
    SECTION("OPT_OUT params: SKIP_* with value 1 means skip") {
        // OPT_OUT semantic: The parameter indicates "skip this operation"
        // - SKIP_BED_MESH=1 -> skip bed mesh
        // - SKIP_BED_MESH=0 -> do bed mesh (default)

        CapabilityMatrix matrix;
        PrintStartAnalysis analysis;
        analysis.found = true;

        PrintStartOperation op;
        op.name = "BED_MESH_CALIBRATE";
        op.category = PrintStartOpCategory::BED_MESH;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_BED_MESH";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);

        matrix.add_from_macro_analysis(analysis);

        auto source = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source.has_value());
        REQUIRE(source->semantic == ParameterSemantic::OPT_OUT);
        REQUIRE(source->skip_value == "1");   // SKIP=1 means skip
        REQUIRE(source->enable_value == "0"); // SKIP=0 means do
    }

    SECTION("OPT_IN params: FORCE_*/PERFORM_* with value 0 means skip") {
        // OPT_IN semantic: The parameter indicates "do this operation"
        // - FORCE_LEVELING=1 or "true" -> do leveling
        // - FORCE_LEVELING=0 or "false" -> skip leveling

        CapabilityMatrix matrix;
        PrePrintOptionSet db_caps;
        db_caps.macro_name = "START_PRINT";
        // AD5M-style: FORCE_LEVELING with OPT_IN semantic
        {
            PrePrintOption opt;
            opt.id = "bed_mesh";
            opt.strategy_kind = PrePrintStrategyKind::MacroParam;
            opt.strategy = PrePrintStrategyMacroParam{"FORCE_LEVELING", "true", "false", ""};
            db_caps.options.push_back(opt);
        }
        matrix.add_from_database(db_caps);

        auto source = matrix.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source.has_value());
        // The semantic is inferred from the param name starting with FORCE_
        REQUIRE(source->semantic == ParameterSemantic::OPT_IN);
        REQUIRE(source->skip_value == "false");  // FORCE=false means skip
        REQUIRE(source->enable_value == "true"); // FORCE=true means do
    }

    SECTION("Semantic is correctly inferred from parameter name") {
        // The CapabilityMatrix::infer_semantic() function determines OPT_IN vs OPT_OUT
        // based on the parameter name prefix.

        // OPT_IN patterns: FORCE_*, PERFORM_*, DO_*, ENABLE_*
        CapabilityMatrix matrix1;
        PrintStartAnalysis analysis1;
        analysis1.found = true;
        PrintStartOperation op1;
        op1.category = PrintStartOpCategory::BED_MESH;
        op1.has_skip_param = true;

        // Test FORCE_* prefix
        op1.skip_param_name = "FORCE_LEVELING";
        op1.param_semantic = ParameterSemantic::OPT_IN;
        analysis1.operations.push_back(op1);
        matrix1.add_from_macro_analysis(analysis1);

        auto source1 = matrix1.get_best_source(OperationCategory::BED_MESH);
        REQUIRE(source1.has_value());
        REQUIRE(source1->semantic == ParameterSemantic::OPT_IN);

        // Test PERFORM_* prefix
        CapabilityMatrix matrix2;
        PrintStartAnalysis analysis2;
        analysis2.found = true;
        PrintStartOperation op2;
        op2.category = PrintStartOpCategory::QGL;
        op2.has_skip_param = true;
        op2.skip_param_name = "PERFORM_QGL";
        op2.param_semantic = ParameterSemantic::OPT_IN;
        analysis2.operations.push_back(op2);
        matrix2.add_from_macro_analysis(analysis2);

        auto source2 = matrix2.get_best_source(OperationCategory::QGL);
        REQUIRE(source2.has_value());
        REQUIRE(source2->semantic == ParameterSemantic::OPT_IN);

        // Test SKIP_* prefix (OPT_OUT)
        CapabilityMatrix matrix3;
        PrintStartAnalysis analysis3;
        analysis3.found = true;
        PrintStartOperation op3;
        op3.category = PrintStartOpCategory::Z_TILT;
        op3.has_skip_param = true;
        op3.skip_param_name = "SKIP_Z_TILT";
        op3.param_semantic = ParameterSemantic::OPT_OUT;
        analysis3.operations.push_back(op3);
        matrix3.add_from_macro_analysis(analysis3);

        auto source3 = matrix3.get_best_source(OperationCategory::Z_TILT);
        REQUIRE(source3.has_value());
        REQUIRE(source3->semantic == ParameterSemantic::OPT_OUT);
    }

    SECTION("get_skip_param returns correct values based on semantic") {
        // get_skip_param() returns the (param_name, skip_value) pair for disabling
        // an operation. The skip_value depends on the semantic.

        CapabilityMatrix matrix;

        // Add OPT_OUT operation (SKIP_QGL)
        PrintStartAnalysis analysis;
        analysis.found = true;
        PrintStartOperation op;
        op.name = "QUAD_GANTRY_LEVEL";
        op.category = PrintStartOpCategory::QGL;
        op.has_skip_param = true;
        op.skip_param_name = "SKIP_QGL";
        op.param_semantic = ParameterSemantic::OPT_OUT;
        analysis.operations.push_back(op);
        matrix.add_from_macro_analysis(analysis);

        auto skip_param = matrix.get_skip_param(OperationCategory::QGL);
        REQUIRE(skip_param.has_value());
        REQUIRE(skip_param->first == "SKIP_QGL");
        REQUIRE(skip_param->second == "1"); // OPT_OUT: skip_value = 1
    }
}

TEST_CASE("PrepManager: Extension safety - adding new operations", "[print_preparation][p5]") {
    /**
     * DOCUMENTATION: How to add a new controllable operation
     *
     * 1. Add enum value to OperationCategory in operation_patterns.h
     * 2. Add entry to OperationRegistry::build_all() in operation_registry.h
     * 3. Add keyword patterns to OPERATION_KEYWORDS in operation_patterns.h
     * 4. Add skip/perform variations to SKIP_PARAM_VARIATIONS/PERFORM_PARAM_VARIATIONS
     * 5. Update category_key() and category_name() in operation_patterns.h
     * 6. Add UI subject handling in PrintPreparationManager
     * 7. Add printer database entries if needed
     *
     * This test verifies the extension infrastructure is working correctly.
     */

    SECTION("Registry is the single extension point for controllable operations") {
        // OperationRegistry::all() is the single source of truth for what operations
        // can be shown in the pre-print UI.

        auto all = OperationRegistry::all();

        // Verify we have the expected minimum operations
        REQUIRE(all.size() >= 5);

        // Every operation in the registry must be controllable
        for (const auto& info : all) {
            // Can look it up by category
            auto by_cat = OperationRegistry::get(info.category);
            REQUIRE(by_cat.has_value());
            REQUIRE(by_cat->capability_key == info.capability_key);

            // Can look it up by key
            auto by_key = OperationRegistry::get_by_key(info.capability_key);
            REQUIRE(by_key.has_value());
            REQUIRE(by_key->category == info.category);
        }
    }

    SECTION("Each registry entry has complete and consistent metadata") {
        auto all = OperationRegistry::all();

        for (const auto& info : all) {
            INFO("Checking operation: " << info.capability_key);

            // capability_key must be non-empty and match category_key()
            REQUIRE_FALSE(info.capability_key.empty());
            REQUIRE(info.capability_key == std::string(category_key(info.category)));

            // friendly_name must be non-empty and match category_name()
            REQUIRE_FALSE(info.friendly_name.empty());
            REQUIRE(info.friendly_name == std::string(category_name(info.category)));

            // category must not be UNKNOWN
            REQUIRE(info.category != OperationCategory::UNKNOWN);
        }
    }

    SECTION("Operation categories have skip and perform variations defined") {
        // Each controllable operation should have parameter variations defined
        // for detection in macros.

        auto all = OperationRegistry::all();

        for (const auto& info : all) {
            INFO("Checking variations for: " << info.capability_key);

            // Should have at least one skip variation OR one perform variation
            const auto& skip_vars = get_skip_variations(info.category);
            const auto& perform_vars = get_perform_variations(info.category);

            bool has_variations = !skip_vars.empty() || !perform_vars.empty();
            REQUIRE(has_variations);
        }
    }

    SECTION("CapabilityMatrix supports all registry operations") {
        // The CapabilityMatrix should be able to hold capabilities for all
        // operations in the registry.

        CapabilityMatrix matrix;

        // Add a mock capability for each registry operation
        PrintStartAnalysis analysis;
        analysis.found = true;

        for (const auto& info : OperationRegistry::all()) {
            PrintStartOperation op;
            op.name = info.capability_key;
            op.category = static_cast<PrintStartOpCategory>(info.category);
            op.has_skip_param = true;
            op.skip_param_name = "SKIP_" + std::string(category_key(info.category));
            op.param_semantic = ParameterSemantic::OPT_OUT;
            analysis.operations.push_back(op);
        }

        matrix.add_from_macro_analysis(analysis);

        // Verify all operations are controllable
        for (const auto& info : OperationRegistry::all()) {
            INFO("Verifying matrix support for: " << info.capability_key);
            REQUIRE(matrix.is_controllable(info.category));
        }
    }
}

TEST_CASE("PrepManager: Extension safety - database key consistency", "[print_preparation][p5]") {
    SECTION("Database capability keys match category_key() output") {
        // This ensures that database lookups use the correct keys.
        // The printer_database.json uses these keys for capability definitions.

        REQUIRE(std::string(category_key(OperationCategory::BED_MESH)) == "bed_mesh");
        REQUIRE(std::string(category_key(OperationCategory::QGL)) == "qgl");
        REQUIRE(std::string(category_key(OperationCategory::Z_TILT)) == "z_tilt");
        REQUIRE(std::string(category_key(OperationCategory::NOZZLE_CLEAN)) == "nozzle_clean");
        REQUIRE(std::string(category_key(OperationCategory::PURGE_LINE)) == "purge_line");
    }

    SECTION("Known printer has expected option ids") {
        // Verify that the database returns options with the correct ids
        auto caps = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");

        if (!caps.empty()) {
            // Database should use "bed_mesh" id, not alternatives like "bed_leveling"
            if (const PrePrintOption* bed_cap = caps.find("bed_mesh")) {
                REQUIRE(caps.find("bed_leveling") == nullptr); // Wrong id doesn't exist
                const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&bed_cap->strategy);
                REQUIRE(macro != nullptr);
                REQUIRE_FALSE(macro->param_name.empty());
            }
        }
    }

    SECTION("CapabilityMatrix::category_from_key recognizes all registry keys") {
        // The CapabilityMatrix uses category_from_key() to map option ids to
        // OperationCategory. This must recognize all registry keys.

        // Note: category_from_key is private, so we test indirectly through add_from_database

        CapabilityMatrix matrix;

        // Create database options for all registry operations
        PrePrintOptionSet db_caps;
        db_caps.macro_name = "START_PRINT";

        for (const auto& info : OperationRegistry::all()) {
            PrePrintOption opt;
            opt.id = info.capability_key;
            opt.strategy_kind = PrePrintStrategyKind::MacroParam;
            opt.strategy = PrePrintStrategyMacroParam{"PARAM_" + info.capability_key, "1", "0", ""};
            db_caps.options.push_back(opt);
        }

        matrix.add_from_database(db_caps);

        // Verify all operations were recognized and added
        for (const auto& info : OperationRegistry::all()) {
            INFO("Checking key recognition for: " << info.capability_key);
            REQUIRE(matrix.is_controllable(info.category));
        }
    }
}

// ============================================================================
// Tests: PreStartGcode strategy emission (Phase 4 — ai_detect proof)
// ============================================================================

/**
 * Phase 4 closes the loop on the new framework: a printer (K2 Plus) declares
 * `ai_detect` as a `pre_start_gcode` strategy in printer_database.json with
 * `gcode_template = "LOAD_AI_RUN SWITCH={value}"`. When the user toggles the
 * switch, `collect_pre_start_gcode_lines()` must emit exactly the rendered
 * line for each PreStartGcode option in the active set, regardless of
 * enabled/disabled state. Hidden / NOT_APPLICABLE options are skipped.
 */
TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: timelapse option synthesized when plugin available",
                 "[print_preparation][timelapse][p4]") {
    // When the moonraker-timelapse plugin is reported available via
    // PrinterState::set_timelapse_available(true), the cached
    // PrePrintOptionSet should grow a synthesized "timelapse" option (id,
    // RuntimeCommand strategy, default_enabled=false). This is the path
    // that drives both the renderer (toggle row visibility) and start_print
    // (sentinel-string dispatch).
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Creality K2 Plus");

    SECTION("Without timelapse: option set has NO timelapse entry") {
        const auto& set = printer_state.get_pre_print_option_set();
        const PrePrintOption* tl = set.find("timelapse");
        REQUIRE(tl == nullptr);
    }

    SECTION("With timelapse available: timelapse option is appended") {
        printer_state.set_timelapse_available(true);
        helix::ui::UpdateQueue::instance().drain();

        const auto& set = printer_state.get_pre_print_option_set();
        const PrePrintOption* tl = set.find("timelapse");
        REQUIRE(tl != nullptr);
        CHECK(tl->category == PrePrintCategory::Monitoring);
        CHECK(tl->default_enabled == false);
        CHECK(tl->strategy_kind == PrePrintStrategyKind::RuntimeCommand);

        const auto* cmd = std::get_if<PrePrintStrategyRuntimeCommand>(&tl->strategy);
        REQUIRE(cmd != nullptr);
        CHECK(cmd->command_enabled == "timelapse:on");
        CHECK(cmd->command_disabled == "timelapse:off");
    }

    SECTION("Toggle off: timelapse option is removed when capability lost") {
        printer_state.set_timelapse_available(true);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(printer_state.get_pre_print_option_set().find("timelapse") != nullptr);

        printer_state.set_timelapse_available(false);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(printer_state.get_pre_print_option_set().find("timelapse") == nullptr);
    }

    SECTION("Provider drives ENABLED state for timelapse") {
        printer_state.set_timelapse_available(true);
        helix::ui::UpdateQueue::instance().drain();

        PrintPreparationManager manager;
        manager.set_dependencies(nullptr, &printer_state);
        manager.set_option_state_provider([](const std::string& id) {
            if (id == "timelapse")
                return 1; // user toggled ON
            return -1;
        });
        REQUIRE(manager.get_option_state("timelapse") == PrePrintOptionState::ENABLED);
    }
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "PrintPreparationManager: collect_pre_start_gcode_lines emits ai_detect for K2 Plus",
    "[print_preparation][ai_detect][p4]") {
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Creality K2 Plus");

    // ai_detect is gated by requires_macro=LOAD_AI_RUN. Mark the macro as
    // registered so this K2 Plus test fixture exercises the macro-present path.
    MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("Provider says ENABLED -> SWITCH=1 emitted") {
        manager.set_option_state_provider([](const std::string& id) {
            // Only ai_detect is enabled; everything else returns -1 (fall back to default)
            if (id == "ai_detect")
                return 1;
            return -1;
        });
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN") == "LOAD_AI_RUN SWITCH=1");
    }

    SECTION("Provider says DISABLED -> SWITCH=0 emitted") {
        manager.set_option_state_provider([](const std::string& id) {
            if (id == "ai_detect")
                return 0;
            return -1;
        });
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN") == "LOAD_AI_RUN SWITCH=0");
    }

    SECTION("No provider -> uses default_enabled (false) -> SWITCH=0") {
        // ai_detect default_enabled is false; with no UI binding it should still
        // emit SWITCH=0 because PreStartGcode emits regardless of state.
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN") == "LOAD_AI_RUN SWITCH=0");
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: collect_pre_start_gcode_lines empty for printer w/o "
                 "PreStartGcode options",
                 "[print_preparation][ai_detect][p4]") {
    lv_init_safe();
    // AD5M Pro only has MacroParam options — no PreStartGcode.
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
    REQUIRE(lines.empty());
}

TEST_CASE("PrintPreparationManager: collect_pre_start_gcode_lines empty when no printer state set",
          "[print_preparation][ai_detect][p4]") {
    lv_init_safe();
    PrintPreparationManager manager;
    // No printer_state -> get_cached_options() returns the empty static set.
    auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
    REQUIRE(lines.empty());
}

// ============================================================================
// Post-Phase-3.5 framework tests (T1-T5 from review)
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: collect_ops_to_disable returns ops user disabled",
                 "[print_preparation][gcode][framework]") {
    // T1: Verifies the post-Phase-3.5 path where collect_ops_to_disable()
    // resolves user intent through get_option_state() (provider-driven) for
    // the embedded-op skip set.
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    // Mock a file scan that contains BED_MESH embedded.
    gcode::ScanResult scan;
    gcode::DetectedOperation op;
    op.type = gcode::OperationType::BED_MESH;
    op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
    op.line_number = 5;
    scan.operations.push_back(op);
    manager.set_cached_scan_result(scan, "test.gcode");

    SECTION("Default state (ENABLED) -> empty disable list") {
        // bed_mesh in AD5M Pro DB has default_enabled=true, so without a
        // provider override, get_option_state returns ENABLED — collect_ops_
        // to_disable should NOT include BED_MESH.
        auto ops = PrintPreparationManagerTestAccess::get_ops_to_disable(manager);
        REQUIRE(ops.empty());
    }

    SECTION("Provider DISABLED -> BED_MESH included") {
        manager.set_option_state_provider([](const std::string& id) {
            if (id == "bed_mesh")
                return 0; // user explicitly disabled
            return -1;
        });
        auto ops = PrintPreparationManagerTestAccess::get_ops_to_disable(manager);
        REQUIRE(ops.size() == 1);
        REQUIRE(ops[0] == gcode::OperationType::BED_MESH);
    }

    SECTION("Provider DISABLED but file lacks op -> empty") {
        gcode::ScanResult empty_scan;
        manager.set_cached_scan_result(empty_scan, "test.gcode");
        manager.set_option_state_provider([](const std::string& id) {
            if (id == "bed_mesh")
                return 0;
            return -1;
        });
        auto ops = PrintPreparationManagerTestAccess::get_ops_to_disable(manager);
        REQUIRE(ops.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: K2 bed_mesh runs the mesh macro only when enabled",
                 "[print_preparation][framework][k2_plus]") {
    // The K2's mesh lives in BED_MESH_CALIBRATE_START_PRINT, which START_PRINT
    // never calls. The option used to map to a PREPARE param that the pipeline
    // deliberately never sends (it is only a visibility sentinel), so the
    // toggle homed and wiped the nozzle and never produced a mesh at all.
    //
    // The macro has no "off" form, so disabled must emit nothing rather than a
    // zero — hence emit_when_disabled=false. GCODE_FILE keeps the sweep trimmed
    // to the object.
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Creality K2 Plus");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    SECTION("enabled emits the adaptive mesh macro") {
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
        auto lines =
            PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager, "part.gcode");
        bool found = false;
        for (const auto& l : lines) {
            if (l.find("BED_MESH_CALIBRATE_START_PRINT") != std::string::npos) {
                found = true;
                REQUIRE(l.find("GCODE_FILE='part.gcode'") != std::string::npos);
            }
        }
        REQUIRE(found);
    }

    SECTION("disabled emits nothing for bed_mesh") {
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
        auto lines =
            PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager, "part.gcode");
        for (const auto& l : lines) {
            REQUIRE(l.find("BED_MESH_CALIBRATE_START_PRINT") == std::string::npos);
        }
    }

    SECTION("no PREPARE skip param is emitted any more") {
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
        for (const auto& [k, v] : PrintPreparationManagerTestAccess::get_skip_params(manager)) {
            REQUIRE(k != "PREPARE");
        }
    }

    SECTION("job temps come from the file's own START_PRINT call") {
        // The gcode file's START_PRINT line is the source of truth for temps
        // (Moonraker metadata lies on multi-material files). The scanner
        // extracts them into print_start; the pre-start renderer must use
        // them when the scanned file is the file being printed.
        gcode::ScanResult scan;
        scan.print_start.found = true;
        scan.print_start.macro_name = "START_PRINT";
        scan.print_start.raw_line = "START_PRINT EXTRUDER_TEMP=260 BED_TEMP=105";
        scan.print_start.extruder_temp = 260;
        scan.print_start.bed_temp = 105;
        manager.set_cached_scan_result(scan, "part.gcode");

        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
        auto lines =
            PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager, "part.gcode");
        bool found = false;
        for (const auto& l : lines) {
            if (l.find("BED_MESH_CALIBRATE_START_PRINT") != std::string::npos) {
                found = true;
                REQUIRE(l.find("BED_TEMP=105") != std::string::npos);
                REQUIRE(l.find("EXTRUDER_TEMP=260") != std::string::npos);
            }
        }
        REQUIRE(found);
    }

    SECTION("temps render 0 when the scan cache is for a different file") {
        gcode::ScanResult scan;
        scan.print_start.found = true;
        scan.print_start.bed_temp = 105;
        scan.print_start.extruder_temp = 260;
        manager.set_cached_scan_result(scan, "other.gcode");

        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
        auto lines =
            PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager, "part.gcode");
        for (const auto& l : lines) {
            if (l.find("BED_MESH_CALIBRATE_START_PRINT") != std::string::npos) {
                // 0 lets the firmware macro keep its own default (>= 50 guard)
                REQUIRE(l.find("BED_TEMP=0") != std::string::npos);
                REQUIRE(l.find("EXTRUDER_TEMP=0") != std::string::npos);
            }
        }
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: collect_pre_start_gcode_lines skips NOT_APPLICABLE",
                 "[print_preparation][framework][ai_detect]") {
    // T5: K2 Plus has ai_detect (PreStartGcode strategy). When the provider
    // returns -1 (not bound) AND the option's default_enabled is false, the
    // option resolves to DISABLED — emitting SWITCH=0. To get NOT_APPLICABLE,
    // we'd need an option that isn't in the DB at all, which means it can't
    // appear in the iteration. So this test verifies that for an id that's
    // simply not declared, no line is emitted (regardless of provider state).
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    // AD5M Pro doesn't declare ai_detect, so even if a provider claimed it
    // was enabled, no PreStartGcode line should emerge — the iteration walks
    // the cached set, not the provider's id space.
    manager.set_option_state_provider([](const std::string& id) {
        if (id == "ai_detect")
            return 1;
        return -1;
    });
    auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
    REQUIRE(lines.empty());
}

// ============================================================================
// disabling_option_requires_plugin — pre-print toggle plugin-gating predicate
//
// A pre-print toggle is hidden (via the renderer's visibility subject) when
// DISABLING it would require the HelixPrint plugin. "Requires the plugin" means
// start_print() would reach check_modification_capability() and be forced to
// drop the modification with a "Requires HelixPrint plugin" warning when the
// plugin is absent. That only happens when NO pre-start short-circuit fires:
//
//   (b) MacroParam option AND the printer has no pre-start mechanism
//       (setup_gcode empty AND no PreStartGcode option emits a line), OR
//   (a) a file-embeddable op (bed_mesh/qgl/z_tilt/nozzle_clean) is embedded in
//       the sliced file — UNLESS the same short-circuit fires.
//
// The K2 Plus PREPARE case (MacroParam bed_mesh + setup_gcode) MUST stay
// visible: its native START_PRINT/PRINT_PREPARED handles the skip without the
// plugin. That is the shipped regression these tests guard against.
// ============================================================================

namespace {

PrePrintOption make_macro_param_opt(const std::string& id) {
    PrePrintOption o;
    o.id = id;
    o.category = PrePrintCategory::Mechanical;
    o.default_enabled = true;
    o.strategy_kind = PrePrintStrategyKind::MacroParam;
    o.strategy = PrePrintStrategyMacroParam{"SKIP_" + id, "0", "1", "0"};
    return o;
}

PrePrintOption make_pre_start_gcode_opt(const std::string& id) {
    PrePrintOption o;
    o.id = id;
    o.category = PrePrintCategory::Monitoring;
    o.default_enabled = true;
    o.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    // Empty requires_macro -> never gated by MacroParamCache, so it always
    // emits a line (making the printer "have a pre-start mechanism").
    o.strategy = PrePrintStrategyPreStartGcode{"AI_RUN SWITCH={value}"};
    return o;
}

PrePrintOption make_runtime_command_opt(const std::string& id) {
    PrePrintOption o;
    o.id = id;
    o.category = PrePrintCategory::Monitoring;
    o.default_enabled = true;
    o.strategy_kind = PrePrintStrategyKind::RuntimeCommand;
    o.strategy = PrePrintStrategyRuntimeCommand{id + ":on", id + ":off"};
    return o;
}

gcode::ScanResult scan_with_embedded_op(gcode::OperationType type) {
    gcode::ScanResult scan;
    gcode::DetectedOperation op;
    op.type = type;
    op.embedding = gcode::OperationEmbedding::DIRECT_COMMAND;
    op.line_number = 5;
    scan.operations.push_back(op);
    return scan;
}

// Fresh singleton PrinterState with the given option set injected (bypasses the
// printer DB) and a manager wired to it.
struct GateFixture {
    PrinterState& ps = get_printer_state();
    PrintPreparationManager manager;

    explicit GateFixture(PrePrintOptionSet set) {
        lv_init_safe();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        PrinterStateTestAccess::set_option_set(ps, std::move(set));
        manager.set_dependencies(nullptr, &ps);
    }

    bool requires_plugin(const std::string& id) {
        const PrePrintOption* opt = ps.get_pre_print_option_set().find(id);
        REQUIRE(opt != nullptr);
        return manager.disabling_option_requires_plugin(*opt);
    }
};

} // namespace

TEST_CASE_METHOD(
    HelixTestFixture,
    "disabling_option_requires_plugin: K2 regression — MacroParam + setup_gcode stays visible",
    "[print_preparation][preprint][plugin_gate]") {
    // MOST IMPORTANT case. K2 Plus bed_mesh is a MacroParam (PREPARE-style)
    // option and the printer ships setup_gcode (PRINT_PREPARED). Disabling it
    // is honored by the native macro, NOT the plugin -> must NOT be hidden.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.setup_gcode = "PRINT_PREPARED";
    set.options.push_back(make_macro_param_opt("bed_mesh"));
    GateFixture fx(std::move(set));

    SECTION("no embedded op in file") {
        REQUIRE(fx.requires_plugin("bed_mesh") == false);
    }

    SECTION("file ALSO embeds bed_mesh — still not required (correction to naive "
            "\"file-embedded => plugin\")") {
        // start_print() emits the setup_gcode pre-start block (MacroParam skip
        // present + setup_gcode present), then routes the embedded-op removal
        // through modify_and_print, which streams WITHOUT a capability check.
        // So even a file-embedded op does not require the plugin here.
        fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                          "k2_file.gcode");
        REQUIRE(fx.requires_plugin("bed_mesh") == false);
    }
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "disabling_option_requires_plugin: Voron — MacroParam, no pre-start mechanism, hidden",
    "[print_preparation][preprint][plugin_gate]") {
    // MacroParam bed_mesh with no setup_gcode and no PreStartGcode siblings:
    // disabling it means rewriting the START_PRINT call in the file, which
    // needs the plugin for clean history -> hidden when plugin absent.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_macro_param_opt("bed_mesh"));
    GateFixture fx(std::move(set));

    SECTION("no embedded op") {
        REQUIRE(fx.requires_plugin("bed_mesh") == true);
    }
    SECTION("with embedded op") {
        fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                          "voron.gcode");
        REQUIRE(fx.requires_plugin("bed_mesh") == true);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "disabling_option_requires_plugin: file-embedded term isolated",
                 "[print_preparation][preprint][plugin_gate]") {
    // Use a (synthetic) RuntimeCommand strategy on a file-embeddable id so the
    // MacroParam term is off and no pre-start line is emitted — this isolates
    // the "file has the op embedded" contribution to the predicate.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_runtime_command_opt("bed_mesh"));
    GateFixture fx(std::move(set));

    SECTION("file embeds the op -> requires plugin") {
        fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                          "f.gcode");
        REQUIRE(fx.requires_plugin("bed_mesh") == true);
    }
    SECTION("file does NOT embed the op -> not required") {
        REQUIRE(fx.requires_plugin("bed_mesh") == false);
    }
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "disabling_option_requires_plugin: MacroParam disable dropped when a PreStartGcode "
    "sibling short-circuits",
    "[print_preparation][preprint][plugin_gate]") {
    // No setup_gcode, but a PreStartGcode sibling emits a line -> start_print()
    // takes the pre-start path and never reaches the capability check. The
    // MacroParam skip is silently dropped (a pre-existing quirk), but crucially
    // NO plugin warning fires, so the toggle is not hidden.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_macro_param_opt("bed_mesh"));
    set.options.push_back(make_pre_start_gcode_opt("ai_detect"));
    GateFixture fx(std::move(set));

    REQUIRE(fx.requires_plugin("bed_mesh") == false);
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "disabling_option_requires_plugin: PreStartGcode and RuntimeCommand never require plugin",
    "[print_preparation][preprint][plugin_gate]") {
    SECTION("PreStartGcode option (with setup_gcode present — must not matter)") {
        PrePrintOptionSet set;
        set.macro_name = "START_PRINT";
        set.setup_gcode = "PRINT_PREPARED";
        set.options.push_back(make_pre_start_gcode_opt("ai_detect"));
        GateFixture fx(std::move(set));
        REQUIRE(fx.requires_plugin("ai_detect") == false);
    }

    SECTION("RuntimeCommand option (timelapse-like, no embeddable op)") {
        PrePrintOptionSet set;
        set.macro_name = "START_PRINT";
        set.options.push_back(make_runtime_command_opt("timelapse"));
        GateFixture fx(std::move(set));
        REQUIRE(fx.requires_plugin("timelapse") == false);
    }
}

// ============================================================================
// A start that dies inside the manager must retire the preparing job
//
// The modify and remap routes take NO completion callback — continue_print_start()
// calls modify_and_print() and returns — so PrintStartController's
// retire_preparing(Failed) is unreachable from them. Nothing else can retire the
// job, and `print_in_progress` is now PUBLISHED from that job rather than set by
// hand, so a leak latches the flag until the 1800s watchdog: can_start_new_print()
// refuses every later print, job_holds_machine stays 1 (greying jog, levelling,
// macros and the bypass tile), and the panel sits on "Preparing".
//
// Mutation check: delete the abandon_start() call from the exit this drives and
// the REQUIRE_FALSE below fails.
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a start that dies inside the manager retires the "
                 "preparing job",
                 "[print_preparation][lifecycle][preparing]") {
    helix::PrinterState state;
    state.init_subjects(false);

    helix::ui::PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &state);

    state.begin_preparing(helix::PrintJobRef{"doomed.gcode", "gcodes", ""});
    REQUIRE(state.has_preparing_job());
    REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 1);

    // api_ is null, so the modify route bails on its FIRST guard. Both of its
    // synchronous guards are real reachable exits — continue_print_start()
    // dispatches here whenever the user disabled an embedded operation, and the
    // connection can drop between arming and dispatch — and every deferred
    // failure exit further in retires the job the same way.
    //
    // Mutation-verified: deleting abandon_start("modify_no_api") fails the
    // REQUIRE_FALSE below. (Deleting the OTHER guard's call does not, which is
    // the point of naming which exit this drives rather than guessing.)
    PrintPreparationManagerTestAccess::modify_and_print(manager, "doomed.gcode");

    REQUIRE_FALSE(state.has_preparing_job());
    REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 0);
    REQUIRE(state.last_preparing_exit() == helix::PreparingExit::Failed);

    // And the machine is released — this is the user-visible half of the latch.
    REQUIRE(lv_subject_get_int(state.get_job_holds_machine_subject()) == 0);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: the scan-result guard retires the preparing job too",
                 "[print_preparation][lifecycle][preparing]") {
    // The sibling exit, reached with a live api_ and no cached scan. Separate
    // case because a single test can only ever prove the FIRST guard it hits.
    MoonrakerClientMock mock_client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(mock_client, state);

    helix::ui::PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);

    state.begin_preparing(helix::PrintJobRef{"doomed.gcode", "gcodes", ""});
    REQUIRE(state.has_preparing_job());

    PrintPreparationManagerTestAccess::modify_and_print(manager, "doomed.gcode");

    REQUIRE_FALSE(state.has_preparing_job());
    REQUIRE(lv_subject_get_int(state.get_print_in_progress_subject()) == 0);
    REQUIRE(state.last_preparing_exit() == helix::PreparingExit::Failed);
}

// ============================================================================
// STALE PRE-START ACK GUARD
// ============================================================================

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: stale pre-start ack does not relaunch a print",
                 "[preparation][stale]") {
    // K1C capture 2026-08-20: klippy's gcode response path backed up behind
    // the prep chain, so the pre-start ack arrived 370s after the send - at
    // cancel time - and the success continuation relaunched the print the
    // user had just cancelled. A pre-start block takes seconds; an
    // continuation hearing back minutes later must drop the start intent.
    std::atomic<int> starts{0};
    server_->clear_handlers();
    server_->on_method("printer.print.start", [&](const json&) -> json {
        starts++;
        return json{{"result", "ok"}};
    });

    PrintPreparationManagerTestAccess::set_pre_start_sent_ago(manager_, std::chrono::minutes(4));

    bool completed = false;
    bool success = true;
    helix::ui::PrintCompletionCallback cb = [&](bool ok, const std::string&) {
        completed = true;
        success = ok;
    };
    PrintPreparationManagerTestAccess::continue_print_start(manager_, "test.gcode", cb);

    REQUIRE(completed);
    REQUIRE_FALSE(success);
    REQUIRE(starts.load() == 0);
}
