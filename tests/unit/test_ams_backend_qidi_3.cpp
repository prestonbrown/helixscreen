// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 88 to 120 of test_ams_backend_qidi.cpp before it was split, in its order. Mostly [general],
// [qidi_string_ids], [homing_guard], [lane], [qidi][dryer][capability], and more

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_backend_qidi_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/zero_lane_numbering.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_device_action_value.h"
#include "ams_error.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/temperature_units.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "notice.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsBackendQidi;
using helix::AmsType;
using helix::DryerInfo;
using helix::kAmsDefaultSlotColor;
using helix::PathTopology;
using helix::QidiBoxTestAccess;
using helix::SlotInfo;
using helix::SlotStatus;
using json = nlohmann::json;

using helix::test::clear_as_the_menu_does;
using helix::test::kControllerGenericPla;
using helix::test::kStockFilasExcerpt;
using helix::test::make_controller_slot_notification;
using helix::test::offers_try_again;
using helix::test::PausedPrint;
using helix::test::QidiHomingGuardFixture;
using helix::test::QidiTmpCacheDir;
using helix::test::RecordingQidiBackend;
using helix::test::RecordingQidiWithApi;

TEST_CASE_METHOD(QidiHomingGuardFixture,
                 "QIDI load/unload/change_tool refuse while PRINTING, proceed while PAUSED",
                 "[ams][qidi_box][homing_guard]") {
    auto check_refused = [](helix::AmsError err, const std::vector<std::string>& sent,
                            const std::string& expected_msg) {
        CHECK_FALSE(err.success());
        CHECK(err.result == helix::AmsResult::WRONG_STATE);
        CHECK(err.user_msg == expected_msg);
        CHECK(sent.empty());
    };

    SECTION("PRINTING blocks all three ops") {
        const std::string msg = "Cannot run filament operation while printing";
        set_print_state(helix::PrintJobState::PRINTING);
        check_refused(backend->load_filament(2), backend->sent, msg);
        check_refused(backend->unload_filament(1), backend->sent, msg);
        check_refused(backend->change_tool(0), backend->sent, msg);
        // QIDI does not self-home, so pausing is a recovery it can honour —
        // the copy says so rather than "finish or cancel the print".
        CHECK(backend->load_filament(2).suggestion.find("Pause the print") != std::string::npos);
    }

    SECTION("PAUSED permits all three ops — QIDI does not self-home") {
        // Pause-then-swap is the runout / colour-change recovery workflow, and
        // nothing QIDI dispatches homes on its own: the unload is M603, and the
        // only toolhead move in the load path (CLEAR_NOZZLE) is routed through
        // ensure_homed_then() precisely BECAUSE the stock macro has no homing
        // guard of its own. ensure_homed_then() emits G28 only when
        // toolhead.homed_axes lacks "xyz", which a paused print never does, and
        // Layer 1 (reject_homing_during_active_print) would refuse it anyway.
        // Only AmsBackend::filament_ops_self_home() backends (AD5X IFS) still
        // refuse here — see test_ams_paused_filament_ops.cpp.
        set_print_state(helix::PrintJobState::PAUSED);
        REQUIRE(backend->load_filament(2).success());
        CHECK_FALSE(backend->sent.empty());
        backend->sent.clear();
        REQUIRE(backend->unload_filament(1).success());
        CHECK_FALSE(backend->sent.empty());
        backend->sent.clear();
        REQUIRE(backend->change_tool(0).success());
        CHECK_FALSE(backend->sent.empty());
    }
}

TEST_CASE_METHOD(QidiHomingGuardFixture,
                 "QIDI load/unload/change_tool proceed to gcode-emit when not printing",
                 "[ams][qidi_box][homing_guard]") {
    SECTION("STANDBY: load_filament emits gcode") {
        set_print_state(helix::PrintJobState::STANDBY);
        REQUIRE(backend->load_filament(2).success());
        CHECK_FALSE(backend->sent.empty());
    }

    SECTION("STANDBY: unload_filament emits gcode") {
        set_print_state(helix::PrintJobState::STANDBY);
        REQUIRE(backend->unload_filament(1).success());
        CHECK_FALSE(backend->sent.empty());
    }

    SECTION("STANDBY: change_tool emits gcode (delegates to load)") {
        set_print_state(helix::PrintJobState::STANDBY);
        REQUIRE(backend->change_tool(0).success());
        CHECK_FALSE(backend->sent.empty());
    }

    SECTION("COMPLETE: load_filament emits gcode") {
        set_print_state(helix::PrintJobState::COMPLETE);
        REQUIRE(backend->load_filament(2).success());
        CHECK_FALSE(backend->sent.empty());
    }
}

