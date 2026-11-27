// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Cases 1 to 46 of test_ams_backend_ad5x_ifs.cpp before it was split, in its order. Mostly
// [ad5x_ifs], [general], [ad5x_ifs][clear_fault], [ad5x_ifs][phase], [ad5x][ifs][spoolman][1071],
// and more

#include "ui_insert_notice.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_fixtures.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_slot_override_store_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/unique_temp_dir.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::kAmsDefaultSlotColor;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::StepOperationType;

using helix::test::Ad5xHomingGuardBackend;
using helix::test::Ad5xHomingGuardFixture;
using helix::test::Ad5xIfsTmpCacheDir;
using helix::test::Ad5xIfsTmpJsonFile;
using helix::test::Ad5xRunoutFixture;
using helix::test::Ad5xRunoutOpBackend;
using helix::test::Ad5xRunoutOpFixture;
using helix::test::boot_from_lane_data;
using helix::test::drain_queue;
using helix::test::firmware_states_slot0;
using helix::test::FrameRig;
using helix::test::GcodeCapturingBackend;
using helix::test::identity_tool_map;
using helix::test::IfsLaneTruth;
using helix::test::json;
using helix::test::kZmodMaterials;
using helix::test::kZmodPalette;
using helix::test::lane_record_reread_lands;
using helix::test::less_waste_vars;
using helix::test::lesswaste_frame;
using helix::test::lesswaste_ready;
using helix::test::LiveAckAd5xIfsBackend;
using helix::test::macro_changes_state;
using helix::test::macro_error;
using helix::test::make_auto_mirror_override;
using helix::test::make_extruder;
using helix::test::make_head_sensor;
using helix::test::make_ifs_frame;
using helix::test::make_ifs_vars_macro;
using helix::test::make_locked_override;
using helix::test::make_motion_sensor;
using helix::test::make_port_sensor;
using helix::test::make_routing_backend;
using helix::test::make_save_variables;
using helix::test::make_zcolor;
using helix::test::make_zmod_head_sensor;
using helix::test::make_zmod_motion_sensor;
using helix::test::material_button;
using helix::test::outside_lane_record;
using helix::test::outside_tool_change_zcolor;
using helix::test::palette_button;
using helix::test::plugin_rig_frame;
using helix::test::QuietRunningAd5xIfs;
using helix::test::RegisteredAd5xIfs;
using helix::test::RunningAd5xIfs;
using helix::test::seat_slot0;
using helix::test::seat_unrecorded_zmod;
using helix::test::seed_standard_colors;
using helix::test::slot_row;
using helix::test::standard_variables;
using helix::test::TestableAd5xIfsBackend;
using helix::test::unload_routed_to_toolhead;
using helix::test::WiredAd5xIfs;
using helix::test::wrap_notification;
using helix::test::zmod_change_frame;
using json = nlohmann::json;

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS type identification", "[ams][ad5x_ifs]") {
    REQUIRE(backend.get_topology() == PathTopology::LINEAR);
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS: manages_active_spool() is true so the UI never auto-writes Spoolman",
                 "[ams][ad5x][ifs][spoolman][1071]") {
    CHECK(backend.manages_active_spool() == true);
}

// 2. parse_save_variables — full JSON

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS parse_save_variables full JSON", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    backend.fire_status(make_save_variables(standard_variables()));

    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));

    // Color/material seeded separately — parse_save_variables does not write
    // colors_[]/materials_[] anymore (those live in zmod's authoritative state,
    // not in lessWaste/bambufy's private namespace).
    seed_standard_colors(backend);
    backend.fire_status(make_save_variables(standard_variables()));

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF0000);
    REQUIRE(info.material == "PLA");
    REQUIRE(info.mapped_tool == 0); // Tool 0 maps to port 1 (slot 0)

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.color_rgb == 0x00FF00);
    REQUIRE(info1.material == "PETG");
    REQUIRE(info1.mapped_tool == 1);

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.color_rgb == 0x0000FF);
    REQUIRE(info2.material == "ABS");
    REQUIRE(info2.mapped_tool == 2);

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.color_rgb == 0xFFFFFF);
    REQUIRE(info3.material == "TPU");
    REQUIRE(info3.mapped_tool == 3);
}

// 3. parse_save_variables with -1 active tool

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS no active tool", "[ams][ad5x_ifs]") {
    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;

    backend.fire_status(make_save_variables(vars));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == -1);
    REQUIRE(sys.current_slot == -1);
    REQUIRE_FALSE(sys.filament_loaded);
}

// 4. Color hex parsing

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS color hex parsing", "[ams][ad5x_ifs]") {
    SECTION("lowercase hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("mixed case hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "Ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("empty string leaves color unchanged") {
        // Seed slot 1 with a known color, then attempt to overwrite slot 0
        // with empty hex — update_slot_from_state's stoul fallback skips the
        // parse, so other slots are unaffected.
        Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
        Ad5xIfsTestAccess::set_color(backend, 0, "");

        auto info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x00FF00);
    }
}

