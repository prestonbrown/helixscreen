// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/printer_state.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization
// ============================================================================

struct LVGLInitializerHelixPrint {
    LVGLInitializerHelixPrint() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerHelixPrint lvgl_init_helix;

// ============================================================================
// Test Fixtures
// ============================================================================

class HelixPrintAPITestFixture {
  public:
    HelixPrintAPITestFixture() {
        state.init_subjects(false);
        client = std::make_unique<MoonrakerClient>();
        api = std::make_unique<MoonrakerAPI>(*client, state);
        reset_callbacks();
    }

    void reset_callbacks() {
        success_called = false;
        error_called = false;
        bool_result = false;
        error_message.clear();
        modified_print_result = {};
    }

    PrinterState state;
    std::unique_ptr<MoonrakerClient> client;
    std::unique_ptr<MoonrakerAPI> api;

    // Callback tracking
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::atomic<bool> bool_result{false};
    std::string error_message;
    ModifiedPrintResult modified_print_result;
};

// ============================================================================
// Plugin Detection Tests
// ============================================================================

TEST_CASE_METHOD(HelixPrintAPITestFixture, "HelixPrint API - has_helix_plugin initial state",
                 "[print][api]") {
    // Initially, plugin should not be detected (no check performed yet)
    REQUIRE(state.service_has_helix_plugin() == false);
}

TEST_CASE_METHOD(HelixPrintAPITestFixture,
                 "HelixPrint API - check_helix_plugin with disconnected client", "[print][api]") {
    // With disconnected client, check should report plugin unavailable
    // (error callback path returns false, not error)

    api->job().check_helix_plugin(
        [this](bool available) {
            bool_result = available;
            success_called = true;
        },
        [this](const MoonrakerError& err) {
            error_message = err.message;
            error_called = true;
        });

    // Give async operation time to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Should complete (either success with false, or trigger error->false path)
    // The implementation treats errors as "plugin not available"
    // So either way, service_has_helix_plugin should be false
    REQUIRE(state.service_has_helix_plugin() == false);
}

// ============================================================================
// Fallback Test Client
// ============================================================================
//
// Scripts two RPC methods to exercise the resilience fallback in
// start_modified_print(): the helix_print plugin's print_modified endpoint
// always fails (mimics the shipped v1.0.0 bug), while the stock
// printer.print.start succeeds. Callbacks fire synchronously so the fallback
// chain resolves within the call.
namespace {
class FallbackScriptedClient : public helix::MoonrakerClient {
  public:
    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        (void)timeout_ms;
        (void)silent;
        (void)intent;
        if (method == "server.helix.print_modified") {
            ++print_modified_calls;
            if (error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::VALIDATION_ERROR;
                err.message = "helix_print v1.0.0 print_modified bug";
                err.method = method;
                error_cb(err);
            }
            return 1;
        }
        if (method == "printer.print.start") {
            ++print_start_calls;
            print_start_filename = params.value("filename", std::string{});
            if (success_cb) {
                success_cb(json::object());
            }
            return 2;
        }
        return 0;
    }

    int print_modified_calls = 0;
    int print_start_calls = 0;
    std::string print_start_filename;
};
} // namespace

TEST_CASE("HelixPrint API - falls back to printer.print.start when plugin print_modified fails",
          "[print][api]") {
    FallbackScriptedClient client;
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    ModifiedPrintResult result;

    const std::string temp_path = ".helix_temp/modified_1766807545_benchy.gcode";

    api.job().start_modified_print(
        "benchy.gcode", temp_path, {"bed_leveling_disabled"},
        [&](const ModifiedPrintResult& r) {
            result = r;
            success_called = true;
        },
        [&](const MoonrakerError&) { error_called = true; });

    // Scripted callbacks fire synchronously; drain to satisfy L048/L052 in case a
    // deferral is ever introduced into the fallback path.
    helix::ui::UpdateQueue::instance().drain();

    // The plugin endpoint was attempted once and failed...
    REQUIRE(client.print_modified_calls == 1);
    // ...then the fallback invoked stock printer.print.start with the temp path
    // verbatim (already gcodes-root-relative — no stripping).
    REQUIRE(client.print_start_calls == 1);
    REQUIRE(client.print_start_filename == temp_path);

    // The caller sees SUCCESS (not error), mapped into a ModifiedPrintResult.
    REQUIRE(success_called.load() == true);
    REQUIRE(error_called.load() == false);
    REQUIRE(result.status == "printing");
    REQUIRE(result.original_filename == "benchy.gcode");
    REQUIRE(result.print_filename == temp_path);
}

// ============================================================================
// Modified Print API Validation Tests (v2.0 - Path-Based)
// ============================================================================