TEST_CASE("QIDI adjusts live on a plain heater and not once the box owns the timer",
          "[ams][qidi][dryer][capability]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, nlohmann::json{{"box_count", 1}});

    SECTION("plain heater_generic: SET_HEATER_TEMPERATURE retargets, we own the clock") {
        auto d = backend.get_dryer_info(0);
        REQUIRE(d.supported);
        CHECK(d.supports_live_temp);
        CHECK(d.supports_live_duration);
    }

    SECTION("box_extras timer: ENABLE_BOX_DRY owns the cycle, END_TIME is whole hours") {
        QidiBoxTestAccess::apply_box_extras(
            backend, nlohmann::json{{"box_drying_state", {{"box1", {{"end_time", 0}}}}}});
        auto d = backend.get_dryer_info(0);
        CHECK_FALSE(d.supports_live_temp);
        CHECK_FALSE(d.supports_live_duration);
    }
}

TEST_CASE("QIDI Box tag fingerprint change clears a prior user edit", "[ams][qidi_box]") {
    QidiTmpCacheDir tmp("swap_clears");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendQidi> harness(&api);
    auto& backend = *harness;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "qidi");
    QidiBoxTestAccess::inject_override_store(backend, std::move(store));

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    mock_printer.client.mock_db_set(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF362D"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Red";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);

    // First observation establishes the baseline whatever the override says.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // A different spool's tag: new ids on all three fields.
    fire_vars(backend, json{{"filament_slot0", 11}, {"color_slot0", 2}, {"vendor_slot0", 2}});

    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    const auto lane = helix::ams::lane_sources(harness.lane(0));
    CHECK_FALSE(lane.pending.has_value());
    CHECK_FALSE(lane.record.has_value());

    // The new tag's identity is what the slot shows now.
    const auto info = backend.get_slot_info(0);
    CHECK(info.material == "ABS");
    CHECK(info.brand == "eSUN");
    REQUIRE(helix::QidiBoxTestAccess::get_color(backend, 2).has_value());
    CHECK(info.color_rgb == *helix::QidiBoxTestAccess::get_color(backend, 2));
}

TEST_CASE("QIDI Box unchanged tag fingerprint keeps a user edit", "[ams][qidi_box]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendQidi> harness(&api);
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);

    const json tag{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}};
    fire_vars(backend, tag);
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    // The same spool re-observed on a later poll.
    fire_vars(backend, tag);

    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("QIDI Box asks about an insert the ids do not vouch for, from the queue",
          "[ams][qidi_box]") {
    static int s_offered_slot = -1;
    s_offered_slot = -1;
    const auto previous = helix::set_ui_request_handler(
        helix::UiRequestKind::OfferClearAfterUnverifiedInsert,
        [](const helix::UiRequest& request) { s_offered_slot = request.slot; });
    MockPrinter mock_printer;
    helix::test::RegisteredWired<AmsBackendQidi> harness(&mock_printer.api);

    fire_vars(*harness, json{{"slot1", 0}});
    fire_vars(*harness, json{{"slot1", 1}});
    CHECK(s_offered_slot == -1);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(s_offered_slot == 1);
    helix::set_ui_request_handler(helix::UiRequestKind::OfferClearAfterUnverifiedInsert, previous);
}

