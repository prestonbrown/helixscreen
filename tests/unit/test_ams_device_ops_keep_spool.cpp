// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Device Operations "Keep Spool Info on Eject" switch: HelixScreen's own
// setting, or the firmware's when the firmware has one on every lane.

#include "ui_ams_device_operations_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "settings_manager.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

/// A backend whose firmware keeps its own per-lane retention setting.
class FirmwareRetentionMock : public AmsBackendMock {
  public:
    FirmwareRetentionMock(bool owns, bool keeps, std::vector<bool>* writes)
        : AmsBackendMock(4), owns_(owns), keeps_(keeps), writes_(writes) {}
    [[nodiscard]] bool firmware_owns_spool_retention() const override {
        return owns_;
    }
    [[nodiscard]] bool printer_retains_spool_info() const override {
        return keeps_;
    }
    AmsError set_firmware_spool_retention(bool keep) override {
        writes_->push_back(keep);
        keeps_ = keep;
        return AmsErrorHelper::success();
    }

  private:
    bool owns_;
    bool keeps_;
    std::vector<bool>* writes_;
};

class DeviceOpsKeepSpoolFixture : public LVGLUITestFixture {
  public:
    DeviceOpsKeepSpoolFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);
        settle();
    }

    ~DeviceOpsKeepSpoolFixture() override {
        NavigationManager::instance().go_back();
        settle();
        helix::ui::destroy_static_panels();
        settle();
        AmsState::instance().set_backend(nullptr);
        SettingsManager::instance().set_ams_keep_spool_info_on_eject(true);
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        process_lvgl(10);
    }

    void show_with(std::unique_ptr<AmsBackendMock> owned) {
        owned->set_operation_delay(0);
        REQUIRE(owned->start().success());
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);
        ams.sync_from_backend();

        auto& overlay = get_ams_device_operations_overlay();
        overlay.init_subjects();
        overlay.show(test_screen());
        settle();
    }

    static lv_obj_t* row(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(get_ams_device_operations_overlay().get_root(), name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    static lv_obj_t* toggle_of(lv_obj_t* row_obj) {
        lv_obj_t* sw = lv_obj_find_by_name(row_obj, "toggle");
        REQUIRE(sw != nullptr);
        return sw;
    }

    void flip(lv_obj_t* sw, bool on) {
        if (on)
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
    }
};

constexpr const char* kOwnRow = "row_ams_keep_spool_info_on_eject";
constexpr const char* kFirmwareRow = "row_ams_firmware_keeps_spool_info";

} // namespace

TEST_CASE_METHOD(DeviceOpsKeepSpoolFixture,
                 "Keep Spool Info shows and writes the firmware's setting where it owns one",
                 "[ams][device_ops][remember_spool]") {
    std::vector<bool> writes;
    show_with(std::make_unique<FirmwareRetentionMock>(true, false, &writes));

    CHECK(lv_obj_has_flag(row(kOwnRow), LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(row(kFirmwareRow), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* sw = toggle_of(row(kFirmwareRow));
    CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED)); // the firmware's value, not ours

    flip(sw, true);
    REQUIRE(writes == std::vector<bool>{true});
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED));
    // HelixScreen's own setting is not what the switch moved.
    CHECK(SettingsManager::instance().get_ams_keep_spool_info_on_eject());

    flip(sw, false);
    CHECK(writes == std::vector<bool>{true, false});
    CHECK(SettingsManager::instance().get_ams_keep_spool_info_on_eject());
}

TEST_CASE_METHOD(DeviceOpsKeepSpoolFixture,
                 "Keep Spool Info stays HelixScreen's setting where the firmware has none",
                 "[ams][device_ops][remember_spool]") {
    std::vector<bool> writes;
    show_with(std::make_unique<FirmwareRetentionMock>(false, false, &writes));

    CHECK(lv_obj_has_flag(row(kFirmwareRow), LV_OBJ_FLAG_HIDDEN));
    flip(toggle_of(row(kOwnRow)), false);
    CHECK_FALSE(SettingsManager::instance().get_ams_keep_spool_info_on_eject());
    CHECK(writes.empty());
}
