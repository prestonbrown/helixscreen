// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace helix {
enum class PrintJobState;
enum class PrintOutcome;
} // namespace helix

/**
 * @brief Print state machine states
 *
 * Extracted from PrintStatusPanel for testability. Maps the raw Moonraker
 * job state + outcome into higher-level UI states.
 */
enum class PrintState {
    Idle,      ///< No active print
    Preparing, ///< Running pre-print operations (homing, leveling, etc.)
    Printing,  ///< Actively printing
    Paused,    ///< Print paused
    Complete,  ///< Print finished successfully
    Cancelled, ///< Print cancelled by user
    Error      ///< Print failed with error
};

/**
 * @brief Derive the UI-level print state from the printer's job state and phase
 *
 * The single definition of this mapping. A live pre-print phase outranks the
 * job state, which is what makes Preparing reachable while print_stats still
 * holds a previous job's terminal state (a host-side pre-start block runs
 * before the printer is handed the job at all).
 *
 * @param job_state    Moonraker's print_stats.state
 * @param start_phase  Pre-print phase, 0 when none is running
 */
PrintState derive_print_state(helix::PrintJobState job_state, int start_phase);

/**
 * @brief Does a job own the machine right now?
 *
 * True for `Preparing`, `Printing` and `Paused`: the states in which a print job
 * is entitled to move the toolhead, and therefore the states in which nothing
 * else may. This is the question the ~60 open-coded `PRINTING || PAUSED`
 * comparisons are all trying to ask, and the reason they get it wrong is that
 * `PrintJobState` cannot express `Preparing` - a job the app has committed to
 * but the printer has not reported yet.
 *
 * Ask this instead of comparing the raw job state whenever the decision is
 * "would acting now fight the printer for the toolhead": motion and extrusion
 * guards, filament operations, tool changes, cooldowns, anything that emits
 * G-code of its own.
 *
 * @warning Not a substitute for switching on the lifecycle when a caller must
 *          distinguish `Paused`. `ams_subscription_backend.cpp` deliberately
 *          ALLOWS a filament op on a paused print when the backend does not
 *          self-home, because then no firmware macro can hide a `G28`. This
 *          predicate is a convenience over the lifecycle, never a replacement
 *          for it.
 */
constexpr bool job_holds_machine(PrintState state) {
    return state == PrintState::Preparing || state == PrintState::Printing ||
           state == PrintState::Paused;
}

/**
 * @brief Result of a state transition attempt
 *
 * Carries all the information the UI layer needs to react to a state change
 * without embedding any widget logic here.
 */
struct StateChangeResult {
    bool state_changed = false;
    bool print_ended = false;
    bool should_reset_progress_bar = false;
    bool should_clear_excluded_objects = false;
    bool should_freeze_complete = false;
    bool should_animate_cancelled = false;
    bool should_animate_error = false;

    PrintState old_state = PrintState::Idle;
    PrintState new_state = PrintState::Idle;

    /// Computed: true when the viewer should be shown (want_viewer && gcode_loaded)
    bool should_show_viewer = false;
};

/**
 * @brief Pure-logic state machine for print lifecycle
 *
 * Owns all the mutable state that PrintStatusPanel previously held inline.
 * Has NO LVGL or widget dependencies — testable in isolation.
 *
 * Usage:
 *   PrintLifecycleState sm;
 *   auto result = sm.on_job_state_changed(job_state, outcome);
 *   if (result.state_changed) { ... react in UI layer ... }
 */
class PrintLifecycleState {
  public:
    // ── Input methods ────────────────────────────────────────────────

    /**
     * @brief Process a job state change from Moonraker
     *
     * Maps PrintJobState + PrintOutcome to the internal PrintState enum and
     * computes all transition side-effects.
     *
     * @param start_phase The LIVE pre-print phase, so this derives the same
     *        answer as `PrinterPrintState::publish_lifecycle_state()` does for
     *        the `print_lifecycle` subject. It used to hard-code 0 on the
     *        reasoning that `on_start_phase_changed()` owned that axis, which is
     *        false: a job-state change arriving while a phase is live must still
     *        let the phase outrank it, exactly as `derive_print_state()` says.
     *        Hard-coding 0 made this object disagree with the published subject
     *        for the whole of a pre-print window - `Printing` instead of
     *        `Preparing` on a firmware-side `PRINT_START`, and `Complete`
     *        instead of `Preparing` when a host-side block runs after a finished
     *        job. Defaulted to 0 so the phase-less unit tests, which exercise
     *        this axis deliberately, keep reading as written.
     */
    StateChangeResult on_job_state_changed(helix::PrintJobState job_state,
                                           helix::PrintOutcome outcome, int start_phase = 0);

