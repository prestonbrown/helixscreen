// tests/unit/test_chamber_heater_backend.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "chamber_heater_backend.h"

#include <cmath>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::chamber;

TEST_CASE("generic backend keeps keyword tiers", "[chamber][backend]") {
    const auto* generic = backend_by_id("generic");
    REQUIRE(generic != nullptr);
    // Tiers preserved from printer_discovery.h chamber_keyword_confidence
    CHECK(generic->discovery_confidence("chamber") == 100);
    CHECK(generic->discovery_confidence("chamber_heater") == 99); // compound penalty
    CHECK(generic->discovery_confidence("enclosure") == 90);
    CHECK(generic->discovery_confidence("cavity") == 85);
    CHECK(generic->discovery_confidence("box") == 60);
    CHECK(generic->discovery_confidence("heater_box1") ==
          0); // "BOX1" not standalone BOX (AFC dryer)
    CHECK(generic->discovery_confidence("hotend") == 0);
    CHECK(generic->discovery_confidence("chamber_humidity") ==
          59); // 100 -1 compound -40 air-quality
    // Original tokenizer splits ONLY on _/whitespace — hyphen is not a separator.
    CHECK(generic->discovery_confidence("chamber-tvoc") == 99); // no air-quality penalty
    CHECK(generic->discovery_confidence("my-box") == 0);        // BOX not standalone
    // A plain heater_generic is one temperature, with no element behind it.
    CHECK(generic->reports_element_temp() == false);
}

TEST_CASE("registry exposes generic as default", "[chamber][backend]") {
    CHECK(registry().empty() == false);
    CHECK(backend_by_id("generic") == registry().front());
}

TEST_CASE("match dispatches to best backend", "[chamber][backend]") {
    CHECK(match("heater_generic chamber").backend == backend_by_id("generic"));
    CHECK(match("heater_generic chamber").confidence > 0);
    CHECK(match("hotend").backend == nullptr); // nothing claims it
}

TEST_CASE("dragonbreath backend matches names and ceiling", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);
    CHECK(db->discovery_confidence("dragonbreath") == 95);
    CHECK(db->discovery_confidence("heater_generic dragonbreath") == 95);
    CHECK(db->discovery_confidence("heater_generic chamber") == 0);
    CHECK(db->diagnostics_object() == "dragonbreath");
    CHECK(db->filter_fan_pin() == "output_pin dragonbreath_filter");
    CHECK(db->fault_reset_gcode() == "DRAGONBREATH_RESET");
    CHECK(db->reports_element_temp() == true); // ptc_temp rides every frame
    CHECK(db->conservative_max_temp() == 60.0);
    CHECK(db->device_autonomous_control() == false);
}

TEST_CASE("dragonbreath parse: live nominal payload", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    // Paste the Reference-payload nominal JSON at the top of this plan:
    nlohmann::json j = nlohmann::json::parse(R"({"temperature":25.5,"target":0.0,
      "connected":true,"heating":false,"fault":false,"inhibited":false,
      "fault_reason":null,"ptc_temp":24.9,"fan_percent":0,"fan_reason":"off",
      "mode":"off","source":"klipper","lease_owned":false})");
    auto d = db->parse_diagnostics(j);
    REQUIRE(d.has_value());
    CHECK(d->fault == false);
    CHECK(d->inhibited == false);
    // fault_reason: null is the device answering "none", not an absent field:
    // engaged as an empty string.
    CHECK(d->fault_reason == "");
    REQUIRE(d->element_temp_c.has_value());
    CHECK(*d->element_temp_c == Catch::Approx(24.9));
    CHECK(d->filter_fan_percent == 0);
    CHECK(d->filter_fan_reason == "off");
    CHECK(d->externally_controlled == false);
}

