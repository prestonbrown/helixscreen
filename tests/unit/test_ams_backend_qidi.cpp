// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 47 of test_ams_backend_qidi.cpp before it was split, in its order. Mostly [general],
// [write_path], [1199], [write_path][max4], [dryer], and more

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

// Type identification — pin down what the stub already advertises so
// later refactors don't silently change it.

TEST_CASE("QIDI Box type identification", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::QIDI_BOX);
    REQUIRE(backend.get_topology() == PathTopology::HUB);
}

TEST_CASE("QIDI Box default system_info shape", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    auto info = backend.get_system_info();

    REQUIRE(info.type == AmsType::QIDI_BOX);
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[0].topology == PathTopology::HUB);
    // Only an explicit enable_box=0 disconnects the unit; one that has not
    // reported yet reads connected, so the UI does not flash "Disconnected".
    REQUIRE(info.units[0].connected);

    // A box is an ordinal, not a lane position: zero-based lane numbers leave it alone.
    ScopedZeroLaneNumbering from_zero;
    CHECK(AmsBackendQidi(nullptr, nullptr).get_system_info().units[0].name == "QIDI Box 1");
}

// parse_save_variables: enable_box gate
// `box_extras.py` reads `save_variables.variables.enable_box` and treats
// 0 as "Box installed but disabled" / 1 as "Box active." Mirror that
// onto AmsUnit::connected so the UI can show the right state.

TEST_CASE("QIDI Box parse_save_variables: enable_box drives the unit's connection",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"enable_box", 0}});
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    fire_vars(backend, json{{"enable_box", 1}});

    REQUIRE(backend.get_system_info().units[0].connected);
    // The box publishes no toolhead sensor.
    CHECK_FALSE(backend.get_system_info().units[0].has_toolhead_sensor);
}

// parse_save_variables: box_count resizes the system
// `box_detect.py` writes save_variables.variables.box_count whenever USB
// enumeration changes. Each physical box = 4 slots, chainable up to 4
// boxes / 16 slots. The backend must model one AmsUnit per physical box so
// the UI stacks boxes instead of drawing one clipped slot row.

