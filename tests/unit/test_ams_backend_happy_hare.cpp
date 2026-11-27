// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 36 of test_ams_backend_happy_hare.cpp before it was split, in its order. Mostly
// [clear], [gate_weights], [recovery], [bypass], [endless_spool], and more

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../test_helpers/happy_hare_test_fixtures.h"
#include "../test_helpers/mock_printer.h"
#include "../ui_test_utils.h"
#include "action_prompt_manager.h"
#include "ams_backend_happy_hare.h"
#include "ams_state.h"
#include "ams_types.h"
#include "buffer_reading.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_mapper.h"
#include "hh_defaults.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "spoolman_types.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_test_access.h"
#include "test_helpers/device_action_controls.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_fixture.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <any>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams_action_from_string;
using helix::AmsAction;
using helix::AmsBackendHappyHareTestHelper;
using helix::AmsError;
using helix::AmsResult;
using helix::AmsType;
using helix::BufferMotion;
using helix::EncoderClogInfo;
using helix::path_segment_from_happy_hare_pos;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::spoolman_mode_from_string;
using helix::spoolman_mode_to_string;
using helix::SpoolmanMode;
using helix::SpoolMotorAction;
using helix::StepOperationType;

using helix::test::action_enabled;
using helix::test::connect_with_fixture;
using helix::test::fan_entry;
using helix::test::feed_params;
using helix::test::find_action;
using helix::test::funnel_cleared_copy;
using helix::test::golden_hh_full_frame;
using helix::test::happy_hare_fixture_at;
using helix::test::HappyHareGoldenFixture;
using helix::test::has_recovery;
using helix::test::hh_gates;
using helix::test::HhFourGates;
using helix::test::HhFourGatesOf;
using helix::test::HhFourGatesWithClient;
using helix::test::HhHelper;
using helix::test::HhHelperWithClient;
using helix::test::join_ints;
using helix::test::kHappyHareGolden;
using helix::test::kObjectsQuery;
using helix::test::PrintLifecycleFixture;
using helix::test::recover_req;
using helix::test::ScopedConfig;
using helix::test::TuningRig;
using helix::test::unit_scoped_commands;
using helix::test::v3_fixture;
using helix::test::v3_linear_fixture;
using helix::test::v4_control_rows;