// 5. Tool mapping reverse lookup

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS tool mapping reverse lookup", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("standard 1:1 mapping") {
        backend.fire_status(make_save_variables(standard_variables()));

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.mapped_tool == i);
        }
    }

    SECTION("non-standard mapping: T0->port3, T1->port1") {
        auto vars = standard_variables();
        // T0->3, T1->1, T2->5(unmapped), T3->2, rest unmapped
        vars["less_waste_tools"] = json::array({3, 1, 5, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        backend.fire_status(make_save_variables(vars));

        // Slot 0 (port 1): first tool mapping to port 1 is T1
        REQUIRE(backend.get_slot_info(0).mapped_tool == 1);
        // Slot 1 (port 2): first tool mapping to port 2 is T3
        REQUIRE(backend.get_slot_info(1).mapped_tool == 3);
        // Slot 2 (port 3): first tool mapping to port 3 is T0
        REQUIRE(backend.get_slot_info(2).mapped_tool == 0);
        // Slot 3 (port 4): no tool maps to port 4
        REQUIRE(backend.get_slot_info(3).mapped_tool == -1);
    }
}

TEST_CASE("AD5X IFS applies one frame's objects together", "[ams][ad5x_ifs]") {
    FrameRig rig;
    json frame = plugin_rig_frame();

    SECTION("a plugin rig's whole frame") {
        rig.deliver(frame);
        const auto sys = rig.backend.get_system_info();
        CHECK(sys.current_tool == 1);
        CHECK(sys.current_slot == 0); // less_waste_tools routes T1 to port 1
        CHECK(sys.filament_loaded);   // the switch reads after the motion sensor
        CHECK(sys.endless_spool_enabled);
        CHECK(sys.units[0].slots[0].mapped_tool == 1);
        CHECK(sys.units[0].slots[1].mapped_tool == 0);
        CHECK(Ad5xIfsTestAccess::port_presence(rig.backend, 0));
        CHECK_FALSE(Ad5xIfsTestAccess::port_presence(rig.backend, 1));
        const auto materials = rig.backend.get_supported_materials();
        REQUIRE(materials);
        CHECK(std::find(materials->begin(), materials->end(), "PLA+") != materials->end());
        CHECK(rig.events == 1);
    }

    SECTION("the wire tool_map outranks the plugin's table in the same frame") {
        frame["ifs"] = json{{"tool_map", identity_tool_map()}};
        rig.deliver(frame);
        CHECK(Ad5xIfsTestAccess::tool_map(rig.backend)[1] == 2);
        CHECK(rig.backend.get_system_info().current_slot == 1);
        CHECK(rig.backend.remap_ready());
    }

    SECTION("the motion sensor alone speaks for the head") {
        frame.erase("zmod_ifs_switch_sensor head_switch_sensor");
        rig.deliver(frame);
        CHECK_FALSE(rig.backend.get_system_info().filament_loaded);
    }

    SECTION("conflicting plugin tables drop the rows and leave the sensors") {
        frame["save_variables"]["variables"]["bambufy_tools"] = json::array({4, 2, 4, 3});
        rig.deliver(frame);
        const auto sys = rig.backend.get_system_info();
        CHECK(sys.current_tool == -1);
        CHECK(Ad5xIfsTestAccess::tool_map(rig.backend)[0] == 1);
        CHECK(sys.filament_loaded);
        CHECK(sys.endless_spool_enabled);
    }
}

TEST_CASE("AD5X IFS reads a frame's plugin rows under the prefix it knows", "[ams][ad5x_ifs]") {
    FrameRig rig;
    rig.deliver(make_save_variables(json{{"bambufy_tools", json::array({1, 2, 3, 4})}}));
    REQUIRE(Ad5xIfsTestAccess::var_prefix(rig.backend) == "bambufy");

    rig.deliver(
        make_save_variables(json{{"bambufy_current_tool", 2}, {"less_waste_current_tool", 3}}));
    CHECK(Ad5xIfsTestAccess::active_tool(rig.backend) == 2);
}

TEST_CASE("AD5X IFS tool_map frames publish only a changed table", "[ams][ad5x_ifs]") {
    FrameRig rig;
    const json table = {{"0", 2}, {"1", "3"}, {"2", 9}, {"3", 4}, {"99", 1}, {"x", 1}};
    rig.deliver(json{{"ifs", {{"tool_map", table}}}});
    const auto map = Ad5xIfsTestAccess::tool_map(rig.backend);
    CHECK(map[0] == 2);
    CHECK(map[1] == AmsBackendAd5xIfs::kUnmappedPort);
    CHECK(map[2] == AmsBackendAd5xIfs::kUnmappedPort);
    CHECK(map[3] == 4);
    CHECK(rig.events == 1);

    rig.deliver(json{{"ifs", {{"tool_map", table}}}});
    CHECK(rig.events == 1);
    CHECK(rig.backend.remap_ready());
}

TEST_CASE("AD5X IFS frames that state nothing publish nothing", "[ams][ad5x_ifs]") {
    FrameRig rig;
    const auto map_before = Ad5xIfsTestAccess::tool_map(rig.backend);
    rig.deliver(GENERATE(
        json{{"save_variables", json::object()}}, json{{"save_variables", {{"variables", 3}}}},
        json{{"save_variables", 7}}, json{{"gcode_macro _ifs_vars", json::object()}},
        json{{"gcode_macro _ifs_vars", {{"variable_backup", "on"}}}}, json{{"ifs", json::object()}},
        json{{"ifs", 3}}, json{{"ifs", {{"tool_map", 1}}}},
        json{{"filament_switch_sensor head_switch_sensor", json::object()}},
        json{{"extruder", {{"temperature", 20.0}}}}));
    CHECK(rig.events == 0);
    CHECK(Ad5xIfsTestAccess::tool_map(rig.backend) == map_before);
    CHECK_FALSE(rig.backend.remap_ready());
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS reports a toolhead sensor once its head switch publishes",
                 "[ams][ad5x_ifs][toolhead_sensor]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};

    SECTION("motion sensor only") {
        backend.fire_status(notification);
        auto sys = backend.get_system_info();
        REQUIRE(sys.units.size() == 1);
        CHECK_FALSE(sys.units[0].has_toolhead_sensor);
    }
    SECTION("head switch present") {
        notification.update(make_head_sensor(false));
        backend.fire_status(notification);
        auto sys = backend.get_system_info();
        REQUIRE(sys.units.size() == 1);
        CHECK(sys.units[0].has_toolhead_sensor);
    }
}

// 9. get_system_info

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS get_system_info", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    backend.fire_status(make_save_variables(standard_variables()));

    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.type_name == "AD5X IFS");
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
    // No bypass on this machine: every path into the hub runs through an
    // IFS lane; there is no external direct-feed spool entry.
    REQUIRE_FALSE(sys.supports_bypass);
    // The ENABLE bit; AVAILABILITY lives in get_endless_spool_capabilities().
    REQUIRE_FALSE(sys.endless_spool_enabled);
    REQUIRE_FALSE(sys.supports_purge);

    // IFS tool mapping: 16 entries (tool→slot), first 4 mapped, rest unmapped
    REQUIRE(sys.tool_to_slot_map.size() == 16);
    REQUIRE(sys.tool_to_slot_map[0] == 0);
    REQUIRE(sys.tool_to_slot_map[1] == 1);
    REQUIRE(sys.tool_to_slot_map[2] == 2);
    REQUIRE(sys.tool_to_slot_map[3] == 3);
    for (size_t i = 4; i < 16; ++i) {
        REQUIRE(sys.tool_to_slot_map[i] == -1);
    }
}