TEST_CASE("QIDI Box parse_save_variables: box_count=2 expands to two 4-slot units",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    REQUIRE(backend.get_system_info().total_slots == 4);

    fire_vars(backend, json{{"box_count", 2}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
    REQUIRE(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[1].first_slot_global_index == 4);

    for (const auto& unit : info.units) {
        for (size_t i = 0; i < unit.slots.size(); ++i) {
            REQUIRE(unit.slots[i].slot_index == static_cast<int>(i));
            REQUIRE(unit.slots[i].global_index ==
                    unit.first_slot_global_index + static_cast<int>(i));
        }
    }
}

TEST_CASE("QIDI Box parse_save_variables: box_count=0 removes all physical units",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{{"box_count", 0}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 0);
    REQUIRE(info.units.empty());
}

TEST_CASE("QIDI Box parse_save_variables: box_count=4 expands to sixteen slots",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{{"box_count", 4}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 16);
    REQUIRE(info.units.size() == 4);
    for (int unit = 0; unit < 4; ++unit) {
        REQUIRE(info.units[static_cast<size_t>(unit)].slot_count == 4);
        REQUIRE(info.units[static_cast<size_t>(unit)].first_slot_global_index == unit * 4);
    }
}

// Every QIDI Box feeds the printer's one extruder, so the overview draws one
// toolhead with each box routed into it.
TEST_CASE("QIDI Box: four boxes converge on one toolhead in the system path layout",
          "[ams][qidi_box][tool_layout][ams_draw]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"box_count", 4}});

    const auto layout = ams_draw::compute_system_tool_layout(backend.get_system_info(), &backend);

    CHECK(layout.total_physical_tools == 1);
    REQUIRE(layout.units.size() == 4);
    for (const auto& unit : layout.units) {
        CHECK(unit.tool_count == 1);
        CHECK(unit.first_physical_tool == 0);
    }
}

// parse_save_variables: per-slot state from slot<N> values
// box_stepper.py writes save_variables.variables.slot<N> as the slot's
// state machine cursor. From box_stepper.py LED-state mapping:
//   0   = empty / no filament
//   1   = filament loaded in box, retracted (available)
//   2   = filament loaded all the way to extruder
//   3   = mid-transition (loading/unloading in progress)
//   -1  = slot load failed
//   -2  = extruder load failed
//   -3  = runout-during-print detected by motion sensor

TEST_CASE("QIDI Box per-slot positive states map to SlotStatus", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{
                           {"slot0", 0}, // empty
                           {"slot1", 1}, // available (parked in box)
                           {"slot2", 2}, // loaded to extruder
                           {"slot3", 3}, // mid-transition
                       });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::EMPTY);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    // Mid-transition: show as AVAILABLE so UI doesn't flicker — the
    // foreground action belongs on system_info_.action, not slot status.
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("QIDI Box per-slot negative states map to BLOCKED", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    SECTION("-1 = slot load failed") {
        fire_vars(backend, json{{"slot0", -1}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
    SECTION("-2 = extruder load failed") {
        fire_vars(backend, json{{"slot0", -2}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
    SECTION("-3 = runout-during-print") {
        fire_vars(backend, json{{"slot0", -3}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
}

// parse_save_variables: value_t<N> tool->slot mapping
// box_extras.py stores tool mappings as save_variables.variables.value_t<N>
// with value "slot<M>". This means "tool N prints from slot M." Default
// (when value_t<N> is missing) is tool N = slot N, which the resize
// code already establishes.

TEST_CASE("QIDI Box parse_save_variables: value_t<N>=slot<M> maps tool N to slot M",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{
                           {"value_t0", "slot2"},
                           {"value_t1", "slot3"},
                       });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[2].mapped_tool == 0);
    REQUIRE(info.units[0].slots[3].mapped_tool == 1);
}

// handle_status routes save_variables changes through to parse
// Moonraker delivers save_variables changes inside notify_status_update as
// `{"save_variables": {"variables": {...}}}`. The backend must extract the
// inner variables payload and feed it to parse_save_variables so live
// updates flow through.

TEST_CASE("QIDI Box handle_status_update applies save_variables changes", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"enable_box", 0}});
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    backend.fire_status(helix::save_variables_frame(json{
        {"enable_box", 1},
        {"box_count", 2},
    }));

    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].connected);
    REQUIRE(info.units[1].connected);
    REQUIRE(info.total_slots == 8);
}

TEST_CASE("QIDI Box handle_status_update ignores unrelated keys", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    // Notification without save_variables shouldn't touch state.
    backend.fire_status(json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    REQUIRE(backend.get_system_info().units[0].connected);
    REQUIRE(backend.get_system_info().total_slots == 4);
}

// last_load_slot: which slot is currently in the extruder
// box_extras.py is the source of truth for "which slot is loaded right
// now." Per-slot `slot<N>=2` is the secondary signal (and may be stale
// after error recovery). When last_load_slot is set, that slot must be
// LOADED and no other slot should claim LOADED.

TEST_CASE("QIDI Box last_load_slot promotes a single slot to LOADED", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{
                           {"slot0", 1}, // available
                           {"slot1", 1}, // available
                           {"slot2", 1}, // available
                           {"slot3", 1}, // available
                           {"last_load_slot", "slot2"},
                       });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
    REQUIRE(backend.get_current_slot() == 2);
    // Default tool=slot mapping, so current_tool == current_slot.
    REQUIRE(backend.get_current_tool() == 2);
    REQUIRE(backend.is_filament_loaded());
}

TEST_CASE("QIDI Box last_load_slot=slot-1 means nothing is in the extruder", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    // Seat slot1 in its own frame so the clear has a current slot to reset.
    fire_vars(backend, json{{"slot1", 2}, {"last_load_slot", "slot1"}});
    REQUIRE(backend.is_filament_loaded());
    REQUIRE(backend.get_current_slot() == 1);

    // slot2 claims LOADED in the same frame that clears the extruder.
    fire_vars(backend, json{
                           {"slot2", 2},
                           {"last_load_slot", "slot-1"},
                       });

    REQUIRE(backend.get_system_info().units[0].slots[2].status == SlotStatus::AVAILABLE);
    REQUIRE_FALSE(backend.is_filament_loaded());
    REQUIRE(backend.get_current_slot() == -1);
    REQUIRE(backend.get_current_tool() == -1);
}

// slot<N>: 2 is firmware truth on its own

TEST_CASE("QIDI Box reports a slot loaded from its state word alone", "[ams][qidi_box][1199]") {
    // No last_load_slot has ever been seen, so the aggregate pair is empty;
    // slot2 = 2 alone still says that slot is loaded.
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot0", 1}, {"slot1", 0}, {"slot2", 2}});

    REQUIRE(backend.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(backend.slot_is_actively_loaded(2));
    CHECK_FALSE(backend.slot_is_actively_loaded(0));
    CHECK_FALSE(backend.slot_is_actively_loaded(1));
    CHECK_FALSE(backend.slot_is_actively_loaded(3));
}

TEST_CASE("QIDI Box out-of-range slots are false, not a crash", "[ams][qidi_box][1199]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot2", 2}, {"last_load_slot", "slot2"}});

    CHECK_FALSE(backend.slot_is_actively_loaded(-1));
    CHECK_FALSE(backend.slot_is_actively_loaded(99));
}

// The ordering hole: a slot<N> repeat must not demote the seated slot

TEST_CASE("QIDI Box keeps the seated slot LOADED across a slot-only payload",
          "[ams][qidi_box][1199]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot2", 2}, {"last_load_slot", "slot2"}, {"enable_box", 1}});
    REQUIRE(backend.get_system_info().current_slot == 2);
    REQUIRE(backend.is_filament_loaded());
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::LOADED);

    // The Box drops slot2's state word back to "available" without saying
    // anything about last_load_slot. The slot<N> loop runs first and wrote
    // AVAILABLE; the aggregate still names slot 2, so the two disagreed.
    fire_vars(backend, json{{"slot2", 1}});

    REQUIRE(backend.get_system_info().current_slot == 2);
    REQUIRE(backend.is_filament_loaded());
    CHECK(backend.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(backend.slot_is_actively_loaded(2));
}

