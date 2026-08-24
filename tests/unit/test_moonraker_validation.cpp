// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/api/moonraker_api_internal.h"
#include "filament_database.h"

#include "../catch_amalgamated.hpp"

using namespace moonraker_internal;

// ============================================================================
// is_safe_path() Tests
// ============================================================================

TEST_CASE("is_safe_path() accepts valid paths", "[moonraker_validation][path]") {
    SECTION("simple filename") {
        REQUIRE(is_safe_path("file.gcode") == true);
    }

    SECTION("path with subdirectory") {
        REQUIRE(is_safe_path("models/part1.gcode") == true);
    }

    SECTION("deeply nested path") {
        REQUIRE(is_safe_path("a/b/c/d/file.gcode") == true);
    }

    SECTION("path with spaces") {
        REQUIRE(is_safe_path("my models/test file.gcode") == true);
    }

    SECTION("path with underscores and hyphens") {
        REQUIRE(is_safe_path("my_models/test-file_v2.gcode") == true);
    }

    SECTION("path with dots in filename") {
        REQUIRE(is_safe_path("file.v1.2.gcode") == true);
    }
}

TEST_CASE("is_safe_path() rejects directory traversal", "[moonraker_validation][path][security]") {
    SECTION("parent directory at start") {
        REQUIRE(is_safe_path("../etc/passwd") == false);
    }

    SECTION("parent directory in middle") {
        REQUIRE(is_safe_path("models/../../../etc/passwd") == false);
    }

    SECTION("double parent directory") {
        REQUIRE(is_safe_path("models/../../secret") == false);
    }

    SECTION("parent directory at end") {
        REQUIRE(is_safe_path("models/..") == false);
    }
}

TEST_CASE("is_safe_path() rejects absolute paths", "[moonraker_validation][path][security]") {
    SECTION("unix absolute path") {
        REQUIRE(is_safe_path("/etc/passwd") == false);
    }

    SECTION("unix root") {
        REQUIRE(is_safe_path("/") == false);
    }

    SECTION("windows drive path") {
        REQUIRE(is_safe_path("C:/Windows/System32") == false);
    }

    SECTION("windows drive lowercase") {
        REQUIRE(is_safe_path("d:/data") == false);
    }
}

TEST_CASE("is_safe_path() rejects dangerous characters", "[moonraker_validation][path][security]") {
    SECTION("pipe character") {
        REQUIRE(is_safe_path("file|exploit") == false);
    }

    SECTION("less than") {
        REQUIRE(is_safe_path("file<exploit") == false);
    }

    SECTION("greater than") {
        REQUIRE(is_safe_path("file>exploit") == false);
    }

    SECTION("asterisk wildcard") {
        REQUIRE(is_safe_path("*.gcode") == false);
    }

    SECTION("question mark wildcard") {
        REQUIRE(is_safe_path("file?.gcode") == false);
    }

    SECTION("null byte") {
        std::string path_with_null = "file";
        path_with_null += '\0';
        path_with_null += ".gcode";
        REQUIRE(is_safe_path(path_with_null) == false);
    }

    SECTION("control characters") {
        REQUIRE(is_safe_path("file\nname") == false);
        REQUIRE(is_safe_path("file\tname") == false);
        REQUIRE(is_safe_path("file\rname") == false);
    }
}

TEST_CASE("is_safe_path() rejects empty path", "[moonraker_validation][path]") {
    REQUIRE(is_safe_path("") == false);
}

// ============================================================================
// is_safe_identifier() Tests
// ============================================================================

TEST_CASE("is_safe_identifier() accepts valid identifiers", "[moonraker_validation][identifier]") {
    SECTION("simple name") {
        REQUIRE(is_safe_identifier("extruder") == true);
    }

    SECTION("name with underscore") {
        REQUIRE(is_safe_identifier("heater_bed") == true);
    }

    SECTION("name with space (for heater_generic names)") {
        REQUIRE(is_safe_identifier("heater_generic chamber") == true);
    }

    SECTION("alphanumeric") {
        REQUIRE(is_safe_identifier("fan0") == true);
        REQUIRE(is_safe_identifier("extruder1") == true);
    }

    SECTION("mixed case") {
        REQUIRE(is_safe_identifier("MyHeater") == true);
    }
}

