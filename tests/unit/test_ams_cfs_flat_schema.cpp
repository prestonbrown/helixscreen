// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CFS flat-schema parse tests.
//
// Community Kalico ports of the K2 (Jacob10383/kalico + an unpublished
// klippy/extras/box.py) reimplement Creality's closed CFS module and publish a
// `box` status object with ZERO key overlap with the stock schema: a flat
// `slots[]` array instead of the stock `T1`..`T4` nested unit objects. The
// stock parser produced 0 units on that payload, so the AMS panel rendered
// empty on a fully populated 4-bay CFS (debug bundle QJKZEMTS, v0.99.106).
//
// The payload below is the real box object from that bundle, verbatim apart
// from formatting. Sections of it are trimmed only where a subtree is
// irrelevant to the parse under test; nothing is invented.

#include "../helix_test_fixture.h"
#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/registered_backend.h"
#include "ui/ams_drawing_utils.h"

#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

namespace {

/// The real flat `box` object from bundle QJKZEMTS (K2 Plus, Jacob10383/kalico
/// v2026.08.00, box_count=4, four loaded bays + one external spool entry).
json make_flat_box_json() {
    return json::parse(R"({
        "data_ready": true,
        "driver_ready": true,
        "filament_detected": false,
        "filament_sensor_error": null,
        "api_version": 1,
        "humidity_pct": 22,
        "loaded_mask": 0,
        "loaded_slot": -1,
        "rfid_insert_reading_enabled": true,
        "rfid_startup_reading_enabled": false,
        "runout": null,
        "runout_swap_enabled": true,
        "slot_filament_mask": 15,
        "state": "IDLE",
        "state_code": 0,
        "status": "OK",
        "status_code": 0,
        "temp_c": 29,
        "tracking_active": false,
        "materials": {
            "ABS":    {"target_temp": 245},
            "ASA":    {"target_temp": 245},
            "PC":     {"target_temp": 270},
            "PET-CF": {"target_temp": 290},
            "PETG":   {"target_temp": 245},
            "PLA":    {"target_temp": 220},
            "PPS":    {"target_temp": 300}
        },
        "load_path": {
            "box_addr": null,
            "buffer": {"active": true, "state_code": 2, "status_code": 0},
            "encoder": {"active": false, "position_mm": null},
            "loaded_mask": 0,
            "loaded_slot": -1,
            "printhead_sensor": {"detected": false, "error": null},
            "slot_filament_mask": 15,
            "source_slot": null,
            "tracking_active": false
        },
        "slots": [
            {"brand": "Xplorer",   "color": "#111111", "external": false, "index": 0,
             "loaded": false, "material": "PPS",    "name": "2026_PPS",
             "present": true, "rfid_percent": null, "rfid_reserve": "", "spoolman_id": null},
            {"brand": "Polymaker", "color": "#F2F2F2", "external": false, "index": 1,
             "loaded": false, "material": "PC",     "name": "2026_PC",
             "present": true, "rfid_percent": null, "rfid_reserve": "", "spoolman_id": null},
            {"brand": "Jayo",      "color": "#111111", "external": false, "index": 2,
             "loaded": false, "material": "PETG",   "name": "2026_PETG",
             "present": true, "rfid_percent": null, "rfid_reserve": "", "spoolman_id": null},
            {"brand": "None",      "color": "#111111", "external": false, "index": 3,
             "loaded": false, "material": "PET-CF", "name": "2026_PET-CF",
             "present": true, "rfid_percent": null, "rfid_reserve": "", "spoolman_id": null},
            {"brand": "",          "color": "",        "external": true,  "index": 4,
             "loaded": false, "material": "",       "name": "",
             "present": true, "rfid_percent": null, "rfid_reserve": "", "spoolman_id": null}
        ]
    })");
}

/// Minimal STOCK-schema box, trimmed from the existing CFS fixture. Present so
/// the schema discriminator and the stock regression can be asserted from this
/// file without reaching into test_ams_backend_cfs.cpp's helpers.
json make_stock_box_json() {
    return json::parse(R"({
        "state": "connect", "filament": 1, "auto_refill": 1, "enable": 1, "filament_useup": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect", "filament": "None", "temperature": "27",
            "dry_and_humidity": "48", "version": "1.1.3", "sn": "SERIAL", "mode": "0",
            "vender": ["Creality", "Creality", "Creality", "Creality"],
            "remain_len": ["35", "57", "-1", "-1"],
            "color_value": ["0000000", "0FFFFFF", "00A2989", "0C12E1F"],
            "material_type": ["101001", "101001", "101001", "101001"],
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
}

} // namespace

// --- Schema discrimination -------------------------------------------------
//
// Schema must be decided from the PAYLOAD, never from PrinterDetector. The
// affected printer is a K2 Plus by every model signal — model detection cannot
// see the firmware swap. See docs/devel/printer-research/CREALITY_CFS_K2_INTERNALS.md
// § "Box schema variants".

TEST_CASE("CFS schema detection", "[ams][cfs][flat]") {
    SECTION("flat payload with slots[] and no T1 detects Flat") {
        REQUIRE(AmsBackendCfs::detect_schema(make_flat_box_json()) == CfsSchema::Flat);
    }

    SECTION("stock payload with T1 detects Stock") {
        REQUIRE(AmsBackendCfs::detect_schema(make_stock_box_json()) == CfsSchema::Stock);
    }

    SECTION("empty object defaults to Stock") {
        // Default must be Stock: every shipped CFS today is stock, and an
        // ambiguous payload must not silently reroute them to a parser that
        // would report zero slots.
        REQUIRE(AmsBackendCfs::detect_schema(json::object()) == CfsSchema::Stock);
    }

    SECTION("T1 present alongside a slots key still detects Stock") {
        // Defensive: if a future firmware carried both, the stock parser is the
        // one with the richer decode, so it wins.
        json box = make_stock_box_json();
        box["slots"] = json::array();
        REQUIRE(AmsBackendCfs::detect_schema(box) == CfsSchema::Stock);
    }

    SECTION("non-array slots is not a flat payload") {
        json box = json::object();
        box["slots"] = "-1";
        REQUIRE(AmsBackendCfs::detect_schema(box) == CfsSchema::Stock);
    }
}

// --- Flat parse ------------------------------------------------------------

TEST_CASE("CFS flat schema: unit and slot topology", "[ams][cfs][flat]") {
    auto info = AmsBackendCfs::parse_box_status(make_flat_box_json());

    SECTION("system-level identity") {
        REQUIRE(info.type == AmsType::CFS);
        REQUIRE(info.type_name == "CFS");
        REQUIRE(info.tip_method == TipMethod::CUT);
    }

    SECTION("one unit built from the flat payload") {
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].connected == true);
        REQUIRE(info.units[0].topology == PathTopology::HUB);
        REQUIRE(info.units[0].first_slot_global_index == 0);
    }

    SECTION("external spool entry is excluded from the unit's bays") {
        // slots[4] carries external:true — it is the external spool holder,
        // not a CFS bay. Counting it would render a phantom 5th slot.
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.units[0].slots.size() == 4);
        REQUIRE(info.total_slots == 4);
    }

    SECTION("slot indices are contiguous and 0-based") {
        for (int i = 0; i < 4; ++i) {
            REQUIRE(info.units[0].slots[static_cast<size_t>(i)].slot_index == i);
            REQUIRE(info.units[0].slots[static_cast<size_t>(i)].global_index == i);
        }
    }
}