TEST_CASE("Happy Hare persistence: an edit writes exactly the fields it changed",
          "[ams][happy_hare][persistence][1597]") {
    // kKeep leaves a field at its SlotInfo default. Grey is the "no colour"
    // sentinel and is never written; pure black is a deliberate pick and is.
    constexpr int kKeep = -99;
    struct Row {
        const char* name;
        int gate;
        int prior_spool;
        int color;
        const char* material;
        int spool;
        int mapped_tool;
        const char* exact;    // a command that must be sent verbatim ("" = none)
        const char* contains; // a fragment some command must carry ("" = none)
        const char* absent;   // a fragment no command may carry ("" = none)
        bool sends_nothing;
        int mapping_after; // gate's mapped_tool afterwards (kKeep = not checked)
    };
    const Row rows[] = {
        {"colour, uppercase hex without #", 0, 0, 0xFF0000, nullptr, kKeep, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=FF0000 TEMP=0", "", "", false, kKeep},
        {"colour on another gate", 1, 0, 0x00FF00, nullptr, kKeep, kKeep,
         "MMU_GATE_MAP GATE=1 COLOR=00FF00 TEMP=0", "", "", false, kKeep},
        {"material alone", 1, 0, kKeep, "PLA", kKeep, kKeep,
         "MMU_GATE_MAP GATE=1 MATERIAL=PLA TEMP=0", "", "", false, kKeep},
        {"Spoolman id", 2, 0, kKeep, nullptr, 42, kKeep, "", "SPOOLID=42", "", false, kKeep},
        {"unlinking a spool sends -1", 0, 123, kKeep, nullptr, 0, kKeep, "", "SPOOLID=-1", "",
         false, kKeep},
        {"every field", 0, 0, 0x0000FF, "PETG", 99, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=0000FF MATERIAL=PETG SPOOLID=99 TEMP=0", "", "", false, kKeep},
        {"default grey is not a colour", 0, 0, 0x808080, "PLA", kKeep, kKeep, "", "",
         "COLOR=", false, kKeep},
        {"pure black is a colour", 0, 0, 0x000000, "ABS", kKeep, kKeep,
         "MMU_GATE_MAP GATE=0 COLOR=000000 MATERIAL=ABS TEMP=0", "", "", false, kKeep},
        {"empty material is not written", 0, 0, 0xFF0000, "", kKeep, kKeep, "", "",
         "MATERIAL=", false, kKeep},
        {"no spool before or after", 0, 0, 0xFF0000, nullptr, 0, kKeep, "", "", "SPOOLID=", false,
         kKeep},
        {"nothing set sends nothing", 0, 0, 0x808080, "", 0, kKeep, "", "", "", true, kKeep},
        {"gate 3", 3, 0, 0xFF0000, nullptr, kKeep, kKeep, "", "MMU_GATE_MAP GATE=3", "", false,
         kKeep},
        {"gate 7", 7, 0, 0xFF0000, nullptr, kKeep, kKeep, "", "MMU_GATE_MAP GATE=7", "", false,
         kKeep},
        {"a tool remap sends MMU_TTG_MAP", 0, 0, kKeep, nullptr, kKeep, 2,
         "MMU_TTG_MAP TOOL=2 GATE=0", "", "", false, kKeep},
        {"the same tool sends no MMU_TTG_MAP", 0, 0, kKeep, "PLA", kKeep, 0, "", "", "MMU_TTG_MAP ",
         false, kKeep},
        {"a default -1 tool leaves the mapping", 2, 0, kKeep, "PLA", kKeep, kKeep, "", "",
         "MMU_TTG_MAP ", false, 2},
    };

    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(8);
        if (row.prior_spool != 0) {
            helper.get_mutable_slot(row.gate)->spoolman_id = row.prior_spool;
        }

        SlotInfo info;
        if (row.color != kKeep)
            info.color_rgb = static_cast<uint32_t>(row.color);
        if (row.material)
            info.material = row.material;
        if (row.spool != kKeep)
            info.spoolman_id = row.spool;
        if (row.mapped_tool != kKeep)
            info.mapped_tool = row.mapped_tool;
        helix::test::apply_edit(helper, row.gate, info);

        if (row.sends_nothing)
            CHECK(helper.captured_gcodes.empty());
        if (*row.exact)
            CHECK(helper.has_gcode(row.exact));
        if (*row.contains)
            CHECK(helper.has_gcode_containing(row.contains));
        if (*row.absent)
            CHECK_FALSE(helper.has_gcode_containing(row.absent));
        if (row.mapping_after != kKeep)
            CHECK(helper.get_slot_entry(row.gate)->info.mapped_tool == row.mapping_after);
    }
}

TEST_CASE("Happy Hare persistence: clearing one field names it empty",
          "[ams][happy_hare][persistence]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->color_rgb = 0xED2C2C;
    SlotInfo* second = helper.get_mutable_slot(1);
    REQUIRE(second != nullptr);
    second->material = "PLA";
    second->color_rgb = 0xED2C2C;

    // The editor saves the whole slot, so the clear is an edit that keeps the
    // colour and empties the material. Happy Hare keeps an omitted parameter
    // at its current value, so the write must name the emptied field with an
    // explicit empty or the gate map goes on remembering PLA.
    SlotInfo cleared_material = *slot;
    cleared_material.material.clear();
    helix::test::apply_edit(helper, 0, cleared_material);
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=ED2C2C MATERIAL= TEMP=0"));

    // The same edit on the colour, keeping the material.
    SlotInfo cleared_color = *second;
    cleared_color.color_rgb = helix::kAmsDefaultSlotColor;
    helix::test::apply_edit(helper, 1, cleared_color);
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=1 COLOR= MATERIAL=PLA TEMP=0"));

    // Neither took the full wipe: a single-field clear keeps every other
    // field the gate map holds.
    REQUIRE_FALSE(helper.has_gcode_containing("SPOOLID=-1"));
    REQUIRE_FALSE(helper.has_gcode_containing("NAME="));
}

TEST_CASE_METHOD(HhHelper, "Happy Hare clear: the wipe names every gate-map field",
                 "[ams][happy_hare][clear]") {
    helper.initialize_test_gates(2);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->color_rgb = 0xFF0000;
    slot->brand = "Prusa";
    slot->spool_name = "Galaxy Black";
    slot->spoolman_id = 123;

    helix::test::apply_edit(helper, 0, funnel_cleared_copy(*slot));

    // Each field named with an explicit empty value (v2/v3 ignore params they
    // do not fetch; VENDOR is v4-only), SPOOLID=-1 unlinks, QUIET keeps the
    // echo out of the printer console.
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));
    // RESET ignores GATE on v2/v3 and wipes every gate; AVAILABLE=0 marks the
    // gate EMPTY, not unknown. With no configfile read yet there is no default
    // temperature to name, so TEMP stays off.
    REQUIRE_FALSE(helper.has_gcode_containing("RESET="));
    REQUIRE_FALSE(helper.has_gcode_containing("TEMP="));
    REQUIRE_FALSE(helper.has_gcode_containing("AVAILABLE="));
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare clear: the wipe returns the gate to default_extruder_temp",
                 "[ams][happy_hare][clear]") {
    // Happy Hare up to v4.2.0 reads an omitted TEMP as default_extruder_temp;
    // later versions keep the gate's own. Naming the default resets the gate
    // on both. The connect-time configfile read supplies it. From 4.2 the gate
    // map holds the weights, so the wipe clears them too.
    std::string tail = " QUIET=1";
    SECTION("v3 keeps it in [mmu]") {
        connect_with_fixture(
            helper, client, {{"configfile_settings", {{"mmu", {{"default_extruder_temp", 215}}}}}});
    }
    SECTION("v4 keeps it in [mmu_parameters], as Klipper's parsed float") {
        connect_with_fixture(helper, client,
                             {{"configfile_settings",
                               {{"mmu_machine", {{"happy_hare_version", "4.2.0"}}},
                                {"mmu_parameters", {{"default_extruder_temp", 215.0}}}}},
                              {"mmu_machine", {{"happy_hare_version", "4.2.0"}}}});
        tail = " WEIGHT=-1 INITIAL_WEIGHT=-1 QUIET=1";
    }

    SlotInfo* slot = helper.get_mutable_slot(1);
    REQUIRE(slot != nullptr);
    slot->material = "PETG";
    slot->nozzle_temp_min = 240;
    helper.clear_captured_gcodes();

    helix::test::apply_edit(helper, 1, funnel_cleared_copy(*slot));

    REQUIRE(helper.has_gcode(
        "MMU_GATE_MAP GATE=1 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 TEMP=215" + tail));
}

TEST_CASE_METHOD(HhHelper, "Happy Hare clear: a material edit does not take the wipe path",
                 "[ams][happy_hare][clear]") {
    helper.initialize_test_gates(2);

    SlotInfo info;
    info.material = "PLA";

    helix::test::apply_edit(helper, 0, info);

    // An editor commit carries the whole slot, so a kept material arrives
    // non-empty and stays on the incremental path.
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL=PLA TEMP=0"));
    REQUIRE_FALSE(helper.has_gcode_containing("SPOOLID=-1"));
    REQUIRE_FALSE(helper.has_gcode_containing("QUIET=1"));
}

TEST_CASE("Happy Hare in Spoolman pull mode refuses an edit whole and keeps no copy",
          "[ams][happy_hare][clear]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    REQUIRE(helper.get_system_info().spoolman_mode == SpoolmanMode::PULL);

    SlotInfo* slot = helper.get_mutable_slot(0);
    REQUIRE(slot != nullptr);
    slot->material = "PLA";
    slot->spoolman_id = 123;

    SlotInfo edit = *slot;
    SECTION("a clear") {
        edit = funnel_cleared_copy(*slot);
    }
    SECTION("an edit") {
        edit.material = "PETG";
        edit.mapped_tool = 1;
    }
    SECTION("a relink, with nothing else moved") {
        edit.spoolman_id = 456;
    }
    const AmsError err = helix::test::apply_edit(helper, 0, edit);

    // Happy Hare refuses local gate-map writes in pull mode and only logs it,
    // so nothing is sent, nothing is kept here, and the refusal is returned.
    CHECK_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
    CHECK_FALSE(helper.has_gcode_containing("MMU_TTG_MAP"));
    CHECK_FALSE(err.success());
    CHECK_FALSE(err.partially_applied);
    const SlotInfo* after = helper.get_mutable_slot(0);
    CHECK(after->material == "PLA");
    CHECK(after->spoolman_id == 123);
}

