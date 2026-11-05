// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app_globals.h"
#include "gcode_file_modifier.h"
#include "gcode_ops_detector.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ============================================================================
// Basic Modification Tests
// ============================================================================

TEST_CASE("GCodeFileModifier - Comment out single line", "[gcode][modifier]") {
    GCodeFileModifier modifier;

    SECTION("Comments out a line with reason") {
        modifier.add_modification(Modification::comment_out(2, "Disabled by HelixScreen"));

        std::string content = "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; BED_MESH_CALIBRATE") != std::string::npos);
        REQUIRE(result.find("[HelixScreen: Disabled by HelixScreen]") != std::string::npos);
    }

    SECTION("Comments out without reason") {
        modifier.add_modification(Modification::comment_out(2));

        std::string content = "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; BED_MESH_CALIBRATE") != std::string::npos);
        REQUIRE(result.find("[HelixScreen:") == std::string::npos);
    }

    SECTION("Already commented lines are skipped") {
        modifier.add_modification(Modification::comment_out(1));

        std::string content = "; This is a comment\nG28\n";
        std::string result = modifier.apply_to_content(content);

        // Should not double-comment
        REQUIRE(result.find("; ; This is a comment") == std::string::npos);
    }
}

TEST_CASE("GCodeFileModifier - Comment out range", "[gcode][modifier]") {
    GCodeFileModifier modifier;

    SECTION("Comments out multiple lines") {
        modifier.add_modification(Modification::comment_out_range(2, 4, "Disabled section"));

        std::string content = "G28\nLINE1\nLINE2\nLINE3\nG1 X0\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; LINE1") != std::string::npos);
        REQUIRE(result.find("; LINE2") != std::string::npos);
        REQUIRE(result.find("; LINE3") != std::string::npos);
        REQUIRE(result.find("G28") != std::string::npos);
        REQUIRE(result.find("G1 X0") != std::string::npos);
    }
}

TEST_CASE("GCodeFileModifier - Inject G-code", "[gcode][modifier]") {
    GCodeFileModifier modifier;

    SECTION("Inject before a line") {
        modifier.add_modification(Modification::inject_before(2, "; Injected comment"));

        std::string content = "G28\nBED_MESH_CALIBRATE\n";
        std::string result = modifier.apply_to_content(content);

        // Find positions to verify order
        size_t injected_pos = result.find("; Injected comment");
        size_t mesh_pos = result.find("BED_MESH_CALIBRATE");

        REQUIRE(injected_pos != std::string::npos);
        REQUIRE(mesh_pos != std::string::npos);
        REQUIRE(injected_pos < mesh_pos);
    }

    SECTION("Inject after a line") {
        modifier.add_modification(Modification::inject_after(1, "; Injected after G28"));

        std::string content = "G28\nBED_MESH_CALIBRATE\n";
        std::string result = modifier.apply_to_content(content);

        size_t g28_pos = result.find("G28");
        size_t injected_pos = result.find("; Injected after G28");
        size_t mesh_pos = result.find("BED_MESH_CALIBRATE");

        REQUIRE(g28_pos < injected_pos);
        REQUIRE(injected_pos < mesh_pos);
    }

    SECTION("Inject multi-line G-code") {
        modifier.add_modification(Modification::inject_before(1, "; Line 1\n; Line 2\n; Line 3"));

        std::string content = "G28\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; Line 1") != std::string::npos);
        REQUIRE(result.find("; Line 2") != std::string::npos);
        REQUIRE(result.find("; Line 3") != std::string::npos);
    }
}

TEST_CASE("GCodeFileModifier - Replace line", "[gcode][modifier]") {
    GCodeFileModifier modifier;

    SECTION("Replace single line") {
        modifier.add_modification(Modification::replace(2, "; SKIPPED: original command"));

        std::string content = "G28\nBED_MESH_CALIBRATE\nG1 X0\n";
        std::string result = modifier.apply_to_content(content);

        // Original line should be gone, replacement should be present
        REQUIRE(result.find("; SKIPPED: original command") != std::string::npos);
        // Count occurrences of BED_MESH - should be 0
        REQUIRE(result.find("BED_MESH_CALIBRATE") == std::string::npos);
    }
}

// ============================================================================
// Multiple Modifications Tests
// ============================================================================

