// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_qidi.h"
#include "ams_error.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/update_queue_test_access.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

// Subclass that captures execute_gcode() invocations so write-path tests
// can assert the exact gcode emitted without needing a real Moonraker.
class RecordingQidiBackend : public AmsBackendQidi {
  public:
    RecordingQidiBackend() : AmsBackendQidi(nullptr, nullptr) {}
    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    std::vector<std::string> sent;
};

// Build a Moonraker-shaped status notification carrying save_variables.
static json make_save_variables_notification(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// =====================================================================
// Type identification — pin down what the stub already advertises so
// later refactors don't silently change it.
// =====================================================================

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
    // Unit must report as disconnected until enable_box=1 arrives.
    REQUIRE_FALSE(info.units[0].connected);
}

// =====================================================================
// parse_save_variables: enable_box gate
// =====================================================================
// `box_extras.py` reads `save_variables.variables.enable_box` and treats
// 0 as "Box installed but disabled" / 1 as "Box active." Mirror that
// onto AmsUnit::connected so the UI can show the right state.

TEST_CASE("QIDI Box parse_save_variables: enable_box=1 connects the unit", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::parse_vars(backend, json{{"enable_box", 1}});

    REQUIRE(backend.get_system_info().units[0].connected);
}

// =====================================================================
// parse_save_variables: box_count resizes the system
// =====================================================================
// `box_detect.py` writes save_variables.variables.box_count whenever USB
// enumeration changes. Each physical box = 4 slots, chainable up to 4
// boxes / 16 slots. The backend must model one AmsUnit per physical box so
// the UI stacks boxes instead of drawing one clipped slot row.

TEST_CASE("QIDI Box parse_save_variables: box_count=2 expands to two 4-slot units",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE(backend.get_system_info().total_slots == 4);

    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

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
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 0}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 0);
    REQUIRE(info.units.empty());
}

TEST_CASE("QIDI Box parse_save_variables: box_count=4 expands to sixteen slots",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 4}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 16);
    REQUIRE(info.units.size() == 4);
    for (int unit = 0; unit < 4; ++unit) {
        REQUIRE(info.units[static_cast<size_t>(unit)].slot_count == 4);
        REQUIRE(info.units[static_cast<size_t>(unit)].first_slot_global_index == unit * 4);
    }
}

// =====================================================================
// parse_save_variables: per-slot state from slot<N> values
// =====================================================================
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
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
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
    AmsBackendQidi backend(nullptr, nullptr);

    SECTION("-1 = slot load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -1}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
    SECTION("-2 = extruder load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -2}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
    SECTION("-3 = runout-during-print") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -3}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status == SlotStatus::BLOCKED);
    }
}

// =====================================================================
// parse_save_variables: value_t<N> tool->slot mapping
// =====================================================================
// box_extras.py stores tool mappings as save_variables.variables.value_t<N>
// with value "slot<M>". This means "tool N prints from slot M." Default
// (when value_t<N> is missing) is tool N = slot N, which the resize
// code already establishes.

TEST_CASE("QIDI Box parse_save_variables: value_t<N>=slot<M> maps tool N to slot M",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot2"},
                                               {"value_t1", "slot3"},
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[2].mapped_tool == 0);
    REQUIRE(info.units[0].slots[3].mapped_tool == 1);
}

// =====================================================================
// handle_status_update routes save_variables changes through to parse
// =====================================================================
// Moonraker delivers save_variables changes inside notify_status_update as
// `{"save_variables": {"variables": {...}}}`. The backend must extract the
// inner variables payload and feed it to parse_save_variables so live
// updates flow through.

TEST_CASE("QIDI Box handle_status_update applies save_variables changes", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::handle_status(backend, make_save_variables_notification(json{
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
    AmsBackendQidi backend(nullptr, nullptr);

    // Notification without save_variables shouldn't touch state.
    QidiBoxTestAccess::handle_status(backend,
                                     json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
    REQUIRE(backend.get_system_info().total_slots == 4);
}

// =====================================================================
// last_load_slot: which slot is currently in the extruder
// =====================================================================
// box_extras.py is the source of truth for "which slot is loaded right
// now." Per-slot `slot<N>=2` is the secondary signal (and may be stale
// after error recovery). When last_load_slot is set, that slot must be
// LOADED and no other slot should claim LOADED.

TEST_CASE("QIDI Box last_load_slot promotes a single slot to LOADED", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
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
}

TEST_CASE("QIDI Box last_load_slot=slot-1 means nothing is in the extruder", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Seed slot2 as loaded, then explicitly clear via last_load_slot
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot2", 2}, // claims LOADED
                                               {"last_load_slot", "slot-1"},
                                           });

    REQUIRE(backend.get_system_info().units[0].slots[2].status == SlotStatus::AVAILABLE);
}

// =====================================================================
// parse_save_variables: RFID per-slot indices
// =====================================================================
// box_extras.py writes save_variables.variables.filament_slot<N> (1-99,
// index into officiall_filas_list.cfg), color_slot<N> (1-24, index into
// the color palette), and vendor_slot<N> (always 1 in the wild so far).
// The backend captures the raw IDs into a private side-table; resolution
// to material/color happens in a follow-up cycle once the cfg resolver
// lands.

TEST_CASE("QIDI Box filament_slot<N> captures raw RFID material index", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 42},
                                               {"filament_slot1", 7},
                                           });

    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 42);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 1) == 7);
    // Unset slots default to 0 (= unknown).
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 2) == 0);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 3) == 0);
}

