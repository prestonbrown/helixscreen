// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_error.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

// ============================================================================
// extract_friendly_message() tests
// ============================================================================

TEST_CASE("extract_friendly_message parses Klipper error strings", "[moonraker][error]") {
    SECTION("extracts message from Python dict repr with single quotes") {
        auto result = MoonrakerError::extract_friendly_message(
            "{'error': 'WebRequestError', 'message': 'Must home axis first'}");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("extracts message from JSON with double quotes and space") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"error": "WebRequestError", "message": "Must home axis first"})");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("extracts message from JSON without space after colon") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"error":"WebRequestError","message":"Must home axis first"})");
        REQUIRE(result == "Must home axis first");
    }

    SECTION("returns raw string when no message key found") {
        auto result = MoonrakerError::extract_friendly_message("Some plain error text");
        REQUIRE(result == "Some plain error text");
    }

    SECTION("returns raw string for empty input") {
        auto result = MoonrakerError::extract_friendly_message("");
        REQUIRE(result == "");
    }

    SECTION("handles message with spaces and punctuation") {
        auto result = MoonrakerError::extract_friendly_message(
            "{'error': 'CommandError', 'message': 'Probe triggered prior to movement'}");
        REQUIRE(result == "Probe triggered prior to movement");
    }

    SECTION("handles message-only dict") {
        auto result = MoonrakerError::extract_friendly_message("{'message': 'Timer too close'}");
        REQUIRE(result == "Timer too close");
    }

    // Creality/Klipper key-error envelope uses "msg" (not "message"), e.g. the
    // K2's SET_HEATER_TEMPERATURE rejection. Without this, toasts show raw JSON.
    SECTION("extracts msg from Creality key-error envelope") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"code":"key69", "msg": "The value 'chamber' is not valid for HEATER", "values": ["chamber", "HEATER"]})");
        REQUIRE(result == "The value 'chamber' is not valid for HEATER");
    }

    SECTION("extracts msg without space after colon") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"code":"key12","msg":"Heater extruder not heating at expected rate"})");
        REQUIRE(result == "Heater extruder not heating at expected rate");
    }

    SECTION("decodes a coded envelope and keeps the text around it") {
        auto result = MoonrakerError::extract_friendly_message(
            R"(Error executing script X: {"coded": "0001-0531-0000-0005", "msg":"Printer is not idle", "action":"none"} (line 4))");
        REQUIRE(result == "Error executing script X: Printer is not idle (line 4)");
    }

    SECTION("an escaped quote in msg does not truncate it") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"code":"key12","msg":"Heater \"extruder\" not heating"})");
        REQUIRE(result == R"(Heater "extruder" not heating)");
    }

    SECTION("prefers message over msg when both present") {
        auto result = MoonrakerError::extract_friendly_message(
            R"({"msg":"raw detail","message":"friendly summary"})");
        REQUIRE(result == "friendly summary");
    }
}

// ============================================================================
// from_json_rpc() integration — error message gets cleaned up
// ============================================================================

TEST_CASE("from_json_rpc extracts friendly message from Klipper errors", "[moonraker][error]") {
    SECTION("Klipper homing error gets cleaned up") {
        json error_obj = {
            {"code", -32603},
            {"message", "{'error': 'WebRequestError', 'message': 'Must home axis first'}"}};

        auto err = MoonrakerError::from_json_rpc(error_obj, "printer.gcode.script");
        REQUIRE(err.message == "Must home axis first");
        REQUIRE(err.method == "printer.gcode.script");
        REQUIRE(err.code == -32603);
    }

    SECTION("plain message passes through unchanged") {
        json error_obj = {{"code", -32601}, {"message", "Method not found"}};

        auto err = MoonrakerError::from_json_rpc(error_obj, "some.method");
        REQUIRE(err.message == "Method not found");
    }

    SECTION("a coded firmware envelope becomes its msg") {
        json error_obj = {
            {"code", 400},
            {"message",
             R"({"coded":"0001-0531-0000-0005","msg":"Printer is not idle, cannot restore power loss print","action":"none"})"}};

        auto err = MoonrakerError::from_json_rpc(error_obj, "printer.gcode.script");
        REQUIRE(err.message == "Printer is not idle, cannot restore power loss print");
    }
}

TEST_CASE("json_rpc_error decodes a firmware envelope in a raw error line",
          "[moonraker][error][firmware_error_envelope]") {
    auto err = MoonrakerError::json_rpc_error(
        "PID_CALIBRATE", R"({"code":"key9001","msg":"Heater extruder not heating"})");
    REQUIRE(err.message == "Heater extruder not heating");
}

TEST_CASE("json_rpc_error extracts a wrapped envelope before decoding it",
          "[moonraker][error][firmware_error_envelope]") {
    auto err = MoonrakerError::json_rpc_error(
        "BED_MESH_CALIBRATE",
        R"({'error': 'WebRequestError', 'message': '{"coded": "0003-0522-0002-0008", "msg":"Lost communication with MCU e0"}'})");
    REQUIRE(err.message == "Lost communication with MCU e0");
}

TEST_CASE("friendly_error_line strips the error prefix and decodes",
          "[moonraker][error][firmware_error_envelope]") {
    REQUIRE(
        MoonrakerError::friendly_error_line(
            R"(!! {"coded": "0003-0522-0002-0008", "msg":"Lost communication with MCU 'e0'"})") ==
        "Lost communication with MCU 'e0'");
    REQUIRE(MoonrakerError::friendly_error_line("Error:  Must home axis first") ==
            "Must home axis first");
    REQUIRE(
        MoonrakerError::friendly_error_line(
            R"(!! {"error": "WebRequestError", "message": "Probe triggered prior to movement"})") ==
        "Probe triggered prior to movement");
    REQUIRE(MoonrakerError::friendly_error_line("plain text") == "plain text");
    REQUIRE(MoonrakerError::friendly_error_line("!! Error: foo") == "foo");
}

