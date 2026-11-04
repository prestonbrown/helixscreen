// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_bar_action_disabled_cond.cpp
 * @brief header_bar's action_button_disabled_cond prop, and the calibration
 *        panels that depend on it to gate Start while the printer is offline.
 *
 * Context (debug bundle XRK8KPTF, K2 Plus, v0.99.98): the WebSocket to Moonraker
 * never opened. The PID panel greyed out its *content* — the gate lives on
 * `overlay_content` — but the Start button sits in the header bar, a sibling of
 * that container, so it stayed live. The reporter pressed it and got a failure
 * dialog. Bed mesh, input shaper and belt tension had the same shape.
 *
 * header_bar's action button supports exactly one disabled binding, and two
 * bind_state_* on one LVGL state clobber each other (each observer asserts BOTH
 * polarities on every fire), so the fix is a single expression-driven binding.
 *
 * The second test reads the real panel XML and drives header_bar with the exact
 * expression each panel ships. That is deliberate: a cond naming a subject that
 * does not exist fails to compile, logs a warning, installs NO observer, and
 * leaves the button permanently enabled — an inert fix that reads fine in review.
 * Compiling each shipped expression against live subjects is what catches that.
 */

#include "../test_fixtures.h"
#include "core/update_queue.h"

#include <fstream>
#include <lvgl.h>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// Pull action_button_disabled_cond="..." out of a panel's XML source.
/// Returns empty if absent — which is the regression this guards.
std::string read_disabled_cond(const std::string& xml_path) {
    std::ifstream f(xml_path);
    if (!f) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    const std::string key = "action_button_disabled_cond=\"";
    const size_t start = src.find(key);
    if (start == std::string::npos) {
        return {};
    }
    const size_t value_start = start + key.size();
    const size_t end = src.find('"', value_start);
    if (end == std::string::npos) {
        return {};
    }
    return src.substr(value_start, end - value_start);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "header_bar action button disables via cond expression",
                 "[header_bar][xml][calibration_gate]") {
    REQUIRE(register_component("header_bar"));

    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    REQUIRE(nav != nullptr);
    lv_subject_set_int(nav, 1); // connected + klippy ready

    const char* attrs[] = {"action_button_text",
                           "Start",
                           "hide_action_button",
                           "false",
                           "action_button_disabled_cond",
                           "nav_buttons_enabled eq 0",
                           nullptr};
    lv_obj_t* hdr = create_component("header_bar", attrs);
    REQUIRE(hdr != nullptr);
    lv_obj_t* btn = lv_obj_find_by_name(hdr, "action_button");
    REQUIRE(btn != nullptr);

    // Ready -> clickable.
    REQUIRE_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));

    // Printer goes away -> Start must go dead. This is the bundle's scenario.
    lv_subject_set_int(nav, 0);
    UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_state(btn, LV_STATE_DISABLED));

    // ...and come back when it returns.
    lv_subject_set_int(nav, 1);
    UpdateQueue::instance().drain();
    REQUIRE_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(XMLTestFixture, "calibration panels gate their header action button on connection",
                 "[header_bar][xml][calibration_gate]") {
    REQUIRE(register_component("header_bar"));

    // Subjects the shipped expressions reference besides nav_buttons_enabled.
    // In production these are registered by each panel's init_subjects() at
    // startup, long before the overlay XML is created.
    static lv_subject_t pid_not_idle;
    static lv_subject_t is_calibrate_all_disabled;
    lv_subject_init_int(&pid_not_idle, 0);
    lv_subject_init_int(&is_calibrate_all_disabled, 0);
    lv_xml_register_subject(nullptr, "pid_cal_not_idle", &pid_not_idle);
    lv_xml_register_subject(nullptr, "is_calibrate_all_disabled", &is_calibrate_all_disabled);

    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    REQUIRE(nav != nullptr);

    struct PanelCase {
        const char* xml;
        const char* label;
    };
    const PanelCase panels[] = {
        {"ui_xml/calibration_pid_panel.xml", "PID"},
        {"ui_xml/bed_mesh_panel.xml", "bed mesh"},
        {"ui_xml/input_shaper_panel.xml", "input shaper"},
        {"ui_xml/panel_belt_tension.xml", "belt tension"},
    };

    for (const auto& p : panels) {
        INFO("panel: " << p.label << " (" << p.xml << ")");

        const std::string cond = read_disabled_cond(p.xml);
        REQUIRE_FALSE(cond.empty());
        // Whatever else it gates on, reachability must be part of it.
        REQUIRE(cond.find("nav_buttons_enabled") != std::string::npos);

        lv_subject_set_int(nav, 1);
        lv_subject_set_int(&pid_not_idle, 0);
        lv_subject_set_int(&is_calibrate_all_disabled, 0);

        const char* attrs[] = {"action_button_text",
                               "Start",
                               "hide_action_button",
                               "false",
                               "action_button_disabled_cond",
                               cond.c_str(),
                               nullptr};
        lv_obj_t* hdr = create_component("header_bar", attrs);
        REQUIRE(hdr != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(hdr, "action_button");
        REQUIRE(btn != nullptr);

        // Idle and reachable: the button is the whole point of the screen.
        REQUIRE_FALSE(lv_obj_has_state(btn, LV_STATE_DISABLED));

        // Unreachable: dead, regardless of what else the expression gates on.
        // A cond that failed to compile installs no observer and fails here.
        lv_subject_set_int(nav, 0);
        UpdateQueue::instance().drain();
        REQUIRE(lv_obj_has_state(btn, LV_STATE_DISABLED));

        lv_obj_delete(hdr);
        UpdateQueue::instance().drain();
    }

    lv_subject_set_int(nav, 1);
    lv_subject_deinit(&pid_not_idle);
    lv_subject_deinit(&is_calibrate_all_disabled);
}