TEST_CASE("QIDI Box color_slot<N> captures raw RFID color index", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"color_slot0", 3},  // some palette index
                                               {"color_slot2", 24}, // max palette index
                                           });

    REQUIRE(QidiBoxTestAccess::color_id(backend, 0) == 3);
    REQUIRE(QidiBoxTestAccess::color_id(backend, 2) == 24);
    REQUIRE(QidiBoxTestAccess::color_id(backend, 1) == 0);
}

TEST_CASE("QIDI Box vendor_slot<N> captures raw RFID vendor index", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"vendor_slot0", 1},
                                               {"vendor_slot3", 1},
                                           });

    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 0) == 1);
    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 3) == 1);
    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 1) == 0);
}

TEST_CASE("QIDI Box RFID side-table resizes with box_count", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"box_count", 2},
                                               {"filament_slot7", 99},
                                           });

    REQUIRE(QidiBoxTestAccess::filament_id(backend, 7) == 99);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 0);
}

// =====================================================================
// handle_status_update: heater_box drying state + aht20_f humidity
// =====================================================================
// The QIDI Box has per-box drying: heater_generic heater_box<N> provides
// temperature + target, aht20_f heater_box<N> provides humidity. Each physical
// box is modeled as its own AmsUnit so the UI can show the matching env data.