TEST_CASE("GCodeFileModifier - Multiple modifications", "[gcode][modifier]") {
    GCodeFileModifier modifier;

    SECTION("Multiple modifications applied correctly") {
        // Add in non-sequential order to test sorting
        modifier.add_modification(Modification::comment_out(4, "Disabled 4"));
        modifier.add_modification(Modification::comment_out(2, "Disabled 2"));

        std::string content = "LINE1\nLINE2\nLINE3\nLINE4\nLINE5\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; LINE2") != std::string::npos);
        REQUIRE(result.find("; LINE4") != std::string::npos);
        REQUIRE(result.find("LINE1\n") != std::string::npos); // Unchanged
        REQUIRE(result.find("LINE3\n") != std::string::npos); // Unchanged
        REQUIRE(result.find("LINE5") != std::string::npos);   // Unchanged
    }

    SECTION("Clear modifications") {
        modifier.add_modification(Modification::comment_out(1));
        modifier.clear_modifications();
        modifier.add_modification(Modification::comment_out(2));

        std::string content = "LINE1\nLINE2\n";
        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("LINE1\n") != std::string::npos); // Should be unchanged
        REQUIRE(result.find("; LINE2") != std::string::npos); // Should be commented
    }
}

// ============================================================================
// Integration with GCodeOpsDetector
// ============================================================================

TEST_CASE("GCodeFileModifier - disable_operation integration", "[gcode][modifier]") {
    GCodeOpsDetector detector;
    GCodeFileModifier modifier;

    SECTION("Disable direct command operation") {
        std::string content = "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
        auto scan = detector.scan_content(content);

        auto op = scan.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        REQUIRE(modifier.disable_operation(*op));

        std::string result = modifier.apply_to_content(content);
        REQUIRE(result.find("; BED_MESH_CALIBRATE") != std::string::npos);
    }

    SECTION("Disable macro call operation") {
        std::string content = "G28\nCLEAN_NOZZLE\nG1 X0 Y0\n";
        auto scan = detector.scan_content(content);

        auto op = scan.get_operation(OperationType::NOZZLE_CLEAN);
        REQUIRE(op.has_value());
        REQUIRE(modifier.disable_operation(*op));

        std::string result = modifier.apply_to_content(content);
        REQUIRE(result.find("; CLEAN_NOZZLE") != std::string::npos);
    }

    SECTION("Disable macro parameter operation") {
        std::string content = "START_PRINT EXTRUDER_TEMP=220 FORCE_LEVELING=true\nG1 X0\n";
        auto scan = detector.scan_content(content);

        auto op = scan.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        REQUIRE(op->embedding == OperationEmbedding::MACRO_PARAMETER);
        REQUIRE(modifier.disable_operation(*op));

        std::string result = modifier.apply_to_content(content);
        // Should replace FORCE_LEVELING=true with FORCE_LEVELING=FALSE
        REQUIRE(result.find("FORCE_LEVELING=FALSE") != std::string::npos);
        REQUIRE(result.find("FORCE_LEVELING=true") == std::string::npos);
    }

    SECTION("Disable numeric macro parameter") {
        std::string content = "START_PRINT FORCE_LEVELING=1\n";
        auto scan = detector.scan_content(content);

        auto op = scan.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        REQUIRE(modifier.disable_operation(*op));

        std::string result = modifier.apply_to_content(content);
        // Numeric values become 0
        REQUIRE(result.find("FORCE_LEVELING=0") != std::string::npos);
    }
}

TEST_CASE("GCodeFileModifier - disable_operations batch", "[gcode][modifier]") {
    GCodeOpsDetector detector;
    GCodeFileModifier modifier;

    SECTION("Disable multiple operation types") {
        std::string content = "G28\nQUAD_GANTRY_LEVEL\nBED_MESH_CALIBRATE\nCLEAN_NOZZLE\n";
        auto scan = detector.scan_content(content);

        modifier.disable_operations(scan, {OperationType::QGL, OperationType::NOZZLE_CLEAN});

        std::string result = modifier.apply_to_content(content);

        REQUIRE(result.find("; QUAD_GANTRY_LEVEL") != std::string::npos);
        REQUIRE(result.find("; CLEAN_NOZZLE") != std::string::npos);
        // BED_MESH_CALIBRATE should remain unchanged
        REQUIRE(result.find("BED_MESH_CALIBRATE\n") != std::string::npos);
    }
}

// ============================================================================
// File I/O Tests
// ============================================================================

