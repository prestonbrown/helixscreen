// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/mock_bypass.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"
#include "test_helpers/unique_temp_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: Initialize Config with a temp directory for isolated testing
// ============================================================================

namespace {

void settle_queue() {
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

/// Settings isolation plus a mock Moonraker whose database holds the bypass
/// record and whose Spoolman holds the active spool: commit_external_spool_edit
/// must be observable on both.
struct ExternalSpoolCommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    std::string temp_dir;
    std::string config_path;

    ExternalSpoolCommitFixture() : api(client, get_printer_state()) {
        temp_dir = helix::test::unique_temp_dir("helix_ext_spool_commit");
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Remove backup files: Config::init() restores from them when the
        // config file is missing.
        std::filesystem::remove(app_constants::update::config_backup_fallback());
        std::filesystem::remove(app_constants::update::legacy_config_backup_fallback());
        std::filesystem::remove(app_constants::update::env_backup_fallback());

        Config::get_instance()->init(config_path);
        AmsState::instance().clear_backends();
        AmsState::instance().set_moonraker_api(&api);
    }

    ~ExternalSpoolCommitFixture() override {
        // Detach the mock BEFORE members are destroyed and while LVGL still
        // runs (base-class teardown has not happened yet).
        settle_queue();
        AmsState::instance().set_moonraker_api(nullptr);
        Config::get_instance()->clear_path();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }
};

} // namespace

namespace {
/// Drain until quiet: a record write's success queues a read, and the read's
/// answer queues its filing.
void settle() {
    settle_queue();
}
} // namespace

// ============================================================================
// The bypass record round trip (no filament backend: AmsState's own store)
// ============================================================================

TEST_CASE("get_external_spool_info is empty with no bypass record", "[external_spool][bypass]") {
    ExternalSpoolCommitFixture fixture;
    settle();
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());
}

TEST_CASE("set_external_spool_info round-trips every field through the bypass record",
          "[external_spool][bypass]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo info;
    info.slot_index = 5; // not the sentinel; the record reads back as -2
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    info.brand = "eSUN";
    info.nozzle_temp_min = 210;
    info.nozzle_temp_max = 210;
    info.bed_temp = 60;
    info.spoolman_id = 42;
    info.spool_name = "My Spool";
    info.remaining_weight_g = 450;
    info.total_weight_g = 1000;
    info.catalog_id = "sunlu-pla-plus-2-0";
    info.product_name = "PLA+ 2.0";

    AmsState::instance().set_external_spool_info(info);
    settle();
    REQUIRE_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).pending.has_value());

    auto result = AmsState::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->slot_index == -2);
    CHECK(result->global_index == -2);
    CHECK(result->color_rgb == 0xFF0000);
    CHECK(result->material == "PLA");
    CHECK(result->brand == "eSUN");
    CHECK(result->nozzle_temp_min == 210);
    CHECK(result->bed_temp == 60);
    CHECK(result->spoolman_id == 42);
    CHECK(result->spool_name == "My Spool");
    CHECK(result->remaining_weight_g == Catch::Approx(450.0f));
    CHECK(result->total_weight_g == Catch::Approx(1000.0f));
    // The catalog pick rides the record too, or the editor reopens on the
    // wrong variant.
    CHECK(result->catalog_id == "sunlu-pla-plus-2-0");
    CHECK(result->product_name == "PLA+ 2.0");
}

TEST_CASE("a black bypass spool is a colour, not an empty record", "[external_spool][bypass]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo info;
    info.color_rgb = 0x000000;
    info.material = "PLA";
    AmsState::instance().set_external_spool_info(info);
    settle();

    auto result = AmsState::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x000000);
    CHECK(fixture.client.mock_db_get("lane_data", "lane1")["color"] == "#000000");
}

TEST_CASE("clear_external_spool_info deletes the bypass record", "[external_spool][bypass]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    AmsState::instance().set_external_spool_info(info);
    settle();
    REQUIRE(AmsState::instance().get_external_spool_info().has_value());

    AmsState::instance().clear_external_spool_info();
    settle();
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
    CHECK(fixture.client.mock_db_get("lane_data", "lane1").is_null());
}

