// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 48 to 87 of test_ams_backend_qidi.cpp before it was split, in its order. Mostly [general],
// [write_path], [error-center][qidi], [error-center], [clear], and more

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

TEST_CASE("QIDI Box per-unit dryer: box1 drying, box2 idle -> distinct DryerInfo",
          "[ams][qidi_box][dryer][multi-unit]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // Expand to 2 boxes.
    fire_vars(backend, json{{"box_count", 2}});

    // Deterministic clock so remaining_min is stable.
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1'000'000}; });
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);

    // Box1 heating to 55C @ 48C now; box2 idle heater.
    backend.fire_status(
        json{{"heater_generic heater_box1", json{{"temperature", 48.0}, {"target", 55.0}}},
             {"heater_generic heater_box2", json{{"temperature", 24.0}, {"target", 0.0}}}});

    // Box1 has an active drying end_time 30 min out; box2 none.
    QidiBoxTestAccess::apply_box_extras(
        backend, json{{"box_drying_state", json{{"box1", json{{"end_time", 1'000'000 + 30 * 60}}},
                                                {"box2", json{{"end_time", 0}}}}}});

    DryerInfo d0 = QidiBoxTestAccess::get_dryer(backend, 0);
    DryerInfo d1 = QidiBoxTestAccess::get_dryer(backend, 1);

    REQUIRE(d0.active);
    REQUIRE(d0.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
    REQUIRE(d0.current_temp_c == Catch::Approx(48.0f).epsilon(0.01));
    REQUIRE(d0.remaining_min == 30);

    REQUIRE_FALSE(d1.active);
    REQUIRE(d1.target_temp_c == Catch::Approx(0.0f).epsilon(0.01));
    REQUIRE(d1.remaining_min == 0);
}

TEST_CASE("QIDI Box config query refines max temp (heater_generic section)",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_config_settings(
        backend, json{{"heater_generic heater_box1", json{{"max_temp", 80.0}}}});
    REQUIRE(QidiBoxTestAccess::get_dryer(backend).max_temp_c == Catch::Approx(80.0f));
}

TEST_CASE("QIDI Box dries with the box's timer commands, else the bare heater",
          "[ams][qidi_box][dryer][write_path]") {
    struct Row {
        const char* name;
        bool timer;
        bool start;
        float temp;
        const char* sent; // nullptr = refused
    };
    const Row rows[] = {
        {"start with a timer", true, true, 55.0f, "ENABLE_BOX_DRY BOX=1 TEMP=55 END_TIME=4"},
        {"start without one", false, true, 55.0f,
         "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=55"},
        {"start out of range", false, true, 150.0f, nullptr},
        {"stop with a timer", true, false, 0.0f, "DISABLE_BOX_DRY BOX=1"},
        {"stop without one", false, false, 0.0f,
         "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=0"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::StartedBackend<RecordingQidiBackend> backend;
        QidiBoxTestAccess::set_drying_timer_supported(backend, row.timer);
        const auto err = row.start ? backend.start_drying(row.temp, 240) : backend.stop_drying(0);
        if (row.sent) {
            CHECK(err.success());
            REQUIRE(backend.sent.size() == 1);
            CHECK(backend.sent[0] == row.sent);
        } else {
            CHECK_FALSE(err.success());
            CHECK(backend.sent.empty());
        }
    }

    // The refusal names the range in the display unit (the per-test reset restores °C).
    helix::units::set_display_temp_unit(helix::units::TempUnit::Fahrenheit);
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    const std::string hint = backend.start_drying(150.0f, 240).suggestion;
    CHECK(hint.find("°F") != std::string::npos);
    CHECK(hint.find("°C") == std::string::npos);
}

TEST_CASE("QIDI Box apply_filas_list ignores bad sections and atomically swaps",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // First load populates all three maps.
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 1).has_value());

    // Reload with only fila data + a typo'd section — colordict/vendor should
    // be cleared (atomic replace), not merged.
    QidiBoxTestAccess::apply_filas_list(backend, R"INI(
[colourdict]
1 = #112233

[fila3]
filament = PLA Metal
type = PLA
min_temp = 190
max_temp = 240
)INI");
    REQUIRE(QidiBoxTestAccess::get_profile(backend, 3).has_value());
    // Old fila1 gone (replaced).
    REQUIRE_FALSE(QidiBoxTestAccess::get_profile(backend, 1).has_value());
    // Typo'd [colourdict] not accepted, old palette wiped.
    REQUIRE(QidiBoxTestAccess::color_count(backend) == 0);
    REQUIRE(QidiBoxTestAccess::vendor_count(backend) == 0);
}

