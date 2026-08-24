// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui_test_utils.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test helper for accessing private members
class PrinterTemperatureStateTestAccess {
  public:
    static void reset(PrinterTemperatureState& state) {
        state.deinit_subjects();
    }
};

TEST_CASE("PrinterTemperatureState: active extruder defaults to 'extruder'",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);

    REQUIRE(state.active_extruder_name() == "extruder");

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: set_active_extruder changes active name",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.set_active_extruder("extruder1");
    REQUIRE(state.active_extruder_name() == "extruder1");

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: set_active_extruder syncs current values",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    // Set extruder1's temperature via per-extruder subjects
    nlohmann::json status = {{"extruder1", {{"temperature", 220.5}, {"target", 230.0}}}};
    state.update_from_status(status);

    // Active subjects should still show "extruder" values (0) since that's the default active
    REQUIRE(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 0);

    // Now switch active to extruder1
    state.set_active_extruder("extruder1");

    // Active subjects should now mirror extruder1's values
    REQUIRE(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 2205);
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2300);

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: update_from_status updates active subjects",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});
    state.set_active_extruder("extruder1");

    // Update with extruder1 data - should reflect in active subjects
    nlohmann::json status = {{"extruder1", {{"temperature", 195.3}, {"target", 200.0}}}};
    state.update_from_status(status);

    REQUIRE(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 1953);
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2000);

    // extruder data should NOT update active (since active is extruder1)
    nlohmann::json status2 = {{"extruder", {{"temperature", 100.0}, {"target", 110.0}}}};
    state.update_from_status(status2);

    // Active subjects should still show extruder1's values
    REQUIRE(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 1953);
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2000);

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: unknown extruder name stays on previous",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.set_active_extruder("extruder1");
    REQUIRE(state.active_extruder_name() == "extruder1");

    // Unknown name should not change active
    state.set_active_extruder("extruder99");
    REQUIRE(state.active_extruder_name() == "extruder1");

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: default active works with single extruder",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    // Should default to "extruder" and update active subjects
    nlohmann::json status = {{"extruder", {{"temperature", 205.0}, {"target", 210.0}}}};
    state.update_from_status(status);

    REQUIRE(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 2050);
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2100);

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: load latch survives cooldown to 0",
          "[core][temperature][swap_preheat]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    // Heat to 250, then let the target cool back to 0.
    state.update_from_status({{"extruder", {{"temperature", 248.0}, {"target", 250.0}}}});
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    state.update_from_status({{"extruder", {{"temperature", 40.0}, {"target", 0.0}}}});

    // Live target is 0, but the latch holds the last non-zero value for purge.
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 0);
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: latch tracks the most recent non-zero target",
          "[core][temperature][swap_preheat]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    state.update_from_status({{"extruder", {{"target", 250.0}}}});
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    // A newer non-zero target replaces the latch.
    state.update_from_status({{"extruder", {{"target", 210.0}}}});
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(210.0));

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: clear_load_latch resets the held target",
          "[core][temperature][swap_preheat]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    state.update_from_status({{"extruder", {{"target", 250.0}}}});
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    // Unload clears the latch (active extruder, empty name).
    state.clear_load_latch();
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(0.0));

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: latch is per-extruder and honors the active one",
          "[core][temperature][swap_preheat]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.update_from_status(
        {{"extruder", {{"target", 250.0}}}, {"extruder1", {{"target", 210.0}}}});

    // Active defaults to "extruder" → its latch.
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(250.0));

    state.set_active_extruder("extruder1");
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(210.0));

    // Clearing a named extruder leaves the other latch intact.
    state.clear_load_latch("extruder");
    state.set_active_extruder("extruder");
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(0.0));
    state.set_active_extruder("extruder1");
    REQUIRE(state.get_active_extruder_last_nonzero_target() == Catch::Approx(210.0));

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: single extruder display_name is 'Nozzle'",
          "[core][temperature][display-name]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    const auto& exts = state.extruders();
    auto it = exts.find("extruder");
    REQUIRE(it != exts.end());
    REQUIRE(it->second.display_name == "Nozzle");

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: multi-extruder display_name is 'Nozzle N'",
          "[core][temperature][display-name]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});

    const auto& exts = state.extruders();
    REQUIRE(exts.find("extruder")->second.display_name == "Nozzle 1");
    REQUIRE(exts.find("extruder1")->second.display_name == "Nozzle 2");
    REQUIRE(exts.find("extruder2")->second.display_name == "Nozzle 3");
    REQUIRE(exts.find("extruder3")->second.display_name == "Nozzle 4");

    PrinterTemperatureStateTestAccess::reset(state);
}

TEST_CASE("PrinterTemperatureState: extruder names are sorted before labeling",
          "[core][temperature][display-name]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    // Klipper isn't guaranteed to return heaters in sorted order. Labels must
    // still align with lexical index — extruder=1, extruder1=2, etc.
    state.init_extruders({"extruder2", "extruder", "extruder3", "extruder1"});

    const auto& exts = state.extruders();
    REQUIRE(exts.find("extruder")->second.display_name == "Nozzle 1");
    REQUIRE(exts.find("extruder1")->second.display_name == "Nozzle 2");
    REQUIRE(exts.find("extruder2")->second.display_name == "Nozzle 3");
    REQUIRE(exts.find("extruder3")->second.display_name == "Nozzle 4");

    PrinterTemperatureStateTestAccess::reset(state);
}
