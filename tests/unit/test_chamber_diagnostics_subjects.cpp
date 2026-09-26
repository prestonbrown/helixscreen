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

// A delta frame carries only changed fields, so an absent field is not a
// report of absence. The frame after a fault latch mentions only ptc_temp
// (the one field that moves every poll) and must leave the latched fault,
// its reason, and the reported fan speed exactly where they were.
TEST_CASE("a ptc_temp-only delta keeps the latched fault, reason, and fan percent",
          "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"fault": true, "inhibited": true, "fault_reason": "ptc_overtemp",
        "ptc_temp": 80.0, "fan_percent": 100, "fan_reason": "thermal_purge",
        "mode": "off", "source": "device", "lease_owned": false}})"));
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_inhibited_subject()) == 1);
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
            std::string(lv_tr("Heater over-temperature")));
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
            "100%");
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
            "80.0°C");

    ts.update_from_status({{"dragonbreath", {{"ptc_temp", 81.0}}}});

    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_inhibited_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
          std::string(lv_tr("Heater over-temperature")));
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_device_driven_subject()) == 1);
    // The delta was processed: the one field it carried did land.
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "81.0°C");
}

// A delta that toggles only the fan carries no ptc_temp, so recognition of a
// dragonbreath frame cannot hinge on that one field: the frame must parse and
// the reported speed must land.
TEST_CASE("a fan-percent-only delta updates the reported speed", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"fault": false, "fault_reason": null, "ptc_temp": 40.0,
        "fan_percent": 0, "fan_reason": "off", "mode": "off", "source": "klipper",
        "lease_owned": false}})"));
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
            "0%");
    REQUIRE(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);

    ts.update_from_status({{"dragonbreath", {{"fan_percent", 100}}}});
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
}

// Absence and false are different reports: a frame that carries fault: false
// is the device clearing the latch, and must clear it.
TEST_CASE("an explicit fault false delta clears a latched fault", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_diagnostics_status());
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);

    ts.update_from_status({{"dragonbreath", {{"fault", false}, {"ptc_temp", 24.9}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
}

// fault_reason: null in a frame is the device ANSWERING "no reason", not the
// field being absent: it engages as an empty reason and clears the text.
TEST_CASE("a fault_reason null delta clears the reason text", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status(faulted_diagnostics_status());
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject())) ==
            std::string(lv_tr("Heater over-temperature")));

    ts.update_from_status(
        {{"dragonbreath", {{"fault", true}, {"fault_reason", nullptr}, {"ptc_temp", 106.3}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject()))
              .empty());
}

// The pin is a REQUEST; the device also runs the filter fan on its own while
// heating and while purging residual element heat (measured on the rig,
// issue #1290). The running-state subjects must follow the reported fan
// speed, not the pin, or the card shows "100%" beside a
// switch that is off.
TEST_CASE("device-driven filter fan keeps the switch on the reported speed",
          "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    // Device heating at target with our pin still 0: the fan runs on the
    // device's own initiative.
    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"fault": false, "inhibited": false, "ptc_temp": 41.0,
        "fan_percent": 100, "fan_reason": "heater",
        "mode": "power_on", "source": "klipper", "lease_owned": true},
      "output_pin dragonbreath_filter": {"value": 0.0}})"));

    CHECK(std::string(lv_subject_get_string(ts.get_chamber_filter_fan_percent_text_subject())) ==
          "100%");
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 1);
    // The pin stays our request; the device-driven flag is what the card's
    // Device badge and toggle-disable bind.
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_device_driven_subject()) == 1);

    // The device reporting the fan stopped is what flips the running pair.
    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"fault": false, "inhibited": false, "ptc_temp": 39.4,
        "fan_percent": 0, "fan_reason": "off",
        "mode": "off", "source": "klipper", "lease_owned": false},
      "output_pin dragonbreath_filter": {"value": 0.0}})"));
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_device_driven_subject()) == 0);
}

// Comms health: the appliance reports its own radio link. An engaged false is
// the device saying it is unreachable — that drives chamber_heater_offline.
// Unknown (absent key, or a backend with no link report at all) is NOT
// offline.
TEST_CASE("connected reports drive chamber_heater_offline", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);

    // One report is a missed poll, not an outage: the run has to build.
    const nlohmann::json down{
        {"dragonbreath", {{"connected", false}, {"protocol_error", nullptr}}}};
    for (int i = 0; i < 3; ++i) {
        ts.update_from_status(down);
        CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    }
    ts.update_from_status(down);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 1);
    // protocol_error: null is inert — no UI subject moves on it.
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject()))
              .empty());

    ts.update_from_status({{"dragonbreath", {{"connected", true}, {"ptc_temp", 24.9}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
}

// Delta rule applied to the link field: after an offline report, a frame that
// does not mention connected carries no news about it, so the heater stays
// offline — while the field the frame DOES carry (ptc_temp) lands, proving
// the frame was parsed rather than dropped.
TEST_CASE("a delta without connected keeps the offline state", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    const nlohmann::json down =
        nlohmann::json::parse(R"({"dragonbreath": {"fault": false, "fault_reason": null,
      "ptc_temp": 40.0, "fan_percent": 0, "fan_reason": "off", "connected": false}})");
    for (int i = 0; i < 4; ++i) {
        ts.update_from_status(down);
    }
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 1);
    REQUIRE(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
            "40.0°C");

    ts.update_from_status({{"dragonbreath", {{"ptc_temp", 41.0}}}});

    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 1);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "41.0°C");
}