TEST_CASE("QIDI Box first tag observation is a baseline, not a clear", "[ams][qidi_box]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendQidi> harness(&api);
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);

    // The very first fingerprint the Box reports disagrees with the restored
    // override, and still must not clear it: nothing was swapped, the screen
    // just started watching.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("QIDI Box own identity push echo does not clear the edit", "[ams][qidi_box]") {
    // execute_gcode must succeed for the echo to be expected at all, and the
    // null-API backend refuses it, so the recording subclass stands in.
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    // The user's edit resolves to fila 11 (ABS), palette row 2 (the exact
    // 0x060606 entry), vendor 2 (eSUN).
    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    helix::test::edit_slot_as_user(backend, 0, info);

    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    bool wrote_fila = false, wrote_color = false, wrote_vendor = false;
    for (const auto& g : backend.sent) {
        wrote_fila |= g.find("VARIABLE=filament_slot0 VALUE=11") != std::string::npos;
        wrote_color |= g.find("VARIABLE=color_slot0 VALUE=2") != std::string::npos;
        wrote_vendor |= g.find("VARIABLE=vendor_slot0 VALUE=2") != std::string::npos;
    }
    REQUIRE(wrote_fila);
    REQUIRE(wrote_color);
    REQUIRE(wrote_vendor);

    // Firmware echoes the written ids back: same composite the edit just
    // pushed, which must read as our own echo rather than a swap.
    fire_vars(backend, json{{"filament_slot0", 11}, {"color_slot0", 2}, {"vendor_slot0", 2}});

    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("QIDI Box a write that never dispatched is refused and saves nothing",
          "[ams][qidi_box]") {
    // Every SAVE_VARIABLE dispatch fails.
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    backend.fail_dispatch = true;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    const auto err = backend.commit_user_edit(0, backend.get_slot_info(0), info);

    CHECK_FALSE(err.success());
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK_FALSE(helix::ams::lane_sources(harness.lane(0)).pending.has_value());
    CHECK(backend.get_slot_info(0).material == "PLA");

    // The ids the failed write would have written, arriving from outside,
    // are another client's edit and show as the Box states them.
    fire_vars(backend, json{{"filament_slot0", 11}, {"color_slot0", 2}, {"vendor_slot0", 2}});
    CHECK(backend.get_slot_info(0).material == "ABS");
}

TEST_CASE("QIDI Box a user edit persists its override to the lane_data record", "[ams][qidi_box]") {
    QidiTmpCacheDir tmp("edit_persists");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendQidi> harness(&api);
    auto& backend = *harness;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "qidi");
    QidiBoxTestAccess::inject_override_store(backend, std::move(store));

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    helix::test::edit_slot_as_user(backend, 0, info);
    // Klipper's answer to the SAVE_VARIABLEs lands through the update queue.
    helix::ui::UpdateQueue::instance().drain();

    // Round-trip through the wire format rather than asserting field spellings.
    const auto rec = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE_FALSE(rec.is_null());
    const auto parsed = helix::ams::from_lane_data_record(rec);
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == 0);
    CHECK(parsed->second.brand == "eSUN");
}

TEST_CASE("QIDI Box startup loads persisted overrides from the database", "[ams][qidi_box]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredBackend<AmsBackendQidi> harness(&api, nullptr);
    auto& backend = *harness;

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    mock_printer.client.mock_db_set("lane_data", "lane1", helix::ams::to_lane_data_record(0, ovr));

    QidiBoxTestAccess::start_load(backend);

    const auto loaded = QidiBoxTestAccess::get_override(backend, 0);
    REQUIRE(loaded.has_value());
    CHECK(loaded->brand == "Polymaker");
    CHECK(loaded->spoolman_id == 42);
}

TEST_CASE("QIDI Box clear_slot_override erases the edit, its lane record and the persisted copy",
          "[ams][qidi_box]") {
    QidiTmpCacheDir tmp("clear_request");
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendQidi> harness(&api);
    auto& backend = *harness;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "qidi");
    QidiBoxTestAccess::inject_override_store(backend, std::move(store));

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"vendor_slot0", 2}});
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(backend.get_slot_info(0).brand == "eSUN");

    backend.clear_slot_override(0);

    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    const auto lane = helix::ams::lane_sources(harness.lane(0));
    CHECK_FALSE(lane.pending.has_value());
    CHECK_FALSE(lane.record.has_value());
    // The clear resets what the slot showed from the tag too; the next parse
    // re-states it.
    CHECK(backend.get_slot_info(0).brand.empty());
}

TEST_CASE("QIDI Box a poll between the edit and its echo keeps the edit on screen",
          "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    helix::test::edit_slot_as_user(backend, 0, info);
    REQUIRE(backend.get_slot_info(0).material == "ABS");

    // The poll races the SAVE_VARIABLE echo: the Box still reports the old
    // ids, which the fingerprint reads as the same spool. The frame's tag
    // paint must not overwrite the fields the user just declared.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    const auto shown = backend.get_slot_info(0);
    CHECK(shown.material == "ABS");
    CHECK(shown.brand == "eSUN");
    CHECK(shown.color_rgb == 0x060606u);
}

