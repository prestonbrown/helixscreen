// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_product_edit_modal.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentProductEditModal;

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "Two filament product modals each title themselves and offer Delete only for an edit",
    "[filament_catalog][modal]") {
    // The Filament panel and the AMS editor each keep one alive all session.
    FilamentProductEditModal add_modal;
    FilamentProductEditModal edit_modal;
    auto dialog = [](const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(ModalStack::instance().top_dialog(), name);
        REQUIRE(obj != nullptr);
        return obj;
    };
    auto title = [&dialog]() { return std::string(lv_label_get_text(dialog("header_title"))); };
    auto delete_shown = [&dialog]() { return !lv_obj_is_hidden(dialog("btn_secondary")); };

    // An id the shipped catalog does not hold is the user's own: it can be deleted.
    for (int round = 0; round < 2; ++round) {
        REQUIRE(edit_modal.show_for_edit(test_screen(), "test_user_only_product"));
        CHECK(title() == "Edit Filament");
        CHECK(delete_shown());
        edit_modal.hide();
        process_lvgl(20);

        REQUIRE(add_modal.show_for_add(test_screen()));
        CHECK(title() == "Add Filament");
        CHECK_FALSE(delete_shown());
        add_modal.hide();
        process_lvgl(20);

        // A shipped product the user never overrode has nothing to delete.
        REQUIRE(edit_modal.show_for_edit(test_screen(), "creality-cr-pla"));
        CHECK_FALSE(delete_shown());
        edit_modal.hide();
        process_lvgl(20);
    }
}
