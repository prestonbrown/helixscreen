// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file demo_mockups.cpp
 * @brief PROTOTYPE: design mockups reached with `ctl demo mock:<component>`.
 *
 * Raises an XML component as a modal (name ends in `_modal`) or as an overlay,
 * with the sample subjects its markup binds. `ctl set <subject> <v>` drives the
 * states (mock_hub_clear, mock_tip_choice, ...).
 */

#include "demo_mockups.h"

#include "ui_modal.h"
#include "ui_nav_manager.h"

#include "core/subject.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace {

std::deque<helix::Subject>& subject_store() {
    static std::deque<helix::Subject> store;
    return store;
}

struct StrBuf {
    char now[256];
    char prev[256];
};

std::deque<StrBuf>& str_store() {
    static std::deque<StrBuf> store;
    return store;
}

void mock_int_subject(const char* name, int value) {
    if (lv_subject_t* s = lv_xml_get_subject(nullptr, name)) {
        lv_subject_set_int(s, value);
        return;
    }
    helix::Subject& s = subject_store().emplace_back();
    s.init_int(value);
    lv_xml_register_subject(nullptr, name, s.get());
}

void mock_str_subject(const char* name, const char* value) {
    if (lv_subject_t* s = lv_xml_get_subject(nullptr, name)) {
        lv_subject_set_string(s, value);
        return;
    }
    StrBuf& b = str_store().emplace_back();
    helix::Subject& s = subject_store().emplace_back();
    s.init_string(b.now, b.prev, sizeof(b.now), value);
    lv_xml_register_subject(nullptr, name, s.get());
}

void set_int(const char* name, int value) {
    if (lv_subject_t* s = lv_xml_get_subject(nullptr, name))
        lv_subject_set_int(s, value);
}

int ud_int(lv_event_t* e) {
    const char* ud = static_cast<const char*>(lv_event_get_user_data(e));
    return ud ? std::atoi(ud) : 0;
}

void register_once() {
    static bool done = false;
    if (done)
        return;
    done = true;

    // Design 1: lane recovery
    mock_int_subject("mock_hub_clear", 0);
    mock_int_subject("mock_jog_step", 1);
    lv_xml_register_event_cb(nullptr, "on_mock_jog_step",
                             [](lv_event_t* e) { set_int("mock_jog_step", ud_int(e)); });
    lv_xml_register_event_cb(nullptr, "on_mock_lane_retract",
                             [](lv_event_t*) { set_int("mock_hub_clear", 1); });
    lv_xml_register_event_cb(nullptr, "on_mock_lane_feed",
                             [](lv_event_t*) { set_int("mock_hub_clear", 0); });

    // Design 2: tip forming
    mock_int_subject("mock_tip_lane", 0);
    mock_int_subject("mock_tip_material", 0);
    mock_int_subject("mock_tip_hotend", 0);
    mock_int_subject("mock_tip_choice", 0);
    lv_xml_register_event_cb(nullptr, "on_mock_tip_lane",
                             [](lv_event_t* e) { set_int("mock_tip_lane", ud_int(e)); });
    lv_xml_register_event_cb(nullptr, "on_mock_tip_material",
                             [](lv_event_t* e) { set_int("mock_tip_material", ud_int(e)); });
    lv_xml_register_event_cb(nullptr, "on_mock_tip_hotend",
                             [](lv_event_t* e) { set_int("mock_tip_hotend", ud_int(e)); });
    lv_xml_register_event_cb(nullptr, "on_mock_tip_choice",
                             [](lv_event_t* e) { set_int("mock_tip_choice", ud_int(e)); });
    mock_int_subject("mock_tip_stage", 0);
    lv_xml_register_event_cb(nullptr, "on_mock_tip_next", [](lv_event_t*) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "mock_tip_stage");
        lv_subject_set_int(s, (lv_subject_get_int(s) + 1) % 4);
    });
    lv_xml_register_event_cb(nullptr, "on_mock_tip_prev", [](lv_event_t*) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "mock_tip_stage");
        lv_subject_set_int(s, (lv_subject_get_int(s) + 3) % 4);
    });
    // Run steps, on calibration_run_step_row's subjects: 0 pending, 1 active, 2 done.
    const char* steps[] = {"tip_load", "tip_form", "tip_unload", "tip_eject"};
    const char* details[] = {"", "Cooling moves at 27 mm, 4 passes", "412 of 600 mm", ""};
    for (int i = 0; i < 4; ++i) {
        mock_int_subject(("cal_run_state_" + std::string(steps[i])).c_str(),
                         i < 2 ? 2 : (i == 2 ? 1 : 0));
        mock_int_subject(("cal_run_in_run_" + std::string(steps[i])).c_str(), 1);
        mock_str_subject(("cal_run_detail_" + std::string(steps[i])).c_str(), details[i]);
    }
    mock_str_subject("mock_tip_v_pos", "27 / 12 mm");
    mock_str_subject("mock_tip_v_len", "12 mm");
    mock_str_subject("mock_tip_v_temp", "200 \xC2\xB0"
                                        "C");
    mock_str_subject("mock_tip_r_pos", "27 mm");
    mock_str_subject("mock_tip_r_len", "12 mm");
    mock_str_subject("mock_tip_r_temp", "200 to 195 \xC2\xB0"
                                        "C");
    mock_int_subject("mock_tip_hl", 1);
    mock_int_subject("mock_tip_progress", 68);

    // Design 3: unload vs eject
    mock_int_subject("mock_eject_stage", 0);
    lv_xml_register_event_cb(nullptr, "on_mock_eject", [](lv_event_t*) {
        set_int("mock_eject_stage", 0);
        helix::ui::ComponentModal::show_owned("mock_eject_pull_modal");
    });
}

} // namespace

bool helix::show_mockup(const std::string& name) {
    const std::string prefix = "mock:";
    if (name.rfind(prefix, 0) != 0)
        return false;
    register_once();
    if (name == "mock:init")
        return true;
    const std::string component = name.substr(prefix.size());
    if (component.size() > 6 && component.compare(component.size() - 6, 6, "_modal") == 0) {
        if (!helix::ui::ComponentModal::show_owned(component.c_str()))
            spdlog::warn("[Mockup] could not create modal {}", component);
        return true;
    }
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), component.c_str(), nullptr));
    if (!root) {
        spdlog::warn("[Mockup] could not create overlay {}", component);
        return true;
    }
    NavigationManager::instance().push_overlay(root);
    return true;
}
