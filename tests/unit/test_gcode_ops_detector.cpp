// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_ops_detector.h"

#include <sstream>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::gcode;

// ============================================================================
// Basic Detection Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - Direct command detection", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Detects BED_MESH_CALIBRATE") {
        auto result = detector.scan_content("G28\nBED_MESH_CALIBRATE\nG1 X0 Y0 Z0.2 E0.5\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
        auto op = result.get_operation(OperationType::BED_MESH);
        REQUIRE(op.has_value());
        REQUIRE(op->embedding == OperationEmbedding::DIRECT_COMMAND);
        REQUIRE(op->line_number == 2);
    }

    SECTION("Detects G29") {
        auto result = detector.scan_content("G28\nG29\nG1 X0 Y0 Z0.2 E0.5\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Detects QUAD_GANTRY_LEVEL") {
        auto result = detector.scan_content("G28\nQUAD_GANTRY_LEVEL\nBED_MESH_CALIBRATE\n");

        REQUIRE(result.has_operation(OperationType::QGL));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Detects Z_TILT_ADJUST") {
        auto result = detector.scan_content("G28\nZ_TILT_ADJUST\n");

        REQUIRE(result.has_operation(OperationType::Z_TILT));
    }

    SECTION("Detects G28 homing") {
        auto result = detector.scan_content("G28\n");

        REQUIRE(result.has_operation(OperationType::HOMING));
    }
}

TEST_CASE("GCodeOpsDetector - Macro call detection", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Detects CLEAN_NOZZLE") {
        auto result = detector.scan_content("G28\nCLEAN_NOZZLE\n");

        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
        auto op = result.get_operation(OperationType::NOZZLE_CLEAN);
        REQUIRE(op->embedding == OperationEmbedding::MACRO_CALL);
    }

    SECTION("Detects NOZZLE_WIPE variant") {
        auto result = detector.scan_content("G28\nNOZZLE_WIPE\n");

        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
    }

    SECTION("Detects HEAT_SOAK") {
        auto result = detector.scan_content("G28\nHEAT_SOAK TEMP=50 DURATION=10\n");

        REQUIRE(result.has_operation(OperationType::CHAMBER_SOAK));
    }

    SECTION("Detects PURGE_LINE") {
        auto result = detector.scan_content("G28\nPURGE_LINE\n");

        REQUIRE(result.has_operation(OperationType::PURGE_LINE));
    }
}

