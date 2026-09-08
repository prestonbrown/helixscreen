// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "printer_discovery.h"
#include "standard_macros.h"
#include "wizard_config_paths.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

// ============================================================================
// StandardMacroInfo Tests
// ============================================================================

TEST_CASE("StandardMacroInfo - is_empty", "[standard_macros]") {
    StandardMacroInfo info;
    info.slot = StandardMacroSlot::LoadFilament;
    info.slot_name = "load_filament";
    info.display_name = "Load Filament";

    SECTION("Empty when all sources are empty") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "";
        REQUIRE(info.is_empty());
    }

    SECTION("Not empty with configured macro") {
        info.configured_macro = "MY_LOAD";
        info.detected_macro = "";
        info.fallback_macro = "";
        REQUIRE_FALSE(info.is_empty());
    }

    SECTION("Not empty with detected macro") {
        info.configured_macro = "";
        info.detected_macro = "LOAD_FILAMENT";
        info.fallback_macro = "";
        REQUIRE_FALSE(info.is_empty());
    }

    SECTION("Not empty with fallback macro") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "HELIX_LOAD";
        REQUIRE_FALSE(info.is_empty());
    }
}

TEST_CASE("StandardMacroInfo - get_macro priority", "[standard_macros]") {
    StandardMacroInfo info;
    info.slot = StandardMacroSlot::BedLevel;
    info.slot_name = "bed_level";
    info.display_name = "Bed Level";

    SECTION("Configured takes priority over detected and fallback") {
        info.configured_macro = "MY_BED_LEVEL";
        info.detected_macro = "BED_MESH_CALIBRATE";
        info.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";
        REQUIRE(info.get_macro() == "MY_BED_LEVEL");
    }

    SECTION("Detected takes priority over fallback when no configured") {
        info.configured_macro = "";
        info.detected_macro = "BED_MESH_CALIBRATE";
        info.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";
        REQUIRE(info.get_macro() == "BED_MESH_CALIBRATE");
    }

    SECTION("Fallback used when no configured or detected") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";
        REQUIRE(info.get_macro() == "HELIX_BED_MESH_IF_NEEDED");
    }

    SECTION("Empty string when all sources empty") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "";
        REQUIRE(info.get_macro().empty());
    }
}

TEST_CASE("StandardMacroInfo - get_source", "[standard_macros]") {
    StandardMacroInfo info;
    info.slot = StandardMacroSlot::CleanNozzle;
    info.slot_name = "clean_nozzle";
    info.display_name = "Clean Nozzle";

    SECTION("CONFIGURED when configured_macro set") {
        info.configured_macro = "MY_CLEAN";
        info.detected_macro = "CLEAN_NOZZLE";
        info.fallback_macro = "HELIX_CLEAN_NOZZLE";
        REQUIRE(info.get_source() == MacroSource::CONFIGURED);
    }

    SECTION("DETECTED when only detected_macro set") {
        info.configured_macro = "";
        info.detected_macro = "CLEAN_NOZZLE";
        info.fallback_macro = "HELIX_CLEAN_NOZZLE";
        REQUIRE(info.get_source() == MacroSource::DETECTED);
    }

    SECTION("FALLBACK when only fallback_macro set") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "HELIX_CLEAN_NOZZLE";
        REQUIRE(info.get_source() == MacroSource::FALLBACK);
    }

    SECTION("NONE when all empty") {
        info.configured_macro = "";
        info.detected_macro = "";
        info.fallback_macro = "";
        REQUIRE(info.get_source() == MacroSource::NONE);
    }
}

// ============================================================================
// Slot Name Conversion Tests
// ============================================================================

TEST_CASE("StandardMacros - slot_to_name", "[standard_macros]") {
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::LoadFilament) == "load_filament");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::UnloadFilament) == "unload_filament");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::Purge) == "purge");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::Pause) == "pause");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::Resume) == "resume");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::Cancel) == "cancel");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::BedMesh) == "bed_mesh");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::BedLevel) == "bed_level");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::CleanNozzle) == "clean_nozzle");
    REQUIRE(StandardMacros::slot_to_name(StandardMacroSlot::HeatSoak) == "heat_soak");
}

