// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "printer_state.h"
#include "update_queue_test_access.h"

namespace helix {

class PrinterPrintStateTestAccess {
  public:
    static void reset_extra(PrinterPrintState& pps) {
        pps.estimated_print_time_ = 0;
        // File-scoped slice geometry + per-print Z cache used by the Z-height
        // layer derivation. Like estimated_print_time_ these survive
        // reset_for_new_print() in production; the test reset() simulates a fresh
        // session on the shared singleton, so clear them for cross-test isolation.
        pps.layer_height_ = 0.0;
        pps.first_layer_height_ = 0.0;
        pps.last_gcode_z_mm_ = 0.0;
        pps.have_gcode_z_ = false;
        pps.layer_z_derived_ = false;
        pps.has_real_layer_data_ = false;
        // Sticky printer capability — session-scoped in production (survives
        // reset_for_new_print, cleared only on a fresh session). The test
        // reset() simulates a fresh session on the shared PrinterState
        // singleton, so clear it here for cross-test isolation.
        pps.printer_reports_layers_ = false;
        pps.slicer_progress_ = 0.0;
        pps.slicer_progress_active_ = false;
        pps.smoothed_remaining_ = 0.0;
        pps.has_smoothed_remaining_ = false;
        pps.sdcard_active_ = false;
        // The job being prepared is session-scoped: it outlives
        // reset_for_new_print() by design, since it exists precisely for the
        // window before the printer reports the job. Leaving it set leaks a
        // live preparing job into the next test, where it relaxes the
        // phase-update guard and the print_active safety reset.
        pps.preparing_job_ = {};
        pps.cancel_preparing_watchdog();
    }

    /// Fire the preparing-job watchdog as if its timer had elapsed.
    ///
    /// The real bound is half an hour - longer than the slowest legitimate
    /// pre-print - so no test can wait for it. Mirrors
    /// ActivePrintMediaManagerTestAccess::fire_pending_retry().
    ///
    /// Invokes the PRODUCTION callback rather than reproducing what it does. An
    /// earlier version open-coded `cancel(); retire(TimedOut);` here, which made
    /// every watchdog assertion a tautology - the test asserted the exit reason
    /// the helper itself had just supplied, and `preparing_watchdog_cb` was
    /// reachable from no test at all. Changing the real callback's exit reason,
    /// or dropping its `preparing_watchdog_ = nullptr` (a double-free setup,
    /// since LVGL frees the one-shot on return), left the suite green.
    ///
    /// Deletes the timer afterwards because LVGL's one-shot repeat count is what
    /// would normally free it, and nothing here runs lv_timer_handler.
    static bool fire_preparing_watchdog(PrinterPrintState& pps) {
        lv_timer_t* timer = pps.preparing_watchdog_;
        if (!timer) {
            return false;
        }
        PrinterPrintState::preparing_watchdog_cb(timer);
        lv_timer_delete(timer);
        return true;
    }

    static bool has_preparing_watchdog(const PrinterPrintState& pps) {
        return pps.preparing_watchdog_ != nullptr;
    }

    /// Mark the layer counters as coming from real slicer/Moonraker fields
    /// rather than being derived from the progress fraction.
    static void set_has_real_layer_data(PrinterPrintState& pps, bool value) {
        pps.has_real_layer_data_ = value;
    }
};

// PrinterStateTestAccess must be in namespace helix to match friend declaration in PrinterState
class PrinterStateTestAccess {
  public:
    static void reset(PrinterState& ps) {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        // Drop the discovered fan list before the subjects go away. init_fans()
        // carries live readings across a re-init for fans that persist (#1181),
        // so a leaked fans_ now leaks speed_percent/ever_ran/rpm into the next
        // test instead of being silently zeroed. Re-initing with an empty list is
        // the public way to clear the list and expire every per-fan subject.
        ps.fan_state_.init_fans({});
        ps.deinit_subjects();
        ps.printer_type_.clear();
        ps.pre_print_option_set_ = PrePrintOptionSet();
        ps.z_offset_calibration_strategy_ = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
        ps.auto_detected_bed_moves_ = false;
        ps.is_paused_ = false;
        ps.last_kinematics_.clear();
        PrinterPrintStateTestAccess::reset_extra(ps.print_domain_);
    }

    static PrinterFanState& get_fan_state(PrinterState& ps) {
        return ps.fan_state_;
    }

    static PrinterPrintState& get_print_state(PrinterState& ps) {
        return ps.print_domain_;
    }

    /// Inject a synthetic pre-print option set (bypasses the printer DB) so tests
    /// can exercise option configurations that no shipped printer declares yet —
    /// e.g. a bed_mesh option with a custom adaptive_param name.
    static void set_option_set(PrinterState& ps, PrePrintOptionSet set) {
        ps.pre_print_option_set_ = std::move(set);
    }

    /**
     * @brief Make the blocking-op guard see a SUSTAINED idle_timeout "Printing"
     *
     * Setting the subject alone no longer reaches is_blocking_operation_active():
     * it reads IdleTimeoutBusy, which requires the flag to hold for SETTLE before
     * it counts (see include/idle_timeout_busy.h). A test that means "an external
     * blocking op is under way" means one that has already outlasted that window,
     * so back-date the transition rather than sleeping. Use this instead of
     * writing the subject directly; the settle behaviour itself is covered in
     * test_idle_timeout_busy.cpp.
     */
    static void set_sustained_idle_timeout_printing(PrinterState& ps, bool on) {
        lv_subject_set_int(ps.get_idle_timeout_printing_subject(), on ? 1 : 0);
        ps.idle_timeout_busy().set_printing(on, IdleTimeoutBusy::clock::now() -
                                                    IdleTimeoutBusy::SETTLE);
    }
};

} // namespace helix

using namespace helix;