TEST_CASE("CFS flat schema: loaded_slot vs the external entry", "[ams][cfs][flat]") {
    // loaded_slot indexes the payload's slots[] — which includes the external
    // entry that the unit's bay vector does not. The bay and external cases
    // must land in different places: bay N maps straight through, external
    // maps to the -2 bypass sentinel (box.py's own convention: the snapshot
    // builder assigns loaded = external_slot when the toolhead sensor detects
    // filament no box lane owns).
    json box = make_flat_box_json();

    SECTION("bay index maps straight through") {
        box["loaded_slot"] = 2;
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.current_slot == 2);
        REQUIRE(info.current_tool == 2);
    }

    SECTION("external index maps to the -2 bypass sentinel") {
        box["loaded_slot"] = 4;
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.current_slot == -2);
        REQUIRE(info.current_tool == -2);
    }

    SECTION("nothing loaded stays -1") {
        box["loaded_slot"] = -1;
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.current_slot == -1);
    }

    SECTION("out-of-range index that is not the external entry stays -1") {
        box["loaded_slot"] = 99;
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.current_slot == -1);
    }

    SECTION("external entry moved with box_count is still recognized") {
        // external_slot = max_physical_slot + 1 in box.py, so a 2-unit box
        // (8 bays, indices 0..7) puts the holder at index 8 — read from the
        // payload, never recomputed from this machine's bay count.
        json two_units = make_flat_box_json();
        json& slots = two_units["slots"];
        for (int i = 4; i < 8; ++i) {
            slots.insert(slots.begin() + i, json{{"brand", ""},
                                                 {"color", ""},
                                                 {"external", false},
                                                 {"index", i},
                                                 {"loaded", false},
                                                 {"material", ""},
                                                 {"name", ""},
                                                 {"present", false},
                                                 {"rfid_percent", nullptr},
                                                 {"rfid_reserve", ""},
                                                 {"spoolman_id", nullptr}});
        }
        slots[8]["index"] = 8;
        slots[8]["external"] = true;
        two_units["loaded_slot"] = 8;
        auto info = AmsBackendCfs::parse_box_status(two_units);
        REQUIRE(info.units.size() == 2);
        REQUIRE(info.total_slots == 8);
        REQUIRE(info.current_slot == -2);
    }
}

TEST_CASE("CFS flat schema: per-slot filament data", "[ams][cfs][flat]") {
    auto info = AmsBackendCfs::parse_box_status(make_flat_box_json());
    REQUIRE(info.units.size() == 1);
    const auto& slots = info.units[0].slots;

    SECTION("materials read straight through — no code table") {
        REQUIRE(slots[0].material == "PPS");
        REQUIRE(slots[1].material == "PC");
        REQUIRE(slots[2].material == "PETG");
        REQUIRE(slots[3].material == "PET-CF");
    }

    SECTION("colors parsed from #RRGGBB") {
        // Stock CFS uses a leading-zero "0RRGGBB" form; the flat schema uses
        // conventional "#RRGGBB". parse_color must not be reused blindly.
        REQUIRE(slots[0].color_rgb == 0x111111);
        REQUIRE(slots[1].color_rgb == 0xF2F2F2);
    }

    SECTION("brands read through, 'None' sentinel becomes empty") {
        REQUIRE(slots[0].brand == "Xplorer");
        REQUIRE(slots[1].brand == "Polymaker");
        REQUIRE(slots[2].brand == "Jayo");
        REQUIRE(slots[3].brand.empty());
    }

    SECTION("spool name carried through") {
        REQUIRE(slots[0].spool_name == "2026_PPS");
        REQUIRE(slots[3].spool_name == "2026_PET-CF");
    }

    SECTION("present + not loaded is AVAILABLE") {
        for (const auto& s : slots) {
            REQUIRE(s.status == SlotStatus::AVAILABLE);
        }
    }

    SECTION("nozzle temps resolved from the box's own materials table") {
        // The fork publishes a per-material target_temp map. Stock CFS has no
        // equivalent, so this is strictly better data than the stock path gets.
        REQUIRE(slots[0].nozzle_temp_max == 300); // PPS
        REQUIRE(slots[1].nozzle_temp_max == 270); // PC
        REQUIRE(slots[2].nozzle_temp_max == 245); // PETG
        REQUIRE(slots[3].nozzle_temp_max == 290); // PET-CF
    }
}

// The module stringifies a null profile field via Python's str(), so ANY text
// field can arrive as the literal "None" — not just brand. Verified against
// box.py `_slot_status`, which builds every entry with
// `str(value.get(key, "")).strip()`.
TEST_CASE("CFS flat schema: 'None' is absent on every text field", "[ams][cfs][flat]") {
    json box = make_flat_box_json();
    box["slots"][0]["material"] = "None";
    box["slots"][0]["name"] = "None";
    box["slots"][0]["brand"] = "None";
    box["slots"][0]["color"] = "None";

    auto info = AmsBackendCfs::parse_box_status(box);
    const auto& s = info.units[0].slots[0];

    SECTION("no field surfaces the literal string") {
        REQUIRE(s.material.empty());
        REQUIRE(s.spool_name.empty());
        REQUIRE(s.brand.empty());
    }

    SECTION("color falls back rather than parsing the sentinel") {
        REQUIRE(s.color_rgb == kAmsDefaultSlotColor);
    }

    SECTION("an empty material resolves no temperature") {
        REQUIRE(s.nozzle_temp_max == 0);
    }
}

TEST_CASE("CFS flat schema: slot status transitions", "[ams][cfs][flat]") {
    SECTION("loaded slot reports LOADED and sets current_slot") {
        json box = make_flat_box_json();
        box["slots"][2]["loaded"] = true;
        box["loaded_slot"] = 2;
        box["loaded_mask"] = 4;

        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
        REQUIRE(info.current_slot == 2);
    }

    SECTION("absent filament reports EMPTY") {
        json box = make_flat_box_json();
        box["slots"][1]["present"] = false;
        box["slot_filament_mask"] = 13;

        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].slots[1].status == SlotStatus::EMPTY);
        REQUIRE(info.units[0].slots[1].is_present() == false);
    }

    SECTION("loaded_slot -1 leaves current_slot unset") {
        auto info = AmsBackendCfs::parse_box_status(make_flat_box_json());
        REQUIRE(info.current_slot == -1);
    }
}

// --- Runout signal ---------------------------------------------------------
//
// filament_detected is the box gate's own presence boolean, the flat analogue
// of stock's filament_useup. `runout` is NOT that signal: it is the
// runout-SWAP PLAN — an ordered `chain` of fallback slots and the loaded_slot
// they back — which the module publishes whenever runout_swap_enabled is on
// and a lane is loaded, a healthy printing box included. Reading the plan's
// presence as an event fires a runout on the load-completion frame of a wholly
// successful load, and the episode that arms drops the loaded lane's
// remembered Spoolman link the moment the bay next reads empty.

