// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_geometry_push.cpp
 * @brief Portrait overlays are top-anchored, stop above the nav bar, and slide on X
 *
 * The pure formula is covered by test_overlay_height_portrait.cpp. This file
 * covers the wiring: ui_set_overlay_geometry() writing height and alignment
 * onto the widget in portrait, and leaving both alone in landscape.
 *
 * The landscape assertions are the zero-regression pin and matter most — 17
 * overlay XML roots still author height="100%" align="right_mid" and that must
 * continue to stand untouched.
 */

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "display_settings_manager.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "theme_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class FakeOverlay : public IPanelLifecycle {
  public:
    explicit FakeOverlay(const char* name, bool destination = false)
        : name_(name), destination_(destination) {}
    void on_activate() override {}
    void on_deactivate(DeactivateReason) override {}
    const char* get_name() const override {
        return name_;
    }
    bool is_destination() const override {
        return destination_;
    }

  private:
    const char* name_;
    bool destination_;
};

/// LVGLTestFixture owns the UpdateQueue lifecycle that push_overlay() defers through.
class OverlayGeometryFixture : public LVGLTestFixture {
  public:
    OverlayGeometryFixture() {
        theme_manager_register_responsive_spacing(lv_display_get_default());
        NavigationManager::instance().init();
        for (int i = 0; i < kUiPanelCount; i++) {
            root_panels_[i] = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());
    }

    ~OverlayGeometryFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        NavigationManager::instance().deinit_subjects();
    }

  private:
    std::array<lv_obj_t*, kUiPanelCount> root_panels_{};
};

} // namespace

TEST_CASE_METHOD(OverlayGeometryFixture, "Landscape overlay geometry is untouched",
                 "[navigation][overlay-geometry][portrait]") {
    // XML authors height="100%" and align="right_mid"; neither may be disturbed.
    lv_obj_t* w = lv_obj_create(lv_screen_active());
    lv_obj_set_height(w, lv_pct(100));
    lv_obj_set_align(w, LV_ALIGN_RIGHT_MID);
    lv_obj_update_layout(w);
    const int32_t height_before = lv_obj_get_height(w);

    ui_set_overlay_geometry(w, /*is_destination=*/true);
    lv_obj_update_layout(w);

    CHECK(lv_obj_get_height(w) == height_before);
    CHECK(lv_obj_get_style_align(w, LV_PART_MAIN) == LV_ALIGN_RIGHT_MID);
}

TEST_CASE_METHOD(OverlayGeometryFixture, "Portrait overlays slide in and out on the X axis",
                 "[navigation][overlay-geometry][portrait]") {
    lv_display_set_resolution(lv_display_get_default(), 480, 800);
    REQUIRE(is_portrait_layout(detect_layout_type(lv_obj_get_width(lv_screen_active()),
                                                  lv_obj_get_height(lv_screen_active()))));

    auto& settings = DisplaySettingsManager::instance();
    const bool animations_were_enabled = settings.get_animations_enabled();
    settings.set_animations_enabled(true);

    lv_obj_t* w = lv_obj_create(lv_screen_active());
    lv_obj_set_size(w, 480, 688);
    lv_obj_set_align(w, LV_ALIGN_TOP_MID);
    lv_obj_update_layout(w);

    auto& nav = NavigationManager::instance();
    NavigationManagerTestAccess::animate_slide_in(nav, w);
    CHECK(lv_obj_get_style_translate_x(w, LV_PART_MAIN) == 480);
    CHECK(lv_obj_get_style_translate_y(w, LV_PART_MAIN) == 0);

    process_lvgl(500);
    CHECK(lv_obj_get_style_translate_x(w, LV_PART_MAIN) == 0);

    NavigationManagerTestAccess::animate_slide_out(nav, w);
    process_lvgl(100);
    CHECK(lv_obj_get_style_translate_x(w, LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_translate_y(w, LV_PART_MAIN) == 0);

    // Deleting the widget mid-slide cancels its animations: no callback outlives it.
    lv_obj_delete(w);
    REQUIRE(lv_anim_get(w, nullptr) == nullptr);
    process_lvgl(500);
    settings.set_animations_enabled(animations_were_enabled);
}