TEST_CASE("StandardMacros - slot_from_name", "[standard_macros]") {
    SECTION("Valid slot names") {
        REQUIRE(StandardMacros::slot_from_name("load_filament") == StandardMacroSlot::LoadFilament);
        REQUIRE(StandardMacros::slot_from_name("unload_filament") ==
                StandardMacroSlot::UnloadFilament);
        REQUIRE(StandardMacros::slot_from_name("purge") == StandardMacroSlot::Purge);
        REQUIRE(StandardMacros::slot_from_name("pause") == StandardMacroSlot::Pause);
        REQUIRE(StandardMacros::slot_from_name("resume") == StandardMacroSlot::Resume);
        REQUIRE(StandardMacros::slot_from_name("cancel") == StandardMacroSlot::Cancel);
        REQUIRE(StandardMacros::slot_from_name("bed_mesh") == StandardMacroSlot::BedMesh);
        REQUIRE(StandardMacros::slot_from_name("bed_level") == StandardMacroSlot::BedLevel);
        REQUIRE(StandardMacros::slot_from_name("clean_nozzle") == StandardMacroSlot::CleanNozzle);
        REQUIRE(StandardMacros::slot_from_name("heat_soak") == StandardMacroSlot::HeatSoak);
    }

    SECTION("Invalid slot names return nullopt") {
        REQUIRE_FALSE(StandardMacros::slot_from_name("invalid_slot").has_value());
        REQUIRE_FALSE(StandardMacros::slot_from_name("LOAD_FILAMENT").has_value());
        REQUIRE_FALSE(StandardMacros::slot_from_name("Load Filament").has_value());
        REQUIRE_FALSE(StandardMacros::slot_from_name("").has_value());
    }
}

// ============================================================================
// Auto-Detection Tests
// ============================================================================

