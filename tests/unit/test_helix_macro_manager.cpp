// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_dir_guard.h"
#include "core/update_queue.h"
#include "macro_manager.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <algorithm>
#include <fstream>
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

// No objects list consumed: absence of the macros is not an observation.
// The Settings row must not offer an install on that vacuum.
TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - get_status returns UNKNOWN before any objects list",
                 "[config][status]") {
    // Fresh discovery, parse_objects() never called.
    REQUIRE(manager_.get_status() == MacroInstallStatus::UNKNOWN);
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - evaluate_status is UNKNOWN for an empty snapshot too",
                 "[config][status]") {
    PrinterDiscovery empty;
    REQUIRE(MacroManager::evaluate_status(empty) == MacroInstallStatus::UNKNOWN);
}

// The update gate must compare version components numerically: lexicographic
// order misjudges the moment a component reaches two digits.
TEST_CASE("MacroManager - version_less compares components numerically", "[config][version]") {
    using M = MacroManager;
    CHECK(M::version_less("2.0.0", "2.1.0"));
    CHECK(M::version_less("2.9.0", "2.10.0")); // string compare would say no
    CHECK_FALSE(M::version_less("2.10.0", "2.9.0"));
    CHECK_FALSE(M::version_less("2.1.0", "2.1.0"));
    CHECK_FALSE(M::version_less("2.1.0", "2.0.0"));
    CHECK(M::version_less("2.1.0", "2.1.1"));
    // Uneven component counts: missing components are zero.
    CHECK(M::version_less("2.1", "2.1.1"));
    CHECK_FALSE(M::version_less("2.1.0", "2.1"));
}

// ============================================================================
// Macro Content Tests
// ============================================================================

TEST_CASE("MacroManager - get_macro_content returns valid Klipper config", "[config][content]") {
    std::string content = MacroManager::get_macro_content();

    // Should contain version header (v2.0+ format)
    REQUIRE(content.find("# helix_macros v") != std::string::npos);

    // Should contain core signal macros
    REQUIRE(content.find("[gcode_macro _HELIX_READY]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro _HELIX_ENDED]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro _HELIX_RESET]") != std::string::npos);

    // Should contain pre-print helper macros
    REQUIRE(content.find("[gcode_macro HELIX_START_PRINT]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_CLEAN_NOZZLE]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro HELIX_BED_MESH_IF_NEEDED]") != std::string::npos);

    // Should contain phase tracking macros
    REQUIRE(content.find("[gcode_macro _HELIX_PHASE_HOMING]") != std::string::npos);
    REQUIRE(content.find("[gcode_macro _HELIX_PHASE_HEATING_BED]") != std::string::npos);

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

    // Only the helpers people run themselves stay in a client's macro list.
    REQUIRE(names.size() == 4);
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_START_PRINT") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_CLEAN_NOZZLE") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_BED_MESH_IF_NEEDED") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "HELIX_UNLOAD_FILAMENT") != names.end());
}

TEST_CASE("MacroManager - a printer without HELIX_READY gets the hidden signal names",
          "[config][content]") {
    PrinterDiscovery fresh;
    fresh.parse_objects(json::array({"extruder", "gcode_macro PRINT_START"}));

    const std::string content = MacroManager::macro_content_for(fresh);

    CHECK(content.find("[gcode_macro _HELIX_READY]") != std::string::npos);
    CHECK(content.find("[gcode_macro HELIX_READY]") == std::string::npos);

    PrinterDiscovery hidden;
    hidden.parse_objects(
        json::array({"gcode_macro _HELIX_STATE", "gcode_macro _HELIX_READY",
                     "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_UNLOAD_FILAMENT"}));

    CHECK(MacroManager::macro_content_for(hidden) == content);
}