TEST_CASE("QIDI Box heater_generic heater_box1 populates unit environment", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].environment.has_value());

    QidiBoxTestAccess::handle_status(
        backend,
        json{{"heater_generic heater_box1", json{{"temperature", 45.5}, {"target", 50.0}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(45.5).epsilon(0.01));
}

TEST_CASE("QIDI Box aht20_f heater_box1 populates humidity", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::handle_status(
        backend, json{{"aht20_f heater_box1", json{{"temperature", 23.0}, {"humidity", 38.7}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->has_humidity);
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(38.7).epsilon(0.01));
}

TEST_CASE("QIDI Box multiple heater_box readings populate matching units", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    QidiBoxTestAccess::handle_status(
        backend, json{
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

// QIDI Q2 firmware 01.01.02.01 (June 2026) refactor: box_config.py declares the
// dryer thermistor as `temperature_sensor heater_temp_a/b_box<N>` (#1047), but it
// still creates `heater_generic heater_box<N>`. Box temperature must come from the
// heater object, NOT the thermistor (which mirrors the heater element and would
// over-report). A thermistor-only delta must not drive the displayed box temp.
TEST_CASE("QIDI Box 01.01.02 box temp comes from heater_generic, not heater_temp thermistor",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // heater_temp thermistor reads hotter than the heater object; it must be
    // ignored for temperature, so the heater_generic value wins.
    QidiBoxTestAccess::handle_status(
        backend, json{
                     {"temperature_sensor heater_temp_a_box1", json{{"temperature", 99.0}}},
                     {"heater_generic heater_box1", json{{"temperature", 55.0}}},
                 });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(55.0).epsilon(0.01));
}

// An unrelated temperature_sensor (chamber/MCU) must NOT be mistaken for a box
// dryer thermistor — the "_box" guard keeps it out of the box environment.
TEST_CASE("QIDI Box non-box temperature_sensor is ignored", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::handle_status(backend,
                                     json{{"temperature_sensor Chamber_Thermal_Protection_Sensor",
                                           json{{"temperature", 60.0}}}});

    REQUIRE_FALSE(backend.get_system_info().units[0].environment.has_value());
}

// Defensive: if the 01.01.02 firmware relocates the humidity field onto a box
// object other than aht20_f, the backend still surfaces it (best-effort — the
// exact source is unconfirmed on hardware, #1047).
TEST_CASE("QIDI Box humidity is read from any matched box object", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::handle_status(backend,
                                     json{{"temperature_sensor heater_temp_a_box1",
                                           json{{"temperature", 30.0}, {"humidity", 22.0}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->has_humidity);
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(22.0).epsilon(0.01));
}

// =====================================================================
// apply_query_response: bootstrap from printer.objects.query result
// =====================================================================
// on_started() issues a printer.objects.query to fetch the initial state
// of save_variables (and per-box heater objects when they exist). The
// response shape is `{result: {status: {save_variables: {...}, ...}}}`.
// apply_query_response unwraps the result.status envelope and feeds the
// inner object through handle_status_update, reusing every parser we
// already test.

TEST_CASE("QIDI Box apply_query_response unwraps result.status and parses", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
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
    AmsBackendQidi backend(nullptr, nullptr);

    // Wrong-shape response — must not crash, must not mutate state.
    QidiBoxTestAccess::apply_query(backend, json{{"error", "timed out"}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
}

TEST_CASE("QIDI Box notifications without heater data leave environment alone", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Seed an environment reading.
    QidiBoxTestAccess::handle_status(
        backend, json{{"heater_generic heater_box1", json{{"temperature", 40.0}}}});
    REQUIRE(backend.get_system_info().units[0].environment.has_value());

    // Unrelated notification should not clobber.
    QidiBoxTestAccess::handle_status(backend,
                                     json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(40.0).epsilon(0.01));
}

// =====================================================================
// Write-path: always enabled (commands verified vs QIDI firmware, #1030)
// =====================================================================

TEST_CASE("QIDI Box reports environment sensors (dryer indicator reachable)", "[ams][qidi_box]") {
    RecordingQidiBackend backend;
    // The box has a PTC heater + aht20_f humidity chip; the env indicator widget
    // is hard-hidden unless the backend advertises environment sensors (#1041).
    REQUIRE(backend.has_environment_sensors());
}

TEST_CASE("QIDI Box manages preheat itself (no UI-driven heat on load)", "[ams][qidi_box]") {
    RecordingQidiBackend backend;
    REQUIRE(backend.supports_auto_heat_on_load());
}

TEST_CASE("QIDI Box load_filament: heats, EXTRUDER_LOADs the slot, clears, cools",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    // Fresh load into an empty extruder; no profile temp → default load temp.
    auto err = backend.load_filament(2);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot2\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box load_filament: addresses the slot directly, not value_t mapping",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    // value_t mapping is for the slicer's T<n> commands; EXTRUDER_LOAD takes the
    // slot name directly, so a mapping must not change which slot is loaded.
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot3"}});

    auto err = backend.load_filament(3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot3\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box load_filament: unloads a different loaded slot first",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Slot 0 is in the extruder; loading slot 2 must retract slot 0 first so
    // EXTRUDER_LOAD doesn't jam on top of loaded filament.
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot0"}});

    auto err = backend.load_filament(2);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "M603 S250\nM109 S250\nEXTRUDER_LOAD SLOT=slot2\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box load_filament accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    auto err = backend.load_filament(5);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot5\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box load_filament: reloading the active slot does not self-unload",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot2"}});

    auto err = backend.load_filament(2);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    // No leading M603 — slot 2 is already the loaded slot.
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot2\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box unload_filament: emits M603 (stock unload)", "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.unload_filament(1);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M603 S250");
}

TEST_CASE("QIDI Box unload_filament with -1 unloads the active slot via M603",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Seed slot 2 as LOADED so unload_filament(-1) targets it.
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot2"}});

    auto err = backend.unload_filament(-1);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M603 S250");
}

TEST_CASE("QIDI Box unload_filament accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    auto err = backend.unload_filament(5);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M603 S250");
}

TEST_CASE("QIDI Box unload_filament with -1 and nothing loaded errors",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.unload_filament(-1);

    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box unload falls back to EXTRUDER_UNLOAD when M603 is absent",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Simulate a firmware revision without the M603 macro.
    QidiBoxTestAccess::set_fw_caps(backend, /*m603=*/false, /*clear_nozzle=*/true);

    auto err = backend.unload_filament(1);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_UNLOAD SLOT=slot1\nM104 S0");
}

TEST_CASE("QIDI Box load omits CLEAR_NOZZLE when the macro is absent",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_fw_caps(backend, /*m603=*/true, /*clear_nozzle=*/false);

    auto err = backend.load_filament(2);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot2\nM104 S0");
}

TEST_CASE("QIDI Box change_tool resolves to the slot and drives the load path",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    // Default mapping is tool=slot, so changing to tool 3 loads slot 3 via the
    // verified EXTRUDER_LOAD sequence rather than a non-existent T3 macro.
    auto err = backend.change_tool(3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot3\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box change_tool resolves tool mappings in later boxes",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}, {"value_t0", "slot5"}});

    auto err = backend.change_tool(0);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "M109 S250\nEXTRUDER_LOAD SLOT=slot5\nCLEAR_NOZZLE\nM104 S0");
}

TEST_CASE("QIDI Box set_tool_mapping emits SAVE_VARIABLE for value_t<N>",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.set_tool_mapping(/*tool=*/1, /*slot_idx=*/3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"slot3\"");
}

TEST_CASE("QIDI Box set_tool_mapping accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    auto err = backend.set_tool_mapping(/*tool=*/1, /*slot_idx=*/5);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"slot5\"");
}

TEST_CASE("QIDI Box lane eject is unsupported until force_move is enabled",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    REQUIRE_FALSE(backend.supports_lane_eject());
    auto err = backend.eject_lane(0);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box [force_move] enable_force_move turns on lane eject",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
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
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    auto err = backend.eject_lane(5);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "FORCE_MOVE STEPPER=\"box_stepper slot5\" VELOCITY=100 DISTANCE=-878");
}

TEST_CASE("QIDI Box lane eject honors configurable distance/velocity settings",
          "[ams][qidi_box][write_path]") {
    // The eject FORCE_MOVE distance/velocity are user-tunable (#1041) because
    // different QIDI Box variants need different push-out distances. Verify
    // eject_lane reads the SettingsManager values, not the hardcoded defaults,
    // and that the stored positive magnitude is negated into the box direction.
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects(); // idempotent; ensures the subjects hold real values
    const int saved_dist = settings.get_qidi_eject_distance();
    const int saved_vel = settings.get_qidi_eject_velocity();

    settings.set_qidi_eject_distance(500);
    settings.set_qidi_eject_velocity(60);

    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", true}}}});

    auto err = backend.eject_lane(2);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] ==
            "FORCE_MOVE STEPPER=\"box_stepper slot2\" VELOCITY=60 DISTANCE=-500");

    // Restore defaults so sibling tests in this shard see the default eject gcode.
    settings.set_qidi_eject_distance(saved_dist > 0 ? saved_dist : 878);
    settings.set_qidi_eject_velocity(saved_vel > 0 ? saved_vel : 100);
}

TEST_CASE("QIDI Box [force_move] disabled keeps lane eject off", "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"force_move", {{"enable_force_move", false}}}});

    REQUIRE_FALSE(backend.supports_lane_eject());
}

// ---------------------------------------------------------------------------
// Max 4 dialect (multi_color_controller): the Max 4 box rejects the Q2
// box_stepper FORCE_MOVE with "Invalid pin value" and drives eject/unload
// through the multi_color_controller command surface instead. Presence of a
// [multi_color_controller] config section is the dialect discriminator; the
// Q2/Plus 4 box has box_stepper/box_extras but no multi_color_controller.
// (prestonbrown/helixscreen#1083)
// ---------------------------------------------------------------------------

TEST_CASE("QIDI Box Max 4 dialect ejects via MULTI_COLOR_BOX_UNLOAD",
          "[ams][qidi_box][write_path][max4]") {
    RecordingQidiBackend backend;
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
    RecordingQidiBackend backend;
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
    RecordingQidiBackend backend;
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
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_config_settings(backend,
                                             json{{"multi_color_controller", json::object()}});

    auto err = backend.eject_lane(99);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

// =====================================================================
// Full-stack integration: on_started actually fires the bootstrap query
// =====================================================================
// Unit-style tests (above) cover what we DO with response data, but they
// pass nullptr for the Moonraker stack so they don't prove on_started()
// even dispatches the query. This test wires up the full MoonrakerClientMock
// stack and asserts the dispatch happened with the expected method.
//
// One integration test catches the wiring; the unit-style tests cover the
// dense behaviour. Both have a job.

TEST_CASE("QIDI Box on_started dispatches printer.objects.query (integration)",
          "[ams][qidi_box][integration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendQidi backend(&api, &client);
    REQUIRE(client.last_send_method().empty());

    auto err = backend.start();
    REQUIRE(err.success());

    // start() calls on_started() which must dispatch printer.objects.query.
    // last_send_method() is captured synchronously inside the mock so we can
    // assert before draining the deferred response callback.
    REQUIRE(client.last_send_method() == "printer.objects.query");
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

TEST_CASE("QIDI Box write-path rejects out-of-range slot/tool indices",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

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

// =====================================================================
// apply_filas_list: parse officiall_filas_list.cfg (ConfigParser INI)
// =====================================================================
// box_extras.py looks up the printer-local file at
//   /home/mks/printer_data/config/officiall_filas_list.cfg
// using ConfigParser. Sections are `[fila<N>]` (N = filament_slot<N>
// index, 1-99) and each section carries min_temp / max_temp (nozzle)
// plus box_min_temp / box_max_temp (drying chamber). We fetch the file
// via Moonraker's file API and parse the same INI shape.

TEST_CASE("QIDI Box apply_filas_list parses sections into fila_profiles_", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
[fila1]
min_temp = 200
max_temp = 220
box_min_temp = 40
box_max_temp = 60

[fila2]
min_temp = 240
max_temp = 260
box_min_temp = 65
box_max_temp = 80
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);

    auto p1 = QidiBoxTestAccess::get_profile(backend, 1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->nozzle_min == 200);
    REQUIRE(p1->nozzle_max == 220);
    REQUIRE(p1->box_min == 40);
    REQUIRE(p1->box_max == 60);

    auto p2 = QidiBoxTestAccess::get_profile(backend, 2);
    REQUIRE(p2.has_value());
    REQUIRE(p2->nozzle_min == 240);
    REQUIRE(p2->nozzle_max == 260);
}

TEST_CASE("QIDI Box apply_filas_list tolerates whitespace, comments, blank lines",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
# leading comment
; semicolon-style comment

  [fila5]
    min_temp   =   195
   max_temp=215   ; inline tail (ignored)
box_min_temp = 35
box_max_temp = 55

# trailing comment
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);
    auto p = QidiBoxTestAccess::get_profile(backend, 5);
    REQUIRE(p.has_value());
    REQUIRE(p->nozzle_min == 195);
    REQUIRE(p->nozzle_max == 215);
    REQUIRE(p->box_min == 35);
    REQUIRE(p->box_max == 55);
}

TEST_CASE("QIDI Box apply_filas_list ignores non-fila sections", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
[printer]
kinematics = corexy

[fila7]
min_temp = 230
max_temp = 250
box_min_temp = 0
box_max_temp = 0
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);
    REQUIRE(QidiBoxTestAccess::get_profile(backend, 7).has_value());
    // No spurious profile from [printer] (parses as fila_id 0 only if we
    // mistakenly accept any section).
    REQUIRE_FALSE(QidiBoxTestAccess::get_profile(backend, 0).has_value());
}

// =====================================================================
// last_load_slot also drives current_slot / current_tool / filament_loaded
// =====================================================================
// The AmsSubscriptionBackend base exposes these via get_current_slot() /
// get_current_tool() / is_filament_loaded(), reading directly from
// system_info_. Mirroring last_load_slot onto slot.status alone left the
// rest of the system at -1 / false even when something was clearly loaded.

TEST_CASE("QIDI Box last_load_slot populates current_slot/current_tool", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot2", 2},
                                               {"last_load_slot", "slot2"},
                                           });

    REQUIRE(backend.get_current_slot() == 2);
    // Default tool=slot mapping, so current_tool == current_slot.
    REQUIRE(backend.get_current_tool() == 2);
    REQUIRE(backend.is_filament_loaded());
}

TEST_CASE("QIDI Box last_load_slot=slot-1 clears current_*", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Seed loaded first.
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot1"}});
    REQUIRE(backend.is_filament_loaded());

    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot-1"}});

    REQUIRE_FALSE(backend.is_filament_loaded());
    REQUIRE(backend.get_current_slot() == -1);
    REQUIRE(backend.get_current_tool() == -1);
}

TEST_CASE("QIDI Box current_tool follows value_t mapping", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Map slot 3 to tool 0, then load it.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot3"},
                                               {"last_load_slot", "slot3"},
                                           });

    REQUIRE(backend.get_current_slot() == 3);
    REQUIRE(backend.get_current_tool() == 0);
}

// =====================================================================
// is_tool_change reflects through to AmsAction
// =====================================================================
// box_extras.py sets save_variables.is_tool_change=1 while
// _BOX_CHANGE_FILAMENT is running, clears it on completion. Map this
// onto AmsAction so the UI shows an in-flight indicator.

TEST_CASE("QIDI Box is_tool_change=1 sets action to LOADING", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);

    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 1}});

    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);
}

