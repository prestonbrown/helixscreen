// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <optional>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixtures
// ============================================================================

// DEFERRED: All tests using this fixture crash with SIGSEGV during destruction
// The crash is in unordered_set<string> destructor with corrupted pointer 0x4079000000000000 (=
// 400.0 as double) Root cause: Memory corruption likely from uninitialized lv_subject_t in
// PrinterState when init_subjects() isn't called. Pre-existing issue - needs investigation.
class MacroManagerTestFixture {
  public:
    MacroManagerTestFixture() : state_(), api_(client_, state_), manager_(api_, hardware_) {}

    void set_helix_macros_installed() {
        // Simulate printer with the current Helix macro pack installed
        json objects = json::array(
            {"gcode_macro HELIX_READY", "gcode_macro HELIX_ENDED", "gcode_macro HELIX_RESET",
             "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_CLEAN_NOZZLE",
             "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro HELIX_UNLOAD_FILAMENT",
             "gcode_macro _HELIX_STATE", "bed_mesh"});
        hardware_.parse_objects(objects);
    }

    void set_no_helix_macros() {
        // Simulate printer without Helix macros
        json objects =
            json::array({"gcode_macro START_PRINT", "gcode_macro CLEAN_NOZZLE", "bed_mesh"});
        hardware_.parse_objects(objects);
    }

    void set_partial_helix_macros() {
        // Simulate printer with legacy v1.x macros (no HELIX_READY)
        json objects = json::array({"gcode_macro HELIX_START_PRINT", "bed_mesh"});
        hardware_.parse_objects(objects);
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    MoonrakerAPI api_;
    PrinterDiscovery hardware_;
    MacroManager manager_;
};

// ============================================================================
// Status Detection Tests
// ============================================================================

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - is_installed returns false when no macros", "[config][status]") {
    set_no_helix_macros();

    REQUIRE_FALSE(manager_.is_installed());
}

TEST_CASE_METHOD(MacroManagerTestFixture, "MacroManager - is_installed returns true when installed",
                 "[config][status]") {
    set_helix_macros_installed();

    REQUIRE(manager_.is_installed());
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - get_status returns NOT_INSTALLED when no macros",
                 "[config][status]") {
    set_no_helix_macros();

    REQUIRE(manager_.get_status() == MacroInstallStatus::NOT_INSTALLED);
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - get_status returns INSTALLED when current version",
                 "[config][status]") {
    set_helix_macros_installed();

    REQUIRE(manager_.get_status() == MacroInstallStatus::INSTALLED);
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - get_status returns OUTDATED for a pre-unload v2.0 install",
                 "[config][status]") {
    // v2.0.0 pack: has HELIX_READY but predates HELIX_UNLOAD_FILAMENT, so the
    // presence ladder infers 2.0.0 and the version compare flags it.
    json objects =
        json::array({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                     "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED",
                     "gcode_macro _HELIX_STATE"});
    hardware_.parse_objects(objects);

    REQUIRE(manager_.get_status() == MacroInstallStatus::OUTDATED);
}

// ============================================================================
// Macro Content Tests
// ============================================================================

TEST_CASE("MacroManager - get_macro_content returns valid Klipper config", "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // Should contain version header (v2.0+ format)
    REQUIRE(content.find("# helix_macros v") != std::string::npos);

    // Should contain core signal macros
    REQUIRE(content.find("[gcode_macro HELIX_READY]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_ENDED]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_RESET]") != std::string::npos);

    // Should contain pre-print helper macros
    REQUIRE(content.find("[gcode_macro HELIX_START_PRINT]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_CLEAN_NOZZLE]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_BED_MESH_IF_NEEDED]") != std::string::npos);

    // Should contain phase tracking macros
    REQUIRE(content.find("[gcode_macro HELIX_PHASE_HOMING]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_PHASE_HEATING_BED]") != std::string::npos);

    // Should contain proper gcode: sections
    REQUIRE(content.find("gcode:") != std::string::npos);

    // Should contain Jinja2 templating
    REQUIRE(content.find("{% set") != std::string::npos);
    REQUIRE(content.find("{% if") != std::string::npos);
}

