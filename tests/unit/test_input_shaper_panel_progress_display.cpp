// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_panel_progress_display.cpp
 * @brief Progress-display behavior of InputShaperPanel across calibration phases
 *
 * Drives the real panel through the mock client's SHAPER_CALIBRATE transcript
 * (button handler -> preflight noise check -> calibrator -> collector) and
 * asserts the subjects the XML binds to:
 * - Sweeping: determinate bar (is_measuring_has_progress = 1)
 * - Analyzing: spinner (is_measuring_has_progress = 0) plus a step label
 *   counting elapsed seconds, refreshed by a 1 Hz timer on the virtual clock
 * - Complete: determinate bar restored
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/ui_panel_input_shaper.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Seconds counter parsed out of an "Analyzing data... Ns" step label, or -1
/// when the label is not an analysis label.
int analysis_seconds(const char* label) {
    static const std::regex re(R"(^Analyzing data\.\.\. (\d+)s$)");
    std::cmatch m;
    if (!label || !std::regex_search(label, m, re)) {
        return -1;
    }
    return std::atoi(m[1].str().c_str());
}

class InputShaperPanelProgressFixture : public LVGLTestFixture {
  public:
    InputShaperPanelProgressFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // The API gates execute_gcode() on its own PrinterState being ready.
        printer_state_.init_subjects(false);
        printer_state_.set_klippy_state_sync(KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, printer_state_);

        // ensure_homed_then() reads the GLOBAL PrinterState, not ours; make it
        // read as fully homed so the calibrator goes straight to the command.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");

        panel_ = &get_global_input_shaper_panel();
        panel_->init_subjects();
        panel_->set_api(&mock_client_, api_.get());
        // The panel is a global singleton shared across test cases; reset it to
        // IDLE so handle_calibrate_x_clicked() (guarded on IDLE) always runs.
        // Also drains the config query on_activate() kicks off.
        panel_->on_activate();
        helix::ui::UpdateQueue::instance().drain();
    }

    ~InputShaperPanelProgressFixture() override {
        panel_->on_deactivate(); // cancels a running calibration + the elapsed timer
        helix::ui::UpdateQueue::instance().drain();
        api_.reset();
    }

    // Pumps the mock transcript (100ms/line timer), LVGL timers (the panel's
    // 1 Hz elapsed label), and the UpdateQueue (the panel's bg_cb callbacks).
    bool pump_until(const std::function<bool()>& done, int max_ticks = 400) {
        for (int i = 0; i < max_ticks && !done(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            helix::ui::UpdateQueue::instance().drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    static lv_subject_t* subject(const char* name) {
        return lv_xml_get_subject(nullptr, name);
    }

    static const char* step_label() {
        return lv_subject_get_string(subject("is_measuring_step_label"));
    }

    static int has_progress() {
        return lv_subject_get_int(subject("is_measuring_has_progress"));
    }

    /// Panel state subject: 0=IDLE, 1=MEASURING, 2=RESULTS, 3=ERROR
    static int panel_state() {
        return lv_subject_get_int(subject("input_shaper_state"));
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    InputShaperPanel* panel_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(InputShaperPanelProgressFixture,
                 "analysis phase swaps the bar for a spinner with an elapsed label",
                 "[input_shaper][panel][progress]") {
    // Observations are taken inside the pump loop and asserted after the run,
    // so a failed CHECK never abandons a half-dispatched mock transcript.
    bool saw_sweeping_determinate = false;
    bool saw_analyzing = false;
    int analyzing_has_progress = -1;
    std::string first_analyzing_label;
    std::vector<int> analyzing_seconds_seen;
    lv_timer_t* elapsed_timer = nullptr;

    panel_->handle_calibrate_x_clicked();

    const bool completed = pump_until([&] {
        const int secs = analysis_seconds(step_label());
        if (secs >= 0) {
            if (!saw_analyzing) {
                saw_analyzing = true;
                analyzing_has_progress = has_progress();
                first_analyzing_label = step_label();
                // lv_timer_handler_safe() only executes timers with a finite
                // repeat count; the elapsed timer is periodic by design, so
                // lend it one for the duration of the observation (restored
                // below) or the harness would never fire it.
                elapsed_timer = panel_->analysis_elapsed_timer_for_test();
                REQUIRE(elapsed_timer != nullptr);
                lv_timer_set_repeat_count(elapsed_timer, 1000);
            }
            if (analyzing_seconds_seen.empty() || analyzing_seconds_seen.back() != secs) {
                analyzing_seconds_seen.push_back(secs);
            }
        } else if (has_progress() == 1 && panel_state() == 1) {
            saw_sweeping_determinate = true;
        }
        return panel_state() == 2; // RESULTS
    });

    // Give the production timer its infinite period back.
    if (elapsed_timer) {
        lv_timer_set_repeat_count(elapsed_timer, -1);
    }

    REQUIRE(completed);

    // Sweep shows the determinate bar.
    CHECK(saw_sweeping_determinate);

    // Analysis shows the spinner (has_progress 0) and a label with a seconds
    // value, not a percentage.
    REQUIRE(saw_analyzing);
    CHECK(analyzing_has_progress == 0);
    CHECK(first_analyzing_label.rfind("Analyzing data... ", 0) == 0);

    // The 1 Hz timer advanced the elapsed count while the phase was showing:
    // the mock's analysis window spans ~2.3s of virtual time, so at least one
    // tick must land with a different seconds value than the first.
    REQUIRE(analyzing_seconds_seen.size() >= 1);
    CHECK(analyzing_seconds_seen.front() == 0);
    CHECK(analyzing_seconds_seen.size() >= 2);

    // Completion restores the determinate bar (the Complete report sets 1).
    CHECK(has_progress() == 1);
}

TEST_CASE_METHOD(InputShaperPanelProgressFixture,
                 "analysis elapsed timer stops when the panel leaves the run",
                 "[input_shaper][panel][progress]") {
    // Leave the panel mid-analysis via the deactivate path, then prove the
    // 1 Hz timer no longer touches the step label: virtual seconds keep
    // flowing but the label stays frozen.
    std::string label_at_deactivate;
    bool label_ticked_after_deactivate = false;
    panel_->handle_calibrate_x_clicked();

    const bool reached = pump_until([&] {
        return analysis_seconds(step_label()) >= 0; // analysis label showing
    });
    REQUIRE(reached);
    label_at_deactivate = step_label();

    // Lend the periodic timer a finite repeat count (see test 1) so the drain
    // below CAN fire it if the deactivation path failed to cancel - otherwise
    // this case would pass vacuously, the harness never running the timer.
    lv_timer_t* elapsed_timer = panel_->analysis_elapsed_timer_for_test();
    REQUIRE(elapsed_timer != nullptr);
    lv_timer_set_repeat_count(elapsed_timer, 1000);

    panel_->on_deactivate();

    // Fixed drain, not pump_until: the mock transcript must fully play out so
    // no timer outlives the fixture, even while we assert the label froze.
    for (int i = 0; i < 60; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        helix::ui::UpdateQueue::instance().drain();
        if (std::string(step_label()) != label_at_deactivate &&
            analysis_seconds(step_label()) >= 0) {
            label_ticked_after_deactivate = true;
        }
    }
    // The label may be rewritten by the state reset, but the elapsed count
    // must never tick forward on its own again.
    CHECK_FALSE(label_ticked_after_deactivate);
}