TEST_CASE("CFS flat schema: runout comes from the gate, not the swap plan", "[ams][cfs][flat]") {
    SECTION("a healthy printing box publishing a swap plan is not a runout") {
        // The live shape from a K2 Plus running the fork: slot 0 loaded and
        // feeding, every bay occupied, gate fed, chain naming slot 1 as backup.
        json box = make_flat_box_json();
        box["filament_detected"] = true;
        box["loaded_slot"] = 0;
        box["loaded_mask"] = 1;
        box["slots"][0]["loaded"] = true;
        box["runout"] = json{{"chain", json::array({1})}, {"loaded_slot", 0}};
        box["state"] = "PRINT";

        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.filament_runout == false);
        REQUIRE(info.units[0].slots[0].status == SlotStatus::LOADED);
    }

    SECTION("an empty gate is a runout even with no swap plan published") {
        json box = make_flat_box_json();
        box["filament_detected"] = false;
        box["runout"] = nullptr;
        REQUIRE(AmsBackendCfs::parse_box_status(box).filament_runout == true);
    }

    SECTION("a null gate reading is not a runout") {
        // Klipper publishes a sensor field as null until its first read — the
        // sibling filament_sensor_error ships that way on a healthy box. A
        // gate that has published no reading has not reported a runout.
        json box = make_flat_box_json();
        box["filament_detected"] = nullptr;
        box["runout"] = json{{"chain", json::array({1})}, {"loaded_slot", 0}};
        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
        REQUIRE(AmsBackendCfs::parse_box_status(box).filament_runout == false);
    }

    SECTION("an absent gate reading is not a runout") {
        json box = make_flat_box_json();
        box.erase("filament_detected");
        box["runout"] = json{{"chain", json::array({1})}, {"loaded_slot", 0}};
        REQUIRE(AmsBackendCfs::parse_box_status(box).filament_runout == false);
    }
}

TEST_CASE("CFS flat schema: unit environment and path sensors", "[ams][cfs][flat]") {
    auto info = AmsBackendCfs::parse_box_status(make_flat_box_json());
    REQUIRE(info.units.size() == 1);
    const auto& unit = info.units[0];

    SECTION("temp_c and humidity_pct populate EnvironmentData") {
        REQUIRE(unit.environment.has_value());
        REQUIRE(unit.environment->temperature_c == 29.0f);
        REQUIRE(unit.environment->humidity_pct == 22.0f);
        REQUIRE(unit.environment->has_humidity == true);
    }

    SECTION("load_path sensors map onto unit capability flags") {
        // The fork exposes an encoder and a printhead sensor that stock CFS
        // does not report at all.
        REQUIRE(unit.has_encoder == true);
        REQUIRE(unit.has_toolhead_sensor == true);
    }

    SECTION("buffer subtree populates BufferHealth") {
        REQUIRE(unit.buffer_health.has_value());
    }
}

// --- Robustness ------------------------------------------------------------
//
// Same discipline as the stock parser: a null or wrong-typed field must
// degrade to a sentinel, never throw out of parse_box_status and drop the
// whole frame (see the safe_string/safe_int rationale in ams_backend_cfs.cpp).

TEST_CASE("CFS flat schema: malformed payloads degrade, never throw", "[ams][cfs][flat]") {
    SECTION("empty slots array yields a unit with no bays, not a crash") {
        json box = make_flat_box_json();
        box["slots"] = json::array();
        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.total_slots == 0);
    }

    SECTION("null scalars fall back to defaults") {
        json box = make_flat_box_json();
        box["temp_c"] = nullptr;
        box["humidity_pct"] = nullptr;
        box["loaded_slot"] = nullptr;
        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
    }

    SECTION("wrong-typed per-slot fields fall back to defaults") {
        json box = make_flat_box_json();
        box["slots"][0]["material"] = 42;
        box["slots"][0]["color"] = nullptr;
        box["slots"][0]["brand"] = json::array();
        box["slots"][0]["present"] = "yes";

        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].slots[0].material.empty());
    }

    SECTION("a non-object slot entry is skipped, later slots still parse") {
        json box = make_flat_box_json();
        box["slots"][1] = "-1";
        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].slots.size() >= 1);
        REQUIRE(info.units[0].slots.back().material == "PET-CF");
    }

    SECTION("missing load_path subtree leaves sensors off") {
        json box = make_flat_box_json();
        box.erase("load_path");
        REQUIRE_NOTHROW(AmsBackendCfs::parse_box_status(box));
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].has_encoder == false);
        REQUIRE(info.units[0].buffer_health.has_value() == false);
    }

    SECTION("malformed color string falls back to the default slot color") {
        json box = make_flat_box_json();
        box["slots"][0]["color"] = "not-a-color";
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units[0].slots[0].color_rgb == kAmsDefaultSlotColor);
    }
}

// --- Fork macro dialect ----------------------------------------------------
//
// Signatures taken from the module itself (box.py `_register_commands` /
// `cmd_load` / `cmd_unload` / `_register_t_commands`), not inferred:
//
//   BOX_LOAD    SLOT=<n>            gcmd.get_int("SLOT", 0, minval=0, maxval=15)
//   BOX_UNLOAD  [MANUAL=0|1]        explicitly REJECTS SLOT
//   T<n>        [FLUSH=0|1]         tool change; default FLUSH=1
//
// There is NO BOX_CHANGE. The module owns the whole feed/purge/park sequence,
// so none of these carry the stock envelope.

TEST_CASE("CFS Fork dialect: gcode builders", "[ams][cfs][flat][fork]") {
    SECTION("load emits the Box-owned T command") {
        REQUIRE(AmsBackendCfs::load_gcode(0, CfsMacroVariant::Fork) == "T0");
        REQUIRE(AmsBackendCfs::load_gcode(7, CfsMacroVariant::Fork) == "T7");
        REQUIRE(AmsBackendCfs::load_gcode(15, CfsMacroVariant::Fork) == "T15");
    }

    SECTION("unload emits a bare BOX_UNLOAD — never with SLOT") {
        const std::string g = AmsBackendCfs::unload_gcode(CfsMacroVariant::Fork);
        REQUIRE(g == "BOX_UNLOAD");
        // box.py raises "BOX_UNLOAD no longer accepts SLOT"; sending one is a
        // hard command error, not a warning.
        REQUIRE(g.find("SLOT") == std::string::npos);
    }

    SECTION("swap emits the T<n> toolchange, not BOX_CHANGE") {
        REQUIRE(AmsBackendCfs::swap_gcode(0, CfsMacroVariant::Fork) == "T0");
        REQUIRE(AmsBackendCfs::swap_gcode(3, CfsMacroVariant::Fork) == "T3");
        REQUIRE(AmsBackendCfs::swap_gcode(3, CfsMacroVariant::Fork).find("BOX_CHANGE") ==
                std::string::npos);
    }

    SECTION("no stock envelope leaks into Fork output") {
        // The module parks, purges and cleans internally. Emitting the stock
        // envelope would send commands this firmware does not define.
        for (const std::string g : {AmsBackendCfs::load_gcode(1, CfsMacroVariant::Fork),
                                    AmsBackendCfs::unload_gcode(CfsMacroVariant::Fork),
                                    AmsBackendCfs::swap_gcode(1, CfsMacroVariant::Fork)}) {
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            REQUIRE(g.find("BOX_SAVE_FAN") == std::string::npos);
            REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
            REQUIRE(g.find("SAVE_GCODE_STATE") == std::string::npos);
            REQUIRE(g.find("TNN=") == std::string::npos);
            REQUIRE(g.find('\n') == std::string::npos); // single command, no script
        }
    }

    SECTION("out-of-range slot yields no command") {
        REQUIRE(AmsBackendCfs::load_gcode(-1, CfsMacroVariant::Fork).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16, CfsMacroVariant::Fork).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(-1, CfsMacroVariant::Fork).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16, CfsMacroVariant::Fork).empty());
    }

    SECTION("stock dialects are untouched by the Fork addition") {
        REQUIRE(AmsBackendCfs::load_gcode(1, CfsMacroVariant::K2).find("CR_BOX_EXTRUDE") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1, CfsMacroVariant::K1).find("BOX_EXTRUDE_MATERIAL") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode(CfsMacroVariant::K2).find("CR_BOX_RETRUDE") !=
                std::string::npos);
    }
}