TEST_CASE("Happy Hare writes the name and temperature to the gate map", "[ams][happy_hare]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    SlotInfo edit;
    edit.material = "PLA";
    edit.spool_name = "PolyTerra";
    edit.nozzle_temp_min = 200;
    edit.nozzle_temp_max = 220;
    REQUIRE(helix::test::apply_edit(helper, 0, edit).success());
    CHECK(helper.has_gcode_containing("NAME=PolyTerra"));
    CHECK(helper.has_gcode_containing("TEMP=210"));

    // A cleared name is an explicit empty, and an unchanged temperature is
    // TEMP=0: an omitted TEMP is Happy Hare's default_extruder_temp, which
    // would overwrite the gate's 210.
    helper.clear_captured_gcodes();
    SlotInfo cleared = *helper.get_mutable_slot(0);
    cleared.spool_name.clear();
    REQUIRE(helix::test::apply_edit(helper, 0, cleared).success());
    CHECK(helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL=PLA NAME= TEMP=0"));
}

TEST_CASE_METHOD(HhHelper, "Happy Hare clear: slot stays blank across the status echo",
                 "[ams][happy_hare][clear]") {
    helper.initialize_test_gates(2);

    SlotInfo identity;
    identity.material = "PLA";
    identity.color_rgb = 0xFF0000;
    identity.spoolman_id = 42;
    REQUIRE(helix::test::apply_edit(helper, 0, identity).success());
    helper.clear_captured_gcodes();

    REQUIRE(helix::test::apply_edit(helper, 0, funnel_cleared_copy(*helper.get_mutable_slot(0)))
                .success());
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=0 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));

    // Live: blank.
    const SlotInfo* live = helper.get_mutable_slot(0);
    REQUIRE(live->material.empty());
    REQUIRE(live->color_rgb == helix::kAmsDefaultSlotColor);
    REQUIRE(live->spoolman_id == 0);

    // The firmware's echo of the cleared gate map: still blank. The cleared
    // override record must not repaint anything onto it.
    nlohmann::json echo = {
        {"gate_material", {"", ""}}, {"gate_color", {"", ""}}, {"gate_spool_id", {0, 0}}};
    helper.test_parse_mmu_state(echo);
    const SlotInfo* echoed = helper.get_mutable_slot(0);
    REQUIRE(echoed->material.empty());
    REQUIRE(echoed->color_rgb == helix::kAmsDefaultSlotColor);
    REQUIRE(echoed->spoolman_id == 0);
}

TEST_CASE("Happy Hare editor write: pull mode still saves a pure tool remap", "[ams][happy_hare]") {
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);

    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    REQUIRE(helper.get_system_info().spoolman_mode == SpoolmanMode::PULL);

    SlotInfo edit;
    edit.mapped_tool = 1;
    const AmsError err = helix::test::apply_edit(helper, 0, edit);

    // No gate-map field changed, so the pull-mode refusal must not fire.
    REQUIRE(err.success());
    REQUIRE(helper.has_gcode_containing("MMU_TTG_MAP TOOL=1 GATE=0"));
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
}

TEST_CASE("Happy Hare clear mid-print: the active gate's firmware write is skipped",
          "[ams][happy_hare][clear]") {
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(2);
    helper.set_current_slot(0);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();

    for (int i : {0, 1}) {
        SlotInfo* slot = helper.get_mutable_slot(i);
        REQUIRE(slot != nullptr);
        slot->material = "PLA";
        slot->spoolman_id = 100 + i;
    }

    // The gate feeding the print: local layer cleared, firmware untouched.
    const AmsError active =
        helix::test::apply_edit(helper, 0, funnel_cleared_copy(*helper.get_mutable_slot(0)));
    REQUIRE_FALSE(active.success());
    REQUIRE(active.partially_applied);
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
    REQUIRE(helper.get_mutable_slot(0)->material.empty());

    // Any other gate still clears normally.
    REQUIRE(helix::test::apply_edit(helper, 1, funnel_cleared_copy(*helper.get_mutable_slot(1)))
                .success());
    REQUIRE(
        helper.has_gcode("MMU_GATE_MAP GATE=1 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1"));
}

TEST_CASE("Happy Hare reset_tool_mappings sends one identity MMU_TTG_MAP per tool",
          "[ams][happy_hare][tool_mapping][reset]") {
    for (int gates : {0, 4, 8}) {
        INFO(gates << " gates");
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (gates > 0)
            helper.initialize_test_gates(gates);

        REQUIRE(helper.reset_tool_mappings().success());
        CHECK(helper.captured_gcodes.size() == static_cast<size_t>(gates));
        for (int t = 0; t < gates; ++t) {
            CHECK(helper.has_gcode("MMU_TTG_MAP TOOL=" + std::to_string(t) +
                                   " GATE=" + std::to_string(t)));
        }
    }
}

// Endless spool

TEST_CASE("Happy Hare endless spool edits on a single unit send MMU_ENDLESS_SPOOL",
          "[ams][happy_hare][endless_spool]") {
    using Op = std::function<AmsError(AmsBackendHappyHareTestHelper&)>;
    struct Row {
        const char* name;
        std::vector<int> groups; // preset endless_spool_group per gate; empty = ungrouped
        Op op;
        const char* gcode;
    };
    const Row rows[] = {
        // ENABLE=1 so HH does not skip RESET while endless spool is disabled.
        {"reset",
         {},
         [](auto& h) { return h.reset_endless_spool(); },
         "MMU_ENDLESS_SPOOL ENABLE=1 RESET=1 QUIET=1"},
        // Ungrouped gates take standalone ids 0,1,2,3; joining gate 0 to gate 2's
        // group gives 2,1,2,3. No ENABLE= on an edit: it would persist a state
        // change the user did not ask for.
        {"join gate 2's group",
         {},
         [](auto& h) { return h.set_endless_spool_backup(0, 2); },
         "MMU_ENDLESS_SPOOL QUIET=1 GROUPS=2,1,2,3"},
        // Removing a backup moves the gate to a fresh standalone group (max+1).
        {"remove a backup",
         {0, 0, 1, 1},
         [](auto& h) { return h.set_endless_spool_backup(0, -1); },
         "MMU_ENDLESS_SPOOL QUIET=1 GROUPS=2,0,1,1"},
        {"switch on",
         {},
         [](auto& h) { return h.set_endless_spool_enabled(true); },
         "MMU_ENDLESS_SPOOL ENABLE=1 QUIET=1"},
        {"switch off",
         {},
         [](auto& h) { return h.set_endless_spool_enabled(false); },
         "MMU_ENDLESS_SPOOL ENABLE=0 QUIET=1"},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        for (size_t i = 0; i < row.groups.size(); ++i)
            helper.get_mutable_slot(static_cast<int>(i))->endless_spool_group = row.groups[i];

        const auto caps = helper.get_endless_spool_capabilities();
        CHECK(caps.available());
        CHECK(caps.editable());
        CHECK(caps.switchable);
        CHECK(caps.editability == helix::printer::EndlessSpoolEditability::Group);

        CHECK(row.op(helper).success());
        CHECK(helper.has_gcode(row.gcode));
    }
}