TEST_CASE("MacroManager - get_macro_content contains parameter handling", "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // HELIX_START_PRINT should accept temperature parameters
    REQUIRE(content.find("BED_TEMP") != std::string::npos);
    REQUIRE(content.find("EXTRUDER_TEMP") != std::string::npos);

    // HELIX_START_PRINT should accept operation flags (PERFORM_* is the standard)
    REQUIRE(content.find("PERFORM_QGL") != std::string::npos);
    REQUIRE(content.find("PERFORM_Z_TILT") != std::string::npos);
    REQUIRE(content.find("PERFORM_BED_MESH") != std::string::npos);
    REQUIRE(content.find("PERFORM_NOZZLE_CLEAN") != std::string::npos);
}

TEST_CASE("MacroManager - get_macro_content includes conditional operations", "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // Should check for QGL availability
    REQUIRE(content.find("quad_gantry_level") != std::string::npos);

    // Should check for Z-tilt availability
    REQUIRE(content.find("z_tilt") != std::string::npos);

    // Should call standard Klipper commands
    REQUIRE(content.find("BED_MESH_CALIBRATE") != std::string::npos);
    REQUIRE(content.find("QUAD_GANTRY_LEVEL") != std::string::npos);
    REQUIRE(content.find("Z_TILT_ADJUST") != std::string::npos);
}

TEST_CASE("MacroManager - get_macro_names returns expected macros", "[config][content]") {
    auto names = MacroManager::get_macro_names();

    // v2.1 has 15 public macros (excluding _HELIX_STATE which starts with _)
    REQUIRE(names.size() == 15);

    // Core signals
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_READY") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_ENDED") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_RESET") != names.end());

    // Pre-print helpers
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_START_PRINT") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_CLEAN_NOZZLE") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_BED_MESH_IF_NEEDED") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_UNLOAD_FILAMENT") != names.end());

    // Phase tracking (spot check a few)
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_PHASE_HOMING") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_PHASE_BED_MESH") != names.end());
}

// ============================================================================
// HELIX_CLEAN_NOZZLE Macro Tests
// ============================================================================

TEST_CASE("MacroManager - HELIX_CLEAN_NOZZLE has configurable brush position",
          "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // Should have configurable variables
    REQUIRE(content.find("variable_brush_x") != std::string::npos);
    REQUIRE(content.find("variable_brush_y") != std::string::npos);
    REQUIRE(content.find("variable_brush_z") != std::string::npos);
    REQUIRE(content.find("variable_wipe_count") != std::string::npos);
}

// ============================================================================
// HELIX_BED_MESH_IF_NEEDED Macro Tests
// ============================================================================

TEST_CASE("MacroManager - HELIX_BED_MESH_IF_NEEDED has age-based logic", "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // Should have MAX_AGE parameter
    REQUIRE(content.find("MAX_AGE") != std::string::npos);

    // Should track last mesh time
    REQUIRE(content.find("variable_last_mesh_time") != std::string::npos);

    // Should check mesh profile
    REQUIRE(content.find("bed_mesh.profile_name") != std::string::npos);
}

// ============================================================================
// Version Tests
// ============================================================================

TEST_CASE("MacroManager - get_version returns valid semver", "[config][version]") {
    std::string version = MacroManager::get_version();

    // Should not be empty
    REQUIRE_FALSE(version.empty());

    // Should match semver pattern (major.minor.patch)
    REQUIRE(version.find('.') != std::string::npos);

    // Should be at least 2.0.0 (v2.0 format)
    REQUIRE(version >= "2.0.0");
}

TEST_CASE("MacroManager - filename constant is valid", "[config][constants]") {
    std::string filename = HELIX_MACROS_FILENAME;

    REQUIRE(filename == "helix_macros.cfg");
    REQUIRE(filename.find(".cfg") != std::string::npos);
}

// ============================================================================
// Integration-Style Tests (using mock)
// ============================================================================

// NOTE: The install/update tests below currently expect callbacks NOT to fire
// because the mock doesn't implement printer.restart. When HTTP file upload
// is implemented, these tests should be updated to verify actual success.

