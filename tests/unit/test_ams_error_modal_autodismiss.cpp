// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_error_modal_autodismiss.cpp
 * @brief Regression tests for #1185 — AMS error modal never auto-dismisses
 *
 * Field report: a "Filament System Error" dialog describing a lane reset that
 * failed at 19:23 was still on screen at 19:39 offering Resume / Eject /
 * Recover, while the print had been recovered elsewhere and was running at 30%.
 * None of the three buttons was correct any more.
 *
 * Two independent triggers must take the dialog down, both on an EDGE:
 *   1. the AMS action leaves ERROR, and
 *   2. the print (re)enters PRINTING — the user may have recovered via the
 *      console, another client, or a macro, a route this panel never sees.
 *
 * Both dismissals are programmatic and must NOT run the user-dismiss callback,
 * which clears backend fault state (redundant gcode) and arms the 3s re-show
 * cooldown (would swallow a genuinely new error).
 */

#include "ui_ams_loading_error_modal.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Mirrors the production lazy registration in ensure_ams_widgets_registered(),
// plus the error-modal component (only that path registers it).
void register_ams_widgets_and_xml_once() {
    static bool done = false;
    if (done) {
        return;
    }
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_loading_error_modal.xml");
    done = true;
}

/// Mock backend that counts clear_fault() calls. The user-dismiss callback is
/// the only thing that should ever reach it from this panel.
class FaultCountingBackend : public AmsBackendMock {
  public:
    explicit FaultCountingBackend(int slots) : AmsBackendMock(slots) {}

    AmsError clear_fault(int slot_index) override {
        ++clear_fault_calls;
        return AmsBackendMock::clear_fault(slot_index);
    }

    int clear_fault_calls = 0;
};

/// XMLTestFixture plus the AmsPanel scaffold every case below needs.
class AmsErrorModalFixture : public XMLTestFixture {
  public:
    /// Drain queued observer callbacks, then let LVGL settle. observe_int_sync
    /// defers handlers onto the UpdateQueue, so both halves are required.
    void pump(int ms = 20) {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(ms);
    }

    /// Install an AFC-flavoured mock backend and bring AmsState up.
    FaultCountingBackend* install_backend() {
        auto mock = std::make_unique<FaultCountingBackend>(4);
        mock->set_afc_mode(true);
        REQUIRE(mock->start().success());
        auto* raw = mock.get();
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        return raw;
    }

    /// Build a real AmsPanel exactly as production does.
    lv_obj_t* build_panel(AmsPanel& panel) {
        register_ams_widgets_and_xml_once();
        panel.init_subjects();
        auto* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
        REQUIRE(obj != nullptr);
        panel.setup(obj, test_screen());
        lv_obj_update_layout(test_screen());
        pump(50);
        return obj;
    }

    void teardown_panel(AmsPanel& panel, lv_obj_t* obj) {
        panel.clear_panel_reference();
        lv_obj_delete(obj);
        pump(20);
        AmsState::instance().set_action(AmsAction::IDLE);
        AmsState::instance().set_backend(nullptr);
        pump(10);
    }

    void set_print_state(helix::PrintJobState s) {
        lv_subject_set_int(state().get_print_state_enum_subject(), static_cast<int>(s));
    }

    /// Drive the real inputs so print_lifecycle is republished alongside the
    /// wire. Only the keep-raw case below needs the two to disagree.
    ///
    /// PHASE FIRST. update_from_status() republishes the lifecycle using
    /// whatever phase is live at that instant, so raising the wire before the
    /// phase publishes a transient Printing on the way to Preparing - which any
    /// edge-into-Printing observer would fire on, making the keep-raw case pass
    /// against a lifecycle reader it is supposed to catch.
    void set_wire_and_phase(helix::PrintJobState s, helix::PrintStartPhase phase) {
        state().set_print_start_state(phase, "", 0);
        pump();
        helix::test::set_wire_state(state(), s);
        pump();
    }
};

} // namespace

