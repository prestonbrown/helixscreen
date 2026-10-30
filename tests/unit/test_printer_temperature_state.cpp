// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui_test_utils.h"
#include "printer_temperature_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("PrinterTemperatureState: active extruder defaults to 'extruder'",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);

    REQUIRE(state.active_extruder_name() == "extruder");

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: set_active_extruder changes active name",
          "[core][temperature][active-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.set_active_extruder("extruder1");
    REQUIRE(state.active_extruder_name() == "extruder1");

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: a partial status frame keeps the fields it omits",
          "[core][temperature][partial]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    state.update_from_status({{"extruder", {{"temperature", 200.0}, {"target", 210.0}}},
                              {"heater_bed", {{"temperature", 60.0}, {"target", 65.0}}}});
    REQUIRE(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2100);
    REQUIRE(lv_subject_get_int(state.get_bed_temp_subject()) == 600);

    state.update_from_status({{"extruder", {{"temperature", 205.0}}}});
    CHECK(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 2050);
    CHECK(lv_subject_get_int(state.get_active_extruder_target_subject()) == 2100);
    CHECK(lv_subject_get_int(state.get_bed_temp_subject()) == 600);
    CHECK(lv_subject_get_int(state.get_bed_target_subject()) == 650);

    state.update_from_status(nlohmann::json::object());
    CHECK(lv_subject_get_int(state.get_active_extruder_temp_subject()) == 2050);
    CHECK(lv_subject_get_int(state.get_bed_target_subject()) == 650);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: status updates route to each extruder's own subjects",
          "[core][temperature][multi-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.update_from_status({{"extruder1", {{"temperature", 220.5}, {"target", 230.0}}}});
    CHECK(lv_subject_get_int(state.get_extruder_temp_subject("extruder1")) == 2205);
    CHECK(lv_subject_get_int(state.get_extruder_target_subject("extruder1")) == 2300);
    CHECK(lv_subject_get_int(state.get_extruder_temp_subject("extruder")) == 0);
    CHECK(lv_subject_get_int(state.get_extruder_target_subject("extruder")) == 0);

    state.update_from_status({{"extruder", {{"temperature", 200.0}, {"target", 210.0}}},
                              {"extruder1", {{"temperature", 222.0}, {"target", 231.0}}}});
    CHECK(lv_subject_get_int(state.get_extruder_temp_subject("extruder")) == 2000);
    CHECK(lv_subject_get_int(state.get_extruder_target_subject("extruder")) == 2100);
    CHECK(lv_subject_get_int(state.get_extruder_temp_subject("extruder1")) == 2220);
    CHECK(lv_subject_get_int(state.get_extruder_target_subject("extruder1")) == 2310);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: init_extruders keeps only extruder heaters",
          "[core][temperature][multi-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);

    state.init_extruders({"extruder", "extruder1", "heater_bed", "temperature_sensor chamber",
                          "extruder_stepper e1"});

    CHECK(state.extruder_count() == 2);
    CHECK(state.extruders().count("extruder") == 1);
    CHECK(state.extruders().count("extruder1") == 1);
    CHECK(state.get_extruder_temp_subject("heater_bed") == nullptr);
    CHECK(state.get_extruder_temp_subject("extruder_stepper e1") == nullptr);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: deinit drops every extruder and re-init starts fresh",
          "[core][temperature][multi-extruder]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});
    state.update_from_status({{"extruder", {{"temperature", 200.0}}}});
    REQUIRE(state.extruder_count() == 2);
    REQUIRE(lv_subject_get_int(state.get_extruder_temp_subject("extruder")) == 2000);

    state.deinit_subjects();
    CHECK(state.extruder_count() == 0);
    CHECK(state.get_extruder_temp_subject("extruder") == nullptr);
    CHECK(state.get_extruder_target_subject("extruder") == nullptr);

    // A second deinit is a no-op, not a double free.
    state.deinit_subjects();
    CHECK(state.extruder_count() == 0);

    // No reading from the previous session survives into the next one.
    state.init_subjects(false);
    state.init_extruders({"extruder"});
    REQUIRE(state.get_extruder_temp_subject("extruder") != nullptr);
    CHECK(lv_subject_get_int(state.get_extruder_temp_subject("extruder")) == 0);

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
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

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: heater power publishes as whole percent",
          "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.set_chamber_heater_name("heater_generic chamber");

    // Klipper reports duty as a 0.0-1.0 fraction on the heater object.
    nlohmann::json status = {
        {"extruder", {{"temperature", 210.0}, {"target", 210.0}, {"power", 0.42}}},
        {"heater_bed", {{"temperature", 60.0}, {"target", 60.0}, {"power", 1.0}}},
        {"heater_generic chamber", {{"temperature", 55.0}, {"target", 60.0}, {"power", 0.0}}}};
    state.update_from_status(status);

    CHECK(lv_subject_get_int(state.get_extruder_power_subject()) == 42);
    CHECK(lv_subject_get_int(state.get_bed_power_subject()) == 100);
    CHECK(lv_subject_get_int(state.get_chamber_power_subject()) == 0);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: a heater reporting no power stays unknown",
          "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    // A temperature_fan drives the chamber by speed and never reports a duty.
    state.set_chamber_heater_name("temperature_fan chamber_fan");

    nlohmann::json status = {
        {"extruder", {{"temperature", 25.0}, {"target", 0.0}}},
        {"temperature_fan chamber_fan", {{"temperature", 30.0}, {"target", 40.0}, {"speed", 0.5}}}};
    state.update_from_status(status);

    // -1 is unknown, and a readout bound to it stays hidden rather than
    // claiming the element is idle.
    CHECK(lv_subject_get_int(state.get_chamber_power_subject()) == -1);
    CHECK(lv_subject_get_int(state.get_extruder_power_subject()) == -1);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: a frame omitting power leaves the last reading",
          "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);

    state.update_from_status({{"heater_bed", {{"temperature", 60.0}, {"power", 0.75}}}});
    REQUIRE(lv_subject_get_int(state.get_bed_power_subject()) == 75);

    // Moonraker sends deltas: a frame carrying only a temperature says nothing
    // about duty, so the reading must survive rather than fall back to zero.
    state.update_from_status({{"heater_bed", {{"temperature", 60.5}}}});
    CHECK(lv_subject_get_int(state.get_bed_power_subject()) == 75);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: out-of-range duty is clamped", "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);

    state.update_from_status({{"heater_bed", {{"power", 1.4}}}});
    CHECK(lv_subject_get_int(state.get_bed_power_subject()) == 100);
    state.update_from_status({{"heater_bed", {{"power", -0.2}}}});
    CHECK(lv_subject_get_int(state.get_bed_power_subject()) == 0);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: per-extruder power reaches its own subject",
          "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    // The machine's tool is extruder; a duty frame for the OTHER head must
    // land on that head's subject without leaking into the active mirror.
    state.update_from_status({{"extruder1", {{"power", 0.73}}}});

    SubjectLifetime picked;
    REQUIRE(state.get_extruder_power_subject("extruder1", picked) != nullptr);
    CHECK(lv_subject_get_int(state.get_extruder_power_subject("extruder1", picked)) == 73);
    CHECK(lv_subject_get_int(state.get_extruder_power_subject()) == -1);
    SubjectLifetime idle;
    CHECK(lv_subject_get_int(state.get_extruder_power_subject("extruder", idle)) == -1);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: a toolchange resyncs active power",
          "[core][temperature][power]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    state.update_from_status({{"extruder", {{"power", 1.0}}}, {"extruder1", {{"power", 0.0}}}});
    REQUIRE(lv_subject_get_int(state.get_extruder_power_subject()) == 100);

    // Moonraker deltas omit unchanged fields, so an idle new tool may never
    // carry a power frame after the change: the mirror must take extruder1's
    // last known duty at the toolchange, not keep the old tool's 100.
    state.set_active_extruder("extruder1");
    CHECK(lv_subject_get_int(state.get_extruder_power_subject()) == 0);

    state.deinit_subjects();
}