TEST_CASE("StandardMacros - auto-detection", "[standard_macros]") {
    auto& macros = StandardMacros::instance();
    macros.reset();

    SECTION("Detects standard macro patterns") {
        helix::PrinterDiscovery hardware;
        json objects = {"extruder",
                        "heater_bed",
                        "gcode_macro LOAD_FILAMENT",
                        "gcode_macro UNLOAD_FILAMENT",
                        "gcode_macro PAUSE",
                        "gcode_macro RESUME",
                        "gcode_macro CANCEL_PRINT",
                        "gcode_macro BED_MESH_CALIBRATE",
                        "gcode_macro CLEAN_NOZZLE"};
        hardware.parse_objects(objects);

        macros.init(hardware);

        REQUIRE(macros.is_initialized());

        // Verify detection
        REQUIRE(macros.get(StandardMacroSlot::LoadFilament).detected_macro == "LOAD_FILAMENT");
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "UNLOAD_FILAMENT");
        REQUIRE(macros.get(StandardMacroSlot::Pause).detected_macro == "PAUSE");
        REQUIRE(macros.get(StandardMacroSlot::Resume).detected_macro == "RESUME");
        REQUIRE(macros.get(StandardMacroSlot::Cancel).detected_macro == "CANCEL_PRINT");
        REQUIRE(macros.get(StandardMacroSlot::BedMesh).detected_macro == "BED_MESH_CALIBRATE");
        REQUIRE(macros.get(StandardMacroSlot::CleanNozzle).detected_macro == "CLEAN_NOZZLE");

        // Slots without matching macros should be empty
        REQUIRE(macros.get(StandardMacroSlot::Purge).detected_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::BedLevel).detected_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::HeatSoak).detected_macro.empty());
    }

    SECTION("Detects M-code variants") {
        helix::PrinterDiscovery hardware;
        json objects = {"extruder", "gcode_macro M701", "gcode_macro M702", "gcode_macro M601",
                        "gcode_macro M602"};
        hardware.parse_objects(objects);

        macros.init(hardware);

        REQUIRE(macros.get(StandardMacroSlot::LoadFilament).detected_macro == "M701");
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "M702");
        REQUIRE(macros.get(StandardMacroSlot::Pause).detected_macro == "M601");
        REQUIRE(macros.get(StandardMacroSlot::Resume).detected_macro == "M602");
    }

    SECTION("QIDI's stock M604/M603 fill an otherwise empty slot") {
        // The macros QIDI's own screen drives. Neither spelling means anything
        // in stock Klipper, and Marlin has no M604 at all, so a printer that
        // defines them is running QIDI's convention.
        //
        // The set below is the stock Q2 one (the config dump in #1030): it
        // carries NO LOAD_FILAMENT, LOAD_MATERIAL, M701, UNLOAD_FILAMENT,
        // UNLOAD_MATERIAL, M702 or QUIT_MATERIAL, so every earlier pattern in
        // both lists misses and the tail entries are what stands between that
        // printer and an empty slot.
        helix::PrinterDiscovery qidi;
        json objects = {"extruder",          "gcode_macro M604",         "gcode_macro M603",
                        "gcode_macro _CG28", "gcode_macro CLEAR_NOZZLE", "gcode_macro PRINT_START"};
        qidi.parse_objects(objects);
        macros.init(qidi);

        REQUIRE(macros.get(StandardMacroSlot::LoadFilament).detected_macro == "M604");
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "M603");
    }

    SECTION("M604/M603 rank last — a conventionally named macro keeps the slot") {
        // Marlin's M603 configures a filament change rather than running one, so
        // a printer that has both readings must not have the QIDI one win.
        helix::PrinterDiscovery mixed;
        json objects = {"extruder", "gcode_macro LOAD_FILAMENT", "gcode_macro UNLOAD_FILAMENT",
                        "gcode_macro M604", "gcode_macro M603"};
        mixed.parse_objects(objects);
        macros.init(mixed);

        REQUIRE(macros.get(StandardMacroSlot::LoadFilament).detected_macro == "LOAD_FILAMENT");
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "UNLOAD_FILAMENT");
    }

    SECTION("Helix override beats Creality QUIT_MATERIAL, not native macros") {
        // The Creality K1 family's stock "unload" (QUIT_MATERIAL) purges
        // filament FORWARD and retracts only part of it — a melt-zone
        // clearer for manually-cut filament, not an unload. With our macro
        // pack installed, HELIX_UNLOAD_FILAMENT takes the slot.
        helix::PrinterDiscovery k1c;
        json objects = {"extruder", "gcode_macro LOAD_MATERIAL", "gcode_macro QUIT_MATERIAL",
                        "gcode_macro HELIX_UNLOAD_FILAMENT"};
        k1c.parse_objects(objects);
        macros.init(k1c);
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro ==
                "HELIX_UNLOAD_FILAMENT");

        // Without the override installed, the stock macro keeps the slot.
        helix::PrinterDiscovery stock;
        json stock_objects = {"extruder", "gcode_macro LOAD_MATERIAL", "gcode_macro QUIT_MATERIAL"};
        stock.parse_objects(stock_objects);
        macros.init(stock);
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "QUIT_MATERIAL");

        // A printer's own UNLOAD_FILAMENT always outranks the override.
        helix::PrinterDiscovery native;
        json native_objects = {"extruder", "gcode_macro UNLOAD_FILAMENT",
                               "gcode_macro HELIX_UNLOAD_FILAMENT"};
        native.parse_objects(native_objects);
        macros.init(native);
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).detected_macro == "UNLOAD_FILAMENT");
    }

    SECTION("Detects alternative bed level patterns") {
        helix::PrinterDiscovery hardware;

        SECTION("QUAD_GANTRY_LEVEL") {
            json objects = {"gcode_macro QUAD_GANTRY_LEVEL"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::BedLevel).detected_macro == "QUAD_GANTRY_LEVEL");
        }

        SECTION("Z_TILT_ADJUST") {
            json objects = {"gcode_macro Z_TILT_ADJUST"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::BedLevel).detected_macro == "Z_TILT_ADJUST");
        }

        SECTION("QGL shorthand") {
            json objects = {"gcode_macro QGL"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::BedLevel).detected_macro == "QGL");
        }
    }

    SECTION("Detects nozzle wipe variants") {
        helix::PrinterDiscovery hardware;

        SECTION("NOZZLE_WIPE") {
            json objects = {"gcode_macro NOZZLE_WIPE"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::CleanNozzle).detected_macro == "NOZZLE_WIPE");
        }

        SECTION("WIPE_NOZZLE") {
            json objects = {"gcode_macro WIPE_NOZZLE"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::CleanNozzle).detected_macro == "WIPE_NOZZLE");
        }
    }

    SECTION("Detects purge variants") {
        helix::PrinterDiscovery hardware;

        SECTION("PURGE") {
            json objects = {"gcode_macro PURGE"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::Purge).detected_macro == "PURGE");
        }

        SECTION("PURGE_LINE") {
            json objects = {"gcode_macro PURGE_LINE"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::Purge).detected_macro == "PURGE_LINE");
        }

        SECTION("PRIME_LINE") {
            json objects = {"gcode_macro PRIME_LINE"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::Purge).detected_macro == "PRIME_LINE");
        }
    }

    SECTION("Detects heat soak variants") {
        helix::PrinterDiscovery hardware;

        SECTION("HEAT_SOAK") {
            json objects = {"gcode_macro HEAT_SOAK"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::HeatSoak).detected_macro == "HEAT_SOAK");
        }

        SECTION("CHAMBER_SOAK") {
            json objects = {"gcode_macro CHAMBER_SOAK"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::HeatSoak).detected_macro == "CHAMBER_SOAK");
        }

        SECTION("SOAK") {
            json objects = {"gcode_macro SOAK"};
            hardware.parse_objects(objects);
            macros.reset();
            macros.init(hardware);
            REQUIRE(macros.get(StandardMacroSlot::HeatSoak).detected_macro == "SOAK");
        }
    }
}

