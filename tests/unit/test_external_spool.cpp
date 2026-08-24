// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "settings_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: Initialize Config with a temp directory for isolated testing
// ============================================================================

namespace {

struct TempConfigFixture : public HelixTestFixture {
    std::string temp_dir;
    std::string config_path;

    TempConfigFixture() {
        temp_dir = std::filesystem::temp_directory_path().string() + "/helix_ext_spool_test_" +
                   std::to_string(rand());
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Remove backup files to prevent cross-test contamination.
        // Config::init() restores from backups when the config file is missing,
        // so stale backup data from a previous test run can leak into this test.
        std::filesystem::remove(AppConstants::Update::config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::env_backup_fallback());

        // Initialize Config singleton with temp path
        Config::get_instance()->init(config_path);

        // Clear any external spool state leaked from other tests in this
        // shard. AmsState's clear also resets the in-memory override, killing
        // an order dependence on which test last wrote it.
        AmsState::instance().clear_external_spool_info();
    }

    ~TempConfigFixture() {
        // Clear the Config singleton's path BEFORE removing temp_dir so any
        // subsequent test that triggers Config::save() doesn't try to write
        // to a now-deleted directory and fail. A failed save inside e.g.
        // TelemetryManager::set_enabled(true) calls CONFIG_RECORD_ERROR
        // which enqueues a phantom telemetry event in the next test's
        // queue, breaking queue-size assertions.
        Config::get_instance()->clear_path();
        std::filesystem::remove_all(temp_dir);
    }
};

/// TempConfigFixture's settings isolation plus the mock-API wiring shape of
/// CommitFixture (test_ams_state_commit_slot.cpp) — commit_external_spool_edit
/// must be observable on BOTH stores (settings.json and the Spoolman server).
struct ExternalSpoolCommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    std::string temp_dir;
    std::string config_path;

    ExternalSpoolCommitFixture() : api(client, get_printer_state()) {
        temp_dir = std::filesystem::temp_directory_path().string() + "/helix_ext_spool_commit_" +
                   std::to_string(rand());
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Same cross-test contamination guard as TempConfigFixture.
        std::filesystem::remove(AppConstants::Update::config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::env_backup_fallback());

        Config::get_instance()->init(config_path);
        // AmsState's clear resets the in-memory override too, not just the
        // settings record (same cross-test guard as TempConfigFixture).
        AmsState::instance().clear_external_spool_info();

        AmsState::instance().set_moonraker_api(&api);
    }

    ~ExternalSpoolCommitFixture() override {
        // Detach the mock BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        AmsState::instance().set_moonraker_api(nullptr);
        Config::get_instance()->clear_path();
        std::filesystem::remove_all(temp_dir);
    }
};

} // namespace

// ============================================================================
// Step 1: SettingsManager external spool persistence
// ============================================================================

TEST_CASE("get_external_spool_info returns empty default when not set",
          "[external_spool][settings]") {
    TempConfigFixture fixture;

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("set_external_spool_info stores and retrieves data", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    info.brand = "eSUN";
    info.nozzle_temp_min = 200;
    info.nozzle_temp_max = 220;
    info.bed_temp = 60;
    info.spoolman_id = 42;
    info.spool_name = "My Spool";
    info.remaining_weight_g = 450;
    info.total_weight_g = 1000;
    info.catalog_id = "sunlu-pla-plus-2-0";
    info.product_name = "PLA+ 2.0";

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xFF0000);
    CHECK(result->material == "PLA");
    CHECK(result->brand == "eSUN");
    CHECK(result->nozzle_temp_min == 200);
    CHECK(result->nozzle_temp_max == 220);
    CHECK(result->bed_temp == 60);
    CHECK(result->spoolman_id == 42);
    CHECK(result->spool_name == "My Spool");
    CHECK(result->remaining_weight_g == Catch::Approx(450.0f));
    CHECK(result->total_weight_g == Catch::Approx(1000.0f));
    // The external spool has no lane_data record — this get/set pair is its
    // ONLY persistence, so the catalog product identity has to round-trip here
    // or the external-spool editor reopens on the wrong variant.
    CHECK(result->catalog_id == "sunlu-pla-plus-2-0");
    CHECK(result->product_name == "PLA+ 2.0");
}

TEST_CASE("set_external_spool_info persists across config reload", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0x00FF00;
    info.material = "PETG";
    info.brand = "Polymaker";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    info.spoolman_id = 99;
    info.spool_name = "Test Spool";
    info.remaining_weight_g = 800;
    info.total_weight_g = 1000;

    settings.set_external_spool_info(info);

    // Reload config from disk
    Config::get_instance()->init(fixture.config_path);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x00FF00);
    CHECK(result->material == "PETG");
    CHECK(result->brand == "Polymaker");
    CHECK(result->spoolman_id == 99);
}

TEST_CASE("clear_external_spool_info removes stored data", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    settings.set_external_spool_info(info);
    REQUIRE(settings.get_external_spool_info().has_value());

    settings.clear_external_spool_info();
    REQUIRE_FALSE(settings.get_external_spool_info().has_value());
}