TEST_CASE("is_safe_identifier() rejects dangerous identifiers",
          "[moonraker_validation][identifier][security]") {
    SECTION("semicolon (G-code injection)") {
        REQUIRE(is_safe_identifier("heater;G28") == false);
    }

    SECTION("newline (G-code injection)") {
        REQUIRE(is_safe_identifier("heater\nG28") == false);
    }

    SECTION("carriage return") {
        REQUIRE(is_safe_identifier("heater\rG28") == false);
    }

    SECTION("path traversal attempt") {
        REQUIRE(is_safe_identifier("../etc") == false);
    }

    SECTION("special characters") {
        REQUIRE(is_safe_identifier("heater!") == false);
        REQUIRE(is_safe_identifier("heater@bed") == false);
        REQUIRE(is_safe_identifier("heater#1") == false);
    }
}

TEST_CASE("is_safe_identifier() rejects empty identifier", "[moonraker_validation][identifier]") {
    REQUIRE(is_safe_identifier("") == false);
}

// ============================================================================
// is_safe_material_param() / gcode_param_value() Tests
// ============================================================================
//
// SET_MATERIAL (AFC) and MMU_GATE_MAP (Happy Hare) used to gate the material on
// is_safe_gcode_param() -> is_safe_identifier(), whose charset is [A-Za-z0-9_ ].
// That rejected `PLA+` — which HelixScreen itself offers from
// filament_database.h — so every spool save silently dropped the material and
// still reported success (bundle XGVDYEB5).

TEST_CASE("is_safe_material_param() accepts real material names",
          "[moonraker_validation][material]") {
    SECTION("plain names") {
        REQUIRE(is_safe_material_param("PLA") == true);
        REQUIRE(is_safe_material_param("ABS") == true);
        REQUIRE(is_safe_material_param("PETG") == true);
    }

    SECTION("plus suffix — the reported failure") {
        REQUIRE(is_safe_material_param("PLA+") == true);
        REQUIRE(is_safe_material_param("ABS+") == true);
        REQUIRE(is_safe_material_param("ASA+") == true);
    }

    SECTION("hyphenated fibre-filled grades") {
        REQUIRE(is_safe_material_param("PA6-CF") == true);
        REQUIRE(is_safe_material_param("PETG-CF") == true);
        REQUIRE(is_safe_material_param("PC-ABS") == true);
        REQUIRE(is_safe_material_param("PLA-GF") == true);
    }

    SECTION("multi-word names") {
        REQUIRE(is_safe_material_param("Silk PLA") == true);
        REQUIRE(is_safe_material_param("Matte PLA") == true);
    }

    SECTION("remaining allowed punctuation") {
        REQUIRE(is_safe_material_param("PLA 1.75") == true);
        REQUIRE(is_safe_material_param("PA (dry)") == true);
        REQUIRE(is_safe_material_param("PC/ABS") == true);
        REQUIRE(is_safe_material_param("wood_fill") == true);
    }
}

TEST_CASE("is_safe_material_param() rejects G-code injection vectors",
          "[moonraker_validation][material][security]") {
    SECTION("semicolon (Klipper comment — truncates the command)") {
        REQUIRE(is_safe_material_param("PLA;G28") == false);
    }

    SECTION("newline (splits a gcode script into two commands)") {
        REQUIRE(is_safe_material_param("PLA\nG28") == false);
    }

    SECTION("carriage return") {
        REQUIRE(is_safe_material_param("PLA\rG28") == false);
    }

    SECTION("hash (Klipper's other comment character)") {
        REQUIRE(is_safe_material_param("PLA#1") == false);
    }

    SECTION("asterisk (checksum separator)") {
        REQUIRE(is_safe_material_param("PLA*42") == false);
    }

    SECTION("quote and backslash — would escape gcode_param_value()'s quoting") {
        REQUIRE(is_safe_material_param("PLA\" EXTRUDE=1 X=\"") == false);
        REQUIRE(is_safe_material_param("PLA\\") == false);
        REQUIRE(is_safe_material_param("PLA'") == false);
    }

    SECTION("equals (would forge a second parameter)") {
        REQUIRE(is_safe_material_param("PLA LANE=lane2") == false);
    }

    SECTION("tabs and other control characters") {
        REQUIRE(is_safe_material_param("PLA\tG28") == false);
        REQUIRE(is_safe_material_param(std::string("PLA\0G28", 7)) == false);
    }

    SECTION("non-ASCII bytes") {
        REQUIRE(is_safe_material_param("Grün") == false);
    }
}

