// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../src/api/moonraker_api_internal.h"

#include "../catch_amalgamated.hpp"

using namespace moonraker_internal;

// ============================================================================
// report_error() tests
// ============================================================================

TEST_CASE("report_error basic functionality", "[moonraker][error][helpers]") {
    SECTION("invokes callback with correct error type") {
        MoonrakerError captured;
        bool called = false;
        auto cb = [&](const MoonrakerError& e) {
            captured = e;
            called = true;
        };

        report_error(cb, MoonrakerErrorType::CONNECTION_LOST, "test_method", "test message");

        REQUIRE(called);
        REQUIRE(captured.type == MoonrakerErrorType::CONNECTION_LOST);
        REQUIRE(captured.method == "test_method");
        REQUIRE(captured.message == "test message");
        REQUIRE(captured.code == 0);
    }

    SECTION("sets error code when provided") {
        MoonrakerError captured;
        auto cb = [&](const MoonrakerError& e) { captured = e; };

        report_error(cb, MoonrakerErrorType::UNKNOWN, "method", "msg", 404);

        REQUIRE(captured.code == 404);
    }

    SECTION("null callback is safe") {
        // Should not crash
        report_error(nullptr, MoonrakerErrorType::CONNECTION_LOST, "test", "msg");
    }
}

TEST_CASE("report_error covers all error types", "[moonraker][error][helpers]") {
    MoonrakerError captured;
    auto cb = [&](const MoonrakerError& e) { captured = e; };

    SECTION("TIMEOUT") {
        report_error(cb, MoonrakerErrorType::TIMEOUT, "m", "timeout");
        REQUIRE(captured.type == MoonrakerErrorType::TIMEOUT);
    }

    SECTION("FILE_NOT_FOUND") {
        report_error(cb, MoonrakerErrorType::FILE_NOT_FOUND, "m", "not found");
        REQUIRE(captured.type == MoonrakerErrorType::FILE_NOT_FOUND);
    }

    SECTION("VALIDATION_ERROR") {
        report_error(cb, MoonrakerErrorType::VALIDATION_ERROR, "m", "invalid");
        REQUIRE(captured.type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("PARSE_ERROR") {
        report_error(cb, MoonrakerErrorType::PARSE_ERROR, "m", "parse failed");
        REQUIRE(captured.type == MoonrakerErrorType::PARSE_ERROR);
    }
}

// ============================================================================
// report_http_error() tests
// ============================================================================

TEST_CASE("report_http_error status code mapping", "[moonraker][error][http]") {
    MoonrakerError captured;
    auto cb = [&](const MoonrakerError& e) { captured = e; };

    SECTION("404 maps to FILE_NOT_FOUND") {
        report_http_error(cb, 404, "download_file", "File not found: test.gcode");

        REQUIRE(captured.type == MoonrakerErrorType::FILE_NOT_FOUND);
        REQUIRE(captured.code == 404);
        REQUIRE(captured.method == "download_file");
        REQUIRE(captured.message.find("404") != std::string::npos);
    }

    SECTION("403 maps to PERMISSION_DENIED") {
        report_http_error(cb, 403, "upload_file", "Access denied");

        REQUIRE(captured.type == MoonrakerErrorType::PERMISSION_DENIED);
        REQUIRE(captured.code == 403);
    }

    SECTION("500 maps to UNKNOWN") {
        report_http_error(cb, 500, "api_call", "Internal server error");

        REQUIRE(captured.type == MoonrakerErrorType::UNKNOWN);
        REQUIRE(captured.code == 500);
    }

    SECTION("other status codes map to UNKNOWN") {
        report_http_error(cb, 502, "api_call", "Bad gateway");

        REQUIRE(captured.type == MoonrakerErrorType::UNKNOWN);
        REQUIRE(captured.code == 502);
    }

    SECTION("null callback is safe") {
        // Should not crash
        report_http_error(nullptr, 404, "test", "msg");
    }
}

// ============================================================================
// report_connection_error() tests
// ============================================================================

TEST_CASE("report_connection_error", "[moonraker][error][connection]") {
    MoonrakerError captured;
    auto cb = [&](const MoonrakerError& e) { captured = e; };

    SECTION("sets CONNECTION_LOST type") {
        report_connection_error(cb, "download_file", "HTTP request failed");

        REQUIRE(captured.type == MoonrakerErrorType::CONNECTION_LOST);
        REQUIRE(captured.method == "download_file");
        REQUIRE(captured.message == "HTTP request failed");
    }

    SECTION("null callback is safe") {
        report_connection_error(nullptr, "test", "msg");
    }
}

// ============================================================================
// report_parse_error() tests
// ============================================================================

TEST_CASE("report_parse_error", "[moonraker][error][parse]") {
    MoonrakerError captured;
    auto cb = [&](const MoonrakerError& e) { captured = e; };

    SECTION("sets PARSE_ERROR type") {
        report_parse_error(cb, "get_config", "Missing required field 'result'");

        REQUIRE(captured.type == MoonrakerErrorType::PARSE_ERROR);
        REQUIRE(captured.method == "get_config");
        REQUIRE(captured.message.find("Missing") != std::string::npos);
    }

    SECTION("null callback is safe") {
        report_parse_error(nullptr, "test", "msg");
    }
}

// ============================================================================
// json_string_list_or() tests
// ============================================================================

TEST_CASE("json_string_list_or normalizes filament type formats", "[moonraker][helpers]") {
    SECTION("semicolon-delimited string passes through") {
        json j = {{"filament_type", "PLA;PLA;ASA;PETG"}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PLA;PLA;ASA;PETG");
    }

    SECTION("single value string passes through") {
        json j = {{"filament_type", "PLA"}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PLA");
    }

    SECTION("JSON array is joined with semicolons") {
        json j = {{"filament_type", json::array({"PLA", "PLA", "ASA", "PETG"})}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PLA;PLA;ASA;PETG");
    }

    SECTION("stringified JSON array is parsed and joined") {
        json j = {{"filament_type", R"(["PLA", "PLA", "ASA", "ABS", "PLA"])"}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PLA;PLA;ASA;ABS;PLA");
    }

    SECTION("missing key returns empty") {
        json j = {{"other", "value"}};
        REQUIRE(json_string_list_or(j, "filament_type").empty());
    }

    SECTION("empty string returns empty") {
        json j = {{"filament_type", ""}};
        REQUIRE(json_string_list_or(j, "filament_type").empty());
    }

    SECTION("empty JSON array returns empty") {
        json j = {{"filament_type", json::array()}};
        REQUIRE(json_string_list_or(j, "filament_type").empty());
    }

    SECTION("null value returns empty") {
        json j = {{"filament_type", nullptr}};
        REQUIRE(json_string_list_or(j, "filament_type").empty());
    }

    SECTION("single-element JSON array") {
        json j = {{"filament_type", json::array({"ABS"})}};
        REQUIRE(json_string_list_or(j, "filament_type") == "ABS");
    }

    SECTION("single-element stringified array") {
        json j = {{"filament_type", R"(["PETG"])"}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PETG");
    }

    SECTION("malformed stringified array treated as plain string") {
        json j = {{"filament_type", "[not valid json"}};
        REQUIRE(json_string_list_or(j, "filament_type") == "[not valid json");
    }

    SECTION("numeric value returns empty") {
        json j = {{"filament_type", 42}};
        REQUIRE(json_string_list_or(j, "filament_type").empty());
    }

    SECTION("mixed array skips non-string elements") {
        json j = {{"filament_type", json::array({"PLA", 42, "ASA", nullptr, "PETG"})}};
        REQUIRE(json_string_list_or(j, "filament_type") == "PLA;ASA;PETG");
    }
}

// ============================================================================
// MoonrakerError static factories
// ============================================================================

TEST_CASE("MoonrakerError::not_ready / validation_error factories set fields",
          "[moonraker][error][helpers]") {
    MoonrakerError nr = MoonrakerError::not_ready("printer.gcode.script", "Klipper is halted");
    CHECK(nr.type == MoonrakerErrorType::NOT_READY);
    CHECK(nr.method == "printer.gcode.script");
    CHECK(nr.message == "Klipper is halted");

    MoonrakerError ve = MoonrakerError::validation_error("motion.move", "NaN or Inf value");
    CHECK(ve.type == MoonrakerErrorType::VALIDATION_ERROR);
    CHECK(ve.method == "motion.move");
    CHECK(ve.message == "NaN or Inf value");
}
