// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_widget_modal_wiring.cpp
 * @brief What the Filament Sensor tile's tap modal actually wires, per print state.
 *
 * Run with: ./build/bin/helix-tests "[filament][widget_tap][wiring]"
 *
 * test_filament_widget_tap_policy.cpp pins WHERE a tap goes. This file pins what
 * the dialog does once it is up, because the two are only correct together and
 * the seam between them is invisible at the UI:
 *
 *   decide_tap_destination() sends paused (print_state_enum == 2) to ModalFull.
 *   At that state runout_guidance_modal.xml hides the Close row and shows the
 *   Cancel Print / Resume Print row instead. RunoutGuidanceModal::on_cancel()
 *   and on_tertiary() null-check their callback and then hide() regardless — so
 *   a surface that shows that row without wiring it presents a live "Resume
 *   Print" button that closes the dialog and leaves the print paused. That is
 *   what shipped: three reviews read the diff and none caught it, because
 *   nothing about the rendered dialog distinguishes wired from unwired.
 *
 * Mutation checks, each verified:
 *   - Drop the set_on_resume()/set_on_cancel_print() calls from
 *     FilamentSensorWidget::show_tap_modal() -> "Paused tap modal wires the
 *     buttons its paused row shows" fails.
 *   - Drop the else-branch's Callback{} clears -> "Status-only reshow leaves no
 *     stale action callbacks" fails.
 *   - Send the cancel macro directly instead of confirming -> both cancel cases
 *     fail.
 *   - Put the hide() back in RunoutGuidanceModal::on_tertiary() -> "Declining
 *     the cancel returns to a working guidance dialog" fails, and nothing else
 *     does. That is the whole reason it is asserted here: the regression is
 *     invisible to every other test in the tree.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_sensor_widget_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/filament_sensor_widget.h"
#include "standard_macros.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::FilamentSensorWidget;
using helix::FilamentSensorWidgetTestAccess;

namespace {

class TapModalFixture : public LVGLUITestFixture {
  public:
    TapModalFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // READY, or MoonrakerAPI refuses every gcode with "Klipper is halted"
        // and each dispatch assertion below fails for a reason unrelated to it.
        state().set_klippy_state_sync(helix::KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        mock_api = std::make_unique<MoonrakerAPI>(mock_client, state());

        previous_api_ = get_moonraker_api();
        set_moonraker_api(mock_api.get());

        // No AMS backend: the tile is only ever shown on printers without one,
        // and it keeps the dispatches on the macro/raw-gcode tiers where the
        // mock's gcode history can see them.
        helix::AmsState::instance().clear_backends();

        widget.attach(test_screen(), test_screen());
    }

    ~TapModalFixture() override {
        widget.detach();
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        mock_api.reset();
    }

    void set_print_state(helix::PrintJobState s) {
        lv_subject_set_int(state().print_state().get_print_state_enum_subject(),
                           static_cast<int>(s));
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Give StandardMacros a real CANCEL_PRINT to resolve, so the Cancel path
    /// reaches a dispatch rather than refusing on an empty slot.
    void configure_cancel_macro() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro CANCEL_PRINT"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::Cancel).is_empty());
    }

