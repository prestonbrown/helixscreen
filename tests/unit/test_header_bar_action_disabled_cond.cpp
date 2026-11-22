// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_header_bar_action_disabled_cond.cpp
 * @brief The calibration panels gate their Start button (in the header, or
 *        the footer's btn_start) on the printer connection while it is offline.
 *
 * Context (K2 Plus, v0.99.98): the WebSocket to Moonraker
 * never opened. The PID panel greyed out its *content* — the gate lives on
 * `overlay_content` — but the Start button sits in the header bar, a sibling of
 * that container, so it stayed live. The reporter pressed it and got a failure
 * dialog. Bed mesh, input shaper and belt tension had the same shape.
 *
 * Two bind_state_* on one LVGL state clobber each other (each observer asserts
 * BOTH polarities on every fire), so each panel's button carries a single
 * expression-driven binding.
 *
 * The test reads the real panel XML and compiles the exact expression each panel
 * ships onto a button in a header's actions slot. That is deliberate: a cond naming a subject that
 * does not exist fails to compile, logs a warning, installs NO observer, and
 * leaves the button permanently enabled — an inert fix that reads fine in review.
 * Compiling each shipped expression against live subjects is what catches that.
 */

#include "../test_fixtures.h"
#include "core/subject.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml_component.h"

#include <fstream>
#include <lvgl.h>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

/// The disabled condition on a panel's header action: the first
/// `<bind_state_if cond="...">` inside its header actions slot. Returns empty
/// if absent, which is the regression this guards.
std::string read_disabled_cond(const std::string& xml_path) {
    std::ifstream f(xml_path);
    if (!f) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    // The quoted value of the first `key` in [from, to).
    auto value_in = [&src](const std::string& key, size_t from, size_t to) -> std::string {
        const size_t start = src.find(key, from);
        if (start == std::string::npos || start >= to) {
            return {};
        }
        const size_t value_start = start + key.size();
        const size_t end = src.find('"', value_start);
        if (end == std::string::npos) {
            return {};
        }
        return src.substr(value_start, end - value_start);
    };

    const size_t actions = src.find("-actions>");
    if (actions != std::string::npos) {
        return value_in("<bind_state_if cond=\"", actions, src.find("-actions>", actions + 1));
    }
    // A screen on the calibration intro frame names its footer primary btn_start.
    // The wizard footer composes the screen's primary_disabled_cond with the
    // connection gate (pinned by the footer case below), so this is what it binds.
    const size_t framed = src.find("primary_name=\"btn_start\"");
    if (framed != std::string::npos) {
        const size_t open = src.rfind('<', framed);
        const std::string own = value_in("primary_disabled_cond=\"", open, src.find('>', framed));
        return "(nav_buttons_enabled eq 0 and (1)) or (" + (own.empty() ? std::string("0") : own) +
               ")";
    }
    // A screen whose primary action sits in its own footer names it btn_start.
    const size_t start = src.find("name=\"btn_start\"");
    if (start == std::string::npos) {
        return {};
    }
    return value_in("<bind_state_if cond=\"", start, src.find("</ui_button>", start));
}

/// Registers a screen whose header actions slot holds one Start button
/// disabled by `cond`, the shape every calibration panel ships.
void register_gated_start_screen(const std::string& name, const std::string& cond) {
    const std::string xml = "<component><view extends=\"overlay_panel\" title=\"Cal\">"
                            "<overlay_panel-actions>"
                            "<ui_button name=\"btn_start\" height=\"content\" text=\"Start\">"
                            "<bind_state_if cond=\"" +
                            cond +
                            "\" state=\"disabled\"/>"
                            "</ui_button>"
                            "</overlay_panel-actions>"
                            "</view></component>";
    REQUIRE(lv_xml_register_component_from_data(name.c_str(), xml.c_str()) == LV_RESULT_OK);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "calibration panels gate their header action button on connection",
                 "[header_bar][xml][calibration_gate]") {
    REQUIRE(register_component("header_bar"));

    // Subjects the shipped expressions reference besides nav_buttons_enabled.
    // In production these are registered by each panel's init_subjects() at
    // startup, long before the overlay XML is created.
    static helix::Subject pid_not_idle;
    pid_not_idle.init_int(0);
    lv_xml_register_subject(nullptr, "pid_cal_not_idle", &pid_not_idle);

    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    REQUIRE(nav != nullptr);

    struct PanelCase {
        const char* xml;
        const char* label;
    };
    const PanelCase panels[] = {
        {"ui_xml/calibration_pid_panel.xml", "PID"},
        {"ui_xml/bed_mesh_panel.xml", "bed mesh"},
    };

    for (const auto& p : panels) {
        INFO("panel: " << p.label << " (" << p.xml << ")");

        const std::string cond = read_disabled_cond(p.xml);
        REQUIRE_FALSE(cond.empty());
        // Whatever else it gates on, reachability must be part of it.
        REQUIRE(cond.find("nav_buttons_enabled") != std::string::npos);

        lv_subject_set_int(nav, 1);
        lv_subject_set_int(&pid_not_idle, 0);

        const std::string screen_name = std::string("gated_start_") + std::to_string(&p - panels);
        register_gated_start_screen(screen_name, cond);
        lv_obj_t* hdr = create_component(screen_name.c_str());
        REQUIRE(hdr != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(hdr, "btn_start");
        REQUIRE(btn != nullptr);
        REQUIRE(lv_obj_get_parent(btn) == lv_obj_find_by_name(hdr, "header_actions"));

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
    pid_not_idle.reset();
}

TEST_CASE_METHOD(
    XMLTestFixture,
    "a wizard footer's primary stays disabled while offline even when its step is ready",
    "[calibration_gate][calibration_run]") {
    REQUIRE(register_component("calibration_wizard_footer"));
    static helix::Subject ready;
    ready.init_int(1);
    lv_xml_register_subject(nullptr, "test_footer_step_ready", &ready);
    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    REQUIRE(nav != nullptr);
    lv_subject_set_int(nav, 1);

    const char* attrs[] = {"primary_text",
                           "Start",
                           "primary_callback",
                           "on_test_footer_start",
                           "primary_name",
                           "test_footer_start",
                           "primary_disabled_cond",
                           "test_footer_step_ready eq 0",
                           nullptr};
    lv_obj_t* footer = create_component("calibration_wizard_footer", attrs);
    REQUIRE(footer != nullptr);
    lv_obj_t* start = lv_obj_find_by_name(footer, "test_footer_start");
    REQUIRE(start != nullptr);

    // Reachable and ready: live.
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(start, LV_STATE_DISABLED));

    // The step's own condition still disables it.
    lv_subject_set_int(&ready, 0);
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_state(start, LV_STATE_DISABLED));

    // Offline: disabled although the step says ready.
    lv_subject_set_int(&ready, 1);
    lv_subject_set_int(nav, 0);
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_state(start, LV_STATE_DISABLED));

    lv_subject_set_int(nav, 1);
    UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(start, LV_STATE_DISABLED));

    lv_obj_delete(footer);
    UpdateQueue::instance().drain();
    ready.reset();
}