TEST_CASE_METHOD(HhHelper, "Happy Hare endless spool is read-only on multi-unit",
                 "[ams][happy_hare][endless_spool]") {
    helper.initialize_test_gates_multi({4, 4});

    auto caps = helper.get_endless_spool_capabilities();
    CHECK(caps.available());
    CHECK_FALSE(caps.editable());
    CHECK_FALSE(caps.switchable);
    CHECK(caps.restriction == helix::printer::EndlessSpoolRestriction::MultiUnit);

    // MMU_ENDLESS_SPOOL has no UNIT= param, so every edit is refused on multi-unit.
    auto result = helper.set_endless_spool_backup(0, 1);
    CHECK_FALSE(result.success());
    CHECK(result.result == AmsResult::NOT_SUPPORTED);
    CHECK_FALSE(helper.set_endless_spool_enabled(true).success());
    CHECK(helper.captured_gcodes.empty());
}

// Bypass: Happy Hare's selector sits on the bypass position and reports gate -2.
// do_unload_filament() ignores the slot entirely and sends MMU_UNLOAD, so the
// sentinel needs no special handling here — but nothing pinned that, and the UI
// decision layer that DID refuse -2 was fixed by teaching it the sentinel is a
// real target. This asserts the backend half of that contract.
TEST_CASE("Happy Hare unload_filament under bypass sends MMU_UNLOAD", "[ams][happy_hare][bypass]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    helix::test::GcodeRecordingApi api{client, state};
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(&api);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(-2); // bypass sentinel

    auto result = helper.unload_filament(-2);

    REQUIRE(result.success());
    helix::ui::UpdateQueue::instance().drain(); // a pre-op G28 hands off on the queue
    REQUIRE(api.contains("MMU_UNLOAD"));
}

TEST_CASE_METHOD(HhHelper,
                 "Happy Hare unload_filament under bypass still refuses an empty toolhead",
                 "[ams][happy_hare][bypass]") {
    // Bypass being engaged is not itself evidence of filament: the selector can
    // sit on the bypass position with nothing fed. The backend's own guard must
    // still bite, so the UI's affordance and the backend's refusal agree.
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.set_filament_loaded(false);
    helper.set_current_slot(-2);

    auto result = helper.unload_filament(-2);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(helper.has_gcode_containing("MMU_UNLOAD"));
}

TEST_CASE("Happy Hare eject_lane and clear_fault address one gate or refuse",
          "[ams][happy_hare][eject][recovery][slot_index]") {
    // MMU_RECOVER is bookkeeping, not a filament move, so clear_fault is a
    // per-gate fault clear. -1 is the documented "no particular gate": bare
    // MMU_RECOVER re-syncs the whole selector. Zero gates means nothing is
    // discovered yet, which must not blame the index the user picked.
    enum class Op { Eject, ClearFault };
    struct Row {
        const char* name;
        Op op;
        int gates;
        bool running;
        int slot;
        AmsResult result;
        const char* gcode; // sent verbatim on success ("" = none)
    };
    const Row rows[] = {
        {"eject gate 0", Op::Eject, 4, true, 0, AmsResult::SUCCESS, "MMU_EJECT GATE=0"},
        {"eject gate 2", Op::Eject, 4, true, 2, AmsResult::SUCCESS, "MMU_EJECT GATE=2"},
        {"eject out of range", Op::Eject, 4, true, 99, AmsResult::INVALID_SLOT, ""},
        {"eject with no gates", Op::Eject, 0, true, 0, AmsResult::NOT_CONNECTED, ""},
        {"eject when stopped", Op::Eject, 4, false, 0, AmsResult::NOT_CONNECTED, ""},
        {"clear gate 2", Op::ClearFault, 4, true, 2, AmsResult::SUCCESS, "MMU_RECOVER GATE=2"},
        {"clear gate 3", Op::ClearFault, 4, true, 3, AmsResult::SUCCESS, "MMU_RECOVER GATE=3"},
        {"clear out of range", Op::ClearFault, 4, true, 99, AmsResult::INVALID_SLOT, ""},
        {"clear no particular gate", Op::ClearFault, 4, true, -1, AmsResult::SUCCESS,
         "MMU_RECOVER"},
        {"clear when stopped", Op::ClearFault, 4, false, 0, AmsResult::NOT_CONNECTED, ""},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(row.gates);
        helper.set_running(row.running);

        const AmsError err =
            row.op == Op::Eject ? helper.eject_lane(row.slot) : helper.clear_fault(row.slot);
        if (row.result == AmsResult::SUCCESS) {
            CHECK(err.success());
            // Exactly the one command: a gated MMU_RECOVER beside the bare one fails.
            CHECK(helper.captured_gcodes == std::vector<std::string>{row.gcode});
        } else {
            CHECK_FALSE(err.success());
            CHECK(err.result == row.result);
            CHECK(helper.captured_gcodes.empty());
        }
    }
}

