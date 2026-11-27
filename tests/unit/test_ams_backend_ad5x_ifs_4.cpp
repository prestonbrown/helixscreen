// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 116 to 143 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs][filament_slot_override], [ad5x_ifs][1631], [ad5x_ifs][1247],
// [ad5x_ifs][filament_slot_override][translucent]

#include "ui_insert_notice.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/unique_temp_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::kAmsDefaultSlotColor;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::StepOperationType;

using helix::test::Ad5xHomingGuardBackend;
using helix::test::Ad5xHomingGuardFixture;
using helix::test::Ad5xIfsTmpCacheDir;
using helix::test::Ad5xIfsTmpJsonFile;
using helix::test::Ad5xRunoutFixture;
using helix::test::Ad5xRunoutOpBackend;
using helix::test::Ad5xRunoutOpFixture;
using helix::test::boot_from_lane_data;
using helix::test::drain_queue;
using helix::test::firmware_states_slot0;
using helix::test::FrameRig;
using helix::test::GcodeCapturingBackend;
using helix::test::identity_tool_map;
using helix::test::IfsLaneTruth;
using helix::test::json;
using helix::test::kZmodMaterials;
using helix::test::kZmodPalette;
using helix::test::lane_record_reread_lands;
using helix::test::less_waste_vars;
using helix::test::lesswaste_frame;
using helix::test::lesswaste_ready;
using helix::test::LiveAckAd5xIfsBackend;
using helix::test::macro_changes_state;
using helix::test::macro_error;
using helix::test::make_auto_mirror_override;
using helix::test::make_extruder;
using helix::test::make_head_sensor;
using helix::test::make_ifs_frame;
using helix::test::make_ifs_vars_macro;
using helix::test::make_locked_override;
using helix::test::make_motion_sensor;
using helix::test::make_port_sensor;
using helix::test::make_routing_backend;
using helix::test::make_save_variables;
using helix::test::make_zcolor;
using helix::test::make_zmod_head_sensor;
using helix::test::make_zmod_motion_sensor;
using helix::test::material_button;
using helix::test::outside_lane_record;
using helix::test::outside_tool_change_zcolor;
using helix::test::palette_button;
using helix::test::plugin_rig_frame;
using helix::test::QuietRunningAd5xIfs;
using helix::test::RegisteredAd5xIfs;
using helix::test::RunningAd5xIfs;
using helix::test::seat_slot0;
using helix::test::seat_unrecorded_zmod;
using helix::test::seed_standard_colors;
using helix::test::slot_row;
using helix::test::standard_variables;
using helix::test::TestableAd5xIfsBackend;
using helix::test::unload_routed_to_toolhead;
using helix::test::WiredAd5xIfs;
using helix::test::wrap_notification;
using helix::test::zmod_change_frame;
using json = nlohmann::json;

// FilamentSlotOverride integration
//
// The override store is loaded once in on_started() and then layered over
// every parse via update_slot_from_state(). These tests exercise the layering
// directly by seeding the in-memory overrides map (via test access) and then
// driving the parse path — parse_adventurer_json is used because it goes
// through update_slot_from_state and is the simplest hook-free entry point
// from a test fixture constructed with nullptr api/client.