TEST_CASE("GCodeFileModifier - File operations", "[gcode][modifier][file]") {
    GCodeFileModifier modifier;

    SECTION("Generate temp path") {
        std::string path = GCodeFileModifier::generate_temp_path("/path/to/3DBenchy.gcode");

        // Anchor on what the cascade itself resolves to. This used to assert the
        // literal "/tmp/helix_test_gcode_mod/", a shape only the ui_test_utils
        // stub of get_helix_cache_dir() ever produced — the real resolver never
        // emits it, so the assertion was pinned to the fake rather than to the
        // behavior under test.
        REQUIRE(path.find(get_helix_cache_dir("gcode_mod") + "/mod_") != std::string::npos);
        REQUIRE(path.find("3DBenchy.gcode") != std::string::npos);
    }

    SECTION("Apply to non-existent file returns error") {
        auto result = modifier.apply("/nonexistent/path/file.gcode");

        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.error_message.empty());
    }

    SECTION("Apply to real temp file") {
        // Create temp input file
        std::string input_path = "/tmp/test_modifier_input_" + std::to_string(getpid()) + ".gcode";
        {
            std::ofstream out(input_path);
            out << "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
        }

        modifier.add_modification(Modification::comment_out(2, "Test"));
        auto result = modifier.apply(input_path);

        REQUIRE(result.success);
        REQUIRE_FALSE(result.modified_path.empty());
        REQUIRE(result.lines_modified == 1);

        // Verify output file content
        std::ifstream in(result.modified_path);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(content.find("; BED_MESH_CALIBRATE") != std::string::npos);

        // Cleanup
        std::remove(input_path.c_str());
        std::remove(result.modified_path.c_str());
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("GCodeFileModifier - Edge cases", "[gcode][modifier][edge]") {
    GCodeFileModifier modifier;

    SECTION("Empty content returns empty") {
        std::string result = modifier.apply_to_content("");
        REQUIRE(result.empty());
    }

    SECTION("No modifications returns original") {
        std::string content = "G28\nBED_MESH_CALIBRATE\n";
        std::string result = modifier.apply_to_content(content);
        REQUIRE(result == content);
    }

    SECTION("Out of range line number is handled") {
        modifier.add_modification(Modification::comment_out(100));

        std::string content = "G28\nG1 X0\n";
        std::string result = modifier.apply_to_content(content);

        // Should not crash, content essentially unchanged (may differ in trailing newline)
        REQUIRE(result.find("G28") != std::string::npos);
        REQUIRE(result.find("G1 X0") != std::string::npos);
    }

    SECTION("Line number 0 is handled") {
        modifier.add_modification(Modification::comment_out(0));

        std::string content = "G28\n";
        // Line 0 is invalid (1-indexed), will be handled as out of range
        REQUIRE_NOTHROW(modifier.apply_to_content(content));
    }

    SECTION("Preserves line endings") {
        modifier.add_modification(Modification::comment_out(2));

        std::string content = "G28\nBED_MESH\nG1\n";
        std::string result = modifier.apply_to_content(content);

        // Should have proper newlines
        size_t newline_count = std::count(result.begin(), result.end(), '\n');
        REQUIRE(newline_count == 2); // Two newlines for three lines
    }
}

// ============================================================================
// Real-world Scenarios
// ============================================================================

TEST_CASE("GCodeFileModifier - Real-world scenarios", "[gcode][modifier]") {
    GCodeOpsDetector detector;
    GCodeFileModifier modifier;

    SECTION("Disable bed leveling in Voron start sequence") {
        std::string content = R"(; Voron start sequence
G28 ; home
QUAD_GANTRY_LEVEL
BED_MESH_CALIBRATE
CLEAN_NOZZLE
G1 X10 Y10 Z0.3 E0.5 ; prime
)";

        auto scan = detector.scan_content(content);

        // User unchecked bed leveling and QGL
        modifier.disable_operations(scan, {OperationType::BED_MESH, OperationType::QGL});

        std::string result = modifier.apply_to_content(content);

        // BED_MESH and QGL should be commented out
        REQUIRE(result.find("; QUAD_GANTRY_LEVEL") != std::string::npos);
        REQUIRE(result.find("; BED_MESH_CALIBRATE") != std::string::npos);

        // CLEAN_NOZZLE should remain active
        REQUIRE(result.find("CLEAN_NOZZLE\n") != std::string::npos);

        // G28 should remain (homing is usually required)
        REQUIRE(result.find("G28 ; home\n") != std::string::npos);
    }

    SECTION("Disable parameter in START_PRINT macro") {
        std::string content = R"(; PrusaSlicer output
START_PRINT EXTRUDER_TEMP=220 BED_TEMP=60 FORCE_LEVELING=true NOZZLE_CLEAN=1
G1 X10 Y10 Z0.2 E0.5
)";

        auto scan = detector.scan_content(content);

        // User unchecked bed leveling
        if (auto op = scan.get_operation(OperationType::BED_MESH)) {
            modifier.disable_operation(*op);
        }

        std::string result = modifier.apply_to_content(content);

        // FORCE_LEVELING should be disabled
        REQUIRE(result.find("FORCE_LEVELING=FALSE") != std::string::npos);

        // Other parameters should remain
        REQUIRE(result.find("EXTRUDER_TEMP=220") != std::string::npos);
        REQUIRE(result.find("BED_TEMP=60") != std::string::npos);
        REQUIRE(result.find("NOZZLE_CLEAN=1") != std::string::npos);
    }
}