// ============================================================================
// START_PRINT Parameter Detection Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - START_PRINT parameter detection", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Detects FORCE_LEVELING=true") {
        auto result = detector.scan_content(
            "START_PRINT EXTRUDER_TEMP=220 BED_TEMP=60 FORCE_LEVELING=true\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
        auto op = result.get_operation(OperationType::BED_MESH);
        REQUIRE(op->embedding == OperationEmbedding::MACRO_PARAMETER);
        REQUIRE(op->macro_name == "START_PRINT");
        REQUIRE(op->param_name == "FORCE_LEVELING");
        REQUIRE(op->param_value == "true");
    }

    SECTION("Detects FORCE_LEVELING=1") {
        auto result = detector.scan_content("START_PRINT FORCE_LEVELING=1\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Ignores FORCE_LEVELING=false") {
        auto result = detector.scan_content("START_PRINT FORCE_LEVELING=false\n");

        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Ignores FORCE_LEVELING=0") {
        auto result = detector.scan_content("START_PRINT FORCE_LEVELING=0\n");

        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Detects multiple parameters") {
        auto result = detector.scan_content(
            "START_PRINT FORCE_LEVELING=1 NOZZLE_CLEAN=true QGL=1 CHAMBER_SOAK=5\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
        REQUIRE(result.has_operation(OperationType::QGL));
        REQUIRE(result.has_operation(OperationType::CHAMBER_SOAK));
    }

    SECTION("Case insensitive parameter detection") {
        auto result = detector.scan_content("start_print force_leveling=TRUE\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }
}

TEST_CASE("GCodeOpsDetector - START_PRINT temperature extraction", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Extracts EXTRUDER_TEMP and BED_TEMP (Creality convention)") {
        auto result =
            detector.scan_content("M104 S0\nSTART_PRINT EXTRUDER_TEMP=260 BED_TEMP=105\nG1 X0\n");

        REQUIRE(result.print_start.found);
        REQUIRE(result.print_start.macro_name == "START_PRINT");
        REQUIRE(result.print_start.extruder_temp == 260);
        REQUIRE(result.print_start.bed_temp == 105);
    }

    SECTION("Extracts EXTRUDER and BED aliases (Voron convention)") {
        auto result = detector.scan_content("PRINT_START EXTRUDER=210 BED=60\n");

        REQUIRE(result.print_start.found);
        REQUIRE(result.print_start.extruder_temp == 210);
        REQUIRE(result.print_start.bed_temp == 60);
    }

    SECTION("Temp-less call reports zero (unknown)") {
        auto result = detector.scan_content("START_PRINT FORCE_LEVELING=1\n");

        REQUIRE(result.print_start.found);
        REQUIRE(result.print_start.extruder_temp == 0);
        REQUIRE(result.print_start.bed_temp == 0);
    }

    SECTION("Parses float values as integers") {
        auto result = detector.scan_content("START_PRINT EXTRUDER_TEMP=220.0 BED_TEMP=60.5\n");

        REQUIRE(result.print_start.extruder_temp == 220);
        REQUIRE(result.print_start.bed_temp == 60);
    }

    SECTION("Case insensitive temperature keys") {
        auto result = detector.scan_content("start_print extruder_temp=215 bed_temp=70\n");

        REQUIRE(result.print_start.extruder_temp == 215);
        REQUIRE(result.print_start.bed_temp == 70);
    }

    SECTION("BED_TEMP is not confused with BED alias and vice versa") {
        auto both = detector.scan_content("START_PRINT BED_TEMP=100 BED=60\n");
        REQUIRE(both.print_start.bed_temp == 100);

        auto alias_only = detector.scan_content("START_PRINT BED=60\n");
        REQUIRE(alias_only.print_start.bed_temp == 60);
    }
}

// ============================================================================
// Scanning Limit Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - Scanning limits", "[gcode][ops]") {
    DetectionConfig config;
    config.max_scan_lines = 10;
    config.stop_at_first_extrusion = true;
    GCodeOpsDetector detector(config);

    SECTION("Stops at first extrusion") {
        std::string content = "G28\n"
                              "G1 X10 Y10 Z0.2 E0.5\n" // First extrusion - should stop here
                              "BED_MESH_CALIBRATE\n";  // Should not be detected

        auto result = detector.scan_content(content);

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Stops at layer marker") {
        DetectionConfig cfg;
        cfg.stop_at_layer_marker = true;
        GCodeOpsDetector det(cfg);

        std::string content = "G28\n"
                              ";LAYER_CHANGE\n"
                              "BED_MESH_CALIBRATE\n";

        auto result = det.scan_content(content);

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Respects line limit") {
        std::string content;
        for (int i = 0; i < 20; i++) {
            content += "; comment line\n";
        }
        content += "BED_MESH_CALIBRATE\n"; // After limit - should not be detected

        auto result = detector.scan_content(content);

        REQUIRE(result.reached_limit);
        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("GCodeOpsDetector - Edge cases", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Ignores comments") {
        auto result = detector.scan_content("; BED_MESH_CALIBRATE\n");

        REQUIRE_FALSE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Handles empty content") {
        auto result = detector.scan_content("");

        REQUIRE(result.operations.empty());
        REQUIRE(result.lines_scanned == 0);
    }

    SECTION("Handles whitespace-only content") {
        auto result = detector.scan_content("   \n\t\n  \n");

        REQUIRE(result.operations.empty());
    }

    SECTION("Detects command with leading whitespace") {
        auto result = detector.scan_content("   BED_MESH_CALIBRATE\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Avoids duplicate detection") {
        // Same operation type shouldn't be detected twice
        auto result = detector.scan_content("BED_MESH_CALIBRATE\nG29\n");

        auto ops = result.get_operations(OperationType::BED_MESH);
        REQUIRE(ops.size() == 1);
        REQUIRE(ops[0].macro_name == "BED_MESH_CALIBRATE"); // First one wins
    }

    SECTION("Detects BED_MESH_PROFILE LOAD") {
        auto result = detector.scan_content("BED_MESH_PROFILE LOAD=default\n");

        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }
}

// ============================================================================
// Display Name Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - Display names", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Operation display names are user-friendly") {
        auto result =
            detector.scan_content("G28\nQUAD_GANTRY_LEVEL\nBED_MESH_CALIBRATE\nCLEAN_NOZZLE\n");

        for (const auto& op : result.operations) {
            INFO("Operation: " << op.display_name());
            REQUIRE_FALSE(op.display_name().empty());
            REQUIRE(op.display_name().find("_") == std::string::npos); // No underscores
        }
    }

    SECTION("Static operation_type_name returns valid strings") {
        REQUIRE(GCodeOpsDetector::operation_type_name(OperationType::BED_MESH) == "bed_mesh");
        REQUIRE(GCodeOpsDetector::operation_type_name(OperationType::QGL) == "qgl");
        REQUIRE(GCodeOpsDetector::operation_type_name(OperationType::NOZZLE_CLEAN) ==
                "nozzle_clean");
    }
}

// ============================================================================
// Custom Pattern Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - Custom patterns", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("Add custom pattern") {
        detector.add_pattern({OperationType::NOZZLE_CLEAN, "MY_CUSTOM_CLEAN",
                              OperationEmbedding::MACRO_CALL, false});

        auto result = detector.scan_content("MY_CUSTOM_CLEAN\n");

        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
    }
}

