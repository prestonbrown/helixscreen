// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capability_overrides.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

// ============================================================================
// OverrideState Parsing Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - parse_state", "[printer][overrides]") {
    SECTION("Parses 'auto' variants") {
        REQUIRE(CapabilityOverrides::parse_state("auto") == OverrideState::AUTO);
        REQUIRE(CapabilityOverrides::parse_state("AUTO") == OverrideState::AUTO);
        REQUIRE(CapabilityOverrides::parse_state("Auto") == OverrideState::AUTO);
    }

    SECTION("Parses 'enable' variants") {
        REQUIRE(CapabilityOverrides::parse_state("enable") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("enabled") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("ENABLE") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("on") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("true") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("yes") == OverrideState::ENABLE);
        REQUIRE(CapabilityOverrides::parse_state("1") == OverrideState::ENABLE);
    }

    SECTION("Parses 'disable' variants") {
        REQUIRE(CapabilityOverrides::parse_state("disable") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("disabled") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("DISABLE") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("off") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("false") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("no") == OverrideState::DISABLE);
        REQUIRE(CapabilityOverrides::parse_state("0") == OverrideState::DISABLE);
    }

    SECTION("Unrecognized values default to AUTO") {
        REQUIRE(CapabilityOverrides::parse_state("") == OverrideState::AUTO);
        REQUIRE(CapabilityOverrides::parse_state("maybe") == OverrideState::AUTO);
        REQUIRE(CapabilityOverrides::parse_state("unknown") == OverrideState::AUTO);
    }
}

TEST_CASE("CapabilityOverrides - state_to_string", "[printer][overrides]") {
    REQUIRE(CapabilityOverrides::state_to_string(OverrideState::AUTO) == "auto");
    REQUIRE(CapabilityOverrides::state_to_string(OverrideState::ENABLE) == "enable");
    REQUIRE(CapabilityOverrides::state_to_string(OverrideState::DISABLE) == "disable");
}

// ============================================================================
// Override State Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - get/set override", "[printer][overrides]") {
    CapabilityOverrides overrides;

    SECTION("Default override is AUTO") {
        REQUIRE(overrides.get_override(capability::BED_MESH) == OverrideState::AUTO);
        REQUIRE(overrides.get_override(capability::QGL) == OverrideState::AUTO);
        REQUIRE(overrides.get_override("unknown_capability") == OverrideState::AUTO);
    }

    SECTION("Can set and get overrides") {
        overrides.set_override(capability::BED_MESH, OverrideState::ENABLE);
        overrides.set_override(capability::QGL, OverrideState::DISABLE);

        REQUIRE(overrides.get_override(capability::BED_MESH) == OverrideState::ENABLE);
        REQUIRE(overrides.get_override(capability::QGL) == OverrideState::DISABLE);
    }

    SECTION("Override can be changed") {
        overrides.set_override(capability::CHAMBER, OverrideState::ENABLE);
        REQUIRE(overrides.get_override(capability::CHAMBER) == OverrideState::ENABLE);

        overrides.set_override(capability::CHAMBER, OverrideState::DISABLE);
        REQUIRE(overrides.get_override(capability::CHAMBER) == OverrideState::DISABLE);
    }
}

// ============================================================================
// Three-State Logic Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - is_available logic", "[printer][overrides]") {
    CapabilityOverrides overrides;

    // Create mock printer hardware
    helix::PrinterDiscovery hardware;
    json objects = {"bed_mesh", "quad_gantry_level", "gcode_macro CLEAN_NOZZLE"};
    hardware.parse_objects(objects);
    overrides.set_hardware(hardware);

    SECTION("AUTO uses detected value") {
        // bed_mesh is detected, should be available
        overrides.set_override(capability::BED_MESH, OverrideState::AUTO);
        REQUIRE(overrides.is_available(capability::BED_MESH));

        // QGL is detected, should be available
        overrides.set_override(capability::QGL, OverrideState::AUTO);
        REQUIRE(overrides.is_available(capability::QGL));

        // z_tilt is NOT detected, should NOT be available
        overrides.set_override(capability::Z_TILT, OverrideState::AUTO);
        REQUIRE_FALSE(overrides.is_available(capability::Z_TILT));

        // chamber is NOT detected, should NOT be available
        overrides.set_override(capability::CHAMBER, OverrideState::AUTO);
        REQUIRE_FALSE(overrides.is_available(capability::CHAMBER));
    }

    SECTION("ENABLE forces capability on") {
        // z_tilt is NOT detected, but ENABLE forces it on
        overrides.set_override(capability::Z_TILT, OverrideState::ENABLE);
        REQUIRE(overrides.is_available(capability::Z_TILT));

        // heat_soak is NOT detected, but ENABLE forces it on
        overrides.set_override(capability::HEAT_SOAK, OverrideState::ENABLE);
        REQUIRE(overrides.is_available(capability::HEAT_SOAK));
    }

    SECTION("DISABLE forces capability off") {
        // bed_mesh IS detected, but DISABLE forces it off
        overrides.set_override(capability::BED_MESH, OverrideState::DISABLE);
        REQUIRE_FALSE(overrides.is_available(capability::BED_MESH));

        // QGL IS detected, but DISABLE forces it off
        overrides.set_override(capability::QGL, OverrideState::DISABLE);
        REQUIRE_FALSE(overrides.is_available(capability::QGL));
    }
}

