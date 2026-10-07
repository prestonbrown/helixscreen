// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A real PrintStatusPanel built from production XML on an LVGL UI fixture.

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "filament_sensor_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "test_helpers/print_status_panel_test_access.h"

#include <functional>
#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

namespace print_status_panel_test {

using helix::ui::UpdateQueue;

/// Owns a real PrintStatusPanel built from production XML (same shape as
/// test_print_status_metadata_strip_fit.cpp's fixture).
///
/// A local PrintStatusPanel whose destructor runs leaves its helix-xml
/// subject registrations dangling for the rest of the process; poking
/// production's process-lifetime singleton re-registers every name against
/// stable storage, healing the entries this fixture's teardown dangles.
class PrintStatusPanelFixture : public LVGLUITestFixture {
  public:
    PrintStatusPanelFixture() : PrintStatusPanelFixture(nullptr) {}

    /// @p before_create runs before the panel is built; what it returns lives
    /// until the panel is gone (e.g. a layout scope the panel was built under).
    explicit PrintStatusPanelFixture(const std::function<std::shared_ptr<void>()>& before_create) {
        heal_global_print_status_panel_subjects();
        if (before_create) {
            setup_ = before_create();
        }
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
        REQUIRE(root_ != nullptr);
    }

    ~PrintStatusPanelFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
        heal_global_print_status_panel_subjects();
    }

    /// Delete the panel's widget tree the way a screen teardown does: the panel
    /// gets no call, only LVGL's own delete event.
    void delete_widget_tree() {
        REQUIRE(root_ != nullptr);
        lv_obj_delete(root_);
        root_ = nullptr;
    }

    /// Destroy the panel while its widget tree is still alive, the way
    /// StaticPanelRegistry::destroy_all() does before lv_deinit().
    void panel_release() {
        UpdateQueue::instance().drain();
        panel_.reset();
    }

    PrintStatusPanel& panel() {
        return *panel_;
    }

  protected:
    lv_obj_t* root_ = nullptr;
    std::shared_ptr<void> setup_; ///< Released after the panel, in member order

  private:
    void heal_global_print_status_panel_subjects() {
        auto& global = get_global_print_status_panel();
        if (!global.are_subjects_initialized()) {
            global.init_subjects();
        }
    }

    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace print_status_panel_test
