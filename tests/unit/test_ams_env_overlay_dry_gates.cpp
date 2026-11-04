// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_env_overlay_dry_gates.cpp
 * @brief The dryer overlay's lane chips and rotate chip: when they show, and which
 *        gates a start or stop names (prestonbrown/helixscreen#1026)
 */

#include "ui_ams_environment_overlay.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "static_panel_registry.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using namespace helix::ui;

namespace {

/// The per-lane rig, recording what the overlay asks of it instead of running a cycle.
class RecordingLaneRig : public AmsBackendMock {
  public:
    explicit RecordingLaneRig(bool per_lane) : AmsBackendMock(4) {
        set_dryer_enabled(true);
        if (per_lane) {
            set_environment_mode("emu_dryer");
        }
    }

    AmsError start_drying_request(const DryingRequest& request) override {
        starts.push_back(request);
        return AmsErrorHelper::success();
    }

    AmsError stop_drying_gates(int, const std::vector<int>& gates) override {
        stops.push_back(gates);
        return AmsErrorHelper::success();
    }

    DryerInfo get_dryer_info(int unit) const override {
        DryerInfo d = AmsBackendMock::get_dryer_info(unit);
        d.active = report_active;
        return d;
    }

    std::vector<DryingRequest> starts;
    std::vector<std::vector<int>> stops;
    bool report_active = false;
};

void reset_overlay_singleton() {
    helix::ui::destroy_static_panels();
    helix::ui::UpdateQueue::instance().drain();
}

lv_obj_t* find_widget(const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(lv_screen_active(), name);
    REQUIRE(obj != nullptr);
    return obj;
}

int subject_int(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}

std::string subject_string(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_string(subj);
}

/// show_beta_features, registered once: the XML subject registry is process-global and
/// never forgets an entry, so the subject has to outlive every test.
lv_subject_t* beta_subject() {
    static lv_subject_t subject;
    static bool registered = false;
    if (!registered) {
        if (lv_subject_t* existing = lv_xml_get_subject(nullptr, "show_beta_features")) {
            return existing;
        }
        lv_subject_init_int(&subject, 1);
        lv_xml_register_subject(nullptr, "show_beta_features", &subject);
        registered = true;
    }
    return &subject;
}

RecordingLaneRig* show_overlay(bool per_lane) {
    // Choosing gates needs no beta features, so every case runs with them off.
    lv_subject_set_int(beta_subject(), 0);
    auto backend = std::make_unique<RecordingLaneRig>(per_lane);
    auto* raw = backend.get();
    AmsState::instance().set_backend(std::move(backend));
    auto zones = raw->get_environment_zones(0);
    REQUIRE_FALSE(zones.empty());
    get_ams_environment_overlay().show_zone(lv_screen_active(), std::move(zones), 0, false);
    helix::ui::UpdateQueue::instance().drain();
    return raw;
}

/// Tap the lane chip at @p index, the way a finger does.
void tap_chip(int index) {
    lv_obj_t* chips = find_widget("dry_gate_chips");
    REQUIRE(index < static_cast<int>(lv_obj_get_child_count(chips)));
    lv_obj_send_event(lv_obj_get_child(chips, index), LV_EVENT_CLICKED, nullptr);
}

void tap_rotate() {
    lv_obj_send_event(find_widget("dry_rotate_chip"), LV_EVENT_CLICKED, nullptr);
}

void press_start_stop() {
    lv_obj_send_event(find_widget("btn_start_stop"), LV_EVENT_CLICKED, nullptr);
}

void finish() {
    reset_overlay_singleton();
    AmsState::instance().set_backend(nullptr);
    lv_subject_set_int(beta_subject(), 1);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "A dryer that heats its gates together offers no lane chips",
                 "[ams][dryer][dry_gates][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay(false);

    CHECK(subject_int("env_dry_gate_count") == 0);
    CHECK(subject_int("env_dry_rotate_visible") == 0);
    CHECK(lv_obj_has_flag(find_widget("dry_gate_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(find_widget("zone_slots_label"), LV_OBJ_FLAG_HIDDEN));

    press_start_stop();
    REQUIRE(backend->starts.size() == 1);
    CHECK(backend->starts[0].gates.empty());
    CHECK_FALSE(backend->starts[0].rotate);
    finish();
}

TEST_CASE_METHOD(XMLTestFixture, "A per-lane dryer offers a chip per lane, all selected",
                 "[ams][dryer][dry_gates][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay(true);
    auto& overlay = get_ams_environment_overlay();

    REQUIRE(subject_int("env_dry_gate_count") == 4);
    CHECK_FALSE(lv_obj_has_flag(find_widget("dry_gate_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(find_widget("zone_slots_label"), LV_OBJ_FLAG_HIDDEN));
    CHECK(subject_string("env_dry_gate_label_0") == "1");
    CHECK(subject_string("env_dry_gate_label_3") == "4");
    for (const char* sel : {"env_dry_gate_selected_0", "env_dry_gate_selected_1",
                            "env_dry_gate_selected_2", "env_dry_gate_selected_3"}) {
        CHECK(subject_int(sel) == 1);
    }
    CHECK(subject_int("env_dry_rotate_visible") == 1);
    CHECK(subject_int("env_dry_rotate_on") == 0);

    SECTION("every lane selected sends the plain whole-unit start") {
        press_start_stop();
        REQUIRE(backend->starts.size() == 1);
        CHECK(backend->starts[0].gates.empty());
        CHECK(backend->starts[0].unit == 0);
        CHECK_FALSE(backend->starts[0].rotate);
    }
    SECTION("a deselected lane is left out") {
        tap_chip(1);
        CHECK(subject_int("env_dry_gate_selected_1") == 0);
        press_start_stop();
        REQUIRE(backend->starts.size() == 1);
        CHECK(backend->starts[0].gates == std::vector<int>{0, 2, 3});
    }
    SECTION("reselecting a lane puts it back") {
        overlay.toggle_dry_gate(1);
        overlay.toggle_dry_gate(1);
        press_start_stop();
        REQUIRE(backend->starts.size() == 1);
        CHECK(backend->starts[0].gates.empty());
    }
    SECTION("the last selected lane stays selected") {
        overlay.toggle_dry_gate(0);
        overlay.toggle_dry_gate(1);
        overlay.toggle_dry_gate(2);
        overlay.toggle_dry_gate(3);
        CHECK(subject_int("env_dry_gate_selected_3") == 1);
        press_start_stop();
        REQUIRE(backend->starts.size() == 1);
        CHECK(backend->starts[0].gates == std::vector<int>{3});
    }
    SECTION("rotation rides the start") {
        tap_rotate();
        CHECK(subject_int("env_dry_rotate_on") == 1);
        overlay.toggle_dry_gate(0);
        press_start_stop();
        REQUIRE(backend->starts.size() == 1);
        CHECK(backend->starts[0].rotate);
        CHECK(backend->starts[0].gates == std::vector<int>{1, 2, 3});
    }
    SECTION("stop names the selected lanes, or none for all of them") {
        backend->report_active = true;
        overlay.refresh();
        CHECK(subject_int("ams_env_overlay_drying_active") == 1);
        press_start_stop();
        overlay.toggle_dry_gate(2);
        press_start_stop();
        REQUIRE(backend->stops.size() == 2);
        CHECK(backend->stops[0].empty());
        CHECK(backend->stops[1] == std::vector<int>{0, 1, 3});
        CHECK(backend->starts.empty());
    }
    finish();
}

TEST_CASE_METHOD(XMLTestFixture, "A fresh showing starts with every lane selected again",
                 "[ams][dryer][dry_gates][overlay]") {
    reset_overlay_singleton();
    auto* backend = show_overlay(true);
    tap_chip(2);
    tap_rotate();
    REQUIRE(subject_int("env_dry_gate_selected_2") == 0);
    REQUIRE(subject_int("env_dry_rotate_on") == 1);

    // Shown again on the same instance, as a second tap on the box would.
    get_ams_environment_overlay().show_zone(lv_screen_active(), backend->get_environment_zones(0),
                                            0, false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(subject_int("env_dry_gate_selected_2") == 1);
    CHECK(subject_int("env_dry_rotate_on") == 0);
    finish();
}

TEST_CASE_METHOD(XMLTestFixture, "An overlay with no zone offers no lane chips",
                 "[ams][dryer][dry_gates][overlay]") {
    reset_overlay_singleton();
    show_overlay(true);
    REQUIRE(subject_int("env_dry_gate_count") == 4);

    // The printer's filament system goes away beneath the open overlay: nothing reports
    // a box any more, so the chips go with it.
    auto bare = std::make_unique<AmsBackendMock>(4);
    bare->set_dryer_enabled(false);
    bare->set_environment_mode("off");
    REQUIRE(bare->get_environment_zones(-1).empty());
    AmsState::instance().set_backend(std::move(bare));
    get_ams_environment_overlay().show_zone(lv_screen_active(), {}, 0, false);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(subject_int("env_dry_gate_count") == 0);
    CHECK(subject_int("env_dry_rotate_visible") == 0);
    finish();
}

TEST_CASE("A Stop on some lanes names the lanes it stopped", "[ams][dryer][dry_gates]") {
    CHECK(stopped_lanes_text({1, 2}, LaneNoun::Gate) == "Stopped drying Gate 2, Gate 3");
    CHECK(stopped_lanes_text({0}, LaneNoun::Lane) == "Stopped drying Lane 1");
}