TEST_CASE("QIDI Box get_slot_info returns valid SlotInfo for expanded slots (box_count>1)",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Default backend is 4 slots; expand to 8 (box_count=2).
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"box_count", 2}, {"slot5", 2}, // mark slot 5 LOADED
                                           });

    auto info = backend.get_slot_info(5);
    REQUIRE(info.slot_index == 1);
    REQUIRE(info.global_index == 5);
    REQUIRE(info.status == SlotStatus::LOADED);
    // Index past the expanded count still rejects.
    REQUIRE(backend.get_slot_info(99).slot_index == -1);
}

TEST_CASE("QIDI Box is_tool_change=0 returns action to IDLE", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 1}});
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 0}});

    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("QIDI Box parse_save_variables applies cached profile to SlotInfo temps",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Cache one profile, then mirror a slot pointing to it.
    QidiBoxTestAccess::apply_filas_list(backend, "[fila3]\nmin_temp=205\nmax_temp=225\n"
                                                 "box_min_temp=45\nbox_max_temp=65\n");
    QidiBoxTestAccess::parse_vars(backend, json{{"filament_slot0", 3}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 205);
    REQUIRE(info.units[0].slots[0].nozzle_temp_max == 225);
}

// =====================================================================
// Dryer capabilities (issue #1019)
// =====================================================================
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
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::handle_status(
        backend,
        json{{"heater_generic heater_box1", json{{"temperature", 48.0}, {"target", 55.0}}}});

    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.current_temp_c == Catch::Approx(48.0f).epsilon(0.01));
    REQUIRE(d.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
}

