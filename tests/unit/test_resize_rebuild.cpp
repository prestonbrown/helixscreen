// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_resize_rebuild.cpp
 * @brief A runtime resize rebuilds the views it would leave stale (#1214)
 *
 * Widgets keep the tokens they were built with. A resize across a tier rebuilds
 * what is on screen at once and every other main panel the next time it is
 * shown; a resize inside a tier rebuilds nothing.
 */

#include "ui_effects.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_base.h"

#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
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

    // Labels without a font of their own follow the theme's, which moves with the tier.
    const lv_font_t* theme_font = lv_theme_get_font_normal(test_screen());
    {
        ScopedResolution micro(disp, 480, 272);
        CHECK(theme_manager_refresh_layout_constants(disp));
        CHECK_FALSE(theme_manager_refresh_layout_constants(disp));
        CHECK(lv_theme_get_font_normal(test_screen()) != theme_font);
    }
    CHECK(theme_manager_refresh_layout_constants(disp));
    CHECK(lv_theme_get_font_normal(test_screen()) == theme_font);
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "a layout change rebuilds the shown panel now, a hidden one when shown, "
                 "and keeps the chrome and open modal above the rebuilt overlay",
                 "[resize][navigation][1214]") {
    lv_xml_register_component_from_data(
        "resize_rebuild_panel",
        "<component><view extends=\"lv_obj\" width=\"100\" height=\"100\"/></component>");
    auto& nav = NavigationManager::instance();
    nav.init();

    lv_obj_t* panels[kUiPanelCount] = {nullptr};
    panels[static_cast<int>(PanelId::Home)] =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "resize_rebuild_panel", nullptr));
    panels[static_cast<int>(PanelId::Controls)] =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "resize_rebuild_panel", nullptr));
    lv_obj_add_flag(panels[static_cast<int>(PanelId::Controls)], LV_OBJ_FLAG_HIDDEN);
    nav.set_panels(panels);

    CountingPanel home(state(), &api());
    CountingPanel controls(state(), &api());
    home.setup(panels[static_cast<int>(PanelId::Home)], test_screen());
    controls.setup(panels[static_cast<int>(PanelId::Controls)], test_screen());
    nav.register_panel_instance(PanelId::Home, &home);
    nav.register_panel_instance(PanelId::Controls, &controls);
    REQUIRE(nav.get_active() == PanelId::Home);
    lv_obj_t* chrome = lv_obj_create(test_screen());
    helix::ui::add_always_on_top(chrome);
    RebuiltOverlay overlay;
    REQUIRE(overlay.show(test_screen()));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* overlay_before = overlay.get_root();

    nav.rebuild_for_layout_change();

    // The chrome (the E-stop) stays over the rebuilt overlay.
    REQUIRE(overlay.get_root() != overlay_before);
    CHECK(lv_obj_get_index(chrome) > lv_obj_get_index(overlay.get_root()));

    KeptModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* dialog = modal.dialog();
    nav.rebuild_for_layout_change();

    CHECK(modal.hides == 0);
    CHECK(ModalStack::instance().top_dialog() == dialog);
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(dialog);
    REQUIRE(lv_obj_get_parent(backdrop) == lv_obj_get_parent(overlay.get_root()));
    CHECK(lv_obj_get_index(backdrop) > lv_obj_get_index(overlay.get_root()));
    modal.hide();
    helix::ui::remove_always_on_top(chrome);
    lv_obj_delete(chrome);
    nav.go_back();
    helix::ui::UpdateQueue::instance().drain();

    CHECK(nav.get_panel_widget(PanelId::Home) != panels[static_cast<int>(PanelId::Home)]);
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

    nav.set_active(PanelId::Home);
    nav.register_panel_instance(PanelId::Home, nullptr);
    nav.register_panel_instance(PanelId::Controls, nullptr);
    lv_obj_t* null_panels[kUiPanelCount] = {nullptr};
    nav.set_panels(null_panels);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);
    nav.deinit_subjects();
}