TEST_CASE("dragonbreath parse: faulted + external-control variants", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    auto faulted = db->parse_diagnostics(nlohmann::json::parse(R"({"fault":true,
      "inhibited":false,"fault_reason":"ptc_overtemp","ptc_temp":106.2,
      "fan_percent":100,"fan_reason":"purge","mode":"off","source":"device",
      "lease_owned":false,"connected":true})"));
    REQUIRE(faulted.has_value());
    CHECK(faulted->fault == true);
    CHECK(faulted->fault_reason == "ptc_overtemp");
    CHECK(faulted->fault_reason_kind == FaultReason::Overtemp);
    REQUIRE(faulted->element_temp_c.has_value());
    CHECK(*faulted->element_temp_c == Catch::Approx(106.2));

    auto ext = db->parse_diagnostics(nlohmann::json::parse(R"({"fault":false,
      "inhibited":false,"fault_reason":null,"ptc_temp":30.1,"fan_percent":40,
      "fan_reason":"filter","mode":"power_on","source":"webui",
      "lease_owned":false,"connected":true})"));
    REQUIRE(ext.has_value());
    CHECK(ext->externally_controlled == true); // heating, source != klipper, no lease
}

TEST_CASE("dragonbreath fault codes classify to generic kinds", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);

    // Substring-heuristic table: the live "ptc_overtemp" plus the documented
    // families (overheat / sensor-short-open / comms-timeout-watchdog-
    // disconnect). Anything unrecognized is Other, never a vendor leak.
    struct Case {
        const char* code;
        FaultReason expected;
    };
    const Case cases[] = {
        {"ptc_overtemp", FaultReason::Overtemp},
        {"element_overheat", FaultReason::Overtemp},
        {"ptc_sensor_fault", FaultReason::SensorFault},
        {"ptc_short_circuit", FaultReason::SensorFault},
        {"thermistor_open", FaultReason::SensorFault},
        {"comms_timeout", FaultReason::CommsLoss},
        {"host_watchdog", FaultReason::CommsLoss},
        {"link_disconnect", FaultReason::CommsLoss},
        {"mystery_code", FaultReason::Other},
    };
    for (const auto& c : cases) {
        CAPTURE(c.code);
        auto d = db->parse_diagnostics(
            nlohmann::json{{"fault", true}, {"fault_reason", c.code}, {"ptc_temp", 24.9}});
        REQUIRE(d.has_value());
        CHECK(d->fault_reason == c.code); // raw code preserved for logs
        CHECK(d->fault_reason_kind == c.expected);
    }

    // No reason key at all -> no report this frame (nullopt, distinct from
    // the engaged None an explicit null reason classifies to).
    auto nominal = db->parse_diagnostics(nlohmann::json{{"ptc_temp", 24.9}});
    REQUIRE(nominal.has_value());
    CHECK_FALSE(nominal->fault_reason_kind.has_value());
    auto null_reason =
        db->parse_diagnostics(nlohmann::json{{"ptc_temp", 24.9}, {"fault_reason", nullptr}});
    REQUIRE(null_reason.has_value());
    CHECK(null_reason->fault_reason_kind == FaultReason::None);
}

// A delta frame carries only changed fields and may legitimately lack
// ptc_temp, so frame recognition accepts any dragonbreath-schema key and only
// the keys present engage. See ChamberHeaterDiagnostics in
// chamber_heater_backend.h for the full field-level rule.
TEST_CASE("dragonbreath parse: delta frames engage only carried fields",
          "[chamber][backend][1290]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);

    // A fan-only delta is ours even without ptc_temp, and nothing else engages.
    auto fan_only = db->parse_diagnostics(nlohmann::json{{"fan_percent", 55}});
    REQUIRE(fan_only.has_value());
    CHECK(fan_only->filter_fan_percent == 55);
    CHECK_FALSE(fan_only->fault.has_value());
    CHECK_FALSE(fan_only->element_temp_c.has_value());
    CHECK_FALSE(fan_only->fault_reason.has_value());
    CHECK_FALSE(fan_only->filter_fan_driver.has_value());
    CHECK_FALSE(fan_only->externally_controlled.has_value());

    // externally_controlled reads mode, source AND lease_owned: a frame
    // carrying only part of the trio is not an answer.
    auto mode_only = db->parse_diagnostics(nlohmann::json{{"mode", "power_on"}});
    REQUIRE(mode_only.has_value());
    CHECK_FALSE(mode_only->externally_controlled.has_value());

    // No dragonbreath key at all: not ours, even as an object.
    CHECK_FALSE(db->parse_diagnostics(nlohmann::json{{"temperature", 21.0}}).has_value());
    CHECK_FALSE(db->parse_diagnostics(nlohmann::json::object()).has_value());
}

