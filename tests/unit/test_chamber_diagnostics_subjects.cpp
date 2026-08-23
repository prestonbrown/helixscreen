// tests/unit/test_chamber_diagnostics_subjects.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 4 of the chamber-heater backend abstraction (issue #1290): backend
// status frames flow into capability-named diagnostics subjects in
// PrinterTemperatureState, and PrinterCapabilitiesState carries the
// printer_has_chamber_heater_diagnostics / _filter_fan gates.
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "settings_manager.h"

#include <lvgl/src/others/translation/lv_translation.h>

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterCapabilitiesState;
using helix::PrinterTemperatureState;

namespace {

/// Nominal faulted dragonbreath frame (live schema, issue #1290): latched
/// fault with reason, PTC element temp at 106.2°C, filter fan purging at
/// 100%, mode "off" (not externally controlled — heater inactive).
nlohmann::json faulted_diagnostics_status() {
    return nlohmann::json::parse(R"({
      "heater_generic dragonbreath": {"temperature": 25.5, "target": 30.0},
      "dragonbreath": {"fault": true, "inhibited": false, "fault_reason": "ptc_overtemp",
        "ptc_temp": 106.2, "fan_percent": 100, "fan_reason": "purge",
        "mode": "off", "source": "device", "lease_owned": false},
      "output_pin dragonbreath_filter": {"value": 1.0}})");
}

} // namespace

TEST_CASE("dragonbreath status drives diagnostics subjects", "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_heater_name("heater_generic dragonbreath");
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_diagnostics_status());

    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_inhibited_subject()) == 0);
    // The UI-facing subject carries the TRANSLATED phrase for the classified
    // kind; the vendor code ("ptc_overtemp") is log-only and has no subject.
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
          std::string(lv_tr("Heater over-temperature")));
    // 106.2°C → canonical decimal-drop rule (whole degrees at/above 100)
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C");
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
    // Capabilities are set by PrinterState::set_hardware in production; unit-level here:
    CHECK(ts.chamber_diagnostics_object() == "dragonbreath");
}

TEST_CASE("fault reason kinds map to translated phrases at the subject border",
          "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    auto fault_with_reason = [&ts](const char* reason) {
        ts.update_from_status(
            {{"dragonbreath", {{"fault", true}, {"fault_reason", reason}, {"ptc_temp", 24.9}}}});
    };
    auto text = [&ts]() {
        return std::string(
            lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject()));
    };

    fault_with_reason("ptc_sensor_fault");
    CHECK(text() == std::string(lv_tr("Heater sensor fault")));

    fault_with_reason("comms_timeout");
    CHECK(text() == std::string(lv_tr("Heater connection lost")));

    fault_with_reason("mystery_code");
    CHECK(text() == std::string(lv_tr("Heater fault")));

    // Reason clears -> empty phrase.
    ts.update_from_status(
        {{"dragonbreath", {{"fault", false}, {"fault_reason", nullptr}, {"ptc_temp", 24.9}}}});
    CHECK(text().empty());
}

TEST_CASE("absent diagnostics objects in a delta frame keep last values", "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_heater_name("heater_generic dragonbreath");
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_diagnostics_status());
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    REQUIRE(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);

    // Moonraker status updates are deltas. A frame touching only the heater
    // carries no news about diagnostics or the filter pin — the subjects keep
    // their last values (they do NOT reset to defaults).
    ts.update_from_status({{"heater_generic dragonbreath", {{"temperature", 26.1}}}});

    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
          std::string(lv_tr("Heater over-temperature")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "106°C");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
}

TEST_CASE("filter fan pin maps output_pin value to on/off", "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    // Unknown until the first pin frame arrives.
    REQUIRE(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == -1);

    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 0.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);

    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 1.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
}

TEST_CASE("diagnostics objects are ignored without a configured source", "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    // set_chamber_diagnostics_source() intentionally NOT called — capability off.

    ts.update_from_status(faulted_diagnostics_status());

    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "--");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == -1);
    CHECK(ts.chamber_diagnostics_object().empty());
}

