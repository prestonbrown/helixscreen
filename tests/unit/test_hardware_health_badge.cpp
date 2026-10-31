// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_health_badge.cpp
 * @brief The headline badge bindings in ui_xml/hardware_health_overlay.xml
 *
 * The overlay stacks three badges in one slot and hides all but one by
 * binding each to a single value of hardware_status_level. Nothing about a
 * wrong ref_value fails loudly at runtime: the overlay simply shows the
 * wrong colour and glyph, or two badges at once.
 *
 * @see include/hardware_validator.h - HardwareStatusLevel
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::UpdateQueue;

namespace {

/// Every badge in the headline slot, in XML order.
constexpr const char* kBadgeNames[] = {"status_icon_container", "status_icon_container_warn",
                                       "status_icon_container_crit"};

/// Builds the real overlay from production XML.
///
/// LVGLUITestFixture registers hardware_status_level and the status text
/// subjects this tree binds before it registers XML components, so the overlay
/// can be built straight from the constructor.
struct HardwareHealthBadgeFixture : public LVGLUITestFixture {
    HardwareHealthBadgeFixture() {
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "hardware_health_overlay", nullptr));
    }

    ~HardwareHealthBadgeFixture() override {
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
    }

    /// Drive the badge selector, then let the observers land.
    void set_level(int level) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, "hardware_status_level");
        REQUIRE(subject != nullptr);
        lv_subject_set_int(subject, level);
        process_lvgl(10);
    }

    bool badge_hidden(const char* name) const {
        lv_obj_t* badge = lv_obj_find_by_name(root_, name);
        REQUIRE(badge != nullptr);
        return lv_obj_has_flag(badge, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* root_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(HardwareHealthBadgeFixture,
                 "hardware_health_overlay: each status level shows exactly one headline badge",
                 "[hardware][hardware-validation][ui]") {
    REQUIRE(root_ != nullptr);

    struct Expectation {
        int level;
        const char* visible;
    };
    const Expectation cases[] = {
        {static_cast<int>(HardwareStatusLevel::OK), "status_icon_container"},
        {static_cast<int>(HardwareStatusLevel::ATTENTION), "status_icon_container_warn"},
        {static_cast<int>(HardwareStatusLevel::CRITICAL), "status_icon_container_crit"},
    };

    for (const auto& expected : cases) {
        INFO("hardware_status_level=" << expected.level);
        set_level(expected.level);

        for (const char* name : kBadgeNames) {
            const bool should_be_visible = std::string(name) == expected.visible;
            INFO("badge=" << name);
            REQUIRE(badge_hidden(name) == !should_be_visible);
        }
    }
}

TEST_CASE_METHOD(HardwareHealthBadgeFixture,
                 "hardware_health_overlay: newly discovered hardware alerts in the headline",
                 "[hardware][hardware-validation][ui]") {
    REQUIRE(root_ != nullptr);

    HardwareValidationResult result;
    result.newly_discovered.push_back(HardwareIssue::info(
        "filament_switch_sensor runout", HardwareType::FILAMENT_SENSOR, "Detected"));
    get_printer_state().hardware_validation_state().set_hardware_validation_result(result);
    process_lvgl(10);

    REQUIRE(badge_hidden("status_icon_container"));
    REQUIRE_FALSE(badge_hidden("status_icon_container_warn"));
    REQUIRE(badge_hidden("status_icon_container_crit"));
}