// ============================================================================
// Real-world G-code Snippet Tests
// ============================================================================

TEST_CASE("GCodeOpsDetector - Robustness edge cases", "[gcode][ops][edge]") {
    GCodeOpsDetector detector;

    SECTION("Handles binary content gracefully") {
        // Simulate corrupted/binary content that might occur in a file
        std::string binary_content = "G28\n";
        binary_content += std::string("\x00\x01\x02\x03\xFF\xFE", 6);
        binary_content += "\nBED_MESH_CALIBRATE\n";

        // Should not crash - may or may not detect operations depending on implementation
        REQUIRE_NOTHROW(detector.scan_content(binary_content));
    }

    SECTION("Handles very long lines") {
        // A single line with 10,000 characters
        std::string long_line(10000, 'X');
        std::string content = long_line + "\nG28\nBED_MESH_CALIBRATE\n";

        auto result = detector.scan_content(content);

        // Should still detect operations after the long line
        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Handles CRLF line endings") {
        auto result = detector.scan_content("G28\r\nBED_MESH_CALIBRATE\r\n");

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }

    SECTION("Handles mixed line endings") {
        auto result = detector.scan_content("G28\n\rBED_MESH_CALIBRATE\r\nCLEAN_NOZZLE\n");

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
    }

    SECTION("Handles null bytes in content") {
        std::string content = "G28\n";
        content += '\0'; // Null byte
        content += "BED_MESH_CALIBRATE\n";

        // Should not crash
        REQUIRE_NOTHROW(detector.scan_content(content));
    }
}

TEST_CASE("GCodeOpsDetector - Real-world G-code snippets", "[gcode][ops]") {
    GCodeOpsDetector detector;

    SECTION("OrcaSlicer Voron start sequence") {
        std::string content = R"(
; generated by OrcaSlicer 2.1.0
M140 S60 ; set bed temp
M104 S220 ; set extruder temp
G28 ; home all
QUAD_GANTRY_LEVEL ; level gantry
BED_MESH_CALIBRATE ; probe bed
CLEAN_NOZZLE ; wipe nozzle
G1 X10 Y10 Z0.3 E0.5 ; start print
)";

        auto result = detector.scan_content(content);

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE(result.has_operation(OperationType::QGL));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));
    }

    SECTION("PrusaSlicer START_PRINT macro") {
        std::string content = R"(
; generated by PrusaSlicer
START_PRINT EXTRUDER_TEMP=220 BED_TEMP=60 FORCE_LEVELING=true NOZZLE_CLEAN=1
G1 X10 Y10 Z0.2 E0.5
)";

        auto result = detector.scan_content(content);

        REQUIRE(result.has_operation(OperationType::BED_MESH));
        REQUIRE(result.has_operation(OperationType::NOZZLE_CLEAN));

        auto leveling = result.get_operation(OperationType::BED_MESH);
        REQUIRE(leveling->embedding == OperationEmbedding::MACRO_PARAMETER);
    }

    SECTION("Simple Ender 3 start sequence") {
        std::string content = R"(
; Creality Ender-3
G28 ; home
G29 ; auto bed level
G1 X0 Y0 Z0.3 E0.5
)";

        auto result = detector.scan_content(content);

        REQUIRE(result.has_operation(OperationType::HOMING));
        REQUIRE(result.has_operation(OperationType::BED_MESH));
    }
}