// --- Fork dialect detection ------------------------------------------------

TEST_CASE("CFS Fork dialect: detected from the supported Box API version",
          "[ams][cfs][flat][fork]") {
    // `api_version` explicitly identifies box.py's command dialect; `slots[]`
    // alone only identifies which status parser to use:
    // the Fork commands are registered in Python, so they never appear in
    // printer.objects.list and has_macro("BOX_LOAD") can never see them.
    SECTION("payload carrying the supported version is the fork") {
        REQUIRE(AmsBackendCfs::detect_fork_dialect(make_flat_box_json()) == true);
    }

    SECTION("stock payload is not") {
        REQUIRE(AmsBackendCfs::detect_fork_dialect(make_stock_box_json()) == false);
    }

    SECTION("a flat payload without api_version is not assumed to be the fork") {
        // Schema and dialect are separate axes. Another flat-schema firmware
        // would parse fine but must not inherit this one's command set.
        json box = make_flat_box_json();
        box.erase("api_version");
        REQUIRE(AmsBackendCfs::detect_schema(box) == CfsSchema::Flat);
        REQUIRE(AmsBackendCfs::detect_fork_dialect(box) == false);
    }

    SECTION("an unsupported API version is not assumed compatible") {
        json box = make_flat_box_json();
        box["api_version"] = 2;
        REQUIRE(AmsBackendCfs::detect_fork_dialect(box) == false);
    }
}

// --- Fork slot metadata write ----------------------------------------------

TEST_CASE("CFS Fork dialect: slot metadata write", "[ams][cfs][flat][fork]") {
    // _BOX_SLOT_SET SLOT=<n> MATERIAL="<str>" COLOR="#RRGGBB" BRAND="..." NAME="..."
    // All three of SLOT/MATERIAL/COLOR are REQUIRED — box.py raises on any
    // missing one, so a color-only write (the stock BOX_MODIFY_TN_DATA shape)
    // is not expressible and must carry the material along.
    SECTION("emits the full Box profile with quoted values") {
        const std::string g =
            AmsBackendCfs::slot_set_gcode(2, "PETG", 0x0A2989, "eSUN", "Ocean Blue", 42);
        REQUIRE(g == "_BOX_SLOT_SET SLOT=2 MATERIAL=\"PETG\" COLOR=\"#0A2989\" BRAND=\"eSUN\" "
                     "NAME=\"Ocean Blue\" SPOOLMAN_ID=42");
    }

    SECTION("clears optional Box profile fields") {
        REQUIRE(AmsBackendCfs::slot_set_gcode(0, "PLA", 0x000000, "", "", 0) ==
                "_BOX_SLOT_SET SLOT=0 MATERIAL=\"PLA\" COLOR=\"#000000\" BRAND=\"\" NAME=\"\" "
                "SPOOLMAN_ID=-1");
    }

    SECTION("escapes quoted profile fields") {
        REQUIRE(AmsBackendCfs::slot_set_gcode(0, "petg", 0xFFFFFF, "A\\B", "Bob \"Blue\"", 7) ==
                "_BOX_SLOT_SET SLOT=0 MATERIAL=\"PETG\" COLOR=\"#FFFFFF\" BRAND=\"A\\\\B\" "
                "NAME=\"Bob \\\"Blue\\\"\" SPOOLMAN_ID=7");
    }

    SECTION("quotes free-text material names") {
        REQUIRE(AmsBackendCfs::slot_set_gcode(0, "PLA Matte", 0xFFFFFF, "", "", 0) ==
                "_BOX_SLOT_SET SLOT=0 MATERIAL=\"PLA MATTE\" COLOR=\"#FFFFFF\" BRAND=\"\" "
                "NAME=\"\" SPOOLMAN_ID=-1");
    }

    SECTION("no command without a material — the module would reject it") {
        REQUIRE(AmsBackendCfs::slot_set_gcode(0, "", 0xFFFFFF, "", "", 0).empty());
    }

    SECTION("out-of-range slot yields no command") {
        REQUIRE(AmsBackendCfs::slot_set_gcode(-1, "PLA", 0xFFFFFF, "", "", 0).empty());
        REQUIRE(AmsBackendCfs::slot_set_gcode(16, "PLA", 0xFFFFFF, "", "", 0).empty());
    }
}

// --- Stock regression ------------------------------------------------------
//
// The two-axis split must not perturb the stock path. This is the guard that
// the schema dispatch is additive.

TEST_CASE("CFS stock schema still parses after the flat split", "[ams][cfs][flat]") {
    auto info = AmsBackendCfs::parse_box_status(make_stock_box_json());

    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "T1");
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == 27.0f);
    REQUIRE(info.units[0].environment->humidity_pct == 48.0f);
    REQUIRE(info.units[0].slots[0].color_rgb == 0x000000);
    REQUIRE(info.units[0].slots[1].color_rgb == 0xFFFFFF);
    // The fork spells the ENABLE bit runout_swap_enabled.
    REQUIRE(info.endless_spool_enabled == true);
}

// ============================================================================
// Real four-box payload (debug bundle L8MMBCCK, K2 Plus, box_count 4)
// ============================================================================

namespace {

json load_box_fixture(const std::string& name) {
    const std::string src = __FILE__;
    const auto pos = src.rfind("tests/unit/");
    const std::string dir =
        (pos == std::string::npos ? std::string() : src.substr(0, pos)) + "tests/fixtures/";
    std::ifstream f(dir + name);
    INFO("fixture missing or unreadable: " << dir + name);
    REQUIRE(f.is_open());
    json j;
    f >> j;
    return j;
}

} // namespace