TEST_CASE("QIDI Box drying_state end_time drives remaining minutes", "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1000}; });
    QidiBoxTestAccess::apply_box_extras(
        backend,
        json{{"box_drying_state", json{{"box1", json{{"dry_state", 1}, {"end_time", 2800}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.active);
    REQUIRE(d.remaining_min == 30);
}

TEST_CASE("QIDI Box drying_state past end_time means not drying", "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{5000}; });
    QidiBoxTestAccess::apply_box_extras(
        backend,
        json{{"box_drying_state", json{{"box1", json{{"dry_state", 0}, {"end_time", 2800}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE_FALSE(d.active);
    REQUIRE(d.remaining_min == 0);
}

TEST_CASE("QIDI Box derives duration for externally-started drying (progress ring)",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1000}; });
    // 60 min remaining, started outside HelixScreen (no commanded duration).
    QidiBoxTestAccess::apply_box_extras(
        backend,
        json{{"box_drying_state", json{{"box1", json{{"dry_state", 1}, {"end_time", 4600}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.duration_min == 60);
    REQUIRE(d.get_progress_pct() == 0); // just started: 60/60 remaining
}

TEST_CASE("QIDI Box per-unit dryer: box1 drying, box2 idle -> distinct DryerInfo",
          "[ams][qidi_box][dryer][multi-unit]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Expand to 2 boxes.
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    // Deterministic clock so remaining_min is stable.
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1'000'000}; });
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);

    // Box1 heating to 55C @ 48C now; box2 idle heater.
    QidiBoxTestAccess::handle_status(
        backend,
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

TEST_CASE("QIDI Box config query refines max temp (box_config section)", "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_config_settings(
        backend, json{{"box_config box0", json{{"target_max_temp_heater_generic", 90.0}}}});
    REQUIRE(QidiBoxTestAccess::get_dryer(backend).max_temp_c == Catch::Approx(90.0f));
}

// =====================================================================
// Dryer write-path: start_drying / stop_drying (issue #1019)
// =====================================================================

TEST_CASE("QIDI Box start_drying uses ENABLE_BOX_DRY when timer supported",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);
    auto err = backend.start_drying(55.0f, 240);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "ENABLE_BOX_DRY BOX=1 TEMP=55 END_TIME=4");
}

TEST_CASE("QIDI Box start_drying falls back to SET_HEATER_TEMPERATURE",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, false);
    auto err = backend.start_drying(55.0f, 240);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=55");
}

TEST_CASE("QIDI Box start_drying rejects out-of-range temp", "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    auto err = backend.start_drying(150.0f, 240);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box stop_drying uses DISABLE_BOX_DRY when timer supported",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);
    auto err = backend.stop_drying(0);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "DISABLE_BOX_DRY BOX=1");
}

TEST_CASE("QIDI Box stop_drying falls back to TARGET=0", "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, false);
    auto err = backend.stop_drying(0);
    REQUIRE(err.success());
    REQUIRE(backend.sent[0] == "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=0");
}

// =====================================================================
// apply_filas_list: extended parse — filament name + type, colordict,
// vendor_list (stock QIDI officiall_filas_list.cfg, real excerpts)
// =====================================================================

// A trimmed-but-real excerpt of the stock file: a few fila sections plus
// the colordict and vendor_list tail. Mirrors the real ConfigParser
// alignment (key/value separated by a run of spaces around `=`).
static const char* STOCK_FILAS_EXCERPT = R"INI(
[fila1]
filament                       = PLA Rapido
min_temp                       = 190
max_temp                       = 240
box_min_temp                   = 0
box_max_temp                   = 0
type                           = PLA

[fila11]
filament                       = ABS Rapido
min_temp                       = 240
max_temp                       = 280
box_min_temp                   = 0
box_max_temp                   = 45
type                           = ABS

[fila42]
filament                       = PETG-CF
min_temp                       = 240
max_temp                       = 270
box_min_temp                   = 0
box_max_temp                   = 45
type                           = PETG-CF

[colordict]
1                              = #FAFAFA
2                              = #060606
18                             = #FF362D
24                             = #B87F2B

[vendor_list]
0                              = Generic
1                              = QIDI
2                              = eSUN
)INI";

TEST_CASE("QIDI Box apply_filas_list captures filament name and type", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    auto p1 = QidiBoxTestAccess::get_profile(backend, 1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->name == "PLA Rapido");
    REQUIRE(p1->type == "PLA");
    REQUIRE(p1->nozzle_min == 190);
    REQUIRE(p1->nozzle_max == 240);

    auto p11 = QidiBoxTestAccess::get_profile(backend, 11);
    REQUIRE(p11.has_value());
    REQUIRE(p11->name == "ABS Rapido");
    REQUIRE(p11->type == "ABS");
    REQUIRE(p11->box_max == 45);

    auto p42 = QidiBoxTestAccess::get_profile(backend, 42);
    REQUIRE(p42.has_value());
    REQUIRE(p42->name == "PETG-CF");
    REQUIRE(p42->type == "PETG-CF");
}

TEST_CASE("QIDI Box apply_filas_list parses colordict to packed RGB", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    REQUIRE(QidiBoxTestAccess::get_color(backend, 1) == 0xFAFAFA);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 2) == 0x060606);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 18) == 0xFF362D);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 24) == 0xB87F2B);
    REQUIRE_FALSE(QidiBoxTestAccess::get_color(backend, 99).has_value());
}