// Clear Spool: the Box has no clear command, so erasing what the firmware
// remembers is three bare-int SAVE_VARIABLEs (row ids start at 1, so 0
// names no row and reads as unset).

TEST_CASE("QIDI Box clearing a spool writes the three identity zeros", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    clear_as_the_menu_does(backend, 0);

    for (const char* gcode : {"SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=0",
                              "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=0",
                              "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=0"}) {
        CAPTURE(gcode);
        REQUIRE(std::find(backend.sent.begin(), backend.sent.end(), std::string(gcode)) !=
                backend.sent.end());
    }
}

TEST_CASE("QIDI Box a cleared slot reads as no identity once the zero echo lands",
          "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(backend.get_slot_info(0).material == "PLA");
    REQUIRE(backend.get_slot_info(0).brand == "QIDI");
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFF362Du);

    clear_as_the_menu_does(backend, 0);
    // The request blanks the slot immediately; the Box still remembers until
    // the zero writes land.
    CHECK(backend.get_slot_info(0).material.empty());
    CHECK(backend.get_slot_info(0).brand.empty());
    CHECK(backend.get_slot_info(0).color_rgb == 0);

    // The zero writes race the polls: the next poll still carries the old ids
    // and repaints the tag.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // The echo: all three ids read back as unset, and the slot must not keep
    // the last paint.
    fire_vars(backend, json{{"filament_slot0", 0}, {"color_slot0", 0}, {"vendor_slot0", 0}});
    const auto shown = backend.get_slot_info(0);
    CHECK(shown.material.empty());
    CHECK(shown.brand.empty());
    CHECK(shown.color_rgb == 0);
}

