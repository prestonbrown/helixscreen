// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_slot_ceiling.cpp
 * @brief A large multi-unit setup binds every lane up to kAmsMaxSlots, and a
 *        lane past it degrades with one warning instead of blanking.
 *
 * The 23-lane backend is the shape of a seven-unit AFC rig: twenty-two unit
 * lanes plus a direct-drive extruder.
 */

#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/log_capture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "tool_state.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendMock;
using helix::AmsState;
using helix::kAmsMaxSlots;
using helix::PathSegment;
using helix::SlotInfo;
using helix::SlotStatus;

namespace {

constexpr int kLargeRigLanes = 23;
constexpr int kLastLane = kLargeRigLanes - 1;

/// Give @p slot a colour and material no sample filament carries, so a match
/// proves the value came from this slot.
void paint_slot(AmsBackendMock& mock, int slot, uint32_t color, const char* material) {
    SlotInfo info = mock.get_slot_info(slot);
    info.color_rgb = color;
    info.material = material;
    REQUIRE(helix::test::apply_edit(mock, slot, info).success());
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: every lane of a 23-lane rig binds its own subjects",
                 "[ams][slot_ceiling]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLargeRigLanes);
    AmsBackendMock* mock = owned.get();
    REQUIRE(mock->get_system_info().total_slots == kLargeRigLanes);
    paint_slot(*mock, kLastLane, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    lv_subject_t* color = ams.get_slot_color_subject(kLastLane);
    REQUIRE(color != nullptr);
    CHECK(static_cast<uint32_t>(lv_subject_get_int(color)) == 0x2266AA);
    REQUIRE(ams.get_slot_status_subject(kLastLane) != nullptr);
    CHECK(lv_subject_get_int(ams.get_slot_status_subject(kLastLane)) ==
          static_cast<int>(SlotStatus::AVAILABLE));
    REQUIRE(ams.get_slot_material_subject(kLastLane) != nullptr);
    CHECK(std::string(lv_subject_get_string(ams.get_slot_material_subject(kLastLane))) == "PETG");

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_slot: lane 23 shows its material, not the unbound dash",
                 "[ams][slot_ceiling][ui]") {
    ui_ams_slot_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLargeRigLanes);
    paint_slot(*owned, kLastLane, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    const std::string index = std::to_string(kLastLane);
    const char* attrs[] = {"slot_index", index.c_str(), nullptr};
    auto* slot = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_slot", attrs));
    REQUIRE(slot != nullptr);
    lv_obj_t* material = lv_obj_find_by_name(slot, "material_label");
    REQUIRE(material != nullptr);
    CHECK(std::string(lv_label_get_text(material)) == "PETG");

    lv_obj_delete(slot);
    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "FilamentPath: the canvas holds state for lane 23",
                 "[ams][slot_ceiling][filament_path]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    ui_filament_path_canvas_set_slot_count(path, kLargeRigLanes);
    ui_filament_path_canvas_set_slot_filament(path, kLastLane, static_cast<int>(PathSegment::HUB),
                                              0x2266AA);
    CHECK(ui_filament_path_canvas_get_slot_filament(path, kLastLane) ==
          static_cast<int>(PathSegment::HUB));
    lv_obj_delete(path);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsState: a loaded lane past the ceiling still drives Currently Loaded",
                 "[ams][slot_ceiling]") {
    constexpr int kLanes = kAmsMaxSlots + 2;
    constexpr int kBeyond = kLanes - 1;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLanes);
    AmsBackendMock* mock = owned.get();
    REQUIRE(mock->get_system_info().total_slots == kLanes);
    REQUIRE(mock->start().success());
    mock->set_operation_delay(10);
    paint_slot(*mock, kBeyond, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));

    helix::LogCapture log;
    REQUIRE(mock->load_filament(kBeyond).success());
    mock->wait_for_operation_thread();
    ams.sync_from_backend();
    ams.update_slot(kBeyond);
    ams.sync_from_backend();

    REQUIRE(mock->get_current_slot() == kBeyond);
    CHECK(ams.get_slot_color_subject(kBeyond) == nullptr);
    CHECK(static_cast<uint32_t>(lv_subject_get_int(ams.get_current_color_subject())) == 0x2266AA);
    // One warning names the overflow, however often the rig syncs.
    CHECK(log.count_containing("per-slot subjects") == 1);

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsState: updating a loaded lane past the ceiling refreshes Currently Loaded",
                 "[ams][slot_ceiling]") {
    constexpr int kLanes = kAmsMaxSlots + 2;
    constexpr int kBeyond = kLanes - 1;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLanes);
    AmsBackendMock* mock = owned.get();
    REQUIRE(mock->start().success());
    mock->set_operation_delay(10);
    paint_slot(*mock, kBeyond, 0x2266AA, "PETG");
    ams.set_backend(std::move(owned));
    REQUIRE(mock->load_filament(kBeyond).success());
    mock->wait_for_operation_thread();
    ams.sync_from_backend();
    REQUIRE(static_cast<uint32_t>(lv_subject_get_int(ams.get_current_color_subject())) == 0x2266AA);

    // A single-slot update, with no full sync after it.
    paint_slot(*mock, kBeyond, 0x11CC33, "ABS");
    ams.update_slot(kBeyond);
    CHECK(static_cast<uint32_t>(lv_subject_get_int(ams.get_current_color_subject())) == 0x11CC33);

    ams.clear_backends();
}

