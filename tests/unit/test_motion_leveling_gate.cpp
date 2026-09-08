// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_leveling_gate.cpp
 * @brief QGL and Z-Tilt obey BOTH reasons to stay dead, on every panel carrying them
 *
 * Two independent conditions forbid dispatching a leveling macro: a job owns
 * the toolhead (job_holds_machine), and a controls operation is already in
 * flight (controls_operation_in_progress). QUAD_GANTRY_LEVEL reaching a
 * printer mid-print is the failure these guards exist to prevent.
 *
 * LVGL resolves a state binding by asserting BOTH polarities on every fire —
 * the observer adds or removes LV_STATE_DISABLED unconditionally — so two
 * bindings aimed at one state do not compose. They settle on whichever
 * subject notified last instead of on the union of the two reasons, which
 * leaves the button live whenever the last notifier happens to be the
 * satisfied one. A single expression binding is what makes the union hold.
 *
 * Three shipped panels carry these buttons: motion_panel, controls_panel and
 * micro/controls_panel. The first two are driven behaviourally below. The
 * micro variant declares <view name="controls_panel"> just as the standard one
 * does, so registering both in a single process would overwrite the other in
 * the global XML component registry and silently corrupt whichever test ran
 * second; it is covered by the source-level case instead.
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/xml_bind_test_utils.h"
#include "printer_state.h"

#include <fstream>
#include <lvgl.h>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using helix::test::PanelSubjectOwner;
using helix::test::require_named;
using helix::test::xml_subject;