TEST_CASE("QIDI Box an edit racing the clear's zero echoes survives them", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    clear_as_the_menu_does(backend, 0);

    // The user re-edits before the zero writes land: the edit stages a fresh
    // override and queues its own SAVE_VARIABLEs behind the clear's.
    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    helix::test::edit_slot_as_user(backend, 0, info);
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // A mixed echo: the clear's filament write landed, its colour and vendor
    // writes have not. Reading that as a spool swap would erase an edit the
    // user made moments ago.
    fire_vars(backend, json{{"filament_slot0", 0}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK(backend.get_slot_info(0).material == "ABS");
}

TEST_CASE("QIDI Box clear writes the firmware zeros while a print is active", "[ams][qidi_box]") {
    MockPrinter mock_printer;
    auto& state = mock_printer.state;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<RecordingQidiBackend> harness(&api);
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    QidiBoxTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    helix::test::set_wire_state(state, helix::PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(state.print_state().get_print_lifecycle() == PrintState::Printing);

    clear_as_the_menu_does(backend, 0);

    // SAVE_VARIABLE moves nothing, so the print does not hold the writes back;
    // without them the next poll repaints the old ids over the clear.
    for (const char* gcode : {"SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=0",
                              "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=0",
                              "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=0"}) {
        CAPTURE(gcode);
        CHECK(std::find(backend.sent.begin(), backend.sent.end(), std::string(gcode)) !=
              backend.sent.end());
    }
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK(backend.get_slot_info(0).material.empty());
}

TEST_CASE("QIDI Box a quoted SAVE_VARIABLE value paints the slot", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    // Qidi Studio writes VALUE="5"; save_variables.py evaluates the literal,
    // so the id arrives as a JSON string rather than an int.
    fire_vars(backend, json{{"filament_slot0", "1"}, {"color_slot0", "18"}, {"vendor_slot0", "1"}});

    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");
    REQUIRE(backend.get_slot_info(0).material == "PLA");
    REQUIRE(backend.get_slot_info(0).brand == "QIDI");
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFF362Du);
}

TEST_CASE("QIDI Box a non-integer slot id keeps the last stated id", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{
                           {"filament_slot0", 42},
                           {"color_slot1", 3},
                       });
    // A value that is neither an int nor digits is ignored - the previously
    // stated id stays. Whitespace around the digits is still digits.
    fire_vars(backend, json{
                           {"filament_slot0", "PLA"},
                           {"color_slot1", " 4 "},
                       });
    CHECK(QidiBoxTestAccess::filament_id(backend, 0) == 42);
    CHECK(QidiBoxTestAccess::color_id(backend, 1) == 4);
    // A negative id names no palette row, in either spelling.
    fire_vars(backend, json{
                           {"filament_slot0", -7},
                           {"color_slot1", "-7"},
                       });
    CHECK(QidiBoxTestAccess::filament_id(backend, 0) == 42);
    CHECK(QidiBoxTestAccess::color_id(backend, 1) == 4);
}
TEST_CASE("QIDI Box a restart compares against the fingerprint the record carried",
          "[ams][qidi_box]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // A record as a session running older code left it: a user override with
    // no fingerprint. Session 1 boots on it, observes the tag, and the
    // observation must persist into the record - two backends over one mock
    // DB stand in for two app lifetimes.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spoolman_id = 42;
    mock_printer.client.mock_db_set("lane_data", "lane1",
                                    helix::ams::to_lane_data_record(0, saved));
    {
        helix::test::RegisteredWired<AmsBackendQidi> session1(&api);
        QidiBoxTestAccess::apply_filas_list(*session1, kStockFilasExcerpt);
        QidiBoxTestAccess::start_load(*session1);
        REQUIRE(QidiBoxTestAccess::get_override(*session1, 0).has_value());
        fire_vars(*session1, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
        const auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        REQUIRE(stored.contains("helix_fingerprint"));
        CHECK(stored.at("helix_fingerprint") == "1|18");
    }

    SECTION("a swap made while the app was down clears the override") {
        helix::test::RegisteredWired<AmsBackendQidi> session2(&api);
        QidiBoxTestAccess::apply_filas_list(*session2, kStockFilasExcerpt);
        QidiBoxTestAccess::start_load(*session2);
        // The loaded record's fingerprint is the baseline, so the first frame
        // after restart is a comparison, not a first observation.
        REQUIRE(QidiBoxTestAccess::get_override(*session2, 0).has_value());
        fire_vars(*session2, json{{"filament_slot0", 11}, {"color_slot0", 2}, {"vendor_slot0", 2}});
        CHECK_FALSE(QidiBoxTestAccess::get_override(*session2, 0).has_value());
        CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
        // The new tag's identity is what the slot shows.
        CHECK(session2->get_slot_info(0).material == "ABS");
        CHECK(session2->get_slot_info(0).brand == "eSUN");
    }

    SECTION("the same spool across a restart keeps the override") {
        helix::test::RegisteredWired<AmsBackendQidi> session2(&api);
        QidiBoxTestAccess::apply_filas_list(*session2, kStockFilasExcerpt);
        QidiBoxTestAccess::start_load(*session2);
        fire_vars(*session2, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
        const auto ovr = QidiBoxTestAccess::get_override(*session2, 0);
        REQUIRE(ovr.has_value());
        CHECK(ovr->brand == "Polymaker");
        CHECK(!mock_printer.client.mock_db_get("lane_data", "lane1").is_null());
    }
}

// A start opens a new lane record store, which frees the one before it. The
// tracker's baseline sink runs inline on every Baseline and Unchanged
// observation and outlives that start, so it must follow the backend's pointer
// to its store rather than hold the store: the one the last read saw is freed.
//
// The edit leaves for Klipper before the restart and its answer lands after
// it. An edit that began inside the window would be refused one layer up
// (AmsState refuses an edit while the lane records are unread); one already
// sent has its record staged when its answer arrives, whichever store is open
// then. Its own save waits for the new store's read, and the fingerprint a
// poll confirms must not slip past that gate through the old store.
//
// Break: bind the sink to override_store_.get() instead of override_store_.
// The sink the first read bound then names the store open_lane_record_store()
// freed, and the poll in the window writes through it: a heap-use-after-free
// under ASAN, and the freed store had read, so its gate waves the write
// through.
TEST_CASE("QIDI Box a restarted backend's fingerprint sink names no replaced store",
          "[qidi][qidi_box][store_restart]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    // No client, so a start ends at the store and its read is the mock's to hold.
    helix::test::RegisteredBackend<RecordingQidiBackend> harness(&api, nullptr);
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    // The Box's ids for the slot, as on_started()'s bootstrap query brings them.
    const auto poll = [&backend](const json& variables) {
        const json status = helix::save_variables_frame(variables);
        QidiBoxTestAccess::apply_query(backend, json{{"result", {{"status", status}}}});
    };
    const json tag{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}};

    // The first start reads its store, and that read binds the sink.
    QidiBoxTestAccess::start_load(backend);
    poll(tag);
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|18");

    // The user's edit is on its way to Klipper, its answer not back yet.
    SlotInfo edited = backend.get_slot_info(0);
    edited.material = "ABS";
    edited.brand = "eSUN";
    edited.color_rgb = 0x060606u;
    backend.hold = true;
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), edited).success());
    REQUIRE(backend.held_complete);
    backend.hold = false;

    // A restart frees that store and opens another, whose first read is held.
    // This window is where a sink holding the old store would write through it.
    mock_printer.client.defer_next("server.database.get_item");
    QidiBoxTestAccess::start_load(backend);
    REQUIRE_FALSE(backend.lane_records_ready());

    // Klipper's answer lands in that window and stages the edit's record.
    backend.held_complete();
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // The Box still states the ids the baseline holds: the same spool again.
    // The new store has not read, so the gate that refused the record's own
    // save refuses this fingerprint too, and nothing reaches the database.
    poll(tag);
    CHECK(mock_printer.client.mock_db_get("lane_data", "lane1").is_null());

    // The read lands. The same poll now persists, through the new store.
    mock_printer.client.fire_deferred("server.database.get_item");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(backend.lane_records_ready());
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), edited).success());
    CHECK_FALSE(
        mock_printer.client.mock_db_get("lane_data", "lane1").contains("helix_fingerprint"));
    poll(tag);
    const auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(stored.contains("helix_fingerprint"));
    CHECK(stored.at("helix_fingerprint") == "1|18");
}