// Read-path resolution: slot_rfid_ ids → SlotInfo material/color/brand
// Once the filas list is cached, parse_save_variables must resolve the raw
// filament_slot/color_slot/vendor_slot indices onto the SlotInfo fields the
// UI reads (material, color_rgb, brand) — in addition to the nozzle temps it
// already applied.

TEST_CASE("QIDI Box read-path resolves material/color/brand from filas list", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    fire_vars(backend, json{
                           {"filament_slot0", 11}, // ABS Rapido / ABS
                           {"color_slot0", 18},    // #FF362D
                           {"vendor_slot0", 2},    // eSUN
                       });

    auto info = backend.get_system_info();
    const auto& s = info.units[0].slots[0];
    REQUIRE(s.material == "ABS");
    REQUIRE(s.color_rgb == 0xFF362D);
    REQUIRE(s.brand == "eSUN");
    // Temps still applied from the same profile.
    REQUIRE(s.nozzle_temp_min == 240);
    REQUIRE(s.nozzle_temp_max == 280);
}

TEST_CASE("QIDI Box read-path leaves fields unchanged when ids miss", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    // Unknown filament id (not in the cfg) + unknown color/vendor ids.
    fire_vars(backend, json{
                           {"filament_slot0", 77},
                           {"color_slot0", 99},
                           {"vendor_slot0", 88},
                       });

    auto info = backend.get_system_info();
    const auto& s = info.units[0].slots[0];
    // Material/brand untouched (empty defaults), color stays at default.
    REQUIRE(s.material.empty());
    REQUIRE(s.brand.empty());
    REQUIRE(s.color_rgb == kAmsDefaultSlotColor);
}

TEST_CASE("QIDI Box read-path resolution survives before filas list loads", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // No filas list yet — ids captured but nothing resolved, no crash.
    fire_vars(backend, json{
                           {"filament_slot0", 1},
                           {"color_slot0", 1},
                           {"vendor_slot0", 1},
                       });
    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].material.empty());
    REQUIRE(info.units[0].slots[0].color_rgb == kAmsDefaultSlotColor);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 1);
}

// Reverse lookups (pure) for apply_user_edit()

TEST_CASE("QIDI Box resolve_fila_id matches name then falls back to type", "[ams][qidi_box]") {
    std::map<int, AmsBackendQidi::FilaProfile> profiles;
    profiles[1] = {"PLA Rapido", "PLA", 190, 240, 0, 0};
    profiles[11] = {"ABS Rapido", "ABS", 240, 280, 0, 45};
    profiles[42] = {"PETG-CF", "PETG-CF", 240, 270, 0, 45};

    // Exact (case-insensitive) name match wins.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "ABS", "abs rapido") == 11);
    // No name match → first profile whose type matches material.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "pla", "") == 1);
    // Nothing matches → 0.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "NYLON", "Whatever") == 0);
}

TEST_CASE("QIDI Box resolve_color_id picks nearest palette entry", "[ams][qidi_box]") {
    std::map<int, uint32_t> palette;
    palette[1] = 0xFAFAFA;  // near-white
    palette[2] = 0x060606;  // near-black
    palette[18] = 0xFF362D; // red

    // Pure white → near-white entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0xFFFFFF) == 1);
    // Pure black → near-black entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0x000000) == 2);
    // Reddish → red entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0xEE2020) == 18);
    // Empty palette → 0.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(std::map<int, uint32_t>{}, 0x123456) == 0);
}

