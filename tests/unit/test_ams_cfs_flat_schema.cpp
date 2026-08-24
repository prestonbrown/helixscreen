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

#include "ams_backend_cfs.h"
#include "ams_types.h"

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
// see the firmware swap. See docs/devel/printers/CREALITY_K2_SUPPORT.md
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
        REQUIRE(info.units[0].slot_count == 8);
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
        REQUIRE(s.color_rgb == AMS_DEFAULT_SLOT_COLOR);
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
        REQUIRE(info.units[0].slots[0].color_rgb == AMS_DEFAULT_SLOT_COLOR);
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
        REQUIRE(AmsBackendCfs::unload_gcode(CfsMacroVariant::K2).find("CR_BOX_CUT") !=
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
