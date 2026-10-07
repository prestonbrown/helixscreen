// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Oracle tests for backend-owned filament-sensor ownership (#1054).
//
// PrinterHardware::is_ams_sensor() used to switch on AmsType to recognize each
// backend's conventionally-named filament sensors (the ones that do NOT carry
// an AMS keyword in their name, e.g. Happy Hare's bare "extruder"/"toolhead").
// That switch was a coupling leak: a new backend had to edit printer_hardware.
//
// These tests pin the EXACT set of bare sensor names each backend recognizes so
// the per-backend predicate that replaces the switch cannot silently regress
// runout/error detection. The substring fallback (keyword-bearing names) is the
// type-independent path and is exercised separately.

#include "ams_backend.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "printer_hardware.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsType;

namespace {

// Build a discovery whose mmu_type() == `type`, with the minimum object list to
// trigger detection. For AFC we also feed real lane + buffer objects so the
// per-lane / per-buffer sensor patterns resolve against discovered names.
helix::PrinterDiscovery make_discovery(AmsType type) {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array();
    switch (type) {
    case AmsType::HAPPY_HARE:
        objects = {"mmu", "mmu_encoder mmu_encoder", "extruder", "heater_bed", "gcode_move"};
        break;
    case AmsType::AFC:
        // lane1/lane2 + a buffer named "TN" so <lane>_* and <buffer>_* resolve.
        objects = {"AFC",      "AFC_stepper lane1", "AFC_stepper lane2", "AFC_buffer TN",
                   "extruder", "heater_bed",        "gcode_move"};
        break;
    case AmsType::CFS:
        objects = {"box", "extruder", "heater_bed", "gcode_move"};
        break;
    case AmsType::AD5X_IFS:
        objects = {"filament_motion_sensor ifs_motion_sensor", "extruder", "heater_bed",
                   "gcode_move"};
        break;
    case AmsType::ACE:
        objects = {"ace", "extruder", "heater_bed", "gcode_move"};
        break;
    case AmsType::TOOL_CHANGER:
        objects = {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"};
        break;
    case AmsType::QIDI_BOX:
        objects = {"box_stepper slot0", "box_stepper slot1", "extruder", "heater_bed"};
        break;
    case AmsType::SNAPMAKER:
        objects = {"filament_detect", "extruder", "heater_bed", "gcode_move"};
        break;
    default:
        objects = {"extruder", "heater_bed", "gcode_move"};
        break;
    }
    hw.parse_objects(objects);
    return hw;
}

// A discovery with NO MMU (plain printer) — used to assert that backend-named
// sensors stay visible when no AMS is detected.
helix::PrinterDiscovery make_no_mmu_discovery() {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);
    return hw;
}

bool is_ams(const std::string& bare, const helix::PrinterDiscovery& d) {
    // Sensors are usually fully prefixed in the wild; the switch strips the
    // "filament_switch_sensor " / "filament_motion_sensor " prefix. Feed the
    // switch-sensor prefix so the strip path is exercised, matching production.
    return PrinterHardware::is_ams_sensor("filament_switch_sensor " + bare, d);
}

} // namespace

// ---------------------------------------------------------------------------
// Happy Hare: bare extruder / toolhead / filament_tension / filament_compression
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: Happy Hare named sensors", "[ams][sensor-ownership]") {
    auto hh = make_discovery(AmsType::HAPPY_HARE);
    REQUIRE(hh.mmu_type() == AmsType::HAPPY_HARE);

    CHECK(is_ams("extruder", hh));
    CHECK(is_ams("toolhead", hh));
    CHECK(is_ams("filament_tension", hh));
    CHECK(is_ams("filament_compression", hh));
    // v3 and v4 per-gate names.
    CHECK(is_ams("mmu_pre_gate_0", hh));
    CHECK(is_ams("mmu_entry_0", hh));
    CHECK(is_ams("mmu_exit_3", hh));

    // Names HH does NOT own (and that carry no AMS keyword) stay visible.
    CHECK_FALSE(is_ams("my_runout", hh));
    CHECK_FALSE(is_ams("door_sensor", hh));
    // AFC-only names are NOT owned by HH.
    CHECK_FALSE(is_ams("tool_start", hh));
    CHECK_FALSE(is_ams("tool_end", hh));
}