TEST_CASE("Happy Hare recover command names only what the user asserted",
          "[ams][happy_hare][recovery]") {
    struct Row {
        int slot;
        bool bypass;
        std::optional<bool> loaded;
        const char* expected;
    };
    // clang-format off
    const Row rows[] = {
        {-1, false, std::nullopt, "MMU_RECOVER"},
        {-1, false, false,        "MMU_RECOVER LOADED=0"},
        {-1, false, true,         "MMU_RECOVER LOADED=1"},
        { 1, false, std::nullopt, "MMU_RECOVER GATE=1"},
        { 1, false, false,        "MMU_RECOVER GATE=1 LOADED=0"},
        { 1, false, true,         "MMU_RECOVER GATE=1 LOADED=1"},
        { 0, false, std::nullopt, "MMU_RECOVER GATE=0"},
        // Bypass replaces the gate: HH forces tool and gate to bypass itself.
        {-1, true,  std::nullopt, "MMU_RECOVER BYPASS=1"},
        { 1, true,  false,        "MMU_RECOVER BYPASS=1 LOADED=0"},
        { 1, true,  true,         "MMU_RECOVER BYPASS=1 LOADED=1"},
    };
    // clang-format on
    for (const auto& row : rows) {
        INFO("slot=" << row.slot << " bypass=" << row.bypass);
        CHECK(helix::AmsBackendHappyHare::build_recover_command(
                  recover_req(row.slot, row.bypass, row.loaded)) == row.expected);
    }
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare recover_with_state sends the built command",
                 "[ams][happy_hare][recovery]") {
    helper.set_running(true);
    helper.set_supports_bypass(true);

    CHECK(helper.supports_recover_with_state());

    REQUIRE(helper.recover_with_state(recover_req(2, false, true)).success());
    REQUIRE(helper.recover_with_state(recover_req(-1, true, std::nullopt)).success());
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_RECOVER GATE=2 LOADED=1", "MMU_RECOVER BYPASS=1"});
}

TEST_CASE_METHOD(HhFourGates, "Happy Hare recover_with_state refuses what the MMU cannot be in",
                 "[ams][happy_hare][recovery]") {
    SECTION("not running") {
        CHECK_FALSE(helper.recover_with_state(recover_req(0, false, true)).success());
    }
    SECTION("gate out of range") {
        helper.set_running(true);
        CHECK(helper.recover_with_state(recover_req(4, false, std::nullopt)).result ==
              AmsResult::INVALID_SLOT);
    }
    SECTION("bypass on an MMU without one") {
        helper.set_running(true);
        helper.set_supports_bypass(false);
        CHECK_FALSE(helper.recover_with_state(recover_req(-1, true, std::nullopt)).success());
    }
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare error-popup Recover lets HH detect the filament position",
          "[ams][happy_hare][recovery]") {
    // Our loaded flag reads false for every position HH calls Unknown, so
    // echoing it as LOADED=0 would tell HH "unloaded" about filament stuck
    // mid-bowden. Bare MMU_RECOVER has HH read its own sensors.
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> hh_reg;
    AmsBackendHappyHareTestHelper& hh = *hh_reg;
    hh.initialize_test_gates(4);
    struct Pos {
        int filament_pos;
        const char* filament;
    };
    const Pos pos = GENERATE(Pos{8, "Loaded"}, Pos{0, "Unloaded"}, Pos{3, "Unknown"});
    nlohmann::json mmu;
    mmu["action"] = "Idle";
    mmu["print_state"] = "pause_locked";
    mmu["filament_pos"] = pos.filament_pos;
    mmu["filament"] = pos.filament;
    mmu["reason_for_pause"] = "Clog detected";
    hh.test_parse_mmu_state(mmu);

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto ev = hh.classify_error("!! Clog detected", ctx);
    REQUIRE(ev.has_value());
    std::string recover_gcode;
    for (const auto& a : ev->recovery_actions)
        if (a.log_tag == "hh::recover")
            recover_gcode = a.gcode;
    INFO(pos.filament);
    CHECK(recover_gcode == "MMU_RECOVER");
}

// preload_lane() Tests

TEST_CASE_METHOD(HhFourGates, "Happy Hare preload_lane sends MMU_PRELOAD for the gate",
                 "[ams][happy_hare][preload]") {
    helper.set_running(true);

    CHECK(helper.supports_lane_preload());
    REQUIRE(helper.preload_lane(2).success());
    CHECK(helper.captured_gcodes == std::vector<std::string>{"MMU_PRELOAD GATE=2"});

    helper.clear_captured_gcodes();
    CHECK(helper.preload_lane(4).result == AmsResult::INVALID_SLOT);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare preload_lane is refused while printing", "[ams][happy_hare][preload]") {
    // Happy Hare's MMU_PRELOAD opens with check_if_printing().
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);

    CHECK(helper.preload_lane(1).result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());
}

// mark_gate_available(): the recovery Happy Hare's failed-pickup error names