TEST_CASE("extract_friendly_message skips escaped quotes in the message value",
          "[moonraker][error][firmware_error_envelope]") {
    // Python repr: a value holding both quote kinds escapes the single quotes.
    REQUIRE(
        MoonrakerError::extract_friendly_message(
            R"({'error': 'x', 'message': '{"coded": "1", "msg":"Lost communication with MCU \'e0\'"}'})") ==
        "Lost communication with MCU 'e0'");
    // JSON: the inner envelope's quotes are escaped.
    REQUIRE(
        MoonrakerError::extract_friendly_message(
            R"({"error": "x", "message": "{\"coded\": \"1\", \"msg\":\"Lost communication\"}"})") ==
        "Lost communication");
    REQUIRE(MoonrakerError::extract_friendly_message(
                R"({"error":"x","message":"say \"hi\" twice"})") == R"(say "hi" twice)");
}

// ============================================================================
// Factory field-setting — these guard the refactor that routes all
// MoonrakerError construction in src/api/ through static factories. Each case
// asserts the factory reproduces exactly what the old hand-rolled block set.
// ============================================================================

TEST_CASE("MoonrakerError::unknown sets UNKNOWN type + message + optional method",
          "[moonraker][error]") {
    SECTION("message only leaves method empty and code zero") {
        auto err = MoonrakerError::unknown("QGL not yet implemented");
        CHECK(err.type == MoonrakerErrorType::UNKNOWN);
        CHECK(err.message == "QGL not yet implemented");
        CHECK(err.method.empty());
        CHECK(err.code == 0);
    }

    SECTION("message + method sets both") {
        auto err = MoonrakerError::unknown("Failed to create destination file", "download_file");
        CHECK(err.type == MoonrakerErrorType::UNKNOWN);
        CHECK(err.message == "Failed to create destination file");
        CHECK(err.method == "download_file");
    }
}

TEST_CASE("MoonrakerError::http_status_error formats \"HTTP <code>\" and stores the code",
          "[moonraker][error]") {
    auto err = MoonrakerError::http_status_error("get_power_devices", 404);
    CHECK(err.type == MoonrakerErrorType::UNKNOWN);
    CHECK(err.code == 404);
    CHECK(err.method == "get_power_devices");
    // The exact string the hand-rolled blocks produced: "HTTP " + to_string(status).
    CHECK(err.message == "HTTP 404");

    // A different status must reflect in both fields (guards a hard-coded 404).
    auto err503 = MoonrakerError::http_status_error("set_device_power", 503);
    CHECK(err503.code == 503);
    CHECK(err503.message == "HTTP 503");
}

TEST_CASE("MoonrakerError::file_not_found sets FILE_NOT_FOUND type + method + message",
          "[moonraker][error]") {
    auto err = MoonrakerError::file_not_found("download_file", "File does not exist on printer");
    CHECK(err.type == MoonrakerErrorType::FILE_NOT_FOUND);
    CHECK(err.method == "download_file");
    CHECK(err.message == "File does not exist on printer");
}

TEST_CASE("MoonrakerError::json_rpc_error sets JSON_RPC type, message, method",
          "[moonraker][error]") {
    auto err = MoonrakerError::json_rpc_error("PID_CALIBRATE", "Calibration failed");
    CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
    CHECK(err.method == "PID_CALIBRATE");
    CHECK(err.message == "Calibration failed");
}

TEST_CASE("MoonrakerError::connection_lost preserves the default message and allows override",
          "[moonraker][error]") {
    SECTION("no args yields the generic transport message") {
        auto err = MoonrakerError::connection_lost();
        CHECK(err.type == MoonrakerErrorType::CONNECTION_LOST);
        CHECK(err.method.empty());
        CHECK(err.message == "WebSocket connection lost");
    }

    SECTION("method only keeps the default message (existing callers must not change)") {
        auto err = MoonrakerError::connection_lost("printer.gcode.script");
        CHECK(err.method == "printer.gcode.script");
        CHECK(err.message == "WebSocket connection lost");
    }

    SECTION("explicit message overrides the default") {
        auto err =
            MoonrakerError::connection_lost("get_power_devices", "Not connected to Moonraker");
        CHECK(err.type == MoonrakerErrorType::CONNECTION_LOST);
        CHECK(err.method == "get_power_devices");
        CHECK(err.message == "Not connected to Moonraker");
    }
}

// moonraker/common.py JsonRPC.execute_method sends a ServerError with status
// 404 as code -32601 and keeps its message; "Method not found" with the same
// code is an unregistered method.
TEST_CASE("MoonrakerError::is_not_found reads Moonraker's not-found shapes", "[moonraker][error]") {
    auto rpc = [](int code, const char* message) {
        return MoonrakerError::from_json_rpc({{"code", code}, {"message", message}}, "m");
    };
    CHECK(rpc(-32601, "Not Found").is_not_found());
    CHECK(rpc(-32601, "Namespace lane_data not found").is_not_found());
    CHECK(rpc(-32601, "Key 'x' in namespace 'y' not found").is_not_found());
    CHECK_FALSE(rpc(-32601, "Method not found").is_not_found());
    CHECK_FALSE(rpc(-32601, "Method not found for transport WEBSOCKET").is_not_found());
    CHECK_FALSE(rpc(500, "Internal Server Error").is_not_found());
    CHECK_FALSE(MoonrakerError::connection_lost("m").is_not_found());

    MoonrakerError http;
    http.code = 404;
    CHECK(http.is_not_found());
}
