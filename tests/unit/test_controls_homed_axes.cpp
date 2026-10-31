// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Controls panel turns PrinterState's homed_axes string into the per-axis
// subjects its home buttons bind their style to.

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

namespace {

int homed(const char* subject_name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, subject_name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ControlsPanel derives its homing subjects from homed_axes",
                 "[controls][homing]") {
    ControlsPanel panel(state(), nullptr);
    panel.init_subjects();

    // XY needs both X and Y; "all" needs all three.
    struct Row {
        const char* axes;
        int x, y, xy, z, all;
    };
    const Row rows[] = {
        {"xyz", 1, 1, 1, 1, 1}, {"", 0, 0, 0, 0, 0},   {"x", 1, 0, 0, 0, 0},
        {"y", 0, 1, 0, 0, 0},   {"xy", 1, 1, 1, 0, 0}, {"z", 0, 0, 0, 1, 0},
        {"xz", 1, 0, 0, 1, 0},  {"yz", 0, 1, 0, 1, 0}, {"xyz", 1, 1, 1, 1, 1},
    };
    for (const auto& row : rows) {
        INFO("homed_axes '" << row.axes << "'");
        state().update_from_status(json{{"toolhead", {{"homed_axes", row.axes}}}});
        helix::ui::UpdateQueue::instance().drain();

        CHECK(homed("x_homed") == row.x);
        CHECK(homed("y_homed") == row.y);
        CHECK(homed("xy_homed") == row.xy);
        CHECK(homed("z_homed") == row.z);
        CHECK(homed("all_homed") == row.all);
    }

    panel.deinit_subjects();
    helix::ui::UpdateQueue::instance().drain();
}