// ============================================================================
// HELIX Fallback Tests
// ============================================================================

TEST_CASE("StandardMacros - HELIX fallbacks", "[standard_macros]") {
    auto& macros = StandardMacros::instance();
    macros.reset();

    helix::PrinterDiscovery hardware;
    json objects = {"extruder", "gcode_macro HELIX_BED_MESH_IF_NEEDED",
                    "gcode_macro HELIX_CLEAN_NOZZLE", "gcode_macro HELIX_BED_MESH_IF_NEEDED"};
    hardware.parse_objects(objects);

    macros.init(hardware);

    SECTION("BedLevel has no fallback (removed in favor of BedMesh slot)") {
        // BedLevel no longer uses HELIX_BED_MESH_IF_NEEDED as a fallback.
        // The new BedMesh slot handles bed mesh calibration separately.
        // BedLevel is now only for physical leveling (QGL, Z_TILT_ADJUST).
        const auto& bed_level = macros.get(StandardMacroSlot::BedLevel);
        REQUIRE(bed_level.fallback_macro.empty());
        REQUIRE(bed_level.detected_macro.empty());
        REQUIRE(bed_level.is_empty());
        REQUIRE(bed_level.get_source() == MacroSource::NONE);
    }

    SECTION("CleanNozzle has HELIX fallback when installed") {
        const auto& clean_nozzle = macros.get(StandardMacroSlot::CleanNozzle);
        REQUIRE(clean_nozzle.fallback_macro == "HELIX_CLEAN_NOZZLE");
        REQUIRE_FALSE(clean_nozzle.is_empty());
        REQUIRE(clean_nozzle.get_source() == MacroSource::FALLBACK);
    }

    SECTION("BedMesh has HELIX fallback when installed") {
        const auto& bed_mesh = macros.get(StandardMacroSlot::BedMesh);
        REQUIRE(bed_mesh.fallback_macro == "HELIX_BED_MESH_IF_NEEDED");
        REQUIRE_FALSE(bed_mesh.is_empty());
        REQUIRE(bed_mesh.get_source() == MacroSource::FALLBACK);
    }

    SECTION("Other slots have no fallbacks") {
        REQUIRE(macros.get(StandardMacroSlot::LoadFilament).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::UnloadFilament).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::Purge).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::Pause).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::Resume).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::Cancel).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::BedLevel).fallback_macro.empty());
        REQUIRE(macros.get(StandardMacroSlot::HeatSoak).fallback_macro.empty());
    }
}

// ============================================================================
// Reset and Initialization State Tests
// ============================================================================

TEST_CASE("StandardMacros - reset clears detection", "[standard_macros]") {
    auto& macros = StandardMacros::instance();
    macros.reset();

    // Initialize with some macros
    helix::PrinterDiscovery hardware;
    json objects = {"gcode_macro LOAD_FILAMENT", "gcode_macro PAUSE"};
    hardware.parse_objects(objects);
    macros.init(hardware);

    REQUIRE(macros.is_initialized());
    REQUIRE_FALSE(macros.get(StandardMacroSlot::LoadFilament).detected_macro.empty());

    // Reset should clear detected macros
    macros.reset();
    REQUIRE_FALSE(macros.is_initialized());
    REQUIRE(macros.get(StandardMacroSlot::LoadFilament).detected_macro.empty());
}

