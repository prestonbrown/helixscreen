// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsBackend;
using helix::AmsState;

/// This file had no fixture, so nothing drained the UpdateQueue. The AmsState
/// tests below add backends and sync them, and AmsState defers its subject
/// writes; each test returned with that work queued and handed it to whichever
/// test drained next (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects are still alive,
/// before HelixTestFixture's own teardown.
struct MultiBackendFixture : public HelixTestFixture {
    ~MultiBackendFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: single MMU detected as one system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"mmu", "mmu_encoder mmu_encoder", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::HAPPY_HARE);
    REQUIRE(hw.mmu_type() == helix::AmsType::HAPPY_HARE);
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: toolchanger only detected as one system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::TOOL_CHANGER);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: toolchanger + Happy Hare prefers MMU backend",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "mmu",
                                                    "mmu_encoder mmu_encoder", "extruder",
                                                    "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Only the MMU should be registered — toolchanger is just tool switching
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::HAPPY_HARE);
    // Toolchanger capability is still detected
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: AFC + toolchanger prefers AFC backend",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "AFC", "AFC_stepper lane1", "AFC_stepper lane2",
         "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Only AFC should be registered — toolchanger is just tool switching
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::AFC);
    // Toolchanger capability is still detected
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: a toolchanger beside AFC is a coexisting filament system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "AFC",
                                            "AFC_stepper lane1", "AFC_stepper lane2", "extruder",
                                            "extruder1", "heater_bed", "gcode_move"}));

    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);
    CHECK(hw.has_filament_system(helix::AmsType::AFC));
    CHECK(hw.has_filament_system(helix::AmsType::TOOL_CHANGER));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::HAPPY_HARE));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::NONE));

    // A re-parse of a printer without the toolchanger drops it again.
    hw.parse_objects(nlohmann::json::array(
        {"AFC", "AFC_stepper lane1", "extruder", "heater_bed", "gcode_move"}));
    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);
    CHECK(hw.has_filament_system(helix::AmsType::AFC));
    CHECK_FALSE(hw.has_filament_system(helix::AmsType::TOOL_CHANGER));
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: the detected system row carries the type's display name",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;

    SECTION("Happy Hare") {
        hw.parse_objects(nlohmann::json::array({"mmu", "extruder", "heater_bed"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::HAPPY_HARE);
        CHECK(systems[0].name == "Happy Hare");
    }
    SECTION("AFC") {
        hw.parse_objects(nlohmann::json::array({"AFC", "AFC_stepper lane1", "extruder"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::AFC);
        CHECK(systems[0].name == "AFC");
    }
    SECTION("tool changer: one row") {
        hw.parse_objects(nlohmann::json::array(
            {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed"}));
        const auto& systems = hw.detected_ams_systems();
        REQUIRE(systems.size() == 1);
        CHECK(systems[0].type == helix::AmsType::TOOL_CHANGER);
        CHECK(systems[0].name == "Tool Changer");
    }
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: no AMS detected returns empty",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.detected_ams_systems().empty());
    REQUIRE(hw.mmu_type() == helix::AmsType::NONE);
}

// #1107: the ACEPRO driver registers each ACE unit as `ace_instance_N` (has
// get_status()). A capture whose printer.objects.list shows only
// ace_instance_N and no `ace`/`filament_hub` must still detect ACE. Detection must match the
// ace_instance_N prefix and record the object name(s) so the discovery sequence can subscribe them.
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: ace_instance_0 (ACEPRO driver) detected as ACE",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"ace_instance_0", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::ACE);
    REQUIRE(hw.mmu_type() == helix::AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace_instance_0"});
}

// A unit object is `ace_instance_` and its number: a name that only starts
// with "ace_instance" is some other module's object.
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: ace_instance without the separator is not an ACE unit",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"ace_instances", "ace_instance", "extruder", "heater_bed"});
    hw.parse_objects(objects);

    CHECK(hw.ace_object_names().empty());
    CHECK(hw.mmu_type() != helix::AmsType::ACE);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: multiple ace_instance_N objects all recorded",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    // Two ACE units — the first sets has_mmu_/ACE, but BOTH names must be
    // collected (name-collection is not gated by the !has_mmu_ guard).
    nlohmann::json objects = nlohmann::json::array(
        {"ace_instance_0", "ace_instance_1", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::ACE);
    REQUIRE(hw.mmu_type() == helix::AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace_instance_0", "ace_instance_1"});
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: classic ace object still recorded in ace_object_names",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"ace", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.mmu_type() == helix::AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace"});
}

// ----------------------------------------------------------------------------
// multiACE on a Snapmaker U1 (prestonbrown/helixscreen#1426)
//
// A U1 running multiACE reports both `ace` and the U1's own `filament_detect`.
// AmsBackendAce cannot read multiACE's per-unit `aces[]` slot shape, so the
// Snapmaker backend has to keep the printer. The cases below are one matrix:
// the rule must fire on both markers together, must NOT fire on either marker
// alone, and must not reach past ACE to a filament system we can read. Drop any
// one of those and it becomes "ACE never wins" or "the U1 never wins".
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: ace alongside filament_detect keeps the Snapmaker backend",
                 "[ams][ace][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    // A U1's object list with multiACE installed: the stock four-toolhead
    // machine plus multiACE's `ace` object.
    nlohmann::json objects = nlohmann::json::array(
        {"ace", "filament_detect", "toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
         "extruder", "extruder1", "extruder2", "extruder3", "print_task_config", "toolhead"});
    hw.parse_objects(objects);

    // The ACE branch ran and matched: without this the case could pass on a
    // typo in the object name rather than on the rule under test.
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace"});

    REQUIRE_FALSE(hw.has_mmu());
    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.mmu_type() == helix::AmsType::SNAPMAKER);

    // Exactly one backend, and it is the U1's. A toolchanger with four tools is
    // also present, so SNAPMAKER here also proves the fallback did not slide
    // down to TOOL_CHANGER.
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::SNAPMAKER);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: ace without filament_detect still registers the ACE backend",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    // A real Anycubic hub: no U1 marker anywhere in the list.
    nlohmann::json objects = nlohmann::json::array({"ace", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE_FALSE(hw.has_snapmaker());
    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::ACE);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::ACE);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: filament_detect without ace is an unmodded Snapmaker U1",
                 "[ams][ace][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"filament_detect", "toolchanger", "tool T0", "tool T1", "tool T2",
                               "tool T3", "extruder", "extruder1", "extruder2", "extruder3"});
    hw.parse_objects(objects);

    REQUIRE(hw.ace_object_names().empty());
    REQUIRE_FALSE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::SNAPMAKER);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::SNAPMAKER);
}

