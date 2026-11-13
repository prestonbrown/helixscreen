// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_panel_registry.cpp
 * @brief PanelRegistry: main-panel widgets, instances and the lazy builder
 */

#include "ui_nav_panel_registry.h"
#include "ui_panel_base.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PanelRegistry;

namespace {

class StubPanel : public PanelBase {
  public:
    StubPanel() : PanelBase(get_printer_state(), nullptr) {}
    void init_subjects() override {}
    const char* get_name() const override {
        return "StubPanel";
    }
    const char* get_xml_component_name() const override {
        return "stub_panel";
    }
};

constexpr int kHome = static_cast<int>(PanelId::Home);
constexpr int kControls = static_cast<int>(PanelId::Controls);
constexpr int kSettings = static_cast<int>(PanelId::Settings);

struct RegistryFixture : public LVGLUITestFixture {
    RegistryFixture() {
        home = lv_obj_create(test_screen());
        controls = lv_obj_create(test_screen());
        lv_obj_t* panels[PanelRegistry::kCount] = {nullptr};
        panels[kHome] = home;
        panels[kControls] = controls;
        registry.set_widgets(panels);
    }

    PanelRegistry registry;
    lv_obj_t* home = nullptr;
    lv_obj_t* controls = nullptr;
};

} // namespace

TEST_CASE_METHOD(RegistryFixture, "Widgets are found by slot and by pointer",
                 "[navigation][panel_registry]") {
    CHECK(registry.widget(kHome) == home);
    CHECK(registry.widget(kSettings) == nullptr);
    CHECK(registry.widget(-1) == nullptr);
    CHECK(registry.widget(PanelRegistry::kCount) == nullptr);

    CHECK(registry.index_of(controls) == kControls);
    CHECK(registry.is_main_panel(home));
    CHECK_FALSE(registry.is_main_panel(test_screen()));
    CHECK(registry.index_of(nullptr) == -1);
}

TEST_CASE_METHOD(RegistryFixture, "A slot clears itself when its widget is deleted",
                 "[navigation][panel_registry]") {
    lv_obj_delete(controls);

    CHECK(registry.widget(kControls) == nullptr);
    CHECK_FALSE(registry.is_main_panel(controls));
    CHECK(registry.widget(kHome) == home);
}

TEST_CASE_METHOD(RegistryFixture, "replace_widget returns the displaced widget",
                 "[navigation][panel_registry]") {
    lv_obj_t* successor = lv_obj_create(test_screen());

    CHECK(registry.replace_widget(kHome, successor) == home);
    CHECK(registry.widget(kHome) == successor);
    CHECK(registry.replace_widget(kSettings, home) == nullptr);
    CHECK(registry.replace_widget(PanelRegistry::kCount, home) == nullptr);
}

TEST_CASE_METHOD(RegistryFixture, "show_only shows one panel and hides the rest",
                 "[navigation][panel_registry]") {
    registry.show_only(kControls);
    CHECK(lv_obj_is_hidden(home));
    CHECK_FALSE(lv_obj_is_hidden(controls));

    registry.show_only(-1);
    CHECK(lv_obj_is_hidden(home));
    CHECK(lv_obj_is_hidden(controls));
}

TEST_CASE_METHOD(RegistryFixture, "Instances register by slot and are found by pointer",
                 "[navigation][panel_registry]") {
    StubPanel panel;
    registry.set_instance(kSettings, &panel);

    CHECK(registry.instance(kSettings) == &panel);
    CHECK(registry.find(&panel) == PanelId::Settings);
    CHECK(registry.find(nullptr) == PanelId::Count);

    registry.set_instance(PanelRegistry::kCount, &panel);
    CHECK(registry.instance(PanelRegistry::kCount) == nullptr);

    registry.clear_instances();
    CHECK(registry.instance(kSettings) == nullptr);
    CHECK(registry.find(&panel) == PanelId::Count);
}

TEST_CASE_METHOD(RegistryFixture, "reset forgets widgets and instances",
                 "[navigation][panel_registry]") {
    StubPanel panel;
    registry.set_instance(kHome, &panel);

    registry.reset();

    CHECK(registry.widget(kHome) == nullptr);
    CHECK(registry.instance(kHome) == nullptr);
}

TEST_CASE_METHOD(RegistryFixture, "The deferred builder runs once for an empty slot",
                 "[navigation][panel_registry]") {
    int builds = 0;
    int built_idx = -1;
    registry.set_deferred_builder([&](int idx) {
        ++builds;
        built_idx = idx;
        registry.replace_widget(idx, lv_obj_create(test_screen()));
    });

    CHECK_FALSE(registry.needs_build(kHome)); // already built
    CHECK_FALSE(registry.needs_build(PanelRegistry::kCount));
    CHECK(registry.needs_build(kSettings));

    registry.ensure_built(kHome);
    CHECK(builds == 0);

    registry.ensure_built(kSettings);
    CHECK(builds == 1);
    CHECK(built_idx == kSettings);
    CHECK(registry.widget(kSettings) != nullptr);

    registry.ensure_built(kSettings);
    CHECK(builds == 1);
}

TEST_CASE_METHOD(RegistryFixture, "Without a builder nothing is built",
                 "[navigation][panel_registry]") {
    CHECK_FALSE(registry.needs_build(kSettings));
    registry.ensure_built(kSettings);
    CHECK(registry.widget(kSettings) == nullptr);
}

TEST_CASE_METHOD(RegistryFixture, "The builder is not re-entered while it runs",
                 "[navigation][panel_registry]") {
    int builds = 0;
    registry.set_deferred_builder([&](int idx) {
        ++builds;
        registry.ensure_built(idx); // slot still empty here
    });

    registry.ensure_built(kSettings);

    CHECK(builds == 1);
}