TEST_CASE("is_safe_material_param() rejects empty material", "[moonraker_validation][material]") {
    REQUIRE(is_safe_material_param("") == false);
}

TEST_CASE("is_safe_material_param() accepts every material HelixScreen offers",
          "[moonraker_validation][material]") {
    // The bug class, not just the one reported instance: any name the filament
    // database offers must survive the validator that persists it, or picking it
    // in the UI silently writes nothing.
    for (size_t i = 0; i < filament::MATERIAL_COUNT; ++i) {
        INFO("material: " << filament::MATERIALS[i].name);
        REQUIRE(is_safe_material_param(filament::MATERIALS[i].name) == true);
    }
}

TEST_CASE("gcode_param_value() quotes only what Klipper would tokenize",
          "[moonraker_validation][material]") {
    SECTION("values without whitespace go out unchanged") {
        REQUIRE(gcode_param_value("PLA") == "PLA");
        REQUIRE(gcode_param_value("PLA+") == "PLA+");
        REQUIRE(gcode_param_value("PA6-CF") == "PA6-CF");
    }

    SECTION("values with whitespace are quoted so shlex keeps them as one token") {
        REQUIRE(gcode_param_value("Silk PLA") == "\"Silk PLA\"");
        REQUIRE(gcode_param_value("Matte PLA") == "\"Matte PLA\"");
    }
}

// ============================================================================
// is_safe_url_param() Tests
// ============================================================================

TEST_CASE("is_safe_url_param() accepts Moonraker config section names",
          "[moonraker_validation][url_param]") {
    SECTION("leading and trailing hyphens (prestonbrown/helixscreen#1241)") {
        REQUIRE(is_safe_url_param("-Power-") == true);
    }

    SECTION("plain identifier") {
        REQUIRE(is_safe_url_param("printer") == true);
    }

    SECTION("dots") {
        REQUIRE(is_safe_url_param("shelly.plug.1") == true);
    }

    SECTION("spaces") {
        REQUIRE(is_safe_url_param("Printer PSU") == true);
    }

    SECTION("parentheses and brackets") {
        REQUIRE(is_safe_url_param("Light (Back)") == true);
        REQUIRE(is_safe_url_param("psu[0]") == true);
    }

    SECTION("punctuation that only matters in G-code") {
        // The device name is percent-encoded into a query string, never emitted
        // as G-code, so `;` and friends carry no injection risk here.
        REQUIRE(is_safe_url_param("psu;light") == true);
        REQUIRE(is_safe_url_param("a&b=c") == true);
        REQUIRE(is_safe_url_param("100%") == true);
    }

    SECTION("multi-byte UTF-8 bytes") {
        REQUIRE(is_safe_url_param("Beleuchtung Küche") == true);
        REQUIRE(is_safe_url_param("电源") == true);
    }
}