TEST_CASE("QIDI Box apply_filas_list parses vendor_list", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 0) == "Generic");
    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 1) == "QIDI");
    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 2) == "eSUN");
    REQUIRE_FALSE(QidiBoxTestAccess::get_vendor(backend, 99).has_value());
}

TEST_CASE("QIDI Box apply_filas_list colordict accepts bare hex (no #)", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, "[colordict]\n1 = FAFAFA\n2 = #060606\n");
    REQUIRE(QidiBoxTestAccess::get_color(backend, 1) == 0xFAFAFA);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 2) == 0x060606);
}

TEST_CASE("QIDI Box apply_filas_list ignores bad sections and atomically swaps",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // First load populates all three maps.
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);
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

TEST_CASE("QIDI Box apply_filas_list still parses temps (regression)", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);
    auto p11 = QidiBoxTestAccess::get_profile(backend, 11);
    REQUIRE(p11.has_value());
    REQUIRE(p11->nozzle_min == 240);
    REQUIRE(p11->nozzle_max == 280);
    REQUIRE(p11->box_min == 0);
    REQUIRE(p11->box_max == 45);
}

// =====================================================================
// Read-path resolution: slot_rfid_ ids → SlotInfo material/color/brand
// =====================================================================
// Once the filas list is cached, parse_save_variables must resolve the raw
// filament_slot/color_slot/vendor_slot indices onto the SlotInfo fields the
// UI reads (material, color_rgb, brand) — in addition to the nozzle temps it
// already applied.