TEST_CASE("AD5X IFS partial override only replaces specified fields",
          "[ams][ad5x_ifs][filament_slot_override]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    // Seed an override that only sets brand. Every other field must fall
    // through to the firmware-reported value (or SlotInfo default), the same
    // as a slot with no override at all.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "Polymaker");   // override wins
    REQUIRE(info.color_rgb == 0xFF5500u); // firmware untouched
    REQUIRE(info.material == "PLA");      // firmware untouched
    REQUIRE(info.spool_name.empty());     // default
    REQUIRE(info.spoolman_id == 0);       // default
    // The override's own defaults are the "unset" sentinels (color_set false,
    // weights -1), so none of them replaces a firmware or SlotInfo value.
    REQUIRE(info.remaining_weight_g == -1.0f);
    REQUIRE(info.total_weight_g == -1.0f);
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS override applies to multiple slots independently",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    helix::ams::FilamentSlotOverride ovr0;
    ovr0.brand = "Polymaker";
    ovr0.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr0);

    helix::ams::FilamentSlotOverride ovr2;
    ovr2.brand = "eSUN";
    ovr2.color_rgb = 0x123456;
    ovr2.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 2, ovr2);

    // Slots 1 and 3 have NO override — must reflect pure firmware data.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmColor2": "#00FF00",
            "ffmColor3": "#0000FF",
            "ffmColor4": "#FFFFFF",
            "ffmType1": "PLA",
            "ffmType2": "PLA",
            "ffmType3": "PLA",
            "ffmType4": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // A stored record neither locks nor declares carries what the machine last
    // said, so the current frame outranks it wherever the two speak about the
    // same field. What survives is the identity firmware cannot carry at all -
    // the brand - and that is what makes each slot's record its own.
    auto info0 = backend.get_slot_info(0);
    REQUIRE(info0.brand == "Polymaker");
    REQUIRE(info0.material == "PLA");
    REQUIRE(info0.color_rgb == 0xFF0000u);

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.brand.empty());
    REQUIRE(info1.color_rgb == 0x00FF00u);
    REQUIRE(info1.material == "PLA");

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.brand == "eSUN");
    REQUIRE(info2.color_rgb == 0x0000FFu);
    REQUIRE(info2.material == "PLA");

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.brand.empty());
    REQUIRE(info3.color_rgb == 0xFFFFFFu);
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS override re-applied on every parse",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: firmware reports orange PLA.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    auto first = backend.get_slot_info(0);
    REQUIRE(first.brand == "Polymaker");
    REQUIRE(first.material == "PLA");

    // Second parse with the same firmware color but a different material. The
    // brand is what the record supplies on every parse and not just the first:
    // firmware cannot carry one, so nothing competes with it. The material is
    // the record remembering what firmware last said, so each frame restates
    // it. Deliberately keep the color stable - a firmware color change is the
    // physical-swap signal, covered by the hardware-swap cases below.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "ABS"}
    })");
    auto second = backend.get_slot_info(0);
    REQUIRE(second.brand == "Polymaker");
    REQUIRE(second.material == "ABS");
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS apply_user_edit takes effect when no override present",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // With no override seeded for the slot, set_slot_info's edit (every SlotInfo
    // field, not just color/material) must be visible via get_slot_info, including
    // brand / spool_name / spoolman_id / color_name carried through a persist=false
    // "preview" write. The parse path must not run with stale overrides_ state and
    // clobber the edit.
    // accidentally runs with stale overrides_ state and clobbers the edit.

    SlotInfo edit;
    edit.color_rgb = 0xAABBCC;
    edit.material = "PETG";
    edit.brand = "UserBrand";
    edit.spool_name = "UserSpool";
    edit.spoolman_id = 99;
    helix::test::apply_edit(backend, 0, edit);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCCu);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.brand == "UserBrand");
    REQUIRE(info.spool_name == "UserSpool");
    REQUIRE(info.spoolman_id == 99);
}

// apply_user_edit() writes through to FilamentSlotOverrideStore + in-memory
// overrides_ map so user edits survive subsequent parses.

TEST_CASE("AD5X IFS a translucency edit reaches the record, whose firmware states no alpha",
          "[ams][ad5x_ifs][filament_slot_override][translucent]") {
    Ad5xIfsTmpCacheDir tmp("translucent_edit");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::inject_override_store(
        backend, std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs"));

    SlotInfo edit;
    edit.material = "PETG";
    edit.color_rgb = 0xE8E8FF;
    edit.translucent = true;
    REQUIRE(helix::test::apply_edit(backend, 0, edit).success());
    const auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->translucent == std::optional<bool>(true));
}

TEST_CASE("AD5X IFS an edit's translucency outlasts its colour's echo; a later colour clears it",
          "[ams][ad5x_ifs][filament_slot_override][translucent]") {
    Ad5xIfsTmpCacheDir tmp("translucent_echo");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::inject_override_store(
        backend, std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs"));
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    SlotInfo edit = backend.get_slot_info(0);
    edit.material = "PETG";
    edit.color_rgb = 0xE8E8FF;
    edit.translucent = true;
    helix::test::edit_slot_as_user(backend, 0, edit);
    // The firmware's echo of the new colour states no alpha.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#E8E8FF", "ffmType1": "PETG"}
    })");
    auto record = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->color_rgb == 0xE8E8FFu);
    CHECK(record->translucent == std::optional<bool>(true));

    // A colour set on the printer's screen, no edit of ours behind it.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    record = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->color_rgb == 0x0055FFu);
    CHECK_FALSE(record->translucent.value_or(false));
}

TEST_CASE("AD5X IFS apply_user_edit stores override in memory and store",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Build a real MoonrakerAPIMock so the backend's override store has a
    // destination to write to. on_started() is not called — overrides_
    // starts empty — so we can assert the persist path populates it.
    Ad5xIfsTmpCacheDir tmp("task10_stores_override");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    // Native ZMOD path is skipped by marking has_ifs_vars_ true — this test
    // focuses on the override-store write, not the Klipper-facing side.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.spoolman_filament_id = 55;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = helix::test::apply_edit(backend, 0, edit);
    REQUIRE(err.success());

    // In-memory reads immediately see the edits — apply_overrides uses the
    // newly-staged override rather than any pre-edit value.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.spoolman_filament_id == 55);
    CHECK(info.remaining_weight_g == 850.0f);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);

    // overrides_ map was written under mutex_ as the persist staging step.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->spoolman_filament_id == 55);

    // Moonraker DB received the record via save_async (which MoonrakerAPIMock
    // dispatches synchronously in-call). Colour and type are the firmware's:
    // the record takes them from its echo, never from the edit.
    auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor_name"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"].is_null());
    CHECK(stored["color"].is_null());
}