TEST_CASE("is_safe_url_param() rejects control characters",
          "[moonraker_validation][url_param][security]") {
    SECTION("empty") {
        REQUIRE(is_safe_url_param("") == false);
    }

    SECTION("newline (log injection / header splitting)") {
        REQUIRE(is_safe_url_param("psu\nX-Evil: 1") == false);
    }

    SECTION("carriage return") {
        REQUIRE(is_safe_url_param("psu\rX-Evil: 1") == false);
    }

    SECTION("tab") {
        REQUIRE(is_safe_url_param("psu\tlight") == false);
    }

    SECTION("embedded NUL") {
        REQUIRE(is_safe_url_param(std::string("psu\0light", 9)) == false);
    }

    SECTION("DEL (0x7F)") {
        REQUIRE(is_safe_url_param(std::string("psu\x7F")) == false);
    }

    SECTION("other low control bytes") {
        REQUIRE(is_safe_url_param(std::string("psu\x01")) == false);
        REQUIRE(is_safe_url_param(std::string("\x1B[31mpsu")) == false);
    }
}

TEST_CASE("is_safe_url_param() is looser than is_safe_identifier for power devices",
          "[moonraker_validation][url_param][1241]") {
    // Regression for prestonbrown/helixscreen#1241: `[power -Power-]` is a legal
    // Moonraker section name, and gating set_device_power() on is_safe_identifier()
    // silently broke power control for it. The two validators must stay distinct:
    // identifiers reach G-code, URL params get percent-encoded.
    REQUIRE(is_safe_identifier("-Power-") == false);
    REQUIRE(is_safe_url_param("-Power-") == true);

    // Both still reject the injection primitives that matter to each.
    REQUIRE(is_safe_identifier("psu\nG28") == false);
    REQUIRE(is_safe_url_param("psu\nG28") == false);
}

// ============================================================================
// is_valid_axis() Tests
// ============================================================================

TEST_CASE("is_valid_axis() accepts valid axes", "[moonraker_validation][axis]") {
    SECTION("uppercase") {
        REQUIRE(is_valid_axis('X') == true);
        REQUIRE(is_valid_axis('Y') == true);
        REQUIRE(is_valid_axis('Z') == true);
        REQUIRE(is_valid_axis('E') == true);
    }

    SECTION("lowercase") {
        REQUIRE(is_valid_axis('x') == true);
        REQUIRE(is_valid_axis('y') == true);
        REQUIRE(is_valid_axis('z') == true);
        REQUIRE(is_valid_axis('e') == true);
    }
}

TEST_CASE("is_valid_axis() rejects invalid axes", "[moonraker_validation][axis]") {
    REQUIRE(is_valid_axis('A') == false);
    REQUIRE(is_valid_axis('B') == false);
    REQUIRE(is_valid_axis('W') == false);
    REQUIRE(is_valid_axis('1') == false);
    REQUIRE(is_valid_axis(' ') == false);
    REQUIRE(is_valid_axis('\0') == false);
}

// ============================================================================
// reject_invalid_path() Tests
// ============================================================================

TEST_CASE("reject_invalid_path() returns false for valid paths", "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError&) { error_called = true; };

    REQUIRE(reject_invalid_path("models/file.gcode", "test_method", on_error) == false);
    REQUIRE(error_called == false);
}

TEST_CASE("reject_invalid_path() returns true and calls error for invalid paths",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerError captured_error;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    };

    SECTION("directory traversal") {
        REQUIRE(reject_invalid_path("../secret", "my_method", on_error, true) == true);
        REQUIRE(error_called == true);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "my_method");
    }

    SECTION("absolute path") {
        error_called = false;
        REQUIRE(reject_invalid_path("/etc/passwd", "file_op", on_error, true) == true);
        REQUIRE(error_called == true);
    }
}

TEST_CASE("reject_invalid_path() works with nullptr callback", "[moonraker_validation][reject]") {
    // Should not crash with nullptr callback
    REQUIRE(reject_invalid_path("../bad", "test", nullptr, true) == true);
    REQUIRE(reject_invalid_path("good/path", "test", nullptr) == false);
}

// ============================================================================
// reject_invalid_identifier() Tests
// ============================================================================

TEST_CASE("reject_invalid_identifier() returns false for valid identifiers",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError&) { error_called = true; };

    REQUIRE(reject_invalid_identifier("heater_bed", "test_method", on_error) == false);
    REQUIRE(error_called == false);
}