// The appliance's own radio link. An engaged false is the device reporting
// itself unreachable; an absent key is no report. A connected-only delta must
// be recognized as ours — the frame the device emits when it drops off WiFi
// carries exactly that one field.
TEST_CASE("dragonbreath parse: connected engages, absent stays unengaged",
          "[chamber][backend][1290]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);

    auto offline = db->parse_diagnostics(nlohmann::json{{"connected", false}});
    REQUIRE(offline.has_value());
    CHECK(offline->device_connected == false);

    auto online = db->parse_diagnostics(nlohmann::json{{"connected", true}});
    REQUIRE(online.has_value());
    CHECK(online->device_connected == true);

    auto silent = db->parse_diagnostics(nlohmann::json{{"ptc_temp", 24.9}});
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->device_connected.has_value());

    // A malformed value in the connected slot is not evidence the device is
    // unreachable — it engages as connected (unknown is not offline).
    auto garbage = db->parse_diagnostics(nlohmann::json{{"connected", "yes"}});
    REQUIRE(garbage.has_value());
    CHECK(garbage->device_connected == true);
}

// protocol_error is raw vendor vocabulary with no UI kind: null engages as an
// empty string, a string engages verbatim for the log.
TEST_CASE("dragonbreath parse: protocol_error engages for logs only", "[chamber][backend][1290]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);

    auto d =
        db->parse_diagnostics(nlohmann::json{{"connected", true}, {"protocol_error", nullptr}});
    REQUIRE(d.has_value());
    CHECK(d->link_error == "");

    auto errored = db->parse_diagnostics(
        nlohmann::json{{"connected", false}, {"protocol_error", "frame_crc"}});
    REQUIRE(errored.has_value());
    CHECK(errored->link_error == "frame_crc");
    CHECK(errored->device_connected == false);
}

TEST_CASE("dragonbreath fan reasons classify to generic drivers", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);

    // Closed vendor vocabulary measured on the rig: off / requested / heater
    // / thermal_purge. Any OTHER non-empty value is still the device acting
    // on its own — a reason we cannot classify is never "we control it".
    struct Case {
        const char* reason;
        FilterFanDriver expected;
    };
    const Case cases[] = {
        {"off", FilterFanDriver::Off},       {"requested", FilterFanDriver::Requested},
        {"heater", FilterFanDriver::Device}, {"thermal_purge", FilterFanDriver::Device},
        {"button", FilterFanDriver::Device},
    };
    for (const auto& c : cases) {
        CAPTURE(c.reason);
        auto d = db->parse_diagnostics(
            nlohmann::json{{"ptc_temp", 24.9}, {"fan_percent", 100}, {"fan_reason", c.reason}});
        REQUIRE(d.has_value());
        CHECK(d->filter_fan_reason == c.reason); // raw reason preserved for logs
        CHECK(d->filter_fan_driver == c.expected);
    }

    // A missing reason key is no report this frame; an explicit null IS a
    // report, of an unknown driver — never Off: no report is not a report
    // that the fan is stopped.
    auto missing = db->parse_diagnostics(nlohmann::json{{"ptc_temp", 24.9}});
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->filter_fan_driver.has_value());
    auto null_reason =
        db->parse_diagnostics(nlohmann::json{{"ptc_temp", 24.9}, {"fan_reason", nullptr}});
    REQUIRE(null_reason.has_value());
    CHECK(null_reason->filter_fan_driver == FilterFanDriver::Unknown);
}

TEST_CASE("dragonbreath parse tolerates null lease fields", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    // .value() throws type_error.302 when a key is present but null; the
    // schema really emits nulls (mock synthesizes fault_reason: null).
    auto d = db->parse_diagnostics(nlohmann::json::parse(R"({"fault":false,
      "inhibited":false,"fault_reason":null,"ptc_temp":24.9,"fan_percent":0,
      "fan_reason":"off","mode":null,"source":null,"lease_owned":null})"));
    REQUIRE(d.has_value());
    CHECK(d->externally_controlled == false);
}

TEST_CASE("dragonbreath parse rejects foreign payloads", "[chamber][backend]") {
    const auto* db = backend_by_id("dragonbreath");
    CHECK_FALSE(
        db->parse_diagnostics(nlohmann::json::parse(R"({"temperature":21.0})")).has_value());
    CHECK_FALSE(db->parse_diagnostics(nlohmann::json::parse("7")).has_value());
}