TEST_CASE_METHOD(HhFourGates, "Happy Hare marks an empty gate available with MMU_GATE_MAP",
                 "[ams][happy_hare][mark_available]") {
    helper.set_running(true);
    helper.feed_mmu_gate_status({1, 0, -1, 2});

    CHECK_FALSE(helper.gate_marked_empty(0));
    CHECK(helper.gate_marked_empty(1));
    CHECK_FALSE(helper.gate_marked_empty(2)); // unknown is not empty
    CHECK_FALSE(helper.gate_marked_empty(3));
    CHECK_FALSE(helper.gate_marked_empty(4));
    CHECK_FALSE(helper.gate_marked_empty(-1));

    REQUIRE(helper.mark_gate_available(1).success());
    // TEMP=0 keeps the gate's temperature; an omitted TEMP resets it.
    CHECK(helper.captured_gcodes ==
          std::vector<std::string>{"MMU_GATE_MAP GATE=1 AVAILABLE=1 TEMP=0 QUIET=1"});

    helper.clear_captured_gcodes();
    CHECK(helper.mark_gate_available(4).result == AmsResult::INVALID_SLOT);
    CHECK(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare mark available waits for a running print to pause",
          "[ams][happy_hare][mark_available]") {
    PrintLifecycleFixture fx;
    helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg(fx.api.get(),
                                                                             &fx.mock_client);
    AmsBackendHappyHareTestHelper& helper = *helper_reg;
    helper.initialize_test_gates(4);
    helper.set_running(true);
    helper.feed_mmu_gate_status({1, 0, 1, 1});

    helix::test::set_wire_state(fx.state, helix::PrintJobState::PRINTING);
    CHECK(helper.mark_gate_available(1).result == AmsResult::WRONG_STATE);
    CHECK(helper.captured_gcodes.empty());

    helix::test::set_wire_state(fx.state, helix::PrintJobState::PAUSED);
    REQUIRE(helper.mark_gate_available(1).success());
    CHECK(helper.has_gcode_containing("MMU_GATE_MAP GATE=1 AVAILABLE=1"));
}

// Gate weights (Happy Hare 4.2): the gate map holds remaining and initial weight

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare 4.2 gate weights reach the slot as the firmware's",
                 "[ams][happy_hare][gate_weights]") {
    connect_with_fixture(helper, client, happy_hare_fixture_at("4.2.0"));
    helper.feed_mmu_status({{"gate_remaining_weight", {750, -1, 0, 120}},
                            {"gate_initial_weight", {1000, -1, 1000, -1}}});

    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0F));
    CHECK(helper.get_slot_info(0).total_weight_g == Catch::Approx(1000.0F));
    CHECK(helper.get_slot_info(2).remaining_weight_g == Catch::Approx(0.0F));
    CHECK(helper.get_slot_info(3).remaining_weight_g == Catch::Approx(120.0F));
    // -1 is Happy Hare not knowing: no firmware weight on the lane.
    const auto firmware = [&](int gate) {
        const auto sources = helix::ams::lane_sources(helper.lane_id(gate));
        REQUIRE(sources.firmware.has_value());
        return *sources.firmware;
    };
    CHECK_FALSE(firmware(1).remaining_weight_g);
    CHECK_FALSE(firmware(3).total_weight_g);
    CHECK(firmware(0).remaining_weight_g == Catch::Approx(750.0F));

    // A gate whose weight Happy Hare forgets drops the firmware's figure.
    helper.feed_mmu_status({{"gate_remaining_weight", {-1, -1, 0, 120}}});
    CHECK_FALSE(firmware(0).remaining_weight_g);

    // Clear Spool names the weights: a gate with no spool id keeps them otherwise.
    helper.get_mutable_slot(3)->material = "PLA";
    helix::test::apply_edit(helper, 3, funnel_cleared_copy(helper.get_slot_info(3)));
    CHECK(helper.has_gcode("MMU_GATE_MAP GATE=3 MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 "
                           "WEIGHT=-1 INITIAL_WEIGHT=-1 QUIET=1"));
}

TEST_CASE_METHOD(HhFourGatesWithClient,
                 "Happy Hare 4.2 in Spoolman pull mode refuses a weight edit",
                 "[ams][happy_hare][gate_weights]") {
    connect_with_fixture(helper, client, happy_hare_fixture_at("4.2.0"));
    helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
    SlotInfo edit = helper.get_slot_info(1);
    edit.remaining_weight_g = 300.0F;

    const AmsError err = helix::test::apply_edit(helper, 1, edit);
    CHECK(err.result == AmsResult::COMMAND_FAILED);
    CHECK_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
}

TEST_CASE_METHOD(HhFourGatesWithClient, "Happy Hare before 4.2 keeps weights beside the gate map",
                 "[ams][happy_hare][gate_weights]") {
    connect_with_fixture(helper, client, happy_hare_fixture_at("4.1.0"));
    SECTION("an edit") {
        SlotInfo edit = helper.get_slot_info(1);
        edit.remaining_weight_g = 300.0F;
        REQUIRE(helix::test::apply_edit(helper, 1, edit).success());
    }
    SECTION("a metered weight") {
        helper.persist_slot_weight(1, 280.0F, 1000.0F);
    }
    CHECK_FALSE(helper.has_gcode_containing("MMU_GATE_MAP"));
}

TEST_CASE_METHOD(HhFourGatesWithClient, "Happy Hare 4.2 writes a metered weight to the gate map",
                 "[ams][happy_hare][gate_weights]") {
    connect_with_fixture(helper, client, happy_hare_fixture_at("4.2.0"));

    SECTION("an unlinked gate") {
        helper.persist_slot_weight(1, 280.4F, 1000.0F);
        CHECK(helper.captured_gcodes ==
              std::vector<std::string>{"MMU_GATE_MAP GATE=1 WEIGHT=280 INITIAL_WEIGHT=1000 "
                                       "TEMP=0 QUIET=1"});
    }
    SECTION("a gate linked to a spool, whose weight is Spoolman's") {
        helper.get_mutable_slot(1)->spoolman_id = 7;
        helper.persist_slot_weight(1, 280.0F, 1000.0F);
        CHECK(helper.captured_gcodes.empty());
    }
    SECTION("pull mode") {
        helper.test_parse_mmu_state(nlohmann::json{{"spoolman_support", "pull"}});
        helper.persist_slot_weight(1, 280.0F, 1000.0F);
        CHECK(helper.captured_gcodes.empty());
    }
}

// form_tip_macro: empty means no tip forming from Happy Hare 4.2

TEST_CASE_METHOD(HhHelperWithClient, "Happy Hare empty form_tip_macro shows no tip step from 4.2",
                 "[ams][happy_hare][tip_method]") {
    helper.initialize_test_gates(4);
    const auto tip_method_for = [&](const std::string& version, const std::string& macro) {
        nlohmann::json fx = happy_hare_fixture_at(version);
        fx["configfile_settings"]["mmu_parameters"]["form_tip_macro"] = macro;
        connect_with_fixture(helper, client, fx);
        return helper.get_system_info().tip_method;
    };
    CHECK(tip_method_for("4.2.0", "") == TipMethod::NONE);
    CHECK(tip_method_for("4.2.0", "_MMU_FORM_TIP") == TipMethod::TIP_FORM);
    CHECK(tip_method_for("4.2.0", "_MMU_CUT_TIP") == TipMethod::CUT);
    // Before 4.2 an empty macro made Happy Hare fail the unload looking for it.
    CHECK(tip_method_for("4.1.0", "") == TipMethod::TIP_FORM);
}

// printer.mmu.drying: the cycle Happy Hare is running, whoever started it

