// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_editor_swatches.cpp
 * @brief The theme editor's role bars are painted chips bound to its palette,
 *        and a tap edits that role in the inline picker.
 *
 * Run with: ./build/bin/helix-tests "[theme_editor][swatch]"
 */

#include "ui_color_picker_view.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"
#include "ui_theme_editor_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "border_radius_sizes.h"
#include "color_picker_model.h"
#include "color_utils.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
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
                 "Theme editor role bars are painted chips of the palette; a tap edits that role "
                 "inline",
                 "[theme_editor][swatch][color_picker]") {
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
        INFO("role " << i);
        const std::string name = "role_" + std::to_string(i);
        lv_obj_t* bar = lv_obj_find_by_name(root, name.c_str());
        REQUIRE(bar != nullptr);
        CHECK(has_swatch_paint(bar));
        CHECK(lv_obj_get_style_radius(bar, LV_PART_MAIN) != LV_RADIUS_CIRCLE);
        CHECK(rgb(swatch_color_of(bar)) ==
              rgb(theme_manager_parse_hex_color(palette.at(i).c_str())));
    }

    // A bar's text reads on its own color: dark on Text's light bar, light on
    // Screen's dark one, after the theme preview's recolor pass.
    auto text_of = [&](const char* bar) {
        lv_obj_t* label = lv_obj_get_child(lv_obj_find_by_name(root, bar), 0);
        REQUIRE(label != nullptr);
        return theme_compute_brightness(lv_obj_get_style_text_color(label, LV_PART_MAIN));
    };
    CHECK(text_of("role_5") < 128);
    CHECK(text_of("role_0") >= 128);

    // The picker sits in the page, editing Primary, with no modal.
    REQUIRE(ColorPickerView::active() != nullptr);
    auto* picker = ColorPickerView::active();
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "theme_editor_role_selected")) == 8);
    CHECK(picker->color() == rgb(theme_manager_parse_hex_color(palette.at(8).c_str())));

    // A tap on a bar edits that role, still with no modal.
    lv_obj_send_event(lv_obj_find_by_name(root, "role_5"), LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(ModalStack::instance().empty());
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "theme_editor_role_selected")) == 5);
    CHECK(picker->color() == rgb(theme_manager_parse_hex_color(palette.at(5).c_str())));
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "color_picker_name"))) ==
          "Text");

    // A pick repaints the bar at once and marks the theme edited.
    picker->pick_cell(0);
    const uint32_t picked = helix::color_palette(helix::ColorPalette::Theme)[0];
    CHECK(rgb(swatch_color_of(lv_obj_find_by_name(root, "role_5"))) == picked);
    CHECK(editor.get_editing_theme().dark.at(5) == helix::color_to_hex_string(picked));
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "theme_editor_dirty")) == 1);
    // The strip holds the theme's own colors, the new one among them.
    const auto& strip = picker->strip_colors();
    CHECK(std::find(strip.begin(), strip.end(), picked) != strip.end());
}

TEST_CASE_METHOD(ThemeEditorSwatchFixture, "The back button asks before dropping unsaved edits",
                 "[theme_editor]") {
    auto& editor = get_theme_editor_overlay();
    REQUIRE(editor.show(lv_screen_active()));
    settle();
    lv_obj_t* root = editor.get_root();
    lv_obj_t* row = lv_obj_find_by_name(root, "row_shadow_intensity");
    REQUIRE(row != nullptr);
    lv_obj_t* slider = lv_obj_find_by_name(row, "slider");
    REQUIRE(slider != nullptr);
    lv_obj_t* save = lv_obj_find_by_name(root, "btn_save");
    REQUIRE(save != nullptr);
    lv_obj_t* title = lv_obj_find_by_name(root, "header_title");
    REQUIRE(title != nullptr);
    auto upper = [](lv_obj_t* label) {
        std::string text = lv_label_get_text(label);
        for (auto& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return text;
    };
    CHECK(lv_obj_has_state(save, LV_STATE_DISABLED));
    CHECK(upper(title).find("MODIFIED") == std::string::npos);
    // Opening fills the readouts from the theme being edited.
    const helix::ThemeData theme =
        helix::load_theme_from_file(DisplaySettingsManager::instance().get_theme_name());
    lv_obj_t* radius =
        lv_obj_find_by_name(lv_obj_find_by_name(root, "row_border_radius"), "value_label");
    CHECK(std::string(lv_label_get_text(radius)) ==
          lv_tr(helix::BorderRadiusSizes::name(theme.properties.border_radius_size)));

    lv_slider_set_value(slider, 10, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    REQUIRE(ModalStack::instance().empty());

    // An edit enables Save, marks the title and moves the readout.
    CHECK_FALSE(lv_obj_has_state(save, LV_STATE_DISABLED));
    CHECK(upper(title).find("MODIFIED") != std::string::npos);
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(row, "value_label"))) == "10");

    lv_obj_send_event(lv_obj_find_by_name(root, "back_button"), LV_EVENT_CLICKED, nullptr);
    settle();

    CHECK_FALSE(ModalStack::instance().empty());
    CHECK(editor.get_root() == root);

    // The slider previewed its edit across the app.
    theme_manager_preview(theme);
}

TEST_CASE_METHOD(ThemeEditorSwatchFixture, "Save As with an empty name says why it did nothing",
                 "[theme_editor]") {
    auto& editor = get_theme_editor_overlay();
    REQUIRE(editor.show(lv_screen_active()));
    settle();
    lv_obj_send_event(lv_obj_find_by_name(editor.get_root(), "btn_save_as"), LV_EVENT_CLICKED,
                      nullptr);
    settle();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_t* status = lv_obj_find_by_name(dialog, "save_as_status");
    lv_obj_t* input = lv_obj_find_by_name(dialog, "theme_name_input");
    REQUIRE(status != nullptr);
    REQUIRE(input != nullptr);
    CHECK(lv_obj_is_hidden(status));

    lv_textarea_set_text(input, "");
    lv_obj_t* confirm = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(confirm != nullptr);
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, nullptr);
    settle();

    CHECK_FALSE(lv_obj_is_hidden(status));
    CHECK(std::string(lv_label_get_text(status)) == lv_tr("Please enter a theme name"));

    // Save As again opens a fresh dialog with no error from the last attempt.
    lv_obj_send_event(lv_obj_find_by_name(editor.get_root(), "btn_save_as"), LV_EVENT_CLICKED,
                      nullptr);
    settle();
    REQUIRE(ModalStack::instance().top_dialog() != dialog);
    status = lv_obj_find_by_name(ModalStack::instance().top_dialog(), "save_as_status");
    REQUIRE(status != nullptr);
    CHECK(lv_obj_is_hidden(status));
    ModalStack::instance().clear();
}
