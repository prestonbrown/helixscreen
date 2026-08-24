// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wiring tests for PrintTuneOverlay's z-offset baby-step: which base it adjusts
// from, and which SET_GCODE_OFFSET form it puts on the wire.
//
// The helper choosing between relative and absolute is unit-tested in
// test_z_offset_utils.cpp. These tests pin the WIRING: that the overlay seeds its
// base from the resolved offset (not the raw live one) and hands the helper the
// right arguments. That is the half that regressed, and the half a helper test
// cannot see.
//
// Background: ZMOD zeroes gcode_move's live offset in END_PRINT/CANCEL_PRINT and
// keeps the real one in save_variables.gcode_offsets. An idle relative
// Z_ADJUST would therefore compute from a phantom 0, and ZMOD's
// SET_GCODE_OFFSET override persists whatever it computes -- silently discarding
// the user's stored offset.

#include "ui_nav_manager.h"
#include "ui_print_tune_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <array>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterState;
using nlohmann::json;

namespace {

class PrintTuneZOffsetFixture : public LVGLUITestFixture {
  public:
    PrintTuneZOffsetFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // Production always has the active root panel at panel_stack_[0], and
        // push_overlay() reads panel_stack_.back() to inherit from.
        // LVGLUITestFixture already called NavigationManager::init().
        for (auto& p : root_panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());

        // execute_gcode() silently swallows sends while klippy reads SHUTDOWN,
        // which is the default once the client is connected.
        state().set_klippy_state_sync(KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        capture_api = std::make_unique<MoonrakerAPI>(mock_client, state());
    }

