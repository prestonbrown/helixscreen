// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wiring tests for the Controls panel's "Z-Offset:" row.
//
// The row is what Negan actually looked at: on ZMOD while idle it read +0.000mm
// even though the printer's real offset was stored elsewhere. The selection rule
// itself is unit-tested in test_z_offset_utils.cpp; what these tests pin is that
// the panel is wired to it AND that all three inputs retrigger a reformat --
// a missing observer would leave the row stale in exactly the way that shipped.

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using nlohmann::json;

namespace {

class ControlsZOffsetFixture : public LVGLUITestFixture {
  public:
    ControlsZOffsetFixture() : panel(state(), nullptr) {}

    ~ControlsZOffsetFixture() override {
        if (panel_obj) {
            panel.on_deactivate();
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Build the panel the way production does. register_observers() only runs
    /// from setup(), and the observers no-op until on_activate() sets active_.
    void build_and_activate() {
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    void set_live_offset(double mm) {
        state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
        settle();
    }
    void set_persisted_offset(double mm) {
        state().update_from_status(json{
            {"save_variables", json{{"variables", json{{"gcode_offsets", json{{"z", mm}}}}}}}});
        settle();
    }
    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    /// The string the "Z-Offset:" row renders.
    std::string row_text() {
        lv_subject_t* subj = lv_xml_get_subject(nullptr, "controls_z_offset");
        if (!subj) {
            return {};
        }
        const char* s = lv_subject_get_string(subj);
        return s ? std::string(s) : std::string();
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsZOffsetFixture,
                 "Controls row shows the stored ZMOD offset while idle, not 0.000",
                 "[ui_integration][controls][zoffset][zmod][regression]") {
    // Negan's exact complaint. Live is zeroed by ZMOD's END_PRINT; -0.150 is what
    // the next print will apply.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);

    build_and_activate();

    CHECK(row_text().find("0.150") != std::string::npos);
    CHECK(row_text().find("0.000") == std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture, "Controls row follows the live offset during a print",
                 "[ui_integration][controls][zoffset][zmod]") {
    set_persisted_offset(-0.15);
    set_live_offset(-0.15);
    set_printing(true);
    build_and_activate();

    // A mid-print baby step lands in gcode_move first; the row must track it
    // rather than the not-yet-updated stored value.
    set_live_offset(-0.16);

    CHECK(row_text().find("0.160") != std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture, "Controls row re-picks its source when the print ends",
                 "[ui_integration][controls][zoffset][zmod][regression]") {
    // This is the observer that would be easiest to forget: nothing about the
    // z-offset subjects changes at END_PRINT except print_active going false,
    // yet the row has to switch back to the stored value.
    set_persisted_offset(-0.16);
    set_live_offset(-0.16);
    set_printing(true);
    build_and_activate();
    REQUIRE(row_text().find("0.160") != std::string::npos);

    // END_PRINT: ZMOD zeroes the live offset, print goes idle.
    set_live_offset(0.0);
    set_printing(false);

    CHECK(row_text().find("0.160") != std::string::npos);
    CHECK(row_text().find("0.000") == std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture,
                 "Controls row updates when the stored offset changes underneath it",
                 "[ui_integration][controls][zoffset][zmod]") {
    // Someone adjusting from Fluidd/Mainsail moves save_variables with no
    // gcode_move change while idle.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    build_and_activate();
    REQUIRE(row_text().find("0.150") != std::string::npos);

    set_persisted_offset(-0.075);

    CHECK(row_text().find("0.075") != std::string::npos);
}

TEST_CASE_METHOD(ControlsZOffsetFixture, "Controls row is unchanged on a non-ZMOD printer",
                 "[ui_integration][controls][zoffset][regression]") {
    // No save_variables ever arrives, so the row must keep rendering the live
    // offset in every print state.
    set_live_offset(-0.2);
    set_printing(false);
    build_and_activate();

    CHECK(row_text().find("0.200") != std::string::npos);

    set_live_offset(0.0);
    CHECK(row_text().find("0.000") != std::string::npos);
}