// #981: update_slot_weight() is the consumption-sink's weight-only persist.
// It MUST update weight without re-asserting filament identity (material/
// color/locks) and MUST NOT rewrite Adventurer5M.json — the firmware-facing
// writers in apply_user_edit() reverted externally-set materials on every 60 s
// persist.

TEST_CASE("AD5X IFS apply_user_edit survives a matching firmware parse",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // After a persist=true write, the user's color/material round-trip through
    // Adventurer5M.json — so the next firmware parse reports the SAME values
    // and apply_overrides re-lays the brand/spool_name/etc. metadata that
    // doesn't live in firmware.
    //
    // Pre-color_set bug: this test fed mismatched firmware values (#000000
    // ABS) and asserted the override won. That worked only because the
    // OverwriteAlways auto-mirror's `observed_color == 0` short-circuit
    // dropped the firmware change. With color_set + the IFS fix, mismatched
    // firmware is correctly detected as an external edit and the user's
    // values are overwritten. That's the desired production behavior — but
    // it makes the prior test premise unrealistic. The realistic flow:
    // write_adventurer_json succeeds in production, firmware reports the
    // user's color back, no change detected, override metadata survives.
    Ad5xIfsTmpCacheDir tmp("task10_next_parse");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    helix::test::edit_slot_as_user(backend, 0, edit);

    // Simulate a subsequent firmware parse that mirrors the user's edit
    // (production: write_adventurer_json succeeded). The override's brand
    // (firmware can't carry brand) must still be visible after re-parse.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");   // override fills the firmware-can't-carry field
    CHECK(info.material == "PLA");      // matches both user + firmware
    CHECK(info.color_rgb == 0xFF5500u); // matches both user + firmware
}

TEST_CASE("AD5X IFS auto-mirror still tracks firmware for slots with no user lock",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Bootstrap (no override, or override with locks=false) MUST pick up genuine
    // external edits so OrcaSlicer's MoonrakerPrinterAgent stays in sync with the
    // printer (#965). Only user-locked slots are sticky; everything else tracks
    // firmware.
    Ad5xIfsTmpCacheDir tmp("ifs_bootstrap_tracks_firmware");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse: firmware reports orange PLA — bootstrap fills the
    // override via the mirror (locks stay false because this came from
    // auto-mirror, not apply_user_edit).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    // Second parse with different color: external edit (Mainsail / LCD)
    // — auto-mirror tracks it because no user lock is in effect.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x0055FFu);
    CHECK(info.material == "PETG");
}

TEST_CASE("AD5X IFS user edit does not file as firmware truth without a printer frame (#1631)",
          "[ams][ad5x_ifs][1631]") {
    // colors_[]/materials_[] and their last_firmware_* baselines are the
    // firmware side of the backend: only a printer frame may move them. An
    // edit routed through them repaints the VendorCache record with the
    // user's own values before any gcode has left the process, and once the
    // override is cleared the lane asserts the abandoned edit as what the
    // machine said (prestonbrown/helixscreen#1631).
    Ad5xIfsTmpCacheDir tmp("ifs_user_edit_not_vendor_cache");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Firmware's last word: lane 1 reads PETG / #00FF00.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");
    const auto lane = backend.lane_id(0);
    auto vendor = helix::ams::lane_sources(lane).firmware;
    REQUIRE(vendor.has_value());
    REQUIRE(vendor->color_rgb == 0x00FF00u);
    REQUIRE(vendor->material == "PETG");

    // The user edits lane 1 to PLA / #7EC8E3. No printer frame follows.
    SlotInfo edit;
    edit.color_rgb = 0x7EC8E3;
    edit.material = "PLA";
    helix::test::edit_slot_as_user(backend, 0, edit);

    // Firmware's record still states its own last reading.
    vendor = helix::ams::lane_sources(lane).firmware;
    REQUIRE(vendor.has_value());
    CHECK(vendor->color_rgb == 0x00FF00u);
    CHECK(vendor->material == "PETG");

    // The edit stands behind the lane as the user's declaration.
    const auto local = helix::ams::lane_sources(lane).pending;
    REQUIRE(local.has_value());
    CHECK(local->color_rgb == 0x7EC8E3u);
    CHECK(local->material == "PLA");

    // The matching echo — firmware republishing what the write carried —
    // must not revert the edit one frame later.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#7EC8E3", "ffmType1": "PLA"}
    })");
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x7EC8E3u);
    CHECK(info.material == "PLA");
}