TEST_CASE("reject_invalid_identifier() returns true and calls error for invalid identifiers",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerError captured_error;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    };

    REQUIRE(reject_invalid_identifier("heater;G28", "set_temp", on_error, true) == true);
    REQUIRE(error_called == true);
    REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    REQUIRE(captured_error.method == "set_temp");
}

TEST_CASE("reject_invalid_identifier() works with nullptr callback",
          "[moonraker_validation][reject]") {
    REQUIRE(reject_invalid_identifier("bad;id", "test", nullptr, true) == true);
    REQUIRE(reject_invalid_identifier("good_id", "test", nullptr) == false);
}

// ============================================================================
// reject_invalid_file_root() Tests
// ============================================================================

TEST_CASE("reject_invalid_file_root() accepts hidden Moonraker roots",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError&) { error_called = true; };

    REQUIRE(reject_invalid_file_root("gcodes", "list_files", on_error) == false);
    REQUIRE(reject_invalid_file_root(".temp", "list_files", on_error) == false);
    REQUIRE(error_called == false);

    // Keep generic identifier validation unchanged for non-file-root callers.
    REQUIRE(reject_invalid_identifier(".temp", "other_method", nullptr, true) == true);
}

TEST_CASE("reject_invalid_file_root() rejects unsafe roots", "[moonraker_validation][reject]") {
    MoonrakerError captured_error;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError& err) { captured_error = err; };

    REQUIRE(reject_invalid_file_root("../secret", "list_files", on_error, true) == true);
    REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
    REQUIRE(captured_error.method == "list_files");

    REQUIRE(reject_invalid_file_root(".temp/shadow", "list_files", nullptr, true) == true);
    REQUIRE(reject_invalid_file_root(".temp\n", "list_files", nullptr, true) == true);
    REQUIRE(reject_invalid_file_root(".temp;G28", "list_files", nullptr, true) == true);
    REQUIRE(reject_invalid_file_root(".", "list_files", nullptr, true) == true);
}

// ============================================================================
// reject_out_of_range() Tests
// ============================================================================

TEST_CASE("reject_out_of_range() returns false for values in range",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError&) { error_called = true; };

    SECTION("value at minimum") {
        REQUIRE(reject_out_of_range(0.0, 0.0, 100.0, "temp", "test", on_error) == false);
        REQUIRE(error_called == false);
    }

    SECTION("value at maximum") {
        REQUIRE(reject_out_of_range(100.0, 0.0, 100.0, "temp", "test", on_error) == false);
        REQUIRE(error_called == false);
    }

    SECTION("value in middle") {
        REQUIRE(reject_out_of_range(50.0, 0.0, 100.0, "temp", "test", on_error) == false);
        REQUIRE(error_called == false);
    }
}

TEST_CASE("reject_out_of_range() returns true and calls error for out of range values",
          "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerError captured_error;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError& err) {
        error_called = true;
        captured_error = err;
    };

    SECTION("value below minimum") {
        REQUIRE(reject_out_of_range(-1.0, 0.0, 100.0, "temperature", "set_temp", on_error, true) ==
                true);
        REQUIRE(error_called == true);
        REQUIRE(captured_error.type == MoonrakerErrorType::VALIDATION_ERROR);
        REQUIRE(captured_error.method == "set_temp");
        REQUIRE(captured_error.message.find("temperature") != std::string::npos);
    }

    SECTION("value above maximum") {
        error_called = false;
        REQUIRE(reject_out_of_range(101.0, 0.0, 100.0, "speed", "set_speed", on_error, true) ==
                true);
        REQUIRE(error_called == true);
    }
}

TEST_CASE("reject_out_of_range() works with nullptr callback", "[moonraker_validation][reject]") {
    REQUIRE(reject_out_of_range(-10.0, 0.0, 100.0, "val", "test", nullptr, true) == true);
    REQUIRE(reject_out_of_range(50.0, 0.0, 100.0, "val", "test", nullptr) == false);
}

