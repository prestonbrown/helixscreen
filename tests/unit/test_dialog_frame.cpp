// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_dialog_frame.cpp
 * @brief dialog_frame: the shared modal card every dialog extends.
 *
 * A dialog's own widgets land in the padded body, its buttons in the footer,
 * the title sits at the left of the header with the icon before it, and a
 * body taller than #dialog_content_max scrolls inside the card. modal_dialog
 * (modal_confirm's card) is built on the frame.
 */

#include "ui_icon_codepoints.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "theme_manager.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

using ::ModalSeverity;

namespace {

bool is_ancestor(lv_obj_t* ancestor, lv_obj_t* obj) {
    for (lv_obj_t* p = obj; p != nullptr; p = lv_obj_get_parent(p)) {
        if (p == ancestor)
            return true;
    }
    return false;
}

/// The visible labels under @p root whose text is @p icon's glyph.
int visible_glyphs(lv_obj_t* root, const char* icon) {
    const char* glyph = helix::ui::icon::lookup_codepoint(icon);
    REQUIRE(glyph != nullptr);
    int found = 0;
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        if (lv_obj_is_hidden(child))
            continue;
        if (lv_obj_check_type(child, &lv_label_class) &&
            std::strcmp(lv_label_get_text(child), glyph) == 0)
            ++found;
        found += visible_glyphs(child, icon);
    }
    return found;
}

void register_test_dialog() {
    static bool registered = false;
    if (registered)
        return;
    REQUIRE(lv_xml_register_component_from_data(
                "dialog_frame_test_dialog",
                "<component>"
                "<api><prop name=\"icon\" type=\"string\" default=\"\"/></api>"
                "<view extends=\"dialog_frame\" title=\"Rename\" icon=\"$icon\" "
                "icon_variant=\"warning\">"
                "<text_body name=\"body_line\" text=\"Body\"/>"
                "<dialog_frame-footer>"
                "<modal_button_row primary_text=\"Save\"/>"
                "</dialog_frame-footer>"
                "</view>"
                "</component>") == LV_RESULT_OK);
    registered = true;
}

lv_obj_t* create_test_dialog(const char* icon) {
    register_test_dialog();
    const char* attrs[] = {"icon", icon, nullptr};
    auto* dlg = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "dialog_frame_test_dialog", attrs));
    REQUIRE(dlg != nullptr);
    lv_obj_update_layout(dlg);
    return dlg;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "dialog_frame routes body children and the footer row",
                 "[dialog_frame][xml]") {
    lv_obj_t* dlg = create_test_dialog("");

    lv_obj_t* body = lv_xml_component_get_slot(dlg, nullptr);
    lv_obj_t* footer = lv_xml_component_get_slot(dlg, "footer");
    REQUIRE(body != nullptr);
    REQUIRE(footer != nullptr);
    CHECK(body == lv_obj_find_by_name(dlg, "dialog_body"));

    lv_obj_t* line = lv_obj_find_by_name(dlg, "body_line");
    REQUIRE(line != nullptr);
    CHECK(lv_obj_get_parent(line) == body);

    lv_obj_t* save = lv_obj_find_by_name(dlg, "btn_primary");
    REQUIRE(save != nullptr);
    CHECK(is_ancestor(footer, save));
    CHECK_FALSE(is_ancestor(body, save));
    CHECK_FALSE(lv_obj_is_hidden(footer));

    // Body, then footer, both under the header.
    lv_area_t body_a, footer_a, title_a;
    lv_obj_get_coords(body, &body_a);
    lv_obj_get_coords(footer, &footer_a);
    lv_obj_get_coords(lv_obj_find_by_name(dlg, "dialog_title"), &title_a);
    CHECK(title_a.y2 <= body_a.y1);
    CHECK(body_a.y2 <= footer_a.y1);

    // Padded sides and the column gap.
    const int lg = theme_manager_get_spacing("space_lg");
    CHECK(lv_obj_get_style_pad_left(body, LV_PART_MAIN) == lg);
    CHECK(lv_obj_get_style_pad_right(body, LV_PART_MAIN) == lg);
    CHECK(lv_obj_get_style_pad_row(body, LV_PART_MAIN) == theme_manager_get_spacing("space_md"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "dialog_frame left-aligns the title after its icon",
                 "[dialog_frame][xml]") {
    SECTION("with an icon") {
        lv_obj_t* dlg = create_test_dialog("alert");
        lv_obj_t* title = lv_obj_find_by_name(dlg, "dialog_title");
        lv_obj_t* icon = lv_obj_find_by_name(dlg, "dialog_icon");
        REQUIRE(title != nullptr);
        REQUIRE(icon != nullptr);
        CHECK(std::string(lv_label_get_text(title)) == "Rename");
        CHECK(lv_obj_get_style_text_align(title, LV_PART_MAIN) == LV_TEXT_ALIGN_LEFT);
        CHECK_FALSE(lv_obj_is_hidden(icon));
        CHECK(visible_glyphs(dlg, "alert") == 1);

        lv_area_t icon_a, title_a, dlg_a;
        lv_obj_get_coords(icon, &icon_a);
        lv_obj_get_coords(title, &title_a);
        lv_obj_get_coords(dlg, &dlg_a);
        CHECK(icon_a.x2 <= title_a.x1);
        // The icon starts at the card's left inset, not centered.
        CHECK(icon_a.x1 - dlg_a.x1 <= theme_manager_get_spacing("space_lg") + 2);
    }
    SECTION("without an icon the title starts at the left inset") {
        lv_obj_t* dlg = create_test_dialog("");
        lv_obj_t* title = lv_obj_find_by_name(dlg, "dialog_title");
        lv_obj_t* icon = lv_obj_find_by_name(dlg, "dialog_icon");
        REQUIRE(title != nullptr);
        REQUIRE(icon != nullptr);
        CHECK(lv_obj_is_hidden(icon));
        lv_area_t title_a, dlg_a;
        lv_obj_get_coords(title, &title_a);
        lv_obj_get_coords(dlg, &dlg_a);
        CHECK(title_a.x1 - dlg_a.x1 <= theme_manager_get_spacing("space_lg") + 2);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "dialog_frame caps the body and scrolls it",
                 "[dialog_frame][xml]") {
    lv_obj_t* dlg = create_test_dialog("");
    lv_obj_t* body = lv_xml_component_get_slot(dlg, nullptr);
    REQUIRE(body != nullptr);

    for (int i = 0; i < 60; ++i) {
        lv_obj_t* row = lv_obj_create(body);
        lv_obj_set_size(row, 50, 40);
    }
    lv_obj_update_layout(dlg);

    const int32_t cap = lv_obj_get_style_max_height(body, LV_PART_MAIN);
    CHECK(cap != LV_COORD_MAX);
    CHECK(lv_obj_get_height(body) <= cap);
    CHECK(lv_obj_is_scrollable(body));
    CHECK(lv_obj_get_scroll_bottom(body) > 0);

    // Fitted to the screen as the Modal system fits every dialog, the body is
    // the part that scrolls and the footer stays on the card and the screen.
    helix::ui::fit_dialog_to_screen(dlg);
    lv_obj_update_layout(dlg);
    CHECK(lv_obj_get_scroll_bottom(body) > 0);
    lv_area_t dlg_a, footer_a;
    lv_obj_get_coords(dlg, &dlg_a);
    lv_obj_get_coords(lv_xml_component_get_slot(dlg, "footer"), &footer_a);
    CHECK(footer_a.y1 > dlg_a.y1);
    CHECK(footer_a.y2 <= dlg_a.y2);
    CHECK(dlg_a.y2 < lv_obj_get_height(lv_screen_active()));
}

TEST_CASE_METHOD(LVGLUITestFixture, "dialog_frame hides an unfilled footer and header_extra",
                 "[dialog_frame][xml]") {
    auto* dlg = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "dialog_frame", nullptr));
    REQUIRE(dlg != nullptr);
    CHECK(lv_obj_is_hidden(lv_xml_component_get_slot(dlg, "footer")));
    CHECK(lv_obj_is_hidden(lv_xml_component_get_slot(dlg, "header_extra")));
}

// modal_confirm's card: title, message and the one severity icon.
TEST_CASE_METHOD(LVGLUITestFixture, "modal_confirm shows its title, message and severity icon",
                 "[dialog_frame][modal]") {
    helix::ui::modal_init_subjects();

    lv_obj_t* warn =
        helix::ui::modal_confirm("Careful", "Body text", ModalSeverity::Warning, "Go", nullptr, {});
    REQUIRE(warn != nullptr);
    process_lvgl(50);

    lv_obj_t* title = lv_obj_find_by_name(warn, helix::ui::kModalTitleWidgetName);
    REQUIRE(title != nullptr);
    CHECK(std::string(lv_label_get_text(title)) == "Careful");
    CHECK(lv_obj_get_style_text_align(title, LV_PART_MAIN) == LV_TEXT_ALIGN_LEFT);
    lv_obj_t* message = lv_obj_find_by_name(warn, "dialog_message");
    REQUIRE(message != nullptr);
    CHECK(std::string(lv_label_get_text(message)) == "Body text");
    CHECK(visible_glyphs(warn, "alert") == 1);
    CHECK(visible_glyphs(warn, "alert_octagon") == 0);
    CHECK(visible_glyphs(warn, "info") == 0);

    lv_obj_t* err =
        helix::ui::modal_confirm("Broken", "Body", ModalSeverity::Error, "Delete", nullptr, {});
    REQUIRE(err != nullptr);
    process_lvgl(50);
    CHECK(visible_glyphs(err, "alert_octagon") == 1);
    CHECK(visible_glyphs(err, "alert") == 0);
}
