// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_resize_rebuild.cpp
 * @brief A runtime resize rebuilds the views it would leave stale (#1214)
 *
 * Widgets keep the tokens they were built with. A resize across a tier rebuilds
 * the shown panel and closed overlays once nothing is in use, and every other
 * main panel the next time it is shown; a resize inside a tier rebuilds nothing.
 */

#include "ui_effects.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_base.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "overlay_base.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

using helix::PanelId;

namespace {

class CountingPanel : public PanelBase {
  public:
    CountingPanel(helix::PrinterState& state, MoonrakerAPI* api) : PanelBase(state, api) {
        subjects_initialized_ = true;
    }

    void init_subjects() override {}

    const char* get_name() const override {
        return "CountingPanel";
    }

    const char* get_xml_component_name() const override {
        return "resize_rebuild_panel";
    }

    void on_activate() override {
        ++activations;
    }

    int activations = 0;
};

class RebuiltOverlay : public OverlayBase {
  public:
    void init_subjects() override {
        subjects_initialized_ = true;
    }

    lv_obj_t* create(lv_obj_t* parent) override {
        parent_screen_ = parent;
        overlay_root_ = lv_obj_create(parent);
        lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
        return overlay_root_;
    }

    const char* get_name() const override {
        return "RebuiltOverlay";
    }
    // A closed overlay that keeps its tree is the one these rebuilds reach.
    bool destroy_on_close() const override {
        return false;
    }
};

/// Two main panels, Home shown, built from one stub component.
class ResizeRebuildFixture : public MoonrakerTestFixture {
  protected:
    CountingPanel home{state(), &api()};
    CountingPanel controls{state(), &api()};
    lv_obj_t* panels[kUiPanelCount] = {nullptr};

    ResizeRebuildFixture() {
        lv_xml_register_component_from_data(
            "resize_rebuild_panel",
            "<component><view extends=\"lv_obj\" width=\"100\" height=\"100\"/></component>");
        auto& nav = NavigationManager::instance();
        nav.init();
        for (PanelId id : {PanelId::Home, PanelId::Controls}) {
            panels[static_cast<int>(id)] = static_cast<lv_obj_t*>(
                lv_xml_create(test_screen(), "resize_rebuild_panel", nullptr));
        }
        lv_obj_add_flag(panels[static_cast<int>(PanelId::Controls)], LV_OBJ_FLAG_HIDDEN);
        nav.set_panels(panels);
        home.setup(panels[static_cast<int>(PanelId::Home)], test_screen());
        controls.setup(panels[static_cast<int>(PanelId::Controls)], test_screen());
        nav.register_panel_instance(PanelId::Home, &home);
        nav.register_panel_instance(PanelId::Controls, &controls);
    }

    ~ResizeRebuildFixture() override {
        auto& nav = NavigationManager::instance();
        nav.set_active(PanelId::Home);
        nav.register_panel_instance(PanelId::Home, nullptr);
        nav.register_panel_instance(PanelId::Controls, nullptr);
        lv_obj_t* null_panels[kUiPanelCount] = {nullptr};
        nav.set_panels(null_panels);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(50);
        nav.deinit_subjects();
    }

    lv_obj_t* shown_home() {
        return NavigationManager::instance().get_panel_widget(PanelId::Home);
    }
};

class KeptModal : public Modal {
  public:
    const char* get_name() const override {
        return "KeptModal";
    }
    const char* component_name() const override {
        return "resize_rebuild_panel";
    }
    void on_hide() override {
        ++hides;
    }

    int hides = 0;
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "a resize reports moved tokens only across a tier",
                 "[resize][theme][1214]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    theme_manager_refresh_layout_constants(disp);

    {
        // Same tiers on every ladder: only the overlay widths follow the pixels.
        ScopedResolution nudged(disp, 820, 490);
        CHECK_FALSE(theme_manager_refresh_layout_constants(disp));
    }
    CHECK_FALSE(theme_manager_refresh_layout_constants(disp));

    {
        // Breakpoints unchanged: only nav_width's own ladder steps up.
        ScopedResolution wider(disp, 1000, 480);
        CHECK(theme_manager_refresh_layout_constants(disp));
    }
    CHECK(theme_manager_refresh_layout_constants(disp));

    // Labels without a font of their own follow the theme's, which moves with the
    // tier. Nothing else of LVGL's default theme moves, though a light/dark switch
    // since startup left it behind the active mode.
    const lv_font_t* theme_font = lv_theme_get_font_normal(test_screen());
    const lv_theme_t* base = lv_theme_default_get();
    REQUIRE(base != nullptr);
    const uint32_t base_flags = base->flags;
    const lv_color_t base_primary = base->color_primary;
    theme_manager_toggle_dark_mode();
    {
        ScopedResolution micro(disp, 480, 272);
        CHECK(theme_manager_refresh_layout_constants(disp));
        CHECK_FALSE(theme_manager_refresh_layout_constants(disp));
        CHECK(lv_theme_get_font_normal(test_screen()) != theme_font);
        CHECK(base->flags == base_flags);
        CHECK(lv_color_eq(base->color_primary, base_primary));
    }
    CHECK(theme_manager_refresh_layout_constants(disp));
    CHECK(lv_theme_get_font_normal(test_screen()) == theme_font);
    theme_manager_toggle_dark_mode();
}

TEST_CASE_METHOD(ResizeRebuildFixture,
                 "a layout change waits for an open modal, then rebuilds; a hidden panel "
                 "rebuilds once, when shown",
                 "[resize][navigation][1214]") {
    auto& nav = NavigationManager::instance();
    KeptModal modal;
    REQUIRE(modal.show(test_screen()));
    const int activations = home.activations;

    nav.rebuild_for_layout_change();
    process_lvgl(600);

    // The modal's owner is neither rebuilt nor deactivated, so its confirm still answers.
    CHECK(shown_home() == panels[static_cast<int>(PanelId::Home)]);
    CHECK(home.activations == activations);
    CHECK(modal.hides == 0);

    modal.hide();
    process_lvgl(600);
    CHECK(shown_home() != panels[static_cast<int>(PanelId::Home)]);
    lv_obj_t* hidden_before = nav.get_panel_widget(PanelId::Controls);
    CHECK(hidden_before == panels[static_cast<int>(PanelId::Controls)]);
    CHECK(controls.activations == 0);

    nav.set_active(PanelId::Controls);
    lv_obj_t* shown = nav.get_panel_widget(PanelId::Controls);
    CHECK(shown != hidden_before);
    CHECK(controls.get_panel() == shown);
    CHECK_FALSE(lv_obj_is_hidden(shown));
    CHECK(controls.activations == 1);

    // Rebuilt once: showing it again keeps the same tree.
    nav.set_active(PanelId::Home);
    nav.set_active(PanelId::Controls);
    CHECK(nav.get_panel_widget(PanelId::Controls) == shown);
}

TEST_CASE_METHOD(ResizeRebuildFixture,
                 "a layout change waits for an open overlay and for its close animation",
                 "[resize][navigation][1214]") {
    auto& nav = NavigationManager::instance();
    RebuiltOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* open_root = overlay.get_root();

    nav.rebuild_for_layout_change();
    process_lvgl(600);
    // A running measurement in the open overlay is left alone.
    CHECK(overlay.get_root() == open_root);
    CHECK(shown_home() == panels[static_cast<int>(PanelId::Home)]);

    nav.go_back();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(nav.is_panel_in_stack(open_root));
    // Stands in for the slide-out, whose completion retires the overlay.
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, open_root);
    lv_anim_set_values(&slide, 0, 100);
    lv_anim_set_duration(&slide, 400);
    lv_anim_set_exec_cb(&slide, [](void*, int32_t) {});
    lv_anim_start(&slide);
    process_lvgl(300);
    CHECK(overlay.get_root() == open_root);

    process_lvgl(600);
    CHECK(overlay.get_root() != open_root);
    CHECK(shown_home() != panels[static_cast<int>(PanelId::Home)]);
}

TEST_CASE_METHOD(ResizeRebuildFixture, "a layout change waits while text is being typed",
                 "[resize][navigation][1214]") {
    auto& keyboard = KeyboardManager::instance();
    keyboard.reset();
    keyboard.init(test_screen());
    lv_obj_t* field = lv_textarea_create(shown_home());
    keyboard.register_textarea(field);
    keyboard.show(field);
    REQUIRE(keyboard.is_visible());

    NavigationManager::instance().rebuild_for_layout_change();
    process_lvgl(600);
    CHECK(shown_home() == panels[static_cast<int>(PanelId::Home)]);

    keyboard.hide();
    process_lvgl(600);
    CHECK(shown_home() != panels[static_cast<int>(PanelId::Home)]);
    keyboard.reset();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ResizeRebuildFixture,
                 "a hot-reload rebuild keeps the chrome above the rebuilt overlay",
                 "[resize][navigation][hot-reload][1214]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* chrome = lv_obj_create(test_screen());
    helix::ui::add_always_on_top(chrome);
    RebuiltOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* overlay_before = overlay.get_root();

    nav.rebuild_active_views();

    // The chrome (the E-stop) stays over the rebuilt overlay.
    REQUIRE(overlay.get_root() != overlay_before);
    CHECK(lv_obj_get_index(chrome) > lv_obj_get_index(overlay.get_root()));

    helix::ui::remove_always_on_top(chrome);
    lv_obj_delete(chrome);
    nav.go_back();
    helix::ui::UpdateQueue::instance().drain();
}
