// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_macro_enhance_wizard_close.cpp
 * @brief The wizard's completion callback may destroy the wizard (#1762)
 *
 * MacroModificationManager frees its wizard from inside the completion
 * callback, so the wizard must be fully hidden before that callback runs and
 * must not touch itself afterwards.
 */

#include "ui_macro_enhance_wizard.h"

#include "../lvgl_ui_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::ui::MacroEnhanceWizard;

namespace {

helix::PrintStartAnalysis one_uncontrollable_op() {
    helix::PrintStartAnalysis analysis;
    analysis.found = true;
    analysis.macro_name = "PRINT_START";
    helix::PrintStartOperation op;
    op.name = "PURGE_LINE";
    op.category = helix::PrintStartOpCategory::PURGE_LINE;
    op.has_skip_param = false;
    analysis.operations.push_back(op);
    return analysis;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "macro enhance wizard is hidden before a completion callback that frees it",
                 "[macro_enhance][modal][1762]") {
    lv_xml_register_component_from_file("A:ui_xml/macro_enhance_modal.xml");

    const char* button = GENERATE("btn_close", "btn_cancel");
    CAPTURE(button);

    auto wizard = std::make_unique<MacroEnhanceWizard>();
    wizard->set_api(api());
    wizard->set_analysis(one_uncontrollable_op());

    int calls = 0;
    bool visible_at_complete = true;
    wizard->set_complete_callback([&](bool, size_t) {
        ++calls;
        visible_at_complete = wizard->is_visible();
        // What MacroModificationManager::on_wizard_complete does.
        wizard.reset();
    });

    REQUIRE(wizard->show(lv_screen_active()));
    process_lvgl(20);
    REQUIRE(wizard->is_visible());

    lv_obj_t* btn = lv_obj_find_by_name(wizard->dialog(), button);
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    process_lvgl(400);

    CHECK(calls == 1);
    CHECK_FALSE(visible_at_complete);
    CHECK(wizard == nullptr);
    CHECK_FALSE(Modal::any_visible());
}
