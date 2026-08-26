// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_clean_capability.cpp
 * @brief The nozzle_clean capability must see the user's own macro assignment (#1354)
 *
 * The capability used to be resolved purely from macro-NAME pattern detection
 * (PrinterDiscovery::has_nozzle_clean_macro()), so a printer whose nozzle-clean
 * macro is called something we do not recognise reported the capability false
 * even after the user pointed /standard_macros/clean_nozzle straight at it. The
 * Controls-panel button worked; the capability chip and has_any_preprint_options
 * did not.
 *
 * The second half of the bug was two hand-maintained name tables — one in
 * PrinterDiscovery, one in StandardMacros — that had already drifted apart.
 * They are now one table in include/macro_patterns.h.
 */

#include "../helix_test_fixture.h"
#include "capability_overrides.h"
#include "config.h"
#include "macro_patterns.h"
#include "printer_discovery.h"
#include "standard_macros.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/**
 * Config sandboxing comes from HelixTestFixture (reset_config_singleton() gives
 * every test an empty, temp-path Config). StandardMacros is a process singleton
 * whose configured_macro survives reset() by design — it is user config — so the
 * destructor clears it through the same config path init() reads, rather than
 * leaving "CLEAN" latched for every later test in the shard.
 */
class NozzleCleanCapabilityFixture : public HelixTestFixture {
  public:
    NozzleCleanCapabilityFixture() {
        assign_clean_nozzle("");
    }

    ~NozzleCleanCapabilityFixture() override {
        assign_clean_nozzle("");
        helix::PrinterDiscovery empty;
        StandardMacros::instance().init(empty);
        StandardMacros::instance().reset();
    }

    /// Write the user's slot assignment the way Settings > Macro Buttons does.
    static void assign_clean_nozzle(const std::string& macro) {
        helix::Config* cfg = helix::Config::get_instance();
        REQUIRE(cfg != nullptr);
        cfg->set<std::string>("/standard_macros/clean_nozzle", macro);
    }

    /// Build a discovery from a list of Klipper object names.
    static helix::PrinterDiscovery discover(const std::vector<std::string>& objects) {
        helix::PrinterDiscovery hardware;
        json list = json::array();
        for (const auto& name : objects) {
            list.push_back(name);
        }
        hardware.parse_objects(list);
        return hardware;
    }

    /// Run the production resolution order: StandardMacros first, then capabilities.
    static bool resolve_nozzle_clean(const helix::PrinterDiscovery& hardware) {
        StandardMacros::instance().init(hardware);
        CapabilityOverrides overrides;
        overrides.set_hardware(hardware);
        return overrides.has_nozzle_clean();
    }
};

} // namespace

// ============================================================================
// Capability resolution
// ============================================================================

TEST_CASE_METHOD(NozzleCleanCapabilityFixture,
                 "nozzle_clean - a recognised macro name still grants the capability",
                 "[1354][printer][overrides][standard_macros]") {
    // No user assignment at all: the pattern-detection path must keep working.
    auto hardware = discover({"extruder", "heater_bed", "gcode_macro CLEAN_NOZZLE"});

    REQUIRE(hardware.has_nozzle_clean_macro());
    REQUIRE(resolve_nozzle_clean(hardware));
}

TEST_CASE_METHOD(NozzleCleanCapabilityFixture,
                 "nozzle_clean - a user-assigned macro the printer defines grants it",
                 "[1354][printer][overrides][standard_macros]") {
    // Bundle 6QWNVZY5: the macro is called CLEAN, which matches no pattern.
    auto hardware = discover({"extruder", "heater_bed", "gcode_macro CLEAN"});

    // Pre-condition: detection alone says no. Without this the test would pass
    // for the wrong reason if CLEAN ever entered the shared pattern table.
    REQUIRE_FALSE(hardware.has_nozzle_clean_macro());

    assign_clean_nozzle("CLEAN");

    REQUIRE(resolve_nozzle_clean(hardware));

    // And the slot really resolved to the user's macro, not to a fallback.
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::CleanNozzle).get_macro() == "CLEAN");
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::CleanNozzle).get_source() ==
            MacroSource::CONFIGURED);
}

TEST_CASE_METHOD(NozzleCleanCapabilityFixture,
                 "nozzle_clean - an assignment the printer cannot resolve does NOT grant it",
                 "[1354][printer][overrides][standard_macros]") {
    // Presets seed slot assignments from a template machine, so a name the
    // connected printer never defines is a real, common state. Granting the
    // capability off it would advertise a button whose only feedback is
    // "Unknown command" from Klipper.
    auto hardware = discover({"extruder", "heater_bed", "gcode_macro SOMETHING_ELSE"});

    assign_clean_nozzle("CLEAN");

    REQUIRE_FALSE(hardware.has_nozzle_clean_macro());
    REQUIRE_FALSE(resolve_nozzle_clean(hardware));

    // The demotion path is what makes it false: init() moved the unresolvable
    // name into missing_macro.
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::CleanNozzle).has_missing_macro());
    REQUIRE_FALSE(
        StandardMacros::instance().has_configured_macro(StandardMacroSlot::CleanNozzle, hardware));
}

TEST_CASE_METHOD(NozzleCleanCapabilityFixture,
                 "nozzle_clean - a DISABLE override still wins over a user assignment",
                 "[1354][printer][overrides][standard_macros]") {
    auto hardware = discover({"extruder", "gcode_macro CLEAN"});
    assign_clean_nozzle("CLEAN");
    StandardMacros::instance().init(hardware);

    CapabilityOverrides overrides;
    overrides.set_hardware(hardware);
    REQUIRE(overrides.has_nozzle_clean());

    overrides.set_override(capability::NOZZLE_CLEAN, OverrideState::DISABLE);
    REQUIRE_FALSE(overrides.has_nozzle_clean());
}

// ============================================================================
// One shared name table
// ============================================================================

TEST_CASE_METHOD(NozzleCleanCapabilityFixture,
                 "nozzle_clean - both consumers read the same name table",
                 "[1354][printer][overrides][standard_macros]") {
    const auto& patterns = helix::macro_patterns::clean_nozzle();
    REQUIRE_FALSE(patterns.empty());

    // The union of the two lists that used to be maintained separately.
    // CLEAR_NOZZLE was StandardMacros-only; PURGE_NOZZLE and NOZZLE_CLEAN were
    // PrinterDiscovery-only. Losing any of them re-opens the drift.
    for (const char* name : {"CLEAN_NOZZLE", "NOZZLE_WIPE", "WIPE_NOZZLE", "CLEAR_NOZZLE",
                             "PURGE_NOZZLE", "NOZZLE_CLEAN"}) {
        INFO("expected in shared table: " << name);
        REQUIRE(std::find(patterns.begin(), patterns.end(), std::string(name)) != patterns.end());
    }

    // Every name in the shared table must satisfy BOTH consumers. Re-forking
    // either list shows up here as a name one of them no longer accepts.
    for (const auto& name : patterns) {
        INFO("shared pattern: " << name);

        auto hardware = discover({"extruder", "gcode_macro " + name});

        // Consumer 1: PrinterDiscovery's cached scan, which backs the capability.
        REQUIRE(hardware.has_nozzle_clean_macro());
        REQUIRE(hardware.nozzle_clean_macro() == name);

        // Consumer 2: StandardMacros' CleanNozzle slot detection.
        StandardMacros::instance().init(hardware);
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::CleanNozzle).detected_macro ==
                name);
    }
}
