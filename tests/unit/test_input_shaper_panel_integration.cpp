// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_input_shaper_panel_integration.cpp
 * @brief Integration tests for the real InputShaperPanel
 *
 * The panel owns a concrete std::unique_ptr<InputShaperCalibrator>
 * (include/ui_panel_input_shaper.h:446) — there is no interface and no seam to
 * inject a stand-in through, so a hand-written "mock calibrator" could only ever
 * assert that the test pushed what the test just pushed. Everything here drives
 * the real panel instead: the pure lookup tables directly, and the rest through
 * InputShaperDeltaFixture, which runs the panel against MoonrakerClientMock's
 * full SHAPER_CALIBRATE transcript.
 */

#include "../../include/calibration_types.h"
#include "../../include/core/update_queue.h"
#include "../../include/input_shaper_calibrator.h"
#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/ui_panel_input_shaper.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/input_shaper_panel_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "lvgl/src/misc/lv_area_private.h" // lv_area_intersect
#include "theme_manager.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::calibration;
// Qualified, not a using-declaration: `ui_panel_input_shaper.h` also declares a
// global ::InputShaperPanelTestAccess (the save-path friend), and pulling the
// helix::ui one into this scope collides with it.
namespace ispta = helix::ui;

// ============================================================================
// Calibrator state machine basics (no API attached)
// ============================================================================

TEST_CASE("InputShaperCalibrator state machine basics", "[calibrator][input_shaper]") {
    InputShaperCalibrator calibrator;

    SECTION("Initial state is IDLE") {
        CHECK(calibrator.get_state() == InputShaperCalibrator::State::IDLE);
    }

    SECTION("Results start empty") {
        const auto& results = calibrator.get_results();
        CHECK_FALSE(results.has_x());
        CHECK_FALSE(results.has_y());
        CHECK_FALSE(results.is_complete());
    }

    SECTION("Cancel returns to IDLE") {
        calibrator.cancel();
        CHECK(calibrator.get_state() == InputShaperCalibrator::State::IDLE);
    }
}

// ============================================================================
// Results-card lookup tables (pure statics on the panel)
// ============================================================================

TEST_CASE("Shaper type explanation mapping", "[input_shaper][panel][results]") {
    // Every type the results card can be handed maps to its own sentence
    // (src/ui/ui_panel_input_shaper.cpp:1296). A missing arm silently degrades
    // to the generic fallback, which reads plausible and says nothing.
    CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("zv")) ==
          "Fast but minimal smoothing — best for well-built printers");
    CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("mzv")) ==
          "Good balance of speed and vibration reduction");
    CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("ei")) ==
          "Strong vibration reduction with moderate speed impact");
    CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("2hump_ei")) ==
          "Heavy smoothing — significant vibration issues detected");
    CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("3hump_ei")) ==
          "Maximum smoothing — consider checking mechanical issues");

    SECTION("Kalico smooth shapers get their own explanations") {
        // Klipper's five have "smooth_" twins on Kalico; they must not fall
        // through to the generic string.
        const std::string fallback = "Vibration compensation active";
        for (const char* type : {"smooth_zv", "smooth_mzv", "smooth_ei", "smooth_2hump_ei",
                                 "smooth_zvd_ei", "smooth_si"}) {
            INFO("type: " << type);
            CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation(type)) !=
                  fallback);
        }
    }

    SECTION("An unknown type falls back to the generic wording") {
        CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("not_a_shaper")) ==
              "Vibration compensation active");
        CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("")) ==
              "Vibration compensation active");
        // Klipper's names are lowercase; the lookup is exact, not case-folded.
        CHECK(std::string(ispta::InputShaperPanelTestAccess::shaper_explanation("MZV")) ==
              "Vibration compensation active");
    }
}

TEST_CASE("Vibration quality thresholds", "[input_shaper][panel][results]") {
    // Quality levels drive the result card's colour: 0=excellent (<5%),
    // 1=good (5-15%), 2=fair (15-25%), 3=poor (>=25%)
    // (src/ui/ui_panel_input_shaper.cpp:1326). Assert the boundaries, since an
    // off-by-one in a `<` vs `<=` is exactly what mis-colours a card.
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(0.0f) == 0);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(4.9f) == 0);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(5.0f) == 1);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(14.9f) == 1);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(15.0f) == 2);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(24.9f) == 2);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(25.0f) == 3);
    CHECK(ispta::InputShaperPanelTestAccess::vibration_quality(100.0f) == 3);

    SECTION("The prose description switches on the same boundaries") {
        // Two independent ladders over the same thresholds; a change to one
        // that misses the other shows a green card with "Poor" text.
        for (float v : {0.0f, 4.9f, 5.0f, 14.9f, 15.0f, 24.9f, 25.0f, 100.0f}) {
            INFO("vibrations: " << v);
            const int quality = ispta::InputShaperPanelTestAccess::vibration_quality(v);
            const std::string desc = ispta::InputShaperPanelTestAccess::quality_description(v);
            const char* expected[] = {"Excellent", "Good", "Fair", "Poor"};
            CHECK(desc.rfind(expected[quality], 0) == 0);
        }
    }
}

