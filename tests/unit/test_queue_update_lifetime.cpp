// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_queue_update_lifetime.cpp
 * @brief Queued main-thread callbacks must not outlive the object they mutate
 *
 * A `queue_update` lambda holding a raw `this` runs against freed memory if the
 * owner dies before the drain — and if the body touches a member `lv_subject_t`,
 * `lv_subject_notify` then walks a freed observer list. The SIGSEGV lands on
 * whichever unrelated test drained next, which is what made #1146 expensive to
 * diagnose. The repair (#1165) is a generation guard on the producing side.
 *
 * These cases pin the guard's observable contract: once the owner has torn down
 * its subjects (or been destroyed), a callback still sitting in the queue is
 * dropped rather than applied.
 */

#include "ui_panel_belt_tension.h"
#include "ui_panel_input_shaper.h"
#include "ui_spool_wizard.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_capabilities_state.h"
#include "printer_plugin_status_state.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "static_subject_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// ============================================================================
// PrinterPrintState — setters defer their subject writes
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterPrintState drops queued setter callbacks once subjects are deinited",
                 "[print_state][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterPrintState print_state;
    print_state.init_subjects(false);

    // Queued, not yet applied.
    print_state.set_print_layer_total(42);
    REQUIRE(UpdateQueue::instance().pending_count() > 0);
    REQUIRE(lv_subject_get_int(print_state.get_print_layer_total_subject()) == 0);

    // Tear the subjects down and stand them back up, exactly as the test
    // isolation listener and reconnect paths do. The in-flight callback now
    // refers to a subject that has been deinited underneath it.
    print_state.deinit_subjects();
    print_state.init_subjects(false);

    UpdateQueue::instance().drain();

    // Without the generation guard the drain writes 42 into the reborn subject;
    // with it, the stale callback is skipped.
    CHECK(lv_subject_get_int(print_state.get_print_layer_total_subject()) == 0);
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterPrintState queued callbacks survive destruction of the owner",
                 "[print_state][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    {
        PrinterPrintState print_state;
        print_state.init_subjects(false);

        // Every deferring setter, so a regression on any one of them is caught.
        print_state.set_print_layer_total(9);
        print_state.set_print_layer_heights(0.2, 0.3);
        print_state.set_print_layer_current(3);
        print_state.set_print_start_state(PrintStartPhase::HEATING_BED, "heating", 50);
        print_state.reset_print_start_state();
        print_state.set_print_in_progress(true);
        print_state.set_estimated_print_time(600);

        REQUIRE(UpdateQueue::instance().pending_count() > 0);

        print_state.deinit_subjects();
    } // destroyed with callbacks still queued

    // Pre-fix this drains seven lambdas into freed member subjects. The
    // assertion below only proves the queue emptied; the use-after-free itself
    // is what an ASAN build of this case reports.
    UpdateQueue::instance().drain();
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

// ============================================================================
// SpoolWizardOverlay — Spoolman responses land after the overlay is dismissed
// ============================================================================

namespace {

/// Installs a mock API as the process-wide one for the duration of the scope,
/// restoring whatever was there before. SpoolWizardOverlay reaches the API
/// through get_moonraker_api() rather than an injected pointer.
class ScopedGlobalApi {
  public:
    explicit ScopedGlobalApi(MoonrakerAPI* api) : previous_(get_moonraker_api()) {
        set_moonraker_api(api);
    }
    ~ScopedGlobalApi() {
        set_moonraker_api(previous_);
    }
    ScopedGlobalApi(const ScopedGlobalApi&) = delete;
    ScopedGlobalApi& operator=(const ScopedGlobalApi&) = delete;

  private:
    IMoonrakerAPI* previous_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SpoolWizard vendor load is dropped when the overlay closes",
                 "[spool_wizard][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    ScopedGlobalApi scoped_api(&api);

    UpdateQueue::instance().drain();

    SpoolWizardOverlay wizard;
    wizard.on_activate();

    // The mock answers both vendor fetches synchronously, so the merge/apply
    // step is queued by the time load_vendors() returns.
    wizard.load_vendors();
    REQUIRE(UpdateQueue::instance().pending_count() > 0);
    REQUIRE(wizard.all_vendors().empty());

    // User backs out of the wizard before the response is applied.
    wizard.on_deactivate();

    UpdateQueue::instance().drain();

    // Pre-fix the deferred body still ran and repopulated all_vendors_ against
    // an overlay that is no longer live.
    CHECK(wizard.all_vendors().empty());
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "SpoolWizard vendor load applies while the overlay is live",
                 "[spool_wizard][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    ScopedGlobalApi scoped_api(&api);

    UpdateQueue::instance().drain();

    SpoolWizardOverlay wizard;
    wizard.on_activate();