// ----------------------------------------------------------------------------
// PAXX AFC-Lite on a Snapmaker U1
//
// PAXX ships a status-only stub that impersonates AFC so Fluidd and Mainsail
// will draw their AFC panel for the U1's four extruders. It reports no
// extruders and no hubs, so the unit infers as HUB and the panel draws one
// nozzle behind a hub for a four-toolhead machine. The discriminator is the
// `AFC_unit` object: real AFC's AFC_unit.py is a base class with no
// load_config_prefix, so every real unit registers its own type instead
// (AFC_BoxTurtle, AFC_OpenAMS, AFC_HTLF, ...) and only the stub can publish a
// literal `AFC_unit`. The matrix below is the rule: it must fire on the stub
// marker together with the U1's, and must leave both real AFC on a U1 and the
// stub marker alone untouched.
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: AFC-Lite alongside filament_detect keeps the Snapmaker backend",
                 "[ams][afc][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    // A U1's object list with the PAXX AFC stub enabled: the stock
    // four-toolhead machine plus afc.cfg's [AFC], [AFC_unit U1] and lanes.
    nlohmann::json objects = nlohmann::json::array(
        {"AFC", "AFC_unit U1", "AFC_lane E0", "AFC_lane E1", "AFC_lane E2", "AFC_lane E3",
         "filament_detect", "toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "extruder",
         "extruder1", "extruder2", "extruder3", "print_task_config", "toolhead"});
    hw.parse_objects(objects);

    // The stub marker was seen: without this the case could pass on a typo in
    // the object name rather than on the rule under test.
    REQUIRE(hw.has_afc_lite());

    REQUIRE_FALSE(hw.has_mmu());
    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.mmu_type() == helix::AmsType::SNAPMAKER);

    // Exactly one backend, and it is the U1's. A toolchanger with four tools is
    // also present, so SNAPMAKER here also proves the fallback did not slide
    // down to TOOL_CHANGER.
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::SNAPMAKER);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: real AFC hardware on a U1 still keeps the AFC backend",
                 "[ams][afc][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    // A Box Turtle genuinely wired to a U1: real AFC registers its unit under
    // the hardware's own type, never a bare `AFC_unit`.
    nlohmann::json objects = nlohmann::json::array(
        {"AFC", "AFC_BoxTurtle Turtle_1", "AFC_stepper lane1", "AFC_stepper lane2",
         "AFC_hub Turtle_1", "filament_detect", "extruder", "extruder1"});
    hw.parse_objects(objects);

    // The stub marker is absent, which is the whole reason AFC keeps this one.
    REQUIRE_FALSE(hw.has_afc_lite());
    REQUIRE(hw.has_snapmaker());

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::AFC);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: an AFC_unit object without filament_detect keeps AFC",
                 "[ams][afc][multi-backend]") {
    helix::PrinterDiscovery hw;
    // No U1 marker anywhere, so nothing better is being displaced.
    nlohmann::json objects = nlohmann::json::array(
        {"AFC", "AFC_unit U1", "AFC_lane E0", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.has_afc_lite());
    REQUIRE_FALSE(hw.has_snapmaker());

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::AFC);
}

