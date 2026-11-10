// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_product_edit_modal.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentProductEditModal;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The filament product modal titles itself and offers Delete only for an edit",
                 "[filament_catalog][modal]") {
    FilamentProductEditModal modal;
    auto shown = [](const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(ModalStack::instance().top_dialog(), name);
        REQUIRE(obj != nullptr);
        return !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    auto title = []() {
        return std::string(lv_label_get_text(
            lv_obj_find_by_name(ModalStack::instance().top_dialog(), "header_title")));
    };

    REQUIRE(modal.show_for_add(test_screen()));
    CHECK(title() == "Add Filament");
    CHECK_FALSE(shown("btn_secondary"));
    modal.hide();
    process_lvgl(20);

    // An id the shipped catalog does not hold is the user's own: it can be deleted.
    REQUIRE(modal.show_for_edit(test_screen(), "test_user_only_product"));
    CHECK(title() == "Edit Filament");
    CHECK(shown("btn_secondary"));
    modal.hide();
    process_lvgl(20);
}