TEST_CASE("QIDI Box resolve_vendor_id matches name, falls back to Generic", "[ams][qidi_box]") {
    std::map<int, std::string> vendors;
    vendors[0] = "Generic";
    vendors[1] = "QIDI";
    vendors[2] = "eSUN";

    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "esun") == 2);
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "QIDI") == 1);
    // Unknown brand → Generic id.
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "Polymaker") == 0);
    // No Generic present and no match → 0.
    std::map<int, std::string> no_generic{{5, "QIDI"}};
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(no_generic, "Polymaker") == 0);
}

// apply_user_edit: write reverse-mapped ids back to save_variables

TEST_CASE("QIDI Box apply_user_edit emits SAVE_VARIABLE for all three ids",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SlotInfo info;
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0xFF362D;

    auto err = helix::test::apply_edit(backend, 0, info);
    REQUIRE(err.success());

    // Three writes: filament_slot0 / color_slot0 / vendor_slot0 — integers
    // unquoted (matches Klipper SAVE_VARIABLE for numeric values).
    std::vector<std::string> sent = backend.sent;
    REQUIRE(sent.size() == 3);
    bool saw_fila = false, saw_color = false, saw_vendor = false;
    for (const auto& g : sent) {
        if (g == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=11")
            saw_fila = true;
        if (g == "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=18")
            saw_color = true;
        if (g == "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=2")
            saw_vendor = true;
    }
    REQUIRE(saw_fila);
    REQUIRE(saw_color);
    REQUIRE(saw_vendor);
}

TEST_CASE("QIDI Box weight update writes none of the box's identity variables",
          "[ams][qidi_box][write_path][1652]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    // A loaded vendor table makes apply_user_edit write vendor_slot for any slot,
    // so a weight that reached it would show up in what was sent.
    REQUIRE(QidiBoxTestAccess::vendor_count(backend) > 0);

    // What the consumption meter's pause and completion flushes do.
    backend.update_slot_weight(0, 500.0f, -1.0f, /*persist=*/true);

    CHECK(backend.sent.empty());
}

TEST_CASE("QIDI Box refuses a material with no filament list row and writes nothing",
          "[ams][qidi_box][write_path]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot1", 1}, {"color_slot1", 18}, {"vendor_slot1", 1}});

    SlotInfo info = backend.get_slot_info(1);
    info.material = "NYLON-X";
    info.color_rgb = 0xFAFAFA;
    const auto err = backend.commit_user_edit(1, backend.get_slot_info(1), info);

    // The editor's inline line names the reason; the Box keeps what it held.
    REQUIRE_FALSE(err.success());
    CHECK(err.user_msg.find("NYLON-X") != std::string::npos);
    CHECK(backend.sent.empty());
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 1).has_value());
    CHECK_FALSE(helix::ams::lane_sources(harness.lane(1)).pending.has_value());
    CHECK(backend.get_slot_info(1).material == "PLA");
}

TEST_CASE("QIDI Box refuses a material before its filament list loads",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    SlotInfo info;
    info.material = "PLA";
    const auto err = helix::test::apply_edit(backend, 0, info);
    REQUIRE_FALSE(err.success());
    CHECK(err.user_msg.find("not loaded") != std::string::npos);
    CHECK(backend.sent.empty());
}

TEST_CASE("QIDI Box shows and records the palette colour the Box holds, not the pick",
          "[ams][qidi_box][write_path]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    // 0x0A0B0C is nearest palette row 2 (#060606).
    SlotInfo info = backend.get_slot_info(0);
    info.color_rgb = 0x0A0B0Cu;
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), info).success());

    CHECK(std::find(backend.sent.begin(), backend.sent.end(),
                    "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=2") != backend.sent.end());
    // Shown at once, before any echo, and the record carries the same row.
    CHECK(backend.get_slot_info(0).color_rgb == 0x060606u);
    const auto record = QidiBoxTestAccess::get_override(backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->color_rgb == 0x060606u);

    // The echo of row 2 ends the edit: nothing is left waiting on it.
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 2}, {"vendor_slot0", 1}});
    CHECK_FALSE(helix::ams::lane_sources(harness.lane(0)).pending.has_value());
    CHECK(backend.get_slot_info(0).color_rgb == 0x060606u);
}