    [[nodiscard]] bool gcode_sent_containing(const std::string& needle) const {
        for (const auto& script : mock_client.gcode_script_history()) {
            if (script.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    RunoutGuidanceModal& modal() {
        return FilamentSensorWidgetTestAccess::tap_modal(widget);
    }
    void show_tap_modal(bool status_only) {
        FilamentSensorWidgetTestAccess::show_tap_modal(widget, status_only);
    }

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> mock_api;
    FilamentSensorWidget widget;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(TapModalFixture, "Paused tap modal wires the buttons its paused row shows",
                 "[filament][widget_tap][wiring]") {
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);

    REQUIRE(modal().is_visible());
    lv_obj_t* dialog = modal().dialog();
    REQUIRE(dialog != nullptr);

    lv_obj_t* resume = lv_obj_find_by_name(dialog, "btn_resume");
    lv_obj_t* cancel = lv_obj_find_by_name(dialog, "btn_cancel_print");
    REQUIRE(resume != nullptr);
    REQUIRE(cancel != nullptr);

    // Half the claim: at print_state_enum == 2 the XML really does show this
    // row, so the buttons are reachable. Without this the wiring assertions
    // below would pass just as happily against a row nobody can press.
    for (lv_obj_t* o = resume; o != dialog; o = lv_obj_get_parent(o)) {
        CHECK_FALSE(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN));
    }

    // The other half: reachable buttons are wired ones.
    CHECK(modal().has_resume_handler());
    CHECK(modal().has_cancel_print_handler());
}

TEST_CASE_METHOD(TapModalFixture,
                 "Cancel Print dispatches nothing until the confirmation is accepted",
                 "[filament][widget_tap][wiring]") {
    // Cancelling a print is destructive and unrecoverable, and this button sits
    // in a dialog whose every other button is harmless — exactly the shape a
    // misplaced tap ruins a print with. dispatch_cancel_print() therefore raises
    // the same confirmation the print-status panel's Stop button does, and both
    // surfaces that call it inherit it.
    configure_cancel_macro();
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);

    lv_obj_t* guidance = modal().dialog();
    REQUIRE(guidance != nullptr);
    lv_obj_t* cancel = lv_obj_find_by_name(guidance, "btn_cancel_print");
    REQUIRE(cancel != nullptr);

    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // The press alone must reach the printer with nothing.
    CHECK_FALSE(gcode_sent_containing("CANCEL_PRINT"));

    // A second, distinct modal is now on top of the stack.
    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm != guidance);

    lv_obj_t* btn_primary = lv_obj_find_by_name(confirm, "btn_primary");
    REQUIRE(btn_primary != nullptr);

    lv_obj_send_event(btn_primary, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // ...and only accepting sends it.
    CHECK(gcode_sent_containing("CANCEL_PRINT"));
}

TEST_CASE_METHOD(TapModalFixture, "Declining the cancel returns to a working guidance dialog",
                 "[filament][widget_tap][wiring]") {
    // The regression this pins: on_tertiary() used to hide unconditionally, so
    // the dialog was already exiting by the time the confirmation appeared and
    // declining left a bare screen. On the runout path it did not come back —
    // check_and_show_runout_guidance() early-returns on
    // runout_modal_shown_for_pause_ until the print state changes — so a user who
    // reconsidered lost Load/Unload/Purge/Resume for the rest of that pause.
    //
    // "Still there" is not enough on its own: a dialog can survive with every
    // callback expired, which is the bug this file was written for. Assert it
    // still works.
    configure_cancel_macro();
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);

    lv_obj_t* guidance = modal().dialog();
    REQUIRE(guidance != nullptr);
    lv_obj_t* cancel = lv_obj_find_by_name(guidance, "btn_cancel_print");
    REQUIRE(cancel != nullptr);
    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // The guidance dialog is still up, underneath the confirmation.
    CHECK(modal().is_visible());
    CHECK(modal().dialog() == guidance);

    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm != guidance);
    lv_obj_t* btn_secondary = lv_obj_find_by_name(confirm, "btn_secondary");
    REQUIRE(btn_secondary != nullptr);

