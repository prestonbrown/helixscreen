// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_busy_overlay.h"
#include "ui_filename_utils.h"
#include "ui_pre_print_options_renderer.h"
#include "ui_print_preparation_manager.h"

#include "../fake_moonraker_client.h"
#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../mocks/mock_websocket_server.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/preprint_config_scope.h"
#include "../test_helpers/print_preparation_manager_test_access.h"
#include "../test_helpers/print_start_collector_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "capability_matrix.h"
#include "gcode_edit_list.h"
#include "gcode_file_modifier.h"
#include "gcode_ops_detector.h"
#include "hv/EventLoopThread.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_auth.h"
#include "moonraker_client.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "moonraker_manager.h"
#include "operation_registry.h"
#include "pre_start_exclude.h"
#include "preprint_predictor.h"
#include "print_start_analyzer.h"
#include "print_start_checks.h"
#include "print_start_collector.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "storage_report.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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

TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: can_modify_gcode",
                 "[print_preparation][safety]") {
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);

    PrintPreparationManager manager;

    SECTION("no printer state at all declines rather than dereferences") {
        CHECK_FALSE(manager.can_modify_gcode());
    }

    SECTION("the plugin is the whole answer") {
        manager.set_dependencies(nullptr, &printer_state);

        // Unknown must not read as permission: the probe answers after first
        // paint, and a rewrite sent before it lands cannot be un-filed from
        // history afterwards.
        CHECK_FALSE(manager.can_modify_gcode());

        printer_state.set_helix_plugin_installed(false);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(manager.can_modify_gcode());

        printer_state.set_helix_plugin_installed(true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(manager.can_modify_gcode());
    }

    SECTION("a transport that keeps no local copy declines even with the plugin") {
        // The rewrite streams through a downloaded copy; asking for one on such a
        // transport fails the start instead of printing the original.
        MockPrinter mock_printer;
        manager.set_dependencies(&mock_printer.api, &printer_state);
        printer_state.set_helix_plugin_installed(true);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(manager.can_modify_gcode());

        mock_printer.api.transfers_mock().mock_no_local_copies();
        manager.set_dependencies(&mock_printer.api, &printer_state); // asks the transport
        CHECK_FALSE(manager.can_modify_gcode());
        manager.set_dependencies(nullptr, nullptr);
    }

    SECTION("file size does not enter into it") {
        // The rewrite streams a line at a time, so the answer cannot depend on
        // how big the file is. A size rule here is what the retired
        // ModificationCapability struct still claimed to enforce.
        manager.set_dependencies(nullptr, &printer_state);
        printer_state.set_helix_plugin_installed(true);
        helix::ui::UpdateQueue::instance().drain();

        manager.set_cached_file_size(10ULL * 1024 * 1024);
        CHECK(manager.can_modify_gcode());
        manager.set_cached_file_size(1000ULL * 1024 * 1024 * 1024);
        CHECK(manager.can_modify_gcode());
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: dropped modifications name the device, not the plugin, "
                 "when the transport keeps no copy",
                 "[print_preparation][safety]") {
    lv_init_safe();
    MockPrinter mock_printer;
    PrintPreparationManager manager;
    manager.set_dependencies(&mock_printer.api, &mock_printer.state);
    mock_printer.state.set_helix_plugin_installed(true);
    helix::ui::UpdateQueue::instance().drain();

    std::vector<std::string> shown;
    set_test_notification_warning_hook([&shown](const std::string& m) { shown.push_back(m); });
    struct HookReset {
        ~HookReset() {
            set_test_notification_warning_hook(nullptr);
        }
    } reset;
    auto warned = [&manager, &shown]() {
        shown.clear();
        manager.warn_modifications_dropped({});
        return shown.size() == 1 ? shown[0] : std::string();
    };

    SECTION("a transport without local copies") {
        mock_printer.api.transfers_mock().mock_no_local_copies();
        manager.set_dependencies(&mock_printer.api, &mock_printer.state);
        REQUIRE(manager.gcode_rewrite_block() == helix::GcodeRewriteBlock::NoLocalCopies);
        CHECK(warned() ==
              "Modifying G-code is not available on this device. Printing original file.");
    }
    SECTION("a missing plugin, for contrast") {
        mock_printer.state.set_helix_plugin_installed(false);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(warned() == "Modifying G-code needs the HelixPrint plugin. Printing original file.");
    }
    manager.set_dependencies(nullptr, nullptr);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: rewrite-gated macro rows do not show the options card "
                 "on a transport without local copies",
                 "[print_preparation][safety]") {
    lv_init_safe();
    MockPrinter mock_printer;
    PrinterState& state = mock_printer.state;
    PrintPreparationManager manager;
    manager.set_dependencies(&mock_printer.api, &state);
    state.set_helix_plugin_installed(true);
    helix::ui::UpdateQueue::instance().drain();

    // A generic profile whose PRINT_START offers one skip, a MacroParam with no
    // pre-start block: disabling it rewrites the job.
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

    auto card = [&state]() {
        // The plugin lands through the queue and its observer queues the count.
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        return lv_subject_get_int(
            state.composite_visibility_state().get_has_any_preprint_options_subject());
    };

    manager.set_macro_analysis(analysis);
    REQUIRE(card() == 1);

    // The row hides on this transport, so the card has nothing to show.
    mock_printer.api.transfers_mock().mock_no_local_copies();
    manager.set_dependencies(&mock_printer.api, &state);
    CHECK(card() == 0);

    // The plugin changing republishes the count too.
    mock_printer.api.transfers_mock().mock_no_local_copies(false);
    manager.set_dependencies(&mock_printer.api, &state);
    state.set_helix_plugin_installed(false);
    CHECK(card() == 0);
    state.set_helix_plugin_installed(true);
    CHECK(card() == 1);
    manager.set_dependencies(nullptr, nullptr);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a queued plugin change after the API is freed reads "
                 "nothing of it",
                 "[print_preparation][preparation][lifecycle]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto device = std::make_unique<MockPrinter>();
    PrintPreparationManager manager;
    manager.set_dependencies(&device->api, &state);
    helix::ui::UpdateQueue::instance().drain();

    // The plugin answer lands through the queue, and its observer queues the
    // option-count publish behind it. The owner frees the API in between.
    state.set_helix_plugin_installed(true);
    helix::ui::UpdateQueue::instance().drain();
    device.reset();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(manager.gcode_rewrite_block() == helix::GcodeRewriteBlock::None);

    // Clearing the dependencies drops the observer, so a later change queues
    // nothing for this manager.
    manager.set_dependencies(nullptr, nullptr);
    state.set_helix_plugin_installed(false);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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

namespace {

/// The six option ids PrePrintOptions carries, with the field each one fills.
struct OptionField {
    const char* id;
    bool PrePrintOptions::*field;
};
const OptionField kOptionFields[] = {
    {"bed_mesh", &PrePrintOptions::bed_mesh},     {"qgl", &PrePrintOptions::qgl},
    {"z_tilt", &PrePrintOptions::z_tilt},         {"nozzle_clean", &PrePrintOptions::nozzle_clean},
    {"purge_line", &PrePrintOptions::purge_line}, {"timelapse", &PrePrintOptions::timelapse},
};

} // namespace

TEST_CASE("PrintPreparationManager: the option-state provider decides each option's state",
          "[print_preparation][options]") {
    // No printer DB is attached, so nothing falls back to a database default.
    lv_init_safe();
    PrintPreparationManager manager;
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("every id follows its provider value: 1 enabled, 0 disabled, -1 hidden") {
        const std::pair<int, PrePrintOptionState> values[] = {
            {1, PrePrintOptionState::ENABLED},
            {0, PrePrintOptionState::DISABLED},
            {-1, PrePrintOptionState::NOT_APPLICABLE},
        };
        for (const auto& [value, expected] : values) {
            for (const auto& f : kOptionFields) {
                state.values[f.id] = value;
            }
            const auto options = manager.read_options_from_subjects();
            for (const auto& f : kOptionFields) {
                INFO(f.id << " = " << value);
                CHECK(options.*(f.field) == (value == 1));
                CHECK(manager.get_option_state(f.id) == expected);
            }
        }
    }

    SECTION("ids are independent; hidden is never read as disabled") {
        state.enable("bed_mesh");
        state.disable("qgl");
        state.hide("z_tilt");
        state.hide("nozzle_clean");
        state.enable("purge_line");
        state.disable("timelapse");

        const auto options = manager.read_options_from_subjects();
        CHECK(options.bed_mesh);
        CHECK_FALSE(options.qgl);
        CHECK_FALSE(options.z_tilt);
        CHECK_FALSE(options.nozzle_clean);
        CHECK(options.purge_line);
        CHECK_FALSE(options.timelapse);
        CHECK(manager.get_option_state("qgl") == PrePrintOptionState::DISABLED);
        CHECK(manager.get_option_state("z_tilt") == PrePrintOptionState::NOT_APPLICABLE);
    }

    SECTION("an id the provider does not know is not applicable") {
        CHECK(manager.get_option_state("nonexistent_op") == PrePrintOptionState::NOT_APPLICABLE);
    }

    SECTION("each read sees the provider's current value") {
        state.disable("bed_mesh");
        REQUIRE_FALSE(manager.read_options_from_subjects().bed_mesh);
        state.enable("bed_mesh");
        CHECK(manager.read_options_from_subjects().bed_mesh);
        state.hide("bed_mesh");
        CHECK_FALSE(manager.read_options_from_subjects().bed_mesh);
        state.enable("bed_mesh");
        CHECK(manager.read_options_from_subjects().bed_mesh);
    }

    SECTION("without a provider every option reads false") {
        PrintPreparationManager bare_manager;
        const auto options = bare_manager.read_options_from_subjects();
        for (const auto& f : kOptionFields) {
            INFO(f.id);
            CHECK_FALSE(options.*(f.field));
        }
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

        // The AD5M Pro's START_PRINT takes no purge or skew parameter, so the card
        // must offer no toggle that would send one.
        CHECK(caps.find("purge_line") == nullptr);
        CHECK(caps.find("skew_correct") == nullptr);
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
        const auto& state_caps = printer_state.profile_state().pre_print_option_set();
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
        const auto& state_caps = printer_state.profile_state().pre_print_option_set();
        REQUIRE(state_caps.empty());
        REQUIRE(state_caps.macro_name.empty());
    }

    SECTION("Manager sees empty capabilities for unknown printer type") {
        // Set an unknown printer type
        printer_state.set_printer_type_sync("Unknown Printer That Does Not Exist");

        // Should return empty capabilities, not crash
        const auto& state_caps = printer_state.profile_state().pre_print_option_set();
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

namespace {

/// Known heating rates for the estimate tests, cleared again on the way out.
struct EstimateRatesScope {
    EstimateRatesScope() {
        auto& rates = ThermalRateManager::instance();
        rates.reset();
        rates.get_model("extruder").set_default_rate(0.5f);
        rates.get_model("heater_bed").set_default_rate(2.0f);
    }
    ~EstimateRatesScope() {
        ThermalRateManager::instance().reset();
    }
};

FileMetadata file_at(double bed_c, double nozzle_c) {
    FileMetadata m;
    m.filename = "estimate.gcode";
    m.first_layer_bed_temp = bed_c;
    m.first_layer_extr_temp = nozzle_c;
    return m;
}

void set_temps(PrinterState& ps, int bed_c, int nozzle_c) {
    lv_subject_set_int(ps.temperature_state().get_bed_temp_subject(), bed_c * 10);
    lv_subject_set_int(ps.temperature_state().get_active_extruder_temp_subject(), nozzle_c * 10);
}

int sum_of_default_phases() {
    const auto homing = static_cast<int>(helix::PrintStartPhase::HOMING);
    int total = 0;
    for (const auto& [phase, secs] : helix::PreprintPredictor::default_phase_durations()) {
        total +=
            phase == homing ? std::max(secs, helix::PreprintPredictor::kMinHomingSeconds) : secs;
    }
    return total;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: estimate heats to the file's temperatures",
                 "[print_preparation][estimate]") {
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);
    lv_subject_t* const estimate = manager.get_preprint_estimate_subject();
    PreprintConfigScope config;
    EstimateRatesScope rates;
    auto& thermal = ThermalRateManager::instance();
    const FileMetadata file = file_at(60, 220);

    SECTION("no metadata yet: no estimate") {
        set_temps(printer_state, 25, 25);
        lv_subject_set_int(estimate, 123);
        manager.recalculate_estimate(nullptr, file.filename);
        REQUIRE(lv_subject_get_int(estimate) == 0);
    }

    SECTION("cold printer, no history: the full heat-up plus every default phase") {
        set_temps(printer_state, 25, 25);
        manager.recalculate_estimate(&file, file.filename);
        const float heat = thermal.estimate_heating_seconds("extruder", 25.0f, 220.0f) +
                           thermal.estimate_heating_seconds("heater_bed", 25.0f, 60.0f);
        REQUIRE(heat > 100.0f);
        REQUIRE(lv_subject_get_int(estimate) == static_cast<int>(heat + sum_of_default_phases()));
        REQUIRE(lv_subject_get_int(estimate) > 300);
    }

    SECTION("a warm printer heats less") {
        set_temps(printer_state, 25, 25);
        manager.recalculate_estimate(&file, file.filename);
        const int cold = lv_subject_get_int(estimate);
        set_temps(printer_state, 55, 200);
        manager.recalculate_estimate(&file, file.filename);
        const float heat = thermal.estimate_heating_seconds("extruder", 200.0f, 220.0f) +
                           thermal.estimate_heating_seconds("heater_bed", 55.0f, 60.0f);
        REQUIRE(lv_subject_get_int(estimate) == static_cast<int>(heat + sum_of_default_phases()));
        REQUIRE(lv_subject_get_int(estimate) < cold);
    }

    SECTION("a printer with its own default phases uses them") {
        const std::string name = "Elegoo Centauri Carbon";
        PreprintConfigScope typed(name);
        printer_state.set_printer_type_sync(name);
        set_temps(printer_state, 25, 25);
        manager.invalidate_predictor_cache();
        manager.recalculate_estimate(&file, file.filename);
        const float heat = thermal.estimate_heating_seconds("extruder", 25.0f, 220.0f) +
                           thermal.estimate_heating_seconds("heater_bed", 25.0f, 60.0f);
        REQUIRE(sum_of_default_phases() != 260);
        REQUIRE(lv_subject_get_int(estimate) == static_cast<int>(heat + sum_of_default_phases()));
    }

    SECTION("history: the recorded wall clock, heating included") {
        helix::PreprintEntry e;
        e.total_seconds = 400;
        e.timestamp = 1000;
        e.temp_bucket = 1;
        e.window = helix::PreprintWindow::PrinterEdge;
        e.phase_durations = {{static_cast<int>(helix::PrintStartPhase::HOMING), 30},
                             {static_cast<int>(helix::PrintStartPhase::BED_MESH), 120}};
        helix::PreprintPredictor::append_to_config(e);
        set_temps(printer_state, 25, 25);
        manager.invalidate_predictor_cache();
        manager.recalculate_estimate(&file, file.filename);
        REQUIRE(lv_subject_get_int(estimate) == 400);

        SECTION("a toggle turned off subtracts its phase") {
            manager.set_option_state_provider(
                [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
            manager.recalculate_estimate(&file, file.filename);
            REQUIRE(lv_subject_get_int(estimate) == 280);
        }

        SECTION("a toggle left on keeps its phase") {
            manager.set_option_state_provider(
                [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
            manager.recalculate_estimate(&file, file.filename);
            REQUIRE(lv_subject_get_int(estimate) == 400);
        }
    }

    SECTION("history that never named a phase: a toggle left on adds nothing") {
        // AD5M-style: the start meshes, the matcher never mapped it, so the
        // mesh is already inside the recorded 400s.
        helix::PreprintEntry e;
        e.total_seconds = 400;
        e.timestamp = 1000;
        e.temp_bucket = 1;
        e.window = helix::PreprintWindow::PrinterEdge;
        e.phase_durations = {{static_cast<int>(helix::PrintStartPhase::HOMING), 30}};
        helix::PreprintPredictor::append_to_config(e);
        set_temps(printer_state, 25, 25);
        manager.invalidate_predictor_cache();
        manager.set_option_state_provider([](const std::string& id) {
            return id == "bed_mesh" || id == "qgl" || id == "z_tilt" || id == "nozzle_clean" ||
                           id == "purge_line"
                       ? 1
                       : -1;
        });
        manager.recalculate_estimate(&file, file.filename);
        REQUIRE(lv_subject_get_int(estimate) == 400);
    }

    SECTION("no history: a toggle turned off subtracts its default phase") {
        set_temps(printer_state, 25, 25);
        manager.recalculate_estimate(&file, file.filename);
        const int all = lv_subject_get_int(estimate);
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
        manager.recalculate_estimate(&file, file.filename);
        const int mesh = helix::PreprintPredictor::default_phase_durations().at(
            static_cast<int>(helix::PrintStartPhase::BED_MESH));
        REQUIRE(lv_subject_get_int(estimate) == all - mesh);
    }
}

TEST_CASE_METHOD(
    HelixTestFixture,
    "PrintPreparationManager: recalculate before any getter still computes an estimate",
    "[print_preparation][estimate]") {
    // The detail view's first open calls recalculate_estimate() before
    // anything has fetched the estimate subject, so the recalculation must
    // initialize the subject itself rather than read back the initial 0.
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    PreprintConfigScope config;
    EstimateRatesScope rates;
    set_temps(printer_state, 25, 25);
    const FileMetadata file = file_at(60, 220);
    manager.recalculate_estimate(&file, file.filename);

    auto& thermal = ThermalRateManager::instance();
    const float heat = thermal.estimate_heating_seconds("extruder", 25.0f, 220.0f) +
                       thermal.estimate_heating_seconds("heater_bed", 25.0f, 60.0f);
    lv_subject_t* const estimate = manager.get_preprint_estimate_subject();
    REQUIRE(lv_subject_get_int(estimate) == static_cast<int>(heat + sum_of_default_phases()));
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
        const auto& caps_v1 = printer_state.profile_state().pre_print_option_set();
        REQUIRE_FALSE(caps_v1.empty());
        REQUIRE(caps_v1.macro_name == "START_PRINT");
        size_t v1_option_count = caps_v1.options.size();

        // Now switch to AD5M (non-Pro)
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M");

        // Verify options updated
        const auto& caps_v2 = printer_state.profile_state().pre_print_option_set();
        REQUIRE_FALSE(caps_v2.empty());
        // Both have START_PRINT but this confirms the lookup happened
        REQUIRE(caps_v2.macro_name == "START_PRINT");

        INFO("AD5M Pro options: " << v1_option_count);
        INFO("AD5M options: " << caps_v2.options.size());
    }

    SECTION("Capabilities become empty when switching to unknown printer") {
        // Start with known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        const auto& caps_known = printer_state.profile_state().pre_print_option_set();
        REQUIRE_FALSE(caps_known.empty());

        // Switch to unknown printer
        printer_state.set_printer_type_sync("Generic Unknown Printer XYZ");

        // Capabilities should now be empty (no stale cache)
        const auto& caps_unknown = printer_state.profile_state().pre_print_option_set();
        REQUIRE(caps_unknown.empty());
        REQUIRE(caps_unknown.macro_name.empty());
    }

    SECTION("Capabilities become empty when clearing printer type") {
        // Start with known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");

        const auto& caps_before = printer_state.profile_state().pre_print_option_set();
        REQUIRE_FALSE(caps_before.empty());

        // Clear printer type
        printer_state.set_printer_type_sync("");

        // Capabilities should be empty
        const auto& caps_after = printer_state.profile_state().pre_print_option_set();
        REQUIRE(caps_after.empty());
    }

    SECTION("No stale cache when rapidly switching printer types") {
        // Rapidly switch between multiple printer types
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        REQUIRE_FALSE(printer_state.profile_state().pre_print_option_set().empty());

        printer_state.set_printer_type_sync("Unknown Printer 1");
        REQUIRE(printer_state.profile_state().pre_print_option_set().empty());

        printer_state.set_printer_type_sync("FlashForge Adventurer 5M");
        REQUIRE_FALSE(printer_state.profile_state().pre_print_option_set().empty());

        printer_state.set_printer_type_sync("");
        REQUIRE(printer_state.profile_state().pre_print_option_set().empty());

        // Final state: set back to known printer
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        const auto& final_caps = printer_state.profile_state().pre_print_option_set();
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
/**
 * Input shaping is not a pre-print step: a U1 start offers no shaper option
 * and sends no SHAPER_CALIBRATE preference.
 */
TEST_CASE_METHOD(HelixTestFixture, "PrintPreparationManager: a U1 start sends no SHAPER_CALIBRATE",
                 "[print_preparation][pre_print_options][snapmaker_u1]") {
    lv_init_safe();

    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Snapmaker U1");

    REQUIRE(printer_state.profile_state().pre_print_option_set().find("shaper_calibrate") ==
            nullptr);

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);
    // Even a provider answering "on" for a shaper id it was never offered
    // changes nothing.
    manager.set_option_state_provider([](const std::string&) { return 1; });

    auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
    REQUIRE(line_containing(lines, "SHAPER").empty());
    REQUIRE(line_containing(lines, "BED_LEVEL") == "SET_PRINT_PREFERENCES BED_LEVEL=1");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: U1 flow calibration needs every used slot's material",
                 "[print_preparation][pre_print_options][snapmaker_u1][preflight]") {
    lv_init_safe();

    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    printer_state.set_printer_type_sync("Snapmaker U1");
    const PrePrintOption* flow =
        printer_state.profile_state().pre_print_option_set().find("flow_calibrate");
    REQUIRE(flow != nullptr);
    REQUIRE(flow->needs_material);

    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);
    int flow_state = 1;
    manager.set_option_state_provider(
        [&flow_state](const std::string& id) { return id == "flow_calibrate" ? flow_state : 1; });
    CHECK(manager.enabled_options_need_material());
    flow_state = 0;
    CHECK_FALSE(manager.enabled_options_need_material());

    // A queued job's saved choice is asked first.
    CHECK(manager.enabled_options_need_material([](const std::string&) { return 1; }));
    flow_state = 1;
    CHECK_FALSE(manager.enabled_options_need_material(
        [](const std::string& id) { return id == "flow_calibrate" ? 0 : -1; }));
    // A saved set that does not name the option falls through to the live state.
    CHECK(manager.enabled_options_need_material([](const std::string&) { return -1; }));
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
                 "PrintPreparationManager: a failed analysis is reported unless the login was "
                 "refused",
                 "[print_preparation][retry][eventloop][slow][api-key]") {
    const bool refused = GENERATE(false, true);
    if (refused) {
        helix::moonraker_auth::set_credential({"printer.lan", 7125, ""});
        helix::moonraker_auth::mark_unauthorized();
        REQUIRE(helix::moonraker_auth::unauthorized());
    }
    auto errors = std::make_shared<std::vector<std::string>>();
    helix::ui::set_test_notification_event_hook([errors](const helix::ui::NotificationEvent& e) {
        if (e.severity == ToastSeverity::ERROR) {
            errors->push_back(e.message);
        }
    });

    set_list_files_always_fail();
    std::atomic<bool> landed{false};
    manager_.set_macro_analysis_callback(
        [&](const helix::PrintStartAnalysis& /*analysis*/) { landed = true; });
    manager_.analyze_print_start_macro();
    const bool finished = wait_for([&]() { return landed.load(); }, 8000);
    helix::ui::set_test_notification_event_hook(nullptr);
    helix::moonraker_auth::set_credential({});

    INFO("refused login: " << refused);
    REQUIRE(finished);
    REQUIRE(get_list_files_call_count() == 3);
    if (refused) {
        CHECK(errors->empty());
    } else {
        CHECK(*errors == std::vector<std::string>{
                             "Could not analyze PRINT_START macro. Some print options may be "
                             "unavailable."});
    }
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: a failed analysis runs again on reconnect",
                 "[print_preparation][retry][eventloop][slow][1233]") {
    set_list_files_always_fail();
    std::atomic<bool> landed{false};
    manager_.set_macro_analysis_callback(
        [&](const helix::PrintStartAnalysis& /*analysis*/) { landed = true; });

    manager_.analyze_print_start_macro();
    REQUIRE(wait_for([&]() { return landed.load(); }, 8000));
    REQUIRE(get_list_files_call_count() == 3);
    REQUIRE_FALSE(manager_.get_macro_analysis()->found);

    set_list_files_success_empty();
    landed = false;
    printer_state_.set_printer_connection_state(static_cast<int>(ConnectionState::DISCONNECTED),
                                                "Disconnected");
    printer_state_.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED),
                                                "Connected");

    REQUIRE(wait_for([&]() { return landed.load(); }, 5000));
    CHECK(get_list_files_call_count() == 1);
    CHECK_FALSE(manager_.is_macro_analysis_in_progress());
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
TEST_CASE("PrintPreparationManager: get_option_state(id) asks the provider once, for that id",
          "[print_preparation][option_state][p3]") {
    lv_init_safe();
    PrintPreparationManager manager;

    int provider_calls = 0;
    std::string last_id;
    manager.set_option_state_provider([&](const std::string& id) {
        ++provider_calls;
        last_id = id;
        return 1;
    });

    REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
    CHECK(provider_calls == 1);
    CHECK(last_id == "bed_mesh");
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
// The operation registry: the one list of controllable operations
// ============================================================================
//
// Adding a controllable operation means an OperationCategory value, an
// OperationRegistry entry, keyword patterns, skip/perform parameter variations,
// category_key()/category_name(), and printer database entries. Source priority
// and OPT_IN/OPT_OUT semantics are test_capability_matrix.cpp's.

TEST_CASE("PrepManager: the operation registry lists every controllable operation once",
          "[print_preparation][p5]") {
    const OperationCategory controllable[] = {
        OperationCategory::BED_MESH, OperationCategory::QGL, OperationCategory::Z_TILT,
        OperationCategory::NOZZLE_CLEAN, OperationCategory::PURGE_LINE};

    SECTION("each controllable category has an entry keyed by category_key()") {
        for (auto cat : controllable) {
            auto info = OperationRegistry::get(cat);
            INFO("Checking category: " << category_key(cat));
            REQUIRE(info.has_value());
            REQUIRE_FALSE(info->friendly_name.empty());
            REQUIRE(info->capability_key == std::string(category_key(cat)));
        }
        CHECK(std::string(category_key(OperationCategory::BED_MESH)) == "bed_mesh");
        CHECK(std::string(category_key(OperationCategory::QGL)) == "qgl");
        CHECK(std::string(category_key(OperationCategory::Z_TILT)) == "z_tilt");
        CHECK(std::string(category_key(OperationCategory::NOZZLE_CLEAN)) == "nozzle_clean");
        CHECK(std::string(category_key(OperationCategory::PURGE_LINE)) == "purge_line");
    }

    SECTION("operations the options panel cannot toggle have no entry") {
        // HOMING is always required, START_PRINT is the macro itself, BED_LEVEL is
        // the parent of QGL/Z_TILT, and soak/skew have no toggle.
        for (auto cat : {OperationCategory::HOMING, OperationCategory::START_PRINT,
                         OperationCategory::UNKNOWN, OperationCategory::CHAMBER_SOAK,
                         OperationCategory::SKEW_CORRECT, OperationCategory::BED_LEVEL}) {
            INFO(static_cast<int>(cat));
            CHECK_FALSE(OperationRegistry::get(cat).has_value());
        }
    }

    SECTION("all() holds every controllable operation, each round-tripping its key and name") {
        const auto all = OperationRegistry::all();
        std::set<OperationCategory> listed;
        for (const auto& info : all) {
            INFO("Checking: " << info.capability_key);
            listed.insert(info.category);
            CHECK(info.category != OperationCategory::UNKNOWN);
            auto by_key = OperationRegistry::get_by_key(info.capability_key);
            REQUIRE(by_key.has_value());
            CHECK(by_key->category == info.category);
            CHECK(info.friendly_name == category_name(info.category));
            CHECK(info.capability_key == category_key(info.category));
            // Detection in macros needs a skip or perform spelling for it.
            CHECK((!get_skip_variations(info.category).empty() ||
                   !get_perform_variations(info.category).empty()));
        }
        for (auto cat : controllable) {
            CHECK(listed.count(cat) == 1);
        }
    }

    SECTION("lookup by key takes only the lowercase capability keys") {
        CHECK_FALSE(OperationRegistry::get_by_key("nonexistent").has_value());
        CHECK_FALSE(OperationRegistry::get_by_key("").has_value());
        CHECK_FALSE(OperationRegistry::get_by_key("BED_MESH").has_value());
        CHECK_FALSE(OperationRegistry::get_by_key("homing").has_value());
    }

    SECTION("the capability matrix accepts every registry operation from either source") {
        CapabilityMatrix from_macro;
        PrintStartAnalysis analysis;
        analysis.found = true;
        CapabilityMatrix from_database;
        PrePrintOptionSet db_caps;
        db_caps.macro_name = "START_PRINT";
        for (const auto& info : OperationRegistry::all()) {
            PrintStartOperation op;
            op.name = info.capability_key;
            op.category = static_cast<PrintStartOpCategory>(info.category);
            op.has_skip_param = true;
            op.skip_param_name = "SKIP_" + std::string(category_key(info.category));
            op.param_semantic = ParameterSemantic::OPT_OUT;
            analysis.operations.push_back(op);

            PrePrintOption opt;
            opt.id = info.capability_key;
            opt.strategy_kind = PrePrintStrategyKind::MacroParam;
            opt.strategy = PrePrintStrategyMacroParam{"PARAM_" + info.capability_key, "1", "0", ""};
            db_caps.options.push_back(opt);
        }
        from_macro.add_from_macro_analysis(analysis);
        from_database.add_from_database(db_caps);
        for (const auto& info : OperationRegistry::all()) {
            INFO("Verifying matrix support for: " << info.capability_key);
            CHECK(from_macro.is_controllable(info.category));
            CHECK(from_database.is_controllable(info.category));
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
        const auto& set = printer_state.profile_state().pre_print_option_set();
        const PrePrintOption* tl = set.find("timelapse");
        REQUIRE(tl == nullptr);
    }

    SECTION("With timelapse available: timelapse option is appended") {
        printer_state.set_timelapse_available(true);
        helix::ui::UpdateQueue::instance().drain();

        const auto& set = printer_state.profile_state().pre_print_option_set();
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
        REQUIRE(printer_state.profile_state().pre_print_option_set().find("timelapse") != nullptr);

        printer_state.set_timelapse_available(false);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(printer_state.profile_state().pre_print_option_set().find("timelapse") == nullptr);
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

    SECTION("skipped entirely when the LOAD_AI_RUN macro is absent, even when enabled") {
        MacroParamCache::instance().clear();
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "ai_detect" ? 1 : -1; });
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(manager);
        REQUIRE(line_containing(lines, "LOAD_AI_RUN").empty());
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
        // extracts them into print_start, and the start hands the renderer
        // its own file's call.
        gcode::ScanResult scan;
        scan.print_start.found = true;
        scan.print_start.macro_name = "START_PRINT";
        scan.print_start.raw_line = "START_PRINT EXTRUDER_TEMP=260 BED_TEMP=105";
        scan.print_start.extruder_temp = 260;
        scan.print_start.bed_temp = 105;

        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
        auto lines = PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(
            manager, "part.gcode", &scan);
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

    SECTION("the file's own mesh call replaces the pre-start one") {
        // A slicer start sequence that calls BED_MESH_CALIBRATE_START_PRINT
        // meshes on its own; sending the macro ahead of the job meshes twice.
        manager.set_option_state_provider(
            [](const std::string& id) { return id == "bed_mesh" ? 1 : -1; });
        const auto sends_mesh = [&](const std::string& start_gcode) {
            const gcode::ScanResult scan = gcode::GCodeOpsDetector().scan_content(start_gcode);
            for (const auto& l : PrintPreparationManagerTestAccess::get_pre_start_gcode_lines(
                     manager, "part.gcode", &scan)) {
                if (l.find("BED_MESH_CALIBRATE_START_PRINT") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };

        CHECK_FALSE(sends_mesh("G28\n"
                               "BED_MESH_CALIBRATE_START_PRINT EXTRUDER_TEMP=250 BED_TEMP=80\n"
                               "START_PRINT EXTRUDER_TEMP=250 BED_TEMP=80\n"
                               "G1 X10 Y10 E5\n"));
        CHECK_FALSE(sends_mesh("  bed_mesh_calibrate_start_print\n"
                               "G1 X10 Y10 E5\n"));
        // Other mesh lines ahead of the call do not hide it.
        CHECK_FALSE(sends_mesh("BED_MESH_CLEAR\n"
                               "BED_MESH_CALIBRATE_START_PRINT EXTRUDER_TEMP=250 BED_TEMP=80\n"
                               "G1 X10 Y10 E5\n"));
        CHECK_FALSE(sends_mesh("BED_MESH_PROFILE LOAD=default\n"
                               "G28\n"
                               "BED_MESH_CALIBRATE_START_PRINT\n"
                               "G1 X10 Y10 E5\n"));

        // Lines that use a stored mesh or only ask for one do not mesh.
        CHECK(sends_mesh("BED_MESH_PROFILE LOAD=default\n"
                         "START_PRINT EXTRUDER_TEMP=250 BED_TEMP=80\n"
                         "G1 X10 Y10 E5\n"));
        CHECK(sends_mesh("BED_MESH_CLEAR\n"
                         "G1 X10 Y10 E5\n"));
        CHECK(sends_mesh("START_PRINT EXTRUDER_TEMP=250 BED_TEMP=80 FORCE_LEVELING=true\n"
                         "G1 X10 Y10 E5\n"));
        CHECK(sends_mesh("G28\n"
                         "G1 X10 Y10 E5\n"));
    }

    SECTION("temps render 0 when the start has no START_PRINT call of its own") {
        // The shown file's scan is not the started file's.
        gcode::ScanResult scan;
        scan.print_start.found = true;
        scan.print_start.bed_temp = 105;
        scan.print_start.extruder_temp = 260;
        manager.set_cached_scan_result(scan, "part.gcode");

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
// DISABLING it does nothing without the HelixPrint plugin. It still does
// something when it is a PreStartGcode option (its line is emitted either way)
// or a MacroParam whose skip rides a pre-start block (setup_gcode, or any
// PreStartGcode line). Otherwise it is hidden when:
//
//   (b) it is a MacroParam whose skip must be rewritten into START_PRINT, OR
//   (a) a file-embeddable op (bed_mesh/qgl/z_tilt/nozzle_clean) is embedded in
//       the sliced file: every start path drops that strip without the plugin.
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
        const PrePrintOption* opt = ps.profile_state().pre_print_option_set().find(id);
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

    SECTION("file ALSO embeds bed_mesh — still visible") {
        // The embedded strip needs the plugin, but the skip the native macro
        // takes does not, so the toggle still does something.
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

TEST_CASE_METHOD(HelixTestFixture,
                 "disabling_option_requires_plugin: embedded strip needs the plugin on the "
                 "pre-start path too",
                 "[print_preparation][preprint][plugin_gate]") {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_runtime_command_opt("bed_mesh"));
    set.options.push_back(make_pre_start_gcode_opt("ai_detect"));
    GateFixture fx(std::move(set));
    fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                      "f.gcode");

    // A pre-start line sends the start through continue_print_start(), which
    // drops the strip without the plugin just as the direct path does.
    REQUIRE(fx.requires_plugin("bed_mesh") == true);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "disabling_option_requires_plugin: PreStartGcode option with an embedded op "
                 "stays visible",
                 "[print_preparation][preprint][plugin_gate]") {
    // Snapmaker U1 shape: bed_mesh writes a firmware preference. The strip of
    // the file's own mesh needs the plugin, the preference does not.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_pre_start_gcode_opt("bed_mesh"));
    GateFixture fx(std::move(set));
    fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                      "u1.gcode");

    REQUIRE(fx.requires_plugin("bed_mesh") == false);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "disabling_option_requires_plugin: a firmware-held option never needs it",
                 "[print_preparation][preprint][plugin_gate][firmware_seeded]") {
    // RuntimeCommand isolates the file-embedded term (see the case above).
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_runtime_command_opt("bed_mesh"));
    GateFixture fx(std::move(set));
    fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                      "f.gcode");
    PrinterStateTestAccess::merge_firmware_option_defaults(fx.ps, {{"bed_mesh", true}});
    helix::ui::UpdateQueue::instance().drain();
    fx.manager.set_option_state_provider([](const std::string&) { return 0; });

    REQUIRE(PrintPreparationManagerTestAccess::get_ops_to_disable(fx.manager).empty());
    REQUIRE(fx.requires_plugin("bed_mesh") == false);
}

// ============================================================================
// An option whose value a self-storing firmware holds strips no embedded op,
// however it came to be off: that firmware skips the file's own command when
// its setting is off.
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "collect_ops_to_disable: a firmware-held option strips nothing",
                 "[print_preparation][preprint][firmware_seeded]") {
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(make_pre_start_gcode_opt("bed_mesh"));
    GateFixture fx(std::move(set));
    fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                      "u1.gcode");

    auto ops = [&]() { return PrintPreparationManagerTestAccess::get_ops_to_disable(fx.manager); };
    auto seed_firmware = [&](bool enabled) {
        PrinterStateTestAccess::merge_firmware_option_defaults(fx.ps, {{"bed_mesh", enabled}});
        helix::ui::UpdateQueue::instance().drain();
    };

    SECTION("firmware stores it off and the user left it alone") {
        seed_firmware(false);
        REQUIRE(fx.manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
        REQUIRE(ops().empty());
    }

    SECTION("same, with the dialog's row reporting the seeded state") {
        seed_firmware(false);
        fx.manager.set_option_state_provider([](const std::string&) { return 0; });
        REQUIRE(ops().empty());
    }

    SECTION("firmware stores it on and the user turned it off") {
        seed_firmware(true);
        fx.manager.set_option_state_provider([](const std::string&) { return 0; });
        REQUIRE(fx.manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);
        REQUIRE(ops().empty());
        // Nothing to drop, so the start path has no plugin warning to give.
        REQUIRE(fx.manager.describe_dropped_modifications(ops()).empty());
    }

    SECTION("a printer with no persisted-prefs provider strips the embedded op") {
        // No firmware defaults were merged: a database default off is a user
        // choice, and it strips the embedded op.
        PrePrintOptionSet db;
        db.macro_name = "START_PRINT";
        db.options.push_back(make_pre_start_gcode_opt("bed_mesh"));
        db.options.back().default_enabled = false;
        PrinterStateTestAccess::set_option_set(fx.ps, std::move(db));
        REQUIRE(ops() == std::vector<gcode::OperationType>{gcode::OperationType::BED_MESH});
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "collect_ops_to_disable: a printer switched to keeps none of the last one's "
                 "stored settings",
                 "[print_preparation][preprint][firmware_seeded]") {
    GateFixture fx(PrePrintOptionSet{});
    auto hardware = [](const std::vector<std::string>& names) {
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json(names));
        return hw;
    };

    // A U1 whose firmware stores bed_mesh off.
    fx.ps.set_printer_type_sync("Snapmaker U1");
    fx.ps.set_hardware(hardware({"print_task_config", "bed_mesh"}));
    PrinterStateTestAccess::merge_firmware_option_defaults(fx.ps, {{"bed_mesh", false}});
    helix::ui::UpdateQueue::instance().drain();

    // Switch to a printer that stores nothing, type first as a rediscovery does.
    fx.ps.set_printer_type_sync("FlashForge Adventurer 5M");
    fx.ps.set_hardware(hardware({"bed_mesh"}));
    helix::ui::UpdateQueue::instance().drain();

    const PrePrintOption* opt = fx.ps.profile_state().pre_print_option_set().find("bed_mesh");
    REQUIRE(opt != nullptr);
    CHECK_FALSE(opt->default_from_firmware);
    CHECK(opt->default_enabled);

    fx.manager.set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    fx.manager.set_cached_scan_result(scan_with_embedded_op(gcode::OperationType::BED_MESH),
                                      "ad5m.gcode");
    REQUIRE(PrintPreparationManagerTestAccess::get_ops_to_disable(fx.manager) ==
            std::vector<gcode::OperationType>{gcode::OperationType::BED_MESH});
}

TEST_CASE_METHOD(HelixTestFixture,
                 "collect_ops_to_disable: a stored setting read off the previous printer does "
                 "not land on the next",
                 "[print_preparation][preprint][firmware_seeded]") {
    GateFixture fx(PrePrintOptionSet{});
    auto hardware = [](const std::vector<std::string>& names) {
        PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json(names));
        return hw;
    };

    fx.ps.set_printer_type_sync("Snapmaker U1");
    fx.ps.set_hardware(hardware({"print_task_config", "bed_mesh"}));
    helix::ui::UpdateQueue::instance().drain();

    // The U1's last frame is read, and its merge is still queued when the
    // switch to a printer that stores nothing replaces the hardware.
    PrinterStateTestAccess::merge_firmware_option_defaults(fx.ps, {{"bed_mesh", false}});
    fx.ps.set_printer_type_sync("FlashForge Adventurer 5M");
    fx.ps.set_hardware(hardware({"bed_mesh"}));
    helix::ui::UpdateQueue::instance().drain();

    const PrePrintOption* opt = fx.ps.profile_state().pre_print_option_set().find("bed_mesh");
    REQUIRE(opt != nullptr);
    CHECK_FALSE(opt->default_from_firmware);
    CHECK(opt->default_enabled);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "collect_ops_to_disable: new hardware starts from no stored settings",
                 "[print_preparation][preprint][firmware_seeded]") {
    GateFixture fx(PrePrintOptionSet{});
    PrinterDiscovery u1;
    u1.parse_objects(nlohmann::json(std::vector<std::string>{"print_task_config", "bed_mesh"}));

    fx.ps.set_printer_type_sync("Snapmaker U1");
    fx.ps.set_hardware(u1);
    PrinterStateTestAccess::merge_firmware_option_defaults(fx.ps, {{"bed_mesh", false}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(fx.ps.profile_state().pre_print_option_set().find("bed_mesh")->default_from_firmware);

    // Another U1, or the same one reconnecting: it reports its own settings.
    fx.ps.set_hardware(u1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(
        fx.ps.profile_state().pre_print_option_set().find("bed_mesh")->default_from_firmware);
}

// ============================================================================
// A start that dies inside the manager must retire the preparing job
//
// The modify and remap routes end without an error message, so
// PrintStartController's retire_preparing(Failed) is unreachable from them.
// Nothing else can retire the job, and `print_in_progress` is PUBLISHED from that
// job rather than set by hand, so a leak latches the flag until the 1800s watchdog:
// can_start_new_print() refuses every later print, job_holds_machine stays 1 (greying jog,
// levelling, macros and the bypass tile), and the panel sits on "Preparing".
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

    state.print_state().begin_preparing(helix::PrintJobRef{"doomed.gcode", "gcodes", ""});
    REQUIRE(state.print_state().has_preparing_job());
    REQUIRE(lv_subject_get_int(state.print_state().get_print_in_progress_subject()) == 1);

    // api_ is null, so the modify route bails on its FIRST guard. Both of its
    // synchronous guards are real reachable exits — continue_print_start()
    // dispatches here whenever the user disabled an embedded operation, and the
    // connection can drop between arming and dispatch — and every deferred
    // failure exit further in retires the job the same way.
    //
    // Mutation-verified: deleting abandon_start("modify_no_api") fails the
    // REQUIRE_FALSE below. (Deleting the OTHER guard's call does not, which is
    // the point of naming which exit this drives rather than guessing.)
    std::optional<bool> completed;
    PrintPreparationManagerTestAccess::modify_and_print(
        manager, "doomed.gcode", nullptr,
        [&completed](bool success, const std::string&) { completed = success; });

    REQUIRE_FALSE(state.print_state().has_preparing_job());
    REQUIRE(lv_subject_get_int(state.print_state().get_print_in_progress_subject()) == 0);
    REQUIRE(state.print_state().last_preparing_exit() == helix::PreparingExit::Failed);
    // The caller waiting on the start learns it ended.
    REQUIRE(completed.has_value());
    CHECK_FALSE(*completed);

    // And the machine is released — this is the user-visible half of the latch.
    REQUIRE(lv_subject_get_int(state.print_state().get_job_holds_machine_subject()) == 0);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: the scan-result guard retires the preparing job too",
                 "[print_preparation][lifecycle][preparing]") {
    // The sibling exit, reached with a live api_ and no cached scan. Separate
    // case because a single test can only ever prove the FIRST guard it hits.
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    helix::ui::PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);

    state.print_state().begin_preparing(helix::PrintJobRef{"doomed.gcode", "gcodes", ""});
    REQUIRE(state.print_state().has_preparing_job());

    PrintPreparationManagerTestAccess::modify_and_print(manager, "doomed.gcode");

    REQUIRE_FALSE(state.print_state().has_preparing_job());
    REQUIRE(lv_subject_get_int(state.print_state().get_print_in_progress_subject()) == 0);
    REQUIRE(state.print_state().last_preparing_exit() == helix::PreparingExit::Failed);
}

// ============================================================================
// modify_and_print_with_remap(): download -> rewrite -> upload -> plugin start
//
// Every hop is asynchronous through the UpdateQueue, and each deferred stage
// enqueues the next one, so a single drain() only advances the chain by one
// step. The helpers below drive it to quiescence and read back what the mocks
// were asked to do.
// ============================================================================

namespace {

/// Run queued callbacks until nothing new is enqueued. Each drain() swaps out
/// one batch, and these stages enqueue their successor from inside a batch.
void drain_until_quiet() {
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance(), 16);
}

/// Number of lines equal to @p want once any trailing CR is stripped.
size_t count_exact_lines(const std::string& content, const std::string& want) {
    size_t n = 0;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == want) {
            ++n;
        }
    }
    return n;
}

std::string read_whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The four-colour U1 fixture the file-transfer mock resolves by basename.
constexpr const char* kRemapFixture = "u1_4color_ring.gcode";

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a remap uploads the rewritten bytes and starts the "
                 "print under the original name",
                 "[print_preparation][remap]") {
    lv_init_safe();
    // The success path stamps the identity override on the GLOBAL state so the
    // status panel shows the original name rather than the temp copy's.
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    const std::string original =
        read_whole_file("assets/test_gcodes/" + std::string(kRemapFixture));
    REQUIRE_FALSE(original.empty());
    REQUIRE(count_exact_lines(original, "T1") == 11);
    REQUIRE(count_exact_lines(original, "T2") == 5);

    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    drain_until_quiet();

    // The rewritten bytes are what left the machine, not the original.
    const auto& uploads = api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    REQUIRE(uploads[0].root == "gcodes");
    CHECK(uploads[0].content != original);
    CHECK(count_exact_lines(uploads[0].content, "T1") == 0);
    CHECK(count_exact_lines(uploads[0].content, "T2") == 16);

    // The staged path is the one shape the consumer helpers recognise, and it
    // still resolves back to the user's filename.
    CHECK(gcode::is_uploaded_rewrite_path(uploads[0].dest_path));
    CHECK(gcode::resolve_gcode_filename(uploads[0].dest_path) == kRemapFixture);

    // History is keyed on the ORIGINAL name; the temp copy is only what runs.
    const auto& starts = api.job_mock().modified_prints();
    REQUIRE(starts.size() == 1);
    CHECK(starts[0].original_filename == kRemapFixture);
    CHECK(starts[0].temp_file_path == uploads[0].dest_path);
    CHECK(starts[0].modifications == std::vector<std::string>{"remap_T1_to_T2"});

    // Neither local copy outlives the flow.
    const auto& downloads = api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
    CHECK_FALSE(std::filesystem::exists(uploads[0].local_path));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a remap cancelled during the transfer never starts",
                 "[print_preparation][remap][preparing_cancel]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    // T1 -> T2 rewrites and uploads a copy; T5 is not in the file, so the
    // original starts unchanged.
    const bool rewritten = GENERATE(true, false);
    CAPTURE(rewritten);
    const std::map<int, int> remap =
        rewritten ? std::map<int, int>{{1, 2}} : std::map<int, int>{{5, 6}};
    const bool withdraw = GENERATE(true, false);
    CAPTURE(withdraw);

    std::optional<bool> completed;
    manager.modify_and_print_with_remap(
        kRemapFixture, remap, nullptr,
        [&completed](bool ok, const std::string&) { completed = ok; });
    if (withdraw) {
        // Stop while the copy is still on its way.
        state.print_state().retire_preparing(helix::PreparingExit::Cancelled);
    }
    drain_until_quiet();

    const size_t sent =
        api.job_mock().started_prints().size() + api.job_mock().modified_prints().size();
    CHECK(sent == (withdraw ? 0u : 1u));
    if (withdraw) {
        REQUIRE(completed.has_value());
        CHECK_FALSE(*completed);
        if (rewritten) {
            // The uploaded copy goes with the start it was made for.
            const auto& uploads = api.transfers_mock().path_uploads();
            REQUIRE(uploads.size() == 1);
            const auto& deleted = api.files_mock().deleted_files();
            CHECK(std::find(deleted.begin(), deleted.end(), "gcodes/" + uploads[0].dest_path) !=
                  deleted.end());
        }
    } else {
        CHECK(state.print_state().preparing_start() == helix::PreparingStart::Accepted);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a plugin fallback for a cancelled job never starts",
                 "[print_preparation][remap][plugin_server_edits][preparing_cancel]") {
    // The fallback rewrites on the screen, minutes after the job was armed. It
    // must stay bound to that job rather than adopt "no job" and print.
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    state.set_helix_plugin_installed(true);
    PluginFeatureEntry entry;
    entry.version = gcode::kServerEditsMinVersion;
    entry.available = true;
    entry.enabled = true;
    state.plugin_status_state().set_features({{gcode::kServerEditsFeature, entry}});
    drain_until_quiet();
    api.job_mock().mock_edited_print_error(MoonrakerError::from_json_rpc(
        {{"code", 503}, {"message", "Feature unavailable"}}, "server.helix.print_edited"));

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
    manager.set_cached_file_size(123456);
    std::optional<bool> completed;
    manager.modify_and_print_with_remap(
        kRemapFixture, {{1, 2}}, nullptr,
        [&completed](bool ok, const std::string&) { completed = ok; });
    REQUIRE(api.job_mock().edited_prints().size() == 1);
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::InFlight);
    const bool withdraw = GENERATE(true, false);
    CAPTURE(withdraw);
    if (!withdraw) {
        // The fallback runs first: the plugin refused, so nothing reached the
        // printer and Stop must not cancel there while the copy is rewritten.
        UpdateQueue::instance().drain();
        CHECK(state.print_state().preparing_start() == helix::PreparingStart::None);
        drain_until_quiet();
        CHECK(api.job_mock().started_prints().size() + api.job_mock().modified_prints().size() ==
              1);
        return;
    }
    // Stop while the plugin's refusal is on its way back.
    state.print_state().retire_preparing(helix::PreparingExit::Cancelled);
    drain_until_quiet();

    CHECK(api.transfers_mock().download_destinations().empty());
    CHECK(api.job_mock().started_prints().empty());
    CHECK(api.job_mock().modified_prints().empty());
    REQUIRE(completed.has_value());
    CHECK_FALSE(*completed);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a staged start that loses its answer stays preparing",
                 "[print_preparation][remap][preparing_cancel][1898]") {
    // A timeout means Klipper is busy running the start: the job stays
    // preparing, Stop or no Stop, and the copy it is printing is not deleted.
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    // The plugin's print_edited path, or the upload path's print_modified.
    const bool edited = GENERATE(true, false);
    CAPTURE(edited);
    const MoonrakerError lost = MoonrakerError::timeout("server.helix.print", 600000);
    if (edited) {
        state.set_helix_plugin_installed(true);
        PluginFeatureEntry entry;
        entry.version = gcode::kServerEditsMinVersion;
        entry.available = true;
        entry.enabled = true;
        state.plugin_status_state().set_features({{gcode::kServerEditsFeature, entry}});
        drain_until_quiet();
        api.job_mock().mock_edited_print_error(lost);
    } else {
        api.job_mock().mock_modified_print_error(lost);
    }

    const bool stop_waiting = GENERATE(true, false);
    CAPTURE(stop_waiting);

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
    if (stop_waiting) {
        state.print_state().note_preparing_cancel_requested();
    }
    manager.set_cached_file_size(123456);
    std::optional<bool> completed;
    manager.modify_and_print_with_remap(
        kRemapFixture, {{1, 2}}, nullptr,
        [&completed](bool ok, const std::string&) { completed = ok; });
    drain_until_quiet();

    REQUIRE(api.job_mock().edited_prints().size() + api.job_mock().modified_prints().size() == 1);
    CHECK(state.print_state().has_preparing_job());
    CHECK(state.print_state().preparing_start() == helix::PreparingStart::Unanswered);
    CHECK_FALSE(completed.has_value());
    CHECK(api.files_mock().deleted_files().empty());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a remap on a transport without local copies is refused",
                 "[print_preparation][remap]") {
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    api.transfers_mock().mock_no_local_copies();

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    drain_until_quiet();

    // Printing the original would run the job on the tools the user remapped
    // away from: nothing is downloaded and nothing starts.
    CHECK(api.transfers_mock().download_destinations().empty());
    CHECK(api.job_mock().started_prints().empty());
    CHECK(api.job_mock().modified_prints().empty());
    CHECK_FALSE(state.print_state().has_preparing_job());
    CHECK(state.print_state().last_preparing_exit() == helix::PreparingExit::Failed);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a remap travels to the plugin as an edit list when "
                 "server_edits is ready",
                 "[print_preparation][remap][plugin_server_edits]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    state.set_helix_plugin_installed(true);

    const bool enabled = GENERATE(true, false);
    CAPTURE(enabled);
    PluginFeatureEntry entry;
    entry.version = gcode::kServerEditsMinVersion;
    entry.available = true;
    entry.enabled = enabled;
    state.plugin_status_state().set_features({{gcode::kServerEditsFeature, entry}});
    drain_until_quiet();

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    manager.set_cached_file_size(123456);
    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    drain_until_quiet();

    const auto& edited = api.job_mock().edited_prints();
    if (enabled) {
        // Nothing crosses the wire but the edit list.
        CHECK(api.transfers_mock().download_destinations().empty());
        CHECK(api.transfers_mock().path_uploads().empty());
        CHECK(api.job_mock().modified_prints().empty());
        REQUIRE(edited.size() == 1);
        CHECK(edited[0].original_filename == kRemapFixture);
        CHECK(gcode::is_uploaded_rewrite_path(edited[0].temp_file_path));
        CHECK(gcode::resolve_gcode_filename(edited[0].temp_file_path) == kRemapFixture);
        CHECK(edited[0].edits == *gcode::to_server_edits(gcode::EditList{{}, {{1, 2}}}));
        CHECK(edited[0].modifications == std::vector<std::string>{"remap_T1_to_T2"});
        CHECK(edited[0].file_size == 123456);
    } else {
        // Switched off, the screen rewrites the file as it always has.
        CHECK(edited.empty());
        CHECK(api.transfers_mock().path_uploads().size() == 1);
        CHECK(api.job_mock().modified_prints().size() == 1);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a print_edited refused for space says so",
                 "[print_preparation][remap][plugin_server_edits][plugin_storage]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    state.set_helix_plugin_installed(true);
    PluginFeatureEntry entry;
    entry.version = gcode::kServerEditsMinVersion;
    entry.available = true;
    entry.enabled = true;
    state.plugin_status_state().set_features({{gcode::kServerEditsFeature, entry}});
    drain_until_quiet();

    const int code = GENERATE(storage::kNoSpaceRpcCode, 409);
    CAPTURE(code);
    api.job_mock().mock_edited_print_error(MoonrakerError::from_json_rpc(
        {{"code", code}, {"message", "Refused"}}, "server.helix.print_edited"));
    std::vector<std::string> errors;
    helix::ui::set_test_notification_error_hook(
        [&errors](const std::string& msg) { errors.push_back(msg); });

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    drain_until_quiet();
    helix::ui::set_test_notification_error_hook(nullptr);

    REQUIRE(api.job_mock().edited_prints().size() == 1);
    REQUIRE(errors.size() == 1);
    if (code == storage::kNoSpaceRpcCode) {
        CHECK(errors[0] == storage::kNoUploadSpaceMessage);
    } else {
        CHECK(errors[0].rfind("Failed to start print: ", 0) == 0);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a failed print_edited falls back only when the "
                 "plugin cannot do the edit",
                 "[print_preparation][remap][plugin_server_edits]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    state.set_helix_plugin_installed(true);
    PluginFeatureEntry entry;
    entry.version = gcode::kServerEditsMinVersion;
    entry.available = true;
    entry.enabled = true;
    state.plugin_status_state().set_features({{gcode::kServerEditsFeature, entry}});
    drain_until_quiet();

    auto rpc = [](int code, const char* message) {
        return MoonrakerError::from_json_rpc({{"code", code}, {"message", message}},
                                             "server.helix.print_edited");
    };
    MoonrakerError lost;
    lost.type = MoonrakerErrorType::CONNECTION_LOST;

    struct Case {
        const char* what;
        MoonrakerError error;
        bool falls_back;
    };
    const Case cases[] = {
        {"timeout", MoonrakerError::timeout("server.helix.print_edited", 1000), false},
        {"connection lost", lost, false},
        {"file changed", rpc(409, "Refusing to edit: line 3 is not the line the scan found"),
         false},
        {"method not found", rpc(-32601, "Method not found"), true},
        {"edit kind the plugin lacks", rpc(400, "Invalid edit list: edit 0: unknown action"), true},
    };
    for (const auto& c : cases) {
        CAPTURE(c.what);
        api.job_mock().mock_edited_print_error(c.error);
        const size_t edited_before = api.job_mock().edited_prints().size();
        const size_t downloads_before = api.transfers_mock().download_destinations().size();
        const size_t modified_before = api.job_mock().modified_prints().size();
        const size_t deletes_before = api.files_mock().deleted_files().size();

        PrintPreparationManager manager;
        manager.set_dependencies(&api, &state);
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        std::optional<bool> completed;
        manager.modify_and_print_with_remap(
            kRemapFixture, {{1, 2}}, nullptr,
            [&completed](bool ok, const std::string&) { completed = ok; });
        drain_until_quiet();

        CHECK(api.job_mock().edited_prints().size() == edited_before + 1);
        // The plugin removes its own copy; nothing here deletes a file it may be printing.
        CHECK(api.files_mock().deleted_files().size() == deletes_before);
        if (c.falls_back) {
            CHECK(api.transfers_mock().download_destinations().size() == downloads_before + 1);
            CHECK(api.job_mock().modified_prints().size() == modified_before + 1);
        } else {
            // No second start: after a timeout the plugin may still start this one.
            CHECK(api.transfers_mock().download_destinations().size() == downloads_before);
            CHECK(api.job_mock().modified_prints().size() == modified_before);
            CHECK(api.job_mock().started_prints().empty());
            // A timeout leaves the job for the printer to settle; a dropped
            // connection with no Stop waiting, like a refusal, fails it.
            const bool lost_answer = c.error.command_may_still_run();
            CHECK(state.print_state().has_preparing_job() == lost_answer);
            CHECK(completed.has_value() == !lost_answer);
            if (completed) {
                CHECK_FALSE(*completed);
            }
        }
        state.print_state().retire_preparing(helix::PreparingExit::Failed);
    }
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a staged copy names the original's whole path",
                 "[print_preparation][remap][reprint]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    const std::string original = std::string("parts/") + kRemapFixture;
    state.print_state().begin_preparing(PrintJobRef{original, "gcodes", ""});

    SECTION("remap") {
        manager.modify_and_print_with_remap(original, {{1, 2}}, nullptr);
    }
    SECTION("pre-print modification") {
        manager.set_cached_scan_result(gcode::ScanResult{}, original);
        PrintPreparationManagerTestAccess::modify_and_print(manager, original);
    }
    drain_until_quiet();

    // The printer reports only the staged name; after a restart it is all
    // there is to recover the original from.
    const auto& uploads = api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(gcode::resolve_gcode_filename(uploads[0].dest_path) == original);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a printer that cannot print from a dot-directory "
                 "gets its copies staged in the visible directory",
                 "[print_preparation][remap][staging]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;
    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    const bool forgex = GENERATE(true, false);
    const std::string route = GENERATE(std::string("remap"), std::string("modify"));
    CAPTURE(forgex, route);
    REQUIRE(state.profile_state().set_printer_type(forgex ? "FlashForge Adventurer 5M (ForgeX)"
                                                          : "FlashForge Adventurer 5M",
                                                   true, false, false));
    const std::string root = forgex ? "helix_staging/modified_" : ".helix_temp/modified_";
    if (route == "remap") {
        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    } else {
        manager.set_cached_scan_result(gcode::ScanResult{}, kRemapFixture);
        PrintPreparationManagerTestAccess::modify_and_print(manager, kRemapFixture);
    }
    drain_until_quiet();

    const auto& uploads = api.transfers_mock().path_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(uploads[0].dest_path.rfind(root, 0) == 0);
    CHECK(gcode::resolve_gcode_filename(uploads[0].dest_path) == kRemapFixture);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a remap that changes nothing prints the original",
                 "[print_preparation][remap]") {
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    // The fixture uses T0-T3, so remapping T7 rewrites no line at all.
    manager.modify_and_print_with_remap(kRemapFixture, {{7, 8}}, nullptr);
    drain_until_quiet();

    CHECK(api.transfers_mock().path_uploads().empty());
    CHECK(api.job_mock().modified_prints().empty());
    REQUIRE(api.job_mock().started_prints().size() == 1);
    CHECK(api.job_mock().started_prints()[0] == kRemapFixture);

    // Both local copies are gone even though neither was uploaded.
    const auto& downloads = api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);
    CHECK_FALSE(std::filesystem::exists(downloads[0]));
}

// BusyOverlay is process-wide, so the transfer error that hides it is bound to
// no object: it must still run when the manager that showed it is gone.
TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a failed download hides the busy overlay after the "
                 "manager is destroyed",
                 "[print_preparation][remap][lifetime]") {
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    {
        PrintPreparationManager manager;
        manager.set_dependencies(&api, &state);
        state.print_state().begin_preparing(PrintJobRef{"no_such_file.gcode", "gcodes", ""});
        manager.modify_and_print_with_remap("no_such_file.gcode", {{1, 2}}, nullptr);
        REQUIRE(BusyOverlay::is_pending());
    }

    drain_until_quiet();
    CHECK_FALSE(BusyOverlay::is_pending());
    CHECK_FALSE(BusyOverlay::is_visible());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a failed remap start answers its completion",
                 "[print_preparation][remap]") {
    // The completion is how the caller learns to re-enable Print: a remap that
    // dies on the way must answer it, once, as a failure.
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{"no_such_file.gcode", "gcodes", ""});

    int answers = 0;
    bool answered_success = true;
    manager.modify_and_print_with_remap("no_such_file.gcode", {{1, 2}}, nullptr,
                                        [&](bool success, const std::string& /*error*/) {
                                            ++answers;
                                            answered_success = success;
                                        });
    drain_until_quiet();

    CHECK(answers == 1);
    CHECK_FALSE(answered_success);
    CHECK_FALSE(state.print_state().has_preparing_job());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: the remap download lands where the sweeper reaps it",
                 "[print_preparation][remap]") {
    // A crash between the download and its delete leaves a full copy of the job
    // behind. GCodeFileModifier::cleanup_temp_files() is the only sweeper, and
    // it matches on the mod_ prefix inside the gcode_mod cache, so the download
    // has to be named into that shape or nothing ever collects it.
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;
    manager.set_dependencies(&api, &state);
    state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});

    manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
    drain_until_quiet();

    const auto& downloads = api.transfers_mock().download_destinations();
    REQUIRE(downloads.size() == 1);

    // Model the leak: put a file back at the download's path and age it past
    // the sweeper's cutoff, so only this file is eligible and a concurrently
    // running shard's fresh temp files are not.
    {
        std::ofstream leaked(downloads[0], std::ios::binary);
        leaked << "G28\n";
    }
    REQUIRE(std::filesystem::exists(downloads[0]));
    std::filesystem::last_write_time(downloads[0], std::filesystem::file_time_type::clock::now() -
                                                       std::chrono::hours(2));

    gcode::GCodeFileModifier::cleanup_temp_files(3600);

    CHECK_FALSE(std::filesystem::exists(downloads[0]));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: every remap failure retires the preparing job",
                 "[print_preparation][remap][preparing]") {
    lv_init_safe();
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    PrintPreparationManager manager;

    // Every exit owes the same thing: the job retired Failed and the machine
    // released. Asserted per section rather than after them, so a section that
    // aborts cannot leave the check running against a case that armed nothing.
    auto expect_job_retired = [&state]() {
        REQUIRE_FALSE(state.print_state().has_preparing_job());
        CHECK(lv_subject_get_int(state.print_state().get_print_in_progress_subject()) == 0);
        CHECK(state.print_state().last_preparing_exit() == PreparingExit::Failed);
        CHECK(lv_subject_get_int(state.print_state().get_job_holds_machine_subject()) == 0);
    };

    SECTION("no API") {
        manager.set_dependencies(nullptr, &state);
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
        drain_until_quiet();
        expect_job_retired();
    }

    SECTION("download fails") {
        manager.set_dependencies(&api, &state);
        state.print_state().begin_preparing(PrintJobRef{"no_such_file.gcode", "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        // Nothing under assets/test_gcodes/ resolves this, so the mock errors.
        manager.modify_and_print_with_remap("no_such_file.gcode", {{1, 2}}, nullptr);
        drain_until_quiet();

        CHECK(api.transfers_mock().path_uploads().empty());
        expect_job_retired();
    }

    SECTION("upload fails") {
        manager.set_dependencies(&api, &state);
        api.transfers_mock().mock_fail_path_uploads();
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
        drain_until_quiet();

        REQUIRE(api.transfers_mock().path_uploads().size() == 1);
        CHECK(api.job_mock().modified_prints().empty());
        // The rewritten copy is not left behind by the failure exit either.
        CHECK_FALSE(std::filesystem::exists(api.transfers_mock().path_uploads()[0].local_path));
        expect_job_retired();
    }

    SECTION("plugin refuses the modified print") {
        manager.set_dependencies(&api, &state);
        api.job_mock().mock_fail_modified_prints();
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
        drain_until_quiet();

        REQUIRE(api.job_mock().modified_prints().size() == 1);
        // The staged copy is already on the printer, so it has to be reclaimed.
        REQUIRE(api.transfers_mock().path_uploads().size() == 1);
        const auto& deleted = api.files_mock().deleted_files();
        REQUIRE(deleted.size() == 1);
        CHECK(deleted[0] == "gcodes/" + api.transfers_mock().path_uploads()[0].dest_path);
        expect_job_retired();
    }

    SECTION("the printer has no room for the rewritten copy") {
        MoonrakerClientMockTestAccess::report_full_disk(mock_printer.client);
        std::vector<std::string> errors;
        helix::ui::set_test_notification_error_hook(
            [&errors](const std::string& msg) { errors.push_back(msg); });
        manager.set_dependencies(&api, &state);
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
        drain_until_quiet();
        helix::ui::set_test_notification_error_hook(nullptr);

        // Nothing is sent that would fill the disk mid-upload.
        CHECK(api.transfers_mock().path_uploads().empty());
        CHECK(api.job_mock().modified_prints().empty());
        CHECK(errors == std::vector<std::string>{storage::kNoUploadSpaceMessage});
        expect_job_retired();
    }

    SECTION("the plugin refuses the start for space") {
        std::vector<std::string> errors;
        helix::ui::set_test_notification_error_hook(
            [&errors](const std::string& msg) { errors.push_back(msg); });
        manager.set_dependencies(&api, &state);
        api.job_mock().mock_modified_print_error(MoonrakerError::from_json_rpc(
            {{"code", storage::kNoSpaceRpcCode}, {"message", "Not enough free space"}},
            "server.helix.print_modified"));
        state.print_state().begin_preparing(PrintJobRef{kRemapFixture, "gcodes", ""});
        REQUIRE(state.print_state().has_preparing_job());

        manager.modify_and_print_with_remap(kRemapFixture, {{1, 2}}, nullptr);
        drain_until_quiet();
        helix::ui::set_test_notification_error_hook(nullptr);
        api.job_mock().mock_modified_print_error(std::nullopt);

        REQUIRE(api.job_mock().modified_prints().size() == 1);
        // The space message, which says where to free some, not a bare failure.
        CHECK(errors == std::vector<std::string>{storage::kNoUploadSpaceMessage});
        expect_job_retired();
    }
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

// ============================================================================
// A SLOW pre-start that was never cancelled must still start the print
// ============================================================================
//
// The staleness bound this replaces read elapsed time as a proxy for "the user
// no longer wants this". On a K2 Plus (2026-08-25) that proxy was wrong in the
// expensive direction: a pre-start block containing a bed mesh legitimately ran
// 494s, the RPC returned SUCCESS, and the 180s bound threw the print away with
// no error shown. The user had told it to print a 32-hour job and it silently
// did nothing.
//
// A backed-up ack (the K1C case the bound was written for) and a slow-but-wanted
// macro arrive through the same callback with the same signature, so no clock
// can separate them. The preparing-job epoch can: PrinterPrintState bumps it on
// begin_preparing and zeroes it on retire_preparing, so cancel, failure and
// supersede all change it while a job that is simply taking its time does not.

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: a slow pre-start still starts when nothing retired it",
                 "[preparation][stale][preprint]") {
    std::atomic<int> starts{0};
    server_->clear_handlers();
    server_->on_method("printer.print.start", [&](const json&) -> json {
        starts++;
        return json{{"result", "ok"}};
    });

    // A job is armed and STAYS armed — nobody cancelled.
    printer_state_.print_state().begin_preparing(
        helix::PrintJobRef{"mesh_print.gcode", "gcodes", ""});
    const int epoch =
        lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject());
    REQUIRE(epoch != 0);
    PrintPreparationManagerTestAccess::set_pre_start_epoch(manager_, epoch);

    // Eight minutes of bed mesh — well past the old 180s bound.
    PrintPreparationManagerTestAccess::set_pre_start_sent_ago(manager_, std::chrono::minutes(8));

    // The SUCCESS path is asynchronous — it reaches the printer over the mock
    // websocket — so the callback lands after this frame would otherwise have
    // returned. Atomics plus wait_for() keep the referands alive and the queue
    // pumped; capturing plain locals by reference here smashes the stack when
    // the late callback writes into a dead frame.
    std::atomic<bool> completed{false};
    std::atomic<bool> success{false};
    helix::ui::PrintCompletionCallback cb = [&](bool ok, const std::string&) {
        success.store(ok);
        completed.store(true);
    };
    PrintPreparationManagerTestAccess::continue_print_start(manager_, "mesh_print.gcode", cb);

    // THE REGRESSION: this was dropped, silently, after eight minutes of work.
    REQUIRE(wait_for([&]() { return completed.load(); }, 5000));
    CHECK(success.load());
    CHECK(starts.load() == 1);
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: a retired job drops its pre-start ack however fresh",
                 "[preparation][stale][preprint]") {
    // The other half of the same rule, and the one the old bound was protecting:
    // intent is gone, so the ack must not start anything — and crucially this
    // holds with NO time having passed, which a staleness bound could never see.
    std::atomic<int> starts{0};
    server_->clear_handlers();
    server_->on_method("printer.print.start", [&](const json&) -> json {
        starts++;
        return json{{"result", "ok"}};
    });

    printer_state_.print_state().begin_preparing(
        helix::PrintJobRef{"cancelled.gcode", "gcodes", ""});
    const int epoch =
        lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject());
    PrintPreparationManagerTestAccess::set_pre_start_epoch(manager_, epoch);

    // The user cancels while the macro is still running.
    printer_state_.print_state().retire_preparing(helix::PreparingExit::Cancelled);

    // Ack lands one second later — fresh by any clock, dead by intent.
    PrintPreparationManagerTestAccess::set_pre_start_sent_ago(manager_, std::chrono::seconds(1));

    bool completed = false;
    bool success = true;
    helix::ui::PrintCompletionCallback cb = [&](bool ok, const std::string&) {
        completed = true;
        success = ok;
    };
    PrintPreparationManagerTestAccess::continue_print_start(manager_, "cancelled.gcode", cb);

    CHECK(starts.load() == 0);
    CHECK(completed);
    CHECK_FALSE(success);
}

TEST_CASE_METHOD(MacroAnalysisRetryFixture,
                 "PrintPreparationManager: a superseding job drops the previous pre-start ack",
                 "[preparation][stale][preprint]") {
    // Retire-then-rearm is the case has_preparing_job() alone cannot see: a job
    // IS armed at ack time, just not the one this block was sent for.
    std::atomic<int> starts{0};
    server_->clear_handlers();
    server_->on_method("printer.print.start", [&](const json&) -> json {
        starts++;
        return json{{"result", "ok"}};
    });

    printer_state_.print_state().begin_preparing(helix::PrintJobRef{"first.gcode", "gcodes", ""});
    const int first_epoch =
        lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject());
    PrintPreparationManagerTestAccess::set_pre_start_epoch(manager_, first_epoch);

    printer_state_.print_state().retire_preparing(helix::PreparingExit::Superseded);
    printer_state_.print_state().begin_preparing(helix::PrintJobRef{"second.gcode", "gcodes", ""});
    REQUIRE(printer_state_.print_state().has_preparing_job()); // armed — but a DIFFERENT job
    REQUIRE(lv_subject_get_int(printer_state_.print_state().get_preparing_epoch_subject()) !=
            first_epoch);

    PrintPreparationManagerTestAccess::set_pre_start_sent_ago(manager_, std::chrono::seconds(1));

    bool completed = false;
    bool success = true;
    helix::ui::PrintCompletionCallback cb = [&](bool ok, const std::string&) {
        completed = true;
        success = ok;
    };
    PrintPreparationManagerTestAccess::continue_print_start(manager_, "first.gcode", cb);

    CHECK(starts.load() == 0);
    CHECK(completed);
    CHECK_FALSE(success);
}