TEST_CASE("QIDI Box an edit that keeps the material leaves the Box's filament row",
          "[ams][qidi_box][write_path]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(
        backend, "[fila1]\nfilament = PLA Basic\ntype = PLA\n[fila7]\nfilament = PLA Rapido\n"
                 "type = PLA\n[colordict]\n18 = #FF362D\n[vendor_list]\n0 = Generic\n"
                 "1 = QIDI\n2 = eSUN\n");
    fire_vars(backend, json{{"filament_slot0", 7}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // Only the brand moves; row 7 is still the product the Box holds.
    SlotInfo info = backend.get_slot_info(0);
    info.brand = "eSUN";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), info).success());
    for (const auto& g : backend.sent) {
        CHECK(g.find("filament_slot0") == std::string::npos);
    }
    CHECK(std::find(backend.sent.begin(), backend.sent.end(),
                    "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=2") != backend.sent.end());
}

TEST_CASE("QIDI Box an edit that names another row writes that row",
          "[ams][qidi_box][write_path]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // The Box holds row 1 (PLA Rapido); ABS names row 11, another product, so
    // keeping the held row would leave the Box on PLA for an ABS lane.
    SlotInfo info = backend.get_slot_info(0);
    info.material = "ABS";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), info).success());
    CHECK(std::find(backend.sent.begin(), backend.sent.end(),
                    "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=11") != backend.sent.end());
}

TEST_CASE("QIDI Box a SAVE_VARIABLE Klipper rejects saves nothing", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    backend.reject = true;

    SlotInfo info = backend.get_slot_info(0);
    info.material = "ABS";
    (void)backend.commit_user_edit(0, backend.get_slot_info(0), info);
    CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    CHECK_FALSE(helix::ams::lane_sources(harness.lane(0)).pending.has_value());
    CHECK(backend.get_slot_info(0).material == "PLA");
}

TEST_CASE("QIDI Box Clear Spool zeroes each variable once", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    SlotInfo cleared = backend.get_slot_info(0);
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.brand.clear();
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), cleared).success());
    static_cast<helix::AmsBackend&>(backend).clear_slot_override(0);
    for (const char* variable : {"filament_slot0", "color_slot0", "vendor_slot0"}) {
        CAPTURE(variable);
        CHECK(std::count_if(backend.sent.begin(), backend.sent.end(), [variable](const auto& g) {
                  return g.find(variable) != std::string::npos;
              }) == 1);
    }
}

TEST_CASE("QIDI Box Clear Spool erases the record only once Klipper runs the clear",
          "[ams][qidi_box][clear]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    SlotInfo branded = backend.get_slot_info(0);
    branded.brand = "eSUN";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), branded).success());
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    // Clear Spool: the cleared edit, then the record's erase, while Klipper
    // has not yet answered the clear.
    backend.hold = true;
    SlotInfo cleared = backend.get_slot_info(0);
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.brand.clear();
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), cleared).success());
    static_cast<helix::AmsBackend&>(backend).clear_slot_override(0);
    REQUIRE(backend.held_complete);
    REQUIRE(QidiBoxTestAccess::get_override(backend, 0).has_value());

    SECTION("Klipper rejects it: the record stays") {
        MoonrakerError err;
        err.message = "rejected";
        backend.held_error(err);
        const auto record = QidiBoxTestAccess::get_override(backend, 0);
        REQUIRE(record.has_value());
        CHECK(record->brand == "eSUN");
    }
    SECTION("Klipper runs it: the record goes, with no empty one after it") {
        backend.held_complete();
        CHECK_FALSE(QidiBoxTestAccess::get_override(backend, 0).has_value());
    }
}

TEST_CASE("QIDI Box a relink Klipper rejects keeps the linked spool's Spoolman details",
          "[ams][qidi_box][spoolman]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});
    SlotInfo linked = backend.get_slot_info(0);
    linked.spoolman_id = 5;
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), linked).success());
    helix::ams::Observation spoolman(helix::ams::ObservationSource::Spoolman);
    spoolman.spoolman_id = 5;
    spoolman.brand = "Polymaker";
    helix::ams::ingest(harness.lane(0), spoolman);

    // The relink changes the Box's rows too, so it waits on Klipper.
    backend.hold = true;
    SlotInfo relinked = backend.get_slot_info(0);
    relinked.spoolman_id = 7;
    relinked.material = "ABS";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), relinked).success());
    REQUIRE(backend.held_error);
    MoonrakerError err;
    err.message = "rejected";
    backend.held_error(err);

    const auto sources = helix::ams::lane_sources(harness.lane(0));
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->brand == std::string("Polymaker"));
}

