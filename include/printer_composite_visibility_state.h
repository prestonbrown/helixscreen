// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "printer_capabilities_state.h"
#include "subject_managed_panel.h"

#include <lvgl.h>

namespace helix {

/// Who records timelapses on this printer, and so which controls it gets. Firmware
/// that owns timelapse may also serve a stub of the plugin API that ignores writes,
/// so it outranks the plugin. XML binds the int values.
enum class TimelapseOwner { None = 0, Plugin = 1, Firmware = 2 };

[[nodiscard]] constexpr TimelapseOwner timelapse_owner(bool firmware_owns, bool plugin_available) {
    return firmware_owns      ? TimelapseOwner::Firmware
           : plugin_available ? TimelapseOwner::Plugin
                              : TimelapseOwner::None;
}

/**
 * @brief Manages the aggregate `has_any_preprint_options` visibility subject
 *
 * `has_any_preprint_options` is set to 1 when at least one PRINT OPTIONS row
 * would be visible — i.e. a row the PRINT_START analysis adds (AND-gated by
 * `helix_plugin_installed`), timelapse (no plugin gate), or any option declared
 * by the `PrePrintOption` framework. Used by `print_file_detail.xml` to hide the
 * entire PRINT OPTIONS card when no row would be visible.
 *
 * There are no per-op `can_show_*` subjects: nothing in XML or production C++
 * needs one, and `PrePrintOptionsRenderer`'s `VisibilitySubjectLookup` callback
 * is wired with a `nullptr`-returning lambda. A plugin-gated option that needs
 * gating declares its own subject.
 *
 * @note Update triggers:
 *   - Hardware discovery (set_hardware_internal)
 *   - Plugin status changes (set_helix_plugin_installed)
 *   - Printer type changes (set_printer_type_internal)
 */
class PrinterCompositeVisibilityState {
  public:
    PrinterCompositeVisibilityState() = default;
    ~PrinterCompositeVisibilityState() = default;

    PrinterCompositeVisibilityState(const PrinterCompositeVisibilityState&) = delete;
    PrinterCompositeVisibilityState& operator=(const PrinterCompositeVisibilityState&) = delete;

    /**
     * @brief Initialize composite visibility subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Recalculate `has_any_preprint_options`
     *
     * Sets to 1 iff:
     *   (plugin_installed AND macro_option_count > 0)
     *   OR timelapse capability OR framework_option_count > 0.
     *
     * Leveling hardware alone never counts: its row exists only when the
     * database declares it or PRINT_START offers a skip parameter for it.
     *
     * Writes the subject on every call; the debug log line fires only when an
     * input flips.
     *
     * @param plugin_installed True if HelixPrint plugin is installed
     * @param capabilities Reference to capabilities state for has_* queries
     * @param framework_option_count Rows the active printer's declared
     *        options draw: options the detail view leaves out, or whose
     *        required macro is missing, are not counted.
     * @param macro_option_count Rows the PRINT_START analysis adds; each needs
     *        the plugin to deliver its skip.
     */
    void update_visibility(bool plugin_installed, const PrinterCapabilitiesState& capabilities,
                           size_t framework_option_count = 0, size_t macro_option_count = 0);

    /**
     * @brief Get aggregate subject: 1 if ANY preprint option row is visible
     */
    lv_subject_t* get_has_any_preprint_options_subject() {
        return &has_any_preprint_options_;
    }

    /// Publishes `timelapse_owner` for XML. Writes only on change.
    void set_timelapse_owner(TimelapseOwner owner);

    /// Publishes `timelapse_enabled`, the plugin's global setting. Writes only on change.
    void set_timelapse_enabled(bool enabled);

    lv_subject_t* get_timelapse_owner_subject() {
        return &timelapse_owner_;
    }
    lv_subject_t* get_timelapse_enabled_subject() {
        return &timelapse_enabled_;
    }

  private:
    friend class PrinterCompositeVisibilityStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Dirty-tracking for debug logging (only log when any input flips).
    bool last_log_state_initialized_ = false;
    int last_any_ = -1;
    bool last_plugin_ = false;

    lv_subject_t has_any_preprint_options_{};
    lv_subject_t timelapse_owner_{};
    lv_subject_t timelapse_enabled_{};
};

} // namespace helix