TEST_CASE("AD5X IFS apply_user_edit writes the edited values to Adventurer5M.json (#1631)",
          "[ams][ad5x_ifs][1631]") {
    // The firmware writers must carry the edit's own values, not whatever
    // the firmware-truth arrays hold: a write sourced from the arrays would
    // upload the stale pre-edit reading right back to the printer.
    Ad5xIfsTmpCacheDir tmp("ifs_user_edit_json_carries_edit");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    const std::string json_path = (tmp.path / "Adventurer5M.json").string();
    {
        std::ofstream out(json_path);
        out << R"({"FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG",)"
            << R"( "ffmColor2": "#0000FF", "ffmType2": "ABS"}})";
    }
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);

    // Firmware truth as the printer last stated it.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG",
                    "ffmColor2": "#0000FF", "ffmType2": "ABS"}
    })");

    SlotInfo edit;
    edit.color_rgb = 0x7EC8E3;
    edit.material = "PLA";
    helix::test::edit_slot_as_user(backend, 0, edit);

    std::ifstream in(json_path);
    std::stringstream buf;
    buf << in.rdbuf();
    auto doc = json::parse(buf.str());
    REQUIRE(doc.contains("FFMInfo"));
    // The edited lane carries the edit, not the stale reading.
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#7EC8E3");
    CHECK(doc["FFMInfo"]["ffmType1"] == "PLA");
    // The read-modify-write leaves every other lane alone.
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#0000FF");
    CHECK(doc["FFMInfo"]["ffmType2"] == "ABS");
}

TEST_CASE("AD5X IFS apply_user_edit _IFS_VARS payload merges the edited lane with firmware's "
          "(#1631)",
          "[ams][ad5x_ifs][1631]") {
    // _IFS_VARS replaces the plugin's whole colors/types list, so the push
    // after an edit must splice the edited lane over the firmware-truth
    // arrays — every other lane keeps what the printer last said. lessWaste
    // shape: 16 tool-indexed entries, identity map until a remap is parsed.
    Ad5xIfsTmpCacheDir tmp("ifs_user_edit_vars_merge");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<GcodeCapturingBackend> backend_reg(&api, nullptr);
    GcodeCapturingBackend& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    const std::string json_path = (tmp.path / "Adventurer5M.json").string();
    {
        std::ofstream out(json_path);
        out << R"({"FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG",)"
            << R"( "ffmColor2": "#112233", "ffmType2": "ABS",)"
            << R"( "ffmColor3": "#445566", "ffmType3": "TPU",)"
            << R"( "ffmColor4": "#778899", "ffmType4": "PA"}})";
    }
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG",
                    "ffmColor2": "#112233", "ffmType2": "ABS",
                    "ffmColor3": "#445566", "ffmType3": "TPU",
                    "ffmColor4": "#778899", "ffmType4": "PA"}
    })");

    SlotInfo edit;
    edit.color_rgb = 0x7EC8E3;
    edit.material = "PLA";
    helix::test::edit_slot_as_user(backend, 0, edit);

    std::string expected_colors = "\"['7EC8E3', '112233', '445566', '778899'";
    std::string expected_types = "\"['PLA', 'ABS', 'TPU', 'PA'";
    for (int t = 4; t < 16; ++t) {
        expected_colors += ", ''";
        expected_types += ", ''";
    }
    expected_colors += "]\"";
    expected_types += "]\"";

    bool colors_ok = false;
    bool types_ok = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g == "_IFS_VARS colors=" + expected_colors)
            colors_ok = true;
        if (g == "_IFS_VARS types=" + expected_types)
            types_ok = true;
    }
    CHECK(colors_ok);
    CHECK(types_ok);
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS apply_user_edit with no store still updates in-memory map",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // Backend constructed with no api/client AND no injected store — the
    // persist path must still stage the override in memory so the current
    // UI session sees the edit, even though there's nowhere to save it.

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    auto err = helix::test::apply_edit(backend, 0, edit);
    // write_adventurer_json will fail with "No API connection" because api_
    // is nullptr. That's expected — but the in-memory override stage
    // happens BEFORE that write and must still be visible.
    (void)err;

    // Colour and type are the firmware's: the record takes them from its echo.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->material.empty());
    CHECK_FALSE(staged->color_set);
}