TEST_CASE("QIDI Box a product name picks the row and becomes the spool's name",
          "[ams][qidi_box][write_path]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    SlotInfo info = backend.get_slot_info(0);
    info.material = "ABS Rapido";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), info).success());
    CHECK(std::find(backend.sent.begin(), backend.sent.end(),
                    "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=11") != backend.sent.end());

    // The material line is what the Box holds; the pick is the spool's name.
    CHECK(backend.get_slot_info(0).material == "ABS");
    CHECK(backend.get_slot_info(0).spool_name == "ABS Rapido");
    const auto record = QidiBoxTestAccess::get_override(backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->material == "ABS");
    CHECK(record->spool_name == "ABS Rapido");
}

TEST_CASE("QIDI Box a record's colour and material never fill what the Box states as unset",
          "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    // Another client zeroed the colour and filament rows; the record from an
    // earlier edit still names both.
    helix::ams::FilamentSlotOverride stale;
    stale.material = "ABS";
    stale.color_rgb = 0x060606u;
    stale.color_set = true;
    stale.brand = "eSUN";
    helix::ams::ingest(harness.lane(0), QidiBoxTestAccess::record_reading(backend, stale));
    fire_vars(backend, json{{"filament_slot0", 0}, {"color_slot0", 0}, {"vendor_slot0", 0}});

    const auto shown = backend.get_slot_info(0);
    CHECK(shown.material.empty());
    CHECK(shown.color_rgb == 0u);
    // The Box holds no brand of its own here, so the record's stands.
    CHECK(shown.brand == "eSUN");
}

TEST_CASE("QIDI Box a brand the vendor table lacks shows from the lane record", "[ams][qidi_box]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 1}});

    // A brand the vendor table lacks is written as Generic (row 0).
    SlotInfo info = backend.get_slot_info(0);
    info.brand = "Polymaker";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), info).success());
    CHECK(std::find(backend.sent.begin(), backend.sent.end(),
                    "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=0") != backend.sent.end());

    fire_vars(backend, json{{"filament_slot0", 1}, {"color_slot0", 18}, {"vendor_slot0", 0}});
    CHECK(backend.get_slot_info(0).brand == "Polymaker");
}

TEST_CASE("QIDI Box apply_user_edit rejects out-of-range slot index",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SlotInfo info;
    info.material = "PLA";
    REQUIRE_FALSE(helix::test::apply_edit(backend, -1, info).success());
    REQUIRE_FALSE(helix::test::apply_edit(backend, 99, info).success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box apply_user_edit accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{{"box_count", 2}});
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SlotInfo info;
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0xFF362D;

    auto err = helix::test::apply_edit(backend, 5, info);
    REQUIRE(err.success());

    bool saw_fila = false;
    for (const auto& g : backend.sent) {
        saw_fila = saw_fila || g == "SAVE_VARIABLE VARIABLE=filament_slot5 VALUE=11";
    }
    REQUIRE(saw_fila);
}

TEST_CASE("QIDI Box apply_user_edit with no palette/vendor data still writes fila",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // Only fila profiles loaded — no colordict / vendor_list.
    QidiBoxTestAccess::apply_filas_list(backend, "[fila1]\nfilament = PLA Rapido\ntype = PLA\n"
                                                 "min_temp = 190\nmax_temp = 240\n");
    SlotInfo info;
    info.material = "PLA";
    info.brand = "eSUN";
    info.color_rgb = 0x123456;

    auto err = helix::test::apply_edit(backend, 0, info);
    REQUIRE(err.success());
    // Only the filament_slot write — empty palette/vendor skip cleanly.
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=1");
}