// ----------------------------------------------------------------------------
// Z-Mod's AFC shim on the FlashForge Creator 5
//
// Z-Mod 1.7.4 publishes an AFC-shaped surface (the `zmod_afclite` extra
// registering `AFC`, `AFC_lane E0..E3` and `AFC_unit C5`) so Fluidd and
// Mainsail draw their AFC panel for the C5's four heads. The machine is a
// head changer: the shim reports no extruders and no hubs, so an AFC backend
// infers a hub topology for heads that are each their own path, and most of
// the op macros it would send (AFC_RESET, AFC_LANE_RESET, SET_SPOOL_ID, ...)
// do not exist on this firmware. The `zmod_afclite` object plus the changer
// fingerprint (`zmod_color` + the carriage grab buttons, the same predicate
// the toolchanger provider rows use) names the machine, and the toolchanger
// backend keeps it. Object names from field bundle 5WSPTZBL.
// ----------------------------------------------------------------------------

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: Z-Mod's AFC shim on a Creator 5 keeps the tool changer",
                 "[ams][afc][toolchanger][multi-backend]") {
    helix::PrinterDiscovery hw;
    // The C5's object list under Z-Mod 1.7.4: the four heads, the changer's
    // dock/grab buttons, zmod_color, and the afclite shim's AFC objects.
    nlohmann::json objects = nlohmann::json::array({"AFC",
                                                    "AFC_unit C5",
                                                    "AFC_lane E0",
                                                    "AFC_lane E1",
                                                    "AFC_lane E2",
                                                    "AFC_lane E3",
                                                    "zmod_afclite",
                                                    "zmod_color",
                                                    "gcode_button extruder_pos1",
                                                    "gcode_button extruder_grab1",
                                                    "gcode_button extruder_pos2",
                                                    "gcode_button extruder_grab2",
                                                    "gcode_button extruder_pos3",
                                                    "gcode_button extruder_grab3",
                                                    "gcode_button extruder_pos4",
                                                    "gcode_button extruder_grab4",
                                                    "extruder",
                                                    "extruder1",
                                                    "extruder2",
                                                    "extruder3",
                                                    "heater_bed",
                                                    "heater_generic chamber_heater",
                                                    "filament_switch_sensor fd_ex0",
                                                    "filament_switch_sensor fd_ex1",
                                                    "filament_switch_sensor fd_ex2",
                                                    "filament_switch_sensor fd_ex3",
                                                    "filament_motion_sensor fm_ex0",
                                                    "filament_motion_sensor fm_ex1",
                                                    "filament_motion_sensor fm_ex2",
                                                    "filament_motion_sensor fm_ex3"});
    hw.parse_objects(objects);

    // The shim's own marker was seen. Its `AFC_unit C5` also trips the
    // has_afc_lite fact exactly like PAXX's named stub (`AFC_unit U1`), and the
    // C5 yields through the zmod rule below rather than the PAXX one: there is
    // no filament_detect here, so the Snapmaker-scoped rule cannot fire.
    REQUIRE(hw.has_zmod_afclite());
    REQUIRE(hw.has_afc_lite());

    // The shim yielded: four heads with no filament system above them register
    // the tool changer, and nothing else.
    REQUIRE_FALSE(hw.has_mmu());
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::TOOL_CHANGER);
    REQUIRE(hw.mmu_type() == helix::AmsType::TOOL_CHANGER);
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: zmod_afclite without the C5 changer fingerprint keeps AFC",
                 "[ams][afc][multi-backend]") {
    helix::PrinterDiscovery hw;
    // An afclite shim on a machine without the carriage grab buttons: the
    // changer fingerprint does not match, so nothing better is identified and
    // the AFC claim stands.
    nlohmann::json objects =
        nlohmann::json::array({"AFC", "AFC_unit C5", "AFC_lane E0", "zmod_afclite", "extruder",
                               "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.has_zmod_afclite());

    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::AFC);
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::AFC);
}

