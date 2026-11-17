// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// screen_secret_state() decides, for every flushed frame, whether the remote screen
// may serve it. It must say Shown exactly while a marked widget can be drawn: the
// Screen Sharing page's pairing code row, open and unhidden.

#include "ui_panel_settings.h"
#include "ui_settings_screen_sharing.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "core/update_queue.h"
#include "remote_screen_settings_manager.h"
#include "screen_secrets.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::screen_secret_state;

namespace {

/// The state as the flush hook reads it: LVGL lays the screen out before it renders.
ScreenSecret state_after_layout() {
    lv_obj_update_layout(lv_screen_active());
    return screen_secret_state();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "screen secret: a marked widget counts only while it can be drawn",
                 "[remote_screen][secret]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 200, 100);
    lv_obj_t* secret = lv_obj_create(parent);
    lv_obj_set_size(secret, 50, 20);
    process_lvgl(5);
    CHECK(state_after_layout() == ScreenSecret::Absent);

    helix::ui::mark_screen_secret(secret);
    CHECK(state_after_layout() == ScreenSecret::Shown);

    lv_obj_set_hidden(parent, true);
    CHECK(state_after_layout() == ScreenSecret::Absent);
    lv_obj_set_hidden(parent, false);
    CHECK(state_after_layout() == ScreenSecret::Shown);

    // Deleted: forgotten, never read again.
    lv_obj_delete(parent);
    CHECK(state_after_layout() == ScreenSecret::Absent);
}

namespace {

/// The Screen Sharing page built by the overlay itself, as Settings opens it.
struct ScreenSharingPageFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    ScreenSharingPageFixture() {
        setup_printer_data(*Config::get_instance(), json::object());
        SettingsManager::instance().init_subjects();
        auto& m = RemoteScreenSettingsManager::instance();
        m.deinit_subjects();
        m.init_subjects();
        get_global_settings_panel().init_subjects();
        auto& overlay = settings::get_screen_sharing_settings_overlay();
        overlay.register_callbacks();
        root_ = overlay.create(test_screen());
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~ScreenSharingPageFixture() override {
        settings::get_screen_sharing_settings_overlay().destroy_overlay_ui(root_);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
        setup_printer_data(*Config::get_instance(), json::object());
        RemoteScreenSettingsManager::instance().on_printer_changed();
    }

    void open() {
        lv_obj_set_hidden(root_, false);
        process_lvgl(5);
    }
};

} // namespace

TEST_CASE_METHOD(ScreenSharingPageFixture,
                 "screen secret: the pairing code row is Shown only while the page shows it",
                 "[remote_screen][secret]") {
    auto& m = RemoteScreenSettingsManager::instance();
    m.set_enabled(true);
    m.set_lan_mode(true);
    m.set_control(true);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);

    // Built but not yet pushed: hidden, nothing on the display.
    CHECK(state_after_layout() == ScreenSecret::Absent);

    open();
    CHECK(state_after_layout() == ScreenSecret::Shown);

    // Page open, code row hidden (control off): the screen is served live.
    m.set_control(false);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);
    CHECK(state_after_layout() == ScreenSecret::Absent);

    // Behind nginx there is no pairing, and no code on the page.
    m.set_control(true);
    m.set_lan_mode(false);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);
    CHECK(state_after_layout() == ScreenSecret::Absent);

    m.set_lan_mode(true);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(5);
    REQUIRE(state_after_layout() == ScreenSecret::Shown);

    // Closed: the overlay is hidden again.
    lv_obj_set_hidden(root_, true);
    CHECK(state_after_layout() == ScreenSecret::Absent);
}