TEST_CASE("QIDI Box read-path resolves material/color/brand from filas list", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    QidiBoxTestAccess::parse_vars(backend, json{
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
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    // Unknown filament id (not in the cfg) + unknown color/vendor ids.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 77},
                                               {"color_slot0", 99},
                                               {"vendor_slot0", 88},
                                           });

    auto info = backend.get_system_info();
    const auto& s = info.units[0].slots[0];
    // Material/brand untouched (empty defaults), color stays at default.
    REQUIRE(s.material.empty());
    REQUIRE(s.brand.empty());
    REQUIRE(s.color_rgb == AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE("QIDI Box read-path resolution survives before filas list loads", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // No filas list yet — ids captured but nothing resolved, no crash.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 1},
                                               {"color_slot0", 1},
                                               {"vendor_slot0", 1},
                                           });
    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].material.empty());
    REQUIRE(info.units[0].slots[0].color_rgb == AMS_DEFAULT_SLOT_COLOR);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 1);
}

// =====================================================================
// Reverse lookups (pure) for set_slot_info()
// =====================================================================

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

// =====================================================================
// set_slot_info: write reverse-mapped ids back to save_variables
// =====================================================================

TEST_CASE("QIDI Box set_slot_info emits SAVE_VARIABLE for all three ids",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    SlotInfo info;
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0xFF362D;

    auto err = backend.set_slot_info(0, info, /*persist=*/true);
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

TEST_CASE("QIDI Box set_slot_info skips fields with no mapping", "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    // Material that doesn't map to any fila; valid color + vendor.
    SlotInfo info;
    info.material = "NYLON-X";
    info.brand = "QIDI";
    info.color_rgb = 0xFAFAFA;

    auto err = backend.set_slot_info(1, info, /*persist=*/true);
    REQUIRE(err.success());
    // No filament_slot write (unmapped), but color + vendor present.
    for (const auto& g : backend.sent) {
        REQUIRE(g.find("filament_slot") == std::string::npos);
    }
    bool saw_color = false, saw_vendor = false;
    for (const auto& g : backend.sent) {
        if (g == "SAVE_VARIABLE VARIABLE=color_slot1 VALUE=1")
            saw_color = true;
        if (g == "SAVE_VARIABLE VARIABLE=vendor_slot1 VALUE=1")
            saw_vendor = true;
    }
    REQUIRE(saw_color);
    REQUIRE(saw_vendor);
}

TEST_CASE("QIDI Box set_slot_info rejects out-of-range slot index", "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    SlotInfo info;
    info.material = "PLA";
    REQUIRE_FALSE(backend.set_slot_info(-1, info, true).success());
    REQUIRE_FALSE(backend.set_slot_info(99, info, true).success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box set_slot_info accepts global slots from later boxes",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});
    QidiBoxTestAccess::apply_filas_list(backend, STOCK_FILAS_EXCERPT);

    SlotInfo info;
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0xFF362D;

    auto err = backend.set_slot_info(5, info, true);
    REQUIRE(err.success());

    bool saw_fila = false;
    for (const auto& g : backend.sent) {
        saw_fila = saw_fila || g == "SAVE_VARIABLE VARIABLE=filament_slot5 VALUE=11";
    }
    REQUIRE(saw_fila);
}

TEST_CASE("QIDI Box set_slot_info with no palette/vendor data still writes fila",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Only fila profiles loaded — no colordict / vendor_list.
    QidiBoxTestAccess::apply_filas_list(backend, "[fila1]\nfilament = PLA Rapido\ntype = PLA\n"
                                                 "min_temp = 190\nmax_temp = 240\n");
    SlotInfo info;
    info.material = "PLA";
    info.brand = "eSUN";
    info.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, info, true);
    REQUIRE(err.success());
    // Only the filament_slot write — empty palette/vendor skip cleanly.
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=1");
}

// ---------------------------------------------------------------------------
// Error-center bridge: current_error()
// ---------------------------------------------------------------------------

TEST_CASE("QIDI Box current_error returns nullopt when no slots are blocked",
          "[ams][qidi_box][error-center]") {
    RecordingQidiBackend backend;
    // Default state: all slots UNKNOWN
    REQUIRE_FALSE(backend.current_error().has_value());

    // Slots with non-BLOCKED statuses also return nullopt
    QidiBoxTestAccess::parse_vars(backend, json{
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
    RecordingQidiBackend backend;
    // slot1 blocked (value -3 = runout-during-print)
    QidiBoxTestAccess::parse_vars(backend, json{
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
    CHECK(ev->detail.find("2") != std::string::npos); // 1-based: slot index 1 → lane 2
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
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend,
                                  json{
                                      {"enable_box", 1}, {"box_count", 2}, {"slot5", -3}, // BLOCKED
                                  });

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->detail.find("6") != std::string::npos); // 1-based: global slot 5 → lane 6
}

TEST_CASE("QIDI Box current_error picks the first blocked slot when multiple blocked",
          "[ams][qidi_box][error-center]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{
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
    CHECK(ev->detail.find("1") != std::string::npos);
}

TEST_CASE("QIDI Box current_error clears when slot unblocks", "[ams][qidi_box][error-center]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"enable_box", 1},
                                               {"box_count", 1},
                                               {"slot0", -1}, // BLOCKED
                                               {"slot1", 1},
                                               {"slot2", 1},
                                               {"slot3", 1},
                                           });
    REQUIRE(backend.current_error().has_value());

    // Slot recovers (positive value = AVAILABLE)
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"enable_box", 1},
                                               {"box_count", 1},
                                               {"slot0", 1}, // was BLOCKED, now AVAILABLE
                                               {"slot1", 1},
                                               {"slot2", 1},
                                               {"slot3", 1},
                                           });
    REQUIRE_FALSE(backend.current_error().has_value());
}