// 9b. Tool remapping — two firmware stacks, two adapters
//
// Forge-X port: the standalone [ifs] module owns the slicer-tool -> lane
// table and echoes it live in printer.ifs.tool_map. Every frame carries the
// COMPLETE table; keys are string tool numbers (0-based, the T<n> a slicer
// writes) and values are 1-based lanes (the number IFS_LOAD SLOT= speaks).
// Writes are IFS_MAP_TOOL verbs; the echo promotes them.
//
// ZMOD: the lessWaste/bambufy plugins expose their table through
// save_variables <prefix>_tools and persist writes with the _IFS_VARS macro
// (the legacy contract, kept byte-for-byte). A machine on either stack can
// remap: remap_ready()/owns_tool_mapping_table() answer whether EITHER signal
// has been seen, and the wire table wins when both are. build_ams_topology()
// (ams_state.cpp) takes ToolTopology::tool_count from get_tool_mapping()'s
// length, so the advertised table also decides the tool list every
// tool-count consumer reads.

TEST_CASE("AD5X IFS printer.ifs tool_map makes remap ready", "[ams][ad5x_ifs]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    // Before any printer.ifs frame there is no evidence the firmware can
    // remap, and no table to build a topology from.
    REQUIRE_FALSE(backend.remap_ready());
    REQUIRE_FALSE(backend.owns_tool_mapping_table());
    REQUIRE(backend.get_tool_mapping().empty());

    // A frame carrying tool_map is the capability.
    backend.fire_status(make_ifs_frame(identity_tool_map()));
    REQUIRE(backend.remap_ready());
    REQUIRE(backend.owns_tool_mapping_table());

    // An older module publishing printer.ifs without tool_map never arms the
    // gate — the field is the contract, not the object.
    helix::test::StartedBackend<AmsBackendAd5xIfs> old_module;
    old_module.fire_status(make_ifs_frame(json()));
    REQUIRE_FALSE(old_module.remap_ready());
    REQUIRE(old_module.get_tool_mapping().empty());
}

// The AmsBackend contract is 0-based slot indices indexed BY tool number, while
// the wire speaks 1-based lanes: an off-by-one aims T0 at the wrong lane. A lane
// this backend cannot address stays a -1 hole, trailing unmapped tools drop off,
// and each tool_map frame is the COMPLETE table: absent entries unmap, and a key
// that names no tool (non-numeric, past the 16-wide register) is ignored.
TEST_CASE("AD5X IFS get_tool_mapping translates the wire tool_map", "[ams][ad5x_ifs]") {
    struct Row {
        const char* name;
        json tool_map;
        std::vector<int> expected;
    };
    const Row rows[] = {
        {"1-based lanes become 0-based slots",
         json{{"0", 4}, {"1", 1}, {"2", 3}, {"3", 2}},
         {3, 0, 2, 1}},
        {"trailing unmapped tools drop off", identity_tool_map(), {0, 1, 2, 3}},
        {"an unaddressable lane stays a hole",
         json{{"0", 1}, {"1", 6}, {"2", 3}, {"3", 4}},
         {0, -1, 2, 3}},
        {"keys that name no tool are ignored", json{{"0", 2}, {"not-a-tool", 3}, {"16", 1}}, {1}},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
            auto& backend = *backend_reg;
            backend.fire_status(make_ifs_frame(identity_tool_map()));
            backend.fire_status(make_ifs_frame(r.tool_map));
            CHECK(backend.get_tool_mapping() == r.expected);
            // Only the advertised topology shrinks; the 16-wide register stays
            CHECK(backend.get_system_info().tool_to_slot_map.size() == 16);
        }
    }
}