TEST_CASE("StandardMacros - all() returns all slots", "[standard_macros]") {
    const auto& macros = StandardMacros::instance();
    const auto& all_slots = macros.all();

    REQUIRE(all_slots.size() == static_cast<size_t>(StandardMacroSlot::COUNT));

    // Verify all slots are present and in order
    REQUIRE(all_slots[0].slot == StandardMacroSlot::LoadFilament);
    REQUIRE(all_slots[1].slot == StandardMacroSlot::UnloadFilament);
    REQUIRE(all_slots[2].slot == StandardMacroSlot::Purge);
    REQUIRE(all_slots[3].slot == StandardMacroSlot::Pause);
    REQUIRE(all_slots[4].slot == StandardMacroSlot::Resume);
    REQUIRE(all_slots[5].slot == StandardMacroSlot::Cancel);
    REQUIRE(all_slots[6].slot == StandardMacroSlot::BedMesh);
    REQUIRE(all_slots[7].slot == StandardMacroSlot::BedLevel);
    REQUIRE(all_slots[8].slot == StandardMacroSlot::ScrewsTilt);
    REQUIRE(all_slots[9].slot == StandardMacroSlot::CleanNozzle);
    REQUIRE(all_slots[10].slot == StandardMacroSlot::HeatSoak);

    // This test exists to make an enum insertion LOUD. It is safe to renumber
    // here only because persistence keys on slot_name, never the index:
    // save_to_config()/load_from_config() write "/standard_macros/<slot_name>",
    // and the quick_button_N settings store names ("clean_nozzle", "bed_level")
    // resolved through slot_from_name(). If a numeric slot value ever starts
    // crossing a persistence boundary, inserting mid-enum silently remaps every
    // user's saved macro assignments and this test is the tripwire.
}

// ============================================================================
// ScrewsTilt slot
// ============================================================================

TEST_CASE("StandardMacros - ScrewsTilt slot", "[standard_macros][screws_tilt]") {
    auto& macros = StandardMacros::instance();
    macros.reset();

    SECTION("The bare command outranks the heating wrapper") {
        // ZMOD ships both. BED_LEVEL_SCREWS_TUNE does more - homes, heats to
        // 130/80, blocks on TEMPERATURE_WAIT, tares, then calls
        // SCREWS_TILT_CALCULATE - so auto-selecting it would silently turn a ~90s
        // operation into a multi-minute heat cycle for every existing ZMOD user.
        // Probe preparation supplies the tare, so the wrapper stays opt-in.
        helix::PrinterDiscovery zmod;
        json objects = {"extruder", "screws_tilt_adjust", "gcode_macro BED_LEVEL_SCREWS_TUNE",
                        "gcode_macro SCREWS_TILT_CALCULATE"};
        zmod.parse_objects(objects);
        macros.init(zmod);

        REQUIRE(macros.get(StandardMacroSlot::ScrewsTilt).detected_macro ==
                "SCREWS_TILT_CALCULATE");
    }

    SECTION("The wrapper is still taken when it is the only one offered") {
        helix::PrinterDiscovery only_wrapper;
        json objects = {"extruder", "gcode_macro BED_LEVEL_SCREWS_TUNE"};
        only_wrapper.parse_objects(objects);
        macros.init(only_wrapper);

        REQUIRE(macros.get(StandardMacroSlot::ScrewsTilt).detected_macro ==
                "BED_LEVEL_SCREWS_TUNE");
    }

    SECTION("A printer with neither leaves the slot empty - there is no fallback") {
        // Nothing generic can synthesize screw guidance, so unlike BedMesh this
        // slot must NOT acquire a HELIX_* fallback.
        helix::PrinterDiscovery none;
        json objects = {"extruder", "heater_bed"};
        none.parse_objects(objects);
        macros.init(none);

        REQUIRE(macros.get(StandardMacroSlot::ScrewsTilt).is_empty());
    }

    SECTION("A user assignment outranks detection") {
        helix::PrinterDiscovery zmod;
        json objects = {"extruder", "gcode_macro BED_LEVEL_SCREWS_TUNE",
                        "gcode_macro SCREWS_TILT_CALCULATE"};
        zmod.parse_objects(objects);
        macros.init(zmod);
        macros.set_macro(StandardMacroSlot::ScrewsTilt, "BED_LEVEL_SCREWS_TUNE");

        REQUIRE(macros.get(StandardMacroSlot::ScrewsTilt).get_macro() == "BED_LEVEL_SCREWS_TUNE");
    }

    SECTION("Adding the slot did not disturb its neighbours") {
        // The enum is indexed into slots_, so an insertion in the middle is
        // exactly how adjacent slots get silently reassigned.
        helix::PrinterDiscovery hw;
        json objects = {"extruder", "gcode_macro QUAD_GANTRY_LEVEL", "gcode_macro CLEAN_NOZZLE",
                        "gcode_macro BED_MESH_CALIBRATE"};
        hw.parse_objects(objects);
        macros.init(hw);

        REQUIRE(macros.get(StandardMacroSlot::BedLevel).detected_macro == "QUAD_GANTRY_LEVEL");
        REQUIRE(macros.get(StandardMacroSlot::CleanNozzle).detected_macro == "CLEAN_NOZZLE");
        REQUIRE(macros.get(StandardMacroSlot::BedMesh).detected_macro == "BED_MESH_CALIBRATE");
        REQUIRE(macros.get(StandardMacroSlot::ScrewsTilt).is_empty());
    }
}

