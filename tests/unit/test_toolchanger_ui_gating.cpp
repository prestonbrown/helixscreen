// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_ui_gating.cpp
 * @brief What the AMS screens offer for a tool changer: Device Operations'
 *        Home, Recover and Abort, and the head picker's loaded wording.
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_batch_filament_modal.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/seeded_override.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::test::ToolChangerHelper;

namespace {

PrinterDiscovery stock_medusahc() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"pin_watch io", "medusahc", "toolhead", "extruder", "extruder1",
                                  "extruder2", "extruder3", "gcode_macro T0", "gcode_macro T1",
                                  "gcode_macro T2", "gcode_macro T3"}));
    return hw;
}

PrinterDiscovery medusahc_beside_ktc() {
    PrinterDiscovery hw;
    hw.parse_objects(
        json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3", "pin_watch io",
                     "medusahc", "extruder", "extruder1", "extruder2", "extruder3"}));
    return hw;
}

/// A changer whose Cancel records how many "Aborting..." notices preceded it.
class AbortingChanger : public ToolChangerHelper {
  public:
    explicit AbortingChanger(int tools) : ToolChangerHelper(tools) {}
    std::vector<std::string>* infos = nullptr;
    size_t infos_at_cancel = 99;
    AmsError cancel() override {
        infos_at_cancel = infos ? infos->size() : 0;
        return AmsErrorHelper::success();
    }
};

int subject_value(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

struct DeviceOpsFixture : LVGLUITestFixture {
    DeviceOpsFixture() {
        StaticPanelRegistry::instance().destroy_all();
        AmsState::instance().init_subjects(true);
    }
    ~DeviceOpsFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        ui::UpdateQueue::instance().drain();
    }
    ui::AmsDeviceOperationsOverlay& show() {
        auto& overlay = ui::get_ams_device_operations_overlay();
        overlay.init_subjects();
        overlay.show(test_screen());
        overlay.refresh();
        ui::UpdateQueue::instance().drain();
        return overlay;
    }
};

} // namespace

TEST_CASE_METHOD(DeviceOpsFixture,
                 "Device Operations offers Home and Recover only where they exist",
                 "[ams][device-ops][toolchanger][medusahc]") {
    SECTION("beside klipper-toolchanger: INITIALIZE_TOOLCHANGER backs both") {
        test::RegisteredBackend<ToolChangerHelper> tc(4);
        (*tc).set_tool_commands(toolchanger_addon::resolve_tool_commands(medusahc_beside_ktc()));
        auto& overlay = show();
        CHECK(subject_value("ams_device_ops_can_home") == 1);
        CHECK(subject_value("ams_device_ops_can_recover") == 1);

        // The backend goes away: nothing is left to home or recover.
        AmsState::instance().clear_backends();
        overlay.refresh();
        CHECK(subject_value("ams_device_ops_can_home") == 0);
        CHECK(subject_value("ams_device_ops_can_recover") == 0);
    }
    SECTION("a stock install has neither") {
        test::RegisteredBackend<ToolChangerHelper> tc(4);
        (*tc).set_tool_commands(toolchanger_addon::resolve_tool_commands(stock_medusahc()));
        show();
        CHECK(subject_value("ams_device_ops_can_home") == 0);
        CHECK(subject_value("ams_device_ops_can_recover") == 0);
    }
}

TEST_CASE_METHOD(DeviceOpsFixture, "Device Operations' Abort says Aborting before it cancels",
                 "[ams][device-ops][toolchanger][cancel]") {
    std::vector<std::string> infos;
    ui::set_test_notification_info_hook([&infos](const std::string& m) { infos.push_back(m); });
    test::RegisteredBackend<AbortingChanger> tc(4);
    (*tc).infos = &infos;
    show();
    lv_event_cb_t abort = lv_xml_get_event_cb(nullptr, "on_ams_device_ops_abort");
    REQUIRE(abort != nullptr);
    abort(nullptr);
    CHECK((*tc).infos_at_cancel == 1);
    REQUIRE_FALSE(infos.empty());
    CHECK(infos.front() == "Aborting...");
    ui::set_test_notification_info_hook(nullptr);
}

namespace {

/// A mount-to-feed changer that knows head 2 holds filament.
class FedChanger : public ToolChangerHelper {
  public:
    explicit FedChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }
    std::optional<bool> head_has_filament(int head) const override {
        return head == 2 ? std::optional<bool>(true) : std::nullopt;
    }
};

std::string row_text(lv_obj_t* dialog, const char* key) {
    lv_obj_t* row = lv_obj_find_by_name(dialog, ("item_" + std::string(key)).c_str());
    REQUIRE(row != nullptr);
    std::string text;
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
        lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            text += lv_label_get_text(child);
        }
    }
    return text;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "The Unload picker reads a fed docked hot end as loaded",
                 "[ams][batch][toolchanger][medusahc]") {
    AmsState::instance().init_subjects(true);
    test::RegisteredBackend<FedChanger> tc(4);
    (*tc).feed_status("ready", 0);
    for (int slot : {1, 2}) {
        SlotInfo info = (*tc).get_slot_info(slot);
        info.material = "ABS";
        test::edit_slot_as_user(*tc, slot, info);
    }
    ui::UpdateQueue::instance().drain();

    REQUIRE(ui::BatchFilamentModal::show_owned(ui::HeadPickOp::Unload,
                                               [](const std::vector<ui::HeadPickRow>&) {}));
    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);
    CHECK(row_text(dialog, "2").find("ABS - loaded") != std::string::npos);
    CHECK(row_text(dialog, "1").find("ABS - ready to load") != std::string::npos);
    Modal::hide(dialog);
    process_lvgl(200);
}