TEST_CASE("clearing a QIDI Box slot takes the linked spool's brand off it",
          "[ams][qidi_box][lane]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<RecordingQidiBackend> harness(&api);
    auto& backend = *harness;

    // A linked Spoolman spool states brand and spool name as lane records.
    SpoolInfo spool;
    spool.id = 1;
    spool.vendor = "Polymaker";
    spool.filament_name = "PolyLite Red";
    helix::test::spool_states(backend, 0, spool);
    backend.repaint_slot_from_lane(0);
    REQUIRE(backend.get_slot_info(0).brand == "Polymaker");
    REQUIRE(backend.get_slot_info(0).spool_name == "PolyLite Red");

    // Field for field what ui_ams_detail does on CLEAR_SPOOL: blank the
    // identity fields, drop the Spoolman handles, commit as a user edit. The
    // lane no longer states brand or spool name and apply_resolved() cannot
    // retire a field no source observes - and nothing on this Box ever
    // restates either - so only the repaint's clear takes them off.
    SlotInfo cleared = backend.get_slot_info(0);
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.color_name.clear();
    cleared.multi_color_hexes.clear();
    cleared.brand.clear();
    cleared.catalog_id.clear();
    cleared.product_name.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;
    helix::test::edit_slot_as_user(backend, 0, cleared);

    CHECK(backend.get_slot_info(0).brand.empty());
    CHECK(backend.get_slot_info(0).spool_name.empty());
}

TEST_CASE("QIDI Box explains an unavailable eject", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot0", 0}, {"slot1", 1}});

    SECTION("force_move off with filament in the lane") {
        QidiBoxTestAccess::set_force_move(backend, false);
        REQUIRE_FALSE(backend.supports_lane_eject());
        CHECK_FALSE(backend.eject_unavailable_reason(1).empty());
        // An empty lane has nothing to eject, so there is nothing to explain.
        CHECK(backend.eject_unavailable_reason(0).empty());
    }

    SECTION("force_move on") {
        QidiBoxTestAccess::set_force_move(backend, true);
        REQUIRE(backend.supports_lane_eject());
        CHECK(backend.eject_unavailable_reason(1).empty());
    }

    SECTION("an out-of-range slot") {
        QidiBoxTestAccess::set_force_move(backend, false);
        CHECK(backend.eject_unavailable_reason(-1).empty());
        CHECK(backend.eject_unavailable_reason(99).empty());
    }

    SECTION("the base backend has no reason") {
        helix::AmsBackendMock mock(4);
        CHECK(mock.eject_unavailable_reason(1).empty());
    }
}

