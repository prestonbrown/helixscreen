// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_content_width_cap_push.cpp
 * @brief The ultrawide content width cap written onto overlays and main panels
 *
 * The pure rule is covered by test_content_width_cap.cpp. These tests cover the
 * wiring: NavigationManager capping a pushed overlay through its right padding,
 * capping a main panel through max_width, honoring the opt-out declared on the
 * view class, and undoing the cap when the screen stops being ultrawide.
 */

#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "theme_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int32_t kBasePadRight = 6;

class FakeOverlay : public IPanelLifecycle {
  public:
    explicit FakeOverlay(bool full_width) : full_width_(full_width) {}
    void on_activate() override {}
    void on_deactivate(DeactivateReason) override {}
    const char* get_name() const override {
        return "FakeOverlay";
    }
    bool uses_full_content_width() const override {
        return full_width_;
    }

  private:
    bool full_width_;
};

class FakePanel : public PanelBase {
  public:
    explicit FakePanel(bool full_width)
        : PanelBase(get_printer_state(), nullptr), full_width_(full_width) {}
    void init_subjects() override {}
    const char* get_name() const override {
        return "FakePanel";
    }
    const char* get_xml_component_name() const override {
        return "fake_panel";
    }
    bool uses_full_content_width() const override {
        return full_width_;
    }

  private:
    bool full_width_;
};

/// A 1480x320 screen with its layout constants, as the resize handler leaves it.
class UltrawideFixture : public LVGLTestFixture {
  public:
    UltrawideFixture() {
        theme_manager_register_responsive_spacing(lv_display_get_default());
        NavigationManager::instance().init();
        for (auto& p : root_panels_) {
            p = lv_obj_create(lv_screen_active());
            lv_obj_set_width(p, lv_pct(100));
        }
        NavigationManager::instance().set_panels(root_panels_.data());
        resize(1480, 320);
    }

    ~UltrawideFixture() override {
        // Constants only: the test's lifecycles are gone, so nothing re-applies.
        lv_display_t* disp = lv_display_get_default();
        lv_display_set_resolution(disp, kTestDisplayWidth, kTestDisplayHeight);
        theme_manager_refresh_layout_constants(disp);
        helix::ui::UpdateQueue::instance().drain();
        NavigationManager::instance().deinit_subjects();
    }

    /// What Application's resize handler does: new constants, then re-apply.
    static void resize(int32_t w, int32_t h) {
        lv_display_t* disp = lv_display_get_default();
        lv_display_set_resolution(disp, w, h);
        theme_manager_refresh_layout_constants(disp);
        NavigationManager::instance().reapply_overlay_widths();
        lv_obj_update_layout(lv_screen_active());
    }

    static lv_obj_t* push(IPanelLifecycle& lifecycle) {
        lv_obj_t* w = lv_obj_create(lv_screen_active());
        lv_obj_set_height(w, lv_pct(100));
        lv_obj_set_align(w, LV_ALIGN_RIGHT_MID);
        lv_obj_set_style_border_width(w, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_right(w, kBasePadRight, LV_PART_MAIN);
        auto& nav = NavigationManager::instance();
        nav.register_overlay_instance(w, &lifecycle);
        nav.push_overlay(w);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(w);
        return w;
    }

    static int32_t read_const(const char* name) {
        const char* s = lv_xml_get_const(nullptr, name);
        REQUIRE(s != nullptr);
        return std::atoi(s);
    }

    std::array<lv_obj_t*, kUiPanelCount> root_panels_{};
};

} // namespace

TEST_CASE_METHOD(UltrawideFixture, "An overlay on an ultrawide screen stops at the cap",
                 "[content-cap][overlay]") {
    const int32_t cap = read_const("content_max_width");
    REQUIRE(cap == 768);

    FakeOverlay lifecycle(/*full_width=*/false);
    lv_obj_t* w = push(lifecycle);

    // The root keeps its full opaque width and occludes the backdrop as before.
    const int32_t width = read_const("overlay_width_transient");
    REQUIRE(width > cap);
    CHECK(lv_obj_get_width(w) == width);

    // Its content ends cap px from its leading edge, beside the nav bar; the
    // overflow sits empty on the right, on top of the padding it was built with.
    CHECK(lv_obj_get_style_pad_right(w, LV_PART_MAIN) == kBasePadRight + width - cap);
    lv_area_t box;
    lv_area_t content;
    lv_obj_get_coords(w, &box);
    lv_obj_get_content_coords(w, &content);
    CHECK(content.x2 + 1 - box.x1 == cap - kBasePadRight);

    // Back at 800x480 the cap no longer binds and the built padding returns.
    resize(kTestDisplayWidth, kTestDisplayHeight);
    CHECK(lv_obj_get_style_pad_right(w, LV_PART_MAIN) == kBasePadRight);
}

TEST_CASE_METHOD(UltrawideFixture, "Views that opt out keep the full width",
                 "[content-cap][overlay]") {
    FakeOverlay full_overlay(/*full_width=*/true);
    lv_obj_t* overlay = push(full_overlay);
    CHECK(lv_obj_get_style_pad_right(overlay, LV_PART_MAIN) == kBasePadRight);

    FakePanel capped(/*full_width=*/false);
    FakePanel full(/*full_width=*/true);
    auto& nav = NavigationManager::instance();
    nav.register_panel_instance(PanelId::Controls, &capped);
    nav.register_panel_instance(PanelId::Home, &full);
    lv_obj_update_layout(lv_screen_active());

    const int32_t screen_width = lv_obj_get_width(lv_screen_active());
    CHECK(lv_obj_get_width(root_panels_[static_cast<int>(PanelId::Controls)]) ==
          read_const("content_max_width"));
    CHECK(lv_obj_get_width(root_panels_[static_cast<int>(PanelId::Home)]) == screen_width);

    nav.register_panel_instance(PanelId::Controls, nullptr);
    nav.register_panel_instance(PanelId::Home, nullptr);
}