namespace {

/// ToolState's subjects for one case, as test_tool_state_ams_topology.cpp sets them up.
struct ScopedToolState {
    ScopedToolState() {
        helix::ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ScopedToolState() {
        helix::ToolState::instance().clear_ams_topology();
        helix::ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState: a spool on a lane past the ceiling reaches its tool",
                 "[ams][slot_ceiling][tool-state]") {
    constexpr int kLanes = kAmsMaxSlots + 2;
    constexpr int kBeyond = kLanes - 1;
    ScopedToolState tool_state;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLanes);
    SlotInfo info = owned->get_slot_info(kBeyond);
    info.spoolman_id = 4242;
    info.spool_name = "Beyond";
    REQUIRE(helix::test::apply_edit(*owned, kBeyond, info).success());
    const int tool = owned->get_slot_info(kBeyond).mapped_tool;
    REQUIRE(tool >= 0);
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    const auto& tools = helix::ToolState::instance().tools();
    REQUIRE(static_cast<int>(tools.size()) > tool);
    CHECK(tools[tool].spoolman_id == 4242);

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsState: a tool changer's tool spool reaches a lane past the ceiling",
                 "[ams][slot_ceiling][tool-state]") {
    constexpr int kLanes = kAmsMaxSlots + 2;
    constexpr int kBeyond = kLanes - 1;
    ScopedToolState tool_state;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<AmsBackendMock>(kLanes);
    AmsBackendMock* mock = owned.get();
    mock->set_tool_changer_mode(true);
    REQUIRE(mock->supports_per_tool_spool_assignment());
    // The lane starts with no spool, so the tool's spool is what fills it.
    SlotInfo info = mock->get_slot_info(kBeyond);
    info.spoolman_id = 0;
    REQUIRE(helix::test::apply_edit(*mock, kBeyond, info).success());
    REQUIRE(mock->get_slot_info(kBeyond).spoolman_id == 0);
    const int tool = mock->get_slot_info(kBeyond).mapped_tool;
    REQUIRE(tool >= 0);
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    auto& ts = helix::ToolState::instance();
    REQUIRE(static_cast<int>(ts.tools().size()) > tool);
    ts.assign_spool(tool, 777, "Tool spool", 500.0f, 1000.0f);
    ams.sync_from_backend();

    CHECK(mock->get_slot_info(kBeyond).spoolman_id == 777);

    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "FilamentPath: the canvas takes every lane up to the ceiling",
                 "[ams][slot_ceiling][filament_path]") {
    using helix::ui::fpath::get_data;
    SECTION("set from C++") {
        lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
        REQUIRE(path != nullptr);
        ui_filament_path_canvas_set_slot_count(path, kAmsMaxSlots);
        CHECK(get_data(path)->slot_count == kAmsMaxSlots);
        ui_filament_path_canvas_set_slot_count(path, kAmsMaxSlots + 1);
        CHECK(get_data(path)->slot_count == kAmsMaxSlots);
        lv_obj_delete(path);
    }
    SECTION("set from XML") {
        ui_filament_path_canvas_register();
        const std::string count = std::to_string(kLargeRigLanes);
        const char* attrs[] = {"slot_count", count.c_str(), nullptr};
        auto* path =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "filament_path_canvas", attrs));
        REQUIRE(path != nullptr);
        CHECK(get_data(path)->slot_count == kLargeRigLanes);
        lv_obj_delete(path);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "EndlessSpoolArrows: a backup between lanes past sixteen is drawn",
                 "[ams][slot_ceiling][endless_spool]") {
    constexpr int kSlotWidth = 30;
    lv_obj_t* arrows = ui_endless_spool_arrows_create(test_screen());
    REQUIRE(arrows != nullptr);
    lv_obj_set_pos(arrows, 0, 0);
    lv_obj_set_size(arrows, kLargeRigLanes * kSlotWidth, 60);
    ui_endless_spool_arrows_set_slot_count(arrows, kLargeRigLanes);
    ui_endless_spool_arrows_set_slot_width(arrows, kSlotWidth);
    ui_endless_spool_arrows_set_slot_overlap(arrows, 0);
    std::vector<int> edges(kLargeRigLanes, -1);
    edges[kLastLane - 1] = kLastLane;
    ui_endless_spool_arrows_set_groups(arrows,
                                       helix::printer::endless_spool_config_from_edges(edges));
    lv_obj_update_layout(test_screen());

    lv_draw_buf_t* snap = lv_snapshot_take(arrows, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    int drawn = 0;
    for (uint32_t y = 0; y < snap->header.h; ++y) {
        const uint8_t* row = snap->data + y * snap->header.stride;
        for (uint32_t x = 0; x < snap->header.w; ++x) {
            if (row[x * 4 + 3] != 0) {
                ++drawn;
            }
        }
    }
    lv_draw_buf_destroy(snap);
    CHECK(drawn > 0);
    lv_obj_delete(arrows);
}
