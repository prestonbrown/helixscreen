// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_editor_swatches.cpp
 * @brief The theme editor's role swatches are painter circles bound to its
 *        palette, and a tap opens the picker for that palette slot.
 *
 * Run with: ./build/bin/helix-tests "[theme_editor][swatch]"
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"
#include "ui_theme_editor_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <array>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

class ThemeEditorSwatchFixture : public LVGLUITestFixture {
  protected:
    ThemeEditorSwatchFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~ThemeEditorSwatchFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

} // namespace

TEST_CASE_METHOD(ThemeEditorSwatchFixture,
                 "Theme editor swatches are painter circles showing the palette",
                 "[theme_editor][swatch]") {
    auto& editor = get_theme_editor_overlay();
    REQUIRE(editor.show(lv_screen_active()));
    settle();
    lv_obj_t* root = editor.get_root();
    REQUIRE(root != nullptr);

    const helix::ThemeData theme =
        helix::load_theme_from_file(DisplaySettingsManager::instance().get_theme_name());
    REQUIRE(theme.is_valid());
    const helix::ModePalette& palette = theme.dark;

    const size_t count = helix::ModePalette::color_names().size();
    REQUIRE(count == 16);
    for (size_t i = 0; i < count; ++i) {
        INFO("swatch " << i);
        const std::string name = "swatch_" + std::to_string(i);
        lv_obj_t* swatch = lv_obj_find_by_name(root, name.c_str());
        REQUIRE(swatch != nullptr);
        lv_obj_t* disc = lv_obj_find_by_name(swatch, "color_swatch_disc");
        REQUIRE(disc != nullptr);
        CHECK(has_swatch_paint(disc));
        CHECK(lv_obj_get_style_radius(disc, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
        CHECK(lv_obj_get_style_border_width(swatch, LV_PART_MAIN) == 0);
        CHECK(rgb(swatch_color_of(swatch)) ==
              rgb(theme_manager_parse_hex_color(palette.at(i).c_str())));
    }
}

TEST_CASE_METHOD(ThemeEditorSwatchFixture, "Tapping a theme swatch opens the picker",
                 "[theme_editor][swatch]") {
    auto& editor = get_theme_editor_overlay();
    REQUIRE(editor.show(lv_screen_active()));
    settle();
    lv_obj_t* swatch = lv_obj_find_by_name(editor.get_root(), "swatch_5");
    REQUIRE(swatch != nullptr);
    REQUIRE(ModalStack::instance().empty());

    lv_obj_send_event(swatch, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(ModalStack::instance().top_component_name() == "color_picker");
}