TEST_CASE("external spool slot_index is always -2", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.slot_index = 5; // Pass in a non-sentinel value
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->slot_index == -2);
    CHECK(result->global_index == -2);
}

TEST_CASE("get_external_spool_info with assigned=true returns spool even with black color",
          "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0x000000; // Black — previously could fail with -1 sentinel
    info.material = "PLA";

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x000000);
    CHECK(result->material == "PLA");
}

TEST_CASE("backward compat: old config without assigned key but with color_rgb",
          "[external_spool][settings]") {
    TempConfigFixture fixture;

    // Manually write old-format config (no "assigned" key, but with color_rgb).
    // Set in-memory only (no save) to avoid contaminating the global backup file.
    Config* config = Config::get_instance();
    std::string prefix = config->df();
    config->set<int>(prefix + "filament/external_spool/color_rgb", 0xFF0000);
    config->set<std::string>(prefix + "filament/external_spool/material", "PETG");

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xFF0000);
    CHECK(result->material == "PETG");
}

// ============================================================================
// Step 2: AmsState external spool subject and get/set
// ============================================================================

TEST_CASE("AmsState get_external_spool_info delegates to SettingsManager",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xAABBCC;
    info.material = "ABS";
    info.brand = "Hatchbox";
    settings.set_external_spool_info(info);

    auto result = AmsState::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xAABBCC);
    CHECK(result->material == "ABS");
    CHECK(result->brand == "Hatchbox");
}

TEST_CASE("AmsState set_external_spool_info writes to SettingsManager",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;

    SlotInfo info;
    info.color_rgb = 0x112233;
    info.material = "TPU";
    info.brand = "NinjaTek";

    AmsState::instance().set_external_spool_info(info);

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x112233);
    CHECK(result->material == "TPU");
    CHECK(result->brand == "NinjaTek");
}

TEST_CASE("AmsState external_spool_color subject updates on set", "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& ams = AmsState::instance();
    ams.init_subjects(false); // false = skip XML registration (no LVGL display)

    SlotInfo info;
    info.color_rgb = 0xDDEEFF;
    info.material = "PLA";

    ams.set_external_spool_info(info);

    int color = lv_subject_get_int(ams.get_external_spool_color_subject());
    CHECK(color == static_cast<int>(0xDDEEFF));
}

TEST_CASE("AmsState external_spool_color subject defaults to 0 when no spool",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // Clear any state from previous tests (singleton persists across test cases)
    ams.clear_external_spool_info();

    int color = lv_subject_get_int(ams.get_external_spool_color_subject());
    CHECK(color == 0);
}

// ============================================================================
// Step 3: commit_external_spool_edit (single authority for external spool writes)
// ============================================================================

TEST_CASE("commit_external_spool_edit set arm syncs active spool and persists",
          "[external-spool][commit]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo info;
    info.spoolman_id = 169;
    info.material = "PLA";
    info.color_rgb = 0xFF8800;

    AmsState::instance().commit_external_spool_edit(info);

    // S5 — persisted via SettingsManager
    auto persisted = SettingsManager::instance().get_external_spool_info();
    REQUIRE(persisted.has_value());
    CHECK(persisted->spoolman_id == 169);
    CHECK(persisted->material == "PLA");
    // S1 — server told which spool is active
    REQUIRE(fixture.api.spoolman_mock().get_mock_active_spool_id() == 169);
}

TEST_CASE("commit_external_spool_edit empty arm erases settings record",
          "[external-spool][commit]") {
    ExternalSpoolCommitFixture fixture;

    // Seed an assigned external spool (id 169) and make the server agree.
    SlotInfo seeded;
    seeded.spoolman_id = 169;
    seeded.material = "PLA";
    AmsState::instance().set_external_spool_info(seeded);
    fixture.api.spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(fixture.api.spoolman_mock().get_mock_active_spool_id() == 169);

    AmsState::instance().commit_external_spool_edit(SlotInfo{});

    // S5 — the settings subtree is ABSENT, not an empty assigned=true record
    REQUIRE_FALSE(SettingsManager::instance().get_external_spool_info().has_value());
    // S1 — the server-side link was cleared too
    REQUIRE(fixture.api.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("commit_external_spool_edit keeps manual entry without spoolman id",
          "[external-spool][commit]") {
    ExternalSpoolCommitFixture fixture;

    // Sentinel: any set_active_spool call from the commit lands here and fails
    // the final check (7 stays 7 only if NO call fired).
    fixture.api.spoolman_mock().set_active_spool(7, nullptr, nullptr);

    SlotInfo info;
    info.spoolman_id = 0;
    info.material = "PLA";
    info.color_rgb = 0x00FF00;

    AmsState::instance().commit_external_spool_edit(info);

    // S5 — manual entry (material set, no spoolman link) still persists
    auto persisted = SettingsManager::instance().get_external_spool_info();
    REQUIRE(persisted.has_value());
    CHECK(persisted->spoolman_id == 0);
    CHECK(persisted->material == "PLA");
    // No API call — a manual entry is not a server-side spool assignment
    CHECK(fixture.api.spoolman_mock().get_mock_active_spool_id() == 7);
}