TEST_CASE("QIDI Box a blocked seated slot stays BLOCKED", "[ams][qidi_box][1199]") {
    // A negative state word is the Box reporting a fault on that slot. Painting
    // it LOADED because the aggregate still names it would hide the error the
    // UI exists to show, so BLOCKED wins the reconciliation.
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot2", 2}, {"last_load_slot", "slot2"}});
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::LOADED);

    fire_vars(backend, json{{"slot2", -1}});

    CHECK(backend.get_slot_info(2).status == SlotStatus::BLOCKED);
    CHECK_FALSE(backend.slot_is_actively_loaded(2));
}

// A toolchange moves the stamp, and never leaves two slots loaded

TEST_CASE("QIDI Box moves the LOADED stamp on a tool change", "[ams][qidi_box][1199]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"slot0", 2}, {"slot1", 1}, {"slot2", 1}, {"last_load_slot", "slot0"}});
    REQUIRE(backend.slot_is_actively_loaded(0));

    fire_vars(backend, json{{"slot0", 1}, {"slot1", 2}, {"slot2", 1}, {"last_load_slot", "slot1"}});

    CHECK(backend.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(backend.slot_is_actively_loaded(1));
    CHECK(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(backend.slot_is_actively_loaded(0));
}

// parse_save_variables: RFID per-slot indices
// box_extras.py writes save_variables.variables.filament_slot<N> (1-99,
// index into officiall_filas_list.cfg), color_slot<N> (1-24, index into
// the color palette), and vendor_slot<N> (always 1 in the wild so far).
// The backend captures the raw IDs into a private side-table; resolution
// to material/color happens in a follow-up cycle once the cfg resolver
// lands.

// handle_status: heater_box drying state + aht20_f humidity
// The QIDI Box has per-box drying: heater_generic heater_box<N> provides
// temperature + target, aht20_f heater_box<N> provides humidity. Each physical
// box is modeled as its own AmsUnit so the UI can show the matching env data.

TEST_CASE("QIDI Box aht20_f heater_box1 populates humidity", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    backend.fire_status(
        json{{"aht20_f heater_box1", json{{"temperature", 23.0}, {"humidity", 38.7}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->has_humidity);
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(38.7).epsilon(0.01));
}

TEST_CASE("QIDI Box multiple heater_box readings populate matching units", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"box_count", 2}});
    REQUIRE_FALSE(backend.get_system_info().units[0].environment.has_value());

    backend.fire_status(json{
        {"heater_generic heater_box1", json{{"temperature", 50.0}}},
        {"heater_generic heater_box2", json{{"temperature", 22.5}}},
    });

    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[1].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(50.0).epsilon(0.01));
    REQUIRE(info.units[1].environment->temperature_c == Catch::Approx(22.5).epsilon(0.01));
}

