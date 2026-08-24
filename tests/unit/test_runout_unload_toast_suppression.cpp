// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_unload_toast_suppression.cpp
 * @brief The "Filament removed" warning vs the manual-pull hint.
 *
 * Run with: ./build/bin/helix-tests "[runout][toast]"
 *
 * TWO notifications fire on the same toolhead 1 -> 0 sensor edge, in the same
 * window, and they are NOT the same thing:
 *
 *   1. FilamentSensorManager's NOTIFY_WARNING("{}: Filament removed") — a
 *      warning that the toolhead unexpectedly went empty. After a deliberate
 *      user-initiated unload this is noise: the user is the one who removed it.
 *      `!ams_active` cannot suppress it, because the sensor edge lands seconds
 *      after the action has already returned to IDLE. That is the whole reason
 *      the post-unload grace exists.
 *
 *   2. ui_manual_pull_prompt's NOTIFY_INFO("Filament is clear of the toolhead.
 *      Pull it out the rest of the way.") — a WANTED hint, armed only by an
 *      explicit user unload, that fires exactly once and disarms.
 *
 * Suppressing (1) must not touch (2). These tests drive one real sensor edge
 * through the real FilamentSensorManager with the real prompt armed, so
 * "I suppressed the wrong toast" cannot pass.
 *
 * The third case matters just as much: with no unload anywhere in the picture,
 * the warning must STILL fire. A suppression that also blinds the genuine runout
 * has not fixed anything.
 *
 * Toasts are observed through the notification hooks in tests/ui_test_utils.h.
 * The production notification layer is REPLACED by log-only stubs in the test
 * binary, so NotificationHistory and PendingStartupWarnings are both unreachable
 * from a unit test and would silently capture nothing — which would make every
 * suppression assertion below vacuously true. The hooks are the supported
 * observation point, and they separate WARNING from INFO for us, so "I
 * suppressed the wrong toast" fails on severity as well as on text.
 */

#include "ui_manual_pull_prompt.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/post_unload_grace_test_access.h"
#include "../ui_test_utils.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "printer_state.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

/// The one sensor both surfaces watch. TOOLHEAD-roled, because that is the role
/// ui_manual_pull_prompt's subject (filament_toolhead_detected) tracks.
constexpr const char* TOOLHEAD_SENSOR = "filament_switch_sensor toolhead_sensor";

/// Substring of the FilamentSensorManager warning. The full text is prefixed
/// with the role's display name, so match on the invariant half.
constexpr const char* REMOVED_WARNING = "Filament removed";
/// Substring of the manual-pull hint (ui_manual_pull_prompt.cpp).
constexpr const char* MANUAL_PULL_HINT = "clear of the toolhead";

nlohmann::json toolhead_status(bool detected) {
    return nlohmann::json{{TOOLHEAD_SENSOR, {{"filament_detected", detected}, {"enabled", true}}}};
}

/// RAII capture of the two toast severities in play, through the hooks the
/// notification stubs call (tests/ui_test_utils.h). Severity is kept separate on
/// purpose: the warning and the hint fire on the same sensor edge, so proving
/// the right one survived means proving WHICH one did.
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