TEST_CASE("AmsState external_spool_color subject updates on set", "[external_spool][ams_state]") {
    ExternalSpoolCommitFixture fixture;
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
    ExternalSpoolCommitFixture fixture;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

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

    // S5 — written as the bypass record
    const auto persisted = fixture.client.mock_db_get("lane_data", "lane1");
    REQUIRE(persisted.is_object());
    CHECK(persisted["spool_id"] == 169);
    CHECK(persisted["filament_type"] == "PLA");
    // S1 — server told which spool is active
    REQUIRE(fixture.client.spoolman_mock().get_mock_active_spool_id() == 169);
}

TEST_CASE("commit_external_spool_edit empty arm erases settings record",
          "[external-spool][commit]") {
    ExternalSpoolCommitFixture fixture;

    // Seed an assigned external spool (id 169) and make the server agree.
    SlotInfo seeded;
    seeded.spoolman_id = 169;
    seeded.material = "PLA";
    AmsState::instance().set_external_spool_info(seeded);
    settle();
    fixture.client.spoolman_mock().set_active_spool_id(169);
    REQUIRE(fixture.client.spoolman_mock().get_mock_active_spool_id() == 169);

    AmsState::instance().commit_external_spool_edit(SlotInfo{});
    helix::ui::UpdateQueue::instance().drain();

    // S5 — the record is deleted, not written empty
    REQUIRE(fixture.client.mock_db_get("lane_data", "lane1").is_null());
    REQUIRE_FALSE(AmsState::instance().get_external_spool_info().has_value());
    // S1 — the server-side link was cleared too
    REQUIRE(fixture.client.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("commit_external_spool_edit keeps manual entry without spoolman id",
          "[external-spool][commit]") {
    ExternalSpoolCommitFixture fixture;

    // Sentinel: any set_active_spool call from the commit lands here and fails
    // the final check (7 stays 7 only if NO call fired).
    fixture.client.spoolman_mock().set_active_spool_id(7);

    SlotInfo info;
    info.spoolman_id = 0;
    info.material = "PLA";
    info.color_rgb = 0x00FF00;

    AmsState::instance().commit_external_spool_edit(info);

    // S5 — manual entry (material set, no spoolman link) still persists
    const auto persisted = fixture.client.mock_db_get("lane_data", "lane1");
    REQUIRE(persisted.is_object());
    CHECK(persisted["spool_id"].is_null());
    CHECK(persisted["filament_type"] == "PLA");
    // No API call — a manual entry is not a server-side spool assignment
    CHECK(fixture.client.spoolman_mock().get_mock_active_spool_id() == 7);
}

// ============================================================================
// The bypass lane (#1632): the external spool's Spoolman record and meter file
// onto kBypassLaneId, and get_external_spool_info() reads through resolve().
// ============================================================================

namespace {

SpoolInfo server_spool_1() {
    SpoolInfo spool;
    spool.id = 1;
    spool.vendor = "Polymaker";
    spool.filament_name = "Jet Black";
    spool.material = "PLA";
    spool.remaining_weight_g = 850.0;
    spool.initial_weight_g = 1000.0;
    return spool;
}

} // namespace

TEST_CASE("the bypass lane's Spoolman record owns the external spool's shown weight",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    // The raw store holds the binding and the meter's count of it.
    SlotInfo raw;
    raw.spoolman_id = 1;
    raw.material = "PLA";
    raw.remaining_weight_g = 400.0f;
    raw.total_weight_g = 1000.0f;
    AmsState::instance().set_external_spool_info(raw);
    settle(); // the record echoes; no edit is in flight

    // The server's answer, filed the way the poll files it.
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, server_spool_1()));

    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->remaining_weight_g == 850.0f);
    CHECK(shown->total_weight_g == 1000.0f);
    CHECK(shown->spoolman_id == 1);
    // Identity rides the same record: the brand is the server's, not the
    // binding stub's blank.
    CHECK(shown->brand == "Polymaker");
}