// ============================================================================
// Detail-view rows from PRINT_START analysis
// ============================================================================

namespace {

PrintStartAnalysis analysis_with_qgl(const std::string& param, ParameterSemantic semantic) {
    PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    PrintStartOperation op;
    op.name = "QUAD_GANTRY_LEVEL";
    op.category = PrintStartOpCategory::QGL;
    op.has_skip_param = true;
    op.skip_param_name = param;
    op.param_semantic = semantic;
    analysis.operations.push_back(op);
    return analysis;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: displayed_options fills rows from the macro "
                 "when the database declares none",
                 "[print_preparation][macro_rows]") {
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);
    MockOptionState state;
    manager.set_option_state_provider(state.provider());

    SECTION("no database set and a SKIP_QGL macro give a QGL row") {
        printer_state.set_printer_type_sync("Voron 2.4");
        manager.set_macro_analysis(analysis_with_qgl("SKIP_QGL", ParameterSemantic::OPT_OUT));

        const auto rows = manager.displayed_options();
        REQUIRE(rows.options.size() == 1);
        const PrePrintOption& qgl = rows.options[0];
        REQUIRE(qgl.id == "qgl");
        REQUIRE(qgl.default_enabled);
        REQUIRE(PrePrintOptionsRenderer::label_for(qgl) == "Quad gantry level");
        const auto* param = std::get_if<PrePrintStrategyMacroParam>(&qgl.strategy);
        REQUIRE(param);
        REQUIRE(param->param_name == "SKIP_QGL");
        REQUIRE(param->skip_value == "1");

        state.disable("qgl");
        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params == std::vector<std::pair<std::string, std::string>>{{"SKIP_QGL", "1"}});
    }

    SECTION("an opt-in param sends the row's own skip value") {
        printer_state.set_printer_type_sync("Voron 2.4");
        manager.set_macro_analysis(analysis_with_qgl("PERFORM_QGL", ParameterSemantic::OPT_IN));

        const auto rows = manager.displayed_options();
        REQUIRE(rows.options.size() == 1);
        const auto* param = std::get_if<PrePrintStrategyMacroParam>(&rows.options[0].strategy);
        REQUIRE(param);
        REQUIRE(param->skip_value == "0");

        state.disable("qgl");
        auto skip_params = PrintPreparationManagerTestAccess::get_skip_params(manager);
        REQUIRE(skip_params ==
                std::vector<std::pair<std::string, std::string>>{{"PERFORM_QGL", "0"}});
    }

    SECTION("a database set wins") {
        printer_state.set_printer_type_sync("FlashForge Adventurer 5M Pro");
        manager.set_macro_analysis(analysis_with_qgl("SKIP_QGL", ParameterSemantic::OPT_OUT));

        const auto rows = manager.displayed_options();
        REQUIRE(rows.find("qgl") == nullptr);
        REQUIRE(rows.options.size() ==
                printer_state.profile_state().pre_print_option_set().options.size());
    }

    SECTION("no analysis gives no rows") {
        printer_state.set_printer_type_sync("Voron 2.4");
        REQUIRE(manager.displayed_options().options.empty());
    }

    SECTION("the card follows whether the macro gave any row") {
        printer_state.set_printer_type_sync("Voron 2.4");
        printer_state.set_helix_plugin_installed(true);
        auto aggregate = [&] {
            UpdateQueueTestAccess::drain(UpdateQueue::instance());
            return lv_subject_get_int(
                printer_state.composite_visibility_state().get_has_any_preprint_options_subject());
        };

        manager.set_macro_analysis(analysis_with_qgl("SKIP_QGL", ParameterSemantic::OPT_OUT));
        REQUIRE(manager.displayed_options().options.size() == 1);
        REQUIRE(aggregate() == 1);

        // QGL found but run unconditionally: no skip param, so no row and no card.
        PrintStartAnalysis unskippable = analysis_with_qgl("SKIP_QGL", ParameterSemantic::OPT_OUT);
        unskippable.operations[0].has_skip_param = false;
        unskippable.operations[0].skip_param_name.clear();
        manager.set_macro_analysis(unskippable);
        REQUIRE(manager.displayed_options().options.empty());
        REQUIRE(aggregate() == 0);
    }

    SECTION("a re-analysis drops the macro rows from the card until it lands") {
        printer_state.set_printer_type_sync("Voron 2.4");
        printer_state.set_helix_plugin_installed(true);
        auto aggregate = [&] {
            UpdateQueueTestAccess::drain(UpdateQueue::instance());
            return lv_subject_get_int(
                printer_state.composite_visibility_state().get_has_any_preprint_options_subject());
        };

        manager.set_macro_analysis(analysis_with_qgl("SKIP_QGL", ParameterSemantic::OPT_OUT));
        REQUIRE(aggregate() == 1);

        PrintPreparationManagerTestAccess::refresh_macro_analysis(manager);
        REQUIRE(manager.displayed_options().options.empty());
        REQUIRE(aggregate() == 0);
    }

    SECTION("an empty analysis gives no rows") {
        printer_state.set_printer_type_sync("Voron 2.4");
        PrintStartAnalysis analysis;
        analysis.found = true;
        analysis.macro_name = "PRINT_START";
        manager.set_macro_analysis(analysis);

        REQUIRE(manager.displayed_options().options.empty());
    }

    SECTION("an operation without a skip param gives no row") {
        printer_state.set_printer_type_sync("Voron 2.4");
        auto analysis = analysis_with_qgl("", ParameterSemantic::OPT_OUT);
        analysis.operations[0].has_skip_param = false;
        manager.set_macro_analysis(analysis);

        REQUIRE(manager.displayed_options().options.empty());
    }
}