    wizard.load_vendors();
    UpdateQueue::instance().drain();

    // Counterpart to the case above: the guard must not swallow the callback
    // when the overlay is still active, or the drop test would pass vacuously.
    CHECK_FALSE(wizard.all_vendors().empty());
}

// ============================================================================
// PrinterCapabilitiesState — async capability setters
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterCapabilitiesState drops queued setters once subjects are deinited",
                 "[capabilities][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Every deferring setter on the class, so a regression on any one is caught.
    caps.set_spoolman_available(true);
    caps.set_timelapse_available(true);
    caps.set_webcam_available(true, "http://cam/stream", "http://cam/snap", true, true, 30);
    caps.set_power_device_count(3);
    caps.set_sensor_count(4);

    REQUIRE(UpdateQueue::instance().pending_count() > 0);
    REQUIRE(lv_subject_get_int(caps.get_printer_has_spoolman_subject()) == 0);

    // Tear down and stand back up on the same live object, which is what the
    // test-isolation and reconnect paths do — and the shape a destructor-only
    // hook would never catch.
    caps.deinit_subjects();
    caps.init_subjects(false);

    UpdateQueue::instance().drain();

    // Pre-fix each of these writes into a subject that was deinited underneath
    // it, and lv_subject_notify walks the freed observer list.
    CHECK(lv_subject_get_int(caps.get_printer_has_spoolman_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_timelapse_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_printer_has_webcam_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_power_device_count_subject()) == 0);
    CHECK(lv_subject_get_int(caps.get_sensor_count_subject()) == 0);
    CHECK(caps.get_webcam_stream_url().empty());
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterCapabilitiesState applies setters while subjects are live",
                 "[capabilities][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    caps.set_spoolman_available(true);
    caps.set_power_device_count(3);
    UpdateQueue::instance().drain();

    // Without this the drop case above would pass vacuously — a guard that
    // swallowed everything would satisfy it just as well.
    CHECK(lv_subject_get_int(caps.get_printer_has_spoolman_subject()) == 1);
    CHECK(lv_subject_get_int(caps.get_power_device_count_subject()) == 3);
}

// ============================================================================
// PrinterState — setters that defer aggregate recomputation
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "PrinterState drops queued setters once subjects are deinited",
                 "[printer_state][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterState state;
    state.init_subjects(false);

    REQUIRE_FALSE(state.service_has_helix_plugin());

    state.set_helix_plugin_installed(true);
    state.set_timelapse_available(true);
    state.set_timelapse_default_enabled(true);

    REQUIRE(UpdateQueue::instance().pending_count() > 0);
    REQUIRE_FALSE(state.service_has_helix_plugin());

    state.deinit_subjects();
    state.init_subjects(false);

    UpdateQueue::instance().drain();

    // The plugin flag is the readable proxy: the queued body calls
    // plugin_status_state_.set_installed(true) plus the aggregate recompute,
    // all against subjects that no longer exist in the generation that queued it.
    CHECK_FALSE(state.service_has_helix_plugin());
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrinterState applies setters while subjects are live",
                 "[printer_state][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterState state;
    state.init_subjects(false);

    state.set_helix_plugin_installed(true);
    UpdateQueue::instance().drain();

    CHECK(state.service_has_helix_plugin());
}

// ============================================================================
// MoonrakerAPI — the literal #1146 culprit
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "MoonrakerAPI build-volume notify survives destruction",
                 "[moonraker_api][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    MoonrakerClientMock client;
    PrinterState state;

    {
        MoonrakerAPI api(client, state);
        api.notify_build_volume_changed();
        REQUIRE(UpdateQueue::instance().pending_count() > 0);
    } // ~MoonrakerAPI invalidates the guard, then lv_subject_deinit()s the subject

    // Honest limits: this asserts only that the drain completed and emptied the
    // queue. Pre-fix the queued lambda called lv_subject_set_int() on the freed
    // build_volume_version_, which is UB that does not reliably detonate in a
    // normal build — an ASAN run of this case is what reports the use-after-free.
    // #1146's SIGSEGV landed on whichever unrelated test drained next, not here.
    UpdateQueue::instance().drain();
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "MoonrakerAPI build-volume notify applies while the API is alive",
                 "[moonraker_api][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    MoonrakerClientMock client;
    PrinterState state;
    MoonrakerAPI api(client, state);

    REQUIRE(lv_subject_get_int(api.get_build_volume_version_subject()) == 0);

    api.notify_build_volume_changed();
    UpdateQueue::instance().drain();

    // The guard must not swallow the notification on a live API, or observers
    // of the build volume would silently stop updating.
    CHECK(lv_subject_get_int(api.get_build_volume_version_subject()) == 1);
}