TEST_CASE("AD5X IFS set_tool_mapping emits IFS_MAP_TOOL", "[ams][ad5x_ifs]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    backend.fire_status(make_ifs_frame(identity_tool_map()));

    // slot_index is 0-based (AmsBackend contract); the verb's SLOT= is the
    // 1-based lane. T2 -> lane 4.
    REQUIRE(backend.set_tool_mapping(2, 3).success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    REQUIRE(backend.captured_gcodes[0] == "IFS_MAP_TOOL TOOL=2 SLOT=4");

    // The wire owns the table: no optimistic local write, so a firmware
    // refusal (a lane with no filament) leaves get_tool_mapping() reporting
    // what the printer actually has until an echo frame lands.
    REQUIRE(backend.get_tool_mapping()[2] == 2);

    // The echo frame is what promotes the new mapping.
    backend.fire_status(make_ifs_frame(json{{"0", 1}, {"1", 2}, {"2", 4}, {"3", 4}}));
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{0, 1, 3, 3});
}

TEST_CASE("AD5X IFS set_tool_mapping rejects a slot the wire cannot address", "[ams][ad5x_ifs]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    backend.fire_status(make_ifs_frame(identity_tool_map()));

    // The wire has no "unmapped" state — every tool always aims at a lane —
    // so an out-of-range slot is a structural error, not a verb to send.
    REQUIRE_FALSE(backend.set_tool_mapping(0, 4).success());
    REQUIRE_FALSE(backend.set_tool_mapping(0, -1).success());
    REQUIRE(backend.captured_gcodes.empty());

    // The tool bound is the backend's addressable register. A tool beyond the
    // firmware's own lane count is the FIRMWARE's refusal to make (surfaced
    // by the gcode error path), so the verb goes out rather than being
    // pre-validated here.
    REQUIRE_FALSE(backend.set_tool_mapping(AmsBackendAd5xIfs::kToolMapSize, 0).success());
    REQUIRE(backend.captured_gcodes.empty());
    REQUIRE(backend.set_tool_mapping(15, 0).success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    REQUIRE(backend.captured_gcodes[0] == "IFS_MAP_TOOL TOOL=15 SLOT=1");
}

TEST_CASE("AD5X IFS set_tool_mapping propagates the dispatch error", "[ams][ad5x_ifs]") {
    // A refusal before dispatch must reach the caller rather than report
    // success for a verb that never left. (A FIRMWARE refusal, aiming at an
    // empty lane, arrives asynchronously on notify_gcode_response and surfaces
    // through the gcode error router.)
    struct RefusingBackend : AmsBackendAd5xIfs {
        using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
        using AmsBackendAd5xIfs::execute_gcode;
        AmsError execute_gcode(const std::string&) override {
            return AmsErrorHelper::not_connected("refused");
        }
    };
    helix::test::RegisteredWired<RefusingBackend> backend_reg;
    auto& backend = *backend_reg;
    backend.fire_status(make_ifs_frame(identity_tool_map()));

    REQUIRE_FALSE(backend.set_tool_mapping(0, 0).success());
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS set_tool_mapping without either contract stays a local write",
                 "[ams][ad5x_ifs]") {
    // Firmware publishing no tool_map and no _IFS_VARS macro: nothing echoes
    // or replays a mapping; a caller that ignores remap_ready() gets the
    // historical local-only behavior instead of a verb aimed at a module that
    // is not there.

    REQUIRE(backend.set_tool_mapping(15, 0).success());
    REQUIRE(Ad5xIfsTestAccess::tool_map(backend)[15] == 1); // port = slot + 1
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS ZMOD plugin contract remaps without printer.ifs",
                 "[ams][ad5x_ifs]") {
    // The ZMOD + lessWaste/bambufy population: the plugin's save_variables table
    // alone must keep remapping working, without the printer.ifs adapter.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    backend.fire_status(make_save_variables(standard_variables()));

    REQUIRE(backend.remap_ready());
    REQUIRE(backend.owns_tool_mapping_table());
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS set_tool_mapping still addresses the full 0..15 tool range",
                 "[ams][ad5x_ifs]") {
    // The ZMOD plugin's _IFS_VARS tool map is 16 wide and the user can still
    // pin a lane to T15.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    backend.fire_status(make_save_variables(standard_variables()));

    // The local map is applied before the gcode write goes out.
    backend.set_tool_mapping(15, 0);
    REQUIRE(Ad5xIfsTestAccess::tool_map(backend)[15] == 1); // port = slot + 1

    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping.size() == 16);
    REQUIRE(mapping[15] == 0);
    for (size_t t = 4; t < 15; ++t) {
        INFO("tool " << t);
        REQUIRE(mapping[t] == -1);
    }

    // One past the top is still rejected, and leaves the map alone.
    auto before = Ad5xIfsTestAccess::tool_map(backend);
    REQUIRE_FALSE(backend.set_tool_mapping(AmsBackendAd5xIfs::kToolMapSize, 0).success());
    REQUIRE(Ad5xIfsTestAccess::tool_map(backend) == before);
}