namespace {

/// controls_operation_in_progress belongs to ControlsPanel's operation guard,
/// not to PrinterState, so XMLTestFixture does not publish it.
using ControlsPanelSubjects = PanelSubjectOwner<ControlsPanel>;

/// Both leveling buttons carry the same guard, so every case asserts on both.
constexpr const char* kLevelingButtons[] = {"btn_qgl", "btn_z_tilt"};

/// Panels that can be built in-process. See the file header for why the micro
/// variant is not in this list.
constexpr const char* kBuildablePanels[] = {"motion_panel", "controls_panel"};

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    REQUIRE(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The <ui_button name="..."> ... </ui_button> block for one button.
std::string button_block(const std::string& src, const std::string& button) {
    const std::string open = "<ui_button name=\"" + button + "\"";
    const size_t start = src.find(open);
    INFO("looking for <ui_button name=\"" << button << "\">");
    REQUIRE(start != std::string::npos);
    const size_t end = src.find("</ui_button>", start);
    REQUIRE(end != std::string::npos);
    return src.substr(start, end - start);
}

/// Count bind_state_* elements in `block` that write LVGL's disabled state.
int disabled_state_binds(const std::string& block) {
    int n = 0;
    size_t pos = 0;
    while ((pos = block.find("<bind_state_", pos)) != std::string::npos) {
        const size_t close = block.find("/>", pos);
        if (close == std::string::npos) {
            break;
        }
        if (block.substr(pos, close - pos).find("state=\"disabled\"") != std::string::npos) {
            ++n;
        }
        pos = close;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "leveling buttons are disabled when a job holds the machine",
                 "[ui][motion_panel][controls_panel][bind_state][leveling_gate]") {
    ControlsPanelSubjects owner(state());

    for (const char* panel_name : kBuildablePanels) {
        INFO("panel: " << panel_name);
        REQUIRE(register_component(panel_name));

        // The dispatch-during-print case: the job guard is the only one asserted.
        lv_subject_set_int(xml_subject("job_holds_machine"), 1);
        lv_subject_set_int(xml_subject("controls_operation_in_progress"), 0);

        lv_obj_t* panel = create_component(panel_name);
        REQUIRE(panel != nullptr);

        for (const char* name : kLevelingButtons) {
            INFO("button: " << name);
            CHECK(lv_obj_has_state(require_named(panel, name), LV_STATE_DISABLED));
        }
    }
}

TEST_CASE_METHOD(XMLTestFixture, "leveling buttons are disabled when a controls operation is in flight",
                 "[ui][motion_panel][controls_panel][bind_state][leveling_gate]") {
    ControlsPanelSubjects owner(state());

    for (const char* panel_name : kBuildablePanels) {
        INFO("panel: " << panel_name);
        REQUIRE(register_component(panel_name));

        // The mirror image: the operation guard alone must be enough.
        lv_subject_set_int(xml_subject("job_holds_machine"), 0);
        lv_subject_set_int(xml_subject("controls_operation_in_progress"), 1);

        lv_obj_t* panel = create_component(panel_name);
        REQUIRE(panel != nullptr);

        for (const char* name : kLevelingButtons) {
            INFO("button: " << name);
            CHECK(lv_obj_has_state(require_named(panel, name), LV_STATE_DISABLED));
        }
    }
}

TEST_CASE_METHOD(XMLTestFixture, "a settling controls operation does not release the job guard",
                 "[ui][motion_panel][controls_panel][bind_state][leveling_gate]") {
    ControlsPanelSubjects owner(state());

    for (const char* panel_name : kBuildablePanels) {
        INFO("panel: " << panel_name);
        REQUIRE(register_component(panel_name));

        lv_subject_t* holds = xml_subject("job_holds_machine");
        lv_subject_t* op = xml_subject("controls_operation_in_progress");
        lv_subject_set_int(holds, 1);
        lv_subject_set_int(op, 1);

        lv_obj_t* panel = create_component(panel_name);
        REQUIRE(panel != nullptr);

        // An operation finishing publishes a 0 while the print goes on holding
        // the toolhead. Being the most recent notifier must not let it decide
        // alone.
        lv_subject_set_int(op, 0);
        UpdateQueue::instance().drain();

        for (const char* name : kLevelingButtons) {
            INFO("button: " << name);
            CHECK(lv_obj_has_state(require_named(panel, name), LV_STATE_DISABLED));
        }
    }
}

TEST_CASE_METHOD(XMLTestFixture, "leveling buttons are live once neither guard is asserted",
                 "[ui][motion_panel][controls_panel][bind_state][leveling_gate]") {
    ControlsPanelSubjects owner(state());

    for (const char* panel_name : kBuildablePanels) {
        INFO("panel: " << panel_name);
        REQUIRE(register_component(panel_name));

        lv_subject_t* holds = xml_subject("job_holds_machine");
        lv_subject_t* op = xml_subject("controls_operation_in_progress");
        lv_subject_set_int(holds, 1);
        lv_subject_set_int(op, 1);

        lv_obj_t* panel = create_component(panel_name);
        REQUIRE(panel != nullptr);

        // Without this the gate could be a constant rather than a binding, and
        // every assertion above would hold for the wrong reason.
        lv_subject_set_int(holds, 0);
        lv_subject_set_int(op, 0);
        UpdateQueue::instance().drain();

        for (const char* name : kLevelingButtons) {
            INFO("button: " << name);
            CHECK_FALSE(lv_obj_has_state(require_named(panel, name), LV_STATE_DISABLED));
        }
    }
}

TEST_CASE("every shipped leveling button carries exactly one disabled binding",
          "[ui][motion_panel][controls_panel][bind_state][leveling_gate]") {
    // Source-level because micro/controls_panel cannot be registered alongside
    // controls_panel, and because a second binding is the defect itself: two of
    // them clobber rather than compose, so "how many" is the property to pin.
    const std::vector<std::string> panels = {
        "ui_xml/motion_panel.xml",
        "ui_xml/controls_panel.xml",
        "ui_xml/micro/controls_panel.xml",
    };

    for (const auto& path : panels) {
        INFO("panel file: " << path);
        const std::string src = read_file(path);
        for (const char* button : kLevelingButtons) {
            INFO("button: " << button);
            CHECK(disabled_state_binds(button_block(src, button)) == 1);
        }
    }
}