TEST_CASE("AD5X IFS apply_user_edit with pre-existing override replaces it",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Seed an old override (simulating a prior load from disk), then overwrite
    // it via apply_user_edit. get_slot_info must reflect the NEW values
    // immediately, not the old staged override.
    Ad5xIfsTmpCacheDir tmp("task10_replace");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride old;
    old.brand = "OldBrand";
    old.spool_name = "OldSpool";
    old.spoolman_id = 7;
    Ad5xIfsTestAccess::seed_override(backend, 0, old);

    // The user re-binds the lane to a different spool. The seeded record names
    // spool 7, so its whole identity is the server's word; re-binding states
    // the new id alone, and what spool 99 IS comes from the source that owns
    // it. The server speaks first, the way the spool picker has already
    // resolved a spool before the edit committing it can be saved.
    SlotInfo edit = backend.get_slot_info(0);
    edit.brand = "NewBrand";
    edit.spool_name = "NewSpool";
    edit.spoolman_id = 99;
    edit.material = "PLA";
    edit.color_rgb = 0xAABBCC;
    SpoolInfo spool;
    spool.id = 99;
    spool.vendor = "NewBrand";
    spool.filament_name = "NewSpool";
    spool.material = "PLA";
    spool.color_hex = "AABBCC";
    helix::test::spool_states(backend, 0, spool);
    helix::test::edit_slot_as_user(backend, 0, edit);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "NewBrand");
    CHECK(info.spool_name == "NewSpool");
    CHECK(info.spoolman_id == 99);

    // Staged override replaced cleanly.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "NewBrand");
    CHECK(staged->spoolman_id == 99);
}

// External color/material edits (Mainsail console, AD5X LCD, native zmod
// dialog, CHANGE_ZCOLOR from any non-Helix source) must REFRESH the Moonraker
// DB lane_data entry that Orca's MoonrakerPrinterAgent reads — they must NOT
// wipe the brand/spool_name/spoolman_id metadata (a "color change = physical
// swap" heuristic would leave lane_data stale after every external
// CHANGE_ZCOLOR).
// Initial startup observations are a baseline and never trigger a sync.
// Genuine spool removal is detected by the eject path (presence true→false
// in parse_adventurer_json) and clears the override there.
// in parse_adventurer_json) and clears the override there.

TEST_CASE("AD5X IFS GET_ZCOLOR eject keeps the override and lane_data (#1071)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Genuine spool removal. presence is owned solely by GET_ZCOLOR now: when
    // apply_zcolor_result reports a slot present->absent the lane renders empty,
    // but #1071: the override (brand/spool_name/spoolman) and the MR DB lane_data
    // row are RETAINED so a re-inserted same spool keeps its assignment —
    // matching the AFC and Happy Hare backends, which never clear the link on
    // empty. (Previously this drove clear_override_locked, which also deleted the
    // lane_data row via store->clear_async.)
    //
    // The sibling test "a lane going empty keeps its Spoolman link" covers the
    // override-retention without a store; this case additionally verifies the MR
    // DB lane_data row survives.
    Ad5xIfsTmpCacheDir tmp("eject_clears_override");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Loaded state: slot 0 present. (Establish presence explicitly — parse no
    // longer infers it.) Baseline color comes from the JSON parse.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Eject: GET_ZCOLOR reports slot 0 ABSENT. The present->absent transition in
    // apply_zcolor_result drops presence/status but RETAINS the override and the
    // MR DB lane_data entry (#1071).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty => absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // The lane renders removed (presence/status dropped)...
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.status != SlotStatus::LOADED);
        // ...but the Spoolman link survives the empty.
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
    }
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK_FALSE(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
}

// _IFS_VARS mirror: when an external CHANGE_ZCOLOR fires, parse_adventurer_json
// must mirror the new colors_/materials_ snapshot into the lessWaste/bambufy
// plugin's <prefix>_colors / <prefix>_types save_variables. Audited 2026-05-04
// against Hrybmo/lesswaste and function3d/bambufy: neither plugin self-syncs
// in response to CHANGE_ZCOLOR, so without this mirror the plugin's runout-
// recovery alternate-port lookup, smart-purge skip decision, and
// _IFS_COLORS_ASSIGN dialog all run against stale color data and silently
// print the wrong color or skip the wrong purge.

TEST_CASE("AD5X IFS external color change mirrors colors+types into _IFS_VARS",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_external");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // First parse establishes baseline — no sync, no mirror.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // External color change → sync fires, _IFS_VARS mirror dispatches.
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // Find the actual payloads — should reflect the new firmware truth.
    bool found_color_payload = false;
    bool found_type_payload = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.find("_IFS_VARS colors=") == 0 && g.find("'0055FF'") != std::string::npos)
            found_color_payload = true;
        if (g.find("_IFS_VARS types=") == 0 && g.find("'PETG'") != std::string::npos)
            found_type_payload = true;
    }
    CHECK(found_color_payload);
    CHECK(found_type_payload);

    // lessWaste prefix → no SHOW=0 suffix (lessWaste's _IFS_VARS doesn't
    // accept it — adding the param would break the macro call).
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS ", 0) == 0) {
            CHECK(g.find("SHOW=0") == std::string::npos);
        }
    }
}

