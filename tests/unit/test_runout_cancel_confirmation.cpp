// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_cancel_confirmation.cpp
 * @brief The runout guidance dialog's Cancel Print button confirms first.
 *
 * Run with: ./build/bin/helix-tests "[filament][runout][cancel]"
 *
 * Cancelling a print is destructive and unrecoverable, and Cancel Print sits in
 * a dialog whose every other button is harmless - the shape a misplaced tap
 * ruins a print with. The press raises PrintCancelModal, the same confirmation
 * the print-status panel's Stop button uses, and the macro reaches the printer
 * only if the user accepts.
 *
 * The guidance dialog stays up behind that confirmation, which is a coupled
 * requirement rather than a nicety: RunoutGuidanceModal::on_tertiary() leaves
 * closing to the callback, and only the confirmed path closes. A dialog that
 * closed on the press would strand the user, because
 * check_and_show_runout_guidance() early-returns while
 * runout_modal_shown_for_pause_ is set - it clears only on a transition to
 * Printing/Idle/Complete/Cancelled/Error - so reconsidering a cancel would cost
 * Load, Unload, Purge and Resume for the rest of that pause.
 *
 * These press the real buttons on the real dialogs rather than calling a private
 * dispatcher: the cancel path has no dispatch method of its own, and the wiring
 * between the two dialogs is the whole behaviour under test.
 */

#include "ui_filament_runout_handler.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_runout_handler_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentRunoutHandler;
using helix::ui::FilamentRunoutHandlerTestAccess;

namespace {

class RunoutCancelFixture : public LVGLUITestFixture {
  public:
    RunoutCancelFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        // execute_gcode() refuses unless Klippy is READY, so a macro dispatched
        // without this never reaches the mock and every send assertion is vacuous.
        state().set_klippy_state_sync(helix::KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        mock_api = std::make_unique<MoonrakerAPI>(mock_client, state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(mock_api.get());
    }

    ~RunoutCancelFixture() override {
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        mock_api.reset();
    }

    /// Give StandardMacros a real CANCEL_PRINT to resolve. Without it the button
    /// refuses up front and never reaches the confirmation at all.
    void configure_cancel_macro() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro CANCEL_PRINT"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::Cancel).is_empty());
    }

    /// execute_gcode() annotates the script, so match on a substring.
    [[nodiscard]] bool cancel_sent() const {
        for (const auto& script : mock_client.gcode_script_history()) {
            if (script.find("CANCEL_PRINT") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// Raise the guidance dialog and press its Cancel Print button, returning the
    /// confirmation that comes up on top.
    lv_obj_t* press_cancel_print(FilamentRunoutHandler& handler) {
        FilamentRunoutHandlerTestAccess::show_guidance_modal(handler);
        lv_obj_t* guidance = FilamentRunoutHandlerTestAccess::guidance_modal(handler).dialog();
        REQUIRE(guidance != nullptr);

        lv_obj_t* cancel = lv_obj_find_by_name(guidance, "btn_cancel_print");
        REQUIRE(cancel != nullptr);
        lv_obj_send_event(cancel, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        return guidance;
    }

    MoonrakerClientMock mock_client;
    std::unique_ptr<MoonrakerAPI> mock_api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(RunoutCancelFixture, "Runout Cancel Print sends nothing on the press alone",
                 "[filament][runout][cancel]") {
    configure_cancel_macro();
    FilamentRunoutHandler handler(mock_api.get());

    lv_obj_t* guidance = press_cancel_print(handler);

    // One tap must not reach the printer.
    CHECK_FALSE(cancel_sent());

    // A second, distinct dialog is now on top asking the question.
    lv_obj_t* confirm = Modal::get_top();
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm != guidance);
    CHECK(lv_obj_find_by_name(confirm, "btn_primary") != nullptr);
    CHECK(lv_obj_find_by_name(confirm, "btn_secondary") != nullptr);
}

TEST_CASE_METHOD(RunoutCancelFixture, "Accepting the runout cancel sends the macro and closes both",
                 "[filament][runout][cancel]") {
    configure_cancel_macro();
    FilamentRunoutHandler handler(mock_api.get());

    press_cancel_print(handler);

    lv_obj_t* btn_primary = lv_obj_find_by_name(Modal::get_top(), "btn_primary");
    REQUIRE(btn_primary != nullptr);
    lv_obj_send_event(btn_primary, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    // Only accepting sends it.
    CHECK(cancel_sent());

    // And the guidance dialog goes with it - leaving it up would strand it over
    // a print that is no longer running.
    process_lvgl(600); // let both exit animations finish
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(FilamentRunoutHandlerTestAccess::guidance_modal(handler).is_visible());
}

TEST_CASE_METHOD(RunoutCancelFixture, "Declining the runout cancel returns to a working dialog",
                 "[filament][runout][cancel]") {
    configure_cancel_macro();
    FilamentRunoutHandler handler(mock_api.get());

    lv_obj_t* guidance = press_cancel_print(handler);

    // The guidance dialog is still up, underneath the confirmation. This is the
    // half that on_tertiary()'s hide would break.
    CHECK(FilamentRunoutHandlerTestAccess::guidance_modal(handler).is_visible());

    lv_obj_t* btn_secondary = lv_obj_find_by_name(Modal::get_top(), "btn_secondary");
    REQUIRE(btn_secondary != nullptr);
    lv_obj_send_event(btn_secondary, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(600); // let the confirmation's exit animation finish
    helix::ui::UpdateQueue::instance().drain();

    // Declining sends nothing - if "Keep Printing" also dispatched, confirming
    // would be theatre.
    CHECK_FALSE(cancel_sent());

    // The user is back on the guidance dialog, and it is the top modal again.
    CHECK(FilamentRunoutHandlerTestAccess::guidance_modal(handler).is_visible());
    CHECK(Modal::get_top() == guidance);

    // "Still on screen" is not enough: a dialog can survive with every callback
    // dead, which is the failure this whole file guards. Press Cancel Print again
    // and require the confirmation comes back - the dialog is live, not a husk.
    lv_obj_t* cancel_again = lv_obj_find_by_name(guidance, "btn_cancel_print");
    REQUIRE(cancel_again != nullptr);
    lv_obj_send_event(cancel_again, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    lv_obj_t* second_confirm = Modal::get_top();
    REQUIRE(second_confirm != nullptr);
    CHECK(second_confirm != guidance);
    CHECK(lv_obj_find_by_name(second_confirm, "btn_primary") != nullptr);
    CHECK_FALSE(cancel_sent());
}
