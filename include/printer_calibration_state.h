// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "idle_timeout_busy.h"
#include "state/volatile_subjects.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <lvgl.h>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Manages calibration and configuration subjects for printer state
 *
 * Tracks firmware retraction settings, manual probe state, and motor enabled state.
 * These subjects are used by calibration panels and Z-offset workflows.
 *
 * Extracted from PrinterState as part of god class decomposition.
 *
 * ## Firmware Retraction (4 subjects)
 * - retract_length_: Length in centimillimeters (0.8mm stored as 80)
 * - retract_speed_: Speed in mm/s (integer)
 * - unretract_extra_length_: Extra length in centimillimeters
 * - unretract_speed_: Speed in mm/s (integer)
 *
 * ## Manual Probe (2 subjects)
 * - manual_probe_active_: 0=inactive, 1=active (during PROBE_CALIBRATE)
 * - manual_probe_z_position_: Z position in microns (0.125mm stored as 125)
 *
 * ## Motor State (1 subject)
 * - motors_enabled_: 0=disabled (Idle), 1=enabled (Ready/Printing)
 *
 * @note Thread safety: update_from_status() should be called from the main thread
 *       (typically via helix::async::invoke in PrinterState)
 */
class PrinterCalibrationState {
  public:
    PrinterCalibrationState() = default;
    ~PrinterCalibrationState() = default;

    // Non-copyable
    PrinterCalibrationState(const PrinterCalibrationState&) = delete;
    PrinterCalibrationState& operator=(const PrinterCalibrationState&) = delete;

    /**
     * @brief Initialize calibration subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update calibration state from Moonraker status JSON
     *
     * Parses firmware_retraction, manual_probe, and idle_timeout sections.
     *
     * @param status JSON status object from Moonraker
     */
    void update_from_status(const nlohmann::json& status);

    // ========================================================================
    // Subject Accessors
    // ========================================================================

    /**
     * @brief Get manual probe active subject
     * @return Integer subject: 0=inactive, 1=active
     */
    lv_subject_t* get_manual_probe_active_subject() {
        return &manual_probe_active_;
    }

    /**
     * @brief Get manual probe Z position subject
     * @return Integer subject: Z position in microns
     */
    lv_subject_t* get_manual_probe_z_position_subject() {
        return &manual_probe_z_position_;
    }

    /**
     * @brief Get motors enabled subject
     * @return Integer subject: 0=disabled, 1=enabled
     */
    lv_subject_t* get_motors_enabled_subject() {
        return &motors_enabled_;
    }

    /**
     * @brief Get idle_timeout "Printing" subject
     *
     * Returns 1 when Klipper's idle_timeout.state == "Printing" — its canonical
     * busy flag, true for the full duration of any blocking operation (G28,
     * BED_MESH_CALIBRATE, QGL, PROBE_ACCURACY, long macro) as well as during a
     * real file print. 0 for "Ready"/"Idle". Consumers combine this with the
     * print-job state to isolate blocking non-print operations.
     *
     * @return Integer subject: 1 if idle_timeout.state == "Printing", else 0
     */
    lv_subject_t* get_idle_timeout_printing_subject() {
        return &idle_timeout_printing_;
    }

    /**
     * @brief Debounced busy flag for the blocking-operation guard
     *
     * The subject above is the literal Klipper state and flips on every burst of
     * executing gcode, housekeeping delayed_gcode included. Callers deciding
     * whether to refuse or queue gcode want this instead — see IdleTimeoutBusy
     * for why the raw flag is not usable as a gate.
     */
    IdleTimeoutBusy& idle_timeout_busy() {
        return idle_timeout_busy_;
    }

    /**
     * @brief Get retract length subject
     * @return Integer subject: length in centimillimeters
     */
    lv_subject_t* get_retract_length_subject() {
        return &retract_length_;
    }

    /**
     * @brief Get retract speed subject
     * @return Integer subject: speed in mm/s
     */
    lv_subject_t* get_retract_speed_subject() {
        return &retract_speed_;
    }

    /**
     * @brief Get unretract extra length subject
     * @return Integer subject: length in centimillimeters
     */
    lv_subject_t* get_unretract_extra_length_subject() {
        return &unretract_extra_length_;
    }