    lv_obj_send_event(btn_secondary, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(600); // let the confirmation's exit animation finish
    helix::ui::UpdateQueue::instance().drain();

    // Declining sends nothing — if "Keep Printing" also dispatched, confirming
    // would be theatre.
    CHECK_FALSE(gcode_sent_containing("CANCEL_PRINT"));

    // The confirmation is gone and the guidance dialog is the top modal again.
    CHECK(modal().is_visible());
    REQUIRE(Modal::get_top() == guidance);

    // And it still works. Its handlers survived, and a real press on a real
    // button still reaches the printer — proving we returned to a live dialog,
    // not a husk.
    CHECK(modal().has_load_filament_handler());
    CHECK(modal().has_resume_handler());
    CHECK(modal().has_cancel_print_handler());

    lv_obj_t* purge = lv_obj_find_by_name(guidance, "btn_purge");
    REQUIRE(purge != nullptr);
    lv_obj_send_event(purge, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(gcode_sent_containing("G1 E50")); // tier-3 purge fallback, no PURGE macro configured
}

TEST_CASE_METHOD(TapModalFixture, "Accepting the cancel closes the guidance dialog too",
                 "[filament][widget_tap][wiring]") {
    // The other side of moving the hide out of on_tertiary(): the dialog must
    // still go away once the cancel is real, or a confirmed cancel would leave it
    // stranded over a print that is no longer running. The runout handler would
    // self-heal via on_print_state_changed; this tile has no such hook, which is
    // why the confirmed path closes it explicitly.
    configure_cancel_macro();
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);

    lv_obj_t* cancel = lv_obj_find_by_name(modal().dialog(), "btn_cancel_print");
    REQUIRE(cancel != nullptr);
    lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* btn_primary = lv_obj_find_by_name(Modal::get_top(), "btn_primary");
    REQUIRE(btn_primary != nullptr);
    lv_obj_send_event(btn_primary, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(gcode_sent_containing("CANCEL_PRINT"));

    process_lvgl(600);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(modal().is_visible());
    CHECK_FALSE(Modal::any_visible());
}

TEST_CASE_METHOD(TapModalFixture, "Status-only reshow leaves no stale action callbacks",
                 "[filament][widget_tap][wiring]") {
    // The modal is a member and retains callbacks until they are overwritten, so
    // "the status-only branch sets nothing" only holds on the very first show.
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);
    REQUIRE(modal().has_load_filament_handler());
    REQUIRE(modal().has_resume_handler());
    modal().hide();

    set_print_state(helix::PrintJobState::PRINTING);
    show_tap_modal(/*status_only=*/true);

    CHECK_FALSE(modal().has_load_filament_handler());
    CHECK_FALSE(modal().has_unload_filament_handler());
    CHECK_FALSE(modal().has_purge_handler());
    CHECK_FALSE(modal().has_resume_handler());
    CHECK_FALSE(modal().has_cancel_print_handler());
}

TEST_CASE_METHOD(TapModalFixture, "Detach closes an open tap modal",
                 "[filament][widget_tap][wiring]") {
    // A grid rebuild detaches the instance and invalidates every token the
    // dialog holds. Left open, the dialog's buttons would all silently no-op.
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);
    REQUIRE(modal().is_visible());

    widget.detach();
    CHECK_FALSE(modal().is_visible());
}

TEST_CASE_METHOD(TapModalFixture, "Tile taps do not latch the advisory icon on",
                 "[filament][widget_tap][wiring]") {
    // runout_is_advisory is static and global: it outlives every show.
    // The tile sets it to 1; nothing used to set it back, so one tap swapped the
    // warning icon for the neutral one on every later runout dialog, including
    // the one that pops when a print pauses.
    //
    // Read through the class accessor rather than an XML scope lookup: this
    // fixture re-registers every component per test case, minting a fresh scope,
    // while the modal's subject registration is a once-per-process static. The
    // scope lookup would therefore answer null from the second test case on and
    // say nothing about the behaviour under test.
    lv_subject_t* advisory = RunoutGuidanceModal::advisory_subject();
    REQUIRE(advisory != nullptr);

    set_print_state(helix::PrintJobState::STANDBY);
    show_tap_modal(/*status_only=*/false);
    CHECK(lv_subject_get_int(advisory) == 1);
    modal().hide();

    // Any surface showing a REAL runout states the warning form for itself.
    RunoutGuidanceModal real_runout;
    real_runout.set_advisory(false);
    CHECK(lv_subject_get_int(advisory) == 0);
}

TEST_CASE_METHOD(TapModalFixture, "Paused runout dialog gates and rings Resume from its subjects",
                 "[filament][widget_tap][wiring][runout]") {
    set_print_state(helix::PrintJobState::PAUSED);
    show_tap_modal(/*status_only=*/false);
    REQUIRE(modal().is_visible());
    lv_obj_t* resume = lv_obj_find_by_name(modal().dialog(), "btn_resume");
    REQUIRE(resume != nullptr);

    // An auto-feed runout with an empty port must not offer a Resume that fails.
    modal().set_resume_blocked(true);
    CHECK(lv_obj_has_state(resume, LV_STATE_DISABLED));
    modal().set_resume_blocked(false);
    CHECK_FALSE(lv_obj_has_state(resume, LV_STATE_DISABLED));

    // Autofeed makes Resume the recovery action, so it carries the accent ring.
    modal().set_autofeed_capable(true);
    CHECK(lv_obj_get_style_outline_width(resume, LV_PART_MAIN) > 0);
    modal().set_autofeed_capable(false);
    CHECK(lv_obj_get_style_outline_width(resume, LV_PART_MAIN) == 0);
}