TEST_CASE("AD5X IFS bambufy prefix gets SHOW=0 to suppress _IFS_VARS echo",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_bambufy_show0");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // Establish baseline + drive an external change.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // bambufy's _IFS_VARS macro accepts SHOW=0 to skip the RESPOND echo
    // (lesswaste does not). Both colors+types pushes get the suffix.
    bool colors_has_show0 = false;
    bool types_has_show0 = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS colors=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            colors_has_show0 = true;
        if (g.rfind("_IFS_VARS types=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            types_has_show0 = true;
    }
    CHECK(colors_has_show0);
    CHECK(types_has_show0);
}

// #1247: lessWaste's <prefix>_colors/_types save_variables are 16-entry
// TOOL-indexed arrays, but the HelixScreen mirror pushed 4-entry port-indexed
// lists — and _IFS_VARS replaces the arrays wholesale, so every push truncated
// them. _RUNOUT_HEAD's backup scan of tools 4..15 then read out of range and no
// backup spool could ever match (the "filament backup fails to switch" report).
// SAVE_VARIABLE persists the damage across reboots, so parse_save_variables
// must detect the truncated shape and dispatch a correctly-shaped repair push.
TEST_CASE("AD5X IFS repairs truncated lessWaste colors/types, leaves healthy state alone",
          "[ams][ad5x_ifs][1247]") {
    // The repair dispatch is captured by the execute_gcode override.
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    seed_standard_colors(backend);

    // Damaged frame: 4-entry colors/types (the truncation signature) beside a
    // healthy 16-entry tools map — exactly what an affected install persists.
    const json identity_tools = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    json damaged{{"less_waste_tools", identity_tools},
                 {"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                 {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})}};
    backend.fire_status(make_save_variables(damaged));

    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS types="));
    std::string expected_tail = "\"['FF0000', '00FF00', '0000FF', 'FFFFFF'";
    for (int i = 0; i < 12; ++i) {
        expected_tail += ", ''";
    }
    expected_tail += "]\"";
    bool repair_shapes_ok = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g == "_IFS_VARS colors=" + expected_tail) {
            repair_shapes_ok = true;
        }
    }
    REQUIRE(repair_shapes_ok);
    // lessWaste repair must not carry the bambufy-only SHOW=0 suffix.
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS ", 0) == 0) {
            CHECK(g.find("SHOW=0") == std::string::npos);
        }
    }

    // Healthy frame: full 16-entry arrays — no further repair dispatch.
    backend.captured_gcodes.clear();
    json healthy_colors = json::array();
    json healthy_types = json::array();
    for (int i = 0; i < 16; ++i) {
        healthy_colors.push_back(i < 4 ? "FF0000" : "");
        healthy_types.push_back(i < 4 ? "PLA" : "");
    }
    json healthy{{"less_waste_tools", identity_tools},
                 {"less_waste_colors", healthy_colors},
                 {"less_waste_types", healthy_types}};
    backend.fire_status(make_save_variables(healthy));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));
}

TEST_CASE("AD5X IFS no repair for bambufy 4-entry arrays (#1247)", "[ams][ad5x_ifs][1247]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

    // bambufy's arrays legitimately hold 4 port-indexed entries — its
    // _RUNOUT_HEAD iterates ifs.types (4) — so the truncation signature check
    // must not fire for that prefix.
    json frame{{"bambufy_tools", json::array({1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4})},
               {"bambufy_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
               {"bambufy_types", json::array({"PLA", "PETG", "ABS", "TPU"})}};
    backend.fire_status(make_save_variables(frame));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));
}

TEST_CASE("AD5X IFS mirror skipped when has_ifs_vars_ is false (stock zmod)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Stock zmod (no lessWaste/bambufy plugin) has no _IFS_VARS macro to call.
    // Sync still fires (lane_data still updates for Orca) but the mirror push
    // is skipped — calling _IFS_VARS on a printer without the macro just
    // produces a "Unknown command" gcode error.
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_stock_zmod");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, false); // stock zmod
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // Sync counter bumped (lane_data still sync'd via save_async).
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) > 0);
    // ...but no _IFS_VARS dispatched.
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS"));
}

