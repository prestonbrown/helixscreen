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

#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_panel_base.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"
#include "printer_state.h"
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
        // The chrome role lives in ui_xml/styles.xml; a test that runs before
        // anything registered that file gets the role alone.
        if (!cap_chrome_style()) {
            lv_xml_register_component_from_data(
                "styles", "<component><styles><style name=\"cap_chrome\"/></styles></component>");
        }
        REQUIRE(cap_chrome_style() != nullptr);
        theme_manager_register_responsive_spacing(lv_display_get_default());
        NavigationManager::instance().init();
        // The panels share a container beside the nav bar, as in the app.
        panel_container_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(panel_container_, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(panel_container_, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(panel_container_, 0, LV_PART_MAIN);
        for (auto& p : root_panels_) {
            p = lv_obj_create(panel_container_);
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

    /// A child of @p parent at 100% width, carrying the chrome role if asked.
    static lv_obj_t* add_child(lv_obj_t* parent, bool chrome) {
        lv_obj_t* child = lv_obj_create(parent);
        lv_obj_set_width(child, lv_pct(100));
        if (chrome) {
            lv_obj_add_style(child, const_cast<lv_style_t*>(cap_chrome_style()), LV_PART_MAIN);
        }
        return child;
    }

    /// An overlay built the way overlay XML is: a flex column, pushed.
    static lv_obj_t* push(IPanelLifecycle& lifecycle, bool with_chrome = false) {
        lv_obj_t* w = lv_obj_create(lv_screen_active());
        lv_obj_set_height(w, lv_pct(100));
        lv_obj_set_align(w, LV_ALIGN_RIGHT_MID);
        lv_obj_set_style_border_width(w, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(w, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_right(w, kBasePadRight, LV_PART_MAIN);
        lv_obj_set_flex_flow(w, LV_FLEX_FLOW_COLUMN);
        if (with_chrome) {
            add_child(w, true);                   // header
            add_child(add_child(w, false), true); // content holding a nested header_bar
            add_child(w, true);                   // footer
        }
        auto& nav = NavigationManager::instance();
        nav.register_overlay_instance(w, &lifecycle);
        nav.push_overlay(w);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(w);
        return w;
    }

    /// Connection loss on Controls clears the overlay stack, as in the app;
    /// then the cached @p w is pushed again. wire_events() installs the
    /// connection observer, which needs the printer-state subjects.
    static void lose_connection_and_repush(lv_obj_t* w, IPanelLifecycle& lifecycle) {
        auto& nav = NavigationManager::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        get_printer_state().init_subjects(false);
        nav.wire_events(lv_obj_create(lv_screen_active()));
        lv_subject_t* conn =
            get_printer_state().network_state().get_printer_connection_state_subject();
        REQUIRE(conn != nullptr);
        for (auto state : {ConnectionState::DISCONNECTED, ConnectionState::CONNECTED}) {
            lv_subject_set_int(conn, static_cast<int>(state));
            queue.drain();
        }
        nav.set_active(PanelId::Controls);
        queue.drain();
        REQUIRE(nav.get_active() == PanelId::Controls);
        lv_subject_set_int(conn, static_cast<int>(ConnectionState::DISCONNECTED));
        queue.drain();
        REQUIRE(lv_obj_is_hidden(w));

        nav.register_overlay_instance(w, &lifecycle);
        nav.push_overlay(w);
        queue.drain();
        lv_obj_update_layout(w);
        get_printer_state().deinit_subjects();
    }

    static int32_t read_const(const char* name) {
        const char* s = lv_xml_get_const(nullptr, name);
        REQUIRE(s != nullptr);
        return std::atoi(s);
    }

    lv_obj_t* panel_container_ = nullptr;
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

TEST_CASE_METHOD(UltrawideFixture, "Chrome spans the view while the content between is capped",
                 "[content-cap][overlay]") {
    FakeOverlay lifecycle(/*full_width=*/false);
    lv_obj_t* w = push(lifecycle, /*with_chrome=*/true);
    lv_obj_t* header = lv_obj_get_child(w, 0);
    lv_obj_t* content = lv_obj_get_child(w, 1);
    lv_obj_t* nested = lv_obj_get_child(content, 0);
    lv_obj_t* footer = lv_obj_get_child(w, 2);

    const int32_t cap = read_const("content_max_width");
    const int32_t width = lv_obj_get_width(w);
    REQUIRE(width > cap);
    CHECK(lv_obj_get_width(content) == cap - kBasePadRight);
    // Header and footer run from the leading edge to the root's own padding.
    CHECK(lv_obj_get_width(header) == width - kBasePadRight);
    CHECK(lv_obj_get_width(footer) == width - kBasePadRight);
    CHECK(lv_obj_get_scroll_right(w) == 0);
    // Chrome inside the content (the print file detail's header) caps with it.
    CHECK(lv_obj_get_width(nested) == lv_obj_get_content_width(content));

    resize(kTestDisplayWidth, kTestDisplayHeight);
    CHECK(lv_obj_get_width(header) == lv_obj_get_width(content));
    CHECK(lv_obj_get_width(footer) == lv_obj_get_width(content));
}

TEST_CASE_METHOD(UltrawideFixture, "A cached overlay reopened after the stack is cleared caps once",
                 "[content-cap][overlay]") {
    FakeOverlay lifecycle(/*full_width=*/false);
    lv_obj_t* w = push(lifecycle, /*with_chrome=*/true);
    lv_obj_t* header = lv_obj_get_child(w, 0);
    const int32_t pad_right = lv_obj_get_style_pad_right(w, LV_PART_MAIN);
    const int32_t header_width = lv_obj_get_width(header);
    REQUIRE(pad_right > kBasePadRight);

    lose_connection_and_repush(w, lifecycle);

    CHECK(lv_obj_get_style_pad_right(w, LV_PART_MAIN) == pad_right);
    CHECK(lv_obj_get_width(header) == header_width);
}

TEST_CASE_METHOD(UltrawideFixture,
                 "An overlay marked unmanaged once stays unmanaged after a stack clear",
                 "[content-cap][overlay]") {
    // Marked once at creation, as the printer image tagger does.
    FakeOverlay lifecycle(/*full_width=*/false);
    lv_obj_t* w = lv_obj_create(lv_screen_active());
    lv_obj_set_width(w, 500);
    helix::nav::set_overlay_width_unmanaged(w);
    auto& nav = NavigationManager::instance();
    nav.register_overlay_instance(w, &lifecycle);
    nav.push_overlay(w);
    helix::ui::UpdateQueue::instance().drain();
    const int32_t pad_right = lv_obj_get_style_pad_right(w, LV_PART_MAIN);

    lose_connection_and_repush(w, lifecycle);

    CHECK(lv_obj_get_width(w) == 500);
    CHECK(lv_obj_get_style_pad_right(w, LV_PART_MAIN) == pad_right);
}

TEST_CASE_METHOD(UltrawideFixture, "Views that opt out keep the full width",
                 "[content-cap][overlay]") {
    FakeOverlay full_overlay(/*full_width=*/true);
    lv_obj_t* overlay = push(full_overlay);
    CHECK(lv_obj_get_style_pad_right(overlay, LV_PART_MAIN) == kBasePadRight);

    // The container caps or not with the shown panel's declaration.
    auto& nav = NavigationManager::instance();
    const PanelId shown = PanelId::Controls;
    lv_obj_t* panel = root_panels_[static_cast<int>(shown)];
    const int32_t full = lv_obj_get_width(panel_container_);
    FakePanel capped(/*full_width=*/false);
    nav.register_panel_instance(shown, &capped);
    nav.set_active(shown);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(lv_obj_is_hidden(panel));
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_width(panel) == read_const("content_max_width"));

    FakePanel opted_out(/*full_width=*/true);
    nav.register_panel_instance(shown, &opted_out);
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_width(panel) == full);

    nav.register_panel_instance(shown, nullptr);
}

namespace {

/// A full-screen panel container built with @p base_pad_right, laid out.
lv_obj_t* make_panel_container(int32_t base_pad_right) {
    lv_obj_t* c = lv_obj_create(lv_screen_active());
    lv_obj_set_size(c, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(c, base_pad_right, LV_PART_MAIN);
    lv_obj_update_layout(c);
    return c;
}

/// A shown panel widget filling @p container.
lv_obj_t* make_panel_widget(lv_obj_t* container) {
    lv_obj_t* w = lv_obj_create(container);
    lv_obj_set_width(w, lv_pct(100));
    return w;
}

} // namespace

TEST_CASE_METHOD(UltrawideFixture, "A panel widget swapped into a shown slot carries the cap",
                 "[content-cap][panel]") {
    helix::ui::PanelRegistry registry;
    const int slot = static_cast<int>(PanelId::Controls);
    const bool opts_out = GENERATE(false, true);
    CAPTURE(opts_out);
    FakePanel panel(/*full_width=*/opts_out);
    registry.set_instance(slot, &panel);

    lv_obj_t* first_container = make_panel_container(kBasePadRight);
    registry.replace_widget(slot, make_panel_widget(first_container));

    // The panel rebuilds its widget into a fresh layout.
    constexpr int32_t kFreshPad = 10;
    lv_obj_t* fresh_container = make_panel_container(kFreshPad);
    const int32_t overflow = lv_obj_get_width(fresh_container) - read_const("content_max_width");
    REQUIRE(overflow > 0);
    registry.replace_widget(slot, make_panel_widget(fresh_container));

    CHECK(lv_obj_get_style_pad_right(fresh_container, LV_PART_MAIN) ==
          kFreshPad + (opts_out ? 0 : overflow));
}

TEST_CASE_METHOD(UltrawideFixture, "A reset registry reads the container's padding afresh",
                 "[content-cap][panel]") {
    helix::ui::PanelRegistry registry;
    const int slot = static_cast<int>(PanelId::Controls);
    FakePanel panel(/*full_width=*/false);
    lv_obj_t* container = make_panel_container(kBasePadRight);
    const int32_t overflow = lv_obj_get_width(container) - read_const("content_max_width");
    REQUIRE(overflow > 0);

    registry.set_instance(slot, &panel);
    registry.replace_widget(slot, make_panel_widget(container));
    REQUIRE(lv_obj_get_style_pad_right(container, LV_PART_MAIN) == kBasePadRight + overflow);

    // Rebuilt from scratch: the container's layout is written anew with another
    // padding, and the panels register again.
    registry.reset();
    constexpr int32_t kRebuiltPad = 10;
    lv_obj_clean(container);
    lv_obj_set_style_pad_right(container, kRebuiltPad, LV_PART_MAIN);
    registry.set_instance(slot, &panel);
    registry.replace_widget(slot, make_panel_widget(container));

    CHECK(lv_obj_get_style_pad_right(container, LV_PART_MAIN) == kRebuiltPad + overflow);
}
