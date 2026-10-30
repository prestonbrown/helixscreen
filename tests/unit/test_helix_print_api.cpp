// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client.h"
#include "../../include/moonraker_job_api.h"
#include "../../include/moonraker_request_tracker.h"
#include "../../include/printer_state.h"
#include "../../include/standing_print_options.h"
#include "../../include/storage_report.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixtures
// ============================================================================

class HelixPrintAPITestFixture : public LVGLTestFixture {
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
    REQUIRE(state.plugin_status_state().service_has_helix_plugin() == false);
}

TEST_CASE_METHOD(HelixPrintAPITestFixture,
                 "HelixPrint API - check_helix_plugin with disconnected client", "[print][api]") {
    // With disconnected client, check should report plugin unavailable
    // (error callback path returns false, not error)

    api->job().check_helix_plugin(
        [this](const helix::HelixPluginInfo& info) {
            bool_result = info.available;
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
    REQUIRE(state.plugin_status_state().service_has_helix_plugin() == false);
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
        const std::string& method, const json& params,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        (void)silent;
        (void)intent;
        if (method == "server.helix.print_modified") {
            ++print_modified_calls;
            print_modified_timeout_ms = timeout_ms;
            if (error_cb) {
                MoonrakerError err;
                err.type = print_modified_error;
                err.code = print_modified_code;
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

    // Defaults to the shipped v1.0.0 endpoint bug — a prompt refusal from a
    // plugin that did answer. Cases needing "no answer at all" set it.
    MoonrakerErrorType print_modified_error = MoonrakerErrorType::VALIDATION_ERROR;
    int print_modified_code = 0;
    uint32_t print_modified_timeout_ms = 0;
    int print_modified_calls = 0;
    int print_start_calls = 0;
    std::string print_start_filename;
};
} // namespace

namespace {
/// Answers server.helix.status with @p result, as a plugin of some vintage does.
class PluginStatusScriptedClient : public helix::MoonrakerClient {
  public:
    explicit PluginStatusScriptedClient(json result) : result_(std::move(result)) {}

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& /*params*/,
        helix::SharedCallback<void(const json&)> success_cb,
        helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/,
        uint32_t /*timeout_ms*/ = 0, bool /*silent*/ = false,
        std::optional<helix::rpc_error_policy::CallerIntent> /*intent*/ = std::nullopt) override {
        if (method == "server.helix.status" && success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", result_}});
        }
        return 1;
    }

  private:
    json result_;
};
} // namespace

TEST_CASE("a plugin status that names no version reports none", "[print][api][plugin_installer]") {
    PrinterState state;
    state.init_subjects(false);
    auto check = [&state](json result) {
        PluginStatusScriptedClient client(std::move(result));
        MoonrakerAPI api(client, state);
        std::optional<helix::HelixPluginInfo> info;
        api.job().check_helix_plugin([&info](const helix::HelixPluginInfo& i) { info = i; },
                                     [](const MoonrakerError&) {});
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(info.has_value());
        return *info;
    };

    const auto unversioned = check(json{{"enabled", true}});
    CHECK(unversioned.present);
    CHECK(unversioned.available);
    CHECK(unversioned.version.empty());

    const auto versioned = check(json{{"enabled", false}, {"version", "1.2.0"}});
    CHECK(versioned.version == "1.2.0");
    CHECK_FALSE(versioned.available);
}

TEST_CASE("a plugin status features map reaches the plugin_feature gate",
          "[print][api][plugin_feature]") {
    PrinterState state;
    state.init_subjects(false);
    PluginStatusScriptedClient client(
        json{{"enabled", true},
             {"version", "1.3.0"},
             {"api", 2},
             {"features",
              {{"plr", {{"version", 1}, {"available", true}, {"enabled", true}}},
               {"print_hooks", {{"version", 1}, {"available", true}, {"enabled", false}}}}}});
    MoonrakerAPI api(client, state);
    std::optional<helix::HelixPluginInfo> info;
    api.job().check_helix_plugin([&info](const helix::HelixPluginInfo& i) { info = i; },
                                 [](const MoonrakerError&) {});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(info.has_value());
    REQUIRE(info->features.size() == 2);

    auto& plugin = state.plugin_status_state();
    plugin.set_features(info->features);
    plugin.set_installed(true);
    CHECK(plugin.plugin_feature("plr", 1) == helix::PluginFeatureState::Ready);
    CHECK(plugin.plugin_feature("plr", 2) == helix::PluginFeatureState::Absent);
    CHECK(plugin.plugin_feature("print_hooks", 1) == helix::PluginFeatureState::Disabled);
    CHECK(plugin.plugin_feature("event_journal", 1) == helix::PluginFeatureState::Absent);

    plugin.set_installed(false);
    CHECK(plugin.plugin_feature("plr", 1) == helix::PluginFeatureState::Absent);

    plugin.set_installed(true);
    plugin.set_features({});
    CHECK(plugin.plugin_feature("plr", 1) == helix::PluginFeatureState::Absent);
}

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
// print_modified's own RPC ceiling, and what the fallback may act on
// ============================================================================

TEST_CASE("print_modified outlives a print-start macro that heats synchronously",
          "[print][api][1451]") {
    // The plugin answers print_modified only after awaiting SDCARD_PRINT_FILE —
    // the same Klipper ack printer.print.start waits on, reached after the
    // plugin has already copied metadata and created its symlink. On a machine
    // whose print-start macro heats synchronously that ack is minutes away, so
    // at the tracker default this RPC gives up while the print it asked for is
    // running.
    FallbackScriptedClient client;
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    api.job().start_modified_print(
        "benchy.gcode", ".helix_temp/mod_benchy.gcode", {}, [](const ModifiedPrintResult&) {},
        [](const MoonrakerError&) {});

    REQUIRE(client.print_modified_calls == 1);
    CHECK(client.print_modified_timeout_ms >
          helix::MoonrakerRequestTracker::kDefaultRequestTimeoutMs);
    // It ends in the same ack as printer.print.start after strictly more work,
    // so it can never deserve the shorter ceiling of the two.
    CHECK(client.print_modified_timeout_ms >= MoonrakerJobAPI::kPrintStartTimeoutMs);
}

TEST_CASE("a print_modified that never answered does not start a second print",
          "[print][api][1451]") {
    // A timeout or a dropped connection says nothing about whether the print
    // began, and the plugin starts it before it answers. Falling back on one
    // aims printer.print.start at a machine that may already be printing, and
    // the caller's error path then deletes the temp file that running job is
    // reading through the plugin's symlink.
    const auto unanswered =
        GENERATE(MoonrakerErrorType::TIMEOUT, MoonrakerErrorType::CONNECTION_LOST);

    FallbackScriptedClient client;
    client.print_modified_error = unanswered;
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    bool success_called = false;
    bool error_called = false;

    api.job().start_modified_print(
        "benchy.gcode", ".helix_temp/mod_benchy.gcode", {"bed_leveling_disabled"},
        [&](const ModifiedPrintResult&) { success_called = true; },
        [&](const MoonrakerError&) { error_called = true; });

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(client.print_modified_calls == 1);
    CHECK(client.print_start_calls == 0);
    // The caller still learns the plugin call failed — it just is not told a
    // print started, and no second start is fired.
    CHECK(error_called);
    CHECK_FALSE(success_called);
}

TEST_CASE("a print_modified refused for space is final: no stock start replaces it",
          "[print][api][plugin_storage]") {
    // The plugin deletes the copy and answers 507 when the print would leave
    // less than its free-space floor. A stock start would aim at the deleted
    // copy, or print below the floor, and hide the space message.
    FallbackScriptedClient client;
    client.print_modified_code = helix::storage::kNoSpaceRpcCode;
    PrinterState state;
    state.init_subjects(false);
    MoonrakerAPI api(client, state);

    bool success_called = false;
    int error_code = 0;
    api.job().start_modified_print(
        "benchy.gcode", ".helix_temp/mod_benchy.gcode", {},
        [&](const ModifiedPrintResult&) { success_called = true; },
        [&](const MoonrakerError& err) { error_code = err.code; });
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(client.print_modified_calls == 1);
    CHECK(client.print_start_calls == 0);
    CHECK_FALSE(success_called);
    CHECK(error_code == helix::storage::kNoSpaceRpcCode);
    // Nothing started, so the screen's start note is withdrawn: a print some
    // other client starts next is not taken for this one.
    CHECK_FALSE(helix::screen_start_ledger().withdraw_any());
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