// apply_query_response: bootstrap from printer.objects.query result
// on_started() issues a printer.objects.query to fetch the initial state
// of save_variables (and per-box heater objects when they exist). The
// response shape is `{result: {status: {save_variables: {...}, ...}}}`.
// apply_query_response unwraps the result.status envelope and feeds the
// inner object through handle_status, reusing every parser we
// already test.

TEST_CASE("QIDI Box apply_query_response unwraps result.status and parses", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    fire_vars(backend, json{{"enable_box", 0}});
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    json response = json{
        {"result",
         json{
             {"status",
              json{
                  {"save_variables",
                   json{{"variables", json{{"enable_box", 1}, {"box_count", 2}}}}},
              }},
         }},
    };
    QidiBoxTestAccess::apply_query(backend, response);

    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].connected);
    REQUIRE(info.units[1].connected);
    REQUIRE(info.total_slots == 8);
}

TEST_CASE("QIDI Box apply_query_response handles missing result gracefully", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    fire_vars(backend, json{{"enable_box", 0}});
    // Wrong-shape response — must not crash, must not mutate state.
    QidiBoxTestAccess::apply_query(backend, json{{"error", "timed out"}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
}

TEST_CASE("QIDI Box notifications without heater data leave environment alone", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;

    // Seed an environment reading.
    backend.fire_status(json{{"heater_generic heater_box1", json{{"temperature", 40.0}}}});
    REQUIRE(backend.get_system_info().units[0].environment.has_value());

    // Unrelated notification should not clobber.
    backend.fire_status(json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(40.0).epsilon(0.01));
}

// Write-path: always enabled (commands verified vs QIDI firmware, #1030)

TEST_CASE("QIDI Box reports environment sensors (dryer indicator reachable)", "[ams][qidi_box]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // The box has a PTC heater + aht20_f humidity chip; the env indicator widget
    // is hard-hidden unless the backend advertises environment sensors (#1041).
    REQUIRE(backend.has_environment_sensors());
}

TEST_CASE("QIDI Box manages preheat itself (no UI-driven heat on load)", "[ams][qidi_box]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    REQUIRE(backend.supports_auto_heat_on_load());
}

// A load heats, EXTRUDER_LOADs the slot by name (value_t mapping is for the
// slicer's T<n> and never changes which slot loads), clears the nozzle and
// cools, unloading a different loaded slot first so the load does not jam on
// it. An unload is the stock M603, or EXTRUDER_UNLOAD on firmware without it.
// change_tool resolves the tool's slot and takes the load path. Global slots
// past the first box address later boxes.
TEST_CASE("QIDI Box write path sends the stock load, unload and mapping commands",
          "[ams][qidi_box][write_path]") {
    using Op = std::function<helix::AmsError(RecordingQidiBackend&)>;
    const std::string load2 = "M109 S250\nEXTRUDER_LOAD SLOT=slot2\nCLEAR_NOZZLE\nM104 S0";
    const std::string unload = "M603 S250\nENABLE_ALL_SENSOR";
    struct Row {
        const char* name;
        json vars;
        bool m603, clear_nozzle;
        Op op;
        const char* sent; // nullptr = refused, nothing sent
    };
    const Row rows[] = {
        {"load into an empty extruder", json::object(), true, true,
         [](auto& b) { return b.load_filament(2); }, load2.c_str()},
        {"load past a tool mapping",
         {{"value_t0", "slot3"}},
         true,
         true,
         [](auto& b) { return b.load_filament(3); },
         "M109 S250\nEXTRUDER_LOAD SLOT=slot3\nCLEAR_NOZZLE\nM104 S0"},
        {"load over another loaded slot",
         {{"last_load_slot", "slot0"}},
         true,
         true,
         [](auto& b) { return b.load_filament(2); },
         "M603 S250\nENABLE_ALL_SENSOR\nM109 S250\nEXTRUDER_LOAD SLOT=slot2\nCLEAR_NOZZLE\nM104 "
         "S0"},
        {"reload the loaded slot",
         {{"last_load_slot", "slot2"}},
         true,
         true,
         [](auto& b) { return b.load_filament(2); },
         load2.c_str()},
        {"load a later box's slot",
         {{"box_count", 2}},
         true,
         true,
         [](auto& b) { return b.load_filament(5); },
         "M109 S250\nEXTRUDER_LOAD SLOT=slot5\nCLEAR_NOZZLE\nM104 S0"},
        {"load without CLEAR_NOZZLE", json::object(), true, false,
         [](auto& b) { return b.load_filament(2); },
         "M109 S250\nEXTRUDER_LOAD SLOT=slot2\nM104 S0"},
        {"unload a slot", json::object(), true, true, [](auto& b) { return b.unload_filament(1); },
         unload.c_str()},
        {"unload the loaded slot",
         {{"last_load_slot", "slot2"}},
         true,
         true,
         [](auto& b) { return b.unload_filament(-1); },
         unload.c_str()},
        {"unload a later box's slot",
         {{"box_count", 2}},
         true,
         true,
         [](auto& b) { return b.unload_filament(5); },
         unload.c_str()},
        {"unload with nothing loaded", json::object(), true, true,
         [](auto& b) { return b.unload_filament(-1); }, nullptr},
        {"unload without M603", json::object(), false, true,
         [](auto& b) { return b.unload_filament(1); },
         "M109 S250\nEXTRUDER_UNLOAD SLOT=slot1\nM104 S0"},
        {"change to tool 3", json::object(), true, true, [](auto& b) { return b.change_tool(3); },
         "M109 S250\nEXTRUDER_LOAD SLOT=slot3\nCLEAR_NOZZLE\nM104 S0"},
        {"change to a tool mapped into a later box",
         {{"box_count", 2}, {"value_t0", "slot5"}},
         true,
         true,
         [](auto& b) { return b.change_tool(0); },
         "M109 S250\nEXTRUDER_LOAD SLOT=slot5\nCLEAR_NOZZLE\nM104 S0"},
        {"map a tool", json::object(), true, true, [](auto& b) { return b.set_tool_mapping(1, 3); },
         "SAVE_VARIABLE VARIABLE=value_t1 VALUE='\"slot3\"'"},
        {"map a tool to a later box's slot",
         {{"box_count", 2}},
         true,
         true,
         [](auto& b) { return b.set_tool_mapping(1, 5); },
         "SAVE_VARIABLE VARIABLE=value_t1 VALUE='\"slot5\"'"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        // A ready, homed printer: the commands go straight out, with no G28 ahead of them.
        MockPrinter printer;
        printer.state.update_from_status(
            json{{"webhooks", {{"state", "ready"}}}, {"toolhead", {{"homed_axes", "xyz"}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
        helix::test::StartedBackend<RecordingQidiBackend> backend(&printer.api);
        if (!row.vars.empty())
            fire_vars(backend, row.vars);
        QidiBoxTestAccess::set_fw_caps(backend, row.m603, row.clear_nozzle);
        const auto err = row.op(backend);
        if (row.sent) {
            CHECK(err.success());
            REQUIRE(backend.sent.size() == 1);
            CHECK(backend.sent[0] == row.sent);
        } else {
            CHECK_FALSE(err.success());
            CHECK(backend.sent.empty());
        }
    }
}

TEST_CASE("QIDI Box lane eject is unsupported until force_move is enabled",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;

    REQUIRE_FALSE(backend.supports_lane_eject());
    auto err = backend.eject_lane(0);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box [force_move] enable_force_move turns on lane eject",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    REQUIRE(backend.supports_lane_eject());

    auto err = backend.eject_lane(1);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "FORCE_MOVE STEPPER=\"box_stepper slot1\" VELOCITY=100 DISTANCE=-878");
}

TEST_CASE("QIDI Box lane eject accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    fire_vars(backend, json{{"box_count", 2}});
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    auto err = backend.eject_lane(5);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "FORCE_MOVE STEPPER=\"box_stepper slot5\" VELOCITY=100 DISTANCE=-878");
}

TEST_CASE("QIDI Box eject sliders set the distance and speed lane eject uses",
          "[ams][qidi_box][write_path][device_actions]") {
    // Different QIDI Box variants need different push-out distances (#1041). The
    // stored positive magnitude is negated into the box direction.
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects(); // idempotent; ensures the subjects hold real values
    const int saved_dist = settings.get_ams_eject_distance();
    const int saved_vel = settings.get_ams_eject_velocity();

    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    const auto sections = backend.get_device_sections();
    REQUIRE(sections.size() == 1);
    const auto actions = backend.get_device_actions();
    REQUIRE(actions.size() == 2);
    for (const auto& a : actions) {
        CHECK(a.type == helix::printer::ActionType::SLIDER);
        CHECK(a.section == sections[0].id);
    }

    // The sliders send float, as the section overlay does.
    REQUIRE(backend.execute_device_action("eject_distance", std::any(500.0f)).success());
    REQUIRE(backend.execute_device_action("eject_velocity", std::any(60.0f)).success());
    CHECK(backend.sent.empty());
    CHECK(helix::printer::device_action_number(backend.get_device_actions()[0].current_value) ==
          500.0);

    auto err = backend.eject_lane(2);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "FORCE_MOVE STEPPER=\"box_stepper slot2\" VELOCITY=60 DISTANCE=-500");

    CHECK_FALSE(backend.execute_device_action("eject_distance").success());
    CHECK_FALSE(backend.execute_device_action("eject_nonsense", std::any(1.0f)).success());

    // Restore so sibling tests in this shard see the default eject gcode.
    settings.set_ams_eject_distance(saved_dist);
    settings.set_ams_eject_velocity(saved_vel);
}

TEST_CASE("QIDI Box supplies its own eject defaults while none is stored",
          "[ams][qidi_box][write_path][device_actions]") {
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    const int saved_dist = settings.get_ams_eject_distance();
    const int saved_vel = settings.get_ams_eject_velocity();

    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    // The generic setting stores 0 until a value is chosen; the box fills it in.
    settings.set_ams_eject_distance(0);
    settings.set_ams_eject_velocity(0);
    REQUIRE(settings.get_ams_eject_distance() == 0);
    CHECK(helix::printer::device_action_number(backend.get_device_actions()[0].current_value) ==
          878.0);
    CHECK(helix::printer::device_action_number(backend.get_device_actions()[1].current_value) ==
          100.0);
    REQUIRE(backend.eject_lane(1).success());
    REQUIRE(backend.sent.size() == 1);
    CHECK(backend.sent[0] == "FORCE_MOVE STEPPER=\"box_stepper slot1\" VELOCITY=100 DISTANCE=-878");

    // A stored value below the box's slider range is raised to it.
    settings.set_ams_eject_distance(50);
    settings.set_ams_eject_velocity(5);
    REQUIRE(backend.eject_lane(1).success());
    REQUIRE(backend.sent.size() == 2);
    CHECK(backend.sent[1] == "FORCE_MOVE STEPPER=\"box_stepper slot1\" VELOCITY=10 DISTANCE=-100");

    settings.set_ams_eject_distance(saved_dist);
    settings.set_ams_eject_velocity(saved_vel);
}

TEST_CASE("QIDI Box [force_move] disabled keeps lane eject off", "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", false}}}});

    REQUIRE_FALSE(backend.supports_lane_eject());
}