// ============================================================================
// Shipped tier — the sequence the printer database ships for one machine
// ============================================================================

TEST_CASE("StandardMacroInfo - shipped outranks detection, configured outranks shipped",
          "[standard_macros][shipped_macro]") {
    StandardMacroInfo info;
    info.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";

    SECTION("fallback alone") {
        CHECK(info.get_macro() == "HELIX_BED_MESH_IF_NEEDED");
        CHECK(info.get_source() == MacroSource::FALLBACK);
    }

    SECTION("detection beats the fallback") {
        info.detected_macro = "BED_MESH_CALIBRATE";
        CHECK(info.get_macro() == "BED_MESH_CALIBRATE");
        CHECK(info.get_source() == MacroSource::DETECTED);
    }

    SECTION("a shipped sequence beats detection") {
        // The case this tier exists for: detection resolves to something that
        // runs, but not to the sequence the machine needs — which tares its
        // load cell before the mesh means anything.
        info.detected_macro = "BED_MESH_CALIBRATE";
        info.shipped_macro = "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE";
        CHECK(info.get_macro() == "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE");
        CHECK(info.get_source() == MacroSource::SHIPPED);
    }

    SECTION("the user's own choice beats everything") {
        // A printer whose shipped sequence is wrong for this user's setup is
        // exactly why the Settings override exists.
        info.detected_macro = "BED_MESH_CALIBRATE";
        info.shipped_macro = "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE";
        info.configured_macro = "MY_MESH";
        CHECK(info.get_macro() == "MY_MESH");
        CHECK(info.get_source() == MacroSource::CONFIGURED);
    }

    SECTION("a shipped sequence alone is not an empty slot") {
        StandardMacroInfo shipped_only;
        shipped_only.shipped_macro = "BED_MESH_CALIBRATE_WITH_WIPE";
        CHECK_FALSE(shipped_only.is_empty());
    }
}

TEST_CASE("StandardMacros - init fills the shipped tier from the printer database",
          "[standard_macros][shipped_macro]") {
    auto& macros = StandardMacros::instance();
    helix::PrinterDiscovery hardware;
    json objects = {"extruder", "heater_bed", "gcode_macro BED_MESH_CALIBRATE"};
    hardware.parse_objects(objects);

    SECTION("a printer with no shipped sequence keeps detection") {
        macros.init(hardware, "Some Random Printer");
        const auto& info = macros.get(StandardMacroSlot::BedMesh);
        CHECK(info.shipped_macro.empty());
        CHECK(info.get_source() == MacroSource::DETECTED);
    }

    SECTION("an unnamed printer fills nothing") {
        // Tests and early startup both reach init() before the printer is known.
        macros.init(hardware, "");
        CHECK(macros.get(StandardMacroSlot::BedMesh).shipped_macro.empty());
    }

    SECTION("the Centauri Carbon's mesh sequence reaches the slot") {
        macros.init(hardware, "Elegoo Centauri Carbon");
        const auto& info = macros.get(StandardMacroSlot::BedMesh);
        REQUIRE_FALSE(info.shipped_macro.empty());
        // The tare is the whole point: mainline-Klipper load_cell_probe aborts
        // on a stale one, and probing plain BED_MESH_CALIBRATE never tares.
        CHECK(info.shipped_macro.find("LOAD_CELL_SAVE_TARE") != std::string::npos);
        CHECK(info.get_source() == MacroSource::SHIPPED);
        CHECK(info.get_macro() == info.shipped_macro);
    }

    macros.reset();
}