// ============================================================================
// Streaming Mode Tests
// ============================================================================

TEST_CASE("GCodeFileModifier - Streaming comment out", "[gcode][modifier][streaming]") {
    GCodeFileModifier modifier;

    // Create a test file
    std::string test_path = "/tmp/helix_stream_test_comment_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(test_path);
        out << "G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
    }

    modifier.add_modification(Modification::comment_out(2, "Disabled"));

    // Force streaming mode
    auto result = modifier.apply_streaming(test_path);

    REQUIRE(result.success);
    REQUIRE_FALSE(result.modified_path.empty());

    // Read and verify modified content
    std::ifstream in(result.modified_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE(content.find("; BED_MESH_CALIBRATE") != std::string::npos);
    REQUIRE(content.find("G28") != std::string::npos);
    REQUIRE(content.find("G1 X0 Y0") != std::string::npos);

    // Cleanup
    std::filesystem::remove(test_path);
    std::filesystem::remove(result.modified_path);
}

TEST_CASE("GCodeFileModifier - Streaming delete line", "[gcode][modifier][streaming]") {
    GCodeFileModifier modifier;

    std::string test_path = "/tmp/helix_stream_test_delete_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(test_path);
        out << "LINE1\nLINE2\nLINE3\nLINE4\n";
    }

    // Delete line 2
    modifier.add_modification({ModificationType::DELETE, 2, 0, "", "Deleted"});

    auto result = modifier.apply_streaming(test_path);

    REQUIRE(result.success);
    REQUIRE(result.lines_removed == 1);

    // Read and verify
    std::ifstream in(result.modified_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE(content.find("LINE1") != std::string::npos);
    REQUIRE(content.find("LINE2") == std::string::npos); // Deleted
    REQUIRE(content.find("LINE3") != std::string::npos);
    REQUIRE(content.find("LINE4") != std::string::npos);

    // Cleanup
    std::filesystem::remove(test_path);
    std::filesystem::remove(result.modified_path);
}

TEST_CASE("GCodeFileModifier - Streaming inject before", "[gcode][modifier][streaming]") {
    GCodeFileModifier modifier;

    std::string test_path = "/tmp/helix_stream_test_inject_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(test_path);
        out << "LINE1\nLINE2\nLINE3\n";
    }

    modifier.add_modification(Modification::inject_before(2, "; INJECTED"));

    auto result = modifier.apply_streaming(test_path);

    REQUIRE(result.success);
    REQUIRE(result.lines_added == 1);

    std::ifstream in(result.modified_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Verify order: LINE1 -> ; INJECTED -> LINE2 -> LINE3
    size_t line1_pos = content.find("LINE1");
    size_t inject_pos = content.find("; INJECTED");
    size_t line2_pos = content.find("LINE2");

    REQUIRE(line1_pos < inject_pos);
    REQUIRE(inject_pos < line2_pos);

    // Cleanup
    std::filesystem::remove(test_path);
    std::filesystem::remove(result.modified_path);
}