// Four boxes on the bus with spools only in box 1. A box is absent only when
// none of its slot indices are listed; an empty box is still a box. The payload
// carries no per-slot box address (load_path.box_addr is null), so nothing may
// depend on one.
TEST_CASE("CFS flat: a real four-box payload parses as four boxes, three of them empty",
          "[ams][cfs][flat][fork][1464]") {
    const auto info =
        AmsBackendCfs::parse_box_status(load_box_fixture("cfs_fork_four_box_L8MMBCCK.json"));

    REQUIRE(info.units.size() == 4);
    CHECK(info.total_slots == 16);
    CHECK(info.present_slot_count() == 16);
    for (int u = 0; u < 4; ++u) {
        INFO("unit " << u);
        CHECK_FALSE(info.units[u].absent);
        CHECK(info.units[u].unit_index == u);
        CHECK(info.units[u].first_slot_global_index == u * 4);
        CHECK(info.units[u].slot_count == 4);
    }
    for (int bay = 0; bay < 16; ++bay) {
        INFO("bay " << bay);
        REQUIRE(info.get_slot_global(bay) != nullptr);
        CHECK(info.slot_exists(bay));
        CHECK(info.get_slot_global(bay)->status ==
              (bay < 4 ? SlotStatus::AVAILABLE : SlotStatus::EMPTY));
        CHECK(info.get_slot_global(bay)->mapped_tool == bay);
    }
    CHECK(info.get_slot_global(0)->material == "PLA");
    CHECK(info.get_slot_global(0)->brand == "Sunlu");
    // The external holder at 16 is not a bay.
    CHECK(info.get_slot_global(16) == nullptr);
    CHECK(info.current_slot == -1);
}

// The payload's one temp/humidity reading belongs to the box load_path.box_addr
// names (1-based), not to whichever box parses first.
TEST_CASE("CFS flat: the environment reading lands on the box load_path.box_addr names",
          "[ams][cfs][flat][fork][environment]") {
    auto box = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");

    const auto units_with_environment = [](const AmsSystemInfo& info) {
        std::vector<int> out;
        for (const auto& u : info.units) {
            if (u.environment.has_value()) {
                out.push_back(u.unit_index);
            }
        }
        return out;
    };

    SECTION("box_addr 4 puts it on the fourth unit only") {
        box["load_path"]["box_addr"] = 4;
        const auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(units_with_environment(info) == std::vector<int>{3});
        CHECK(info.units[3].environment->temperature_c == 27.0f);
        CHECK(info.units[3].environment->humidity_pct == 54.0f);
    }

    SECTION("box_addr 2 puts it on the second unit only") {
        box["load_path"]["box_addr"] = 2;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{1});
    }

    SECTION("a null box_addr keeps it on the first unit") {
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0});
    }

    SECTION("a box_addr naming no listed box keeps it on the first unit") {
        box["load_path"]["box_addr"] = 9;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0});
        box["load_path"]["box_addr"] = 0;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0});
    }
}

TEST_CASE("CFS flat: four boxes converge on one toolhead in the system path layout",
          "[ams][cfs][flat][tool_layout][ams_draw]") {
    const auto info =
        AmsBackendCfs::parse_box_status(load_box_fixture("cfs_fork_four_box_L8MMBCCK.json"));
    const auto layout = ams_draw::compute_system_tool_layout(info, nullptr);

    CHECK(layout.total_physical_tools == 1);
    REQUIRE(layout.units.size() == 4);
    for (int u = 0; u < 4; ++u) {
        INFO("unit " << u);
        CHECK(layout.units[u].tool_count == 1);
        CHECK(layout.units[u].first_physical_tool == 0);
    }
}

// ============================================================================
// Golden sequence: stock frames merged as deltas (a unit joining, the active
// lane letter, the toolhead switch, a runout latch, a bay insert, a unit going
// off the bus), the extruder and motor objects, then a flat fork frame. Each
// frame is pinned as a text snapshot of everything the parse leaves behind plus
// the gcode it dispatches after releasing the lock.
// ============================================================================

namespace {

class CapturingCfs : public AmsBackendCfs {
  public:
    CapturingCfs() : AmsBackendCfs(nullptr, nullptr) {}

    std::vector<std::string> captured;

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
};

struct CfsGoldenFixture : public HelixTestFixture {
    CfsGoldenFixture() {
        backend.set_event_callback(
            [this](const std::string& name, const std::string&) { events.push_back(name); });
    }

    ~CfsGoldenFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string feed(const json& params) {
        events.clear();
        backend.captured.clear();
        json notification;
        notification["params"] = json::array({params, 0.0});
        CfsTestAccess::handle_status(backend, notification);
        helix::ui::UpdateQueue::instance().drain();
        return snapshot();
    }

    std::string snapshot() {
        const AmsSystemInfo info = backend.get_system_info();
        std::string out = fmt::format(
            "slot={} tool={} loaded={} runout={} action={} bypass={} declared={} endless={} "
            "total={} variant={}\n",
            info.current_slot, info.current_tool, info.filament_loaded, info.filament_runout,
            ams_action_to_string(info.action), info.supports_bypass,
            CfsTestAccess::bypass_declared(backend), info.endless_spool_enabled, info.total_slots,
            static_cast<int>(CfsTestAccess::macro_variant(backend)));
        std::string ttg;
        for (const int v : info.tool_to_slot_map) {
            ttg += (ttg.empty() ? "" : ",") + std::to_string(v);
        }
        out += fmt::format("  ttg={} units={}\n", ttg, info.units.size());
        for (const auto& unit : info.units) {
            out += fmt::format("  unit{} absent={} first={}:", unit.unit_index, unit.absent,
                               unit.first_slot_global_index);
            for (const auto& s : unit.slots) {
                out += fmt::format(" [{} {}/{}/{} #{:06X}]", slot_status_to_string(s.status),
                                   s.material, s.brand, s.spool_name, s.color_rgb);
            }
            out += "\n";
        }
        std::string gcodes;
        for (const auto& g : backend.captured) {
            gcodes += (gcodes.empty() ? "" : "|") + g;
        }
        out += fmt::format("  events={} gcodes='{}'", events.size(), gcodes);
        return out;
    }

    helix::test::RegisteredBackend<CapturingCfs> backend_reg{};
    CapturingCfs& backend = *backend_reg;
    std::vector<std::string> events;
};

json golden_unit(const std::string& letter, std::vector<std::string> vendors) {
    return json{{"state", "connect"},
                {"filament", letter},
                {"version", "1.1.3"},
                {"sn", "GOLDEN"},
                {"vender", vendors},
                {"remain_len", json::array({"35", "57", "-1", "-1"})},
                {"color_value", json::array({"0FF0000", "000FF00", "0000000", "0000000"})},
                {"material_type", json::array({"101001", "101001", "-1", "-1"})},
                {"change_color_num", json::array({"-1", "-1", "-1", "-1"})}};
}

} // namespace