TEST_CASE("MacroManager - a printer with HELIX_READY keeps every name its macros can call",
          "[config][content]") {
    PrinterDiscovery existing;
    existing.parse_objects(
        json::array({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT"}));

    const std::string content = MacroManager::macro_content_for(existing);

    // A PRINT_START macro instrumented by HelixScreen 1.0 calls these directly,
    // from lines it wrote into the macro itself. Our docs tell people to add
    // HELIX_ENDED and HELIX_RESET by hand. No producer of this list exists in
    // the tree, so it is enumerated here.
    static const std::vector<std::string> called_names = {"HELIX_PHASE_HOMING",
                                                          "HELIX_PHASE_QGL",
                                                          "HELIX_PHASE_Z_TILT",
                                                          "HELIX_PHASE_BED_MESH",
                                                          "HELIX_PHASE_CLEANING",
                                                          "HELIX_PHASE_PURGING",
                                                          "HELIX_PHASE_HEATING_NOZZLE",
                                                          "HELIX_PHASE_HEATING_BED",
                                                          "HELIX_READY",
                                                          "HELIX_ENDED",
                                                          "HELIX_RESET"};

    for (const auto& macro_name : called_names) {
        INFO("existing printers call " << macro_name);
        CHECK(content.find("[gcode_macro " + macro_name + "]") != std::string::npos);
        CHECK(content.find("_" + macro_name) == std::string::npos);
    }
    // The state container is hidden on every printer.
    CHECK(content.find("[gcode_macro _HELIX_STATE]") != std::string::npos);
    CHECK(content.find("MACRO=_HELIX_STATE") != std::string::npos);
}

TEST_CASE("MacroManager - with_visible_signal_names rewrites only the signal names",
          "[config][content]") {
    const std::string input = "[gcode_macro _HELIX_READY]\n"
                              "    _HELIX_ENDED\n"
                              "    _HELIX_RESET\n"
                              "    _HELIX_PHASE_Z_TILT\n"
                              "    _HELIX_READY\n"
                              "MACRO=_HELIX_STATE\n"
                              "_HELIX_PREP\n"
                              "_HELIX_BASE_BED_MESH_CALIBRATE\n"
                              "HELIX_START_PRINT\n"
                              "RESPOND MSG=\"HELIX:READY\"\n";

    CHECK(MacroManager::with_visible_signal_names(input) == "[gcode_macro HELIX_READY]\n"
                                                            "    HELIX_ENDED\n"
                                                            "    HELIX_RESET\n"
                                                            "    HELIX_PHASE_Z_TILT\n"
                                                            "    HELIX_READY\n"
                                                            "MACRO=_HELIX_STATE\n"
                                                            "_HELIX_PREP\n"
                                                            "_HELIX_BASE_BED_MESH_CALIBRATE\n"
                                                            "HELIX_START_PRINT\n"
                                                            "RESPOND MSG=\"HELIX:READY\"\n");
    CHECK(MacroManager::with_visible_signal_names("").empty());
}

TEST_CASE("MacroManager - the hidden pack still reads as the current version",
          "[config][content]") {
    // The pack's version is inferred from which macros exist, and a printer on
    // the hidden names must not be offered an update for lacking HELIX_READY.
    PrinterDiscovery hidden;
    hidden.parse_objects(
        json::array({"gcode_macro _HELIX_STATE", "gcode_macro _HELIX_READY",
                     "gcode_macro HELIX_START_PRINT", "gcode_macro HELIX_CLEAN_NOZZLE",
                     "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro HELIX_UNLOAD_FILAMENT"}));

    CHECK(MacroManager::evaluate_status(hidden) == MacroInstallStatus::INSTALLED);
}

TEST_CASE("MacroManager - get_macro_names ignores a commented-out section header",
          "[config][content]") {
    // Klipper never defines a macro whose [gcode_macro ...] header is
    // commented out, so the parser must not either.
    ConfigDirGuard guard("commented_section");
    {
        std::ofstream out(guard.dir / "helix_macros.cfg");
        out << "[gcode_macro HELIX_READY]\n"
               "gcode:\n"
               "    RESPOND MSG=\"HELIX:READY\"\n"
               "\n"
               "# [gcode_macro HELIX_PHASE_QGL]\n"
               "#gcode:\n"
               "#    RESPOND MSG=\"HELIX:PHASE:QGL\"\n";
    }

    auto names = MacroManager::get_macro_names();

    CHECK(std::find(names.begin(), names.end(), "HELIX_READY") != names.end());
    CHECK(std::find(names.begin(), names.end(), "HELIX_PHASE_QGL") == names.end());
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
    std::string filename = kHelixMacrosFilename;

    REQUIRE(filename == "helix_macros.cfg");
    REQUIRE(filename.find(".cfg") != std::string::npos);
}

// ============================================================================
// Integration-Style Tests (using mock)
// ============================================================================

// Step 1 of both install_files() and update_files() is upload_macro_file(),
// which goes through MoonrakerAPI::transfers() over HTTP - not over the
// websocket client - so MoonrakerClientMock records nothing for these paths.
// What it does do is fail SYNCHRONOUSLY on the calling thread, with
// err.method == "upload_file". That error callback is the observable proof that
// install_files()/update_files() actually reached the upload step: an
// implementation that returned early, or never touched the API, leaves it
// unfired.
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

// ============================================================================
// Staging Flow via the Mock File API
// ============================================================================
//
// The mock transfer API keeps an in-memory config root (set_config_files):
// downloads read it, uploads write it. install_files() chains upload ->
// printer.cfg backup -> include splice, crossing the UpdateQueue between
// steps, so these cases run on the LVGL test base (queue init + drain) and
// assert on what actually landed in the config root.

namespace {

const char* kPrinterCfg = "[stepper_x]\nstep_pin: PF0\n[extruder]\nnozzle_diameter: 0.4\n";

class MacroStageFixture : public LVGLTestFixture {
  public:
    MacroStageFixture() {
        api_.set_config_files({{"printer.cfg", kPrinterCfg}});
    }

    ~MacroStageFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    // A drained step can queue the next one; pump until quiet.
    void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty(12);
    }

    std::map<std::string, std::string> config_files() {
        return api_.transfers_mock().get_config_files();
    }

    size_t backup_count() {
        size_t n = 0;
        for (const auto& [name, _] : config_files()) {
            if (name.rfind("printer.cfg.helixbak-", 0) == 0) {
                ++n;
            }
        }
        return n;
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    MoonrakerAPIMock api_{client_, state_};
    PrinterDiscovery hardware_;
    MacroManager manager_{api_, hardware_};
};

} // namespace

// Real-transfer fixture: the REAL MoonrakerFileTransferAPI over a mock
// client with no HTTP server, on the LVGL base so the queued error
// continuation can drain. This is the fixture the CONNECTION_LOST-vs-
// VALIDATION_ERROR contract below depends on - the mock transfer API
// succeeds unconditionally and could never express it.
class MacroUploadFixture : public LVGLTestFixture {
  public:
    ~MacroUploadFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    MoonrakerAPI api_{client_, state_};
    PrinterDiscovery hardware_;
    MacroManager manager_{api_, hardware_};
};

TEST_CASE_METHOD(MacroUploadFixture, "MacroManager - install reaches the upload step",
                 "[config][install]") {
    hardware_.parse_objects(
        json::array({"gcode_macro START_PRINT", "gcode_macro CLEAN_NOZZLE", "bed_mesh"}));

    bool success_called = false;
    std::optional<MoonrakerError> error;

    manager_.install_files([&]() { success_called = true; },
                           [&](const MoonrakerError& err) { error = err; });
    // The error continuation hops through the UpdateQueue even on failure.
    settle();

    REQUIRE_FALSE(success_called); // nothing can have succeeded without a server
    REQUIRE(error.has_value());
    CHECK(error->method == "upload_file"); // it got as far as the upload
    // Not VALIDATION_ERROR: the empty config-root path must not be refused.
    CHECK(error->type == MoonrakerErrorType::CONNECTION_LOST);
    CHECK_FALSE(error->message.empty());
}

TEST_CASE_METHOD(MacroUploadFixture, "MacroManager - update reaches the upload step",
                 "[config][install]") {
    hardware_.parse_objects(
        json::array({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                     "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED",
                     "gcode_macro HELIX_UNLOAD_FILAMENT"}));

    bool success_called = false;
    std::optional<MoonrakerError> error;

    manager_.update_files([&]() { success_called = true; },
                          [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(success_called);
    REQUIRE(error.has_value());
    CHECK(error->method == "upload_file");
    CHECK(error->type == MoonrakerErrorType::CONNECTION_LOST);
    CHECK_FALSE(error->message.empty());
}

TEST_CASE_METHOD(MacroStageFixture,
                 "install_files uploads the pack, backs up printer.cfg, splices the include",
                 "[config][install][1271]") {
    hardware_.parse_objects(
        json::array({"gcode_macro START_PRINT", "gcode_macro CLEAN_NOZZLE", "bed_mesh"}));

    bool staged = false;
    std::optional<MoonrakerError> error;
    manager_.install_files([&] { staged = true; }, [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(error.has_value());
    REQUIRE(staged);

    // The macro pack landed whole.
    auto macros = api_.get_uploaded_config("helix_macros.cfg");
    REQUIRE(macros.has_value());
    CHECK(macros->find("[gcode_macro HELIX_CLEAN_NOZZLE]") != std::string::npos);

    // printer.cfg gained the include and kept its own sections.
    auto cfg = api_.get_uploaded_config("printer.cfg");
    REQUIRE(cfg.has_value());
    CHECK(cfg->find("[include helix_macros.cfg]") != std::string::npos);
    CHECK(cfg->find("[stepper_x]") != std::string::npos);

    // The pre-edit content survives on the printer as a timestamped sibling.
    REQUIRE(backup_count() == 1);
    for (const auto& [name, content] : config_files()) {
        if (name.rfind("printer.cfg.helixbak-", 0) == 0) {
            CHECK(content == kPrinterCfg);
        }
    }
}

TEST_CASE_METHOD(MacroStageFixture, "install_files writes no include and no backup when one exists",
                 "[config][install][1271]") {
    const std::string with_include = std::string("[include helix_macros.cfg]\n") + kPrinterCfg;
    api_.set_config_files({{"printer.cfg", with_include}});

    bool staged = false;
    std::optional<MoonrakerError> error;
    manager_.install_files([&] { staged = true; }, [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(error.has_value());
    REQUIRE(staged);
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("") == with_include);
    CHECK(backup_count() == 0);
}

TEST_CASE_METHOD(MacroStageFixture, "install_files stages the hidden signal names",
                 "[config][install]") {
    hardware_.parse_objects(json::array({"gcode_macro START_PRINT", "bed_mesh"}));

    manager_.install_files([] {}, [](const MoonrakerError&) {});
    settle();

    auto macros = api_.get_uploaded_config("helix_macros.cfg");
    REQUIRE(macros.has_value());
    CHECK(macros->find("[gcode_macro _HELIX_READY]") != std::string::npos);
    CHECK(macros->find("[gcode_macro HELIX_READY]") == std::string::npos);
}

TEST_CASE_METHOD(MacroStageFixture, "install_files keeps the visible names a printer has",
                 "[config][install]") {
    hardware_.parse_objects(json::array({"gcode_macro HELIX_READY", "bed_mesh"}));

    manager_.install_files([] {}, [](const MoonrakerError&) {});
    settle();

    auto macros = api_.get_uploaded_config("helix_macros.cfg");
    REQUIRE(macros.has_value());
    CHECK(macros->find("[gcode_macro HELIX_READY]") != std::string::npos);
    CHECK(macros->find("[gcode_macro _HELIX_READY]") == std::string::npos);
}

TEST_CASE_METHOD(MacroStageFixture, "update_files keeps the signal names the printer has",
                 "[config][install]") {
    SECTION("visible names stay visible") {
        hardware_.parse_objects(
            json::array({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT"}));

        manager_.update_files([] {}, [](const MoonrakerError&) {});
        settle();

        auto macros = api_.get_uploaded_config("helix_macros.cfg");
        REQUIRE(macros.has_value());
        CHECK(macros->find("[gcode_macro HELIX_READY]") != std::string::npos);
        CHECK(macros->find("[gcode_macro _HELIX_READY]") == std::string::npos);
    }

    SECTION("hidden names stay hidden") {
        hardware_.parse_objects(
            json::array({"gcode_macro _HELIX_READY", "gcode_macro HELIX_UNLOAD_FILAMENT"}));

        manager_.update_files([] {}, [](const MoonrakerError&) {});
        settle();

        auto macros = api_.get_uploaded_config("helix_macros.cfg");
        REQUIRE(macros.has_value());
        CHECK(macros->find("[gcode_macro _HELIX_READY]") != std::string::npos);
        CHECK(macros->find("[gcode_macro HELIX_READY]") == std::string::npos);
    }
}

TEST_CASE_METHOD(MacroStageFixture, "update_files replaces only the macro file",
                 "[config][install][1271]") {
    hardware_.parse_objects(
        json::array({"gcode_macro HELIX_READY", "gcode_macro HELIX_START_PRINT",
                     "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"}));

    bool staged = false;
    std::optional<MoonrakerError> error;
    manager_.update_files([&] { staged = true; }, [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(error.has_value());
    REQUIRE(staged);
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("") == kPrinterCfg);
    CHECK(backup_count() == 0);
    CHECK(api_.get_uploaded_config("helix_macros.cfg")
              .value_or("")
              .find("[gcode_macro HELIX_UNLOAD_FILAMENT]") != std::string::npos);
}

// The mock's transfer API completes synchronously on the calling thread, so
// the ONLY thing that can delay the continuation is the main-thread hop. If
// that hop is dropped, these callbacks run inline and the first CHECK fails.
TEST_CASE_METHOD(MacroStageFixture,
                 "staging and restart completions land via the main-thread queue",
                 "[config][install][1271][threading]") {
    hardware_.parse_objects(json::array({"gcode_macro START_PRINT", "bed_mesh"}));

    SECTION("install_files") {
        bool staged = false;
        manager_.install_files([&] { staged = true; }, [](const MoonrakerError&) {});
        CHECK_FALSE(staged); // still queued
        settle();
        CHECK(staged);
    }

    SECTION("update_files") {
        bool staged = false;
        manager_.update_files([&] { staged = true; }, [](const MoonrakerError&) {});
        CHECK_FALSE(staged);
        settle();
        CHECK(staged);
    }

    SECTION("install_files with the include already present") {
        // The fast path: download finds the include, skips backup and splice.
        // Its completion fires from the download callback, which runs INSIDE
        // the first drain (as the second hop of the chain) - so the wrap is
        // pinned by drain granularity: one pass may run the download, but the
        // completion must land on a LATER pass, not inline inside it.
        api_.set_config_files(
            {{"printer.cfg", std::string("[include helix_macros.cfg]\n") + kPrinterCfg}});
        bool staged = false;
        manager_.install_files([&] { staged = true; }, [](const MoonrakerError&) {});
        CHECK_FALSE(staged);
        helix::ui::UpdateQueue::instance().drain(); // runs the upload + download hops
        CHECK_FALSE(staged);                        // the completion itself must still be queued
        settle();
        CHECK(staged);
        CHECK(backup_count() == 0); // the fast path rewrites nothing
    }

    SECTION("request_restart") {
        bool done = false;
        manager_.request_restart([&] { done = true; }, [](const MoonrakerError&) {});
        CHECK_FALSE(done);
        settle();
        CHECK(done);
    }
}

// ============================================================================
// helix_skips.cfg staging
// ============================================================================

namespace {

using helix::skip_wrappers::Op;

const std::vector<Op> kVoronOps = {Op::BedMesh, Op::Qgl};

std::string first_line(const std::string& text) {
    return text.substr(0, text.find('\n'));
}

} // namespace

TEST_CASE_METHOD(MacroStageFixture,
                 "install_files stages helix_skips.cfg with its include on the first line",
                 "[config][install][skip_wrappers]") {
    hardware_.parse_objects(json::array({"gcode_macro START_PRINT", "bed_mesh"}));
    hardware_.set_skip_wrappers(kVoronOps, {});

    bool staged = false;
    std::optional<MoonrakerError> error;
    manager_.install_files([&] { staged = true; }, [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(error.has_value());
    REQUIRE(staged);
    CHECK(api_.get_uploaded_config("helix_skips.cfg").value_or("") ==
          helix::skip_wrappers::generate(kVoronOps));
    const std::string cfg = api_.get_uploaded_config("printer.cfg").value_or("");
    CHECK(first_line(cfg) == "[include helix_skips.cfg]");
    CHECK(cfg.find("[include helix_macros.cfg]") != std::string::npos);
    CHECK(cfg.find("[stepper_x]") != std::string::npos);
    CHECK(backup_count() == 1); // both includes in one edit, one backup
}

TEST_CASE_METHOD(MacroStageFixture, "install_files stages no skips file when nothing is wrappable",
                 "[config][install][skip_wrappers]") {
    hardware_.parse_objects(json::array({"gcode_macro START_PRINT"}));

    bool staged = false;
    manager_.install_files([&] { staged = true; }, [](const MoonrakerError&) {});
    settle();

    REQUIRE(staged);
    CHECK_FALSE(api_.get_uploaded_config("helix_skips.cfg").has_value());
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("").find("helix_skips") ==
          std::string::npos);
}

TEST_CASE_METHOD(MacroStageFixture, "update_files adds the skips to an install that predates them",
                 "[config][install][skip_wrappers]") {
    const std::string with_include = std::string("[include helix_macros.cfg]\n") + kPrinterCfg;
    api_.set_config_files({{"printer.cfg", with_include}});
    hardware_.parse_objects(json::array({"gcode_macro HELIX_READY", "bed_mesh"}));
    hardware_.set_skip_wrappers(kVoronOps, {});

    bool staged = false;
    manager_.update_files([&] { staged = true; }, [](const MoonrakerError&) {});
    settle();

    REQUIRE(staged);
    CHECK(api_.get_uploaded_config("helix_skips.cfg").value_or("") ==
          helix::skip_wrappers::generate(kVoronOps));
    const std::string cfg = api_.get_uploaded_config("printer.cfg").value_or("");
    CHECK(cfg == "[include helix_skips.cfg]\n" + with_include);
    CHECK(backup_count() == 1);
}

TEST_CASE_METHOD(MacroStageFixture, "uninstall removes both files and both includes",
                 "[config][install][skip_wrappers]") {
    api_.set_config_files(
        {{"printer.cfg",
          std::string("[include helix_skips.cfg]\n[include helix_macros.cfg]\n") + kPrinterCfg}});

    bool done = false;
    std::optional<MoonrakerError> error;
    manager_.uninstall([&] { done = true; }, [&](const MoonrakerError& err) { error = err; });
    settle();

    REQUIRE_FALSE(error.has_value());
    REQUIRE(done);
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("") == kPrinterCfg);
    const auto& deleted = api_.files_mock().deleted_files();
    CHECK(std::find(deleted.begin(), deleted.end(), "config/helix_macros.cfg") != deleted.end());
    CHECK(std::find(deleted.begin(), deleted.end(), "config/helix_skips.cfg") != deleted.end());
}

TEST_CASE_METHOD(MacroStageFixture, "remove_skips leaves the helper macros installed",
                 "[config][install][skip_wrappers]") {
    const std::string macros_only = std::string("[include helix_macros.cfg]\n") + kPrinterCfg;
    api_.set_config_files({{"printer.cfg", "[include helix_skips.cfg]\n" + macros_only}});

    bool done = false;
    manager_.remove_skips([&] { done = true; }, [](const MoonrakerError&) {});
    settle();

    REQUIRE(done);
    CHECK(api_.get_uploaded_config("printer.cfg").value_or("") == macros_only);
    CHECK(api_.files_mock().deleted_files() == std::vector<std::string>{"config/helix_skips.cfg"});
}

TEST_CASE_METHOD(MacroManagerTestFixture,
                 "MacroManager - a wrappable step without its wrapper reads as OUTDATED",
                 "[config][status][skip_wrappers]") {
    set_helix_macros_installed();
    REQUIRE(manager_.get_status() == MacroInstallStatus::INSTALLED);

    hardware_.set_skip_wrappers(kVoronOps, {});
    CHECK(manager_.get_status() == MacroInstallStatus::OUTDATED);

    hardware_.set_skip_wrappers(kVoronOps, {Op::BedMesh});
    CHECK(manager_.get_status() == MacroInstallStatus::OUTDATED);

    hardware_.set_skip_wrappers(kVoronOps, kVoronOps);
    CHECK(manager_.get_status() == MacroInstallStatus::INSTALLED);
}

TEST_CASE_METHOD(MacroStageFixture, "a commented-out include is neither counted nor mangled",
                 "[config][install][skip_wrappers]") {
    const std::string commented =
        std::string("#[include helix_skips.cfg]\n[include helix_macros.cfg]\n") + kPrinterCfg;

    SECTION("install still adds the live include") {
        api_.set_config_files({{"printer.cfg", commented}});
        hardware_.parse_objects(json::array({"gcode_macro START_PRINT", "bed_mesh"}));
        hardware_.set_skip_wrappers(kVoronOps, {});
        manager_.install_files([] {}, [](const MoonrakerError&) {});
        settle();
        CHECK(api_.get_uploaded_config("printer.cfg").value_or("") ==
              "[include helix_skips.cfg]\n" + commented);
    }
    SECTION("uninstall removes the live lines and keeps the commented one whole") {
        api_.set_config_files({{"printer.cfg", commented}});
        manager_.uninstall([] {}, [](const MoonrakerError&) {});
        settle();
        CHECK(api_.get_uploaded_config("printer.cfg").value_or("") ==
              std::string("#[include helix_skips.cfg]\n") + kPrinterCfg);
    }
}
