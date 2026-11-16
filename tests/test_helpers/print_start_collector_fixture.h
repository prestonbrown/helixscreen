// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_client_mock.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "printer_state.h"

#include <memory>
#include <string>

#include "hv/json.hpp"

/// The checked-in replay and start-file fixtures, found from this source tree.
inline std::string collector_fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

using helix::PrinterState;
using helix::PrintStartPhase;
using json = nlohmann::json;

/**
 * @brief Test fixture for PrintStartCollector proactive heater detection tests
 *
 * Provides initialized PrinterState and mock MoonrakerClient for testing
 * the collector's fallback completion and proactive heater detection.
 */
class PrintStartCollectorHeaterFixture : public LVGLTestFixture {
  public:
    PrintStartCollectorHeaterFixture() {
        state_.init_subjects(false);
        // Mark print as active so set_print_start_state() accepts phase updates
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);
        client_ = std::make_unique<MoonrakerClientMock>();
        collector_ = PrintStartCollector::create(*client_, state_);
        collector_->set_profile(PrintStartProfile::load_default());
    }

    ~PrintStartCollectorHeaterFixture() override {
        if (collector_->is_active()) {
            collector_->stop();
        }
        collector_.reset();
        client_.reset();
    }

    PrinterState& state() {
        return state_;
    }
    MoonrakerClientMock& client() {
        return *client_;
    }
    PrintStartCollector& collector() {
        return *collector_;
    }

    /**
     * @brief Get current print start phase from PrinterState subject
     */
    PrintStartPhase get_current_phase() {
        return static_cast<PrintStartPhase>(
            lv_subject_get_int(state_.print_state().get_print_start_phase_subject()));
    }

    /**
     * @brief Get current print start message from PrinterState subject
     */
    std::string get_current_message() {
        return lv_subject_get_string(state_.print_state().get_print_start_message_subject());
    }

    /**
     * @brief Set bed temperature and target in PrinterState subjects
     *
     * Values are in decidegrees (temp * 10) as stored in PrinterState.
     * Example: 60.0C = 600 decidegrees
     */
    void set_bed_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.temperature_state().get_bed_temp_subject(), temp_decideg);
        lv_subject_set_int(state_.temperature_state().get_bed_target_subject(), target_decideg);
    }

    /**
     * @brief Set extruder temperature and target in PrinterState subjects
     */
    void set_extruder_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.temperature_state().get_active_extruder_temp_subject(),
                           temp_decideg);
        lv_subject_set_int(state_.temperature_state().get_active_extruder_target_subject(),
                           target_decideg);
    }

    /**
     * @brief Set both bed and extruder temperatures
     */
    void set_all_temps(int bed_temp, int bed_target, int ext_temp, int ext_target) {
        set_bed_temps(bed_temp, bed_target);
        set_extruder_temps(ext_temp, ext_target);
    }

    /**
     * @brief Set progress and layer for completion fallback tests
     */
    void set_progress_and_layer(int progress, int layer) {
        lv_subject_set_int(state_.print_state().get_print_progress_subject(), progress);
        lv_subject_set_int(state_.print_state().get_print_layer_current_subject(), layer);
    }

    /**
     * @brief Process pending async UI updates
     *
     * Since set_print_start_state() defers subject updates through the UpdateQueue,
     * we need to drain the queue to see the updates in tests.
     */
    void drain_async_updates() {
        helix::ui::UpdateQueue::instance().drain();
    }

    /**
     * @brief Feed a raw gcode response line through the real signal path
     *
     * Dispatches notify_gcode_response so the collector's pattern matching runs
     * and (on a match) latches current_phase_ AND sets real_signal_seen_ — the
     * exact firmware-speaks condition that gates the proactive detector off.
     */
    void send_gcode_response(const std::string& line) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }

    /**
     * @brief Reset collector's internal state back to IDLE for proactive detection tests
     *
     * After start(), the collector's internal current_phase_ is INITIALIZING.
     * state().reset_print_start_state() only resets the PrinterState subject, not
     * the collector's internal current_phase_. This helper calls collector().reset()
     * which resets current_phase_ to INITIALIZING, then forces the PrinterState
     * subject back to IDLE for test isolation, and re-enables fallbacks.
     */
    void reset_collector_to_idle() {
        // Ensure print is considered active so set_print_start_state() doesn't
        // reject non-IDLE phase updates via the print_active_ guard
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);

        collector_->reset();
        drain_async_updates(); // Process reset()'s queued INITIALIZING state
        state_.print_state().reset_print_start_state(); // Override back to IDLE
        drain_async_updates();                          // Process the IDLE update
    }

    int get_current_progress() {
        return lv_subject_get_int(state_.print_state().get_print_start_progress_subject());
    }

    /// A status frame through the real notify_status_update path, so the collector's
    /// status subscription handles it as it would a live WebSocket frame.
    void send_status_update(const json& object) {
        client().dispatch_status_update(object);
        drain_async_updates();
    }

    /// start(), with the INITIALIZING update it queues applied.
    void start_and_settle() {
        collector_->start();
        drain_async_updates();
        drain_async_updates();
    }

    /// One fallback check as the ETA timer runs it, with the updates it queues applied.
    void tick_fallbacks() {
        collector_->check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
    }

  protected:
    PrinterState state_;
    std::unique_ptr<MoonrakerClientMock> client_;
    std::shared_ptr<PrintStartCollector> collector_;
};