namespace {
const std::string kCfsGolden = R"GOLD(stock full frame
slot=-1 tool=-1 loaded=false runout=false action=Idle bypass=true declared=false endless=true total=4 variant=0
  ttg=0,1,2,3 units=1
  unit0 absent=false first=0: [Available PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
stock delta: unit 2 joins
slot=-1 tool=-1 loaded=false runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Available PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
stock delta: lane A selected
slot=0 tool=0 loaded=false runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Available PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
toolhead switch on
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
null toolhead reading
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
extruder heating
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=0 gcodes=''
motor ready
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
runout latch
slot=0 tool=0 loaded=true runout=true action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Empty // #808080] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
bay 3 filled
slot=0 tool=0 loaded=true runout=true action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Available /Creality/ #000000] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
idle after runout
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=8 variant=0
  ttg=0,1,2,3,4,5,6,7 units=2
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Available /Creality/ #000000] [Empty // #808080]
  unit1 absent=false first=4: [Available PLA/Creality/ #FF0000] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes='BOX_INFO_REFRESH ADDR=1 NUM=4'
unit 2 leaves the bus
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=4 variant=0
  ttg=0,1,2,3 units=1
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Available /Creality/ #000000] [Empty // #808080]
  events=1 gcodes=''
measuring wheel noise
slot=0 tool=0 loaded=true runout=false action=Idle bypass=true declared=false endless=true total=4 variant=0
  ttg=0,1,2,3 units=1
  unit0 absent=false first=0: [Loaded PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Available /Creality/ #000000] [Empty // #808080]
  events=1 gcodes=''
toolhead switch off
slot=0 tool=0 loaded=false runout=false action=Idle bypass=true declared=false endless=true total=4 variant=0
  ttg=0,1,2,3 units=1
  unit0 absent=false first=0: [Available PLA/Creality/ #FF0000] [Available PLA/Creality/ #00FF00] [Available /Creality/ #000000] [Empty // #808080]
  events=1 gcodes=''
flat fork frame
slot=-1 tool=-1 loaded=false runout=true action=Idle bypass=true declared=false endless=true total=16 variant=2
  ttg=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 units=4
  unit0 absent=false first=0: [Available PLA/Sunlu/ #D75BA7] [Available PLA/Slic3D/ #111111] [Available PLA/Elegoo/ #F2F2F2] [Available PLA/Elegoo/Grey #6F7379]
  unit1 absent=false first=4: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  unit2 absent=false first=8: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  unit3 absent=false first=12: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
flat delta: toolhead on
slot=-1 tool=-1 loaded=true runout=true action=Idle bypass=true declared=false endless=true total=16 variant=2
  ttg=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 units=4
  unit0 absent=false first=0: [Available PLA/Sunlu/ #D75BA7] [Available PLA/Slic3D/ #111111] [Available PLA/Elegoo/ #F2F2F2] [Available PLA/Elegoo/Grey #6F7379]
  unit1 absent=false first=4: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  unit2 absent=false first=8: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  unit3 absent=false first=12: [Empty // #808080] [Empty // #808080] [Empty // #808080] [Empty // #808080]
  events=1 gcodes=''
)GOLD";
} // namespace

TEST_CASE_METHOD(CfsGoldenFixture, "CFS golden status sequence", "[ams][cfs][golden]") {
    const std::vector<std::pair<std::string, json>> frames = {
        {"stock full frame",
         json{
             {"box", json{{"state", "connect"},
                          {"filament", 1},
                          {"auto_refill", 1},
                          {"enable", 1},
                          {"filament_useup", 0},
                          {"map", json{{"T1A", "T1A"}, {"T1B", "T1B"}}},
                          {"T1", golden_unit("None", {"Creality", "Creality", "none", "none"})}}}}},
        {"stock delta: unit 2 joins",
         json{{"box", json{{"T2", golden_unit("None", {"Creality", "none", "none", "none"})}}}}},
        {"stock delta: lane A selected", json{{"box", json{{"T1", json{{"filament", "A"}}}}}}},
        {"toolhead switch on",
         json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", true}}}}},
        {"null toolhead reading",
         json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", nullptr}}}}},
        {"extruder heating", json{{"extruder", json{{"target", 220.0}, {"temperature", 215.5}}}}},
        {"motor ready", json{{"motor_control", json{{"motor_ready", true}}}}},
        {"runout latch", json{{"box", json{{"filament", 1}, {"filament_useup", 1}}}}},
        {"bay 3 filled",
         json{{"box", json{{"T1", json{{"vender", json::array({"Creality", "Creality", "Creality",
                                                               "none"})}}}}}}},
        {"idle after runout", json{{"box", json{{"filament", 1}, {"filament_useup", 0}}}}},
        {"unit 2 leaves the bus", json{{"box", json{{"T2", json{{"state", "None"}}}}}}},
        {"measuring wheel noise", json{{"box", json{{"measuring_wheel", 3}}}}},
        {"toolhead switch off",
         json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", false}}}}},
        {"flat fork frame", json{{"box", load_box_fixture("cfs_fork_four_box_L8MMBCCK.json")}}},
        {"flat delta: toolhead on",
         json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", true}}}}},
    };

    std::string all;
    for (const auto& [name, params] : frames) {
        all += name + "\n" + feed(params) + "\n";
    }
    CHECK(all == kCfsGolden);
}

// ============================================================================
// Flat frames are deltas. Klipper sends a status object's changed top-level
// fields only, each one whole, so a frame that edits a slot carries `slots`
// and nothing else. The backend reads the box as the sum of every frame
// since the session began (Jacob10383/k2-plus-custom-firmware#3).
// ============================================================================

namespace {

/// The frame Klipper sends when a status object goes from @p before to
/// @p after: every top-level field whose value changed, carried whole.
json klipper_delta(const json& before, const json& after) {
    json out = json::object();
    for (const auto& [key, value] : after.items()) {
        if (!before.contains(key) || before[key] != value) {
            out[key] = value;
        }
    }
    return out;
}

json box_frame(const json& box) {
    return json{{"box", box}};
}

std::vector<int> units_with_environment(const AmsSystemInfo& info) {
    std::vector<int> out;
    for (const auto& u : info.units) {
        if (u.environment.has_value()) {
            out.push_back(u.unit_index);
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a slots-only delta keeps the environment reading",
                 "[ams][cfs][flat][environment]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    feed(box_frame(full));
    REQUIRE(units_with_environment(backend.get_system_info()) == std::vector<int>{0});

    // The user sets spool info on bay 2: box.py republishes slots[] and
    // nothing else changed.
    json edited = full;
    edited["slots"][1]["material"] = "PETG";
    edited["slots"][1]["brand"] = "Jayo";
    const json delta = klipper_delta(full, edited);
    REQUIRE(delta.size() == 1);
    REQUIRE(delta.contains("slots"));
    feed(box_frame(delta));

    const auto info = backend.get_system_info();
    REQUIRE(units_with_environment(info) == std::vector<int>{0});
    CHECK(info.units[0].environment->temperature_c == 27.0f);
    CHECK(info.units[0].environment->humidity_pct == 54.0f);
    CHECK(info.units[0].slots[1].material == "PETG");
    CHECK(info.units[0].slots[1].brand == "Jayo");
    CHECK(info.units.size() == 4);
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: an environment-only delta keeps the slots",
                 "[ams][cfs][flat][environment]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    feed(box_frame(full));
    REQUIRE(backend.get_system_info().supports_bypass);

    json warmer = full;
    warmer["temp_c"] = 31;
    const json delta = klipper_delta(full, warmer);
    REQUIRE(delta == json{{"temp_c", 31}});
    feed(box_frame(delta));

    const auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 4);
    REQUIRE(info.total_slots == 16);
    CHECK(info.units[0].slots[0].material == "PLA");
    CHECK(info.units[0].slots[0].brand == "Sunlu");
    CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    REQUIRE(units_with_environment(info) == std::vector<int>{0});
    CHECK(info.units[0].environment->temperature_c == 31.0f);
    CHECK(info.units[0].environment->humidity_pct == 54.0f);
    // The external holder is still the one slots[] listed.
    CHECK(info.supports_bypass);
}

TEST_CASE_METHOD(CfsGoldenFixture,
                 "CFS flat: a load_path delta moves the environment to box_addr and back",
                 "[ams][cfs][flat][environment]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    REQUIRE(full["load_path"]["box_addr"].is_null());
    feed(box_frame(full));
    REQUIRE(units_with_environment(backend.get_system_info()) == std::vector<int>{0});

    json on_box4 = full;
    on_box4["load_path"]["box_addr"] = 4;
    feed(box_frame(klipper_delta(full, on_box4)));
    auto info = backend.get_system_info();
    REQUIRE(units_with_environment(info) == std::vector<int>{3});
    CHECK(info.units[3].environment->temperature_c == 27.0f);

    // Nothing loaded again: the firmware reads the lowest responding box and
    // names no address, so the reading returns to the first box.
    feed(box_frame(klipper_delta(on_box4, full)));
    info = backend.get_system_info();
    REQUIRE(units_with_environment(info) == std::vector<int>{0});
    CHECK(info.units[0].environment->humidity_pct == 54.0f);
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a runout-plan delta reads the bays it names",
                 "[ams][cfs][flat]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    feed(box_frame(full));
    REQUIRE(backend.get_endless_spool_config().empty());

    json planned = full;
    planned["runout"] = json{{"loaded_slot", 2}, {"chain", json::array({3})}};
    feed(box_frame(klipper_delta(full, planned)));

    CHECK(endless_spool_backup_for(backend.get_endless_spool_config(), 2) == 3);
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a new session forgets the last one's frames",
                 "[ams][cfs][flat][environment]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    feed(box_frame(full));
    REQUIRE(units_with_environment(backend.get_system_info()) == std::vector<int>{0});

    CfsTestAccess::start_load(backend);

    // The new session's box states its slots but no reading yet: the old
    // session's temp_c must not fill the gap.
    json no_reading = full;
    no_reading.erase("temp_c");
    no_reading.erase("humidity_pct");
    feed(box_frame(json{{"slots", no_reading["slots"]}}));

    const auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 4);
    CHECK(units_with_environment(info).empty());
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a delta field replaces its predecessor whole",
                 "[ams][cfs][flat]") {
    const json full = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    REQUIRE(full["materials"].contains("PLA"));
    feed(box_frame(full));
    const int stated = full["materials"]["PLA"]["target_temp"].get<int>();
    REQUIRE(backend.get_system_info().units[0].slots[0].nozzle_temp_max == stated);

    // A material deleted from box.py's table: Klipper sends the whole new
    // table, and the PLA entry it no longer lists is gone.
    json edited = full;
    edited["materials"].erase("PLA");
    feed(box_frame(klipper_delta(full, edited)));

    const auto info = backend.get_system_info();
    CHECK(info.units[0].slots[0].material == "PLA");
    CHECK(info.units[0].slots[0].nozzle_temp_max != stated);
}

namespace {

/// The four-box fixture with bay 2 threaded through to the toolhead.
json flat_box_with_bay_2_loaded() {
    json box = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    box["loaded_slot"] = 2;
    box["slots"][2]["loaded"] = true;
    return box;
}

} // namespace

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a slots-only edit keeps the loaded bay",
                 "[ams][cfs][flat]") {
    const json full = flat_box_with_bay_2_loaded();
    feed(box_frame(full));
    REQUIRE(backend.get_system_info().current_slot == 2);

    json edited = full;
    edited["slots"][0]["material"] = "PETG";
    const json delta = klipper_delta(full, edited);
    REQUIRE(delta.size() == 1);
    feed(box_frame(delta));

    const auto info = backend.get_system_info();
    CHECK(info.current_slot == 2);
    CHECK(info.current_tool == 2);
    CHECK(info.units[0].slots[0].material == "PETG");
}

TEST_CASE_METHOD(CfsGoldenFixture,
                 "CFS flat: a delta before the new session's first full frame changes nothing",
                 "[ams][cfs][flat]") {
    const json full = flat_box_with_bay_2_loaded();
    feed(box_frame(full));
    REQUIRE(backend.get_system_info().current_slot == 2);

    CfsTestAccess::start_load(backend);

    // A notify can beat the initial query's answer. It names no slots, so
    // there is no box to parse yet.
    feed(box_frame(json{{"temp_c", 31}}));
    auto info = backend.get_system_info();
    CHECK(info.current_slot == 2);
    CHECK(info.units.size() == 4);

    // The query's full frame lands, and the delta's field is in the merge.
    json answered = full;
    answered.erase("temp_c");
    feed(box_frame(answered));
    info = backend.get_system_info();
    CHECK(info.current_slot == 2);
    REQUIRE(units_with_environment(info) == std::vector<int>{0});
    CHECK(info.units[0].environment->temperature_c == 31.0f);
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS: a stock unit frame after flat ones returns to stock",
                 "[ams][cfs][flat]") {
    feed(box_frame(load_box_fixture("cfs_fork_four_box_L8MMBCCK.json")));
    REQUIRE(CfsTestAccess::macro_variant(backend) == CfsMacroVariant::Fork);

    feed(box_frame(json{{"state", "connect"},
                        {"filament", 1},
                        {"auto_refill", 1},
                        {"filament_useup", 0},
                        {"map", json{{"T1A", "T1A"}, {"T1B", "T1B"}}},
                        {"T1", golden_unit("None", {"Creality", "Creality", "none", "none"})}}));
    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(CfsTestAccess::macro_variant(backend) != CfsMacroVariant::Fork);
    CHECK_FALSE(info.filament_runout);

    // Stock deltas that name no unit read as stock: the runout latch and the
    // tool map come from them, and the merged units survive.
    feed(box_frame(json{{"filament", 1}, {"filament_useup", 1}}));
    feed(box_frame(json{{"map", json{{"T1A", "T1B"}, {"T1B", "T1A"}}}}));
    info = backend.get_system_info();
    CHECK(info.filament_runout);
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].slots[0].brand == "Creality");
    REQUIRE(info.tool_to_slot_map.size() >= 2);
    CHECK(info.tool_to_slot_map[0] == 1);
    CHECK(info.tool_to_slot_map[1] == 0);
}

namespace {

/// The four-box fixture as revision 2 reports it: one reading per box in
/// boxes[], listed out of address order so a join by position would misfile
/// them. Box 2 is offline and box 4 is not listed.
json make_revision2_box() {
    json box = load_box_fixture("cfs_fork_four_box_L8MMBCCK.json");
    box["api_revision"] = 2;
    box["boxes"] = json::parse(R"([
        {"address": 3, "online": true, "slots": [8, 9, 10, 11],
         "temp_c": 30.5, "humidity_pct": 61, "fault": null},
        {"address": 1, "online": true, "slots": [0, 1, 2, 3],
         "temp_c": 24, "humidity_pct": 38, "fault": null},
        {"address": 2, "online": false, "slots": [4, 5, 6, 7],
         "temp_c": null, "humidity_pct": null, "fault": null}
    ])");
    return box;
}

} // namespace

TEST_CASE("CFS flat: revision 2 gives each box its own environment reading",
          "[ams][cfs][flat][fork][environment][boxes]") {
    json box = make_revision2_box();

    SECTION("readings join on address") {
        const auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units.size() == 4);
        REQUIRE(units_with_environment(info) == std::vector<int>{0, 2});
        CHECK(info.units[0].environment->temperature_c == 24.0f);
        CHECK(info.units[0].environment->humidity_pct == 38.0f);
        CHECK(info.units[0].environment->has_humidity);
        CHECK(info.units[2].environment->temperature_c == 30.5f);
        CHECK(info.units[2].environment->humidity_pct == 61.0f);
    }

    SECTION("an online box with null readings shows none") {
        box["boxes"][0]["temp_c"] = nullptr;
        box["boxes"][0]["humidity_pct"] = nullptr;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0});
    }

    SECTION("an offline box shows none even with a stale reading") {
        box["boxes"][2]["temp_c"] = 22;
        box["boxes"][2]["humidity_pct"] = 40;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0, 2});
    }

    SECTION("the top-level legacy pair is not also attributed") {
        box["load_path"]["box_addr"] = 4;
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)) ==
                std::vector<int>{0, 2});
    }

    SECTION("revision 1 ignores boxes[] and attributes the top-level pair as before") {
        box.erase("api_revision");
        box["load_path"]["box_addr"] = 2;
        const auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(units_with_environment(info) == std::vector<int>{1});
        CHECK(info.units[1].environment->temperature_c == 27.0f);
        CHECK(info.units[1].environment->humidity_pct == 54.0f);
    }

    SECTION("revision 2 before the first poll (boxes empty) shows no reading") {
        box["boxes"] = json::array();
        REQUIRE(units_with_environment(AmsBackendCfs::parse_box_status(box)).empty());
    }
}