TEST_CASE("CapabilityOverrides - convenience methods", "[printer][overrides]") {
    CapabilityOverrides overrides;

    helix::PrinterDiscovery hardware;
    json objects = {"bed_mesh",
                    "quad_gantry_level",
                    "z_tilt",
                    "gcode_macro CLEAN_NOZZLE",
                    "gcode_macro HEAT_SOAK",
                    "heater_generic chamber"};
    hardware.parse_objects(objects);
    overrides.set_hardware(hardware);

    SECTION("Convenience methods work with defaults") {
        REQUIRE(overrides.has_bed_mesh());
        REQUIRE(overrides.has_qgl());
        REQUIRE(overrides.has_z_tilt());
        REQUIRE(overrides.has_nozzle_clean());
        REQUIRE(overrides.has_heat_soak());
        REQUIRE(overrides.has_chamber());
    }

    SECTION("Convenience methods respect overrides") {
        overrides.set_override(capability::BED_MESH, OverrideState::DISABLE);
        overrides.set_override(capability::QGL, OverrideState::DISABLE);

        REQUIRE_FALSE(overrides.has_bed_mesh());
        REQUIRE_FALSE(overrides.has_qgl());
        REQUIRE(overrides.has_z_tilt()); // Not overridden
    }
}

// ============================================================================
// No Capabilities Set Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - no capabilities set", "[printer][overrides]") {
    CapabilityOverrides overrides;
    // Don't call set_hardware()

    SECTION("AUTO returns false when no capabilities set") {
        overrides.set_override(capability::BED_MESH, OverrideState::AUTO);
        REQUIRE_FALSE(overrides.is_available(capability::BED_MESH));
    }

    SECTION("ENABLE still works without capabilities") {
        overrides.set_override(capability::BED_MESH, OverrideState::ENABLE);
        REQUIRE(overrides.is_available(capability::BED_MESH));
    }

    SECTION("DISABLE still works without capabilities") {
        overrides.set_override(capability::BED_MESH, OverrideState::DISABLE);
        REQUIRE_FALSE(overrides.is_available(capability::BED_MESH));
    }
}

// ============================================================================
// Summary Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - summary", "[printer][overrides]") {
    CapabilityOverrides overrides;

    SECTION("Summary shows all capabilities with no printer caps") {
        std::string summary = overrides.summary();

        REQUIRE(summary.find("bed_mesh=") != std::string::npos);
        REQUIRE(summary.find("qgl=") != std::string::npos);
        REQUIRE(summary.find("z_tilt=") != std::string::npos);
        REQUIRE(summary.find("nozzle_clean=") != std::string::npos);
        REQUIRE(summary.find("heat_soak=") != std::string::npos);
        REQUIRE(summary.find("chamber=") != std::string::npos);
        REQUIRE(summary.find("speaker=") != std::string::npos);
    }

    SECTION("Summary shows auto(Y) for detected capabilities") {
        helix::PrinterDiscovery hardware;
        json objects = {"bed_mesh"};
        hardware.parse_objects(objects);
        overrides.set_hardware(hardware);

        std::string summary = overrides.summary();
        REQUIRE(summary.find("bed_mesh=auto(Y)") != std::string::npos);
        REQUIRE(summary.find("qgl=auto(N)") != std::string::npos);
    }

    SECTION("Summary shows ENABLE/DISABLE for overrides") {
        overrides.set_override(capability::BED_MESH, OverrideState::ENABLE);
        overrides.set_override(capability::QGL, OverrideState::DISABLE);

        std::string summary = overrides.summary();
        REQUIRE(summary.find("bed_mesh=ENABLE") != std::string::npos);
        REQUIRE(summary.find("qgl=DISABLE") != std::string::npos);
    }
}