// The stock firmware's Klipper binding publishes no fault, no filtration
// speed and no reset command, so those slots stay unengaged for every frame.
// What it does publish is its own link state and which control loop is
// holding the heater.
TEST_CASE("panda_breath backend: the surfaces stock actually has", "[chamber][backend][1290]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);
    CHECK(pb->discovery_confidence("heater_generic panda_breath") == 95);
    CHECK(pb->discovery_confidence("heater_generic pandabreath") == 95);
    CHECK(pb->discovery_confidence("heater_generic chamber") == 0);
    CHECK(pb->diagnostics_object() == "panda_breath");
    CHECK(pb->filter_fan_pin().empty());        // no filtration pin in the binding
    CHECK(pb->fault_reset_gcode().empty());     // nothing to reset: no fault surface
    CHECK(pb->reports_element_temp() == false); // the PTC temp stays on the appliance
    CHECK(pb->conservative_max_temp() == 60.0);
    CHECK(pb->device_autonomous_control() == true); // stock Auto drives from bed temp
}

// Recognition cannot hinge on one field: Moonraker deltas carry only what
// changed. Any stock-specific key marks the frame ours, and the three keys a
// plain heater also publishes (temperature/target/smoothed_temp) mark nothing,
// or the backend would claim every heater payload in the printer.
TEST_CASE("panda_breath parse: recognition spans the stock schema", "[chamber][backend][1290]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    const char* const witnesses[] = {
        "connected",     "work_mode",      "work_on",           "device_target",
        "auto_enabled",  "auto_target",    "auto_filtertemp",   "auto_hotbedtemp",
        "filament_temp", "filament_timer", "remaining_seconds", "filament_drying_active"};
    for (const char* key : witnesses) {
        CAPTURE(key);
        CHECK(pb->parse_diagnostics(nlohmann::json{{key, 0}}).has_value());
    }

    for (const char* shared : {"temperature", "target", "smoothed_temp"}) {
        CAPTURE(shared);
        CHECK_FALSE(pb->parse_diagnostics(nlohmann::json{{shared, 23.0}}).has_value());
    }
    CHECK_FALSE(pb->parse_diagnostics(nlohmann::json::object()).has_value());
    CHECK_FALSE(pb->parse_diagnostics(nlohmann::json::parse("7")).has_value());
}

// The binding holds a WebSocket to the appliance and reports whether it is up.
// Absent is no report; a value we cannot read is not evidence the appliance is
// gone, so it engages as connected rather than raising the offline banner.
TEST_CASE("panda_breath parse: connected engages, absent stays unengaged",
          "[chamber][backend][1290]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    auto offline = pb->parse_diagnostics(nlohmann::json{{"connected", false}});
    REQUIRE(offline.has_value());
    CHECK(offline->device_connected == false);

    auto online = pb->parse_diagnostics(nlohmann::json{{"connected", true}});
    REQUIRE(online.has_value());
    CHECK(online->device_connected == true);

    auto silent = pb->parse_diagnostics(nlohmann::json{{"work_mode", 2}});
    REQUIRE(silent.has_value());
    CHECK_FALSE(silent->device_connected.has_value());

    auto garbage = pb->parse_diagnostics(nlohmann::json{{"connected", "yes"}});
    REQUIRE(garbage.has_value());
    CHECK(garbage->device_connected == true);
}

// work_mode names the loop holding the heater: 1 = the appliance's own auto
// cycle, 2 = the target Klipper set, 3 = a filament-drying run. Only 2 is our
// target closing the loop. work_mode latches at its last value after the
// output stops, so work_on is what makes the answer present-tense.
TEST_CASE("panda_breath parse: only a klipper target counts as ours", "[chamber][backend][1290]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    struct Row {
        int work_mode;
        bool work_on;
        bool expected;
    };
    const Row rows[] = {
        {1, true, true},   // appliance auto loop holds the chamber
        {3, true, true},   // drying cycle: the appliance is running itself
        {2, true, false},  // heating to the target we set
        {1, false, false}, // output off: nobody is driving
        {2, false, false}, {3, false, false},
    };
    for (const auto& r : rows) {
        CAPTURE(r.work_mode, r.work_on);
        auto d = pb->parse_diagnostics(
            nlohmann::json{{"work_mode", r.work_mode}, {"work_on", r.work_on}});
        REQUIRE(d.has_value());
        CHECK(d->externally_controlled == r.expected);
    }

    // Both halves are needed: one alone is not an answer.
    auto mode_only = pb->parse_diagnostics(nlohmann::json{{"work_mode", 1}});
    REQUIRE(mode_only.has_value());
    CHECK_FALSE(mode_only->externally_controlled.has_value());
    auto on_only = pb->parse_diagnostics(nlohmann::json{{"work_on", true}});
    REQUIRE(on_only.has_value());
    CHECK_FALSE(on_only->externally_controlled.has_value());

    // A value we cannot read fails toward not raising the badge.
    auto garbage =
        pb->parse_diagnostics(nlohmann::json{{"work_mode", "auto"}, {"work_on", nullptr}});
    REQUIRE(garbage.has_value());
    CHECK(garbage->externally_controlled == false);
}