    ~PrintTuneZOffsetFixture() override {
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        capture_api.reset();
        // The overlay is a lazily-created singleton owning XML-scope subjects;
        // drop it so the next test case builds a clean one.
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Bring the overlay up the way ControlsPanel::handle_zoffset_tune() does.
    PrintTuneOverlay& show_overlay() {
        auto& overlay = get_print_tune_overlay();
        overlay.show(lv_screen_active(), capture_api.get(), state());
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
        overlay.on_activate(); // seeds the adjustment base from PrinterState
        helix::ui::UpdateQueue::instance().drain();
        return overlay;
    }

    /// Klipper's live gcode offset, in mm.
    void set_live_offset(double mm) {
        state().update_from_status(
            json{{"gcode_move", json{{"homing_origin", {0.0, 0.0, mm, 0.0}}}}});
    }
    /// ZMOD's stored offset, in mm.
    void set_persisted_offset(double mm) {
        state().update_from_status(json{
            {"save_variables", json{{"variables", json{{"gcode_offsets", json{{"z", mm}}}}}}}});
    }
    void set_printing(bool printing) {
        state().update_from_status(
            json{{"print_stats", json{{"state", printing ? "printing" : "standby"}}}});
    }
    void set_homed(bool homed) {
        state().update_from_status(json{{"toolhead", json{{"homed_axes", homed ? "xyz" : ""}}}});
    }

    const std::string& last_gcode() const {
        return mock_client.last_send_script();
    }

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> capture_api;
    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};
};

} // namespace

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: idle ZMOD baby-step sends absolute Z= from the stored offset",
                 "[ui_integration][zoffset][zmod][regression]") {
    // The data-loss path. Stored -0.150, live zeroed by END_PRINT, printer idle.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2); // 0.010mm
    overlay.handle_z_adjust(-1);     // one step closer to the bed
    helix::ui::UpdateQueue::instance().drain();

    // Relative would have been Z_ADJUST=-0.010, landing on -0.010 and persisting
    // that over the user's -0.150.
    CHECK(last_gcode() == "SET_GCODE_OFFSET Z=-0.160 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: idle ZMOD baby-step moves the persisted subject with it",
                 "[ui_integration][zoffset][zmod]") {
    // Otherwise the Controls row keeps showing the pre-adjust value until ZMOD's
    // save_variables change is broadcast back.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(state().get_persisted_z_offset_subject()) == -160);
    CHECK(lv_subject_get_int(state().get_gcode_z_offset_subject()) == -160);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: mid-print baby-step still sends the relative Z_ADJUST form",
                 "[ui_integration][zoffset][zmod]") {
    // Mid-print START_PRINT has already applied the stored offset, so the live
    // value IS the base and the cheap relative form is correct. This is the
    // guard against the fix changing behavior during a print.
    set_persisted_offset(-0.15);
    set_live_offset(-0.15);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: a non-ZMOD printer keeps the legacy relative behavior",
                 "[ui_integration][zoffset][regression]") {
    // No save_variables ever arrives, so nothing about the emitted gcode changes
    // for the overwhelming majority of printers.
    set_live_offset(-0.15);
    set_printing(false);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(1); // farther from the bed
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=0.010 MOVE=1");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture, "PrintTune: MOVE=1 is omitted when the axes are unhomed",
                 "[ui_integration][zoffset]") {
    // MOVE=1 against an unhomed axis is a Klipper error, and idle-on-ZMOD is
    // exactly when the printer is most likely to be unhomed.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);
    set_homed(false);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z=-0.160");
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: an offset already past the travel guard still steps normally",
                 "[ui_integration][zoffset][1280][regression]") {
    // #1280: the guard used to clamp the ABSOLUTE offset to +/-2mm. A toolhead
    // running +2.5mm was yanked to 2.000 on the first tap -- a 0.5mm nose dive
    // into the print. The guard bounds session travel now, so a step is a step.
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2); // 0.010mm
    overlay.handle_z_adjust(1);      // farther from the bed
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=0.010 MOVE=1");
    CHECK(lv_subject_get_int(state().get_gcode_z_offset_subject()) == 2510);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: stepping toward the bed from a large offset moves one step",
                 "[ui_integration][zoffset][1280][regression]") {
    // The dangerous direction. Under the absolute clamp this produced a
    // -0.500mm jump; it must be -0.010mm.
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(2);
    overlay.handle_z_adjust(-1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(last_gcode() == "SET_GCODE_OFFSET Z_ADJUST=-0.010 MOVE=1");
    CHECK(lv_subject_get_int(state().get_gcode_z_offset_subject()) == 2490);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture, "PrintTune: session travel is still bounded at 2mm",
                 "[ui_integration][zoffset][1280]") {
    // The guard has to survive the fix: 60 x 0.05mm would reach 3mm of travel,
    // and must stop at 2mm past the offset the overlay opened on.
    set_live_offset(2.5);
    set_printing(true);
    set_homed(true);

    auto& overlay = show_overlay();
    overlay.handle_z_step_select(0); // 0.050mm
    for (int i = 0; i < 60; ++i) {
        overlay.handle_z_adjust(-1);
    }
    helix::ui::UpdateQueue::instance().drain();

    // 2.5 - 2.0 = 0.5mm, not 2.5 - 3.0 = -0.5mm.
    CHECK(lv_subject_get_int(state().get_gcode_z_offset_subject()) == 500);
}

TEST_CASE_METHOD(PrintTuneZOffsetFixture,
                 "PrintTune: the displayed offset seeds from the stored value while idle",
                 "[ui_integration][zoffset][zmod][regression]") {
    // Negan's report: opening the tune screen while idle showed 0.000.
    set_persisted_offset(-0.15);
    set_live_offset(0.0);
    set_printing(false);

    show_overlay();

    lv_subject_t* subj = lv_xml_get_subject(nullptr, "tune_z_offset_display");
    REQUIRE(subj != nullptr);
    const char* shown = lv_subject_get_string(subj);
    REQUIRE(shown != nullptr);
    CHECK(std::string(shown).find("0.150") != std::string::npos);
    CHECK(std::string(shown).find("0.000") == std::string::npos);
}
