// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_toolchange_dwell.cpp
 * @brief Telling an AMS tool change apart from a runout.
 *
 * Run with: ./build/bin/helix-tests "[runout][dwell]"
 *
 * A tool change drags filament off the toolhead sensor and feeds the next lane
 * past it. At the sensor edge that is byte-for-byte a runout, so no amount of
 * inspecting the edge can separate them. Only duration can: a swap leaves the
 * sensor clear for tens of seconds and then refills, a runout never refills.
 *
 * So the removal toast serves helix::RUNOUT_TOAST_DWELL before it
 * fires, and a refill inside the dwell cancels it. The dwell is scoped to "a job
 * holds the machine AND an AMS backend is present", because that is the only
 * situation in which a tool change can be the cause.
 *
 * Three things have to hold at once, and the second is the one that makes this
 * worth testing at all:
 *
 *   1. A tool change announces NOTHING - not the removal, and not the insertion
 *      that follows it, since a removal nobody was told about needs no all-clear.
 *   2. A removal that outlasts the dwell IS announced. A suppression that also
 *      swallows the genuine runout has made the printer quieter, not better.
 *   3. Printers the dwell does not cover keep the immediate edge toast.
 *
 * Toasts are observed through the notification hooks in tests/ui_test_utils.h;
 * the production notification layer is replaced by log-only stubs in the test
 * binary, so asserting against anything else would pass vacuously.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "printer_state.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

constexpr const char* HEAD_SENSOR = "filament_switch_sensor head_switch_sensor";

/// Substrings of the two toasts. The full text is prefixed with the role's
/// display name, so match on the invariant half.
constexpr const char* REMOVED_WARNING = "Filament removed";
constexpr const char* INSERTED_INFO = "Filament inserted";

nlohmann::json head_status(bool detected) {
    return nlohmann::json{{HEAD_SENSOR, {{"filament_detected", detected}, {"enabled", true}}}};
}

/// A backend that reports whichever AmsType the test needs. The dwell asks only
/// "is there a backend, and what type", so nothing else has to be faithful.
class TypedBackend : public AmsBackendMock {
  public:
    explicit TypedBackend(AmsType type) : AmsBackendMock(4), type_(type) {}

    AmsType get_type() const override {
        return type_;
    }

  private:
    AmsType type_;
};

/// RAII capture of both toast severities. Severity is kept apart on purpose: the
/// removal is a WARNING and the insertion an INFO, so a suppression that caught
/// the wrong one fails on severity as well as on text.
class ToastCapture {
  public:
    ToastCapture() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings_.push_back(msg); });
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& msg) { infos_.push_back(msg); });
    }

    ~ToastCapture() {
        helix::ui::set_test_notification_warning_hook(nullptr);
        helix::ui::set_test_notification_info_hook(nullptr);
    }

    ToastCapture(const ToastCapture&) = delete;
    ToastCapture& operator=(const ToastCapture&) = delete;

    [[nodiscard]] int warnings_containing(const std::string& needle) const {
        return count(warnings_, needle);
    }

    [[nodiscard]] int infos_containing(const std::string& needle) const {
        return count(infos_, needle);
    }

  private:
    static int count(const std::vector<std::string>& msgs, const std::string& needle) {
        int n = 0;
        for (const auto& m : msgs) {
            if (m.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    std::vector<std::string> warnings_;
    std::vector<std::string> infos_;
};

/// A real sensor edge on the real manager, mid-print, with a real backend
/// installed. The manager is a process singleton, so the fixture has to start
/// from a known sensor set with the startup grace already expired - otherwise
/// whichever test ran first in the shard decides the answer.
class DwellFixture : public LVGLTestFixture {
  public:
    DwellFixture() {
        get_printer_state().init_subjects(false);
        AmsState::instance().init_subjects(false);
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({HEAD_SENSOR});
        fsm.set_sensor_role(HEAD_SENSOR, FilamentSensorRole::RUNOUT);

        // Filament present, and the startup grace spent, before anything is
        // asserted. discover_sensors() re-anchors that grace, so it has to be
        // cleared after the sensors are installed, not before.
        fsm.update_from_status(head_status(true));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(fsm.is_in_startup_grace_period());

        // None of the OTHER suppression terms may be standing, or a green test
        // would prove nothing about the dwell.
        REQUIRE_FALSE(is_wizard_active());
        REQUIRE_FALSE(AmsState::instance().is_filament_operation_active());
        REQUIRE_FALSE(AmsState::instance().post_unload_runout_grace_armed());
    }

    ~DwellFixture() override {
        AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Install a backend and start a print: both halves of the dwell's scope.
    static void printing_with(AmsType type) {
        AmsState::instance().add_backend(std::make_unique<TypedBackend>(type));
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(job_holds_machine(get_printer_state().get_print_lifecycle()));
    }

    static void sensor(bool detected) {
        FilamentSensorManager::instance().update_from_status(head_status(detected));
        helix::ui::UpdateQueue::instance().drain();
    }

    /// A status payload carrying no sensor key, which is what Moonraker sends
    /// most of the time. The dwell is swept on every payload, so this is how a
    /// dwell that has come due gets noticed.
    static void tick() {
        FilamentSensorManager::instance().update_from_status(nlohmann::json::object());
        helix::ui::UpdateQueue::instance().drain();
    }

    static void age_past_dwell() {
        PostUnloadGraceTestAccess::age_removal_dwell(
            FilamentSensorManager::instance(), helix::RUNOUT_TOAST_DWELL + std::chrono::seconds(1));
    }
};

} // namespace

TEST_CASE_METHOD(DwellFixture, "A tool change inside the dwell announces nothing at all",
                 "[runout][dwell][sensors]") {
    printing_with(AmsType::AD5X_IFS);

    ToastCapture toasts;

    // The swap pulls filament off the head sensor...
    sensor(false);
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);
    CHECK(PostUnloadGraceTestAccess::removal_dwell_pending(FilamentSensorManager::instance()));

    // ...and the next lane arrives before the dwell is up.
    sensor(true);

    // Neither half is news. The insertion is suppressed too: an all-clear for a
    // removal nobody was told about is its own false alarm.
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);
    CHECK(toasts.infos_containing(INSERTED_INFO) == 0);
    CHECK_FALSE(
        PostUnloadGraceTestAccess::removal_dwell_pending(FilamentSensorManager::instance()));

    // And the dwell is spent, not merely quiet - a later tick cannot resurrect it.
    age_past_dwell();
    tick();
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);
}