// Bundle AQ6DALWG, "filament type changing on boot". On startup, the initial
// printer.objects.query response feeds parse_save_variables which iterates
// update_slot_from_state for every slot BEFORE Adventurer5M.json is fetched and
// parsed. At that point colors_[idx] is empty, so the firmware-color branch in
// update_slot_from_state is skipped and entry->info.color_rgb is whatever was
// last left there — initially the SlotInfo default (kAmsDefaultSlotColor /
// 0x808080), or a value leaked by a prior apply_overrides call.
//
// That must not populate last_firmware_color_ with a phantom 0x808080 baseline.
// Seconds later parse_adventurer_json arrives with the real firmware color
// (e.g. #898989); the diff against a phantom baseline would be misread as a
// physical spool swap and wipe the user override (brand, spoolman_id, weights,
// material), flipping the user's PETG spool back to firmware-truth PLA on every
// boot, with the next boot loading 0 overrides.
TEST_CASE("AD5X IFS empty colors_[] on boot does NOT establish phantom baseline",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("boot_phantom_baseline_no_clear");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg(&api);
    auto& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a saved override (PETG, brand, spoolman_id) — what the user
    // configured in a prior session and persisted into filament_slot store.
    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PETG"}, {"color", "#898989"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Gray";
    ovr.spoolman_id = 42;
    ovr.material = "PETG";
    ovr.color_rgb = 0x898989;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Boot path step 1: parse_save_variables fires from the initial
    // printer.objects.query response. lessWaste/bambufy tools data is
    // present, which triggers update_slot_from_state for every slot — but
    // colors_[idx] is still empty because Adventurer5M.json hasn't been
    // This is the call that must not establish a phantom
    //     // 0x808080 baseline.
    json save_vars;
    save_vars["less_waste_tools"] =
        json::array({0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}); // tool 0 -> port 1
    save_vars["less_waste_current_tool"] = -1;
    save_vars["less_waste_external"] = 0;
    backend.fire_status(make_save_variables(save_vars));

    // Override must still be intact after parse_save_variables — the helper
    // had no firmware-truth color so it MUST have skipped the swap check.
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
        CHECK(staged->spoolman_id == 42);
    }
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Boot path step 2: Adventurer5M.json finally arrives with the real
    // firmware color. This is the FIRST real firmware reading for the slot,
    // so it MUST establish the baseline rather than be misread as a swap.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#898989", "ffmType1": "PLA"}
    })");

    // Override survives: brand + spoolman_id still present. The material is
    // what firmware states.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.material == "PLA");
        CHECK(info.color_rgb == 0x898989u);
    }
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
    }
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x898989u);

    // The slot is now loaded (firmware reported a real color). In production the
    // post-parse GET_ZCOLOR establishes presence (parse_adventurer_json does not).
    // Seed it here so the SUBSEQUENT external-edit parse below takes the sync path.
    // (Seeding only now preserves the phantom-baseline guard checked above: the
    // empty-colors_ parse_vars in step 1 still must not clear the override.)
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // Boot path step 3: a SUBSEQUENT firmware parse with a different color
    // is an external edit — sync override + lane_data, KEEP brand metadata.
    // (The color delta is not a physical-swap signal: wiping the override would leave
    // lane_data stale.)
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.color_rgb == 0xFF5500u);
        // Colour and type follow firmware, which holds both.
        CHECK(info.material == "PLA");
    }
    auto staged3 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged3.has_value());
    CHECK(staged3->brand == "Polymaker");
    CHECK(staged3->color_rgb == 0xFF5500u);
    CHECK(staged3->material == "PLA");
}

TEST_CASE("AD5X IFS first firmware color observation does NOT clear override",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Even when the override's saved color differs from what the firmware
    // reports at startup, the very first observation is a BASELINE. This
    // matches real-world startup: override loaded from lane_data arrives
    // before firmware is polled; the colors may not match exactly
    // (rounding, scheme differences). We must not clear on first observation.
    Ad5xIfsTmpCacheDir tmp("task11_first_observation_no_clear");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500; // saved = orange
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a DIFFERENT color on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->spoolman_id == 42);
    }
    // DB entry preserved.
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // Steady state: a SECOND parse of the SAME firmware color that was used
    // to establish the baseline must ALSO not clear. This locks in the
    // invariant that the baseline-first-observation path doesn't leave
    // last_firmware_color_ in a weird state that fires on unchanged polls.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
}

// apply_user_edit must not wipe the override it just staged: the following
// update_slot_from_state -> check_external_color_change compares the user's new
// color against the prior firmware baseline, and must not treat that as an
// external edit.