// ============================================================================
// Live-before delta display + firmware X-overwrite warning
// ============================================================================
//
// Drives the REAL panel through the mock client's full Calibrate All
// transcript (preflight -> X sweep/analysis/CSV -> Y sweep/analysis/CSV) with
// a staged live-before configuration, then asserts the delta/verdict/warning
// subjects the results cards bind to.

namespace {

class InputShaperDeltaFixture : public LVGLUITestFixture {
  public:
    InputShaperDeltaFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        // A previous test's mock run in THIS process may have left the
        // calibration CSVs behind; the marker-line injection below keys off the
        // X file appearing, so both must start absent. Scoped to our own PID -
        // on the old fixed path this deleted the fixture of whichever
        // concurrent shard happened to be mid-parse.
        MoonrakerClientMock::remove_shaper_csvs();

        // Live-before config staged per-test (mock default is mzv@36.7/ei@47.6;
        // the staged pair deliberately differs so a leaked default fails loud).
        // zv@100 is far off the mock's 53.8 Hz resonance, so under the
        // thresholded (firmware-matching) residual metric the old setting
        // leaves clearly more vibration than the new fit.
        mock_client_.set_input_shaper_values("zv", 100.0, "zv", 100.0);

        printer_state_.init_subjects(false);
        printer_state_.set_klippy_state_sync(KlippyState::READY);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, printer_state_);

        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        lv_subject_copy_string(get_printer_state().motion_state().get_homed_axes_subject(), "xyz");

        panel_ = &get_global_input_shaper_panel();
        panel_->init_subjects();
        panel_->set_api(&mock_client_, api_.get());