// Discovery re-runs on every klippy ready. A tool that is still there keeps its
// subjects, so whatever is bound to them keeps receiving readings.
TEST_CASE("PrinterTemperatureState: rediscovery keeps the subjects of unchanged extruders",
          "[core][temperature][rediscovery]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder"});

    SubjectLifetime lt;
    lv_subject_t* temp = state.get_extruder_temp_subject("extruder", lt);
    REQUIRE(temp != nullptr);
    int last = -1;
    lv_observer_t* obs = lv_subject_add_observer(
        temp,
        [](lv_observer_t* o, lv_subject_t* s) {
            *static_cast<int*>(lv_observer_get_user_data(o)) = lv_subject_get_int(s);
        },
        &last);
    const int version = lv_subject_get_int(state.get_extruder_version_subject());

    state.init_extruders({"extruder"});

    REQUIRE(*lt);
    state.update_from_status({{"extruder", {{"temperature", 100.0}}}});
    CHECK(last == 1000);
    SubjectLifetime again;
    CHECK(state.get_extruder_temp_subject("extruder", again) == temp);
    CHECK(lv_subject_get_int(state.get_extruder_version_subject()) == version);

    lv_observer_remove(obs);
    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: rediscovery frees removed and adds new extruders",
          "[core][temperature][rediscovery]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    SubjectLifetime kept, removed;
    lv_subject_t* kept_subj = state.get_extruder_temp_subject("extruder", kept);
    REQUIRE(state.get_extruder_temp_subject("extruder1", removed) != nullptr);
    REQUIRE(state.extruders().at("extruder").display_name == "Nozzle 1");
    const int version = lv_subject_get_int(state.get_extruder_version_subject());

    state.init_extruders({"extruder2", "extruder"});

    CHECK_FALSE(*removed);
    CHECK(*kept);
    SubjectLifetime again;
    CHECK(state.get_extruder_temp_subject("extruder", again) == kept_subj);
    CHECK(state.get_extruder_temp_subject("extruder1") == nullptr);
    CHECK(state.get_extruder_temp_subject("extruder2") != nullptr);
    CHECK(state.extruders().at("extruder").display_name == "Nozzle 1");
    CHECK(state.extruders().at("extruder2").display_name == "Nozzle 2");
    CHECK(lv_subject_get_int(state.get_extruder_version_subject()) == version + 1);

    // Dropping to one tool relabels the survivor without the number.
    state.init_extruders({"extruder"});
    CHECK(state.extruders().at("extruder").display_name == "Nozzle");
    CHECK(state.get_extruder_temp_subject("extruder", again) == kept_subj);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: pressure advance is tracked per extruder and mirrored "
          "for the active one",
          "[core][temperature][pa_tune]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    lv_subject_t* active = state.get_active_extruder_pressure_advance_subject();
    CHECK(lv_subject_get_int(active) == -1);
    CHECK_FALSE(state.extruder_pressure_advance("extruder"));

    state.update_from_status({{"extruder", {{"pressure_advance", 0.042}, {"smooth_time", 0.04}}},
                              {"extruder1", {{"pressure_advance", 0.5}}}});
    CHECK(lv_subject_get_int(active) == 420);
    REQUIRE(state.extruder_pressure_advance("extruder1"));
    CHECK(*state.extruder_pressure_advance("extruder1") == Catch::Approx(0.5f));
    CHECK_FALSE(state.extruder_pressure_advance("extruder7"));

    // A frame without the field leaves the value alone.
    state.update_from_status({{"extruder", {{"temperature", 200.0}}}});
    CHECK(lv_subject_get_int(active) == 420);

    // Switching tools shows the new tool's K.
    state.set_active_extruder("extruder1");
    CHECK(lv_subject_get_int(active) == 5000);

    state.deinit_subjects();
}