TEST_CASE("AD5X IFS apply_user_edit does not wipe override on color edit",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Baseline firmware parse establishes last_firmware_color_.
    // Then user saves a new override with a DIFFERENT color.
    // The override must survive — not get treated as a hardware swap.
    Ad5xIfsTmpCacheDir tmp("task11_apply_user_edit_no_wipe");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse: color FF5500, no override — establishes baseline.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // User edits: sets NEW color + brand. This must not trigger a "physical swap"
    // clear although the new color (0x00FF00) differs from the baseline (0xFF5500).
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PLA";
    edit.brand = "Polymaker";
    helix::test::edit_slot_as_user(backend, 0, edit);

    // Assert override is present and unharmed.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.color_rgb == 0x00FF00u);
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK_FALSE(staged->color_set);

    // The baseline holds the printer's last reading: an edit is not a
    // firmware observation, so the baseline does not move until the echo
    // parse below reports the new color back.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);

    // Follow-up firmware parse with the NEW color should also not clear
    // (baseline now matches — no swap signal).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PLA"}
    })");
    auto staged2 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged2.has_value());
    CHECK(staged2->brand == "Polymaker");
}

TEST_CASE("AD5X IFS firmware color unchanged across parses does NOT clear",
          "[ams][ad5x_ifs][filament_slot_override]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(nullptr, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Parse twice with the same firmware color — no clear should fire on
    // either iteration (baseline then unchanged).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS firmware color change with no override creates a minimal one",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // No override seeded and no override store wired up — sync_override_to_
    // firmware_locked still creates the in-memory minimal override (and
    // skips save_async cleanly when override_store_ is null). This proves
    // the helper is null-safe and that lane_data publication doesn't gate
    // the in-memory sync.

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR. (Baseline parse never syncs — first observation.)
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // First parse establishes baseline only — no sync.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0xFF5500u);
        CHECK(info.brand.empty());
    }
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Second parse: external color change. Minimal override created (carries
    // color + material only; brand etc. left at defaults).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
        CHECK(info.brand.empty());
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");
    CHECK(staged->brand.empty());
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS nullopt firmware color does not update the baseline",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // nullopt is the explicit "no reading" signal (empty colors_[idx] —
    // parse hasn't filled yet, transient JSON race). Must not update the
    // baseline. If it did, an intermittent empty poll would mask a real
    // subsequent swap (the next non-empty poll would look like a transition
    // from a phantom baseline) OR worse, clear on every unread poll.

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports FF5500 — baseline established.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // No reading (nullopt). Must NOT update baseline and must NOT clear.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, std::nullopt);
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Firmware reports FF5500 again — still matches baseline, still no clear.
    // (If the no-reading signal had wrongly overwritten the baseline, this
    // would look like a transition and trigger a spurious sync.)
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS pure black (#000000) is a real reading, not a no-signal sentinel",
                 "[ams][ad5x_ifs][filament_slot_override]") {
    // External color edits to pure black via Mainsail console / native LCD /
    // CHANGE_ZCOLOR must not be dropped by an `observed_color == 0` skip conflating
    // black with "no reading". With std::optional, 0 is a real reading and an edit
    // FROM another color TO black must trigger sync_override_to_firmware_locked.

    SECTION("First observation = black establishes a black baseline") {
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);
    }

    SECTION("External edit FROM color TO black triggers sync") {
        // Establish baseline as red.
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF0000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u);

        // External edit changes color to pure black. Baseline must update.
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);

        // Override should now exist with the black firmware color
        // (sync_override_to_firmware_locked created/updated it). color_set
        // must be true so to_lane_data_record actually emits "color":"#000000".
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->color_rgb == 0u);
        CHECK(staged->color_set == true);
    }

    SECTION("Steady-state black does not churn") {
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        // Subsequent observations of identical black must NOT bump the
        // override (it->second == color short-circuit).
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);
        // First-observation rule applies: no sync was fired, so no override.
        CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    }
}

// Explicit clear_slot_override — user pressed "Clear slot metadata".
// Verifies the same clear pathway used by hardware-event detection is
// reachable through the public API with no swap signal required.

TEST_CASE("AD5X IFS clear_slot_override erases in-memory override and MR DB entry",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg(&api, nullptr);
    AmsBackendAd5xIfs& backend = *backend_reg;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed both halves of the override — lane_data on the Moonraker side
    // and the in-memory map on the backend side — so the clear has something
    // to remove at each layer.
    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.total_weight_g = 1000.0f;
    ovr.remaining_weight_g = 750.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Prime with a firmware parse so slots_ has an entry to reset. This also
    // establishes the last_firmware_color_ baseline — unrelated to the
    // clear_slot_override path but good sanity for the test.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.remaining_weight_g == 750.0f);
    }
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // User presses "Clear slot metadata". Override MUST be removed from both
    // layers — no swap signal needed.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    auto info = backend.get_slot_info(0);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.remaining_weight_g < 0.0f); // -1.0 sentinel ("unknown")
    CHECK(info.total_weight_g < 0.0f);
    CHECK(info.color_name.empty());
    // Firmware-sourced color flows through — clear only touches override-exclusive fields.
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");
}
