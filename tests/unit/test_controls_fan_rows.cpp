// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The secondary fan rows on the Controls cooling card: which fans show and in
// what order, what each row says, the "N additional fans" overflow, and the
// live speed on a row.

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

namespace {

/// Every label under @p obj, depth first.
void collect_labels(lv_obj_t* obj, std::vector<std::string>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* txt = lv_label_get_text(obj);
        out.emplace_back(txt ? txt : "");
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        collect_labels(lv_obj_get_child(obj, static_cast<int32_t>(i)), out);
    }
}

class ControlsFanRowsFixture : public LVGLUITestFixture {
  public:
    ControlsFanRowsFixture() : panel(state(), nullptr) {}

    ~ControlsFanRowsFixture() override {
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
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
        lv_obj_t* l = lv_obj_find_by_name(panel_obj, "secondary_fans_list");
        REQUIRE(l != nullptr);
        return l;
    }

    /// One entry per row, each the row's labels in order.
    std::vector<std::vector<std::string>> rows() {
        std::vector<std::vector<std::string>> out;
        lv_obj_t* l = list();
        for (uint32_t i = 0; i < lv_obj_get_child_count(l); ++i) {
            out.emplace_back();
            collect_labels(lv_obj_get_child(l, static_cast<int32_t>(i)), out.back());
        }
        return out;
    }

    /// The display name PrinterState derived for @p object_name.
    std::string display_name(const std::string& object_name) {
        for (const auto& fan : state().fan_state().get_fans()) {
            if (fan.object_name == object_name) {
                return fan.display_name;
            }
        }
        FAIL("no such fan: " << object_name);
        return {};
    }

    void set_speed(const std::string& object_name, double speed) {
        state().update_from_status(json{{object_name, json{{"speed", speed}}}});
        settle();
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsFanRowsFixture,
                 "Controls fan rows show the two highest-priority secondary fans",
                 "[controls][fans][fan-rows]") {
    // Priority: a chamber fan, then controllable generic fans, then heater fans,
    // then controller fans. The part-cooling "fan" never appears here.
    state().fan_state().init_fans({"fan", "controller_fan board", "heater_fan hotend_fan",
                                   "fan_generic exhaust", "fan_generic chamber_circ"});
    build_and_activate();

    const auto r = rows();
    REQUIRE(r.size() == 3);

    REQUIRE(r[0].size() == 3);
    CHECK(r[0][0] == display_name("fan_generic chamber_circ"));
    CHECK(r[0][1] == "Off");

    REQUIRE(r[1].size() == 3);
    CHECK(r[1][0] == display_name("fan_generic exhaust"));

    // Two fans did not fit.
    REQUIRE(r[2].size() == 2);
    CHECK(r[2][0] == "2 additional fans");
}

TEST_CASE_METHOD(ControlsFanRowsFixture, "Controls fan rows mark a controllable fan differently",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "fan_generic exhaust", "heater_fan hotend_fan"});
    build_and_activate();

    const auto r = rows();
    REQUIRE(r.size() == 2);
    REQUIRE(r[0].size() == 3);
    REQUIRE(r[1].size() == 3);
    // exhaust is controllable, the heater fan is automatic: the indicators differ.
    CHECK(r[0][2] != r[1][2]);
    CHECK_FALSE(r[0][2].empty());
    CHECK_FALSE(r[1][2].empty());
}

TEST_CASE_METHOD(ControlsFanRowsFixture, "Controls fan rows say 'fan' for a single overflow",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "fan_generic a", "fan_generic b", "fan_generic c"});
    build_and_activate();

    const auto r = rows();
    REQUIRE(r.size() == 3);
    REQUIRE(r[2].size() == 2);
    CHECK(r[2][0] == "1 additional fan");
}

TEST_CASE_METHOD(ControlsFanRowsFixture, "Controls fan row speed follows the fan, no overflow row",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "fan_generic exhaust"});
    build_and_activate();
    REQUIRE(rows().size() == 1);
    CHECK(rows()[0].size() == 3);
    REQUIRE(rows().at(0).at(1) == "Off");

    set_speed("fan_generic exhaust", 0.5);
    CHECK(rows().at(0).at(1) == "50%");

    set_speed("fan_generic exhaust", 0.0);
    CHECK(rows().at(0).at(1) == "Off");
}

TEST_CASE_METHOD(ControlsFanRowsFixture, "Controls fan speed skips a deleted row's label",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "fan_generic exhaust"});
    build_and_activate();
    lv_obj_t* row = lv_obj_get_child(list(), 0);
    REQUIRE(row != nullptr);
    const uint32_t before = lv_obj_get_child_count(list());
    lv_obj_delete(row);
    REQUIRE(lv_obj_get_child_count(list()) == before - 1);

    set_speed("fan_generic exhaust", 0.5);
}

TEST_CASE_METHOD(ControlsFanRowsFixture, "Controls fan rows rebuild when fans are rediscovered",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "fan_generic exhaust"});
    build_and_activate();
    REQUIRE(rows().size() == 1);

    state().fan_state().init_fans({"fan", "fan_generic exhaust", "fan_generic filter"});
    settle();
    settle();
    CHECK(rows().size() == 2);

    // A tap anywhere on the list opens the fan overlay.
    CHECK(lv_obj_has_flag(list(), LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(ControlsFanRowsFixture,
                 "Controls fan rows rank a chamber-role fan first, by the role registry's rule",
                 "[controls][fans][fan-rows]") {
    // A filter or nevermore fan is the enclosure's air handling even when its
    // name does not say "chamber"; a temperature_fan is chamber-role too.
    state().fan_state().init_fans({"fan", "fan_generic exhaust", "fan_generic nevermore"});
    build_and_activate();

    const auto r = rows();
    REQUIRE(r.size() == 2);
    CHECK(r[0][0] == display_name("fan_generic nevermore"));
    CHECK(r[1][0] == display_name("fan_generic exhaust"));
}

TEST_CASE_METHOD(ControlsFanRowsFixture,
                 "Controls fan rows rank a temperature fan ahead of a heater fan",
                 "[controls][fans][fan-rows]") {
    state().fan_state().init_fans({"fan", "heater_fan hotend_fan", "temperature_fan enclosure"});
    build_and_activate();

    const auto r = rows();
    REQUIRE(r.size() == 2);
    CHECK(r[0][0] == display_name("temperature_fan enclosure"));
    CHECK(r[1][0] == display_name("heater_fan hotend_fan"));
}