// =====================================================================
// AmsAction::ERROR from BLOCKED lane (AmsErrorBridge hook)
// =====================================================================
// AmsErrorBridge fires when a backend's action transitions INTO ERROR and
// current_error() returns an event. QIDI's parser never set ERROR — it only
// ever wrote LOADING or IDLE — so the bridge was unreachable in production.
// Fix: any BLOCKED slot forces action = ERROR with higher precedence than
// is_tool_change (a mid-flight change on a broken lane is still ERROR).

TEST_CASE("QIDI Box blocked lane sets action to ERROR so AmsErrorBridge fires",
          "[ams][qidi_box][error-center]") {
    RecordingQidiBackend backend;
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);

    // Feed a negative slot (runout-during-print = -3). action must become ERROR.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"enable_box", 1},
                                               {"slot0", 1},
                                               {"slot1", -3},
                                               {"slot2", 1},
                                               {"slot3", 1},
                                           });
    REQUIRE(backend.get_system_info().action == AmsAction::ERROR);

    // Feed all-valid slots. action must leave ERROR.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"enable_box", 1},
                                               {"slot0", 1},
                                               {"slot1", 1},
                                               {"slot2", 1},
                                               {"slot3", 1},
                                           });
    REQUIRE(backend.get_system_info().action != AmsAction::ERROR);
}

// =====================================================================
// Homing guard (Layer 2): refuse toolhead-motion filament ops while a
// print is active. QIDI gates via refuse_if_printing() directly (it does
// NOT use check_preconditions()'s running_/busy checks). A mid-print
// load/unload moves the toolhead and can collide with the part.
// =====================================================================

namespace {

// RecordingQidiBackend variant that keeps a real (mock) api_ so
// refuse_if_printing() can read the live print-job state, while still capturing
// emitted gcode so "sent nothing" is verifiable.
class RecordingQidiWithApi : public AmsBackendQidi {
  public:
    RecordingQidiWithApi(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendQidi(api, client) {}
    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }
    std::vector<std::string> sent;
};

struct QidiHomingGuardFixture : public LVGLTestFixture {
    QidiHomingGuardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend = std::make_unique<RecordingQidiWithApi>(api.get(), &mock_client);
        // Force the synchronous execute_gcode() path (no CLEAR_NOZZLE wipe →
        // no ensure_homed_then() async query), so a STANDBY load emits gcode
        // synchronously and `sent` is deterministic. M603 keeps unload one-line.
        QidiBoxTestAccess::set_fw_caps(*backend, /*m603=*/true, /*clear_nozzle=*/false);
    }
    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }
    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<RecordingQidiWithApi> backend;
};

} // namespace

TEST_CASE_METHOD(QidiHomingGuardFixture,
                 "QIDI load/unload/change_tool refuse while PRINTING, proceed while PAUSED",
                 "[ams][qidi_box][homing_guard]") {
    auto check_refused = [](AmsError err, const std::vector<std::string>& sent,
                            const std::string& expected_msg) {
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
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