// Controller dialect (multi_color_controller): the Max 4 box rejects the
// box_stepper FORCE_MOVE with "Invalid pin value" and drives load, unload and
// eject through the multi_color_controller command surface instead. Presence of
// a [multi_color_controller] config section is the dialect discriminator: the
// Max 4 and Q2 firmware 01.01.02.x carry it, Q2 1.1.x and the Plus 4 do not.
// (prestonbrown/helixscreen#1083)

TEST_CASE("QIDI Box multi_color_controller dialect loads and unloads through the controller",
          "[ams][qidi_box][write_path][max4]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller", json::object()}});
    std::string expected;
    SECTION("a load into an empty extruder") {
        REQUIRE(backend.load_filament(2).success());
        expected = "MULTI_COLOR_LOAD SLOT=slot2";
    }
    SECTION("a load over another slot: the controller swaps it, so no unload goes first") {
        fire_vars(backend, json{{"last_load_slot", "slot0"}});
        REQUIRE(backend.load_filament(2).success());
        expected = "MULTI_COLOR_LOAD SLOT=slot2";
    }
    SECTION("an unload of a named slot") {
        REQUIRE(backend.unload_filament(1).success());
        expected = "MULTI_COLOR_UNLOAD SLOT=slot1";
    }
    SECTION("an unload of the active slot") {
        fire_vars(backend, json{{"last_load_slot", "slot3"}});
        REQUIRE(backend.unload_filament(-1).success());
        expected = "MULTI_COLOR_UNLOAD SLOT=slot3";
    }
    REQUIRE(backend.sent.size() == 1);
    CHECK(backend.sent[0] == expected);
}