// A backend with no link report (a plain heater_generic chamber) must leave
// the offline subject at 0 forever, even when some other object in the frame
// carries connected: false — unknown is not offline.
TEST_CASE("a generic chamber heater never reports offline", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_heater_name("heater_generic chamber");
    ts.set_chamber_diagnostics_source("generic", "", "");

    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);

    ts.update_from_status({{"heater_generic chamber", {{"temperature", 24.2}, {"target", 0.0}}},
                           {"dragonbreath", {{"connected", false}}}});
    // The frame was processed — the heater branch landed its temperature —
    // yet no link report was read off the stray appliance-shaped object.
    CHECK(lv_subject_get_int(ts.get_chamber_temp_subject()) == 242);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
}

// The vendor protocol_error string dies at the subject border: the frame is
// parsed (its other fields land) and the error reaches no UI subject.
TEST_CASE("protocol_error never reaches a UI subject", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status({{"dragonbreath",
                            {{"protocol_error", "frame_crc"},
                             {"connected", true},
                             {"ptc_temp", 24.9},
                             {"fan_percent", 0}}}});

    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_fault_reason_text_subject()))
              .empty());
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "24.9°C");
}

TEST_CASE("filter fan pin maps output_pin value to on/off", "[chamber][subjects]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    // Unknown until the first pin frame arrives.
    REQUIRE(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == -1);
    REQUIRE(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == -1);

    // No diagnostics frames here: a backend with a pin but no reported speed
    // drives the running state from the pin.
    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 0.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_on_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_device_driven_subject()) == 0);

    ts.update_from_status({{"output_pin dragonbreath_filter", {{"value", 1.0}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_filter_fan_requested_subject()) == 1);
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
         {"chamber_heater_fault", "chamber_heater_inhibited", "chamber_heater_offline",
          "chamber_heater_fault_reason_text", "chamber_heater_element_temp_text",
          "chamber_filter_fan_percent_text", "chamber_filter_fan_on",
          "chamber_filter_fan_requested", "chamber_filter_fan_device_driven"}) {
        CAPTURE(name);
        REQUIRE(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    ts.deinit_subjects();

    PrinterCapabilitiesState caps;
    caps.init_subjects(true);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_chamber_heater_diagnostics") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_chamber_filter_fan") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "printer_has_chamber_element_temp") != nullptr);
    caps.deinit_subjects();
}