// ============================================================================
// Calibration panels — async results applied after the panel is dismissed
//
// Both panels are process-lifetime singletons (get_global_*_panel()), so the
// exposure is not freed memory: it is a dismissed panel's subjects being
// written and repainted from a reply that arrived too late. The tests use the
// real global instances, because their subjects are what the XML-registered
// names resolve to.
// ============================================================================

namespace {

/// Wires a global calibration panel to a stack-allocated mock and unwires it on
/// scope exit. Without this the singleton keeps a dangling MoonrakerAPI* after
/// the test returns, and the next test to open the panel dereferences it.
/// Declare after the mocks it borrows so it is destroyed first.
template <typename Panel> class ScopedPanelApi {
  public:
    ScopedPanelApi(Panel& panel, helix::MoonrakerClient* client, MoonrakerAPI* api)
        : panel_(panel) {
        panel_.set_api(client, api);
    }
    ~ScopedPanelApi() {
        panel_.on_deactivate();
        panel_.set_api(nullptr, nullptr);
    }
    ScopedPanelApi(const ScopedPanelApi&) = delete;
    ScopedPanelApi& operator=(const ScopedPanelApi&) = delete;

  private:
    Panel& panel_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "BeltTensionPanel drops hardware detection once subjects are deinited",
                 "[belt_tension][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& panel = get_global_belt_tension_panel();
    panel.deinit_subjects(); // Known-clean subject state regardless of shard order
    panel.init_subjects();
    ScopedPanelApi<BeltTensionPanel> wired(panel, &client, &api);

    UpdateQueue::instance().drain();

    panel.on_activate();

    // The calibrator marshals its own reply through the queue, so the first
    // drain is what hands the hardware to the panel — which then queues its
    // own apply. process_pending() swaps the queue once, so work enqueued
    // during a drain lands in the next one.
    UpdateQueue::instance().drain();
    REQUIRE(UpdateQueue::instance().pending_count() > 0);

    // Tear the subjects down and stand them back up on the same live panel —
    // what StaticSubjectRegistry and test isolation do, and the shape no
    // destructor hook would ever catch.
    panel.deinit_subjects();
    panel.init_subjects();

    lv_subject_t* adxl = lv_xml_get_subject(nullptr, "bt_hw_adxl");
    REQUIRE(adxl != nullptr);
    const std::string before = lv_subject_get_string(adxl);

    UpdateQueue::instance().drain();

    // Pre-fix the queued apply wrote the detection result into a subject that
    // had been deinited and re-inited underneath it.
    CHECK(std::string(lv_subject_get_string(adxl)) == before);
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "BeltTensionPanel applies hardware detection while subjects are live",
                 "[belt_tension][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& panel = get_global_belt_tension_panel();
    panel.deinit_subjects();
    panel.init_subjects();
    ScopedPanelApi<BeltTensionPanel> wired(panel, &client, &api);

    UpdateQueue::instance().drain();

    lv_subject_t* adxl = lv_xml_get_subject(nullptr, "bt_hw_adxl");
    REQUIRE(adxl != nullptr);
    const std::string before = lv_subject_get_string(adxl);

    panel.on_activate();
    UpdateQueue::instance().drain(); // calibrator hop
    UpdateQueue::instance().drain(); // panel apply

    // Counterpart to the case above: the guard must not swallow the result on a
    // live panel, or the drop test would pass vacuously. Both the success and
    // the Klippy-not-ready error branch overwrite the "Detecting..." default,
    // so this holds whatever the mock's klippy state happens to be.
    CHECK(std::string(lv_subject_get_string(adxl)) != before);
}

TEST_CASE_METHOD(LVGLTestFixture, "InputShaperPanel drops the config query once the overlay closes",
                 "[input_shaper][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& panel = get_global_input_shaper_panel();
    panel.init_subjects();
    ScopedPanelApi<InputShaperPanel> wired(panel, &client, &api);

    UpdateQueue::instance().drain();

    lv_subject_t* configured = lv_xml_get_subject(nullptr, "is_shaper_configured");
    REQUIRE(configured != nullptr);

    // Establish the "configured" state through the panel itself rather than
    // assuming what an earlier test left behind.
    client.set_input_shaper_configured(true);
    panel.on_activate();
    UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(configured) == 1);

    // Now queue the opposite answer and back out before it is applied.
    client.set_input_shaper_configured(false);
    panel.on_activate();
    REQUIRE(UpdateQueue::instance().pending_count() > 0);

    panel.on_deactivate(); // OverlayBase::on_deactivate() invalidates lifetime_

    UpdateQueue::instance().drain();

    // Pre-fix the unconfigured result repainted a panel the user had left.
    CHECK(lv_subject_get_int(configured) == 1);
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "InputShaperPanel applies the config query while the overlay is live",
                 "[input_shaper][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& panel = get_global_input_shaper_panel();
    panel.init_subjects();
    ScopedPanelApi<InputShaperPanel> wired(panel, &client, &api);