        // Create the panel's XML view so widget-level assertions (legend
        // entries, captions) can run against real bindings. The view is NOT
        // wired through panel_->create(): the persistent singleton would keep
        // widget pointers past this fixture's screen teardown. The panel only
        // ever touches subjects, so driving it and binding the view to the
        // same subjects keeps both in sync.
        view_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "input_shaper_panel", nullptr));
        REQUIRE(view_ != nullptr);

        panel_->on_activate(); // resets to IDLE, hides the gated legend rows
        helix::ui::UpdateQueue::instance().drain();
    }

    ~InputShaperDeltaFixture() override {
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        helix::ui::UpdateQueue::instance().drain();

        // Drop the panel's subjects BEFORE the view: the view's bind observers
        // live on those subjects, and the singleton panel would otherwise
        // leave observers pointing into the deleted widget tree for whatever
        // test runs next.
        panel_->deinit_subjects();
        lv_obj_delete(view_);
        helix::ui::UpdateQueue::instance().drain();
        api_.reset();

        // Do not strand this process's CSVs in /tmp: the path carries our PID,
        // so nothing else will ever overwrite them.
        MoonrakerClientMock::remove_shaper_csvs();
    }

    /// Widget by name inside the panel view (fails loud when the XML drops it)
    static lv_obj_t* view_widget(lv_obj_t* root, const char* name) {
        lv_obj_t* w = lv_obj_find_by_name(root, name);
        REQUIRE(w != nullptr);
        return w;
    }

    // Pumps the mock transcript (100ms/line), LVGL timers, and the UpdateQueue
    // until pred() holds. Optionally injects the Creality copy-marker line
    // when the step label hits marker_trigger: "Step 2 of 2" (Calibrate All's
    // Y run) or "Calibrating Y axis..." (a Y-only run) is set by
    // start_calibration('Y') right before the Y collector registers, and
    // survives until the Y sweep's first progress line one tick later - so it
    // is observable exactly while the marker has a live collector to land on.
    bool pump_until(const std::function<bool()>& done, bool inject_copy_marker,
                    const char* marker_trigger = "Step 2 of 2", int max_ticks = 600) {
        bool marker_sent = false;
        for (int i = 0; i < max_ticks && !done(); ++i) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            helix::ui::UpdateQueue::instance().drain();

            if (inject_copy_marker && !marker_sent &&
                subject_string("is_measuring_step_label") == marker_trigger) {
                marker_sent = true;
                mock_client_.dispatch_gcode_response(
                    "copy_TestAxis_y_to_x Recommended shaper_type_x = ei, shaper_freq_x "
                    "= 71.4 Hz");
            }
            if (inject_copy_marker && !marker_sent &&
                subject_string("is_measuring_axis_label") == marker_trigger) {
                marker_sent = true;
                mock_client_.dispatch_gcode_response(
                    "copy_TestAxis_y_to_x Recommended shaper_type_x = ei, shaper_freq_x "
                    "= 71.4 Hz");
            }
        }
        return done();
    }

    static lv_subject_t* subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return s;
    }

    static std::string subject_string(const char* name) {
        return lv_subject_get_string(subject(name));
    }

    static int subject_int(const char* name) {
        return lv_subject_get_int(subject(name));
    }

    /// Panel state subject: 0=IDLE, 1=MEASURING, 2=RESULTS, 3=ERROR
    static int panel_state() {
        return subject_int("input_shaper_state");
    }

  protected:
    MoonrakerClientMock mock_client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
    InputShaperPanel* panel_ = nullptr;
    lv_obj_t* view_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "Calibrate All results show the live-before delta and residual verdict",
                 "[input_shaper][panel][delta]") {
    SECTION("with the firmware copy-marker line") {
        panel_->handle_calibrate_all_clicked();

        const bool done = pump_until([&] { return panel_state() == 2; },
                                     /*inject_copy_marker=*/true);
        REQUIRE(done);

        // Live-before value folded into the delta line: staged zv@100.0 (the
        // mock recommends mzv@53.8).
        CHECK(subject_string("is_x_delta_text") == "zv @ 100.0 Hz -> mzv @ 53.8 Hz");
        CHECK(subject_int("is_x_has_delta") == 1);

        // Verdict: the old setting re-scored on today's PSD must show MORE
        // residual than the new fit (the old notch at 69.8 Hz sits off the
        // mock's 53.8 Hz resonance peak).
        REQUIRE(subject_int("is_x_has_verdict") == 1);
        const std::string verdict = subject_string("is_x_verdict_text");
        std::cmatch m;
        static const std::regex verdict_re(
            R"(^Old setting on today's data: (\d+\.\d)% residual - now: (\d+\.\d)%$)");
        INFO("verdict: " << verdict);
        REQUIRE(std::regex_search(verdict.c_str(), m, verdict_re));
        const double old_pct = std::atof(m[1].str().c_str());
        const double new_pct = std::atof(m[2].str().c_str());
        CHECK(old_pct > new_pct);

        // The Y card gets the same treatment (old Y staged ei@69.8).
        REQUIRE(subject_int("is_y_has_verdict") == 1);

        // Firmware warning: visible only because the marker line was injected
        // during the Y run; it belongs on the X card, whose measured value is
        // the one the fork discarded.
        CHECK(subject_int("is_x_fw_overwrite_warn") == 1);

        // Chart key: the legend names all three curves, and the Previous entry
        // (the muted old-setting curve) exists only when a before-config was
        // captured.
        CHECK(std::string(lv_label_get_text(view_widget(view_, "legend_x_measured_label"))) ==
              "Measured (shaper off)");
        lv_obj_t* prev_dot = view_widget(view_, "legend_x_previous_dot");
        lv_obj_t* prev_label = view_widget(view_, "legend_x_previous_label");
        CHECK_FALSE(lv_obj_has_flag(prev_dot, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(prev_label, LV_OBJ_FLAG_HIDDEN));
        CHECK(std::string(lv_label_get_text(prev_label)) == "Previous");
        lv_obj_t* y_prev_dot = view_widget(view_, "legend_y_previous_dot");
        CHECK_FALSE(lv_obj_has_flag(y_prev_dot, LV_OBJ_FLAG_HIDDEN));

        // The chart's Y quantity is named above the plot.
        REQUIRE(lv_obj_find_by_name(view_, "chart_x_caption") != nullptr);
        REQUIRE(lv_obj_find_by_name(view_, "chart_y_caption") != nullptr);
    }

    SECTION("without the marker line the warning stays hidden") {
        panel_->handle_calibrate_all_clicked();

        const bool done = pump_until([&] { return panel_state() == 2; },
                                     /*inject_copy_marker=*/false);
        REQUIRE(done);

        // Delta and verdict still populate from the staged live-before config...
        CHECK(subject_int("is_x_has_delta") == 1);
        CHECK(subject_int("is_x_has_verdict") == 1);
        // ...so the Previous legend entry is present too...
        CHECK_FALSE(
            lv_obj_has_flag(view_widget(view_, "legend_x_previous_label"), LV_OBJ_FLAG_HIDDEN));
        // ...but no firmware overwrite happened, so no warning.
        CHECK(subject_int("is_x_fw_overwrite_warn") == 0);
    }
}

TEST_CASE_METHOD(InputShaperDeltaFixture, "delta rows stay hidden without a live-before config",
                 "[input_shaper][panel][delta]") {
    mock_client_.set_input_shaper_configured(false);

    panel_->handle_calibrate_x_clicked();

    const bool done = pump_until([&] { return panel_state() == 2; },
                                 /*inject_copy_marker=*/false);
    REQUIRE(done);

    CHECK(subject_int("is_x_has_delta") == 0);
    CHECK(subject_int("is_x_has_verdict") == 0);
    CHECK(subject_string("is_x_delta_text").empty());
    CHECK(subject_int("is_x_fw_overwrite_warn") == 0);

    // The Previous legend entry hides with the delta rows (no before-config),
    // while the always-on key entries and the chart caption remain.
    CHECK(lv_obj_has_flag(view_widget(view_, "legend_x_previous_dot"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(view_widget(view_, "legend_x_previous_label"), LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(view_widget(view_, "legend_x_measured_label"))) ==
          "Measured (shaper off)");
    REQUIRE(lv_obj_find_by_name(view_, "chart_x_caption") != nullptr);
}

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "a Y-only run warns on the Y card when firmware overwrites X",
                 "[input_shaper][panel][delta][fw_overwrite]") {
    // The fork's copy_TestAxis_y_to_x fires at the end of ANY Y run; with no X
    // result in this session the warning must still surface - on the Y card,
    // the only card a Y-only run shows.
    panel_->handle_calibrate_y_clicked();

    const bool done = pump_until([&] { return panel_state() == 2; },
                                 /*inject_copy_marker=*/true,
                                 /*marker_trigger=*/"Calibrating Y axis...");
    REQUIRE(done);

    // No X result exists, so the X card is absent and only the Y card shows.
    CHECK(subject_int("is_results_has_x") == 0);
    CHECK(subject_int("is_x_fw_overwrite_warn") == 1);

    // The Y card's copy of the warning row is visible. The X card's own copy
    // is unreachable because the whole X card is hidden (its row's own flag
    // stays clear - LVGL hides children through the parent, not their flags).
    CHECK_FALSE(lv_obj_has_flag(view_widget(view_, "fw_overwrite_warn_y"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(view_widget(view_, "result_card_x"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(InputShaperDeltaFixture,
                 "close drops the stored X result so a Y-only apply never sends X",
                 "[input_shaper][panel][apply][stale_x]") {
    // RAII spdlog capture: the mock client logs every SET_INPUT_SHAPER with
    // its full arguments, which is the observable for what Apply sent.
    class GcodeLogCapture {
      public:
        GcodeLogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(256)) {
            sink_->set_level(spdlog::level::trace);
            spdlog::default_logger()->sinks().push_back(sink_);
        }
        ~GcodeLogCapture() {
            auto& sinks = spdlog::default_logger()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
        }
        [[nodiscard]] int count(const std::string& needle) const {
            int n = 0;
            for (const auto& l : sink_->last_formatted(256)) {
                if (l.find(needle) != std::string::npos) {
                    ++n;
                }
            }
            return n;
        }

      private:
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    } capture;

    // 1. Calibrate All stores an X result.
    panel_->handle_calibrate_all_clicked();
    REQUIRE(pump_until([&] { return panel_state() == 2; }, false));
    REQUIRE(subject_int("is_results_has_x") == 1);

    // 2. Close the results - clear_results() must drop the stored X result.
    panel_->handle_close_clicked();
    helix::ui::UpdateQueue::instance().drain();

    // 3. A later Y-only calibration + Apply must send SET_INPUT_SHAPER for Y
    //    only; a stale x_result_ would silently re-send X with the previous
    //    session's values.
    panel_->handle_calibrate_y_clicked();
    REQUIRE(pump_until([&] { return panel_state() == 2; }, false));

    panel_->handle_apply_clicked();
    // The apply chain is async (lifetime-deferred success callbacks)
    for (int i = 0; i < 20; ++i) {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        helix::ui::UpdateQueue::instance().drain();
        if (capture.count("SET_INPUT_SHAPER") > 0) {
            break;
        }
    }

    CHECK(capture.count("SHAPER_TYPE_X") == 0);
    CHECK(capture.count("SHAPER_TYPE_Y") == 1);
}

// ============================================================================
// Test print pattern
// ============================================================================

TEST_CASE_METHOD(InputShaperDeltaFixture, "Print test pattern sends the acceleration tuning tower",
                 "[input_shaper][panel][test_pattern]") {
    // The button's whole job is one command: TUNING_TOWER ramps acceleration
    // across the print so ringing can be compared band by band
    // (src/ui/ui_panel_input_shaper.cpp:2071).
    mock_client_.clear_gcode_script_history();

    panel_->handle_print_test_pattern_clicked();
    helix::ui::UpdateQueue::instance().drain();

    const auto& history = mock_client_.gcode_script_history();
    auto it = std::find_if(history.begin(), history.end(), [](const std::string& g) {
        return g.find("TUNING_TOWER") != std::string::npos;
    });
    REQUIRE(it != history.end());
    INFO("sent: " << *it);
    // The parameters are the test: a bare TUNING_TOWER with the wrong target or
    // band would still contain the command name and do nothing useful.
    CHECK(it->find("COMMAND=SET_VELOCITY_LIMIT") != std::string::npos);
    CHECK(it->find("PARAMETER=ACCEL") != std::string::npos);
    CHECK(it->find("START=1500") != std::string::npos);
    CHECK(it->find("FACTOR=500") != std::string::npos);
    CHECK(it->find("BAND=5") != std::string::npos);
}

// ============================================================================
// Current-config header card
// ============================================================================

TEST_CASE_METHOD(InputShaperDeltaFixture, "InputShaperPanel current config subjects",
                 "[input_shaper][panel][subjects]") {
    SECTION("Configured shaper populates the display subjects") {
        InputShaperConfig config;
        config.is_configured = true;
        config.shaper_type_x = "mzv";
        config.shaper_freq_x = 36.7f;
        config.shaper_type_y = "ei";
        config.shaper_freq_y = 47.6f;

        ispta::InputShaperPanelTestAccess::populate_current_config(*panel_, config);
        helix::ui::UpdateQueue::instance().drain();

        CHECK(subject_int("is_shaper_configured") == 1);
        // Types are uppercased for display; frequencies get the shared "%.1f Hz"
        // formatter (src/ui/ui_panel_input_shaper.cpp:1206).
        CHECK(subject_string("is_current_x_type") == "MZV");
        CHECK(subject_string("is_current_x_freq") == "36.7 Hz");
        CHECK(subject_string("is_current_y_type") == "EI");
        CHECK(subject_string("is_current_y_freq") == "47.6 Hz");
        // Max accel is not part of the current-config query.
        CHECK(subject_string("is_current_max_accel").empty());
    }

    SECTION("Unconfigured shaper clears whatever was displayed before") {
        // Seed a configured state first: clearing is the branch that matters,
        // and asserting empty strings against never-populated subjects proves
        // nothing.
        InputShaperConfig configured;
        configured.is_configured = true;
        configured.shaper_type_x = "mzv";
        configured.shaper_freq_x = 36.7f;
        configured.shaper_type_y = "ei";
        configured.shaper_freq_y = 47.6f;
        ispta::InputShaperPanelTestAccess::populate_current_config(*panel_, configured);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(subject_string("is_current_x_type") == "MZV");

        ispta::InputShaperPanelTestAccess::populate_current_config(*panel_, InputShaperConfig{});
        helix::ui::UpdateQueue::instance().drain();

        CHECK(subject_int("is_shaper_configured") == 0);
        CHECK(subject_string("is_current_x_type").empty());
        CHECK(subject_string("is_current_x_freq").empty());
        CHECK(subject_string("is_current_y_type").empty());
        CHECK(subject_string("is_current_y_freq").empty());
    }
}

// ============================================================================
// Per-axis results card
// ============================================================================

TEST_CASE_METHOD(InputShaperDeltaFixture, "Per-axis result population",
                 "[input_shaper][panel][results]") {
    // A single-axis run must fill that axis's card and leave the other one
    // untouched (src/ui/ui_panel_input_shaper.cpp:1345 populate_axis_result).
    panel_->handle_calibrate_x_clicked();
    REQUIRE(pump_until([&] { return panel_state() == 2; }, /*inject_copy_marker=*/false));

    CHECK(subject_int("is_results_has_x") == 1);
    CHECK(subject_int("is_results_has_y") == 0);

    // The mock recommends mzv @ 53.8 Hz with 1.6% residual vibration and a
    // 4000 mm/s^2 accel ceiling.
    CHECK(subject_string("is_result_x_shaper") == "Optimal: MZV @ 53.8 Hz");
    CHECK(subject_string("is_result_x_vibration") == "1.6%");
    CHECK(subject_string("is_result_x_max_accel") == "4000 mm/s\xC2\xB2");
    // 1.6% < 5% -> quality 0, and the explanation is mzv's, not the fallback.
    CHECK(subject_int("is_result_x_quality") == 0);
    CHECK(subject_string("is_result_x_explanation") ==
          std::string("* ") + ispta::InputShaperPanelTestAccess::shaper_explanation("mzv"));

    // The comparison table lists all five fits and marks the recommended row.
    CHECK(subject_int("is_x_num_shapers") == 5);
    const int recommended = subject_int("is_x_recommended_row");
    REQUIRE(recommended >= 0);
    REQUIRE(recommended < 5);
    char row_name[32];
    snprintf(row_name, sizeof(row_name), "is_x_cmp_%d_type", recommended);
    const std::string row_type = lv_subject_get_string(subject(row_name));
    INFO("recommended row " << recommended << ": " << row_type);
    CHECK(row_type == "MZV *");

    // The Y card stayed empty.
    CHECK(subject_string("is_result_y_shaper").empty());
    CHECK(subject_int("is_y_num_shapers") == 0);
}

// ============================================================================
// Live sweep chart in MEASURING (prestonbrown/helixscreen#1812)
// ============================================================================

#include "../test_helpers/sweep_preview_fakes.h"
#include "resonance_console.h"

namespace {

/// Runs the panel against the mock's transcript with a manual stream source
/// fed by ConsoleLedFeeder.
class InputShaperPreviewFixture : public InputShaperDeltaFixture {
  public:
    InputShaperPreviewFixture() {
        ispta::InputShaperPanelTestAccess::set_preview_source_factory(
            *panel_, helix::calibration::test::manual_factory(log_));
        ispta::InputShaperPanelTestAccess::set_overlay_root(*panel_, view_);
        set_reachable(true);
        mock_client_.register_method_callback(
            "notify_gcode_response", "preview_fixture", [this](const nlohmann::json& msg) {
                const auto line = msg["params"][0].get<std::string>();
                if (const auto hz = parse_testing_frequency(line)) {
                    latest_hz_ = *hz;
                }
            });
    }

    ~InputShaperPreviewFixture() override {
        mock_client_.unregister_method_callback("notify_gcode_response", "preview_fixture");
        panel_->on_ui_destroyed();
        ispta::InputShaperPanelTestAccess::set_preview_source_factory(*panel_, nullptr);
        ispta::InputShaperPanelTestAccess::set_overlay_root(*panel_, nullptr);
        ispta::InputShaperPanelTestAccess::set_parent_screen(*panel_, nullptr);
    }

    void set_reachable(bool reachable) {
        LiveSweepFacts facts;
        facts.socket_path = "/tmp/k.sock";
        facts.socket_reachable = reachable;
        facts.cfg.accel_chip = "adxl345";
        facts.cfg.from_printer = true;
        ispta::InputShaperPanelTestAccess::set_preview_facts(*panel_, facts);
    }

    /// One mock tick, then samples up to the latest console frequency.
    void tick() {
        lv_tick_inc(100);
        lv_timer_handler_safe();
        helix::ui::UpdateQueue::instance().drain();
        feeder_.feed(log_, latest_hz_);
    }

    bool run_until(const std::function<bool()>& done, int max_ticks = 600) {
        for (int i = 0; i < max_ticks && !done(); ++i) {
            tick();
        }
        return done();
    }

    [[nodiscard]] const helix::ui::ShaperSweepChart& chart() const {
        return ispta::InputShaperPanelTestAccess::sweep_chart(*panel_);
    }

    /// Every clean point's column value is on the drawn curve. STANDARD
    /// stores 200 of the 201-slot grid, so one point may be picked out.
    [[nodiscard]] bool chart_shows(SweepColumn column) const {
        if (chart().chart() == nullptr || chart().preview().points().empty()) {
            return false;
        }
        const auto amps = ui_frequency_response_chart_get_series_amplitudes(chart().chart(), 0);
        int missing = 0;
        for (const auto& p : chart().preview().points()) {
            if (!p.clean) {
                continue;
            }
            const float v = sweep_column_value(p, column);
            if (std::find(amps.begin(), amps.end(), v) == amps.end()) {
                ++missing;
            }
        }
        return missing <= 1;
    }

    helix::calibration::test::SourceLog log_;
    helix::calibration::test::ConsoleLedFeeder feeder_;
    float latest_hz_ = 0.0f;
};

} // namespace

TEST_CASE_METHOD(InputShaperPreviewFixture, "the measuring view draws the live sweep",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return log_.created == 1; }));
    CHECK(log_.accel_chip == "adxl345");
    CHECK(log_.plan.freq_start_hz == 5.0f);
    CHECK(log_.plan.freq_end_hz == 135.0f);
    CHECK(log_.plan.hz_per_sec == 1.0f);

    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    CHECK(panel_state() == 1);
    CHECK(chart_shows(SweepColumn::X));

    // Analyzing: the stream is finishing or done, the curve stays up.
    REQUIRE(run_until(
        [&] { return subject_string("is_measuring_step_label").rfind("Analyzing", 0) == 0; }));
    CHECK(chart().has_curve());
    CHECK(subject_int("is_measuring_has_chart") == 1);

    REQUIRE(run_until([&] { return panel_state() == 2; }));
    CHECK(log_.stopped == log_.created);
    CHECK(subject_int("is_measuring_has_chart") == 0);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "Calibrate All gives Y a fresh live sweep",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_all_clicked();
    // Once a curve is up, the chart never goes blank while measuring: X's CSV
    // curve holds it between X's result and Y's first points.
    bool shown = false;
    bool blanked = false;
    auto watch = [&] {
        const int has = subject_int("is_measuring_has_chart");
        shown = shown || has == 1;
        blanked = blanked || (shown && has == 0 && panel_state() == 1);
    };
    REQUIRE(run_until([&] {
        watch();
        return log_.created == 2;
    }));
    CHECK(subject_int("is_measuring_has_chart") == 1);
    REQUIRE(run_until([&] {
        watch();
        return !chart().preview().points().empty();
    }));
    CHECK(chart_shows(SweepColumn::Y));
    CHECK_FALSE(blanked);
    REQUIRE(run_until([&] { return panel_state() == 2; }));
    CHECK(log_.created == 2);
    CHECK(log_.stopped == 2);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "no live sweep without klippy's socket",
                 "[input_shaper][panel][preview]") {
    set_reachable(false);
    bool ever_shown = false;
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] {
        ever_shown = ever_shown || subject_int("is_measuring_has_chart") == 1;
        return panel_state() == 2;
    }));
    CHECK(log_.created == 0);
    CHECK_FALSE(ever_shown);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "aborting the sweep stops the live stream",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    panel_->handle_cancel_clicked();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(log_.created >= 1);
    CHECK(log_.stopped == log_.created);
    CHECK(subject_int("is_measuring_has_chart") == 0);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "no stream where the layout hides the live chart",
                 "[input_shaper][panel][preview]") {
    lv_subject_t* fits = lv_xml_find_subject(lv_xml_component_get_scope("input_shaper_panel"),
                                             "is_measuring_chart_fits");
    REQUIRE(fits != nullptr);
    CHECK(helix::ui::live_chart_layout_fits("input_shaper_panel", "is_measuring_chart_fits"));
    lv_subject_set_int(fits, 0);
    CHECK_FALSE(helix::ui::live_chart_layout_fits("input_shaper_panel", "is_measuring_chart_fits"));

    panel_->handle_calibrate_x_clicked();
    const bool done = run_until([&] { return panel_state() == 2; });
    lv_subject_set_int(fits, 1);
    REQUIRE(done);
    CHECK(log_.created == 0);
    // An absent gate reads as fitting.
    CHECK(helix::ui::live_chart_layout_fits("input_shaper_panel", "no_such_gate"));
}

TEST_CASE_METHOD(InputShaperPreviewFixture,
                 "a rebuild drops the live chart with the tree it lived in",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    REQUIRE(chart().chart() != nullptr);

    // What a hot reload does: free the whole widget tree and create it again.
    // The live chart is a child of a host inside that tree.
    ispta::InputShaperPanelTestAccess::set_parent_screen(*panel_, test_screen());
    REQUIRE(panel_->rebuild());
    view_ = panel_->get_root();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(chart().chart() == nullptr);
    CHECK(log_.stopped == log_.created);
    CHECK(subject_int("is_measuring_has_chart") == 0);

    // Stop and leave after the rebuild touch only the new tree.
    panel_->handle_cancel_clicked();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(chart().chart() == nullptr);
    CHECK(lv_obj_find_by_name(view_, "chart_container_measuring") != nullptr);
}

#include "moonraker_api_mock.h"

TEST_CASE_METHOD(InputShaperPreviewFixture,
                 "opening the panel asks the printer for klippy's socket",
                 "[input_shaper][panel][preview]") {
    // Nothing seeds the facts: the panel learns them from the mock printer.
    ispta::InputShaperPanelTestAccess::set_preview_facts(*panel_, LiveSweepFacts{});
    MoonrakerAPIMock mock_api(mock_client_, printer_state_);
    panel_->set_api(&mock_client_, &mock_api);
    panel_->on_activate();
    helix::ui::UpdateQueue::instance().drain();

    panel_->handle_calibrate_x_clicked();
    const bool streamed = run_until([&] { return log_.created == 1; });
    CHECK(log_.accel_chip == "adxl345");
    const bool finished = run_until([&] { return panel_state() == 2; });
    panel_->set_api(&mock_client_, api_.get());
    CHECK(streamed);
    CHECK(finished);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "dropping the panel's subjects stops the live stream",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return log_.created == 1 && log_.last != nullptr; }));
    REQUIRE(log_.stopped == 0);
    panel_->deinit_subjects();
    CHECK(log_.stopped == log_.created);
    panel_->init_subjects();
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "cleanup frees the live chart and stops its stream",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    REQUIRE(chart().chart() != nullptr);
    panel_->cleanup();
    CHECK(chart().chart() == nullptr);
    CHECK(log_.stopped == log_.created);
    CHECK(subject_int("is_measuring_has_chart") == 0);
}