// ============================================================================
// Modal level — the suppression flag
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AmsLoadingErrorModal dismiss_silently skips the dismiss callback (#1185)",
                 "[ui_integration][ams][regression][1185]") {
    lv_xml_register_component_from_file("A:ui_xml/ams_loading_error_modal.xml");

    helix::ui::AmsLoadingErrorModal modal;
    int dismiss_calls = 0;
    modal.set_dismiss_callback([&dismiss_calls]() { ++dismiss_calls; });

    // A programmatic dismiss must not run the dismiss callback — that callback
    // sends fault-clearing gcode and arms the re-show cooldown.
    REQUIRE(modal.show(lv_screen_active(), "lane reset failed", []() {}));
    process_lvgl(20);
    REQUIRE(modal.is_visible());

    modal.dismiss_silently();
    process_lvgl(20);
    CHECK_FALSE(modal.is_visible());
    CHECK(dismiss_calls == 0);

    // The flag must be one-shot: the next USER dismissal still fires.
    REQUIRE(modal.show(lv_screen_active(), "lane reset failed again", []() {}));
    process_lvgl(20);
    REQUIRE(modal.is_visible());

    modal.hide();
    process_lvgl(20);
    CHECK_FALSE(modal.is_visible());
    CHECK(dismiss_calls == 1);

    // Dismissing an already-hidden modal is a no-op and must not arm the flag
    // for a later real dismissal.
    modal.dismiss_silently();
    process_lvgl(10);
    REQUIRE(modal.show(lv_screen_active(), "third error", []() {}));
    process_lvgl(20);
    modal.hide();
    process_lvgl(20);
    CHECK(dismiss_calls == 2);
}