TEST_CASE("QIDI Box apply_user_edit carries an explicit zero for a cleared field",
          "[ams][qidi_box][write_path][1633]") {
    helix::test::RegisteredWired<RecordingQidiBackend> harness;
    auto& backend = *harness;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    // The Box states a full identity for slot 0, so the edit has an id to
    // clear and the fingerprint baseline the echo is judged against.
    fire_vars(
        backend,
        json{{"box_count", 1}, {"filament_slot0", 11}, {"color_slot0", 18}, {"vendor_slot0", 2}});

    auto sent_fila = [&backend] {
        for (const auto& g : backend.sent) {
            if (g.rfind("SAVE_VARIABLE VARIABLE=filament_slot0", 0) == 0) {
                return g;
            }
        }
        return std::string{};
    };
    auto sent_color = [&backend] {
        for (const auto& g : backend.sent) {
            if (g.rfind("SAVE_VARIABLE VARIABLE=color_slot0", 0) == 0) {
                return g;
            }
        }
        return std::string{};
    };

    SECTION("material cleared alone writes fila zero") {
        SlotInfo info = backend.get_slot_info(0);
        info.material.clear();

        helix::test::edit_slot_as_user(backend, 0, info);
        // The zero is the whole point: without it the Box keeps restating
        // fila 11 forever and the user's clear never reaches the variables.
        REQUIRE(sent_fila() == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=0");
        // The untouched colour still resolves to its row.
        REQUIRE(sent_color() == "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=18");

        // The zero echoes back as a fingerprint change while the edit is in
        // flight, which is ours, not a spool swap.
        fire_vars(backend, json{{"box_count", 1},
                                {"filament_slot0", 0},
                                {"color_slot0", 18},
                                {"vendor_slot0", 2}});
        CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
    }

    SECTION("colour cleared alone writes color zero") {
        SlotInfo info = backend.get_slot_info(0);
        info.color_rgb = kAmsDefaultSlotColor;

        helix::test::edit_slot_as_user(backend, 0, info);
        REQUIRE(sent_color() == "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=0");
        // The untouched material keeps the row the Box holds: no rewrite.
        REQUIRE(sent_fila().empty());

        fire_vars(backend, json{{"box_count", 1},
                                {"filament_slot0", 11},
                                {"color_slot0", 0},
                                {"vendor_slot0", 2}});
        CHECK(QidiBoxTestAccess::get_override(backend, 0).has_value());
    }

    SECTION("a cleared field the Box never held writes no zero") {
        // No fila was ever stated for slot 1, so an empty material has
        // nothing to clear and must not invent a write.
        SlotInfo info;
        info.material.clear();
        info.color_rgb = 0xFF362D;

        REQUIRE(helix::test::apply_edit(backend, 1, info).success());
        for (const auto& g : backend.sent) {
            CHECK(g.rfind("SAVE_VARIABLE VARIABLE=filament_slot1", 0) != 0);
        }
    }
}

// Error-center bridge: current_error()

TEST_CASE("QIDI Box current_error returns nullopt when no slots are blocked",
          "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // Default state: all slots UNKNOWN
    REQUIRE_FALSE(backend.current_error().has_value());

    // Slots with non-BLOCKED statuses also return nullopt
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", 1}, // AVAILABLE
                           {"slot1", 2}, // LOADED
                           {"slot2", 0}, // EMPTY
                           {"slot3", 1}, // AVAILABLE
                       });
    REQUIRE_FALSE(backend.current_error().has_value());
}

TEST_CASE("QIDI Box current_error returns CRITICAL event for first blocked slot",
          "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // slot1 blocked (value -3 = runout-during-print)
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", 1},  // AVAILABLE
                           {"slot1", -3}, // BLOCKED
                           {"slot2", 1},  // AVAILABLE
                           {"slot3", 1},  // AVAILABLE
                       });

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::QIDI);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK_FALSE(ev->title.empty());
    // The whole label, not just the digit: the noun comes from the backend's
    // lane_noun() and the number from lane_number(), and a user reads both.
    CHECK(ev->detail.find("Slot 2") != std::string::npos);
    CHECK(ev->sticky);
    // Recovery has one dismiss affordance — a button-less modal is a non-dismissible
    // UI trap (RecoveryModalPresenter with 0 buttons hides the button container).
    CHECK(ev->recovery_actions.size() == 1);
    // Empty gcode == dismiss: closes the modal, sends nothing. Previously this
    // had to be a Klipper comment because a blank gcode was sent as the LABEL
    // ("OK" to Klipper); that fallback is gone (#1172).
    CHECK(ev->recovery_actions[0].gcode.empty());
}