TEST_CASE("QIDI Box a colour list paints its RGB without the palette",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // No officiall_filas_list yet: the list spells the colour itself.
    fire_vars(backend, json{{"color_slot0", json::array({"228332"})}});
    CHECK(backend.get_slot_info(0).color_rgb == 0x228332u);

    fire_vars(backend, json{{"color_slot0", json::array({"#fafafa"})}});
    CHECK(backend.get_slot_info(0).color_rgb == 0xFAFAFAu);

    // An empty list states no colour, the same as a zero row id.
    helix::test::StartedBackend<AmsBackendQidi> zero_row;
    fire_vars(zero_row, json{{"color_slot0", 0}});
    fire_vars(backend, json{{"color_slot0", json::array()}});
    CHECK(backend.get_slot_info(0).color_rgb != 0xFAFAFAu);
    CHECK(backend.get_slot_info(0).color_rgb == zero_row.get_slot_info(0).color_rgb);
}

TEST_CASE("QIDI Box a string filament id resolves from the controller's slot info",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SECTION("controller info and save_variables in one notification") {
        json status = make_controller_slot_notification(0, 1, kControllerGenericPla);
        status["save_variables"] =
            json{{"variables",
                  json{{"filament_slot1", "QFA90"}, {"color_slot1", json::array({"228332"})}}}};
        backend.fire_status(status);
    }
    SECTION("controller info after save_variables") {
        fire_vars(backend,
                  json{{"filament_slot1", "QFA90"}, {"color_slot1", json::array({"228332"})}});
        CHECK(backend.get_slot_info(1).material.empty());
        backend.fire_status(make_controller_slot_notification(0, 1, kControllerGenericPla));
    }

    const auto s = backend.get_slot_info(1);
    CHECK(s.material == "PLA");
    CHECK(s.brand == "Generic");
    CHECK(s.color_rgb == 0x228332u);
    CHECK(s.nozzle_temp_min == 190);
    CHECK(s.nozzle_temp_max == 240);
}

TEST_CASE("QIDI Box a partial controller update keeps the slot's other fields",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"filament_slot2", "QFA90"}});
    backend.fire_status(make_controller_slot_notification(0, 2, kControllerGenericPla));
    REQUIRE(backend.get_slot_info(2).material == "PLA");

    // Subscription updates carry only the keys that changed.
    backend.fire_status(make_controller_slot_notification(0, 2, json{{"max_temp", 230}}));
    const auto s = backend.get_slot_info(2);
    CHECK(s.material == "PLA");
    CHECK(s.brand == "Generic");
    CHECK(s.nozzle_temp_max == 230);
}

TEST_CASE("QIDI Box a numeric id keeps the Box tables over controller info",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    json status = make_controller_slot_notification(0, 0, kControllerGenericPla);
    status["save_variables"] =
        json{{"variables", json{{"filament_slot0", 11}, {"color_slot0", 18}, {"vendor_slot0", 2}}}};
    backend.fire_status(status);

    const auto s = backend.get_slot_info(0);
    CHECK(s.material == "ABS");
    CHECK(s.brand == "eSUN");
    CHECK(s.color_rgb == 0xFF362Du);
}

