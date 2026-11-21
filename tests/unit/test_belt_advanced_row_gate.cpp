// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_advanced_row_gate.cpp
 * @brief The Advanced panel's rows follow printer support, not beta features
 *        (prestonbrown/helixscreen#1736).
 *
 * Builds the real advanced_panel.xml with beta features off and drives the
 * subjects each row's visibility binding reads. G-code Console needs
 * platform_console_supported. The calibration steps' conditions live in the
 * calibration catalog (test_calibration_catalog.cpp).
 */

#include "ui_status_pill.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/xml_stand_in_subject.h"
#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "setting_group.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

namespace {

/// Builds the real advanced_panel.xml so its rows' visibility bindings can
/// be exercised against a live tree, in the same shape
/// production's xml_registration.xml uses (see AdvancedPowerGroupFixture in
/// test_host_power_availability.cpp for the sibling POWER-group case).
class AdvancedBeltRowFixture : public XMLTestFixture {
  public:
    AdvancedBeltRowFixture() : XMLTestFixture() {
        setting_group_register();
        ui_status_pill_register_widget();
        REQUIRE(register_component("setting_group_header"));
        REQUIRE(register_component("setting_action_row"));
        REQUIRE(register_component("advanced_panel"));

        // The XML subject registry is process-global and never forgets an
        // entry, so every subject below is a function-local STATIC: a fixture
        // member would dangle after this test and any later consumer of the
        // name dereferences freed memory.
        helix::test::xml_stand_in("show_beta_features", 0);
        helix::test::xml_stand_in("printer_has_probe", 1);
        helix::test::xml_stand_in("platform_console_supported", 1);
        beta_ = subject("show_beta_features");
        beta_before_ = lv_subject_get_int(beta_);
        lv_subject_set_int(beta_, 0);

        panel_ = create_component("advanced_panel");
        REQUIRE(panel_ != nullptr);
        process_lvgl(50);
    }

    ~AdvancedBeltRowFixture() override {
        lv_subject_set_int(beta_, beta_before_);
    }

    /// Hidden itself or under a hidden ancestor, up to the panel root.
    bool hidden_in_panel(lv_obj_t* obj) const {
        for (; obj && obj != panel_; obj = lv_obj_get_parent(obj)) {
            if (lv_obj_is_hidden(obj)) {
                return true;
            }
        }
        return false;
    }

    lv_obj_t* row(const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(panel_, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    lv_subject_t* subject(const char* name) {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
        REQUIRE(subj != nullptr);
        return subj;
    }

    lv_obj_t* panel_ = nullptr;
    lv_subject_t* beta_ = nullptr;
    int beta_before_ = 0;
};

} // namespace

TEST_CASE_METHOD(AdvancedBeltRowFixture, "Advanced G-code Console row needs a console build",
                 "[console][advanced][xml]") {
    auto* supported = subject("platform_console_supported");
    const int before = lv_subject_get_int(supported);
    for (int has_console : {1, 0}) {
        lv_subject_set_int(supported, has_console);
        process_lvgl(10);
        CHECK(hidden_in_panel(row("row_console")) == (has_console == 0));
    }
    lv_subject_set_int(supported, before);
}