// Step 1 of both install() and update() is upload_macro_file(), which goes
// through MoonrakerAPI::transfers() over HTTP - not over the websocket client -
// so MoonrakerClientMock records nothing for these paths. What it does do is
// fail SYNCHRONOUSLY on the calling thread, with err.method == "upload_file".
// That error callback is the observable proof that install()/update() actually
// reached the upload step: an implementation that returned early, or never
// touched the API, leaves it unfired.
//
// err.type is what separates "reached the upload and could not reach a server"
// from "was refused before a request was ever built". upload_macro_file() passes
// path="" meaning "upload straight to the config root", and
// upload_file_with_name() used to run reject_invalid_path() on that path -
// is_safe_path("") is false, so every install/update on a real printer died with
// VALIDATION_ERROR before the URL check. The fix scopes that guard to non-empty
// paths (the directory component is optional) and validates the filename
// instead. CONNECTION_LOST here is the assertion that the upload got past
// validation and failed only for want of a configured server.

TEST_CASE_METHOD(MacroManagerTestFixture, "MacroManager - install reaches the upload step",
                 "[config][install]") {
    set_no_helix_macros();

    bool success_called = false;
    std::optional<MoonrakerError> error;

    manager_.install([&]() { success_called = true; },
                     [&](const MoonrakerError& err) { error = err; });

    REQUIRE_FALSE(success_called); // nothing can have succeeded without a server
    REQUIRE(error.has_value());
    CHECK(error->method == "upload_file"); // it got as far as the upload
    // Not VALIDATION_ERROR: the empty config-root path must not be refused.
    CHECK(error->type == MoonrakerErrorType::CONNECTION_LOST);
    CHECK_FALSE(error->message.empty());
}

TEST_CASE_METHOD(MacroManagerTestFixture, "MacroManager - update reaches the upload step",
                 "[config][install]") {
    set_helix_macros_installed();

    bool success_called = false;
    std::optional<MoonrakerError> error;

    manager_.update([&]() { success_called = true; },
                    [&](const MoonrakerError& err) { error = err; });

    REQUIRE_FALSE(success_called);
    REQUIRE(error.has_value());
    CHECK(error->method == "upload_file");
    CHECK(error->type == MoonrakerErrorType::CONNECTION_LOST);
    CHECK_FALSE(error->message.empty());
}

// Direct coverage of the validation contract that the two tests above depend on,
// so a regression is attributable to upload_file_with_name() rather than to
// MacroManager. The fixture's MoonrakerAPI has no HTTP base URL configured, so
// anything that clears validation stops at CONNECTION_LOST - which is exactly
// how we tell "accepted" from "refused" without a server.
TEST_CASE_METHOD(MacroManagerTestFixture,
                 "upload_file_with_name - empty path means the root of the root",
                 "[config][install][upload][validation]") {
    std::optional<MoonrakerError> error;

    api_.transfers().upload_file_with_name("config", "", "helix_macros.cfg", "content", nullptr,
                                           [&](const MoonrakerError& err) { error = err; });

    REQUIRE(error.has_value());
    CHECK(error->method == "upload_file");
    CHECK(error->type == MoonrakerErrorType::CONNECTION_LOST);
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "upload_file_with_name - rejects an empty or traversing filename",
                 "[config][install][upload][validation]") {
    SECTION("empty filename is refused even though an empty path is allowed") {
        std::optional<MoonrakerError> error;
        api_.transfers().upload_file_with_name("config", "", "", "content", nullptr,
                                               [&](const MoonrakerError& err) { error = err; });
        REQUIRE(error.has_value());
        CHECK(error->type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("filename is used verbatim in the form, so traversal must be refused") {
        std::optional<MoonrakerError> error;
        api_.transfers().upload_file_with_name("config", "", "../../etc/passwd", "content", nullptr,
                                               [&](const MoonrakerError& err) { error = err; });
        REQUIRE(error.has_value());
        CHECK(error->type == MoonrakerErrorType::VALIDATION_ERROR);
    }

    SECTION("a traversing directory path is still refused") {
        std::optional<MoonrakerError> error;
        api_.transfers().upload_file_with_name("config", "../../etc", "passwd", "content", nullptr,
                                               [&](const MoonrakerError& err) { error = err; });
        REQUIRE(error.has_value());
        CHECK(error->type == MoonrakerErrorType::VALIDATION_ERROR);
    }
}