/// A real sensor edge on a real manager, with the manual-pull prompt armed the
/// way an explicit user unload arms it.
class UnloadToastFixture : public LVGLTestFixture {
  public:
    UnloadToastFixture() {
        get_printer_state().init_subjects(false);
        AmsState::instance().init_subjects(false);
        // No AMS backend: the post-unload grace is the ONLY thing that can
        // suppress the warning here, so a pass cannot be some other gate's doing.
        AmsState::instance().clear_backends();

        auto& fsm = FilamentSensorManager::instance();
        // The manual-pull prompt observes filament_toolhead_detected. Without
        // init_subjects() that subject never gets a value, arm_manual_pull_prompt()
        // sees "no filament here to watch" and silently registers no observer —
        // the hint then cannot fire and its test passes vacuously.
        fsm.init_subjects();
        PostUnloadGraceTestAccess::reset(fsm);
        fsm.set_master_enabled(true);
        fsm.discover_sensors({TOOLHEAD_SENSOR});
        fsm.set_sensor_role(TOOLHEAD_SENSOR, FilamentSensorRole::TOOLHEAD);

        // Filament present. The prompt only arms when the sensor currently SEES
        // filament, so this baseline has to land before arm_manual_pull_prompt().
        fsm.update_from_status(toolhead_status(true));
        PostUnloadGraceTestAccess::clear_startup_grace(fsm);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(fsm.is_in_startup_grace_period());
        // Nothing above may be suppressing the warning for its own reasons, or a
        // green test would prove nothing about the grace.
        REQUIRE_FALSE(is_wizard_active());
        REQUIRE_FALSE(AmsState::instance().is_filament_operation_active());
        // The prompt arms only from a "filament present" reading.
        REQUIRE(lv_subject_get_int(fsm.get_toolhead_detected_subject()) == 1);
    }

    ~UnloadToastFixture() override {
        helix::ui::disarm_manual_pull_prompt();
        AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The toolhead empties. One edge, both surfaces see it.
    static void pull_filament() {
        FilamentSensorManager::instance().update_from_status(toolhead_status(false));
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(UnloadToastFixture, "A genuine runout still raises the Filament removed warning",
                 "[runout][toast][sensors]") {
    // Nothing was unloaded. The toolhead simply went empty — this is the event
    // the warning exists to report, and it must survive the suppression.
    REQUIRE_FALSE(AmsState::instance().post_unload_runout_grace_armed());

    ToastCapture toasts;
    pull_filament();

    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
}

TEST_CASE_METHOD(UnloadToastFixture,
                 "A deliberate unload suppresses the warning but keeps the manual-pull hint",
                 "[runout][toast][sensors]") {
    auto& ams = AmsState::instance();

    // Exactly what an explicit user unload leaves behind: the prompt armed
    // (ui_panel_filament.cpp / ui_ams_sidebar.cpp both call this), and the
    // post-unload grace armed by the operation returning to IDLE.
    helix::ui::arm_manual_pull_prompt();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);
    REQUIRE(ams.post_unload_runout_grace_armed());

    ToastCapture toasts;
    pull_filament();

    // The warning is the noise — gone.
    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 0);
    // The hint is the point — still there, exactly once. Both fire on the SAME
    // edge, so a suppression that caught the wrong one fails right here.
    CHECK(toasts.infos_containing(MANUAL_PULL_HINT) == 1);

    // And the toast only PEEKED: the idle runout modal is the sole consumer and
    // its one shot is still unspent.
    CHECK(ams.post_unload_runout_grace_armed());
    CHECK(ams.consume_post_unload_runout_grace());
}

TEST_CASE_METHOD(UnloadToastFixture, "An expired grace stops suppressing the warning",
                 "[runout][toast][sensors]") {
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);
    // Long after the unload, an empty toolhead is a real runout again. Without
    // the time bound this warning would stay suppressed indefinitely.
    AmsStateTestAccess::age_post_unload_runout_grace(ams, AmsStateTestAccess::grace_window() +
                                                              std::chrono::seconds(1));

    ToastCapture toasts;
    pull_filament();

    CHECK(toasts.warnings_containing(REMOVED_WARNING) == 1);
}

TEST_CASE_METHOD(UnloadToastFixture, "Filament INSERTED is still announced during the grace",
                 "[runout][toast][sensors]") {
    auto& ams = AmsState::instance();
    AmsStateTestAccess::arm_post_unload_runout_grace(ams);

    // Only the REMOVAL side is expected after an unload. Filament arriving in
    // that window is news, and suppressing the whole toast rather than the
    // removal edge would have swallowed it.
    pull_filament();

    ToastCapture toasts;
    FilamentSensorManager::instance().update_from_status(toolhead_status(true));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.infos_containing("Filament inserted") == 1);
}