namespace {
/// The details scan over the mock, whose partial download serves planted files.
struct ObjectScanFixture : public LVGLTestFixture {
    MockPrinter mock_printer;
    helix::ui::PrintPreparationManager manager;

    ObjectScanFixture() {
        manager.set_dependencies(&mock_printer.api, &mock_printer.state);
    }
    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};
} // namespace

TEST_CASE_METHOD(ObjectScanFixture,
                 "The file scan collects EXCLUDE_OBJECT_DEFINE objects in file order",
                 "[print_preparation][pre_start_exclude]") {
    helix::PlantedGcode file("scan_objects.gcode", "",
                             "; HEADER\n"
                             "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25.5,-4.1 "
                             "POLYGON=[[20,-9],[31,-9],[31,1],[20,1]]\n"
                             "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
                             "START_PRINT\n"
                             "G1 X10 Y10 E1\n");
    manager.scan_file_for_operations(file.name(), "");
    settle();

    REQUIRE(manager.has_scan_result_for(file.name()));
    const auto& objects = manager.get_scan_result()->objects;
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].name == "Cone_id_0");
    CHECK(objects[0].polygon.size() == 4);
    CHECK(objects[1].name == "Cube_id_1");
}

TEST_CASE_METHOD(ObjectScanFixture, "Definitions past the scan window are not in the scan",
                 "[print_preparation][pre_start_exclude]") {
    std::string content = "; HEADER\n";
    content += std::string(helix::kPrinterStopScanBytes, ';') + "\n";
    content += "EXCLUDE_OBJECT_DEFINE NAME=Late_A CENTER=1,1\n";
    content += "EXCLUDE_OBJECT_DEFINE NAME=Late_B CENTER=9,9\n";
    helix::PlantedGcode file("late_defines.gcode", "", content);
    manager.scan_file_for_operations(file.name(), "");
    settle();

    REQUIRE(manager.has_scan_result_for(file.name()));
    CHECK(manager.get_scan_result()->objects.empty());
}