TEST_CASE("QIDI Box string ids and colour lists fingerprint the spool",
          "[ams][qidi_box][qidi_string_ids]") {
    MockPrinter mock_printer;
    helix::test::RegisteredWired<AmsBackendQidi> harness(&mock_printer.api);
    auto& backend = *harness;

    backend.fire_status(make_controller_slot_notification(0, 0, kControllerGenericPla));
    backend.fire_status(make_controller_slot_notification(
        0, 3, json{{"filament_id", "QFB10"}, {"filament_type", "ABS"}}));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);

    const json tag{{"filament_slot0", "QFA90"}, {"color_slot0", json::array({"228332"})}};
    fire_vars(backend, tag);
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#228332");

    // The same spool re-observed keeps the edit.
    fire_vars(backend, tag);
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // A different spool clears it.
    fire_vars(backend, json{{"filament_slot0", "QFB10"}, {"color_slot0", json::array({"228332"})}});
    CHECK(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFB10|#228332");
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("QIDI Box our edit's echo over string ids is not a swap",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;

    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    backend.fire_status(make_controller_slot_notification(0, 0, kControllerGenericPla));
    fire_vars(backend, json{{"filament_slot0", "QFA90"}, {"color_slot0", json::array({"228332"})}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#228332");

    auto info = backend.get_slot_info(0);
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0x060606u;
    helix::test::edit_slot_as_user(backend, 0, info);
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // The filament write lands before the colour write.
    fire_vars(backend, json{{"filament_slot0", 11}, {"color_slot0", json::array({"228332"})}});
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());

    fire_vars(backend, json{{"filament_slot0", 11}, {"color_slot0", 2}});
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK(backend.get_slot_info(0).material == "ABS");
}

TEST_CASE("QIDI Box the same spool restated as a product id keeps the edit",
          "[ams][qidi_box][qidi_string_ids]") {
    MockPrinter mock_printer;
    helix::test::RegisteredWired<AmsBackendQidi> harness(&mock_printer.api);
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    // The firmware has told us QFA90 is PLA.
    backend.fire_status(make_controller_slot_notification(
        0, 3, json{{"filament_id", "QFA90"}, {"filament_type", "PLA"}}));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);

    // fila row 1 is PLA, palette row 1 is #FAFAFA.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|1");

    fire_vars(backend, json{{"filament_slot0", "QFA90"}, {"color_slot0", json::array({"FAFAFA"})}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#FAFAFA");
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // A product id of a different material is a different spool.
    backend.fire_status(make_controller_slot_notification(
        0, 3, json{{"filament_id", "QFB10"}, {"filament_type", "ABS"}}));
    fire_vars(backend, json{{"filament_slot0", "QFB10"}, {"color_slot0", json::array({"FAFAFA"})}});
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("QIDI Box controller info for another product id paints nothing",
          "[ams][qidi_box][qidi_string_ids]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    backend.fire_status(make_controller_slot_notification(0, 0, kControllerGenericPla));
    fire_vars(backend, json{{"filament_slot0", "QFA90"}});
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // The new id lands before the controller restates the slot.
    fire_vars(backend, json{{"filament_slot0", "QFL30"}});
    CHECK(backend.get_slot_info(0).material.empty());
    CHECK(backend.get_slot_info(0).brand.empty());

    backend.fire_status(make_controller_slot_notification(
        0, 0, json{{"filament_id", "QFL30"}, {"filament_type", "PLA"}, {"vendor_str", "Bambu"}}));
    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(0).brand == "Bambu");
}

TEST_CASE("QIDI Box a product id the controller has not described defers the swap check",
          "[ams][qidi_box][qidi_string_ids]") {
    MockPrinter mock_printer;
    helix::test::RegisteredWired<AmsBackendQidi> harness(&mock_printer.api);
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 1}});
    REQUIRE(QidiBoxTestAccess::last_fingerprint(backend, 0) == "1|1");

    // QFA90 arrives before anything has said what it is.
    fire_vars(backend, json{{"filament_slot0", "QFA90"}, {"color_slot0", json::array({"FAFAFA"})}});
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());

    SECTION("the same material: the same spool") {
        backend.fire_status(make_controller_slot_notification(
            0, 0, json{{"filament_id", "QFA90"}, {"filament_type", "PLA"}}));
        CHECK(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#FAFAFA");
        CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
    }
    SECTION("another material: a different spool") {
        backend.fire_status(make_controller_slot_notification(
            0, 0, json{{"filament_id", "QFA90"}, {"filament_type", "PETG"}}));
        CHECK(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#FAFAFA");
        CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    }
}

TEST_CASE("QIDI Box a controller delta without both id and type pairs nothing",
          "[ams][qidi_box][qidi_string_ids]") {
    MockPrinter mock_printer;
    helix::test::RegisteredWired<AmsBackendQidi> harness(&mock_printer.api);
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    backend.fire_status(make_controller_slot_notification(
        0, 3, json{{"filament_id", " QFA90 "}, {"filament_type", "PLA"}}));
    // A type-only delta names no id: it must not re-pair QFA90.
    backend.fire_status(make_controller_slot_notification(0, 3, json{{"filament_type", "PETG"}}));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    QidiBoxTestAccess::seed_override(backend, 0, ovr);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 1}});
    fire_vars(backend, json{{"filament_slot0", "QFA90"}, {"color_slot0", json::array({"FAFAFA"})}});
    CHECK(QidiBoxTestAccess::last_fingerprint(backend, 0) == "QFA90|#FAFAFA");
    CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
}