// ============================================================================
// Panel level — the two dismissal triggers
// ============================================================================

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel shows the error modal when the action enters ERROR",
                 "[ui_integration][ams][regression][1185]") {
    // Baseline for every case below: without this, "the modal went away"
    // assertions would pass vacuously.
    install_backend();

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    CHECK_FALSE(panel.is_error_modal_visible());

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();

    CHECK(panel.is_error_modal_visible());

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture, "AmsPanel dismisses the error modal when AMS leaves ERROR",
                 "[ui_integration][ams][regression][1185]") {
    // THE #1185 REGRESSION. Before the fix the observer's non-ERROR branch only
    // reset the cooldown, so the dialog stayed on screen forever.
    auto* backend = install_backend();

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();
    REQUIRE(panel.is_error_modal_visible());

    AmsState::instance().set_action(AmsAction::IDLE);
    pump();

    CHECK_FALSE(panel.is_error_modal_visible());
    // Programmatic dismiss: the backend must not be told to clear a fault that
    // already cleared itself.
    CHECK(backend->clear_fault_calls == 0);

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel auto-dismiss leaves the re-show cooldown unarmed (#1185)",
                 "[ui_integration][ams][regression][1185]") {
    // The user-dismiss callback stamps error_modal_dismiss_time_, suppressing
    // any error that arrives within 3s. An automatic dismissal must not stamp
    // it, or a genuinely new fault right after recovery would be swallowed.
    auto* backend = install_backend();
    set_print_state(helix::PrintJobState::PAUSED);

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();
    REQUIRE(panel.is_error_modal_visible());

    // Resume the print — the panel takes the stale dialog down without running
    // the dismiss callback. The AMS action is still ERROR at this point.
    set_print_state(helix::PrintJobState::PRINTING);
    pump();
    REQUIRE_FALSE(panel.is_error_modal_visible());
    CHECK(backend->clear_fault_calls == 0);

    // A fresh ERROR notification immediately afterwards must show the dialog
    // again — it would be suppressed for 3s had the cooldown been armed.
    lv_subject_notify(AmsState::instance().get_ams_action_subject());
    pump();
    CHECK(panel.is_error_modal_visible());

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel dismisses the error modal when a paused print resumes (#1185)",
                 "[ui_integration][ams][regression][1185]") {
    // The field case: an AFC fault pauses the print, the user recovers from the
    // console, the print resumes — but ams_action never left ERROR, so only the
    // print-state edge can take the dialog down. PAUSED -> PRINTING is not an
    // edge under is_active_print_state() (which counts PAUSED as active), which
    // is why the panel tests PRINTING explicitly.
    auto* backend = install_backend();
    set_print_state(helix::PrintJobState::PAUSED);

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();
    REQUIRE(panel.is_error_modal_visible());

    set_print_state(helix::PrintJobState::PRINTING);
    pump();
    CHECK_FALSE(panel.is_error_modal_visible());
    CHECK(backend->clear_fault_calls == 0);

    // Second cycle: the panel must still track the edge after one use. Pause,
    // fault again, resume again.
    set_print_state(helix::PrintJobState::PAUSED);
    pump();
    lv_subject_notify(AmsState::instance().get_ams_action_subject());
    pump();
    REQUIRE(panel.is_error_modal_visible());

    set_print_state(helix::PrintJobState::PRINTING);
    pump();
    CHECK_FALSE(panel.is_error_modal_visible());
    CHECK(backend->clear_fault_calls == 0);

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel keeps an error raised mid-print visible (#1185 edge, not level)",
                 "[ui_integration][ams][regression][1185]") {
    // If trigger 2 is ever rewritten as a level check ("is printing?" instead of
    // "just started printing?"), an error raised while the print is already
    // running would be hidden the moment anything re-notified the print subject.
    install_backend();
    set_print_state(helix::PrintJobState::PRINTING);

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();
    REQUIRE(panel.is_error_modal_visible());

    // Further PRINTING updates are not transitions and must leave it alone.
    lv_subject_notify(state().get_print_state_enum_subject());
    pump();
    CHECK(panel.is_error_modal_visible());

    lv_subject_notify(state().get_print_state_enum_subject());
    pump();
    CHECK(panel.is_error_modal_visible());

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel observers' first tick is not a transition (#1185 sentinel)",
                 "[ui_integration][ams][regression][1185]") {
    // Both observers sync once at registration; observe_int_sync defers that
    // tick onto the UpdateQueue, so it lands AFTER setup() when the panel can
    // actually show a modal. With the AMS already faulted and the print already
    // running, the action tick shows the dialog and the print tick follows
    // immediately — the -1 sentinel is what stops that first tick from being
    // read as an edge into PRINTING and tearing the dialog straight back down.
    install_backend();
    set_print_state(helix::PrintJobState::PRINTING);
    AmsState::instance().set_action(AmsAction::ERROR);

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);
    pump();

    CHECK(panel.is_error_modal_visible());

    teardown_panel(panel, obj);
}

TEST_CASE_METHOD(AmsErrorModalFixture,
                 "AmsPanel's resume trigger reads the wire on purpose (#1185 keep-raw)",
                 "[ui_integration][ams][regression][1185]") {
    // This trigger asks a VALUE question about what the printer reports - "does
    // print_stats say printing again?" - not the capability question
    // job_holds_machine() answers, so it is one of the sites that keeps
    // PrintJobState deliberately.
    //
    // The consequence is observable, which is why it is pinned rather than only
    // commented: during a firmware-side PRINT_START Klipper reports `printing`
    // while a pre-print phase is still live, so print_lifecycle stays Preparing.
    // Reading the lifecycle here would push the dismissal to the END of
    // PRINT_START. Migrating the observer turns this case red instead of
    // shifting the timing silently.
    install_backend();
    set_wire_and_phase(helix::PrintJobState::STANDBY, helix::PrintStartPhase::IDLE);

    AmsPanel panel(state(), &api());
    lv_obj_t* obj = build_panel(panel);

    AmsState::instance().set_action(AmsAction::ERROR);
    pump();
    REQUIRE(panel.is_error_modal_visible());

    set_wire_and_phase(helix::PrintJobState::PRINTING, helix::PrintStartPhase::HOMING);
    REQUIRE(state().get_print_lifecycle() == PrintState::Preparing);

    CHECK_FALSE(panel.is_error_modal_visible());

    set_wire_and_phase(helix::PrintJobState::STANDBY, helix::PrintStartPhase::IDLE);
    teardown_panel(panel, obj);
}
