// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_discovery.h"
#include "standard_macros.h"

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
    REQUIRE(all_slots[8].slot == StandardMacroSlot::CleanNozzle);
    REQUIRE(all_slots[9].slot == StandardMacroSlot::HeatSoak);
}