TEST_CASE("chamber diagnostics capability setters round-trip", "[chamber][capabilities]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_element_temp_subject()) == 0);

    caps.set_has_chamber_heater_diagnostics(true);
    caps.set_has_chamber_filter_fan(true);
    caps.set_has_chamber_element_temp(true);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 1);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 1);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_element_temp_subject()) == 1);

    caps.set_has_chamber_heater_diagnostics(false);
    caps.set_has_chamber_filter_fan(false);
    caps.set_has_chamber_element_temp(false);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_element_temp_subject()) == 0);
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
    nlohmann::json objects = {"heater_generic dragonbreath", "heater_generic ptc_heater",
                              "extruder", "heater_bed"};
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
        CHECK(lv_subject_get_int(caps.get_printer_has_chamber_element_temp_subject()) == 1);

        // End-to-end: a diagnostics frame through the full status path lands.
        state.update_from_status(faulted_diagnostics_status());
        CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 1);
        CHECK(std::string(lv_subject_get_string(
                  ts.get_chamber_heater_element_temp_text_subject())) == "106°C");
    }

    SECTION("manual override to a different heater detaches diagnostics") {
        settings.set_chamber_heater_assignment("heater_generic ptc_heater");
        state.set_hardware(hw);

        CHECK(ts.chamber_heater_name() == "heater_generic ptc_heater");
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

// The stock Panda Breath binding (issue #1290) publishes link state and which
// control loop holds the heater, and nothing else the card draws: no fault, no
// filtration speed, no element temperature. Discovery must wire the diagnostics
// object while leaving both readout capabilities off, or the card renders two
// rows that can only ever say "--".
TEST_CASE("set_hardware wires the stock panda_breath surfaces and no others",
          "[chamber][subjects][state][hardware][1290]") {
    lv_init_safe();
    helix::PrinterState& state = get_printer_state();
    helix::PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment("auto");
    settings.set_chamber_heater_assignment("auto");

    auto& ts = helix::PrinterStateTestAccess::get_temperature_state(state);
    auto& caps = helix::PrinterStateTestAccess::get_capabilities_state(state);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = {"heater_generic panda_breath", "panda_breath", "extruder",
                              "heater_bed"};
    hw.parse_objects(objects);
    REQUIRE(hw.chamber_heater_name() == "heater_generic panda_breath");
    REQUIRE(hw.chamber_heater_backend_id() == "panda_breath");
    REQUIRE(hw.chamber_diagnostics_object() == "panda_breath");
    CHECK(hw.chamber_filter_fan_pin().empty());

    state.set_hardware(hw);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_heater_diagnostics_subject()) == 1);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_filter_fan_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_chamber_element_temp_subject()) == 0);

    // The appliance holding its own auto target while our target reads 0 — the
    // state the rig sits in at rest.
    state.update_from_status(nlohmann::json::parse(R"({
      "panda_breath": {"temperature": 23.0, "target": 0.0, "connected": true,
                       "work_mode": 1, "work_on": true, "device_target": 60.0,
                       "auto_enabled": true}})"));
    CHECK(lv_subject_get_int(ts.get_chamber_heater_externally_controlled_subject()) == 1);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_fault_subject()) == 0);
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "--");

    // The stock schema's own link field reaches the shared offline debounce
    // (whose run length is pinned on the other appliance source).
    const nlohmann::json down{{"panda_breath", {{"connected", false}}}};
    for (int i = 0; i < 3; ++i) {
        state.update_from_status(down);
        CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    }
    state.update_from_status(down);
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 1);

    settings.set_chamber_heater_assignment("auto");
    settings.set_chamber_sensor_assignment("auto");
}

// externally_controlled is computed from mode + source + lease_owned, so it is
// an answer only when a frame carries all three. Its surface is informational:
// another controller driving the heater is not a fault.
TEST_CASE("externally-controlled reaches its subject only on a complete trio",
          "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    // Heating, and neither our lease nor a klipper source: somebody else.
    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"ptc_temp": 55.0, "mode": "power_on", "source": "device",
                       "lease_owned": false}})"));
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_externally_controlled_subject()) == 1);

    // Same heat, driven through klipper: ours.
    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"ptc_temp": 56.0, "mode": "power_on", "source": "klipper",
                       "lease_owned": true}})"));
    CHECK(lv_subject_get_int(ts.get_chamber_heater_externally_controlled_subject()) == 0);

    // Back to somebody else, then a delta carrying only part of the trio. A
    // partial trio is not a confident "no", so the subject holds.
    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"ptc_temp": 57.0, "mode": "power_on", "source": "device",
                       "lease_owned": false}})"));
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_externally_controlled_subject()) == 1);

    ts.update_from_status(nlohmann::json::parse(R"({
      "dragonbreath": {"ptc_temp": 58.0, "mode": "power_on"}})"));
    CHECK(lv_subject_get_int(ts.get_chamber_heater_externally_controlled_subject()) == 1);
    // The delta was processed: the field it did carry landed.
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "58.0°C");
}

// Measured on a U1: the appliance reports connected:false for exactly one poll
// roughly every twenty minutes and recovers on the next frame. Raising the
// banner on that would flash an alarm several times an hour for nothing.
TEST_CASE("a one-poll link flap never reaches the banner", "[chamber][subjects][1290]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState ts;
    ts.init_subjects(false);
    ts.set_chamber_diagnostics_source("dragonbreath", "dragonbreath",
                                      "output_pin dragonbreath_filter");

    ts.update_from_status({{"dragonbreath", {{"connected", true}, {"ptc_temp", 30.0}}}});
    REQUIRE(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);

    // The flap, at the longest length measured on hardware (two reports).
    ts.update_from_status({{"dragonbreath", {{"connected", false}, {"ptc_temp", 30.05}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    ts.update_from_status({{"dragonbreath", {{"connected", false}, {"ptc_temp", 30.1}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    ts.update_from_status({{"dragonbreath", {{"connected", true}, {"ptc_temp", 30.2}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);

    // A recovered flap must not leave the run part-built: a later lone flap
    // still has to start from zero rather than tipping the banner over.
    ts.update_from_status({{"dragonbreath", {{"connected", false}, {"ptc_temp", 30.3}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);
    ts.update_from_status({{"dragonbreath", {{"connected", true}, {"ptc_temp", 30.4}}}});
    CHECK(lv_subject_get_int(ts.get_chamber_heater_offline_subject()) == 0);

    // The frames landed, so this is not a test passing on a dropped payload.
    CHECK(std::string(lv_subject_get_string(ts.get_chamber_heater_element_temp_text_subject())) ==
          "30.4°C");
}