// ---------------------------------------------------------------------------
// AFC: tool_start/tool_end, <lane>_prep/_load/_selector,
//      <buffer>_expanded/_compressed, *_home_pin
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: AFC named + per-lane + per-buffer sensors",
          "[ams][sensor-ownership]") {
    auto afc = make_discovery(AmsType::AFC);
    REQUIRE(afc.mmu_type() == AmsType::AFC);
    REQUIRE(afc.afc_lane_names().size() == 2);

    // Fixed extruder sensors.
    CHECK(is_ams("tool_start", afc));
    CHECK(is_ams("tool_end", afc));

    // Per-lane sensors for each discovered lane.
    for (const auto& lane : afc.afc_lane_names()) {
        CHECK(is_ams(lane + "_prep", afc));
        CHECK(is_ams(lane + "_load", afc));
        CHECK(is_ams(lane + "_selector", afc));
    }

    // Per-buffer sensors for each discovered buffer.
    REQUIRE_FALSE(afc.afc_buffer_names().empty());
    for (const auto& buf : afc.afc_buffer_names()) {
        CHECK(is_ams(buf + "_expanded", afc));
        CHECK(is_ams(buf + "_compressed", afc));
    }

    // HTLF home-pin suffix, regardless of unit name.
    CHECK(is_ams("anything_home_pin", afc));
    CHECK(is_ams("turtle1_home_pin", afc));

    // A per-lane sensor for a lane name that is NOT discovered (and carries no
    // AMS keyword) is NOT owned — only the discovered lanes' sensors match.
    CHECK_FALSE(is_ams("xyz_prep", afc));
    CHECK_FALSE(is_ams("xyz_load", afc));
    // HH-only names are NOT owned by AFC.
    CHECK_FALSE(is_ams("toolhead", afc));
    CHECK_FALSE(is_ams("filament_tension", afc));
}

// ---------------------------------------------------------------------------
// AD5X IFS: ifs_motion_sensor, head_switch_sensor,
//           _ifs_port_sensor_N, _ifs_motion_sensor_N
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: AD5X IFS named sensors", "[ams][sensor-ownership]") {
    auto ifs = make_discovery(AmsType::AD5X_IFS);
    REQUIRE(ifs.mmu_type() == AmsType::AD5X_IFS);

    CHECK(is_ams("ifs_motion_sensor", ifs));
    CHECK(is_ams("head_switch_sensor", ifs));
    CHECK(is_ams("_ifs_port_sensor_1", ifs));
    CHECK(is_ams("_ifs_port_sensor_4", ifs));
    CHECK(is_ams("_ifs_motion_sensor_2", ifs));

    // Not owned.
    CHECK_FALSE(is_ams("head_runout", ifs));
    CHECK_FALSE(is_ams("ifs_port_sensor_1", ifs)); // missing leading underscore
}

// ---------------------------------------------------------------------------
// CFS: bare "filament_sensor"
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: CFS toolhead sensor", "[ams][sensor-ownership]") {
    auto cfs = make_discovery(AmsType::CFS);
    REQUIRE(cfs.mmu_type() == AmsType::CFS);

    CHECK(is_ams("filament_sensor", cfs));

    // CFS owns only that one bare name.
    CHECK_FALSE(is_ams("extruder", cfs));
    CHECK_FALSE(is_ams("toolhead", cfs));
    CHECK_FALSE(is_ams("tool_start", cfs));
}

// ---------------------------------------------------------------------------
// Backends with NO named-sensor case in the switch (default → substring only).
// "filament_sensor" must NOT be suppressed for these; "extruder" must NOT be.
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: ACE/ToolChanger have no named-sensor ownership",
          "[ams][sensor-ownership]") {
    for (AmsType t : {AmsType::ACE, AmsType::TOOL_CHANGER}) {
        auto d = make_discovery(t);
        // These backends recognized no bare named sensors in the original switch.
        CHECK_FALSE(is_ams("filament_sensor", d)); // CFS-only name, not owned here
        CHECK_FALSE(is_ams("extruder", d));        // HH-only name, not owned here
        CHECK_FALSE(is_ams("toolhead", d));
        CHECK_FALSE(is_ams("tool_start", d)); // AFC-only name, not owned here
    }
}

// ---------------------------------------------------------------------------
// Substring fallback is type-independent and fires even with NO MMU detected.
// This path is unchanged by the refactor — guard it so we don't regress it.
// ---------------------------------------------------------------------------
TEST_CASE("backend-managed sensor: keyword substring path is type-independent",
          "[ams][sensor-ownership]") {
    auto none = make_no_mmu_discovery();
    REQUIRE_FALSE(none.has_mmu());

    // Keyword-bearing names are AMS sensors regardless of detected backend.
    CHECK(PrinterHardware::is_ams_sensor("filament_switch_sensor lane1", none));
    CHECK(PrinterHardware::is_ams_sensor("filament_switch_sensor mmu_gate", none));
    CHECK(PrinterHardware::is_ams_sensor("filament_switch_sensor afc_whatever", none));
    CHECK(PrinterHardware::is_ams_sensor("filament_switch_sensor gate_2", none));
    CHECK(PrinterHardware::is_ams_sensor("filament_switch_sensor hub_sensor", none));

    // Backend-named sensors WITHOUT a keyword stay visible when no MMU detected
    // (a plain printer's "extruder" runout switch must not be hidden).
    CHECK_FALSE(PrinterHardware::is_ams_sensor("filament_switch_sensor extruder", none));
    CHECK_FALSE(PrinterHardware::is_ams_sensor("filament_switch_sensor toolhead", none));
    CHECK_FALSE(PrinterHardware::is_ams_sensor("filament_switch_sensor filament_sensor", none));
}
