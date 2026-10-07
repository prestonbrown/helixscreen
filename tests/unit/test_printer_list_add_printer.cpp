// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_list_add_printer.cpp
 * @brief Add Printer from the Printers list opens what it opens after the list closes.
 *
 * Closing the list pops an overlay, and the pop hides every stray child of the screen. What
 * the add callback puts on screen must arrive after that sweep, or it is hidden as it opens.
 */

#include "ui_nav_manager.h"
#include "ui_printer_list_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class AddPrinterFixture : public LVGLUITestFixture {
  public:
    AddPrinterFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        auto& nav = NavigationManager::instance();
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = lv_obj_create(test_screen());
        nav.set_panels(panels);

        list_overlay_ = lv_obj_create(test_screen());
        lv_obj_add_flag(list_overlay_, LV_OBJ_FLAG_HIDDEN);
        nav.register_overlay_instance(list_overlay_, nullptr);
        nav.push_overlay(list_overlay_);
        drain();
    }

    ~AddPrinterFixture() override {
        auto& nav = NavigationManager::instance();
        nav.set_printer_callbacks(nullptr, nullptr);
        nav.unregister_overlay_instance(list_overlay_);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    lv_obj_t* list_overlay_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(AddPrinterFixture, "Printers list: Add Printer opens after the list closes",
                 "[multi-printer][navigation]") {
    lv_obj_t* opened = nullptr;
    NavigationManager::instance().set_printer_callbacks(
        [](const std::string&) {}, [&] { opened = lv_obj_create(test_screen()); });

    helix::ui::get_printer_list_overlay().handle_add_printer();
    drain();

    REQUIRE(opened != nullptr);
    CHECK_FALSE(lv_obj_has_flag(opened, LV_OBJ_FLAG_HIDDEN));
}