TEST_CASE_METHOD(HelixPrintAPITestFixture,
                 "HelixPrint API - start_modified_print validates original filename",
                 "[print][api][security]") {
    SECTION("Rejects path traversal in original filename") {
        api->job().start_modified_print(
            "../../../etc/passwd",       // Malicious original path
            ".helix_temp/mod_123.gcode", // Valid temp path
            {"test_mod"}, [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(error_called == true);
        REQUIRE(success_called == false);
        REQUIRE(error_message.find("directory traversal") != std::string::npos);
    }

    SECTION("Rejects filename with newlines") {
        api->job().start_modified_print(
            "test\nfile.gcode",          // Newline injection
            ".helix_temp/mod_123.gcode", // Valid temp path
            {"test_mod"}, [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(error_called == true);
        REQUIRE(success_called == false);
    }

    SECTION("Accepts valid filename") {
        // This will fail due to disconnected client, but should pass validation
        api->job().start_modified_print(
            "benchy.gcode", ".helix_temp/mod_benchy.gcode", {"bed_leveling_disabled"},
            [this](const ModifiedPrintResult& result) {
                modified_print_result = result;
                success_called = true;
            },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Should fail due to disconnected client, not validation
        // Error message should NOT mention "directory traversal" or "illegal characters"
        if (error_called) {
            REQUIRE(error_message.find("directory traversal") == std::string::npos);
            REQUIRE(error_message.find("illegal characters") == std::string::npos);
        }
    }

    SECTION("Accepts filename with subdirectory") {
        api->job().start_modified_print(
            "prints/2024/benchy.gcode", // Valid subdirectory path
            ".helix_temp/mod_benchy.gcode", {"test_mod"},
            [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Should NOT fail validation (may fail due to network)
        if (error_called) {
            REQUIRE(error_message.find("directory traversal") == std::string::npos);
        }
    }
}

TEST_CASE_METHOD(HelixPrintAPITestFixture,
                 "HelixPrint API - start_modified_print validates temp file path",
                 "[print][api][security]") {
    SECTION("Rejects path traversal in temp path") {
        api->job().start_modified_print(
            "benchy.gcode",        // Valid original
            "../../../etc/passwd", // Malicious temp path
            {"test_mod"}, [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(error_called == true);
        REQUIRE(success_called == false);
        REQUIRE(error_message.find("directory traversal") != std::string::npos);
    }

    SECTION("Rejects temp path with newlines") {
        api->job().start_modified_print(
            "benchy.gcode",
            ".helix_temp/mod\n123.gcode", // Newline injection
            {"test_mod"}, [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        REQUIRE(error_called == true);
        REQUIRE(success_called == false);
    }
}

// ============================================================================
// ModifiedPrintResult Structure Tests
// ============================================================================

TEST_CASE("HelixPrint API - ModifiedPrintResult structure", "[print][api]") {
    ModifiedPrintResult result;

    SECTION("Default values are empty") {
        REQUIRE(result.original_filename.empty());
        REQUIRE(result.print_filename.empty());
        REQUIRE(result.temp_filename.empty());
        REQUIRE(result.status.empty());
    }

    SECTION("Can be populated") {
        result.original_filename = "benchy.gcode";
        result.print_filename = ".helix_print/benchy.gcode";
        result.temp_filename = ".helix_temp/mod_123_benchy.gcode";
        result.status = "printing";

        REQUIRE(result.original_filename == "benchy.gcode");
        REQUIRE(result.print_filename == ".helix_print/benchy.gcode");
        REQUIRE(result.temp_filename == ".helix_temp/mod_123_benchy.gcode");
        REQUIRE(result.status == "printing");
    }
}

// ============================================================================
// Modification List Tests
// ============================================================================

TEST_CASE_METHOD(HelixPrintAPITestFixture, "HelixPrint API - handles empty modifications list",
                 "[print][api]") {
    // Empty modifications list should be valid
    api->job().start_modified_print(
        "benchy.gcode", ".helix_temp/mod_benchy.gcode", {}, // Empty modifications
        [this](const ModifiedPrintResult&) { success_called = true; },
        [this](const MoonrakerError& err) {
            error_message = err.message;
            error_called = true;
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Should not fail validation due to empty modifications
    if (error_called) {
        REQUIRE(error_message.find("modifications") == std::string::npos);
    }
}

TEST_CASE_METHOD(HelixPrintAPITestFixture, "HelixPrint API - handles multiple modifications",
                 "[print][api]") {
    std::vector<std::string> mods = {"bed_leveling_disabled", "z_tilt_disabled", "qgl_disabled",
                                     "nozzle_clean_disabled"};

    api->job().start_modified_print(
        "benchy.gcode", ".helix_temp/mod_benchy.gcode", mods,
        [this](const ModifiedPrintResult&) { success_called = true; },
        [this](const MoonrakerError& err) {
            error_message = err.message;
            error_called = true;
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Should not fail validation
    if (error_called) {
        REQUIRE(error_message.find("directory traversal") == std::string::npos);
    }
}

// ============================================================================
// Path Format Tests (v2.0 API)
// ============================================================================

TEST_CASE_METHOD(HelixPrintAPITestFixture, "HelixPrint API - accepts various valid temp paths",
                 "[print][api]") {
    SECTION("Standard .helix_temp path") {
        api->job().start_modified_print(
            "print.gcode", ".helix_temp/mod_12345_print.gcode", {"test_mod"},
            [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Should not fail validation (network error is expected)
        if (error_called) {
            REQUIRE(error_message.find("directory traversal") == std::string::npos);
            REQUIRE(error_message.find("temp path") == std::string::npos);
        }
    }

    SECTION("Path with special characters in filename") {
        api->job().start_modified_print(
            "my-print_v2.0 (final).gcode", ".helix_temp/mod_my-print_v2.0 (final).gcode",
            {"test_mod"}, [this](const ModifiedPrintResult&) { success_called = true; },
            [this](const MoonrakerError& err) {
                error_message = err.message;
                error_called = true;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Should not fail validation
        if (error_called) {
            REQUIRE(error_message.find("directory traversal") == std::string::npos);
        }
    }
}
