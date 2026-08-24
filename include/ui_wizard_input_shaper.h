// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "input_shaper_calibrator.h"
#include "lvgl/lvgl.h"
#include "wizard_step.h"

#include <atomic>
#include <memory>
#include <string>

/**
 * @file ui_wizard_input_shaper.h
 * @brief Wizard input shaper calibration step - optional accelerometer calibration
 *
 * Provides input shaper calibration during first-run wizard when an accelerometer
 * is detected. Uses InputShaperCalibrator for the actual calibration workflow.
 *
 * ## Skip Logic:
 *
 * - No accelerometer detected: Skip entirely (input shaper can be configured later
 *   in Settings → Advanced → Input Shaper)
 * - Accelerometer detected: Show wizard step for calibration
 * - Footer shows "Skip" button (via wizard_show_skip subject) to allow skipping
 * - After successful calibration, footer changes to "Next"
 *
 * ## Subject Bindings:
 *
 * - wizard_input_shaper_status (string) - Current calibration status message
 * - wizard_input_shaper_progress (int) - Calibration progress 0-100
 *
 * ## Validation:
 *
 * Step is validated when calibration completed successfully.
 * User can also skip via the footer "Skip" button without completing calibration.
 */

namespace helix {
namespace calibration {
class InputShaperCalibrator;
} // namespace calibration
} // namespace helix

/**
 * @class WizardInputShaperStep
 * @brief Input shaper calibration step for the first-run wizard
 */
class WizardInputShaperStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::InputShaper;
    }
    const char* component_name() const override {
        return "wizard_input_shaper";
    }
    const char* log_name() const override {
        return "Wizard Input Shaper";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override;

    WizardInputShaperStep();
    ~WizardInputShaperStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardInputShaperStep(const WizardInputShaperStep&) = delete;
    WizardInputShaperStep& operator=(const WizardInputShaperStep&) = delete;
    WizardInputShaperStep(WizardInputShaperStep&&) = delete;
    WizardInputShaperStep& operator=(WizardInputShaperStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the input shaper calibration UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources
     */
    void cleanup() override;

    /**
     * @brief Abort an in-progress calibration via M112 + firmware_restart.
     *
     * Suppresses the recovery / disconnect modals, invalidates async tokens,
     * triggers the calibrator's emergency abort sequence, and resets the
     * wizard UI back to its Start-able state. Safe to call when nothing is
     * running (becomes a no-op and returns false).
     *
     * Used by both the in-step Cancel button and by cleanup() when the
     * user backs out while calibration is still running.
     *
     * @return true if an active calibration was aborted; false if nothing was running.
     */
    bool abort_in_progress_calibration();

    /**
     * @brief Check if step is validated
     *
     * @return true if calibration complete or user explicitly skipped
     */
    bool is_validated() const override;

    /**
     * @brief Check if this step should be skipped
     *
     * Skips if no accelerometer is detected from the printer.
     *
     * @return true if step should be skipped, false otherwise
     */
    bool should_skip() const;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Input Shaper";
    }

    // ========================================================================
    // State accessors for testing and wizard flow
    // ========================================================================

    /**
     * @brief Check if accelerometer is available
     *
     * Queries printer_has_accelerometer subject.
     *
     * @return true if accelerometer detected
     */
    bool has_accelerometer() const;

    /**
     * @brief Get the calibrator instance
     *
     * @return Pointer to the InputShaperCalibrator (never null)
     */
    helix::calibration::InputShaperCalibrator* get_calibrator();

    /**
     * @brief Check if calibration was completed
     */
    bool is_calibration_complete() const {
        return calibration_complete_;
    }

    /**
     * @brief Set calibration complete flag
     */
    void set_calibration_complete(bool complete) {
        calibration_complete_ = complete;
    }

    /**
     * @brief Check if user explicitly skipped calibration
     */
    bool is_user_skipped() const {
        return user_skipped_;
    }

    /**
     * @brief Set user skipped flag
     */
    void set_user_skipped(bool skipped) {
        user_skipped_ = skipped;
    }

    // ========================================================================
    // Subject access for testing
    // ========================================================================

    lv_subject_t* get_status_subject() {
        return &calibration_status_;
    }

    lv_subject_t* get_progress_subject() {
        return &calibration_progress_;
    }

    lv_subject_t* get_started_subject() {
        return &calibration_started_;
    }

    lv_subject_t* get_active_subject() {
        return &calibration_active_;
    }

    lv_subject_t* get_indeterminate_subject() {
        return &calibration_indeterminate_;
    }

    /**
     * @brief Show the analysis-phase treatment: spinner on, elapsed "Ns" label
     *
     * Arms the shared ElapsedLabelTimer against the status subject. Main
     * thread only (touches LVGL and subjects).
     */
    void begin_analysis_display();

    /// Stop the elapsed timer; the status label keeps its last text
    void cancel_analysis_display();

    /**
     * @brief Test seam: raw handle of the analysis elapsed timer
     *
     * The unit-test harness only executes timers with a finite repeat count,
     * so tests lend this periodic timer one temporarily (restoring -1 after),
     * mirroring InputShaperPanel::analysis_elapsed_timer_for_test().
     */
    [[nodiscard]] lv_timer_t* analysis_timer_for_test() const {
        return analysis_elapsed_.timer_for_test();
    }

    /**
     * @brief Get lifetime token for async callback safety
     *
     * Used by callbacks to check if step is still valid before updating subjects.
     */
    helix::LifetimeToken get_lifetime_token() {
        return lifetime_.token();
    }

    /**
     * @brief Get the screen root object
     *
     * @return Pointer to the screen root object, or nullptr if not created
     */
    lv_obj_t* get_screen_root() const {
        return screen_root_;
    }

    /**
     * @brief Bar value for a calibration progress report on the combined
     *        X-then-Y bar, or -1 when the report must not move the bar
     *
     * The wizard shows one bar for both axes: X maps onto its first half
     * (percent/2), Y onto its second (50 + percent/2). Analysis-phase reports
     * carry no meaningful percent - the collector emits 0 purely as the
     * phase-change signal - so they return -1 and the caller updates the
     * status label instead, leaving the bar at its last sweep value.
     *
     * @param percent Sweep/completion percent (0-100) from the collector
     * @param phase Phase the report was tagged with
     * @param second_axis True for the Y-axis run, false for X
     * @return Bar value 0-100, or -1 for analysis-phase reports
     */
    static int combined_bar_value(int percent, ShaperCalibrationPhase phase, bool second_axis) {
        if (phase == ShaperCalibrationPhase::Analyzing) {
            return -1;
        }
        return second_axis ? 50 + percent / 2 : percent / 2;
    }

    /**
     * @brief Low-RAM warning modal shown before calibration (see memory_utils.h).
     *
     * Public because the file-static LVGL trampolines in ui_wizard_input_shaper.cpp
     * store and dismiss it directly — it's a plain UI handle with no invariant, so
     * getter/setter ceremony bought nothing. Cleared on cleanup() so a lingering
     * modal never outlives the step.
     */
    lv_obj_t* low_ram_warn_dialog_ = nullptr;

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects
    lv_subject_t calibration_status_;
    lv_subject_t calibration_progress_;
    lv_subject_t calibration_started_;       ///< 0=not started, 1=started (hides Start button)
    lv_subject_t calibration_active_;        ///< 1 iff calibration is running (controls Cancel
                                             ///< button visibility). Cleared on complete / cancel /
                                             ///< error. Distinct from `started_` which stays at 1
                                             ///< post-completion to keep the Start button hidden.
    lv_subject_t calibration_indeterminate_; ///< 1 during the offline analysis phase (no
                                             ///< percent): hides the bar, shows the spinner

    // String buffers for subjects
    char status_buffer_[128] = "Ready to calibrate";

    // Analysis-phase elapsed label ("Analyzing data... Ns"), 1 Hz on the
    // virtual clock. Cancel-safe from every teardown path (helper guarantees).
    helix::ui::ElapsedLabelTimer analysis_elapsed_;

    // Calibrator instance (owns the calibrator)
    std::unique_ptr<helix::calibration::InputShaperCalibrator> calibrator_;

    // State tracking
    bool subjects_initialized_ = false;
    bool calibration_complete_ = false;
    bool user_skipped_ = false;

    // Lifetime guard for async callback safety
    helix::AsyncLifetimeGuard lifetime_;
};

// ============================================================================
// Global Instance Access
// ============================================================================

WizardInputShaperStep* get_wizard_input_shaper_step();