    /**
     * @brief Update print progress percentage
     * @return false if the update was guarded (e.g. in Complete state)
     */
    bool on_progress_changed(int progress);

    /**
     * @brief Update current/total layer info
     * @return false if the update was guarded
     */
    bool on_layer_changed(int layer, int total, bool has_real_data);

    /**
     * @brief Update elapsed print duration
     * @return false if the update was guarded
     */
    bool on_duration_changed(int seconds, helix::PrintOutcome outcome);

    /**
     * @brief Update estimated time remaining
     * @return false if the update was guarded
     */
    bool on_time_left_changed(int seconds, helix::PrintOutcome outcome);

    /**
     * @brief Update start phase (for Preparing detection)
     * @return true if the state actually changed
     */
    bool on_start_phase_changed(int phase, helix::PrintJobState current_job_state);

    /**
     * @brief Update pre-print elapsed time (only stored during Preparing)
     */
    void on_preprint_elapsed_changed(int seconds);

    /**
     * @brief Update pre-print remaining time (only stored during Preparing)
     */
    void on_preprint_remaining_changed(int seconds, int slicer_remaining);

    /**
     * @brief Update temperature readings (always accepted)
     */
    void on_temperature_changed(int nz_cur, int nz_tgt, int bed_cur, int bed_tgt);

    /**
     * @brief Update speed percentage (always accepted)
     */
    void on_speed_changed(int speed);

    /**
     * @brief Update flow percentage (always accepted)
     */
    void on_flow_changed(int flow);

    /**
     * @brief Mark whether gcode geometry is loaded for the 3D viewer
     */
    void set_gcode_loaded(bool loaded);

    // ── Accessors ────────────────────────────────────────────────────

    PrintState state() const {
        return current_state_;
    }
    /// True during Preparing, Printing, or Paused — i.e. a print job is in
    /// progress. False in Idle and terminal states (Complete, Cancelled, Error).
    static bool is_active(PrintState s) {
        return s == PrintState::Printing || s == PrintState::Paused || s == PrintState::Preparing;
    }
    bool is_active() const {
        return is_active(current_state_);
    }
    int progress() const {
        return current_progress_;
    }
    int current_layer() const {
        return current_layer_;
    }
    int total_layers() const {
        return total_layers_;
    }
    int elapsed_seconds() const {
        return elapsed_seconds_;
    }
    int remaining_seconds() const {
        return remaining_seconds_;
    }
    int preprint_elapsed_seconds() const {
        return preprint_elapsed_seconds_;
    }
    int preprint_remaining_seconds() const {
        return preprint_remaining_seconds_;
    }
    int nozzle_current() const {
        return nozzle_current_;
    }
    int nozzle_target() const {
        return nozzle_target_;
    }
    int bed_current() const {
        return bed_current_;
    }
    int bed_target() const {
        return bed_target_;
    }
    int speed_percent() const {
        return speed_percent_;
    }
    int flow_percent() const {
        return flow_percent_;
    }
    bool gcode_loaded() const {
        return gcode_loaded_;
    }

    /**
     * @brief Whether the 3D viewer is desired for the current state
     *
     * True during active print and all terminal states, so the user can see
     * where the print stopped. The UI should combine this with gcode_loaded()
     * to decide actual visibility.
     */
    bool want_viewer() const {
        return current_state_ != PrintState::Idle;
    }

    /**
     * @brief Map current_layer from Moonraker total_layers space into the
     * gcode viewer's layer space.
     *
     * Slicer metadata total_layers and the viewer's parsed layer count often
     * differ (e.g. Moonraker 240 vs viewer 2912), so the viewer layer must be
     * rescaled. Returns current_layer_ unchanged when either count is unknown.
     *
     * Used by both the live progress update path and the terminal→Idle
     * re-freeze path so they cannot drift.
     */
    int map_current_layer_to_viewer(int viewer_max_layer) const {
        if (total_layers_ > 0 && viewer_max_layer > 0) {
            return (current_layer_ * viewer_max_layer) / total_layers_;
        }
        return current_layer_;
    }

  private:
    friend class PrintLifecycleStateTestAccess;

    PrintState current_state_ = PrintState::Idle;
    bool gcode_loaded_ = false;

    int current_progress_ = 0;
    int current_layer_ = 0;
    int total_layers_ = 0;

    int elapsed_seconds_ = 0;
    int remaining_seconds_ = 0;
    int preprint_elapsed_seconds_ = 0;
    int preprint_remaining_seconds_ = 0;

    int nozzle_current_ = 0;
    int nozzle_target_ = 0;
    int bed_current_ = 0;
    int bed_target_ = 0;

    int speed_percent_ = 100;
    int flow_percent_ = 100;
};