TEST_CASE_METHOD(InputShaperPreviewFixture, "a sweep error stops the live stream",
                 "[input_shaper][panel][preview]") {
    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    REQUIRE(log_.stopped == 0);
    mock_client_.dispatch_gcode_response("!! Move out of range: 0.000 400.000 10.000 [0.000]");
    REQUIRE(run_until([&] { return panel_state() == 3; }));
    CHECK(log_.stopped == log_.created);
    CHECK(subject_int("is_measuring_has_chart") == 0);
}

TEST_CASE_METHOD(InputShaperPreviewFixture,
                 "the results chart draws its raw PSD like the live curve",
                 "[input_shaper][panel][preview]") {
    // The results charts exist only in a created view.
    ispta::InputShaperPanelTestAccess::set_parent_screen(*panel_, test_screen());
    REQUIRE(panel_->rebuild());
    view_ = panel_->get_root();
    helix::ui::UpdateQueue::instance().drain();
    // The rebuild's activation asked this fixture's API, which has no socket.
    set_reachable(true);

    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    REQUIRE(chart().chart() != nullptr);
    const auto live = ui_frequency_response_chart_get_series_style(chart().chart(), 0);
    const lv_color_t live_color = ui_frequency_response_chart_get_series_color(chart().chart(), 0);
    REQUIRE(live.fill);

    REQUIRE(run_until([&] { return panel_state() == 2; }));
    auto* results = ispta::InputShaperPanelTestAccess::results_chart(*panel_, 'X');
    REQUIRE(results != nullptr);
    const int raw = ispta::InputShaperPanelTestAccess::raw_series_id(*panel_, 'X');
    REQUIRE(raw >= 0);
    const auto style = ui_frequency_response_chart_get_series_style(results, raw);
    CHECK(style.line_width == live.line_width);
    CHECK(style.glow == live.glow);
    CHECK(style.fill == live.fill);
    CHECK(lv_color_eq(ui_frequency_response_chart_get_series_color(results, raw), live_color));
}