TEST_CASE("AD5X IFS the wire tool_map takes precedence over the plugin contract",
          "[ams][ad5x_ifs]") {
    // Both signals at once: live firmware state wins over save_variables
    // rows (which survive a firmware swap), and the write path follows the
    // wire — IFS_MAP_TOOL, never the _IFS_VARS mirror.
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // The plugin's table, reversed so the two contracts are distinguishable.
    auto vars = standard_variables();
    vars["less_waste_tools"] = json::array({4, 3, 2, 1, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    backend.fire_status(make_save_variables(vars));
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{3, 2, 1, 0});

    // The wire frame replaces the plugin's table wholesale.
    backend.fire_status(make_ifs_frame(identity_tool_map()));
    REQUIRE(backend.get_tool_mapping() == std::vector<int>{0, 1, 2, 3});

    // The save_variables frame above also staged the plugin's colors/types
    // repair mirror (4-entry lessWaste arrays); drop it so the capture below
    // holds only the mapping write under test.
    backend.captured_gcodes.clear();
    REQUIRE(backend.set_tool_mapping(1, 0).success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    REQUIRE(backend.captured_gcodes[0] == "IFS_MAP_TOOL TOOL=1 SLOT=1");
    REQUIRE_FALSE(backend.any_gcode_starts_with("_IFS_VARS"));
}

TEST_CASE("AD5X IFS reset_tool_mappings emits the firmware reset verb", "[ams][ad5x_ifs]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    backend.fire_status(make_ifs_frame(identity_tool_map()));

    REQUIRE(backend.reset_tool_mappings().success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    REQUIRE(backend.captured_gcodes[0] == "IFS_MAP_TOOL RESET=1");

    // Without the wire table there is nothing to reset — not_supported, the
    // base contract's answer, stands.
    GcodeCapturingBackend ungated(nullptr, nullptr);
    REQUIRE_FALSE(ungated.reset_tool_mappings().success());
    REQUIRE(ungated.captured_gcodes.empty());
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS tool_map echo refreshes the per-lane mapped tool",
                 "[ams][ad5x_ifs]") {
    backend.fire_status(make_ifs_frame(identity_tool_map()));
    REQUIRE(backend.get_slot_info(1).mapped_tool == 1); // lane 2 carries T1

    // T0 re-aimed at lane 2: the per-lane badge follows the wire.
    backend.fire_status(make_ifs_frame(json{{"0", 2}, {"1", 1}, {"2", 3}, {"3", 4}}));
    REQUIRE(backend.get_slot_info(1).mapped_tool == 0);
    REQUIRE(backend.get_slot_info(0).mapped_tool == 1);
}

TEST_CASE("AD5X IFS required_status_objects asks for the module when present", "[ams][ad5x_ifs]") {
    // Klipper's objects/list only carries objects that implement get_status(),
    // so listing [ifs] there is the capability check.
    helix::PrinterDiscovery with_module;
    with_module.parse_objects(json::array({"save_variables", "webhooks", "ifs"}));
    const auto requested = AmsBackendAd5xIfs::required_status_objects(with_module);
    REQUIRE(std::find(requested.begin(), requested.end(), "ifs") != requested.end());

    helix::PrinterDiscovery bare;
    bare.parse_objects(json::array({"save_variables", "webhooks"}));
    for (const auto& object : AmsBackendAd5xIfs::required_status_objects(bare)) {
        REQUIRE(object != "ifs");
    }
}

// 10. Bypass mode

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS bypass mode", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("external=1 activates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        backend.fire_status(make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("external=0 deactivates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 0;
        backend.fire_status(make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }

    SECTION("toggle bypass via parse") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        backend.fire_status(make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());

        vars["less_waste_external"] = 0;
        backend.fire_status(make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }
}

// 11. build_color_list_value format

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS build_color_list_value format", "[ams][ad5x_ifs]") {
    seed_standard_colors(backend);

    std::string colors = Ad5xIfsTestAccess::build_colors(backend);
    // var_prefix_ defaults to "less_waste" and tool_map_ is all-unmapped, so
    // the builder takes the identity fallback (T0..T3 -> ports 1..4, matching
    // lessWaste's own variable_tools default) and emits a 16-entry
    // TOOL-indexed list: the 4 port colours then 12 empty entries for the
    // unmapped virtual tools (#1247 — lessWaste's _RUNOUT_HEAD scans all 16
    // tool slots, so a 4-entry payload truncated the arrays and no backup
    // lane could ever match).
    std::string expected = "\"['FF0000', '00FF00', '0000FF', 'FFFFFF'";
    for (int i = 0; i < 12; ++i) {
        expected += ", ''";
    }
    expected += "]\"";
    REQUIRE(colors == expected);
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS lessWaste list payload projects tool_map_ per tool",
                 "[ams][ad5x_ifs]") {
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Swapped T0/T1, T2/T3, virtual tools 4-14 unmapped, T15 -> port 1.
    json vars{{"less_waste_tools", json::array({2, 1, 4, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 1})}};
    backend.fire_status(make_save_variables(vars));

    // Tool t's entry is colors_[tool_map_[t]-1]; unmapped tools carry ''.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend) ==
            "\"['00FF00', 'FF0000', 'FFFFFF', '0000FF', '', '', '', '', '', '', '', "
            "'', '', '', '', 'FF0000']\"");
    REQUIRE(Ad5xIfsTestAccess::build_types(backend) ==
            "\"['PETG', 'PLA', 'TPU', 'ABS', '', '', '', '', '', '', '', "
            "'', '', '', '', 'PLA']\"");
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS bambufy list payload stays port-indexed 4-entry",
                 "[ams][ad5x_ifs]") {
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

    // bambufy's _RUNOUT_HEAD iterates ifs.types (4 entries) and indexes
    // ifs.colors[port-1] — the port-indexed shape is correct there, and the
    // #1247 tool-indexing must not leak across prefixes.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend) ==
            "\"['FF0000', '00FF00', '0000FF', 'FFFFFF']\"");
    REQUIRE(Ad5xIfsTestAccess::build_types(backend) == "\"['PLA', 'PETG', 'ABS', 'TPU']\"");
}

// 12. build_tool_map_value format (the ZMOD plugin's _IFS_VARS tools= payload)

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS build_tool_map_value format", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    backend.fire_status(make_save_variables(standard_variables()));

    std::string tools = Ad5xIfsTestAccess::build_tools(backend);
    REQUIRE(tools == "\"[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]\"");
}