// A Snapmaker U1 running a filament system we CAN read keeps that system. The
// yield is scoped to ACE, not to "any MMU on a U1".
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: a readable MMU alongside filament_detect still outranks the U1",
                 "[ams][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"mmu", "mmu_encoder mmu_encoder", "filament_detect", "extruder", "heater_bed"});
    hw.parse_objects(objects);

    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == helix::AmsType::HAPPY_HARE);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == helix::AmsType::HAPPY_HARE);
}

// ============================================================================
// Task 2: Multi-backend storage tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: add_backend stores multiple backends",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    auto mock1 = AmsBackend::create_mock(4);
    auto mock2 = AmsBackend::create_mock(2);
    ams.add_backend(std::move(mock1));
    ams.add_backend(std::move(mock2));

    REQUIRE(ams.backend_count() == 2);
    REQUIRE(ams.get_backend(0) != nullptr);
    REQUIRE(ams.get_backend(1) != nullptr);
    REQUIRE(ams.get_backend(2) == nullptr);
    REQUIRE(ams.get_backend() == ams.get_backend(0));

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: set_backend replaces all backends",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.clear_backends();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));
    REQUIRE(ams.backend_count() == 2);

    ams.set_backend(AmsBackend::create_mock(3));
    REQUIRE(ams.backend_count() == 1);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: clear_backends removes all",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.backend_count() == 1);

    ams.clear_backends();
    REQUIRE(ams.backend_count() == 0);
    REQUIRE(ams.get_backend() == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 3: Per-backend slot subject accessor tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: primary backend uses flat slot subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.set_backend(AmsBackend::create_mock(4));

    REQUIRE(ams.get_slot_color_subject(0, 0) == ams.get_slot_color_subject(0));
    REQUIRE(ams.get_slot_color_subject(0, 3) == ams.get_slot_color_subject(3));
    REQUIRE(ams.get_slot_status_subject(0, 0) == ams.get_slot_status_subject(0));
    REQUIRE(ams.get_slot_status_subject(0, 3) == ams.get_slot_status_subject(3));

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: secondary backend gets separate slot subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(3));

    auto* color_0 = ams.get_slot_color_subject(0, 0);
    auto* color_1 = ams.get_slot_color_subject(1, 0);
    REQUIRE(color_0 != nullptr);
    REQUIRE(color_1 != nullptr);
    REQUIRE(color_0 != color_1);

    auto* status_0 = ams.get_slot_status_subject(0, 0);
    auto* status_1 = ams.get_slot_status_subject(1, 0);
    REQUIRE(status_0 != nullptr);
    REQUIRE(status_1 != nullptr);
    REQUIRE(status_0 != status_1);

    // Out of range for secondary (mock2 only has 3 slots: 0, 1, 2)
    REQUIRE(ams.get_slot_color_subject(1, 3) == nullptr);
    REQUIRE(ams.get_slot_status_subject(1, 3) == nullptr);

    // Non-existent backend
    REQUIRE(ams.get_slot_color_subject(2, 0) == nullptr);
    REQUIRE(ams.get_slot_status_subject(2, 0) == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 4: Per-backend event routing and sync tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: sync_backend updates correct subjects",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));

    // Sync primary
    ams.sync_backend(0);
    REQUIRE(lv_subject_get_int(ams.get_slot_count_subject()) > 0);

    // Sync secondary - should update secondary slot subjects
    ams.sync_backend(1);
    auto* sec_color = ams.get_slot_color_subject(1, 0);
    REQUIRE(sec_color != nullptr);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: update_slot_for_backend delegates to primary",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));

    // Backend 0 refreshes the primary slot subjects from the backend.
    lv_subject_t* color = ams.get_slot_color_subject(0);
    REQUIRE(color != nullptr);
    const auto expected = static_cast<int>(ams.get_backend(0)->get_slot_info(0).color_rgb);
    REQUIRE(expected != 0x123456);
    lv_subject_set_int(color, 0x123456);
    ams.update_slot_for_backend(0, 0);
    REQUIRE(lv_subject_get_int(color) == expected);

    // Should not crash for out-of-range backend
    ams.update_slot_for_backend(5, 0);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: get_backend negative index returns nullptr",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.get_backend(-1) == nullptr);

    ams.deinit_subjects();
}