// ============================================================================
// Speaker Override Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - speaker override", "[printer][overrides]") {
    CapabilityOverrides overrides;

    SECTION("Speaker DISABLE hides sound settings") {
        overrides.set_override(capability::SPEAKER, OverrideState::DISABLE);
        REQUIRE_FALSE(overrides.has_speaker());
    }

    SECTION("Speaker ENABLE forces sound settings visible") {
        overrides.set_override(capability::SPEAKER, OverrideState::ENABLE);
        REQUIRE(overrides.has_speaker());
    }

    SECTION("Speaker AUTO with beeper detected returns true") {
        helix::PrinterDiscovery hardware;
        json objects = {"output_pin beeper"};
        hardware.parse_objects(objects);
        overrides.set_hardware(hardware);

        overrides.set_override(capability::SPEAKER, OverrideState::AUTO);
        REQUIRE(overrides.has_speaker());
    }

    SECTION("Speaker AUTO without beeper returns false (no sound backend in test)") {
        helix::PrinterDiscovery hardware;
        json objects = {"bed_mesh"};
        hardware.parse_objects(objects);
        overrides.set_hardware(hardware);

        overrides.set_override(capability::SPEAKER, OverrideState::AUTO);
        // No beeper and no sound backend in unit tests
        REQUIRE_FALSE(overrides.has_speaker());
    }
}

// ============================================================================
// Copy and Move Tests
// ============================================================================

TEST_CASE("CapabilityOverrides - copy semantics", "[printer][overrides]") {
    CapabilityOverrides original;
    original.set_override(capability::BED_MESH, OverrideState::ENABLE);
    original.set_override(capability::QGL, OverrideState::DISABLE);

    helix::PrinterDiscovery hardware;
    json objects = {"bed_mesh"};
    hardware.parse_objects(objects);
    original.set_hardware(hardware);

    SECTION("Copy constructor preserves state") {
        CapabilityOverrides copy(original);

        REQUIRE(copy.get_override(capability::BED_MESH) == OverrideState::ENABLE);
        REQUIRE(copy.get_override(capability::QGL) == OverrideState::DISABLE);
        REQUIRE(copy.is_available(capability::BED_MESH));
    }

    SECTION("Copy assignment preserves state") {
        CapabilityOverrides copy;
        copy = original;

        REQUIRE(copy.get_override(capability::BED_MESH) == OverrideState::ENABLE);
        REQUIRE(copy.get_override(capability::QGL) == OverrideState::DISABLE);
    }
}

TEST_CASE("CapabilityOverrides - heater interrupt support", "[printer][overrides]") {
    CapabilityOverrides overrides;
    // No hardware yet: nothing is supported.
    REQUIRE_FALSE(overrides.supports_heater_interrupt());

    helix::PrinterDiscovery hardware;
    overrides.set_hardware(hardware);
    REQUIRE_FALSE(overrides.supports_heater_interrupt());

    hardware.set_is_kalico(true);
    overrides.set_hardware(hardware);
    REQUIRE(overrides.supports_heater_interrupt());
}

TEST_CASE("CapabilityOverrides - MPC calibration support", "[printer][overrides]") {
    CapabilityOverrides overrides;
    REQUIRE_FALSE(overrides.supports_mpc_calibration());

    helix::PrinterDiscovery hardware;
    overrides.set_hardware(hardware);
    REQUIRE_FALSE(overrides.supports_mpc_calibration());

    hardware.set_is_kalico(true);
    overrides.set_hardware(hardware);
    REQUIRE(overrides.supports_mpc_calibration());
}

TEST_CASE("CapabilityOverrides - is_forced_on", "[capability_overrides]") {
    CapabilityOverrides overrides;

    SECTION("ENABLE on an undetected capability is forced on") {
        helix::PrinterDiscovery hardware;
        overrides.set_hardware(hardware);
        overrides.set_override(capability::QGL, OverrideState::ENABLE);
        CHECK(overrides.is_forced_on(capability::QGL));
    }

    SECTION("ENABLE on a detected capability is not") {
        helix::PrinterDiscovery hardware;
        hardware.parse_objects(json{"quad_gantry_level"});
        overrides.set_hardware(hardware);
        overrides.set_override(capability::QGL, OverrideState::ENABLE);
        CHECK_FALSE(overrides.is_forced_on(capability::QGL));
    }

    SECTION("AUTO and DISABLE are not") {
        helix::PrinterDiscovery hardware;
        overrides.set_hardware(hardware);
        overrides.set_override(capability::QGL, OverrideState::AUTO);
        CHECK_FALSE(overrides.is_forced_on(capability::QGL));
        overrides.set_override(capability::QGL, OverrideState::DISABLE);
        CHECK_FALSE(overrides.is_forced_on(capability::QGL));
    }
}
