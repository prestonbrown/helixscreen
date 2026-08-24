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
        CHECK(err.details.is_null());
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

TEST_CASE("MoonrakerError::json_rpc_error sets JSON_RPC type, message, method, optional details",
          "[moonraker][error]") {
    SECTION("without details leaves details null") {
        auto err = MoonrakerError::json_rpc_error("PID_CALIBRATE", "Calibration failed");
        CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
        CHECK(err.method == "PID_CALIBRATE");
        CHECK(err.message == "Calibration failed");
        CHECK(err.details.is_null());
    }

    SECTION("details payload is carried through") {
        json payload = {{"result", "failed"}, {"code", 5}};
        auto err =
            MoonrakerError::json_rpc_error("some.method", "Server returned failure", payload);
        CHECK(err.type == MoonrakerErrorType::JSON_RPC_ERROR);
        CHECK(err.details == payload);
    }
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