// 14. Slot status mapping

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS slot status mapping", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("port with filament, not active → AVAILABLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 2 has filament, active tool is T0 (mapped to port 1)
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);

        auto info = backend.get_slot_info(1); // slot 1 = port 2
        REQUIRE(info.status == SlotStatus::AVAILABLE);
    }

    SECTION("port with filament, is active + head loaded → LOADED") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 1 has filament, active tool is T0 (mapped to port 1), head has filament
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);

        auto info = backend.get_slot_info(0); // slot 0 = port 1
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("port without filament → EMPTY") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_3"] =
            json{{"filament_detected", false}};
        backend.fire_status(notification);

        auto info = backend.get_slot_info(2); // slot 2 = port 3
        REQUIRE(info.status == SlotStatus::EMPTY);
    }
}

// 15. Action state tracking

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS action state tracking", "[ams][ad5x_ifs]") {
    SECTION("LOADING + head sensor triggered → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);

        // Head sensor triggers → load complete
        backend.fire_status(make_head_sensor(true));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("UNLOADING + head sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);

        backend.fire_status(make_head_sensor(false));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("LOADING + head sensor NOT triggered → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        backend.fire_status(make_head_sensor(false));
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    }
}

// 16. Path segments

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS path segments", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("get_filament_segment: no filament anywhere → NONE") {
        REQUIRE(backend.get_filament_segment() == PathSegment::NONE);
    }

    SECTION("get_filament_segment: head has filament → NOZZLE") {
        backend.fire_status(make_head_sensor(true));
        REQUIRE(backend.get_filament_segment() == PathSegment::NOZZLE);
    }

    SECTION("get_filament_segment: port has filament, active tool set, head empty → LANE") {
        json notification;
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = 0; // T0 → port 1
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        // head sensor NOT set (defaults to false)
        backend.fire_status(notification);

        REQUIRE(backend.get_filament_segment() == PathSegment::LANE);
    }

    SECTION("get_slot_filament_segment: active slot with head filament → NOZZLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);

        REQUIRE(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }

    SECTION("get_slot_filament_segment: non-active slot with filament → HUB") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        backend.fire_status(notification);

        // Slot 1 (port 2) has filament but is not active — shows at hub
        REQUIRE(backend.get_slot_filament_segment(1) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: empty slot → NONE") {
        // Slot 2 with port_presence_=false → NONE.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }

    SECTION("get_slot_filament_segment: non-active slot with color data → HUB") {
        // Slot 2 with port_presence_=true and not the active slot → HUB.
        seed_standard_colors(backend);
        backend.fire_status(make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: out of range → NONE") {
        REQUIRE(backend.get_slot_filament_segment(-1) == PathSegment::NONE);
        REQUIRE(backend.get_slot_filament_segment(4) == PathSegment::NONE);
    }
}

// 17. Wrapped notification format (real WebSocket path)

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS handles wrapped notify_status_update", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("wrapped port sensor updates state") {
        auto wrapped = wrap_notification(make_port_sensor(1, true));
        backend.fire_status(wrapped);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    }

    SECTION("wrapped head sensor updates state") {
        auto wrapped = wrap_notification(make_head_sensor(true));
        backend.fire_status(wrapped);

        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    }

    SECTION("wrapped save_variables updates state") {
        seed_standard_colors(backend);
        auto wrapped = wrap_notification(make_save_variables(standard_variables()));
        backend.fire_status(wrapped);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("wrapped combined notification updates all state") {
        json status;
        status["save_variables"] = json{{"variables", standard_variables()}};
        status["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
        status["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

        backend.fire_status(wrap_notification(status));

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

        auto sys = backend.get_system_info();
        REQUIRE(sys.current_tool == 0);
        REQUIRE(sys.current_slot == 0);
        REQUIRE(sys.filament_loaded);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("wrapped notification completes load action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        backend.fire_status(wrap_notification(make_head_sensor(true)));

        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("wrapped notification completes unload action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        // Head sensor was true, now cleared
        backend.fire_status(make_head_sensor(true));
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        backend.fire_status(wrap_notification(make_head_sensor(false)));

        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("unwrapped format still works (initial query response)") {
        // on_started() callback sends unwrapped format — must still work
        backend.fire_status(make_port_sensor(2, true));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == true);
    }
}

// 18. Action timeout safety net

TEST_CASE("AD5X IFS clear_fault releases the operation lock and sends no stop",
          "[ams][ad5x_ifs][clear_fault]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    REQUIRE(backend.clear_fault(0).success());
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOCK"});
}

TEST_CASE("AD5X IFS reset retires a fault with one release", "[ams][ad5x_ifs][clear_fault]") {
    helix::test::StartedBackend<GcodeCapturingBackend> backend;
    Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    // The sidebar's Reset skips clear_fault() on this promise.
    REQUIRE(backend.reset_retires_fault());
    REQUIRE(backend.reset().success());
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOCK"});
}

TEST_CASE_METHOD(RegisteredAd5xIfs, "AD5X IFS action timeout resets stuck operations",
                 "[ams][ad5x_ifs]") {
    SECTION("LOADING surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    }

    SECTION("UNLOADING surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    }

    SECTION("IDLE does not change on timeout check") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::IDLE);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("action does not reset before timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    }

    SECTION("get_system_info surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));

        auto sys = backend.get_system_info();
        REQUIRE(sys.action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(RegisteredAd5xIfs,
                 "AD5X IFS PURGING gets a longer dedicated timeout budget (#1065 Bug 2)",
                 "[ams][ad5x_ifs]") {
    // A real purge runs far longer than the generic 90 s phase budget (~3 min
    // whole-op from cold; a 90 s ERROR mid-purge was seen twice in the field).
    // PURGING gets its own longer budget so a healthy-but-slow purge isn't killed.

    SECTION("does not ERROR within the dedicated budget (past the old 90 s window)") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(200));
        REQUIRE(backend.get_current_action() == AmsAction::PURGING);
    }

    SECTION("still ERRORs once the dedicated budget is exceeded") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(241));
        REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    }
}

TEST_CASE("AD5X IFS motion during PURGING resets the timeout clock (#1065 Bug 2)",
          "[ams][ad5x_ifs]") {
    // ifs_motion_sensor activity proves filament is still moving, so the purge is
    // progressing — reset the clock rather than ERRORing. The budget is then
    // effectively "time since last motion", so a long purge that keeps moving is
    // never falsely failed, while a genuinely stalled one still surfaces ERROR.
    SECTION("motion keeps a long-but-moving purge alive past the budget") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(300)); // past budget
        backend.fire_status(make_motion_sensor(true));                         // still moving
        Ad5xIfsTestAccess::run_action_timeout(backend);
        REQUIRE(backend.get_current_action() == AmsAction::PURGING);
    }

    SECTION("a stalled purge with no motion still ERRORs") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(300));
        Ad5xIfsTestAccess::run_action_timeout(backend);
        REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    }
}

// Indeterminate ("Working…") detector (#1065 row 14)
//
// During a phase-tracked load/unload the constrained AD5X's shared main-thread
// status feed can starve while klippy runs the blocking macro, freezing the
// live "Heat 225/230" number so it reads as a hang. The backend raises
// AmsSystemInfo::operation_indeterminate when no genuine progress signal (a
// temp-VALUE change, head transition, motion, or phase change) has landed within
// kIndeterminateThresholdSeconds (~8s); the sidebar then swaps the frozen number
// for a "Working…" busy state. Time is INJECTED via set_progress_age (no sleeps),
// so these stay fast and untagged (L052).

TEST_CASE_METHOD(WiredAd5xIfs,
                 "AD5X IFS indeterminate: a stalled progress feed trips the flag until progress or "
                 "completion (#1065 row 14)",
                 "[ams][ad5x_ifs][1065]") {
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false); // load -> HEATING
    backend.fire_status(make_extruder(150.0, 230.0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Just under threshold - does not fire.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(7));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Feed starves past the 8s threshold.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    // Still HEATING: this is the busy mitigation, not the coarse ERROR timeout
    // (HEATING's 300s ERROR budget is untouched; only the progress clock aged).
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);
    REQUIRE(backend.get_system_info().operation_indeterminate);

    SECTION("a temp-VALUE change is progress and clears it") {
        backend.fire_status(make_extruder(180.0, 230.0));
        CHECK_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    }

    SECTION("completing the operation clears it") {
        Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/false);
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
        // get_system_info() re-runs the detector; with no active op the flag is off.
        CHECK_FALSE(backend.get_system_info().operation_indeterminate);
        CHECK_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    }
}

