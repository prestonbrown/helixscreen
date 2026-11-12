// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"
#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "../helix_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "first_run_tour.h"
#include "printer_image_manager.h"
#include "tour_overlay.h"
#include "tour_steps.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Config;
using helix::tour::build_tour_steps;
using helix::tour::FirstRunTour;
using helix::tour::TourStep;

namespace {
void reset_tour_settings() {
    auto* cfg = Config::get_instance();
    cfg->set<bool>("/tour/completed", false);
    cfg->set<int>("/tour/last_seen_version", 0);
    // is_wizard_required() checks per-printer first, then falls back to root-level.
    // In the singleton-without-init test environment, active_printer_id_ is empty,
    // so the root-level key is the one that's read.
    cfg->set<bool>("/wizard_completed", true);
    // Edit mode gates the tour, and the subject is global, so a case that leaves
    // it set would decide every case that runs after it.
    lv_subject_set_int(&get_home_edit_mode_subject(), 0);
    FirstRunTour::instance().set_printer_ready(true);
    // In-memory singleton state is left clean by the public API used in these
    // tests: start()/skip() always pair, and advance() past the last step calls
    // finish() which resets running_. No explicit reset needed.
}
} // namespace

TEST_CASE("FirstRunTour gate: blocks when tour already completed", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/last_seen_version", helix::tour::kTourVersion);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: blocks when wizard not complete", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/wizard_completed", false);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: allows auto-start when fresh and wizards done", "[tour]") {
    reset_tour_settings();
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE("FirstRunTour gate: re-triggers when last_seen_version is behind TOUR_VERSION",
          "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/last_seen_version", 0);
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

// The edit-mode subject is owned by app_globals_init_subjects(), which the UI
// fixture runs; without it the subject has no value to set.
TEST_CASE_METHOD(LVGLUITestFixture, "FirstRunTour gate: blocks while home grid edit mode is active",
                 "[tour]") {
    reset_tour_settings();
    // Pin that the gate is otherwise open, so a gate stuck at false cannot pass.
    REQUIRE(FirstRunTour::should_auto_start() == true);

    lv_subject_set_int(&get_home_edit_mode_subject(), 1);
    REQUIRE(FirstRunTour::should_auto_start() == false);

    lv_subject_set_int(&get_home_edit_mode_subject(), 0);
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE_METHOD(LVGLUITestFixture, "FirstRunTour: wizard completion re-evaluates the gate",
                 "[tour][wizard]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/wizard_completed", false);

    // Home is already the active panel while the wizard owns the screen, so the
    // deferred set_active(PanelId::Home) returns without activating anything.
    // on_activate() is the tour's other entry point and does not run here.
    REQUIRE(NavigationManager::instance().get_active() == helix::PanelId::Home);
    REQUIRE(FirstRunTour::should_auto_start() == false);

    auto& tour = FirstRunTour::instance();
    REQUIRE(tour.is_running() == false);

    ui_wizard_complete();
    process_lvgl(200); // past the 100ms nav timer, and drains the async start

    REQUIRE(tour.is_running() == true);
    tour.skip();
}

TEST_CASE_METHOD(LVGLUITestFixture, "FirstRunTour: a modal shown before the deferred start wins",
                 "[tour]") {
    reset_tour_settings();
    auto& tour = FirstRunTour::instance();
    REQUIRE_FALSE(tour.is_running());
    REQUIRE(FirstRunTour::should_auto_start());

    // The gate passes, then a modal comes up before the queued start runs.
    tour.maybe_start();
    lv_obj_t* backdrop = lv_obj_create(lv_screen_active());
    lv_obj_t* dialog = lv_obj_create(backdrop);
    ModalStack::instance().push(backdrop, dialog, "plr_offer_stand_in");
    CHECK_FALSE(FirstRunTour::should_auto_start());
    process_lvgl(50);
    CHECK_FALSE(tour.is_running());

    ModalStack::instance().remove(backdrop, true);
    lv_obj_delete(backdrop);
    CHECK(FirstRunTour::should_auto_start());
    tour.maybe_start();
    process_lvgl(50);
    CHECK(tour.is_running());
    tour.skip();
}

TEST_CASE("FirstRunTour mark_completed writes both flags", "[tour]") {
    reset_tour_settings();
    FirstRunTour::mark_completed();
    auto* cfg = Config::get_instance();
    REQUIRE(cfg->get<bool>("/tour/completed", false) == true);
    REQUIRE(cfg->get<int>("/tour/last_seen_version", 0) == helix::tour::kTourVersion);
}

namespace {
std::vector<std::string> titles(const std::vector<TourStep>& steps) {
    std::vector<std::string> out;
    for (const auto& s : steps)
        out.push_back(s.title_key);
    return out;
}
bool has_title(const std::vector<TourStep>& steps, const std::string& title) {
    const auto t = titles(steps);
    return std::find(t.begin(), t.end(), title) != t.end();
}
const TourStep& step_titled(const std::vector<TourStep>& steps, const std::string& title) {
    auto it = std::find_if(steps.begin(), steps.end(),
                           [&](const TourStep& s) { return s.title_key == title; });
    REQUIRE(it != steps.end());
    return *it;
}
} // namespace

TEST_CASE("Tour steps: the filament system step exists only with a filament system", "[tour]") {
    auto with_ams = build_tour_steps(true);
    auto without = build_tour_steps(false);
    REQUIRE(with_ams.size() == without.size() + 1);
    CHECK(has_title(with_ams, "Your filament system"));
    CHECK_FALSE(has_title(without, "Your filament system"));
}

TEST_CASE("Tour steps: the at-a-glance step targets AMS when present, nozzle otherwise", "[tour]") {
    CHECK(step_titled(build_tour_steps(true), "Your printer at a glance").target_name == "ams");
    CHECK(step_titled(build_tour_steps(false), "Your printer at a glance").target_name ==
          "nozzle_temps");
}

TEST_CASE("Tour steps: the full tour opens with welcome and ends on the navbar", "[tour]") {
    auto steps = build_tour_steps(true);
    CHECK(steps.front().title_key == "Welcome to HelixScreen");
    CHECK(steps.front().target_name.empty());
    CHECK(steps.back().target_name == "nav_btn_settings");
    CHECK(step_titled(steps, "Customize your home screen").target_name == "fan_stack");
    CHECK(step_titled(steps, "Customize your home screen").fallback_target_name == "printer_image");
    CHECK(step_titled(steps, "Your printer at a glance").fallback_target_name == "print_status");
    CHECK_FALSE(has_title(steps, "New in 1.1"));
}

TEST_CASE("Tour steps: panel steps name the screen that holds their target", "[tour]") {
    auto steps = build_tour_steps(true);
    CHECK(step_titled(steps, "Load and unload").panel == helix::tour::TourPanel::Filament);
    CHECK(step_titled(steps, "Load and unload").anchor_hint ==
          helix::tour::TooltipAnchor::LeftNarrowed);
    const auto& ams = step_titled(steps, "Your filament system");
    CHECK(ams.panel == helix::tour::TourPanel::AmsOverlay);
    CHECK(ams.target_name == "ams_unit_card");
    CHECK(ams.fallback_target_name == "unit_cards_row");
    CHECK(step_titled(steps, "Controls").panel == helix::tour::TourPanel::Home);
}

TEST_CASE("Tour steps: the filament system step directly follows the Filament panel step",
          "[tour]") {
    auto t = titles(build_tour_steps(true));
    auto filament = std::find(t.begin(), t.end(), "Load and unload");
    REQUIRE(filament != t.end());
    REQUIRE(filament + 1 != t.end());
    CHECK(*(filament + 1) == "Your filament system");
}

TEST_CASE("Tour steps: Print Status is shown as a picture", "[tour]") {
    const auto steps = build_tour_steps(false);
    const auto& s = step_titled(steps, "A new Print Status screen");
    CHECK(s.target_name.empty());
    CHECK(s.shows_print_status_preview);
}

TEST_CASE("Tour steps: a 1.0 upgrader sees only what is new", "[tour]") {
    using V = std::vector<std::string>;
    CHECK(titles(build_tour_steps(true, 1)) == V{"New in 1.1", "Customize your home screen",
                                                 "A new Print Status screen", "Load and unload",
                                                 "Your filament system"});
    CHECK(titles(build_tour_steps(false, 1)) == V{"New in 1.1", "Customize your home screen",
                                                  "A new Print Status screen", "Load and unload"});
}

TEST_CASE("Tour steps: nothing is new to someone on the current version", "[tour]") {
    CHECK(build_tour_steps(true, helix::tour::kTourVersion).empty());
}

TEST_CASE("auto_start_seen_version: only a finished tour counts as seen", "[tour]") {
    CHECK(helix::tour::auto_start_seen_version(false, 1) == 0);
    CHECK(helix::tour::auto_start_seen_version(true, 0) == 0);
    CHECK(helix::tour::auto_start_seen_version(true, 1) == 1);
}

TEST_CASE("FirstRunTour gate: waits until the printer has been discovered", "[tour]") {
    reset_tour_settings();
    FirstRunTour::instance().set_printer_ready(false);
    CHECK_FALSE(FirstRunTour::should_auto_start());
    FirstRunTour::instance().set_printer_ready(true);
    CHECK(FirstRunTour::should_auto_start());
}

TEST_CASE("FirstRunTour gate: a 1.0 upgrader is offered the tour", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/last_seen_version", 1);
    REQUIRE(helix::tour::kTourVersion == 2);
    CHECK(FirstRunTour::should_auto_start());
}

TEST_CASE_METHOD(LVGLUITestFixture, "FirstRunTour: navigating elsewhere cancels and stays there",
                 "[tour]") {
    reset_tour_settings();
    auto& nav = NavigationManager::instance();
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    nav.set_panels(panels.data());
    nav.set_active(helix::PanelId::Home);

    auto& t = FirstRunTour::instance();
    t.start();
    REQUIRE(t.is_running());

    nav.set_active(helix::PanelId::Controls);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    CHECK_FALSE(t.is_running());
    CHECK(nav.get_active() == helix::PanelId::Controls);
    CHECK(Config::get_instance()->get<bool>("/tour/completed", false));
    nav.shutdown();
}

TEST_CASE_METHOD(LVGLUITestFixture, "FirstRunTour: steps open their panel, skip returns Home",
                 "[tour]") {
    reset_tour_settings();
    auto& nav = NavigationManager::instance();
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    nav.set_panels(panels.data());
    nav.set_active(helix::PanelId::Home);

    auto& t = FirstRunTour::instance();
    t.start();
    const auto t_list = titles(build_tour_steps(helix::tour::hardware_has_ams()));
    const auto index = std::find(t_list.begin(), t_list.end(), "Load and unload") - t_list.begin();
    for (auto i = 0; i < index; ++i) {
        REQUIRE(t.is_running());
        CHECK(nav.get_active() == helix::PanelId::Home);
        t.advance();
    }
    REQUIRE(t.is_running());
    CHECK(nav.get_active() == helix::PanelId::Filament);

    t.skip();
    CHECK(nav.get_active() == helix::PanelId::Home);
    CHECK_FALSE(t.is_running());
    nav.shutdown();
}

TEST_CASE_METHOD(HelixTestFixture, "FirstRunTour: start() sets is_running", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    REQUIRE(!t.is_running());
    t.start();
    REQUIRE(t.is_running());
    t.skip();
    REQUIRE(!t.is_running());
}

TEST_CASE_METHOD(HelixTestFixture, "FirstRunTour: skip writes tour.completed", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    t.start();
    t.skip();
    REQUIRE(Config::get_instance()->get<bool>("/tour/completed", false) == true);
}

TEST_CASE_METHOD(HelixTestFixture, "FirstRunTour: advance past last step finishes", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    t.start();
    const auto count = build_tour_steps(helix::tour::hardware_has_ams()).size();
    for (std::size_t i = 0; i < count; ++i)
        t.advance();
    REQUIRE(!t.is_running());
    REQUIRE(Config::get_instance()->get<bool>("/tour/completed", false) == true);
}

namespace {
helix::tour::TourStep plain_step(const char* target) {
    helix::tour::TourStep step;
    step.target_name = target;
    step.title_key = "Title";
    step.body_key = "Body";
    return step;
}

lv_obj_t* named_box(const char* name, int x, int width) {
    lv_obj_t* box = lv_obj_create(lv_screen_active());
    lv_obj_set_name(box, name);
    lv_obj_set_pos(box, x, 100);
    lv_obj_set_size(box, width, 80);
    lv_obj_update_layout(box);
    return box;
}

// TourOverlay builds its root as dim, highlight, tooltip, in that order.
lv_obj_t* highlight_of(helix::tour::TourOverlay& overlay) {
    return lv_obj_get_child(overlay.root(), 1);
}
lv_obj_t* tooltip_of(helix::tour::TourOverlay& overlay) {
    return lv_obj_get_child(overlay.root(), 2);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "TourOverlay: a missing target falls back to the second name",
                 "[tour]") {
    named_box("fallback_target", 40, 200);
    auto with_fallback = plain_step("absent_target");
    with_fallback.fallback_target_name = "fallback_target";
    helix::tour::TourOverlay overlay({with_fallback, plain_step("absent_target")}, {}, {});

    overlay.show_step(0);
    CHECK_FALSE(lv_obj_has_flag(highlight_of(overlay), LV_OBJ_FLAG_HIDDEN));
    overlay.show_step(1);
    CHECK(lv_obj_has_flag(highlight_of(overlay), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "TourOverlay: LeftNarrowed fits the card beside its target",
                 "[tour]") {
    REQUIRE(lv_display_get_horizontal_resolution(nullptr) >= 800);
    named_box("wide_target", 340, 400);
    named_box("left_edge_target", 40, 400);
    auto narrowed = plain_step("wide_target");
    narrowed.anchor_hint = helix::tour::TooltipAnchor::LeftNarrowed;
    auto no_room = plain_step("left_edge_target");
    no_room.anchor_hint = helix::tour::TooltipAnchor::LeftNarrowed;
    auto centered = plain_step("");
    centered.anchor_hint = helix::tour::TooltipAnchor::Center;
    helix::tour::TourOverlay overlay({centered, narrowed, no_room}, {}, {});
    lv_obj_t* card = tooltip_of(overlay);

    overlay.show_step(0);
    lv_obj_update_layout(card);
    const int full_width = lv_obj_get_width(card);
    REQUIRE(full_width > 340);

    overlay.show_step(1);
    lv_obj_update_layout(card);
    CHECK(lv_obj_get_width(card) < 340);
    CHECK(lv_obj_get_x(card) + lv_obj_get_width(card) <= 340);

    overlay.show_step(2);
    lv_obj_update_layout(card);
    CHECK(lv_obj_get_width(card) == full_width);

    overlay.show_step(0);
    lv_obj_update_layout(card);
    CHECK(lv_obj_get_width(card) == full_width);
}

TEST_CASE_METHOD(LVGLUITestFixture, "TourOverlay: the preview belongs only to its own step",
                 "[tour]") {
    REQUIRE(lv_display_get_vertical_resolution(nullptr) >= 400);
    auto with_picture = plain_step("");
    with_picture.shows_print_status_preview = true;
    helix::tour::TourOverlay overlay({with_picture, plain_step("")}, {}, {});
    lv_obj_t* preview = lv_obj_find_by_name(tooltip_of(overlay), "tour_preview");
    REQUIRE(preview != nullptr);

    overlay.show_step(0);
    CHECK_FALSE(lv_obj_has_flag(preview, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* printer = lv_obj_find_by_name(preview, "tour_preview_printer");
    REQUIRE(printer != nullptr);
    const std::string expected = helix::PrinterImageManager::instance().get_displayed_image_path(
        lv_display_get_horizontal_resolution(nullptr));
    REQUIRE_FALSE(expected.empty());
    CHECK(static_cast<const char*>(lv_image_get_src(printer)) == expected);
    overlay.show_step(1);
    CHECK(lv_obj_has_flag(preview, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "TourOverlay: hide_card lasts until the next step", "[tour]") {
    helix::tour::TourOverlay overlay({plain_step("")}, {}, {});
    overlay.show_step(0);
    overlay.hide_card();
    CHECK(lv_obj_has_flag(tooltip_of(overlay), LV_OBJ_FLAG_HIDDEN));
    overlay.show_step(0);
    CHECK_FALSE(lv_obj_has_flag(tooltip_of(overlay), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(HelixTestFixture, "FirstRunTour: a running tour holds back the plugin prompt",
                 "[tour]") {
    reset_tour_settings();
    CHECK_FALSE(PrintSelectPanel::plugin_screen_state().modal_up);
    auto& t = FirstRunTour::instance();
    t.start();
    REQUIRE(t.is_running());
    CHECK(PrintSelectPanel::plugin_screen_state().modal_up);
    t.skip();
}

TEST_CASE("FirstRunTour: the Filament step target exists in both layouts", "[tour]") {
    std::ifstream file("ui_xml/filament_panel.xml");
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string xml = text.str();
    const std::string tag = "<filament_operations_group name=\"filament_operations_group\"";
    const auto first = xml.find(tag);
    REQUIRE(first != std::string::npos);
    CHECK(xml.find(tag, first + 1) != std::string::npos);
}