TEST_CASE("PrinterTemperatureState: the fitted nozzle an extruder reports is kept per extruder",
          "[core][temperature][plugin_fitcheck]") {
    lv_init_safe();
    PrinterTemperatureState state;
    state.init_subjects(false);
    state.init_extruders({"extruder", "extruder1"});

    const auto& info = [&state](const char* name) -> const ExtruderInfo& {
        return state.extruders().at(name);
    };
    CHECK(info("extruder").nozzle_diameter == 0.0);
    CHECK(info("extruder").nozzle_volume_type.empty());

    state.update_from_status(
        {{"extruder", {{"nozzle_diameter", 0.2}, {"nozzle_volume_type", "standard"}}},
         {"extruder1", {{"nozzle_diameter", 0.6}, {"nozzle_volume_type", "high_flow"}}}});
    CHECK(info("extruder").nozzle_diameter == Catch::Approx(0.2));
    CHECK(info("extruder").nozzle_volume_type == "standard");
    CHECK(info("extruder1").nozzle_diameter == Catch::Approx(0.6));
    CHECK(info("extruder1").nozzle_volume_type == "high_flow");

    // Stock Klipper answers null for both; a frame without them leaves them alone.
    state.update_from_status(
        {{"extruder", {{"nozzle_diameter", nullptr}, {"nozzle_volume_type", nullptr}}},
         {"extruder1", {{"temperature", 210.0}}}});
    CHECK(info("extruder").nozzle_diameter == Catch::Approx(0.2));
    CHECK(info("extruder").nozzle_volume_type == "standard");
    CHECK(info("extruder1").nozzle_diameter == Catch::Approx(0.6));

    // A diameter that is not a positive number says nothing about the nozzle.
    state.update_from_status(
        {{"extruder", {{"nozzle_diameter", 0.0}}}, {"extruder1", {{"nozzle_diameter", "0.4"}}}});
    CHECK(info("extruder").nozzle_diameter == Catch::Approx(0.2));
    CHECK(info("extruder1").nozzle_diameter == Catch::Approx(0.6));
    state.update_from_status({{"extruder", {{"nozzle_diameter", -0.4}}}});
    CHECK(info("extruder").nozzle_diameter == Catch::Approx(0.2));

    state.deinit_subjects();
}