TEST_CASE("GCodeFileModifier - Streaming replace line", "[gcode][modifier][streaming]") {
    GCodeFileModifier modifier;

    std::string test_path = "/tmp/helix_stream_test_replace_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(test_path);
        out << "OLD_LINE1\nOLD_LINE2\nOLD_LINE3\n";
    }

    modifier.add_modification(Modification::replace(2, "NEW_LINE2", "Replaced"));

    auto result = modifier.apply_streaming(test_path);

    REQUIRE(result.success);
    REQUIRE(result.lines_modified == 1);

    std::ifstream in(result.modified_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE(content.find("OLD_LINE1") != std::string::npos);
    REQUIRE(content.find("OLD_LINE2") == std::string::npos); // Replaced
    REQUIRE(content.find("NEW_LINE2") != std::string::npos);
    REQUIRE(content.find("OLD_LINE3") != std::string::npos);

    // Cleanup
    std::filesystem::remove(test_path);
    std::filesystem::remove(result.modified_path);
}

TEST_CASE("GCodeFileModifier - Streaming replace on the last line",
          "[gcode][modifier][streaming]") {
    // Streaming emits the separating newline BEFORE each line rather than after
    // it, so the final line is the one place that bookkeeping can drop a
    // separator or emit a spare one. The output ends with a newline exactly
    // when the source does: Klipper never runs an unterminated final line.
    auto run = [](const std::string& source, const std::string& replacement) {
        GCodeFileModifier modifier;
        std::string test_path =
            "/tmp/helix_stream_test_last_" + std::to_string(getpid()) + ".gcode";
        {
            std::ofstream out(test_path);
            out << source;
        }

        modifier.add_modification(Modification::replace(3, replacement, "Replaced"));
        auto result = modifier.apply_streaming(test_path);

        std::string content;
        {
            std::ifstream in(result.modified_path);
            content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        std::filesystem::remove(test_path);
        std::filesystem::remove(result.modified_path);
        return std::make_tuple(result.success, result.lines_modified, content);
    };

    SECTION("source ends with a newline") {
        auto [ok, modified, content] = run("L1\nL2\nOLD_LAST\n", "NEW_LAST");
        REQUIRE(ok);
        REQUIRE(modified == 1);
        REQUIRE(content == "L1\nL2\nNEW_LAST\n");
    }

    SECTION("source has no trailing newline") {
        auto [ok, modified, content] = run("L1\nL2\nOLD_LAST", "NEW_LAST");
        REQUIRE(ok);
        REQUIRE(modified == 1);
        REQUIRE(content == "L1\nL2\nNEW_LAST");
    }

    SECTION("multi-line replacement of the last line") {
        auto [ok, modified, content] = run("L1\nL2\nOLD_LAST\n", "NEW_A\nNEW_B");
        REQUIRE(ok);
        REQUIRE(modified == 1);
        REQUIRE(content == "L1\nL2\nNEW_A\nNEW_B\n");
    }
}

TEST_CASE("GCodeFileModifier - Streaming with no modifications", "[gcode][modifier][streaming]") {
    // apply_buffered() has an explicit empty-set branch; streaming relies on
    // every line falling through its lookup miss. Both must produce the source
    // back, at a temp path, with nothing counted as changed.
    GCodeFileModifier modifier;

    std::string test_path = "/tmp/helix_stream_test_empty_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(test_path);
        out << "G28\nG1 X0 Y0\nM104 S200\n";
    }

    auto result = modifier.apply_streaming(test_path);

    REQUIRE(result.success);
    REQUIRE(result.error_message.empty());
    REQUIRE(result.lines_modified == 0);
    REQUIRE(result.lines_added == 0);
    REQUIRE(result.lines_removed == 0);
    REQUIRE(result.modified_path != test_path);

    std::ifstream in(result.modified_path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == "G28\nG1 X0 Y0\nM104 S200\n");

    std::filesystem::remove(test_path);
    std::filesystem::remove(result.modified_path);
}

