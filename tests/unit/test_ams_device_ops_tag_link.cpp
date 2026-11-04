// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The Device Operations row that turns tag-declared Spoolman links on and off:
// its capability gate, its toggle, and the gate's reset without a backend.

#include "ui_ams_device_operations_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

/// A backend whose tags can name a Spoolman spool, as a stock CFS's can.
class TagNamingMock : public AmsBackendMock {
  public:
    TagNamingMock() : AmsBackendMock(4) {}
    [[nodiscard]] bool tags_name_spoolman_spools() const override {
        return true;
    }
};

class DeviceOpsTagLinkFixture : public LVGLUITestFixture {
  public:
    DeviceOpsTagLinkFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_ams_tag_links_spoolman(true);
        get_printer_state().capabilities_state().set_spoolman_available(true);
        settle();
    }

    ~DeviceOpsTagLinkFixture() override {
        NavigationManager::instance().go_back();
        settle();
        helix::ui::destroy_static_panels();
        settle();
        AmsState::instance().set_backend(nullptr);
        SettingsManager::instance().set_ams_tag_links_spoolman(true);
        get_printer_state().capabilities_state().set_spoolman_available(false);
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

    lv_obj_t* row() {
        lv_obj_t* obj = lv_obj_find_by_name(get_ams_device_operations_overlay().get_root(),
                                            "row_ams_tag_links_spoolman");
        REQUIRE(obj != nullptr);
        return obj;
    }

    static int capability() {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "ams_device_ops_tags_name_spools");
        REQUIRE(subject != nullptr);
        return lv_subject_get_int(subject);
    }
};

} // namespace

TEST_CASE_METHOD(DeviceOpsTagLinkFixture,
                 "Link Spools from Tags shows where tags name spools and Spoolman is set up",
                 "[ams][device_ops][tag_link]") {
    SECTION("a backend whose tags name spools") {
        show_with(std::make_unique<TagNamingMock>());
        CHECK(capability() == 1);
        CHECK_FALSE(lv_obj_has_flag(row(), LV_OBJ_FLAG_HIDDEN));
    }
    SECTION("a backend whose tags cannot") {
        show_with(std::make_unique<AmsBackendMock>(4));
        CHECK(capability() == 0);
        CHECK(lv_obj_has_flag(row(), LV_OBJ_FLAG_HIDDEN));
    }
}

TEST_CASE_METHOD(DeviceOpsTagLinkFixture, "Link Spools from Tags toggles the setting",
                 "[ams][device_ops][tag_link]") {
    show_with(std::make_unique<TagNamingMock>());
    lv_obj_t* sw = lv_obj_find_by_name(row(), "toggle");
    REQUIRE(sw != nullptr);
    REQUIRE(lv_obj_has_state(sw, LV_STATE_CHECKED));

    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK_FALSE(SettingsManager::instance().get_ams_tag_links_spoolman());

    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(SettingsManager::instance().get_ams_tag_links_spoolman());
}

TEST_CASE_METHOD(DeviceOpsTagLinkFixture, "Link Spools from Tags hides when the backend goes",
                 "[ams][device_ops][tag_link]") {
    show_with(std::make_unique<TagNamingMock>());
    REQUIRE(capability() == 1);

    AmsState::instance().set_backend(nullptr);
    get_ams_device_operations_overlay().refresh();
    settle();
    CHECK(capability() == 0);
}