// Everything the stock binding does not publish stays unengaged, so the state
// layer keeps whatever it last knew instead of showing a cleared fault or a
// stopped fan the device never reported.
TEST_CASE("panda_breath parse leaves absent surfaces unengaged", "[chamber][backend][1290]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    auto d = pb->parse_diagnostics(nlohmann::json::parse(R"({
        "temperature":23.0,"target":0.0,"smoothed_temp":23.0,"connected":true,
        "work_mode":1,"work_on":true,"device_target":60.0,"auto_enabled":true,
        "auto_target":45,"auto_filtertemp":30,"auto_hotbedtemp":80,
        "filament_temp":60,"filament_timer":12,"remaining_seconds":0,
        "filament_drying_active":false})"));
    REQUIRE(d.has_value());
    CHECK(d->device_connected == true);
    CHECK(d->externally_controlled == true);
    CHECK_FALSE(d->fault.has_value());
    CHECK_FALSE(d->inhibited.has_value());
    CHECK_FALSE(d->fault_reason.has_value());
    CHECK_FALSE(d->fault_reason_kind.has_value());
    CHECK_FALSE(d->element_temp_c.has_value());
    CHECK_FALSE(d->filter_fan_percent.has_value());
    CHECK_FALSE(d->filter_fan_reason.has_value());
    CHECK_FALSE(d->filter_fan_driver.has_value());
    CHECK_FALSE(d->link_error.has_value());
}

TEST_CASE("appliance beats generic on its own name", "[chamber][backend]") {
    CHECK(match("heater_generic dragonbreath").backend == backend_by_id("dragonbreath"));
    CHECK(match("heater_generic panda_breath").backend == backend_by_id("panda_breath"));
    // printer-native chamber still wins over appliance tiers (100 > 95)
    CHECK(match("heater_generic chamber").backend == backend_by_id("generic"));
}

TEST_CASE("keyword_confidence pins the keyword rule", "[chamber][backend]") {
    struct Row {
        const char* name;
        int expected;
    };
    const Row rows[] = {
        // Tiers.
        {"chamber", 100},
        {"enclosure", 90},
        {"cavity", 85},
        {"box", 60},
        // Case-insensitive; keyword match is substring, BOX is token-only.
        {"ChAmBeR", 100},
        {"EnClOsUrE_TeMp", 89},
        {"ENCLOSURE_top", 89},
        {"my chamber", 99},   // whitespace separates tokens
        {"chamber-tvoc", 99}, // hyphen is NOT a separator: no air-quality token
        {"my-box", 0},
        {"boxx", 0},
        {"box1_heater", 0}, // numbered filament box: BOX1 is not BOX
        // Compound penalty.
        {"chamber_heater", 99},
        {"box_fan", 59},
        // Every air-quality token carries the -40 (compound -1 alongside).
        {"chamber_tvoc", 59},
        {"chamber_voc", 59},
        {"chamber_co2", 59},
        {"chamber_gas", 59},
        {"chamber_humidity", 59},
        {"chamber_iaq", 59},
        {"chamber_aqi", 59},
        {"chamber_pm25", 59},
        {"chamber_pm10", 59},
        {"chamber_particulate", 59},
        {"chamber_pressure", 59},
        {"enclosure_tvoc", 49},
        {"cavity_pressure", 44},
        {"box_gas", 19},
        // Air-quality tokens without a chamber keyword score nothing.
        {"tvoc", 0},
        {"humidity", 0},
        {"temperature_sensor voc", 0},
        // Appliance names carry no chamber keyword: keyword-only callers
        // (sensor/cooling-fan paths) must not see match()'s 95.
        {"dragonbreath", 0},
        {"heater_generic dragonbreath", 0},
        {"panda_breath", 0},
        // Everything else.
        {"hotend", 0},
        {"", 0},
    };
    for (const auto& r : rows) {
        INFO("name=" << r.name);
        CHECK(keyword_confidence(r.name) == r.expected);
    }
}