    /**
     * @brief Get unretract speed subject
     * @return Integer subject: speed in mm/s
     */
    lv_subject_t* get_unretract_speed_subject() {
        return &unretract_speed_;
    }

    /**
     * @brief Claim the "printer is busy — your change will queue" toast for the
     *        current blocking episode.
     *
     * The discretionary-gcode busy guard lets benign commands (fan/temp/LED) queue
     * behind a blocking op instead of rejecting them, and wants to tell the user
     * ONCE per episode rather than once per command (bundle 7CT79XXK saw four
     * back-to-back toasts). Returns true for the first caller after the blocking op
     * started, false thereafter, until PrinterState re-arms it via
     * arm_busy_queue_toast() once the composite blocking condition clears.
     *
     * Both claim and re-arm normally run on the main thread (status parsing is
     * queued to it, and NOTIFY_INFO self-defers). The flag is atomic purely so a
     * background caller of execute_gcode remains safe against the concurrent
     * re-arm; it is a lone flag with no data published alongside it, so relaxed
     * ordering is sufficient.
     */
    bool claim_busy_queue_toast() {
        return !busy_queue_toast_shown_.exchange(true, std::memory_order_relaxed);
    }

    /// Re-arm the once-per-episode busy-queue toast so the next blocking episode
    /// announces itself again. Called by PrinterState::update_from_status when the
    /// COMPOSITE blocking condition (is_blocking_operation_active) is clear — not on
    /// any single subject's falling edge, which would re-toast mid-episode (#1108).
    void arm_busy_queue_toast() {
        busy_queue_toast_shown_.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Restore subjects invalidated by a Klipper restart to their defaults.
     *
     * Called by PrinterState on any Klippy state transition. Klipper reports these
     * as DELTA-only fields, so a value cached before a restart otherwise persists
     * until the field next changes — which made a freshly-restarted idle printer
     * look busy and wedged discretionary G-code (#1129).
     */
    void reset_klippy_volatile() {
        volatile_.reset_all();
        // The debounced view is not a subject and so is not in volatile_, but it
        // caches the same delta-only field and would otherwise keep reporting a
        // pre-restart operation as still running — the exact #1129 wedge, just
        // one layer down.
        idle_timeout_busy_.set_printing(false);
    }

  private:
    friend class PrinterCalibrationStateTestAccess;

    SubjectManager subjects_;
    helix::subjects::VolatileSubjects volatile_;
    bool subjects_initialized_ = false;

    // Firmware retraction settings (from firmware_retraction Klipper module)
    // Lengths stored as centimillimeters (x100) to preserve 0.01mm precision with integers
    lv_subject_t retract_length_{};         // centimm (e.g., 80 = 0.8mm)
    lv_subject_t retract_speed_{};          // mm/s (integer, e.g., 35)
    lv_subject_t unretract_extra_length_{}; // centimm (e.g., 0 = 0.0mm)
    lv_subject_t unretract_speed_{};        // mm/s (integer, e.g., 35)

    // Manual probe subjects (for Z-offset calibration)
    lv_subject_t manual_probe_active_{};     // 0=inactive, 1=active (PROBE_CALIBRATE running)
    lv_subject_t manual_probe_z_position_{}; // Z position * 1000 (for 0.001mm resolution)

    // Motor enabled state (from idle_timeout.state)
    lv_subject_t motors_enabled_{}; // 0=disabled (Idle), 1=enabled (Ready/Printing)

    // idle_timeout.state == "Printing" flag (Klipper's canonical busy indicator)
    lv_subject_t idle_timeout_printing_{}; // 0=not printing/busy, 1=state == "Printing"

    // Debounced view of the same flag, used by the blocking-op guard. The subject
    // above stays the literal Klipper state; this one waits for it to settle.
    IdleTimeoutBusy idle_timeout_busy_;

    // Latches the once-per-blocking-episode "your change will queue" toast. Claimed
    // by the busy guard (main thread), re-armed on the blocking op's falling edge in
    // update_from_status (websocket thread) — hence atomic.
    std::atomic<bool> busy_queue_toast_shown_{false};
};

} // namespace helix
