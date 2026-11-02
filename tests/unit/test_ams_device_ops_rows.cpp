// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_device_ops_rows.cpp
 * @brief The Device Operations rows every backend shares: the live status
 *        line, the one Endless Spool switch, and the entry-point gate.
 *
 * Run with: ./build/bin/helix-tests "[ams][device-ops][rows]"
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

/// A mock whose Endless Spool takes an on/off switch, recording each request.
class SwitchableEndlessMock : public AmsBackendMock {
  public:
    SwitchableEndlessMock(bool switchable, bool on) : AmsBackendMock(4) {
        set_endless_spool_supported(true);
        switchable_ = switchable;
        on_ = on;
    }

    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override {
        auto caps = AmsBackendMock::get_endless_spool_capabilities();
        caps.enabled = on_ ? helix::printer::EndlessSpoolEnabled::On
                           : helix::printer::EndlessSpoolEnabled::Off;
        caps.switchable = switchable_;
        return caps;
    }

    /// No device sections at all: the overlay still has its generic rows.
    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override {
        return {};
    }

    std::vector<bool> requests;

    void set_event_callback(EventCallback callback) override {
        listener_ = callback;
        AmsBackendMock::set_event_callback(std::move(callback));
    }

    /// What a backend reports when Klipper rejects the switch after the send.
    void report_switch_refused() {
        REQUIRE(listener_);
        listener_(AmsBackend::kEventEndlessSpoolRefused, "");
    }

  protected:
    AmsError apply_endless_spool_enabled(bool enabled) override {
        requests.push_back(enabled);
        return AmsErrorHelper::success();
    }

  private:
    EventCallback listener_;
    bool switchable_ = false;
    bool on_ = false;
};

class DeviceOpsRowsFixture : public LVGLUITestFixture {
  public:
    DeviceOpsRowsFixture() {
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().deinit_subjects();
    }

    ~DeviceOpsRowsFixture() override {
        NavigationManager::instance().go_back();
        settle();
        helix::ui::destroy_static_panels();
        settle();
        AmsState::instance().set_backend(nullptr);
    }

    void settle() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        process_lvgl(10);
    }

    /// Install @p backend, build the overlay through show(), and return it.
    SwitchableEndlessMock* show_with(std::unique_ptr<SwitchableEndlessMock> owned) {
        owned->set_operation_delay(0);
        REQUIRE(owned->start().success());
        SwitchableEndlessMock* raw = owned.get();
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);
        ams.sync_from_backend();

        auto& overlay = get_ams_device_operations_overlay();
        overlay.init_subjects();
        overlay.show(test_screen());
        settle();
        return raw;
    }

    lv_obj_t* find(const char* name) {
        return lv_obj_find_by_name(get_ams_device_operations_overlay().get_root(), name);
    }

    bool visible(const char* name) {
        lv_obj_t* obj = find(name);
        REQUIRE(obj != nullptr);
        return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* endless_switch() {
        lv_obj_t* row = find("row_endless_spool");
        REQUIRE(row != nullptr);
        lv_obj_t* sw = lv_obj_find_by_name(row, "toggle");
        REQUIRE(sw != nullptr);
        return sw;
    }
};

} // namespace

TEST_CASE_METHOD(DeviceOpsRowsFixture, "device ops status line is the live AMS narration",
                 "[ams][device-ops][rows]") {
    show_with(std::make_unique<SwitchableEndlessMock>(false, false));
    lv_obj_t* label = find("status_label");
    REQUIRE(label != nullptr);

    // Every action reaches the line as it happens, not on the next refresh,
    // purging included.
    AmsState::instance().set_action(AmsAction::PURGING);
    settle();
    CHECK(std::string(lv_label_get_text(label)) == lv_tr("Purging"));

    AmsState::instance().set_action_detail("Waiting for slot 2");
    settle();
    CHECK(std::string(lv_label_get_text(label)) == "Waiting for slot 2");
}

TEST_CASE_METHOD(DeviceOpsRowsFixture, "one Endless Spool switch follows the capability",
                 "[ams][device-ops][rows][endless]") {
    SECTION("a backend that cannot switch it hides the row and refuses the request") {
        auto* backend = show_with(std::make_unique<SwitchableEndlessMock>(false, true));
        CHECK_FALSE(visible("row_endless_spool"));
        CHECK_FALSE(backend->set_endless_spool_enabled(false).success());
        CHECK(backend->requests.empty());
    }

    SECTION("a switchable backend shows the row, checked as reported") {
        show_with(std::make_unique<SwitchableEndlessMock>(true, true));
        CHECK(visible("row_endless_spool"));
        CHECK(lv_obj_has_state(endless_switch(), LV_STATE_CHECKED));
    }

    SECTION("toggling the switch asks the backend for the new state") {
        auto* backend = show_with(std::make_unique<SwitchableEndlessMock>(true, false));
        lv_obj_t* sw = endless_switch();
        REQUIRE_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
        CHECK(backend->requests == std::vector<bool>{true});
    }

    SECTION("a rejection Klipper reports after the send puts the switch back") {
        auto* backend = show_with(std::make_unique<SwitchableEndlessMock>(true, false));
        lv_obj_t* sw = endless_switch();
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
        REQUIRE(lv_obj_has_state(sw, LV_STATE_CHECKED));
        backend->report_switch_refused();
        settle();
        CHECK_FALSE(lv_obj_has_state(sw, LV_STATE_CHECKED));
    }
}

TEST_CASE_METHOD(DeviceOpsRowsFixture, "no vendor-named unload row in the generic overlay",
                 "[ams][device-ops][rows]") {
    show_with(std::make_unique<SwitchableEndlessMock>(false, false));
    CHECK(find("row_afc_unload_after_print") == nullptr);
    CHECK(lv_xml_get_subject(nullptr, "ams_device_ops_unload_after_print_configurable") == nullptr);
}

TEST_CASE_METHOD(DeviceOpsRowsFixture, "device ops are offered for any backend, sections or not",
                 "[ams][device-ops][rows][entry]") {
    show_with(std::make_unique<SwitchableEndlessMock>(false, false));
    lv_subject_t* has = lv_xml_get_subject(nullptr, "ams_has_device_ops");
    REQUIRE(has != nullptr);
    CHECK(lv_subject_get_int(has) == 1);

    AmsState::instance().set_backend(nullptr);
    CHECK(lv_subject_get_int(has) == 0);
}