    UpdateQueue::instance().drain();

    lv_subject_t* configured = lv_xml_get_subject(nullptr, "is_shaper_configured");
    REQUIRE(configured != nullptr);

    client.set_input_shaper_configured(true);
    panel.on_activate();
    UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(configured) == 1);

    // The guard must not swallow an answer that arrives while the panel is up.
    client.set_input_shaper_configured(false);
    panel.on_activate();
    UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(configured) == 0);
}

// ============================================================================
// PrinterPluginStatusState — a sub-component with its own guard
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterPluginStatusState drops the phase-tracking setter after deinit",
                 "[plugin_status][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterPluginStatusState plugin_status;
    plugin_status.init_subjects(false);

    REQUIRE_FALSE(plugin_status.is_phase_tracking_enabled());

    plugin_status.set_phase_tracking_enabled(true);
    REQUIRE(UpdateQueue::instance().pending_count() > 0);
    REQUIRE_FALSE(plugin_status.is_phase_tracking_enabled());

    // The #1146 shape: subjects torn down and re-inited on a LIVE object, so the
    // destructor is never the hook that saves us. The generation must advance in
    // deinit_subjects() or the queued body writes the re-inited subject.
    plugin_status.deinit_subjects();
    plugin_status.init_subjects(false);

    UpdateQueue::instance().drain();

    CHECK_FALSE(plugin_status.is_phase_tracking_enabled());
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrinterPluginStatusState applies the phase-tracking setter while live",
                 "[plugin_status][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    PrinterPluginStatusState plugin_status;
    plugin_status.init_subjects(false);

    plugin_status.set_phase_tracking_enabled(true);
    UpdateQueue::instance().drain();

    // The guard must not swallow a setter issued against the live generation.
    CHECK(plugin_status.is_phase_tracking_enabled());
}

// ============================================================================
// AmsState — deferred slot setters
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "AmsState drops the pending-slot setter after deinit",
                 "[ams][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    lv_subject_t* pending = ams.get_pending_target_slot_subject();
    REQUIRE(pending != nullptr);
    lv_subject_set_int(pending, 0);

    ams.set_pending_target_slot(7);
    REQUIRE(UpdateQueue::instance().pending_count() > 0);

    ams.deinit_subjects();
    ams.init_subjects(false);

    UpdateQueue::instance().drain();

    // Re-read through the accessor: init_subjects() rebuilt the subject, and the
    // callback queued against the previous generation must not have written it.
    CHECK(lv_subject_get_int(ams.get_pending_target_slot_subject()) != 7);
    CHECK(UpdateQueue::instance().pending_count() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsState applies the pending-slot setter while live",
                 "[ams][lifetime][queue_update]") {
    UpdateQueue::instance().drain();

    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    ams.set_pending_target_slot(5);
    UpdateQueue::instance().drain();

    CHECK(lv_subject_get_int(ams.get_pending_target_slot_subject()) == 5);
}

// ============================================================================
// InputShaperPanel — subject teardown joins the ordered shutdown pass (#1180)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "InputShaperPanel registers its subject teardown with StaticSubjectRegistry",
                 "[input_shaper][lifetime][queue_update]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& panel = get_global_input_shaper_panel();
    panel.init_subjects();
    ScopedPanelApi<InputShaperPanel> wired(panel, &client, &api);

    UpdateQueue::instance().drain();

    lv_subject_t* configured = lv_xml_get_subject(nullptr, "is_shaper_configured");
    REQUIRE(configured != nullptr);

    // Settle on the NOT-configured answer, so the stale reply queued below is the
    // opposite value and a leaked write is distinguishable from the default.
    client.set_input_shaper_configured(false);
    panel.on_activate();
    UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(configured) == 0);

    // Queue the "configured" reply, then run the registered teardown rather than
    // closing the overlay. Pre-#1180 there was no entry to run at all, which is
    // what the return value below pins.
    client.set_input_shaper_configured(true);
    panel.on_activate();
    REQUIRE(UpdateQueue::instance().pending_count() > 0);

    CHECK(StaticSubjectRegistry::instance().deinit_one("InputShaperPanel"));

    // Stand the subjects back up, as a reconnect or the next test would.
    panel.init_subjects();
    UpdateQueue::instance().drain();

    lv_subject_t* reborn = lv_xml_get_subject(nullptr, "is_shaper_configured");
    REQUIRE(reborn != nullptr);

    // Without the invalidate in deinit_subjects() the stale reply writes 1 into
    // the reborn subject; with it, the callback is dropped.
    CHECK(lv_subject_get_int(reborn) == 0);
    CHECK(UpdateQueue::instance().pending_count() == 0);
}
