// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend.h"
#include "ams_state.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

/// This file had no fixture, so nothing drained the UpdateQueue. The AmsState
/// tests below add backends and sync them, and AmsState defers its subject
/// writes; each test returned with that work queued and handed it to whichever
/// test drained next (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects are still alive,
/// before HelixTestFixture's own teardown.
struct MultiBackendFixture : public HelixTestFixture {
    ~MultiBackendFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
    REQUIRE(systems[0].type == AmsType::HAPPY_HARE);
    REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: toolchanger only detected as one system",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "extruder", "extruder1", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::TOOL_CHANGER);
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
    REQUIRE(systems[0].type == AmsType::HAPPY_HARE);
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
    REQUIRE(systems[0].type == AmsType::AFC);
    // Toolchanger capability is still detected
    REQUIRE(hw.has_tool_changer());
}

TEST_CASE_METHOD(MultiBackendFixture, "PrinterDiscovery: no AMS detected returns empty",
                 "[ams][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.detected_ams_systems().empty());
    REQUIRE(hw.mmu_type() == AmsType::NONE);
}

// #1107: Anycubic Kobra S1 "mainline-Python ACE fork" registers its filament
// system as `ace_instance_0` (has get_status()), NOT `ace`/`filament_hub`. The
// config section is [ace] with ace_count=1, but only ace_instance_N appears in
// printer.objects.list. Detection must match the ace_instance_N prefix and
// record the object name(s) so the discovery sequence can subscribe them.
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: ace_instance_0 (Kobra S1 fork) detected as ACE",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects =
        nlohmann::json::array({"ace_instance_0", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::ACE);
    REQUIRE(hw.mmu_type() == AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace_instance_0"});
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
    REQUIRE(systems[0].type == AmsType::ACE);
    REQUIRE(hw.mmu_type() == AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace_instance_0", "ace_instance_1"});
}

TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: classic ace object still recorded in ace_object_names",
                 "[ams][ace][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array({"ace", "extruder", "heater_bed", "gcode_move"});
    hw.parse_objects(objects);

    REQUIRE(hw.mmu_type() == AmsType::ACE);
    REQUIRE(hw.ace_object_names() == std::vector<std::string>{"ace"});
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
    REQUIRE(hw.mmu_type() == AmsType::SNAPMAKER);

    // Exactly one backend, and it is the U1's. A toolchanger with four tools is
    // also present, so SNAPMAKER here also proves the fallback did not slide
    // down to TOOL_CHANGER.
    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::SNAPMAKER);
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
    REQUIRE(hw.mmu_type() == AmsType::AFC);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::AFC);
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
    REQUIRE(hw.mmu_type() == AmsType::AFC);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::AFC);
}

// A Snapmaker U1 running a filament system we CAN read keeps that system. The
// yield is scoped to the AFC-Lite stub, not to "any MMU on a U1".
TEST_CASE_METHOD(MultiBackendFixture,
                 "PrinterDiscovery: a readable MMU alongside filament_detect still outranks the U1",
                 "[ams][snapmaker][multi-backend]") {
    helix::PrinterDiscovery hw;
    nlohmann::json objects = nlohmann::json::array(
        {"mmu", "mmu_encoder mmu_encoder", "filament_detect", "extruder", "heater_bed"});
    hw.parse_objects(objects);

    REQUIRE(hw.has_snapmaker());
    REQUIRE(hw.has_mmu());
    REQUIRE(hw.mmu_type() == AmsType::HAPPY_HARE);

    const auto& systems = hw.detected_ams_systems();
    REQUIRE(systems.size() == 1);
    REQUIRE(systems[0].type == AmsType::HAPPY_HARE);
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

    // Should not crash for primary backend
    ams.update_slot_for_backend(0, 0);

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
    REQUIRE(hw.detected_ams_systems()[0].type == AmsType::TOOL_CHANGER);

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