TEST_CASE("QIDI Box Max 4 dialect ejects via MULTI_COLOR_BOX_UNLOAD",
          "[ams][qidi_box][write_path][max4]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // [multi_color_controller] present, force_move absent — the Max 4 shape.
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller", json::object()}});

    // Eject is available on the Max 4 without [force_move] enable_force_move.
    REQUIRE(backend.supports_lane_eject());

    auto err = backend.eject_lane(1);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "MULTI_COLOR_BOX_UNLOAD SLOT=slot1");
}

TEST_CASE("QIDI Box Max 4 dialect is detected from an instanced section key",
          "[ams][qidi_box][write_path][max4]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // Some configs instance the section (e.g. "[multi_color_controller box0]").
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller box0", json::object()}});

    REQUIRE(backend.supports_lane_eject());
    auto err = backend.eject_lane(0);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "MULTI_COLOR_BOX_UNLOAD SLOT=slot0");
}

TEST_CASE("QIDI Box Max 4 dialect takes precedence over box_stepper FORCE_MOVE",
          "[ams][qidi_box][write_path][max4]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    // Even if force_move is also enabled, a multi_color_controller printer must
    // use MULTI_COLOR_BOX_UNLOAD, never the FORCE_MOVE that errors on the Max 4.
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller", json::object()},
                                                  {"force_move", {{"enable_force_move", true}}}});

    auto err = backend.eject_lane(2);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "MULTI_COLOR_BOX_UNLOAD SLOT=slot2");
    REQUIRE(backend.sent[0].find("FORCE_MOVE") == std::string::npos);
}

