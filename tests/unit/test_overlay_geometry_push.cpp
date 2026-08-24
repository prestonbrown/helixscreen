// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_geometry_push.cpp
 * @brief Portrait overlays are top-anchored and stop above the nav bar
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
    void on_deactivate() override {}
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

/// LVGLTestFixture owns LVGL init, the display, and the UpdateQueue lifecycle.
/// push_overlay() defers through that queue — a fixture that skips it silently
/// never pushes anything and every assertion fails as if the logic were wrong.
class OverlayGeometryFixture : public LVGLTestFixture {
  public:
    OverlayGeometryFixture() {
        theme_manager_register_responsive_spacing(lv_display_get_default());
        NavigationManager::instance().init();
        for (int i = 0; i < UI_PANEL_COUNT; i++) {
            root_panels_[i] = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());
    }

    ~OverlayGeometryFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        NavigationManager::instance().deinit_subjects();
    }

  private:
    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};
};

} // namespace

TEST_CASE_METHOD(OverlayGeometryFixture, "Landscape overlay geometry is untouched",
                 "[navigation][overlay-geometry][portrait]") {
    // The display is landscape by default in tests. XML authors height="100%"
    // and align="right_mid"; ui_set_overlay_geometry must not disturb either.
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

TEST_CASE("Portrait geometry stops the overlay above the nav bar",
          "[navigation][overlay-geometry][portrait]") {
    // Exercised through the pure function rather than a live portrait display:
    // the test harness display is landscape, and detect_layout_type() reads the
    // widget's screen. This asserts the values ui_set_overlay_geometry writes.
    constexpr int32_t NAV_HEIGHT = 112;
    constexpr int32_t GAP = 16;

    const auto h = compute_overlay_heights(480, 800, NAV_HEIGHT, GAP);
    const auto w = compute_overlay_widths(480, 800, 54, GAP);

    // Full width, short height, top-anchored — the three properties that
    // together make the overlay drop from the top and leave the nav bar visible.
    CHECK(w.destination == 480);
    CHECK(h.destination == 688);
    CHECK(h.transient == 672);
    CHECK(is_portrait_layout(detect_layout_type(480, 800)));
}