// ============================================================================
// Dryer (#1299)
// ============================================================================

// Stock drives a drying cycle through two commands that take whole hours, and
// the binding refuses anything outside 1-12. The cycle is the appliance
// running itself, so it cannot share the chamber with a print.
TEST_CASE("panda_breath backend: a dryer stock can drive", "[chamber][backend][dryer][1299]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    const helix::DryerInfo caps = pb->dryer_capabilities();
    CHECK(caps.supported);
    CHECK_FALSE(caps.allows_during_print);
    CHECK(caps.max_temp_c == 60.0f);
    CHECK(caps.max_duration_min == 720);
    CHECK(caps.duration_step_min == 60);

    CHECK(pb->dryer_start_gcode(55.0f, 240) == "PANDA_BREATH_DRY_START TEMP=55 HOURS=4");
    CHECK(pb->dryer_start_gcode(55.0f, 90) == "PANDA_BREATH_DRY_START TEMP=55 HOURS=2");
    CHECK(pb->dryer_start_gcode(55.0f, 20) == "PANDA_BREATH_DRY_START TEMP=55 HOURS=1");
    CHECK(pb->dryer_start_gcode(55.0f, 5000) == "PANDA_BREATH_DRY_START TEMP=55 HOURS=12");
    CHECK(pb->dryer_start_gcode(54.6f, 60) == "PANDA_BREATH_DRY_START TEMP=55 HOURS=1");
    CHECK(pb->dryer_stop_gcode() == "PANDA_BREATH_DRY_STOP");
}

// DragonBreath's glue publishes no drying command or status, and a plain
// heater_generic has no drying cycle at all.
TEST_CASE("dryer: backends without one say so", "[chamber][backend][dryer][1299]") {
    for (const char* id : {"dragonbreath", "generic"}) {
        CAPTURE(id);
        const auto* b = backend_by_id(id);
        REQUIRE(b != nullptr);
        CHECK_FALSE(b->dryer_capabilities().supported);
        CHECK(b->dryer_start_gcode(55.0f, 240).empty());
        CHECK(b->dryer_stop_gcode().empty());
    }
}

// filament_temp is the cycle's target, filament_timer its length in hours and
// remaining_seconds the countdown. Each engages only when the frame names it.
TEST_CASE("panda_breath parse: the drying cycle", "[chamber][backend][dryer][1299]") {
    const auto* pb = backend_by_id("panda_breath");
    REQUIRE(pb != nullptr);

    auto running = pb->parse_diagnostics(nlohmann::json::parse(R"({
        "work_mode":3,"work_on":true,"filament_drying_active":true,
        "filament_temp":55,"filament_timer":4,"remaining_seconds":11520})"));
    REQUIRE(running.has_value());
    CHECK(running->drying_active == true);
    CHECK(running->drying_target_c == 55);
    CHECK(running->drying_duration_min == 240);
    CHECK(running->drying_remaining_s == 11520);

    auto stopped = pb->parse_diagnostics(nlohmann::json{{"filament_drying_active", false}});
    REQUIRE(stopped.has_value());
    CHECK(stopped->drying_active == false);
    CHECK_FALSE(stopped->drying_remaining_s.has_value());

    auto tick = pb->parse_diagnostics(nlohmann::json{{"remaining_seconds", 600}});
    REQUIRE(tick.has_value());
    CHECK_FALSE(tick->drying_active.has_value());
    CHECK(tick->drying_remaining_s == 600);

    // A value we cannot read is no report.
    auto garbage = pb->parse_diagnostics(nlohmann::json{{"filament_drying_active", "yes"},
                                                        {"remaining_seconds", "soon"},
                                                        {"filament_timer", nullptr}});
    REQUIRE(garbage.has_value());
    CHECK_FALSE(garbage->drying_active.has_value());
    CHECK_FALSE(garbage->drying_remaining_s.has_value());
    CHECK_FALSE(garbage->drying_duration_min.has_value());

    auto negative = pb->parse_diagnostics(nlohmann::json{{"remaining_seconds", -5}});
    REQUIRE(negative.has_value());
    CHECK(negative->drying_remaining_s == 0);
}