TEST_CASE_METHOD(DwellFixture, "A removal that outlasts the dwell is announced once",
                 "[runout][dwell][sensors]") {
    printing_with(AmsType::AD5X_IFS);

    ToastCapture toasts;
    sensor(false);
    REQUIRE(toasts.warnings_containing(REMOVED_WARNING) == 0);

    // Filament never comes back. This is the event the toast exists to report,
    // and it has to survive the suppression that quiets the tool change.
    age_past_dwell();
    tick();
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);

    // Exactly once. The record is erased as it fires, so further payloads - of
    // which a print sends many - must not repeat it.
    tick();
    tick();
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
}

TEST_CASE_METHOD(DwellFixture, "With no AMS backend the removal toast still fires on the edge",
                 "[runout][dwell][sensors]") {
    // A printer with no AMS has no tool change to confuse a runout with, so it
    // keeps the immediate warning. This is what scopes the dwell rather than
    // letting it slow every printer down.
    helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(AmsState::instance().get_backend() == nullptr);

    ToastCapture toasts;
    sensor(false);

    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
    CHECK_FALSE(
        PostUnloadGraceTestAccess::removal_dwell_pending(FilamentSensorManager::instance()));
}

TEST_CASE_METHOD(DwellFixture,
                 "With no job holding the machine the removal toast fires on the edge",
                 "[runout][dwell][sensors]") {
    // Idle, so a tool change is not what emptied the sensor. ACE rather than
    // AD5X because AD5X has its own idle auto-unload suppression, which would
    // make this pass for the wrong reason.
    AmsState::instance().add_backend(std::make_unique<TypedBackend>(AmsType::ACE));
    helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(job_holds_machine(get_printer_state().get_print_lifecycle()));

    ToastCapture toasts;
    sensor(false);

    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
}

TEST_CASE_METHOD(DwellFixture, "A dwell outstanding when the job ends is dropped, not deferred",
                 "[runout][dwell][sensors]") {
    printing_with(AmsType::AD5X_IFS);

    ToastCapture toasts;
    sensor(false);
    REQUIRE(PostUnloadGraceTestAccess::removal_dwell_pending(FilamentSensorManager::instance()));

    // The print ends with the sensor still clear. Whatever the head sensor does
    // between prints belongs to the idle-side suppressions - firing a mid-print
    // runout warning after the job is over would be a fresh false alarm, one
    // the dwell itself introduced.
    helix::test::set_wire_state(get_printer_state(), PrintJobState::COMPLETE);
    helix::ui::UpdateQueue::instance().drain();
    age_past_dwell();
    tick();

    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);
    CHECK_FALSE(
        PostUnloadGraceTestAccess::removal_dwell_pending(FilamentSensorManager::instance()));
}
