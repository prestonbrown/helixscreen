// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_load_reentrancy.cpp
 * @brief A wizard screen load whose drain runs another load, or the wizard's
 *        completion, leaves the screen to that work.
 *
 * Loading a step drains the update queue after cleaning the previous step, so
 * queued work can change step or finish the wizard under the load. The load
 * that ran last owns the content container; the superseded one must neither
 * clean it nor build its own step into it.
 */

#include "ui_wizard.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "config.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "src/ui/wizard/wizard_step_registry.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"

#include <algorithm>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::wizard::StepId;

namespace {

class WizardLoadFixture : public LVGLUITestFixture {
  public:
    WizardLoadFixture() {
        // The WiFi step asks for a WiFi manager; test mode keeps it a mock.
        scoped_config_ = std::make_unique<ScopedRuntimeConfig>();
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        wizard_ = ui_wizard_create(test_screen());
        REQUIRE(wizard_ != nullptr);
        content_ = lv_obj_find_by_name(wizard_, "wizard_content");
        REQUIRE(content_ != nullptr);
    }

    int current_step() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "current_step");
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    void navigate(StepId step) {
        ui_wizard_navigate_to_step(step);
        process_lvgl(80);
    }

    lv_obj_t* wizard_ = nullptr;
    lv_obj_t* content_ = nullptr;
    std::unique_ptr<ScopedRuntimeConfig> scoped_config_;
};

} // namespace

TEST_CASE_METHOD(WizardLoadFixture, "wizard: a step change drained by a screen load wins",
                 "[wizard][drain_reentrancy]") {
    navigate(StepId::Wifi);
    const uint32_t wifi_children = lv_obj_get_child_count(content_);
    REQUIRE(wifi_children > 0);
    navigate(StepId::Connection);
    REQUIRE(current_step() == static_cast<int>(StepId::Connection));

    helix::ui::UpdateQueue::instance().queue("test::queued_step_change",
                                             []() { ui_wizard_navigate_to_step(StepId::Wifi); });
    ui_wizard_navigate_to_step(StepId::Connection);
    process_lvgl(80);

    CHECK(current_step() == static_cast<int>(StepId::Wifi));
    CHECK(lv_obj_get_child_count(content_) == wifi_children);
    // The screen on show is the one the last load built.
    CHECK(lv_obj_find_by_name(content_, "wizard_wifi_setup_root") != nullptr);
    CHECK(lv_obj_find_by_name(content_, "wizard_connection_root") == nullptr);
}

TEST_CASE_METHOD(WizardLoadFixture,
                 "wizard: a completion drained by a screen load leaves the content alone",
                 "[wizard][drain_reentrancy]") {
    auto* cfg = helix::Config::get_instance();
    const bool was_completed = cfg->get<bool>("/wizard_completed", false);
    navigate(StepId::Wifi);
    const uint32_t wifi_children = lv_obj_get_child_count(content_);
    REQUIRE(wifi_children > 0);
    lv_obj_t* wifi_screen = lv_obj_get_child(content_, 0);
    int wifi_screen_deleted = 0;
    lv_obj_add_event_cb(
        wifi_screen, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, &wifi_screen_deleted);

    helix::ui::UpdateQueue::instance().queue("test::queued_completion",
                                             []() { ui_wizard_complete(); });
    ui_wizard_navigate_to_step(StepId::Connection);

    // The completion condemned the container, which the async tick frees with
    // everything in it; the superseded load neither cleaned it nor built into it.
    CHECK(wifi_screen_deleted == 0);
    CHECK(lv_obj_get_child_count(content_) == wifi_children);
    process_lvgl(80);
    CHECK(wifi_screen_deleted == 1);

    cfg->set<bool>("/wizard_completed", was_completed);
    cfg->set<bool>(cfg->df() + "wizard_completed", was_completed);
}