namespace {

/// The part of @p obj a user can see: its box clipped by every scrolling or
/// clipping ancestor.
lv_area_t visible_area(lv_obj_t* obj) {
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    for (lv_obj_t* p = lv_obj_get_parent(obj); p != nullptr; p = lv_obj_get_parent(p)) {
        lv_area_t parent_area;
        lv_obj_get_coords(p, &parent_area);
        if (!lv_area_intersect(&area, &area, &parent_area)) {
            return lv_area_t{0, 0, -1, -1};
        }
    }
    return area;
}

bool fully_visible(lv_obj_t* obj) {
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const lv_area_t seen = visible_area(obj);
    return seen.x1 == box.x1 && seen.y1 == box.y1 && seen.x2 == box.x2 && seen.y2 == box.y2;
}

bool overlaps(lv_obj_t* a, lv_obj_t* b) {
    lv_area_t aa;
    lv_area_t bb;
    lv_area_t common;
    lv_obj_get_coords(a, &aa);
    lv_obj_get_coords(b, &bb);
    return lv_area_intersect(&common, &aa, &bb);
}

} // namespace

TEST_CASE_METHOD(InputShaperPreviewFixture, "the results view's legend, colors and table fit",
                 "[input_shaper][panel][results]") {
    ispta::InputShaperPanelTestAccess::set_parent_screen(*panel_, test_screen());
    REQUIRE(panel_->rebuild());
    view_ = panel_->get_root();
    helix::ui::UpdateQueue::instance().drain();
    set_reachable(true);

    panel_->handle_calibrate_x_clicked();
    REQUIRE(run_until([&] { return subject_int("is_measuring_has_chart") == 1; }));
    REQUIRE(chart().chart() != nullptr);
    const lv_color_t measured = theme_manager_get_color("chart_measured");
    CHECK(lv_color_eq(ui_frequency_response_chart_get_series_color(chart().chart(), 0), measured));

    REQUIRE(run_until([&] { return panel_state() == 2; }));
    lv_obj_update_layout(view_);

    SECTION("the measured curve and its legend dot take chart_measured") {
        auto* results = ispta::InputShaperPanelTestAccess::results_chart(*panel_, 'X');
        REQUIRE(results != nullptr);
        const int raw = ispta::InputShaperPanelTestAccess::raw_series_id(*panel_, 'X');
        REQUIRE(raw >= 0);
        CHECK(lv_color_eq(ui_frequency_response_chart_get_series_color(results, raw), measured));
        CHECK(lv_color_eq(
            lv_obj_get_style_bg_color(view_widget(view_, "legend_x_measured_dot"), LV_PART_MAIN),
            measured));
    }

    SECTION("the legend shows in full on the opened results, clear of Save") {
        lv_obj_t* save = view_widget(view_, "btn_save");
        for (const char* name : {"legend_x_measured_dot", "legend_x_measured_label",
                                 "legend_x_shaper_dot", "chart_container_x"}) {
            lv_obj_t* w = view_widget(view_, name);
            INFO(name);
            CHECK(fully_visible(w));
            CHECK_FALSE(overlaps(w, save));
        }
    }

    SECTION("every comparison table row fits its table") {
        lv_obj_t* table = view_widget(view_, "shaper_table_x");
        lv_area_t table_box;
        lv_obj_get_coords(table, &table_box);
        const uint32_t rows = lv_obj_get_child_count(table);
        int checked = 0;
        for (uint32_t r = 0; r < rows; ++r) {
            lv_obj_t* row = lv_obj_get_child(table, static_cast<int32_t>(r));
            if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_child_count(row) < 4) {
                continue;
            }
            lv_area_t row_box;
            lv_obj_get_coords(row, &row_box);
            const int32_t content_right =
                row_box.x2 - lv_obj_get_style_pad_right(row, LV_PART_MAIN);
            lv_area_t last;
            lv_obj_get_coords(lv_obj_get_child(row, -1), &last);
            INFO("row " << r << " last cell ends at " << last.x2 << ", content at " << content_right
                        << ", table at " << table_box.x2);
            CHECK(last.x2 <= content_right);
            CHECK(row_box.x2 <= table_box.x2);
            ++checked;
        }
        CHECK(checked >= 6);
    }
}