TEST_CASE("resolve_macro_script - substitution and self-preparation",
          "[standard_macros][shipped_macro]") {
    StandardMacroInfo info;

    SECTION("a plain macro name passes through and prepares nothing itself") {
        info.detected_macro = "BED_MESH_CALIBRATE";
        const auto r = resolve_macro_script(info, "_hs_temp");
        CHECK(r.script == "BED_MESH_CALIBRATE");
        // Not self-preparing: probe_preparation still gets to prepend its tare,
        // which is the whole reason ZMOD machines probe successfully.
        CHECK_FALSE(r.self_prepares);
    }

    SECTION("a shipped sequence keeps its own preparation") {
        info.detected_macro = "BED_MESH_CALIBRATE";
        info.shipped_macro = "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE";
        const auto r = resolve_macro_script(info, "_hs_temp");
        CHECK(r.script == "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE");
        CHECK(r.self_prepares);
    }

    SECTION("{profile} is substituted everywhere it appears") {
        info.shipped_macro = "BED_MESH_PROFILE LOAD={profile}\nBED_MESH_PROFILE SAVE={profile}";
        const auto r = resolve_macro_script(info, "_hs_temp");
        CHECK(r.script == "BED_MESH_PROFILE LOAD=_hs_temp\nBED_MESH_PROFILE SAVE=_hs_temp");
        CHECK(r.script.find("{profile}") == std::string::npos);
    }

    SECTION("a profile name containing the placeholder does not loop forever") {
        info.shipped_macro = "SAVE={profile}";
        const auto r = resolve_macro_script(info, "{profile}x");
        CHECK(r.script == "SAVE={profile}x");
    }

    SECTION("a user override is never treated as self-preparing") {
        // The user picked a bare macro name; assuming it tares would skip the
        // preparation their machine still needs.
        info.shipped_macro = "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE";
        info.configured_macro = "MY_MESH";
        const auto r = resolve_macro_script(info, "_hs_temp");
        CHECK(r.script == "MY_MESH");
        CHECK_FALSE(r.self_prepares);
    }

    SECTION("an empty slot resolves to nothing, and the caller decides") {
        const auto r = resolve_macro_script(info, "_hs_temp");
        CHECK(r.script.empty());
        CHECK_FALSE(r.self_prepares);
    }
}

TEST_CASE("resolve_macro_script - a conditional fallback is refused when the op must happen",
          "[standard_macros][shipped_macro]") {
    // HELIX_BED_MESH_IF_NEEDED reports "using existing mesh" and returns without
    // probing when a recent one exists. Correct at print start; for a Calibrate
    // button it would advance the UI to naming and offer to save a mesh nothing
    // re-measured. Callers that mean "now" must not receive it.
    StandardMacroInfo info;
    info.fallback_macro = "HELIX_BED_MESH_IF_NEEDED";

    CHECK(resolve_macro_script(info, "", /*accept_fallback=*/true).script ==
          "HELIX_BED_MESH_IF_NEEDED");
    CHECK(resolve_macro_script(info, "", /*accept_fallback=*/false).script.empty());

    SECTION("refusing the fallback does not refuse the tiers above it") {
        info.detected_macro = "BED_MESH_CALIBRATE";
        CHECK(resolve_macro_script(info, "", /*accept_fallback=*/false).script ==
              "BED_MESH_CALIBRATE");

        info.shipped_macro = "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE";
        const auto r = resolve_macro_script(info, "", /*accept_fallback=*/false);
        CHECK(r.script == "LOAD_CELL_SAVE_TARE\nBED_MESH_CALIBRATE_WITH_WIPE");
        CHECK(r.self_prepares);
    }
}

TEST_CASE("get_saved_printer_type - the source populated during discovery",
          "[standard_macros][shipped_macro]") {
    // StandardMacros::init() runs inside the discovery callback, BEFORE
    // auto_detect_and_save sets PrinterState's copy. Reading PrinterState there
    // yields "" on every run and the shipped tier silently never fills, so the
    // production call site must read config instead.
    helix::Config* config = helix::Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string saved =
        config->get<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");

    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "Elegoo Centauri Carbon");
    CHECK(helix::get_saved_printer_type() == "Elegoo Centauri Carbon");

    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, "");
    CHECK(helix::get_saved_printer_type().empty());

    config->set<std::string>(config->df() + helix::wizard::PRINTER_TYPE, saved);
}