TEST_CASE("chamber diagnostics subjects are XML-registered", "[chamber][xml][structural]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(true); // register_xml=true: full production path

    for (const char* name :
         {"chamber_heater_fault", "chamber_heater_inhibited", "chamber_heater_fault_reason_text",
          "chamber_heater_element_temp_text", "chamber_filter_fan_percent_text",
          "chamber_filter_fan_on", "chamber_filter_fan_on_text", "chamber_filter_fan_icon"}) {
        CAPTURE(name);
        REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    ts.deinit_subjects();

    PrinterCapabilitiesState caps;
    caps.init_subjects(true);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_chamber_heater_diagnostics") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_chamber_filter_fan") != nullptr);
    caps.deinit_subjects();
}

TEST_CASE("chamber diagnostics capability setters round-trip", "[chamber][capabilities]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);

    caps.set_has_chamber_heater_diagnostics(true);
    caps.set_has_chamber_filter_fan(true);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 1);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 1);

    caps.set_has_chamber_heater_diagnostics(false);
    caps.set_has_chamber_filter_fan(false);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);
}

TEST_CASE("chamber required_status_objects lists only non-empty surfaces", "[chamber][subjects]") {
    using helix::chamber::required_status_objects;

    auto both = required_status_objects("dragonbreath", "output_pin dragonbreath_filter");
    REQUIRE(both.size() == 2);
    CHECK(both[0] == "dragonbreath");
    CHECK(both[1] == "output_pin dragonbreath_filter");

    CHECK(required_status_objects("", "").empty());
    CHECK(required_status_objects("dragonbreath", "").size() == 1);
}

// PrinterState::set_hardware gating (issue #1290): backend diagnostics attach
// only while the RESOLVED chamber heater is discovery's own pick — a manual
// override to a different heater (or "none") detaches the source and clears
// the capabilities. Pattern per test_printer_state.cpp set_hardware cases.
TEST_CASE("set_hardware wires diagnostics only when the resolved heater is the discovery pick",
          "[chamber][subjects][state][hardware]") {
    lv_init_safe();
    helix::PrinterState& state = get_printer_state();
    helix::PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment("auto");

    auto& ts = helix::PrinterStateTestAccess::get_temperature_state(state);
    auto& caps = helix::PrinterStateTestAccess::get_capabilities_state(state);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = {"heater_generic dragonbreath", "extruder", "heater_bed"};
    hw.parse_objects(objects);
    REQUIRE(hw.chamber_heater_name() == "heater_generic dragonbreath");
    REQUIRE(hw.chamber_heater_backend_id() == "dragonbreath");
    REQUIRE(hw.chamber_diagnostics_object() == "dragonbreath");

    auto restore_settings = [&settings]() {
        settings.set_chamber_heater_assignment("auto");
        settings.set_chamber_sensor_assignment("auto");
    };

    SECTION("auto mode wires diagnostics for the discovered backend heater") {
        settings.set_chamber_heater_assignment("auto");
        state.set_hardware(hw);

        CHECK(ts.chamber_diagnostics_object() == "dragonbreath");
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 1);
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 1);

        // End-to-end: a diagnostics frame through the full status path lands.
        state.update_from_status(faulted_diagnostics_status());
        CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
        CHECK(std::string(lv_subject_get_string(
                  ts.get_chamber_heater_element_temp_text_subject())) == "106°C");
    }

    SECTION("manual override to a different heater detaches diagnostics") {
        settings.set_chamber_heater_assignment("heater_generic chamber");
        state.set_hardware(hw);

        CHECK(ts.chamber_heater_name() == "heater_generic chamber");
        CHECK(ts.chamber_diagnostics_object().empty());
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);

        // The frame is ignored — subjects keep their defaults.
        state.update_from_status(faulted_diagnostics_status());
        CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
        CHECK(std::string(lv_subject_get_string(
                  ts.get_chamber_heater_element_temp_text_subject())) == "--");
        CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == -1);
    }

    SECTION("'none' override detaches diagnostics") {
        settings.set_chamber_heater_assignment("none");
        state.set_hardware(hw);

        CHECK(ts.chamber_heater_name().empty());
        CHECK(ts.chamber_diagnostics_object().empty());
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);

        state.update_from_status(faulted_diagnostics_status());
        CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
        CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == -1);
    }

    restore_settings();
}
