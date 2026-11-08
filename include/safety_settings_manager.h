// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

namespace helix {

/**
 * @brief Domain-specific manager for safety settings
 *
 * Owns all safety-related LVGL subjects and persistence:
 * - estop_require_confirmation (0/1)
 * - cancel_escalation_enabled (0/1)
 * - cancel_escalation_timeout (dropdown index 0-3 -> 15/30/60/120s)
 * - macro_require_confirmation (0/1)
 * - allow_cold_extrude (0/1)
 * - tool_change_require_confirmation (0/1)
 * - min_toast_severity (dropdown index 0=All / 1=Warnings & errors / 2=Errors only, #1213)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class SafetySettingsManager {
  public:
    static SafetySettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    SafetySettingsManager(const SafetySettingsManager&) = delete;
    SafetySettingsManager& operator=(const SafetySettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // GETTERS / SETTERS
    // =========================================================================

    bool get_estop_require_confirmation() const {
        return settings_.get_bool(Key::EstopConfirm);
    }
    void set_estop_require_confirmation(bool require) {
        settings_.set(Key::EstopConfirm, require);
    }

    bool get_cancel_escalation_enabled() const {
        return settings_.get_bool(Key::CancelEscalation);
    }
    void set_cancel_escalation_enabled(bool enabled) {
        settings_.set(Key::CancelEscalation, enabled);
    }

    /** @brief Get cancel escalation timeout in seconds (15, 30, 60, or 120) */
    int get_cancel_escalation_timeout_seconds() const;

    /** @brief Set cancel escalation timeout in seconds (clamped to valid values) */
    void set_cancel_escalation_timeout_seconds(int seconds);

    /** @brief Whether macro runs require a confirmation modal */
    bool get_macro_require_confirmation() const {
        return settings_.get_bool(Key::MacroConfirm);
    }
    void set_macro_require_confirmation(bool require) {
        settings_.set(Key::MacroConfirm, require);
    }

    /** @brief Whether filament load/unload may run below min_extrude_temp (#978) */
    bool get_allow_cold_extrude() const {
        return settings_.get_bool(Key::AllowColdExtrude);
    }
    void set_allow_cold_extrude(bool allow) {
        settings_.set(Key::AllowColdExtrude, allow);
    }

    /** @brief Whether a tap that mounts another tool asks first */
    bool get_tool_change_require_confirmation() const {
        return settings_.get_bool(Key::ToolChangeConfirm);
    }
    void set_tool_change_require_confirmation(bool require) {
        settings_.set(Key::ToolChangeConfirm, require);
    }

    /** @brief Get the minimum toast severity index (0=All, 1=Warnings & errors, 2=Errors only) */
    int get_min_toast_severity() const;

    /** @brief Set the minimum toast severity index (updates subject + persists + pushes to the
     *         notification toast gate). #1213 */
    void set_min_toast_severity(int index);

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief E-Stop confirmation subject (integer: 0=immediate, 1=require confirm) */
    lv_subject_t* subject_estop_require_confirmation() {
        return settings_.subject(Key::EstopConfirm);
    }

    /** @brief Cancel escalation enabled subject (integer: 0=disabled, 1=enabled) */
    lv_subject_t* subject_cancel_escalation_enabled() {
        return settings_.subject(Key::CancelEscalation);
    }

    /** @brief Cancel escalation timeout subject (integer: dropdown index 0-3) */
    lv_subject_t* subject_cancel_escalation_timeout() {
        return &cancel_escalation_timeout_subject_;
    }

    /** @brief Macro confirmation subject (integer: 0=run immediately, 1=require confirm) */
    lv_subject_t* subject_macro_require_confirmation() {
        return settings_.subject(Key::MacroConfirm);
    }

    /** @brief Allow-cold-extrude subject (integer: 0=gate on temp, 1=always allow) */
    lv_subject_t* subject_allow_cold_extrude() {
        return settings_.subject(Key::AllowColdExtrude);
    }

    /** @brief Tool-change confirmation subject (integer: 0=change immediately, 1=confirm) */
    lv_subject_t* subject_tool_change_require_confirmation() {
        return settings_.subject(Key::ToolChangeConfirm);
    }

    /** @brief Minimum toast severity subject (integer: dropdown index 0/1/2, #1213) */
    lv_subject_t* subject_min_toast_severity() {
        return &min_toast_severity_subject_;
    }

  private:
    SafetySettingsManager();
    ~SafetySettingsManager() = default;

    enum class Key : uint8_t {
        EstopConfirm,
        CancelEscalation,
        MacroConfirm,
        AllowColdExtrude,
        ToolChangeConfirm,
        COUNT
    };

    SubjectManager subjects_;
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;

    lv_subject_t cancel_escalation_timeout_subject_{};
    lv_subject_t min_toast_severity_subject_{};

    bool subjects_initialized_ = false;
};

} // namespace helix