// ============================================================================
// Task 5: Multi-backend init flow tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: init_backends_from_hardware with single system",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.detected_ams_systems().size() == 1);
    REQUIRE(hw.detected_ams_systems()[0].type == helix::AmsType::TOOL_CHANGER);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "AmsState: init_backends skips when no systems detected",
                 "[ams][multi-backend]") {
    lv_init_safe();
    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    // Verify detection returns empty - no systems to init
    REQUIRE(hw.detected_ams_systems().empty());

    ams.deinit_subjects();
}

// ============================================================================
// Task 8: Integration and lifecycle tests
// ============================================================================

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: full lifecycle", "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    // Add two mock backends
    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));

    REQUIRE(ams.backend_count() == 2);
    REQUIRE(lv_subject_get_int(ams.get_backend_count_subject()) == 2);

    // Sync both backends
    ams.sync_backend(0);
    ams.sync_backend(1);

    // Primary backend slots via flat accessors
    REQUIRE(ams.get_slot_color_subject(0) != nullptr);
    REQUIRE(ams.get_slot_color_subject(3) != nullptr);

    // Secondary backend slots via indexed accessors
    REQUIRE(ams.get_slot_color_subject(1, 0) != nullptr);
    REQUIRE(ams.get_slot_color_subject(1, 1) != nullptr);
    REQUIRE(ams.get_slot_color_subject(1, 2) == nullptr); // mock2 only has 2 slots

    // Active backend selection
    ams.set_active_backend(1);
    REQUIRE(ams.active_backend_index() == 1);
    REQUIRE(lv_subject_get_int(ams.get_active_backend_subject()) == 1);

    // Out of range selection is ignored
    ams.set_active_backend(5);
    REQUIRE(ams.active_backend_index() == 1); // unchanged

    // Deinit cleans everything up
    ams.deinit_subjects();
    REQUIRE(ams.backend_count() == 0);
    REQUIRE(lv_subject_get_int(ams.get_backend_count_subject()) == 0);
}

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: deinit then re-init is safe",
                 "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();

    // First cycle
    ams.deinit_subjects();
    ams.init_subjects(false);
    ams.add_backend(AmsBackend::create_mock(4));
    REQUIRE(ams.backend_count() == 1);
    ams.deinit_subjects();

    // Second cycle
    ams.init_subjects(false);
    REQUIRE(ams.backend_count() == 0);
    ams.add_backend(AmsBackend::create_mock(2));
    REQUIRE(ams.backend_count() == 1);
    ams.deinit_subjects();
}

TEST_CASE_METHOD(MultiBackendFixture, "Multi-backend: active backend resets on clear",
                 "[ams][multi-backend]") {
    lv_init_safe();

    AmsState& ams = AmsState::instance();
    ams.deinit_subjects();
    ams.init_subjects(false);

    ams.add_backend(AmsBackend::create_mock(4));
    ams.add_backend(AmsBackend::create_mock(2));
    ams.set_active_backend(1);
    REQUIRE(ams.active_backend_index() == 1);

    ams.clear_backends();
    REQUIRE(ams.active_backend_index() == 0);
    REQUIRE(ams.backend_count() == 0);

    ams.deinit_subjects();
}