TEST_CASE("the external spool's meter outranks a stale local weight on the bypass lane",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    // A manual entry: no spool, a weight the user stated when they edited.
    SlotInfo manual;
    manual.material = "PLA";
    manual.remaining_weight_g = 500.0f;
    manual.total_weight_g = 1000.0f;
    AmsState::instance().commit_external_spool_edit(manual);
    settle(); // the record echoes; no edit is in flight

    // The consumption sink meters the print, filing what it wrote.
    helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
    metered.remaining_weight_g = 490.0f;
    metered.total_weight_g = 1000.0f;
    helix::ams::ingest(helix::ams::kBypassLaneId, metered);

    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->remaining_weight_g == 490.0f);
}

TEST_CASE("a weight edit on the external spool reaches observers as the edited weight",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;
    AmsState::instance().init_subjects(true);

    // A manual entry the user already declared a weight for.
    SlotInfo manual;
    manual.material = "PLA";
    manual.remaining_weight_g = 0.0f;
    manual.total_weight_g = 1000.0f;
    AmsState::instance().commit_external_spool_edit(manual);
    REQUIRE(helix::ams::lane_sources(helix::ams::kBypassLaneId).pending.has_value());

    // Displays re-read the resolved record when the colour subject fires, and
    // never again until it fires next, so what they read then is what stays on screen.
    static float seen_weight = -1.0f;
    seen_weight = -1.0f;
    lv_observer_t* observer = lv_subject_add_observer(
        AmsState::instance().get_external_spool_color_subject(),
        [](lv_observer_t*, lv_subject_t*) {
            if (auto ext = AmsState::instance().get_external_spool_info()) {
                seen_weight = ext->remaining_weight_g;
            }
        },
        nullptr);

    SlotInfo edited = manual;
    edited.remaining_weight_g = 100.0f;
    AmsState::instance().commit_external_spool_edit(edited);
    lv_observer_remove(observer);

    REQUIRE(AmsState::instance().get_external_spool_info()->remaining_weight_g == 100.0f);
    CHECK(seen_weight == 100.0f);
}

TEST_CASE("re-linking the external spool drops the previous spool's lane record",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo first;
    first.spoolman_id = 1;
    first.material = "PLA";
    AmsState::instance().commit_external_spool_edit(first);
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, server_spool_1()));
    REQUIRE(AmsState::instance().get_external_spool_info()->remaining_weight_g == 850.0f);

    SlotInfo second;
    second.spoolman_id = 2;
    second.material = "PETG";
    second.remaining_weight_g = 600.0f;
    second.total_weight_g = 750.0f;
    AmsState::instance().commit_external_spool_edit(second);

    // Spool 1's record describes a spool that is no longer bound; it must not
    // paint spool 2's display.
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->spoolman_id == 2);
    CHECK(shown->remaining_weight_g == 600.0f);
}

TEST_CASE("re-binding through the sync funnel drops the previous spool's pick and meter",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    // Spool 1 bound and polled: its record stands, the user picked a colour
    // for it, and a meter count from the last print is still on the lane.
    SlotInfo first;
    first.spoolman_id = 1;
    first.material = "PLA";
    first.remaining_weight_g = 400.0f;
    first.total_weight_g = 1000.0f;
    AmsState::instance().set_external_spool_info(first);
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, server_spool_1()));

    helix::ams::Observation pick(helix::ams::ObservationSource::Pending);
    pick.color_rgb = 0x00FF00;
    pick.color_name = "Picked for spool 1";
    helix::ams::file_pending(helix::ams::kBypassLaneId, pick);

    helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
    metered.remaining_weight_g = 490.0f;
    helix::ams::ingest(helix::ams::kBypassLaneId, metered);

    // The setup reached the branch: the pick outranks the spool's colour, and
    // the meter's count is on the lane.
    const auto before = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    REQUIRE(before.pending.has_value());
    REQUIRE(before.metered.has_value());
    REQUIRE(AmsState::instance().get_external_spool_info()->color_rgb == 0x00FF00);

    // Another client sets the active spool: the notify handler's write, which
    // reaches the lane through this same setter.
    SlotInfo second;
    second.spoolman_id = 2;
    second.material = "PETG";
    second.color_rgb = 0xFF0000;
    second.remaining_weight_g = 600.0f;
    second.total_weight_g = 750.0f;
    AmsState::instance().set_external_spool_info(second);

    // All three of spool 1's records are gone, so nothing of it paints spool 2.
    const auto sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    CHECK_FALSE(sources.spoolman.has_value());
    CHECK_FALSE(sources.metered.has_value());
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->spoolman_id == 2);
    CHECK(shown->color_rgb == 0xFF0000);
    CHECK(shown->color_name.empty());
    CHECK(shown->remaining_weight_g == 600.0f);
}