TEST_CASE("reject_out_of_range() handles negative ranges", "[moonraker_validation][reject]") {
    bool error_called = false;
    MoonrakerAPI::ErrorCallback on_error = [&](const MoonrakerError&) { error_called = true; };

    SECTION("negative range - value in range") {
        REQUIRE(reject_out_of_range(-50.0, -100.0, 0.0, "offset", "test", on_error) == false);
        REQUIRE(error_called == false);
    }

    SECTION("negative range - value out of range") {
        REQUIRE(reject_out_of_range(10.0, -100.0, 0.0, "offset", "test", on_error, true) == true);
        REQUIRE(error_called == true);
    }
}

// ============================================================================
// Safety Limit Validation Functions Tests
// ============================================================================

TEST_CASE("is_safe_temperature() validates temperature ranges", "[moonraker_validation][safety]") {
    SafetyLimits limits;
    limits.min_temperature_celsius = 0.0;
    limits.max_temperature_celsius = 350.0;

    SECTION("valid temperatures") {
        REQUIRE(is_safe_temperature(0.0, limits) == true);
        REQUIRE(is_safe_temperature(200.0, limits) == true);
        REQUIRE(is_safe_temperature(350.0, limits) == true);
    }

    SECTION("invalid temperatures") {
        REQUIRE(is_safe_temperature(-10.0, limits) == false);
        REQUIRE(is_safe_temperature(400.0, limits) == false);
    }
}

TEST_CASE("is_safe_fan_speed() validates fan speed ranges", "[moonraker_validation][safety]") {
    SafetyLimits limits;
    limits.min_fan_speed_percent = 0.0;
    limits.max_fan_speed_percent = 100.0;

    SECTION("valid speeds") {
        REQUIRE(is_safe_fan_speed(0.0, limits) == true);
        REQUIRE(is_safe_fan_speed(50.0, limits) == true);
        REQUIRE(is_safe_fan_speed(100.0, limits) == true);
    }

    SECTION("invalid speeds") {
        REQUIRE(is_safe_fan_speed(-1.0, limits) == false);
        REQUIRE(is_safe_fan_speed(101.0, limits) == false);
    }
}

TEST_CASE("is_safe_feedrate() validates feedrate ranges", "[moonraker_validation][safety]") {
    SafetyLimits limits;
    limits.min_feedrate_mm_min = 0.0;
    limits.max_feedrate_mm_min = 10000.0;

    SECTION("valid feedrates") {
        REQUIRE(is_safe_feedrate(0.0, limits) == true);
        REQUIRE(is_safe_feedrate(5000.0, limits) == true);
        REQUIRE(is_safe_feedrate(10000.0, limits) == true);
    }

    SECTION("invalid feedrates") {
        REQUIRE(is_safe_feedrate(-100.0, limits) == false);
        REQUIRE(is_safe_feedrate(15000.0, limits) == false);
    }
}

TEST_CASE("is_safe_distance() validates distance ranges", "[moonraker_validation][safety]") {
    SafetyLimits limits;
    limits.min_relative_distance_mm = -500.0;
    limits.max_relative_distance_mm = 500.0;

    SECTION("valid distances") {
        REQUIRE(is_safe_distance(-500.0, limits) == true);
        REQUIRE(is_safe_distance(0.0, limits) == true);
        REQUIRE(is_safe_distance(500.0, limits) == true);
    }

    SECTION("invalid distances") {
        REQUIRE(is_safe_distance(-600.0, limits) == false);
        REQUIRE(is_safe_distance(600.0, limits) == false);
    }
}

TEST_CASE("is_safe_position() validates position ranges", "[moonraker_validation][safety]") {
    SafetyLimits limits;
    limits.min_absolute_position_mm = 0.0;
    limits.max_absolute_position_mm = 300.0;

    SECTION("valid positions") {
        REQUIRE(is_safe_position(0.0, limits) == true);
        REQUIRE(is_safe_position(150.0, limits) == true);
        REQUIRE(is_safe_position(300.0, limits) == true);
    }

    SECTION("invalid positions") {
        REQUIRE(is_safe_position(-10.0, limits) == false);
        REQUIRE(is_safe_position(350.0, limits) == false);
    }
}