TEST_CASE("QIDI Box current_error scans slots from later boxes", "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{
                           {"enable_box", 1}, {"box_count", 2}, {"slot5", -3}, // BLOCKED
                       });

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->detail.find("Slot 6") != std::string::npos); // 1-based: global slot 5
}

TEST_CASE("QIDI Box current_error picks the first blocked slot when multiple blocked",
          "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", -1}, // BLOCKED (slot-load-fail)
                           {"slot1", -2}, // BLOCKED (extruder-load-fail)
                           {"slot2", 1},
                           {"slot3", 1},
                       });

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    // First blocked slot is index 0 → lane 1
    CHECK(ev->detail.find("Slot 1") != std::string::npos);
}

TEST_CASE("QIDI Box current_error clears when slot unblocks", "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", -1}, // BLOCKED
                           {"slot1", 1},
                           {"slot2", 1},
                           {"slot3", 1},
                       });
    REQUIRE(backend.current_error().has_value());

    // Slot recovers (positive value = AVAILABLE)
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", 1}, // was BLOCKED, now AVAILABLE
                           {"slot1", 1},
                           {"slot2", 1},
                           {"slot3", 1},
                       });
    REQUIRE_FALSE(backend.current_error().has_value());
}

TEST_CASE("QIDI Box current_error words the fault retry_step names and offers Retry",
          "[ams][qidi_box][error-center][qidi]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    PausedPrint paused;
    // QDE_004_001 on the Box's second slot: the firmware sets slot1 to -1 and
    // records the step for TRY_MOVE_AGAIN.
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", 1},
                           {"slot1", -1},
                           {"retry_step", "QDE_004_001_2"},
                       });
    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->code == "QDE_004_001");
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->title.find("Slot 2") != std::string::npos);
    REQUIRE(ev->recovery_actions.size() == 2);
    CHECK(ev->recovery_actions[0].gcode == "TRY_MOVE_AGAIN");
    // The action carries the slot it moves filament on.
    CHECK(ev->recovery_actions[0].acts_on_lane == 1);
    CHECK(ev->recovery_actions[1].gcode.empty());
}

TEST_CASE("QIDI Box offers no Retry outside a paused print",
          "[ams][qidi_box][error-center][qidi]") {
    // Both slotN and retry_step persist across restarts: at boot or idle the
    // fault shows, but a one-tap heat and filament move on days-old state does
    // not.
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot1", -1},
                           {"retry_step", "QDE_004_001_2"},
                       });
    const auto standing = backend.current_error();
    REQUIRE(standing.has_value());
    CHECK(standing->title.find("Slot 2") != std::string::npos);
    CHECK_FALSE(offers_try_again(standing));

    helix::ClassifyContext printing;
    printing.is_printing = true;
    const auto line = backend.classify_error(
        "!!Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
        "reload 1B.",
        printing);
    REQUIRE(line.has_value());
    CHECK_FALSE(offers_try_again(line));
}

TEST_CASE("QIDI Box offers no Retry when retry_step names another slot than the dialog",
          "[ams][qidi_box][error-center][qidi]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    PausedPrint paused;
    // Slot 2 is blocked, but the step TRY_MOVE_AGAIN would re-run is slot 3's
    // (a stale step, or a later fault that recorded none): retrying would move
    // filament on a slot the dialog does not name.
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot1", -1},
                           {"slot2", 1},
                           {"retry_step", "QDE_004_001_3"},
                       });
    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->detail.find("Slot 2") != std::string::npos);
    CHECK_FALSE(offers_try_again(ev));

    // `_00` is the firmware's "no slot": nothing for TRY_MOVE_AGAIN to name.
    fire_vars(backend, json{{"retry_step", "QDE_004_001_00"}});
    CHECK_FALSE(offers_try_again(backend.current_error()));
}