// The sidebar's stall watchdog (ui_ams_sidebar) drives the detector by calling
// AmsState::sync_from_backend() on a main-thread timer while an op is active —
// which reaches the backend only through get_system_info(). So get_system_info()
// MUST run the timeout check itself, WITHOUT a prior status frame or an explicit
// run_action_timeout(): that is the only signal available when the WebSocket feed
// has stalled (#1065 row 14). If this regresses, the "Working…" state never
// appears on a real hang even though the flag logic is correct.
TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS indeterminate: get_system_info() trips the flag on its own (#1065 watchdog path)",
    "[ams][ad5x_ifs][1065]") {
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    backend.fire_status(make_extruder(150.0, 230.0));
    REQUIRE_FALSE(backend.get_system_info().operation_indeterminate);

    // Feed starves past the threshold. No run_action_timeout(), no status frame —
    // only the watchdog's get_system_info() call, exactly as sync_from_backend()
    // reaches it.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    REQUIRE(backend.get_system_info().operation_indeterminate);
}

TEST_CASE("AD5X IFS indeterminate: healthy heat never false-fires; frozen value does "
          "(#1065 row 14)",
          "[ams][ad5x_ifs][1065]") {
    SECTION("rising temps within threshold never trip") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

        // Each distinct temp frame is a value change that resets the clock, even
        // with realistic multi-second gaps between frames — so a normal heat-up
        // never trips the detector.
        for (double t : {100.0, 130.0, 160.0, 190.0, 220.0, 230.0}) {
            Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(6));
            backend.fire_status(make_extruder(t, 230.0));
            REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
        }
    }

    SECTION("identical (frozen) frames do NOT reset — the clock elapses") {
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

        backend.fire_status(make_extruder(150.0, 230.0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

        // The subject is frozen at 150°C: the SAME value keeps (nominally)
        // arriving but the clock ages past threshold with no VALUE change, so the
        // detector must fire — this is the exact row-14 behavior.
        Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
        backend.fire_status(make_extruder(150.0, 230.0));
        REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    }
}

// 19. Variable prefix auto-detection (lessWaste vs bambufy)

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS variable prefix auto-detection", "[ams][ad5x_ifs]") {
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("defaults to less_waste prefix") {
        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "less_waste");
    }

    SECTION("detects bambufy prefix from colors") {
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_types"] = json::array({"PLA", "PETG", "ABS", "TPU"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        vars["bambufy_current_tool"] = 0;
        vars["bambufy_external"] = 0;

        seed_standard_colors(backend);
        backend.fire_status(make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);

        // Color/material sourced from CHANGE_ZCOLOR/GET_ZCOLOR (seeded above),
        // not from <prefix>_colors.
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("detects bambufy prefix from tools alone") {
        json vars;
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

        backend.fire_status(make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
    }
}

// 20. Motion sensor triggers load/unload completion (native ZMOD)

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS motion sensor completes load/unload", "[ams][ad5x_ifs]") {
    SECTION("LOADING + motion sensor detected → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        backend.fire_status(make_motion_sensor(true));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("UNLOADING + motion sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        backend.fire_status(make_motion_sensor(false));
        REQUIRE(backend.get_current_action() == AmsAction::IDLE);
    }

    SECTION("LOADING + motion sensor NOT detected → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        backend.fire_status(make_motion_sensor(false));
        REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    }
}