TEST_CASE_METHOD(CfsGoldenFixture, "CFS flat: a delta without boxes[] keeps the per-box readings",
                 "[ams][cfs][flat][environment][boxes]") {
    const json full = make_revision2_box();
    feed(box_frame(full));
    REQUIRE(units_with_environment(backend.get_system_info()) == std::vector<int>{0, 2});

    json edited = full;
    edited["slots"][1]["material"] = "PETG";
    const json delta = klipper_delta(full, edited);
    REQUIRE_FALSE(delta.contains("boxes"));
    feed(box_frame(delta));

    auto info = backend.get_system_info();
    REQUIRE(units_with_environment(info) == std::vector<int>{0, 2});
    CHECK(info.units[2].environment->temperature_c == 30.5f);

    json box3_cooler = edited;
    box3_cooler["boxes"][0]["temp_c"] = 28;
    feed(box_frame(klipper_delta(edited, box3_cooler)));
    info = backend.get_system_info();
    REQUIRE(units_with_environment(info) == std::vector<int>{0, 2});
    CHECK(info.units[2].environment->temperature_c == 28.0f);
    CHECK(info.units[0].environment->temperature_c == 24.0f);
}

TEST_CASE_METHOD(CfsGoldenFixture,
                 "CFS flat: a box reporting offline reads disconnected until it returns",
                 "[ams][cfs][flat][boxes]") {
    const json full = make_revision2_box();
    feed(box_frame(full));
    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 4);
    CHECK(info.units[0].connected);
    CHECK_FALSE(info.units[1].connected);
    CHECK(info.units[2].connected);
    // Not listed in boxes[]: nothing says it is offline.
    CHECK(info.units[3].connected);

    json back = full;
    back["boxes"][2]["online"] = true;
    back["boxes"][2]["temp_c"] = 23;
    back["boxes"][2]["humidity_pct"] = 41;
    feed(box_frame(klipper_delta(full, back)));
    info = backend.get_system_info();
    CHECK(info.units[1].connected);
    REQUIRE(units_with_environment(info) == std::vector<int>{0, 1, 2});

    feed(box_frame(klipper_delta(back, full)));
    info = backend.get_system_info();
    CHECK_FALSE(info.units[1].connected);
    CHECK(info.units[0].connected);
}