TEST_CASE("GCodeFileModifier - Buffered copy with no modifications keeps the final newline",
          "[gcode][modifier]") {
    // A small file takes the buffered path, whose empty-set branch copies the
    // source line by line. The copy ends with a newline exactly when the source
    // does: Klipper never runs an unterminated final line.
    auto copy_of = [](const std::string& source) {
        GCodeFileModifier modifier;
        std::string test_path =
            "/tmp/helix_buffered_test_empty_" + std::to_string(getpid()) + ".gcode";
        {
            std::ofstream out(test_path);
            out << source;
        }

        auto result = modifier.apply(test_path);
        REQUIRE(result.success);
        REQUIRE(result.modified_path != test_path);

        std::string content;
        {
            std::ifstream in(result.modified_path);
            content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        std::filesystem::remove(test_path);
        std::filesystem::remove(result.modified_path);
        return content;
    };

    SECTION("source ends with a newline") {
        REQUIRE(copy_of("G28\nG1 X0 Y0\nM104 S200\n") == "G28\nG1 X0 Y0\nM104 S200\n");
    }

    SECTION("source has no trailing newline") {
        REQUIRE(copy_of("G28\nG1 X0 Y0\nM104 S200") == "G28\nG1 X0 Y0\nM104 S200");
    }
}

TEST_CASE("GCodeFileModifier - Auto-select streaming for large files",
          "[gcode][modifier][streaming]") {
    // This test verifies that apply() selects the appropriate mode based on file size
    // We can't easily create a 5MB test file, so we test the logic path

    GCodeFileModifier modifier;

    // Create a small file (should use buffered mode)
    std::string small_path = "/tmp/helix_small_test_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(small_path);
        out << "G28\nG1 X0\n";
    }

    // apply() should succeed and use buffered mode (we can verify by log if needed)
    auto result = modifier.apply(small_path);
    REQUIRE(result.success);

    // Cleanup
    std::filesystem::remove(small_path);
    if (!result.modified_path.empty()) {
        std::filesystem::remove(result.modified_path);
    }
}

// ============================================================================
// PrintStartCallInfo Tests (Phase 5C)
// ============================================================================

TEST_CASE("PrintStartCallInfo - with_skip_params", "[gcode][detector][print_start]") {
    SECTION("Basic skip param addition") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START EXTRUDER=210 BED=60";
        info.line_number = 5;

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_BED_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "PRINT_START EXTRUDER=210 BED=60 SKIP_BED_MESH=1");
    }

    SECTION("Multiple skip params") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START EXTRUDER=210";
        info.line_number = 3;

        std::vector<std::pair<std::string, std::string>> skip_params = {
            {"SKIP_BED_MESH", "1"}, {"SKIP_QGL", "1"}, {"SKIP_CLEAN", "1"}};

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "PRINT_START EXTRUDER=210 SKIP_BED_MESH=1 SKIP_QGL=1 SKIP_CLEAN=1");
    }

    SECTION("Handles trailing whitespace in original line") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START EXTRUDER=210  \n";
        info.line_number = 1;

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "PRINT_START EXTRUDER=210 SKIP_MESH=1");
    }

    SECTION("Returns original line if not found") {
        PrintStartCallInfo info;
        info.found = false;
        info.raw_line = "G28";

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "G28");
    }

    SECTION("Returns original line if no skip params") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START EXTRUDER=210";

        std::vector<std::pair<std::string, std::string>> skip_params;

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "PRINT_START EXTRUDER=210");
    }

    SECTION("Works with START_PRINT variant") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "START_PRINT";
        info.raw_line = "START_PRINT TEMP=200";
        info.line_number = 2;

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_BED_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        REQUIRE(result == "START_PRINT TEMP=200 SKIP_BED_MESH=1");
    }

    SECTION("Handles empty raw_line") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "";

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        // Empty line should produce skip params only (with leading space)
        REQUIRE(result == " SKIP_MESH=1");
    }

    SECTION("Handles raw_line with only whitespace") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "   \n";

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_MESH", "1"}};

        std::string result = info.with_skip_params(skip_params);
        // Whitespace should be trimmed, resulting in skip params only
        REQUIRE(result == " SKIP_MESH=1");
    }

    SECTION("Validates param names - rejects invalid characters") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START";

        // These should be rejected (spaces, special chars)
        std::vector<std::pair<std::string, std::string>> bad_params = {
            {"SKIP MESH", "1"},  // space
            {"SKIP;MESH", "1"},  // semicolon (G-code comment)
            {"SKIP\nMESH", "1"}, // newline
            {"", "1"}            // empty
        };

        std::string result = info.with_skip_params(bad_params);
        // Should reject all invalid params, returning original line
        REQUIRE(result == "PRINT_START");
    }

    SECTION("Validates param names - accepts valid characters") {
        PrintStartCallInfo info;
        info.found = true;
        info.macro_name = "PRINT_START";
        info.raw_line = "PRINT_START";

        // These should all be accepted
        std::vector<std::pair<std::string, std::string>> good_params = {
            {"SKIP_BED_MESH", "1"}, {"SKIP123", "1"}, {"skip_lower", "0"} // lowercase is valid
        };

        std::string result = info.with_skip_params(good_params);
        REQUIRE(result == "PRINT_START SKIP_BED_MESH=1 SKIP123=1 skip_lower=0");
    }
}