TEST_CASE_METHOD(HhFourGates, "Happy Hare drying cycle reports its time left from end_time",
                 "[ams][happy_hare][dryer][drying_cycle]") {
    std::time_t wall = 1'000'000;
    helper.test_set_clock([&wall] { return wall; });
    helper.test_parse_mmu_state({{"drying_state", {{"active", false}}}});
    const auto feed = [&](const nlohmann::json& mmu, double eventtime) {
        helper.feed_mmu_status_at(mmu, eventtime);
    };

    SECTION("a cycle started elsewhere") {
        // Started at reactor 1000 s for 240 min; this frame is a minute in.
        feed({{"drying",
               {{{"temp", 55},
                 {"time", 240},
                 {"humidity_target", nullptr},
                 {"start_time", 1000.0},
                 {"end_time", 1000.0 + 240 * 60}}}}},
             1060.0);
        CHECK(helper.get_dryer_info(0).remaining_min == 239);
        CHECK(helper.get_dryer_info(0).duration_min == 240);
        // Ten minutes later on the wall clock, with no frame in between.
        wall += 600;
        CHECK(helper.get_dryer_info(0).remaining_min == 229);
    }
    SECTION("the subscription's snapshot carries no eventtime; the next frame does") {
        feed({{"drying", {{{"time", 60}, {"start_time", 50.0}, {"end_time", 3650.0}}}}}, 0.0);
        feed({{"action", "Idle"}}, 650.0);
        CHECK(helper.get_dryer_info(0).remaining_min == 50);
    }
    SECTION("no cycle, or a Happy Hare without the field, keeps HelixScreen's own clock") {
        if (GENERATE(true, false)) {
            feed({{"drying", {nullptr}}}, 500.0);
        }
        REQUIRE(helper.start_drying(50.0f, 90, -1, 0).success());
        wall += 30 * 60;
        CHECK(helper.get_dryer_info(0).remaining_min == 60);
    }
}

TEST_CASE_METHOD(HhHelper,
                 "Happy Hare advertises lane eject, gate select and gate check; Reset reads Home",
                 "[ams][happy_hare][capability]") {
    CHECK(helper.supports_lane_eject());
    CHECK(helper.supports_gate_select());
    CHECK(helper.supports_gate_check());
    CHECK(helper.reset_button_label() == "Home");
}

TEST_CASE("Happy Hare per-gate arrays land on their own gates and skip unreadable entries",
          "[ams][happy_hare][v4][edge]") {
    // A short array leaves the remaining gates alone; a wrong-typed entry
    // leaves its gate alone; a negative spool id is Happy Hare's "unlinked".
    enum class Field { Temp, Name, Spool };
    struct Row {
        const char* name;
        Field field;
        int gates;
        std::vector<nlohmann::json> frames;
        std::vector<std::string> expect;
    };
    using J = nlohmann::json;
    const Row rows[] = {
        {"temperatures",
         Field::Temp,
         4,
         {J{{"gate_temperature", {210, 220, 230, 240}}}},
         {"210", "220", "230", "240"}},
        {"temperatures of the wrong type",
         Field::Temp,
         4,
         {J{{"gate_temperature", {210, "not_a_number", 230, nullptr}}}},
         {"210", "0", "230", "0"}},
        {"fewer temperatures than gates",
         Field::Temp,
         8,
         {J{{"gate_temperature", {200, 210, 220, 230}}}},
         {"200", "210", "220", "230", "0", "0", "0", "0"}},
        {"names",
         Field::Name,
         4,
         {J{{"gate_name", {"Red PLA", "Blue PETG", "Black ABS", ""}}}},
         {"Red PLA", "Blue PETG", "Black ABS", ""}},
        {"all names empty", Field::Name, 4, {J{{"gate_name", {"", "", "", ""}}}}, {"", "", "", ""}},
        {"spool ids",
         Field::Spool,
         4,
         {J{{"gate_spool_id", {5, 0, 12, 99}}}},
         {"5", "0", "12", "99"}},
        {"spool ids of the wrong type",
         Field::Spool,
         3,
         {J{{"gate_spool_id", {"bad", 7, nullptr}}}},
         {"0", "7", "0"}},
        {"a negative spool id unlinks",
         Field::Spool,
         2,
         {J{{"gate_spool_id", {10, 20}}}, J{{"gate_spool_id", {-1, 20}}}},
         {"0", "20"}},
        {"fewer spool ids than gates",
         Field::Spool,
         4,
         {J{{"gate_spool_id", {3, 8}}}},
         {"3", "8", "0", "0"}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(row.gates);
        for (const auto& frame : row.frames) {
            helper.test_parse_mmu_state(frame);
        }
        REQUIRE(helper.get_system_info().total_slots == row.gates);
        for (int g = 0; g < row.gates; ++g) {
            INFO("gate " << g);
            const SlotInfo slot = helper.get_slot_info(g);
            switch (row.field) {
            case Field::Temp:
                CHECK(std::to_string(slot.nozzle_temp_min) == row.expect[g]);
                CHECK(slot.nozzle_temp_max == slot.nozzle_temp_min);
                break;
            case Field::Name:
                CHECK(slot.color_name == row.expect[g]);
                break;
            case Field::Spool:
                CHECK(std::to_string(slot.spoolman_id) == row.expect[g]);
                break;
            }
        }
    }
}

TEST_CASE_METHOD(HhFourGates,
                 "Happy Hare bowden_progress is clamped to -1..100 and ignores non-integers",
                 "[ams][happy_hare][v4][bowden][edge]") {
    CHECK(helper.get_bowden_progress() == -1); // not available until reported

    // Applied in order: a string leaves the previous value standing.
    const std::pair<nlohmann::json, int> steps[] = {
        {75, 75}, {-1, -1}, {0, 0}, {100, 100}, {50, 50}, {"invalid", 50}, {150, 100}, {-5, -1},
    };
    for (const auto& [value, expected] : steps) {
        INFO("bowden_progress " << value.dump());
        helper.test_parse_mmu_state({{"bowden_progress", value}});
        CHECK(helper.get_bowden_progress() == expected);
    }
}

TEST_CASE("SpoolmanMode reads Happy Hare's spellings and defaults to OFF",
          "[ams][happy_hare][v4][edge]") {
    const std::pair<const char*, SpoolmanMode> rows[] = {
        {"off", SpoolmanMode::OFF},           {"Off", SpoolmanMode::OFF},
        {"readonly", SpoolmanMode::READONLY}, {"Read Only", SpoolmanMode::READONLY},
        {"push", SpoolmanMode::PUSH},         {"Push", SpoolmanMode::PUSH},
        {"pull", SpoolmanMode::PULL},         {"Pull", SpoolmanMode::PULL},
        {"PUSH", SpoolmanMode::OFF},          {"", SpoolmanMode::OFF},
        {"sync", SpoolmanMode::OFF},          {"unknown", SpoolmanMode::OFF},
    };
    for (const auto& [text, mode] : rows) {
        INFO("spoolman_support \"" << text << "\"");
        CHECK(spoolman_mode_from_string(text) == mode);
    }
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::OFF)) == "Off");
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::PUSH)) == "Push");
    CHECK(std::string(spoolman_mode_to_string(SpoolmanMode::PULL)) == "Pull");
}