TEST_CASE("CFS flat: revision 1 ignores boxes[] online flags", "[ams][cfs][flat][boxes]") {
    json box = make_revision2_box();
    box.erase("api_revision");
    const auto info = AmsBackendCfs::parse_box_status(box);
    for (const auto& u : info.units) {
        INFO("unit " << u.unit_index);
        CHECK(u.connected);
    }
}

// A K2 Plus on the k2-improvements box.py, API revision 1 with a boxes[] list
// (bundle EH9J7764). The printer picture picks its CFS variant off a unit that
// is connected and not absent, so this payload has to produce one.
TEST_CASE("CFS flat schema: a k2-improvements revision 1 payload has a present unit",
          "[ams][cfs][flat][printer_image_variants]") {
    const json box = json::parse(R"({
        "api_version": 1, "data_ready": true, "driver_ready": true,
        "boxes": [{"address": 1, "fault": null, "humidity_pct": 20, "online": true,
                   "slots": [0, 1, 2, 3], "state": "IDLE", "state_code": 0,
                   "status": "OK", "status_code": 0, "temp_c": 23}],
        "filament_detected": true, "filament_sensor_error": null,
        "humidity_pct": 20, "loaded_mask": 4, "loaded_slot": 2,
        "runout": {"chain": [], "loaded_slot": 2}, "runout_swap_enabled": true,
        "slot_filament_mask": 7, "state": "IDLE", "state_code": 0,
        "status": "OK", "status_code": 0, "temp_c": 23, "tracking_active": false,
        "slots": [
            {"index": 0, "external": false, "present": true, "loaded": false,
             "material": "PLA", "color": "#1A1A1A", "brand": "Overture", "name": ""},
            {"index": 1, "external": false, "present": true, "loaded": false,
             "material": "PLA", "color": "#AEEA00", "brand": "SUNLU", "name": ""},
            {"index": 2, "external": false, "present": true, "loaded": true,
             "material": "PLA", "color": "#FFFFFF", "brand": "Overture", "name": ""},
            {"index": 3, "external": false, "present": false, "loaded": false,
             "material": "", "color": "", "brand": "", "name": ""},
            {"index": 4, "external": true, "present": true, "loaded": false,
             "material": "", "color": "", "brand": "", "name": ""}
        ]
    })");

    const auto info = AmsBackendCfs::parse_box_status(box);
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].connected);
    CHECK_FALSE(info.units[0].absent);
}