// ============================================================================
// GCodeFileModifier - PRINT_START Skip Params (Phase 5C)
// ============================================================================

TEST_CASE("GCodeFileModifier - add_print_start_skip_params", "[gcode][modifier][print_start]") {
    GCodeOpsDetector detector;
    GCodeFileModifier modifier;

    SECTION("Adds skip params to PRINT_START call") {
        std::string content = "G28\nPRINT_START EXTRUDER=210 BED=60\nG1 X0 Y0\n";
        auto scan = detector.scan_content(content);

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_BED_MESH", "1"}};

        bool added = modifier.add_print_start_skip_params(scan, skip_params);
        REQUIRE(added);

        std::string result = modifier.apply_to_content(content);
        REQUIRE(result.find("PRINT_START EXTRUDER=210 BED=60 SKIP_BED_MESH=1") !=
                std::string::npos);
    }

    SECTION("Adds multiple skip params") {
        std::string content = "; Start\nPRINT_START BED=55\nG1 X10\n";
        auto scan = detector.scan_content(content);

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_MESH", "1"},
                                                                        {"SKIP_QGL", "1"}};

        bool added = modifier.add_print_start_skip_params(scan, skip_params);
        REQUIRE(added);

        std::string result = modifier.apply_to_content(content);
        REQUIRE(result.find("SKIP_MESH=1") != std::string::npos);
        REQUIRE(result.find("SKIP_QGL=1") != std::string::npos);
    }

    SECTION("Returns false when PRINT_START not found") {
        std::string content = "G28\nG1 X0 Y0\nM104 S200\n";
        auto scan = detector.scan_content(content);

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_BED_MESH", "1"}};

        bool added = modifier.add_print_start_skip_params(scan, skip_params);
        REQUIRE_FALSE(added);
    }

    SECTION("Returns false with empty skip params") {
        std::string content = "PRINT_START EXTRUDER=200\n";
        auto scan = detector.scan_content(content);

        std::vector<std::pair<std::string, std::string>> skip_params;

        bool added = modifier.add_print_start_skip_params(scan, skip_params);
        REQUIRE_FALSE(added);
    }

    SECTION("Works with START_PRINT variant") {
        std::string content = "G28\nSTART_PRINT TEMP=215\nG1 Z0.2\n";
        auto scan = detector.scan_content(content);

        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_CLEAN", "1"}};

        bool added = modifier.add_print_start_skip_params(scan, skip_params);
        REQUIRE(added);

        std::string result = modifier.apply_to_content(content);
        REQUIRE(result.find("START_PRINT TEMP=215 SKIP_CLEAN=1") != std::string::npos);
    }
}

TEST_CASE("GCodeFileModifier - Combined file ops and skip params",
          "[gcode][modifier][print_start]") {
    GCodeOpsDetector detector;
    GCodeFileModifier modifier;

    SECTION("Disable file operations AND add skip params") {
        std::string content = R"(G28
PRINT_START EXTRUDER=210 BED=60
BED_MESH_CALIBRATE
QUAD_GANTRY_LEVEL
G1 X0 Y0
)";

        auto scan = detector.scan_content(content);

        // Disable file-embedded operations
        modifier.disable_operations(scan, {OperationType::QGL});

        // Add skip params for macro operations
        std::vector<std::pair<std::string, std::string>> skip_params = {{"SKIP_BED_MESH", "1"}};
        modifier.add_print_start_skip_params(scan, skip_params);

        std::string result = modifier.apply_to_content(content);

        // QGL should be commented out (file operation)
        REQUIRE(result.find("; QUAD_GANTRY_LEVEL") != std::string::npos);

        // PRINT_START should have skip param added (macro operation)
        REQUIRE(result.find("SKIP_BED_MESH=1") != std::string::npos);

        // BED_MESH_CALIBRATE should remain (not disabled)
        REQUIRE(result.find("BED_MESH_CALIBRATE\n") != std::string::npos);
    }

    SECTION("Voron-style print with mixed operations") {
        std::string content = R"(; PrusaSlicer G-code for Voron 2.4
PRINT_START EXTRUDER=240 BED=110
; Start sequence handled by macro
G1 X10 Y10 F3000
)";

        auto scan = detector.scan_content(content);

        // Add multiple skip params
        std::vector<std::pair<std::string, std::string>> skip_params = {
            {"SKIP_BED_MESH", "1"}, {"SKIP_QGL", "1"}, {"SKIP_CLEAN", "1"}};
        modifier.add_print_start_skip_params(scan, skip_params);

        std::string result = modifier.apply_to_content(content);

        // All skip params should be present
        REQUIRE(result.find("PRINT_START EXTRUDER=240 BED=110 SKIP_BED_MESH=1 SKIP_QGL=1 "
                            "SKIP_CLEAN=1") != std::string::npos);

        // Original structure preserved
        REQUIRE(result.find("G1 X10 Y10 F3000") != std::string::npos);
    }
}