TEST_CASE("Happy Hare v4 status fields: values, defaults, wrong types and deltas",
          "[ams][happy_hare][v4][status][edge]") {
    // Each field keeps its default against a frame that omits it or sends the
    // wrong type, and frames merge as deltas.
    using J = nlohmann::json;
    struct Expect {
        std::string espooler;
        std::string sync_state;
        bool sync_drive;
        int clog;
        int flow;
        float purge;
        SpoolmanMode spoolman;
        int pending;
    };
    const Expect defaults{"", "", false, 0, -1, 0.0f, SpoolmanMode::OFF, -1};
    struct Row {
        const char* name;
        std::vector<J> frames;
        Expect expect;
    };
    const Row rows[] = {
        {"every field",
         {J{{"espooler_active", "rewind"},
            {"sync_feedback_state", "tension"},
            {"sync_drive", true},
            {"clog_detection_enabled", 2},
            {"encoder", {{"flow_rate", 95}}},
            {"toolchange_purge_volume", 150.5}}},
         {"rewind", "tension", true, 2, 95, 150.5f, SpoolmanMode::OFF, -1}},
        {"a v3 frame", {J{{"gate_status", {1, 1, 1, 1}}}}, defaults},
        {"spoolman fields",
         {J{{"spoolman_support", "pull"}, {"pending_spool_id", 42}}},
         {"", "", false, 0, -1, 0.0f, SpoolmanMode::PULL, 42}},
        {"wrong types",
         {J{{"espooler_active", 42},
            {"sync_feedback_state", true},
            {"sync_drive", "yes"},
            {"clog_detection_enabled", "2"},
            {"encoder", "not_object"},
            {"toolchange_purge_volume", "big"}}},
         defaults},
        {"encoder without flow_rate", {J{{"encoder", {{"some_other_field", 42}}}}}, defaults},
        {"a float flow_rate truncates",
         {J{{"encoder", {{"flow_rate", 95.7}}}}},
         {"", "", false, 0, 95, 0.0f, SpoolmanMode::OFF, -1}},
        {"separate frames merge",
         {J{{"espooler_active", "rewind"}}, J{{"clog_detection_enabled", 1}}},
         {"rewind", "", false, 1, -1, 0.0f, SpoolmanMode::OFF, -1}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        helper.initialize_test_gates(4);
        for (const auto& frame : row.frames) {
            helper.test_parse_mmu_state(frame);
        }
        const auto info = helper.get_system_info();
        CHECK(info.espooler_state == row.expect.espooler);
        CHECK(info.sync_feedback_state == row.expect.sync_state);
        CHECK(info.sync_drive == row.expect.sync_drive);
        CHECK(info.clog_detection == row.expect.clog);
        CHECK(info.encoder_flow_rate == row.expect.flow);
        CHECK(info.toolchange_purge_volume == Catch::Approx(row.expect.purge));
        CHECK(info.spoolman_mode == row.expect.spoolman);
        CHECK(info.pending_spool_id == row.expect.pending);
    }
}

TEST_CASE("Happy Hare splits gates across units from per-unit counts or evenly",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    // Per-unit counts (unit_gate_counts, else a num_gates string) apply only
    // when they sum to the gate count and match num_units; otherwise the
    // gates split evenly. num_units below 1 means one unit.
    using J = nlohmann::json;
    struct Row {
        const char* name;
        J setup; // sent first; null = none
        J frame;
        std::vector<int> counts;
    };
    const std::vector<int> g8(8, 1), g10(10, 1);
    const Row rows[] = {
        {"num_gates string",
         J{{"num_units", 2}},
         J{{"num_gates", "6,4"}, {"gate_status", {1, 1, 0, 1, 1, 1, 1, 0, 1, 1}}},
         {6, 4}},
        {"no per-unit counts", J{{"num_units", 2}}, J{{"gate_status", g8}}, {4, 4}},
        {"counts that do not sum to the gates",
         J{{"num_units", 2}},
         J{{"num_gates", "6,4"}, {"gate_status", g8}},
         {4, 4}},
        {"unit_gate_counts outranks num_gates",
         J{{"num_units", 2}},
         J{{"num_gates", "5,5"}, {"unit_gate_counts", {3, 7}}, {"gate_status", g10}},
         {3, 7}},
        {"an unreadable token",
         J{{"num_units", 2}},
         J{{"num_gates", "6,abc"}, {"gate_status", g8}},
         {4, 4}},
        {"one unit", J(), J{{"num_units", 1}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"num_units 0", J(), J{{"num_units", 0}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"num_units -1", J(), J{{"num_units", -1}, {"gate_status", {1, 1, 1, 1}}}, {4}},
        {"with an active unit",
         J(),
         J{{"num_units", 2}, {"unit", 1}, {"gate_status", {1, 1, 1, 1, 0, 0, 0, 0}}},
         {4, 4}},
        {"num_gates as a one-entry array", J(), J{{"gate_status", g8}, {"num_gates", {8}}}, {8}},
        {"num_gates as a per-unit array",
         J{{"num_units", 2}},
         J{{"gate_status", g10}, {"num_gates", {6, 4}}},
         {6, 4}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        helix::test::RegisteredBackend<AmsBackendHappyHareTestHelper> helper_reg;
        AmsBackendHappyHareTestHelper& helper = *helper_reg;
        if (!row.setup.is_null())
            helper.test_parse_mmu_state(row.setup);
        helper.test_parse_mmu_state(row.frame);

        const auto info = helper.get_system_info();
        REQUIRE(info.units.size() == row.counts.size());
        int first = 0;
        for (size_t u = 0; u < row.counts.size(); ++u) {
            INFO("unit " << u);
            CHECK(info.units[u].slot_count == row.counts[u]);
            CHECK(info.units[u].first_slot_global_index == first);
            first += row.counts[u];
        }
        CHECK(info.total_slots == first);
        if (row.counts.size() == 1) {
            CHECK(info.units[0].name == "MMU");
            // "MMU" is a fallback, not a name to show in place of the system's.
            CHECK(info.units[0].display_name.empty());
        }
    }
}