TEST_CASE_METHOD(WizardLoadFixture,
                 "wizard: a completion and a new wizard in one drain leave the old content alone",
                 "[wizard][drain_reentrancy]") {
    auto* cfg = helix::Config::get_instance();
    const bool was_completed = cfg->get<bool>("/wizard_completed", false);
    navigate(StepId::Wifi);
    const uint32_t wifi_children = lv_obj_get_child_count(content_);
    REQUIRE(wifi_children > 0);
    lv_obj_t* wifi_screen = lv_obj_get_child(content_, 0);
    int wifi_screen_deleted = 0;
    lv_obj_add_event_cb(
        wifi_screen, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, &wifi_screen_deleted);

    lv_obj_t* fresh = nullptr;
    helix::ui::UpdateQueue::instance().queue("test::complete_then_create", [this, &fresh]() {
        ui_wizard_complete();
        fresh = ui_wizard_create(test_screen());
    });
    ui_wizard_navigate_to_step(StepId::Connection);

    REQUIRE(fresh != nullptr);
    CHECK(wifi_screen_deleted == 0);
    CHECK(lv_obj_get_child_count(content_) == wifi_children);
    lv_obj_t* fresh_content = lv_obj_find_by_name(fresh, "wizard_content");
    REQUIRE(fresh_content != nullptr);
    CHECK(lv_obj_get_child_count(fresh_content) == 0);
    process_lvgl(80);

    cfg->set<bool>("/wizard_completed", was_completed);
    cfg->set<bool>(cfg->df() + "wizard_completed", was_completed);
}

// Leaving PrinterIdentify decides whether the requested step is now skipped.
// A load run by that load's drain takes over the decision for its own target.
TEST_CASE_METHOD(WizardLoadFixture,
                 "wizard: a step change drained after leaving printer identify keeps the redirect",
                 "[wizard][drain_reentrancy]") {
    auto* cfg = helix::Config::get_instance();
    const std::string preset_key = cfg->df() + "preset";
    const auto prior_preset = cfg->get<std::string>(preset_key, "");
    navigate(StepId::PrinterIdentify);
    REQUIRE(current_step() == static_cast<int>(StepId::PrinterIdentify));

    // A preset collapses the hardware pickers for this run.
    cfg->set<std::string>(preset_key, "drain_reentrancy_preset");
    helix::wizard_mark_preset_applied_this_session();
    const auto skips = helix::wizard::skip_vector(helix::wizard::build_context());
    const auto fan = std::find_if(skips.begin(), skips.end(), [](const helix::StepSkip& s) {
        return s.id == StepId::FanSelect;
    });
    REQUIRE(fan != skips.end());
    REQUIRE(fan->skipped);
    const auto expected = helix::wizard_next(StepId::FanSelect, skips);
    REQUIRE(expected.has_value());

    helix::ui::UpdateQueue::instance().queue(
        "test::queued_step_change", []() { ui_wizard_navigate_to_step(StepId::FanSelect); });
    ui_wizard_navigate_to_step(StepId::HeaterSelect);
    process_lvgl(80);

    CHECK(current_step() == static_cast<int>(*expected));

    cfg->set<std::string>(preset_key, prior_preset);
    helix::wizard_reset_preset_session_state();
}

// A load superseded by a completion leaves the step it cleaned behind for the
// load its drain runs. A wizard created by that drain starts from no step, so
// its first load decides no PrinterIdentify redirect.
TEST_CASE_METHOD(WizardLoadFixture,
                 "wizard: a wizard created in a load's drain starts with no step left behind",
                 "[wizard][drain_reentrancy]") {
    auto* cfg = helix::Config::get_instance();
    const bool was_completed = cfg->get<bool>("/wizard_completed", false);
    const std::string preset_key = cfg->df() + "preset";
    const auto prior_preset = cfg->get<std::string>(preset_key, "");
    navigate(StepId::PrinterIdentify);
    REQUIRE(current_step() == static_cast<int>(StepId::PrinterIdentify));

    // A preset skips FanSelect, which a load leaving PrinterIdentify would
    // redirect past.
    cfg->set<std::string>(preset_key, "drain_reentrancy_preset");
    helix::wizard_mark_preset_applied_this_session();
    const auto skips = helix::wizard::skip_vector(helix::wizard::build_context());
    const auto fan = std::find_if(skips.begin(), skips.end(), [](const helix::StepSkip& s) {
        return s.id == StepId::FanSelect;
    });
    REQUIRE(fan != skips.end());
    REQUIRE(fan->skipped);

    lv_obj_t* fresh = nullptr;
    helix::ui::UpdateQueue::instance().queue("test::complete_then_create", [this, &fresh]() {
        ui_wizard_complete();
        fresh = ui_wizard_create(test_screen());
    });
    ui_wizard_navigate_to_step(StepId::HeaterSelect);
    REQUIRE(fresh != nullptr);

    ui_wizard_navigate_to_step(StepId::FanSelect);
    process_lvgl(80);
    CHECK(current_step() == static_cast<int>(StepId::FanSelect));

    cfg->set<std::string>(preset_key, prior_preset);
    helix::wizard_reset_preset_session_state();
    cfg->set<bool>("/wizard_completed", was_completed);
    cfg->set<bool>(cfg->df() + "wizard_completed", was_completed);
}