// ============================================================================
// A scan of other G-code
// ============================================================================

TEST_CASE("GCodeFileModifier - a line that is not the scanned one is refused",
          "[gcode][modifier][stale_scan]") {
    const std::string path = "/tmp/helix_stale_scan_" + std::to_string(getpid()) + ".gcode";
    {
        std::ofstream out(path);
        out << "G28\nPRINT_START BED=60\nBED_MESH_CALIBRATE\nG1 X0 Y0\n";
    }

    auto rewrite = [&](bool streaming, Modification mod) {
        GCodeFileModifier modifier;
        modifier.add_modification(std::move(mod));
        return streaming ? modifier.apply_streaming(path) : modifier.apply(path);
    };

    for (const bool streaming : {false, true}) {
        CAPTURE(streaming);

        SECTION(std::string("comment out, ") + (streaming ? "streaming" : "buffered")) {
            const auto stale =
                rewrite(streaming, Modification::comment_out(3).expecting("CLEAN_NOZZLE"));
            REQUIRE_FALSE(stale.success);
            REQUIRE(stale.error_message.find("line 3") != std::string::npos);
            REQUIRE((stale.modified_path.empty() || !std::filesystem::exists(stale.modified_path)));

            const auto fresh =
                rewrite(streaming, Modification::comment_out(3).expecting("BED_MESH_CALIBRATE\r"));
            REQUIRE(fresh.success);
            std::ifstream in(fresh.modified_path);
            const std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            REQUIRE(content.find("; BED_MESH_CALIBRATE") != std::string::npos);
            std::filesystem::remove(fresh.modified_path);
        }

        SECTION(std::string("replace, ") + (streaming ? "streaming" : "buffered")) {
            const auto stale =
                rewrite(streaming, Modification::replace(2, "PRINT_START BED=60 SKIP_MESH=1")
                                       .expecting("PRINT_START BED=110"));
            REQUIRE_FALSE(stale.success);
            REQUIRE((stale.modified_path.empty() || !std::filesystem::exists(stale.modified_path)));
        }
    }

    SECTION("disable_operation checks the operation's own line") {
        DetectedOperation op;
        op.type = OperationType::BED_MESH;
        op.embedding = OperationEmbedding::DIRECT_COMMAND;
        op.line_number = 3;
        op.raw_line = "BED_MESH_CALIBRATE ADAPTIVE=1";
        GCodeFileModifier modifier;
        REQUIRE(modifier.disable_operation(op));
        REQUIRE_FALSE(modifier.apply(path).success);
    }

    SECTION("a macro parameter is switched off only on the scanned call") {
        // A scan of other G-code whose START_PRINT, on this file's line 2,
        // asks for leveling.
        const auto scan = GCodeOpsDetector{}.scan_content(
            "G28\nSTART_PRINT BED=110 FORCE_LEVELING=true\nG1 X0 Y0\n");
        const auto op = scan.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        REQUIRE(op->embedding == OperationEmbedding::MACRO_PARAMETER);
        REQUIRE(op->line_number == 2);
        GCodeFileModifier modifier;
        REQUIRE(modifier.disable_operation(*op));
        REQUIRE_FALSE(modifier.apply(path).success);
    }

    SECTION("the START_PRINT rewrite checks the scanned call") {
        // A scan of other G-code whose START_PRINT sits on this file's line 2.
        const auto scan = GCodeOpsDetector{}.scan_content("G28\nPRINT_START BED=110\nG1 X0 Y0\n");
        REQUIRE(scan.print_start.line_number == 2);
        GCodeFileModifier modifier;
        REQUIRE(modifier.add_print_start_skip_params(scan, {{"SKIP_BED_MESH", "1"}}));
        REQUIRE_FALSE(modifier.apply(path).success);
    }

    std::filesystem::remove(path);
}
