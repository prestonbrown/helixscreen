// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_setting_dropdown_row_bind_selected.cpp
 * @brief setting_dropdown_row must open on the persisted option, not option 0.
 *
 * The component wraps an lv_dropdown, and the engine implements bind_selected on
 * that inner widget. A binding attribute written on the component instance
 * therefore reaches the dropdown only if setting_dropdown_row declares it in
 * <api> and forwards it, the way setting_action_row forwards bind_description to
 * its label's bind_text. An attribute the <api> does not declare is dropped at
 * the component boundary with no parse error, so the row still builds and the
 * dropdown still reads index 0 — indistinguishable from a working binding unless
 * the selection itself is asserted.
 *
 * None of the rows here have a C++ seeding fallback, so the binding is the only
 * thing putting the saved value on screen. row_completion_alert is the deliberate
 * counter-example: it carries no binding and SafetySettingsOverlay::on_activate()
 * seeds it by hand from AudioSettingsManager.
 *
 * bind_selected is one-way on purpose. The rows' value_changed callbacks already
 * write their subject through the settings managers, and AboutSettingsOverlay's
 * channel handler vetoes an unconfigured Dev channel by restoring the previous
 * value it reads back from that subject — which only works while nothing else
 * writes the subject ahead of it.
 */

#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "ui_settings_safety.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// Reads back the selection of the lv_dropdown inside a setting_dropdown_row.
uint32_t row_selection(lv_obj_t* root, const char* row_name) {
    REQUIRE(root != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(root, row_name);
    REQUIRE(row != nullptr);
    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dropdown != nullptr);
    return lv_dropdown_get_selected(dropdown);
}

lv_subject_t* require_subject(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return s;
}

/// Builds the real Safety overlay from production XML.
///
/// SafetySettingsManager owns every settings_* subject the tree binds and
/// SettingsManager owns filament_auto_cooldown; both must be registered before
/// lv_xml_create() or the bindings resolve to nothing and the tree under
/// assertion is not the production one. Neither is torn down here: both
/// self-register their deinit with StaticSubjectRegistry, and other cases in this
/// binary share the same process-wide registration. Only the values this fixture
/// writes are restored.
struct SafetyDropdownFixture : public LVGLUITestFixture {
    SafetyDropdownFixture() {
        SettingsManager::instance().init_subjects();
        SafetySettingsManager::instance().init_subjects();
        helix::settings::get_safety_settings_overlay().init_subjects();

        timeout_ = require_subject("settings_cancel_escalation_timeout");
        severity_ = require_subject("settings_min_toast_severity");
        saved_timeout_ = lv_subject_get_int(timeout_);
        saved_severity_ = lv_subject_get_int(severity_);
    }

    ~SafetyDropdownFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        lv_subject_set_int(timeout_, saved_timeout_);
        lv_subject_set_int(severity_, saved_severity_);
        UpdateQueue::instance().drain();
    }

    /// Creation is deliberately not in the constructor: the binding seeds the
    /// widget when the tree is built, so each case sets its subject first.
    void build() {
        root_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "settings_safety_overlay",
                                                    nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(10);
    }

    lv_obj_t* root_ = nullptr;
    lv_subject_t* timeout_ = nullptr;
    lv_subject_t* severity_ = nullptr;
    int saved_timeout_ = 0;
    int saved_severity_ = 0;
};

} // namespace

TEST_CASE_METHOD(SafetyDropdownFixture, "Safety dropdown rows show the persisted selection",
                 "[settings][safety][dropdown][binding]") {
    SECTION("escalation timeout opens on the saved index") {
        // 2 = "60 seconds". Index 0 is what an unbound dropdown reports, so the
        // value under test must not be 0.
        lv_subject_set_int(timeout_, 2);
        build();
        CHECK(row_selection(root_, "row_cancel_escalation_timeout") == 2u);
    }

    SECTION("minimum toast severity opens on the saved index") {
        // 2 = "Errors only".
        lv_subject_set_int(severity_, 2);
        build();
        CHECK(row_selection(root_, "row_min_toast_severity") == 2u);
    }

    SECTION("both rows bind independently") {
        lv_subject_set_int(timeout_, 3);
        lv_subject_set_int(severity_, 1);
        build();
        CHECK(row_selection(root_, "row_cancel_escalation_timeout") == 3u);
        CHECK(row_selection(root_, "row_min_toast_severity") == 1u);
    }

    SECTION("the binding stays live after the tree is built") {
        // A one-shot seed would pass the cases above and fail this one: the
        // manager's setters write the subject, so anything changing a setting
        // from elsewhere has to move the row.
        lv_subject_set_int(timeout_, 0);
        build();
        REQUIRE(row_selection(root_, "row_cancel_escalation_timeout") == 0u);

        lv_subject_set_int(timeout_, 3);
        process_lvgl(10);
        CHECK(row_selection(root_, "row_cancel_escalation_timeout") == 3u);
    }

    SECTION("a subject past the end of the list selects the last option") {
        // lv_dropdown_set_selected() supplies this clamp; the case pins it as
        // part of the binding's contract so a future hand-rolled index
        // calculation cannot quietly index past a four-option row.
        lv_subject_set_int(timeout_, 99);
        build();
        CHECK(row_selection(root_, "row_cancel_escalation_timeout") == 3u);
    }

    SECTION("a negative subject leaves the row alone") {
        // -1 is the usual spelling of "unset". Widening it would wrap to a huge
        // index and clamp to the LAST option, which reads as a real choice the
        // user never made, so the binding declines to move the widget at all.
        lv_subject_set_int(timeout_, 2);
        build();
        REQUIRE(row_selection(root_, "row_cancel_escalation_timeout") == 2u);

        lv_subject_set_int(timeout_, -1);
        process_lvgl(10);
        CHECK(row_selection(root_, "row_cancel_escalation_timeout") == 2u);
    }

    SECTION("the hand-seeded completion alert row is untouched by the binding") {
        // row_completion_alert has no subject; on_activate() seeds it. Building
        // the tree alone must leave it at 0 rather than picking up a neighbour's
        // value, which is what a subject name leaking across rows would look like.
        lv_subject_set_int(timeout_, 3);
        lv_subject_set_int(severity_, 2);
        build();
        CHECK(row_selection(root_, "row_completion_alert") == 0u);
    }
}
