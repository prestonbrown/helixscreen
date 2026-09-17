// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

namespace helix {

/**
 * @brief Manages HelixPrint plugin status subjects for UI feature gating
 *
 * Tracks whether the HelixPrint Klipper plugin is installed and whether
 * phase tracking is enabled. Both subjects use tri-state semantics:
 * -1=unknown, 0=disabled/not installed, 1=enabled/installed.
 *
 * Extracted from PrinterState as part of god class decomposition.
 *
 * The unknown (-1) state allows the UI to distinguish between:
 * - "Still checking" (show loading/spinner)
 * - "Definitely not available" (show install prompt)
 * - "Available" (show feature controls)
 *
 * @note set_helix_plugin_installed triggers composite visibility updates
 *       in PrinterState (the has_any_preprint_options aggregate subject).
 */
class PrinterPluginStatusState {
  public:
    PrinterPluginStatusState() = default;
    ~PrinterPluginStatusState() = default;

    // Non-copyable
    PrinterPluginStatusState(const PrinterPluginStatusState&) = delete;
    PrinterPluginStatusState& operator=(const PrinterPluginStatusState&) = delete;

    /**
     * @brief Initialize plugin status subjects
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
     * @brief Set helix plugin installed status
     *
     * Called from within helix::ui::queue_update() by PrinterState, which
     * handles the async dispatch and the subsequent visibility update.
     *
     * @param installed True if HelixPrint plugin is installed
     */
    void set_installed(bool installed);

    /**
     * @brief Set phase tracking enabled status (async update)
     *
     * Thread-safe: Uses helix::ui::queue_update() for main-thread execution.
     *
     * @param enabled True if phase tracking is enabled
     */
    void set_phase_tracking_enabled(bool enabled);

    // ========================================================================
    // Subject accessors
    // ========================================================================

    /// Tri-state: -1=unknown, 0=not installed, 1=installed
    lv_subject_t* get_helix_plugin_installed_subject() {
        return &helix_plugin_installed_;
    }

    /// Tri-state: -1=unknown, 0=disabled, 1=enabled
    lv_subject_t* get_phase_tracking_enabled_subject() {
        return &phase_tracking_enabled_;
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    /**
     * @brief Check if HelixPrint plugin is installed
     *
     * @return true only when value is 1 (installed), false for -1 (unknown) or 0 (not installed)
     */
    int helix_plugin_state() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&helix_plugin_installed_));
    }

    bool service_has_helix_plugin() const {
        return helix_plugin_state() == 1;
    }

    /**
     * @brief Check if phase tracking is enabled
     *
     * @return true only when value is 1 (enabled), false for -1 (unknown) or 0 (disabled)
     */
    bool is_phase_tracking_enabled() const {
        return lv_subject_get_int(const_cast<lv_subject_t*>(&phase_tracking_enabled_)) == 1;
    }

  private:
    friend class PrinterPluginStatusStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    /// Expires the deferred setter below when the subjects go away. Declared
    /// after `subjects_` so reverse-order member destruction invalidates it
    /// before the subjects it protects (#1165, #1146).
    AsyncLifetimeGuard async_lifetime_;

    // Plugin status subjects (tri-state: -1=unknown, 0=no, 1=yes)
    lv_subject_t helix_plugin_installed_{}; // HelixPrint Klipper plugin
    lv_subject_t phase_tracking_enabled_{}; // Phase tracking toggle in plugin
};

} // namespace helix