TEST_CASE("dragonbreath rejection codes classify at the border", "[chamber][backend][rejection]") {
    const auto* db = backend_by_id("dragonbreath");
    REQUIRE(db != nullptr);
    auto parse = [db](const nlohmann::json& frame) {
        auto d = db->parse_diagnostics(frame);
        REQUIRE(d.has_value());
        return *d;
    };

    SECTION("each vendor code lands on one generic kind and cause") {
        struct Row {
            const char* code;
            CommandRejection kind;
            RefusalCause cause;
        };
        const Row rows[] = {
            {"countermanded", CommandRejection::Refused, RefusalCause::TurnedOff},
            {"fault_latched", CommandRejection::Refused, RefusalCause::Fault},
            {"inhibited", CommandRejection::Refused, RefusalCause::Inhibited},
            {"conflict_retries_exhausted", CommandRejection::Conflict, RefusalCause::Unspecified},
            {"missing_lease_id", CommandRejection::Refused, RefusalCause::Unspecified},
            {"lease_lost", CommandRejection::Refused, RefusalCause::Unspecified},
            {"heater_busy", CommandRejection::Refused, RefusalCause::Unspecified},
            {"invalid_command", CommandRejection::Refused, RefusalCause::Unspecified},
            {"revision_conflict", CommandRejection::Conflict, RefusalCause::Unspecified},
            {"auth_failed", CommandRejection::Auth, RefusalCause::Unspecified},
            {"heater_inhibited", CommandRejection::Refused, RefusalCause::Inhibited},
            {"heater_fault", CommandRejection::Refused, RefusalCause::Fault},
            {"power_off", CommandRejection::Refused, RefusalCause::TurnedOff},
            {"device_offline", CommandRejection::Refused, RefusalCause::Unspecified},
            {"something_new", CommandRejection::Refused, RefusalCause::Unspecified},
            {"", CommandRejection::None, RefusalCause::Unspecified},
        };
        for (const auto& r : rows) {
            INFO("code=" << r.code);
            const auto d = parse({{"protocol_error_code", r.code}});
            REQUIRE(d.rejection.has_value());
            CHECK(*d.rejection == r.kind);
            CHECK(d.refusal_cause.value_or(RefusalCause::Unspecified) == r.cause);
            CHECK(d.rejection_code.value_or("?") == r.code);
        }
    }

    SECTION("seq and requested target engage only when carried") {
        const auto d = parse({{"protocol_error", "revision_conflict"},
                              {"protocol_error_code", "revision_conflict"},
                              {"protocol_error_seq", 3},
                              {"protocol_error_target", 60.0}});
        CHECK(d.rejection_seq.value_or(-1) == 3);
        CHECK(d.rejection_target_c.value_or(0.0) == Catch::Approx(60.0));

        const auto delta = parse({{"protocol_error_seq", 4}});
        CHECK_FALSE(delta.rejection.has_value());
        CHECK(delta.rejection_seq.value_or(-1) == 4);
        CHECK_FALSE(delta.rejection_target_c.has_value());

        const auto null_target = parse({{"protocol_error_target", nullptr}});
        REQUIRE(null_target.rejection_target_c.has_value());
        CHECK(std::isnan(*null_target.rejection_target_c));
    }

    SECTION("an older glue's free-text error is never a rejection") {
        const auto d = parse({{"protocol_error", "revision_conflict"}});
        CHECK_FALSE(d.rejection.has_value());
        CHECK(d.link_error.value_or("") == "revision_conflict");

        // Its clearing still reports that nothing is outstanding.
        const auto cleared = parse({{"protocol_error", ""}});
        REQUIRE(cleared.rejection.has_value());
        CHECK(*cleared.rejection == CommandRejection::None);
    }

    SECTION("only the appliance has a route around the gcode queue") {
        CHECK(db->set_target_method() == "printer.dragonbreath.set_target");
        CHECK(backend_by_id("generic")->set_target_method().empty());
        CHECK(backend_by_id("panda_breath")->set_target_method().empty());
    }
}