// The plugin-modified start confirms through the same navigate callback the
// direct start uses, so object picks wrapped around it follow the print.
TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a plugin-modified start sends the picks it was handed",
                 "[print_preparation][pre_start_exclude]") {
    lv_init_safe();
    PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);

    MockPrinter mock_printer;
    mock_printer.client.connect("ws://mock/websocket", []() {}, []() {});
    mock_printer.state.set_klippy_state_sync(helix::KlippyState::READY);
    set_moonraker_api(&mock_printer.api);
    PrintPreparationManager manager;
    manager.set_dependencies(&mock_printer.api, &mock_printer.state);
    manager.set_cached_scan_result(gcode::ScanResult{}, kRemapFixture);
    mock_printer.client.clear_gcode_script_history();

    int navigated = 0;
    PrintPreparationManagerTestAccess::modify_and_print(
        manager, kRemapFixture,
        helix::ui::with_pre_start_exclusions([&navigated]() { ++navigated; }, {"Cube_id_1"}));
    drain_until_quiet();

    CHECK(mock_printer.api.transfers_mock().path_uploads().size() == 1); // the modified copy
    CHECK(navigated == 1);
    std::vector<std::string> exclusions;
    for (const auto& line : mock_printer.client.gcode_script_history()) {
        if (line.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
            exclusions.push_back(line);
        }
    }
    CHECK(exclusions == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    set_moonraker_api(nullptr);
    mock_printer.client.disconnect();
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: estimate reads the history window the start will run in",
                 "[print_preparation][estimate]") {
    // The pre-start line exists only once the extruder temperature is known,
    // which for this file only its scanned START_PRINT call states. A start
    // sends it, so the collector measures the host-pre-start window.
    PrePrintOption ext_only = make_pre_start_gcode_opt("ai_detect");
    ext_only.strategy = PrePrintStrategyPreStartGcode{"{?ext}AI_RUN T={extruder_temp}{/?}"};
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    set.options.push_back(ext_only);
    GateFixture fx(std::move(set));

    PreprintConfigScope config;
    for (const auto& [window, total] : {std::pair{helix::PreprintWindow::HostPreStart, 500},
                                        std::pair{helix::PreprintWindow::PrinterEdge, 300}}) {
        helix::PreprintEntry e;
        e.total_seconds = total;
        e.timestamp = 1000;
        e.temp_bucket = 1;
        e.window = window;
        helix::PreprintPredictor::append_to_config(e);
    }

    gcode::ScanResult scan;
    scan.print_start.found = true;
    scan.print_start.macro_name = "START_PRINT";
    scan.print_start.extruder_temp = 220;
    scan.print_start.bed_temp = 60;
    const FileMetadata file = file_at(60, 220);
    PrintPreparationManagerTestAccess::keep_scan(fx.manager, file.filename, scan);

    set_temps(fx.ps, 25, 25);
    fx.manager.recalculate_estimate(&file, file.filename);
    REQUIRE(lv_subject_get_int(fx.manager.get_preprint_estimate_subject()) == 500);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a start records the phases its toggles turned off",
                 "[print_preparation][estimate]") {
    lv_init_safe();
    PrinterState& printer_state = get_printer_state();
    PrinterStateTestAccess::reset(printer_state);
    printer_state.init_subjects(false);
    PrintPreparationManager manager;
    manager.set_dependencies(nullptr, &printer_state);

    // bed_mesh off, qgl on, the rest not shown.
    manager.set_option_state_provider([](const std::string& id) {
        return id == "bed_mesh" ? 0 : id == "qgl" ? 1 : -1;
    });
    const helix::PreprintPhaseOverrides overrides =
        PrintPreparationManagerTestAccess::preprint_phase_overrides(manager);
    CHECK(overrides ==
          helix::PreprintPhaseOverrides{{static_cast<int>(helix::PrintStartPhase::BED_MESH), false},
                                        {static_cast<int>(helix::PrintStartPhase::QGL), true}});
    CHECK(helix::PreprintPredictor::phases_skipped_by(overrides) ==
          std::set<int>{static_cast<int>(helix::PrintStartPhase::BED_MESH)});
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: starting a print hands its phase toggles to the "
                 "collector",
                 "[print_preparation][estimate]") {
    lv_init_safe();
    MoonrakerManager moonraker;
    MoonrakerManagerTestAccess::install_client(
        moonraker, std::make_unique<helix::test::FakeMoonrakerClient>());
    moonraker.init_print_start_collector();
    const auto collector = moonraker.print_start_collector();
    REQUIRE(collector);
    set_moonraker_manager(&moonraker);
    struct Uninstall {
        ~Uninstall() {
            set_moonraker_manager(nullptr);
        }
    } uninstall;
    REQUIRE_FALSE(PrintStartCollectorTestAccess::phase_overrides(*collector).has_value());

    MockPrinter mock_printer;
    PrintPreparationManager manager;
    manager.set_dependencies(&mock_printer.api, &mock_printer.state);
    // bed_mesh off, qgl on, the rest not shown.
    manager.set_option_state_provider([](const std::string& id) {
        return id == "bed_mesh" ? 0 : id == "qgl" ? 1 : -1;
    });

    // The file's own scan, so the start can act on the steps turned off.
    PrintPreparationManagerTestAccess::keep_scan(manager, "part.gcode", gcode::ScanResult{});

    manager.start_print("part.gcode", "", []() {}, [](bool, const std::string&) {});
    helix::ui::UpdateQueue::instance().drain();

    const auto told = PrintStartCollectorTestAccess::phase_overrides(*collector);
    REQUIRE(told.has_value());
    CHECK(*told ==
          helix::PreprintPhaseOverrides{{static_cast<int>(helix::PrintStartPhase::BED_MESH), false},
                                        {static_cast<int>(helix::PrintStartPhase::QGL), true}});
    manager.set_dependencies(nullptr, nullptr);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrintPreparationManager: a macro analysis reaches the collector's checklist "
                 "order when the file has no start block",
                 "[print_preparation][collector]") {
    lv_init_safe();
    MoonrakerManager moonraker;
    MoonrakerManagerTestAccess::install_client(
        moonraker, std::make_unique<helix::test::FakeMoonrakerClient>());
    moonraker.init_print_start_collector();
    const auto collector = moonraker.print_start_collector();
    REQUIRE(collector);
    set_moonraker_manager(&moonraker);
    struct Uninstall {
        ~Uninstall() {
            set_moonraker_manager(nullptr);
        }
    } uninstall;
    REQUIRE(collector->checklist_input().expected_order.empty());

    helix::PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    helix::PrintStartOperation mesh;
    mesh.name = "BED_MESH_CALIBRATE";
    mesh.category = helix::OperationCategory::BED_MESH;
    helix::PrintStartOperation purge;
    purge.name = "LINE_PURGE";
    purge.category = helix::OperationCategory::PURGE_LINE;
    analysis.operations = {mesh, purge};

    PrintPreparationManager manager;
    manager.set_macro_analysis(analysis);

    CHECK(collector->checklist_input().expected_order ==
          std::vector<helix::PrintStartPhase>{helix::PrintStartPhase::BED_MESH,
                                              helix::PrintStartPhase::PURGING});
}