namespace {

/// The quoted value of @p attr on the `<calibration_wizard_footer name="footer">`
/// element of @p xml_path, or empty.
std::string footer_attr(const std::string& xml_path, const std::string& footer,
                        const std::string& attr) {
    std::ifstream f(xml_path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    const size_t at = src.find("<calibration_wizard_footer name=\"" + footer + "\"");
    if (at == std::string::npos) {
        return {};
    }
    const size_t end = src.find("/>", at);
    const std::string key = attr + "=\"";
    const size_t start = src.find(key, at);
    if (start == std::string::npos || start > end) {
        return {};
    }
    const size_t value_start = start + key.size();
    return src.substr(value_start, src.find('"', value_start) - value_start);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "the run's See results and Done stay live while offline, and its Save does not",
                 "[calibration_gate][calibration_run]") {
    REQUIRE(register_component("calibration_wizard_footer"));
    static helix::Subject offers_save;
    offers_save.init_int(1);
    lv_xml_register_subject(nullptr, "cal_run_offers_save", &offers_save);
    lv_subject_t* nav = state().network_state().get_nav_buttons_enabled_subject();
    REQUIRE(nav != nullptr);

    auto make_footer = [this](const std::string& needs) {
        const char* attrs[] = {"primary_text",          "Go",           "primary_callback",
                               "on_test_footer_go",     "primary_name", "test_footer_go",
                               "primary_needs_printer", needs.c_str(),  nullptr};
        lv_obj_t* footer = create_component("calibration_wizard_footer", attrs);
        REQUIRE(footer != nullptr);
        return footer;
    };
    auto disabled = [](lv_obj_t* footer) {
        UpdateQueue::instance().drain();
        lv_obj_t* btn = lv_obj_find_by_name(footer, "test_footer_go");
        REQUIRE(btn != nullptr);
        return lv_obj_has_state(btn, LV_STATE_DISABLED);
    };

    const std::string see_results = footer_attr("ui_xml/calibration_run_overlay.xml",
                                                "cal_run_running_footer", "primary_needs_printer");
    const std::string results = footer_attr("ui_xml/calibration_run_overlay.xml",
                                            "cal_run_results_footer", "primary_needs_printer");
    REQUIRE_FALSE(see_results.empty());
    REQUIRE_FALSE(results.empty());

    lv_subject_set_int(nav, 0);
    lv_obj_t* running = make_footer(see_results);
    lv_obj_t* finished = make_footer(results);
    CHECK_FALSE(disabled(running));
    // Save needs the printer.
    CHECK(disabled(finished));
    // Done sends nothing.
    lv_subject_set_int(&offers_save, 0);
    CHECK_FALSE(disabled(finished));

    lv_subject_set_int(nav, 1);
    lv_subject_set_int(&offers_save, 1);
    CHECK_FALSE(disabled(finished));

    lv_obj_delete(running);
    lv_obj_delete(finished);
    UpdateQueue::instance().drain();
    offers_save.reset();
}