// 20b. Phase tracker — live load/unload progress feedback

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: unload sequence (temp + head sensor)",
                 "[ams][ad5x_ifs][phase]") {
    // Filament present at the toolhead before unload begins.
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Begin a phased unload (firmware: HEATING → CUTTING → UNLOADING → IDLE).
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // Before any temp or target seen — detail names neither. The backend has no
    // signal to name a target with, and asserting one it never observed would be
    // a fabrication.
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle");

    // First temp frame: still heating, detail gains the live current temp.
    backend.fire_status(make_extruder(185.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle to 230°C (185°C)");

    // Temp reaches target → CUTTING.
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::CUTTING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Cutting filament");

    // Head sensor drops (cut + retract started) → UNLOADING.
    backend.fire_status(make_head_sensor(false));
    REQUIRE(backend.get_current_action() == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Retracting filament from nozzle");

    // Action timeout backstop surfaces ERROR (firmware did not confirm completion;
    // detail is preserved as the error context for the error-center bridge).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(WiredAd5xIfs, "AD5X IFS phase: load sequence (temp + head sensor)",
                 "[ams][ad5x_ifs][phase]") {
    // Fresh load: no filament at the toolhead initially.
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // Heating not yet complete.
    backend.fire_status(make_extruder(190.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::HEATING);

    // Temp reaches target → LOADING (feeding filament toward nozzle).
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(backend.get_current_action() == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Feeding filament to nozzle");

    // Head sensor rises (filament reached nozzle) → PURGING.
    backend.fire_status(make_head_sensor(true));
    REQUIRE(backend.get_current_action() == AmsAction::PURGING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Purging old filament");

    // Timeout backstop surfaces ERROR; detail preserved for error-center bridge.
    // PURGING has a dedicated 240s budget (#1065 Bug 2), so age past it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(250));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());
}

TEST_CASE_METHOD(
    WiredAd5xIfs,
    "AD5X IFS get_system_info surfaces the phase machine's operation_phase/detail (#1065 Bug 2)",
    "[ams][ad5x_ifs][1065]") {
    // The phase machine writes operation_phase (0/1/2) + operation_detail into
    // system_info_, but AmsState::sync_from_backend reads them off the
    // AmsSystemInfo that get_system_info() returns to drive the ams_operation_phase
    // subject the right-side step tracker observes. If get_system_info() drops
    // those fields, the steps render but the active one never highlights and the
    // detail line goes blank ("the 1-2-3 steps show but fail to launch any of
    // them").
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false); // load -> HEATING, phase 0

    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0); // internal phase-machine state
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());

    // get_system_info() is the UI-facing view — it MUST carry the same values.
    auto info = backend.get_system_info();
    CHECK(info.operation_phase == 0);
    CHECK(info.operation_detail == Ad5xIfsTestAccess::operation_detail(backend));

    // Advance to LOADING (phase 1) and re-check the UI view tracks it.
    backend.fire_status(make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);
    CHECK(backend.get_system_info().operation_phase == 1);
}
