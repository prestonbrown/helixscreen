// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Controls temperature card's "N more sensors" row, its only sensor output.
// The count is every enabled
// sensor except the chamber one (which has its own row), and tapping the row
// opens the sensors overlay.

#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/discovery_of_objects.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::sensors::TemperatureSensorManager;

namespace {

void collect_label_text(lv_obj_t* obj, std::vector<std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* txt = lv_label_get_text(obj);
        if (txt && txt[0]) {
            out.emplace_back(txt);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_label_text(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

class ControlsMoreSensorsFixture : public LVGLUITestFixture {
  public:
    ControlsMoreSensorsFixture() : panel(state(), nullptr) {
        auto& tsm = TemperatureSensorManager::instance();
        tsm.deinit_subjects();
        tsm.init_subjects();
    }

    ~ControlsMoreSensorsFixture() override {
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        TemperatureSensorManager::instance().discover({});
        TemperatureSensorManager::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    void build_and_activate() {
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    lv_obj_t* list() {
        lv_obj_t* l = lv_obj_find_by_name(panel_obj, "secondary_temps_list");
        REQUIRE(l != nullptr);
        return l;
    }

    /// The row's caption, or "" when the row is not shown (the chevron glyph
    /// is not a caption).
    std::string caption() {
        if (lv_obj_has_flag(list(), LV_OBJ_FLAG_HIDDEN)) {
            return "";
        }
        std::vector<std::string> labels;
        collect_label_text(list(), labels);
        for (const auto& t : labels) {
            if (t.size() > 12 && t.compare(t.size() - 12, 12, "more sensors") == 0) {
                return t;
            }
        }
        return "";
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsMoreSensorsFixture,
                 "Controls more-sensors row counts every enabled non-chamber sensor",
                 "[controls][temps][more-sensors]") {
    TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({"temperature_sensor mcu_temp", "temperature_sensor raspberry_pi",
                                   "temperature_fan exhaust_fan", "temperature_sensor chamber"}));
    build_and_activate();

    REQUIRE(caption() == "3 more sensors");
}

TEST_CASE_METHOD(ControlsMoreSensorsFixture, "Controls more-sensors row is empty with no sensors",
                 "[controls][temps][more-sensors]") {
    TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({"temperature_sensor chamber"}));
    build_and_activate();

    CHECK(caption().empty());
}

TEST_CASE_METHOD(ControlsMoreSensorsFixture, "Controls more-sensors row follows rediscovery",
                 "[controls][temps][more-sensors]") {
    TemperatureSensorManager::instance().discover(helix::test::discovery_of(
        {"temperature_sensor mcu_temp", "temperature_sensor raspberry_pi"}));
    build_and_activate();
    REQUIRE(caption() == "2 more sensors");

    TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({"temperature_sensor mcu_temp", "temperature_sensor raspberry_pi",
                                   "temperature_fan exhaust_fan"}));
    settle();
    CHECK(caption() == "3 more sensors");

    TemperatureSensorManager::instance().discover({});
    settle();
    CHECK(caption().empty());
}

TEST_CASE_METHOD(ControlsMoreSensorsFixture, "Controls more-sensors row is a tap target",
                 "[controls][temps][more-sensors]") {
    TemperatureSensorManager::instance().discover(
        helix::test::discovery_of({"temperature_sensor mcu_temp"}));
    build_and_activate();

    CHECK(lv_obj_has_flag(list(), LV_OBJ_FLAG_CLICKABLE));
}
