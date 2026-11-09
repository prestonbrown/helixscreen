// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hardware_validator.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

namespace helix {

/**
 * @brief Manages hardware validation subjects for UI display
 *
 * Tracks hardware validation state including issue counts, severity levels,
 * and formatted status text for the Settings panel Hardware Health section.
 *
 * One of PrinterState's domain sub-states.
 *
 * ## Subjects (8 total):
 * - hardware_status_level_ (int): 0=ok, 1=attention, 2=critical (headline badge)
 * - hardware_critical_count_ (int): Count of critical issues
 * - hardware_warning_count_ (int): Count of warning issues
 * - hardware_info_count_ (int): Count of info issues
 * - hardware_session_count_ (int): Count of session change issues
 * - hardware_status_title_ (string): e.g., "All Healthy" or "3 Issues Detected"
 * - hardware_status_detail_ (string): e.g., "1 critical, 2 warnings"
 * - hardware_issues_label_ (string): "1 Hardware Issue" or "5 Hardware Issues"
 */
class PrinterHardwareValidationState {
  public:
    PrinterHardwareValidationState() = default;
    ~PrinterHardwareValidationState() = default;

    PrinterHardwareValidationState(const PrinterHardwareValidationState&) = delete;
    PrinterHardwareValidationState& operator=(const PrinterHardwareValidationState&) = delete;

    /**
     * @brief Initialize hardware validation subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    // ========================================================================
    // Setters
    // ========================================================================

    /**
     * @brief Set hardware validation result and update all subjects
     *
     * Updates all hardware validation subjects based on the validation result.
     * Call after HardwareValidator::validate() completes.
     *
     * @param result Validation result from HardwareValidator
     */
    void set_hardware_validation_result(const HardwareValidationResult& result);

    /**
     * @brief Remove a hardware issue from the cached validation result
     *
     * Removes the issue matching the given hardware name from all issue lists
     * and updates all related subjects (counts, status text, etc.).
     * Used when user clicks "Ignore" or "Save" on a hardware issue.
     *
     * @param hardware_name The hardware name to remove (e.g., "filament_sensor runout")
     */
    void remove_hardware_issue(const std::string& hardware_name);

    /// Re-format the status texts in the current language.
    void refresh_texts();

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /**
     * @brief Get the headline badge level subject for UI binding
     *
     * Integer subject: 0=ok, 1=attention, 2=critical. Each headline badge in
     * hardware_health_overlay binds to exactly one of these values, so the
     * XML never has to decode a HardwareIssueSeverity ordinal itself.
     */
    lv_subject_t* get_hardware_status_level_subject() {
        return &hardware_status_level_;
    }

    lv_subject_t* get_hardware_critical_count_subject() {
        return &hardware_critical_count_;
    }

    lv_subject_t* get_hardware_warning_count_subject() {
        return &hardware_warning_count_;
    }

    lv_subject_t* get_hardware_info_count_subject() {
        return &hardware_info_count_;
    }

    lv_subject_t* get_hardware_session_count_subject() {
        return &hardware_session_count_;
    }

    /**
     * @brief Get the hardware status title subject
     *
     * String subject with formatted title like "All Healthy" or "3 Issues Detected".
     */
    lv_subject_t* get_hardware_status_title_subject() {
        return &hardware_status_title_;
    }

    /**
     * @brief Get the hardware status detail subject
     *
     * String subject with formatted detail like "1 critical, 2 warnings".
     */
    lv_subject_t* get_hardware_status_detail_subject() {
        return &hardware_status_detail_;
    }

    /**
     * @brief Get the hardware issues label subject
     *
     * String subject with formatted label like "1 Hardware Issue" or "5 Hardware Issues".
     * Used for settings panel row label binding.
     */
    lv_subject_t* get_hardware_issues_label_subject() {
        return &hardware_issues_label_;
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    /**
     * @brief Check if hardware validation has any issues
     */
    bool has_hardware_issues() const {
        return hardware_validation_result_.has_issues();
    }

    /**
     * @brief Get the stored hardware validation result
     *
     * Returns the most recent validation result set via set_hardware_validation_result().
     * Use this to access detailed issue information for UI display.
     *
     * @return Reference to the stored validation result
     */
    const HardwareValidationResult& get_hardware_validation_result() const {
        return hardware_validation_result_;
    }

  private:
    friend class PrinterHardwareValidationStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Hardware validation subjects
    lv_subject_t hardware_status_level_{};   // Integer: 0=ok, 1=attention, 2=critical
    lv_subject_t hardware_critical_count_{}; // Integer: count of critical issues
    lv_subject_t hardware_warning_count_{};  // Integer: count of warning issues
    lv_subject_t hardware_info_count_{};     // Integer: count of info issues
    lv_subject_t hardware_session_count_{};  // Integer: count of session change issues
    lv_subject_t hardware_status_title_{};   // String: e.g., "All Healthy"
    lv_subject_t hardware_status_detail_{};  // String: e.g., "1 critical, 2 warnings"
    lv_subject_t hardware_issues_label_{};   // String: "1 Hardware Issue" / "5 Hardware Issues"

    // Stored validation result for UI access
    HardwareValidationResult hardware_validation_result_;
    bool has_result_ = false; ///< Before the first result the texts keep their defaults

    // String buffers for subject storage
    char hardware_status_title_buf_[64]{};
    char hardware_status_detail_buf_[128]{};
    char hardware_issues_label_buf_[96]{}; // "1 Hardware Issue" / "5 Hardware Issues"
};

} // namespace helix