TEST_CASE("QIDI Box Max 4 dialect still rejects an out-of-range slot",
          "[ams][qidi_box][write_path][max4]") {
    // The slot-range guard must run before the dialect branch — a bad index must
    // never reach MULTI_COLOR_BOX_UNLOAD. With no unit configured, any index is
    // out of range.
    helix::test::StartedBackend<RecordingQidiBackend> backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller", json::object()}});

    auto err = backend.eject_lane(99);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

// Full-stack integration: on_started actually fires the bootstrap query
// Unit-style tests (above) cover what we DO with response data, but they
// pass nullptr for the Moonraker stack so they don't prove on_started()
// even dispatches the query. This test wires up the full MoonrakerClientMock
// stack and asserts the dispatch happened with the expected method.
//
// One integration test catches the wiring; the unit-style tests cover the
// dense behaviour. Both have a job.

TEST_CASE("QIDI Box on_started dispatches printer.objects.query (integration)",
          "[ams][qidi_box][integration]") {
    MockPrinter mock_printer;
    auto& client = mock_printer.client;
    auto& api = mock_printer.api;

    AmsBackendQidi backend(&api, &client);
    REQUIRE(client.last_send_method().empty());

    auto err = backend.start();
    REQUIRE(err.success());

    // start() calls on_started() which must dispatch printer.objects.query.
    // last_send_method() is captured synchronously inside the mock so we can
    // assert before draining the deferred response callback.
    REQUIRE(client.last_send_method() == "printer.objects.query");
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

TEST_CASE("QIDI Box write-path rejects out-of-range slot/tool indices",
          "[ams][qidi_box][write_path]") {
    helix::test::StartedBackend<RecordingQidiBackend> backend;

    SECTION("load_filament: negative slot") {
        REQUIRE_FALSE(backend.load_filament(-1).success());
    }
    SECTION("load_filament: slot >= slot_count") {
        REQUIRE_FALSE(backend.load_filament(99).success());
    }
    SECTION("unload_filament: explicit out-of-range slot") {
        REQUIRE_FALSE(backend.unload_filament(99).success());
    }
    SECTION("change_tool: negative tool") {
        REQUIRE_FALSE(backend.change_tool(-1).success());
    }
    SECTION("set_tool_mapping: out-of-range") {
        REQUIRE_FALSE(backend.set_tool_mapping(-1, 0).success());
        REQUIRE_FALSE(backend.set_tool_mapping(0, 99).success());
    }
    REQUIRE(backend.sent.empty());
}

// last_load_slot also drives current_slot / current_tool / filament_loaded
// The AmsSubscriptionBackend base exposes these via get_current_slot() /
// get_current_tool() / is_filament_loaded(), reading directly from
// system_info_. Mirroring last_load_slot onto slot.status alone left the
// rest of the system at -1 / false even when something was clearly loaded.

TEST_CASE("QIDI Box current_tool follows value_t mapping", "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // Map slot 3 to tool 0, then load it.
    fire_vars(backend, json{
                           {"value_t0", "slot3"},
                           {"last_load_slot", "slot3"},
                       });

    REQUIRE(backend.get_current_slot() == 3);
    REQUIRE(backend.get_current_tool() == 0);
}

// is_tool_change reflects through to AmsAction
// box_extras.py sets save_variables.is_tool_change=1 while
// _BOX_CHANGE_FILAMENT is running, clears it on completion. Map this
// onto AmsAction so the UI shows an in-flight indicator.

TEST_CASE("QIDI Box get_slot_info returns valid SlotInfo for expanded slots (box_count>1)",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // Default backend is 4 slots; expand to 8 (box_count=2).
    fire_vars(backend, json{
                           {"box_count", 2}, {"slot5", 2}, // mark slot 5 LOADED
                       });

    auto info = backend.get_slot_info(5);
    REQUIRE(info.slot_index == 1);
    REQUIRE(info.global_index == 5);
    REQUIRE(info.status == SlotStatus::LOADED);
    // Index past the expanded count still rejects.
    REQUIRE(backend.get_slot_info(99).slot_index == -1);
}

TEST_CASE("QIDI Box is_tool_change shows LOADING while set and IDLE once cleared",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    fire_vars(backend, json{{"is_tool_change", 1}});
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    fire_vars(backend, json{{"is_tool_change", 0}});

    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("QIDI Box parse_save_variables applies cached profile to SlotInfo temps",
          "[ams][qidi_box]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    // Cache one profile, then mirror a slot pointing to it.
    QidiBoxTestAccess::apply_filas_list(backend, "[fila3]\nmin_temp=205\nmax_temp=225\n"
                                                 "box_min_temp=45\nbox_max_temp=65\n");
    fire_vars(backend, json{{"filament_slot0", 3}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 205);
    REQUIRE(info.units[0].slots[0].nozzle_temp_max == 225);
}

// Dryer capabilities (issue #1019)
// The QIDI Box has a PTC box heater that acts as a filament dryer.
// The backend must advertise dryer support with sane defaults so the
// UI shows the dryer control panel.

TEST_CASE("QIDI Box advertises dryer support with sane capability defaults",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    DryerInfo d = backend.get_dryer_info();
    REQUIRE(d.supported);
    REQUIRE(d.min_temp_c == Catch::Approx(35.0f));
    REQUIRE(d.max_temp_c == Catch::Approx(90.0f)); // settable ceiling, pre-config-query
    REQUIRE(d.max_duration_min == 720);
}

TEST_CASE("QIDI Box heater status populates dryer current/target temp", "[ams][qidi_box][dryer]") {
    helix::test::StartedBackend<AmsBackendQidi> backend;
    backend.fire_status(
        json{{"heater_generic heater_box1", json{{"temperature", 48.0}, {"target", 55.0}}}});

    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.current_temp_c == Catch::Approx(48.0f).epsilon(0.01));
    REQUIRE(d.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
}

TEST_CASE("QIDI Box drying_state end_time drives the remaining and derived total minutes",
          "[ams][qidi_box][dryer]") {
    const auto dryer_at = [](std::time_t now, int dry_state, int end_time) {
        AmsBackendQidi backend(nullptr, nullptr);
        QidiBoxTestAccess::set_clock(backend, [now] { return now; });
        QidiBoxTestAccess::apply_box_extras(
            backend,
            json{{"box_drying_state",
                  json{{"box1", json{{"dry_state", dry_state}, {"end_time", end_time}}}}}});
        return QidiBoxTestAccess::get_dryer(backend);
    };
    const DryerInfo running = dryer_at(1000, 1, 2800);
    CHECK(running.active);
    CHECK(running.remaining_min == 30);
    const DryerInfo past = dryer_at(5000, 0, 2800);
    CHECK_FALSE(past.active);
    CHECK(past.remaining_min == 0);
    // Started outside HelixScreen, with no commanded duration: the ring starts full.
    const DryerInfo external = dryer_at(1000, 1, 4600);
    CHECK(external.duration_min == 60);
    CHECK(external.get_progress_pct() == 0);
}