TEST_CASE("a Spoolman record naming a different spool than the binding is not resolved",
          "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    // The binding names spool 2; spool 1's record arrives on the lane after
    // that, the shape a late poll answer or a restored config leaves behind.
    SlotInfo rebound;
    rebound.spoolman_id = 2;
    rebound.material = "PETG";
    rebound.remaining_weight_g = 600.0f;
    rebound.total_weight_g = 750.0f;
    AmsState::instance().set_external_spool_info(rebound);
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, server_spool_1()));

    // Spool 1's weight and brand must not paint spool 2's record.
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->spoolman_id == 2);
    CHECK(shown->remaining_weight_g == 600.0f);
    CHECK(shown->brand.empty());
}

TEST_CASE("clearing the external spool resets the bypass lane", "[external_spool][1632]") {
    ExternalSpoolCommitFixture fixture;

    SlotInfo linked;
    linked.spoolman_id = 1;
    linked.material = "PLA";
    AmsState::instance().commit_external_spool_edit(linked);
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, server_spool_1()));

    AmsState::instance().commit_external_spool_edit(SlotInfo{});

    const auto sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    CHECK_FALSE(sources.spoolman.has_value());
    CHECK_FALSE(sources.pending.has_value());
    CHECK_FALSE(AmsState::instance().get_external_spool_info().has_value());
}

// ============================================================================
// active_spool_describes_bypass() — the guard both arms of Application's
// notify_active_spool_set handler consult before writing the bypass record.
// ============================================================================

TEST_CASE("active_spool_describes_bypass: true with no AMS at all",
          "[external-spool][bypass-guard]") {
    ExternalSpoolCommitFixture fixture;
    AmsState::instance().clear_backends();

    // Nothing else can own Moonraker's global active spool, so it describes the
    // bypass by elimination. This is the original startup-sync behaviour and it
    // must survive the guard.
    CHECK(AmsState::instance().active_spool_describes_bypass());
}

TEST_CASE("active_spool_describes_bypass: false when an AMS is feeding from a lane",
          "[external-spool][bypass-guard]") {
    ExternalSpoolCommitFixture fixture;

    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_operation_delay(0);
    AmsState::instance().set_backend(std::move(mock));
    REQUIRE_FALSE(AmsState::instance().any_bypass_active());

    // THE regression. An AMS slot edit sets Moonraker's global active spool, so
    // the notification that comes back describes a LANE. Answering "yes" here
    // is what let assigning to lane 3 overwrite the bypass, and what let
    // clearing lane 3 (which posts set_active_spool(0)) erase it entirely.
    CHECK_FALSE(AmsState::instance().active_spool_describes_bypass());

    AmsState::instance().clear_backends();
}

TEST_CASE("active_spool_describes_bypass: true when the AMS is feeding from its bypass",
          "[external-spool][bypass-guard]") {
    ExternalSpoolCommitFixture fixture;

    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_operation_delay(0);
    AmsBackendMock* raw = mock.get();
    AmsState::instance().set_backend(std::move(mock));
    REQUIRE(raw->start());
    REQUIRE(helix::test::unload_for_bypass(*raw));
    REQUIRE(raw->enable_bypass().success());
    REQUIRE(AmsState::instance().any_bypass_active());

    // The bypass IS the path in use, so the global active spool really is its
    // spool — a user swapping the bypass spool in Spoolman's web UI must still
    // reach the bypass record.
    CHECK(AmsState::instance().active_spool_describes_bypass());

    AmsState::instance().clear_backends();
}