TEST_CASE("QIDI Box with two slots blocked retries only the one the dialog names",
          "[ams][qidi_box][error-center][qidi]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    PausedPrint paused;
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", -1},
                           {"slot1", -1},
                           {"retry_step", "QDE_004_001_2"},
                       });
    // The dialog names the first blocked slot; the step is the second's.
    auto first = backend.current_error();
    REQUIRE(first.has_value());
    CHECK(first->detail.find("Slot 1") != std::string::npos);
    CHECK_FALSE(offers_try_again(first));

    fire_vars(backend, json{{"retry_step", "QDE_004_001_1"}});
    auto named = backend.current_error();
    REQUIRE(named.has_value());
    CHECK(named->title.find("Slot 1") != std::string::npos);
    CHECK(offers_try_again(named));
}

TEST_CASE("QIDI Box retry_step alone does not raise a fault",
          "[ams][qidi_box][error-center][qidi]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // Persisted across a restart with every slot healthy: stale, not a fault.
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"box_count", 1},
                           {"slot0", 1},
                           {"slot1", 1},
                           {"retry_step", "QDE_004_006_2"},
                       });
    CHECK_FALSE(backend.current_error().has_value());
}

TEST_CASE("QIDI Box console fault gets Retry only once retry_step names it",
          "[ams][qidi_box][error-center][qidi]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    const std::string line = "// Code:QDE_004_006; Message:Extruder loading failure.";
    helix::ClassifyContext paused;
    paused.is_paused = true;

    fire_vars(backend, json{{"enable_box", 1}, {"box_count", 1}, {"retry_step", "None"}});
    // The line usually lands before retry_step does: the Box still owns it,
    // and offers no Retry yet.
    const auto early = backend.classify_error(line, paused);
    REQUIRE(early.has_value());
    CHECK(early->code == "QDE_004_006");
    CHECK_FALSE(offers_try_again(early));

    // retry_step now names this fault on slot 1, but the line names no slot:
    // the text would not say which slot Retry moves filament on.
    fire_vars(backend, json{{"retry_step", "QDE_004_006_1"}});
    const auto ev = backend.classify_error(line, paused);
    REQUIRE(ev.has_value());
    CHECK(ev->code == "QDE_004_006");
    CHECK_FALSE(offers_try_again(ev));

    // A different code is not the step TRY_MOVE_AGAIN would re-run.
    CHECK_FALSE(offers_try_again(backend.classify_error(
        "// Code:QDE_004_009; Message:Extruder unloading failure.", paused)));

    // A line naming another slot than retry_step's is not that step either.
    fire_vars(backend, json{{"retry_step", "QDE_004_001_2"}});
    const auto other_slot = backend.classify_error(
        "!!Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
        "reload 1C.",
        paused);
    REQUIRE(other_slot.has_value());
    CHECK_FALSE(offers_try_again(other_slot));
    const auto named = backend.classify_error(
        "!!Code:QDE_004_001; Message:Slot loading failure, please check the trigger, please "
        "reload 1B.",
        paused);
    REQUIRE(offers_try_again(named));
    // The tap re-checks the pause: the print can end while the dialog is up.
    CHECK(named->recovery_actions[0].requires_paused);
}

// AmsAction::ERROR from BLOCKED lane (AmsErrorBridge hook)
// AmsErrorBridge fires when a backend's action transitions INTO ERROR and
// current_error() returns an event. QIDI's parser never set ERROR — it only
// ever wrote LOADING or IDLE — so the bridge was unreachable in production.
// Fix: any BLOCKED slot forces action = ERROR with higher precedence than
// is_tool_change (a mid-flight change on a broken lane is still ERROR).

TEST_CASE("QIDI Box blocked lane sets action to ERROR so AmsErrorBridge fires",
          "[ams][qidi_box][error-center]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);

    // Feed a negative slot (runout-during-print = -3). action must become ERROR.
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"slot0", 1},
                           {"slot1", -3},
                           {"slot2", 1},
                           {"slot3", 1},
                       });
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);

    // Feed all-valid slots. action must leave ERROR.
    fire_vars(backend, json{
                           {"enable_box", 1},
                           {"slot0", 1},
                           {"slot1", 1},
                           {"slot2", 1},
                           {"slot3", 1},
                       });
    REQUIRE(backend.get_system_info().action != AmsAction::ERROR);
}
